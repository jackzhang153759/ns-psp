#!/bin/bash
# Unified Mesa Switch SDK build (GL + EGL + GLES1/2 + Vulkan/NVK), Linux container flavour.
# Derived from build-unified.sh but native to the devkitPro Debian container.
set -eu

cd /project

STAGE=/out/stage
PREFIX_DIR="$STAGE/opt/devkitpro/portlibs/switch"

# Switch objects must be ELF aarch64, not the x86_64 host default.
export MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu
export PATH="/root/.cargo/bin:$PATH"
echo "=== [0/8] rust target ==="
rustup target list --installed | grep -qx aarch64-unknown-linux-gnu || \
    rustup target add aarch64-unknown-linux-gnu
rustup target list --installed

echo "=== [1/8] installing cross wrappers ==="
mkdir -p /usr/local/libexec
cp /project/bindgen-switch-wrapper.sh /usr/local/libexec/bindgen
cp /project/rustc-switch-wrapper.sh /usr/local/libexec/rustc
cp /project/bindgen-atomic-shim.h /usr/local/libexec/bindgen-atomic-shim.h
chmod +x /usr/local/libexec/bindgen /usr/local/libexec/rustc

MESON_BIN="$(command -v meson)"
NINJA_BIN="$(command -v ninja)"
[ "$MESON_BIN" = "/usr/local/bin/meson" ] || ln -sf "$MESON_BIN" /usr/local/bin/meson
[ "$NINJA_BIN" = "/usr/local/bin/ninja" ] || ln -sf "$NINJA_BIN" /usr/local/bin/ninja

echo "=== [2/8] installing nouveau headers ==="
# Do NOT overwrite /opt/devkitpro/portlibs/switch/include/nouveau.h: that file is
# libdrm_nouveau's public ABI header and provides struct nouveau_list, which
# gallium's nouveau/switch winsys needs.  Mesa's own nouveau.h lives in the
# in-tree include paths where nvk and gallium pick it up first.
cp /project/src/nouveau/headers/nv_device_info.h /opt/devkitpro/portlibs/switch/include/

echo "=== [3/8] alias clang headers ==="
if [ ! -d /usr/lib/llvm-15/lib/clang/15/include ]; then
    ln -sf /usr/lib/llvm-15/lib/clang/15.0.6/include /usr/lib/llvm-15/lib/clang/15/include
fi

echo "=== [4/8] posix stub archives ==="
if [ ! -f /opt/devkitpro/portlibs/switch/lib/libdl.a ]; then
    cd /tmp
    echo "void __mesa_switch_stub_lib(void) {}" > stub_posix.c
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc -c stub_posix.c -o stub_posix.o
    for L in dl rt util; do
        /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs \
            /opt/devkitpro/portlibs/switch/lib/lib${L}.a stub_posix.o
    done
    cd /project
fi

echo "=== [5/8] native host tools ==="
if [ ! -f builddir-native/meson-private/coredata.dat ]; then
    meson setup builddir-native \
        -Dvulkan-drivers= -Dgallium-drivers= -Dshader-cache=true \
        -Dplatforms= -Dglx=disabled -Degl=disabled -Dopengl=false \
        -Dgles1=disabled -Dgles2=disabled -Dtools=[] -Dllvm=enabled \
        -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dinstall-mesa-clc=true
fi
ninja -C builddir-native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2

# NOTE: do NOT prepend /usr/local/libexec here.  ninja's "compiler for the build
# machine" rule invokes bare `rustc`; if the cross wrapper shadows it, proc-macro
# crates get compiled for aarch64 and fail to link on the x86_64 host.
export PATH="/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH"

echo "=== [6/8] configuring cross build ==="
SETUP_MODE=""
if [ "${WIPE:-0}" = "1" ]; then
    SETUP_MODE="--wipe"
fi
meson setup builddir-switch $SETUP_MODE \
    --cross-file switch_cross_file.txt \
    --default-library=static \
    --prefix=/opt/devkitpro/portlibs/switch \
    --libdir=lib \
    --buildtype=release \
    -Doptimization=${MESA_OPTIMIZATION:-2} \
    -Db_lto=false \
    -Db_ndebug=true \
    -Dvulkan-drivers=nouveau \
    -Dgallium-drivers=nouveau,zink \
    -Dgallium-rusticl=false \
    -Dplatforms=switch \
    -Degl-native-platform=switch \
    -Dglx=disabled \
    -Degl=enabled \
    -Dopengl=true \
    -Dgles1=enabled \
    -Dgles2=enabled \
    -Dvideo-codecs= \
    -Dshader-cache=enabled \
    -Dxmlconfig=enabled \
    -Dexpat=enabled \
    -Dtools=[] \
    -Dllvm=disabled \
    -Dshared-glapi=disabled \
    -Dshared-llvm=disabled \
    -Dmesa-clc=system \
    -Dprecomp-compiler=system \
    -Dcpp_rtti=false \
    -Dbuild-tests=false \
    -Dnvk-build-id="67e632f90cb18a5af10f020cdf671ea262ad9a0b"

echo "=== [7/8] building Mesa for Switch ==="
ninja -C builddir-switch -j 16

echo "=== [8/8] installing into stage ==="
rm -rf "$STAGE"
meson install -C builddir-switch --destdir "$STAGE"

for pc_file in "$PREFIX_DIR"/lib/pkgconfig/*.pc; do
    [ -f "$pc_file" ] && sed -i 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$pc_file"
done

# Vulkan/OpenGL package files ship with two-stage installs; copy in-tree copies if absent.
mkdir -p "$PREFIX_DIR/lib/cmake/Vulkan" "$PREFIX_DIR/lib/cmake/OpenGL"
[ -f "$PREFIX_DIR/lib/cmake/Vulkan/VulkanConfig.cmake" ] || cp /project/VulkanConfig.cmake "$PREFIX_DIR/lib/cmake/Vulkan/"
[ -f "$PREFIX_DIR/lib/cmake/OpenGL/OpenGLConfig.cmake" ] || cp /project/OpenGLConfig.cmake "$PREFIX_DIR/lib/cmake/OpenGL/"

echo "=== SDK CONTENTS ==="
find "$PREFIX_DIR/lib" -maxdepth 2 -name '*.a' -o -maxdepth 2 -name '*.cmake' -o -maxdepth 2 -name '*.pc' | sort
echo "=== DONE ==="
