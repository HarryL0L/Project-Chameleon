# ahb_probe

Checks, on the phone, that a plain Termux process can use the Android vendor
GPU driver (e.g. `/vendor/lib64/egl/libGLES_mali.so`, loaded through
`/system/lib64/libEGL.so`) the way the Project-Chameleon KWin shim needs:

1. system `libEGL` / `libGLESv2` / `libnativewindow` load from Termux
2. EGL initialises; extensions KWin needs are listed (surfaceless, no-config, …)
3. surfaceless GLES 3 context
4. `AHardwareBuffer` allocation; is its native handle a dmabuf?
5. AHB → EGLImage → texture → FBO, GPU renders into it
6. native fence fd export (becomes DRM `IN_FENCE_FD`)
7. AHB sent over a Unix socket still holds the GPU pixels (zero-copy transport)
8. CPU readback of a GPU-rendered AHB

## Run (in Termux)

```sh
pkg install clang
./build.sh
./ahb_probe              # full check
./ahb_probe --try-vendor # also tries dlopen()ing libGLES_mali.so directly
```

`PASS`/`FAIL` lines are critical checks, `WARN` lines are things the shim can
work around. Paste the whole output back, including the `EGL_EXTENSIONS` line.
