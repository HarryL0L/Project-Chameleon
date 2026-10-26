#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the KWin shim for Android arm64 with the NDK:
#   CC=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang shim/build-android.sh out/
set -e
cd "$(dirname "$0")/.."
OUT=${1:-out}
mkdir -p "$OUT"
: "${CC:?set CC to the NDK aarch64-linux-android29-clang}"
CFLAGS="-O2 -g -Wall -Wextra -Wno-missing-field-initializers -fPIC -fvisibility=hidden -fno-emulated-tls \
    -Ishim/include -Ishim/include/libdrm"
# The card node and gbm's buffers in every program of the session; the card
# itself, input, the app link and apps' buffers in chameleon-kernel.
LIB_SRC="shim/core/interpose.c shim/core/crash.c shim/core/ahb.c"
KERNEL_SRC="shim/core/main.c shim/core/card.c shim/core/kms.c shim/core/link.c shim/core/evdev.c shim/core/input.c \
    shim/core/keymap.c shim/core/clipboard.c shim/core/text.c shim/core/clients.c shim/core/ahb.c shim/core/wlpath.c"
"$CC" $CFLAGS -shared -o "$OUT/libchameleon.so" $LIB_SRC -ldl -Wl,-soname,libchameleon.so
"$CC" $CFLAGS -fPIE -pie -o "$OUT/chameleon-kernel" $KERNEL_SRC -ldl
"$CC" $CFLAGS -shared -o "$OUT/libchameleon_udev.so" shim/udev/*.c -Wl,-soname,libchameleon_udev.so
"$CC" $CFLAGS -shared -o "$OUT/libgbm.so" shim/gbm/gbm.c -L"$OUT" -lchameleon -Wl,-soname,libgbm.so
# glvnd EGL vendor (EGL + GLES on Android's driver); not linked to
# libchameleon.so, since it is loaded into every program glvnd routes to it.
"$CC" $CFLAGS -shared -o "$OUT/libEGL_chameleon.so" shim/vendor/vendor.c shim/vendor/bridge.c shim/vendor/wayland.c shim/egl/egl.c \
    shim/gles/gles.c shim/gles/gles_forward.c shim/gles/glsl_fix.c -ldl \
    -Wl,--version-script=shim/vendor/vendor.map -Wl,-soname,libEGL_chameleon.so
# Chameleon's input method, started by the compositor (packaging/kwinrc).
"$CC" $CFLAGS -o "$OUT/chameleon-ime" shim/ime/ime.c -ldl
cp shim/chameleon shim/chameleon-vendor-install "$OUT/"

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
