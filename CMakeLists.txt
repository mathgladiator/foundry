cmake_minimum_required(VERSION 3.24)
project(foundry LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(cmake/deps.cmake)
include(cmake/foundry_system.cmake)

# Generated channel code must match its schemas (ORG.md, src/generated).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(check_generated ALL
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_channels.py" --check
  COMMENT "checking src/generated against src/channels"
  VERBATIM)

# Systems: every directory under src/systems with a system.json.
file(GLOB system_manifests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/systems/*/system.json")
foreach(manifest ${system_manifests})
  get_filename_component(dir "${manifest}" DIRECTORY)
  foundry_add_system("${dir}")
endforeach()

# Shaders: HLSL compiled to SPIR-V with DXC, embedded as headers.
set(FOUNDRY_DXC "${FOUNDRY_DEPS_DIR}/dxc/bin/dxc" CACHE FILEPATH "DXC (tools/bootstrap.sh)")
set(shader_header_dir "${CMAKE_BINARY_DIR}/shaders")
set(shader_headers "")
function(foundry_shader hlsl stage entry name)
  set(spv "${shader_header_dir}/${name}.spv")
  set(hdr "${shader_header_dir}/${name}.h")
  add_custom_command(OUTPUT "${hdr}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_header_dir}"
    COMMAND "${FOUNDRY_DXC}" -spirv -fspv-target-env=vulkan1.0 -T ${stage} -E ${entry} "${hlsl}" -Fo "${spv}"
    COMMAND "${CMAKE_COMMAND}" -DIN=${spv} -DOUT=${hdr} -DNAME=${name} -P "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
    DEPENDS "${hlsl}" "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
    COMMENT "shader: ${name}"
    VERBATIM)
  set(shader_headers ${shader_headers} "${hdr}" PARENT_SCOPE)
endfunction()
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/cel.hlsl" vs_6_0 vs_main cel_vs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/cel.hlsl" ps_6_0 ps_main cel_ps_spirv)
add_custom_target(shaders DEPENDS ${shader_headers})

# The runtime (src/runtime): the engine that ships in games. Never depends on the host.
file(GLOB_RECURSE runtime_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/runtime/*.cpp")
add_library(foundry_runtime STATIC ${runtime_sources})
add_dependencies(foundry_runtime shaders)
target_include_directories(foundry_runtime PUBLIC
  "${CMAKE_SOURCE_DIR}/src/runtime" "${CMAKE_SOURCE_DIR}/src/common" "${CMAKE_SOURCE_DIR}/src/generated"
  PRIVATE "${shader_header_dir}")
target_compile_options(foundry_runtime PRIVATE -Wall -Wextra)
target_link_libraries(foundry_runtime PUBLIC SDL3::SDL3)

# The foundry application (src/host). Each subdirectory is one slice.
file(GLOB_RECURSE host_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/host/*.cpp")
add_executable(foundry ${host_sources} "${CMAKE_SOURCE_DIR}/src/generated/json/registry.cpp")
target_include_directories(foundry PRIVATE
  "${CMAKE_SOURCE_DIR}/src/host"
  "${CMAKE_SOURCE_DIR}/src/common"
  "${CMAKE_SOURCE_DIR}/src/generated")
target_compile_definitions(foundry PRIVATE
  FOUNDRY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  FOUNDRY_BUILD_DIR="${CMAKE_BINARY_DIR}")
target_compile_options(foundry PRIVATE -Wall -Wextra)
target_link_libraries(foundry PRIVATE
  foundry_runtime imgui SDL3::SDL3 httplib::httplib nlohmann_json::nlohmann_json header_libs wasmtime_c meshoptimizer)

# Tests.
enable_testing()
add_executable(boundary_test "${CMAKE_SOURCE_DIR}/tests/boundary_test.cpp")
target_include_directories(boundary_test PRIVATE "${CMAKE_SOURCE_DIR}/src/common" "${CMAKE_SOURCE_DIR}/src/generated")
target_compile_options(boundary_test PRIVATE -ffp-contract=off)
add_test(NAME boundary COMMAND boundary_test)
