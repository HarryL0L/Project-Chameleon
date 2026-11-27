/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stand-in for Android's libEGL.so, libGLESv2.so, libnativewindow.so and
 * libmediandk.so in the host tests of the glvnd vendor: just enough EGL,
 * GLES, AHardwareBuffer and AImageReader to be driven through glvnd,
 * recording what reaches "the driver" in `fake_state`. AHardwareBuffers are
 * memfds named like dmabufs; a window surface renders into its AImageReader.
 */
#define EGL_EGLEXT_PROTOTYPES
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define API __attribute__((visibility("default")))

API struct {
    int robust_attrib_seen, client_version;
    char shader[512];
    int tex_uploads;
    int create_image_target;
    int query_counter_calls;
    int swaps, swap_interval_calls, last_swap_interval;
    int readers, geometry_calls;
    void *current_draw;
    /* textures made EGLImages of AHardwareBuffers (EGL_MESA_image_dma_buf_export) */
    int native_images, images_destroyed, copies, finishes;
    GLuint copy_source; /* texture the last glCopyTexSubImage2D read */
    GLuint swizzle_a_one; /* the last texture told to read alpha as 1 */
    int fence_waits, fence_waits_flushed; /* eglClientWaitSyncKHR on fences */
    GLuint vao_bound;
    /* Fence files: one with each frame out of an AImageReader and for each
     * EGL_SYNC_NATIVE_FENCE_ANDROID sync, signalled gpu_ms after it is made
     * (set by a test; while 0, frames come without one, as finished). */
    int gpu_ms, fence_files;
    /* native fences (GPU timestamps); gpu_hold: new fences stay unsignalled
     * until fake_gpu_release() */
    int fences_created, fences_destroyed, flushes, gpu_hold, query_deletes;
} fake_state;

static void *fence_signal(void *arg)
{
    int *a = arg;
    usleep((useconds_t)a[1] * 1000);
    close(a[0]); /* the read end is readable from then (hung up) */
    free(a);
    return NULL;
}

static int fence_file(void)
{
    int p[2], *a = malloc(2 * sizeof *a);
    pthread_t t;
    if (!a || pipe2(p, O_CLOEXEC) != 0)
        return -1;
    a[0] = p[1];
    a[1] = fake_state.gpu_ms;
    pthread_create(&t, NULL, fence_signal, a);
    pthread_detach(t);
    fake_state.fence_files++;
    return p[0];
}

static EGLDisplay const k_display = (EGLDisplay)0x1234;
static EGLContext const k_context = (EGLContext)0x5678;

API EGLDisplay eglGetDisplay(EGLNativeDisplayType d)
{
    return d == EGL_DEFAULT_DISPLAY ? k_display : EGL_NO_DISPLAY;
}
API EGLBoolean eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    if (major)
        *major = 1;
    if (minor)
        *minor = 4; /* as Mali */
    return d == k_display;
}
API EGLint eglGetError(void) { return EGL_SUCCESS; }
API EGLBoolean eglBindAPI(EGLenum api) { return api == EGL_OPENGL_ES_API; }
API const char *eglQueryString(EGLDisplay d, EGLint name)
{
    (void)d;
    switch (name) {
    case EGL_EXTENSIONS:
        return "EGL_KHR_image_base EGL_ANDROID_get_frame_timestamps EGL_ANDROID_native_fence_sync "
               "EGL_KHR_no_config_context EGL_KHR_reusable_sync EGL_KHR_surfaceless_context EGL_KHR_lock_surface3";
    case EGL_VENDOR:
        return "fake Android";
    case EGL_VERSION:
        return "1.4 Android META-EGL";
    case EGL_CLIENT_APIS:
        return "OpenGL_ES";
    }
    return NULL;
}
API EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attribs)
{
    (void)d, (void)c, (void)share;
    for (const EGLint *a = attribs; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT)
            fake_state.robust_attrib_seen = 1;
        if (a[0] == EGL_CONTEXT_CLIENT_VERSION)
            fake_state.client_version = a[1];
    }
    return k_context;
}
static EGLContext g_current_ctx;
static EGLSurface g_current_read;
API EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c)
{
    fake_state.current_draw = dr;
    g_current_read = rd;
    g_current_ctx = c;
    return d == k_display;
}
API EGLSurface eglGetCurrentSurface(EGLint which)
{
    return which == EGL_READ ? g_current_read : fake_state.current_draw;
}
API EGLContext eglGetCurrentContext(void) { return g_current_ctx; }
API EGLDisplay eglGetCurrentDisplay(void) { return g_current_ctx ? k_display : EGL_NO_DISPLAY; }
API EGLBoolean eglSwapInterval(EGLDisplay d, EGLint interval)
{
    (void)d;
    fake_state.swap_interval_calls++;
    fake_state.last_swap_interval = interval;
    return EGL_TRUE;
}
/* Like Mali: a 10-bit config sorts before the RGBA_8888 one when an app
 * asks for a few bits per colour. */
