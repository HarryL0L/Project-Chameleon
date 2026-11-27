#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds chameleon-kernel, libchameleon.so and libgbm.so for the desktop
# (memfd instead of AHardwareBuffer) and runs kms_test.c against the fake
# device with the system's real libdrm, and the input and output paths
# against a real Wayland server.
# Needs: cc, libdrm-dev, libwayland-dev, libegl-dev, libgles-dev, libudev-dev, libinput-dev,
# libvulkan-dev, mesa-vulkan-drivers (lavapipe), vulkan-validationlayers, vulkan-tools,
# libxcb1-dev, libxcb-dri3-0, libxcb-present0, libx11-xcb1.
set -e
cd "$(dirname "$0")/../.."
if [ -z "$OUT" ]; then
    OUT=$(mktemp -d)
    trap 'rm -rf "$OUT"' EXIT
fi
mkdir -p "$OUT"
CFLAGS="-O1 -g -Wall -Wextra -Wno-missing-field-initializers -fPIC -DCHAM_HOST_TEST $(pkg-config --cflags libdrm)"
LIB_SRC="shim/core/interpose.c shim/core/crash.c shim/core/ahb.c shim/core/wlsync.c"
KERNEL_SRC="shim/core/main.c shim/core/card.c shim/core/kms.c shim/core/link.c shim/core/evdev.c shim/core/input.c \
    shim/core/keymap.c shim/core/clipboard.c shim/core/text.c shim/core/ahb.c shim/core/wlpath.c shim/core/dmabuf.c"
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
# The same for a session that started before the app reported its screen.
start_kernel "$OUT/runtime-late" "$OUT/presenter-late" 1
EVDEV_TEST_LATE_APP=1 CHAMELEON_DRM_PATH="$OUT/card8" CHAMELEON_SOCKET="$OUT/presenter-late" \
    XDG_RUNTIME_DIR="$OUT/runtime-late" \
    LD_PRELOAD="$OUT/libchameleon.so:$OUT/libchameleon_udev.so" "$OUT/evdev_test"
# The session's init adopts and reaps what detaches below it.
cc -O1 -g -Wall -Wextra -o "$OUT/chameleon-init" shim/init/init.c
cc -O1 -g -Wall -Wextra -o "$OUT/init_test" shim/test/init_test.c
"$OUT/init_test" "$OUT/chameleon-init"
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
    shim/vendor/vendor.c shim/vendor/bridge.c shim/vendor/wayland.c shim/vendor/window.c shim/egl/egl.c shim/gles/gles.c shim/gles/gles_forward.c \
    shim/gles/glsl_fix.c -ldl -Wl,--version-script=shim/vendor/vendor.map
cc -O1 -g -Wall -Wextra -o "$OUT/vendor_test" shim/test/vendor_test.c -lEGL -lGLESv2 -ldl
printf '{"file_format_version": "1.0.0", "ICD": {"library_path": "%s"}}\n' "$OUT/libEGL_chameleon.so" \
    > "$OUT/chameleon-vendor.json"
__EGL_VENDOR_LIBRARY_FILENAMES="$OUT/chameleon-vendor.json" CHAMELEON_ANDROID_EGL="$OUT/fake_android_gl.so" \
    CHAMELEON_ANDROID_GLES="$OUT/fake_android_gl.so" "$OUT/vendor_test"
