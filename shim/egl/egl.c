/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * EGL side of the Chameleon glvnd vendor (libEGL_chameleon.so): forwards to
 * Android's EGL (/system/lib64/libEGL.so -> the vendor driver, e.g.
 * libGLES_mali.so) and adds what KWin expects from a Mesa GBM stack:
 *
 *  - EGL_EXT_image_dma_buf_import(_modifiers): a dmabuf that belongs to a
 *    Chameleon gbm_bo is imported as its AHardwareBuffer
 *    (EGL_NATIVE_BUFFER_ANDROID)
 *  - eglTerminate is a no-op, because the Android display is process-wide
 *  - on the GBM platform a config's EGL_NATIVE_VISUAL_ID is a GBM (DRM)
 *    format, as EGL_KHR_platform_gbm says, not an Android one
 *  - only configs in the window buffers' formats are window configs
 *
 * Nothing here is exported: glvnd's libEGL.so.1 gets every entry point
 * through cham_egl_get_proc(), and displays through vendor/vendor.c's
 * getPlatformDisplay (glvnd itself implements eglGetDisplay,
 * eglGetPlatformDisplay, eglGetCurrent* and eglQueryAPI).
 */
#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <drm_fourcc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/chameleon_drm.h"
#include "../core/cham_shim.h"
#include "../vendor/vendor.h"

static void *g_lib;
static __thread EGLint t_error; /* error raised by the shim itself */

static int not_for_windows(EGLDisplay dpy, EGLConfig config);

static void *(*real_get_proc)(const char *);

static void load(void)
{
    /* CHAMELEON_ANDROID_EGL: a stand-in library for the host tests */
    const char *path = getenv("CHAMELEON_ANDROID_EGL");
    if (!path)
        path = sizeof(void *) == 8 ? "/system/lib64/libEGL.so" : "/system/lib/libEGL.so";
    g_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) {
        cham_log("cannot load Android's libEGL: %s", dlerror());
        return;
    }
    *(void **)&real_get_proc = dlsym(g_lib, "eglGetProcAddress");
}

static void *real(const char *name)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load);
    void *p = g_lib ? dlsym(g_lib, name) : NULL;
    if (!p && real_get_proc)
        p = real_get_proc(name);
    return p;
}

