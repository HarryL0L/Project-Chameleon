/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Connection from chameleon-kernel to the Chameleon presenter app.
 *
 * - One reader thread (re)connects to $PREFIX/tmp/chameleon-0 and handles
 *   CONFIG, RELEASE, FRAME_QUEUED and FRAME_DONE; input goes on to the input
 *   thread without g_lock.
 * - Buffers are registered with the presenter lazily, on first present.
 * - At most one page flip is pending, as with KMS. It completes on a vblank
 *   tick once the presenter copied the previous frame (FRAME_QUEUED) and
 *   Android latched enough frames (FRAME_DONE), see g_flip; or on a
 *   simulated vblank when nothing is on screen (app closed or paused), so
 *   the compositor keeps running either way.
 * - Vblank ticks follow the display's vsync, from Android's present fences
 *   (CHAM_PRESENTED), see "the vsync grid".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
static uint64_t g_surface_gone_ns;  /* when the app's surface went away (0: it has one, or never had) */
static int g_returned;              /* it is back: the next PING's answer from the compositor is logged */
static uint32_t g_first_w, g_first_h, g_first_mhz, g_first_flags; /* ...and what it said */
static uint32_t g_win_w, g_win_h, g_first_win_w, g_first_win_h; /* the app's whole window (CHAM_WINDOW) */
static pthread_cond_t g_cond;
static int g_replaced;              /* another producer took the app over */
static uint64_t g_app_version;      /* the app speaks another protocol version (CHAM_BYE_VERSION) */

static struct {
    int active;
    int simulated;                  /* complete on a timer, not FRAME_DONE */
    uint64_t frame;
    uint64_t user_data;
    uint32_t crtc_id;
    int event_fd;
    uint64_t deadline_ns;           /* simulated: when; real: watchdog, or the vblank tick it completes on */
    uint64_t queued_ns;             /* when the compositor committed */
    uint64_t period_ns;
    int on_tick;                    /* completes at deadline_ns, a vblank tick */
    int held;                       /* waited for a copy or a latch */
    uint64_t prev_frame, prev2_frame; /* the frames shown before it, if any */
    uint64_t seq;                   /* bumped per queued flip */
    int grid;                       /* deadline_ns is on the display's vsync grid */
} g_flip;
/* Like a display engine, and like VKMS: a flip completes on the first
 * vblank tick after the commit reached the app, which copies the new front
 * buffer during the next frame (scanout reading it), while the compositor
 * leaves that buffer alone until the next flip. So that flip also waits
 * until the previous copy is done (its buffer is free then), and until
 * Android latched the frame before, so few frames wait in Android. When
 * Android keeps every queued frame (back pressure), the one before that is
 * enough: two may wait. A compositor aims its commit at the vblank, while
 * Android wants the buffer a few ms before its own latch; the second frame
 * covers that gap. Ticks follow Android's latch times.
 * CHAMELEON_ANDROID_QUEUE=1 or 2 overrides the choice;
 * CHAMELEON_EARLY_FLIP=0: at the latch, as before. */
static int g_early = 1;
static int g_android_queue;         /* 0: choose per flip */
static int g_back_pressure;         /* CHAM_CONFIG_BACK_PRESSURE */
static uint64_t g_latch_ns;         /* last latch: the vblank grid's phase */
static uint64_t g_latched_frame;    /* newest frame Android has latched (or skipped) */
static uint64_t g_copied_frame;     /* newest frame the app has copied (FRAME_QUEUED) */
static uint64_t g_presented_frame, g_presented_prev; /* newest frames handed to the app */
static uint32_t g_sequence;
static uint64_t g_last_flip_ns;     /* the last page-flip event's time */

/* Activity counters, logged every 5 s while something happens. */
static struct {
    unsigned commits, presented, not_shown, flips, simulated;
    uint64_t wait_sum, wait_max;    /* commit to shown, shown flips */
    unsigned held;                  /* flips that waited for a copy or a latch */
    unsigned second;                /* flips only a second frame in Android let through */
    unsigned on_vsync;              /* flips on the display's vsync grid */
    uint64_t since;
} g_stats;

