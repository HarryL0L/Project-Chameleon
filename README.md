<p align="center">
  <img src="branding/logo.svg" width="168" alt="Project Chameleon logo: a chameleon wrapped around a Wayland-style disc">
</p>

<h1 align="center">Project Chameleon</h1>

<p align="center">
  <b>A real KDE Plasma / KWin Wayland desktop on Android, drawn by the phone's own GPU.</b><br>
  Unmodified <code>kwin_wayland</code> from Termux · vendor GLES driver · zero-copy <code>AHardwareBuffer</code>s · no root
</p>

<p align="center">
  <a href="../../actions/workflows/build.yml"><img src="../../actions/workflows/build.yml/badge.svg" alt="Build"></a>
</p>

---

Chameleon lets the stock Termux build of **KWin** (and a whole Plasma session
on top of it) run as if it were on a PC with a real graphics card:

- KWin runs with its normal **DRM backend**, not nested inside another
  compositor, and without a single patch.
- Rendering goes through Android's **vendor GPU driver** (Mali, Adreno, …)
  via EGL/GLES. It doesn't use a software rasteriser.
- Finished frames travel to an Android app as **`AHardwareBuffer`s plus
  fences**. The pixels are never copied through the CPU.
- Touch (as a touchscreen or a trackpad), mouse, hardware keyboards and the
  Android on-screen keyboard are fed back into KWin.

It works by giving KWin the few things it expects from a Linux PC, a KMS
display device, GBM buffers and an EGL driver, all backed by Android APIs.

## How it works

> The diagrams are interactive on GitHub: use the controls in the corner of
> each one to zoom, pan or open it full screen.

```mermaid
flowchart TB
    subgraph T["Termux: Linux userland, same Android user as the app"]
        direction TB
        CMD(["chameleon startplasma-wayland"])
        APPS["Wayland apps<br/>Konsole, Dolphin, Firefox …"]
        subgraph K["kwin_wayland process: unmodified KWin, DRM backend"]
            direction TB
            CORE["KWin compositor<br/>scene · windows · DRM backend"]
            GLVND["glvnd<br/>libEGL.so.1 · libGLESv2.so.2"]
            VEND["libEGL_chameleon.so<br/>glvnd EGL vendor"]
            GBM["libgbm.so<br/>gbm_bo = AHardwareBuffer"]
            KMS["libchameleon.so<br/>fake KMS device"]
            INP["input thread<br/>fake-input client"]
        end
    end

    subgraph A["Android"]
        direction TB
        DRV["vendor GPU driver<br/>Mali · Adreno · …"]
        APP["Chameleon app<br/>SurfaceView + ASurfaceControl"]
        SF["SurfaceFlinger → display"]
        TOUCH["touch · mouse · keyboards"]
    end

    CMD --> CORE
    APPS <-->|Wayland| CORE
    CORE -->|EGL / GLES| GLVND --> VEND --> DRV
    CORE -->|buffers| GBM
    GBM -.->|gralloc| DRV
    CORE -->|atomic commit| KMS
    KMS <==>|"Unix socket<br/>frames + fences ⇄ page flips, input"| APP
    APP --> SF
    TOUCH --> APP
    KMS -->|CHAM_INPUT| INP
    INP -->|fake input| CORE

    classDef kwin fill:#6C4CF1,stroke:#4b32b8,color:#fff
    classDef shim fill:#22B8E6,stroke:#1789ad,color:#06222c
    classDef android fill:#3DDC84,stroke:#2aa862,color:#06220f
    class KMS,GBM,VEND,INP shim
    class CORE,GLVND,APPS,CMD kwin
    class DRV,APP,SF,TOUCH android
```

Purple is KDE/Termux software as shipped, blue is Chameleon's shim, and green
is Android. KWin and the app talk over one Unix socket,
`$PREFIX/tmp/chameleon-0`.

<details>
<summary><b>🦎 The Chameleon app</b>: Android side, shows frames and collects input</summary>

