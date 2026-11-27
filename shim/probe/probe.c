/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chameleon-probe [card] [gl] [vulkan]: asks the session's graphics stack
 * what it has and then makes it do it, as a program of the session would:
 * the card's capabilities, EGL and OpenGL ES on a gbm device (every buffer
 * format made, drawn into, fenced and read back; a shader, texture uploads
 * in both byte orders, the fence's and the GPU's clocks) and Vulkan through
 * the loader (each device drawn on and read back). A driver that lists a
 * thing and then gets it wrong shows here as FAIL, not as a name in a list.
 *
 * Each part runs in a process of its own, which says what it is about to do:
 * a crash or a hang inside a driver is reported with that step, and the
 * other parts still run. Run it in a session (`chameleon --probe`, and at the
 * start of `chameleon --debug-log`). Lines: ok, FAIL (a thing that should
 * work didn't), none (an optional thing isn't there), info.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define STAGE_SECONDS 30

/* Shared with the stages' processes. */
static struct shared {
    char step[200];
    int failed, none;
} *g;

static void step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void step(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->step, sizeof g->step, fmt, ap);
    va_end(ap);
}

static void say(const char *kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(const char *kind, const char *fmt, ...)
{
    char line[1800];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!strcmp(kind, "FAIL"))
        __atomic_add_fetch(&g->failed, 1, __ATOMIC_RELAXED);
    else if (!strcmp(kind, "none"))
        __atomic_add_fetch(&g->none, 1, __ATOMIC_RELAXED);
    printf("probe:   %-4s %s\n", kind, line);
}
#define CHECK(cond, ...) say((cond) ? "ok" : "FAIL", __VA_ARGS__)

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void *lib(const char *name)
{
    char path[512];
    const char *prefix = getenv("PREFIX");
    step("loading %s", name);
    void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { /* Termux's own, which its programs find by their run path */
        snprintf(path, sizeof path, "%s/lib/%s", prefix ? prefix : "/data/data/com.termux/files/usr", name);
        h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    }
    return h;
}

static int has(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; p && (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0))
            return 1;
    return 0;
}

/* needed: programs fail without it; else it is optional. */
static void want(const char *list, const char *name, int needed, const char *why)
{
    if (has(list, name))
        say("ok", "%s", name);
    else
        say(needed ? "FAIL" : "none", "%s: %s", name, why);
}

/* ---- the card ---- */

struct cap {
    uint64_t cap, value;
};
#define GET_CAP _IOWR('d', 0x0c, struct cap)
#define SET_CLIENT_CAP _IOW('d', 0x0d, struct cap)

static void stage_card(void)
{
    step("opening /dev/dri/card0");
    int card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    CHECK(card >= 0, "/dev/dri/card0 opens%s%s", card < 0 ? ": " : "", card < 0 ? strerror(errno) : "");
    step("opening /dev/dri/renderD128");
    int render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    CHECK(render >= 0, "/dev/dri/renderD128 opens%s%s", render < 0 ? ": " : "", render < 0 ? strerror(errno) : "");
    static const struct {
        int n;
        const char *name;
    } caps[] = {{0x1, "DUMB_BUFFER"}, {0x5, "PRIME"}, {0x6, "TIMESTAMP_MONOTONIC"}, {0x7, "ASYNC_PAGE_FLIP"},
                {0x8, "CURSOR_WIDTH"}, {0x9, "CURSOR_HEIGHT"}, {0x10, "ADDFB2_MODIFIERS"},
                {0x12, "CRTC_IN_VBLANK_EVENT"}, {0x13, "SYNCOBJ"}, {0x14, "SYNCOBJ_TIMELINE"},
                {0x15, "ATOMIC_ASYNC_PAGE_FLIP"}},
      ccaps[] = {{2, "UNIVERSAL_PLANES"}, {3, "ATOMIC"}, {6, "CURSOR_PLANE_HOTSPOT"}, {7, "PLANE_COLOR_PIPELINE"}};
    for (unsigned i = 0; card >= 0 && i < sizeof caps / sizeof *caps; i++) {
        struct cap c = {.cap = (uint64_t)caps[i].n};
        step("asking the card for capability %s", caps[i].name);
        if (ioctl(card, GET_CAP, &c))
            say("info", "capability %s: %s", caps[i].name, strerror(errno));
        else
            say("info", "capability %s = %llu", caps[i].name, (unsigned long long)c.value);
    }
    for (unsigned i = 0; card >= 0 && i < sizeof ccaps / sizeof *ccaps; i++) {
        struct cap c = {.cap = (uint64_t)ccaps[i].n, .value = 1};
        step("setting client capability %s", ccaps[i].name);
        int r = ioctl(card, SET_CLIENT_CAP, &c);
        if (i < 2)
            CHECK(r == 0, "client capability %s%s%s", ccaps[i].name, r ? ": " : "", r ? strerror(errno) : "");
        else
            say(r ? "none" : "ok", "client capability %s%s%s", ccaps[i].name, r ? ": " : "", r ? strerror(errno) : "");
    }
}

/* ---- EGL and OpenGL ES ---- */

#define EGL_PLATFORM_GBM 0x31D7
#define EGL_EXTENSIONS 0x3055
#define EGL_VENDOR 0x3053
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_NONE 0x3038
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#define GL_EXTENSIONS 0x1F03
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_TEXTURE_2D 0x0DE1
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_TIMESTAMP 0x8E28
#define GL_QUERY_RESULT 0x8866
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TRIANGLE_STRIP 0x0005
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_MAX_TEXTURE_SIZE 0x0D33

typedef void (*anyfn)(void);
#define SYM(h, ret, name, ...) ret (*name)(__VA_ARGS__) = (ret (*)(__VA_ARGS__))dlsym(h, #name)
#define PROC(ret, var, name, ...) ret (*var)(__VA_ARGS__) = (ret (*)(__VA_ARGS__))eglGetProcAddress(name)

