#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace fp {

enum class ActivationPhase { idle, starting, running, stopping, stopped, failed };
inline const char* activationPhaseText(ActivationPhase phase) {
    switch (phase) {
    case ActivationPhase::idle: return "idle";
    case ActivationPhase::starting: return "starting";
    case ActivationPhase::running: return "running";
    case ActivationPhase::stopping: return "stopping";
    case ActivationPhase::stopped: return "stopped";
    case ActivationPhase::failed: return "failed";
    }
    return "failed";
}
struct ActivationStatus {
    uint64_t processStarted = 0, request = 0;
    uint32_t pid = 0, revision = 0;
    char build[64]{};
    ActivationPhase phase = ActivationPhase::idle;
};
inline ActivationPhase effectiveActivationPhase(ActivationPhase phase, bool stopRequested) {
    return phase == ActivationPhase::running && stopRequested ? ActivationPhase::stopping : phase;
}
inline bool activationNeedsRearm(ActivationPhase phase) {
    return phase == ActivationPhase::stopping || phase == ActivationPhase::stopped;
}
enum class ActivationResult { waiting, ready, failed, versionMismatch };
inline ActivationResult activationResult(const ActivationStatus& status, uint32_t pid,
                                         uint64_t processStarted, uint64_t request,
                                         uint32_t revision, const char* build) {
    if (!request || !processStarted || status.pid != pid || status.processStarted != processStarted ||
        status.request != request)
        return ActivationResult::waiting;
    if (status.revision != revision || strcmp(status.build, build))
        return ActivationResult::versionMismatch;
    if (status.phase == ActivationPhase::running) return ActivationResult::ready;
    if (status.phase == ActivationPhase::failed) return ActivationResult::failed;
    return ActivationResult::waiting;
}

namespace activation_detail {
// Reads only the shared top-level protocol, while validating and skipping the
// diagnostic payload. Keys inside strings or nested objects cannot acknowledge a request.
class Reader {
    const char* at;
    const char* end;
    void space() {
        while (at != end && (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')) ++at;
    }
    bool take(char c) {
        space();
        if (at == end || *at != c) return false;
        ++at;
        return true;
    }
    bool literal(const char* text) {
        const size_t n = strlen(text);
        if (size_t(end - at) < n || memcmp(at, text, n)) return false;
        at += n;
        return true;
    }
    bool string(char* out = nullptr, size_t capacity = 0) {
        if (!take('"')) return false;
        size_t n = 0;
        while (at != end) {
            unsigned char c = static_cast<unsigned char>(*at++);
            if (c == '"') {
                if (out) {
                    if (n >= capacity) return false;
                    out[n] = 0;
                }
                return true;
            }
            if (c < 0x20) return false;
            if (c == '\\') {
                if (at == end) return false;
                c = static_cast<unsigned char>(*at++);
                switch (c) {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    unsigned code = 0;
                    for (unsigned i = 0; i < 4; ++i) {
                        if (at == end) return false;
                        const char digit = *at++;
                        unsigned v;
                        if (digit >= '0' && digit <= '9') v = unsigned(digit - '0');
                        else if (digit >= 'a' && digit <= 'f') v = unsigned(digit - 'a') + 10;
                        else if (digit >= 'A' && digit <= 'F') v = unsigned(digit - 'A') + 10;
                        else return false;
                        code = code * 16 + v;
                    }
                    // Protocol keys/build identifiers are ASCII; unknown values need no decoding.
                    if (out && (!code || code > 0x7f)) return false;
                    c = static_cast<unsigned char>(code);
                    break;
                }
                default: return false;
                }
            }
            if (out) {
                if (n + 1 >= capacity) return false;
                out[n] = char(c);
            }
            ++n;
        }
        return false;
    }
    bool number(uint64_t* out = nullptr) {
        space();
        if (at == end) return false;
        const bool negative = *at == '-';
        if (negative && (++at == end || out)) return false;
        if (*at < '0' || *at > '9') return false;
        uint64_t value = 0;
        const bool zero = *at == '0';
        do {
            const unsigned digit = unsigned(*at++ - '0');
            if (out && value > (UINT64_MAX - digit) / 10) return false;
            value = value * 10 + digit;
            if (zero) break;
        } while (at != end && *at >= '0' && *at <= '9');
        if (!out && at != end && *at == '.') {
            if (++at == end || *at < '0' || *at > '9') return false;
            do { ++at; } while (at != end && *at >= '0' && *at <= '9');
        }
        if (!out && at != end && (*at == 'e' || *at == 'E')) {
            if (++at == end) return false;
            if (*at == '+' || *at == '-') ++at;
            if (at == end || *at < '0' || *at > '9') return false;
            do { ++at; } while (at != end && *at >= '0' && *at <= '9');
        }
        if (out) *out = value;
        return true;
    }
    bool skip(unsigned depth = 0) {
        space();
        if (at == end || depth > 16) return false;
        if (*at == '"') return string();
        if (*at == 't') return literal("true");
        if (*at == 'f') return literal("false");
        if (*at == 'n') return literal("null");
        if (*at == '{' || *at == '[') {
            const bool object = *at++ == '{';
            const char close = object ? '}' : ']';
            if (take(close)) return true;
            do {
                if (object && (!string() || !take(':'))) return false;
                if (!skip(depth + 1)) return false;
                if (take(close)) return true;
            } while (take(','));
            return false;
        }
        return number();
    }
public:
    Reader(const char* data, size_t size) : at(data), end(data + size) {}
    bool read(ActivationStatus& result) {
        ActivationStatus status;
        unsigned fields = 0;
        if (!take('{')) return false;
        do {
            char key[80];
            if (!string(key, sizeof key) || !take(':')) return false;
            unsigned bit = 0;
            uint64_t value = 0;
            if (!strcmp(key, "pid")) {
                bit = 1;
                if (!number(&value) || value > UINT32_MAX) return false;
                status.pid = uint32_t(value);
            } else if (!strcmp(key, "revision")) {
                bit = 2;
                if (!number(&value) || value > UINT32_MAX) return false;
                status.revision = uint32_t(value);
            } else if (!strcmp(key, "build")) {
                bit = 4;
                if (!string(status.build, sizeof status.build)) return false;
            } else if (!strcmp(key, "process_started")) {
                bit = 8;
                if (!number(&status.processStarted)) return false;
            } else if (!strcmp(key, "activation_request")) {
                bit = 16;
                if (!number(&status.request)) return false;
            } else if (!strcmp(key, "activation_state")) {
                bit = 32;
                char state[24];
                if (!string(state, sizeof state)) return false;
                bool found = false;
                const ActivationPhase phases[] = {ActivationPhase::idle, ActivationPhase::starting,
                                                  ActivationPhase::running, ActivationPhase::stopping,
                                                  ActivationPhase::stopped, ActivationPhase::failed};
                for (ActivationPhase phase : phases)
                    if (!strcmp(state, activationPhaseText(phase))) {
                        status.phase = phase;
                        found = true;
                    }
                if (!found) return false;
            } else if (!skip()) return false;
            if (bit & fields) return false;
            fields |= bit;
            if (take('}')) {
                space();
                if (at != end || fields != 63) return false;
                result = status;
                return true;
            }
        } while (take(','));
        return false;
    }
};
} // namespace activation_detail

inline bool readActivationStatus(const char* data, size_t size, ActivationStatus& status) {
    if (!data || !size || size > 64 * 1024) return false;
    return activation_detail::Reader(data, size).read(status);
}

} // namespace fp
