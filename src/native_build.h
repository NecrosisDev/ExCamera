#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#pragma comment(lib, "bcrypt.lib")
namespace fp {
// Source archive for static verification (Exanima 0.9.5.2g, Steam build 25803178).
// Core targets resolve by signature; optional capabilities require their complete group.
// Hook health verifies native callback execution. This build has not been play-tested.
// Hellmode differs from it in 17 bytes, outside every hooked function, and passes every native contract.
inline constexpr const char* executableHashes[] = {
    "af9033c7857565ddf2e842a486b0a158b32375a8f95e18b69f4e171b919b6f70", // 0.9.5.2g
    "9a7839d5f948d3c3fa94d57de2a047fed77c8319007519ea6946d91ad6e85815", // 0.9.5.2g Hellmode
};
inline bool supportedExecutable(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD bytes = 0, size = 0;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&size, sizeof(size), &bytes, 0) >= 0) {
        std::vector<UCHAR> object(size), buffer(65536);
        UCHAR digest[32];
        if (BCryptCreateHash(alg, &hash, object.data(), size, nullptr, 0, 0) >= 0) {
            ok = true;
            for (;;) {
                DWORD n = 0;
                if (!ReadFile(file, buffer.data(), DWORD(buffer.size()), &n, nullptr)) {
                    ok = false;
                    break;
                }
                if (!n) break;
                if (BCryptHashData(hash, buffer.data(), n, 0) < 0) {
                    ok = false;
                    break;
                }
            }
            if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
            char hex[65]{};
            if (ok) {
                for (int i = 0; i < 32; i++) sprintf_s(hex + 2 * i, 3, "%02x", digest[i]);
                ok = false;
                for (const char* known : executableHashes) ok = ok || strcmp(hex, known) == 0;
            }
            BCryptDestroyHash(hash);
        }
    }
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return ok;
}
} // namespace fp
