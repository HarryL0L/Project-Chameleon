// Chameleon presenter: receives AHardwareBuffers + acquire fences from one
// Termux producer over a SOCK_SEQPACKET socket and shows them on an
// ASurfaceControl child of the activity's SurfaceView. No pixel copies:
// SurfaceFlinger (and usually an HWC overlay) reads the producer's buffer.
//
// Copy mode (CHAM_HELLO_COPY, used by the KWin shim): the producer reuses a
// buffer as soon as the next frame is latched, KMS-style, while
// SurfaceFlinger still scans it out for one more vsync. Each frame is then
// blitted on the GPU into a small presenter-owned pool and the pool buffer is
// shown instead.
//
// Protocol: common/chameleon_proto.h.

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>

#include <atomic>
#include <mutex>
#include <string>

#include "cham_io.h"
#include "chameleon_proto.h"

#define TAG "Chameleon"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

constexpr uint32_t kMaxBuffers = 64;

constexpr int kPoolSize = 4;

struct Buffer {
    AHardwareBuffer *ahb = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    // Copy mode only: the producer buffer as a blit source.
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint tex = 0, fbo = 0;
};

// Copy mode: presenter-owned buffers that SurfaceFlinger actually shows.
struct PoolBuf {
    AHardwareBuffer *ahb = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint tex = 0, fbo = 0;
    int32_t width = 0, height = 0;
    bool held = false;      // on screen or queued in SurfaceFlinger
    int release_fence = -1; // must signal before we draw into it again
    uint64_t serial = 0;    // guards against callbacks for a reallocated slot
};

// All state below is guarded by g_lock. SurfaceFlinger completion callbacks
// arrive on binder threads, surface events on the UI thread, protocol
// messages on the server thread.
std::mutex g_lock;
ASurfaceControl *g_sc = nullptr;
int32_t g_width = 0, g_height = 0, g_refresh_mhz = 60000;
int g_client = -1;
uint64_t g_client_gen = 0;  // bumped per connection; stale callbacks are dropped
Buffer g_buffers[kMaxBuffers];
int64_t g_displayed = -1;   // buffer id currently latched on g_sc
float g_frame_rate_vote = 0; // Hz; applied to g_sc on the next transaction
bool g_vote_dirty = false;
bool g_copy_mode = false;    // negotiated per connection via HELLO
PoolBuf g_pool[kPoolSize];
int64_t g_pool_displayed = -1;
uint64_t g_pool_serial = 0;

// API 30; resolved at runtime because minSdk is 29.
using SetFrameRateFn = void (*)(ASurfaceTransaction *, ASurfaceControl *, float, int8_t);
SetFrameRateFn set_frame_rate()
{
    static SetFrameRateFn fn = [] {
        void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
        return lib ? (SetFrameRateFn)dlsym(lib, "ASurfaceTransaction_setFrameRate") : nullptr;
    }();
    return fn;
}

// API 31: fires when SurfaceFlinger latches the transaction - the analogue of
// a KMS page-flip event. OnComplete only fires after the frame was presented
// (one vsync later), which halves the frame rate of a one-flip-in-flight
// producer.
using OnCommitFn = void (*)(void *context, ASurfaceTransactionStats *stats);
using SetOnCommitFn = void (*)(ASurfaceTransaction *, void *, OnCommitFn);
SetOnCommitFn set_on_commit()
{
    static SetOnCommitFn fn = [] {
        void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
        return lib ? (SetOnCommitFn)dlsym(lib, "ASurfaceTransaction_setOnCommit") : nullptr;
    }();
    return fn;
}

void send_locked(uint32_t type, uint32_t id, uint64_t a, uint64_t b, int fd = -1)
{
    if (g_client < 0)
        return;
    cham_msg msg{type, id, a, b};
    int rc = cham_send(g_client, &msg, fd);
    if (rc < 0)
        LOGE("send type %u failed: %s", type, strerror(-rc));
}

void send_config_locked()
{
    uint64_t size = (uint64_t)(uint32_t)g_width | ((uint64_t)(uint32_t)g_height << 32);
    if (!g_sc)
        size = 0;
    send_locked(CHAM_CONFIG, 0, size, (uint64_t)g_refresh_mhz);
}

struct FrameCtx {
    uint64_t gen;
    ASurfaceControl *sc;  // compared by address only, never dereferenced
    uint32_t id;
    int64_t prev;
    uint64_t frame;
    bool frame_done_on_commit;
    std::atomic<int> refs;  // one per registered callback
    bool copy = false;       // id/prev are pool indices
    uint64_t serial = 0;     // pool serial of `prev`
};

