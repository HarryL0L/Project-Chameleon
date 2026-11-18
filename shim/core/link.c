/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Connection from chameleon-kernel to the Chameleon presenter app: one per
 * screen of the card (struct link), each with the threads and state below.
 *
 * - One reader thread (re)connects to $PREFIX/tmp/chameleon-0 and handles
 *   CONFIG, RELEASE, FRAME_QUEUED and FRAME_DONE; input goes on to the input
 *   thread without g_lock.
 * - Buffers are registered with the presenter lazily, on first present.
 * - At most one page flip is pending, as with KMS. It completes on a vblank
 *   tick once the presenter copied the previous frame (FRAME_QUEUED) and
 *   Android latched enough frames (FRAME_DONE), see L->flip; or on a
 *   simulated vblank when nothing is on screen (app closed or paused), so
 *   the compositor keeps running either way.
 * - Vblank ticks follow the display's vsync, from Android's present fences
 *   (CHAM_PRESENTED), see "the vsync grid".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
#define PRESENT_FENCES 4
#define LATCHES 16

/* One link per screen of the card: screen 0 is the app's main window, a
 * further one a further window of it (its own process there, speaking the
 * same protocol on a connection of its own). */
struct link {
    int screen;
    int sock;
    uint64_t gen;                   /* bumped per presenter connection */
    struct cham_bo *slots[MAX_SLOTS];
    uint32_t cfg_w, cfg_h;
    int cfg_seen;                   /* a non-zero CONFIG ever arrived */
    uint64_t surface_gone_ns;       /* when the app's surface went away (0: it has one, or never had) */
    int returned;                   /* it is back: the next PING's answer from the compositor is logged */
    uint32_t first_w, first_h, first_mhz, first_flags; /* ...and what it said */
    uint32_t win_w, win_h, first_win_w, first_win_h; /* the app's whole window (CHAM_WINDOW) */
    int replaced;                   /* another producer took the app over */
    uint64_t app_version;           /* the app speaks another protocol version (CHAM_BYE_VERSION) */

    struct {
        int active;
        int simulated;              /* complete on a timer, not FRAME_DONE */
        uint64_t frame;
        uint64_t user_data;
        uint32_t crtc_id;
        int event_fd;
        uint64_t deadline_ns;       /* simulated: when; real: watchdog, or the vblank tick it completes on */
        uint64_t queued_ns;         /* when the compositor committed */
        uint64_t period_ns;
        int on_tick;                /* completes at deadline_ns, a vblank tick */
        int held;                   /* waited for a copy or a latch */
        uint64_t prev_frame, prev2_frame; /* the frames shown before it, if any */
        uint64_t seq;               /* bumped per queued flip */
        int grid;                   /* deadline_ns is on the display's vsync grid */
    } flip;
    int back_pressure;              /* CHAM_CONFIG_BACK_PRESSURE */
    int queue_logged;
    uint64_t latch_ns;              /* last latch: the vblank grid's phase */
    uint64_t latched_frame;         /* newest frame Android has latched (or skipped) */
    uint64_t copied_frame;          /* newest frame the app has copied (FRAME_QUEUED) */
    uint64_t presented_frame, presented_prev; /* newest frames handed to the app */
    uint32_t sequence;
    uint64_t last_flip_ns;          /* the last page-flip event's time */

    /* Activity counters, logged every 5 s while something happens. */
    struct {
        unsigned commits, presented, not_shown, flips, simulated;
        uint64_t wait_sum, wait_max; /* commit to shown, shown flips */
        unsigned held;              /* flips that waited for a copy or a latch */
        unsigned second;            /* flips only a second frame in Android let through */
        unsigned on_vsync;          /* flips on the display's vsync grid */
        uint64_t since;
    } stats;

    /* The cursor plane as last committed, and whether the presenter has it. */
    struct link_cursor cursor;
    int cursor_sent;
    uint64_t cursor_gen;
    int cfg_cursor;                 /* this app shows it (CHAM_CONFIG_CURSOR) */
    /* The primary plane's place, and whether this app shows one that isn't the
     * whole screen (CHAM_CONFIG_PLANE). */
    struct link_plane plane;
    int cfg_plane;

    /* the vsync grid, see there */
    int present_fd[PRESENT_FENCES]; /* not signalled yet, oldest first */
    int present_fds;
    uint64_t vsync_ns;              /* newest vsync seen */
    uint64_t vsync_ref_ns;          /* an older one: the period is measured from it */
    uint64_t vsync_period;          /* measured, 0 until known */
    uint64_t mode_period;           /* the mode's (the compositor's) refresh period */
    uint64_t latches[LATCHES];      /* recent latch times */
    unsigned latch_next;
    uint64_t tick_phase;            /* ticks: at a vsync + this, ... */
    uint64_t tick_phase_period;     /* ...on a grid of this period (0: none yet) */
    uint64_t asked_ns;              /* when the app was asked for this screen's window (0: not, or it came) */
};
#define LINK(n) {.screen = n, .sock = -1, .cursor = {.fence = -1}, .plane = {.plain = 1}}
static struct link g_links[CHAM_SCREENS] = {LINK(0), LINK(1), LINK(2), LINK(3)};
static int g_screens = 1;           /* links in use */
static pthread_cond_t g_cond;

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

