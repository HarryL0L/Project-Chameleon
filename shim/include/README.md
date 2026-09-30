# Third-party headers

Build-time headers copied from other projects, so the shim builds without
them installed. They are not shipped in the APK or the .deb. Each keeps its
own licence (below), not Project Chameleon's GPL.

| File | From | Licence |
|---|---|---|
| `xf86drm.h` | [libdrm](https://gitlab.freedesktop.org/mesa/drm) 2.4.134 | MIT |
| `libdrm/drm.h`, `libdrm/drm_mode.h`, `libdrm/drm_fourcc.h` | libdrm 2.4.134 (`include/drm/`, copies of the Linux kernel's UAPI headers) | MIT |
| `gbm.h` | [Mesa](https://gitlab.freedesktop.org/mesa/mesa) 25 (`src/gbm/main/gbm.h`) | MIT |
| `glvnd/libeglabi.h`, `glvnd/GLdispatchABI.h` | [libglvnd](https://gitlab.freedesktop.org/glvnd/libglvnd) 1.7.0 (`include/glvnd/`) | MIT-style (NVIDIA) |

The copyright and licence notices are at the top of each file.
`drm_fourcc.h` carries only `SPDX-License-Identifier: MIT` upstream; that
licence's text is below.

## Changes

All files are unmodified copies, except:

- `glvnd/GLdispatchABI.h`: includes `<GLES2/gl2.h>` instead of `<GL/gl.h>`
  (only `GLboolean` is needed, and the Android NDK has no desktop GL
  headers). Marked with a comment in the file.

## MIT licence

```
Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```
