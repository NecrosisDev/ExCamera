#include "native_camera.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

#include "build_identity.hpp"
#include "collision.hpp"
#include "controller.hpp"
#include "diagnostics.hpp"
#include "frustum.hpp"
#include "input.hpp"
#include "lifecycle.hpp"
#include "settings_store.hpp"
#include "settings_worker.hpp"

namespace exanima::camera {
namespace {

using NavSampler = float(*)(void*, float);
using HeadSampler = float(*)(uintptr_t, float);
using Projection = void(*)(uintptr_t);
using CameraMouse = char(*)();
using Snapshots = void(*)();
using Present = void(*)();
using LevelRay = bool(*)(uintptr_t, const float*, const float*, float);
using LevelMeshTrace = int(*)(uintptr_t, const float*, const float*, float*);

constexpr uintptr_t level_mesh_trace_rva = 0x602d0;
constexpr uintptr_t nav_sampler_rva = 0x94720;
constexpr uintptr_t head_sampler_rva = 0x8b6e0;
constexpr uintptr_t projection_rva = 0x9f2e0;
constexpr uintptr_t camera_mouse_rva = 0x11ef40;
constexpr uintptr_t snapshots_rva = 0x11eb50;
constexpr uintptr_t present_rva = 0x21e60;
constexpr uintptr_t level_ray_rva = 0x1430f0;
constexpr uintptr_t level_ray_secondary_rva = 0x1431c0;
constexpr uintptr_t player_root_rva = 0x4f0830;
constexpr uintptr_t current_scene_rva = 0x49b300;
constexpr uintptr_t main_viewport_rva = 0x4efc90;
constexpr uintptr_t gui_manager_rva = 0x49a960;
constexpr uintptr_t ui_open_count_rva = 0x499860;
constexpr uintptr_t camera_mouse_active_rva = 0x4efd00;
constexpr uintptr_t camera_mouse_turn_rva = 0x4efd10;
constexpr uintptr_t key_word_rva = 0x49cdc0;

constexpr auto nav_prefix = std::to_array<uint8_t>({
    0x53, 0x48, 0x8d, 0x64, 0x24, 0xa0, 0x66, 0x0f, 0x7f, 0x74, 0x24, 0x40,
    0x66, 0x44, 0x0f, 0x7f, 0x44, 0x24, 0x50, 0x48, 0x89, 0xcb, 0x0f, 0x28, 0xf1,
});
constexpr auto mouse_prefix = std::to_array<uint8_t>({
    0x53, 0x57, 0x56, 0x48, 0x8d, 0x64, 0x24, 0xa0, 0x30, 0xdb, 0x48, 0x8b,
});
constexpr auto present_prefix = std::to_array<uint8_t>({
    0x48, 0x8d, 0x64, 0x24, 0xd8, 0x48, 0x83, 0x3d, 0x93, 0x6a, 0x47, 0x00, 0x00,
});
constexpr auto level_ray_prefix = std::to_array<uint8_t>({
    0x53, 0x57, 0x56, 0x41, 0x54, 0x41, 0x55, 0x48, 0x8d, 0x64, 0x24, 0xb0,
    0x48, 0x89, 0xcb, 0xf3, 0x0f, 0x11, 0x5c, 0x24, 0x20, 0x48, 0x8b, 0x02,
});
constexpr auto level_mesh_trace_prefix = std::to_array<uint8_t>({
    0x53, 0x57, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
    0x8d, 0xa4, 0x24, 0x60, 0xff, 0xff, 0xff, 0x66, 0x0f, 0x7f, 0x74, 0x24, 0x70,
});

static_assert(nav_prefix.size() <= 32 && mouse_prefix.size() <= 32 && present_prefix.size() <= 32);

template <typename T>
T read(uintptr_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

template <typename T>
void write(uintptr_t address, const T& value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}

bool readable(uintptr_t address, SIZE_T size) {
    if (!address || !size) return false;
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &information, sizeof(information)) != sizeof(information))
        return false;
    if (information.State != MEM_COMMIT || (information.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_EXECUTE)))
        return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(information.BaseAddress);
    if (address < begin) return false;
    const uintptr_t offset = address - begin;
    return offset <= information.RegionSize && size <= information.RegionSize - offset;
}

excamera::Pose pose_from(const float* values) {
    return {
        {values[0], values[1], values[2]},
        {values[3], values[4], values[5]},
        {values[6], values[7], values[8]},
        {values[9], values[10], values[11]},
    };
}

void pose_to(uintptr_t address, const excamera::Pose& pose) {
    const float values[12]{
        pose.right.x, pose.right.y, pose.right.z,
        pose.up.x, pose.up.y, pose.up.z,
        pose.back.x, pose.back.y, pose.back.z,
        pose.origin.x, pose.origin.y, pose.origin.z,
    };
    std::memcpy(reinterpret_cast<void*>(address), values, sizeof(values));
}

struct OwnedCamera {
    uintptr_t view = 0;
    uintptr_t camera = 0;
    uintptr_t controller = 0;
    uintptr_t root = 0;
    uintptr_t scene = 0;
    std::array<float, 12> pose{};
    std::array<float, 12> pose2{};
    std::array<float, 16> expanded{};
    float fov = 75.f;
    float near_plane = 100.f;
    uint32_t dirty = 0;
    bool valid = false;
};

