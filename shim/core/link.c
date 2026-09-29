/*
 * Connection from KWin's process to the Chameleon presenter app.
 *
 * - One reader thread (re)connects to $PREFIX/tmp/chameleon-0 and handles
 *   CONFIG, RELEASE and FRAME_DONE.
 * - Buffers are registered with the presenter lazily, on first present.
 * - At most one page flip is pending, as with KMS. It completes on the
 *   presenter's FRAME_DONE (SurfaceFlinger latched the frame), or on a
 *   simulated vblank when nothing is on screen (app closed or paused), so
 *   KWin keeps running either way.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <drm.h>

#include "../../common/cham_io.h"
#include "internal.h"

#define MAX_SLOTS 64

static int g_sock = -1;
static uint64_t g_gen;              /* bumped per presenter connection */
static struct cham_bo *g_slots[MAX_SLOTS];
static uint32_t g_cfg_w, g_cfg_h;
static int g_cfg_seen;              /* a non-zero CONFIG ever arrived */
static uint32_t g_first_w, g_first_h, g_first_mhz; /* ...and what it said */
static pthread_cond_t g_cond;
static int g_replaced;              /* another producer took the app over */

static struct {
    int active;
    int simulated;                  /* complete on a timer, not FRAME_DONE */
    uint64_t frame;
    uint64_t user_data;
    uint32_t crtc_id;
    int event_fd;
    uint64_t deadline_ns;           /* simulated: when; real: watchdog */
} g_flip;
static uint32_t g_sequence;

/* Activity counters, logged every 5 s while something happens. */
static struct {
    unsigned commits, presented, not_shown, flips, simulated;
    uint64_t since;
} g_stats;

static void stats_maybe_log_locked(void)
{
    uint64_t now = now_ns();
    if (!g_stats.since)
        g_stats.since = now;
    if (now - g_stats.since < 5000000000ull)
        return;
    if (g_stats.commits || g_stats.flips)
        cham_log("last 5 s: %u commits, %u presented, %u not shown (no app surface), %u flips (%u simulated)",
                 g_stats.commits, g_stats.presented, g_stats.not_shown, g_stats.flips, g_stats.simulated);
    memset(&g_stats, 0, sizeof g_stats);
    g_stats.since = now;
}

static const char *socket_path(void)
{
    const char *p = getenv("CHAMELEON_SOCKET");
    return p ? p : CHAM_SOCKET_PATH;
}

static void timespec_at(struct timespec *ts, uint64_t ns)
{
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
}

/* ---- page flips ---- */

static void emit_flip_locked(uint64_t ts_ns)
{
    /* Copy mode: the presenter released our buffer with the fence of its
     * blit. Wait for it so KWin can't render into a buffer being copied. */
    bo_wait_release_fences_locked();

    struct drm_event_vblank ev = {
        .base = {.type = DRM_EVENT_FLIP_COMPLETE, .length = sizeof ev},
        .user_data = g_flip.user_data,
        .tv_sec = (uint32_t)(ts_ns / 1000000000ull),
        .tv_usec = (uint32_t)(ts_ns % 1000000000ull / 1000),
        .sequence = ++g_sequence,
        .crtc_id = g_flip.crtc_id,
    };
    if (write(g_flip.event_fd, &ev, sizeof ev) != (ssize_t)sizeof ev)
        cham_log("cannot deliver page-flip event: %s", strerror(errno));
    g_flip.active = 0;
    g_stats.flips++;
    g_stats.simulated += g_flip.simulated;
    pthread_cond_broadcast(&g_cond);
}

int link_flip_pending_locked(void)
{
    return g_flip.active;
}