- A full-screen `SurfaceView` whose `ASurfaceControl` receives KWin's buffers
  with `ASurfaceTransaction_setBuffer`, together with KWin's acquire fence.
- It reports the surface size and the panel's fastest refresh rate to KWin
  (`CONFIG`). KWin sees them as the mode of a DSI connector.
- **Page flips:** when SurfaceFlinger latches a frame, the app sends
  `FRAME_DONE`, which the shim turns into KWin's page-flip event. KWin's
  frame pacing therefore follows the real display.
- **Input:** touches are sent as direct touch or trackpad gestures (your
  choice in settings). Mouse and hardware keys are forwarded, and the
  on-screen keyboard comes with an extra row (Esc, Tab, sticky Ctrl/Alt,
  arrows, Home/End, PgUp/PgDn).
- **Same user as Termux:** the APK is signed with Termux's public GitHub test
  key and declares `sharedUserId="com.termux"`. That's why it can listen on
  `$PREFIX/tmp/chameleon-0`, and why it needs Termux from GitHub releases.

Code: [`android/`](android/) · protocol: [`common/chameleon_proto.h`](common/chameleon_proto.h)
</details>

<details>
<summary><b>🖥️ libchameleon.so</b>: a fake KMS display device for KWin</summary>

- Preloaded into `kwin_wayland` only. Opening
  `$PREFIX/tmp/chameleon-card0` (KWin finds it through `KWIN_DRM_DEVICES`)
  returns a pipe that behaves like a DRM card: `stat()` says it's a character
  device, and every DRM `ioctl` on it is answered in user space.
- It offers one connector, one CRTC and one primary plane, with atomic
  modesetting, `TEST_ONLY` commits, `EBUSY` while a flip is pending, and
  page-flip events written to the pipe.
- An atomic commit sends a `PRESENT` to the app with the framebuffer's
  `AHardwareBuffer` and KWin's out-fence. If the app is closed, a simulated
  vblank keeps KWin running.
- It also contains the input thread and a crash reporter that logs the last
  GL/EGL calls and a symbolised backtrace.

Code: [`shim/core/`](shim/core/)
</details>

<details>
<summary><b>🧱 libgbm.so</b>: GBM buffers are AHardwareBuffers</summary>

- A complete `libgbm` API: every `gbm_bo` is an `AHardwareBuffer` allocated
  through gralloc.
- A buffer's dmabuf fd is the gralloc handle's own dmabuf, so KWin's usual
  path (gbm → PRIME fd → `DRM_IOCTL_MODE_ADDFB2` → EGL import) works
  unchanged.

Code: [`shim/gbm/`](shim/gbm/)
</details>

<details>
<summary><b>🎨 libEGL_chameleon.so</b>: a glvnd EGL vendor for Android's GPU driver</summary>

- Termux programs link glvnd's `libEGL.so.1` / `libGLESv2.so.2`, which load
  *vendor* libraries. Chameleon is one: glvnd asks it for displays and for
  every EGL and GL entry point, and it hands out Android's
  `libEGL.so` / `libGLESv2.so`, i.e. the vendor driver.
- **Inside KWin**, it adds what a Mesa GBM stack provides: the GBM platform,
  and `EGL_EXT_image_dma_buf_import`, which imports a Chameleon buffer as its
  `AHardwareBuffer` through `EGL_NATIVE_BUFFER_ANDROID`.
