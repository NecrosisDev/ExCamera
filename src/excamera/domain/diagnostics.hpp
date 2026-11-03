#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <exanima/runtime.h>

namespace excamera {

// Copies only the intersection of the producer and consumer layouts. The
// caller's struct_size and any unknown trailing bytes are preserved.
inline bool copy_camera_diagnostics(const ExaCameraDiagnostics& source,
                                    ExaCameraDiagnostics& output) noexcept {
    if (source.struct_size != sizeof(source) || output.struct_size < EXA_CAMERA_DIAGNOSTICS_BASE_SIZE)
        return false;
    const uint32_t requested = output.struct_size;
    const uint32_t bytes = std::min(requested, source.struct_size);
    std::memcpy(&output, &source, bytes);
    output.struct_size = requested;
    return true;
}

} // namespace excamera