#define REAL(ret, name, params) \
    static ret(*p_##name) params; \
    if (!p_##name)                \
        *(void **)&p_##name = real(#name);

/* Plain pass-through for everything the shim doesn't need to change. */
#define FWD(ret, name, params, args, fail) \
    EGLAPI ret EGLAPIENTRY name params     \
    {                                      \
        REAL(ret, name, params)            \
        CHAM_NOTE_CALL(#name);             \
        if (!p_##name)                     \
            return fail;                   \
        return p_##name args;              \
    }

FWD(EGLBoolean, eglBindAPI, (EGLenum api), (api), EGL_FALSE)
FWD(EGLint, eglClientWaitSync, (EGLDisplay d, EGLSync s, EGLint f, EGLTime t), (d, s, f, t), EGL_FALSE)
FWD(EGLSurface, eglCreatePbufferFromClientBuffer, (EGLDisplay d, EGLenum t, EGLClientBuffer b, EGLConfig c, const EGLint *a), (d, t, b, c, a), EGL_NO_SURFACE)
FWD(EGLSurface, eglCreatePbufferSurface, (EGLDisplay d, EGLConfig c, const EGLint *a), (d, c, a), EGL_NO_SURFACE)
FWD(EGLSurface, eglCreatePixmapSurface, (EGLDisplay d, EGLConfig c, EGLNativePixmapType p, const EGLint *a), (d, c, p, a), EGL_NO_SURFACE)
FWD(EGLSurface, eglCreatePlatformPixmapSurface, (EGLDisplay d, EGLConfig c, void *p, const EGLAttrib *a), (d, c, p, a), EGL_NO_SURFACE)
FWD(EGLSync, eglCreateSync, (EGLDisplay d, EGLenum t, const EGLAttrib *a), (d, t, a), EGL_NO_SYNC)
FWD(EGLBoolean, eglDestroyContext, (EGLDisplay d, EGLContext c), (d, c), EGL_FALSE)
FWD(EGLBoolean, eglDestroyImage, (EGLDisplay d, EGLImage i), (d, i), EGL_FALSE)
FWD(EGLBoolean, eglDestroySync, (EGLDisplay d, EGLSync s), (d, s), EGL_FALSE)
FWD(EGLBoolean, eglGetConfigs, (EGLDisplay d, EGLConfig *c, EGLint n, EGLint *num), (d, c, n, num), EGL_FALSE)
FWD(EGLBoolean, eglGetSyncAttrib, (EGLDisplay d, EGLSync s, EGLint a, EGLAttrib *v), (d, s, a, v), EGL_FALSE)
FWD(EGLBoolean, eglQueryContext, (EGLDisplay d, EGLContext c, EGLint a, EGLint *v), (d, c, a, v), EGL_FALSE)
FWD(EGLBoolean, eglReleaseThread, (void), (), EGL_FALSE)
FWD(EGLBoolean, eglWaitClient, (void), (), EGL_FALSE)
FWD(EGLBoolean, eglWaitGL, (void), (), EGL_FALSE)
FWD(EGLBoolean, eglWaitNative, (EGLint e), (e), EGL_FALSE)
FWD(EGLBoolean, eglWaitSync, (EGLDisplay d, EGLSync s, EGLint f), (d, s, f), EGL_FALSE)

/* ---- version ----
 * EGL 1.5. Android reports its driver's version (Mali: 1.4), but since
 * Android 10 its libEGL has every EGL 1.5 entry point for any driver, mapped
 * to the KHR functions where the driver is 1.4 (eglWaitSync ->
 * eglWaitSyncKHR, eglCreateSync -> eglCreateSyncKHR, ...), and this layer
 * forwards them. Programs that resolve core 1.5 functions through libepoxy
 * (KWin 6.8's eglWaitSync) only get them from a 1.5 display, as on Mesa.
 * What a 1.4 driver may still lack shows in the extension string as before
 * (Mali: EGL_KHR_gl_texture_3D_image, EGL_KHR_cl_event2). */

EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    REAL(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))
    CHAM_NOTE_CALL("eglInitialize");
    EGLint ma = 0, mi = 0;
    if (!p_eglInitialize || !p_eglInitialize(d, &ma, &mi))
        return EGL_FALSE;
    if (ma == 1 && mi < 5)
        mi = 5;
    if (major)
        *major = ma;
    if (minor)
        *minor = mi;
    return EGL_TRUE;
}

static const char *version_string(const char *android)
{
    static char version[96];
    if (!android || strncmp(android, "1.", 2) != 0 || android[2] >= '5')
        return android;
    snprintf(version, sizeof version, "1.5 Chameleon (Android: %s)", android);
    return version;
}

/* ---- contexts ----
 * KWin asks for robust access (+ lose-context-on-reset) and high priority.
 * With that context, Mali-G77 crashed (SIGBUS, NULL+0x29) in glTexImage2D
 * on KWin's first texture upload, while the same upload works in a plain
 * context. Drop those attributes unless CHAMELEON_EGL_KEEP lists them
 * ("robust", "priority"). KWin runs fine without either. */
static int keep(const char *what)
{
    const char *list = getenv("CHAMELEON_EGL_KEEP");
    return list && strstr(list, what);
}

EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share, const EGLint *attribs)
{
    REAL(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))
    if (!p_eglCreateContext)
        return EGL_NO_CONTEXT;
    EGLint filtered[64];
    int n = 0;
    static int logged;
    for (const EGLint *a = attribs; a && a[0] != EGL_NONE && n < 62; a += 2) {
        int robust = a[0] == EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT || a[0] == EGL_CONTEXT_OPENGL_ROBUST_ACCESS ||
                     a[0] == EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT ||
                     a[0] == EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY || a[0] == 0x334C /* NV purge */;
        int priority = a[0] == EGL_CONTEXT_PRIORITY_LEVEL_IMG;
        if ((robust && !keep("robust")) || (priority && !keep("priority"))) {
            if (logged++ < 8)
                cham_log("dropping context attribute 0x%x (set CHAMELEON_EGL_KEEP=%s to keep it)", a[0],
                         robust ? "robust" : "priority");
            continue;
        }
        filtered[n++] = a[0];
        filtered[n++] = a[1];
    }
    filtered[n] = EGL_NONE;
    return p_eglCreateContext(dpy, config, share, filtered);
}

static EGLBoolean fail(EGLint error)
{
    t_error = error;
    return EGL_FALSE;
}

void cham_setEGLError(EGLint error)
{
    t_error = error;
}

void *cham_egl_real(const char *name)
{
    return real(name);
}

/* ---- surfaces ----
 * Window surfaces on a Wayland display are ours (vendor/wayland.c): the
 * Android surface behind one is what the driver gets wherever a surface is
 * passed. A gbm_surface (gbm/gbm.c) is an Android window already, so its
 * surface is the driver's own. Everything else goes straight through. */

#define SURF(s) cham_wl_android_surface(s)

/* A gbm_surface's window surface, if `native` is one (*handled). As with
 * Mesa, the config's GBM format must be the surface's. */
static EGLSurface gbm_window_surface(EGLDisplay dpy, EGLConfig config, void *native, const EGLint *attribs,
                                     int *handled)
{
    uint32_t format = 0;
    void *window = cham_gbm_window(native, &format);
    *handled = window != NULL;
    if (!window)
        return EGL_NO_SURFACE;
    EGLint visual = 0;
    if (!eglGetConfigAttrib(dpy, config, EGL_NATIVE_VISUAL_ID, &visual) || (uint32_t)visual != format) {
        t_error = EGL_BAD_MATCH;
        return EGL_NO_SURFACE;
    }
    REAL(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *))
    return p_eglCreateWindowSurface ? p_eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)window, attribs)
                                    : EGL_NO_SURFACE;
}