static void stats_maybe_log_locked(struct link *L)
{
    uint64_t now = now_ns();
    if (!L->stats.since)
        L->stats.since = now;
    if (now - L->stats.since < 5000000000ull)
        return;
    if (L->stats.commits || L->stats.flips) {
        unsigned shown = L->stats.flips - L->stats.simulated;
        char which[24] = "";
        if (L->screen)
            snprintf(which, sizeof which, "screen %d, ", L->screen + 1);
        cham_log("%slast 5 s: %u commits, %u presented, %u not shown (no app surface), %u flips (%u simulated); "
                 "commit to shown %.1f ms avg, %.1f max; %u waited for Android, %u used a second frame there; "
                 "%u on the display's vsync",
                 which, L->stats.commits, L->stats.presented, L->stats.not_shown, L->stats.flips, L->stats.simulated,
                 shown ? L->stats.wait_sum / shown / 1e6 : 0.0, L->stats.wait_max / 1e6, L->stats.held,
                 L->stats.second, L->stats.on_vsync);
    }
    memset(&L->stats, 0, sizeof L->stats);
    L->stats.since = now;
}

/* Where a screen's window is reached: the first at the socket itself, a
 * further one at its name with ".N" (its number, from 2) added. */
static const char *socket_path(const struct link *L)
{
    static char paths[CHAM_SCREENS][108];
    const char *p = getenv("CHAMELEON_SOCKET");
    if (!p)
        p = CHAM_SOCKET_PATH;
    if (!L->screen)
        return p;
    snprintf(paths[L->screen], sizeof paths[0], "%s.%d", p, L->screen + 1);
    return paths[L->screen];
}

static void send_cursor_locked(struct link *L, int new_image, int fence);

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
#define GRID_MAX_AGE_NS 500000000ull     /* older vsyncs and latches aren't used */

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

static void vsync_seen_locked(struct link *L, uint64_t t)
{
    uint64_t p = L->mode_period;
    if (!p || t <= L->vsync_ns)
        return;
    if (!L->vsync_ref_ns || t - L->vsync_ref_ns > 4000000000ull)
        L->vsync_ref_ns = t; /* a fresh start: the mode may have changed meanwhile */
    else if (t - L->vsync_ref_ns >= 200000000ull) {
        /* The refreshes since the reference, counted with the period
         * measured so far: a panel a little off its mode's rate is whole
         * refreshes off the mode's count after a few seconds. */
        uint64_t q = L->vsync_period ? L->vsync_period : p;
        uint64_t n = (t - L->vsync_ref_ns + q / 2) / q, est = n ? (t - L->vsync_ref_ns) / n : 0;
        if (est > p - p / 20 && est < p + p / 20)
            L->vsync_period = est;
        else if (n)
            L->vsync_ref_ns = t, L->vsync_period = 0; /* the display runs at another rate */
    }
    if (!L->vsync_ns && !L->screen)
        cham_log("vblank ticks follow the display's vsync (Android's present fences)");
    L->vsync_ns = t;
}

static void read_present_fences_locked(struct link *L)
{
    int keep = 0;
    for (int i = 0; i < L->present_fds; i++) {
        uint64_t t;
        int r = fence_time(L->present_fd[i], &t);
        if (r == 0) {
            L->present_fd[keep++] = L->present_fd[i];
            continue;
        }
        close(L->present_fd[i]);
        if (r > 0)
            vsync_seen_locked(L, t);
    }
    L->present_fds = keep;
}

static void add_present_fence_locked(struct link *L, int fd)
{
    read_present_fences_locked(L);
    if (L->present_fds == PRESENT_FENCES) { /* never signalled: drop the oldest */
        close(L->present_fd[0]);
        memmove(L->present_fd, L->present_fd + 1, sizeof L->present_fd - sizeof L->present_fd[0]);
        L->present_fds--;
    }
    L->present_fd[L->present_fds++] = fd;
}

static void close_present_fences_locked(struct link *L)
{
    for (int i = 0; i < L->present_fds; i++)
        close(L->present_fd[i]);
    L->present_fds = 0;
}

/* t's place in a refresh of period p that starts at `base`, in [0, p). */
static uint64_t phase_of(uint64_t t, uint64_t base, uint64_t p)
{
    int64_t r = (int64_t)(t - base) % (int64_t)p;
    return (uint64_t)(r < 0 ? r + (int64_t)p : r);
}

