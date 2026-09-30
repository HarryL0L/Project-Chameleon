/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stand-in for Android's libEGL.so and libGLESv2.so that renders for real,
 * on Mesa (llvmpipe, surfaceless), so the actual kwin_wayland can run on the
 * shim on a desktop (shim/test/run-kwin-test.sh). Unlike fake_android_gl.c it
 * compiles KWin's shaders and draws.
 *
 * Mesa is loaded with its own glvnd into a separate link namespace
 * (dlmopen), so its dispatch state can't mix with the glvnd that KWin and
 * the Chameleon vendor use in the main namespace.
 *
 * An AHardwareBuffer (the host build's memfd stand-in, see core/ahb.c) is
 * imported as EGL_NATIVE_BUFFER_ANDROID by giving it a Mesa texture of its
 * size: one EGLImage per buffer, shared by all its imports. Pixels don't
 * reach the memfd; nothing reads them there.
 *
 * The GLES entry points are generated (gen_mesa_gl.py) and forward to the
 * Mesa namespace's libGLESv2.
 */
#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define API __attribute__((visibility("default")))

#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif

/* Layout of the host build's AHardwareBuffer (core/ahb.c). */
struct host_ahb {
    struct {
        uint32_t width, height, layers, format;
        uint64_t usage;
        uint32_t stride, rfu0;
        uint64_t rfu1;
    } desc;
    struct {
        int version, numFds, numInts;
        int data[];
    } *nh;
};

static void *g_egl, *g_gles;
static void *(*m_get_proc)(const char *);

static void load(void)
{
    const char *json = getenv("MESA_EGL_VENDOR_JSON");
    const char *saved = getenv("__EGL_VENDOR_LIBRARY_FILENAMES");
    char *keep = saved ? strdup(saved) : NULL;
    /* Mesa's glvnd reads this when it first loads its vendors */
    setenv("__EGL_VENDOR_LIBRARY_FILENAMES", json ? json : "/usr/share/glvnd/egl_vendor.d/50_mesa.json", 1);
    g_egl = dlmopen(LM_ID_NEWLM, "libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (g_egl) {
        Lmid_t lm;
        dlinfo(g_egl, RTLD_DI_LMID, &lm);
        g_gles = dlmopen(lm, "libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
        *(void **)&m_get_proc = dlsym(g_egl, "eglGetProcAddress");
        /* loads Mesa while the variable still names it */
        void (*init_vendors)(void) = (void (*)(void))dlsym(g_egl, "eglGetDisplay");
        if (init_vendors) {
            EGLDisplay (*gpd)(EGLenum, void *, const EGLAttrib *) = dlsym(g_egl, "eglGetPlatformDisplay");
            gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        }
    }
    if (keep) {
        setenv("__EGL_VENDOR_LIBRARY_FILENAMES", keep, 1);
        free(keep);
    } else {
        unsetenv("__EGL_VENDOR_LIBRARY_FILENAMES");
    }
    if (!g_egl || !g_gles)
        fprintf(stderr, "mesa_android_gl: cannot load Mesa: %s\n", dlerror());
}

static void *mesa(const char *name)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load);
    void *p = NULL;
    if (!strncmp(name, "gl", 2) && g_gles)
        p = dlsym(g_gles, name);
    if (!p && g_egl)
        p = dlsym(g_egl, name);
    if (!p && m_get_proc)
        p = m_get_proc(name);
    return p;
}

