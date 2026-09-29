/*
 * ahb_probe - check whether a plain Termux process can drive the Android
 * vendor GPU driver (e.g. /vendor/lib64/egl/libGLES_mali.so) the way the
 * Project-Chameleon KWin shim needs it to.
 *
 * Everything is resolved at runtime with dlopen()/dlsym() from the *system*
 * libraries, so the binary never links against Termux's Mesa libEGL and no
 * Android/EGL headers are required to build it.
 *
 * What it checks:
 *   1. The system EGL/GLES/nativewindow libraries load from a Termux process.
 *   2. EGL initialises on the vendor driver; lists the extensions KWin needs.
 *   3. A surfaceless GLES 3 context can be made current (KWin requirement).
 *   4. An AHardwareBuffer can be allocated, and its native handle is a dmabuf.
 *   5. AHB -> EGLImage -> texture -> FBO works and the GPU renders into it.
 *   6. Android native fence sync fds can be exported (-> DRM IN_FENCE_FD).
 *   7. The AHB survives a trip over a Unix socket and still holds the pixels
 *      (the zero-copy transport to the presenter app).
 *   8. CPU readback of a GPU-rendered AHB.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- minimal EGL / GLES / AHB declarations --------------------------------- */

typedef void *EGLDisplay, *EGLContext, *EGLConfig, *EGLSurface, *EGLImageKHR,
    *EGLClientBuffer, *EGLSyncKHR;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean, EGLenum;
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte;

#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONFIG_KHR ((EGLConfig)0)
#define EGL_NO_IMAGE_KHR ((EGLImageKHR)0)
#define EGL_NO_SYNC_KHR ((EGLSyncKHR)0)
#define EGL_TRUE 1
#define EGL_NONE 0x3038
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#define EGL_PBUFFER_BIT 0x0001
#define EGL_WINDOW_BIT 0x0004
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#define EGL_NO_NATIVE_FENCE_FD_ANDROID (-1)

#define GL_NO_ERROR 0
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;
typedef struct {
    int32_t left, top, right, bottom;
} ARect;
typedef struct {
    int version, numFds, numInts;
    int data[];
} native_handle_t;

#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN 3ULL
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)
#define AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT (1ULL << 9)
#define AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY (1ULL << 11)

/* ---- resolved entry points ------------------------------------------------ */