#define CONFIG_10BIT ((EGLConfig)0x76)
#define CONFIG_RGBA8 ((EGLConfig)0x77)
API EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *a, EGLConfig *configs, EGLint size, EGLint *n)
{
    (void)d, (void)a;
    const EGLConfig all[] = {CONFIG_10BIT, CONFIG_RGBA8};
    *n = 2;
    if (configs) {
        *n = size < 2 ? size : 2;
        for (EGLint i = 0; i < *n; i++)
            configs[i] = all[i];
    }
    return EGL_TRUE;
}
API EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attribute, EGLint *value)
{
    (void)d;
    if (attribute == EGL_NATIVE_VISUAL_ID)
        *value = c == CONFIG_10BIT ? 0x2b /* RGBA_1010102 */ : 1 /* RGBA_8888 */;
    else if (attribute == EGL_SURFACE_TYPE)
        *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
    else
        *value = 8;
    return EGL_TRUE;
}

/* ---- AHardwareBuffer + AImageReader stand-ins ---- */

typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} ahb_desc;
typedef struct AHardwareBuffer {
    ahb_desc desc;
    int refs;
    struct { int version, numFds, numInts; int data[1]; } nh;
} AHardwareBuffer;

static AHardwareBuffer *fake_ahb_new(uint32_t w, uint32_t h, uint32_t format)
{
    AHardwareBuffer *b = calloc(1, sizeof *b);
    b->desc = (ahb_desc){.width = w, .height = h, .layers = 1, .format = format, .stride = w};
    b->refs = 1;
    b->nh.numFds = 1;
    b->nh.data[0] = memfd_create("dmabuf-fake", MFD_CLOEXEC); /* found by name, like a real dmabuf */
    if (ftruncate(b->nh.data[0], (off_t)w * h * 4) != 0)
        abort();
    return b;
}
API void AHardwareBuffer_describe(const AHardwareBuffer *b, ahb_desc *d) { *d = b->desc; }
API const void *AHardwareBuffer_getNativeHandle(const AHardwareBuffer *b) { return &b->nh; }
API void AHardwareBuffer_acquire(AHardwareBuffer *b) { b->refs++; }
API void AHardwareBuffer_release(AHardwareBuffer *b)
{
    if (--b->refs == 0) {
        close(b->nh.data[0]);
        free(b);
    }
}
/* One packet: the description plus the fd (what libchameleon's host build reads). */
API int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *b, int sock)
{
    struct iovec iov = {.iov_base = (void *)&b->desc, .iov_len = sizeof b->desc};
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } control;
    memset(&control, 0, sizeof control);
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.buf,
                        .msg_controllen = sizeof control.buf};
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &b->nh.data[0], sizeof(int));
    return sendmsg(sock, &mh, MSG_NOSIGNAL) == (ssize_t)sizeof b->desc ? 0 : -1;
}

