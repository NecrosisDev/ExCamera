cmake_minimum_required(VERSION 3.24)
project(ExanimaPlatform VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(CTest)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

function(exanima_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

add_library(minhook STATIC
    src/vendor/minhook/src/buffer.c
    src/vendor/minhook/src/hook.c
    src/vendor/minhook/src/trampoline.c
    src/vendor/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC src/vendor/minhook/include)
if(MSVC)
    target_compile_options(minhook PRIVATE /W3 /utf-8)
endif()

add_library(exanima_runtime_core STATIC
    src/runtime/hook_broker.cpp
    src/runtime/session.cpp
)
target_include_directories(exanima_runtime_core PUBLIC include src/runtime)
target_link_libraries(exanima_runtime_core PUBLIC minhook)
exanima_warnings(exanima_runtime_core)

add_library(ExanimaRuntime SHARED
    src/runtime/dll_main.cpp
    src/runtime/runtime.cpp
    src/excamera/native/native_camera.cpp
)
target_compile_definitions(ExanimaRuntime PRIVATE EXANIMA_RUNTIME_EXPORTS)
target_include_directories(ExanimaRuntime PRIVATE src/excamera/native)
target_link_libraries(ExanimaRuntime PRIVATE exanima_runtime_core excamera_domain exanima_platform)
exanima_warnings(ExanimaRuntime)

add_library(exanima_platform STATIC
    src/platform/build_identity.cpp
    src/excamera/native/settings_store.cpp
    src/excamera/native/settings_worker.cpp
)
target_include_directories(exanima_platform PUBLIC src/platform src/excamera/native)
target_link_libraries(exanima_platform PUBLIC bcrypt shell32 excamera_domain)
exanima_warnings(exanima_platform)

add_executable(ExCamera
    src/launcher/main.cpp
    src/launcher/remote.cpp
)
target_include_directories(ExCamera PRIVATE include src/launcher)
target_link_libraries(ExCamera PRIVATE exanima_platform)
exanima_warnings(ExCamera)

add_executable(hook_broker_tests tests/contract/hook_broker_tests.cpp)
target_link_libraries(hook_broker_tests PRIVATE exanima_runtime_core)
exanima_warnings(hook_broker_tests)

add_executable(session_tests tests/integration/session_tests.cpp)
target_link_libraries(session_tests PRIVATE exanima_runtime_core)
exanima_warnings(session_tests)

add_executable(build_identity_tests tests/contract/build_identity_tests.cpp)
target_link_libraries(build_identity_tests PRIVATE exanima_platform)
exanima_warnings(build_identity_tests)

add_library(excamera_domain STATIC
    src/excamera/domain/math.cpp
    src/excamera/domain/controller.cpp
    src/excamera/domain/input.cpp
)
target_include_directories(excamera_domain PUBLIC src/excamera/domain include)
exanima_warnings(excamera_domain)

add_executable(camera_math_tests tests/unit/camera_math_tests.cpp)
target_link_libraries(camera_math_tests PRIVATE excamera_domain)
exanima_warnings(camera_math_tests)

add_executable(camera_controller_tests tests/unit/camera_controller_tests.cpp)
target_link_libraries(camera_controller_tests PRIVATE excamera_domain)
exanima_warnings(camera_controller_tests)

add_executable(camera_input_tests tests/unit/camera_input_tests.cpp)
target_link_libraries(camera_input_tests PRIVATE excamera_domain)
exanima_warnings(camera_input_tests)

add_executable(camera_collision_tests tests/unit/camera_collision_tests.cpp)
target_link_libraries(camera_collision_tests PRIVATE excamera_domain)
exanima_warnings(camera_collision_tests)

add_executable(camera_frustum_tests tests/unit/camera_frustum_tests.cpp)
target_link_libraries(camera_frustum_tests PRIVATE excamera_domain)
exanima_warnings(camera_frustum_tests)

add_executable(camera_ground_tests tests/unit/camera_ground_tests.cpp)
target_link_libraries(camera_ground_tests PRIVATE excamera_domain)
exanima_warnings(camera_ground_tests)

add_executable(camera_diagnostics_tests tests/unit/camera_diagnostics_tests.cpp)
target_link_libraries(camera_diagnostics_tests PRIVATE excamera_domain)
exanima_warnings(camera_diagnostics_tests)

add_executable(camera_lifecycle_tests tests/unit/camera_lifecycle_tests.cpp)
target_link_libraries(camera_lifecycle_tests PRIVATE excamera_domain)
exanima_warnings(camera_lifecycle_tests)

add_executable(settings_store_tests tests/unit/settings_store_tests.cpp)
target_link_libraries(settings_store_tests PRIVATE exanima_platform)
exanima_warnings(settings_store_tests)

add_executable(settings_model_tests tests/unit/settings_model_tests.cpp)
target_include_directories(settings_model_tests PRIVATE src)
exanima_warnings(settings_model_tests)

if(BUILD_TESTING)
    add_test(NAME hook_broker_contracts COMMAND hook_broker_tests)
    add_test(NAME session_lifecycle COMMAND session_tests)
    add_test(NAME runtime_package_smoke COMMAND ExCamera --self-test)
    add_test(NAME build_identity COMMAND build_identity_tests)
    add_test(NAME camera_math COMMAND camera_math_tests)
    add_test(NAME camera_controller COMMAND camera_controller_tests)
    add_test(NAME camera_input COMMAND camera_input_tests)
    add_test(NAME camera_collision COMMAND camera_collision_tests)
    add_test(NAME camera_frustum COMMAND camera_frustum_tests)
    add_test(NAME camera_ground COMMAND camera_ground_tests)
    add_test(NAME camera_diagnostics COMMAND camera_diagnostics_tests)
    add_test(NAME camera_lifecycle COMMAND camera_lifecycle_tests)
    add_test(NAME settings_store COMMAND settings_store_tests)
    add_test(NAME settings_model COMMAND settings_model_tests)
endif()

install(TARGETS ExanimaRuntime ExCamera RUNTIME DESTINATION .)
install(FILES LICENSE DESTINATION . RENAME LICENSE.txt)
install(FILES src/vendor/minhook/LICENSE.txt DESTINATION . RENAME LICENSE-MinHook.txt)
