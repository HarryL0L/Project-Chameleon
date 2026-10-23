# Chameleon for Termux (the shim)

Runs the Termux `kwin_wayland` (6.7.5, no Chameleon-specific patches) and a Plasma session on
the Chameleon app's screen, rendering with Android's GPU driver.

| file | replaces | does |
|---|---|---|
| `chameleon-kernel` (started by `chameleon`) | the kernel | fake KMS device: one DSI connector sized to the app (with an EDID), one CRTC, one primary plane, atomic commits → PRESENT, FRAME_DONE → page-flip event; the input devices, the link to the app and the input thread |
| `libchameleon.so` (LD_PRELOAD, KWin) | the device node | `/dev/dri/card0`: open, stat and DRM ioctls go to `chameleon-kernel` as the system call's arguments (`core/interpose.c`, `core/card.c`); apps' GPU buffers (`core/clients.c`), crash reporter |
| `libgbm.so` (LD_LIBRARY_PATH) | Mesa's libgbm | `gbm_bo` = `AHardwareBuffer`, fd = the gralloc handle's dmabuf |
| `libEGL_chameleon.so` (glvnd vendor) | Mesa's EGL vendor | EGL/GLES on `/system/lib64/libEGL.so` / `libGLESv2.so` (→ vendor driver); adds the GBM platform, dmabuf import (as `AHardwareBuffer`) and the card as an `EGLDevice` (`EGL_EXT_device_drm`, primary node only) inside KWin, and the Mali workarounds: `#if GL_*` → `#if defined(GL_*)` in GLSL, hidden timer-query extension, no robust/priority contexts |

## Run

1. Install `chameleon.apk` from the CI artifact and open it.
2. Install the Termux package from the artifact
   (`apt install ./chameleon_<version>_aarch64.deb`; files go to
   `$PREFIX/lib/chameleon`, commands to `$PREFIX/bin`). Or copy the artifact's
   `chameleon/` folder into Termux (not `/sdcard`, which is `noexec`),
   replacing an older copy completely, and run
   `chmod +x chameleon/chameleon* chameleon/bin/* && chameleon/chameleon --install`.
   Then:

```sh
chameleon kwin_wayland               # just KWin
chameleon kwin_wayland konsole       # KWin plus a first app
chameleon startplasma-wayland        # a Plasma session
```

`chameleon` puts its `bin/` directory first in `PATH`, clears `DISPLAY` /
`WAYLAND_DISPLAY`, starts the app's broker (which hands `chameleon-kernel`'s
connection to the app; log in `$XDG_RUNTIME_DIR/chameleon-broker.log`) and
`chameleon-kernel` (the card, input devices and app link for the whole
session; it exits with the session) and runs the command
(in a new D-Bus session if there is none). Every `kwin_wayland` started inside - directly, by
`kwin_wayland_wrapper` or by `startplasma-wayland` - is then
`bin/kwin_wayland`, which attaches the shim to that one process and runs the
real KWin. No `--drm` is needed anywhere: with neither
`WAYLAND_DISPLAY` nor `DISPLAY` set KWin picks the DRM backend by itself (the
script adds `--drm` anyway unless another backend is asked for), so
plasma-workspace needs no change either.

KWin loads the EGL vendor through Termux's own glvnd (`libEGL.so.1`,
`libGLESv2.so.2`): `chameleon` sets `__EGL_VENDOR_LIBRARY_FILENAMES` for the
whole session, with this vendor first and the installed ones (Mesa) after
it.

### The EGL vendor for other programs

`chameleon-vendor-install` registers `libEGL_chameleon.so` with
glvnd for every Termux program (`--remove` undoes it); `chameleon` already
does the same for its session. It takes Wayland windows whose compositor is
a Chameleon KWin (rendered on the GPU, handed over as `AHardwareBuffer`s),
off-screen displays (`EGL_PLATFORM_SURFACELESS_MESA`, `EGL_PLATFORM_ANDROID_KHR`)
and the default display with `CHAMELEON_EGL_DEFAULT=1`; X11 windows and other
compositors still go to Mesa. Check it
with the probe: `CHAMELEON_PROBE_GLVND=1 CHAMELEON_EGL_DEFAULT=1 ./ahb_probe`.

