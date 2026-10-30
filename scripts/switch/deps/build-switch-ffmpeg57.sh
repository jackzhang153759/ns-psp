#!/usr/bin/env bash
# 为 Switch 交叉编译 PPSSPP 需要的精简版 FFmpeg 57（libavcodec 57.24.102）。
#
# 为什么必须用这个版本：devkitPro 的 switch-ffmpeg（libavcodec 61 / FFmpeg 7.x）
# 会让 PSP 的 PMF 过场动画花屏、以及"动画播完必定卡死"（ppsspp 移植文档有明确记录）。
# NaGaa95 的 ppsspp-nx 发布版同样基于这里 pinned 的 ppsspp-ffmpeg 源码。
#
# 源码：hrydgard/ppsspp-ffmpeg @ 82049cca2e4c1516ed00a77b502a21f91b7843f4
# 产物：/c/switch-build/ffmpeg57/prefix（lib + include），供 -DFFMPEG_DIR 使用
#
# 用法（必须在 devkitPro 的 msys2 bash 里跑）：
#   C:/devkitPro/msys2/usr/bin/bash.exe -lc "bash /f/SWITCH-psp/patches/build-switch-ffmpeg57.sh"

set -euo pipefail

export DEVKITPRO=${DEVKITPRO:-/c/devkitPro}
export DEVKITA64=$DEVKITPRO/devkitA64
export PORTLIBS_PREFIX=$DEVKITPRO/portlibs/switch
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PORTLIBS_PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$PORTLIBS_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

SRC=/c/switch-build/ffmpeg57/src
BUILD=/c/switch-build/ffmpeg57/build
PREFIX=/c/switch-build/ffmpeg57/prefix
JOBS=${JOBS:-8}

if [ ! -f "$SRC/configure" ]; then
  echo "ERROR: $SRC/configure 不存在，先拉取 ppsspp-ffmpeg 源码"
  exit 1
fi

echo "=== 交叉编译 FFmpeg 57 for Switch ==="
rm -rf "$BUILD" "$PREFIX"
mkdir -p "$BUILD" "$PREFIX"
cd "$BUILD"

"$SRC/configure" \
  --prefix="$PREFIX" \
  --enable-cross-compile \
  --arch=aarch64 \
  --cross-prefix=aarch64-none-elf- \
  --target-os=horizon \
  --extra-cflags="-g -D__SWITCH__ -D_GNU_SOURCE -O3 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -pie -ffunction-sections -fdata-sections -ftls-model=local-exec" \
  --extra-cxxflags="-g -D__SWITCH__ -D_GNU_SOURCE -O3 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -pie -ffunction-sections -fdata-sections -ftls-model=local-exec" \
  --extra-ldflags="-g -fPIE -pie -L${PORTLIBS_PREFIX}/lib -L${DEVKITPRO}/libnx/lib" \
  --disable-shared \
  --enable-static \
  --enable-zlib \
  --disable-runtime-cpudetect \
  --disable-everything \
  --disable-filters \
  --disable-programs \
  --disable-network \
  --disable-avfilter \
  --disable-postproc \
  --disable-encoders \
  --disable-protocols \
  --disable-hwaccels \
  --disable-doc \
  --enable-decoder=h264 \
  --enable-decoder=mpeg4 \
  --enable-decoder=mpeg2video \
  --enable-decoder=mjpeg \
  --enable-decoder=mjpegb \
  --enable-decoder=aac \
  --enable-decoder=aac_latm \
  --enable-decoder=atrac3 \
  --enable-decoder=atrac3p \
  --enable-decoder=mp3 \
  --enable-decoder=pcm_s16le \
  --enable-decoder=pcm_s8 \
  --enable-encoder=huffyuv \
  --enable-encoder=ffv1 \
  --enable-encoder=mjpeg \
  --enable-encoder=pcm_s16le \
  --enable-demuxer=h264 \
  --enable-demuxer=m4v \
  --enable-demuxer=mpegvideo \
  --enable-demuxer=mpegps \
  --enable-demuxer=mp3 \
  --enable-demuxer=avi \
  --enable-demuxer=aac \
  --enable-demuxer=pmp \
  --enable-demuxer=oma \
  --enable-demuxer=pcm_s16le \
  --enable-demuxer=pcm_s8 \
  --enable-demuxer=wav \
  --enable-muxer=avi \
  --enable-parser=h264 \
  --enable-parser=mpeg4video \
  --enable-parser=mpegaudio \
  --enable-parser=mpegvideo \
  --enable-parser=aac \
  --enable-parser=aac_latm

make -j"$JOBS"
make install

echo "=== 完成 ==="
ls -lh "$PREFIX/lib"/libav*.a "$PREFIX/lib"/libsw*.a 2>/dev/null
grep -n "LIBAVCODEC_VERSION_MAJOR" "$PREFIX/include/libavcodec/version.h" 2>/dev/null | head -1