static void stats_maybe_log_locked(void)
{
    uint64_t now = now_ns();
    if (!g_stats.since)
        g_stats.since = now;
    if (now - g_stats.since < 5000000000ull)
        return;
    if (g_stats.commits || g_stats.flips) {
        unsigned shown = g_stats.flips - g_stats.simulated;
        cham_log("last 5 s: %u commits, %u presented, %u not shown (no app surface), %u flips (%u simulated); "
                 "commit to shown %.1f ms avg, %.1f max; %u waited for Android, %u used a second frame there; "
                 "%u on the display's vsync",
                 g_stats.commits, g_stats.presented, g_stats.not_shown, g_stats.flips, g_stats.simulated,
                 shown ? g_stats.wait_sum / shown / 1e6 : 0.0, g_stats.wait_max / 1e6, g_stats.held,
                 g_stats.second, g_stats.on_vsync);
    }
    memset(&g_stats, 0, sizeof g_stats);
    g_stats.since = now;
}

static const char *socket_path(void)
{
    const char *p = getenv("CHAMELEON_SOCKET");
    return p ? p : CHAM_SOCKET_PATH;
}

/* The cursor plane as last committed, and whether the presenter has it. */
static struct link_cursor g_cursor = {.fence = -1};
static int g_cursor_sent;
static uint64_t g_cursor_gen;
static int g_cfg_cursor;            /* this app shows it (CHAM_CONFIG_CURSOR) */
/* The primary plane's place, and whether this app shows one that isn't the
 * whole screen (CHAM_CONFIG_PLANE). */
static struct link_plane g_plane = {.plain = 1};
static int g_cfg_plane;
static void send_cursor_locked(int new_image, int fence);

static void timespec_at(struct timespec *ts, uint64_t ns)
{
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
}

/* ---- the vsync grid ---- */

/* A display engine completes flips on its vblanks, so page-flip events are
 * whole refreshes apart, and compositors plan their frames on that grid.
 * Android's latch times wander by milliseconds with SurfaceFlinger's load,
 * so ticks don't follow them directly: the period and phase of the grid
 * come from Android's present fences, which signal on the display's vsync,
 * and ticks sit at the point of each refresh where Android latches (the
 * start of the stretch the recent latches fall in). Without present fences
 * (an older app) ticks follow the last latch. */
#define PRESENT_FENCES 4
#define LATCHES 16
#define GRID_MAX_AGE_NS 500000000ull     /* older vsyncs and latches aren't used */
static int g_present_fd[PRESENT_FENCES]; /* not signalled yet, oldest first */
static int g_present_fds;
static uint64_t g_vsync_ns;              /* newest vsync seen */
static uint64_t g_vsync_ref_ns;          /* an older one: the period is measured from it */
static uint64_t g_vsync_period;          /* measured, 0 until known */
static uint64_t g_mode_period;           /* the mode's (the compositor's) refresh period */
static uint64_t g_latches[LATCHES];      /* recent latch times */
static unsigned g_latch_next;
static uint64_t g_tick_phase;            /* ticks: at a vsync + this, ... */
static uint64_t g_tick_phase_period;     /* ...on a grid of this period (0: none yet) */

/* A sync file's signal time: 1 with *ns, 0 if not signalled yet, -1 if it
 * has none (error, or signalled with an error). */
static int fence_time(int fd, uint64_t *ns)
{
    struct sync_fence_info fi[8];
    struct sync_file_info info;
    memset(&info, 0, sizeof info);
    memset(fi, 0, sizeof fi);
    info.num_fences = 8;
    info.sync_fence_info = (uint64_t)(uintptr_t)fi;
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0) {
#ifdef CHAM_HOST_TEST
        /* the test's stand-in: a socket that holds the 8-byte time once signalled */
        struct pollfd p = {fd, POLLIN, 0};
        if (errno == ENOTTY && poll(&p, 1, 0) == 0)
            return 0;
        return errno == ENOTTY && read(fd, ns, sizeof *ns) == (ssize_t)sizeof *ns && *ns ? 1 : -1;
#endif
        return -1;
    }
    if (info.status <= 0)
        return info.status == 0 ? 0 : -1;
    uint64_t t = 0;
    for (uint32_t i = 0; i < info.num_fences && i < 8; i++)
        if ((uint64_t)fi[i].timestamp_ns > t)
            t = (uint64_t)fi[i].timestamp_ns;
    *ns = t;
    return t ? 1 : -1;
}