void unref(FrameCtx *ctx)
{
    if (ctx->refs.fetch_sub(1) == 1)
        delete ctx;
}

void on_commit(void *context, ASurfaceTransactionStats *stats)
{
    auto *ctx = static_cast<FrameCtx *>(context);
    int64_t latch = ASurfaceTransactionStats_getLatchTime(stats);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (ctx->gen == g_client_gen)
            send_locked(CHAM_FRAME_DONE, 0, ctx->frame, (uint64_t)latch);
    }
    unref(ctx);
}

void on_complete(void *context, ASurfaceTransactionStats *stats)
{
    auto *ctx = static_cast<FrameCtx *>(context);

    int release_fence = -1;
    ASurfaceControl **controls = nullptr;
    size_t count = 0;
    ASurfaceTransactionStats_getASurfaceControls(stats, &controls, &count);
    for (size_t i = 0; i < count; i++) {
        if (controls[i] == ctx->sc) {
            release_fence = ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, controls[i]);
            break;
        }
    }
    ASurfaceTransactionStats_releaseASurfaceControls(controls);
    int64_t latch = ASurfaceTransactionStats_getLatchTime(stats);

    if (ctx->copy) {
        std::lock_guard<std::mutex> lock(g_lock);
        // The pool buffer this frame replaced is ours again once the fence signals.
        if (ctx->prev >= 0 && ctx->prev != ctx->id && g_pool[ctx->prev].serial == ctx->serial) {
            PoolBuf &p = g_pool[ctx->prev];
            p.held = false;
            if (p.release_fence >= 0)
                close(p.release_fence);
            p.release_fence = release_fence;
            release_fence = -1;
        }
        if (ctx->gen == g_client_gen && !ctx->frame_done_on_commit)
            send_locked(CHAM_FRAME_DONE, 0, ctx->frame, (uint64_t)latch);
    } else {
        std::lock_guard<std::mutex> lock(g_lock);
        if (ctx->gen == g_client_gen) {
            // The buffer this frame replaced is free once release_fence signals.
            if (ctx->prev >= 0 && ctx->prev != ctx->id)
                send_locked(CHAM_RELEASE, (uint32_t)ctx->prev, 0, 0, release_fence);
            if (!ctx->frame_done_on_commit)
                send_locked(CHAM_FRAME_DONE, 0, ctx->frame, (uint64_t)latch);
        }
    }
    if (release_fence >= 0)
        close(release_fence);
    unref(ctx);
}

// Activity counters, logged every 5 s (logcat -s Chameleon).
struct {
    unsigned presents, shown, copied, skipped;
    int64_t since;
    bool sample;          // read back the next copied frame's centre pixel
    uint8_t pixel[4];     // ...last result
    bool have_pixel;
} g_stats;

int64_t mono_ns()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void stats_log_locked()
{
    int64_t now = mono_ns();
    if (!g_stats.since)
        g_stats.since = now;
    if (now - g_stats.since < 5000000000)
        return;
    char pixel[48] = "";
    if (g_stats.have_pixel)
        snprintf(pixel, sizeof pixel, "; centre pixel %u,%u,%u", g_stats.pixel[0], g_stats.pixel[1], g_stats.pixel[2]);
    LOGI("last 5 s: %u presents, %u shown (%u via copy), %u skipped (no surface/buffer or copy failed); surface %dx%d%s",
         g_stats.presents, g_stats.shown, g_stats.copied, g_stats.skipped, g_width, g_height, pixel);
    g_stats = {};
    g_stats.since = now;
    g_stats.sample = true;
}

// ---- copy mode (runs on the server thread, which owns the GL context) ----

EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLContext g_ctx = EGL_NO_CONTEXT;
PFNEGLCREATESYNCKHRPROC p_eglCreateSyncKHR;
PFNEGLDESTROYSYNCKHRPROC p_eglDestroySyncKHR;
PFNEGLWAITSYNCKHRPROC p_eglWaitSyncKHR;
PFNEGLDUPNATIVEFENCEFDANDROIDPROC p_eglDupNativeFenceFDANDROID;

