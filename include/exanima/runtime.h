#ifndef EXANIMA_RUNTIME_H
#define EXANIMA_RUNTIME_H

#include <stdint.h>
#include "result.h"

#if defined(_WIN32) && defined(EXANIMA_RUNTIME_EXPORTS)
#define EXA_RUNTIME_API __declspec(dllexport)
#elif defined(_WIN32)
#define EXA_RUNTIME_API __declspec(dllimport)
#else
#define EXA_RUNTIME_API
#endif

#define EXA_RUNTIME_ABI_VERSION 2u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ExaRuntimeState {
    EXA_RUNTIME_UNLOADED = 0,
    EXA_RUNTIME_ATTACHING = 1,
    EXA_RUNTIME_DORMANT = 2,
    EXA_RUNTIME_ACTIVE = 3,
    EXA_RUNTIME_STOPPING = 4,
    EXA_RUNTIME_STOPPED = 5,
    EXA_RUNTIME_REFUSED = 6,
    EXA_RUNTIME_FAULTED = 7
} ExaRuntimeState;

typedef struct ExaRuntimeStatus {
    uint32_t struct_size;
    uint32_t abi_version;
    ExaRuntimeState state;
    uint32_t generation;
    uint32_t installed_hooks;
    uint32_t active_hooks;
    ExaResult last_error;
    uint32_t module_faults;
    uint32_t restoration_failed;
} ExaRuntimeStatus;

#define EXA_CAMERA_DIAGNOSTICS_ABI_VERSION 1u
/* Size through back[3]. Newer fields are optional and selected by struct_size. */
#define EXA_CAMERA_DIAGNOSTICS_BASE_SIZE 56u
#define EXA_CAMERA_DIAGNOSTICS_METRICS_SIZE 72u
#define EXA_CAMERA_DIAGNOSTICS_GROUND_SIZE 80u
#define EXA_CAMERA_DIAGNOSTICS_GROUND_AVAILABLE (1u << 4)
#define EXA_CAMERA_DIAGNOSTICS_LEVEL_AVAILABLE (1u << 0)
#define EXA_CAMERA_DIAGNOSTICS_QUERY_FAILED (1u << 1)
#define EXA_CAMERA_DIAGNOSTICS_CURSOR_OWNED (1u << 2)
#define EXA_CAMERA_DIAGNOSTICS_CUSTOM_PRESENTED (1u << 3)

typedef enum ExaCameraMode {
    EXA_CAMERA_FIRST_PERSON = 0,
    EXA_CAMERA_THIRD_PERSON = 1,
    EXA_CAMERA_OVERHEAD = 2
} ExaCameraMode;

typedef struct ExaCameraDiagnostics {
    uint32_t struct_size;
    uint32_t abi_version;
    ExaCameraMode mode;
    uint32_t flags;
    uint32_t collision_queries;
    uint32_t collision_failures;
    float clearance;
    float applied_distance;
    float pivot[3];
    float back[3];
    /* Optional fields present when struct_size covers them. */
    uint32_t camera_samples;
    uint32_t collision_mesh_traces;
    uint64_t collision_nanoseconds;
    float support_height;
    float minimum_camera_y;
} ExaCameraDiagnostics;

EXA_RUNTIME_API ExaResult ExaRuntimeStart(void);
EXA_RUNTIME_API ExaResult ExaRuntimeStop(void);
EXA_RUNTIME_API ExaResult ExaRuntimeGetStatus(ExaRuntimeStatus* status);
EXA_RUNTIME_API ExaResult ExaCameraSetMode(uint32_t mode);
EXA_RUNTIME_API ExaResult ExaCameraSetLook(uint64_t packed_yaw_pitch);
/* packed_distance is an IEEE-754 binary32 distance in centimetres, 0 to 1200. */
EXA_RUNTIME_API ExaResult ExaCameraSetDistance(uint32_t packed_distance);
EXA_RUNTIME_API ExaResult ExaCameraGetDiagnostics(ExaCameraDiagnostics* diagnostics);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
