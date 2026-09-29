/* Private to libEGL_chameleon.so. */
#ifndef CHAM_VENDOR_H
#define CHAM_VENDOR_H

#include <EGL/egl.h>

/* egl/egl.c */
void *cham_egl_get_proc(const char *name);
EGLDisplay cham_egl_android_display(void);
int cham_egl_available(void); /* Android's libEGL.so loads (no driver yet) */

/* gles/gles.c */
void *cham_gl_get_proc(const char *name);

/* bridge.c: is libchameleon.so (KWin's fake KMS device) in this process? */
int cham_core_present(void);

#endif