/* EGLAttrib attributes as EGLint ones (all window surface values fit). */
static const EGLint *int_attribs(const EGLAttrib *attribs, EGLint *out, int size)
{
    if (!attribs)
        return NULL;
    int n = 0;
    for (; attribs[n] != EGL_NONE && n + 2 < size; n += 2) {
        out[n] = (EGLint)attribs[n];
        out[n + 1] = (EGLint)attribs[n + 1];
    }
    out[n] = EGL_NONE;
    return out;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                                                     const EGLint *attribs)
{
    CHAM_NOTE_CALL("eglCreateWindowSurface");
    if (not_for_windows(dpy, config)) {
        t_error = EGL_BAD_MATCH; /* no EGL_WINDOW_BIT */
        return EGL_NO_SURFACE;
    }
    int handled;
    EGLSurface s = cham_wl_create_window_surface(dpy, config, (void *)win, &handled);
    if (!handled)
        s = gbm_window_surface(dpy, config, (void *)win, attribs, &handled);
    if (handled)
        return s;
    REAL(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *))
    return p_eglCreateWindowSurface ? p_eglCreateWindowSurface(dpy, config, win, attribs) : EGL_NO_SURFACE;
}

EGLAPI EGLSurface EGLAPIENTRY eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void *win,
                                                             const EGLAttrib *attribs)
{
    CHAM_NOTE_CALL("eglCreatePlatformWindowSurface");
    if (not_for_windows(dpy, config)) {
        t_error = EGL_BAD_MATCH; /* no EGL_WINDOW_BIT */
        return EGL_NO_SURFACE;
    }
    int handled;
    EGLint ints[64];
    EGLSurface s = cham_wl_create_window_surface(dpy, config, win, &handled);
    if (!handled)
        s = gbm_window_surface(dpy, config, win, int_attribs(attribs, ints, 64), &handled);
    if (handled)
        return s;
    REAL(EGLSurface, eglCreatePlatformWindowSurface, (EGLDisplay, EGLConfig, void *, const EGLAttrib *))
    return p_eglCreatePlatformWindowSurface ? p_eglCreatePlatformWindowSurface(dpy, config, win, attribs)
                                            : EGL_NO_SURFACE;
}

static EGLSurface EGLAPIENTRY shim_eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig config, void *win,
                                                                      const EGLint *attribs)
{
    CHAM_NOTE_CALL("eglCreatePlatformWindowSurfaceEXT");
    if (not_for_windows(dpy, config)) {
        t_error = EGL_BAD_MATCH; /* no EGL_WINDOW_BIT */
        return EGL_NO_SURFACE;
    }
    int handled;
    EGLSurface s = cham_wl_create_window_surface(dpy, config, win, &handled);
    if (!handled)
        s = gbm_window_surface(dpy, config, win, attribs, &handled);
    if (handled)
        return s;
    REAL(EGLSurface, eglCreatePlatformWindowSurfaceEXT, (EGLDisplay, EGLConfig, void *, const EGLint *))
    return p_eglCreatePlatformWindowSurfaceEXT ? p_eglCreatePlatformWindowSurfaceEXT(dpy, config, win, attribs)
                                               : EGL_NO_SURFACE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    CHAM_NOTE_CALL("eglDestroySurface");
    if (cham_wl_is_surface(surface))
        return cham_wl_destroy_surface(dpy, surface);
    REAL(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface))
    return p_eglDestroySurface ? p_eglDestroySurface(dpy, surface) : EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    CHAM_NOTE_CALL("eglMakeCurrent");
    REAL(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
    if (!p_eglMakeCurrent)
        return EGL_FALSE;
    cham_wl_before_make_current(draw); /* a resized wl_egl_window gets its new buffers here */
    if (read != draw)
        cham_wl_before_make_current(read);
    EGLBoolean ok = p_eglMakeCurrent(dpy, SURF(draw), SURF(read), ctx);
    if (ok)
        cham_wl_after_make_current(dpy, draw);
    return ok;
}

EGLAPI EGLBoolean EGLAPIENTRY eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value)
{
    CHAM_NOTE_CALL("eglQuerySurface");
    REAL(EGLBoolean, eglQuerySurface, (EGLDisplay, EGLSurface, EGLint, EGLint *))
    return p_eglQuerySurface ? p_eglQuerySurface(dpy, SURF(surface), attribute, value) : EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value)
{
    CHAM_NOTE_CALL("eglSurfaceAttrib");
    REAL(EGLBoolean, eglSurfaceAttrib, (EGLDisplay, EGLSurface, EGLint, EGLint))
    return p_eglSurfaceAttrib ? p_eglSurfaceAttrib(dpy, SURF(surface), attribute, value) : EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
    REAL(EGLBoolean, eglBindTexImage, (EGLDisplay, EGLSurface, EGLint))
    return p_eglBindTexImage ? p_eglBindTexImage(dpy, SURF(surface), buffer) : EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer)
{
    REAL(EGLBoolean, eglReleaseTexImage, (EGLDisplay, EGLSurface, EGLint))
    return p_eglReleaseTexImage ? p_eglReleaseTexImage(dpy, SURF(surface), buffer) : EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglCopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target)
{
    REAL(EGLBoolean, eglCopyBuffers, (EGLDisplay, EGLSurface, EGLNativePixmapType))
    return p_eglCopyBuffers ? p_eglCopyBuffers(dpy, SURF(surface), target) : EGL_FALSE;
}

