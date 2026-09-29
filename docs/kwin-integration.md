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

Implemented in `shim/core/input.c`:

- The socket path comes from KWin's own `bind()` of `$XDG_RUNTIME_DIR/wayland-N`
  (interposed), libwayland-client is `dlopen()`ed, and the protocol tables are
  written out by hand (no wayland-scanner at build time).
- The app sends `CHAM_INPUT` messages with positions as fractions of its
  surface; the shim maps them onto the output's logical geometry from
  `zxdg_output_v1` (falls back to the KMS mode size), so output scaling works.
- Keys are X keysyms (`keyboard_keysym`, v6): KWin finds the keycode and Shift
  level in its keymap, or maps a spare keycode for anything else, so text from
  the Android keyboard works in any language.
- `shim/test/input_test.c` runs the whole path against a real
  libwayland-server with a stub fake-input global.

## Screen size: the output follows the app's window

- No udev in the Termux package, so `DrmBackend::handleUdevEvent` never runs
  and a changed connector is never re-read. The other paths to
  `DrmGpu::updateOutputs()` (`repairPresentation()` after a failed commit)
  would need a failing frame to trigger them.
- Output management does work: `kde_output_management_v2` (v21 in 6.7.5) is
  blacklisted for other clients but, like fake input, allowed for KWin's own
  pid. `set_custom_modes` (v18+) makes `DrmOutput::refreshModes` add a
  `DrmConnector::generateMode()` CVT mode, then `mode` switches to it through
  `Workspace::applyOutputConfiguration` → atomic modeset.
- libxcvt rounds `hdisplay` down to a multiple of 8, so `output.c` asks for
  the width rounded up and the app crops the rest (`place()` in
  `presenter.cpp`); `output_visible()` maps input onto the visible part.
- The fake KMS device accepts any mode size (the plane must match the mode);
  the connector keeps advertising only the app's size at startup.
- KWin stores the custom mode and picks it again at the next start; the shim
  then compares it with the app's size and switches if needed.
- `kde_output_device_registry_v2` must be bound at v21+ (older binds are a
  protocol error), so the device and its modes are v21 objects: the event
  tables in `shim/core/output_proto.h` go up to v21.
- `shim/test/output_test.c` plays KWin's side (device, modes, management,
  CVT-like widths) against the real shim.

## Mali-G77: no GPU timer queries

KWin measures render time with `GL_EXT_disjoint_timer_query`
(`glGetInteger64v(GL_TIMESTAMP)` / `glQueryCounter`). On Mali-G77 (driver
r32p1) the next write into texture memory after that dies with SIGBUS
(NULL+0x29) deep in the driver, whether the pixels come from client memory,
a PBO or `glTexSubImage2D`. The launcher sets `KWIN_NO_TIMER_QUERY=1` and the
GLES shim hides the extension by default; KWin then falls back to CPU-side
render time estimates.