/* The grid: ticks at anchor + k * period. 1: on the display's vsync; 0: on
 * the last latch (no vsyncs known); -1: none yet. */
static int grid_locked(struct link *L, uint64_t now, uint64_t *anchor, uint64_t *period)
{
    uint64_t p = L->mode_period, v = L->vsync_period;
    int measured = v > p - p / 20 && v < p + p / 20; /* ...and still the mode's rate */
    if (measured)
        p = v;
    /* Only with the display's measured period: the mode's is a little off
     * it, which puts the latches of earlier refreshes in the wrong place. */
    if (p && measured && L->vsync_ns && (now > L->vsync_ns ? now - L->vsync_ns : 0) < GRID_MAX_AGE_NS) {
        /* where in the refresh the recent latches fall: they come late by
         * varying amounts, never early, so the stretch they cover starts
         * after the widest gap between them */
        uint64_t ph[LATCHES];
        int n = 0;
        for (int i = 0; i < LATCHES; i++) {
            uint64_t l = L->latches[i];
            if (!l || (l < L->vsync_ns ? L->vsync_ns - l : l - L->vsync_ns) > GRID_MAX_AGE_NS)
                continue;
            uint64_t v = phase_of(l, L->vsync_ns, p);
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
            uint64_t later = phase_of(start, L->tick_phase, p), q = L->tick_phase_period;
            if (q < p - p / 100 || q > p + p / 100 || later > 1000000) {
                L->tick_phase = start;
                L->tick_phase_period = p;
            }
            *anchor = L->vsync_ns + L->tick_phase;
            *period = p;
            return 1;
        }
    }
    if (L->latch_ns && L->flip.period_ns) {
        *anchor = L->latch_ns;
        *period = L->flip.period_ns;
        return 0;
    }
    return -1;
}

/* The tick a flip completes on: the first after t (nearest: the one nearest
 * t), and on the vsync grid never at or before the last page flip's. */
static uint64_t tick_locked(struct link *L, uint64_t t, int nearest)
{
    uint64_t a, p;
    int grid = grid_locked(L, now_ns(), &a, &p);
    if (grid < 0 || (grid == 0 && nearest))
        return t; /* no grid; or as before: at the latch itself */
    int64_t d = (int64_t)(t - a), k = d / (int64_t)p;
    if (d < 0 && k * (int64_t)p != d)
        k--; /* floor */
    uint64_t tick = a + (uint64_t)(k * (int64_t)p);
    if (nearest ? t - tick > p / 2 : tick <= t)
        tick += p;
    if (grid == 1) {
        while (L->last_flip_ns && tick < L->last_flip_ns + p / 2)
            tick += p;
        L->flip.grid = 1;
    }
    return tick;
}

/* ---- page flips ---- */

static void emit_flip_locked(struct link *L, uint64_t ts_ns)
{
    struct drm_event_vblank ev = {
        .base = {.type = DRM_EVENT_FLIP_COMPLETE, .length = sizeof ev},
        .user_data = L->flip.user_data,
        .tv_sec = (uint32_t)(ts_ns / 1000000000ull),
        .tv_usec = (uint32_t)(ts_ns % 1000000000ull / 1000),
        .sequence = ++L->sequence,
        .crtc_id = L->flip.crtc_id,
    };
    if (write(L->flip.event_fd, &ev, sizeof ev) != (ssize_t)sizeof ev)
        cham_log("cannot deliver page-flip event: %s", strerror(errno));
    L->flip.active = 0;
    L->flip.on_tick = L->flip.held = 0;
    L->last_flip_ns = ts_ns;
    L->stats.on_vsync += L->flip.grid;
    L->flip.grid = 0;
    L->stats.flips++;
    L->stats.simulated += L->flip.simulated;
    if (!L->flip.simulated) {
        uint64_t wait = ts_ns > L->flip.queued_ns ? ts_ns - L->flip.queued_ns : 0;
        L->stats.wait_sum += wait;
        if (wait > L->stats.wait_max)
            L->stats.wait_max = wait;
    }
    pthread_cond_broadcast(&g_cond);
}

int link_flip_pending_locked(int screen)
{
    return g_links[screen].flip.active;
}

/* Completes the pending flip (the vblank thread). Copy mode: the presenter
 * released our buffers with the fences of its blits; they must signal
 * first, so the compositor can't render into a buffer still being copied.
 * They are waited for without g_lock, which the compositor's requests and
 * the app's messages need meanwhile. */
