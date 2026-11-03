#include "build_identity.hpp"

#include <filesystem>
#include <fstream>

#include "../test_support.hpp"

namespace {

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "excamera-build-identity-test";
    TemporaryDirectory() {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

bool sha256_matches_known_abc_vector() {
    TemporaryDirectory temporary;
    const std::filesystem::path file = temporary.path / "abc.bin";
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << "abc";
    }
    const std::string digest = exanima::platform::file_sha256(file.c_str());
    CHECK(digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(!exanima::platform::supported_game_executable(file.c_str()));
    return true;
}

bool missing_or_invalid_files_fail_closed() {
    TemporaryDirectory temporary;
    const std::filesystem::path missing = temporary.path / "missing.exe";
    CHECK(exanima::platform::file_sha256(missing.c_str()).empty());
    CHECK(exanima::platform::file_sha256(nullptr).empty());
    CHECK(!exanima::platform::supported_game_executable(missing.c_str()));
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"sha256 matches known abc vector", sha256_matches_known_abc_vector},
        {"missing or invalid files fail closed", missing_or_invalid_files_fail_closed},
    });
}
