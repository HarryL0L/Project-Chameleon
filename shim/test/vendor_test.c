/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host test of libEGL_chameleon.so as a real glvnd vendor: this program uses
 * the system's glvnd libEGL/libGLESv2 exactly as a Termux app would, with
 * __EGL_VENDOR_LIBRARY_FILENAMES pointing at the Chameleon vendor, and the
 * vendor's "Android" driver replaced by fake_android_gl.c.
 */
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"

struct fake_state {
    int robust_attrib_seen, client_version;
    char shader[512];
    int tex_uploads;
    int create_image_target;
    int query_counter_calls;
};

/* Chameleon's libgbm (VENDOR_TEST_GBM), by hand: no gbm.h here. */
static struct {
    void *(*create_device)(int);
    void *(*surface_create)(void *, uint32_t, uint32_t, uint32_t, uint32_t);
    void *(*lock)(void *);
    void (*release)(void *, void *);
    int (*has_free)(void *);
    void (*surface_destroy)(void *);
    uint32_t (*bo_width)(void *), (*bo_format)(void *);
    union { uint32_t u32; uint64_t u64; } (*bo_handle)(void *);
} gbm;

/* EGL on the GBM platform the way Weston's GL renderer uses it: a window
 * surface on a gbm_surface, swaps, and the swapped buffer locked for KMS. */
static void gbm_surface_checks(EGLDisplay dpy, EGLConfig config)
{
    printf("gbm surface\n");
    void *lib = dlopen(getenv("VENDOR_TEST_GBM"), RTLD_NOW | RTLD_LOCAL); /* as Weston's drm-backend.so brings it */
#define G(field, name) (*(void **)&gbm.field = lib ? dlsym(lib, name) : NULL)
    G(create_device, "gbm_create_device");
    G(surface_create, "gbm_surface_create");
    G(lock, "gbm_surface_lock_front_buffer");
    G(release, "gbm_surface_release_buffer");
    G(has_free, "gbm_surface_has_free_buffers");
    G(surface_destroy, "gbm_surface_destroy");
    G(bo_width, "gbm_bo_get_width");
    G(bo_format, "gbm_bo_get_format");
    G(bo_handle, "gbm_bo_get_handle");
#undef G
    CHECK(gbm.create_device && gbm.bo_handle, "Chameleon's libgbm");
    if (!gbm.create_device || !gbm.bo_handle)
        return;
    const char *path = getenv("CHAMELEON_DRM_PATH");
    int card = open(path ? path : "/dev/dri/card0", O_RDWR | O_CLOEXEC); /* gbm's GEM handles live in the card file */
    void *dev = gbm.create_device(card);
    void *surface = gbm.surface_create(dev, 64, 32, 0x34324241 /* AB24 */, 5 /* SCANOUT | RENDERING */);
    void *other = gbm.surface_create(dev, 64, 32, 0x34324258 /* XB24 */, 5);
    CHECK(surface && other, "gbm_surface_create");
    EGLSurface es = eglCreatePlatformWindowSurface(dpy, config, surface, NULL);
    CHECK(es != EGL_NO_SURFACE, "window surface on a gbm surface of the config's format");
    CHECK(eglCreatePlatformWindowSurface(dpy, config, other, NULL) == EGL_NO_SURFACE && eglGetError() == EGL_BAD_MATCH,
          "...EGL_BAD_MATCH for one of another format");
    CHECK(gbm.lock(surface) == NULL, "nothing to lock before a swap");

    eglSwapBuffers(dpy, es);
    void *first = gbm.lock(surface);
    CHECK(first && gbm.bo_width(first) == 64 && gbm.bo_format(first) == 0x34324241 && gbm.bo_handle(first).u32,
          "the swapped buffer locked as a gbm_bo with a GEM handle");
    eglSwapBuffers(dpy, es);
    void *second = gbm.lock(surface);
    CHECK(second && second != first, "the next swap, another buffer");
    CHECK(gbm.has_free(surface), "free buffers left while two are locked");
    gbm.release(surface, first);
    gbm.release(surface, second);
    int same = 0;
    for (int i = 0; i < 8; i++) {
        eglSwapBuffers(dpy, es);
        void *bo = gbm.lock(surface);
        same |= bo == first;
        if (bo)
            gbm.release(surface, bo);
    }
    CHECK(same, "a buffer coming round again is the same gbm_bo");
    eglDestroySurface(dpy, es);
    gbm.surface_destroy(surface);
    gbm.surface_destroy(other);
}