#define READER_SLOTS 5
enum { FREE, QUEUED, ACQUIRED };
struct AImageReader;
typedef struct {
    uint32_t magic; /* like ANativeWindow's '_wnd' */
    struct AImageReader *reader;
} fake_window;
typedef struct AImageReader {
    int32_t width, height, format, max_images, acquired;
    int32_t buffer_width, buffer_height; /* what the window gives (setBuffersGeometry) */
    AHardwareBuffer *buffers[READER_SLOTS];
    int state[READER_SLOTS];
    fake_window window;
} AImageReader;
typedef struct AImage {
    AImageReader *reader;
    int slot;
} AImage;

API int AImageReader_newWithUsage(int32_t w, int32_t h, int32_t format, uint64_t usage, int32_t max_images,
                                  AImageReader **out)
{
    (void)usage;
    fake_state.readers++;
    AImageReader *r = calloc(1, sizeof *r);
    r->width = r->buffer_width = w;
    r->height = r->buffer_height = h;
    r->format = format;
    r->max_images = max_images;
    r->window.magic = 0x5f776e64;
    r->window.reader = r;
    for (int i = 0; i < READER_SLOTS; i++)
        r->buffers[i] = fake_ahb_new((uint32_t)w, (uint32_t)h, (uint32_t)format);
    *out = r;
    return 0;
}
/* As a BufferQueue's producer: buffers of this size from the next one on
 * (0x0: the reader's own size). */
API int ANativeWindow_setBuffersGeometry(fake_window *w, int32_t width, int32_t height, int32_t format)
{
    (void)format;
    if (!w || w->magic != 0x5f776e64 || width < 0 || height < 0)
        return -22;
    w->reader->buffer_width = width ? width : w->reader->width;
    w->reader->buffer_height = height ? height : w->reader->height;
    fake_state.geometry_calls++;
    return 0;
}
API int AImageReader_getWindow(AImageReader *r, void **window)
{
    *window = &r->window;
    return 0;
}
API int AImageReader_acquireNextImageAsync(AImageReader *r, AImage **image, int *fence)
{
    if (r->acquired >= r->max_images)
        return -30002; /* AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED */
    for (int i = 0; i < READER_SLOTS; i++) {
        if (r->state[i] == QUEUED) {
            r->state[i] = ACQUIRED;
            r->acquired++;
            *image = calloc(1, sizeof **image);
            (*image)->reader = r;
            (*image)->slot = i;
            *fence = fake_state.gpu_ms ? fence_file() : -1;
            return 0;
        }
    }
    return -30001; /* AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE */
}
API int AImage_getHardwareBuffer(const AImage *image, AHardwareBuffer **out)
{
    *out = image->reader->buffers[image->slot];
    return 0;
}
API void AImage_delete(AImage *image)
{
    image->reader->state[image->slot] = FREE;
    image->reader->acquired--;
    free(image);
}
API void AImageReader_delete(AImageReader *r)
{
    for (int i = 0; i < READER_SLOTS; i++)
        AHardwareBuffer_release(r->buffers[i]);
    free(r);
}

/* Window surfaces render into the reader: a swap queues a free buffer. */
typedef struct {
    AImageReader *reader;
} fake_surface;
API EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType win, const EGLint *a)
{
    (void)d, (void)c, (void)a;
    fake_window *w = (fake_window *)win;
    if (!w || w->magic != 0x5f776e64)
        return EGL_NO_SURFACE;
    fake_surface *s = calloc(1, sizeof *s);
    s->reader = w->reader;
    return (EGLSurface)s;
}
API EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface surface)
{
    (void)d;
    free(surface);
    return EGL_TRUE;
}
/* A producer's frame: queues a free buffer of the window's size and gives
 * it, for a producer that has pixels to put in (the stand-in for Android's
 * Vulkan swapchain). NULL when the reader holds them all: a real driver
 * would block here. */
