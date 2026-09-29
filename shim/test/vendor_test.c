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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                         \
    do {                                         \
        int ok_ = (cond);                        \
        printf("  %s  ", ok_ ? "ok  " : "FAIL"); \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
        failures += !ok_;                        \
    } while (0)

struct fake_state {
    int robust_attrib_seen, client_version;
    char shader[512];
    int tex_uploads;
    int create_image_target;
    int query_counter_calls;
};

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
        CHECK(eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, (void *)1, NULL) != EGL_NO_DISPLAY, "GBM display for KWin");
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
    if (kwin)
        CHECK(exts && strstr(exts, "EGL_EXT_image_dma_buf_import"), "dmabuf import for KWin");
    else
        CHECK(exts && !strstr(exts, "dma_buf_import"), "no dmabuf import outside KWin");

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
              !strstr(glexts, "BGRA8888") && !strstr(glexts, "disjoint_timer_query"),
          "glGetString through glvnd hides extensions: '%s'", glexts ? glexts : "(null)");
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

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