static EGLBoolean EGLAPIENTRY shim_eglSetDamageRegionKHR(EGLDisplay dpy, EGLSurface surface, EGLint *rects, EGLint n)
{
    REAL(EGLBoolean, eglSetDamageRegionKHR, (EGLDisplay, EGLSurface, EGLint *, EGLint))
    return p_eglSetDamageRegionKHR ? p_eglSetDamageRegionKHR(dpy, SURF(surface), rects, n) : EGL_FALSE;
}

/* The driver's swap, with or without damage, in one signature for wayland.c. */
static EGLBoolean driver_swap(EGLDisplay dpy, EGLSurface surface, const EGLint *rects, EGLint n)
{
    if (rects && n > 0) {
        REAL(EGLBoolean, eglSwapBuffersWithDamageKHR, (EGLDisplay, EGLSurface, const EGLint *, EGLint))
        if (!p_eglSwapBuffersWithDamageKHR)
            *(void **)&p_eglSwapBuffersWithDamageKHR = real("eglSwapBuffersWithDamageEXT");
        if (p_eglSwapBuffersWithDamageKHR)
            return p_eglSwapBuffersWithDamageKHR(dpy, surface, rects, n);
    }
    REAL(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface))
    return p_eglSwapBuffers ? p_eglSwapBuffers(dpy, surface) : EGL_FALSE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    CHAM_NOTE_CALL("eglSwapBuffers");
    return cham_wl_swap(dpy, surface, NULL, 0, driver_swap);
}

static EGLBoolean EGLAPIENTRY shim_eglSwapBuffersWithDamage(EGLDisplay dpy, EGLSurface surface, const EGLint *rects,
                                                             EGLint n)
{
    CHAM_NOTE_CALL("eglSwapBuffersWithDamage");
    return cham_wl_swap(dpy, surface, rects, n, driver_swap);
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
    CHAM_NOTE_CALL("eglSwapInterval");
    if (cham_wl_swap_interval(interval))
        return EGL_TRUE; /* ours: paced by frame callbacks */
    REAL(EGLBoolean, eglSwapInterval, (EGLDisplay, EGLint))
    return p_eglSwapInterval ? p_eglSwapInterval(dpy, interval) : EGL_FALSE;
}

EGLAPI EGLint EGLAPIENTRY eglGetError(void)
{
    REAL(EGLint, eglGetError, (void))
    EGLint real_error = p_eglGetError ? p_eglGetError() : EGL_NOT_INITIALIZED;
    if (t_error) {
        EGLint e = t_error;
        t_error = 0;
        return e;
    }
    return real_error;
}

/* ---- configs ---- */

/* The DRM format of an Android config's buffers (AHARDWAREBUFFER_FORMAT_*), 0
 * if none matches. */
static EGLint drm_format(EGLint android, EGLint alpha)
{
    switch (android) {
    case 1: /* R8G8B8A8_UNORM */
        return alpha ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_XBGR8888;
    case 2: /* R8G8B8X8_UNORM */
        return DRM_FORMAT_XBGR8888;
    case 3: /* R8G8B8_UNORM */
        return DRM_FORMAT_BGR888;
    case 4: /* R5G6B5_UNORM */
        return DRM_FORMAT_RGB565;
    case 5: /* B8G8R8A8_UNORM */
        return alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
    case 0x16: /* R16G16B16A16_FLOAT */
        return alpha ? DRM_FORMAT_ABGR16161616F : DRM_FORMAT_XBGR16161616F;
    case 0x2b: /* R10G10B10A2_UNORM */
        return alpha ? DRM_FORMAT_ABGR2101010 : DRM_FORMAT_XBGR2101010;
    }
    return 0;
}

/* ---- configs ----
 * Window surfaces here render into Android buffers in RGBA_8888 or
 * RGBX_8888 (a Wayland window's AImageReader, a gbm_surface), which a config
 * of another format (10-bit, RGB565) can't render into: as with Mesa, which
 * offers windows only in formats the compositor takes, those aren't window
 * configs. Apps asking for "at least 1 bit" per colour then get an 8-bit
 * one instead of the 10-bit config EGL's sort order puts first. */

#define ANDROID_RGBA_8888 1
#define ANDROID_RGBX_8888 2

/* A config whose format no window buffer here has (a bad config is left to
 * the driver to report). */
static int not_for_windows(EGLDisplay dpy, EGLConfig config)
{
    REAL(EGLBoolean, eglGetConfigAttrib, (EGLDisplay, EGLConfig, EGLint, EGLint *))
    EGLint visual = 0;
    return p_eglGetConfigAttrib && p_eglGetConfigAttrib(dpy, config, EGL_NATIVE_VISUAL_ID, &visual) &&
           visual != ANDROID_RGBA_8888 && visual != ANDROID_RGBX_8888;
}