API AHardwareBuffer *fake_window_queue(fake_window *w)
{
    if (!w || w->magic != 0x5f776e64)
        return NULL;
    AImageReader *r = w->reader;
    for (int i = 0; i < READER_SLOTS; i++) {
        if (r->state[i] == FREE) {
            AHardwareBuffer *b = r->buffers[i];
            if ((int32_t)b->desc.width != r->buffer_width || (int32_t)b->desc.height != r->buffer_height) {
                /* the queue reallocates a buffer of another size */
                AHardwareBuffer_release(b);
                r->buffers[i] = fake_ahb_new((uint32_t)r->buffer_width, (uint32_t)r->buffer_height, (uint32_t)r->format);
            }
            r->state[i] = QUEUED;
            return r->buffers[i];
        }
    }
    return NULL;
}
API EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface surface)
{
    (void)d;
    fake_surface *s = surface;
    if (!fake_window_queue(&s->reader->window))
        return EGL_FALSE;
    fake_state.swaps++;
    return EGL_TRUE;
}
API EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface surface, EGLint attribute, EGLint *value)
{
    (void)d;
    fake_surface *s = surface;
    if (attribute == EGL_WIDTH)
        *value = s->reader->buffer_width;
    else if (attribute == EGL_HEIGHT)
        *value = s->reader->buffer_height;
    else
        return EGL_FALSE;
    return EGL_TRUE;
}
API EGLBoolean eglSignalSyncKHR(EGLDisplay d, EGLSyncKHR s, EGLenum mode)
{
    (void)mode;
    return d == k_display && s != EGL_NO_SYNC_KHR;
}
/* Android's libEGL: the EGL 1.5 entry point, even on a 1.4 driver. */
API EGLBoolean eglWaitSync(EGLDisplay d, EGLSync s, EGLint flags)
{
    (void)flags;
    return d == k_display && s != EGL_NO_SYNC;
}

API EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c)
{
    (void)d, (void)c;
    return EGL_TRUE;
}
/* Images of AHardwareBuffers are distinct; any other is 0x42. */
API EGLImageKHR eglCreateImageKHR(EGLDisplay d, EGLContext c, EGLenum target, EGLClientBuffer b, const EGLint *a)
{
    (void)d, (void)c, (void)a;
    fake_state.create_image_target = (int)target;
    if (target != EGL_NATIVE_BUFFER_ANDROID)
        return (EGLImageKHR)0x42;
    if (!b)
        return EGL_NO_IMAGE_KHR;
    fake_state.native_images++;
    return (EGLImageKHR)malloc(1);
}
API EGLBoolean eglDestroyImageKHR(EGLDisplay d, EGLImageKHR image)
{
    (void)d;
    if (image != (EGLImageKHR)0x42) {
        fake_state.images_destroyed++;
        free(image);
    }
    return EGL_TRUE;
}
API EGLBoolean eglDestroyImage(EGLDisplay d, EGLImage image)
{
    return eglDestroyImageKHR(d, image);
}
API EGLClientBuffer eglGetNativeClientBufferANDROID(const struct AHardwareBuffer *b)
{
    return (EGLClientBuffer)b;
}
/* EGL_SYNC_NATIVE_FENCE_ANDROID syncs: a pipe each, readable once "the GPU"
 * passed it; its fd exists once flushed, as on Android. */