static int near(const uint8_t *p, int r, int g_, int b, int a)
{
    return abs(p[0] - r) <= 2 && abs(p[1] - g_) <= 2 && abs(p[2] - b) <= 2 && (a < 0 || abs(p[3] - a) <= 2);
}

/* The time a fence signalled, CLOCK_MONOTONIC ns; 0 if it doesn't say. */
static uint64_t fence_time(int fd)
{
    struct sync_fence_info info[4];
    struct sync_file_info file = {.num_fences = 4, .sync_fence_info = (uint64_t)(uintptr_t)info};
    uint64_t t = 0;
    memset(info, 0, sizeof info);
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &file) != 0)
        return 0;
    for (unsigned i = 0; i < file.num_fences && i < 4; i++)
        if (info[i].timestamp_ns > t)
            t = info[i].timestamp_ns;
    return t;
}

static void stage_gl(void)
{
    void *gbm = lib("libgbm.so.1"), *egl, *gl;
    if (!gbm)
        gbm = lib("libgbm.so");
    egl = lib("libEGL.so.1");
    if (!egl)
        egl = lib("libEGL.so");
    gl = lib("libGLESv2.so.2");
    if (!gl)
        gl = lib("libGLESv2.so");
    CHECK(gbm != NULL, "gbm's library loads");
    CHECK(egl != NULL, "EGL's library loads");
    CHECK(gl != NULL, "OpenGL ES's library loads");
    step("opening the render node");
    int render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (!gbm || !egl || !gl || render < 0) {
        CHECK(render >= 0, "the render node opens");
        return;
    }
    SYM(gbm, void *, gbm_create_device, int);
    SYM(gbm, void *, gbm_bo_create, void *, uint32_t, uint32_t, uint32_t, uint32_t);
    SYM(gbm, void, gbm_bo_destroy, void *);
    SYM(gbm, int, gbm_bo_get_fd, void *);
    SYM(gbm, uint32_t, gbm_bo_get_stride, void *);
    SYM(gbm, uint64_t, gbm_bo_get_modifier, void *);
    SYM(gbm, const char *, gbm_device_get_backend_name, void *);
    SYM(egl, const char *, eglQueryString, void *, int);
    SYM(egl, void *, eglGetPlatformDisplay, unsigned, void *, const intptr_t *);
    SYM(egl, unsigned, eglInitialize, void *, int *, int *);
    SYM(egl, unsigned, eglBindAPI, unsigned);
    SYM(egl, void *, eglCreateContext, void *, void *, void *, const int *);
    SYM(egl, unsigned, eglMakeCurrent, void *, void *, void *, void *);
    SYM(egl, int, eglGetError, void);
    SYM(egl, anyfn, eglGetProcAddress, const char *);
    SYM(gl, const char *, glGetString, unsigned);
    SYM(gl, int, glGetError, void);
    SYM(gl, void, glGetIntegerv, unsigned, int *);
    SYM(gl, void, glGenTextures, int, unsigned *);
    SYM(gl, void, glBindTexture, unsigned, unsigned);
    SYM(gl, void, glTexParameteri, unsigned, unsigned, int);
    SYM(gl, void, glTexImage2D, unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    SYM(gl, void, glGenFramebuffers, int, unsigned *);
    SYM(gl, void, glBindFramebuffer, unsigned, unsigned);
    SYM(gl, void, glFramebufferTexture2D, unsigned, unsigned, unsigned, unsigned, int);
    SYM(gl, unsigned, glCheckFramebufferStatus, unsigned);
    SYM(gl, void, glViewport, int, int, int, int);
    SYM(gl, void, glClearColor, float, float, float, float);
    SYM(gl, void, glClear, unsigned);
    SYM(gl, void, glReadPixels, int, int, int, int, unsigned, unsigned, void *);
    SYM(gl, void, glFlush, void);
    SYM(gl, void, glFinish, void);
    SYM(gl, unsigned, glCreateShader, unsigned);
    SYM(gl, void, glShaderSource, unsigned, int, const char *const *, const int *);
    SYM(gl, void, glCompileShader, unsigned);
    SYM(gl, void, glGetShaderiv, unsigned, unsigned, int *);
    SYM(gl, void, glGetShaderInfoLog, unsigned, int, int *, char *);
    SYM(gl, unsigned, glCreateProgram, void);
    SYM(gl, void, glAttachShader, unsigned, unsigned);
    SYM(gl, void, glBindAttribLocation, unsigned, unsigned, const char *);
    SYM(gl, void, glLinkProgram, unsigned);
    SYM(gl, void, glGetProgramiv, unsigned, unsigned, int *);
    SYM(gl, void, glUseProgram, unsigned);
    SYM(gl, void, glVertexAttribPointer, unsigned, int, unsigned, unsigned char, int, const void *);
    SYM(gl, void, glEnableVertexAttribArray, unsigned);
    SYM(gl, void, glDrawArrays, unsigned, int, int);

    step("asking EGL for its client extensions");
    const char *cl = eglQueryString(NULL, EGL_EXTENSIONS);
    say("info", "EGL client extensions: %s", cl ? cl : "(none)");
    CHECK(has(cl, "EGL_KHR_platform_gbm") || has(cl, "EGL_MESA_platform_gbm"),
          "EGL_KHR_platform_gbm or EGL_MESA_platform_gbm (the display a compositor makes)");
    want(cl, "EGL_EXT_device_base", 0, "the card's devices can't be listed");
    step("making a gbm device on the render node");
    void *dev = gbm_create_device(render);
    CHECK(dev != NULL, "a gbm device on the render node (%s)",
          dev && gbm_device_get_backend_name ? gbm_device_get_backend_name(dev) : "?");
    step("making an EGL display on the gbm device");
    void *dpy = dev && eglGetPlatformDisplay ? eglGetPlatformDisplay(EGL_PLATFORM_GBM, dev, NULL) : NULL;
    int maj = 0, min = 0;
    step("eglInitialize");
    if (!dpy || !eglInitialize(dpy, &maj, &min)) {
        say("FAIL", "an EGL display on it (EGL error 0x%x)", eglGetError());
        return;
    }
    const char *de = eglQueryString(dpy, EGL_EXTENSIONS);
    say("info", "EGL %d.%d, vendor %s", maj, min, eglQueryString(dpy, EGL_VENDOR));
    say("info", "EGL display extensions: %s", de);
    want(de, "EGL_KHR_surfaceless_context", 1, "compositors draw without a window");
    want(de, "EGL_KHR_no_config_context", 0, "a config is needed for each format");
    want(de, "EGL_EXT_image_dma_buf_import", 1, "every buffer is a dmabuf");
    want(de, "EGL_EXT_image_dma_buf_import_modifiers", 0, "formats' layouts can't be asked");
    want(de, "EGL_KHR_image_base", 1, "buffers can't become textures");
    want(de, "EGL_ANDROID_native_fence_sync", 0, "no fences to and from the card");
    want(de, "EGL_KHR_fence_sync", 0, "no fences");
    want(de, "EGL_KHR_wait_sync", 0, "no waiting for fences on the GPU");
    want(de, "EGL_IMG_context_priority", 0, "a compositor can't ask to go first");
    want(de, "EGL_EXT_create_context_robustness", 0, "a GPU reset can't be noticed");
    want(de, "EGL_MESA_image_dma_buf_export", 0, "textures can't be shared as dmabufs (Firefox's WebGL)");
    want(de, "EGL_WL_bind_wayland_display", 0, "old Wayland clients (wl_drm)");

    step("eglCreateContext for OpenGL ES 3 without a config");
    eglBindAPI(EGL_OPENGL_ES_API);
    void *ctx = eglCreateContext(dpy, NULL, NULL, (int[]){EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE});
    int es3 = ctx != NULL;
    if (!ctx) {
        step("eglCreateContext for OpenGL ES 2 without a config");
        ctx = eglCreateContext(dpy, NULL, NULL, (int[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
    }
    step("eglMakeCurrent without a surface");
    if (!ctx || !eglMakeCurrent(dpy, NULL, NULL, ctx)) {
        say("FAIL", "a context without a config or a surface (EGL error 0x%x)", eglGetError());
        return;
    }
    say(es3 ? "ok" : "none", "an OpenGL ES 3 context%s", es3 ? "" : ": only OpenGL ES 2");
    int max_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    say("info", "GPU: %s | %s | %s | GLSL %s | textures up to %d", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
        glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION), max_size);
    const char *ge = glGetString(GL_EXTENSIONS);
    say("info", "OpenGL ES extensions: %s", ge);
    want(ge, "GL_OES_EGL_image", 1, "buffers can't become textures");
    want(ge, "GL_EXT_texture_format_BGRA8888", 1, "wl_shm windows and cursors are BGRA");
    want(ge, "GL_EXT_read_format_bgra", 0, "screenshots and recordings read BGRA");
    want(ge, "GL_OES_EGL_image_external", 0, "video buffers");
    want(ge, "GL_EXT_texture_norm16", 0, "16-bit buffers");
    want(ge, "GL_EXT_color_buffer_half_float", 0, "HDR and colour management");
    want(ge, "GL_EXT_buffer_storage", 0, "faster vertex uploads");
    want(ge, "GL_EXT_disjoint_timer_query", 0, "the GPU's time for a frame can't be measured");
    want(ge, "GL_KHR_debug", 0, "no driver messages");
    if (!es3) {
        want(ge, "GL_EXT_unpack_subimage", 0, "partial uploads are slower");
        want(ge, "GL_EXT_texture_storage", 0, "");
        want(ge, "GL_OES_packed_depth_stencil", 0, "");
    }

    PROC(void, image_target, "glEGLImageTargetTexture2DOES", unsigned, void *);
    PROC(void *, create_image, "eglCreateImageKHR", void *, void *, unsigned, void *, const int *);
    PROC(unsigned, destroy_image, "eglDestroyImageKHR", void *, void *);
    PROC(void *, create_sync, "eglCreateSyncKHR", void *, unsigned, const int *);
    PROC(unsigned, destroy_sync, "eglDestroySyncKHR", void *, void *);
    PROC(int, dup_fence, "eglDupNativeFenceFDANDROID", void *, void *);
    CHECK(image_target && create_image && destroy_image,
          "the functions of the image extensions are handed out (listed isn't enough)");
    if (has(de, "EGL_ANDROID_native_fence_sync"))
        CHECK(create_sync && destroy_sync && dup_fence, "the functions of the fence extensions are handed out");
    if (!image_target || !create_image)
        return;
    int fences = has(de, "EGL_ANDROID_native_fence_sync") && create_sync && dup_fence;

    /* Each format a compositor or an app may ask gbm for: made, drawn into,
     * fenced, read back in both byte orders, polled. */
    static const struct {
        uint32_t fourcc;
        const char *name;
        int needed, alpha;
    } formats[] = {{0x34325258, "XRGB8888", 1, 0}, {0x34325241, "ARGB8888", 1, 1}, {0x34324258, "XBGR8888", 1, 0},
                   {0x34324241, "ABGR8888", 1, 1}, {0x36314752, "RGB565", 0, 0}, {0x30334258, "XBGR2101010", 0, 0},
                   {0x30334241, "ABGR2101010", 0, 1}, {0x48344241, "ABGR16161616F", 0, 1}};
    unsigned target_tex = 0, target_fbo = 0; /* an ABGR8888 buffer kept as the later tests' render target */
    for (unsigned i = 0; i < sizeof formats / sizeof *formats; i++) {
        const char *name = formats[i].name;
        step("gbm_bo_create of a 256x256 %s buffer", name);
        double t0 = now_ms();
        void *bo = gbm_bo_create(dev, 256, 256, formats[i].fourcc, 1 /* SCANOUT */ | 4 /* RENDERING */);
        double t_alloc = now_ms() - t0;
        step("gbm_bo_get_fd of the %s buffer", name);
        int fd = bo ? gbm_bo_get_fd(bo) : -1;
        if (fd < 0) {
            say(formats[i].needed ? "FAIL" : "none", "%s buffers: gbm doesn't make them", name);
            continue;
        }
        int attrs[] = {0x3057, 256, 0x3056, 256, 0x3271, (int)formats[i].fourcc, 0x3272, fd, 0x3273, 0,
                       0x3274, (int)gbm_bo_get_stride(bo), EGL_NONE};
        step("eglCreateImageKHR from the %s buffer's dmabuf", name);
        t0 = now_ms();
        void *img = create_image(dpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
        double t_image = now_ms() - t0;
        unsigned tex = 0, fbo = 0, st = 0;
        if (img) {
            step("glEGLImageTargetTexture2DOES with the %s image", name);
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            image_target(GL_TEXTURE_2D, img);
            step("making the %s texture a render target", name);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        }
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            say(formats[i].needed ? "FAIL" : "none",
                "%s buffers: gbm makes one, but %s (EGL error 0x%x, framebuffer status 0x%x)", name,
                img ? "its texture is no render target" : "EGL refuses it as an image", eglGetError(), st);
            close(fd);
            continue;
        }
        uint8_t px[4] = {0}, bg[4] = {0};
        char fence_text[80] = "no fence";
        int fence_ok = 1;
        step("clearing the %s buffer", name);
        glViewport(0, 0, 256, 256);
        glClearColor(1.0f, 0.5f, 0.25f, 0.5f);
        t0 = now_ms();
        glClear(GL_COLOR_BUFFER_BIT);
        if (fences) {
            step("a native fence after drawing into the %s buffer", name);
            void *sync = create_sync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
            glFlush();
            int fence = sync ? dup_fence(dpy, sync) : -1;
            if (fence >= 0) {
                struct pollfd p = {.fd = fence, .events = POLLIN};
                step("waiting for the fence of the %s buffer", name);
                fence_ok = poll(&p, 1, 2000) == 1;
                snprintf(fence_text, sizeof fence_text, "fence %s", fence_ok ? "signals" : "NEVER SIGNALS");
                close(fence);
            } else {
                snprintf(fence_text, sizeof fence_text, "fence has no file");
                fence_ok = 0;
            }
            if (sync && destroy_sync)
                destroy_sync(dpy, sync);
        }
        step("glFinish after drawing into the %s buffer", name);
        glFinish();
        double t_draw = now_ms() - t0;
        step("glReadPixels RGBA from the %s buffer", name);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int rgba_err = glGetError();
        step("glReadPixels BGRA from the %s buffer", name);
        glReadPixels(0, 0, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, bg);
        int bgra_err = glGetError();
        struct pollfd pd = {.fd = fd, .events = POLLIN};
        step("poll of the %s buffer's dmabuf", name);
        int ready = poll(&pd, 1, 100);
        /* The four 8-bit formats must read back exactly; of the others
         * (fewer bits, two bits of alpha, floats a driver may not read as
         * bytes) the colour is checked loosely, and only if it is read. */
        int exact = formats[i].needed;
        int right = exact ? rgba_err == 0 && near(px, 255, 128, 64, formats[i].alpha ? 128 : 255)
                          : rgba_err != 0 || (abs(px[0] - 255) <= 8 && abs(px[1] - 128) <= 8 && abs(px[2] - 64) <= 8);
        CHECK(right && fence_ok && ready == 1,
              "%s buffers: made in %.2f ms, image in %.2f ms, drawn in %.2f ms, %s, read back %02x%02x%02x%02x%s, "
              "dmabuf %s, layout %#llx",
              name, t_alloc, t_image, t_draw, fence_text, px[0], px[1], px[2], px[3],
              right ? ""
              : !formats[i].alpha && near(px, 255, 128, 64, -1)
                    ? " (WRONG: the alpha written is read back, where a format without alpha reads 1)"
                    : " (WRONG: want ff8040 and its alpha)",
              ready == 1 ? "ready" : "NEVER READY",
              (unsigned long long)gbm_bo_get_modifier(bo));
        if (exact && has(ge, "GL_EXT_read_format_bgra"))
            CHECK(bgra_err == 0 && near(bg, px[2], px[1], px[0], px[3]), "%s buffers read back as BGRA: %02x%02x%02x%02x%s",
                  name, bg[0], bg[1], bg[2], bg[3], bgra_err ? " (refused)" : "");
        close(fd);
        if (formats[i].fourcc == 0x34324241) {
            target_tex = tex;
            target_fbo = fbo;
        } else {
            destroy_image(dpy, img);
            gbm_bo_destroy(bo);
        }
    }
    if (!target_fbo)
        return;
    (void)target_tex;
    glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);

    /* A shader: compiled, linked, drawn with, and the colours it computed. */
    static const char *const vs = "attribute vec2 pos;\nvarying vec2 at;\n"
                                  "void main() { at = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char *const fs_plain = "precision mediump float;\nvarying vec2 at;\n"
                                        "void main() { gl_FragColor = vec4(at.x, at.y, 0.25, 1.0); }\n";
    static const char *const fs_tex = "precision mediump float;\nvarying vec2 at;\nuniform sampler2D tex;\n"
                                      "void main() { gl_FragColor = texture2D(tex, at); }\n";
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    unsigned progs[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
        int ok = 0, ok2 = 0, linked = 0;
        char log[300] = "";
        step("compiling a vertex shader");
        unsigned v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(v, 1, &vs, NULL);
        glCompileShader(v);
        glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
        step("compiling a fragment shader");
        glShaderSource(f, 1, k ? &fs_tex : &fs_plain, NULL);
        glCompileShader(f);
        glGetShaderiv(f, GL_COMPILE_STATUS, &ok2);
        if (!ok || !ok2)
            glGetShaderInfoLog(ok ? f : v, sizeof log, NULL, log);
        step("linking a shader program");
        progs[k] = glCreateProgram();
        glAttachShader(progs[k], v);
        glAttachShader(progs[k], f);
        glBindAttribLocation(progs[k], 0, "pos");
        glLinkProgram(progs[k]);
        glGetProgramiv(progs[k], GL_LINK_STATUS, &linked);
        if (!ok || !ok2 || !linked) {
            say("FAIL", "a shader program compiles and links: %s", log);
            return;
        }
    }
    uint8_t a[4], b[4];
    step("drawing with a shader");
    glUseProgram(progs[0]);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, quad);
    glEnableVertexAttribArray(0);
    double t0 = now_ms();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    double t_first = now_ms() - t0;
    step("reading back what the shader drew");
    glReadPixels(64, 64, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);
    glReadPixels(192, 128, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
    CHECK(abs(a[0] - 64) <= 3 && abs(a[1] - 64) <= 3 && abs(a[2] - 64) <= 2 && abs(b[0] - 192) <= 3 && abs(b[1] - 128) <= 3,
          "a shader compiles, links and draws what it computes (%02x%02x%02x and %02x%02x%02x; first draw %.2f ms)", a[0],
          a[1], a[2], b[0], b[1], b[2], t_first);

    /* Uploads: the same four pixels as RGBA and as BGRA must look the same. */
    static const uint8_t rgba[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    static const uint8_t bgra[16] = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 0, 255, 255, 255};
    for (int k = 0; k < 2; k++) {
        if (k && !has(ge, "GL_EXT_texture_format_BGRA8888"))
            break;
        unsigned up = 0;
        uint8_t q[4][4];
        step("uploading a %s texture", k ? "BGRA" : "RGBA");
        glGenTextures(1, &up);
        glBindTexture(GL_TEXTURE_2D, up);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        while (glGetError()) {
        }
        glTexImage2D(GL_TEXTURE_2D, 0, k ? GL_BGRA : GL_RGBA, 2, 2, 0, k ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE,
                     k ? bgra : rgba);
        int err = glGetError();
        step("drawing a %s texture", k ? "BGRA" : "RGBA");
        glUseProgram(progs[1]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();
        step("reading back the drawn %s texture", k ? "BGRA" : "RGBA");
        glReadPixels(64, 64, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q[0]);
        glReadPixels(192, 64, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q[1]);
        glReadPixels(64, 192, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q[2]);
        glReadPixels(192, 192, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q[3]);
        CHECK(!err && near(q[0], 255, 0, 0, 255) && near(q[1], 0, 255, 0, 255) && near(q[2], 0, 0, 255, 255) &&
                  near(q[3], 255, 255, 0, 255),
              "a texture uploaded as %s is drawn in its colours (red %02x%02x%02x, green %02x%02x%02x, blue "
              "%02x%02x%02x; GL error 0x%x)",
              k ? "BGRA" : "RGBA", q[0][0], q[0][1], q[0][2], q[1][0], q[1][1], q[1][2], q[2][0], q[2][1], q[2][2], err);
    }

    /* The fences' clock: a fence must signal between the draw and now. */
    if (fences) {
        struct timespec ts;
        step("a native fence for its time");
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t before = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        glUseProgram(progs[0]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        void *sync = create_sync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
        glFlush();
        int fence = sync ? dup_fence(dpy, sync) : -1;
        struct pollfd p = {.fd = fence, .events = POLLIN};
        if (fence >= 0 && poll(&p, 1, 2000) == 1) {
            step("asking the fence for its time");
            uint64_t t = fence_time(fence);
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t after = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            if (!t)
                say("none", "a fence's time: the fence doesn't say when it signalled");
            else
                CHECK(t + 1000000 >= before && t <= after + 1000000,
                      "a fence's time is the GPU's finish (%.3f ms after the draw began, seen %.3f ms later)",
                      ((double)t - (double)before) / 1e6, ((double)after - (double)t) / 1e6);
        }
        if (fence >= 0)
            close(fence);
    }
    /* The GPU's clock, as a compositor reads it to time its frames. */
    if (has(ge, "GL_EXT_disjoint_timer_query")) {
        PROC(void, gen, "glGenQueriesEXT", int, unsigned *);
        PROC(void, counter, "glQueryCounterEXT", unsigned, unsigned);
        PROC(void, result, "glGetQueryObjectui64vEXT", unsigned, unsigned, uint64_t *);
        PROC(void, stamp_ext, "glGetInteger64vEXT", unsigned, int64_t *);
        /* OpenGL ES 3 has the clock as a function of its own, which is what
         * a compositor calls there; the extension's name is for ES 2. */
        void (*stamp)(unsigned, int64_t *) = (void (*)(unsigned, int64_t *))dlsym(gl, "glGetInteger64v");
        if (!es3 || !stamp)
            stamp = stamp_ext;
        CHECK(gen && counter && result && stamp, "the timer query's functions are handed out");
        if (gen && counter && result && stamp) {
            unsigned q = 0;
            int64_t start = 0, by_ext = 0;
            uint64_t end = 0;
            step("a GPU timestamp query");
            gen(1, &q);
            stamp(GL_TIMESTAMP, &start);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            counter(q, GL_TIMESTAMP);
            step("glFinish before the timestamp query's result");
            glFinish();
            step("the timestamp query's result");
            result(q, GL_QUERY_RESULT, &end);
            double ms = (double)((int64_t)end - start) / 1e6;
            CHECK(ms > -1.0 && ms < 1000.0,
                  "GPU timestamps: a frame's end minus its start is %.3f ms (a compositor plans its frames by this)", ms);
            if (stamp_ext && stamp != stamp_ext) {
                step("glGetInteger64vEXT(GL_TIMESTAMP)");
                stamp_ext(GL_TIMESTAMP, &by_ext);
                stamp(GL_TIMESTAMP, &start);
                ms = (double)(start - by_ext) / 1e6;
                CHECK(ms > -1.0 && ms < 1000.0,
                      "the GPU's clock by the extension's own name, glGetInteger64vEXT: %s",
                      !by_ext ? "it answers nothing (0)" : ms > -1.0 && ms < 1000.0 ? "the same clock" : "ANOTHER clock");
            }
        }
    }
}

/* ---- Vulkan ---- */

#define VKFN(name) static PFN_##name name
VKFN(vkGetInstanceProcAddr);
VKFN(vkCreateInstance);
VKFN(vkDestroyInstance);
VKFN(vkEnumeratePhysicalDevices);
VKFN(vkGetPhysicalDeviceProperties);
VKFN(vkGetPhysicalDeviceProperties2);
VKFN(vkEnumerateDeviceExtensionProperties);
VKFN(vkGetPhysicalDeviceQueueFamilyProperties);
VKFN(vkGetPhysicalDeviceMemoryProperties);
VKFN(vkCreateDevice);
VKFN(vkDestroyDevice);
VKFN(vkGetDeviceQueue);
VKFN(vkCreateImage);
VKFN(vkGetImageMemoryRequirements);
VKFN(vkAllocateMemory);
VKFN(vkBindImageMemory);
VKFN(vkCreateImageView);
VKFN(vkCreateBuffer);
VKFN(vkGetBufferMemoryRequirements);
VKFN(vkBindBufferMemory);
VKFN(vkMapMemory);
VKFN(vkCreateRenderPass);
VKFN(vkCreateFramebuffer);
VKFN(vkCreateFence);
VKFN(vkResetFences);
VKFN(vkWaitForFences);
VKFN(vkCreateCommandPool);
VKFN(vkAllocateCommandBuffers);
VKFN(vkBeginCommandBuffer);
VKFN(vkEndCommandBuffer);
VKFN(vkCmdBeginRenderPass);
VKFN(vkCmdEndRenderPass);
VKFN(vkCmdCopyImageToBuffer);
VKFN(vkQueueSubmit);
VKFN(vkDeviceWaitIdle);

static uint32_t memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
        if ((bits & 1u << i) && (mem.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return 0;
}

static int listed(const VkExtensionProperties *l, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(l[i].extensionName, name))
            return 1;
    return 0;
}

#define SIZE 64
static void vulkan_device(VkInstance inst, VkPhysicalDevice phys, int index, int v11)
{
    VkPhysicalDeviceProperties props;
    step("vkGetPhysicalDeviceProperties of GPU %d", index);
    vkGetPhysicalDeviceProperties(phys, &props);
    uint32_t n = 0;
    step("listing the extensions of %s", props.deviceName);
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL);
    VkExtensionProperties *exts = calloc(n + 1, sizeof *exts);
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, exts);
    char driver[600] = "";
    int ours = 0;
    if (v11 && vkGetPhysicalDeviceProperties2 && props.apiVersion >= VK_API_VERSION_1_1) {
        VkPhysicalDeviceDrmPropertiesEXT drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        if (listed(exts, n, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) {
            drm.pNext = props2.pNext;
            props2.pNext = &drm;
        }
        if (props.apiVersion >= VK_API_VERSION_1_2 || listed(exts, n, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
            drv.pNext = props2.pNext;
            props2.pNext = &drv;
        }
        step("vkGetPhysicalDeviceProperties2 of %s", props.deviceName);
        vkGetPhysicalDeviceProperties2(phys, &props2);
        ours = drm.hasRender && drm.renderMajor == 226 && drm.renderMinor == 128;
        snprintf(driver, sizeof driver, ", driver %s (%s)", drv.driverName, drv.driverInfo);
    }
    say("info", "GPU %d: %s, Vulkan %u.%u.%u%s, %u extensions%s", index, props.deviceName,
        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
        VK_API_VERSION_PATCH(props.apiVersion), driver, n, ours ? ": the card's driver" : "");
    char names[1500] = "";
    for (uint32_t i = 0; i < n && strlen(names) + VK_MAX_EXTENSION_NAME_SIZE + 2 < sizeof names; i++) {
        strcat(names, " ");
        strcat(names, exts[i].extensionName + (strncmp(exts[i].extensionName, "VK_", 3) ? 0 : 3));
    }
    say("info", "GPU %d extensions (without VK_):%s", index, names);
    static const struct {
        const char *name, *why;
    } wanted[] = {{"VK_KHR_swapchain", "no windows"},
                  {"VK_EXT_physical_device_drm", "programs can't tell which card it is"},
                  {"VK_KHR_external_memory_fd", "no buffers shared as files"},
                  {"VK_EXT_external_memory_dma_buf", "no dmabufs in or out (compositors' Vulkan renderers)"},
                  {"VK_EXT_image_drm_format_modifier", "no dmabuf layouts"},
                  {"VK_KHR_external_fence_fd", "no fences as files"},
                  {"VK_KHR_external_semaphore_fd", "no semaphores as files"},
                  {"VK_KHR_timeline_semaphore", "(or Vulkan 1.2)"},
                  {"VK_KHR_synchronization2", "(or Vulkan 1.3)"},
                  {"VK_KHR_dynamic_rendering", "(or Vulkan 1.3; zink and DXVK want it)"}};
    for (unsigned i = 0; i < sizeof wanted / sizeof *wanted; i++)
        say(listed(exts, n, wanted[i].name) ? "ok" : "none", "GPU %d %s%s%s", index, wanted[i].name,
            listed(exts, n, wanted[i].name) ? "" : ": ", listed(exts, n, wanted[i].name) ? "" : wanted[i].why);
    free(exts);
    (void)inst;

    uint32_t families = 0, family = 0;
    step("the queue families of %s", props.deviceName);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &families, NULL);
    VkQueueFamilyProperties *qf = calloc(families + 1, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &families, qf);
    while (family < families && !(qf[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        family++;
    free(qf);
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                   .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci};
    VkDevice dev;
    step("vkCreateDevice on %s", props.deviceName);
    double t0 = now_ms();
    VkResult r = vkCreateDevice(phys, &dci, NULL, &dev);
    double t_device = now_ms() - t0;
    if (r != VK_SUCCESS) {
        say("FAIL", "GPU %d: a device (VkResult %d)", index, r);
        return;
    }
    VkQueue queue;
    vkGetDeviceQueue(dev, family, 0, &queue);
    step("an image to draw into on %s", props.deviceName);
    VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                              .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {SIZE, SIZE, 1}, .mipLevels = 1,
                              .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                              .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage image;
    VkMemoryRequirements need;
    VkDeviceMemory image_memory, buffer_memory;
    VkImageView view;
    VkBuffer buffer;
    VkRenderPass pass;
    VkFramebuffer fb;
    VkFence fence;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    void *mapped = NULL;
    r = vkCreateImage(dev, &imci, NULL, &image);
    vkGetImageMemoryRequirements(dev, image, &need);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = need.size,
                                .memoryTypeIndex = memory_type(phys, need.memoryTypeBits, 0)};
    r |= vkAllocateMemory(dev, &mai, NULL, &image_memory);
    r |= vkBindImageMemory(dev, image, image_memory, 0);
    VkImageViewCreateInfo ivci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = imci.format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    r |= vkCreateImageView(dev, &ivci, NULL, &view);
    step("a buffer to read back into on %s", props.deviceName);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SIZE * SIZE * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    r |= vkCreateBuffer(dev, &bci, NULL, &buffer);
    vkGetBufferMemoryRequirements(dev, buffer, &need);
    mai.allocationSize = need.size;
    mai.memoryTypeIndex = memory_type(phys, need.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    r |= vkAllocateMemory(dev, &mai, NULL, &buffer_memory);
    r |= vkBindBufferMemory(dev, buffer, buffer_memory, 0);
    r |= vkMapMemory(dev, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    step("a render pass on %s", props.deviceName);
    VkAttachmentDescription attachment = {.format = imci.format, .samples = VK_SAMPLE_COUNT_1_BIT,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                          .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                    .pColorAttachments = &color};
    VkSubpassDependency after = {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
                                 .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                   .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass,
                                   .dependencyCount = 1, .pDependencies = &after};
    r |= vkCreateRenderPass(dev, &rpci, NULL, &pass);
    VkFramebufferCreateInfo fbci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
                                    .attachmentCount = 1, .pAttachments = &view, .width = SIZE, .height = SIZE,
                                    .layers = 1};
    r |= vkCreateFramebuffer(dev, &fbci, NULL, &fb);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    r |= vkCreateFence(dev, &fci, NULL, &fence);
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    r |= vkCreateCommandPool(dev, &cpci, NULL, &pool);
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    r |= vkAllocateCommandBuffers(dev, &cbai, &cmd);
    if (r != VK_SUCCESS || !mapped) {
        say("FAIL", "GPU %d: an image, a buffer and a render pass to draw with (VkResult %d)", index, r);
        return;
    }
    int right = 1, frames = 0;
    double start = now_ms(), first = 0;
    const uint8_t *px = mapped;
    step("drawing and reading back on %s", props.deviceName);
    while (frames < 3 || (now_ms() - start < 500 && right)) {
        int red = frames & 1;
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkClearValue clear = {.color.float32 = {red ? 1.0f : 0.0f, red ? 0.0f : 1.0f, 0.25f, 1.0f}};
        VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
                                    .framebuffer = fb, .renderArea = {{0, 0}, {SIZE, SIZE}}, .clearValueCount = 1,
                                    .pClearValues = &clear};
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {SIZE, SIZE, 1}};
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
        t0 = now_ms();
        r = vkBeginCommandBuffer(cmd, &begin);
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cmd);
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        r |= vkEndCommandBuffer(cmd);
        r |= vkResetFences(dev, 1, &fence);
        r |= vkQueueSubmit(queue, 1, &submit, fence);
        r |= vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
        if (!frames)
            first = now_ms() - t0;
        const uint8_t *p = px + (SIZE / 2 * SIZE + SIZE / 2) * 4;
        right = r == VK_SUCCESS && near(p, red ? 255 : 0, red ? 0 : 255, 64, 255);
        frames++;
    }
    CHECK(right,
          "GPU %d draws and reads back the right colours (device made in %.1f ms, first frame %.2f ms, then %.3f ms a "
          "frame over %d frames)%s",
          index, t_device, first, frames > 1 ? (now_ms() - start - first) / (frames - 1) : 0.0, frames,
          right ? "" : " - WRONG pixels or a failed call");
    step("ending the device on %s", props.deviceName);
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
}

static void stage_vulkan(void)
{
    void *loader = lib("libvulkan.so.1");
    if (!loader) {
        const char *prefix = getenv("PREFIX");
        char path[512];
        /* never the bare "libvulkan.so": that name is Android's own loader too */
        snprintf(path, sizeof path, "%s/lib/libvulkan.so", prefix ? prefix : "/data/data/com.termux/files/usr");
        step("loading %s", path);
        loader = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    }
    if (!loader) {
        say("none", "Vulkan: no loader is installed (libvulkan.so.1)");
        return;
    }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)dlsym(loader, "vkCreateInstance");
    PFN_vkEnumerateInstanceVersion version = (PFN_vkEnumerateInstanceVersion)dlsym(loader, "vkEnumerateInstanceVersion");
    if (!vkGetInstanceProcAddr || !vkCreateInstance) {
        say("FAIL", "the Vulkan loader has its entry points");
        return;
    }
    uint32_t api = VK_API_VERSION_1_0;
    if (version)
        version(&api);
    int v11 = api >= VK_API_VERSION_1_1;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = v11 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    step("vkCreateInstance");
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) {
        say(r == VK_ERROR_INCOMPATIBLE_DRIVER ? "none" : "FAIL", "Vulkan: an instance (VkResult %d%s)", r,
            r == VK_ERROR_INCOMPATIBLE_DRIVER ? ": no driver" : "");
        return;
    }
#define LOAD(name) name = (PFN_##name)vkGetInstanceProcAddr(inst, #name)
    LOAD(vkDestroyInstance);
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceProperties);
    LOAD(vkGetPhysicalDeviceProperties2);
    LOAD(vkEnumerateDeviceExtensionProperties);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkCreateDevice);
    LOAD(vkDestroyDevice);
    LOAD(vkGetDeviceQueue);
    LOAD(vkCreateImage);
    LOAD(vkGetImageMemoryRequirements);
    LOAD(vkAllocateMemory);
    LOAD(vkBindImageMemory);
    LOAD(vkCreateImageView);
    LOAD(vkCreateBuffer);
    LOAD(vkGetBufferMemoryRequirements);
    LOAD(vkBindBufferMemory);
    LOAD(vkMapMemory);
    LOAD(vkCreateRenderPass);
    LOAD(vkCreateFramebuffer);
    LOAD(vkCreateFence);
    LOAD(vkResetFences);
    LOAD(vkWaitForFences);
    LOAD(vkCreateCommandPool);
    LOAD(vkAllocateCommandBuffers);
    LOAD(vkBeginCommandBuffer);
    LOAD(vkEndCommandBuffer);
    LOAD(vkCmdBeginRenderPass);
    LOAD(vkCmdEndRenderPass);
    LOAD(vkCmdCopyImageToBuffer);
    LOAD(vkQueueSubmit);
    LOAD(vkDeviceWaitIdle);
    VkPhysicalDevice phys[8];
    uint32_t n = 8;
    step("vkEnumeratePhysicalDevices");
    vkEnumeratePhysicalDevices(inst, &n, phys);
    say(n ? "info" : "none", "Vulkan: loader %u.%u, %u GPU%s listed", VK_API_VERSION_MAJOR(api),
        VK_API_VERSION_MINOR(api), n, n == 1 ? "" : "s");
    for (uint32_t i = 0; i < n; i++)
        vulkan_device(inst, phys[i], (int)i, v11);
    step("vkDestroyInstance");
    vkDestroyInstance(inst, NULL);
}