static struct {
    EGLDisplay (*GetDisplay)(void *);
    EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*Terminate)(EGLDisplay);
    const char *(*QueryString)(EGLDisplay, EGLint);
    EGLBoolean (*BindAPI)(EGLenum);
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
    EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
    EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
    EGLint (*GetError)(void);
    void *(*GetProcAddress)(const char *);
    /* extensions */
    EGLClientBuffer (*GetNativeClientBufferANDROID)(const AHardwareBuffer *);
    EGLImageKHR (*CreateImageKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLBoolean (*DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    EGLSyncKHR (*CreateSyncKHR)(EGLDisplay, EGLenum, const EGLint *);
    EGLBoolean (*DestroySyncKHR)(EGLDisplay, EGLSyncKHR);
    EGLint (*DupNativeFenceFDANDROID)(EGLDisplay, EGLSyncKHR);
} egl;

static struct {
    const GLubyte *(*GetString)(GLenum);
    GLenum (*GetError)(void);
    void (*GenTextures)(GLsizei, GLuint *);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(GLbitfield);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (*Flush)(void);
    void (*Finish)(void);
    void (*PixelStorei)(GLenum, GLint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    void (*EGLImageTargetTexture2DOES)(GLenum, void *);
} gl;

static struct {
    int (*allocate)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
    void (*release)(AHardwareBuffer *);
    void (*describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
    int (*lock)(AHardwareBuffer *, uint64_t, int32_t, const ARect *, void **);
    int (*unlock)(AHardwareBuffer *, int32_t *);
    int (*sendHandleToUnixSocket)(const AHardwareBuffer *, int);
    int (*recvHandleFromUnixSocket)(int, AHardwareBuffer **);
    const native_handle_t *(*getNativeHandle)(const AHardwareBuffer *); /* LL-NDK, may be hidden */
} ahb;

#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT 0x30BF
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT 0x3138
#define EGL_LOSE_CONTEXT_ON_RESET_EXT 0x31BF
#define EGL_CONTEXT_PRIORITY_LEVEL_IMG 0x3100
#define EGL_CONTEXT_PRIORITY_HIGH_IMG 0x3101

/* Context flavours: the probe's own, and what KWin 6.7 asks for. */
enum ctx_kind { CTX_PLAIN3, CTX_V2, CTX_V2_ROBUST, CTX_V2_PRIORITY, CTX_KWIN };
static enum ctx_kind g_ctx_kind = CTX_PLAIN3;
/* Like KWin: a global share context first, then the one used for rendering,
 * created sharing with it. */
static int g_ctx_shared;

/* ---- reporting ------------------------------------------------------------ */

static int failures;

static void result(int ok, int critical, const char *what, const char *detail)
{
    const char *tag = ok ? "PASS" : (critical ? "FAIL" : "WARN");
    printf("[%s] %s%s%s\n", tag, what, detail && *detail ? " - " : "", detail ? detail : "");
    if (!ok && critical)
        failures++;
}

static int has_ext(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; p && (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
            return 1;
    return 0;
}

static void *must_sym(void *lib, const char *libname, const char *name)
{
    void *p = dlsym(lib, name);
    if (!p) {
        char msg[256];
        snprintf(msg, sizeof msg, "%s missing from %s", name, libname);
        result(0, 1, "resolve symbol", msg);
    }
    return p;
}

#define SYM(tbl, field, lib, libname, name) \
    (*(void **)&(tbl).field = must_sym(lib, libname, name))
#define PROC(tbl, field, name) (*(void **)&(tbl).field = egl.GetProcAddress(name))

/* ---- steps ---------------------------------------------------------------- */

static const char *libdir(void)
{
    return sizeof(void *) == 8 ? "/system/lib64" : "/system/lib";
}

/* CHAMELEON_PROBE_GLVND=1 runs everything through Termux's glvnd
 * (libEGL.so.1 / libGLESv2.so.2) instead of Android's libraries, i.e.
 * through whichever vendor glvnd picks - the Chameleon vendor with
 *   __EGL_VENDOR_LIBRARY_FILENAMES=$XDG_RUNTIME_DIR/chameleon-egl-vendor.json (written by `chameleon`)
 *   CHAMELEON_EGL_DEFAULT=1   (the probe uses the default display) */
static void *open_lib(const char *name)
{
    char path[256], msg[512];
    const char *glvnd = getenv("CHAMELEON_PROBE_GLVND");
    const char *prefix = getenv("PREFIX");
    if (glvnd && *glvnd == '1' && strcmp(name, "libnativewindow.so") != 0)
        snprintf(path, sizeof path, "%s/lib/%s", prefix && *prefix ? prefix : "/data/data/com.termux/files/usr",
                 strcmp(name, "libEGL.so") == 0 ? "libEGL.so.1" : "libGLESv2.so.2");
    else
        snprintf(path, sizeof path, "%s/%s", libdir(), name);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    snprintf(msg, sizeof msg, "%s%s%s", path, h ? "" : ": ", h ? "" : dlerror());
    result(h != NULL, 1, "dlopen", msg);
    return h;
}

static int load_libs(void)
{
    void *legl = open_lib("libEGL.so");
    void *lgles = open_lib("libGLESv2.so");
    void *lnw = open_lib("libnativewindow.so");
    if (!legl || !lgles || !lnw)
        return 0;

    SYM(egl, GetDisplay, legl, "libEGL", "eglGetDisplay");
    SYM(egl, Initialize, legl, "libEGL", "eglInitialize");
    SYM(egl, Terminate, legl, "libEGL", "eglTerminate");
    SYM(egl, QueryString, legl, "libEGL", "eglQueryString");
    SYM(egl, BindAPI, legl, "libEGL", "eglBindAPI");
    SYM(egl, ChooseConfig, legl, "libEGL", "eglChooseConfig");
    SYM(egl, CreateContext, legl, "libEGL", "eglCreateContext");
    SYM(egl, DestroyContext, legl, "libEGL", "eglDestroyContext");
    SYM(egl, MakeCurrent, legl, "libEGL", "eglMakeCurrent");
    SYM(egl, CreateWindowSurface, legl, "libEGL", "eglCreateWindowSurface");
    SYM(egl, DestroySurface, legl, "libEGL", "eglDestroySurface");
    SYM(egl, SwapBuffers, legl, "libEGL", "eglSwapBuffers");
    SYM(egl, GetError, legl, "libEGL", "eglGetError");
    SYM(egl, GetProcAddress, legl, "libEGL", "eglGetProcAddress");

    SYM(gl, GetString, lgles, "libGLESv2", "glGetString");
    SYM(gl, GetError, lgles, "libGLESv2", "glGetError");
    SYM(gl, GenTextures, lgles, "libGLESv2", "glGenTextures");
    SYM(gl, DeleteTextures, lgles, "libGLESv2", "glDeleteTextures");
    SYM(gl, BindTexture, lgles, "libGLESv2", "glBindTexture");
    SYM(gl, TexParameteri, lgles, "libGLESv2", "glTexParameteri");
    SYM(gl, GenFramebuffers, lgles, "libGLESv2", "glGenFramebuffers");
    SYM(gl, DeleteFramebuffers, lgles, "libGLESv2", "glDeleteFramebuffers");
    SYM(gl, BindFramebuffer, lgles, "libGLESv2", "glBindFramebuffer");
    SYM(gl, FramebufferTexture2D, lgles, "libGLESv2", "glFramebufferTexture2D");
    SYM(gl, CheckFramebufferStatus, lgles, "libGLESv2", "glCheckFramebufferStatus");
    SYM(gl, Viewport, lgles, "libGLESv2", "glViewport");
    SYM(gl, ClearColor, lgles, "libGLESv2", "glClearColor");
    SYM(gl, Clear, lgles, "libGLESv2", "glClear");
    SYM(gl, ReadPixels, lgles, "libGLESv2", "glReadPixels");
    SYM(gl, Flush, lgles, "libGLESv2", "glFlush");
    SYM(gl, Finish, lgles, "libGLESv2", "glFinish");
    SYM(gl, PixelStorei, lgles, "libGLESv2", "glPixelStorei");
    SYM(gl, TexImage2D, lgles, "libGLESv2", "glTexImage2D");
    SYM(gl, TexSubImage2D, lgles, "libGLESv2", "glTexSubImage2D");

    SYM(ahb, allocate, lnw, "libnativewindow", "AHardwareBuffer_allocate");
    SYM(ahb, release, lnw, "libnativewindow", "AHardwareBuffer_release");
    SYM(ahb, describe, lnw, "libnativewindow", "AHardwareBuffer_describe");
    SYM(ahb, lock, lnw, "libnativewindow", "AHardwareBuffer_lock");
    SYM(ahb, unlock, lnw, "libnativewindow", "AHardwareBuffer_unlock");
    SYM(ahb, sendHandleToUnixSocket, lnw, "libnativewindow", "AHardwareBuffer_sendHandleToUnixSocket");
    SYM(ahb, recvHandleFromUnixSocket, lnw, "libnativewindow", "AHardwareBuffer_recvHandleFromUnixSocket");
    *(void **)&ahb.getNativeHandle = dlsym(lnw, "AHardwareBuffer_getNativeHandle");
    result(ahb.getNativeHandle != NULL, 0, "AHardwareBuffer_getNativeHandle visible",
           ahb.getNativeHandle ? "" : "hidden by linker namespace; shim must track dmabuf fds another way");
    return failures == 0;
}

static void report_ext(const char *list, const char *name, int critical, const char *why)
{
    char msg[256];
    snprintf(msg, sizeof msg, "%s (%s)", name, why);
    result(has_ext(list, name), critical, "extension", msg);
}

static EGLDisplay init_egl(EGLContext *ctx_out)
{
    EGLDisplay dpy = egl.GetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!dpy || egl.Initialize(dpy, &major, &minor) != EGL_TRUE) {
        char msg[64];
        snprintf(msg, sizeof msg, "eglGetError=0x%x", egl.GetError());
        result(0, 1, "eglInitialize(EGL_DEFAULT_DISPLAY)", msg);
        return NULL;
    }
    char msg[256];
    snprintf(msg, sizeof msg, "EGL %d.%d, vendor \"%s\", version \"%s\"", major, minor,
             egl.QueryString(dpy, EGL_VENDOR), egl.QueryString(dpy, EGL_VERSION));
    result(1, 1, "eglInitialize", msg);

    const char *exts = egl.QueryString(dpy, EGL_EXTENSIONS);
    printf("       EGL_EXTENSIONS: %s\n", exts ? exts : "(null)");
    report_ext(exts, "EGL_KHR_image_base", 1, "EGLImage");
    report_ext(exts, "EGL_ANDROID_image_native_buffer", 1, "AHB -> EGLImage");
    report_ext(exts, "EGL_ANDROID_get_native_client_buffer", 1, "AHB -> EGLClientBuffer");
    report_ext(exts, "EGL_KHR_surfaceless_context", 1, "required by KWin");
    report_ext(exts, "EGL_KHR_no_config_context", 0, "required by KWin; shim can emulate");
    report_ext(exts, "EGL_ANDROID_native_fence_sync", 0, "sync_file fences for IN_FENCE_FD");
    report_ext(exts, "EGL_KHR_fence_sync", 0, "fences");
    report_ext(exts, "EGL_EXT_image_dma_buf_import", 0, "native dmabuf import; shim fakes it if absent");
    report_ext(exts, "EGL_EXT_image_dma_buf_import_modifiers", 0, "shim fakes it if absent");

    PROC(egl, GetNativeClientBufferANDROID, "eglGetNativeClientBufferANDROID");
    PROC(egl, CreateImageKHR, "eglCreateImageKHR");
    PROC(egl, DestroyImageKHR, "eglDestroyImageKHR");
    PROC(egl, CreateSyncKHR, "eglCreateSyncKHR");
    PROC(egl, DestroySyncKHR, "eglDestroySyncKHR");
    PROC(egl, DupNativeFenceFDANDROID, "eglDupNativeFenceFDANDROID");
    PROC(gl, EGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES");

    egl.BindAPI(EGL_OPENGL_ES_API);
    EGLint ctx_attribs[16];
    int na = 0;
    ctx_attribs[na++] = EGL_CONTEXT_CLIENT_VERSION;
    ctx_attribs[na++] = g_ctx_kind == CTX_PLAIN3 ? 3 : 2;
    if (g_ctx_kind == CTX_V2_ROBUST || g_ctx_kind == CTX_KWIN) {
        ctx_attribs[na++] = EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT;
        ctx_attribs[na++] = EGL_TRUE;
        ctx_attribs[na++] = EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT;
        ctx_attribs[na++] = EGL_LOSE_CONTEXT_ON_RESET_EXT;
    }
    if (g_ctx_kind == CTX_V2_PRIORITY || g_ctx_kind == CTX_KWIN) {
        ctx_attribs[na++] = EGL_CONTEXT_PRIORITY_LEVEL_IMG;
        ctx_attribs[na++] = EGL_CONTEXT_PRIORITY_HIGH_IMG;
    }
    ctx_attribs[na] = EGL_NONE;
    EGLContext ctx = EGL_NO_CONTEXT, share = EGL_NO_CONTEXT;
    if (g_ctx_shared && has_ext(exts, "EGL_KHR_no_config_context")) {
        share = egl.CreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
        result(share != EGL_NO_CONTEXT, 1, "global share context", "");
        if (!share || egl.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, share) != EGL_TRUE)
            return NULL;
        gl.GetString(GL_VERSION); /* KWin queries its strings in each context */
        gl.GetString(GL_EXTENSIONS);
    }
    if (has_ext(exts, "EGL_KHR_no_config_context"))
        ctx = egl.CreateContext(dpy, EGL_NO_CONFIG_KHR, share, ctx_attribs);
    int used_config = 0;
    if (!ctx) {
        const EGLint cfg_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                                      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                      EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLConfig cfg;
        EGLint n = 0;
        if (egl.ChooseConfig(dpy, cfg_attribs, &cfg, 1, &n) == EGL_TRUE && n > 0) {
            ctx = egl.CreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
            used_config = 1;
        }
    }
    snprintf(msg, sizeof msg, "%s, eglGetError=0x%x",
             used_config ? "created with an EGLConfig" : "created with EGL_NO_CONFIG_KHR", egl.GetError());
    result(ctx != EGL_NO_CONTEXT, 1, "GLES 3 context", msg);
    if (!ctx)
        return NULL;

    int current = egl.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_TRUE;
    result(current, 1, "surfaceless eglMakeCurrent", "");
    if (!current)
        return NULL;

    snprintf(msg, sizeof msg, "\"%s\" / \"%s\" / \"%s\"", gl.GetString(GL_VENDOR),
             gl.GetString(GL_RENDERER), gl.GetString(GL_VERSION));
    result(1, 1, "GL strings", msg);
    const char *glexts = (const char *)gl.GetString(GL_EXTENSIONS);
    report_ext(glexts, "GL_OES_EGL_image", 1, "EGLImage -> texture");
    report_ext(glexts, "GL_OES_EGL_image_external", 0, "external textures");
    report_ext(glexts, "GL_EXT_EGL_image_storage", 0, "immutable EGLImage storage");
    result(egl.GetNativeClientBufferANDROID && egl.CreateImageKHR && gl.EGLImageTargetTexture2DOES, 1,
           "EGLImage entry points resolved", "");

    *ctx_out = ctx;
    return dpy;
}

static AHardwareBuffer *alloc_ahb(uint32_t w, uint32_t h, uint64_t usage, const char *label)
{
    AHardwareBuffer_Desc d = {.width = w, .height = h, .layers = 1,
                              .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, .usage = usage};
    AHardwareBuffer *buf = NULL;
    int rc = ahb.allocate(&d, &buf);
    char msg[160];
    if (rc == 0) {
        ahb.describe(buf, &d);
        snprintf(msg, sizeof msg, "%s: %ux%u stride %u usage 0x%llx", label, d.width, d.height,
                 d.stride, (unsigned long long)d.usage);
    } else {
        snprintf(msg, sizeof msg, "%s: error %d", label, rc);
    }
    result(rc == 0, 1, "AHardwareBuffer_allocate", msg);
    return rc == 0 ? buf : NULL;
}

static void inspect_handle(AHardwareBuffer *buf)
{
    if (!ahb.getNativeHandle)
        return;
    const native_handle_t *nh = ahb.getNativeHandle(buf);
    if (!nh || nh->numFds < 1) {
        result(0, 0, "native handle", "no fds");
        return;
    }
    /* Vendor grallocs order their fds differently (MediaTek puts a
     * gralloc_extra metadata fd first), so look at every one. */
    int dmabuf_index = -1;
    printf("       native handle: %d fds, %d ints\n", nh->numFds, nh->numInts);
    for (int i = 0; i < nh->numFds; i++) {
        char link[128] = "?", path[64];
        snprintf(path, sizeof path, "/proc/self/fd/%d", nh->data[i]);
        ssize_t n = readlink(path, link, sizeof link - 1);
        link[n > 0 ? n : 0] = '\0';
        struct stat st = {0};
        fstat(nh->data[i], &st);
        printf("       fd[%d] -> \"%s\" inode %llu size %lld\n", i, link,
               (unsigned long long)st.st_ino, (long long)st.st_size);
        if (dmabuf_index < 0 && strstr(link, "dmabuf"))
            dmabuf_index = i;
    }
    char msg[64];
    snprintf(msg, sizeof msg, "fd[%d]", dmabuf_index);
    result(dmabuf_index >= 0, 0, "native handle contains a dmabuf", dmabuf_index >= 0 ? msg : "none found");
}

/* Wraps an AHB as a texture-backed FBO. Returns the FBO, or 0. */
static GLuint wrap_ahb(EGLDisplay dpy, AHardwareBuffer *buf, EGLImageKHR *img, GLuint *tex)
{
    const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    *img = egl.CreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                              egl.GetNativeClientBufferANDROID(buf), attribs);
    if (*img == EGL_NO_IMAGE_KHR) {
        char msg[64];
        snprintf(msg, sizeof msg, "eglGetError=0x%x", egl.GetError());
        result(0, 1, "eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)", msg);
        return 0;
    }
    GLuint fbo = 0;
    gl.GenTextures(1, tex);
    gl.BindTexture(GL_TEXTURE_2D, *tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, *img);
    gl.GenFramebuffers(1, &fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        char msg[64];
        snprintf(msg, sizeof msg, "status 0x%x, glGetError 0x%x", status, gl.GetError());
        result(0, 1, "AHB-backed framebuffer complete", msg);
        return 0;
    }
    return fbo;
}

static void unwrap_ahb(EGLDisplay dpy, EGLImageKHR img, GLuint tex, GLuint fbo)
{
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo)
        gl.DeleteFramebuffers(1, &fbo);
    if (tex)
        gl.DeleteTextures(1, &tex);
    if (img)
        egl.DestroyImageKHR(dpy, img);
}

static int pixel_matches(const uint8_t *px, const uint8_t *want)
{
    for (int i = 0; i < 4; i++)
        if (abs((int)px[i] - (int)want[i]) > 2)
            return 0;
    return 1;
}

static const uint8_t MAGENTA[4] = {255, 0, 255, 255};

static void render_color(uint32_t w, uint32_t h)
{
    gl.Viewport(0, 0, w, h);
    gl.ClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
}

static int read_back(uint32_t w, uint32_t h, const char *what)
{
    uint8_t px[4] = {0};
    gl.ReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    char msg[96];
    snprintf(msg, sizeof msg, "center pixel %u,%u,%u,%u (want 255,0,255,255), glGetError 0x%x",
             px[0], px[1], px[2], px[3], gl.GetError());
    int ok = pixel_matches(px, MAGENTA);
    result(ok, 1, what, msg);
    return ok;
}

static void test_fence(EGLDisplay dpy)
{
    if (!egl.CreateSyncKHR || !egl.DupNativeFenceFDANDROID) {
        result(0, 0, "native fence fd export", "entry points missing");
        return;
    }
    const EGLint attribs[] = {EGL_NONE};
    EGLSyncKHR sync = egl.CreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    gl.Flush();
    int fd = sync ? egl.DupNativeFenceFDANDROID(dpy, sync) : EGL_NO_NATIVE_FENCE_FD_ANDROID;
    char msg[64];
    snprintf(msg, sizeof msg, "fd %d", fd);
    result(fd >= 0, 0, "native fence fd export (-> IN_FENCE_FD)", msg);
    if (fd >= 0)
        close(fd);
    if (sync)
        egl.DestroySyncKHR(dpy, sync);
}

static void test_socket_roundtrip(EGLDisplay dpy, AHardwareBuffer *buf, uint32_t w, uint32_t h)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        result(0, 1, "socketpair", strerror(errno));
        return;
    }
    AHardwareBuffer *got = NULL;
    int rc_send = ahb.sendHandleToUnixSocket(buf, sv[0]);
    int rc_recv = rc_send == 0 ? ahb.recvHandleFromUnixSocket(sv[1], &got) : -1;
    close(sv[0]);
    close(sv[1]);
    char msg[64];
    snprintf(msg, sizeof msg, "send %d, recv %d", rc_send, rc_recv);
    result(rc_send == 0 && rc_recv == 0 && got, 1, "AHB over Unix socket", msg);
    if (!got)
        return;

    EGLImageKHR img = NULL;
    GLuint tex = 0, fbo = wrap_ahb(dpy, got, &img, &tex);
    if (fbo)
        read_back(w, h, "received AHB shares the GPU-rendered pixels (zero-copy)");
    unwrap_ahb(dpy, img, tex, fbo);
    ahb.release(got);
}