#define SYNCS 16
static EGLSyncKHR const k_plain_sync = (EGLSyncKHR)0x5ec; /* any EGL_SYNC_FENCE_KHR sync */
static struct fake_sync {
    int used, flushed, signalled, pipe[2];
    uint64_t time;
    ino_t ino; /* the pipe's: its fds outlive the sync, as a sync file does */
} g_syncs[SYNCS];
static void sync_signal(struct fake_sync *s)
{
    struct timespec t;
    if (s->signalled)
        return;
    clock_gettime(CLOCK_MONOTONIC, &t);
    s->time = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
    s->signalled = 1;
    if (write(s->pipe[1], "x", 1) != 1)
        s->signalled = 0;
}
API void fake_gpu_release(void)
{
    for (int i = 0; i < SYNCS; i++)
        if (g_syncs[i].used)
            sync_signal(&g_syncs[i]);
}
API EGLSyncKHR eglCreateSyncKHR(EGLDisplay d, EGLenum type, const EGLint *a)
{
    (void)a;
    if (d == k_display && type == EGL_SYNC_FENCE_KHR)
        return k_plain_sync;
    if (d != k_display || type != EGL_SYNC_NATIVE_FENCE_ANDROID)
        return EGL_NO_SYNC_KHR;
    for (int i = 0; i < SYNCS; i++) {
        struct fake_sync *s = &g_syncs[i];
        if (s->used || pipe(s->pipe) != 0)
            continue;
        struct stat st;
        s->used = 1;
        s->flushed = s->signalled = 0;
        s->ino = fstat(s->pipe[0], &st) == 0 ? st.st_ino : 0;
        fake_state.fences_created++;
        if (!fake_state.gpu_hold && !fake_state.gpu_ms)
            sync_signal(s);
        return (EGLSyncKHR)s;
    }
    return EGL_NO_SYNC_KHR;
}
static struct fake_sync *fake_sync(EGLSyncKHR s)
{
    for (int i = 0; i < SYNCS; i++)
        if (g_syncs[i].used && (EGLSyncKHR)&g_syncs[i] == s)
            return &g_syncs[i];
    return NULL;
}
API EGLBoolean eglDestroySyncKHR(EGLDisplay d, EGLSyncKHR sync)
{
    struct fake_sync *s = fake_sync(sync);
    if (d == k_display && sync == k_plain_sync)
        return EGL_TRUE;
    if (d != k_display || !s)
        return EGL_FALSE;
    close(s->pipe[0]);
    close(s->pipe[1]);
    s->used = 0;
    fake_state.fences_destroyed++;
    return EGL_TRUE;
}
API EGLint eglDupNativeFenceFDANDROID(EGLDisplay d, EGLSyncKHR sync)
{
    (void)d;
    struct fake_sync *s = fake_sync(sync);
    if (!s)
        return 42; /* any other sync: a number to check the dispatch with */
    if (!s->flushed)
        return EGL_NO_NATIVE_FENCE_FD_ANDROID;
    if (fake_state.gpu_ms && !s->signalled)
        return fence_file(); /* signalled gpu_ms from now */
    fake_state.fence_files++;
    return dup(s->pipe[0]);
}
API EGLint eglClientWaitSyncKHR(EGLDisplay d, EGLSyncKHR s, EGLint flags, EGLTimeKHR timeout)
{
    (void)d, (void)timeout;
    if (s != k_plain_sync)
        return EGL_FALSE;
    fake_state.fence_waits++;
    fake_state.fence_waits_flushed += (flags & EGL_SYNC_FLUSH_COMMANDS_BIT_KHR) != 0;
    return EGL_CONDITION_SATISFIED_KHR;
}
API void glFlush(void)
{
    fake_state.flushes++;
    for (int i = 0; i < SYNCS; i++)
        g_syncs[i].flushed |= g_syncs[i].used;
}
/* The vendor's way to read a fence's signal time here (SYNC_IOC_FILE_INFO
 * needs a real sync file); FAKE_FENCE_NO_TIME: fences tell no time. */
API int cham_test_fence_time(int fd, uint64_t *ns)
{
    struct stat st;
    if (getenv("FAKE_FENCE_NO_TIME") || fstat(fd, &st) != 0)
        return -1;
    for (int i = 0; i < SYNCS; i++) {
        if (g_syncs[i].ino && g_syncs[i].ino == st.st_ino) {
            if (!g_syncs[i].signalled)
                return 0;
            *ns = g_syncs[i].time;
            return 1;
        }
    }
    return -1;
}
/* The driver's own timer query: Mali r32p1's raw 13 MHz ticks, and a
 * result for any query. */