# A driver whose fences tell no time keeps the timer query hidden.
__EGL_VENDOR_LIBRARY_FILENAMES="$OUT/chameleon-vendor.json" CHAMELEON_ANDROID_EGL="$OUT/fake_android_gl.so" \
    CHAMELEON_ANDROID_GLES="$OUT/fake_android_gl.so" FAKE_FENCE_NO_TIME=1 "$OUT/vendor_test"
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
    shim/test/wayland_client_test.c -lEGL -lvulkan $(pkg-config --libs wayland-server wayland-client wayland-egl) \
    -ldl -lpthread
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
# ...and programs told to wait for the GPU themselves whatever the compositor does.
wayland_test "$OUT/runtime-wl4" "" CHAMELEON_SYNC_WAIT=1 FENCE_TEST_FORCE_WAIT=1
# The card's Vulkan driver, under the system's Vulkan loader, with Mesa's
# lavapipe standing in for Android's driver: alone, with the Khronos
# validation layer checking every call made through it, and in a session,
# where it names the card's nodes. Then a real program: vulkaninfo asks it
# everything there is to ask.
cc -O1 -g -Wall -Wextra -Wno-missing-field-initializers -fPIC -fvisibility=hidden -Ishim/include \
    -Ishim/include/libdrm $(pkg-config --cflags libdrm) -shared -o "$OUT/libvulkan_chameleon.so" \
    shim/vulkan/vulkan.c shim/vulkan/wsi.c shim/vendor/window.c shim/vendor/bridge.c -ldl -lpthread \
    -Wl,--version-script=shim/vulkan/vulkan.map
cc -O1 -g -Wall -Wextra -fPIC -shared -o "$OUT/fake_android_vulkan.so" shim/test/fake_android_vulkan.c -ldl
cc -O1 -g -Wall -Wextra -Wno-missing-field-initializers -o "$OUT/vulkan_test" shim/test/vulkan_test.c -lvulkan -ldl
printf '{"file_format_version": "1.0.1", "ICD": {"library_path": "%s", "api_version": "1.4.0"}}\n' \
    "$OUT/libvulkan_chameleon.so" > "$OUT/chameleon-vulkan.json"
# vulkan [VAR=value...] PROGRAM...: only this driver, on the stand-in.
vulkan() {
    env VK_DRIVER_FILES="$OUT/chameleon-vulkan.json" CHAMELEON_ANDROID_VULKAN="$OUT/fake_android_vulkan.so" \
        VULKAN_TEST_ICD="$OUT/libvulkan_chameleon.so" "$@"
}
vulkan "$OUT/vulkan_test"
if ! vulkan VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation "$OUT/vulkan_test" > "$OUT/vulkan-validation.log" 2>&1 ||
    grep -q "Validation Error\|VUID-" "$OUT/vulkan-validation.log"; then
    cat "$OUT/vulkan-validation.log"
    echo "vulkan_test under the validation layer: FAILED"
    exit 1
fi
grep "VK_LAYER_KHRONOS_validation\|vulkan_test:" "$OUT/vulkan-validation.log"
start_kernel "$OUT/runtime-vk" "$OUT/presenter-none" 0
vulkan_session() {
    vulkan XDG_RUNTIME_DIR="$OUT/runtime-vk" CHAMELEON_DRM_PATH="$OUT/card4" CHAMELEON_SOCKET="$OUT/presenter-none" \
        LD_PRELOAD="$OUT/libchameleon.so" "$@"
}
vulkan_session VULKAN_TEST_CARD=1 "$OUT/vulkan_test"
vulkan_session vulkaninfo > "$OUT/vulkaninfo.txt"
if ! grep -q "renderMajor *= 226" "$OUT/vulkaninfo.txt" || ! grep -q "renderMinor *= 128" "$OUT/vulkaninfo.txt"; then
    echo "vulkaninfo doesn't show the card's render node: FAILED"
    exit 1
fi
echo "  ok    vulkaninfo: $(grep -c . "$OUT/vulkaninfo.txt") lines, the card's render node 226:128 among them"
# The filter the launcher gives the session's loader: with another driver for
# the same GPU installed, programs see the card's driver and Mesa's software
# one, not that driver.
cp "$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -n 1)" "$OUT/other_icd.json"
SELECT=$(sed -n 's/.*export VK_LOADER_DRIVERS_SELECT="\(.*\)"$/\1/p' shim/chameleon)
gpus() {
    env VK_ADD_DRIVER_FILES="$OUT/chameleon-vulkan.json:$OUT/other_icd.json" \
        CHAMELEON_ANDROID_VULKAN="$OUT/fake_android_vulkan.so" "$@" vulkaninfo --summary 2>/dev/null | grep -c "^GPU[0-9]"
}
if [ -z "$SELECT" ] || [ "$(gpus)" != 3 ] || [ "$(gpus VK_LOADER_DRIVERS_SELECT="$SELECT")" != 2 ]; then
    echo "the session's driver filter '$SELECT' doesn't leave the card's driver and lavapipe: FAILED"
    exit 1