void link_queue_flip_locked(uint64_t user_data, uint32_t crtc_id, int event_fd, uint64_t frame, int presented,
                            uint32_t refresh_mhz)
{
    uint64_t period = 1000000000000ull / (refresh_mhz ? refresh_mhz : 60000);
    g_flip.active = 1;
    g_flip.simulated = !presented;
    g_flip.frame = frame;
    g_flip.user_data = user_data;
    g_flip.crtc_id = crtc_id;
    g_flip.event_fd = event_fd;
    /* Simulated vblank one period out; a real flip gets a generous watchdog
     * in case the presenter stalls (e.g. the app got frozen). */
    g_flip.deadline_ns = now_ns() + (presented ? 250000000ull : period);
    pthread_cond_broadcast(&g_cond);
}

static void *vblank_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        stats_maybe_log_locked();
        if (!g_flip.active) {
            struct timespec idle;
            timespec_at(&idle, now_ns() + 1000000000ull);
            pthread_cond_timedwait(&g_cond, &g_lock, &idle);
            continue;
        }
        uint64_t now = now_ns();
        if (now >= g_flip.deadline_ns) {
            if (!g_flip.simulated)
                cham_log("presenter did not report frame %llu, completing the flip anyway",
                         (unsigned long long)g_flip.frame);
            emit_flip_locked(now);
            continue;
        }
        struct timespec ts;
        timespec_at(&ts, g_flip.deadline_ns);
        pthread_cond_timedwait(&g_cond, &g_lock, &ts);
    }
    return NULL;
}

/* ---- presenting ---- */

static int send_msg_locked(uint32_t type, uint32_t id, uint64_t a, uint64_t b, int fd)
{
    struct cham_msg msg = {type, id, a, b};
    return g_sock >= 0 ? cham_send(g_sock, &msg, fd) : -ENOTCONN;
}

int link_present_locked(struct cham_bo *bo, int in_fence, uint64_t frame)
{
    g_stats.commits++;
    if (g_sock < 0 || !g_cfg_w || !g_cfg_h) {
        g_stats.not_shown++;
        return 0;
    }
    if (bo->slot < 0 || bo->slot_gen != g_gen) {
        int slot = -1;
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (!g_slots[i]) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            cham_log("presenter buffer table full");
            return 0;
        }
        if (send_msg_locked(CHAM_BUFFER_ADD, (uint32_t)slot, 0, 0, -1) < 0 || ahb_send(bo->ahb, g_sock) != 0) {
            cham_log("cannot send buffer to the presenter");
            return 0;
        }
        g_slots[slot] = bo;
        bo->slot = slot;
        bo->slot_gen = g_gen;
    }
    int ok = send_msg_locked(CHAM_PRESENT, (uint32_t)bo->slot, frame, now_ns(), in_fence) == 0;
    g_stats.presented += ok;
    return ok;
}

void link_forget_bo_locked(struct cham_bo *bo)
{
    if (bo->slot >= 0 && bo->slot_gen == g_gen) {
        g_slots[bo->slot] = NULL;
        send_msg_locked(CHAM_BUFFER_REMOVE, (uint32_t)bo->slot, 0, 0, -1);
    }
    bo->slot = -1;
}

/* ---- reader thread ---- */

static int connect_presenter(void)
{
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, socket_path(), sizeof addr.sun_path - 1);
    if (s >= 0 && connect(s, (struct sockaddr *)&addr, sizeof addr) == 0)
        return s;
    if (s >= 0)
        close(s);
    return -1;
}

static void handle_msg_locked(const struct cham_msg *m, int *fd)
{
    switch (m->type) {
    case CHAM_CONFIG:
        g_cfg_w = CHAM_CONFIG_WIDTH(m);
        g_cfg_h = CHAM_CONFIG_HEIGHT(m);
        if (g_cfg_w && g_cfg_h && !g_cfg_seen) {
            g_cfg_seen = 1;
            g_first_w = g_cfg_w;
            g_first_h = g_cfg_h;
            g_first_mhz = (uint32_t)m->b;
        }
        pthread_cond_broadcast(&g_cond);
        if (g_cfg_w && g_cfg_h)
            input_post(m); /* output.c: KWin's screen follows the window */
        break;
    case CHAM_RELEASE:
        if (m->id < MAX_SLOTS && g_slots[m->id]) {
            struct cham_bo *bo = g_slots[m->id];
            if (bo->release_fence >= 0)
                close(bo->release_fence);
            bo->release_fence = *fd;
            *fd = -1;
        }
        break;
    case CHAM_FRAME_DONE:
        if (g_flip.active && !g_flip.simulated && m->a >= g_flip.frame)
            emit_flip_locked(m->b ? m->b : now_ns());
        break;
    case CHAM_BYE:
        if (m->a == CHAM_BYE_REPLACED)
            g_replaced = 1;
        break;
    case CHAM_INPUT:
        input_post(m);
        break;
    }
}