API void glGetInteger64v(GLenum pname, GLint64 *v)
{
    *v = pname == 0x8E28 /* GL_TIMESTAMP_EXT */ ? 761072096440 : 0;
}
static void fake_query_result(GLuint id, GLenum pname, GLuint64 *v)
{
    (void)id, (void)pname;
    *v = 1234;
}
API void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *v)
{
    (void)id, (void)pname;
    *v = 1234;
}
API void glDeleteQueries(GLsizei n, const GLuint *ids)
{
    (void)ids;
    fake_state.query_deletes += n;
}
static void fake_query_counter(GLuint id, GLenum target)
{
    (void)id, (void)target;
    fake_state.query_counter_calls++;
}
static void fake_image_target_texture(GLenum target, void *image);
API void (*eglGetProcAddress(const char *name))(void)
{
    if (strcmp(name, "glQueryCounterEXT") == 0)
        return (void (*)(void))fake_query_counter;
    if (strcmp(name, "glGetQueryObjectui64vEXT") == 0)
        return (void (*)(void))fake_query_result;
    if (strcmp(name, "glEGLImageTargetTexture2DOES") == 0)
        return (void (*)(void))fake_image_target_texture;
    return NULL;
}

/* ---- GLES ---- */

API const GLubyte *glGetString(GLenum name)
{
    if (name == GL_EXTENSIONS)
        return (const GLubyte *)"GL_OES_a GL_EXT_texture_format_BGRA8888 GL_EXT_disjoint_timer_query GL_OES_b";
    if (name == GL_VERSION)
        return (const GLubyte *)"OpenGL ES 3.2 fake";
    return (const GLubyte *)"fake";
}
/* Textures and framebuffers, enough for a texture to be given an EGLImage's
 * storage: 2D textures on one unit, the read framebuffer binding. */
#define NAMES 32
static struct {
    int exists, target, width, height, internal, immutable, level1;
    void *image;
} g_tex[NAMES];
static GLuint g_tex_2d, g_read_fb, g_fb_tex[NAMES];
static GLenum g_error;

