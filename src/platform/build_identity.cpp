#include "build_identity.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <vector>

namespace exanima::platform {

std::string file_sha256(const wchar_t* path) {
    if (!path) return {};
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, copied = 0;
    std::array<unsigned char, 32> digest{};
    bool success = false;
    std::vector<unsigned char> object;
    std::vector<unsigned char> buffer;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<UCHAR*>(&object_size),
            sizeof(object_size), &copied, 0) >= 0) {
        try {
            object.resize(object_size);
            buffer.resize(65536);
            if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0) {
                success = true;
                for (;;) {
                    DWORD count = 0;
                    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
                        success = false;
                        break;
                    }
                    if (!count) break;
                    if (BCryptHashData(hash, buffer.data(), count, 0) < 0) {
                        success = false;
                        break;
                    }
                }
                if (success) success = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
            }
        } catch (...) {
            success = false;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!success) return {};

    static constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = digits[digest[index] >> 4];
        result[index * 2 + 1] = digits[digest[index] & 15];
    }
    return result;
}

bool supported_game_executable(const wchar_t* path) {
    return file_sha256(path) == standard_exanima_sha256;
}

} // namespace exanima::platform
