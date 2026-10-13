# Chameleon for Termux (the shim)

Runs the Termux `kwin_wayland` (6.7.5, no Chameleon-specific patches) and a Plasma session on
the Chameleon app's screen, rendering with Android's GPU driver.

| file | replaces | does |
|---|---|---|
| `libchameleon.so` (LD_PRELOAD) | the kernel | fake KMS device at `$PREFIX/tmp/chameleon-card0`: one DSI connector sized to the app, one CRTC, one primary plane, atomic commits → PRESENT, FRAME_DONE → page-flip event |
| `libgbm.so` (LD_LIBRARY_PATH) | Mesa's libgbm | `gbm_bo` = `AHardwareBuffer`, fd = the gralloc handle's dmabuf |
| `libEGL_chameleon.so` (glvnd vendor) | Mesa's EGL vendor | EGL/GLES on `/system/lib64/libEGL.so` / `libGLESv2.so` (→ vendor driver); adds the GBM platform and dmabuf import (as `AHardwareBuffer`) inside KWin, and the Mali workarounds: `#if GL_*` → `#if defined(GL_*)` in GLSL, hidden BGRA8888 / timer-query extensions, no robust/priority contexts |

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
`WAYLAND_DISPLAY`, starts the app's broker (which hands KWin's connection to
the app; log in `$XDG_RUNTIME_DIR/chameleon-broker.log`) and runs the command
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
  `$XDG_RUNTIME_DIR/chameleon-evdev`). While a program such as KWin's
  libinput reads a device, its input goes there; otherwise, and for the soft
  keyboard and a mouse not captured yet, input goes through KWin's fake-input
  protocol (`core/input.c`). The touchpad has no such fallback. Many phones report the right mouse button as Back: a
  Back key from a mouse is taken as the right button (so a real side Back
  button is one too). Some also turn the middle button into Home, which no
  app receives. While the mouse is captured, a touchpad is captured with it
  and not used yet.
- udev: `libchameleon_udev.so`, preloaded into KWin, is the device manager:
  every udev monitor is Chameleon's own, since an Android app can't open the
  kernel's uevent socket (`udev/udev.c`), enumerations list Chameleon's input
  devices, and their `/dev/input/eventN` nodes open, stat and answer evdev
  ioctls like the kernel's (`udev/evdev.c`). Preloaded into any other
  program with the same `XDG_RUNTIME_DIR` (`libinput debug-events`), it
  shows the same devices while KWin runs.
- KWin's screen follows the app's window (rotation, keyboard) and the
  display's refresh rate (battery saver): the shim
  switches KWin's mode over `kde_output_management_v2`, adding a custom mode
  when needed (`core/output.c`).
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
- the input path against a real libwayland-server fake-input global;
- the evdev touchscreen, keyboard, mouse and touchpad seen by the system's
  real libinput through `libchameleon_udev.so`: listing, nodes, multitouch,
  keys, relative motion, buttons, wheel, touchpad motion and tapping
  (`evdev_test`);
- the screen following the app's window and refresh rate, against a
  stand-in for KWin's output management (`output_test`);
- udev monitors and libinput's udev backend starting on them, with the
  system's real libudev and libinput (`udev_test`);
- the GLSL fix-ups (`glsl_fix_test`);
- the EGL vendor through the system's real glvnd, with a stand-in for
  Android's driver, both as any program and as KWin;
- a Wayland app rendering through the vendor into a real libwayland-server
  compositor: buffers registered with the KWin side, dmabuf parameters,
  releases, frame pacing and resizing.

CI runs it on every push.
