// Chameleon presenter: receives AHardwareBuffers + acquire fences from one
// Termux producer over a SOCK_SEQPACKET socket and shows them on an
// ASurfaceControl child of the activity's SurfaceView. No pixel copies:
// SurfaceFlinger (and usually an HWC overlay) reads the producer's buffer.
//
// Protocol: common/chameleon_proto.h.

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <mutex>
#include <string>

#include "cham_io.h"
#include "chameleon_proto.h"

#define TAG "Chameleon"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

constexpr uint32_t kMaxBuffers = 64;

struct Buffer {
    AHardwareBuffer *ahb = nullptr;
    int32_t width = 0;
    int32_t height = 0;
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
};

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

    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (ctx->gen == g_client_gen) {
            // The buffer this frame replaced is free once release_fence signals.
            if (ctx->prev >= 0 && ctx->prev != ctx->id)
                send_locked(CHAM_RELEASE, (uint32_t)ctx->prev, 0, 0, release_fence);
            send_locked(CHAM_FRAME_DONE, 0, ctx->frame, (uint64_t)latch);
        }
    }
    if (release_fence >= 0)
        close(release_fence);
    delete ctx;
}

void present(uint32_t id, uint64_t frame, int fence)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Buffer *buf = id < kMaxBuffers ? &g_buffers[id] : nullptr;
    if (!g_sc || !buf || !buf->ahb) {
        // Nothing to show on: hand the buffer straight back.
        if (fence >= 0)
            close(fence);
        send_locked(CHAM_RELEASE, id, 0, 0);
        send_locked(CHAM_FRAME_DONE, 0, frame, 0);
        return;
    }

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
    auto *ctx = new FrameCtx{g_client_gen, g_sc, id, g_displayed, frame};
    g_displayed = id;
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
    if (g_buffers[id].ahb)
        AHardwareBuffer_release(g_buffers[id].ahb);
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    g_buffers[id] = {ahb, (int32_t)desc.width, (int32_t)desc.height};
    LOGI("buffer %u: %ux%u format %u", id, desc.width, desc.height, desc.format);
}

void remove_buffer_locked(uint32_t id)
{
    if (id < kMaxBuffers && g_buffers[id].ahb) {
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
            // Newest producer wins, like a compositor restarting.
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
        case CHAM_HELLO:
            LOGI("producer protocol v%llu", (unsigned long long)msg.a);
            break;
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

void *server_main(void *arg)
{
    std::string path = *static_cast<std::string *>(arg);
    delete static_cast<std::string *>(arg);

    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
    unlink(path.c_str());
    if (listener < 0 || bind(listener, (sockaddr *)&addr, sizeof addr) != 0 || listen(listener, 4) != 0) {
        LOGE("cannot listen on %s: %s (is this app installed with Termux's shared user?)",
             path.c_str(), strerror(errno));
        return nullptr;
    }
    chmod(path.c_str(), 0600);
    LOGI("listening on %s", path.c_str());

    for (;;) {
        int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno != EINTR)
                LOGE("accept failed: %s", strerror(errno));
            continue;
        }
        while (serve(client, listener)) {
            client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0)
                break;
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
    if (g_displayed >= 0)
        send_locked(CHAM_RELEASE, (uint32_t)g_displayed, 0, 0);
    g_displayed = -1;
    g_width = g_height = 0;
    send_config_locked();  // 0x0: producer pauses
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
