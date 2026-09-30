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
#include <GLES2/gl2.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define API __attribute__((visibility("default")))

API struct {
    int robust_attrib_seen, client_version;
    char shader[512];
    int tex_uploads;
    int create_image_target;
    int query_counter_calls;
    int swaps, swap_interval_calls, last_swap_interval;
    void *current_draw;
} fake_state;

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
        *minor = 5;
    return d == k_display;
}
API EGLint eglGetError(void) { return EGL_SUCCESS; }
API EGLBoolean eglBindAPI(EGLenum api) { return api == EGL_OPENGL_ES_API; }
API const char *eglQueryString(EGLDisplay d, EGLint name)
{
    (void)d;
    switch (name) {
    case EGL_EXTENSIONS:
        return "EGL_KHR_image_base EGL_ANDROID_native_fence_sync EGL_KHR_no_config_context "
               "EGL_KHR_surfaceless_context";
    case EGL_VENDOR:
        return "fake Android";
    case EGL_VERSION:
        return "1.5 fake";
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
API EGLBoolean eglSwapInterval(EGLDisplay d, EGLint interval)
{
    (void)d;
    fake_state.swap_interval_calls++;
    fake_state.last_swap_interval = interval;
    return EGL_TRUE;
}
API EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *a, EGLConfig *configs, EGLint size, EGLint *n)
{
    (void)d, (void)a;
    if (configs && size > 0)
        configs[0] = (EGLConfig)0x77;
    *n = 1;
    return EGL_TRUE;
}
API EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attribute, EGLint *value)
{
    (void)d, (void)c;
    *value = attribute == EGL_NATIVE_VISUAL_ID ? 1 /* RGBA_8888 */ : 8;
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
    AImageReader *r = calloc(1, sizeof *r);
    r->width = w;
    r->height = h;
    r->format = format;
    r->max_images = max_images;
    r->window.magic = 0x5f776e64;
    r->window.reader = r;
    for (int i = 0; i < READER_SLOTS; i++)
        r->buffers[i] = fake_ahb_new((uint32_t)w, (uint32_t)h, (uint32_t)format);
    *out = r;
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
            *fence = -1;
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
API EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface surface)
{
    (void)d;
    fake_surface *s = surface;
    for (int i = 0; i < READER_SLOTS; i++) {
        if (s->reader->state[i] == FREE) {
            s->reader->state[i] = QUEUED;
            fake_state.swaps++;
            return EGL_TRUE;
        }
    }
    return EGL_FALSE; /* a real driver would block here */
}
API EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface surface, EGLint attribute, EGLint *value)
{
    (void)d;
    fake_surface *s = surface;
    if (attribute == EGL_WIDTH)
        *value = s->reader->width;
    else if (attribute == EGL_HEIGHT)
        *value = s->reader->height;
    else
        return EGL_FALSE;
    return EGL_TRUE;
}
API EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c)
{
    (void)d, (void)c;
    return EGL_TRUE;
}
API EGLImageKHR eglCreateImageKHR(EGLDisplay d, EGLContext c, EGLenum target, EGLClientBuffer b, const EGLint *a)
{
    (void)d, (void)c, (void)b, (void)a;
    fake_state.create_image_target = (int)target;
    return (EGLImageKHR)0x42;
}
API EGLint eglDupNativeFenceFDANDROID(EGLDisplay d, EGLSyncKHR s)
{
    (void)d, (void)s;
    return 42;
}
static void fake_query_counter(GLuint id, GLenum target)
{
    (void)id, (void)target;
    fake_state.query_counter_calls++;
}
API void (*eglGetProcAddress(const char *name))(void)
{
    if (strcmp(name, "glQueryCounterEXT") == 0)
        return (void (*)(void))fake_query_counter;
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
API void glGetIntegerv(GLenum pname, GLint *v)
{
    (void)pname;
    *v = 0;
}
API void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *src, const GLint *len)
{
    (void)shader, (void)len;
    fake_state.shader[0] = 0;
    for (GLsizei i = 0; i < count; i++)
        strncat(fake_state.shader, src[i], sizeof fake_state.shader - strlen(fake_state.shader) - 1);
}
API void glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *p)
{
    (void)t, (void)l, (void)i, (void)w, (void)h, (void)b, (void)f, (void)ty, (void)p;
    fake_state.tex_uploads++;
}
API void glBindFramebuffer(GLenum t, GLuint f) { (void)t, (void)f; }
API void glClear(GLbitfield m) { (void)m; }
