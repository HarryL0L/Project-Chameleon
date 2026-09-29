/*
 * chameleon_demo - Termux-side producer that renders with the vendor GPU
 * (through /system/lib64/libEGL.so) into AHardwareBuffers and streams them to
 * the Chameleon presenter app with zero copies. It exercises the path the
 * KWin shim uses: allocate -> render -> fence -> PRESENT -> wait for
 * FRAME_DONE ("page flip") and RELEASE (the shim then asks for copy mode).
 *
 * Prints frames per second and the submit -> on-screen (FRAME_DONE) latency.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "cham_io.h"
#include "chameleon_proto.h"

/* ---- minimal EGL / GLES / AHB declarations (see probe/ahb_probe.c) ---- */
typedef void *EGLDisplay, *EGLContext, *EGLConfig, *EGLSurface, *EGLImageKHR, *EGLClientBuffer, *EGLSyncKHR;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean, EGLenum, GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;

#define EGL_NONE 0x3038
#define EGL_TRUE 1
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#define GL_TEXTURE_2D 0x0DE1
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RENDERER 0x1F01

typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

static struct {
    EGLDisplay (*GetDisplay)(void *);
    EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*BindAPI)(EGLenum);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLint (*GetError)(void);
    void *(*GetProcAddress)(const char *);
    EGLClientBuffer (*GetNativeClientBufferANDROID)(const AHardwareBuffer *);
    EGLImageKHR (*CreateImageKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLBoolean (*DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    EGLSyncKHR (*CreateSyncKHR)(EGLDisplay, EGLenum, const EGLint *);
    EGLBoolean (*DestroySyncKHR)(EGLDisplay, EGLSyncKHR);
    EGLint (*WaitSyncKHR)(EGLDisplay, EGLSyncKHR, EGLint);
    EGLint (*DupNativeFenceFDANDROID)(EGLDisplay, EGLSyncKHR);
} egl;

static struct {
    const unsigned char *(*GetString)(GLenum);
    void (*GenTextures)(GLsizei, GLuint *);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(GLbitfield);
    void (*Flush)(void);
    void (*EGLImageTargetTexture2DOES)(GLenum, void *);
} gl;

static struct {
    int (*allocate)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
    void (*release)(AHardwareBuffer *);
    int (*sendHandleToUnixSocket)(const AHardwareBuffer *, int);
} ahb;

#define AHB_FORMAT_R8G8B8A8_UNORM 1
#define AHB_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)
#define AHB_USAGE_GPU_COLOR_OUTPUT (1ULL << 9)
#define AHB_USAGE_COMPOSER_OVERLAY (1ULL << 11)

static void die(const char *what)
{
    fprintf(stderr, "chameleon_demo: %s\n", what);
    exit(1);
}

static void *sym(void *lib, const char *name)
{
    void *p = dlsym(lib, name);
    if (!p) {
        fprintf(stderr, "chameleon_demo: missing symbol %s\n", name);
        exit(1);
    }
    return p;
}

#define LOAD(tbl, field, lib, name) (*(void **)&(tbl).field = sym(lib, name))
#define PROC(tbl, field, name)                                  \
    do {                                                        \
        *(void **)&(tbl).field = egl.GetProcAddress(name);      \
        if (!(tbl).field)                                       \
            die("missing EGL/GL extension entry point " name);  \
    } while (0)

static EGLDisplay g_dpy;

static void init_gpu(void)
{
    const char *dir = "/system/lib64";
    char path[128];
    snprintf(path, sizeof path, "%s/libEGL.so", dir);
    void *legl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    snprintf(path, sizeof path, "%s/libGLESv2.so", dir);
    void *lgles = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    snprintf(path, sizeof path, "%s/libnativewindow.so", dir);
    void *lnw = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!legl || !lgles || !lnw)
        die(dlerror());

    LOAD(egl, GetDisplay, legl, "eglGetDisplay");
    LOAD(egl, Initialize, legl, "eglInitialize");
    LOAD(egl, BindAPI, legl, "eglBindAPI");
    LOAD(egl, CreateContext, legl, "eglCreateContext");
    LOAD(egl, MakeCurrent, legl, "eglMakeCurrent");
    LOAD(egl, GetError, legl, "eglGetError");
    LOAD(egl, GetProcAddress, legl, "eglGetProcAddress");
    LOAD(gl, GetString, lgles, "glGetString");
    LOAD(gl, GenTextures, lgles, "glGenTextures");
    LOAD(gl, DeleteTextures, lgles, "glDeleteTextures");
    LOAD(gl, BindTexture, lgles, "glBindTexture");
    LOAD(gl, GenFramebuffers, lgles, "glGenFramebuffers");
    LOAD(gl, DeleteFramebuffers, lgles, "glDeleteFramebuffers");
    LOAD(gl, BindFramebuffer, lgles, "glBindFramebuffer");
    LOAD(gl, FramebufferTexture2D, lgles, "glFramebufferTexture2D");
    LOAD(gl, CheckFramebufferStatus, lgles, "glCheckFramebufferStatus");
    LOAD(gl, Viewport, lgles, "glViewport");
    LOAD(gl, Enable, lgles, "glEnable");
    LOAD(gl, Disable, lgles, "glDisable");
    LOAD(gl, Scissor, lgles, "glScissor");
    LOAD(gl, ClearColor, lgles, "glClearColor");
    LOAD(gl, Clear, lgles, "glClear");
    LOAD(gl, Flush, lgles, "glFlush");
    LOAD(ahb, allocate, lnw, "AHardwareBuffer_allocate");
    LOAD(ahb, release, lnw, "AHardwareBuffer_release");
    LOAD(ahb, sendHandleToUnixSocket, lnw, "AHardwareBuffer_sendHandleToUnixSocket");

    PROC(egl, GetNativeClientBufferANDROID, "eglGetNativeClientBufferANDROID");
    PROC(egl, CreateImageKHR, "eglCreateImageKHR");
    PROC(egl, DestroyImageKHR, "eglDestroyImageKHR");
    PROC(egl, CreateSyncKHR, "eglCreateSyncKHR");
    PROC(egl, DestroySyncKHR, "eglDestroySyncKHR");
    PROC(egl, WaitSyncKHR, "eglWaitSyncKHR");
    PROC(egl, DupNativeFenceFDANDROID, "eglDupNativeFenceFDANDROID");
    PROC(gl, EGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES");

    g_dpy = egl.GetDisplay((void *)0);
    if (!g_dpy || egl.Initialize(g_dpy, NULL, NULL) != EGL_TRUE)
        die("eglInitialize failed");
    egl.BindAPI(EGL_OPENGL_ES_API);
    const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = egl.CreateContext(g_dpy, (EGLConfig)0 /* EGL_NO_CONFIG_KHR */, (EGLContext)0, attribs);
    if (!ctx || egl.MakeCurrent(g_dpy, (EGLSurface)0, (EGLSurface)0, ctx) != EGL_TRUE)
        die("cannot make a surfaceless GLES 3 context current");
    printf("GPU: %s\n", gl.GetString(GL_RENDERER));
}

/* ---- buffers ------------------------------------------------------------ */

#define NUM_BUFFERS 3
/* Like KMS: one pending flip. More only queues frames SurfaceFlinger drops.
 * Override with $CHAMELEON_IN_FLIGHT to experiment. */
static int g_max_in_flight = 1;

struct buffer {
    AHardwareBuffer *ahb;
    EGLImageKHR image;
    GLuint tex, fbo;
    int busy;          /* owned by the presenter until RELEASE */
    int release_fence; /* must signal before we render into it again */
};

static struct buffer g_bufs[NUM_BUFFERS];
static uint32_t g_width, g_height;
static int g_sock = -1;

static void destroy_buffers(void)
{
    for (int i = 0; i < NUM_BUFFERS; i++) {
        struct buffer *b = &g_bufs[i];
        if (!b->ahb)
            continue;
        struct cham_msg m = {CHAM_BUFFER_REMOVE, (uint32_t)i, 0, 0};
        cham_send(g_sock, &m, -1);
        gl.DeleteFramebuffers(1, &b->fbo);
        gl.DeleteTextures(1, &b->tex);
        egl.DestroyImageKHR(g_dpy, b->image);
        ahb.release(b->ahb);
        if (b->release_fence >= 0)
            close(b->release_fence);
        memset(b, 0, sizeof *b);
        b->release_fence = -1;
    }
}

static void create_buffers(uint32_t w, uint32_t h)
{
    for (int i = 0; i < NUM_BUFFERS; i++) {
        struct buffer *b = &g_bufs[i];
        AHardwareBuffer_Desc d = {.width = w, .height = h, .layers = 1,
                                  .format = AHB_FORMAT_R8G8B8A8_UNORM,
                                  .usage = AHB_USAGE_GPU_SAMPLED_IMAGE | AHB_USAGE_GPU_COLOR_OUTPUT |
                                           AHB_USAGE_COMPOSER_OVERLAY};
        if (ahb.allocate(&d, &b->ahb) != 0)
            die("AHardwareBuffer_allocate failed");
        const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        b->image = egl.CreateImageKHR(g_dpy, (EGLContext)0, EGL_NATIVE_BUFFER_ANDROID,
                                      egl.GetNativeClientBufferANDROID(b->ahb), attribs);
        if (!b->image)
            die("eglCreateImageKHR failed");
        gl.GenTextures(1, &b->tex);
        gl.BindTexture(GL_TEXTURE_2D, b->tex);
        gl.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, b->image);
        gl.GenFramebuffers(1, &b->fbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, b->fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->tex, 0);
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            die("AHB framebuffer incomplete");
        b->busy = 0;
        b->release_fence = -1;

        struct cham_msg m = {CHAM_BUFFER_ADD, (uint32_t)i, 0, 0};
        if (cham_send(g_sock, &m, -1) < 0 || ahb.sendHandleToUnixSocket(b->ahb, g_sock) != 0)
            die("sending buffer to presenter failed");
    }
    printf("allocated %d x %ux%u buffers\n", NUM_BUFFERS, w, h);
}

