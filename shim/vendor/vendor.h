/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private to libEGL_chameleon.so. */
#ifndef CHAM_VENDOR_H
#define CHAM_VENDOR_H

#include <EGL/egl.h>
#include <stdint.h>

/* egl/egl.c */
void *cham_egl_get_proc(const char *name);
EGLDisplay cham_egl_android_display(void);
int cham_egl_available(void); /* Android's libEGL.so loads (no driver yet) */

void *cham_egl_real(const char *name);      /* Android's entry point */
void cham_setEGLError(EGLint error);         /* reported by eglGetError */

/* wayland.c: EGL_PLATFORM_WAYLAND_KHR */
int cham_wl_platform_available(void);
const char *cham_wl_vendor_string(void); /* EGL_VENDOR override, or NULL */
EGLDisplay cham_wl_get_display(void *wl_display);
EGLSurface cham_wl_create_window_surface(EGLDisplay dpy, EGLConfig config, void *native, int *handled);
int cham_wl_is_surface(EGLSurface surface);
EGLSurface cham_wl_android_surface(EGLSurface surface);
void cham_wl_before_make_current(EGLSurface draw);
void cham_wl_after_make_current(EGLDisplay dpy, EGLSurface draw);
int cham_wl_swap_interval(EGLint interval);
EGLBoolean cham_wl_swap(EGLDisplay dpy, EGLSurface surface, const EGLint *rects, EGLint n,
                        EGLBoolean (*swap)(EGLDisplay, EGLSurface, const EGLint *, EGLint));
EGLBoolean cham_wl_destroy_surface(EGLDisplay dpy, EGLSurface surface);

/* gles/gles.c */
void *cham_gl_get_proc(const char *name);

/* bridge.c: is libchameleon.so (the card node, gbm's buffers) in this
 * process? It is in every program of a Chameleon session. */
int cham_core_present(void);
/* bridge.c: this process renders with Android's driver: hand libchameleon's
 * crash reporter the breadcrumbs (it installs itself then). */
void cham_crash_arm(void);
/* bridge.c: the Android window behind one of Chameleon's gbm surfaces (and
 * its DRM format), NULL for any other native window. */
void *cham_gbm_window(void *native, uint32_t *format);

#endif