bool gl_init()
{
    if (g_ctx != EGL_NO_CONTEXT)
        return true;
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g_dpy, nullptr, nullptr))
        return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g_ctx = eglCreateContext(g_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
    if (g_ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_ctx)) {
        LOGE("copy mode: cannot create a GLES 3 context (0x%x)", eglGetError());
        g_ctx = EGL_NO_CONTEXT;
        return false;
    }
    p_eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    p_eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    p_eglWaitSyncKHR = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
    p_eglDupNativeFenceFDANDROID =
        (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    LOGI("copy mode: GL %s", (const char *)glGetString(GL_RENDERER));
    return true;
}

// Makes the GPU (not the CPU) wait for a sync_file. Takes ownership of fd.
void gpu_wait(int fd)
{
    if (fd < 0)
        return;
    const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
    EGLSyncKHR sync = p_eglCreateSyncKHR(g_dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (sync == EGL_NO_SYNC_KHR) {
        close(fd);
        return;
    }
    p_eglWaitSyncKHR(g_dpy, sync, 0);
    p_eglDestroySyncKHR(g_dpy, sync);  // EGL owns and closes fd
}

int gpu_fence()
{
    const EGLint attribs[] = {EGL_NONE};
    EGLSyncKHR sync = p_eglCreateSyncKHR(g_dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    glFlush();
    int fd = sync != EGL_NO_SYNC_KHR ? p_eglDupNativeFenceFDANDROID(g_dpy, sync) : -1;
    if (sync != EGL_NO_SYNC_KHR)
        p_eglDestroySyncKHR(g_dpy, sync);
    if (fd < 0)
        glFinish();  // no fence available: make the copy synchronous instead
    return fd;
}

bool wrap_ahb(AHardwareBuffer *ahb, EGLImageKHR *image, GLuint *tex, GLuint *fbo)
{
    const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    *image = eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                               eglGetNativeClientBufferANDROID(ahb), attribs);
    if (*image == EGL_NO_IMAGE_KHR)
        return false;
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)*image);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

void unwrap(EGLImageKHR *image, GLuint *tex, GLuint *fbo)
{
    if (g_ctx == EGL_NO_CONTEXT)
        return;
    if (*fbo)
        glDeleteFramebuffers(1, fbo);
    if (*tex)
        glDeleteTextures(1, tex);
    if (*image != EGL_NO_IMAGE_KHR)
        eglDestroyImageKHR(g_dpy, *image);
    *image = EGL_NO_IMAGE_KHR;
    *tex = *fbo = 0;
}

void pool_free_locked(PoolBuf &p)
{
    unwrap(&p.image, &p.tex, &p.fbo);
    if (p.ahb)
        AHardwareBuffer_release(p.ahb);  // SurfaceFlinger keeps its own reference
    if (p.release_fence >= 0)
        close(p.release_fence);
    p = PoolBuf{};
}

// Returns a pool index we may draw into at the current surface size, or -1.
int pool_acquire_locked()
{
    int empty = -1, stale = -1;
    for (int i = 0; i < kPoolSize; i++) {
        PoolBuf &p = g_pool[i];
        if (!p.ahb) {
            if (empty < 0)
                empty = i;
        } else if (!p.held) {
            if (p.width == g_width && p.height == g_height)
                return i;
            if (stale < 0)
                stale = i;
        }
    }
    int i = empty >= 0 ? empty : stale;
    if (i < 0)
        return -1;
    pool_free_locked(g_pool[i]);
    PoolBuf &p = g_pool[i];
    AHardwareBuffer_Desc desc{};
    desc.width = (uint32_t)g_width;
    desc.height = (uint32_t)g_height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    if (AHardwareBuffer_allocate(&desc, &p.ahb) != 0 || !wrap_ahb(p.ahb, &p.image, &p.tex, &p.fbo)) {
        LOGE("copy mode: cannot allocate a %dx%d pool buffer", g_width, g_height);
        pool_free_locked(p);
        return -1;
    }
    p.width = g_width;
    p.height = g_height;
    p.serial = ++g_pool_serial;
    return i;
}

