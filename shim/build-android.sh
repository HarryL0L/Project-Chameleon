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
"$CC" $CFLAGS -shared -o "$OUT/libEGL.so.1" shim/egl/egl.c -L"$OUT" -lchameleon -ldl -Wl,-soname,libEGL.so.1
"$CC" $CFLAGS -shared -o "$OUT/libGLESv2.so.2" shim/gles/gles.c shim/gles/gles_forward.c shim/gles/glsl_fix.c \
    -L"$OUT" -lchameleon -ldl -Wl,-soname,libGLESv2.so.2
cp shim/chameleon-kwin "$OUT/"

# Each replacement must export exactly what the Termux library it shadows
# does (lists taken from Termux's glvnd 1.7 / Mesa 26.2 packages): a missing
# symbol breaks KWin or epoxy at load time or, worse, silently skips a fix.
NM="$(dirname "$CC")/llvm-nm"
for lib in libGLESv2.so.2 libEGL.so.1 libgbm.so; do
    "$NM" -D --defined-only "$OUT/$lib" | awk '{print $3}' | grep -v '^__' | sort > "$OUT/.exports"
    if ! diff -u "shim/exports/$lib.txt" "$OUT/.exports"; then
        echo "build-android.sh: $lib exports differ from shim/exports/$lib.txt" >&2
        exit 1
    fi
done
rm -f "$OUT/.exports"
echo "shim built in $OUT (exports verified)"