fi
echo "  ok    the session's driver filter ($SELECT): 2 of 3 drivers, the card's and the software one"
# CHAMELEON_VULKAN_DRIVER: Android's Vulkan is opened through libadrenotools
# (a stand-in here, found below $PREFIX as on a phone) with the named file as
# its driver, given as a directory and a name, and with a directory that has
# its two hook libraries and nothing else: links to them in the session's
# runtime directory. Where that can't be done the program is told why and runs
# on the phone's own driver.
cc -O1 -g -Wall -Wextra -fPIC -fvisibility=hidden -shared -o "$OUT/fake_adrenotools.so" shim/test/fake_adrenotools.c -ldl
AT="$(cd "$OUT" && pwd -P)/adrenotools"
rm -rf "$AT/run"
mkdir -p "$AT/usr/lib" "$AT/driver" "$AT/none" "$AT/run"
cp "$OUT/fake_adrenotools.so" "$AT/usr/lib/libadrenotools.so"
: > "$AT/driver/libvulkan_other.so"
# other_driver WHAT TOLD ASKED [VAR=value...]: vulkan_test passes, the program
# was told TOLD and the library was asked ASKED (nothing, if empty).
other_driver() {
    what=$1 told=$2 asked=$3
    shift 3
    rm -f "$AT/asked"
    if ! vulkan PREFIX="$AT/usr" XDG_RUNTIME_DIR="$AT/run" FAKE_ADRENOTOOLS_LOG="$AT/asked" "$@" "$OUT/vulkan_test" > "$AT/out" 2>&1 ||
        ! grep -qF "chameleon: vulkan: $told" "$AT/out" || [ "$(cat "$AT/asked" 2>/dev/null)" != "$asked" ]; then
        grep "chameleon:\|FAIL" "$AT/out" || true
        cat "$AT/asked" 2>/dev/null || true
        echo "CHAMELEON_VULKAN_DRIVER, $what: FAILED"
        exit 1
    fi
    echo "  ok    CHAMELEON_VULKAN_DRIVER, $what"
}
ASKED="features 1 tmp (none) hooks $AT/run/chameleon-vulkan-hooks dir $AT/driver/ name libvulkan_other.so redirect (none) mapping (none)"
other_driver "a driver file: Android's Vulkan opened with it" "Android's Vulkan is given the driver $AT/driver/libvulkan_other.so" \
    "$ASKED" CHAMELEON_VULKAN_DRIVER="$AT/none/../driver/libvulkan_other.so"
if [ "$(ls "$AT/run/chameleon-vulkan-hooks" | tr '\n' ' ')" != "libhook_impl.so libmain_hook.so " ] ||
    [ "$(readlink "$AT/run/chameleon-vulkan-hooks/libhook_impl.so")" != "$AT/usr/lib/libhook_impl.so" ] ||
    [ "$(readlink "$AT/run/chameleon-vulkan-hooks/libmain_hook.so")" != "$AT/usr/lib/libmain_hook.so" ]; then
    ls -l "$AT/run/chameleon-vulkan-hooks"
    echo "CHAMELEON_VULKAN_DRIVER, the hook libraries' directory: FAILED"
    exit 1
fi
echo "  ok    CHAMELEON_VULKAN_DRIVER, the hook libraries' directory: links to the two, nothing else"
other_driver "a second program: the directory as it is" "Android's Vulkan is given the driver $AT/driver/libvulkan_other.so" \
    "$ASKED" CHAMELEON_VULKAN_DRIVER="$AT/driver/libvulkan_other.so"
other_driver "no such file: the phone's own driver" "CHAMELEON_VULKAN_DRIVER=$AT/driver/missing.so: No such file or directory; using the phone's own driver" \
    "" CHAMELEON_VULKAN_DRIVER="$AT/driver/missing.so"