static void vsync_seen_locked(uint64_t t)
{
    uint64_t p = g_mode_period;
    if (!p || t <= g_vsync_ns)
        return;
    if (!g_vsync_ref_ns || t - g_vsync_ref_ns > 4000000000ull)
        g_vsync_ref_ns = t; /* a fresh start: the mode may have changed meanwhile */
    else if (t - g_vsync_ref_ns >= 200000000ull) {
        /* The refreshes since the reference, counted with the period
         * measured so far: a panel a little off its mode's rate is whole
         * refreshes off the mode's count after a few seconds. */
        uint64_t q = g_vsync_period ? g_vsync_period : p;
        uint64_t n = (t - g_vsync_ref_ns + q / 2) / q, est = n ? (t - g_vsync_ref_ns) / n : 0;
        if (est > p - p / 20 && est < p + p / 20)
            g_vsync_period = est;
        else if (n)
            g_vsync_ref_ns = t, g_vsync_period = 0; /* the display runs at another rate */
    }
    if (!g_vsync_ns)
        cham_log("vblank ticks follow the display's vsync (Android's present fences)");
    g_vsync_ns = t;
}

static void read_present_fences_locked(void)
{
    int keep = 0;
    for (int i = 0; i < g_present_fds; i++) {
        uint64_t t;
        int r = fence_time(g_present_fd[i], &t);
        if (r == 0) {
            g_present_fd[keep++] = g_present_fd[i];
            continue;
        }
        close(g_present_fd[i]);
        if (r > 0)
            vsync_seen_locked(t);
    }
    g_present_fds = keep;
}

static void add_present_fence_locked(int fd)
{
    read_present_fences_locked();
    if (g_present_fds == PRESENT_FENCES) { /* never signalled: drop the oldest */
        close(g_present_fd[0]);
        memmove(g_present_fd, g_present_fd + 1, sizeof g_present_fd - sizeof g_present_fd[0]);
        g_present_fds--;
    }
    g_present_fd[g_present_fds++] = fd;
}

static void close_present_fences_locked(void)
{
    for (int i = 0; i < g_present_fds; i++)
        close(g_present_fd[i]);
    g_present_fds = 0;
}

/* t's place in a refresh of period p that starts at `base`, in [0, p). */
static uint64_t phase_of(uint64_t t, uint64_t base, uint64_t p)
{
    int64_t r = (int64_t)(t - base) % (int64_t)p;
    return (uint64_t)(r < 0 ? r + (int64_t)p : r);
}

/* The grid: ticks at anchor + k * period. 1: on the display's vsync; 0: on
 * the last latch (no vsyncs known); -1: none yet. */