/* How the EGL vendor renders Wayland windows: an EGL window surface on an
 * AImageReader (libmediandk), whose frames come out as AHardwareBuffers. */
static void test_image_reader(EGLDisplay dpy, EGLContext ctx)
{
    void *media = dlopen(sizeof(void *) == 8 ? "/system/lib64/libmediandk.so" : "/system/lib/libmediandk.so",
                         RTLD_NOW | RTLD_LOCAL);
    result(media != NULL, 0, "libmediandk (AImageReader) loads", media ? "" : dlerror());
    if (!media)
        return;
    int (*reader_new)(int32_t, int32_t, int32_t, uint64_t, int32_t, void **);
    int (*get_window)(void *, void **);
    int (*acquire)(void *, void **, int *);
    int (*image_ahb)(const void *, AHardwareBuffer **);
    void (*image_delete)(void *);
    void (*reader_delete)(void *);
    *(void **)&reader_new = dlsym(media, "AImageReader_newWithUsage");
    *(void **)&get_window = dlsym(media, "AImageReader_getWindow");
    *(void **)&acquire = dlsym(media, "AImageReader_acquireNextImageAsync");
    *(void **)&image_ahb = dlsym(media, "AImage_getHardwareBuffer");
    *(void **)&image_delete = dlsym(media, "AImage_delete");
    *(void **)&reader_delete = dlsym(media, "AImageReader_delete");
    if (!reader_new || !get_window || !acquire || !image_ahb || !image_delete || !reader_delete) {
        result(0, 0, "AImageReader API", "symbols missing");
        return;
    }
    void *reader = NULL, *window = NULL;
    int r = reader_new(64, 32, 1 /* RGBA_8888 */, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 3, &reader);
    if (r == 0)
        r = get_window(reader, &window);
    result(r == 0 && window, 0, "AImageReader window", "");
    if (r != 0 || !window)
        return;
    const EGLint cfg_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                  EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                  EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    EGLSurface surf = EGL_NO_SURFACE;
    if (egl.ChooseConfig(dpy, cfg_attribs, &cfg, 1, &n) && n > 0)
        surf = egl.CreateWindowSurface(dpy, cfg, window, NULL);
    char msg[128];
    snprintf(msg, sizeof msg, "eglGetError=0x%x", egl.GetError());
    result(surf != EGL_NO_SURFACE, 0, "EGL window surface on it", surf ? "" : msg);
    if (surf != EGL_NO_SURFACE && egl.MakeCurrent(dpy, surf, surf, ctx)) {
        gl.ClearColor(0.0f, 1.0f, 0.0f, 1.0f);
        gl.Clear(GL_COLOR_BUFFER_BIT);
        int swapped = egl.SwapBuffers(dpy, surf);
        void *image = NULL;
        int fence = -1;
        for (int i = 0; swapped && i < 100 && acquire(reader, &image, &fence) != 0; i++)
            usleep(1000);
        AHardwareBuffer *frame = NULL;
        if (image)
            image_ahb(image, &frame);
        AHardwareBuffer_Desc d = {0};
        if (frame)
            ahb.describe(frame, &d);
        snprintf(msg, sizeof msg, "%ux%u, stride %u, format %u", d.width, d.height, d.stride, d.format);
        result(frame != NULL, 0, "frame comes out as an AHardwareBuffer (Wayland apps on the GPU)", frame ? msg : "");
        if (fence >= 0)
            close(fence);
        if (image)
            image_delete(image);
        egl.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    }
    if (surf != EGL_NO_SURFACE)
        egl.DestroySurface(dpy, surf);
    reader_delete(reader);
}