class NativeCamera {
public:
    Result start(HookBroker& broker) noexcept {
        base_ = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"Exanima.exe"));
        if (!base_) return EXA_RESULT_UNSUPPORTED;
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(base_), path.data(),
            static_cast<DWORD>(path.size()));
        if (!length || length >= path.size() || !platform::supported_game_executable(path.data()))
            return EXA_RESULT_UNSUPPORTED;
        for (const uintptr_t rva : {level_ray_rva, level_ray_secondary_rva}) {
            if (!readable(base_ + rva, level_ray_prefix.size()) ||
                std::memcmp(reinterpret_cast<const void*>(base_ + rva), level_ray_prefix.data(),
                            level_ray_prefix.size()) != 0)
                return EXA_RESULT_UNSUPPORTED;
        }
        if (!readable(base_ + level_mesh_trace_rva, level_mesh_trace_prefix.size()) ||
            std::memcmp(reinterpret_cast<const void*>(base_ + level_mesh_trace_rva),
                        level_mesh_trace_prefix.data(), level_mesh_trace_prefix.size()) != 0)
            return EXA_RESULT_UNSUPPORTED;

        settings_ = excamera::settings::load(excamera::settings::path());
        controller_ = excamera::CameraController(settings_);
        controller_.set_mode(excamera::CameraMode::first_person);
        lifecycle_.reset();
        if (!settings_worker_.start(excamera::settings::path())) return EXA_RESULT_BACKEND_FAILURE;
        mode_ = excamera::CameraMode::first_person;
        look_ = {};
        look_valid_ = false;
        owned_ = {};
        original_nav_ = nullptr;
        original_camera_mouse_ = nullptr;
        original_present_ = nullptr;
        projection_ = reinterpret_cast<Projection>(base_ + projection_rva);
        head_sampler_ = reinterpret_cast<HeadSampler>(base_ + head_sampler_rva);
        copy_snapshots_ = reinterpret_cast<Snapshots>(base_ + snapshots_rva);
        level_ray_ = reinterpret_cast<LevelRay>(base_ + level_ray_rva);
        level_ray_secondary_ = reinterpret_cast<LevelRay>(base_ + level_ray_secondary_rva);
        mesh_trace_ = reinterpret_cast<LevelMeshTrace>(base_ + level_mesh_trace_rva);
        validated_level_ = 0;
        faults_.store(0, std::memory_order_relaxed);
        restoration_failed_.store(false, std::memory_order_relaxed);
        last_error_.store(EXA_RESULT_OK, std::memory_order_relaxed);
        started_ = false;
        accepting_.store(false, std::memory_order_release);
        last_tick_ = 0;
        restore_requested_.store(false, std::memory_order_relaxed);
        cursor_restore_requested_.store(false, std::memory_order_relaxed);
        cursor_owned_ = false;
        cursor_valid_ = false;
        LARGE_INTEGER performance_frequency{};
        QueryPerformanceFrequency(&performance_frequency);
        performance_frequency_ = performance_frequency.QuadPart > 0
            ? static_cast<uint64_t>(performance_frequency.QuadPart) : 1;
        camera_samples_ = 0;
        collision_mesh_traces_ = 0;
        collision_nanoseconds_ = 0;
        collision_queries_ = 0;
        collision_failures_ = 0;
        level_available_ = false;
        query_failed_ = false;
        custom_presented_ = false;
        last_clearance_ = 0.f;
        applied_distance_ = 0.f;
        diagnostic_pivot_ = {};
        diagnostic_back_ = {};
        diagnostic_ground_ = {};
        input_.reset(GetAsyncKeyState(VK_F6) < 0);

        HookDefinition definition{
            "excamera.camera.nav_sampler",
            reinterpret_cast<void*>(base_ + nav_sampler_rva),
            reinterpret_cast<void*>(&nav_detour),
            reinterpret_cast<void**>(&original_nav_),
            nav_prefix,
        };
        Result result = broker.install(definition);
        if (result != EXA_RESULT_OK) {
            settings_worker_.stop();
            return result;
        }
        HookDefinition mouse_definition{
            "excamera.input.camera_mouse",
            reinterpret_cast<void*>(base_ + camera_mouse_rva),
            reinterpret_cast<void*>(&mouse_detour),
            reinterpret_cast<void**>(&original_camera_mouse_),
            mouse_prefix,
        };
        result = broker.install(mouse_definition);
        if (result != EXA_RESULT_OK) {
            settings_worker_.stop();
            return result;
        }
        HookDefinition present_definition{
            "excamera.lifecycle.present",
            reinterpret_cast<void*>(base_ + present_rva),
            reinterpret_cast<void*>(&present_detour),
            reinterpret_cast<void**>(&original_present_),
            present_prefix,
        };
        result = broker.install(present_definition);
        if (result != EXA_RESULT_OK) {
            settings_worker_.stop();
            return result;
        }
        started_ = true;
        accepting_.store(true, std::memory_order_release);
        return EXA_RESULT_OK;
    }

    Result stop() noexcept {
        if (!started_ && !cursor_owned_) return EXA_RESULT_OK;
        accepting_.store(false, std::memory_order_release);
        restore_requested_.store(true, std::memory_order_release);
        if (cursor_owned_) cursor_restore_requested_.store(true, std::memory_order_release);
        const ULONGLONG began = GetTickCount64();
        while (restore_requested_.load(std::memory_order_acquire) ||
               cursor_restore_requested_.load(std::memory_order_acquire)) {
            if (GetTickCount64() - began >= 2000) {
                restoration_failed_.store(true, std::memory_order_relaxed);
                last_error_.store(EXA_RESULT_BACKEND_FAILURE, std::memory_order_relaxed);
                return EXA_RESULT_BACKEND_FAILURE;
            }
            Sleep(1);
        }
        settings_worker_.stop();
        AcquireSRWLockExclusive(&lock_);
        controller_.reset();
        lifecycle_.reset();
        input_.reset(GetAsyncKeyState(VK_F6) < 0);
        mode_ = excamera::CameraMode::first_person;
        look_valid_ = false;
        cursor_valid_ = false;
        started_ = false;
        ReleaseSRWLockExclusive(&lock_);
        return restoration_failed_.load(std::memory_order_relaxed)
            ? EXA_RESULT_BACKEND_FAILURE : EXA_RESULT_OK;
    }

    Result set_mode(uint32_t mode) noexcept {
        if (!started_ || mode > static_cast<uint32_t>(excamera::CameraMode::overhead))
            return EXA_RESULT_INVALID_STATE;
        AcquireSRWLockExclusive(&lock_);
        mode_ = static_cast<excamera::CameraMode>(mode);
        controller_.set_mode(mode_);
        cursor_valid_ = false;
        ReleaseSRWLockExclusive(&lock_);
        return EXA_RESULT_OK;
    }

    Result set_distance(float distance) noexcept {
        if (!started_) return EXA_RESULT_INVALID_STATE;
        if (!std::isfinite(distance) || distance < 0.f || distance > 1200.f)
            return EXA_RESULT_INVALID_ARGUMENT;
        AcquireSRWLockExclusive(&lock_);
        settings_.third_distance = distance;
        controller_.set_settings(settings_);
        settings_worker_.request(settings_);
        ReleaseSRWLockExclusive(&lock_);
        return EXA_RESULT_OK;
    }

    Result set_look(float yaw, float pitch) noexcept {
        if (!started_ || !std::isfinite(yaw) || !std::isfinite(pitch)) return EXA_RESULT_INVALID_ARGUMENT;
        AcquireSRWLockExclusive(&lock_);
        look_ = excamera::sanitize_look({yaw, pitch});
        look_valid_ = true;
        ReleaseSRWLockExclusive(&lock_);
        return EXA_RESULT_OK;
    }

    uint32_t faults() const noexcept {
        return faults_.load(std::memory_order_relaxed) + settings_worker_.error_count();
    }
    bool restoration_failed() const noexcept { return restoration_failed_.load(std::memory_order_relaxed); }
    Result last_error() const noexcept {
        return settings_worker_.error_count() ? EXA_RESULT_BACKEND_FAILURE : last_error_.load(std::memory_order_relaxed);
    }

    Result diagnostics(ExaCameraDiagnostics& output) noexcept {
        AcquireSRWLockShared(&lock_);
        const bool active = started_;
        bool written = false;
        if (active) {
            ExaCameraDiagnostics snapshot{};
            snapshot.struct_size = sizeof(snapshot);
            snapshot.abi_version = EXA_CAMERA_DIAGNOSTICS_ABI_VERSION;
            snapshot.mode = static_cast<ExaCameraMode>(mode_);
            snapshot.flags = (level_available_ ? EXA_CAMERA_DIAGNOSTICS_LEVEL_AVAILABLE : 0u) |
                (query_failed_ ? EXA_CAMERA_DIAGNOSTICS_QUERY_FAILED : 0u) |
                (cursor_owned_.load(std::memory_order_relaxed) ? EXA_CAMERA_DIAGNOSTICS_CURSOR_OWNED : 0u) |
                (custom_presented_ ? EXA_CAMERA_DIAGNOSTICS_CUSTOM_PRESENTED : 0u) |
                (diagnostic_ground_.available ? EXA_CAMERA_DIAGNOSTICS_GROUND_AVAILABLE : 0u);
            snapshot.collision_queries = collision_queries_;
            snapshot.collision_failures = collision_failures_;
            snapshot.clearance = last_clearance_;
            snapshot.applied_distance = applied_distance_;
            snapshot.pivot[0] = diagnostic_pivot_.x;
            snapshot.pivot[1] = diagnostic_pivot_.y;
            snapshot.pivot[2] = diagnostic_pivot_.z;
            snapshot.back[0] = diagnostic_back_.x;
            snapshot.back[1] = diagnostic_back_.y;
            snapshot.back[2] = diagnostic_back_.z;
            snapshot.camera_samples = camera_samples_;
            snapshot.collision_mesh_traces = collision_mesh_traces_;
            snapshot.collision_nanoseconds = collision_nanoseconds_;
            snapshot.support_height = diagnostic_ground_.available ? diagnostic_ground_.support_height : 0.f;
            snapshot.minimum_camera_y = diagnostic_ground_.available
                ? diagnostic_ground_.support_height + excamera::ground_margin_cm : 0.f;
            written = excamera::copy_camera_diagnostics(snapshot, output);
        }
        ReleaseSRWLockShared(&lock_);
        return active ? (written ? EXA_RESULT_OK : EXA_RESULT_INVALID_ARGUMENT) : EXA_RESULT_INVALID_STATE;
    }

    static float nav_detour(void* self, float offset) {
        return instance().nav(self, offset);
    }

    static NativeCamera& instance() {
        static NativeCamera camera;
        return camera;
    }

