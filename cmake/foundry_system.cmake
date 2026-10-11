# foundry_add_system(<dir>): builds one system version two ways (ORG.md):
#   - a WebAssembly module, build/wasm/<name>.wasm, which the host loads;
#   - a native static library, sys_<name>, for the merged runtime;
# plus a native runner, build/native/run_<name>, which the host uses to check
# that the native build gives byte-identical output to the WebAssembly build.
set(FOUNDRY_WASI_CXX "${FOUNDRY_WASI_SDK}/bin/clang++")
set(FOUNDRY_SYSTEM_INCLUDES "${CMAKE_SOURCE_DIR}/src/common" "${CMAKE_SOURCE_DIR}/src/generated")

function(foundry_add_system dir)
  get_filename_component(name "${dir}" NAME)
  file(GLOB srcs CONFIGURE_DEPENDS "${dir}/*.cpp")
  if(NOT srcs)
    message(WARNING "system ${name} has no sources; skipped")
    return()
  endif()

  # Native static library.
  add_library(sys_${name} STATIC ${srcs})
  target_include_directories(sys_${name} PUBLIC ${FOUNDRY_SYSTEM_INCLUDES})
  target_compile_options(sys_${name} PRIVATE -fno-exceptions -ffp-contract=off -Wall -Wextra)

  # Native runner: stdin = packed input, stdout = packed output.
  configure_file("${CMAKE_SOURCE_DIR}/cmake/native_runner.cpp.in"
                 "${CMAKE_BINARY_DIR}/runners/${name}.cpp" @ONLY)
  add_executable(run_${name} "${CMAKE_BINARY_DIR}/runners/${name}.cpp")
  target_link_libraries(run_${name} PRIVATE sys_${name})
  set_target_properties(run_${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native")

  # WebAssembly module.
  set(objs "")
  foreach(src ${srcs})
    get_filename_component(stem "${src}" NAME_WE)
    set(obj "${CMAKE_BINARY_DIR}/wasm/obj/${name}/${stem}.o")
    set(inc_flags "")
    foreach(inc ${FOUNDRY_SYSTEM_INCLUDES})
      list(APPEND inc_flags "-I${inc}")
    endforeach()
    add_custom_command(OUTPUT "${obj}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/wasm/obj/${name}"
      COMMAND "${FOUNDRY_WASI_CXX}" --target=wasm32-wasip1 -O2 -std=c++20 -fno-exceptions -ffp-contract=off
              -Wall -Wextra ${inc_flags} -MD -MF "${obj}.d" -c "${src}" -o "${obj}"
      DEPENDS "${src}"
      DEPFILE "${obj}.d"
      COMMENT "wasm: compiling ${name}/${stem}.cpp"
      VERBATIM)
    list(APPEND objs "${obj}")
  endforeach()
  set(wasm "${CMAKE_BINARY_DIR}/wasm/${name}.wasm")
  add_custom_command(OUTPUT "${wasm}"
    COMMAND "${FOUNDRY_WASI_CXX}" --target=wasm32-wasip1 -O2 -mexec-model=reactor
            -fno-exceptions ${objs} -o "${wasm}"
    DEPENDS ${objs}
    COMMENT "wasm: linking ${name}.wasm"
    VERBATIM)
  add_custom_target(wasm_${name} ALL DEPENDS "${wasm}")

  # One target that builds every form of the system.
  add_custom_target(system_${name})
  add_dependencies(system_${name} wasm_${name} run_${name} sys_${name})
endfunction()
