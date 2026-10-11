#!/usr/bin/env bash
# Downloads prebuilt toolchains into .deps/ (gitignored). Source dependencies
# (SDL3, Dear ImGui, cpp-httplib, nlohmann/json, cgltf, stb) are fetched by CMake.
#
# On Debian-family Linux without X11 development headers (and without root),
# the header packages are downloaded with `apt-get download` and extracted into
# .deps/sysroot, with their .so links pointed at the system's runtime libraries.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/.deps"
WASI_SDK_VERSION=34
WASMTIME_VERSION=v49.0.1
DXC_TAG=v1.9.2607
DXC_ASSET=linux_dxc_2026_07_29.x86_x64.tar.gz

mkdir -p "$DEPS/dl"
cd "$DEPS"

if [ ! -d "wasi-sdk-${WASI_SDK_VERSION}.0-x86_64-linux" ]; then
  echo "== wasi-sdk ${WASI_SDK_VERSION}"
  curl -sSL -o dl/wasi-sdk.tar.gz \
    "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VERSION}/wasi-sdk-${WASI_SDK_VERSION}.0-x86_64-linux.tar.gz"
  tar -xzf dl/wasi-sdk.tar.gz
fi

if [ ! -d "wasmtime-${WASMTIME_VERSION}-x86_64-linux-c-api" ]; then
  echo "== wasmtime ${WASMTIME_VERSION} C API"
  curl -sSL -o dl/wasmtime-c-api.tar.xz \
    "https://github.com/bytecodealliance/wasmtime/releases/download/${WASMTIME_VERSION}/wasmtime-${WASMTIME_VERSION}-x86_64-linux-c-api.tar.xz"
  tar -xJf dl/wasmtime-c-api.tar.xz
fi

if [ ! -x "dxc/bin/dxc" ]; then
  echo "== DXC ${DXC_TAG} (HLSL to SPIR-V)"
  curl -sSL -o dl/dxc.tar.gz \
    "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_TAG}/${DXC_ASSET}"
  mkdir -p dxc && tar -xzf dl/dxc.tar.gz -C dxc --strip-components=1
fi

if [ ! -f /usr/include/xcb/xcb.h ] && [ ! -f "$DEPS/sysroot/usr/include/xcb/xcb.h" ]; then
  echo "== X11 headers (no system X11 dev packages; extracting locally)"
  mkdir -p dl/debs sysroot
  (cd dl/debs && apt-get download libx11-dev x11proto-dev libxext-dev libxcursor-dev \
     libxi-dev libxrandr-dev libxfixes-dev libxrender-dev libxss-dev libxcb1-dev)
  for d in dl/debs/*.deb; do dpkg -x "$d" sysroot; done
fi

if [ -d "$DEPS/sysroot/usr/lib/x86_64-linux-gnu" ]; then
  # Point the dev .so links at the system runtime libraries.
  for lib in X11 Xext Xcursor Xi Xrandr Xfixes Xrender Xss xcb; do
    sys=$(ls /usr/lib/x86_64-linux-gnu/lib${lib}.so.* 2>/dev/null | sort | head -1 || true)
    if [ -n "$sys" ]; then
      ln -sf "$sys" "$DEPS/sysroot/usr/lib/x86_64-linux-gnu/lib${lib}.so"
    fi
  done
fi

echo "== done: $DEPS"
