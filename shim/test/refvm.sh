#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Boots a real Linux kernel in QEMU (Ubuntu's 6.8 generic kernel: evdev and
# uinput built in, the vkms KMS driver loaded) and runs a command there,
# printing its output: the reference Chameleon's emulated devices are
# compared with. The contents of <dir> appear as /t in the VM.
# REFVM_DRM=virtio gives the VM a virtio-gpu card (with a render node)
# instead of vkms.
# Usage: refvm.sh <cache dir> <dir> <command> [args...]
# Needs: qemu-system-x86_64, busybox (static), cpio, zstd, dpkg-deb, curl, sha256sum.
set -e
CACHE=$1 DIR=$2
shift 2
KV=6.8.0-147-generic
K=$CACHE/kernel-$KV
# The last package's file marks a complete extraction.
if [ ! -f "$K/lib/modules/$KV/kernel/drivers/gpu/drm/virtio/virtio-gpu.ko.zst" ]; then
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
3b9188304d600fc35a4f4d1f99751f7ea485bb7f9e0cb87831d4ebf2d3423d9d linux-modules-${KV}_6.8.0-147.147_amd64.deb
EOF
fi
R=$CACHE/root
rm -rf "$R" && mkdir -p "$R/bin" "$R/dev" "$R/proc" "$R/sys" "$R/tmp"
cp "$(command -v busybox)" "$R/bin/busybox"
for a in sh mount poweroff insmod sleep; do ln -s busybox "$R/bin/$a"; done
M=$K/lib/modules/$KV/kernel
if [ "$REFVM_DRM" = virtio ]; then
    zstd -q -d -f "$M/drivers/virtio/virtio_dma_buf.ko.zst" -o "$R/virtio_dma_buf.ko"
    zstd -q -d -f "$M/drivers/gpu/drm/virtio/virtio-gpu.ko.zst" -o "$R/virtio-gpu.ko"
    MODULES="/virtio_dma_buf.ko /virtio-gpu.ko" DEVICE="-device virtio-gpu-pci"
else
    zstd -q -d -f "$M/drivers/gpu/drm/vkms/vkms.ko.zst" -o "$R/vkms.ko"
    MODULES=/vkms.ko DEVICE=
fi
cp -r "$DIR" "$R/t"
{
    echo '#!/bin/sh'
    echo 'mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev'
    for m in $MODULES; do echo "insmod $m 2>/dev/null"; done
    echo 'n=0; while [ ! -e /dev/dri/card0 ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done'
    echo 'cd /t'
    echo 'echo "@@@ start"'
    for a in "$@"; do printf "'%s' " "$a"; done
    echo
    echo 'echo "@@@ exit $?"'
    echo 'poweroff -f'
} > "$R/init"
chmod +x "$R/init"
(cd "$R" && find . | cpio -o -H newc --quiet | gzip -1) > "$CACHE/initrd.gz"
# shellcheck disable=SC2086
timeout 600 qemu-system-x86_64 -m 1024 -smp 2 -nographic -no-reboot $DEVICE \
    -kernel "$K/boot/vmlinuz-$KV" -initrd "$CACHE/initrd.gz" \
    -append "console=ttyS0 quiet panic=-1 rdinit=/init" 2>/dev/null |
    tr -d '\r' | sed -n '/@@@ start$/,/^@@@ exit/p' | sed '1d'
