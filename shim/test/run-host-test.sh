#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds chameleon-kernel, libchameleon.so and libgbm.so for the desktop
# (memfd instead of AHardwareBuffer) and runs kms_test.c against the fake
# device with the system's real libdrm, and the input and output paths
# against a real Wayland server.
# Needs: cc, libdrm-dev, libwayland-dev, libegl-dev, libgles-dev, libudev-dev, libinput-dev.
set -e
cd "$(dirname "$0")/../.."
if [ -z "$OUT" ]; then
    OUT=$(mktemp -d)
    trap 'rm -rf "$OUT"' EXIT
fi
mkdir -p "$OUT"
CFLAGS="-O1 -g -Wall -Wextra -Wno-missing-field-initializers -fPIC -DCHAM_HOST_TEST $(pkg-config --cflags libdrm)"
LIB_SRC="shim/core/interpose.c shim/core/crash.c shim/core/ahb.c"
KERNEL_SRC="shim/core/main.c shim/core/card.c shim/core/kms.c shim/core/link.c shim/core/evdev.c shim/core/input.c \
    shim/core/keymap.c shim/core/clipboard.c shim/core/text.c shim/core/clients.c shim/core/ahb.c shim/core/wlpath.c"
cc $CFLAGS -shared -fvisibility=hidden -Ishim/include/libdrm -o "$OUT/libchameleon.so" $LIB_SRC -ldl -lpthread \
    -Wl,-soname,libchameleon.so
cc $CFLAGS -Ishim/include/libdrm -o "$OUT/chameleon-kernel" $KERNEL_SRC -ldl -lpthread
# A test's chameleon-kernel, as the launcher starts one for a session: it
# returns once it answers, and exits with this script. Each test has its own
# runtime directory (the service's sockets) and presenter socket.
start_kernel() {
    mkdir -p "$1" && chmod 700 "$1"
    XDG_RUNTIME_DIR="$1" CHAMELEON_SOCKET="$2" CHAMELEON_WAIT=${3:-30} "$OUT/chameleon-kernel" $$
}
cc $CFLAGS -shared -fvisibility=hidden -Ishim/include -o "$OUT/libgbm.so" shim/gbm/gbm.c -L"$OUT" -lchameleon \
    -Wl,-soname,libgbm.so
cc $CFLAGS -Ishim/include -o "$OUT/kms_test" shim/test/kms_test.c "$OUT/libgbm.so" $(pkg-config --libs libdrm) -lpthread \
    -Wl,-rpath,"$OUT"
# The standard names of gbm and libdrm, as the launcher makes them.
mkdir -p "$OUT/names"
ln -sf "$OUT/libgbm.so" "$OUT/names/libgbm.so.1"
ln -sf "$(pkg-config --variable=libdir libdrm)/libdrm.so" "$OUT/names/libdrm.so.2"
start_kernel "$OUT/runtime-kms" "$OUT/presenter" 5
CHAMELEON_DRM_PATH="$OUT/card0" CHAMELEON_SOCKET="$OUT/presenter" XDG_RUNTIME_DIR="$OUT/runtime-kms" \
    LD_LIBRARY_PATH="$OUT/names" LD_PRELOAD="$OUT/libchameleon.so" "$OUT/kms_test"
