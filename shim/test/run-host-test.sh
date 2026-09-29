#!/bin/sh
# Builds libchameleon.so + libgbm.so for the desktop (memfd instead of
# AHardwareBuffer) and runs kms_test.c against the fake device with the
# system's real libdrm. Needs: cc, libdrm-dev.
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
