#!/bin/sh
# Builds the KWin shim for Android arm64 with the NDK:
#   CC=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang shim/build-android.sh out/
set -e
cd "$(dirname "$0")/.."
OUT=${1:-out}
mkdir -p "$OUT"
: "${CC:?set CC to the NDK aarch64-linux-android29-clang}"
CFLAGS="-O2 -g -Wall -Wextra -Wno-missing-field-initializers -fPIC -fvisibility=hidden -fno-emulated-tls \
    -Ishim/include -Ishim/include/libdrm"
"$CC" $CFLAGS -shared -o "$OUT/libchameleon.so" shim/core/*.c -ldl -Wl,-soname,libchameleon.so
"$CC" $CFLAGS -shared -o "$OUT/libgbm.so" shim/gbm/gbm.c -L"$OUT" -lchameleon -Wl,-soname,libgbm.so
# glvnd EGL vendor (EGL + GLES on Android's driver); not linked to
# libchameleon.so, since it is loaded into every program glvnd routes to it.
"$CC" $CFLAGS -shared -o "$OUT/libEGL_chameleon.so" shim/vendor/vendor.c shim/vendor/bridge.c shim/vendor/wayland.c shim/egl/egl.c \
    shim/gles/gles.c shim/gles/gles_forward.c shim/gles/glsl_fix.c -ldl \
    -Wl,--version-script=shim/vendor/vendor.map -Wl,-soname,libEGL_chameleon.so
cp shim/chameleon shim/chameleon-vendor-install "$OUT/"
mkdir -p "$OUT/bin" && cp shim/bin/kwin_wayland "$OUT/bin/"

# The fake libgbm.so must export exactly what Termux's Mesa libgbm does: a
# missing symbol breaks KWin at load time. The vendor exports only its entry.
NM="$(dirname "$CC")/llvm-nm"
"$NM" -D --defined-only "$OUT/libEGL_chameleon.so" | awk '{print $3}' | grep -v '^__\(bss\|end\|edata\)' > "$OUT/.exports"
if [ "$(cat "$OUT/.exports")" != "__egl_Main" ]; then
    echo "build-android.sh: libEGL_chameleon.so must export only __egl_Main, not:" >&2
    cat "$OUT/.exports" >&2
    exit 1
fi
if "$NM" -D --undefined-only "$OUT/libEGL_chameleon.so" | grep -q 'cham_'; then
    echo "build-android.sh: libEGL_chameleon.so must not depend on libchameleon.so" >&2
    exit 1
fi
"$NM" -D --defined-only "$OUT/libgbm.so" | awk '{print $3}' | grep -v '^__' | sort > "$OUT/.exports"
if ! diff -u shim/exports/libgbm.so.txt "$OUT/.exports"; then
    echo "build-android.sh: libgbm.so exports differ from shim/exports/libgbm.so.txt" >&2
    exit 1
fi
rm -f "$OUT/.exports"
echo "shim built in $OUT (exports verified)"