other_driver "a directory: the phone's own driver" "CHAMELEON_VULKAN_DRIVER=$AT/driver: not a file; using the phone's own driver" \
    "" CHAMELEON_VULKAN_DRIVER="$AT/driver"
other_driver "libadrenotools not installed: the phone's own driver" "CHAMELEON_VULKAN_DRIVER needs libadrenotools" \
    "" CHAMELEON_VULKAN_DRIVER="$AT/driver/libvulkan_other.so" PREFIX="$AT/none"
other_driver "no runtime directory: the phone's own driver" "CHAMELEON_VULKAN_DRIVER: cannot make \$XDG_RUNTIME_DIR/chameleon-vulkan-hooks: No such file or directory; using the phone's own driver" \
    "" CHAMELEON_VULKAN_DRIVER="$AT/driver/libvulkan_other.so" XDG_RUNTIME_DIR=
other_driver "libadrenotools fails: the linker's reason, the phone's own driver" "libadrenotools cannot open Android's Vulkan with $AT/driver/libvulkan_other.so (/nowhere/libhook_impl.so: " \
    "$ASKED" CHAMELEON_VULKAN_DRIVER="$AT/driver/libvulkan_other.so" FAKE_ADRENOTOOLS_FAIL=1
# A Vulkan program's window: a real Wayland server on one side, the loader
# and this driver on the other, and the stand-in's swapchain presenting into
# the fake Android window between them. Again under the validation layer.
vulkan_window() {
    rt=$1
    shift
    wayland_test "$rt" "" VULKAN_WINDOW_TEST=1 VK_DRIVER_FILES="$OUT/chameleon-vulkan.json" \
        CHAMELEON_ANDROID_VULKAN="$OUT/fake_android_vulkan.so" "$@"
}
vulkan_window "$OUT/runtime-vkw"
if ! vulkan_window "$OUT/runtime-vkw2" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    > "$OUT/vulkan-window-validation.log" 2>&1 || grep -q "Validation Error\|VUID-" "$OUT/vulkan-window-validation.log"; then
    cat "$OUT/vulkan-window-validation.log"
    echo "Vulkan windows under the validation layer: FAILED"
    exit 1
fi
grep "PASSED" "$OUT/vulkan-window-validation.log" | sed 's/^/  (validation layer) /'
# ...and on X11: the frames go to an X server (a stand-in with DRI3 and
# Present) as pixmaps of the session's buffers.
cc -O1 -g -Wall -Wextra -Wno-missing-field-initializers -o "$OUT/x11_window_test" shim/test/x11_window_test.c \
    -lvulkan $(pkg-config --libs xcb) -ldl -lpthread
x11_window() {
    rt=$1
    shift
    start_kernel "$rt" "$OUT/presenter-none" 0
    env XDG_RUNTIME_DIR="$rt" "$@" VK_DRIVER_FILES="$OUT/chameleon-vulkan.json" \
        CHAMELEON_ANDROID_VULKAN="$OUT/fake_android_vulkan.so" CHAMELEON_ANDROID_NATIVEWINDOW="$FAKE" \
        CHAMELEON_ANDROID_MEDIANDK="$FAKE" CHAMELEON_DRM_PATH="$OUT/card9" CHAMELEON_SOCKET="$OUT/presenter-none" \
        LD_PRELOAD="$OUT/libchameleon.so" "$OUT/x11_window_test"
}
x11_window "$OUT/runtime-x11"
if ! x11_window "$OUT/runtime-x112" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    > "$OUT/x11-window-validation.log" 2>&1 || grep -q "Validation Error\|VUID-" "$OUT/x11-window-validation.log"; then
    cat "$OUT/x11-window-validation.log"
    echo "Vulkan windows on X11 under the validation layer: FAILED"
    exit 1