API GLenum glGetError(void)
{
    GLenum e = g_error;
    g_error = GL_NO_ERROR;
    return e;
}
API void glGetIntegerv(GLenum pname, GLint *v)
{
    *v = pname == GL_TEXTURE_BINDING_2D ? (GLint)g_tex_2d : pname == GL_READ_FRAMEBUFFER_BINDING ? (GLint)g_read_fb : 0;
}
API void glGenTextures(GLsizei n, GLuint *names)
{
    for (GLsizei i = 0; i < n; i++) {
        GLuint t = 1;
        while (t < NAMES - 1 && g_tex[t].exists)
            t++;
        memset(&g_tex[t], 0, sizeof g_tex[t]);
        g_tex[t].exists = 1;
        names[i] = t;
    }
}
API void glDeleteTextures(GLsizei n, const GLuint *names)
{
    for (GLsizei i = 0; i < n; i++)
        if (names[i] < NAMES)
            g_tex[names[i]].exists = 0;
}
API GLboolean glIsTexture(GLuint t)
{
    return t < NAMES && g_tex[t].exists && g_tex[t].target;
}
API void glBindTexture(GLenum target, GLuint t)
{
    if (t >= NAMES || (t && g_tex[t].target && (GLenum)g_tex[t].target != target)) {
        g_error = GL_INVALID_OPERATION;
        return;
    }
    if (t)
        g_tex[t].target = (int)target;
    if (target == GL_TEXTURE_2D)
        g_tex_2d = t;
}
API void glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *p)
{
    (void)t, (void)b, (void)f, (void)ty, (void)p;
    fake_state.tex_uploads++;
    if (!g_tex_2d)
        return;
    if (g_tex[g_tex_2d].immutable) {
        g_error = GL_INVALID_OPERATION;
        return;
    }
    if (l == 1)
        g_tex[g_tex_2d].level1 = w;
    else if (l == 0)
        g_tex[g_tex_2d].width = w, g_tex[g_tex_2d].height = h, g_tex[g_tex_2d].internal = i,
        g_tex[g_tex_2d].image = NULL;
}
API void glTexStorage2D(GLenum t, GLsizei levels, GLenum internal, GLsizei w, GLsizei h)
{
    (void)t, (void)levels;
    g_tex[g_tex_2d].width = w, g_tex[g_tex_2d].height = h, g_tex[g_tex_2d].internal = (int)internal;
    g_tex[g_tex_2d].immutable = 1;
}
API void glGetTexParameteriv(GLenum t, GLenum pname, GLint *v)
{
    (void)t;
    *v = pname == GL_TEXTURE_IMMUTABLE_FORMAT ? g_tex[g_tex_2d].immutable : 0;
}
API void glGetTexLevelParameteriv(GLenum t, GLint level, GLenum pname, GLint *v)
{
    (void)t;
    *v = level == 1 ? (pname == GL_TEXTURE_WIDTH ? g_tex[g_tex_2d].level1 : 0)
         : pname == GL_TEXTURE_WIDTH           ? g_tex[g_tex_2d].width
         : pname == GL_TEXTURE_HEIGHT          ? g_tex[g_tex_2d].height
         : pname == GL_TEXTURE_INTERNAL_FORMAT ? g_tex[g_tex_2d].internal
                                               : 0;
}
API void glTexParameteri(GLenum t, GLenum pname, GLint v)
{
    if (t == GL_TEXTURE_2D && pname == GL_TEXTURE_SWIZZLE_A && v == GL_ONE)
        fake_state.swizzle_a_one = g_tex_2d;
}
API void glBindVertexArray(GLuint array) { fake_state.vao_bound = array; }
static void fake_image_target_texture(GLenum target, void *image)
{
    (void)target;
    if (!g_tex_2d || g_tex[g_tex_2d].immutable) {
        g_error = GL_INVALID_OPERATION;
        return;
    }
    g_tex[g_tex_2d].image = image;
}
API void glGenFramebuffers(GLsizei n, GLuint *names)
{
    for (GLsizei i = 0; i < n; i++)
        names[i] = 7 + (GLuint)i;
}
API void glDeleteFramebuffers(GLsizei n, const GLuint *names) { (void)n, (void)names; }
API void glBindFramebuffer(GLenum t, GLuint f)
{
    if (t == GL_READ_FRAMEBUFFER || t == GL_FRAMEBUFFER)
        g_read_fb = f;
}
API void glFramebufferTexture2D(GLenum t, GLenum attachment, GLenum textarget, GLuint tex, GLint level)
{
    (void)t, (void)attachment, (void)textarget, (void)level;
    if (g_read_fb < NAMES)
        g_fb_tex[g_read_fb] = tex;
}
API GLenum glCheckFramebufferStatus(GLenum t)
{
    (void)t;
    return g_read_fb && g_fb_tex[g_read_fb] ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
}
API void glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
    (void)t, (void)l, (void)xo, (void)yo, (void)x, (void)y, (void)w, (void)h;
    if (!g_read_fb || !g_tex[g_tex_2d].image) {
        g_error = GL_INVALID_OPERATION;
        return;
    }
    fake_state.copies++;
    fake_state.copy_source = g_fb_tex[g_read_fb];
}
API void glFinish(void) { fake_state.finishes++; }
API void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *src, const GLint *len)
{
    (void)shader, (void)len;
    fake_state.shader[0] = 0;
    for (GLsizei i = 0; i < count; i++)
        strncat(fake_state.shader, src[i], sizeof fake_state.shader - strlen(fake_state.shader) - 1);
}
API void glClear(GLbitfield m) { (void)m; }
