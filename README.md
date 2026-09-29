# Project-Chameleon

Run unmodified KWin (Wayland) on Android through Termux, rendering with the
phone's vendor GPU driver and presenting through an Android `SurfaceControl`.

Planned stack:

```
KWin (--drm, unmodified)
  ├─ fake libdrm/KMS  → atomic commit = present
  ├─ fake libgbm      → gbm_bo = AHardwareBuffer
  └─ EGL shim         → system libEGL → vendor GLES driver (Mali, …)
        ↓ AHardwareBuffer + fence over a Unix socket
Presenter app → ASurfaceTransaction_setBuffer → SurfaceFlinger
```

## Status

1. [`probe/`](probe/) — verified on Mali-G77 (see `probe/RESULTS.md`).
2. **Presenter app** ([`android/`](android/)) + **demo producer**
   ([`termux/demo/`](termux/demo/)) — zero-copy GPU frames from Termux to the
   screen. ← current
3. Fake libgbm / libdrm / EGL shim for KWin.

## How the app talks to Termux

The APK is signed with Termux's **public GitHub-release test key** and declares
`sharedUserId="com.termux"`, so it runs as the Termux user. It listens on
`$PREFIX/tmp/chameleon-0` (a `SOCK_SEQPACKET` Unix socket); Termux processes
connect to it directly. Protocol: [`common/chameleon_proto.h`](common/chameleon_proto.h).

This only installs next to **Termux from GitHub releases** (and plugins from
the same source). F-Droid/Play builds use a different key.

## Try it

1. Download the `chameleon-<sha>` artifact from the latest
   [Actions run](../../actions) and unzip it on the phone.
2. Install `chameleon.apk` and open **Chameleon** (black screen = waiting).
3. In Termux:
   ```sh
   cp /sdcard/Download/chameleon-*/chameleon_demo ~ && chmod +x ~/chameleon_demo
   ~/chameleon_demo     # or build it: termux/demo/build.sh
   ```
   Switch to the app: a colour-cycling screen with a bouncing white box. The
   demo prints fps and submit→on-screen latency.