int main(void)
{
    const char *fake_path = getenv("CHAMELEON_ANDROID_EGL");
    if (!fake_path || !getenv("__EGL_VENDOR_LIBRARY_FILENAMES")) {
        fprintf(stderr, "run via run-host-test.sh\n");
        return 2;
    }

    /* VENDOR_TEST_KWIN=1: libchameleon.so is preloaded, as in kwin_wayland */
    int kwin = getenv("VENDOR_TEST_KWIN") != NULL;
    printf("glvnd vendor (%s)\n", kwin ? "inside KWin, with libchameleon.so" : "any other program");
    const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    CHECK(client && strstr(client, "EGL_MESA_platform_surfaceless"), "client extensions: surfaceless platform");
    if (kwin) {
        CHECK(client && strstr(client, "EGL_KHR_platform_gbm"), "GBM platform for KWin");
        CHECK(eglGetDisplay(EGL_DEFAULT_DISPLAY) != EGL_NO_DISPLAY, "default display for KWin");
        EGLDisplay gbm_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, (void *)1, NULL);
        CHECK(gbm_dpy != EGL_NO_DISPLAY, "GBM display for KWin");
        /* The card as an EGLDevice, as Mesa describes a KMS-only card. */
        PFNEGLQUERYDISPLAYATTRIBEXTPROC display_attrib =
            (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress("eglQueryDisplayAttribEXT");
        PFNEGLQUERYDEVICESTRINGEXTPROC device_string =
            (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
        PFNEGLQUERYDEVICESEXTPROC query_devices = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
        EGLAttrib device = 0;
        EGLint gmajor, gminor;
        CHECK(client && strstr(client, "EGL_EXT_device_query"), "EGL_EXT_device_query for KWin");
        CHECK(display_attrib && eglInitialize(gbm_dpy, &gmajor, &gminor) &&
                  display_attrib(gbm_dpy, EGL_DEVICE_EXT, &device) && device,
              "the GBM display's EGLDevice");
        const char *dev_exts = device ? device_string((EGLDeviceEXT)device, EGL_EXTENSIONS) : NULL;
        const char *file = device ? device_string((EGLDeviceEXT)device, EGL_DRM_DEVICE_FILE_EXT) : NULL;
        CHECK(dev_exts && strstr(dev_exts, "EGL_EXT_device_drm") && !strstr(dev_exts, "render_node") && file &&
                  strcmp(file, "/dev/dri/card0") == 0,
              "it is the card, /dev/dri/card0, without a render node (%s)", file ? file : "none");
        EGLDeviceEXT devices[8];
        EGLint count = 0, ours = 0;
        if (query_devices && query_devices(8, devices, &count))
            for (EGLint i = 0; i < count; i++)
                ours += devices[i] == (EGLDeviceEXT)device;
        CHECK(ours == 0, "not enumerated, as Mesa leaves out a card without a render node");
    } else {
        CHECK(client && !strstr(client, "platform_gbm"), "no GBM platform outside KWin");
        CHECK(eglGetDisplay(EGL_DEFAULT_DISPLAY) == EGL_NO_DISPLAY,
              "default display left to other vendors outside KWin");
    }

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    CHECK(dpy != EGL_NO_DISPLAY, "surfaceless display");
    EGLint major = 0, minor = 0;
    CHECK(eglInitialize(dpy, &major, &minor) && major == 1, "eglInitialize (EGL %d.%d)", major, minor);
    const char *vendor = eglQueryString(dpy, EGL_VENDOR);
    CHECK(vendor && strcmp(vendor, "fake Android") == 0, "display belongs to the Android driver");
    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (kwin) {
        CHECK(exts && strstr(exts, "EGL_EXT_image_dma_buf_import"), "dmabuf import for KWin");
        /* Everything gbm allocates is importable, or KWin takes those formats
         * for external-only and can't render into them. */
        PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats =
            (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
        EGLint formats[16], n = 0;
        int listed = 0;
        if (query_formats && query_formats(dpy, 16, formats, &n))
            for (EGLint i = 0; i < n; i++)
                listed += formats[i] == 0x34324258 || formats[i] == 0x34324241 || formats[i] == 0x34325258 ||
                          formats[i] == 0x34325241; /* XB24 AB24 XR24 AR24 */
        CHECK(listed == 4, "dmabuf import of XBGR/ABGR/XRGB/ARGB8888, gbm's formats (%d of 4)", listed);
    } else
        CHECK(exts && !strstr(exts, "dma_buf_import"), "no dmabuf import outside KWin");
    /* EGL_KHR_platform_gbm: a config's native visual is its GBM format (the
     * stand-in's config is Android's RGBA_8888 with alpha). */
    EGLConfig config;
    EGLint nconfigs = 0, visual = 0;
    eglChooseConfig(dpy, (const EGLint[]){EGL_NONE}, &config, 1, &nconfigs);
    eglGetConfigAttrib(dpy, config, EGL_NATIVE_VISUAL_ID, &visual);
    if (kwin)
        CHECK(visual == 0x34324241, "native visual of an RGBA_8888 config: ABGR8888 for GBM (0x%x)", visual);
    else
        CHECK(visual == 1, "native visual left Android's outside KWin (0x%x)", visual);

    CHECK(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI(GLES)");
    const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT, EGL_TRUE, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, (EGLConfig)0, EGL_NO_CONTEXT, attribs);
    CHECK(ctx != EGL_NO_CONTEXT, "eglCreateContext");
    CHECK(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx), "eglMakeCurrent (surfaceless)");

    void *fake = dlopen(fake_path, RTLD_NOW | RTLD_NOLOAD);
    struct fake_state *st = fake ? dlsym(fake, "fake_state") : NULL;
    CHECK(st != NULL, "driver stand-in loaded by the vendor");
    if (!st)
        return 1;
    CHECK(!st->robust_attrib_seen && st->client_version == 2, "robust-access attribute dropped, version kept");

    const char *glexts = (const char *)glGetString(GL_EXTENSIONS);
    CHECK(glexts && strstr(glexts, "GL_OES_a") && strstr(glexts, "GL_OES_b") &&
              strstr(glexts, "GL_EXT_texture_format_BGRA8888") && !strstr(glexts, "disjoint_timer_query"),
          "glGetString through glvnd hides the timer query only: '%s'", glexts ? glexts : "(null)");
    CHECK(glexts && strstr(glexts, "GL_EXT_unpack_subimage"),
          "GL_EXT_unpack_subimage added on OpenGL ES 3 (KWin asks for it by name)");
    const char *src = "#if GL_FOO\nx\n#endif\n";
    glShaderSource(1, 1, &src, NULL);
    CHECK(strstr(st->shader, "defined(GL_FOO)") != NULL, "glShaderSource through glvnd gets the GLSL fix");
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, "\1\2\3\4");
    CHECK(st->tex_uploads == 1, "glTexImage2D reaches the driver");

    PFNEGLCREATEIMAGEKHRPROC create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    CHECK(create_image && create_image(dpy, ctx, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)1, NULL) ==
                              (EGLImageKHR)0x42 && st->create_image_target == EGL_GL_TEXTURE_2D_KHR,
          "eglCreateImageKHR via the vendor's dispatch stub");
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence =
        (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    CHECK(dup_fence && dup_fence(dpy, (EGLSyncKHR)1) == 42, "eglDupNativeFenceFDANDROID via dispatch stub");
    void (*query_counter)(GLuint, GLenum) = (void (*)(GLuint, GLenum))eglGetProcAddress("glQueryCounterEXT");
    if (query_counter)
        query_counter(1, 0x8E28 /* GL_TIMESTAMP_EXT */);
    CHECK(st->query_counter_calls == 1, "GL extension function through glvnd's GL dispatch");

    GLenum (*reset_arb)(void) = (GLenum(*)(void))eglGetProcAddress("glGetGraphicsResetStatusARB");
    CHECK(reset_arb && reset_arb() == GL_NO_ERROR, "glGetGraphicsResetStatusARB (Qt) answers GL_NO_ERROR");

    if (kwin)
        gbm_surface_checks(dpy, config);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