static void test_cpu_readback(EGLDisplay dpy, uint32_t w, uint32_t h)
{
    AHardwareBuffer *buf = alloc_ahb(w, h,
                                     AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                         AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                                         AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                                     "CPU-readable buffer");
    if (!buf)
        return;
    EGLImageKHR img = NULL;
    GLuint tex = 0, fbo = wrap_ahb(dpy, buf, &img, &tex);
    if (fbo) {
        render_color(w, h);
        gl.Finish();
        void *ptr = NULL;
        AHardwareBuffer_Desc d;
        ahb.describe(buf, &d);
        int rc = ahb.lock(buf, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &ptr);
        if (rc == 0 && ptr) {
            const uint8_t *px = (const uint8_t *)ptr + ((size_t)(h / 2) * d.stride + w / 2) * 4;
            char msg[96];
            snprintf(msg, sizeof msg, "center pixel %u,%u,%u,%u", px[0], px[1], px[2], px[3]);
            result(pixel_matches(px, MAGENTA), 0, "CPU readback of GPU-rendered AHB", msg);
            ahb.unlock(buf, NULL);
        } else {
            result(0, 0, "AHardwareBuffer_lock", "failed");
        }
    }
    unwrap_ahb(dpy, img, tex, fbo);
    ahb.release(buf);
}

