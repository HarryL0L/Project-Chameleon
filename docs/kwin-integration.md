# KWin integration notes

Target: KWin 6.7.5 as packaged in HarryL0L/termux-packages `dev/c-test`
(`x11-packages/kwin`), which makes udev and libinput optional.

## Output: `--drm` with a fake device (no KWin changes)

- `KWIN_DRM_DEVICES=<path>` bypasses udev discovery entirely
  (`DrmBackend::initialize` → `addGpu(path)`), so the package's stub `Udev`
  (reports no devices) is fine.
- `addGpu` → `Session::openRestricted(path)` (noop session = plain `open`)
  → `drmIsKMS(fd)` → `DrmDevice::openWithAuthentication` (`drmGetVersion`,
  GBM device) → `EGL_PLATFORM_GBM_KHR` display.
- The shim (LD_PRELOAD) fakes that device: libdrm KMS calls, libgbm (bo =
  AHardwareBuffer, fd = the gralloc handle's dmabuf fd), EGL → system libEGL.
- Atomic commit → `CHAM_PRESENT`; `CHAM_FRAME_DONE` → page-flip event on the
  fake fd. One pending flip, exactly like the demo's default pacing.

## Input: fake input from inside KWin's own process

- `DrmBackend::createInputBackend()` returns nullptr without libinput, so the
  DRM backend has no input devices of its own.
- `org_kde_kwin_fake_input` supports pointer (relative + absolute), buttons,
  axis, touch down/motion/up/frame and keyboard key/keysym.
- It is on KWin's interface blacklist, but `allowInterface()` returns true
  for any client whose pid == KWin's pid, and `authenticate` is accepted
  unconditionally (`// TODO: make secure`).
- So the shim, already loaded inside KWin, opens a Wayland client connection
  to KWin's own socket on a helper thread, binds fake input, and feeds it
  touch/keys that the presenter app sends over the chameleon socket.

## Mali-G77: no GPU timer queries

KWin measures render time with `GL_EXT_disjoint_timer_query`
(`glGetInteger64v(GL_TIMESTAMP)` / `glQueryCounter`). On Mali-G77 (driver
r32p1) the next write into texture memory after that dies with SIGBUS
(NULL+0x29) deep in the driver, whether the pixels come from client memory,
a PBO or `glTexSubImage2D`. The launcher sets `KWIN_NO_TIMER_QUERY=1` and the
GLES shim hides the extension by default; KWin then falls back to CPU-side
render time estimates.