- **Driver workarounds:** it rewrites GLSL `#if GL_ext` to
  `#if defined(GL_ext)` (Mesa tolerates the bare form; strict drivers don't).
  It can also hide extensions and drop context attributes that a driver
  mishandles.
- **Wayland apps on the GPU:** under a Chameleon KWin it also takes
  `EGL_PLATFORM_WAYLAND_KHR`. Each `wl_egl_window` renders into an
  `AImageReader`, a real Android window surface, so the driver's buffer
  handling works as usual. Every frame then reaches KWin as an
  `AHardwareBuffer`: it is registered with KWin's shim, wrapped in a
  `zwp_linux_dmabuf_v1` buffer and committed on the app's surface. Nothing
  is copied, and frame callbacks pace the app.
- **Qt without a rebuild:** Android's driver speaks OpenGL ES only, while
  Termux's Qt is built for desktop OpenGL. Qt already switches to OpenGL ES
  on EGL implementations without desktop GL, as it does for NVIDIA's. So in
  Qt apps on its Wayland platform, the vendor reports an EGL vendor string
  that triggers that path, and stock Qt Quick apps (plasmashell, System
  Settings…) render on the GPU. `CHAMELEON_EGL_QT=mesa` leaves them to Mesa.
- **Scope:** apps that need desktop OpenGL itself, X11 windows, and Wayland
  windows under any other compositor still go to Mesa.

Code: [`shim/vendor/`](shim/vendor/), [`shim/egl/`](shim/egl/), [`shim/gles/`](shim/gles/)
</details>

<details>
<summary><b>⌨️ Input</b>: from Android touch to KWin</summary>

- The app sends `CHAM_INPUT` messages over the same socket: touch points,
  relative/absolute pointer motion, buttons, scroll and X keysyms. Positions
  are fractions of the surface.
- KWin always runs its **fake-input** backend and trusts clients from its own
  process. The shim's input thread therefore connects to KWin's own Wayland
  socket and binds `org_kde_kwin_fake_input`. It scales positions to the
  output's logical size (`xdg_output`), so display scaling just works.
- Keys are keysyms, so KWin looks them up in its keymap (adding Shift when
  needed) or maps them to a spare key. That's why text in any language,
  typed on the Android keyboard, arrives as typed.

Code: [`shim/core/input.c`](shim/core/input.c), [`android/…/TouchInput.java`](android/app/src/main/java/io/github/harryl0l/chameleon/TouchInput.java), [`KeyInput.java`](android/app/src/main/java/io/github/harryl0l/chameleon/KeyInput.java)
</details>

### One frame, end to end

```mermaid
sequenceDiagram
    autonumber
    participant K as KWin
    participant S as libchameleon + EGL vendor
    participant G as GPU driver
    participant A as Chameleon app
    participant F as SurfaceFlinger

    K->>S: render into a gbm buffer (an AHardwareBuffer)
    S->>G: GLES draw calls (EGLImage of the buffer)
    K->>S: atomic commit (FB_ID, OUT_FENCE_PTR)
    S-->>K: out-fence (native fence sync)
    S->>A: PRESENT(buffer, acquire fence)
    A->>F: ASurfaceTransaction_setBuffer(buffer, fence)
    F-->>A: on commit: frame latched
    A->>S: FRAME_DONE(frame, latch time)
    S-->>K: DRM page-flip event → KWin schedules the next frame
    A->>S: RELEASE(buffer, release fence)
```

### Launch flow

```mermaid
flowchart LR
    U(["chameleon startplasma-wayland"]) --> P["PATH = lib/chameleon/bin:$PATH<br/>unset DISPLAY, WAYLAND_DISPLAY<br/>+ D-Bus session"]
    P --> SP["startplasma-wayland"]
    SP --> W["kwin_wayland_wrapper --xwayland"]
    W -->|"starts kwin_wayland by name"| B["lib/chameleon/bin/kwin_wayland<br/>attaches the shim to this process only"]
    B --> R["real kwin_wayland --drm"]
    SP --> PS["plasmashell, apps …<br/>(untouched)"]
```

No `--drm` flag or plasma-workspace change is needed: with neither
`WAYLAND_DISPLAY` nor `DISPLAY` set, KWin picks its DRM backend by itself.

## Getting started

**Requirements:** an arm64 Android 10+ device, and
[Termux from GitHub releases](https://github.com/termux/termux-app/releases)
(the app shares its signing key) with KWin installed.

1. Download the `chameleon-<commit>` artifact from the latest
   [Actions run](../../actions/workflows/build.yml) and unzip it.
2. Install `chameleon.apk` and open **Chameleon**.
3. In Termux, install the package, then start a session:

   ```sh
   apt install /sdcard/Download/chameleon-*/chameleon_*_aarch64.deb

   chameleon kwin_wayland konsole           # KWin with a terminal
   chameleon startplasma-wayland            # or a full Plasma session
   ```

   Without the package, the artifact's `chameleon/` folder works too. Copy it
   into Termux (not `/sdcard`, which is `noexec`), run
   `chmod +x chameleon/chameleon* chameleon/bin/*`, then
   `chameleon/chameleon --install` to put `chameleon` in `$PATH`.

4. Switch to the app. The ⌨️ button opens the keyboard, and ⚙️ switches between
   **direct touch** and **trackpad** (tap to click, two-finger tap for right
   click, two-finger drag to scroll, tap-and-drag to hold).

**Optional:** `chameleon-vendor-install` registers the EGL vendor with glvnd
for all Termux programs (`--remove` undoes it; removing the package does too).

The package also installs `chameleon-probe`, which checks a device's EGL,
AHardwareBuffer and AImageReader support, and `chameleon-demo`, a minimal
zero-copy producer for the app.

## Repository layout

| Path | What |
|---|---|
| [`android/`](android/) | The Chameleon app: presenter (C++, `ASurfaceControl`), input, settings |
| [`common/`](common/) | Socket protocol shared by both sides |
| [`shim/core/`](shim/core/) | `libchameleon.so`: fake KMS device, presenter link, input, crash reporter |
| [`shim/gbm/`](shim/gbm/) | Fake `libgbm.so` on `AHardwareBuffer` |
| [`shim/vendor/`](shim/vendor/), [`shim/egl/`](shim/egl/), [`shim/gles/`](shim/gles/) | `libEGL_chameleon.so`, the glvnd EGL vendor |
| [`shim/bin/`](shim/bin/), [`shim/chameleon`](shim/chameleon) | Launchers |
| [`shim/test/`](shim/test/) | Host tests: fake KMS with real libdrm, input with real libwayland, vendor with real glvnd |
| [`probe/`](probe/) | `ahb_probe`: checks a device's EGL / AHardwareBuffer / fence support |
| [`termux/demo/`](termux/demo/) | `chameleon_demo`: minimal zero-copy producer |
| [`docs/`](docs/) | Design notes ([KWin integration](docs/kwin-integration.md)) |
| [`packaging/`](packaging/) | Termux `.deb` packaging |
| [`branding/`](branding/) | Logo and launcher-icon generator |

## Building

Everything builds on GitHub Actions ([`build.yml`](.github/workflows/build.yml)).
Each push produces one artifact: the APK, the Termux package
(`chameleon_<version>_aarch64.deb`, built by
[`packaging/build-deb.sh`](packaging/build-deb.sh)), the same files as a
plain `chameleon/` folder, and the probe and demo. It also runs the host tests.

Locally:

```sh
gradle -p android assembleDebug                                       # the app
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang \
    shim/build-android.sh out/chameleon                               # the shim
packaging/build-deb.sh out/chameleon out/ahb_probe out/chameleon_demo 0.1.0 out/
shim/test/run-host-test.sh     # needs libdrm-dev libwayland-dev libegl-dev libgles-dev
```

## Roadmap

- [x] Zero-copy presenter app with display-accurate frame pacing
- [x] Unmodified KWin on a fake KMS device, rendering on the vendor GPU driver
- [x] Touch, trackpad, mouse and keyboard input
- [x] EGL/GLES as a glvnd vendor; `chameleon <session>` launcher
- [x] GPU rendering for Wayland apps (Wayland platform in the EGL vendor)
- [ ] Screen resize and rotation (keyboard-aware resizing, mini window)
- [ ] Xwayland acceleration

## Notes

- Not affiliated with KDE, Termux or Google. The "W" disc in the logo is a
  stylised nod to Wayland, not the official Wayland logo.
- `shim/include/` vendors MIT-licensed headers from libdrm, Mesa (`gbm.h`)
  and libglvnd.