/* ---- CPU -> texture uploads (what KWin does for every window) ----
 * Each variant runs in a child process, so a driver crash is reported
 * instead of killing the probe. KWin crashed in Mali's glTexImage2D
 * (SIGBUS, NULL+0x29) on its first upload; the data pointer was a tagged
 * heap pointer (0xb4...). These isolate whether tagging is the trigger. */

enum upload_mode { UPLOAD_MALLOC, UPLOAD_MMAP, UPLOAD_UNTAGGED, UPLOAD_MID_FRAME };



#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_TEXTURE_2D_ 0x0DE1

static int upload_child(enum upload_mode mode)
{
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 1);
    if (!load_libs())
        return 10;
    EGLContext ctx;
    EGLDisplay dpy = init_egl(&ctx);
    if (!dpy)
        return 11;
    const int w = 273, h = 273;
    size_t size = (size_t)w * h * 4;
    uint8_t *data;
    if (mode == UPLOAD_MMAP) {
        data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    } else {
        data = malloc(size);
        if (mode == UPLOAD_UNTAGGED)
            data = (uint8_t *)((uintptr_t)data & ((1ull << 56) - 1));
    }
    memset(data, 0x80, size);
    int rounds = 1;
    if (mode == UPLOAD_MID_FRAME) {
        /* Like KWin: render into an AHB-backed framebuffer and upload
         * textures while it is still bound, several frames in a row. */
        AHardwareBuffer *fb = alloc_ahb(1080, 1800,
                                        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                            AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                                            AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
                                        "frame");
        EGLImageKHR img;
        GLuint fbtex;
        if (!fb || !wrap_ahb(dpy, fb, &img, &fbtex))
            return 13;
        rounds = 30;
    }
    for (int i = 0; i < rounds; i++) {
        if (mode == UPLOAD_MID_FRAME)
            render_color(1080, 1800);
        GLuint tex;
        gl.GenTextures(1, &tex);
        gl.BindTexture(GL_TEXTURE_2D_, tex);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, w); /* as KWin does */
        gl.TexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        gl.TexSubImage2D(GL_TEXTURE_2D_, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl.Flush();
    }
    gl.Finish();
    return gl.GetError() == GL_NO_ERROR ? 0 : 12;
}

