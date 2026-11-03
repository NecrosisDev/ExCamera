#include "settings_store.hpp"
#include "settings_worker.hpp"

#include <filesystem>
#include <fstream>

#include "../test_support.hpp"

namespace {

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "excamera-settings-tests";
    TemporaryDirectory() {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

bool near(float left, float right, float tolerance = 1e-5f) {
    return std::fabs(left - right) <= tolerance;
}

bool atomic_round_trip_preserves_values() {
    TemporaryDirectory temporary;
    const std::filesystem::path file = temporary.path / "controller.ini";
    excamera::CameraSettings settings;
    settings.first_fov = 90.f;
    settings.third_fov = 0.f;
    settings.look_sensitivity = 0.005f;
    settings.third_distance = 310.f;
    settings.third_shoulder = -25.f;
    CHECK(excamera::settings::save(file, settings));
    CHECK(!std::filesystem::exists(file.wstring() + L".tmp"));
    const excamera::CameraSettings loaded = excamera::settings::load(file);
    CHECK(near(loaded.first_fov, 90.f));
    CHECK(near(loaded.third_fov, 0.f));
    CHECK(near(loaded.look_sensitivity, 0.005f));
    CHECK(near(loaded.third_distance, 310.f));
    CHECK(near(loaded.third_shoulder, -25.f));
    return true;
}

bool worker_coalesces_and_flushes_latest_settings() {
    TemporaryDirectory temporary;
    const std::filesystem::path file = temporary.path / "controller.ini";
    excamera::settings::Worker worker;
    CHECK(worker.start(file));
    excamera::CameraSettings settings;
    settings.first_fov = 80.f;
    worker.request(settings);
    settings.first_fov = 95.f;
    worker.request(settings);
    worker.stop();
    const excamera::CameraSettings loaded = excamera::settings::load(file);
    CHECK(near(loaded.first_fov, 95.f));
    CHECK(worker.error_count() == 0);
    return true;
}

bool invalid_files_and_values_fall_back_safely() {
    TemporaryDirectory temporary;
    const std::filesystem::path file = temporary.path / "controller.ini";
    {
        std::ofstream output(file, std::ios::trunc);
        output << "not a settings file";
    }
    excamera::CameraSettings loaded = excamera::settings::load(file);
    CHECK(near(loaded.first_fov, 75.f));
    CHECK(near(loaded.third_fov, 82.f));
    excamera::CameraSettings invalid;
    invalid.first_fov = 1000.f;
    invalid.look_sensitivity = -1.f;
    CHECK(excamera::settings::save(file, invalid));
    loaded = excamera::settings::load(file);
    CHECK(near(loaded.first_fov, 130.f));
    CHECK(near(loaded.look_sensitivity, 0.0001f));
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"atomic round trip preserves values", atomic_round_trip_preserves_values},
        {"worker coalesces and flushes latest settings", worker_coalesces_and_flushes_latest_settings},
        {"invalid files and values fall back safely", invalid_files_and_values_fall_back_safely},
    });
}