# udev monitors are Chameleon's; libinput's udev backend starts on them.
cc $CFLAGS -shared -fvisibility=hidden -o "$OUT/libchameleon_udev.so" shim/udev/*.c -Wl,-soname,libchameleon_udev.so
cc -O1 -g -Wall -Wextra -o "$OUT/udev_test" shim/test/udev_test.c $(pkg-config --cflags --libs libinput libudev)
LD_PRELOAD="$OUT/libchameleon_udev.so" "$OUT/udev_test"
cc -O1 -Wall -Wextra -o "$OUT/glsl_fix_test" shim/test/glsl_fix_test.c shim/gles/glsl_fix.c
"$OUT/glsl_fix_test"
cc -O1 -g -Wall -Wextra -o "$OUT/input_test" shim/test/input_test.c $(pkg-config --cflags --libs wayland-server)
cc $CFLAGS -o "$OUT/chameleon-ime" shim/ime/ime.c -ldl
start_kernel "$OUT/runtime" "$OUT/presenter-input"
CHAMELEON_DRM_PATH="$OUT/card1" CHAMELEON_SOCKET="$OUT/presenter-input" XDG_RUNTIME_DIR="$OUT/runtime" \
    CHAMELEON_IME="$OUT/chameleon-ime" LD_PRELOAD="$OUT/libchameleon.so:$OUT/libchameleon_udev.so" "$OUT/input_test"
# ...or, once something reads it, to the evdev touchscreen, through real libinput.
cc -O1 -g -Wall -Wextra -o "$OUT/evdev_test" shim/test/evdev_test.c -lpthread $(pkg-config --cflags --libs libinput libudev libdrm)
start_kernel "$OUT/runtime-ev" "$OUT/presenter-evdev"
CHAMELEON_DRM_PATH="$OUT/card7" CHAMELEON_SOCKET="$OUT/presenter-evdev" XDG_RUNTIME_DIR="$OUT/runtime-ev" \
    LD_PRELOAD="$OUT/libchameleon.so:$OUT/libchameleon_udev.so" "$OUT/evdev_test"
# A forked child doesn't need the dynamic linker for overridden calls.
cc -O1 -g -Wall -Wextra -o "$OUT/fork_test" shim/test/fork_test.c
mkdir -p "$OUT/ld-debug"
LD_BIND_NOW=1 LD_DEBUG=symbols LD_DEBUG_OUTPUT="$OUT/ld-debug/log" \
    LD_PRELOAD="$OUT/libchameleon.so:$OUT/libchameleon_udev.so" "$OUT/fork_test" "$OUT/ld-debug/log"
# The glvnd EGL vendor, driven through the system's (glvnd) libEGL/libGLESv2
# with a stand-in for Android's EGL/GLES driver.
cc -O1 -g -Wall -Wextra -fPIC -shared -Wl,-Bsymbolic -o "$OUT/fake_android_gl.so" shim/test/fake_android_gl.c
cc -O1 -g -Wall -Wextra -Wno-missing-field-initializers -fPIC -fvisibility=hidden -Ishim/include \
    -Ishim/include/libdrm $(pkg-config --cflags libdrm) -shared -o "$OUT/libEGL_chameleon.so" \
    shim/vendor/vendor.c shim/vendor/bridge.c shim/vendor/wayland.c shim/egl/egl.c shim/gles/gles.c shim/gles/gles_forward.c \
    shim/gles/glsl_fix.c -ldl -Wl,--version-script=shim/vendor/vendor.map
cc -O1 -g -Wall -Wextra -o "$OUT/vendor_test" shim/test/vendor_test.c -lEGL -lGLESv2 -ldl
printf '{"file_format_version": "1.0.0", "ICD": {"library_path": "%s"}}\n' "$OUT/libEGL_chameleon.so" \
    > "$OUT/chameleon-vendor.json"
__EGL_VENDOR_LIBRARY_FILENAMES="$OUT/chameleon-vendor.json" CHAMELEON_ANDROID_EGL="$OUT/fake_android_gl.so" \
    CHAMELEON_ANDROID_GLES="$OUT/fake_android_gl.so" "$OUT/vendor_test"
start_kernel "$OUT/runtime-vendor" "$OUT/presenter-none" 0
__EGL_VENDOR_LIBRARY_FILENAMES="$OUT/chameleon-vendor.json" CHAMELEON_ANDROID_EGL="$OUT/fake_android_gl.so" \
    CHAMELEON_ANDROID_GLES="$OUT/fake_android_gl.so" XDG_RUNTIME_DIR="$OUT/runtime-vendor" \
    CHAMELEON_DRM_PATH="$OUT/card2" CHAMELEON_SOCKET="$OUT/presenter-none" VENDOR_TEST_SESSION=1 \
    CHAMELEON_ANDROID_NATIVEWINDOW="$OUT/fake_android_gl.so" CHAMELEON_ANDROID_MEDIANDK="$OUT/fake_android_gl.so" \
    VENDOR_TEST_GBM="$OUT/libgbm.so" LD_PRELOAD="$OUT/libchameleon.so" "$OUT/vendor_test"
# GPU rendering for Wayland apps: the vendor's Wayland platform against a
# real libwayland-server compositor, buffers registered with chameleon-kernel
# (which learns the socket from the compositor's bind()).
cc -O1 -g -Wall -Wextra -Wno-missing-field-initializers $(pkg-config --cflags libdrm) -o "$OUT/wayland_client_test" \
    shim/test/wayland_client_test.c -lEGL $(pkg-config --libs wayland-server wayland-client wayland-egl) -ldl -lpthread
cc -O1 -Wall -Wextra -fPIC -shared -o "$OUT/fake_qtgui.so" shim/test/fake_qtgui.c
FAKE="$OUT/fake_android_gl.so"
# wayland_test RUNTIME EXTRA_PRELOAD [VAR=value...]: a session of its own each.
wayland_test() {
    start_kernel "$1" "$OUT/presenter-none" 0
    rt=$1 extra=$2
    shift 2
    env XDG_RUNTIME_DIR="$rt" WAYLAND_DISPLAY=wayland-cwl "$@" \
        __EGL_VENDOR_LIBRARY_FILENAMES="$OUT/chameleon-vendor.json" CHAMELEON_ANDROID_EGL="$FAKE" \
        CHAMELEON_ANDROID_GLES="$FAKE" CHAMELEON_ANDROID_NATIVEWINDOW="$FAKE" CHAMELEON_ANDROID_MEDIANDK="$FAKE" \
        CHAMELEON_DRM_PATH="$OUT/card3" CHAMELEON_SOCKET="$OUT/presenter-none" \
        LD_PRELOAD="$OUT/libchameleon.so$extra" "$OUT/wayland_client_test"
}
wayland_test "$OUT/runtime-wl" ""
# ...a Qt built for desktop OpenGL is steered to OpenGL ES (or left to Mesa
# with CHAMELEON_EGL_QT=mesa).
wayland_test "$OUT/runtime-wl2" ":$OUT/fake_qtgui.so" EXPECT_DESKTOP_GL_QT=1
wayland_test "$OUT/runtime-wl3" ":$OUT/fake_qtgui.so" EXPECT_DESKTOP_GL_QT=1 CHAMELEON_EGL_QT=mesa