static void test_uploads(void)
{
    static const struct {
        enum upload_mode mode;
        enum ctx_kind ctx;
        const char *what;
        int shared;
    } variants[] = {
        {UPLOAD_MALLOC, CTX_PLAIN3, "upload, GLES3 context, malloc'd (tagged) data"},
        {UPLOAD_MMAP, CTX_PLAIN3, "upload, GLES3 context, mmap'd data"},
        {UPLOAD_MALLOC, CTX_V2, "upload, CLIENT_VERSION 2 context"},
        {UPLOAD_MALLOC, CTX_V2_ROBUST, "upload, v2 + robust access context"},
        {UPLOAD_MALLOC, CTX_V2_PRIORITY, "upload, v2 + high priority context"},
        {UPLOAD_MALLOC, CTX_KWIN, "upload, KWin's context (v2 + robust + high priority)"},
        {UPLOAD_MID_FRAME, CTX_PLAIN3, "30 uploads while an AHB framebuffer is bound"},
        {UPLOAD_MID_FRAME, CTX_KWIN, "same, KWin's context"},
        {UPLOAD_MALLOC, CTX_V2, "upload, v2 context sharing a global context (as KWin does)", 1},
        {UPLOAD_MID_FRAME, CTX_V2, "30 mid-frame uploads, same shared contexts", 1},
        {UPLOAD_MID_FRAME, CTX_KWIN, "same, KWin's full attributes", 1},
    };
    for (size_t i = 0; i < sizeof variants / sizeof variants[0]; i++) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            g_ctx_kind = variants[i].ctx;
            g_ctx_shared = variants[i].shared;
            _exit(upload_child(variants[i].mode));
        }
        int status = 0;
        waitpid(pid, &status, 0);
        char msg[96];
        if (WIFSIGNALED(status))
            snprintf(msg, sizeof msg, "driver crashed: signal %d (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
        else
            snprintf(msg, sizeof msg, "%s", WEXITSTATUS(status) == 0    ? "ok"
                                            : WEXITSTATUS(status) == 10 ? "could not load the GL libraries"
                                            : WEXITSTATUS(status) == 11 ? "could not create a GL context"
                                            : WEXITSTATUS(status) == 13 ? "could not set up the AHB framebuffer"
                                                                        : "GL error");
        result(WIFEXITED(status) && WEXITSTATUS(status) == 0, 0, variants[i].what, msg);
    }
}