static void *reader_main(void *arg)
{
    (void)arg;
    int warned = 0;
    for (;;) {
        int s = connect_presenter();
        if (s < 0) {
            if (!warned++)
                cham_log("waiting for the Chameleon app at %s", socket_path());
            sleep(1);
            continue;
        }
        warned = 0;
        struct cham_msg hello = {CHAM_HELLO, 0, CHAM_PROTO_VERSION, CHAM_HELLO_COPY};
        cham_send(s, &hello, -1);

        pthread_mutex_lock(&g_lock);
        g_sock = s;
        g_gen++;
        memset(g_slots, 0, sizeof g_slots);
        pthread_mutex_unlock(&g_lock);
        cham_log("connected to the presenter (pid %d)", (int)getpid());

        for (;;) {
            struct cham_msg m;
            int fd = -1;
            if (cham_recv(s, &m, &fd) <= 0)
                break;
            pthread_mutex_lock(&g_lock);
            handle_msg_locked(&m, &fd);
            pthread_mutex_unlock(&g_lock);
            if (fd >= 0)
                close(fd);
        }

        pthread_mutex_lock(&g_lock);
        g_sock = -1;
        g_cfg_w = g_cfg_h = 0;
        memset(g_slots, 0, sizeof g_slots);
        if (g_flip.active && !g_flip.simulated) {
            g_flip.simulated = 1;
            g_flip.deadline_ns = now_ns();
            pthread_cond_broadcast(&g_cond);
        }
        pthread_mutex_unlock(&g_lock);
        close(s);
        pthread_mutex_lock(&g_lock);
        int replaced = g_replaced;
        pthread_mutex_unlock(&g_lock);
        if (replaced) {
            cham_log("another producer (e.g. a second KWin) took over the Chameleon app; this KWin "
                     "keeps running without a screen. Stop the other one and restart this one.");
            return NULL;
        }
        cham_log("presenter disconnected");
        sleep(1); /* never spin, whatever went wrong */
    }
    return NULL;
}

static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void start_threads(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cond, &attr);

    pthread_t t;
    pthread_create(&t, NULL, reader_main, NULL);
    pthread_detach(t);
    pthread_create(&t, NULL, vblank_main, NULL);
    pthread_detach(t);
}

void link_start(void)
{
    pthread_once(&g_once, start_threads);
}

void link_app_size(uint32_t *width, uint32_t *height)
{
    pthread_mutex_lock(&g_lock);
    *width = g_cfg_w;
    *height = g_cfg_h;
    pthread_mutex_unlock(&g_lock);
}

int link_wait_config(uint32_t *width, uint32_t *height, uint32_t *refresh_mhz, int timeout_ms)
{
    struct timespec ts;
    timespec_at(&ts, now_ns() + (uint64_t)timeout_ms * 1000000ull);
    pthread_mutex_lock(&g_lock);
    while (!g_cfg_seen) {
        if (pthread_cond_timedwait(&g_cond, &g_lock, &ts) == ETIMEDOUT)
            break;
    }
    int ok = g_cfg_seen;
    if (ok) {
        *width = g_first_w;
        *height = g_first_h;
        *refresh_mhz = g_first_mhz ? g_first_mhz : 60000;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