static void complete_flip_locked(struct link *L, uint64_t ts_ns)
{
    int fences[MAX_SLOTS];
    int n = bo_take_release_fences_locked(L->screen, fences, MAX_SLOTS);
    if (n) {
        uint64_t seq = L->flip.seq;
        pthread_mutex_unlock(&g_lock);
        for (int i = 0; i < n; i++) {
            struct pollfd pfd = {fences[i], POLLIN, 0};
            poll(&pfd, 1, 100);
            close(fences[i]);
        }
        pthread_mutex_lock(&g_lock);
        if (!L->flip.active || L->flip.seq != seq)
            return; /* cancelled meanwhile (its file closed) */
    }
    emit_flip_locked(L, ts_ns);
}

void link_cancel_flip_locked(int event_fd)
{
    for (struct link *L = g_links; L < g_links + g_screens; L++) {
        if (L->flip.active && L->flip.event_fd == event_fd) {
            L->flip.active = 0;
            L->flip.on_tick = L->flip.held = 0;
            pthread_cond_broadcast(&g_cond);
        }
    }
}

/* Completes the pending flip on the next vblank tick once the previous
 * copy is done and Android has latched what it should have by now. */
static void schedule_tick_locked(struct link *L)
{
    if (!L->flip.active || L->flip.simulated || L->flip.on_tick)
        return;
    uint64_t copied = L->copied_frame > L->latched_frame ? L->copied_frame : L->latched_frame;
    int queue = g_android_queue ? g_android_queue : L->back_pressure ? 2 : 1;
    if (queue != L->queue_logged) {
        if (!L->screen)
            cham_log("up to %d frame%s may wait in Android", queue, queue > 1 ? "s" : "");
        L->queue_logged = queue;
    }
    uint64_t must_latch = queue >= 2 ? L->flip.prev2_frame : L->flip.prev_frame;
    if (copied < L->flip.prev_frame || L->latched_frame < must_latch) {
        L->stats.held += !L->flip.held; /* once per flip */
        L->flip.held = 1;
        return;
    }
    L->stats.second += L->latched_frame < L->flip.prev_frame; /* the previous frame still waits */
    read_present_fences_locked(L);
    L->flip.on_tick = 1;
    L->flip.deadline_ns = tick_locked(L, now_ns(), 0);
    pthread_cond_broadcast(&g_cond);
}

void link_queue_flip_locked(int screen, uint64_t user_data, uint32_t crtc_id, int event_fd, uint64_t frame,
                            int presented, uint32_t refresh_mhz)
{
    struct link *L = &g_links[screen];
    uint64_t period = 1000000000000ull / (refresh_mhz ? refresh_mhz : 60000);
    L->flip.active = 1;
    L->flip.seq++;
    L->flip.simulated = !presented;
    L->flip.frame = frame;
    L->flip.user_data = user_data;
    L->flip.crtc_id = crtc_id;
    L->flip.event_fd = event_fd;
    /* Simulated vblank one period out; a real flip gets a generous watchdog
     * in case the presenter stalls (e.g. the app got frozen). */
    L->flip.queued_ns = now_ns();
    L->flip.deadline_ns = L->flip.queued_ns + (presented ? 250000000ull : period);
    L->flip.period_ns = period;
    L->flip.on_tick = L->flip.held = 0;
    L->flip.grid = 0;
    if (period != L->mode_period) {
        /* another refresh rate: what was measured at the old one doesn't apply */
        L->mode_period = period;
        L->vsync_ref_ns = L->vsync_period = L->tick_phase_period = 0;
        memset(L->latches, 0, sizeof L->latches);
    }
    if (presented) {
        L->flip.prev2_frame = L->presented_prev;
        L->flip.prev_frame = L->presented_frame;
        L->presented_prev = L->presented_frame;
        L->presented_frame = frame;
    }
    pthread_cond_broadcast(&g_cond);
    if (g_early)
        schedule_tick_locked(L);
}

static void *vblank_main(void *arg)
{
    struct link *L = arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        stats_maybe_log_locked(L);
        if (!L->flip.active) {
            struct timespec idle;
            timespec_at(&idle, now_ns() + 1000000000ull);
            pthread_cond_timedwait(&g_cond, &g_lock, &idle);
            continue;
        }
        uint64_t now = now_ns();
        if (now >= L->flip.deadline_ns) {
            if (L->flip.on_tick) {
                complete_flip_locked(L, L->flip.deadline_ns);
                continue;
            }
            if (!L->flip.simulated)
                cham_log("presenter did not report frame %llu, completing the flip anyway",
                         (unsigned long long)L->flip.frame);
            complete_flip_locked(L, now);
            continue;
        }
        struct timespec ts;
        timespec_at(&ts, L->flip.deadline_ns);
        pthread_cond_timedwait(&g_cond, &g_lock, &ts);
    }
    return NULL;
}

/* ---- presenting ---- */

static int send_msg_locked(struct link *L, uint32_t type, uint32_t id, uint64_t a, uint64_t b, int fd)
{
    struct cham_msg msg = {type, id, a, b};
    return L->sock >= 0 ? cham_send(L->sock, &msg, fd) : -ENOTCONN;
}

