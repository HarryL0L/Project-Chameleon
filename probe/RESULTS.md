# Probe results

## Mali-G77 MC9 (MediaTek), driver r32p1, EGL 1.4 "Android META-EGL" — VIABLE

- Direct `dlopen` of `/vendor/lib64/egl/libGLES_mali.so`: blocked by the
  linker namespace → the shim must go through `/system/lib64/libEGL.so`.
- Native: `EGL_KHR_no_config_context`, `EGL_KHR_surfaceless_context`,
  `EGL_ANDROID_native_fence_sync` + `EGL_KHR_wait_sync` (KWin enables native
  fence sync with this pair), `EGL_ANDROID_get_native_client_buffer`,
  `GL_OES_EGL_image`.
- Missing (shim fakes): `EGL_EXT_image_dma_buf_import(_modifiers)`,
  `EGL_EXT_platform_base`/GBM platform, `EGL_EXT_device_query`.
- AHB native handle (MediaTek gralloc), 256x256 RGBA8888, 3 fds / 76 ints:
  - fd[0] `anon_inode:gralloc_extra` — metadata
  - fd[1] `/dmabuf:G:pid [...]`, 266240 bytes (262144 pixels + 4096) — **pixel memory**
  - fd[2] ashmem — gralloc shared metadata
  → fake `gbm_bo_get_fd` returns `dup(fd[1])`; the shim's AHB lookup table is
  keyed by fd[1]'s inode. AHB stride is in pixels (×4 for bytes).
- GPU render into AHB, fence fd export, zero-copy AHB over Unix socket, CPU
  readback: all pass.

## Presenter + demo (realme RMX3031, 120 Hz panel, Android 13)

Termux process renders on Mali into 3 AHBs → SOCK_SEQPACKET → presenter app
→ `ASurfaceTransaction_setBuffer` (acquire fence) on a SurfaceView child,
one frame in flight, FRAME_DONE on `setOnCommit`:

| | fps shown | dropped | submit→latch | submit→FRAME_DONE |
|---|---|---|---|---|
| FRAME_DONE on OnComplete | ~60 | 0 | ~6 ms | ~15 ms (2 vsyncs) |
| FRAME_DONE on OnCommit   | **119.9–120.0** | **0** | ~6.6 ms | **~7.2 ms** |

1080×1800 floating window. Zero copies end to end.