#define M(ret, name, params) \
    static ret(*m_##name) params; \
    if (!m_##name)                \
        *(void **)&m_##name = mesa(#name);

/* ---- displays: Android has one, the default one ---- */

API EGLDisplay eglGetDisplay(EGLNativeDisplayType d)
{
    M(EGLDisplay, eglGetPlatformDisplay, (EGLenum, void *, const EGLAttrib *))
    if (d != EGL_DEFAULT_DISPLAY || !m_eglGetPlatformDisplay)
        return EGL_NO_DISPLAY;
    return m_eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
}

API const char *eglQueryString(EGLDisplay d, EGLint name)
{
    M(const char *, eglQueryString, (EGLDisplay, EGLint))
    const char *s = m_eglQueryString(d, name);
    if (name != EGL_EXTENSIONS || d == EGL_NO_DISPLAY || !s)
        return s;
    static char *ext;
    if (!ext && asprintf(&ext, "%s EGL_ANDROID_image_native_buffer EGL_ANDROID_get_native_client_buffer", s) < 0)
        ext = NULL;
    return ext ? ext : s;
}

/* ---- AHardwareBuffer import ---- */

API EGLClientBuffer eglGetNativeClientBufferANDROID(const struct AHardwareBuffer *ahb)
{
    return (EGLClientBuffer)ahb;
}

struct image {
    dev_t dev;
    ino_t ino;
    EGLImageKHR image;
    int refs;
    struct image *next;
};
static struct image *g_images;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static EGLContext g_alloc_ctx;

/* A Mesa texture the size of the buffer, made in a context of our own. */
static EGLImageKHR new_image(EGLDisplay d, const struct host_ahb *b)
{
    M(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))
    M(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
    M(EGLContext, eglGetCurrentContext, (void))
    M(EGLDisplay, eglGetCurrentDisplay, (void))
    M(EGLSurface, eglGetCurrentSurface, (EGLint))
    M(EGLBoolean, eglBindAPI, (EGLenum))
    M(EGLImageKHR, eglCreateImageKHR, (EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *))
    M(void, glGenTextures, (GLsizei, GLuint *))
    M(void, glBindTexture, (GLenum, GLuint))
    M(void, glTexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))
    M(void, glDeleteTextures, (GLsizei, const GLuint *))
    M(void, glFinish, (void))
    if (!g_alloc_ctx) {
        m_eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        g_alloc_ctx = m_eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
    }
    EGLDisplay cur_d = m_eglGetCurrentDisplay();
    EGLContext cur_c = m_eglGetCurrentContext();
    EGLSurface cur_draw = m_eglGetCurrentSurface(EGL_DRAW), cur_read = m_eglGetCurrentSurface(EGL_READ);
    m_eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, g_alloc_ctx);
    GLuint tex;
    m_glGenTextures(1, &tex);
    m_glBindTexture(GL_TEXTURE_2D, tex);
    m_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)b->desc.width, (GLsizei)b->desc.height);
    EGLImageKHR image = m_eglCreateImageKHR(d, g_alloc_ctx, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)tex,
                                            NULL);
    m_glBindTexture(GL_TEXTURE_2D, 0);
    m_glDeleteTextures(1, &tex); /* the image keeps the storage */
    m_glFinish();
    if (cur_c != EGL_NO_CONTEXT)
        m_eglMakeCurrent(cur_d, cur_draw, cur_read, cur_c);
    else
        m_eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return image;
}

API EGLImageKHR eglCreateImageKHR(EGLDisplay d, EGLContext c, EGLenum target, EGLClientBuffer buffer,
                                  const EGLint *attribs)
{
    M(EGLImageKHR, eglCreateImageKHR, (EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *))
    if (target != EGL_NATIVE_BUFFER_ANDROID)
        return m_eglCreateImageKHR(d, c, target, buffer, attribs);
    const struct host_ahb *b = buffer;
    struct stat st;
    if (!b || fstat(b->nh->data[0], &st) != 0)
        return EGL_NO_IMAGE_KHR;
    pthread_mutex_lock(&g_lock);
    struct image *i = g_images;
    while (i && !(i->dev == st.st_dev && i->ino == st.st_ino))
        i = i->next;
    if (!i) {
        EGLImageKHR image = new_image(d, b);
        if (image != EGL_NO_IMAGE_KHR) {
            i = calloc(1, sizeof *i);
            *i = (struct image){st.st_dev, st.st_ino, image, 0, g_images};
            g_images = i;
        }
    }
    EGLImageKHR out = i ? i->image : EGL_NO_IMAGE_KHR;
    if (i)
        i->refs++;
    pthread_mutex_unlock(&g_lock);
    return out;
}

API EGLBoolean eglDestroyImageKHR(EGLDisplay d, EGLImageKHR image)
{
    M(EGLBoolean, eglDestroyImageKHR, (EGLDisplay, EGLImageKHR))
    pthread_mutex_lock(&g_lock);
    for (struct image **p = &g_images; *p; p = &(*p)->next) {
        if ((*p)->image == image) {
            struct image *i = *p;
            if (--i->refs == 0) {
                *p = i->next;
                free(i);
                break;
            }
            pthread_mutex_unlock(&g_lock);
            return EGL_TRUE;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return m_eglDestroyImageKHR(d, image);
}

API EGLBoolean eglDestroyImage(EGLDisplay d, EGLImage image)
{
    return eglDestroyImageKHR(d, image);
}

/* ---- everything else: Mesa's own ---- */

API void (*eglGetProcAddress(const char *name))(void)
{
    static const struct {
        const char *name;
        void *fn;
    } own[] = {
        {"eglGetDisplay", (void *)eglGetDisplay},
        {"eglQueryString", (void *)eglQueryString},
        {"eglGetNativeClientBufferANDROID", (void *)eglGetNativeClientBufferANDROID},
        {"eglCreateImageKHR", (void *)eglCreateImageKHR},
        {"eglDestroyImageKHR", (void *)eglDestroyImageKHR},
        {"eglDestroyImage", (void *)eglDestroyImage},
    };
    for (size_t i = 0; i < sizeof own / sizeof own[0]; i++)
        if (!strcmp(name, own[i].name))
            return (void (*)(void))own[i].fn;
    return (void (*)(void))mesa(name);
}

void *mesa_android_gl_sym(const char *name)
{
    return mesa(name);
}

#include "mesa_android_gl_forward.inc"