EGLAPI EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs, EGLint size,
                                              EGLint *num)
{
    REAL(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
    CHAM_NOTE_CALL("eglChooseConfig");
    if (!p_eglChooseConfig)
        return EGL_FALSE;
    EGLint type = EGL_WINDOW_BIT, by_id = 0;
    for (const EGLint *a = attribs; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_SURFACE_TYPE)
            type = a[1];
        else if (a[0] == EGL_CONFIG_ID && a[1] != EGL_DONT_CARE)
            by_id = 1; /* the other attributes don't count then */
    }
    if (!num || by_id || type == EGL_DONT_CARE || !(type & EGL_WINDOW_BIT))
        return p_eglChooseConfig(dpy, attribs, configs, size, num);
    EGLint all = 0;
    if (!p_eglChooseConfig(dpy, attribs, NULL, 0, &all))
        return EGL_FALSE;
    EGLConfig *list = all > 0 ? malloc(sizeof *list * (size_t)all) : NULL;
    if (all > 0 && (!list || !p_eglChooseConfig(dpy, attribs, list, all, &all))) {
        free(list);
        t_error = EGL_BAD_ALLOC;
        return EGL_FALSE;
    }
    EGLint matching = 0, returned = 0;
    for (EGLint i = 0; i < all; i++) {
        if (not_for_windows(dpy, list[i]))
            continue;
        matching++;
        if (configs && returned < size)
            configs[returned++] = list[i];
    }
    free(list);
    *num = configs ? returned : matching;
    return EGL_TRUE;
}

/* A compositor on the fake card (the only process with the GBM platform)
 * picks configs by their GBM format, which its window surfaces render in. */
EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value)
{
    REAL(EGLBoolean, eglGetConfigAttrib, (EGLDisplay, EGLConfig, EGLint, EGLint *))
    CHAM_NOTE_CALL("eglGetConfigAttrib");
    if (!p_eglGetConfigAttrib)
        return EGL_FALSE;
    if (!p_eglGetConfigAttrib(dpy, config, attribute, value))
        return EGL_FALSE;
    if (attribute == EGL_SURFACE_TYPE && not_for_windows(dpy, config))
        *value &= ~EGL_WINDOW_BIT;
    if (attribute == EGL_NATIVE_VISUAL_ID && cham_core_present()) {
        EGLint alpha = 0;
        p_eglGetConfigAttrib(dpy, config, EGL_ALPHA_SIZE, &alpha);
        *value = drm_format(*value, alpha);
    }
    return EGL_TRUE;
}

/* ---- displays ---- */

static EGLDisplay android_display(void)
{
    REAL(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType))
    return p_eglGetDisplay ? p_eglGetDisplay(EGL_DEFAULT_DISPLAY) : EGL_NO_DISPLAY;
}

/* KWin may create and destroy several EglDisplays; Android has one per
 * process, and terminating it would pull it from under the others. */
EGLAPI EGLBoolean EGLAPIENTRY eglTerminate(EGLDisplay dpy)
{
    (void)dpy;
    return EGL_TRUE;
}

/* GBM and the card's EGLDevice only where Chameleon's gbm (libchameleon.so,
 * in every program of a Chameleon session) is loaded. */
static const char k_client_extensions_kms[] =
    "EGL_EXT_client_extensions EGL_EXT_platform_base EGL_KHR_platform_gbm EGL_MESA_platform_gbm "
    "EGL_MESA_platform_surfaceless EGL_KHR_platform_android EGL_KHR_client_get_all_proc_addresses "
    "EGL_EXT_device_base EGL_EXT_device_enumeration EGL_EXT_device_query EGL_EXT_platform_device";
static const char k_client_extensions[] =
    "EGL_EXT_client_extensions EGL_EXT_platform_base EGL_MESA_platform_surfaceless EGL_KHR_platform_android "
    "EGL_KHR_client_get_all_proc_addresses";

EGLAPI const char *EGLAPIENTRY eglQueryString(EGLDisplay dpy, EGLint name)
{
    REAL(const char *, eglQueryString, (EGLDisplay, EGLint))
    if (dpy == EGL_NO_DISPLAY && name == EGL_EXTENSIONS)
        return cham_core_present() ? k_client_extensions_kms : k_client_extensions;
    if (name == EGL_VENDOR && cham_wl_vendor_string())
        return cham_wl_vendor_string();
    const char *s = p_eglQueryString ? p_eglQueryString(dpy, name) : NULL;
    if (name == EGL_VERSION)
        return version_string(s);
    if (name != EGL_EXTENSIONS || !s)
        return s;

    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static char *cached;
    pthread_mutex_lock(&lock);
    if (!cached) {
        /* dmabufs can only be imported when they are Chameleon gbm buffers */
        const char *extra = cham_core_present()
                                ? " EGL_EXT_image_dma_buf_import EGL_EXT_image_dma_buf_import_modifiers"
                                : "";
        cached = malloc(strlen(s) + strlen(extra) + 1);
        strcpy(cached, s);
        if (!strstr(s, "EGL_EXT_image_dma_buf_import_modifiers"))
            strcat(cached, extra);
    }
    pthread_mutex_unlock(&lock);
    return cached;
}

/* ---- the card as an EGLDevice (EGL_EXT_device_base) ----
 * Where the card exists (a Chameleon session), it is the GPU's device, as
 * Mesa makes a DRM device with a render node one: enumerated, with its
 * primary and render nodes (EGL_EXT_device_drm, _render_node), and the
 * device of every display here. A display opened on it directly
 * (EGL_EXT_platform_device, as wlroots does) is the same as a GBM one. */