/* The presenter's id for `bo`, registered with it now if it has none on this
 * connection; -1 if that fails. */
static int slot_locked(struct link *L, struct cham_bo *bo)
{
    if (bo->slot[L->screen] >= 0 && bo->slot_gen[L->screen] == L->gen)
        return bo->slot[L->screen];
    int slot = -1;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!L->slots[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        cham_log("presenter buffer table full");
        return -1;
    }
    if (send_msg_locked(L, CHAM_BUFFER_ADD, (uint32_t)slot, 0, 0, -1) < 0 || ahb_send(bo->ahb, L->sock) != 0) {
        cham_log("cannot send buffer to the presenter");
        return -1;
    }
    L->slots[slot] = bo;
    bo->slot[L->screen] = slot;
    bo->slot_gen[L->screen] = L->gen;
    return slot;
}

int link_present_locked(int screen, struct cham_bo *bo, int in_fence, uint64_t frame)
{
    struct link *L = &g_links[screen];
    L->stats.commits++;
    if (L->sock < 0 || !L->cfg_w || !L->cfg_h) {
        L->stats.not_shown++;
        return 0;
    }
    int slot = bo ? slot_locked(L, bo) : (int)CHAM_PRESENT_UNCHANGED;
    if (bo && slot < 0)
        return 0;
    if (bo && !L->plane.plain && L->cfg_plane &&
        send_msg_locked(L, CHAM_PLANE, L->plane.crtc_w | L->plane.crtc_h << 16,
                        CHAM_CURSOR_RECT(L->plane.src_x, L->plane.src_y, L->plane.src_w, L->plane.src_h),
                        CHAM_CURSOR_RECT(L->plane.dst_x, L->plane.dst_y, L->plane.dst_w, L->plane.dst_h), -1) < 0)
        return 0;
    int ok = send_msg_locked(L, CHAM_PRESENT, (uint32_t)slot, frame, now_ns(), in_fence) == 0;
    L->stats.presented += ok;
    return ok;
}

int link_app_cursor(void)
{
    const char *env = getenv("CHAMELEON_CURSOR_PLANE");
    if (env && *env)
        return *env == '1';
    pthread_mutex_lock(&g_lock);
    const struct link *L = &g_links[0]; /* the card is made as the first window's app allows */
    int yes = L->cfg_seen && (L->first_flags & CHAM_CONFIG_CURSOR);
    pthread_mutex_unlock(&g_lock);
    return yes;
}

/* Sends the cursor plane's state (L->cursor) to the presenter; new_image: it
 * must copy the buffer again. Takes over fence. */
static void send_cursor_locked(struct link *L, int new_image, int fence)
{
    struct cham_msg m = {CHAM_CURSOR, CHAM_CURSOR_HIDE, 0, 0};
    L->cursor_sent = 0;
    /* An app without it (CHAMELEON_CURSOR_PLANE=1) doesn't know the message;
     * one without a surface shows nothing, and may not be reading (Android
     * freezes apps in the background): it gets the cursor with its surface. */
    if (!L->cfg_cursor || !L->cfg_w || !L->cfg_h) {
        if (fence >= 0)
            close(fence);
        return;
    }
    if (L->cursor.bo) {
        new_image |= L->cursor.bo->slot[L->screen] < 0 || L->cursor.bo->slot_gen[L->screen] != L->gen || L->cursor_gen != L->gen;
        int slot = L->sock >= 0 ? slot_locked(L, L->cursor.bo) : -1;
        m.id = slot < 0 ? CHAM_CURSOR_HIDE : (uint32_t)slot | (new_image ? CHAM_CURSOR_NEW : 0);
        m.a = CHAM_INPUT_DELTA(L->cursor.x, L->cursor.y);
        m.b = CHAM_CURSOR_RECT(L->cursor.src_x, L->cursor.src_y, L->cursor.w, L->cursor.h);
    }
    if (L->sock >= 0 && cham_send(L->sock, &m, fence) == 0) {
        L->cursor_sent = 1;
        L->cursor_gen = L->gen;
    }
    if (fence >= 0)
        close(fence);
}

void link_cursor_locked(int screen, const struct link_cursor *c)
{
    struct link *L = &g_links[screen];
    const struct link_cursor *o = &L->cursor;
    int same = c->bo == o->bo && !c->new_image &&
               (!c->bo || (c->x == o->x && c->y == o->y && c->src_x == o->src_x && c->src_y == o->src_y &&
                           c->w == o->w && c->h == o->h));
    int new_image = c->new_image || c->bo != o->bo || !L->cursor_sent; /* unsent: its image too */
    L->cursor = *c;
    L->cursor.fence = -1;
    if (same && L->cursor_sent && L->cursor_gen == L->gen) {
        if (c->fence >= 0)
            close(c->fence);
        return;
    }
    send_cursor_locked(L, new_image, c->fence);
}

