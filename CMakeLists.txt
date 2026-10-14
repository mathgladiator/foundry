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
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_pipelines.py" --check
  COMMENT "checking src/generated and src/library against src/channels and the pipelines"
  VERBATIM)

# Systems: every directory under src/systems with a system.json.
file(GLOB system_manifests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/systems/*/system.json")
set(system_libs "")
foreach(manifest ${system_manifests})
  get_filename_component(dir "${manifest}" DIRECTORY)
  foundry_add_system("${dir}")
  get_filename_component(name "${dir}" NAME)
  if(TARGET sys_${name})
    list(APPEND system_libs sys_${name})
  endif()
endforeach()

# Shaders: HLSL compiled to SPIR-V with DXC, embedded as headers.
set(FOUNDRY_DXC "${FOUNDRY_DEPS_DIR}/dxc/bin/dxc" CACHE FILEPATH "DXC (tools/bootstrap.sh)")
set(shader_header_dir "${CMAKE_BINARY_DIR}/shaders")
set(shader_headers "")
file(GLOB shader_includes CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/runtime/shaders/*.hlsli")
function(foundry_shader hlsl stage entry name)
  set(spv "${shader_header_dir}/${name}.spv")
  set(hdr "${shader_header_dir}/${name}.h")
  add_custom_command(OUTPUT "${hdr}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_header_dir}"
    COMMAND "${FOUNDRY_DXC}" -spirv -fspv-target-env=vulkan1.0 -T ${stage} -E ${entry} "${hlsl}" -Fo "${spv}"
    COMMAND "${CMAKE_COMMAND}" -DIN=${spv} -DOUT=${hdr} -DNAME=${name} -P "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
    DEPENDS "${hlsl}" ${shader_includes} "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
    COMMENT "shader: ${name}"
    VERBATIM)
  set(shader_headers ${shader_headers} "${hdr}" PARENT_SCOPE)
endfunction()
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/cel.hlsl" vs_6_0 vs_main cel_vs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/cel.hlsl" ps_6_0 ps_main cel_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/skinned.hlsl" vs_6_0 vs_main skinned_vs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/skinned.hlsl" ps_6_0 ps_main skinned_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/skinned.hlsl" ps_6_0 shadow_ps skinned_shadow_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/scatter.hlsl" cs_6_0 cs_main scatter_cs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/field.hlsl" vs_6_0 vs_main field_vs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/field.hlsl" ps_6_0 ps_main field_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/fire.hlsl" vs_6_0 vs_main fire_vs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/fire.hlsl" ps_6_0 ps_main fire_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/terrain.hlsl" vs_6_0 vs_main terrain_vs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/terrain.hlsl" ps_6_0 ps_main terrain_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/sky.hlsl" vs_6_0 sky_vs sky_vs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/runtime/shaders/sky.hlsl" ps_6_0 sky_ps sky_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/host/viewport/shaders/viewport.hlsl" vs_6_0 mesh_vs viewport_mesh_vs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/host/viewport/shaders/viewport.hlsl" ps_6_0 mesh_ps viewport_mesh_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/host/viewport/shaders/viewport.hlsl" vs_6_0 full_vs viewport_full_vs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/host/viewport/shaders/viewport.hlsl" ps_6_0 ink_ps viewport_ink_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/host/viewport/shaders/viewport.hlsl" ps_6_0 down_ps viewport_down_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/host/viewport/shaders/viewport.hlsl" vs_6_0 line_vs viewport_line_vs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/host/viewport/shaders/viewport.hlsl" ps_6_0 line_ps viewport_line_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/host/viewport/shaders/viewport.hlsl" vs_6_0 skyfull_vs viewport_sky_vs_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/host/viewport/shaders/viewport.hlsl" ps_6_0 sky_ps viewport_sky_ps_spirv)
foundry_shader("${CMAKE_SOURCE_DIR}/src/host/viewport/shaders/viewport.hlsl" ps_6_0 shadow_ps viewport_shadow_ps_spirv)
add_custom_target(shaders DEPENDS ${shader_headers})

# The runtime (src/runtime): the engine that ships in games. Never depends on the host.
file(GLOB_RECURSE runtime_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/runtime/*.cpp")
add_library(foundry_runtime STATIC ${runtime_sources})
add_dependencies(foundry_runtime shaders)
target_include_directories(foundry_runtime PUBLIC
  "${CMAKE_SOURCE_DIR}/src/runtime" "${CMAKE_SOURCE_DIR}/src/common" "${CMAKE_SOURCE_DIR}/src/generated"
  PRIVATE "${shader_header_dir}")
target_compile_options(foundry_runtime PRIVATE -Wall -Wextra)
target_link_libraries(foundry_runtime PUBLIC SDL3::SDL3 PRIVATE Jolt)

# The library (src/library): the C API over the runtime, with every system merged in natively.
# Pipelines' fragments and executors are generated by tools/gen_pipelines.py. Development
# builds compile in the diagnostics (runtime_diag.h) and every pipeline, draft or final.
file(GLOB library_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/library/*.cpp")
add_library(foundry_library STATIC ${library_sources})
target_include_directories(foundry_library PUBLIC "${CMAKE_SOURCE_DIR}/src/library")
target_compile_definitions(foundry_library PUBLIC FOUNDRY_DIAGNOSTICS=1)
target_compile_options(foundry_library PRIVATE -Wall -Wextra -ffp-contract=off)
target_link_libraries(foundry_library PUBLIC foundry_runtime ${system_libs})

# Tools built against the library the way a game is (src/tools), starting with the stress
# platform and one stress binary per pipeline (design/quality.perf.md).
# The video titles' font: Roboto, from Dear ImGui's fonts, embedded.
set(font_header "${CMAKE_BINARY_DIR}/fonts/roboto_medium.h")
add_custom_command(OUTPUT "${font_header}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/fonts"
  COMMAND "${CMAKE_COMMAND}" -DIN=${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf -DOUT=${font_header} -DNAME=roboto_medium
          -P "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS "${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf" "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
  COMMENT "font: Roboto-Medium"
  VERBATIM)
add_custom_target(fonts DEPENDS "${font_header}")
file(GLOB stress_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/tools/stress/*.cpp")
add_library(foundry_stress STATIC ${stress_sources})
add_dependencies(foundry_stress fonts)
target_include_directories(foundry_stress PUBLIC "${CMAKE_SOURCE_DIR}/src/tools/stress" PRIVATE "${CMAKE_BINARY_DIR}/fonts")
target_compile_definitions(foundry_stress PRIVATE
  FOUNDRY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  FOUNDRY_WORKSPACE="${CMAKE_SOURCE_DIR}/demo")
target_compile_options(foundry_stress PRIVATE -Wall -Wextra)
target_link_libraries(foundry_stress PUBLIC foundry_library SDL3::SDL3 PRIVATE nlohmann_json::nlohmann_json header_libs Jolt)
file(GLOB stress_bindings CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/generated/stress/stress_*.c")
# Every pipeline's binding without its main, for the plotters' region runs.
set(pipeline_bindings ${stress_bindings})
list(FILTER pipeline_bindings EXCLUDE REGEX "stress_plotter_")
add_library(foundry_stress_bindings STATIC ${pipeline_bindings})
target_compile_definitions(foundry_stress_bindings PRIVATE STRESS_NO_MAIN)
target_link_libraries(foundry_stress_bindings PUBLIC foundry_stress)
foreach(binding ${stress_bindings})
  get_filename_component(stem "${binding}" NAME_WE)
  add_executable(${stem} "${binding}")
  target_link_libraries(${stem} PRIVATE foundry_stress)
  if(stem MATCHES "^stress_plotter_")
    target_link_libraries(${stem} PRIVATE foundry_stress_bindings)
  endif()
  set_target_properties(${stem} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stress" LINKER_LANGUAGE CXX)
endforeach()

# The foundry application (src/host). Each subdirectory is one slice.
file(GLOB_RECURSE host_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/host/*.cpp")
add_executable(foundry ${host_sources} "${CMAKE_SOURCE_DIR}/src/generated/json/registry.cpp")
target_include_directories(foundry PRIVATE
  "${CMAKE_SOURCE_DIR}/src/host"
  "${CMAKE_SOURCE_DIR}/src/common"
  "${CMAKE_SOURCE_DIR}/src/generated"
  "${shader_header_dir}")
add_dependencies(foundry shaders)
target_compile_definitions(foundry PRIVATE
  FOUNDRY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  FOUNDRY_BUILD_DIR="${CMAKE_BINARY_DIR}")
target_compile_options(foundry PRIVATE -Wall -Wextra)
target_link_libraries(foundry PRIVATE
  foundry_library foundry_runtime imgui SDL3::SDL3 httplib::httplib nlohmann_json::nlohmann_json header_libs wasmtime_c meshoptimizer Jolt)

# Tests.
enable_testing()
add_executable(boundary_test "${CMAKE_SOURCE_DIR}/tests/boundary_test.cpp")
target_include_directories(boundary_test PRIVATE "${CMAKE_SOURCE_DIR}/src/common" "${CMAKE_SOURCE_DIR}/src/generated")
target_compile_options(boundary_test PRIVATE -ffp-contract=off)
add_test(NAME boundary COMMAND boundary_test)
add_executable(fracture_test "${CMAKE_SOURCE_DIR}/tests/fracture_test.cpp"
  "${CMAKE_SOURCE_DIR}/src/host/assets/fracture.cpp" "${CMAKE_SOURCE_DIR}/src/host/core/util.cpp")
target_include_directories(fracture_test PRIVATE "${CMAKE_SOURCE_DIR}/src/host" "${CMAKE_SOURCE_DIR}/src/common"
  "${CMAKE_SOURCE_DIR}/src/runtime")
target_link_libraries(fracture_test PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME fracture COMMAND fracture_test)
add_executable(results_test "${CMAKE_SOURCE_DIR}/tests/results_test.cpp")
target_link_libraries(results_test PRIVATE foundry_library Jolt)
add_test(NAME results COMMAND results_test)
add_executable(region_test "${CMAKE_SOURCE_DIR}/tests/region_test.cpp")
target_link_libraries(region_test PRIVATE foundry_library Jolt)
add_test(NAME region COMMAND region_test)
add_executable(ground_test "${CMAKE_SOURCE_DIR}/tests/ground_test.cpp")
target_link_libraries(ground_test PRIVATE foundry_library)
add_test(NAME ground COMMAND ground_test)
add_executable(sky_test "${CMAKE_SOURCE_DIR}/tests/sky_test.cpp" "${CMAKE_SOURCE_DIR}/src/runtime/sky/sky.cpp")
target_include_directories(sky_test PRIVATE "${CMAKE_SOURCE_DIR}/src/runtime")
add_test(NAME sky COMMAND sky_test)
add_executable(walker_test "${CMAKE_SOURCE_DIR}/tests/walker_test.cpp"
  "${CMAKE_SOURCE_DIR}/src/host/physics/walker.cpp" "${CMAKE_SOURCE_DIR}/src/host/physics/toy.cpp")
target_include_directories(walker_test PRIVATE "${CMAKE_SOURCE_DIR}/src/host" "${CMAKE_SOURCE_DIR}/src/common"
  "${CMAKE_SOURCE_DIR}/src/generated")
target_link_libraries(walker_test PRIVATE foundry_library foundry_runtime Jolt nlohmann_json::nlohmann_json)
add_test(NAME walker COMMAND walker_test)