int main(int argc, char **argv)
{
    const uint32_t w = 256, h = 256;
    int try_vendor = argc > 1 && strcmp(argv[1], "--try-vendor") == 0;

    printf("Project-Chameleon AHB/EGL probe (%s)\n", sizeof(void *) == 8 ? "64-bit" : "32-bit");

    if (try_vendor) {
        /* Informational only: shows why the shim goes through the system loader. */
        const char *mali = sizeof(void *) == 8 ? "/vendor/lib64/egl/libGLES_mali.so" : "/vendor/lib/egl/libGLES_mali.so";
        void *h = dlopen(mali, RTLD_NOW | RTLD_LOCAL);
        result(h != NULL, 0, "direct dlopen of vendor driver", h ? mali : dlerror());
    }

    test_uploads();

    if (!load_libs())
        goto done;

    EGLContext ctx = EGL_NO_CONTEXT;
    EGLDisplay dpy = init_egl(&ctx);
    if (!dpy)
        goto done;

    /* The usage KWin's scanout buffers would get from the fake GBM. */
    AHardwareBuffer *buf = alloc_ahb(w, h,
                                     AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                         AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                                         AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
                                     "scanout buffer");
    if (buf) {
        inspect_handle(buf);
        EGLImageKHR img = NULL;
        GLuint tex = 0, fbo = wrap_ahb(dpy, buf, &img, &tex);
        if (fbo) {
            result(1, 1, "AHB -> EGLImage -> texture -> FBO", "");
            render_color(w, h);
            read_back(w, h, "GPU renders into AHB");
            test_fence(dpy);
            gl.Finish();
            test_socket_roundtrip(dpy, buf, w, h);
        }
        unwrap_ahb(dpy, img, tex, fbo);
        ahb.release(buf);
    }

    test_cpu_readback(dpy, w, h);
    test_image_reader(dpy, ctx);

    egl.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    egl.DestroyContext(dpy, ctx);
    egl.Terminate(dpy);

done:
    printf("\n%s: %d critical failure(s)\n", failures ? "RESULT: NOT VIABLE" : "RESULT: VIABLE", failures);
    return failures ? 1 : 0;
}
