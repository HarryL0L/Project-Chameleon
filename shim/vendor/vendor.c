/*
 * libEGL_chameleon.so: a libglvnd EGL vendor library for Android's GPU.
 *
 * glvnd's libEGL.so.1 / libGLESv2.so.2 (what Termux programs link against)
 * load vendor libraries listed in JSON files, from
 * $PREFIX/share/glvnd/egl_vendor.d/ or $__EGL_VENDOR_LIBRARY_FILENAMES, and
 * call their __egl_Main(). From then on glvnd asks the vendor for displays
 * and for every EGL and GL entry point by name; this one hands out the
 * Chameleon EGL/GLES layer (egl/egl.c, gles/gles.c) on top of Android's
 * libEGL.so / libGLESv2.so, i.e. the vendor driver (Mali, Adreno, ...).
 *
 * Which displays it takes (anything else is left to the next vendor, e.g.
 * Mesa):
 *  - EGL_PLATFORM_GBM_KHR, and eglGetDisplay(EGL_DEFAULT_DISPLAY): only in a
 *    process running on Chameleon's fake KMS device (kwin_wayland started by
 *    chameleon-kwin, where libchameleon.so provides the gbm buffers);
 *    CHAMELEON_EGL_DEFAULT=1 also takes the default display elsewhere.
 *  - EGL_PLATFORM_SURFACELESS_MESA and EGL_PLATFORM_ANDROID_KHR: always
 *    (off-screen rendering on the GPU).
 *  - EGL_PLATFORM_WAYLAND_KHR: when the app's compositor is a KWin running
 *    with the Chameleon shim (wayland.c): windows render on the GPU and
 *    reach KWin as AHardwareBuffers, without copies.
 * X11 windows are not handled.
 *
 * Only __egl_Main is exported (see vendor.map).
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glvnd/libeglabi.h>

#include "../core/cham_shim.h"
#include "vendor.h"

static const __EGLapiExports *g_exports;

/* ---- dispatch stubs for EGL display extensions ----
 *
 * glvnd dispatches core EGL itself, but an extension function taking an
 * EGLDisplay must come with a vendor-provided stub that finds the display's
 * vendor and calls its implementation through glvnd's dispatch table (at
 * the index glvnd assigns with setDispatchIndex). */

static __eglMustCastToProperFunctionPointerType fetch(EGLDisplay dpy, int index)
{
    g_exports->threadInit();
    __EGLvendorInfo *vendor = g_exports->getVendorFromDisplay(dpy);
    if (!vendor || index < 0) {
        g_exports->setEGLError(EGL_BAD_DISPLAY);
        return NULL;
    }
    __eglMustCastToProperFunctionPointerType fn = g_exports->fetchDispatchEntry(vendor, index);
    if (!fn) {
        g_exports->setEGLError(EGL_BAD_DISPLAY);
        return NULL;
    }
    g_exports->setLastVendor(vendor);
    return fn;
}