void link_plane_locked(int screen, const struct link_plane *p)
{
    g_links[screen].plane = *p;
}

int link_shows_planes_locked(int screen)
{
    return g_links[screen].cfg_plane;
}

void link_screen_off_locked(int screen, int off)
{
    send_msg_locked(&g_links[screen], CHAM_SCREEN, 0, off ? 1 : 0, 0, -1);
}

void link_window_locked(int screen, uint32_t *width, uint32_t *height)
{
    *width = g_links[screen].win_w;
    *height = g_links[screen].win_h;
}

void link_forget_bo_locked(struct cham_bo *bo)
{
    for (struct link *L = g_links; L < g_links + CHAM_SCREENS; L++) {
        if (L->cursor.bo == bo) {
            L->cursor.bo = NULL; /* the plane let go of it already; */
            L->cursor_sent = 0;  /* the app, which shows its copy, hears of it with the next state */
        }
        if (bo->slot[L->screen] >= 0 && bo->slot_gen[L->screen] == L->gen) {
            L->slots[bo->slot[L->screen]] = NULL;
            send_msg_locked(L, CHAM_BUFFER_REMOVE, (uint32_t)bo->slot[L->screen], 0, 0, -1);
        }
        bo->slot[L->screen] = -1;
    }
}

/* ---- reader thread ---- */

static int connect_presenter(struct link *L)
{
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, socket_path(L), sizeof addr.sun_path - 1);
    if (s >= 0 && connect(s, (struct sockaddr *)&addr, sizeof addr) == 0)
        return s;
    if (s >= 0)
        close(s);
    return -1;
}

/* The app's surface went away or came back (in the background, the screen
 * off, the app restarted): the log says how long it was gone, for reports of
 * what happened meanwhile. */
static void surface_changed_locked(struct link *L, int had_surface)
{
    int has_surface = L->cfg_w && L->cfg_h;
    if (had_surface && !has_surface) {
        L->surface_gone_ns = now_ns();
    } else if (!had_surface && has_surface && L->surface_gone_ns) {
        uint64_t s = (now_ns() - L->surface_gone_ns) / 1000000000ull;
        L->surface_gone_ns = 0;
        if (s >= 2 && !L->screen) {
            cham_log("the app's window is back after %llu s", (unsigned long long)s);
            L->returned = 1;
        }
    }
}

