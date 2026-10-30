#!/usr/bin/env bash
# 编译 PPSSPP Switch 版（Vulkan/NVK）NRO。
#
# 必须在 devkitPro 的 msys2 bash 里运行（cmake 只装在那里）：
#   /c/devkitPro/msys2/usr/bin/bash.exe -lc "bash /f/SWITCH-psp/ppsspp-nx/scripts/switch/build.sh"
#
# 可用环境变量覆盖（都有默认值，默认即本项目实测可用的路径）：
#   BUILD_DIR      构建目录（必须在 C 盘 NTFS，F 盘 exFAT 会让 cmake try_compile 失败）
#   MESA_SDK       Mesa Switch SDK 前缀
#   FFMPEG_SDK     FFmpeg 57 前缀（**必须是 57，不能是 devkitPro 的 7.x**）
#   SDL_PREFIX     SDL3 安装前缀
#   JOBS           并行编译数，默认 8
#   PYTHON_EXECUTABLE  cmake 需要的 python（留空则自动探测）

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="$DEVKITPRO/devkitA64"
export PORTLIBS="$DEVKITPRO/portlibs/switch"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PORTLIBS/bin:$PATH"
export PKG_CONFIG_PATH="$PORTLIBS/lib/pkgconfig"

SRC="$REPO_ROOT"
BUILD="${BUILD_DIR:-/c/switch-build/ppsspp-nx-build2}"
SDL_PREFIX="${SDL_PREFIX:-/c/switch-build/sdl-prefix}"
MESA_SDK="${MESA_SDK:-/c/switch-build/mesa-sdk}"
FFMPEG_SDK="${FFMPEG_SDK:-/c/switch-build/ffmpeg57/prefix}"
JOBS="${JOBS:-8}"

echo "=== 配置 PPSSPP Switch (Vulkan / NVK) ==="
echo "源码:   $SRC"
echo "构建:   $BUILD"
echo "Mesa:   $MESA_SDK"
echo "FFmpeg: $FFMPEG_SDK"
echo "SDL3:   $SDL_PREFIX"
echo

# python：优先用环境变量，其次用已知路径，都没有就不传（让 cmake 自己找）
PY="${PYTHON_EXECUTABLE:-}"
if [ -z "$PY" ] && [ -f "C:/Users/Administrator/.workbuddy/binaries/python/versions/3.13.12/python.exe" ]; then
  PY="C:/Users/Administrator/.workbuddy/binaries/python/versions/3.13.12/python.exe"
fi
PY_ARG=()
[ -n "$PY" ] && PY_ARG=(-DPYTHON_EXECUTABLE="$PY")

cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" \
  -U "FFmpeg_*" -U "HAVE_LIBAVCODEC*" \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$SDL_PREFIX;$MESA_SDK;$PORTLIBS" \
  -DSDL3_DIR="$SDL_PREFIX/lib/cmake/SDL3" \
  -DOpenGL_DIR="$MESA_SDK/lib/cmake/OpenGL" \
  -DVulkan_DIR="$MESA_SDK/lib/cmake/Vulkan" \
  -D_OPENGL_expat_LIBRARY="$MESA_SDK/lib/libexpat.a" \
  -D_OPENGL_zstd_LIBRARY="$MESA_SDK/lib/libzstd.a" \
  -D_VULKAN_SWITCH_ELF_LIBRARY="$MESA_SDK/lib/libelf.a" \
  -D_VULKAN_SWITCH_EXPAT_LIBRARY="$MESA_SDK/lib/libexpat.a" \
  -D_VULKAN_SWITCH_ZSTD_LIBRARY="$MESA_SDK/lib/libzstd.a" \
  -D_VULKAN_SWITCH_Z_LIBRARY="$MESA_SDK/lib/libz.a" \
  -D_VULKAN_SWITCH_NX_LIBRARY="$DEVKITPRO/libnx/lib/libnx.a" \
  -DUSE_LIBNX=ON \
  -DSWITCH_ENABLE_LTO=OFF \
  -DUSE_FFMPEG=ON \
  -DFFMPEG_DIR="$FFMPEG_SDK" \
  -DUSE_DISCORD=OFF \
  -DUSE_MINIUPNPC=OFF \
  -DHEADLESS=OFF \
  -DUNITTEST=OFF \
  -DATLAS_TOOL=OFF \
  -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
  -DOPENXR=OFF \
  -DCMAKE_DEPENDS_USE_COMPILER=OFF \
  "${PY_ARG[@]}"

echo "=== 编译 PPSSPP_NRO (并行 $JOBS) ==="
cmake --build "$BUILD" --target PPSSPP_NRO --parallel "$JOBS"

echo
echo "=== 编译完成 ==="
ls -la "$BUILD/PPSSPP.nro"