static int grid_locked(uint64_t now, uint64_t *anchor, uint64_t *period)
{
    uint64_t p = g_mode_period, v = g_vsync_period;
    int measured = v > p - p / 20 && v < p + p / 20; /* ...and still the mode's rate */
    if (measured)
        p = v;
    /* Only with the display's measured period: the mode's is a little off
     * it, which puts the latches of earlier refreshes in the wrong place. */
    if (p && measured && g_vsync_ns && (now > g_vsync_ns ? now - g_vsync_ns : 0) < GRID_MAX_AGE_NS) {
        /* where in the refresh the recent latches fall: they come late by
         * varying amounts, never early, so the stretch they cover starts
         * after the widest gap between them */
        uint64_t ph[LATCHES];
        int n = 0;
        for (int i = 0; i < LATCHES; i++) {
            uint64_t l = g_latches[i];
            if (!l || (l < g_vsync_ns ? g_vsync_ns - l : l - g_vsync_ns) > GRID_MAX_AGE_NS)
                continue;
            uint64_t v = phase_of(l, g_vsync_ns, p);
            int j = n++;
            for (; j > 0 && ph[j - 1] > v; j--)
                ph[j] = ph[j - 1];
            ph[j] = v;
        }
        if (n >= 4) {
            uint64_t start = ph[0], widest = ph[0] + p - ph[n - 1];
            for (int i = 1; i < n; i++) {
                if (ph[i] - ph[i - 1] > widest) {
                    widest = ph[i] - ph[i - 1];
                    start = ph[i];
                }
            }
            /* Which latches are in the window changes the start a little;
             * ticks move only to an earlier latch, or when the latches
             * moved for real (more than 1 ms later). */
            uint64_t later = phase_of(start, g_tick_phase, p), q = g_tick_phase_period;
            if (q < p - p / 100 || q > p + p / 100 || later > 1000000) {
                g_tick_phase = start;
                g_tick_phase_period = p;
            }
            *anchor = g_vsync_ns + g_tick_phase;
            *period = p;
            return 1;
        }
    }
    if (g_latch_ns && g_flip.period_ns) {
        *anchor = g_latch_ns;
        *period = g_flip.period_ns;
        return 0;
    }
    return -1;
}

/* The tick a flip completes on: the first after t (nearest: the one nearest
 * t), and on the vsync grid never at or before the last page flip's. */
static uint64_t tick_locked(uint64_t t, int nearest)
{
    uint64_t a, p;
    int grid = grid_locked(now_ns(), &a, &p);
    if (grid < 0 || (grid == 0 && nearest))
        return t; /* no grid; or as before: at the latch itself */
    int64_t d = (int64_t)(t - a), k = d / (int64_t)p;
    if (d < 0 && k * (int64_t)p != d)
        k--; /* floor */
    uint64_t tick = a + (uint64_t)(k * (int64_t)p);
    if (nearest ? t - tick > p / 2 : tick <= t)
        tick += p;
    if (grid == 1) {
        while (g_last_flip_ns && tick < g_last_flip_ns + p / 2)
            tick += p;
        g_flip.grid = 1;
    }
    return tick;
}

/* ---- page flips ---- */

static void emit_flip_locked(uint64_t ts_ns)
{
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
    g_flip.on_tick = g_flip.held = 0;
    g_last_flip_ns = ts_ns;
    g_stats.on_vsync += g_flip.grid;
    g_flip.grid = 0;
    g_stats.flips++;
    g_stats.simulated += g_flip.simulated;
    if (!g_flip.simulated) {
        uint64_t wait = ts_ns > g_flip.queued_ns ? ts_ns - g_flip.queued_ns : 0;
        g_stats.wait_sum += wait;
        if (wait > g_stats.wait_max)
            g_stats.wait_max = wait;
    }
    pthread_cond_broadcast(&g_cond);
}

int link_flip_pending_locked(void)
{
    return g_flip.active;
}

/* Completes the pending flip (the vblank thread). Copy mode: the presenter
 * released our buffers with the fences of its blits; they must signal
 * first, so the compositor can't render into a buffer still being copied.
 * They are waited for without g_lock, which the compositor's requests and
 * the app's messages need meanwhile. */
static void complete_flip_locked(uint64_t ts_ns)
{
    int fences[MAX_SLOTS];
    int n = bo_take_release_fences_locked(fences, MAX_SLOTS);
    if (n) {
        uint64_t seq = g_flip.seq;
        pthread_mutex_unlock(&g_lock);
        for (int i = 0; i < n; i++) {
            struct pollfd pfd = {fences[i], POLLIN, 0};
            poll(&pfd, 1, 100);
            close(fences[i]);
        }
        pthread_mutex_lock(&g_lock);
        if (!g_flip.active || g_flip.seq != seq)
            return; /* cancelled meanwhile (its file closed) */
    }
    emit_flip_locked(ts_ns);
}