Useful variables: `CHAMELEON_WAIT` (seconds to wait for the app, default 30),
`CHAMELEON_MODE=1080x2400@120` (mode if the app isn't open), `CHAMELEON_DPI`
(default 400; decides KWin's default scale), `CHAMELEON_RESIZE=0` (KWin's
screen keeps its first size instead of following the app's window).

## Status / limits

- Input: direct touch, trackpad mode (a touchpad without buttons, so
  libinput's tap to click, scrolling and gestures apply), hardware keyboards
  (by Linux key code, so the desktop's keyboard layout applies) and the mouse
  (relative: the app captures it with Android's pointer capture, as games
  do) appear as evdev devices (`core/evdev.c`, served on
  `$XDG_RUNTIME_DIR/chameleon-evdev`); a mouse not captured yet is an
  absolute tablet. As with the kernel, input reaches only the programs
  reading a device at the time. The Android keyboard's text goes to text
  fields through `chameleon-ime`, the compositor's input method
  (`zwp_input_method_v1`, `ime/ime.c`); elsewhere it is typed as keys of the
  desktop's layout (`core/text.c`, `core/keymap.c`), so characters the layout
  lacks can't be typed into X11 apps, as with a hardware keyboard. Many
  phones report the right mouse button as Back: a
  Back key from a mouse is taken as the right button (so a real side Back
  button is one too). Some also turn the middle button into Home, which no
  app receives. While the mouse is captured, a touchpad is captured with it
  and not used yet.
- udev: `libchameleon_udev.so`, preloaded into the whole session by
  `chameleon`, is the device manager:
  every udev monitor is Chameleon's own, since an Android app can't open the
  kernel's uevent socket (`udev/udev.c`), subsystem `input` is Chameleon's
  input devices, and their `/dev/input/eventN` nodes open, stat, read and
  answer evdev ioctls like the kernel's (`udev/evdev.c`): each open file has
  evdev's queue (whole frames, `SYN_DROPPED` when it overflows), live key,
  axis and touch state, its own clock, grab and revoke, and subsystem `drm` is
  the fake card, `card0` with node `/dev/dri/card0`, which compositors find
  the way they find a GPU (KWin needs no `KWIN_DRM_DEVICES`). The card also
  has the sysfs entries a KMS-only platform card has (`/sys/dev/char/226:0`,
  `/sys/devices/platform/chameleon`, `/dev/dri` listing `card0`, no render
  node; `udev/sysfs.c`), from which libdrm names and describes it as for
  vkms, while it exists. Real entries in `/dev/dri` (a GPU's render node,
  where Android lets apps reach one) are listed next to it and stay real. Enumerations of other
  subsystems (`power_supply`, `backlight`, ...) pass through to the real
  libudev, without its monitor events. Any program in the session
  (`libinput debug-events`) sees the same devices while KWin runs.
- KWin's screen follows the app's window (rotation, keyboard) and the
  display's refresh rate (battery saver): the connector's one mode becomes
  the window's, and the card sends a hotplug uevent to udev monitors, which
  are connections to the same server (`core/kms.c`, `udev/udev.c`).
- Clipboard: text is shared with Android both ways through KWin's
  `ext_data_control_v1` (KWin 6.4+; `core/clipboard.c`), only in pipes.
- Apps using OpenGL ES render on the GPU through the vendor (`vendor/wayland.c`);
  a Qt built for desktop OpenGL (Termux's default) is steered to OpenGL ES
  through the EGL vendor string, as Qt does for NVIDIA's EGL
  (`CHAMELEON_EGL_QT=mesa` leaves such apps on Mesa instead);
  their buffers are registered with KWin's shim (`core/clients.c`) so KWin
  can import them. The app waits for the GPU before handing a frame over (no
  explicit sync yet). Apps drawing on the CPU still use `wl_shm`.
- KWin frames are copied once on the GPU by the app (copy mode) because KWin
  reuses a buffer as soon as the next one is latched, while SurfaceFlinger
  still scans it out for one more vsync.

## Test on a desktop

`shim/test/run-host-test.sh` (needs `libdrm-dev libwayland-dev libegl-dev
libgles-dev libudev-dev libinput-dev`) runs, with memfd stand-ins for `AHardwareBuffer`:

- the fake device driven by the real libdrm the way KWin does: enumeration,
  properties, gbm → prime → framebuffer, modesets, 60 page flips through a
  stub presenter, simulated vblank without one;
- typed text and `chameleon-ime` against a real libwayland-server with stub
  `wl_seat` and input-method globals (`input_test`);
- the evdev touchscreen, keyboard, mouse and touchpad seen by the system's
  real libinput through `libchameleon_udev.so`: listing, nodes, multitouch,
  keys, relative motion, buttons, wheel, touchpad motion and tapping
  (`evdev_test`);
- the screen following the app's window: the connector's mode and the
  card's hotplug uevent, filtered by subsystem and tag (`evdev_test`);
- libdrm naming and describing the card from its sysfs entries: primary
  node, no render node, node type, `drmGetDevice2` (`evdev_test`);
- udev monitors and libinput's udev backend starting on them, and other
  subsystems passing through, with the system's real libudev and libinput
  (`udev_test`);
- the GLSL fix-ups (`glsl_fix_test`);

`shim/test/run-conformance.sh` compares Chameleon's evdev devices with the
real kernel's: `evdev_conformance.c` runs the same reader-side checks on
the host against Chameleon's keyboard and touchscreen and, in a QEMU VM
running a real Linux kernel (`refvm.sh`), against uinput devices with the
same capabilities, and the two transcripts are diffed. Needs QEMU, a static
busybox, cpio, zstd and apt (it downloads Ubuntu's kernel once).

- the EGL vendor through the system's real glvnd, with a stand-in for
  Android's driver, both as any program and as KWin;
- a Wayland app rendering through the vendor into a real libwayland-server
  compositor: buffers registered with the KWin side, dmabuf parameters,
  releases, frame pacing and resizing.

CI runs it on every push.
