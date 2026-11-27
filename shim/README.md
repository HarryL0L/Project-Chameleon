# Chameleon for Termux (the shim)

Runs the Termux `kwin_wayland` (6.8, no Chameleon-specific patches) and a Plasma session on
the Chameleon app's screen, rendering with Android's GPU driver.

| file | replaces | does |
|---|---|---|
| `chameleon-kernel` (started by `chameleon`) | the kernel | fake KMS device: one DSI connector sized to the app (with an EDID), one CRTC, a primary and a cursor plane, atomic commits → PRESENT, FRAME_DONE → page-flip event on the display's vsync; the render node `renderD128`; the input devices, the link to the app and the input thread |
| `chameleon-init` (the session's first process) | init / `systemd --user` | adopts every program of the session whose parent exits (the bus's services, `chameleon-kernel`, a sound server) and reaps it, so none becomes an orphan of Android's init, which some phones' process cleaners kill; passes SIGTERM/SIGHUP and the exit status through |
| `libchameleon.so` (LD_PRELOAD, every program of the session) | the device node | `/dev/dri/card0`: open, stat and DRM ioctls go to `chameleon-kernel` as the system call's arguments (`core/interpose.c`, `core/card.c`); apps' GPU buffers for the compositor's dmabuf imports (shared with `chameleon-kernel` as GEM handles on the render node, `vendor/window.c`); crash reporter where Android's driver renders |
| `libgbm.so` (LD_LIBRARY_PATH, the session) | Mesa's libgbm | `gbm_bo` = `AHardwareBuffer`, fd = the gralloc handle's dmabuf; `gbm_bo_import` takes the dmabufs of the session's buffers |
| `libEGL_chameleon.so` (glvnd vendor) | Mesa's EGL vendor | EGL/GLES on `/system/lib64/libEGL.so` / `libGLESv2.so` (→ vendor driver); adds the GBM platform, dmabuf import (as `AHardwareBuffer`) and the card as an `EGLDevice` (`EGL_EXT_device_drm`, `EGL_EXT_device_drm_render_node`) in a Chameleon session, and the Mali workarounds: `#if GL_*` → `#if defined(GL_*)` in GLSL, hidden timer-query extension, no robust/priority contexts; window configs only in the window buffers' 8-bit RGBA/RGBX (a 10-bit config stays off-screen) |
| `libvulkan_chameleon.so` (a driver for the Vulkan loader) | Mesa's Vulkan driver | Vulkan on `/system/lib64/libvulkan.so` (→ vendor driver), up to the version and with the extensions `vulkan/vk_forward.h` has the functions of; names the card's nodes (`VK_EXT_physical_device_drm`); Wayland and X11 windows (`VK_KHR_wayland_surface`, `VK_KHR_xcb_surface`, `VK_KHR_xlib_surface`, `VK_KHR_swapchain`) in a Chameleon session |

## Run

1. Install `chameleon.apk` from the CI artifact and open it.
2. Install the Termux package from the artifact
   (`apt install ./chameleon_<version>_aarch64.deb`; files go to
   `$PREFIX/lib/chameleon`, commands to `$PREFIX/bin`). A folder built with
   `shim/build-android.sh` works without the package: copy it into Termux
   (not `/sdcard`, which is `noexec`), replacing an older copy completely,
   and run
   `chmod +x chameleon/chameleon* && chameleon/chameleon --install`.
   Then:

```sh
chameleon kwin_wayland               # just KWin
chameleon kwin_wayland konsole       # KWin plus a first app
chameleon startplasma-wayland        # a Plasma session
```

`chameleon` preloads `libchameleon.so` and `libchameleon_udev.so` into the
whole session and puts its own directory first in `LD_LIBRARY_PATH` (our
`libgbm.so`), so every program sees the same device layer, as a device node
is there for every program. It runs below `chameleon-init`, clears `DISPLAY` / `WAYLAND_DISPLAY`, starts
the app's broker (which hands `chameleon-kernel`'s connection to the app;
log in `$XDG_RUNTIME_DIR/chameleon-broker.log`) and `chameleon-kernel` (the
card, input devices, app link and apps' GPU buffers for the whole session;
it exits with the session) and runs the command (in a new D-Bus session if
there is none). No `--drm` is needed anywhere: with neither
`WAYLAND_DISPLAY` nor `DISPLAY` set, KWin, Weston and sway pick their DRM
backend by themselves, so plasma-workspace needs no change either. The
compositor's Wayland socket is learnt from whichever program binds it
(`kwin_wayland_wrapper` for KWin); `CHAMELEON_WAYLAND_SOCKET` names it for a
compositor given its socket from outside the session. An Xwayland built
with glamor renders X11 windows on Android's OpenGL ES through the render
node and hands them to the compositor as session buffers; X11 OpenGL apps
keep Mesa's GLX (`libGLX_chameleon.so.0` is a name for `libGLX_mesa.so.0`);
with zink they run on the card's Vulkan driver, whose X11 windows Xwayland
shows (DRI3 pixmaps of session buffers, imported by its gbm).

