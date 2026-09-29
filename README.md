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

Step 1 is [`probe/`](probe/): verify the GPU/AHB half works on your device.