static void handle_msg_locked(struct link *L, const struct cham_msg *m, int *fd)
{
    switch (m->type) {
    case CHAM_CONFIG: {
        int had_surface = L->cfg_w && L->cfg_h;
        L->cfg_w = CHAM_CONFIG_WIDTH(m);
        L->cfg_h = CHAM_CONFIG_HEIGHT(m);
        surface_changed_locked(L, had_surface);
        L->back_pressure = !!(m->id & CHAM_CONFIG_BACK_PRESSURE);
        L->cfg_cursor = !!(m->id & CHAM_CONFIG_CURSOR);
        L->cfg_plane = !!(m->id & CHAM_CONFIG_PLANE);
        if (L->cfg_w && L->cfg_h && !L->cfg_seen) {
            L->cfg_seen = 1;
            L->first_w = L->cfg_w;
            L->first_h = L->cfg_h;
            L->first_mhz = (uint32_t)m->b;
            L->first_flags = m->id;
            L->first_win_w = L->win_w;
            L->first_win_h = L->win_h;
        }
        pthread_cond_broadcast(&g_cond);
        if (L->cfg_w && L->cfg_h) {
            /* the connector's mode follows the window (input.c); a further
             * screen's connector is plugged in by its window's first size */
            struct cham_msg panel = *m;
            if (L->screen)
                panel.type = LINK_SCREEN_PANEL, panel.id = (uint32_t)L->screen;
            input_post(&panel);
        }
        /* The app's window is back: show it the compositor's current frame.
         * The compositor may have drawn its last frames while there was no
         * window (e.g. the screen size changed as the app went to the
         * background) and draws nothing new if nothing changes. Frame 0
         * completes no page flip. */
        struct cham_bo *shown = !had_surface && L->cfg_w && L->cfg_h ? kms_screen_bo_locked(L->screen) : NULL;
        if (shown) {
            send_cursor_locked(L, 1, -1); /* a new surface: the cursor's image again too */
            link_present_locked(L->screen, shown, -1, 0);
        }
        break;
    }
    case CHAM_RELEASE:
        if (m->id < MAX_SLOTS && L->slots[m->id]) {
            struct cham_bo *bo = L->slots[m->id];
            if (bo->release_fence[L->screen] >= 0)
                close(bo->release_fence[L->screen]);
            bo->release_fence[L->screen] = *fd;
            *fd = -1;
        }
        break;
    case CHAM_FRAME_DONE: {
        /* b: when Android latched it; 0 if skipped. A transaction that changed
         * nothing on screen latches nothing, and Android reports -1 for it: a
         * time that isn't in the recent past is no latch time. */
        uint64_t now = now_ns(), latch = m->b <= now && now - m->b < 1000000000ull ? m->b : 0;
        if (latch) {
            L->latch_ns = latch;
            L->latches[L->latch_next++ % LATCHES] = latch;
        }
        if (m->a > L->latched_frame)
            L->latched_frame = m->a; /* latched, or skipped: not waiting in Android */
        if (L->flip.active && !L->flip.simulated && m->a >= L->flip.frame) {
            /* latched: complete it on the tick of that latch (in the past
             * by now, so at once), on the vblank thread */
            read_present_fences_locked(L);
            L->flip.on_tick = 1;
            L->flip.deadline_ns = latch ? tick_locked(L, latch, 1) : now;
            pthread_cond_broadcast(&g_cond);
        } else if (g_early)
            schedule_tick_locked(L); /* the pending flip may have waited for this latch */
        break;
    }
    case CHAM_FRAME_QUEUED:
        if (m->a > L->copied_frame)
            L->copied_frame = m->a;
        if (g_early)
            schedule_tick_locked(L); /* ...or for this copy */
        break;
    case CHAM_BYE:
        if (m->a == CHAM_BYE_REPLACED)
            L->replaced = 1;
        else if (m->a == CHAM_BYE_VERSION)
            L->app_version = m->b;
        break;
    case CHAM_WINDOW:
        L->win_w = CHAM_CONFIG_WIDTH(m);
        L->win_h = CHAM_CONFIG_HEIGHT(m);
        if (!L->screen)
            input_post(m); /* the touchpad's positions are fractions of it (evdev.c) */
        break;
    case CHAM_PRESENTED:
        if (*fd >= 0) {
            add_present_fence_locked(L, *fd);
            *fd = -1;
        }
        break;
    case CHAM_PING: {
        /* Answered under the lock every card request takes, so a stuck
         * service doesn't answer; the input thread asks the compositor the
         * same in turn (input.c), and the answer says how that stands. */
        send_msg_locked(L, CHAM_PONG, 0, m->a, input_compositor_wait_ms(), -1);
        struct cham_msg ask = *m;
        ask.id = (uint32_t)L->returned;
        L->returned = 0;
        input_post(&ask);
        break;
    }
    }
}

