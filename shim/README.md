# KWin shim

Runs the Termux `kwin_wayland --drm` (6.7.5, unmodified) on the Chameleon
app's screen, rendering with Android's GPU driver.

| file | replaces | does |
|---|---|---|
| `libchameleon.so` (LD_PRELOAD) | the kernel | fake KMS device at `$PREFIX/tmp/chameleon-card0`: one DSI connector sized to the app, one CRTC, one primary plane, atomic commits → PRESENT, FRAME_DONE → page-flip event |
| `libgbm.so` (LD_LIBRARY_PATH) | Mesa's libgbm | `gbm_bo` = `AHardwareBuffer`, fd = the gralloc handle's dmabuf |
| `libEGL_chameleon.so` (glvnd vendor) | Mesa's EGL vendor | EGL/GLES on `/system/lib64/libEGL.so` / `libGLESv2.so` (→ vendor driver); adds the GBM platform and dmabuf import (as `AHardwareBuffer`) inside KWin, and the Mali workarounds: `#if GL_*` → `#if defined(GL_*)` in GLSL, hidden BGRA8888 / timer-query extensions, no robust/priority contexts |

## Run

1. Install `chameleon.apk` from the CI artifact and open it.
2. Copy the artifact's `kwin-shim/` directory into Termux (not `/sdcard`,
   which is `noexec`). Replace an older copy completely (`rm -rf kwin-shim`
   first), then:

```sh
chmod +x kwin-shim/chameleon* kwin-shim/bin/*
kwin-shim/chameleon --install        # once: `chameleon` in $PREFIX/bin

chameleon kwin_wayland               # just KWin
chameleon kwin_wayland konsole       # KWin plus a first app
chameleon startplasma-wayland        # a Plasma session
```

`chameleon` puts `kwin-shim/bin` first in `PATH`, clears `DISPLAY` /
`WAYLAND_DISPLAY` and runs the command (in a new D-Bus session if there is
none). Every `kwin_wayland` started inside - directly, by
`kwin_wayland_wrapper` or by `startplasma-wayland` - is then
`bin/kwin_wayland`, which attaches the shim to that one process and runs the
real, unmodified KWin. No `--drm` is needed anywhere: with neither
`WAYLAND_DISPLAY` nor `DISPLAY` set KWin picks the DRM backend by itself (the
script adds `--drm` anyway unless another backend is asked for), so
plasma-workspace needs no change either.

KWin loads the EGL vendor through Termux's own glvnd (`libEGL.so.1`,
`libGLESv2.so.2`): the script points `__EGL_VENDOR_LIBRARY_FILENAMES` at it
for KWin only. `chameleon-kwin ...` still works (= `chameleon kwin_wayland ...`).

### The EGL vendor for other programs

`kwin-shim/chameleon-vendor-install` registers `libEGL_chameleon.so` with
glvnd for every Termux program (`--remove` undoes it); `chameleon` already
does the same for its session. It takes Wayland windows whose compositor is
a Chameleon KWin (rendered on the GPU, handed over as `AHardwareBuffer`s),
off-screen displays (`EGL_PLATFORM_SURFACELESS_MESA`, `EGL_PLATFORM_ANDROID_KHR`)
and the default display with `CHAMELEON_EGL_DEFAULT=1`; X11 windows and other
compositors still go to Mesa. Check it
with the probe: `CHAMELEON_PROBE_GLVND=1 CHAMELEON_EGL_DEFAULT=1 ./ahb_probe`.

Useful variables: `CHAMELEON_WAIT` (seconds to wait for the app, default 30),
`CHAMELEON_MODE=1080x2400@120` (mode if the app isn't open), `CHAMELEON_DPI`
(default 400; decides KWin's default scale).

## Status / limits

- Input: touch (direct or trackpad), mouse and keyboard from the app, injected
  through KWin's fake-input protocol (`core/input.c`).
- The mode is fixed when KWin starts; later app window size changes are
  scaled by the app.
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
libgles-dev`) runs, with memfd stand-ins for `AHardwareBuffer`:

- the fake device driven by the real libdrm the way KWin does: enumeration,
  properties, gbm → prime → framebuffer, modesets, 60 page flips through a
  stub presenter, simulated vblank without one;
- the input path against a real libwayland-server fake-input global;
- the EGL vendor through the system's real glvnd, with a stand-in for
  Android's driver, both as any program and as KWin.
- a Wayland app rendering through the vendor into a real libwayland-server
  compositor: buffers registered with the KWin side, dmabuf parameters,
  releases, frame pacing and resizing.

CI runs it on every push.
