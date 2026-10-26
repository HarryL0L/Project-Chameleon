# KWin integration notes

Target: KWin 6.7.5 as packaged in
[BullyMaguire-lol/termux-packages `dev/kwin-wayland`](https://github.com/BullyMaguire-lol/termux-packages/tree/dev/kwin-wayland)
(`x11-packages/kwin`), which makes udev and libinput optional.

## Output: `--drm` with a fake device (no KWin changes)

- `KWIN_DRM_DEVICES=<path>` bypasses udev discovery entirely
  (`DrmBackend::initialize` → `addGpu(path)`), so the package's stub `Udev`
  (reports no devices) is fine.
- `addGpu` → `Session::openRestricted(path)` (noop session = plain `open`)
  → `drmIsKMS(fd)` → `DrmDevice::openWithAuthentication` (`drmGetVersion`,
  GBM device) → `EGL_PLATFORM_GBM_KHR` display.
- `chameleon-kernel` serves that device and `libchameleon.so` (LD_PRELOAD,
  every program of the session) makes `/dev/dri/card0` its node (DRM ioctls go there as system calls), the
  fake `libgbm.so` (LD_LIBRARY_PATH) makes a bo an AHardwareBuffer (fd = the
  gralloc handle's dmabuf fd), and EGL goes through glvnd to the
  `libEGL_chameleon.so` vendor, which forwards to Android's libEGL.
- Atomic commit → `CHAM_PRESENT`; `CHAM_FRAME_DONE` → page-flip event on the
  fake fd. One pending flip, as with a real KMS driver.
- KWin's DPMS off (idle, Plasma's power management) is an atomic commit
  with CRTC `ACTIVE` 0 and no mode; the shim sends `CHAM_SCREEN` (off, and on
  again), and the app shows "tap to wake", which sends a pointer nudge: any
  input wakes KWin's screen.

## Input: evdev devices and an input method

- KWin's DRM backend runs libinput, which finds Chameleon's devices through
  udev (`libchameleon_udev.so`) and reads their `/dev/input/eventN` nodes as
  it would a kernel's (`shim/core/evdev.c`, `shim/udev/`).
- The Android keyboard: the package installs `chameleon-ime.desktop`
  (`X-KDE-Wayland-VirtualKeyboard=true`, so the Virtual Keyboard settings
  page lists it) and a system-wide `$PREFIX/etc/xdg/kwinrc` naming it as
  `[Wayland] InputMethod`; the user's own kwinrc overrides that. KWin gives it a
  `zwp_input_method_v1` context while the focused client has text-input
  enabled, and it commits the keyboard's text, composing text and deletions
  there (`shim/ime/ime.c`). Without a context, `shim/core/text.c` types the
  text as keys of the keymap KWin sends every `wl_keyboard`
  (`shim/core/keymap.c`).
- `shim/core/input.c` connects to KWin's own Wayland socket as a client, for
  the clipboard and that keymap. The socket path comes from the `bind()` of
  `$XDG_RUNTIME_DIR/wayland-N` (interposed in every program; for Plasma,
  `kwin_wayland_wrapper` binds it and passes KWin only its fd), or from
  `$CHAMELEON_WAYLAND_SOCKET`;
  libwayland-client is `dlopen()`ed, and the protocol tables are written out
  by hand (no wayland-scanner at build time).
- `shim/test/evdev_test.c` runs the devices against the system's libinput,
  `shim/test/input_test.c` the text path against a real libwayland-server.

## Screen size: the output follows the app's window

- The connector's one mode is the app window's size and refresh rate. When
  the window changes, the card sends a udev `change` uevent with
  `HOTPLUG=1`, and KWin re-reads the connector as it does for a real monitor
  (`DrmBackend::handleUdevEvent` → `DrmGpu::updateOutputs()`).
- The EDID stays the one of the first size, so KWin keeps treating it as the
  same monitor (scale and settings stay).
- Builds that used KWin's output management saved custom modes in
  `~/.config/kwinoutputconfig.json`; KWin keeps re-adding them, so that file
  has to go once.

## Mali-G77: no GPU timer queries

KWin measures render time with `GL_EXT_disjoint_timer_query`
(`glGetInteger64v(GL_TIMESTAMP)` / `glQueryCounter`). On Mali-G77 (driver
r32p1) the next write into texture memory after that dies with SIGBUS
(NULL+0x29) deep in the driver, whether the pixels come from client memory,
a PBO or `glTexSubImage2D`. The GLES shim hides the extension by default;
on OpenGL ES KWin uses timer queries only with it, so it falls back to
CPU-side render time estimates.
