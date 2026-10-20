#pragma once
// Winsock headers must precede windows.h in the translation unit.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include "tracking.h"
#pragma comment(lib, "ws2_32.lib")

namespace fp {
class TrackingUdp {
public:
    enum class Status { off, waiting, receiving, unavailable };
    explicit TrackingUdp(unsigned port = trackingPort) : requestedPort(port) {}
    TrackingUdp(const TrackingUdp&) = delete;
    TrackingUdp& operator=(const TrackingUdp&) = delete;
    // Called only by the existing session worker, outside all game-state locks.
    void service(bool enabled) {
        if (!enabled) { stop(); return; }
        const ULONGLONG now = GetTickCount64();
        if (worker) {
            const DWORD result = WaitForSingleObject(worker, 0);
            if (result == WAIT_TIMEOUT) return;
            if (result != WAIT_OBJECT_0) {
                // Never replace or close a worker whose completion is uncertain.
                state = Status::unavailable;
                return;
            }
            CloseHandle(worker);
            CloseHandle(wake);
            worker = wake = nullptr;
            clearSample();
            boundPort = 0;
            state = Status::unavailable;
            nextAttemptAt = now + retryDelayMs;
        }
        if (now < nextAttemptAt) return;
        clearSample();
        state = Status::waiting;
        wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (wake) worker = CreateThread(nullptr, 0, entry, this, 0, nullptr);
        if (!worker) {
            if (wake) CloseHandle(wake);
            wake = nullptr;
            state = Status::unavailable;
            nextAttemptAt = now + retryDelayMs;
        }
    }
    void stop() {
        nextAttemptAt = 0;
        if (worker) {
            SetEvent(wake);
            // Ordinary polling work is bounded; OS call latency is not guaranteed.
            if (WaitForSingleObject(worker, INFINITE) != WAIT_OBJECT_0) {
                state = Status::unavailable;
                return; // retain ownership rather than close a possibly live worker
            }
            CloseHandle(worker);
            CloseHandle(wake);
            worker = wake = nullptr;
        }
        clearSample();
        boundPort = 0;
        state = Status::off;
    }
    TrackingSample snapshot() {
        AcquireSRWLockShared(&lock);
        const auto copy = latest;
        ReleaseSRWLockShared(&lock);
        return copy;
    }
    Status status() const { return state.load(); }
    unsigned port() const { return boundPort.load(); }
private:
    HANDLE worker = nullptr, wake = nullptr;
    SRWLOCK lock = SRWLOCK_INIT;
    TrackingSample latest;
    std::atomic<Status> state{Status::off};
    unsigned generation = 0;
    static constexpr ULONGLONG retryDelayMs = 1000;
    ULONGLONG nextAttemptAt = 0; // session worker only, like worker/wake ownership
    void clearSample() {
        AcquireSRWLockExclusive(&lock);
        latest = {};
        ReleaseSRWLockExclusive(&lock);
    }
    const unsigned requestedPort;
    std::atomic<unsigned> boundPort{0};
    static DWORD WINAPI entry(void* p) { return static_cast<TrackingUdp*>(p)->receive(); }
    DWORD receive() {
        state = Status::waiting;
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) { state = Status::unavailable; return 1; }
        SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(u_short(requestedPort));
        BOOL exclusive = TRUE;
        u_long nonblocking = 1;
        const bool ready = socketHandle != INVALID_SOCKET &&
            !setsockopt(socketHandle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                        reinterpret_cast<const char*>(&exclusive), sizeof exclusive) &&
            !bind(socketHandle, reinterpret_cast<const sockaddr*>(&address), sizeof address) &&
            !ioctlsocket(socketHandle, FIONBIO, &nonblocking);
        if (!ready) {
            if (socketHandle != INVALID_SOCKET) closesocket(socketHandle);
            WSACleanup();
            state = Status::unavailable;
            return 1;
        }
        unsigned connection = ++generation;
        int addressSize = sizeof address;
        if (!getsockname(socketHandle, reinterpret_cast<sockaddr*>(&address), &addressSize))
            boundPort = ntohs(address.sin_port);
        uint64_t sequence = 0;
        sockaddr_in acceptedSender{};
        ULONGLONG lastAccepted = 0;
        bool haveSender = false;
        bool receiveFailed = false;
        while (!receiveFailed && WaitForSingleObject(wake, 4) == WAIT_TIMEOUT) {
            // Bounded work even if a sender floods the endpoint; shutdown always gets a turn.
            for (unsigned i = 0; i < 64; ++i) {
                char bytes[49]; // oversized packets must never masquerade as a six-double packet
                sockaddr_in sender{};
                int senderSize = sizeof sender;
                const int n = recvfrom(socketHandle, bytes, sizeof bytes, 0,
                                       reinterpret_cast<sockaddr*>(&sender), &senderSize);
                if (n == SOCKET_ERROR) {
                    const int error = WSAGetLastError();
                    if (error == WSAEMSGSIZE) continue;
                    // Empty queues and interrupted calls may retry after the wake poll.
                    if (error == WSAEWOULDBLOCK || error == WSAEINTR) break;
                    // Do not leave a broken receiver alive: service() can only reap
                    // and back off/restart a completed worker. Retire its pose now.
                    receiveFailed = true;
                    clearSample();
                    state = Status::unavailable;
                    break;
                }
                TrackingSample sample;
                if (!decodeTracking(bytes, size_t(n), sample.rotation)) continue;
                const ULONGLONG now = GetTickCount64();
                const bool continuous = haveSender && now >= lastAccepted &&
                                        now - lastAccepted <= trackingStaleMs;
                const bool sameSender = haveSender && sender.sin_addr.s_addr == acceptedSender.sin_addr.s_addr &&
                                        sender.sin_port == acceptedSender.sin_port;
                // Two local tracking applications must not alternate the camera pose.
                // A fresh connection owns the stream until it goes stale.
                if (continuous && !sameSender) continue;
                if (haveSender && !continuous) ++connection;
                generation = connection;
                acceptedSender = sender;
                haveSender = true;
                lastAccepted = now;
                sample.valid = true;
                sample.generation = connection;
                sample.sequence = ++sequence;
                sample.received = now;
                AcquireSRWLockExclusive(&lock);
                latest = sample;
                ReleaseSRWLockExclusive(&lock);
                state = Status::receiving;
            }
        }
        closesocket(socketHandle);
        boundPort = 0;
        WSACleanup();
        return receiveFailed ? 1 : 0;
    }
};
} // namespace fp