private:
    float nav(void* self, float offset) noexcept {
        if (!original_nav_) return 0.f;
        const float result = original_nav_(self, offset);
        if (restore_requested_.load(std::memory_order_acquire)) {
            AcquireSRWLockExclusive(&lock_);
            __try {
                restore_after_native();
            } __finally {
                restore_requested_.store(false, std::memory_order_release);
                ReleaseSRWLockExclusive(&lock_);
            }
            return result;
        }
        if (!accepting_.load(std::memory_order_acquire)) return result;
        if (TryAcquireSRWLockExclusive(&lock_) == FALSE) return result;
        __try {
            __try {
                apply(self, offset);
            } __finally {
                ReleaseSRWLockExclusive(&lock_);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            handle_fault();
        }
        return result;
    }

    static void present_detour() {
        instance().present();
    }

    void present() noexcept {
        if (!original_present_) return;
        original_present_();
        if (!restore_requested_.load(std::memory_order_acquire) &&
            !cursor_restore_requested_.load(std::memory_order_acquire)) return;
        AcquireSRWLockExclusive(&lock_);
        __try {
            restore_after_native();
            if (cursor_restore_requested_.load(std::memory_order_acquire)) {
                release_cursor();
                cursor_restore_requested_.store(false, std::memory_order_release);
            }
        } __finally {
            restore_requested_.store(false, std::memory_order_release);
            ReleaseSRWLockExclusive(&lock_);
        }
    }

    static char mouse_detour() {
        return instance().camera_mouse();
    }

    char camera_mouse() noexcept {
        if (!original_camera_mouse_) return 0;
        if (cursor_restore_requested_.load(std::memory_order_acquire)) {
            AcquireSRWLockExclusive(&lock_);
            release_cursor();
            cursor_restore_requested_.store(false, std::memory_order_release);
            ReleaseSRWLockExclusive(&lock_);
            return original_camera_mouse_();
        }
        if (!started_ || !accepting_.load(std::memory_order_acquire)) return original_camera_mouse_();
        bool suppress = false;
        if (TryAcquireSRWLockExclusive(&lock_) == FALSE) return original_camera_mouse_();
        __try {
            __try {
                suppress = evaluate_mouse();
            } __finally {
                ReleaseSRWLockExclusive(&lock_);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            handle_fault();
            suppress = false;
        }
        return finish_mouse(suppress);
    }

    char finish_mouse(bool suppress) noexcept {
        if (!suppress) return original_camera_mouse_();
        write<uint8_t>(base_ + camera_mouse_active_rva, 0);
        write<uint8_t>(base_ + camera_mouse_turn_rva, 0);
        write<uint32_t>(base_ + key_word_rva, read<uint32_t>(base_ + key_word_rva) & ~0x300u);
        copy_snapshots_();
        return 0;
    }

    void release_cursor() noexcept {
        if (cursor_owned_) {
            ClipCursor(nullptr);
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            cursor_owned_ = false;
        }
        cursor_valid_ = false;
    }

    void capture_cursor(HWND window, const RECT& client) noexcept {
        POINT top_left{client.left, client.top};
        POINT bottom_right{client.right, client.bottom};
        ClientToScreen(window, &top_left);
        ClientToScreen(window, &bottom_right);
        RECT clip{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
        ClipCursor(&clip);
        SetCursor(nullptr);
        cursor_owned_ = true;

        POINT center{client.right / 2, client.bottom / 2};
        ClientToScreen(window, &center);
        if (!cursor_valid_) {
            SetCursorPos(center.x, center.y);
            cursor_valid_ = true;
            return;
        }
        POINT current{};
        if (!GetCursorPos(&current)) {
            cursor_valid_ = false;
            return;
        }
        const float dx = static_cast<float>(current.x - center.x);
        const float dy = static_cast<float>(current.y - center.y);
        SetCursorPos(center.x, center.y);
        if (!dx && !dy) return;
        look_ = excamera::sanitize_look({look_.yaw + dx * settings_.look_sensitivity,
            look_.pitch - dy * settings_.look_sensitivity});
        look_valid_ = true;
    }

    bool hotkey(int key, bool& previous) noexcept {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool edge = down && !previous;
        previous = down;
        return edge;
    }

    void adjust_fov(float delta) noexcept {
        settings_.first_fov += delta;
        if (settings_.third_fov > 0.f) settings_.third_fov += delta;
        settings_ = excamera::sanitize_camera_settings(settings_);
        controller_.set_settings(settings_);
        settings_worker_.request(settings_);
    }

    void reset_fov() noexcept {
        settings_.first_fov = 75.f;
        settings_.third_fov = 82.f;
        controller_.set_settings(settings_);
        settings_worker_.request(settings_);
    }

    bool evaluate_mouse() noexcept {
        const HWND window = GetForegroundWindow();
        DWORD process_id = 0;
        if (window) GetWindowThreadProcessId(window, &process_id);
        const bool focused = window && process_id == GetCurrentProcessId();
        RECT client{};
        const bool client_valid = focused && GetClientRect(window, &client) &&
            client.right > 0 && client.bottom > 0;
        if (client_valid) aspect_ = static_cast<float>(client.right) / static_cast<float>(client.bottom);
        const uintptr_t gui = read<uintptr_t>(base_ + gui_manager_rva);
        const bool native_ui = read<int>(base_ + ui_open_count_rva) > 0 ||
            (gui && (read<uintptr_t>(gui + 0x48) != 0 || read<uintptr_t>(gui + 0x70) != 0));
        if (!native_ui && mode_ != excamera::CameraMode::overhead && started_) {
            if (hotkey(VK_F9, f9_down_)) adjust_fov(-5.f);
            if (hotkey(VK_F10, f10_down_)) adjust_fov(5.f);
            if (hotkey(VK_F11, f11_down_)) reset_fov();
        } else {
            f9_down_ = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            f10_down_ = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
            f11_down_ = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        }

        excamera::InputSnapshot snapshot;
        snapshot.mode = mode_;
        snapshot.focused = focused;
        snapshot.native_ui_active = native_ui;
        snapshot.client_valid = client_valid;
        snapshot.f6_down = GetAsyncKeyState(VK_F6) < 0;
        const excamera::InputOutput output = input_.evaluate(snapshot);
        if (output.change_mode) {
            mode_ = output.next_mode;
            controller_.set_mode(mode_);
        }
        if (output.restore_cursor) release_cursor();
        if (output.capture_cursor) capture_cursor(window, client);
        if (output.reset_deltas) cursor_valid_ = false;
        return output.suppress_native;
    }

    void handle_fault() noexcept {
        faults_.fetch_add(1, std::memory_order_relaxed);
        last_error_.store(EXA_RESULT_BACKEND_FAILURE, std::memory_order_relaxed);
        accepting_.store(false, std::memory_order_release);
        AcquireSRWLockExclusive(&lock_);
        __try {
            release_cursor();
            if (!restore_owned()) restoration_failed_.store(true, std::memory_order_relaxed);
        } __finally {
            ReleaseSRWLockExclusive(&lock_);
        }
    }

    void capture_owned(uintptr_t view, uintptr_t camera, uintptr_t controller, uintptr_t root, uintptr_t scene) {
        owned_.view = view;
        owned_.camera = camera;
        owned_.controller = controller;
        owned_.root = root;
        owned_.scene = scene;
        std::memcpy(owned_.pose.data(), reinterpret_cast<const void*>(camera + 0x40), 48);
        std::memcpy(owned_.pose2.data(), reinterpret_cast<const void*>(camera + 0x70), 48);
        std::memcpy(owned_.expanded.data(), reinterpret_cast<const void*>(camera + 0xa0), 64);
        owned_.fov = read<float>(camera + 0x1c8);
        owned_.near_plane = read<float>(camera + 0x1cc);
        owned_.dirty = read<uint32_t>(camera + 0xe4);
        owned_.valid = true;
    }

    bool restore_owned() noexcept {
        if (!owned_.valid) return true;
        const OwnedCamera saved = owned_;
        owned_.valid = false;
        if (!readable(saved.camera, 0x1e0) || !readable(saved.view, 0x130)) return true;
        __try {
            if (read<uintptr_t>(saved.camera + 0x160) != saved.controller) return true;
            if (saved.root && read<uintptr_t>(base_ + player_root_rva) != saved.root) return true;
            if (saved.scene && read<uintptr_t>(base_ + current_scene_rva) != saved.scene) return true;
            std::memcpy(reinterpret_cast<void*>(saved.camera + 0x40), saved.pose.data(), 48);
            std::memcpy(reinterpret_cast<void*>(saved.camera + 0x70), saved.pose2.data(), 48);
            std::memcpy(reinterpret_cast<void*>(saved.camera + 0xa0), saved.expanded.data(), 64);
            write<float>(saved.camera + 0x1c8, saved.fov);
            write<float>(saved.camera + 0x1cc, saved.near_plane);
            write<uint32_t>(saved.camera + 0xe4, saved.dirty);
            projection_(saved.view);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void restore_after_native() noexcept {
        if (!owned_.valid) return;
        const uintptr_t view = owned_.view;
        owned_.valid = false;
        __try {
            if (readable(view, 0x130)) projection_(view);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            restoration_failed_.store(true, std::memory_order_relaxed);
            last_error_.store(EXA_RESULT_BACKEND_FAILURE, std::memory_order_relaxed);
        }
    }

    bool sample_head(uintptr_t head, uintptr_t mesh, float offset, excamera::Pose& pose) {
        const uintptr_t table = read<uintptr_t>(head);
        if (read<uintptr_t>(table + 0x140) != base_ + head_sampler_rva) return false;
        head_sampler_(head, offset);
        if (read<uintptr_t>(head + 0x18) != mesh || read<uintptr_t>(mesh + 0x160) != head) return false;
        const float* sampled = reinterpret_cast<const float*>(mesh + 0xa0);
        pose = excamera::strided_pose(sampled);
        return excamera::finite(pose);
    }

    excamera::GroundConstraint sample_ground(uintptr_t root, const excamera::Vec3& eye,
                                             const excamera::Vec3& sector_origin) noexcept {
        std::array<excamera::GroundContact, excamera::max_ground_contacts> contacts{};
        std::size_t count = 0;
        const int leg_count = read<int>(root + 0x9d8) + 1;
        if (leg_count < 0 || leg_count > 16) return {};
        const uintptr_t legs = read<uintptr_t>(root + 0x9e0);
        if (leg_count && !readable(legs, static_cast<SIZE_T>(leg_count) * sizeof(uintptr_t))) return {};
        for (int leg_index = 0; leg_index < leg_count && count < contacts.size(); ++leg_index) {
            const uintptr_t leg = read<uintptr_t>(legs + leg_index * sizeof(uintptr_t));
            if (!leg || !readable(leg, 0x5b4) || read<int>(leg + 0x5b0) != 0x10) continue;
            const int contact_count = read<int>(leg + 0x338);
            if (contact_count < 0 || contact_count >= 16) continue;
            for (int contact_index = 0; contact_index <= contact_count && count < contacts.size(); ++contact_index) {
                const uintptr_t contact = read<uintptr_t>(leg + 0x340 + contact_index * 0x98);
                if (!contact || !readable(contact, 0x98)) continue;
                const uint32_t flags = read<uint32_t>(contact + 0x10);
                if (!(flags & excamera::ground_support_flag)) continue;
                contacts[count++] = excamera::GroundContact{{
                    read<float>(contact + 0x3c) + sector_origin.x,
                    read<float>(contact + 0x40) + sector_origin.y,
                    read<float>(contact + 0x44) + sector_origin.z,
                }, flags};
            }
        }
        return excamera::evaluate_ground_contacts(contacts.data(), count, eye);
    }

    uintptr_t level_grid(uintptr_t viewport) const noexcept {
        __try {
            if (!readable(viewport + 0x1f0, sizeof(uintptr_t))) return 0;
            const uintptr_t manager = read<uintptr_t>(viewport + 0x1f0);
            if (!readable(manager, 0x328) || !(read<uint32_t>(manager + 0xc) & 0x2000000)) return 0;
            const uintptr_t level = read<uintptr_t>(manager + 0x320);
            return readable(level, 0x28) ? level : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    int level_blocked(uintptr_t level, const excamera::Vec3& origin,
                      const excamera::Vec3& direction, float length) noexcept {
        if (!level || !level_ray_ || !level_ray_secondary_) return -1;
        const float native_origin[3]{origin.x, origin.y, origin.z};
        const float native_direction[3]{direction.x, direction.y, direction.z};
        __try {
            return level_ray_(level, native_origin, native_direction, length) ||
                level_ray_secondary_(level, native_origin, native_direction, length) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    bool trace_distance(uintptr_t mesh, const excamera::Vec3& global_origin,
                        const excamera::Vec3& direction, float length, float& distance) noexcept {
        distance = std::numeric_limits<float>::infinity();
        if (!mesh || !mesh_trace_ || !readable(mesh, 0x2b0)) return false;
        const excamera::Vec3 origin = global_origin - excamera::Vec3{
            read<float>(mesh + 0x64), read<float>(mesh + 0x68), read<float>(mesh + 0x6c)};
        const float native_origin[3]{origin.x, origin.y, origin.z};
        const float native_direction[3]{direction.x, direction.y, direction.z};
        ++collision_mesh_traces_;
        __try {
            float remaining = length;
            const int hit = mesh_trace_(mesh, native_origin, native_direction, &remaining);
            if (hit < 0) return true;
            if (!std::isfinite(remaining) || remaining < 0.f || remaining > length) return false;
            distance = remaining;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    excamera::SegmentHits level_trace(uintptr_t level, const excamera::Vec3& origin,
                                      const excamera::Vec3& direction, float length) noexcept {
        excamera::SegmentHits hits;
        ++collision_queries_;
        __try {
            const int count = read<int>(level + 0x1c);
            const uintptr_t entries = read<uintptr_t>(level + 0x20);
            if (count < 0 || count > 4096 || (count && !entries) ||
                (count && !readable(entries, static_cast<SIZE_T>(count) * 0x54))) {
                hits.failed = true;
            } else {
                int geometry_count = 0;
                const excamera::Vec3 end = origin + direction * length;
                for (int index = 0; index < count && !hits.failed; ++index) {
                    const uintptr_t entry = entries + static_cast<uintptr_t>(index) * 0x54;
                    if (!(read<uint32_t>(entry + 0x10) & 1)) continue;
                    for (int side = 0; side < 2 && !hits.failed; ++side) {
                        const uintptr_t mesh = read<uintptr_t>(entry + side * 8);
                        if (!mesh) continue;
                        ++geometry_count;
                        float distance;
                        if (!trace_distance(mesh, origin, direction, length, distance)) {
                            hits.failed = true;
                            break;
                        }
                        if (distance < hits.from_origin) hits.from_origin = distance;
                        if (!trace_distance(mesh, end, -direction, length, distance)) {
                            hits.failed = true;
                            break;
                        }
                        if (distance < hits.from_end) hits.from_end = distance;
                    }
                }
                if (!geometry_count) hits.failed = true;
            }
            if (!hits.failed && validated_level_ != level) {
                const excamera::Vec3 end = origin + direction * length;
                const int forward = level_blocked(level, origin, direction, length);
                const int reverse = level_blocked(level, end, -direction, length);
                const bool forward_hit = hits.from_origin < length;
                const bool reverse_hit = hits.from_end < length;
                if (forward < 0 || reverse < 0 || (forward == 1) != forward_hit ||
                    (reverse == 1) != reverse_hit)
                    hits.failed = true;
                else
                    validated_level_ = level;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hits.failed = true;
        }
        if (hits.failed) {
            ++collision_failures_;
            query_failed_ = true;
        }
        return hits;
    }

    void record_collision_time(const LARGE_INTEGER& started) noexcept {
        LARGE_INTEGER finished{};
        QueryPerformanceCounter(&finished);
        if (finished.QuadPart <= started.QuadPart) return;
        const uint64_t ticks = static_cast<uint64_t>(finished.QuadPart - started.QuadPart);
        const uint64_t frequency = performance_frequency_ ? performance_frequency_ : 1;
        const uint64_t seconds = ticks / frequency;
        const uint64_t remainder = ticks % frequency;
        collision_nanoseconds_ += seconds * 1000000000ull + remainder * 1000000000ull / frequency;
    }

    excamera::SegmentHits level_probe(uintptr_t viewport, const excamera::Vec3& origin,
                                      const excamera::Vec3& direction, float length) noexcept {
        LARGE_INTEGER started{};
        QueryPerformanceCounter(&started);
        const uintptr_t level = level_grid(viewport);
        level_available_ = level != 0;
        excamera::SegmentHits hits;
        if (!level) hits.failed = true;
        else hits = level_trace(level, origin, direction, length);
        record_collision_time(started);
        return hits;
    }

    float level_clearance(uintptr_t viewport, const excamera::Vec3& origin,
                          const excamera::Vec3& direction, float length) noexcept {
        return excamera::segment_clearance(origin, direction, length,
            [this, viewport, &origin, &direction](float probe_length) noexcept {
                return level_probe(viewport, origin, direction, probe_length);
            });
    }

    void apply(void* self, float offset) {
        ++camera_samples_;
        const uintptr_t viewport = read<uintptr_t>(base_ + main_viewport_rva);
        const uintptr_t root = read<uintptr_t>(base_ + player_root_rva);
        if (!viewport || !root) {
            owned_.valid = false;
            return;
        }
        const uintptr_t view = read<uintptr_t>(viewport + 0x1a8);
        const uintptr_t camera = view ? read<uintptr_t>(view + 0x50) : 0;
        if (!view || !camera || read<uintptr_t>(camera + 0x160) != reinterpret_cast<uintptr_t>(self)) {
            owned_.valid = false;
            return;
        }
        const uintptr_t head = read<uintptr_t>(root + 0x11e0);
        const uintptr_t mesh = head ? read<uintptr_t>(head + 0x18) : 0;
        const uintptr_t sector = mesh ? read<uintptr_t>(mesh + 0x130) : 0;
        if (!head || !mesh || !sector || read<uintptr_t>(head + 0x580) != root) {
            owned_.valid = false;
            return;
        }

        excamera::CameraFrame frame;
        frame.identity = {root, read<uintptr_t>(base_ + current_scene_rva), head, camera,
            reinterpret_cast<uintptr_t>(self)};
        if (lifecycle_.changed(frame.identity)) {
            owned_.valid = false;
            look_valid_ = false;
            cursor_valid_ = false;
            input_.reset(GetAsyncKeyState(VK_F6) < 0);
            last_tick_ = 0;
        }
        frame.native_camera_pose = pose_from(reinterpret_cast<const float*>(camera + 0x40));
        frame.native_fov = read<float>(camera + 0x1c8);
        frame.native_near_plane = read<float>(camera + 0x1cc);
        frame.sector_origin = {
            read<float>(sector + 0x64), read<float>(sector + 0x68), read<float>(sector + 0x6c)};
        frame.look = look_;
        frame.look_valid = look_valid_;
        const ULONGLONG now = GetTickCount64();
        frame.delta_seconds = last_tick_ ? static_cast<float>(now - last_tick_) * 0.001f : 1.f / 60.f;
        last_tick_ = now;
        frame.control_allowed = started_ && accepting_.load(std::memory_order_relaxed);
        if (!sample_head(head, mesh, offset, frame.head_pose)) {
            owned_.valid = false;
            return;
        }
        if (!look_valid_) {
            look_ = excamera::look_from_pose(frame.head_pose);
            look_valid_ = true;
            frame.look = look_;
            frame.look_valid = true;
        }

        if (mode_ == excamera::CameraMode::third_person) {
            query_failed_ = false;
            excamera::Pose eye;
            if (!excamera::eye_pose(frame.head_pose, settings_.eye_setback, eye)) {
                owned_.valid = false;
                return;
            }
            eye.origin = eye.origin + frame.sector_origin;
            frame.ground = sample_ground(root, eye.origin, frame.sector_origin);
            const excamera::Pose orientation = excamera::look_pose(look_);
            const excamera::Vec3 desired_pivot = excamera::third_person_pivot(
                eye, settings_.third_height, settings_.third_shoulder, look_);
            const excamera::Vec3 pivot_offset = desired_pivot - eye.origin;
            const float pivot_distance = excamera::length(pivot_offset);
            if (pivot_distance > 0.001f && excamera::finite(pivot_offset)) {
                frame.pivot_clearance = level_clearance(
                    viewport, eye.origin, pivot_offset / pivot_distance, pivot_distance);
            }
            const excamera::Vec3 pivot = excamera::third_person_pivot(
                eye, settings_.third_height, settings_.third_shoulder, look_, frame.pivot_clearance);
            const float fov = settings_.third_fov > 0.f ? settings_.third_fov : settings_.first_fov;
            const excamera::CameraFrustum frustum{orientation, fov, aspect_, settings_.near_plane};
            frame.clearance = excamera::frustum_clearance(pivot, frustum, settings_.third_distance,
                [this, viewport](excamera::Vec3 origin, excamera::Vec3 direction, float length) noexcept {
                    return level_probe(viewport, origin, direction, length);
                });
        }

        const excamera::CameraOutput output = controller_.evaluate(frame);
        custom_presented_ = output.write_camera;
        if (mode_ != excamera::CameraMode::third_person) {
            level_available_ = false;
            query_failed_ = false;
        }
        const bool third = output.mode == excamera::CameraMode::third_person && output.write_camera;
        diagnostic_ground_ = third ? frame.ground : excamera::GroundConstraint{};
        last_clearance_ = third
            ? excamera::ground_clearance(output.pivot, output.pose.back, frame.clearance, frame.ground)
            : 0.f;
        applied_distance_ = third ? excamera::length(output.pose.origin - output.pivot) : 0.f;
        diagnostic_pivot_ = third ? output.pivot : excamera::Vec3{};
        diagnostic_back_ = third ? output.pose.back : excamera::Vec3{};
        if (!output.write_camera) {
            owned_.valid = false;
            return;
        }
        capture_owned(view, camera, reinterpret_cast<uintptr_t>(self), root, frame.identity.scene);
        pose_to(camera + 0x40, output.pose);
        pose_to(camera + 0x70, output.pose);
        const excamera::Matrix4 expanded = excamera::expand_affine(output.pose);
        std::memcpy(reinterpret_cast<void*>(camera + 0xa0), expanded.rows.data(), 64);
        write<float>(camera + 0x1c8, output.fov);
        write<float>(camera + 0x1cc, output.near_plane);
        write<uint32_t>(camera + 0xe4, 1);
        projection_(view);
    }

    SRWLOCK lock_ = SRWLOCK_INIT;
    uintptr_t base_ = 0;
    NavSampler original_nav_ = nullptr;
    CameraMouse original_camera_mouse_ = nullptr;
    Present original_present_ = nullptr;
    HeadSampler head_sampler_ = nullptr;
    Projection projection_ = nullptr;
    Snapshots copy_snapshots_ = nullptr;
    LevelRay level_ray_ = nullptr;
    LevelRay level_ray_secondary_ = nullptr;
    LevelMeshTrace mesh_trace_ = nullptr;
    uintptr_t validated_level_ = 0;
    float aspect_ = 16.f / 9.f;
    excamera::CameraSettings settings_{};
    excamera::settings::Worker settings_worker_;
    excamera::LifecycleTracker lifecycle_;
    excamera::CameraController controller_;
    excamera::InputController input_;
    excamera::CameraMode mode_ = excamera::CameraMode::first_person;
    excamera::Look look_{};
    bool look_valid_ = false;
    OwnedCamera owned_{};
    std::atomic<bool> accepting_{false};
    std::atomic<bool> restore_requested_{false};
    std::atomic<bool> cursor_restore_requested_{false};
    std::atomic<bool> cursor_owned_{false};
    bool cursor_valid_ = false;
    bool f9_down_ = false;
    bool f10_down_ = false;
    bool f11_down_ = false;
    std::atomic<uint32_t> faults_{0};
    std::atomic<bool> restoration_failed_{false};
    std::atomic<Result> last_error_{EXA_RESULT_OK};
    ULONGLONG last_tick_ = 0;
    bool started_ = false;
    uint32_t collision_queries_ = 0;
    uint32_t collision_failures_ = 0;
    uint32_t camera_samples_ = 0;
    uint32_t collision_mesh_traces_ = 0;
    uint64_t collision_nanoseconds_ = 0;
    uint64_t performance_frequency_ = 1;
    bool level_available_ = false;
    bool query_failed_ = false;
    bool custom_presented_ = false;
    float last_clearance_ = 0.f;
    float applied_distance_ = 0.f;
    excamera::Vec3 diagnostic_pivot_{};
    excamera::Vec3 diagnostic_back_{};
    excamera::GroundConstraint diagnostic_ground_{};
};

} // namespace

Result start(HookBroker& broker) noexcept {
    return NativeCamera::instance().start(broker);
}

Result stop() noexcept {
    return NativeCamera::instance().stop();
}

Result set_mode(uint32_t mode) noexcept {
    return NativeCamera::instance().set_mode(mode);
}

Result set_look(float yaw, float pitch) noexcept {
    return NativeCamera::instance().set_look(yaw, pitch);
}

Result set_distance(float distance) noexcept {
    return NativeCamera::instance().set_distance(distance);
}

Result diagnostics(ExaCameraDiagnostics& output) noexcept {
    return NativeCamera::instance().diagnostics(output);
}

uint32_t fault_count() noexcept {
    return NativeCamera::instance().faults();
}

bool restoration_failed() noexcept {
    return NativeCamera::instance().restoration_failed();
}

Result last_error() noexcept {
    return NativeCamera::instance().last_error();
}

} // namespace exanima::camera