/* ---- the stages, each in a process of its own ---- */

static void stage_test_crash(void)
{
    step("a step of the probe's own test");
    raise(SIGSEGV);
}

static void stage_test_hang(void)
{
    step("another step of the probe's own test");
    for (;;)
        pause();
}

static void run(const char *title, void (*fn)(void), int seconds)
{
    printf("probe: == %s\n", title);
    fflush(stdout);
    g->step[0] = 0;
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        fflush(stdout);
        _exit(0);
    }
    if (pid < 0) {
        say("FAIL", "%s: cannot start it: %s", title, strerror(errno));
        return;
    }
    int status = 0;
    double end = now_ms() + seconds * 1000.0;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (done < 0 && errno != EINTR)
            return;
        if (now_ms() > end) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            say("FAIL", "%s: HUNG for %d s while: %s", title, seconds, g->step);
            return;
        }
        usleep(20000);
    }
    if (WIFSIGNALED(status))
        say("FAIL", "%s: CRASHED (signal %d, %s) while: %s", title, WTERMSIG(status), strsignal(WTERMSIG(status)),
            g->step);
    else if (WIFEXITED(status) && WEXITSTATUS(status))
        say("FAIL", "%s: ended early (status %d) while: %s", title, WEXITSTATUS(status), g->step);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    g = mmap(NULL, sizeof *g, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g == MAP_FAILED)
        return 2;
    int all = argc < 2;
    const char *test = getenv("CHAMELEON_PROBE_TEST"); /* the host test's: a stage that crashes, one that hangs */
#define ASKED(name) (all || ({ int y_ = 0; for (int i_ = 1; i_ < argc; i_++) y_ |= !strcmp(argv[i_], name); y_; }))
    double start = now_ms();
    printf("probe: what the session's graphics stack has, and whether it does it\n");
    if (ASKED("card"))
        run("the card", stage_card, STAGE_SECONDS);
    if (ASKED("gl"))
        run("EGL and OpenGL ES, as a compositor uses them", stage_gl, STAGE_SECONDS);
    if (ASKED("vulkan"))
        run("Vulkan, through the loader", stage_vulkan, STAGE_SECONDS);
    if (test) {
        run("the probe's own test: a crash", stage_test_crash, 5);
        run("the probe's own test: a hang", stage_test_hang, 1);
    }
    printf("probe: == done in %.1f s: %d failed, %d optional things not there\n", (now_ms() - start) / 1000.0,
           g->failed, g->none);
    return g->failed ? 1 : 0;
}
