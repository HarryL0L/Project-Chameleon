#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Boots a real Linux kernel in QEMU (Ubuntu's 6.8 generic kernel: evdev and
# uinput built in, the vkms KMS driver loaded) and runs a command there,
# printing its output: the reference Chameleon's emulated devices are
# compared with. The contents of <dir> appear as /t in the VM.
# Usage: refvm.sh <cache dir> <dir> <command> [args...]
# Needs: qemu-system-x86_64, busybox (static), cpio, zstd, dpkg-deb, apt-get download.
set -e
CACHE=$1 DIR=$2
shift 2
KV=6.8.0-147-generic
K=$CACHE/kernel-$KV
if [ ! -f "$K/boot/vmlinuz-$KV" ]; then
    mkdir -p "$CACHE/debs" "$K"
    (cd "$CACHE/debs" && apt-get download -q linux-image-unsigned-$KV linux-modules-extra-$KV >/dev/null)
    for d in "$CACHE"/debs/*.deb; do dpkg-deb -x "$d" "$K"; done
fi
R=$CACHE/root
rm -rf "$R" && mkdir -p "$R/bin" "$R/dev" "$R/proc" "$R/sys" "$R/tmp"
cp "$(command -v busybox)" "$R/bin/busybox"
for a in sh mount poweroff insmod; do ln -s busybox "$R/bin/$a"; done
zstd -q -d -f "$K/lib/modules/$KV/kernel/drivers/gpu/drm/vkms/vkms.ko.zst" -o "$R/vkms.ko"
cp -r "$DIR" "$R/t"
{
    echo '#!/bin/sh'
    echo 'mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev'
    echo 'insmod /vkms.ko 2>/dev/null'
    echo 'cd /t'
    echo 'echo "@@@ start"'
    for a in "$@"; do printf "'%s' " "$a"; done
    echo
    echo 'echo "@@@ exit $?"'
    echo 'poweroff -f'
} > "$R/init"
chmod +x "$R/init"
(cd "$R" && find . | cpio -o -H newc --quiet | gzip -1) > "$CACHE/initrd.gz"
timeout 600 qemu-system-x86_64 -m 1024 -smp 2 -nographic -no-reboot \
    -kernel "$K/boot/vmlinuz-$KV" -initrd "$CACHE/initrd.gz" \
    -append "console=ttyS0 quiet panic=-1 rdinit=/init" 2>/dev/null |
    tr -d '\r' | sed -n '/@@@ start$/,/^@@@ exit/p' | sed '1d'
