# KWin shim

Runs the Termux `kwin_wayland --drm` (6.7.5, unmodified) on the Chameleon
app's screen, rendering with Android's GPU driver.

| file | replaces | does |
|---|---|---|
| `libchameleon.so` (LD_PRELOAD) | the kernel | fake KMS device at `$PREFIX/tmp/chameleon-card0`: one DSI connector sized to the app, one CRTC, one primary plane, atomic commits → PRESENT, FRAME_DONE → page-flip event |
| `libgbm.so` (LD_LIBRARY_PATH) | Mesa's libgbm | `gbm_bo` = `AHardwareBuffer`, fd = the gralloc handle's dmabuf |
| `libEGL.so.1` (LD_LIBRARY_PATH) | glvnd's libEGL | forwards to `/system/lib64/libEGL.so` (→ vendor driver), adds the GBM platform and dmabuf import (as `AHardwareBuffer`) |
| `libGLESv2.so.2` (symlink, made by the launcher) | glvnd's libGLESv2 | Android's `libGLESv2.so` |

## Run

1. Install `chameleon.apk` from the CI artifact and open it.
2. Copy the artifact's `kwin-shim/` directory into Termux (not `/sdcard`,
   which is `noexec`), then:

```sh
chmod +x kwin-shim/chameleon-kwin
kwin-shim/chameleon-kwin            # or: kwin-shim/chameleon-kwin konsole
```

Useful variables: `CHAMELEON_WAIT` (seconds to wait for the app, default 30),
`CHAMELEON_MODE=1080x2400@120` (mode if the app isn't open), `CHAMELEON_DPI`
(default 400; decides KWin's default scale).

## Status / limits

- Output only: no input yet (next: touch/keys → KWin's fake-input protocol).
- The mode is fixed when KWin starts; later app window size changes are
  scaled by the app.
- Apps inside KWin render in software (`wl_shm`); client dmabufs are refused
  because there is no `AHardwareBuffer` behind them.
- KWin frames are copied once on the GPU by the app (copy mode) because KWin
  reuses a buffer as soon as the next one is latched, while SurfaceFlinger
  still scans it out for one more vsync.

## Test on a desktop

`shim/test/run-host-test.sh` (needs `libdrm-dev`) builds the core with memfd
stand-ins for `AHardwareBuffer` and drives the fake device with the real
libdrm the way KWin does: enumeration, properties, gbm → prime → framebuffer,
modesets, 60 page flips through a stub presenter, simulated vblank without
one. CI runs it on every push.
