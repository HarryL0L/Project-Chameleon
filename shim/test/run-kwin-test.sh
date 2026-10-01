#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Runs the real, installed kwin_wayland on the shim (desktop build) and checks
# its log: the fake KMS device, the glvnd EGL vendor and GLES rendering on a
# stand-in for Android's driver backed by Mesa (llvmpipe, mesa_android_gl.c),
# and a stand-in app (stub_presenter.py) that reports window sizes and
# answers frames. The screen must follow the sizes.
#
# Made for trying new KWin releases before they reach Termux, e.g. in an Arch
# container with kde-unstable. Needs: kwin, mesa, libglvnd, libdrm, wayland,
# dbus, python3, cc, pkg-config. kwin_wayland must not carry file
# capabilities (Arch sets cap_sys_nice): the loader ignores LD_PRELOAD for
# such binaries. `setcap -r "$(command -v kwin_wayland)"` as root.
#
# KWIN_TEST_SECONDS (default 16) is how long KWin runs; the log is kept in
# $OUT/kwin.log.
set -e
cd "$(dirname "$0")/../.."
if [ -z "$OUT" ]; then
    OUT=$(mktemp -d)
    trap 'rm -rf "$OUT"' EXIT
fi
mkdir -p "$OUT"
KWIN=$(command -v kwin_wayland) || { echo "kwin_wayland is not installed"; exit 2; }
if command -v getcap >/dev/null 2>&1 && [ -n "$(getcap "$KWIN")" ]; then
    echo "$KWIN has file capabilities ($(getcap "$KWIN")): LD_PRELOAD would be ignored; see the header"
    exit 2
fi
SECONDS_TOTAL=${KWIN_TEST_SECONDS:-16}

CFLAGS="-O1 -g -Wall -Wno-missing-field-initializers -fPIC -DCHAM_HOST_TEST $(pkg-config --cflags libdrm)"
cc $CFLAGS -shared -fvisibility=hidden -Ishim/include/libdrm -o "$OUT/libchameleon.so" shim/core/*.c -ldl -lpthread \
    -Wl,-soname,libchameleon.so
cc $CFLAGS -shared -fvisibility=hidden -Ishim/include -o "$OUT/libgbm.so.1" shim/gbm/gbm.c -L"$OUT" -lchameleon \
    -Wl,-soname,libgbm.so.1 -Wl,-rpath,"$OUT"
ln -sf libgbm.so.1 "$OUT/libgbm.so"
cc -O1 -g -Wall -Wno-missing-field-initializers -fPIC -fvisibility=hidden -Ishim/include -Ishim/include/libdrm \
    $(pkg-config --cflags libdrm) -shared -o "$OUT/libEGL_chameleon.so" shim/vendor/vendor.c shim/vendor/bridge.c \
    shim/vendor/wayland.c shim/egl/egl.c shim/gles/gles.c shim/gles/gles_forward.c shim/gles/glsl_fix.c -ldl \
    -Wl,--version-script=shim/vendor/vendor.map
python3 shim/test/gen_mesa_gl.py "$(pkg-config --variable=includedir glesv2 2>/dev/null || echo /usr/include)/GLES3/gl32.h" \
    > "$OUT/mesa_android_gl_forward.inc"
cc -O1 -g -Wall -fPIC -shared -fvisibility=hidden -Wl,-Bsymbolic -I"$OUT" -o "$OUT/mesa_android_gl.so" \
    shim/test/mesa_android_gl.c -ldl -lpthread
printf '{"file_format_version": "1.0.0", "ICD": {"library_path": "%s"}}\n' "$OUT/libEGL_chameleon.so" \
    > "$OUT/vendor.json"

# The app: 1280x720, rotated at 4 s, a 90 Hz width that isn't a multiple of 8
# at 8 s (the shim asks for 1920, the app crops), rotated back at 11 s.
mkdir -p "$OUT/runtime" && chmod 700 "$OUT/runtime"
python3 shim/test/stub_presenter.py "$OUT/presenter" 1280x720@60 4 720x1280@60 8 1918x1080@90 11 1280x720@60 \
    > "$OUT/presenter.log" 2>&1 &
PRESENTER_PID=$!
sleep 0.5

(
    unset WAYLAND_DISPLAY DISPLAY
    export XDG_RUNTIME_DIR="$OUT/runtime" QT_FORCE_STDERR_LOGGING=1 QT_LOGGING_RULES="kwin_*.debug=true"
    export CHAMELEON_DRM_PATH="$OUT/card0" KWIN_DRM_DEVICES="$OUT/card0" CHAMELEON_SOCKET="$OUT/presenter" CHAMELEON_WAIT=5
    export KWIN_COMPOSE=O2ES KWIN_DISABLE_VULKAN=1 KWIN_DRM_USE_MODIFIERS=0 KWIN_NO_TIMER_QUERY=1
    export __EGL_VENDOR_LIBRARY_FILENAMES="$OUT/vendor.json"
    export CHAMELEON_ANDROID_EGL="$OUT/mesa_android_gl.so" CHAMELEON_ANDROID_GLES="$OUT/mesa_android_gl.so"
    export LD_LIBRARY_PATH="$OUT" LD_PRELOAD="$OUT/libchameleon.so"
    # SIGKILL at the end: KWin 6.8 betas can crash in their own teardown
    # (commit thread vs. exit), which says nothing about the shim.
    dbus-run-session -- timeout -s KILL "$SECONDS_TOTAL" "$KWIN" --drm
) > "$OUT/kwin.log" 2>&1 || true
for _ in 1 2 3 4 5 6 7 8 9 10; do # it prints a summary once KWin is gone
    kill -0 "$PRESENTER_PID" 2>/dev/null || break
    sleep 0.2
done
kill "$PRESENTER_PID" 2>/dev/null || true

failures=0
check() { # check <description> <grep -E pattern> [absent]
    if [ "$3" = absent ]; then
        grep -aqE "$2" "$OUT/kwin.log" && ok= || ok=1
    else
        grep -aqE "$2" "$OUT/kwin.log" && ok=1 || ok=
    fi
    if [ -n "$ok" ]; then
        echo "  ok    $1"
    else
        echo "  FAIL  $1"
        failures=$((failures + 1))
    fi
}
echo "KWin $("$KWIN" --version 2>/dev/null | cut -d' ' -f2) on the shim"
check "the fake device is KWin's GPU (atomic modesetting)" "Using Atomic Mode Setting on gpu \".*card0\""
check "OpenGL compositing (KWin's shaders compiled)" "OpenGL compositing has been successfully initialized"
check "KWin set the app's size" "chameleon: KWin set mode 1280x720"
check "frames reach the app" "chameleon: last 5 s: [1-9][0-9]* commits, [1-9][0-9]* presented"
check "rotation: screen follows (720x1280)" "chameleon: output: KWin's screen is now 720x1280"
check "90 Hz, width rounded up to 8 (1920x1080)" "chameleon: output: KWin's screen is now 1920x1080 @ 89"
check "rotated back (1280x720)" "chameleon: output: KWin's screen is now 1280x720"
check "no failed buffer imports" "Error creating EGLImageKHR" absent
check "no refused output changes" "KWin refused the change" absent
check "no crash" "chameleon: crash:" absent
tail -1 "$OUT/presenter.log"
echo
[ "$failures" = 0 ] && echo "PASSED: 0 failure(s)" || { echo "FAILED: $failures failure(s) (log: $OUT/kwin.log)"; exit 1; }
