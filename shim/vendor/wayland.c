/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * EGL_PLATFORM_WAYLAND_KHR for apps: Wayland windows rendered by Android's
 * GPU driver and handed to the compositor without copies.
 *
 * Each wl_egl_window gets a window of window.c - an AImageReader - and a
 * real Android window surface on it: the app renders into "framebuffer 0"
 * as usual, with the driver's own buffer handling (buffer age, damage,
 * fences). After every eglSwapBuffers the frame is taken out of the reader
 * and committed on the app's wl_surface; frame callbacks pace swaps for
 * swap interval >= 1.
 *
 * The surfaces the app sees are ours (struct surface); egl.c swaps in the
 * Android surface wherever one is passed to the driver.
 *
 * Only offered in a Chameleon session (libchameleon.so is there and the
 * render node opens), whatever socket the app reaches the compositor
 * through (a proxy passes the dmabufs on unchanged); elsewhere glvnd moves
 * on to Mesa.
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../core/cham_shim.h"
#include "vendor.h"
#include "window.h"

#define AHB_FORMAT_R8G8B8A8 1
#define AHB_FORMAT_R8G8B8X8 2

/* libwayland-egl's struct, WL_EGL_WINDOW_VERSION 3 (wayland-egl-backend.h) */
struct wl_egl_window {
    const intptr_t version;
    int width;
    int height;
    int dx;
    int dy;
    int attached_width;
    int attached_height;
    void *driver_private;
    void (*resize_callback)(struct wl_egl_window *, void *);
    void (*destroy_window_callback)(void *);
    void *surface; /* wl_surface */
};

static int g_wayland_used;

/* A Chameleon session: windows' buffers are shared through its render node. */
int cham_wl_platform_available(void)
{
    return cham_window_available();
}

/* Android has OpenGL ES only, and Termux's Qt is built for desktop OpenGL
 * (QOpenGLContext::openGLModuleType(), a build-time constant: 0 = LibGL).
 * Such a Qt asks EGL for EGL_OPENGL_BIT configs - except when the EGL vendor
 * string contains "NVIDIA", whose EGL has no usable desktop GL either: then
 * it picks OpenGL ES configs and contexts (qeglconvenience.cpp,
 * QEglConfigChooser; q_glFormatFromConfig). So for a desktop-GL Qt on our
 * Wayland display, EGL_VENDOR says so (cham_wl_vendor_string).
 * CHAMELEON_EGL_QT=mesa leaves such apps to Mesa instead. */
static int qt_is_desktop_gl(void)
{
    int (*qt_module_type)(void);
    *(void **)&qt_module_type = dlsym(RTLD_DEFAULT, "_ZN14QOpenGLContext16openGLModuleTypeEv");
    return qt_module_type && qt_module_type() == 0;
}

static int app_needs_desktop_gl(void)
{
    const char *qt = getenv("CHAMELEON_EGL_QT");
    return qt && strcmp(qt, "mesa") == 0 && qt_is_desktop_gl();
}

const char *cham_wl_vendor_string(void)
{
    static int qt = -1;
    if (!g_wayland_used)
        return NULL;
    if (qt < 0) {
        qt = qt_is_desktop_gl();
        if (qt)
            cham_log("wayland: desktop-OpenGL Qt: steering it to OpenGL ES (EGL vendor string with \"NVIDIA\")");
    }
    return qt ? "Chameleon (Android OpenGL ES; no desktop GL, like NVIDIA EGL for Qt)" : NULL;
}

/* The EGLDisplay for a wl_display: Android's, once we know we can serve it. */
EGLDisplay cham_wl_get_display(void *native)
{
    if (!native || !cham_wl_platform_available() || app_needs_desktop_gl() || cham_window_connect(native) != 0)
        return EGL_NO_DISPLAY;
    g_wayland_used = 1;
    return cham_egl_android_display();
}

/* ---- window surfaces ---- */

struct surface {
    struct cham_window *w;
    EGLDisplay dpy;
    struct wl_egl_window *window;
    EGLSurface android;
    int width, height; /* the size asked for */
    int swap_interval;
    pthread_mutex_t lock;
};

/* Our surfaces. EGLSurface handles from Android are never dereferenced:
 * a handle is ours only if it is in this table. */
static pthread_mutex_t g_surfaces_lock = PTHREAD_MUTEX_INITIALIZER;
static struct surface *g_surfaces[256];

static struct surface *as_surface(EGLSurface handle)
{
    if (!g_wayland_used || !handle)
        return NULL;
    struct surface *found = NULL;
    pthread_mutex_lock(&g_surfaces_lock);
    for (size_t i = 0; i < sizeof g_surfaces / sizeof g_surfaces[0]; i++)
        if ((EGLSurface)g_surfaces[i] == handle)
            found = g_surfaces[i];
    pthread_mutex_unlock(&g_surfaces_lock);
    return found;
}