The compositor loads the EGL vendor through Termux's own glvnd
(`libEGL.so.1`, `libGLESv2.so.2`): `chameleon` sets
`__EGL_VENDOR_LIBRARY_FILENAMES` for the whole session, with this vendor
first and the installed ones (Mesa) after it.

### A record for a bug report

`chameleon --debug-log PROGRAM...` runs the session as usual and records it:
everything its programs print, each line with its time, in
`~/chameleon-debug-DATE-TIME.log` (`CHAMELEON_DEBUG_DIR` names another
directory). The file starts with the package's version and the phone's model,
Android version and chipset; `chameleon-kernel` adds the first of every
request the card gets and of every refusal with the program that got it,
requests a program waited 8 ms or more for, and at the end the session's
report. The file stays under 28 MiB: of a longer session its start and its
most recent part are kept. It holds what programs printed, which can name
files: read it before sharing it.

`chameleon --report` prints the report of the running session from another
terminal: frames committed, shown and simulated with the time from commit to
shown, every card request by name with its count, refusals and time, each
kind of refusal with the program that first got it, and the card's reason for
every refused commit.

### The EGL vendor for other programs

`chameleon-vendor-install` registers `libEGL_chameleon.so` with
glvnd for every Termux program (`--remove` undoes it); `chameleon` already
does the same for its session. It takes Wayland windows whose compositor
runs on Chameleon (rendered on the GPU, handed over as `AHardwareBuffer`s),
off-screen displays (`EGL_PLATFORM_SURFACELESS_MESA`, `EGL_PLATFORM_ANDROID_KHR`)
and the default display with `CHAMELEON_EGL_DEFAULT=1`; X11 windows and other
compositors still go to Mesa. Check it
with the probe: `CHAMELEON_PROBE_GLVND=1 CHAMELEON_EGL_DEFAULT=1 ./ahb_probe`.

