#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Chameleon's devices against the real kernel's. evdev_conformance.c runs on
# the host against Chameleon's keyboard and touchscreen and, in the reference
# VM (refvm.sh), against uinput devices with the same capabilities;
# kms_conformance.c runs against Chameleon's card and against vkms. Each pair
# of transcripts must be identical.
# Needs what run-host-test.sh needs, plus what refvm.sh needs.
# CACHE (default ~/.cache/chameleon-refvm) keeps the downloaded kernel.
set -e
cd "$(dirname "$0")/../.."
CACHE=${CACHE:-$HOME/.cache/chameleon-refvm}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CFLAGS="-O1 -g -Wall -Wextra -Wno-missing-field-initializers -fPIC -DCHAM_HOST_TEST $(pkg-config --cflags libdrm)"
cc $CFLAGS -shared -fvisibility=hidden -Ishim/include/libdrm -o "$OUT/libchameleon.so" shim/core/interpose.c \
    shim/core/crash.c shim/core/ahb.c -ldl -lpthread -Wl,-soname,libchameleon.so
cc $CFLAGS -Ishim/include/libdrm -o "$OUT/chameleon-kernel" shim/core/main.c shim/core/card.c shim/core/kms.c \
    shim/core/link.c shim/core/evdev.c shim/core/input.c shim/core/keymap.c shim/core/clipboard.c shim/core/text.c \
    shim/core/clients.c shim/core/ahb.c shim/core/wlpath.c -ldl -lpthread
cc $CFLAGS -shared -fvisibility=hidden -o "$OUT/libchameleon_udev.so" shim/udev/*.c -Wl,-soname,libchameleon_udev.so
cc $CFLAGS -shared -fvisibility=hidden -Ishim/include -o "$OUT/libgbm.so" shim/gbm/gbm.c -L"$OUT" -lchameleon \
    -Wl,-soname,libgbm.so
cc -O1 -g -Wall -Wextra -o "$OUT/probe" shim/test/evdev_conformance.c
cc -O1 -g -Wall -Wextra -Ishim/include -o "$OUT/kms_probe" shim/test/kms_conformance.c "$OUT/libgbm.so" \
    -Wl,-rpath,"$OUT"
mkdir -p "$OUT/vm" "$OUT/runtime" "$OUT/runtime-kms" && chmod 700 "$OUT/runtime" "$OUT/runtime-kms"
# The devices' service, as the launcher starts it (it exits with this
# script): one with the evdev probe's stub app, one with no app.
XDG_RUNTIME_DIR="$OUT/runtime" CHAMELEON_SOCKET="$OUT/presenter" "$OUT/chameleon-kernel" $$ 2>/dev/null
XDG_RUNTIME_DIR="$OUT/runtime-kms" CHAMELEON_SOCKET="$OUT/presenter-none" CHAMELEON_WAIT=0 "$OUT/chameleon-kernel" $$ \
    2>/dev/null
cc -O1 -Wall -Wextra -static -DREF_KERNEL -o "$OUT/vm/probe" shim/test/evdev_conformance.c
cc -O1 -Wall -Wextra -static -DREF_KERNEL -o "$OUT/vm/kms_probe" shim/test/kms_conformance.c
run_host() {
    CHAMELEON_DRM_PATH="$OUT/card0" CHAMELEON_SOCKET="$OUT/presenter" XDG_RUNTIME_DIR="$OUT/runtime" \
        LD_PRELOAD="$OUT/libchameleon.so:$OUT/libchameleon_udev.so" "$OUT/probe" "$@"
}
run_host dump "$OUT/vm/caps.bin" 2>/dev/null
run_host 2>/dev/null > "$OUT/chameleon.txt"
sh shim/test/refvm.sh "$CACHE" "$OUT/vm" ./probe run caps.bin > "$OUT/kernel.txt"
grep -q '^@@@ exit 0$' "$OUT/kernel.txt" || { echo "the reference VM run failed:"; cat "$OUT/kernel.txt"; exit 1; }
sed -i '/^@@@ exit/d' "$OUT/kernel.txt"
# Known differences, each for a reason:
# - a revoked node polls POLLHUP without POLLERR: Chameleon's nodes are pipes,
#   and no pipe or socket can poll both;
# - Chameleon's devices can't be removed yet (no hotplug).
sed -i -e 's/^revoked poll revents 0x18$/revoked poll revents 0x10/' -e '/^== removal$/,$d' "$OUT/kernel.txt"
sed -i '/^== removal$/,$d' "$OUT/chameleon.txt"
failed=
if diff -u "$OUT/kernel.txt" "$OUT/chameleon.txt" > "$OUT/diff.txt"; then
    echo "PASSED: Chameleon's evdev devices match the kernel's ($(wc -l < "$OUT/kernel.txt") lines)"
else
    echo "FAILED: evdev differences from the kernel (- kernel, + Chameleon):"
    cat "$OUT/diff.txt"
    [ -z "$KEEP" ] || cp "$OUT/kernel.txt" "$OUT/chameleon.txt" "$KEEP"
    failed=1
fi

CHAMELEON_DRM_PATH="$OUT/card0" XDG_RUNTIME_DIR="$OUT/runtime-kms" \
    LD_PRELOAD="$OUT/libchameleon.so" "$OUT/kms_probe" 2>/dev/null > "$OUT/kms-chameleon.txt"
sh shim/test/refvm.sh "$CACHE" "$OUT/vm" ./kms_probe > "$OUT/kms-kernel.txt"
grep -q '^@@@ exit 0$' "$OUT/kms-kernel.txt" || { echo "the reference VM run failed:"; cat "$OUT/kms-kernel.txt"; exit 1; }
sed -i '/^@@@ exit/d' "$OUT/kms-kernel.txt"
# Known difference: FIONREAD on the card is answered by its pipe (a byte
# count) instead of ENOTTY, so that this ioctl, frequent on sockets, never
# pays for a look at the card.
sed -i 's/^\(FIONREAD  *\)Inappropriate ioctl for device$/\1ok/' "$OUT/kms-kernel.txt"
if diff -u "$OUT/kms-kernel.txt" "$OUT/kms-chameleon.txt" > "$OUT/kms-diff.txt"; then
    echo "PASSED: Chameleon's card matches vkms ($(wc -l < "$OUT/kms-kernel.txt") lines)"
else
    echo "FAILED: card differences from the kernel (- vkms, + Chameleon):"
    cat "$OUT/kms-diff.txt"
    [ -z "$KEEP" ] || cp "$OUT/kms-kernel.txt" "$OUT/kms-chameleon.txt" "$KEEP"
    failed=1
fi
[ -z "$failed" ]