void present_copy_locked(uint32_t id, uint64_t frame, int fence, Buffer *buf)
{
    int pi = gl_init() ? pool_acquire_locked() : -1;
    if (pi < 0 || (!buf->fbo && !wrap_ahb(buf->ahb, &buf->image, &buf->tex, &buf->fbo))) {
        // Can't copy: skip the frame but keep the producer's clock running.
        g_stats.skipped++;
        if (fence >= 0)
            close(fence);
        send_locked(CHAM_RELEASE, id, 0, 0);
        send_locked(CHAM_FRAME_DONE, 0, frame, 0);
        return;
    }
    PoolBuf &p = g_pool[pi];
    g_stats.shown++;
    g_stats.copied++;
    gpu_wait(p.release_fence);  // SurfaceFlinger finished scanning it out
    p.release_fence = -1;
    gpu_wait(fence);            // the producer finished rendering

    glBindFramebuffer(GL_READ_FRAMEBUFFER, buf->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, p.fbo);
    glBlitFramebuffer(0, 0, buf->width, buf->height, 0, 0, p.width, p.height, GL_COLOR_BUFFER_BIT,
                      buf->width == p.width && buf->height == p.height ? GL_NEAREST : GL_LINEAR);
    if (g_stats.sample) {
        // Once per stats period: is the copied frame actually non-black?
        glBindFramebuffer(GL_READ_FRAMEBUFFER, p.fbo);
        glReadPixels(p.width / 2, p.height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, g_stats.pixel);
        g_stats.have_pixel = true;
        g_stats.sample = false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    int blit_fence = gpu_fence();

    // The producer's buffer is free as soon as the copy has read it.
    send_locked(CHAM_RELEASE, id, 0, 0, blit_fence);

    ASurfaceTransaction *txn = ASurfaceTransaction_create();
    ASurfaceTransaction_setBuffer(txn, g_sc, p.ahb, blit_fence);  // takes ownership
    ARect src{0, 0, p.width, p.height};
    ARect dst{0, 0, g_width, g_height};
    ASurfaceTransaction_setGeometry(txn, g_sc, src, dst, ANATIVEWINDOW_TRANSFORM_IDENTITY);
    ASurfaceTransaction_setBufferTransparency(txn, g_sc, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
    ASurfaceTransaction_setVisibility(txn, g_sc, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    if (g_vote_dirty && g_frame_rate_vote > 0 && set_frame_rate()) {
        set_frame_rate()(txn, g_sc, g_frame_rate_vote, 0 /* COMPATIBILITY_DEFAULT */);
        g_vote_dirty = false;
    }
    p.held = true;
    bool commit = set_on_commit() != nullptr;
    int64_t prev = g_pool_displayed;
    uint64_t prev_serial = prev >= 0 ? g_pool[prev].serial : 0;
    auto *ctx = new FrameCtx{g_client_gen, g_sc, (uint32_t)pi, prev, frame, commit, {commit ? 2 : 1}, true, prev_serial};
    g_pool_displayed = pi;
    if (commit)
        set_on_commit()(txn, ctx, on_commit);
    ASurfaceTransaction_setOnComplete(txn, ctx, on_complete);
    ASurfaceTransaction_apply(txn);
    ASurfaceTransaction_delete(txn);
}

void present(uint32_t id, uint64_t frame, int fence)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_stats.presents++;
    stats_log_locked();
    Buffer *buf = id < kMaxBuffers ? &g_buffers[id] : nullptr;
    if (!g_sc || !buf || !buf->ahb) {
        // Nothing to show on: hand the buffer straight back.
        g_stats.skipped++;
        if (fence >= 0)
            close(fence);
        send_locked(CHAM_RELEASE, id, 0, 0);
        send_locked(CHAM_FRAME_DONE, 0, frame, 0);
        return;
    }
    if (g_copy_mode) {
        present_copy_locked(id, frame, fence, buf);
        return;
    }
    g_stats.shown++;

    ASurfaceTransaction *txn = ASurfaceTransaction_create();
    // Takes ownership of the fence; SurfaceFlinger waits on it, not us.
    ASurfaceTransaction_setBuffer(txn, g_sc, buf->ahb, fence);
    ARect src{0, 0, buf->width, buf->height};
    ARect dst{0, 0, g_width, g_height};
    ASurfaceTransaction_setGeometry(txn, g_sc, src, dst, ANATIVEWINDOW_TRANSFORM_IDENTITY);
    ASurfaceTransaction_setBufferTransparency(txn, g_sc, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
    ASurfaceTransaction_setVisibility(txn, g_sc, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    if (g_vote_dirty && g_frame_rate_vote > 0 && set_frame_rate()) {
        set_frame_rate()(txn, g_sc, g_frame_rate_vote, 0 /* COMPATIBILITY_DEFAULT */);
        g_vote_dirty = false;
    }
    bool commit = set_on_commit() != nullptr;
    auto *ctx = new FrameCtx{g_client_gen, g_sc, id, g_displayed, frame, commit, {commit ? 2 : 1}};
    g_displayed = id;
    if (commit)
        set_on_commit()(txn, ctx, on_commit);
    ASurfaceTransaction_setOnComplete(txn, ctx, on_complete);
    ASurfaceTransaction_apply(txn);
    ASurfaceTransaction_delete(txn);
}

void add_buffer(uint32_t id, AHardwareBuffer *ahb)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (id >= kMaxBuffers) {
        AHardwareBuffer_release(ahb);
        return;
    }
    if (g_buffers[id].ahb) {
        unwrap(&g_buffers[id].image, &g_buffers[id].tex, &g_buffers[id].fbo);
        AHardwareBuffer_release(g_buffers[id].ahb);
    }
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    g_buffers[id] = Buffer{};
    g_buffers[id].ahb = ahb;
    g_buffers[id].width = (int32_t)desc.width;
    g_buffers[id].height = (int32_t)desc.height;
    LOGI("buffer %u: %ux%u format %u", id, desc.width, desc.height, desc.format);
}

void remove_buffer_locked(uint32_t id)
{
    if (id < kMaxBuffers && g_buffers[id].ahb) {
        unwrap(&g_buffers[id].image, &g_buffers[id].tex, &g_buffers[id].fbo);
        // SurfaceFlinger keeps its own reference while the buffer is on screen.
        AHardwareBuffer_release(g_buffers[id].ahb);
        g_buffers[id] = {};
    }
}

// Returns true if the session ended because a new client is waiting.
bool serve(int client, int listener)
{
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_client = client;
        g_client_gen++;
        g_displayed = -1;
        g_copy_mode = false;
        send_config_locked();
    }
    LOGI("producer connected");

    bool replaced = false;
    for (;;) {
        pollfd fds[2] = {{client, POLLIN, 0}, {listener, POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[1].revents & POLLIN) {
            // Newest producer wins, like a compositor restarting; tell the
            // old one so it doesn't reconnect and take the screen back.
            std::lock_guard<std::mutex> lock(g_lock);
            send_locked(CHAM_BYE, 0, CHAM_BYE_REPLACED, 0);
            replaced = true;
            break;
        }
        if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;

        cham_msg msg;
        int fd = -1;
        int rc = cham_recv(client, &msg, &fd);
        if (rc <= 0) {
            if (rc < 0)
                LOGE("recv failed: %s", strerror(-rc));
            break;
        }
        switch (msg.type) {
        case CHAM_HELLO: {
            std::lock_guard<std::mutex> lock(g_lock);
            g_copy_mode = (msg.b & CHAM_HELLO_COPY) != 0;
            LOGI("producer protocol v%llu%s", (unsigned long long)msg.a, g_copy_mode ? ", copy mode" : "");
            break;
        }
        case CHAM_BUFFER_ADD: {
            AHardwareBuffer *ahb = nullptr;
            int err = AHardwareBuffer_recvHandleFromUnixSocket(client, &ahb);
            if (err != 0 || !ahb) {
                LOGE("buffer %u: recvHandleFromUnixSocket failed (%d)", msg.id, err);
                goto done;
            }
            add_buffer(msg.id, ahb);
            break;
        }
        case CHAM_BUFFER_REMOVE: {
            std::lock_guard<std::mutex> lock(g_lock);
            remove_buffer_locked(msg.id);
            break;
        }
        case CHAM_PRESENT:
            present(msg.id, msg.a, fd);
            fd = -1;
            break;
        default:
            LOGE("unknown message %u", msg.type);
            break;
        }
        if (fd >= 0)
            close(fd);
    }
done:
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_client = -1;
        g_client_gen++;
        g_displayed = -1;
        for (uint32_t i = 0; i < kMaxBuffers; i++)
            remove_buffer_locked(i);
    }
    close(client);
    LOGI("producer disconnected%s", replaced ? " (replaced by a new one)" : "");
    return replaced;
}

// Human-readable state for the on-screen status line (nativeStatus()).
std::mutex g_status_lock;
std::string g_status = "starting";

void set_status(const std::string &status)
{
    std::lock_guard<std::mutex> lock(g_status_lock);
    g_status = status;
}

int open_listener(const std::string &path, ino_t *ino)
{
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
    unlink(path.c_str());
    if (listener < 0 || bind(listener, (sockaddr *)&addr, sizeof addr) != 0 || listen(listener, 4) != 0) {
        std::string err = strerror(errno);
        LOGE("cannot listen on %s: %s (is this app installed with Termux's shared user?)", path.c_str(), err.c_str());
        set_status("cannot listen on " + path + ": " + err);
        if (listener >= 0)
            close(listener);
        return -1;
    }
    chmod(path.c_str(), 0600);
    struct stat st{};
    stat(path.c_str(), &st);
    *ino = st.st_ino;
    LOGI("listening on %s", path.c_str());
    set_status("waiting for Termux (listening on " + path + ")");
    return listener;
}

void *server_main(void *arg)
{
    std::string path = *static_cast<std::string *>(arg);
    delete static_cast<std::string *>(arg);

    ino_t ino = 0;
    int listener = -1;
    for (;;) {
        // (Re)create the socket if it's missing or someone replaced the file:
        // then nothing could ever connect to us again.
        struct stat st{};
        if (listener < 0 || stat(path.c_str(), &st) != 0 || st.st_ino != ino) {
            if (listener >= 0) {
                LOGE("socket %s vanished; re-creating it", path.c_str());
                close(listener);
            }
            listener = open_listener(path, &ino);
            if (listener < 0) {
                sleep(2);
                continue;
            }
        }
        pollfd pfd{listener, POLLIN, 0};
        if (poll(&pfd, 1, 2000) <= 0)
            continue;
        int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        while (client >= 0) {
            set_status("connected to Termux");
            bool replaced = serve(client, listener);
            set_status("waiting for Termux (listening on " + path + ")");
            client = replaced ? accept4(listener, nullptr, nullptr, SOCK_CLOEXEC) : -1;
        }
    }
    return nullptr;
}

}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_io_github_harryl0l_chameleon_PresenterActivity_nativeStart(JNIEnv *env, jclass, jstring jpath)
{
    const char *path = env->GetStringUTFChars(jpath, nullptr);
    auto *arg = new std::string(path);
    env->ReleaseStringUTFChars(jpath, path);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, server_main, arg) == 0)
        pthread_detach(thread);
}