fi
grep "PASSED" "$OUT/x11-window-validation.log" | sed 's/^/  (validation layer) /'
# Android's own libraries are opened through Termux's namespace helper where
# $PREFIX has one (a stand-in here, which writes down what it is asked for):
# those of the EGL vendor, of gbm and the windows, the Vulkan driver's, and
# libadrenotools for another driver. Not with CHAMELEON_ANDROID_NAMESPACE=0;
# and the plain way where there is no helper, as in every run above.
NS="$(cd "$OUT" && pwd -P)/ns"
mkdir -p "$NS/usr/lib" "$NS/android"
cc -O1 -g -Wall -Wextra -fPIC -fvisibility=hidden -shared -o "$NS/usr/lib/libtermux-platform-ns.so" \
    shim/test/fake_platform_ns.c -ldl
cp "$OUT/fake_adrenotools.so" "$NS/usr/lib/libadrenotools.so"
for lib in libEGL.so libGLESv2.so libnativewindow.so libmediandk.so; do
    ln -sf "$OUT/fake_android_gl.so" "$NS/android/$lib"
done
ln -sf "$OUT/fake_android_vulkan.so" "$NS/android/libvulkan.so"
# apart WHAT ASKED [VAR=value...] PROGRAM: the program passes and the helper
# was asked for the libraries ASKED, each as often as said, no others.
asked_for() {
    sort "$NS/asked" 2>/dev/null | uniq -c | awk '{printf "%s x%s ", $2, $1}'
}
apart() {
    what=$1 asked=$2
    shift 2
    rm -f "$NS/asked"
    if ! env PREFIX="$NS/usr" FAKE_PLATFORM_NS_LOG="$NS/asked" "$@" > "$NS/out" 2>&1 ||
        [ "$(asked_for)" != "$asked" ]; then
        tail -n 20 "$NS/out"
        echo "asked for: $(asked_for)"
        echo "Android's libraries apart from the program's, $what: FAILED"
        exit 1
    fi
    echo "  ok    Android's libraries apart from the program's, $what"
}
apart "the EGL vendor's, gbm's and the windows'" "libEGL.so x1 libGLESv2.so x1 libmediandk.so x2 libnativewindow.so x2 " \
    __EGL_VENDOR_LIBRARY_FILENAMES="$OUT/chameleon-vendor.json" CHAMELEON_ANDROID_EGL="$NS/android/libEGL.so" \
    CHAMELEON_ANDROID_GLES="$NS/android/libGLESv2.so" XDG_RUNTIME_DIR="$OUT/runtime-vendor" \
    CHAMELEON_DRM_PATH="$OUT/card2" CHAMELEON_SOCKET="$OUT/presenter-none" VENDOR_TEST_SESSION=1 \
    CHAMELEON_ANDROID_NATIVEWINDOW="$NS/android/libnativewindow.so" \
    CHAMELEON_ANDROID_MEDIANDK="$NS/android/libmediandk.so" \
    VENDOR_TEST_GBM="$OUT/libgbm.so" LD_PRELOAD="$OUT/libchameleon.so" "$OUT/vendor_test"
apart "the Vulkan driver's" "libvulkan.so x1 " \
    VK_DRIVER_FILES="$OUT/chameleon-vulkan.json" VULKAN_TEST_ICD="$OUT/libvulkan_chameleon.so" \
    CHAMELEON_ANDROID_VULKAN="$NS/android/libvulkan.so" "$OUT/vulkan_test"
apart "libadrenotools for another driver" "libadrenotools.so x1 " \
    VK_DRIVER_FILES="$OUT/chameleon-vulkan.json" VULKAN_TEST_ICD="$OUT/libvulkan_chameleon.so" \
    CHAMELEON_ANDROID_VULKAN="$NS/android/libvulkan.so" XDG_RUNTIME_DIR="$AT/run" \
    CHAMELEON_VULKAN_DRIVER="$AT/driver/libvulkan_other.so" "$OUT/vulkan_test"
apart "not with CHAMELEON_ANDROID_NAMESPACE=0" "" \
    VK_DRIVER_FILES="$OUT/chameleon-vulkan.json" VULKAN_TEST_ICD="$OUT/libvulkan_chameleon.so" \
    CHAMELEON_ANDROID_VULKAN="$NS/android/libvulkan.so" CHAMELEON_ANDROID_NAMESPACE=0 "$OUT/vulkan_test"