static void *reader_main(void *arg)
{
    struct link *L = arg;
    int warned = 0;
    for (;;) {
        int s = connect_presenter(L);
        if (s < 0) {
            /* a further screen's window is opened when the user wants it */
            if (!warned++ && !L->screen)
                cham_log("waiting for the Chameleon app at %s", socket_path(L));
            sleep(1);
            continue;
        }
        warned = 0;
        /* The app shows the way to a further window only where there is a
         * screen for it (an older app doesn't know the flag). */
        struct cham_msg hello = {CHAM_HELLO, 0, CHAM_PROTO_VERSION,
                                 CHAM_HELLO_COPY | (g_screens > 1 && !L->screen ? CHAM_HELLO_SCREENS : 0)};
        cham_send(s, &hello, -1);

        pthread_mutex_lock(&g_lock);
        L->sock = s;
        L->asked_ns = 0;
        L->gen++;
        if (kms_screen_off_locked(L->screen))
            link_screen_off_locked(L->screen, 1); /* the app shows "tap to wake" */
        memset(L->slots, 0, sizeof L->slots);
        pthread_mutex_unlock(&g_lock);
        if (L->screen)
            cham_log("connected to the app's window for screen %d", L->screen + 1);
        else
            cham_log("connected to the presenter (pid %d)", (int)getpid());

        for (;;) {
            struct cham_msg m;
            int fd = -1;
            if (cham_recv(s, &m, &fd) <= 0)
                break;
            if (m.type == CHAM_INPUT || m.type == CHAM_CLIPBOARD_OFFER) {
                /* for the input thread, in order with WINDOW and CONFIG
                 * (this thread posts those too); nothing here needs g_lock.
                 * Touches go to their screen's touchscreen; the absolute
                 * pointer and the clipboard are the first window's. */
                int touch = m.type == CHAM_INPUT && m.id >= CHAM_INPUT_TOUCH_DOWN && m.id <= CHAM_INPUT_TOUCH_FRAME;
                if (touch)
                    m.a = (uint32_t)m.a | (uint64_t)L->screen << 32; /* LINK_INPUT_SCREEN */
                if (!L->screen || (m.type == CHAM_INPUT && m.id != CHAM_INPUT_POINTER_ABS))
                    input_post(&m);
                if (fd >= 0)
                    close(fd);
                continue;
            }
            pthread_mutex_lock(&g_lock);
            handle_msg_locked(L, &m, &fd);
            pthread_mutex_unlock(&g_lock);
            if (fd >= 0)
                close(fd);
        }

        pthread_mutex_lock(&g_lock);
        L->sock = -1;
        int had_surface = L->cfg_w && L->cfg_h;
        L->cfg_w = L->cfg_h = 0;
        surface_changed_locked(L, had_surface);
        close_present_fences_locked(L);
        memset(L->slots, 0, sizeof L->slots);
        if (L->flip.active && !L->flip.simulated) {
            L->flip.simulated = 1;
            L->flip.deadline_ns = now_ns();
            pthread_cond_broadcast(&g_cond);
        }
        pthread_mutex_unlock(&g_lock);
        close(s);
        pthread_mutex_lock(&g_lock);
        int replaced = L->replaced;
        uint64_t app_version = L->app_version;
        pthread_mutex_unlock(&g_lock);
        if (L->screen) {
            /* its window is closed: the monitor is unplugged (input.c) */
            struct cham_msg gone = {LINK_SCREEN_PANEL, (uint32_t)L->screen, 0, 0};
            input_post(&gone);
            cham_log("the app's window for screen %d is gone", L->screen + 1);
        }
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
        if (!L->screen)
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

    for (struct link *L = g_links; L < g_links + g_screens; L++) {
        pthread_t t;
        pthread_create(&t, NULL, reader_main, L);
        pthread_detach(t);
        pthread_create(&t, NULL, vblank_main, L);
        pthread_detach(t);
    }
}

void link_start(int screens)
{
    if (screens > 1)
        g_screens = screens <= CHAM_SCREENS ? screens : CHAM_SCREENS; /* before the threads: only read after */
    pthread_once(&g_once, start_threads);
}

int link_screen_open(void)
{
    int ret = -EBUSY;
    pthread_mutex_lock(&g_lock);
    uint64_t now = now_ns();
    for (struct link *L = g_links + 1; L < g_links + g_screens; L++) {
        /* One asked for a while ago that never came (the app couldn't open
         * it) is free again. */
        if (L->sock >= 0 || (L->asked_ns && now - L->asked_ns < 10000000000ull))
            continue;
        ret = send_msg_locked(&g_links[0], CHAM_WINDOW_OPEN, 0, (uint64_t)L->screen, 0, -1) == 0 ? L->screen
                                                                                                  : -ENOTCONN;
        if (ret > 0)
            L->asked_ns = now;
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return ret;
}

int link_screen_close(int screen)
{
    if (screen < 1 || screen >= g_screens)
        return -EINVAL;
    pthread_mutex_lock(&g_lock);
    int ret = send_msg_locked(&g_links[screen], CHAM_WINDOW_CLOSE, 0, 0, 0, -1) == 0 ? 0 : -ENOTCONN;
    pthread_mutex_unlock(&g_lock);
    return ret;
}

int link_screen_title(int screen, const char *text)
{
    int p[2];
    if (screen < 0 || screen >= g_screens)
        return -EINVAL;
    if (pipe2(p, O_CLOEXEC) != 0)
        return -errno;
    ssize_t n = write(p[1], text, strlen(text)); /* short: it fits a pipe */
    close(p[1]);
    pthread_mutex_lock(&g_lock);
    int ret = n >= 0 && send_msg_locked(&g_links[screen], CHAM_TITLE, 0, 0, 0, p[0]) == 0 ? 0 : -ENOTCONN;
    pthread_mutex_unlock(&g_lock);
    close(p[0]);
    return ret;
}

void link_clipboard(uint32_t type, uint64_t a, int fd)
{
    pthread_mutex_lock(&g_lock);
    send_msg_locked(&g_links[0], type, 0, a, 0, fd); /* Android's clipboard: the first window's side */
    pthread_mutex_unlock(&g_lock);
}


int link_wait_config(uint32_t *width, uint32_t *height, uint32_t *refresh_mhz, uint32_t *win_width,
                     uint32_t *win_height, int timeout_ms)
{
    struct timespec ts;
    timespec_at(&ts, now_ns() + (uint64_t)timeout_ms * 1000000ull);
    pthread_mutex_lock(&g_lock);
    const struct link *L = &g_links[0];
    while (!L->cfg_seen) {
        if (pthread_cond_timedwait(&g_cond, &g_lock, &ts) == ETIMEDOUT)
            break;
    }
    int ok = L->cfg_seen;
    if (ok) {
        *width = L->first_w;
        *height = L->first_h;
        *refresh_mhz = L->first_mhz ? L->first_mhz : 60000;
        *win_width = L->first_win_w;
        *win_height = L->first_win_h;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