JNIEXPORT void JNICALL
Java_io_github_harryl0l_chameleon_PresenterActivity_nativeSurfaceCreated(JNIEnv *env, jclass, jobject surface)
{
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window)
        return;
    ASurfaceControl *sc = ASurfaceControl_createFromWindow(window, "chameleon");
    ANativeWindow_release(window);
    std::lock_guard<std::mutex> lock(g_lock);
    g_sc = sc;
    g_displayed = -1;
    g_vote_dirty = true;
    LOGI("surface control %s", sc ? "created" : "FAILED");
}

JNIEXPORT void JNICALL
Java_io_github_harryl0l_chameleon_PresenterActivity_nativeSurfaceChanged(JNIEnv *, jclass, jint width, jint height,
                                                                          jint refresh_mhz)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_width = width;
    g_height = height;
    g_refresh_mhz = refresh_mhz;
    LOGI("surface %dx%d @ %d mHz", width, height, refresh_mhz);
    send_config_locked();
}

JNIEXPORT void JNICALL
Java_io_github_harryl0l_chameleon_PresenterActivity_nativeSurfaceDestroyed(JNIEnv *, jclass)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_sc) {
        ASurfaceTransaction *txn = ASurfaceTransaction_create();
        ASurfaceTransaction_reparent(txn, g_sc, nullptr);
        ASurfaceTransaction_apply(txn);
        ASurfaceTransaction_delete(txn);
        ASurfaceControl_release(g_sc);
        g_sc = nullptr;
    }
    // Detached from the display: SurfaceFlinger drops every pool buffer.
    for (PoolBuf &p : g_pool)
        p.held = false;
    g_pool_displayed = -1;
    if (g_displayed >= 0)
        send_locked(CHAM_RELEASE, (uint32_t)g_displayed, 0, 0);
    g_displayed = -1;
    g_width = g_height = 0;
    send_config_locked();  // 0x0: producer pauses
}

JNIEXPORT jstring JNICALL
Java_io_github_harryl0l_chameleon_PresenterActivity_nativeStatus(JNIEnv *env, jclass)
{
    std::string status;
    {
        std::lock_guard<std::mutex> lock(g_status_lock);
        status = g_status;
    }
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_client >= 0 && (g_displayed >= 0 || g_pool_displayed >= 0))
            status = "showing frames";
    }
    return env->NewStringUTF(status.c_str());
}

JNIEXPORT void JNICALL
Java_io_github_harryl0l_chameleon_PresenterActivity_nativeSetFrameRateVote(JNIEnv *, jclass, jfloat hz)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_frame_rate_vote = hz;
    g_vote_dirty = true;
    LOGI("frame rate vote %.1f Hz", hz);
}

}  // extern "C"
