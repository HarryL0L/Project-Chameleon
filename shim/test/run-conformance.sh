#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Chameleon's evdev devices against the real kernel's: evdev_conformance.c
# runs on the host against Chameleon's keyboard and touchscreen and, in the
# reference VM (refvm.sh), against uinput devices with the same
# capabilities; the two transcripts must be identical.
# Needs what run-host-test.sh needs, plus what refvm.sh needs.
# CACHE (default ~/.cache/chameleon-refvm) keeps the downloaded kernel.
set -e
cd "$(dirname "$0")/../.."
CACHE=${CACHE:-$HOME/.cache/chameleon-refvm}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CFLAGS="-O1 -g -Wall -Wextra -Wno-missing-field-initializers -fPIC -DCHAM_HOST_TEST $(pkg-config --cflags libdrm)"
cc $CFLAGS -shared -fvisibility=hidden -Ishim/include/libdrm -o "$OUT/libchameleon.so" shim/core/*.c -ldl -lpthread \
    -Wl,-soname,libchameleon.so
cc $CFLAGS -shared -fvisibility=hidden -o "$OUT/libchameleon_udev.so" shim/udev/*.c -Wl,-soname,libchameleon_udev.so
cc -O1 -g -Wall -Wextra -o "$OUT/probe" shim/test/evdev_conformance.c
mkdir -p "$OUT/vm" "$OUT/runtime" && chmod 700 "$OUT/runtime"
cc -O1 -Wall -Wextra -static -DREF_KERNEL -o "$OUT/vm/probe" shim/test/evdev_conformance.c
run_host() {
    CHAMELEON_DRM_PATH="$OUT/card0" CHAMELEON_SOCKET="$OUT/presenter" XDG_RUNTIME_DIR="$OUT/runtime" \
        LD_PRELOAD="$OUT/libchameleon.so:$OUT/libchameleon_udev.so" "$OUT/probe" "$@"
}
run_host dump "$OUT/vm/caps.bin" 2>/dev/null
run_host 2>/dev/null > "$OUT/chameleon.txt"
sh shim/test/refvm.sh "$CACHE" "$OUT/vm" ./probe run caps.bin > "$OUT/kernel.txt"
grep -q '^@@@ exit 0$' "$OUT/kernel.txt" || { echo "the reference VM run failed:"; cat "$OUT/kernel.txt"; exit 1; }
sed -i '/^@@@ exit/d' "$OUT/kernel.txt"
if diff -u "$OUT/kernel.txt" "$OUT/chameleon.txt" > "$OUT/diff.txt"; then
    echo "PASSED: Chameleon's evdev devices match the kernel's ($(wc -l < "$OUT/kernel.txt") lines)"
else
    echo "FAILED: differences from the kernel (- kernel, + Chameleon):"
    cat "$OUT/diff.txt"
    [ -z "$KEEP" ] || cp "$OUT/kernel.txt" "$OUT/chameleon.txt" "$KEEP"
    exit 1
fi