void link_cancel_flip_locked(int event_fd)
{
    if (g_flip.active && g_flip.event_fd == event_fd) {
        g_flip.active = 0;
        g_flip.on_tick = g_flip.held = 0;
        pthread_cond_broadcast(&g_cond);
    }
}

/* Completes the pending flip on the next vblank tick once the previous
 * copy is done and Android has latched what it should have by now. */
static void schedule_tick_locked(void)
{
    if (!g_flip.active || g_flip.simulated || g_flip.on_tick)
        return;
    uint64_t copied = g_copied_frame > g_latched_frame ? g_copied_frame : g_latched_frame;
    int queue = g_android_queue ? g_android_queue : g_back_pressure ? 2 : 1;
    static int logged;
    if (queue != logged) {
        cham_log("up to %d frame%s may wait in Android", queue, queue > 1 ? "s" : "");
        logged = queue;
    }
    uint64_t must_latch = queue >= 2 ? g_flip.prev2_frame : g_flip.prev_frame;
    if (copied < g_flip.prev_frame || g_latched_frame < must_latch) {
        g_stats.held += !g_flip.held; /* once per flip */
        g_flip.held = 1;
        return;
    }
    g_stats.second += g_latched_frame < g_flip.prev_frame; /* the previous frame still waits */
    read_present_fences_locked();
    g_flip.on_tick = 1;
    g_flip.deadline_ns = tick_locked(now_ns(), 0);
    pthread_cond_broadcast(&g_cond);
}

