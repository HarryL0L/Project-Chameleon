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
