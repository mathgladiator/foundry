# cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed.cmake
# Writes a C++ header with the file's bytes as `inline constexpr unsigned char NAME[]`.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR size "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "// Generated from ${IN}. Do not edit.\n#pragma once\n#include <cstddef>\ninline constexpr unsigned char ${NAME}[] = {${bytes}};\ninline constexpr size_t ${NAME}_size = ${size};\n")