Useful variables: `CHAMELEON_WAIT` (seconds to wait for the app, default 30),
`CHAMELEON_MODE=1080x2400@120` (mode if the app isn't open), `CHAMELEON_DPI`
(default 400; decides KWin's default scale), `CHAMELEON_RESIZE=0` (KWin's
screen keeps its first size instead of following the app's window),
`CHAMELEON_VULKAN=0` (the session without the card's Vulkan driver) or
`CHAMELEON_VULKAN=all` (every installed Vulkan driver next to it),
`CHAMELEON_VULKAN_DRIVER=/a/driver.so` (another driver below the card's
Vulkan driver in place of the phone's own; see below),
`CHAMELEON_ANDROID_NAMESPACE=0` (Android's libraries loaded among the
program's own, as before there was a namespace for them; see below),
`CHAMELEON_SYNC_WAIT=1` (programs wait for the GPU before handing any frame
over, whoever reads it; see the buffers' fences below).

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
  the way they find a GPU (KWin needs no `KWIN_DRM_DEVICES`), and its render
  node `renderD128` (`/dev/dri/renderD128`): the same device for programs
  that only render and share buffers, never master and limited to the
  ioctls the kernel allows render nodes. The card also has the sysfs
  entries a platform card with a render node has (`/sys/dev/char/226:0`
  and `226:128`, `/sys/devices/platform/chameleon`, `/dev/dri` listing
  `card0` and `renderD128`; `udev/sysfs.c`), from which libdrm names and
  describes it as for virtio-gpu, while it exists, and EGL lists it as the
  GPU's device (`EGL_EXT_device_drm` with `_render_node`; wlroots opens its
  display there). Real entries in `/dev/dri` are listed next to them and
  stay real, except a real `card0` or `renderD128`, which ours hide. Enumerations of other
  subsystems (`power_supply`, `backlight`, ...) pass through to the real
  libudev, without its monitor events. Any program in the session
  (`libinput debug-events`) sees the same devices while KWin runs.
- KWin's screen follows the app's window (rotation, keyboard) and the
  display's refresh rate (battery saver): the connector's one mode becomes
  the window's, and the card sends a hotplug uevent to udev monitors, which
  are connections to the same server (`core/kms.c`, `udev/udev.c`).
- Clipboard: text is shared with Android both ways through KWin's
  `ext_data_control_v1` (KWin 6.4+; `core/clipboard.c`), only in pipes.
  Android's text copied before the desktop is reachable (session starting,
  compositor restarting) becomes its clipboard once it is, unless the desktop
  copied something since.
- Apps using OpenGL ES render on the GPU through the vendor (`vendor/wayland.c`, `vendor/window.c`);
  a Qt built for desktop OpenGL (Termux's default) is steered to OpenGL ES
  through the EGL vendor string, as Qt does for NVIDIA's EGL
  (`CHAMELEON_EGL_QT=mesa` leaves such apps on Mesa instead);
  their buffers are shared with `chameleon-kernel` as GEM handles on the
  app's render node, as a driver's buffers are kernel objects, where the
  compositor's dmabuf import and PRIME on the card's nodes (wlroots'
  test import) find them, also for apps behind a Wayland
  proxy (Firefox's `wayland-proxy-N`). A second compositor nested inside the
  session can't import them. Apps drawing on the CPU still use `wl_shm`.
- Session buffers have fences, as dmabufs have on Linux, kept in
  `chameleon-kernel` (Android's drivers attach none, and its older kernels
  lack the calls): `poll()` on a buffer's dmabuf says whether GPU work on it
  is pending, `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` gives the fence to wait for,
  `DMA_BUF_IOCTL_IMPORT_SYNC_FILE` adds one. An app window's frame carries
  the fence of its GPU work, and so does a dmabuf a program hands the
  compositor itself (Xwayland's glamor). No driver makes a reader wait,
  though: only for a buffer whose reader has asked for its fences (KWin
  does for each frame) is the wait left to it; otherwise, and for X11
  windows, the program waits for the GPU before it hands the frame over, as
  it does for every frame with `CHAMELEON_SYNC_WAIT=1`. Limits: only
  `poll()` of the dmabuf alone is answered (polled with other files, or
  with epoll, it always reads as ready), and only fences of GPU work that
  writes are made.
- EGL is reported as 1.5 even where the driver says 1.4 (Mali): Android's
  libEGL has every 1.5 function for any driver since Android 10, mapped to
  the KHR ones, and libepoxy users (KWin 6.8's fence waits) need a 1.5
  display. 1.5 features a driver lacks are still missing (Mali:
  `EGL_KHR_gl_texture_3D_image`, `EGL_KHR_cl_event2`) and fail with an EGL
  error.
- Every buffer of the session is one object in `chameleon-kernel`, as a GEM
  object in the kernel: any process's gbm buffer and any app's registered
  one imports by PRIME on either node and through EGL in any process, and
  stays while its dmabuf is held anywhere (KWin 6.8 keeps only the fd;
  another process may have it), until the kernel's count of the dmabuf's
  references (fdinfo) drops to `chameleon-kernel`'s own; on kernels without
  that count, only its own fds are seen (`core/ahb.c`). A process keeps a
  buffer it made that way itself only while `chameleon-kernel` doesn't have
  it. A dmabuf of another device can't be imported (ENODEV, as virtio-gpu).
- KWin frames are copied once on the GPU by the app (copy mode) because KWin
  reuses a buffer as soon as the next one is latched, while SurfaceFlinger
  still scans it out for one more vsync.
- Vulkan: `libvulkan_chameleon.so` is listed to the session's Vulkan loader
  (`VK_ADD_DRIVER_FILES`) and runs programs on Android's Vulkan driver, at
  that driver's Vulkan version. Another installed driver for the same GPU
  (Mesa's panvk or turnip, a wrapper of Android's) would list the GPU a
  second time, in no set order, so the session's loader is told to use the
  card's driver and Mesa's software one (`VK_LOADER_DRIVERS_SELECT`, kept
  if already set); one program runs on another driver with that variable
  set for it, e.g. `VK_LOADER_DRIVERS_SELECT='*panfrost*'`. The loader and
  Android's `libvulkan.so` both
  write into the first word of instances, physical devices, devices, queues
  and command buffers, so programs hold wrappers of those and every call
  swaps them for the driver's own (`vulkan/vulkan.c`; the forwarders are
  generated from Khronos's header in `include/vulkan` by `vulkan/gen_vk.py`, which also finds
  each function that carries such an object in an array or a structure and
  stops if one isn't written by hand). A Wayland window is an Android
  swapchain on the same kind of window an EGL surface has (an
  `AImageReader`, `vendor/window.c`): each presented image goes to the
  compositor as it is, shared through the render node (`vulkan/wsi.c`).
  Its formats are 8-bit RGBA, plain or sRGB (no BGRA: Android's buffers
  have none); FIFO waits for the compositor's frame callback, mailbox
  doesn't; its size is the swapchain's. Android stores an opaque window's
  images as RGBA all the same, so the compositor's dmabuf import reads
  alpha as 1 from a buffer declared XBGR or XRGB, as a driver importing
  that format does (`egl/egl.c`). An X11 window (xcb or Xlib) is fed the
  same way, but its frames go to the X server as Mesa's drivers send
  theirs: each buffer becomes a DRI3 pixmap once (Xwayland's gbm finds the
  session's buffer behind the dmabuf), each frame a `PresentPixmap`;
  `IdleNotify` gives a buffer back, `CompleteNotify` paces FIFO, and Present's
  events come on a queue of the driver's, not the program's. Its size is the
  window's own: a present after the window changed size is
  `VK_SUBOPTIMAL_KHR`. It needs an X server with DRI3 and Present (Xwayland
  with glamor); libxcb and its DRI3 and Present parts, and libX11-xcb for
  Xlib, are loaded from Termux when there. Left out for now: dma-buf import
  and export, the debug extensions (the loader and the validation layer
  provide those).
- Android's own libraries (`libEGL.so`, `libGLESv2.so`, `libvulkan.so`,
  `libnativewindow.so`, `libmediandk.so`) are loaded apart from the
  program's: into the linker namespace Termux keeps for Android's libraries
  (`libtermux-platform-ns.so` of its package `libandroid-stub`, one
  namespace per process, shared with Termux's own stand-ins for Android's
  libraries). Loaded among the program's own they answer to their names
  there: a program that asks for `libvulkan.so`, as some do (KWin among
  them), then gets Android's Vulkan, without the card's driver and its
  windows, where it means the Vulkan loader; and Android's libraries take
  Termux's `libz.so` for theirs. That is still how it goes without Termux's
  helper or with `CHAMELEON_ANDROID_NAMESPACE=0`. The price: the phone's GPU
  driver takes the few of Android's libraries it needs itself
  (`libnativewindow.so` and what that needs) from the program's namespace,
  so those are in the process a second time; and a GPU driver that needs
  `libvulkan.so` itself brings that name back.
- Another GPU driver below the card's Vulkan driver, experimental:
  `CHAMELEON_VULKAN_DRIVER=/a/driver.so`, for a session or for one program,
  has Android's Vulkan run that driver in place of the phone's own - a build
  for Android of Mesa's turnip or panvk, which talks to the phone's GPU
  kernel driver and knows Android's buffers. Windows, the card's nodes and
  everything else stay the card's driver's. Android's `libvulkan.so` has no
  setting for this: it is done by
  [libadrenotools](https://github.com/bylaws/libadrenotools), which opens a
  copy of that library of its own and answers the copy's request for the
  driver, using functions inside Android's linker that aren't public (arm64,
  Android 9 and later; an Android update may end it). It isn't part of
  Chameleon: it is Termux's package `libadrenotools`, whose
  `$PREFIX/lib/libadrenotools.so` the driver loads when the variable is
  set. The library's two hook libraries are handed to it in a directory of
  the session's that links to them and holds nothing else, because it has
  Android's libraries looked up there too and Termux's `lib` has files named
  as some of those. The driver file must be where programs may load
  libraries from (Termux's home, not shared storage). If any of it fails the
  program is told why and runs on the phone's driver; `vulkaninfo` shows
  which driver answered (`driverName`). The compositor's OpenGL ES stays the
  phone's.

## Test on a desktop

`shim/test/run-host-test.sh` (needs `libdrm-dev libwayland-dev libegl-dev
libgles-dev libudev-dev libinput-dev libvulkan-dev mesa-vulkan-drivers
vulkan-validationlayers vulkan-tools libxcb1-dev libxcb-dri3-0
libxcb-present0 libx11-xcb1`) runs, with memfd stand-ins for `AHardwareBuffer`:

- the fake device driven by the real libdrm the way KWin does: enumeration,
  properties, gbm → prime → framebuffer, modesets, 60 page flips through a
  stub presenter, simulated vblank without one; and the way older programs
  do: `SETCRTC`, `PAGE_FLIP` with and without events, the legacy cursor,
  `WAIT_VBLANK`, DPMS; dumb buffers drawn into through
  `mmap` of the card, shown by `SETCRTC`, a flip and `DIRTYFB`, exported
  and imported by PRIME and kept while the exported file is open; the
  buffers' fences through `poll()` and the two sync-file calls, from another
  process too;
- `chameleon-init` adopting and reaping a program that detached, signals and
  the exit status passing through (`init_test`);
- the session's report after `kms_test` (its requests, refusals and the
  card's reasons), the recorder of `--debug-log` keeping a long session's
  start and end under its limit, and the launcher's option with a stand-in
  session;
- typed text and `chameleon-ime` against a real libwayland-server with stub
  `wl_seat` and input-method globals (`input_test`);
- the evdev touchscreen, keyboard, mouse and touchpad seen by the system's
  real libinput through `libchameleon_udev.so`: listing, nodes, multitouch,
  keys, relative motion, buttons, wheel, touchpad motion and tapping
  (`evdev_test`);
- the screen following the app's window: the connector's mode and the
  card's hotplug uevent, filtered by subsystem and tag (`evdev_test`);
- libdrm naming and describing the card from its sysfs entries: primary
  and render nodes, node types, `drmGetDevice2` (`evdev_test`);
- the render node's files: never master, only render ioctls, PRIME with
  the primary node, across exec (`kms_test`);
- udev monitors and libinput's udev backend starting on them, and other
  subsystems passing through, with the system's real libudev and libinput
  (`udev_test`);
- the GLSL fix-ups (`glsl_fix_test`);
- the Vulkan driver under the system's Vulkan loader, with Mesa's lavapipe
  in Android's place: its lists, every kind of wrapped object reaching the
  driver as the driver's own, frames drawn through it, the card's nodes in a
  session; once more with the Khronos validation layer checking each call;
  `vulkaninfo`; and `CHAMELEON_VULKAN_DRIVER` with a stand-in for
  libadrenotools (`vulkan_test`);
- Android's libraries opened through Termux's namespace helper where there
  is one (a stand-in that writes down what it is asked for), each of them
  and libadrenotools, and not with `CHAMELEON_ANDROID_NAMESPACE=0`;
- a Vulkan program's window against a real libwayland-server: surface,
  swapchains (FIFO and mailbox, opaque and with alpha), each frame's pixels
  as the compositor reads them from the shared buffer
  (`wayland_client_test`, also under the validation layer);
- the same on X11, against a stand-in X server with DRI3 and Present: the
  pixmaps of the session's buffers, each frame's pixels as the server reads
  them, buffers given back, a window that changes size, the window's
  objects gone with the surface, an X server without DRI3
  (`x11_window_test`, also under the validation layer); and `gbm_bo_import`
  of another process's buffer (`kms_test`);

`shim/test/run-conformance.sh` compares Chameleon's evdev devices with the
real kernel's: `evdev_conformance.c` runs the same reader-side checks on
the host against Chameleon's keyboard and touchscreen and, in a QEMU VM
running a real Linux kernel (`refvm.sh`), against uinput devices with the
same capabilities, and the two transcripts are diffed; likewise the card
against vkms (`kms_conformance.c`) and its render node against virtio-gpu's
(`render_conformance.c`). Needs QEMU, a static
busybox, cpio, zstd and curl (it downloads Ubuntu's kernel from Launchpad
once). CI runs it on every push too.

- the EGL vendor through the system's real glvnd, with a stand-in for
  Android's driver, both as any program and as KWin;
- a Wayland app rendering through the vendor into a real libwayland-server
  compositor: buffers registered with the KWin side, dmabuf parameters,
  releases, frame pacing and resizing.

CI runs it on every push.