static int track(struct surface *s, int add)
{
    int ok = 0;
    pthread_mutex_lock(&g_surfaces_lock);
    for (size_t i = 0; i < sizeof g_surfaces / sizeof g_surfaces[0]; i++) {
        if (add && !g_surfaces[i]) {
            g_surfaces[i] = s;
            ok = 1;
            break;
        }
        if (!add && g_surfaces[i] == s) {
            g_surfaces[i] = NULL;
            ok = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_surfaces_lock);
    return ok;
}

static EGLSurface (*real_create_window_surface)(EGLDisplay, EGLConfig, void *, const EGLint *);
static EGLBoolean (*real_destroy_surface)(EGLDisplay, EGLSurface);
static EGLBoolean (*real_get_config_attrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
static EGLSurface (*real_get_current_surface)(EGLint);
static EGLContext (*real_get_current_context)(void);
static EGLBoolean (*real_make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*real_swap_interval)(EGLDisplay, EGLint);

static int load_egl(void)
{
    if (real_make_current)
        return 1;
    *(void **)&real_create_window_surface = cham_egl_real("eglCreateWindowSurface");
    *(void **)&real_destroy_surface = cham_egl_real("eglDestroySurface");
    *(void **)&real_get_config_attrib = cham_egl_real("eglGetConfigAttrib");
    *(void **)&real_get_current_surface = cham_egl_real("eglGetCurrentSurface");
    *(void **)&real_get_current_context = cham_egl_real("eglGetCurrentContext");
    *(void **)&real_swap_interval = cham_egl_real("eglSwapInterval");
    *(void **)&real_make_current = cham_egl_real("eglMakeCurrent");
    return real_create_window_surface && real_destroy_surface && real_get_config_attrib && real_get_current_surface &&
           real_get_current_context && real_swap_interval && real_make_current;
}

static void follow_resize(struct surface *s);

/* wl_egl_window_resize(). Apps resize right before drawing the next frame
 * (and glvnd skips eglMakeCurrent when nothing changes), so ask for the new
 * size now. */
static void window_resized(struct wl_egl_window *window, void *data)
{
    (void)window;
    struct surface *s = data;
    pthread_mutex_lock(&s->lock);
    follow_resize(s);
    pthread_mutex_unlock(&s->lock);
}

static void window_destroyed(void *data)
{
    struct surface *s = data;
    s->window = NULL;
}

/* Called by egl.c for eglCreate(Platform)WindowSurface. EGL_NO_SURFACE with
 * *handled = 0 means "not a wl_egl_window, let Android have it". */
EGLSurface cham_wl_create_window_surface(EGLDisplay dpy, EGLConfig config, void *native, int *handled)
{
    *handled = 0;
    struct wl_egl_window *window = native;
    if (!g_wayland_used || !window || !load_egl())
        return EGL_NO_SURFACE;
    /* A wl_egl_window starts with its version; an ANativeWindow with a
     * magic number far larger than any version. */
    if (window->version < 3 || window->version > 16 || !window->surface)
        return EGL_NO_SURFACE;
    *handled = 1;
    if (window->driver_private) {
        cham_setEGLError(EGL_BAD_ALLOC); /* already has a surface */
        return EGL_NO_SURFACE;
    }

    struct surface *s = calloc(1, sizeof *s);
    s->dpy = dpy;
    s->window = window;
    s->swap_interval = 1;
    pthread_mutex_init(&s->lock, NULL);
    EGLint alpha = 8, visual = AHB_FORMAT_R8G8B8A8;
    real_get_config_attrib(dpy, config, EGL_ALPHA_SIZE, &alpha);
    real_get_config_attrib(dpy, config, EGL_NATIVE_VISUAL_ID, &visual);
    s->width = window->width > 0 ? window->width : 1;
    s->height = window->height > 0 ? window->height : 1;
    s->android = EGL_NO_SURFACE;
    s->w = cham_window_create(NULL, window->surface, s->width, s->height, visual == AHB_FORMAT_R8G8B8X8 || alpha == 0);
    if (s->w) {
        s->android = real_create_window_surface(dpy, config, cham_window_native(s->w), NULL);
        if (s->android == EGL_NO_SURFACE)
            cham_log("wayland: eglCreateWindowSurface on the AImageReader failed");
    }
    if (s->android == EGL_NO_SURFACE || !track(s, 1)) {
        if (s->android != EGL_NO_SURFACE)
            real_destroy_surface(dpy, s->android);
        if (s->w)
            cham_window_destroy(s->w);
        free(s);
        cham_setEGLError(EGL_BAD_ALLOC);
        return EGL_NO_SURFACE;
    }
    window->driver_private = s;
    window->resize_callback = window_resized;
    window->destroy_window_callback = window_destroyed;
    return (EGLSurface)s;
}

int cham_wl_is_surface(EGLSurface surface)
{
    return as_surface(surface) != NULL;
}

/* The Android surface to hand the driver for `surface` (itself if not ours). */
EGLSurface cham_wl_android_surface(EGLSurface surface)
{
    struct surface *s = as_surface(surface);
    return s ? s->android : surface;
}

/* Follows wl_egl_window_resize(): the window gives buffers of the new size
 * from its next one on, and the driver's surface takes that size. Called
 * when the app resizes, where it is about to draw (make current) and after
 * a swap. */
static void follow_resize(struct surface *s)
{
    if (!s->window || (s->window->width == s->width && s->window->height == s->height))
        return;
    if (s->window->width <= 0 || s->window->height <= 0)
        return;
    if (cham_window_resize(s->w, s->window->width, s->window->height) != 0)
        return;
    s->width = s->window->width;
    s->height = s->window->height;
}

void cham_wl_before_make_current(EGLSurface draw)
{
    struct surface *s = as_surface(draw);
    if (s) {
        pthread_mutex_lock(&s->lock);
        follow_resize(s);
        pthread_mutex_unlock(&s->lock);
    }
}

void cham_wl_after_make_current(EGLDisplay dpy, EGLSurface draw)
{
    /* The reader is not a display: never let the driver wait for "vsync";
     * pacing comes from Wayland frame callbacks. */
    if (as_surface(draw))
        real_swap_interval(dpy, 0);
}

/* eglSwapInterval for the current surface, if it is ours. */
int cham_wl_swap_interval(EGLint interval)
{
    if (!g_wayland_used || !load_egl())
        return 0;
    EGLSurface cur = real_get_current_surface(EGL_DRAW);
    pthread_mutex_lock(&g_surfaces_lock);
    struct surface *found = NULL;
    for (size_t i = 0; i < sizeof g_surfaces / sizeof g_surfaces[0] && !found; i++) {
        struct surface *s = g_surfaces[i];
        if (!s)
            continue;
        if (s->android == cur) {
            found = s;
            s->swap_interval = interval < 0 ? 0 : interval;
        }
    }
    pthread_mutex_unlock(&g_surfaces_lock);
    return found != NULL;
}

/* eglSwapBuffers(WithDamage) on one of our surfaces. `swap` does the
 * driver's swap on the Android surface. */
EGLBoolean cham_wl_swap(EGLDisplay dpy, EGLSurface surface, const EGLint *rects, EGLint n,
                        EGLBoolean (*swap)(EGLDisplay, EGLSurface, const EGLint *, EGLint))
{
    struct surface *s = as_surface(surface);
    if (!s)
        return swap(dpy, surface, rects, n);
    pthread_mutex_lock(&s->lock);
    EGLBoolean ok = EGL_FALSE;

    /* Swap interval >= 1: at most one frame ahead of the compositor. */
    if (s->swap_interval > 0 && cham_window_wait_frame(s->w) != 0)
        goto out;
    if (!swap(dpy, s->android, rects, n))
        goto out;
    int dx = s->window ? s->window->dx : 0, dy = s->window ? s->window->dy : 0, width = 0, height = 0;
    /* EGL's damage rectangles start at the bottom-left corner. */
    if (cham_window_commit(s->w, s->swap_interval > 0, dx, dy, rects, n, 1, &width, &height) != 0)
        goto out;
    if (s->window) {
        s->window->dx = s->window->dy = 0;
        s->window->attached_width = width;
        s->window->attached_height = height;
    }
    ok = EGL_TRUE;
    follow_resize(s);
out:
    pthread_mutex_unlock(&s->lock);
    return ok;
}

EGLBoolean cham_wl_destroy_surface(EGLDisplay dpy, EGLSurface surface)
{
    (void)dpy;
    struct surface *s = as_surface(surface);
    if (!s)
        return EGL_FALSE;
    track(s, 0);
    pthread_mutex_lock(&s->lock);
    real_destroy_surface(s->dpy, s->android);
    cham_window_destroy(s->w);
    if (s->window) {
        s->window->driver_private = NULL;
        s->window->resize_callback = NULL;
        s->window->destroy_window_callback = NULL;
    }
    pthread_mutex_unlock(&s->lock);
    free(s);
    return EGL_TRUE;
}
