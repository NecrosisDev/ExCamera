#ifndef EXANIMA_RESULT_H
#define EXANIMA_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ExaResult {
    EXA_RESULT_OK = 0,
    EXA_RESULT_INVALID_ARGUMENT = 1,
    EXA_RESULT_INVALID_STATE = 2,
    EXA_RESULT_ALREADY_INITIALIZED = 3,
    EXA_RESULT_NOT_FOUND = 4,
    EXA_RESULT_DUPLICATE_ID = 5,
    EXA_RESULT_ENTRY_NOT_READABLE = 6,
    EXA_RESULT_ENTRY_NOT_EXECUTABLE = 7,
    EXA_RESULT_ENTRY_MODIFIED = 8,
    EXA_RESULT_FOREIGN_DETOUR = 9,
    EXA_RESULT_INVALID_DETOUR = 10,
    EXA_RESULT_BACKEND_FAILURE = 11,
    EXA_RESULT_UNSUPPORTED = 12
} ExaResult;

static inline const char* ExaResultString(ExaResult result) {
    switch (result) {
        case EXA_RESULT_OK: return "ok";
        case EXA_RESULT_INVALID_ARGUMENT: return "invalid argument";
        case EXA_RESULT_INVALID_STATE: return "invalid state";
        case EXA_RESULT_ALREADY_INITIALIZED: return "already initialized";
        case EXA_RESULT_NOT_FOUND: return "not found";
        case EXA_RESULT_DUPLICATE_ID: return "duplicate id";
        case EXA_RESULT_ENTRY_NOT_READABLE: return "entry not readable";
        case EXA_RESULT_ENTRY_NOT_EXECUTABLE: return "entry not executable";
        case EXA_RESULT_ENTRY_MODIFIED: return "entry modified";
        case EXA_RESULT_FOREIGN_DETOUR: return "foreign detour";
        case EXA_RESULT_INVALID_DETOUR: return "invalid detour";
        case EXA_RESULT_BACKEND_FAILURE: return "backend failure";
        case EXA_RESULT_UNSUPPORTED: return "unsupported";
        default: return "unknown result";
    }
}

#ifdef __cplusplus
} // extern "C"
namespace exanima {
using Result = ExaResult;
}
#endif

#endif
