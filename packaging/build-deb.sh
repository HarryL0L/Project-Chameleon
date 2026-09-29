#!/bin/sh
# Builds the Termux package (aarch64 .deb) from a built shim directory:
#
#   packaging/build-deb.sh out/chameleon out/ahb_probe out/chameleon_demo 0.1.0 out/
#
# Layout under $PREFIX (/data/data/com.termux/files/usr):
#   lib/chameleon/          libchameleon.so, libEGL_chameleon.so, the launch
#                           scripts and bin/kwin_wayland - and the fake
#                           libgbm.so, which must stay out of $PREFIX/lib where
#                           it would replace Mesa's for every program
#   bin/chameleon, bin/chameleon-vendor-install -> ../lib/chameleon/...
#   bin/chameleon-probe, bin/chameleon-demo
#   share/doc/chameleon/README.md
# Install on the phone: apt install ./chameleon_<version>_aarch64.deb
set -e
SHIM=${1:?built shim directory}
PROBE=${2:?ahb_probe}
DEMO=${3:?chameleon_demo}
VERSION=${4:?version}
OUTDIR=${5:-.}
cd "$(dirname "$0")/.."

PREFIX=/data/data/com.termux/files/usr
ROOT=$(mktemp -d)
chmod 755 "$ROOT"
trap 'rm -rf "$ROOT"' EXIT
LIB="$ROOT$PREFIX/lib/chameleon"
mkdir -p "$LIB/bin" "$ROOT$PREFIX/bin" "$ROOT$PREFIX/share/doc/chameleon" "$ROOT/DEBIAN"

install -m 644 "$SHIM/libchameleon.so" "$SHIM/libgbm.so" "$SHIM/libEGL_chameleon.so" "$LIB/"
install -m 755 "$SHIM/chameleon" "$SHIM/chameleon-kwin" "$SHIM/chameleon-vendor-install" "$LIB/"
install -m 755 "$SHIM/bin/kwin_wayland" "$LIB/bin/"
ln -s ../lib/chameleon/chameleon "$ROOT$PREFIX/bin/chameleon"
ln -s ../lib/chameleon/chameleon-vendor-install "$ROOT$PREFIX/bin/chameleon-vendor-install"
install -m 755 "$PROBE" "$ROOT$PREFIX/bin/chameleon-probe"
install -m 755 "$DEMO" "$ROOT$PREFIX/bin/chameleon-demo"
install -m 644 README.md "$ROOT$PREFIX/share/doc/chameleon/README.md"

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
 Runs the unmodified Termux kwin_wayland (and a Plasma session) on a fake
 KMS device whose frames are shown by the Chameleon Android app, rendering
 with the phone's GPU driver through a glvnd EGL vendor. Wayland apps using
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
DEB="$OUTDIR/chameleon_${VERSION}_aarch64.deb"
dpkg-deb --root-owner-group -Zxz --build "$ROOT" "$DEB" >/dev/null
echo "$DEB"
