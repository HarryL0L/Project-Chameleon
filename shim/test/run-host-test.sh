#!/bin/sh
# Builds libchameleon.so + libgbm.so for the desktop (memfd instead of
# AHardwareBuffer) and runs kms_test.c against the fake device with the
# system's real libdrm, and the input path against a real Wayland server.
# Needs: cc, libdrm-dev, libwayland-dev, libegl-dev, libgles-dev.
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-$(mktemp -d)}
mkdir -p "$OUT"
CFLAGS="-O1 -g -Wall -Wextra -Wno-missing-field-initializers -fPIC -DCHAM_HOST_TEST $(pkg-config --cflags libdrm)"
cc $CFLAGS -shared -fvisibility=hidden -Ishim/include/libdrm -o "$OUT/libchameleon.so" shim/core/*.c -ldl -lpthread \
    -Wl,-soname,libchameleon.so
cc $CFLAGS -shared -fvisibility=hidden -Ishim/include -o "$OUT/libgbm.so" shim/gbm/gbm.c -L"$OUT" -lchameleon \
    -Wl,-soname,libgbm.so
cc $CFLAGS -Ishim/include -o "$OUT/kms_test" shim/test/kms_test.c "$OUT/libgbm.so" $(pkg-config --libs libdrm) -lpthread \
    -Wl,-rpath,"$OUT"
CHAMELEON_DRM_PATH="$OUT/card0" CHAMELEON_SOCKET="$OUT/presenter" CHAMELEON_WAIT=5 \
    LD_PRELOAD="$OUT/libchameleon.so" "$OUT/kms_test"
cc -O1 -Wall -Wextra -o "$OUT/glsl_fix_test" shim/test/glsl_fix_test.c shim/gles/glsl_fix.c
"$OUT/glsl_fix_test"
cc -O1 -g -Wall -Wextra -o "$OUT/input_test" shim/test/input_test.c $(pkg-config --cflags --libs wayland-server)
mkdir -p "$OUT/runtime" && chmod 700 "$OUT/runtime"
CHAMELEON_DRM_PATH="$OUT/card1" CHAMELEON_SOCKET="$OUT/presenter-input" XDG_RUNTIME_DIR="$OUT/runtime" \
    LD_PRELOAD="$OUT/libchameleon.so" "$OUT/input_test"
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
__EGL_VENDOR_LIBRARY_FILENAMES="$OUT/chameleon-vendor.json" CHAMELEON_ORIG___EGL_VENDOR_LIBRARY_FILENAMES= \
    CHAMELEON_ANDROID_EGL="$OUT/fake_android_gl.so" CHAMELEON_ANDROID_GLES="$OUT/fake_android_gl.so" \
    CHAMELEON_DRM_PATH="$OUT/card2" CHAMELEON_SOCKET="$OUT/presenter-none" VENDOR_TEST_KWIN=1 \
    LD_PRELOAD="$OUT/libchameleon.so" "$OUT/vendor_test"
# GPU rendering for Wayland apps: the vendor's Wayland platform against a
# real libwayland-server compositor, buffers registered with libchameleon.so.
cc -O1 -g -Wall -Wextra -Wno-missing-field-initializers $(pkg-config --cflags libdrm) -o "$OUT/wayland_client_test" \
    shim/test/wayland_client_test.c -lEGL $(pkg-config --libs wayland-server wayland-client wayland-egl) -ldl -lpthread
mkdir -p "$OUT/runtime-wl" && chmod 700 "$OUT/runtime-wl"
FAKE="$OUT/fake_android_gl.so"
XDG_RUNTIME_DIR="$OUT/runtime-wl" WAYLAND_DISPLAY=wayland-cwl CHAMELEON_WAYLAND_SOCKET="$OUT/runtime-wl/wayland-cwl" \
    __EGL_VENDOR_LIBRARY_FILENAMES="$OUT/chameleon-vendor.json" CHAMELEON_ANDROID_EGL="$FAKE" \
    CHAMELEON_ANDROID_GLES="$FAKE" CHAMELEON_ANDROID_NATIVEWINDOW="$FAKE" CHAMELEON_ANDROID_MEDIANDK="$FAKE" \
    CHAMELEON_DRM_PATH="$OUT/card3" CHAMELEON_SOCKET="$OUT/presenter-none" \
    LD_PRELOAD="$OUT/libchameleon.so" "$OUT/wayland_client_test"
# ...and a Qt built for desktop OpenGL keeps its windows on Mesa.
cc -O1 -Wall -Wextra -fPIC -shared -o "$OUT/fake_qtgui.so" shim/test/fake_qtgui.c
rm -rf "$OUT/runtime-wl"/* && XDG_RUNTIME_DIR="$OUT/runtime-wl" WAYLAND_DISPLAY=wayland-cwl \
    CHAMELEON_WAYLAND_SOCKET="$OUT/runtime-wl/wayland-cwl" EXPECT_DESKTOP_GL_QT=1 \
    __EGL_VENDOR_LIBRARY_FILENAMES="$OUT/chameleon-vendor.json" CHAMELEON_ANDROID_EGL="$FAKE" \
    CHAMELEON_ANDROID_GLES="$FAKE" CHAMELEON_ANDROID_NATIVEWINDOW="$FAKE" CHAMELEON_ANDROID_MEDIANDK="$FAKE" \
    CHAMELEON_DRM_PATH="$OUT/card4" CHAMELEON_SOCKET="$OUT/presenter-none" \
    LD_PRELOAD="$OUT/libchameleon.so:$OUT/fake_qtgui.so" "$OUT/wayland_client_test"