static const int g_device;
#define CHAM_DEVICE ((EGLDeviceEXT)&g_device)
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT /* EGL_EXT_device_drm_render_node, newer than the NDK's headers */
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif

void *cham_egl_device(void)
{
    return CHAM_DEVICE;
}

static EGLBoolean EGLAPIENTRY shim_eglQueryDevicesEXT(EGLint max, EGLDeviceEXT *devices, EGLint *num)
{
    if (!num || (devices && max <= 0))
        return fail(EGL_BAD_PARAMETER);
    *num = cham_core_present() ? 1 : 0;
    if (devices && *num)
        devices[0] = CHAM_DEVICE;
    return EGL_TRUE;
}

static const char *EGLAPIENTRY shim_eglQueryDeviceStringEXT(EGLDeviceEXT device, EGLint name)
{
    if (device != CHAM_DEVICE) {
        fail(EGL_BAD_DEVICE_EXT);
        return NULL;
    }
    if (name == EGL_EXTENSIONS)
        return "EGL_EXT_device_drm EGL_EXT_device_drm_render_node";
    if (name == EGL_DRM_DEVICE_FILE_EXT)
        return CHAM_DRM_PATH; /* the name libdrm finds for it in sysfs */
    if (name == EGL_DRM_RENDER_NODE_FILE_EXT)
        return CHAM_RENDER_PATH;
    fail(EGL_BAD_PARAMETER);
    return NULL;
}

static EGLBoolean EGLAPIENTRY shim_eglQueryDeviceAttribEXT(EGLDeviceEXT device, EGLint attribute, EGLAttrib *value)
{
    (void)attribute, (void)value;
    return fail(device != CHAM_DEVICE ? EGL_BAD_DEVICE_EXT : EGL_BAD_ATTRIBUTE);
}

static EGLBoolean EGLAPIENTRY shim_eglQueryDisplayAttribEXT(EGLDisplay dpy, EGLint attribute, EGLAttrib *value)
{
    (void)dpy;
    if (attribute != EGL_DEVICE_EXT || !value)
        return fail(EGL_BAD_ATTRIBUTE);
    if (!cham_core_present())
        return fail(EGL_BAD_ATTRIBUTE); /* no device: EGL_EXT_device_query isn't offered here */
    *value = (EGLAttrib)CHAM_DEVICE;
    return EGL_TRUE;
}

/* ---- dmabuf import ---- */

static EGLBoolean EGLAPIENTRY shim_eglQueryDmaBufFormatsEXT(EGLDisplay dpy, EGLint max, EGLint *formats, EGLint *num)
{
    (void)dpy;
    int n;
    const uint32_t *ours = cham_formats(&n);
    if (max > 0 && formats)
        for (int i = 0; i < n && i < max; i++)
            formats[i] = (EGLint)ours[i];
    *num = max > 0 && formats ? (n < max ? n : max) : n;
    return EGL_TRUE;
}

/* No explicit modifiers: gralloc picks the layout (implicit). */
static EGLBoolean EGLAPIENTRY shim_eglQueryDmaBufModifiersEXT(EGLDisplay dpy, EGLint format, EGLint max,
                                                                EGLuint64KHR *modifiers, EGLBoolean *external_only,
                                                                EGLint *num)
{
    (void)dpy, (void)format, (void)max, (void)modifiers, (void)external_only;
    *num = 0;
    return EGL_TRUE;
}

static EGLImageKHR import_dmabuf(EGLDisplay dpy, const EGLint *attribs)
{
    int fd = -1;
    for (const EGLint *a = attribs; a && a[0] != EGL_NONE; a += 2)
        if (a[0] == EGL_DMA_BUF_PLANE0_FD_EXT)
            fd = a[1];
    /* This process's own gbm buffers, or any other buffer of the session
     * chameleon-kernel knows (another process's gbm buffer, an app's);
     * any other dmabuf has no AHardwareBuffer we could hand the driver. */
    struct cham_bo *bo = cham_bo_from_fd(fd);
    AHardwareBuffer *session = bo ? NULL : cham_buffer_ahb_acquire(fd);
    AHardwareBuffer *ahb = bo ? cham_bo_ahb(bo) : session;
    if (!ahb) {
        t_error = EGL_BAD_MATCH;
        return EGL_NO_IMAGE_KHR;
    }
    static EGLClientBuffer (*get_client_buffer)(const struct AHardwareBuffer *);
    static EGLImageKHR (*create_image)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    if (!get_client_buffer)
        *(void **)&get_client_buffer = real("eglGetNativeClientBufferANDROID");
    if (!create_image)
        *(void **)&create_image = real("eglCreateImageKHR");
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    if (!get_client_buffer || !create_image) {
        t_error = EGL_BAD_ALLOC;
    } else {
        const EGLint image_attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        image = create_image(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, get_client_buffer(ahb), image_attribs);
    }
    cham_buffer_ahb_release(session); /* the image holds its own */
    return image;
}

