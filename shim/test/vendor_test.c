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
#include <GLES3/gl31.h>
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
    int swaps, swap_interval_calls, last_swap_interval;
    int readers, geometry_calls;
    void *current_draw;
    int native_images, images_destroyed, copies, finishes;
    GLuint copy_source;
};

/* A texture as Firefox makes one for WebGL (bound, level 0 only). */
static GLuint texture(GLenum internal, GLenum format, int width, int height)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, width, height, 0, format, GL_UNSIGNED_BYTE, NULL);
    return t;
}

/* EGL_MESA_image_dma_buf_export the way Firefox (WebGL) and GTK 4 use it:
 * an EGLImage of a texture, queried and exported as a dmabuf for another
 * process, which imports it. */
static void export_checks(EGLDisplay dpy, EGLContext ctx, struct fake_state *st)
{
    printf("texture export\n");
    PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC query =
        (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    PFNEGLEXPORTDMABUFIMAGEMESAPROC export =
        (PFNEGLEXPORTDMABUFIMAGEMESAPROC)eglGetProcAddress("eglExportDMABUFImageMESA");
    PFNEGLCREATEIMAGEKHRPROC create_khr = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    CHECK(query && export && create_khr, "eglExportDMABUFImage(Query)MESA via the vendor's dispatch stubs");
    if (!query || !export || !create_khr)
        return;

    /* Firefox: glTexImage2D(NULL), eglCreateImage without attributes. */
    GLuint tex = texture(GL_RGBA, GL_RGBA, 64, 32);
    glBindTexture(GL_TEXTURE_2D, 0);
    int images = st->native_images;
    EGLImage image = eglCreateImage(dpy, ctx, EGL_GL_TEXTURE_2D, (EGLClientBuffer)(uintptr_t)tex, NULL);
    GLint bound = -1;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    CHECK(image != EGL_NO_IMAGE && image != (EGLImage)0x42 && st->native_images == images + 1 && st->copies == 0,
          "an RGBA texture's image is a session buffer's (AHardwareBuffer), contents not copied unless preserved");
    CHECK(bound == 0 && glGetError() == GL_NO_ERROR, "the app's texture binding is left as it was, no GL error");
    int fourcc = 0, planes = 0;
    EGLuint64KHR modifier = 0;
    CHECK(query(dpy, image, &fourcc, &planes, &modifier) && fourcc == 0x34324241 && planes == 1 &&
              modifier == 0x00ffffffffffffffULL,
          "query: ABGR8888, 1 plane, implicit modifier (0x%x, %d)", fourcc, planes);
    CHECK(query(dpy, image, NULL, NULL, NULL), "query with every output NULL");
    int fd = -1;
    EGLint stride = 0, offset = -1;
    CHECK(export(dpy, image, &fd, &stride, &offset) && fd >= 0 && stride == 64 * 4 && offset == 0,
          "export: a dmabuf fd, stride %d, offset %d", stride, offset);
    int fd2 = -1;
    CHECK(export(dpy, image, &fd2, NULL, NULL) && fd2 >= 0 && fd2 != fd, "each export a new fd for the caller");
    if (fd2 >= 0)
        close(fd2);
    CHECK(!query(dpy, (EGLImageKHR)0x42, &fourcc, &planes, &modifier) && eglGetError() == EGL_BAD_PARAMETER,
          "the driver's own images aren't exportable: EGL_BAD_PARAMETER");

    /* Gone from this process, the buffer stays chameleon-kernel's as long as
     * its dmabuf is held: what another process (the compositor) imports. */
    int destroyed = st->images_destroyed;
    CHECK(eglDestroyImage(dpy, image) && st->images_destroyed == destroyed + 1, "eglDestroyImage reaches the driver");
    CHECK(!query(dpy, image, &fourcc, &planes, &modifier), "...and the image is no longer exportable");
    const EGLint import[] = {EGL_WIDTH, 64, EGL_HEIGHT, 32, EGL_LINUX_DRM_FOURCC_EXT, 0x34324241,
                             EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                             EGL_DMA_BUF_PLANE0_PITCH_EXT, 256, EGL_NONE};
    void *(*own_bo)(int) = (void *(*)(int))dlsym(RTLD_DEFAULT, "cham_bo_from_fd");
    CHECK(own_bo && fd >= 0 && !own_bo(fd), "this process let go of the buffer (chameleon-kernel has it)");
    EGLImageKHR imported = fd >= 0 ? create_khr(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, import) : NULL;
    CHECK(imported != EGL_NO_IMAGE_KHR, "...and the exported dmabuf imports from there");
    if (imported)
        eglDestroyImage(dpy, imported);
    if (fd >= 0)
        close(fd);

    /* GTK 4: a finished texture, EGL_IMAGE_PRESERVED_KHR. */
    GLuint rgb = texture(GL_RGB8, GL_RGB, 16, 16);
    const EGLint preserved[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    image = create_khr(dpy, ctx, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)rgb, preserved);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    CHECK(image != EGL_NO_IMAGE_KHR && image != (EGLImageKHR)0x42 && st->copies == 1 && st->copy_source == rgb &&
              st->finishes == 1 && query(dpy, image, &fourcc, NULL, NULL) && fourcc == 0x34324258,
          "preserved: the contents copied (GPU finished), an RGB texture is XBGR8888 (0x%x)", fourcc);
    CHECK(bound == (GLint)rgb && glGetError() == GL_NO_ERROR, "binding kept, no GL error");
    eglDestroyImage(dpy, image);

    /* Anything else stays the driver's. */
    GLuint immutable;
    glGenTextures(1, &immutable);
    glBindTexture(GL_TEXTURE_2D, immutable);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 8, 8);
    image = create_khr(dpy, ctx, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)immutable, NULL);
    CHECK(image == (EGLImageKHR)0x42 && !query(dpy, image, NULL, NULL, NULL),
          "an immutable texture (glTexStorage2D) is the driver's, not exportable");
    GLuint mipmapped = texture(GL_RGBA, GL_RGBA, 8, 8);
    glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    image = create_khr(dpy, ctx, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)mipmapped, NULL);
    CHECK(image == (EGLImageKHR)0x42, "...as is one with mipmap levels");
    GLuint plain = texture(GL_RGBA, GL_RGBA, 8, 8);
    const EGLint level1[] = {EGL_GL_TEXTURE_LEVEL_KHR, 1, EGL_NONE};
    image = create_khr(dpy, ctx, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)plain, level1);
    CHECK(image == (EGLImageKHR)0x42, "...and an image of another level");
    image = create_khr(dpy, ctx, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)999, NULL);
    CHECK(image == (EGLImageKHR)0x42 && glGetError() == GL_NO_ERROR, "...and a name that isn't a texture, no GL error");
    CHECK(st->native_images == st->images_destroyed, "every session buffer image destroyed (%d made, %d destroyed)",
          st->native_images, st->images_destroyed);
}

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

    /* VENDOR_TEST_SESSION=1: libchameleon.so is preloaded, as in every
     * program of a Chameleon session */
    int session = getenv("VENDOR_TEST_SESSION") != NULL;
    printf("glvnd vendor (%s)\n",
           session ? "in a Chameleon session, with libchameleon.so" : "without libchameleon.so");
    const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    CHECK(client && strstr(client, "EGL_MESA_platform_surfaceless"), "client extensions: surfaceless platform");
    if (session) {
        CHECK(client && strstr(client, "EGL_KHR_platform_gbm"), "GBM platform in a session");
        /* libchameleon.so is in every program of a session: the default
         * display stays Mesa's (X11 apps) unless asked for. */
        CHECK(eglGetDisplay(EGL_DEFAULT_DISPLAY) == EGL_NO_DISPLAY, "default display left to other vendors");
        setenv("CHAMELEON_EGL_DEFAULT", "1", 1);
        CHECK(eglGetDisplay(EGL_DEFAULT_DISPLAY) != EGL_NO_DISPLAY, "...taken with CHAMELEON_EGL_DEFAULT=1");
        unsetenv("CHAMELEON_EGL_DEFAULT");
        EGLDisplay gbm_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, (void *)1, NULL);
        CHECK(gbm_dpy != EGL_NO_DISPLAY, "GBM display for the compositor");
        /* The card as an EGLDevice, as Mesa describes a DRM device with a render node. */
        PFNEGLQUERYDISPLAYATTRIBEXTPROC display_attrib =
            (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress("eglQueryDisplayAttribEXT");
        PFNEGLQUERYDEVICESTRINGEXTPROC device_string =
            (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
        PFNEGLQUERYDEVICESEXTPROC query_devices = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
        EGLAttrib device = 0;
        EGLint gmajor, gminor;
        CHECK(client && strstr(client, "EGL_EXT_device_query") && strstr(client, "EGL_EXT_device_enumeration") &&
                  strstr(client, "EGL_EXT_platform_device"),
              "EGL_EXT_device_base and EGL_EXT_platform_device in a session");
        CHECK(display_attrib && eglInitialize(gbm_dpy, &gmajor, &gminor) &&
                  display_attrib(gbm_dpy, EGL_DEVICE_EXT, &device) && device,
              "the GBM display's EGLDevice");
        const char *dev_exts = device ? device_string((EGLDeviceEXT)device, EGL_EXTENSIONS) : NULL;
        const char *file = device ? device_string((EGLDeviceEXT)device, EGL_DRM_DEVICE_FILE_EXT) : NULL;
        const char *render = device ? device_string((EGLDeviceEXT)device, EGL_DRM_RENDER_NODE_FILE_EXT) : NULL;
        CHECK(dev_exts && strstr(dev_exts, "EGL_EXT_device_drm") && strstr(dev_exts, "EGL_EXT_device_drm_render_node") &&
                  file && strcmp(file, "/dev/dri/card0") == 0 && render && strcmp(render, "/dev/dri/renderD128") == 0,
              "it is the card, /dev/dri/card0 with /dev/dri/renderD128 (%s, %s)", file ? file : "none",
              render ? render : "none");
        EGLDeviceEXT devices[8];
        EGLint count = 0, ours = 0;
        if (query_devices && query_devices(8, devices, &count))
            for (EGLint i = 0; i < count; i++)
                ours += devices[i] == (EGLDeviceEXT)device;
        CHECK(ours == 1, "enumerated, as Mesa lists a DRM device with a render node");
        EGLDisplay dev_dpy = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, (void *)device, NULL);
        CHECK(dev_dpy != EGL_NO_DISPLAY && eglInitialize(dev_dpy, &gmajor, &gminor),
              "a display on the device itself (EGL_PLATFORM_DEVICE_EXT, as wlroots opens one)");
    } else {
        CHECK(client && !strstr(client, "platform_gbm"), "no GBM platform without libchameleon.so");
        CHECK(eglGetDisplay(EGL_DEFAULT_DISPLAY) == EGL_NO_DISPLAY,
              "default display left to other vendors without libchameleon.so");
    }

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    CHECK(dpy != EGL_NO_DISPLAY, "surfaceless display");
    EGLint major = 0, minor = 0;
    CHECK(eglInitialize(dpy, &major, &minor) && major == 1 && minor == 5,
          "eglInitialize: EGL %d.%d over the driver's 1.4 (Android's libEGL has the 1.5 functions)", major, minor);
    const char *version = eglQueryString(dpy, EGL_VERSION);
    CHECK(version && strncmp(version, "1.5 ", 4) == 0, "EGL_VERSION '%s'", version ? version : "?");
    CHECK(eglWaitSync(dpy, (EGLSync)1, 0) == EGL_TRUE, "eglWaitSync reaches the driver");
    const char *vendor = eglQueryString(dpy, EGL_VENDOR);
    CHECK(vendor && strcmp(vendor, "fake Android") == 0, "display belongs to the Android driver");
    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (session) {
        CHECK(exts && strstr(exts, "EGL_EXT_image_dma_buf_import"), "dmabuf import for the compositor");
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
        CHECK(exts && strstr(exts, "EGL_MESA_image_dma_buf_export"), "dmabuf export of textures (Firefox WebGL)");
    } else
        CHECK(exts && !strstr(exts, "dma_buf_import") && !strstr(exts, "dma_buf_export"),
              "no dmabuf import or export without libchameleon.so");
    /* EGL_KHR_platform_gbm: a config's native visual is its GBM format (the
     * stand-in's config is Android's RGBA_8888 with alpha). */
    EGLConfig config;
    EGLint nconfigs = 0, visual = 0;
    eglChooseConfig(dpy, (const EGLint[]){EGL_NONE}, &config, 1, &nconfigs);
    eglGetConfigAttrib(dpy, config, EGL_NATIVE_VISUAL_ID, &visual);
    if (session)
        CHECK(visual == 0x34324241, "native visual of an RGBA_8888 config: ABGR8888 for GBM (0x%x)", visual);
    else
        CHECK(visual == 1, "native visual left Android's without libchameleon.so (0x%x)", visual);
    /* Window configs are those of the window buffers' formats: the driver's
     * 10-bit config, which sorts first, is for off-screen use only. */
    EGLConfig two[2] = {0};
    EGLint type = 0, count = 0;
    eglChooseConfig(dpy, (const EGLint[]){EGL_RED_SIZE, 1, EGL_NONE}, NULL, 0, &count);
    CHECK(nconfigs == 1 && count == 1 && config == (EGLConfig)0x77,
          "a window config for 1 bit per colour is the 8-bit one (%d of them)", count);
    eglChooseConfig(dpy, (const EGLint[]){EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE}, two, 2, &count);
    eglGetConfigAttrib(dpy, (EGLConfig)0x76, EGL_SURFACE_TYPE, &type);
    CHECK(count == 2 && two[0] == (EGLConfig)0x76 && type == EGL_PBUFFER_BIT,
          "the 10-bit config stays for pbuffers, without EGL_WINDOW_BIT (0x%x)", type);
    CHECK(eglCreateWindowSurface(dpy, (EGLConfig)0x76, (EGLNativeWindowType)0, NULL) == EGL_NO_SURFACE &&
              eglGetError() == EGL_BAD_MATCH,
          "a window surface with it: EGL_BAD_MATCH");

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

    if (session) {
        export_checks(dpy, ctx, st);
        gbm_surface_checks(dpy, config);
    } else {
        GLuint tex = texture(GL_RGBA, GL_RGBA, 8, 8);
        CHECK(eglCreateImage(dpy, ctx, EGL_GL_TEXTURE_2D, (EGLClientBuffer)(uintptr_t)tex, NULL) == (EGLImage)0x42,
              "a texture's image is the driver's without libchameleon.so");
    }

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