#define STUB(ret, name, params, args, failure)                        \
    static int idx_##name = -1;                                      \
    static ret EGLAPIENTRY stub_##name params                        \
    {                                                                \
        ret(EGLAPIENTRY * fn) params;                                \
        *(void **)&fn = (void *)fetch(dpy, idx_##name);              \
        return fn ? fn args : (failure);                             \
    }

STUB(EGLImageKHR, eglCreateImageKHR,
     (EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attribs),
     (dpy, ctx, target, buffer, attribs), EGL_NO_IMAGE_KHR)
STUB(EGLBoolean, eglDestroyImageKHR, (EGLDisplay dpy, EGLImageKHR image), (dpy, image), EGL_FALSE)
STUB(EGLSyncKHR, eglCreateSyncKHR, (EGLDisplay dpy, EGLenum type, const EGLint *attribs), (dpy, type, attribs),
     EGL_NO_SYNC_KHR)
STUB(EGLBoolean, eglDestroySyncKHR, (EGLDisplay dpy, EGLSyncKHR sync), (dpy, sync), EGL_FALSE)
STUB(EGLint, eglClientWaitSyncKHR, (EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout),
     (dpy, sync, flags, timeout), EGL_FALSE)
STUB(EGLint, eglWaitSyncKHR, (EGLDisplay dpy, EGLSyncKHR sync, EGLint flags), (dpy, sync, flags), EGL_FALSE)
STUB(EGLBoolean, eglGetSyncAttribKHR, (EGLDisplay dpy, EGLSyncKHR sync, EGLint attribute, EGLint *value),
     (dpy, sync, attribute, value), EGL_FALSE)
STUB(EGLint, eglDupNativeFenceFDANDROID, (EGLDisplay dpy, EGLSyncKHR sync), (dpy, sync),
     EGL_NO_NATIVE_FENCE_FD_ANDROID)
STUB(EGLBoolean, eglQueryDmaBufFormatsEXT, (EGLDisplay dpy, EGLint max, EGLint *formats, EGLint *num),
     (dpy, max, formats, num), EGL_FALSE)
STUB(EGLBoolean, eglQueryDmaBufModifiersEXT,
     (EGLDisplay dpy, EGLint format, EGLint max, EGLuint64KHR *modifiers, EGLBoolean *external, EGLint *num),
     (dpy, format, max, modifiers, external, num), EGL_FALSE)
STUB(EGLBoolean, eglSwapBuffersWithDamageKHR, (EGLDisplay dpy, EGLSurface surface, const EGLint *rects, EGLint n),
     (dpy, surface, rects, n), EGL_FALSE)
STUB(EGLBoolean, eglSwapBuffersWithDamageEXT, (EGLDisplay dpy, EGLSurface surface, const EGLint *rects, EGLint n),
     (dpy, surface, rects, n), EGL_FALSE)
STUB(EGLBoolean, eglSetDamageRegionKHR, (EGLDisplay dpy, EGLSurface surface, EGLint *rects, EGLint n),
     (dpy, surface, rects, n), EGL_FALSE)
STUB(EGLBoolean, eglPresentationTimeANDROID, (EGLDisplay dpy, EGLSurface surface, EGLnsecsANDROID time),
     (dpy, surface, time), EGL_FALSE)

#define ENTRY(name) {#name, (void *)stub_##name, &idx_##name}
static const struct {
    const char *name;
    void *stub;
    int *index;
} k_stubs[] = {
    ENTRY(eglCreateImageKHR),        ENTRY(eglDestroyImageKHR),        ENTRY(eglCreateSyncKHR),
    ENTRY(eglDestroySyncKHR),        ENTRY(eglClientWaitSyncKHR),      ENTRY(eglWaitSyncKHR),
    ENTRY(eglGetSyncAttribKHR),      ENTRY(eglDupNativeFenceFDANDROID), ENTRY(eglQueryDmaBufFormatsEXT),
    ENTRY(eglQueryDmaBufModifiersEXT), ENTRY(eglSwapBuffersWithDamageKHR), ENTRY(eglSwapBuffersWithDamageEXT),
    ENTRY(eglSetDamageRegionKHR),    ENTRY(eglPresentationTimeANDROID),
};

static void *get_dispatch_address(const char *name)
{
    for (size_t i = 0; i < sizeof k_stubs / sizeof k_stubs[0]; i++)
        if (strcmp(name, k_stubs[i].name) == 0)
            return cham_egl_get_proc(name) ? k_stubs[i].stub : NULL;
    /* Client extensions take no display, so they need no dispatch; handing
     * out the function itself is what Mesa does too. */
    if (strcmp(name, "eglGetNativeClientBufferANDROID") == 0)
        return cham_egl_get_proc(name);
    return NULL;
}

static void set_dispatch_index(const char *name, int index)
{
    for (size_t i = 0; i < sizeof k_stubs / sizeof k_stubs[0]; i++)
        if (strcmp(name, k_stubs[i].name) == 0)
            *k_stubs[i].index = index;
}

/* ---- the rest of the vendor interface ---- */

static int claim_default_display(void)
{
    const char *env = getenv("CHAMELEON_EGL_DEFAULT");
    return cham_core_present() || (env && strcmp(env, "1") == 0);
}

static EGLDisplay get_platform_display(EGLenum platform, void *native, const EGLAttrib *attribs)
{
    (void)attribs;
    switch (platform) {
    case EGL_NONE: /* eglGetDisplay(): EGL_DEFAULT_DISPLAY or an unknown handle */
        return native == EGL_DEFAULT_DISPLAY && claim_default_display() ? cham_egl_android_display()
                                                                        : EGL_NO_DISPLAY;
    case EGL_PLATFORM_GBM_KHR:
        /* Only Chameleon's own gbm devices have AHardwareBuffers behind them. */
        return cham_core_present() ? cham_egl_android_display() : EGL_NO_DISPLAY;
    case EGL_PLATFORM_SURFACELESS_MESA:
    case EGL_PLATFORM_ANDROID_KHR:
        return cham_egl_android_display();
    case EGL_PLATFORM_WAYLAND_KHR:
        /* only under a KWin running with the Chameleon shim */
        return cham_wl_get_display(native);
    }
    return EGL_NO_DISPLAY;
}

static EGLBoolean get_supports_api(EGLenum api)
{
    return api == EGL_OPENGL_ES_API; /* Android has no desktop OpenGL */
}

static const char *get_vendor_string(int name)
{
    if (name != __EGL_VENDOR_STRING_PLATFORM_EXTENSIONS)
        return NULL;
    static char list[256];
    snprintf(list, sizeof list, "%s%sEGL_MESA_platform_surfaceless EGL_KHR_platform_android",
             cham_core_present() ? "EGL_KHR_platform_gbm EGL_MESA_platform_gbm " : "",
             cham_wl_platform_available() ? "EGL_KHR_platform_wayland EGL_EXT_platform_wayland " : "");
    return list;
}

static void *get_proc_address(const char *name)
{
    if (name[0] == 'g' && name[1] == 'l' && strncmp(name, "glGetGraphicsResetStatus", 24) != 0) {
        void *fn = cham_gl_get_proc(name);
        if (fn)
            return fn;
    }
    return cham_egl_get_proc(name); /* EGL, and GL extensions via Android's eglGetProcAddress */
}

/* A launcher that pins glvnd to this library for one process can save the
 * previous __EGL_VENDOR_LIBRARY_FILENAMES in CHAMELEON_ORIG_...; it is put
 * back once glvnd has loaded its vendors. (bin/kwin_wayland doesn't: KWin
 * hands its startup environment to the programs it starts, so it lists this
 * vendor first and the installed ones after it instead.) */
static void restore_environment(void)
{
    const char *saved = getenv("CHAMELEON_ORIG___EGL_VENDOR_LIBRARY_FILENAMES");
    if (!saved)
        return;
    if (*saved)
        setenv("__EGL_VENDOR_LIBRARY_FILENAMES", saved, 1);
    else
        unsetenv("__EGL_VENDOR_LIBRARY_FILENAMES");
    unsetenv("CHAMELEON_ORIG___EGL_VENDOR_LIBRARY_FILENAMES");
}

__attribute__((visibility("default"))) EGLBoolean __egl_Main(uint32_t version, const __EGLapiExports *exports,
                                                              __EGLvendorInfo *vendor, __EGLapiImports *imports)
{
    (void)vendor;
    if (EGL_VENDOR_ABI_GET_MAJOR_VERSION(version) != EGL_VENDOR_ABI_MAJOR_VERSION) {
        cham_log("glvnd EGL vendor ABI %u.%u is not supported (built for %u.%u)",
                 EGL_VENDOR_ABI_GET_MAJOR_VERSION(version), EGL_VENDOR_ABI_GET_MINOR_VERSION(version),
                 EGL_VENDOR_ABI_MAJOR_VERSION, EGL_VENDOR_ABI_MINOR_VERSION);
        return EGL_FALSE;
    }
    restore_environment();
    /* Only check that Android's EGL loads: eglGetDisplay() would load the
     * whole GPU driver into every program glvnd starts, even those whose
     * displays end up with another vendor. */
    if (!cham_egl_available()) {
        cham_log("Android's EGL is not usable here; leaving EGL to the other vendors");
        return EGL_FALSE;
    }
    g_exports = exports;
    memset(imports, 0, sizeof *imports);
    imports->getPlatformDisplay = get_platform_display;
    imports->getSupportsAPI = get_supports_api;
    imports->getVendorString = get_vendor_string;
    imports->getProcAddress = get_proc_address;
    imports->getDispatchAddress = get_dispatch_address;
    imports->setDispatchIndex = set_dispatch_index;
    return EGL_TRUE;
}
