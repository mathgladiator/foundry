# Third-party dependencies. Source libraries come from FetchContent; prebuilt
# toolchains (wasi-sdk, Wasmtime) come from tools/bootstrap.sh into .deps/.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

set(FOUNDRY_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH "Prebuilt dependencies (tools/bootstrap.sh)")
set(FOUNDRY_WASI_SDK "${FOUNDRY_DEPS_DIR}/wasi-sdk-34.0-x86_64-linux" CACHE PATH "wasi-sdk root")
set(FOUNDRY_WASMTIME "${FOUNDRY_DEPS_DIR}/wasmtime-v49.0.1-x86_64-linux-c-api" CACHE PATH "Wasmtime C API root")

if(NOT EXISTS "${FOUNDRY_WASI_SDK}/bin/clang++" OR NOT EXISTS "${FOUNDRY_WASMTIME}/include/wasmtime.h")
  message(FATAL_ERROR "Missing prebuilt dependencies. Run tools/bootstrap.sh first.")
endif()

# Locally extracted X11 headers (see tools/bootstrap.sh), used only when the
# system has no X11 development packages.
set(FOUNDRY_SYSROOT "${FOUNDRY_DEPS_DIR}/sysroot")
if(NOT EXISTS "/usr/include/X11/Xlib.h" AND EXISTS "${FOUNDRY_SYSROOT}/usr/include/X11/Xlib.h")
  string(APPEND CMAKE_C_FLAGS " -isystem ${FOUNDRY_SYSROOT}/usr/include")
  string(APPEND CMAKE_CXX_FLAGS " -isystem ${FOUNDRY_SYSROOT}/usr/include")
  list(APPEND CMAKE_LIBRARY_PATH "${FOUNDRY_SYSROOT}/usr/lib/x86_64-linux-gnu")
  list(APPEND CMAKE_INCLUDE_PATH "${FOUNDRY_SYSROOT}/usr/include")
endif()

# SDL3: X11 only for now (runs under Xwayland on Wayland desktops).
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
set(SDL_X11 ON CACHE BOOL "" FORCE)
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.8
  GIT_SHALLOW TRUE)

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9b
  GIT_SHALLOW TRUE)

FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.58.0
  GIT_SHALLOW TRUE)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)

FetchContent_Declare(cgltf
  GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
  GIT_TAG v1.15
  GIT_SHALLOW TRUE)

FetchContent_Declare(meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG v1.2
  GIT_SHALLOW TRUE)

FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)

# Jolt Physics (design/plotter.md, CLAUDE.md "Decisions"): a static library linked beside the runtime
# and into the host. Only the library: no samples, tests, or viewer.
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG v5.6.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR Build)

set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SDL3 httplib nlohmann_json imgui cgltf stb meshoptimizer JoltPhysics)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

add_library(header_libs INTERFACE)
target_include_directories(header_libs SYSTEM INTERFACE ${cgltf_SOURCE_DIR} ${stb_SOURCE_DIR})

add_library(wasmtime_c STATIC IMPORTED)
set_target_properties(wasmtime_c PROPERTIES
  IMPORTED_LOCATION "${FOUNDRY_WASMTIME}/lib/libwasmtime.a"
  INTERFACE_INCLUDE_DIRECTORIES "${FOUNDRY_WASMTIME}/include"
  INTERFACE_LINK_LIBRARIES "pthread;dl;m")
