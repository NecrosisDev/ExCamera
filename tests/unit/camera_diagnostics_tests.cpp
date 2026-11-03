#include "diagnostics.hpp"

#include <array>
#include <cstddef>
#include <cstring>

#include "../test_support.hpp"

namespace {

static_assert(EXA_CAMERA_DIAGNOSTICS_BASE_SIZE == 56);
static_assert(EXA_CAMERA_DIAGNOSTICS_BASE_SIZE == offsetof(ExaCameraDiagnostics, camera_samples));
static_assert(EXA_CAMERA_DIAGNOSTICS_METRICS_SIZE == offsetof(ExaCameraDiagnostics, support_height));
static_assert(EXA_CAMERA_DIAGNOSTICS_GROUND_SIZE == sizeof(ExaCameraDiagnostics));
static_assert(sizeof(ExaCameraDiagnostics) == 80);

struct Buffer {
    alignas(ExaCameraDiagnostics) std::array<std::byte, sizeof(ExaCameraDiagnostics) + 32> bytes{};
};

ExaCameraDiagnostics source() {
    ExaCameraDiagnostics result{};
    result.struct_size = sizeof(result);
    result.abi_version = EXA_CAMERA_DIAGNOSTICS_ABI_VERSION;
    result.mode = EXA_CAMERA_THIRD_PERSON;
    result.flags = 3;
    result.collision_queries = 42;
    result.collision_failures = 2;
    result.clearance = 100.5f;
    result.applied_distance = 95.f;
    result.pivot[0] = 1.f;
    result.pivot[1] = 2.f;
    result.pivot[2] = 3.f;
    result.back[0] = 4.f;
    result.back[1] = 5.f;
    result.back[2] = 6.f;
    result.camera_samples = 123;
    result.collision_mesh_traces = 456;
    result.collision_nanoseconds = 789012;
    result.support_height = 12.5f;
    result.minimum_camera_y = 92.5f;
    return result;
}

Buffer buffer() {
    Buffer result;
    std::memset(result.bytes.data(), 0xa5, result.bytes.size());
    return result;
}

bool full_snapshot_copies_all_fields() {
    Buffer storage = buffer();
    auto& output = *reinterpret_cast<ExaCameraDiagnostics*>(storage.bytes.data());
    output.struct_size = sizeof(output);
    const auto expected = source();
    CHECK(excamera::copy_camera_diagnostics(expected, output));
    CHECK(output.struct_size == sizeof(output));
    CHECK(output.abi_version == EXA_CAMERA_DIAGNOSTICS_ABI_VERSION);
    CHECK(output.mode == EXA_CAMERA_THIRD_PERSON);
    CHECK(output.flags == 3);
    CHECK(output.collision_queries == 42 && output.collision_failures == 2);
    CHECK(output.clearance == 100.5f && output.applied_distance == 95.f);
    CHECK(output.camera_samples == 123 && output.collision_mesh_traces == 456);
    CHECK(output.collision_nanoseconds == 789012);
    CHECK(output.support_height == 12.5f && output.minimum_camera_y == 92.5f);
    for (int index = 0; index < 3; ++index) {
        CHECK(output.pivot[index] == static_cast<float>(index + 1));
        CHECK(output.back[index] == static_cast<float>(index + 4));
    }
    return true;
}

bool legacy_snapshot_preserves_appended_storage() {
    Buffer storage = buffer();
    auto& output = *reinterpret_cast<ExaCameraDiagnostics*>(storage.bytes.data());
    output.struct_size = EXA_CAMERA_DIAGNOSTICS_BASE_SIZE;
    CHECK(excamera::copy_camera_diagnostics(source(), output));
    CHECK(output.struct_size == EXA_CAMERA_DIAGNOSTICS_BASE_SIZE);
    CHECK(output.mode == EXA_CAMERA_THIRD_PERSON);
    CHECK(output.collision_queries == 42);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (std::size_t index = EXA_CAMERA_DIAGNOSTICS_BASE_SIZE; index < storage.bytes.size(); ++index)
        CHECK(bytes[index] == 0xa5);
    return true;
}

bool metrics_snapshot_preserves_ground_fields() {
    Buffer storage = buffer();
    auto& output = *reinterpret_cast<ExaCameraDiagnostics*>(storage.bytes.data());
    output.struct_size = EXA_CAMERA_DIAGNOSTICS_METRICS_SIZE;
    CHECK(excamera::copy_camera_diagnostics(source(), output));
    CHECK(output.struct_size == EXA_CAMERA_DIAGNOSTICS_METRICS_SIZE);
    CHECK(output.camera_samples == 123 && output.collision_nanoseconds == 789012);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (std::size_t index = EXA_CAMERA_DIAGNOSTICS_METRICS_SIZE; index < storage.bytes.size(); ++index)
        CHECK(bytes[index] == 0xa5);
    return true;
}

bool oversized_snapshot_preserves_unknown_tail() {
    Buffer storage = buffer();
    auto& output = *reinterpret_cast<ExaCameraDiagnostics*>(storage.bytes.data());
    output.struct_size = sizeof(output) + 16;
    CHECK(excamera::copy_camera_diagnostics(source(), output));
    CHECK(output.struct_size == sizeof(output) + 16);
    CHECK(output.camera_samples == 123 && output.collision_nanoseconds == 789012);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (std::size_t index = sizeof(output); index < storage.bytes.size(); ++index)
        CHECK(bytes[index] == 0xa5);
    return true;
}

bool invalid_sizes_do_not_write() {
    Buffer storage = buffer();
    auto& output = *reinterpret_cast<ExaCameraDiagnostics*>(storage.bytes.data());
    output.struct_size = EXA_CAMERA_DIAGNOSTICS_BASE_SIZE - 1;
    auto expected = storage.bytes;
    CHECK(!excamera::copy_camera_diagnostics(source(), output));
    CHECK(storage.bytes == expected);
    auto invalid_source = source();
    invalid_source.struct_size = EXA_CAMERA_DIAGNOSTICS_BASE_SIZE;
    output.struct_size = sizeof(output);
    expected = storage.bytes;
    CHECK(!excamera::copy_camera_diagnostics(invalid_source, output));
    CHECK(storage.bytes == expected);
    return true;
}

} // namespace

int main() {
    return run_tests({
        {"full snapshot copies all fields", full_snapshot_copies_all_fields},
        {"legacy snapshot preserves appended storage", legacy_snapshot_preserves_appended_storage},
        {"metrics snapshot preserves ground fields", metrics_snapshot_preserves_ground_fields},
        {"oversized snapshot preserves unknown tail", oversized_snapshot_preserves_unknown_tail},
        {"invalid sizes do not write", invalid_sizes_do_not_write},
    });
}
