#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the Termux package (aarch64 .deb) from a built shim directory:
#
#   packaging/build-deb.sh out/chameleon out/ahb_probe 0.1.0 out/
#
# Layout under $PREFIX (/data/data/com.termux/files/usr):
#   lib/chameleon/          libchameleon.so, libEGL_chameleon.so, the launch
#                           scripts and bin/kwin_wayland - and the fake
#                           libgbm.so, which must stay out of $PREFIX/lib where
#                           it would replace Mesa's for every program
#   bin/chameleon, bin/chameleon-vendor-install -> ../lib/chameleon/...
#   bin/chameleon-probe
#   share/doc/chameleon/README.md, copyright (GPL-2.0-or-later)
# Install on the phone: apt install ./chameleon_<version>_aarch64.deb
set -e
SHIM=${1:?built shim directory}
PROBE=${2:?ahb_probe}
VERSION=${3:?version}
OUTDIR=${4:-.}
cd "$(dirname "$0")/.."

PREFIX=/data/data/com.termux/files/usr
ROOT=$(mktemp -d)
chmod 755 "$ROOT"
trap 'rm -rf "$ROOT"' EXIT
LIB="$ROOT$PREFIX/lib/chameleon"
mkdir -p "$LIB/bin" "$ROOT$PREFIX/bin" "$ROOT$PREFIX/share/doc/chameleon" "$ROOT/DEBIAN"

install -m 644 "$SHIM/libchameleon.so" "$SHIM/libgbm.so" "$SHIM/libEGL_chameleon.so" "$LIB/"
install -m 755 "$SHIM/chameleon" "$SHIM/chameleon-vendor-install" "$LIB/"
install -m 755 "$SHIM/bin/kwin_wayland" "$LIB/bin/"
ln -s ../lib/chameleon/chameleon "$ROOT$PREFIX/bin/chameleon"
ln -s ../lib/chameleon/chameleon-vendor-install "$ROOT$PREFIX/bin/chameleon-vendor-install"
install -m 755 "$PROBE" "$ROOT$PREFIX/bin/chameleon-probe"
install -m 644 README.md "$ROOT$PREFIX/share/doc/chameleon/README.md"
{
    cat <<'NOTICE'
Project Chameleon <https://github.com/HarryL0L/Project-Chameleon>

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2 of the License, or (at your option)
any later version. The GPL version 2 follows.

NOTICE
    cat LICENSE
} > "$ROOT$PREFIX/share/doc/chameleon/copyright"

SIZE=$(du -sk "$ROOT$PREFIX" | cut -f1)
cat > "$ROOT/DEBIAN/control" <<CONTROL
Package: chameleon
Version: $VERSION
Architecture: aarch64
Maintainer: Project Chameleon <https://github.com/HarryL0L/Project-Chameleon>
Installed-Size: $SIZE
Depends: libglvnd, libwayland, dbus
Recommends: kwin
Homepage: https://github.com/HarryL0L/Project-Chameleon
Description: KWin and Plasma on Android's GPU, shown by the Chameleon app
 Runs the Termux kwin_wayland (no Chameleon-specific patches) and a Plasma
 session on a fake KMS device whose frames are shown by the Chameleon
 Android app, rendering with the phone's GPU driver through a glvnd EGL
 vendor. Wayland apps using
 OpenGL ES render on the GPU too. Needs the Chameleon app (chameleon.apk).
 .
 Usage: chameleon kwin_wayland [app] | chameleon startplasma-wayland
CONTROL
# The glvnd registration made by chameleon-vendor-install points into this
# package; drop it with the package.
cat > "$ROOT/DEBIAN/postrm" <<'POSTRM'
#!/data/data/com.termux/files/usr/bin/sh
case "$1" in
remove | purge)
    rm -f /data/data/com.termux/files/usr/share/glvnd/egl_vendor.d/10_chameleon.json
    ;;
esac
exit 0
POSTRM
chmod 755 "$ROOT/DEBIAN/postrm"

mkdir -p "$OUTDIR"
# No "~" in the file name (the package version keeps it): some Android file
# managers and unzip apps can't open or extract such files.
DEB="$OUTDIR/chameleon_$(printf '%s' "$VERSION" | tr '~' '-')_aarch64.deb"
dpkg-deb --root-owner-group -Zxz --build "$ROOT" "$DEB" >/dev/null
echo "$DEB"