void link_queue_flip_locked(uint64_t user_data, uint32_t crtc_id, int event_fd, uint64_t frame, int presented,
                            uint32_t refresh_mhz)
{
    uint64_t period = 1000000000000ull / (refresh_mhz ? refresh_mhz : 60000);
    g_flip.active = 1;
    g_flip.seq++;
    g_flip.simulated = !presented;
    g_flip.frame = frame;
    g_flip.user_data = user_data;
    g_flip.crtc_id = crtc_id;
    g_flip.event_fd = event_fd;
    /* Simulated vblank one period out; a real flip gets a generous watchdog
     * in case the presenter stalls (e.g. the app got frozen). */
    g_flip.queued_ns = now_ns();
    g_flip.deadline_ns = g_flip.queued_ns + (presented ? 250000000ull : period);
    g_flip.period_ns = period;
    g_flip.on_tick = g_flip.held = 0;
    g_flip.grid = 0;
    if (period != g_mode_period) {
        /* another refresh rate: what was measured at the old one doesn't apply */
        g_mode_period = period;
        g_vsync_ref_ns = g_vsync_period = g_tick_phase_period = 0;
        memset(g_latches, 0, sizeof g_latches);
    }
    if (presented) {
        g_flip.prev2_frame = g_presented_prev;
        g_flip.prev_frame = g_presented_frame;
        g_presented_prev = g_presented_frame;
        g_presented_frame = frame;
    }
    pthread_cond_broadcast(&g_cond);
    if (g_early)
        schedule_tick_locked();
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
            if (g_flip.on_tick) {
                complete_flip_locked(g_flip.deadline_ns);
                continue;
            }
            if (!g_flip.simulated)
                cham_log("presenter did not report frame %llu, completing the flip anyway",
                         (unsigned long long)g_flip.frame);
            complete_flip_locked(now);
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

/* The presenter's id for `bo`, registered with it now if it has none on this
 * connection; -1 if that fails. */
static int slot_locked(struct cham_bo *bo)
{
    if (bo->slot >= 0 && bo->slot_gen == g_gen)
        return bo->slot;
    int slot = -1;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!g_slots[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        cham_log("presenter buffer table full");
        return -1;
    }
    if (send_msg_locked(CHAM_BUFFER_ADD, (uint32_t)slot, 0, 0, -1) < 0 || ahb_send(bo->ahb, g_sock) != 0) {
        cham_log("cannot send buffer to the presenter");
        return -1;
    }
    g_slots[slot] = bo;
    bo->slot = slot;
    bo->slot_gen = g_gen;
    return slot;
}

int link_present_locked(struct cham_bo *bo, int in_fence, uint64_t frame)
{
    g_stats.commits++;
    if (g_sock < 0 || !g_cfg_w || !g_cfg_h) {
        g_stats.not_shown++;
        return 0;
    }
    int slot = bo ? slot_locked(bo) : (int)CHAM_PRESENT_UNCHANGED;
    if (bo && slot < 0)
        return 0;
    if (bo && !g_plane.plain && g_cfg_plane &&
        send_msg_locked(CHAM_PLANE, g_plane.crtc_w | g_plane.crtc_h << 16,
                        CHAM_CURSOR_RECT(g_plane.src_x, g_plane.src_y, g_plane.src_w, g_plane.src_h),
                        CHAM_CURSOR_RECT(g_plane.dst_x, g_plane.dst_y, g_plane.dst_w, g_plane.dst_h), -1) < 0)
        return 0;
    int ok = send_msg_locked(CHAM_PRESENT, (uint32_t)slot, frame, now_ns(), in_fence) == 0;
    g_stats.presented += ok;
    return ok;
}

int link_app_cursor(void)
{
    const char *env = getenv("CHAMELEON_CURSOR_PLANE");
    if (env && *env)
        return *env == '1';
    pthread_mutex_lock(&g_lock);
    int yes = g_cfg_seen && (g_first_flags & CHAM_CONFIG_CURSOR);
    pthread_mutex_unlock(&g_lock);
    return yes;
}

/* Sends the cursor plane's state (g_cursor) to the presenter; new_image: it
 * must copy the buffer again. Takes over fence. */
static void send_cursor_locked(int new_image, int fence)
{
    struct cham_msg m = {CHAM_CURSOR, CHAM_CURSOR_HIDE, 0, 0};
    g_cursor_sent = 0;
    /* An app without it (CHAMELEON_CURSOR_PLANE=1) doesn't know the message;
     * one without a surface shows nothing, and may not be reading (Android
     * freezes apps in the background): it gets the cursor with its surface. */
    if (!g_cfg_cursor || !g_cfg_w || !g_cfg_h) {
        if (fence >= 0)
            close(fence);
        return;
    }
    if (g_cursor.bo) {
        new_image |= g_cursor.bo->slot < 0 || g_cursor.bo->slot_gen != g_gen || g_cursor_gen != g_gen;
        int slot = g_sock >= 0 ? slot_locked(g_cursor.bo) : -1;
        m.id = slot < 0 ? CHAM_CURSOR_HIDE : (uint32_t)slot | (new_image ? CHAM_CURSOR_NEW : 0);
        m.a = CHAM_INPUT_DELTA(g_cursor.x, g_cursor.y);
        m.b = CHAM_CURSOR_RECT(g_cursor.src_x, g_cursor.src_y, g_cursor.w, g_cursor.h);
    }
    if (g_sock >= 0 && cham_send(g_sock, &m, fence) == 0) {
        g_cursor_sent = 1;
        g_cursor_gen = g_gen;
    }
    if (fence >= 0)
        close(fence);
}

void link_cursor_locked(const struct link_cursor *c)
{
    const struct link_cursor *o = &g_cursor;
    int same = c->bo == o->bo && !c->new_image &&
               (!c->bo || (c->x == o->x && c->y == o->y && c->src_x == o->src_x && c->src_y == o->src_y &&
                           c->w == o->w && c->h == o->h));
    int new_image = c->new_image || c->bo != o->bo || !g_cursor_sent; /* unsent: its image too */
    g_cursor = *c;
    g_cursor.fence = -1;
    if (same && g_cursor_sent && g_cursor_gen == g_gen) {
        if (c->fence >= 0)
            close(c->fence);
        return;
    }
    send_cursor_locked(new_image, c->fence);
}

void link_plane_locked(const struct link_plane *p)
{
    g_plane = *p;
}

int link_shows_planes_locked(void)
{
    return g_cfg_plane;
}

void link_screen_off_locked(int off)
{
    send_msg_locked(CHAM_SCREEN, 0, off ? 1 : 0, 0, -1);
}

void link_forget_bo_locked(struct cham_bo *bo)
{
    if (g_cursor.bo == bo) {
        g_cursor.bo = NULL; /* the plane let go of it already; */
        g_cursor_sent = 0;  /* the app, which shows its copy, hears of it with the next state */
    }
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

/* The app's surface went away or came back (in the background, the screen
 * off, the app restarted): the log says how long it was gone, for reports of
 * what happened meanwhile. */
static void surface_changed_locked(int had_surface)
{
    int has_surface = g_cfg_w && g_cfg_h;
    if (had_surface && !has_surface) {
        g_surface_gone_ns = now_ns();
    } else if (!had_surface && has_surface && g_surface_gone_ns) {
        uint64_t s = (now_ns() - g_surface_gone_ns) / 1000000000ull;
        g_surface_gone_ns = 0;
        if (s >= 2) {
            cham_log("the app's window is back after %llu s", (unsigned long long)s);
            g_returned = 1;
        }
    }
}

static void handle_msg_locked(const struct cham_msg *m, int *fd)
{
    switch (m->type) {
    case CHAM_CONFIG: {
        int had_surface = g_cfg_w && g_cfg_h;
        g_cfg_w = CHAM_CONFIG_WIDTH(m);
        g_cfg_h = CHAM_CONFIG_HEIGHT(m);
        surface_changed_locked(had_surface);
        g_back_pressure = !!(m->id & CHAM_CONFIG_BACK_PRESSURE);
        g_cfg_cursor = !!(m->id & CHAM_CONFIG_CURSOR);
        g_cfg_plane = !!(m->id & CHAM_CONFIG_PLANE);
        if (g_cfg_w && g_cfg_h && !g_cfg_seen) {
            g_cfg_seen = 1;
            g_first_w = g_cfg_w;
            g_first_h = g_cfg_h;
            g_first_mhz = (uint32_t)m->b;
            g_first_flags = m->id;
            g_first_win_w = g_win_w;
            g_first_win_h = g_win_h;
        }
        pthread_cond_broadcast(&g_cond);
        if (g_cfg_w && g_cfg_h)
            input_post(m); /* the connector's mode follows the window (input.c) */
        /* The app's window is back: show it the compositor's current frame.
         * The compositor may have drawn its last frames while there was no
         * window (e.g. the screen size changed as the app went to the
         * background) and draws nothing new if nothing changes. Frame 0
         * completes no page flip. */
        struct cham_bo *shown = !had_surface && g_cfg_w && g_cfg_h ? kms_screen_bo_locked() : NULL;
        if (shown) {
            send_cursor_locked(1, -1); /* a new surface: the cursor's image again too */
            link_present_locked(shown, -1, 0);
        }
        break;
    }
    case CHAM_RELEASE:
        if (m->id < MAX_SLOTS && g_slots[m->id]) {
            struct cham_bo *bo = g_slots[m->id];
            if (bo->release_fence >= 0)
                close(bo->release_fence);
            bo->release_fence = *fd;
            *fd = -1;
        }
        break;
    case CHAM_FRAME_DONE: {
        /* b: when Android latched it; 0 if skipped. A transaction that changed
         * nothing on screen latches nothing, and Android reports -1 for it: a
         * time that isn't in the recent past is no latch time. */
        uint64_t now = now_ns(), latch = m->b <= now && now - m->b < 1000000000ull ? m->b : 0;
        if (latch) {
            g_latch_ns = latch;
            g_latches[g_latch_next++ % LATCHES] = latch;
        }
        if (m->a > g_latched_frame)
            g_latched_frame = m->a; /* latched, or skipped: not waiting in Android */
        if (g_flip.active && !g_flip.simulated && m->a >= g_flip.frame) {
            /* latched: complete it on the tick of that latch (in the past
             * by now, so at once), on the vblank thread */
            read_present_fences_locked();
            g_flip.on_tick = 1;
            g_flip.deadline_ns = latch ? tick_locked(latch, 1) : now;
            pthread_cond_broadcast(&g_cond);
        } else if (g_early)
            schedule_tick_locked(); /* the pending flip may have waited for this latch */
        break;
    }
    case CHAM_FRAME_QUEUED:
        if (m->a > g_copied_frame)
            g_copied_frame = m->a;
        if (g_early)
            schedule_tick_locked(); /* ...or for this copy */
        break;
    case CHAM_BYE:
        if (m->a == CHAM_BYE_REPLACED)
            g_replaced = 1;
        else if (m->a == CHAM_BYE_VERSION)
            g_app_version = m->b;
        break;
    case CHAM_WINDOW:
        g_win_w = CHAM_CONFIG_WIDTH(m);
        g_win_h = CHAM_CONFIG_HEIGHT(m);
        input_post(m); /* the touchpad's positions are fractions of it (evdev.c) */
        break;
    case CHAM_PRESENTED:
        if (*fd >= 0) {
            add_present_fence_locked(*fd);
            *fd = -1;
        }
        break;
    case CHAM_PING: {
        /* Answered under the lock every card request takes, so a stuck
         * service doesn't answer; the input thread asks the compositor the
         * same in turn (input.c), and the answer says how that stands. */
        send_msg_locked(CHAM_PONG, 0, m->a, input_compositor_wait_ms(), -1);
        struct cham_msg ask = *m;
        ask.id = (uint32_t)g_returned;
        g_returned = 0;
        input_post(&ask);
        break;
    }
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
        if (kms_screen_off_locked())
            link_screen_off_locked(1); /* the app shows "tap to wake" */
        memset(g_slots, 0, sizeof g_slots);
        pthread_mutex_unlock(&g_lock);
        cham_log("connected to the presenter (pid %d)", (int)getpid());

        for (;;) {
            struct cham_msg m;
            int fd = -1;
            if (cham_recv(s, &m, &fd) <= 0)
                break;
            if (m.type == CHAM_INPUT || m.type == CHAM_CLIPBOARD_OFFER) {
                /* for the input thread, in order with WINDOW and CONFIG
                 * (this thread posts those too); nothing here needs g_lock */
                input_post(&m);
                if (fd >= 0)
                    close(fd);
                continue;
            }
            pthread_mutex_lock(&g_lock);
            handle_msg_locked(&m, &fd);
            pthread_mutex_unlock(&g_lock);
            if (fd >= 0)
                close(fd);
        }

        pthread_mutex_lock(&g_lock);
        g_sock = -1;
        int had_surface = g_cfg_w && g_cfg_h;
        g_cfg_w = g_cfg_h = 0;
        surface_changed_locked(had_surface);
        close_present_fences_locked();
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
        uint64_t app_version = g_app_version;
        pthread_mutex_unlock(&g_lock);
        if (app_version) {
            cham_log("the Chameleon app speaks protocol %llu, this package %d: update both to the same "
                     "release, then start again",
                     (unsigned long long)app_version, CHAM_PROTO_VERSION);
            return NULL;
        }
        if (replaced) {
            cham_log("another producer (e.g. a second Chameleon session) took over the Chameleon app; "
                     "this session keeps running without a screen. Stop the other one and restart this one.");
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
    const char *early = getenv("CHAMELEON_EARLY_FLIP");
    g_early = !(early && *early == '0');
    const char *queue = getenv("CHAMELEON_ANDROID_QUEUE");
    g_android_queue = queue && *queue ? (atoi(queue) >= 2 ? 2 : 1) : 0;

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

void link_clipboard(uint32_t type, uint64_t a, int fd)
{
    pthread_mutex_lock(&g_lock);
    send_msg_locked(type, 0, a, 0, fd);
    pthread_mutex_unlock(&g_lock);
}


int link_wait_config(uint32_t *width, uint32_t *height, uint32_t *refresh_mhz, uint32_t *win_width,
                     uint32_t *win_height, int timeout_ms)
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
        *win_width = g_first_win_w;
        *win_height = g_first_win_h;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
