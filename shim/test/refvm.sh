#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Boots a real Linux kernel in QEMU (Ubuntu's 6.8 generic kernel: evdev and
# uinput built in, the vkms KMS driver loaded) and runs a command there,
# printing its output: the reference Chameleon's emulated devices are
# compared with. The contents of <dir> appear as /t in the VM.
# Usage: refvm.sh <cache dir> <dir> <command> [args...]
# Needs: qemu-system-x86_64, busybox (static), cpio, zstd, dpkg-deb, curl, sha256sum.
set -e
CACHE=$1 DIR=$2
shift 2
KV=6.8.0-147-generic
K=$CACHE/kernel-$KV
if [ ! -f "$K/boot/vmlinuz-$KV" ]; then
    # From Launchpad, which keeps every published package (the archive drops
    # a kernel once a newer one replaces it).
    mkdir -p "$CACHE/debs" "$K"
    while read -r sum deb; do
        [ -f "$CACHE/debs/$deb" ] ||
            curl -fsSL -o "$CACHE/debs/$deb" "https://launchpad.net/ubuntu/+archive/primary/+files/$deb"
        echo "$sum  $CACHE/debs/$deb" | sha256sum -c --quiet
        dpkg-deb -x "$CACHE/debs/$deb" "$K"
    done <<EOF
8f1975936dd8820c9442c2ee7f1d48a5083f316548b3345b14703c8a42c79171 linux-image-unsigned-${KV}_6.8.0-147.147_amd64.deb
b3f16d16b54796497884b27810b0942243bdc6ef26da1ce821e948ece39a9492 linux-modules-extra-${KV}_6.8.0-147.147_amd64.deb
EOF
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