/* ---- timing ------------------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define HISTORY 64
static uint64_t g_submit_ns[HISTORY]; /* indexed by frame % HISTORY */

/* ---- rendering ---------------------------------------------------------- */

static void hsv(float h, float *r, float *g, float *b)
{
    float x = 1.0f - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f);
    int i = (int)(h * 6.0f) % 6;
    float rr[6] = {1, x, 0, 0, x, 1}, gg[6] = {x, 1, 1, x, 0, 0}, bb[6] = {0, 0, x, 1, 1, x};
    *r = rr[i] * 0.35f;
    *g = gg[i] * 0.35f;
    *b = bb[i] * 0.35f;
}

static void render(struct buffer *b, uint64_t frame)
{
    if (b->release_fence >= 0) {
        /* GPU-side wait: the CPU never blocks on the presenter. */
        const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, b->release_fence, EGL_NONE};
        EGLSyncKHR sync = egl.CreateSyncKHR(g_dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
        if (sync) {
            egl.WaitSyncKHR(g_dpy, sync, 0);
            egl.DestroySyncKHR(g_dpy, sync); /* EGL owns and closes the fd */
        } else {
            close(b->release_fence);
        }
        b->release_fence = -1;
    }

    gl.BindFramebuffer(GL_FRAMEBUFFER, b->fbo);
    gl.Viewport(0, 0, (GLsizei)g_width, (GLsizei)g_height);
    float r, g, bl;
    hsv((float)(frame % 600) / 600.0f, &r, &g, &bl);
    gl.Disable(GL_SCISSOR_TEST);
    gl.ClearColor(r, g, bl, 1.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);

    /* A white box bouncing across the screen shows tearing/stutter at a glance. */
    GLsizei box = (GLsizei)(g_height < g_width ? g_height : g_width) / 6;
    uint32_t span_x = g_width - (uint32_t)box, span_y = g_height - (uint32_t)box;
    uint32_t px = (uint32_t)(frame * 9) % (2 * span_x), py = (uint32_t)(frame * 5) % (2 * span_y);
    GLint x = (GLint)(px < span_x ? px : 2 * span_x - px);
    GLint y = (GLint)(py < span_y ? py : 2 * span_y - py);
    gl.Enable(GL_SCISSOR_TEST);
    gl.Scissor(x, y, box, box);
    gl.ClearColor(1, 1, 1, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Disable(GL_SCISSOR_TEST);
}

static int present(int id, uint64_t frame)
{
    const EGLint attribs[] = {EGL_NONE};
    EGLSyncKHR sync = egl.CreateSyncKHR(g_dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    gl.Flush(); /* the native fence fd only exists once the sync is flushed */
    int fence = sync ? egl.DupNativeFenceFDANDROID(g_dpy, sync) : -1;
    if (sync)
        egl.DestroySyncKHR(g_dpy, sync);

    uint64_t t = now_ns();
    g_submit_ns[frame % HISTORY] = t;
    struct cham_msg m = {CHAM_PRESENT, (uint32_t)id, frame, t};
    int rc = cham_send(g_sock, &m, fence);
    if (fence >= 0)
        close(fence);
    return rc;
}

/* ---- main loop ---------------------------------------------------------- */

static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static int connect_presenter(void)
{
    const char *path = getenv("CHAMELEON_SOCKET");
    if (!path)
        path = CHAM_SOCKET_PATH;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
    if (s < 0 || connect(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
        fprintf(stderr, "chameleon_demo: cannot connect to %s: %s\n"
                        "  Is the Chameleon app installed and open?\n", path, strerror(errno));
        exit(1);
    }
    printf("connected to %s\n", path);
    return s;
}

int main(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < NUM_BUFFERS; i++)
        g_bufs[i].release_fence = -1;

    init_gpu();
    g_sock = connect_presenter();
    struct cham_msg hello = {CHAM_HELLO, 0, CHAM_PROTO_VERSION, 0};
    cham_send(g_sock, &hello, -1);

    const char *env = getenv("CHAMELEON_IN_FLIGHT");
    if (env && atoi(env) > 0)
        g_max_in_flight = atoi(env) < NUM_BUFFERS ? atoi(env) : NUM_BUFFERS - 1;

    /* shown: frames SurfaceFlinger latched on their own vsync.
     * dropped: frames superseded before being latched (same latch time as
     * the previous frame, or none). latency: submit -> latch, and
     * submit -> completion callback (includes the display's present). */
    uint64_t frame = 0, shown = 0, dropped = 0, latch_sum = 0, done_sum = 0, last_latch = 0;
    uint64_t stat_start = now_ns();
    int in_flight = 0;

    while (!g_quit) {
        int free_id = -1;
        for (int i = 0; i < NUM_BUFFERS && g_width; i++)
            if (g_bufs[i].ahb && !g_bufs[i].busy) {
                free_id = i;
                break;
            }
        int can_render = free_id >= 0 && in_flight < g_max_in_flight;

        struct pollfd pfd = {g_sock, POLLIN, 0};
        int n = poll(&pfd, 1, can_render ? 0 : 1000);
        if (n < 0 && errno != EINTR)
            break;
        if (n > 0) {
            struct cham_msg m;
            int fd;
            int rc = cham_recv(g_sock, &m, &fd);
            if (rc <= 0) {
                printf("presenter closed the connection\n");
                break;
            }
            switch (m.type) {
            case CHAM_CONFIG: {
                uint32_t w = CHAM_CONFIG_WIDTH(&m), h = CHAM_CONFIG_HEIGHT(&m);
                printf("presenter: %ux%u @ %.2f Hz%s\n", w, h, m.b / 1000.0, w ? "" : " (paused)");
                if (w != g_width || h != g_height) {
                    destroy_buffers();
                    g_width = w;
                    g_height = h;
                    in_flight = 0;
                    if (w && h)
                        create_buffers(w, h);
                }
                break;
            }
            case CHAM_RELEASE:
                if (m.id < NUM_BUFFERS) {
                    g_bufs[m.id].busy = 0; /* idempotent */
                    if (g_bufs[m.id].release_fence >= 0)
                        close(g_bufs[m.id].release_fence);
                    g_bufs[m.id].release_fence = fd;
                    fd = -1;
                }
                break;
            case CHAM_FRAME_DONE:
                if (in_flight > 0)
                    in_flight--;
                {
                    uint64_t submit = g_submit_ns[m.a % HISTORY];
                    if (m.b == 0 || m.b == last_latch) {
                        dropped++;
                    } else {
                        shown++;
                        latch_sum += m.b > submit ? m.b - submit : 0;
                        done_sum += now_ns() - submit;
                        last_latch = m.b;
                    }
                }
                break;
            }
            if (fd >= 0)
                close(fd);
            continue; /* drain messages before rendering */
        }

        if (can_render) {
            render(&g_bufs[free_id], frame);
            if (present(free_id, frame) < 0) {
                printf("send failed; presenter gone\n");
                break;
            }
            g_bufs[free_id].busy = 1;
            in_flight++;
            frame++;
        }

        uint64_t t = now_ns();
        if (t - stat_start >= 1000000000ull) {
            double secs = (t - stat_start) / 1e9;
            if (shown || dropped)
                printf("%6.1f fps shown  %3llu dropped   submit->latch %5.2f ms   submit->done %5.2f ms\n",
                       shown / secs, (unsigned long long)dropped,
                       shown ? latch_sum / 1e6 / (double)shown : 0.0,
                       shown ? done_sum / 1e6 / (double)shown : 0.0);
            shown = dropped = latch_sum = done_sum = 0;
            stat_start = t;
        }
    }

    destroy_buffers();
    close(g_sock);
    return 0;
}