static EGLImageKHR EGLAPIENTRY shim_eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                                                       EGLClientBuffer buffer, const EGLint *attribs)
{
    if (target == EGL_LINUX_DMA_BUF_EXT)
        return import_dmabuf(dpy, attribs);
    REAL(EGLImageKHR, eglCreateImageKHR, (EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *))
    return p_eglCreateImageKHR ? p_eglCreateImageKHR(dpy, ctx, target, buffer, attribs) : EGL_NO_IMAGE_KHR;
}

EGLAPI EGLImage EGLAPIENTRY eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                                           const EGLAttrib *attribs)
{
    EGLint converted[64];
    int n = 0;
    for (const EGLAttrib *a = attribs; a && a[0] != EGL_NONE && n < 62; a += 2) {
        converted[n++] = (EGLint)a[0];
        converted[n++] = (EGLint)a[1];
    }
    converted[n] = EGL_NONE;
    return shim_eglCreateImageKHR(dpy, ctx, target, buffer, converted);
}

/* ---- proc addresses ----
 *
 * glvnd's libEGL asks the vendor for each entry point by name (core EGL for
 * its static dispatch, extensions for its dynamic dispatch table). Ours come
 * first; everything else is Android's own function. */

/* Extension entry points whose calls are recorded for the crash report. */
#define TRACED_VOID(id, str, params, args)             \
    static void (*p_tr_##id) params;                   \
    static void tr_##id params                         \
    {                                                  \
        CHAM_NOTE_CALL(str);                           \
        p_tr_##id args;                                \
    }
#define TRACED(ret, id, str, params, args)             \
    static ret (*p_tr_##id) params;                    \
    static ret tr_##id params                          \
    {                                                  \
        CHAM_NOTE_CALL(str);                           \
        return p_tr_##id args;                         \
    }
TRACED_VOID(QueryCounterEXT, "glQueryCounterEXT", (unsigned id, unsigned target), (id, target))
TRACED_VOID(GetQueryObjecti64vEXT, "glGetQueryObjecti64vEXT", (unsigned id, unsigned pname, int64_t *v), (id, pname, v))
TRACED_VOID(GetQueryObjectui64vEXT, "glGetQueryObjectui64vEXT", (unsigned id, unsigned pname, uint64_t *v),
            (id, pname, v))
TRACED_VOID(BeginQueryEXT, "glBeginQueryEXT", (unsigned target, unsigned id), (target, id))
TRACED_VOID(EndQueryEXT, "glEndQueryEXT", (unsigned target), (target))
TRACED_VOID(EGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES", (unsigned target, void *image),
            (target, image))
TRACED_VOID(EGLImageTargetRenderbufferStorageOES, "glEGLImageTargetRenderbufferStorageOES",
            (unsigned target, void *image), (target, image))
TRACED(EGLSyncKHR, CreateSyncKHR, "eglCreateSyncKHR", (EGLDisplay d, EGLenum t, const EGLint *a), (d, t, a))
TRACED(EGLBoolean, DestroySyncKHR, "eglDestroySyncKHR", (EGLDisplay d, EGLSyncKHR s), (d, s))
TRACED(EGLint, ClientWaitSyncKHR, "eglClientWaitSyncKHR", (EGLDisplay d, EGLSyncKHR s, EGLint f, EGLTimeKHR t),
       (d, s, f, t))
TRACED(EGLint, WaitSyncKHR, "eglWaitSyncKHR", (EGLDisplay d, EGLSyncKHR s, EGLint f), (d, s, f))
TRACED(EGLint, DupNativeFenceFDANDROID, "eglDupNativeFenceFDANDROID", (EGLDisplay d, EGLSyncKHR s), (d, s))

#define TR(id, str) {str, (void **)&p_tr_##id, (void *)tr_##id}
static const struct {
    const char *name;
    void **real;
    void *wrapper;
} k_traced[] = {
    TR(QueryCounterEXT, "glQueryCounterEXT"),
    TR(GetQueryObjecti64vEXT, "glGetQueryObjecti64vEXT"),
    TR(GetQueryObjectui64vEXT, "glGetQueryObjectui64vEXT"),
    TR(BeginQueryEXT, "glBeginQueryEXT"),
    TR(EndQueryEXT, "glEndQueryEXT"),
    TR(EGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES"),
    TR(EGLImageTargetRenderbufferStorageOES, "glEGLImageTargetRenderbufferStorageOES"),
    TR(CreateSyncKHR, "eglCreateSyncKHR"),
    TR(DestroySyncKHR, "eglDestroySyncKHR"),
    TR(ClientWaitSyncKHR, "eglClientWaitSyncKHR"),
    TR(WaitSyncKHR, "eglWaitSyncKHR"),
    TR(DupNativeFenceFDANDROID, "eglDupNativeFenceFDANDROID"),
};

#define OWN(name) {#name, (void *)name}
static const struct {
    const char *name;
    void *fn;
} k_own[] = {
    /* changed behaviour */
    OWN(eglCreateContext),
    OWN(eglGetError),
    OWN(eglTerminate),
    OWN(eglQueryString),
    OWN(eglCreateImage),
    {"eglCreateImageKHR", (void *)shim_eglCreateImageKHR},
    {"eglQueryDmaBufFormatsEXT", (void *)shim_eglQueryDmaBufFormatsEXT},
    {"eglQueryDmaBufModifiersEXT", (void *)shim_eglQueryDmaBufModifiersEXT},
    {"eglQueryDevicesEXT", (void *)shim_eglQueryDevicesEXT},
    {"eglQueryDisplayAttribEXT", (void *)shim_eglQueryDisplayAttribEXT},
    {"eglQueryDisplayAttribKHR", (void *)shim_eglQueryDisplayAttribEXT},
    {"eglQueryDeviceStringEXT", (void *)shim_eglQueryDeviceStringEXT},
    {"eglQueryDeviceAttribEXT", (void *)shim_eglQueryDeviceAttribEXT},
    {"eglCreatePlatformWindowSurfaceEXT", (void *)shim_eglCreatePlatformWindowSurfaceEXT},
    {"eglSetDamageRegionKHR", (void *)shim_eglSetDamageRegionKHR},
    {"eglSwapBuffersWithDamageKHR", (void *)shim_eglSwapBuffersWithDamage},
    {"eglSwapBuffersWithDamageEXT", (void *)shim_eglSwapBuffersWithDamage},
    /* pass-through, recorded for the crash report */
    OWN(eglBindAPI), OWN(eglBindTexImage), OWN(eglChooseConfig), OWN(eglClientWaitSync), OWN(eglCopyBuffers),
    OWN(eglCreatePbufferFromClientBuffer), OWN(eglCreatePbufferSurface), OWN(eglCreatePixmapSurface),
    OWN(eglCreatePlatformPixmapSurface), OWN(eglCreatePlatformWindowSurface), OWN(eglCreateSync),
    OWN(eglCreateWindowSurface), OWN(eglDestroyContext), OWN(eglDestroyImage), OWN(eglDestroySurface),
    OWN(eglDestroySync), OWN(eglGetConfigAttrib), OWN(eglGetConfigs), OWN(eglGetSyncAttrib),
    OWN(eglInitialize), OWN(eglMakeCurrent), OWN(eglQueryContext), OWN(eglQuerySurface), OWN(eglReleaseTexImage),
    OWN(eglReleaseThread), OWN(eglSurfaceAttrib), OWN(eglSwapBuffers), OWN(eglSwapInterval), OWN(eglWaitClient),
    OWN(eglWaitGL), OWN(eglWaitNative), OWN(eglWaitSync),
};

/* glGetGraphicsResetStatus under any of its names. Apps with robustness
 * (Qt asks for it; the context attributes are dropped, see eglCreateContext)
 * call it on every make-current and treat anything but GL_NO_ERROR as a
 * lost context, so the ES entry point is used and a reset is logged. */
static unsigned reset_status(void)
{
    static unsigned (*fn)(void);
    static int resolved;
    if (!resolved) {
        *(void **)&fn = cham_gl_get_proc("glGetGraphicsResetStatus");
        if (!fn)
            *(void **)&fn = real("glGetGraphicsResetStatusEXT");
        if (!fn)
            *(void **)&fn = real("glGetGraphicsResetStatusKHR");
        resolved = 1;
    }
    unsigned status = fn ? fn() : 0;
    static int logged;
    if (status && logged++ < 4)
        cham_log("the GPU driver reports a context reset (0x%x)", status);
    return status;
}

/* Desktop-GL extension names (glFooARB) have no OpenGL ES entry points.
 * Android's loader may still hand out a forwarder for any gl* name, so they
 * are answered here: the ES core function of the same name, or nothing. */
static void *desktop_gl_proc(const char *name)
{
    size_t n = strlen(name);
    if (n < 5 || name[0] != 'g' || name[1] != 'l' || strcmp(name + n - 3, "ARB") != 0)
        return NULL;
    char core[128];
    if (n - 3 >= sizeof core)
        return (void *)-1;
    memcpy(core, name, n - 3);
    core[n - 3] = 0;
    void *fn = cham_gl_get_proc(core);
    return fn ? fn : (void *)-1;
}

/* Our function for `name`, or Android's; NULL if neither has it. */
void *cham_egl_get_proc(const char *name)
{
    for (size_t i = 0; i < sizeof k_own / sizeof k_own[0]; i++)
        if (strcmp(name, k_own[i].name) == 0)
            return k_own[i].fn;
    if (strncmp(name, "glGetGraphicsResetStatus", 24) == 0)
        return (void *)reset_status;
    void *desktop = desktop_gl_proc(name);
    if (desktop)
        return desktop == (void *)-1 ? NULL : desktop;
    void *fn = real(name);
    for (size_t i = 0; fn && i < sizeof k_traced / sizeof k_traced[0]; i++) {
        if (strcmp(name, k_traced[i].name) == 0) {
            *k_traced[i].real = fn;
            return k_traced[i].wrapper;
        }
    }
    return fn;
}

int cham_egl_available(void)
{
    return real("eglGetDisplay") != NULL;
}

/* Android's EGLDisplay (there is one per process), for the vendor glue. */
EGLDisplay cham_egl_android_display(void)
{
    cham_crash_arm();
    return android_display();
}
