/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A card with two screens (CHAMELEON_SCREENS=2) against the real libdrm: the
 * second connector is a second window of the app, plugged in while that
 * window exists. Two stand-in app windows, each on its own socket as the
 * app's two processes are.
 *
 * Run via run-host-test.sh.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "../../common/chameleon_evdev.h"
#include "../core/internal.h"
#include "../../common/cham_io.h"

static int failures;
#define CHECK(cond, ...)                                           \
    do {                                                           \
        if (cond) {                                                \
            printf("  ok   " __VA_ARGS__);                         \
        } else {                                                   \
            printf("  FAIL " __VA_ARGS__);                         \
            printf("       (%s, errno %s)\n", #cond, strerror(errno)); \
            failures++;                                            \
        }                                                          \
        printf("\n");                                              \
    } while (0)

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- a stand-in window of the app ---- */

struct window {
    char path[108];
    uint32_t win_w, win_h, w, h, mhz;
    volatile int open;       /* the window exists: it takes the service's connection and reports its size */
    volatile int connected;
    volatile int presents, unchanged, buffers, hellos;
    volatile int asked_open, asked_close, titles; /* CHAM_WINDOW_OPEN (its screen), _CLOSE, CHAM_TITLE */
    char title[256];
    volatile int icons; /* CHAM_ICON */
    char icon[64];
    volatile uint64_t hello_flags;
    volatile int touch; /* 1: a finger is to touch it at (1/4, 1/2), 2: it did */
};

static void take_buffer(int c)
{
    char packet[64];
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } control;
    struct iovec iov = {.iov_base = packet, .iov_len = sizeof packet};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.buf,
                        .msg_controllen = sizeof control.buf};
    if (recvmsg(c, &mh, MSG_CMSG_CLOEXEC) > 0) { /* the (fake) AHardwareBuffer */
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        int fd = -1;
        if (cm && cm->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(cm), sizeof fd);
        if (fd >= 0)
            close(fd);
    }
}

static void *window_main(void *arg)
{
    struct window *win = arg;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", win->path);
    for (;;) {
        while (!win->open)
            usleep(10000);
        /* Nothing listens while the window is closed, as no broker has a
         * window to hand a connection to. */
        int l = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        unlink(win->path);
        bind(l, (struct sockaddr *)&addr, sizeof addr);
        listen(l, 1);
        int c = accept(l, NULL, NULL);
        close(l);
        unlink(win->path);
        struct cham_msg window = {CHAM_WINDOW, 0, win->win_w | (uint64_t)win->win_h << 32, 0};
        struct cham_msg cfg = {CHAM_CONFIG, CHAM_CONFIG_CURSOR | CHAM_CONFIG_PLANE, win->w | (uint64_t)win->h << 32,
                               win->mhz};
        cham_send(c, &window, -1);
        cham_send(c, &cfg, -1);
        win->connected = 1;
        while (win->open) {
            if (win->touch == 1) {
                struct cham_msg down = {CHAM_INPUT, CHAM_INPUT_TOUCH_DOWN, 7, CHAM_INPUT_POS(0x4000, 0x8000)};
                struct cham_msg up = {CHAM_INPUT, CHAM_INPUT_TOUCH_UP, 7, 0};
                struct cham_msg frame = {CHAM_INPUT, CHAM_INPUT_TOUCH_FRAME, 0, 0};
                cham_send(c, &down, -1);
                cham_send(c, &frame, -1);
                cham_send(c, &up, -1);
                cham_send(c, &frame, -1);
                win->touch = 2;
            }
            struct pollfd p = {c, POLLIN, 0};
            if (poll(&p, 1, 20) <= 0)
                continue;
            struct cham_msg m;
            int fd = -1;
            if (cham_recv(c, &m, &fd) <= 0)
                break;
            if (m.type == CHAM_TITLE && fd >= 0) {
                ssize_t n = read(fd, win->title, sizeof win->title - 1);
                win->title[n > 0 ? n : 0] = 0;
                win->titles++;
            }
            if (m.type == CHAM_ICON && fd >= 0) {
                ssize_t n = read(fd, win->icon, sizeof win->icon - 1);
                win->icon[n > 0 ? n : 0] = 0;
                win->icons++;
            }
            if (fd >= 0)
                close(fd);
            if (m.type == CHAM_WINDOW_OPEN) {
                win->asked_open = (int)m.a;
            } else if (m.type == CHAM_WINDOW_CLOSE) {
                win->asked_close++;
                win->open = 0; /* as the app: the window closes for good */
            } else if (m.type == CHAM_HELLO) {
                win->hello_flags = m.b;
                win->hellos++;
            } else if (m.type == CHAM_BUFFER_ADD) {
                take_buffer(c);
                win->buffers++;
            } else if (m.type == CHAM_CURSOR) {
                if (m.id != CHAM_CURSOR_HIDE && (m.id & CHAM_CURSOR_NEW)) {
                    struct cham_msg rel = {CHAM_RELEASE, m.id & ~CHAM_CURSOR_NEW, 0, 0};
                    cham_send(c, &rel, -1);
                }
            } else if (m.type == CHAM_PRESENT) {
                win->presents++;
                win->unchanged += m.id == CHAM_PRESENT_UNCHANGED;
                struct cham_msg queued = {CHAM_FRAME_QUEUED, 0, m.a, mono_ns()};
                cham_send(c, &queued, -1);
                usleep(4000);
                if (m.id != CHAM_PRESENT_UNCHANGED) {
                    struct cham_msg rel = {CHAM_RELEASE, m.id, 0, 0};
                    cham_send(c, &rel, -1);
                }
                struct cham_msg done = {CHAM_FRAME_DONE, 0, m.a, mono_ns()};
                cham_send(c, &done, -1);
            }
        }
        close(c); /* the window is closed */
        win->connected = 0;
    }
    return NULL;
}

/* ---- helpers ---- */

/* The service's input devices (common/chameleon_evdev.h): the list, and a
 * connection reading device `index` (-1: just the list), past its STATE. */
static int open_input(int index, struct cham_evdev_info *list, uint32_t *count)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", getenv("XDG_RUNTIME_DIR"), CHAM_EVDEV_SOCKET_NAME);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    uint32_t n = 0, want = (uint32_t)index;
    struct cham_evdev_info info;
    struct cham_evdev_record h;
    struct cham_evdev_state state;
    int ok = connect(s, (struct sockaddr *)&addr, sizeof addr) == 0 && recv(s, &n, sizeof n, MSG_WAITALL) == sizeof n &&
             n <= CHAM_EVDEV_MAX_DEVICES;
    for (uint32_t i = 0; ok && i < n; i++) {
        ok = recv(s, &info, sizeof info, MSG_WAITALL) == sizeof info;
        if (ok && list)
            list[i] = info;
    }
    if (count)
        *count = ok ? n : 0;
    if (ok && index >= 0)
        ok = send(s, &want, sizeof want, 0) == sizeof want && recv(s, &h, sizeof h, MSG_WAITALL) == sizeof h &&
             h.type == CHAM_EVDEV_STATE && recv(s, &state, sizeof state, MSG_WAITALL) == sizeof state;
    if (!ok || index < 0) {
        close(s);
        return -1;
    }
    return s;
}

/* The device's next frames within the time: 1 if a contact came down at
 * (x, y), 0 if nothing came, -1 for anything else. */
static int touched_at(int s, int32_t x, int32_t y, int timeout_ms)
{
    int got_x = 0, got_y = 0, down = 0, any = 0;
    struct pollfd p = {s, POLLIN, 0};
    while (!(got_x && got_y && down) && poll(&p, 1, timeout_ms) == 1) {
        struct cham_evdev_record h;
        struct input_event e[64];
        if (recv(s, &h, sizeof h, MSG_WAITALL) != sizeof h || h.size > sizeof e ||
            recv(s, e, h.size, MSG_WAITALL) != (ssize_t)h.size)
            return -1;
        any = 1;
        for (size_t i = 0; h.type == CHAM_EVDEV_FRAME && i < h.size / sizeof e[0]; i++) {
            got_x |= e[i].type == EV_ABS && e[i].code == ABS_MT_POSITION_X && e[i].value == x;
            got_y |= e[i].type == EV_ABS && e[i].code == ABS_MT_POSITION_Y && e[i].value == y;
            down |= e[i].type == EV_KEY && e[i].code == BTN_TOUCH && e[i].value == 1;
        }
    }
    return got_x && got_y && down ? 1 : any ? -1 : 0;
}

/* A program of the session asking for a window, as chameleon-screens does. */
static int64_t ask_screen(uint32_t what, int screen, const char *text)
{
    static int card = -1;
    if (card < 0) {
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", getenv("XDG_RUNTIME_DIR"), CHAM_CARD_SOCKET_NAME);
        card = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (connect(card, (struct sockaddr *)&addr, sizeof addr) != 0)
            return -ECONNREFUSED;
    }
    struct cham_msg req = {CARD_SCREEN, what, (uint64_t)(int64_t)screen, 0}, done = {0};
    int fd = -1;
    if (cham_send(card, &req, -1) != 0 || (text && send(card, text, strlen(text), MSG_NOSIGNAL) < 0) ||
        cham_recv(card, &done, &fd) != 1 || done.type != CARD_DONE)
        return -EPIPE;
    return (int64_t)done.a;
}

static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *value)
{
    uint32_t id = 0;
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
    for (uint32_t i = 0; props && i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p && strcmp(p->name, name) == 0) {
            id = p->prop_id;
            if (value)
                *value = props->prop_values[i];
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static int g_flips;
static unsigned g_flip_crtc[8];
static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, unsigned crtc, void *data)
{
    (void)fd, (void)seq, (void)sec, (void)usec, (void)data;
    if (g_flips < 8)
        g_flip_crtc[g_flips] = crtc;
    g_flips++;
}

/* Waits until `count` page-flip events came since the last call's end. */
static int wait_flips(int fd, int count, int timeout_ms)
{
    uint64_t end = mono_ns() + (uint64_t)timeout_ms * 1000000ull;
    g_flips = 0;
    while (g_flips < count && mono_ns() < end) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 50) <= 0)
            continue;
        drmEventContext ctx = {.version = 3, .page_flip_handler2 = on_flip};
        drmHandleEvent(fd, &ctx);
    }
    return g_flips;
}

static int flipped_on(unsigned crtc)
{
    for (int i = 0; i < g_flips && i < 8; i++)
        if (g_flip_crtc[i] == crtc)
            return 1;
    return 0;
}

static int wait_for(volatile int *what, int value, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 10 && *what != value; i++)
        usleep(10000);
    return *what == value;
}

static uint32_t make_fb(int fd, struct gbm_device *gbm, uint32_t w, uint32_t h)
{
    struct gbm_bo *bo = gbm_bo_create(gbm, w, h, DRM_FORMAT_XBGR8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    uint32_t handles[4] = {bo ? gbm_bo_get_handle(bo).u32 : 0}, pitches[4] = {bo ? gbm_bo_get_stride(bo) : 0},
             offsets[4] = {0}, fb = 0;
    if (!bo || drmModeAddFB2(fd, w, h, DRM_FORMAT_XBGR8888, handles, pitches, offsets, &fb, 0) != 0)
        return 0;
    return fb;
}

/* One screen's objects and the properties a compositor sets. */
struct pipe_ids {
    uint32_t conn, crtc, plane, w, h;
    uint32_t p_conn_crtc, p_active, p_mode, p_fb, p_pcrtc, p_src[4], p_dst[4];
    uint32_t fbs[2], blob;
};

static void pipe_props(int fd, struct pipe_ids *p)
{
    static const char *const src[] = {"SRC_X", "SRC_Y", "SRC_W", "SRC_H"};
    static const char *const dst[] = {"CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H"};
    p->p_conn_crtc = prop_id(fd, p->conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
    p->p_active = prop_id(fd, p->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
    p->p_mode = prop_id(fd, p->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
    p->p_fb = prop_id(fd, p->plane, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
    p->p_pcrtc = prop_id(fd, p->plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
    for (int i = 0; i < 4; i++) {
        p->p_src[i] = prop_id(fd, p->plane, DRM_MODE_OBJECT_PLANE, src[i], NULL);
        p->p_dst[i] = prop_id(fd, p->plane, DRM_MODE_OBJECT_PLANE, dst[i], NULL);
    }
}

static void add_modeset(drmModeAtomicReq *req, const struct pipe_ids *p)
{
    drmModeAtomicAddProperty(req, p->conn, p->p_conn_crtc, p->crtc);
    drmModeAtomicAddProperty(req, p->crtc, p->p_active, 1);
    drmModeAtomicAddProperty(req, p->crtc, p->p_mode, p->blob);
    drmModeAtomicAddProperty(req, p->plane, p->p_fb, p->fbs[0]);
    drmModeAtomicAddProperty(req, p->plane, p->p_pcrtc, p->crtc);
    uint64_t src[4] = {0, 0, (uint64_t)p->w << 16, (uint64_t)p->h << 16}, dst[4] = {0, 0, p->w, p->h};
    for (int i = 0; i < 4; i++) {
        drmModeAtomicAddProperty(req, p->plane, p->p_src[i], src[i]);
        drmModeAtomicAddProperty(req, p->plane, p->p_dst[i], dst[i]);
    }
}

static int flip(int fd, const struct pipe_ids *p, int which)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, p->plane, p->p_fb, p->fbs[which]);
    int ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL);
    drmModeAtomicFree(req);
    return ret;
}

int main(void)
{
    const char *dev_path = getenv("CHAMELEON_DRM_PATH"), *sock = getenv("CHAMELEON_SOCKET");
    if (!dev_path || !sock) {
        fprintf(stderr, "run via run-host-test.sh\n");
        return 2;
    }
    static struct window first = {.win_w = 1080, .win_h = 2400, .w = 1080, .h = 1800, .mhz = 120000, .open = 1};
    static struct window second = {.win_w = 1080, .win_h = 2400, .w = 1080, .h = 1500, .mhz = 60000};
    snprintf(first.path, sizeof first.path, "%s", sock);
    snprintf(second.path, sizeof second.path, "%s.2", sock);
    pthread_t t;
    pthread_create(&t, NULL, window_main, &first);
    pthread_create(&t, NULL, window_main, &second);

    printf("a card with two screens\n");
    int fd = open(dev_path, O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0 && drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0, "the card opens, atomic");
    CHECK(first.hellos == 1 && (first.hello_flags & CHAM_HELLO_SCREENS),
          "the app's first window is told of the second screen");
    drmModeRes *res = drmModeGetResources(fd);
    CHECK(res && res->count_crtcs == 2 && res->count_connectors == 2 && res->count_encoders == 2,
          "resources: 2 crtcs / connectors / encoders");
    drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
    CHECK(pres && pres->count_planes == 4, "four planes: a primary and a cursor plane each");
    if (!res || res->count_crtcs != 2 || res->count_connectors != 2 || !pres || pres->count_planes != 4) {
        printf("\nFAILED: not the objects to test\n");
        return 1;
    }
    struct pipe_ids a = {.conn = res->connectors[0], .crtc = res->crtcs[0], .plane = pres->planes[0], .w = 1080,
                         .h = 1800};
    struct pipe_ids b = {.conn = res->connectors[1], .crtc = res->crtcs[1], .plane = pres->planes[2], .w = 1080,
                         .h = 1500};
    uint32_t masks[4];
    uint64_t types[4];
    for (int i = 0; i < 4; i++) {
        drmModePlane *pl = drmModeGetPlane(fd, pres->planes[i]);
        masks[i] = pl ? pl->possible_crtcs : 0;
        drmModeFreePlane(pl);
        prop_id(fd, pres->planes[i], DRM_MODE_OBJECT_PLANE, "type", &types[i]);
    }
    CHECK(masks[0] == 1 && masks[1] == 1 && masks[2] == 2 && masks[3] == 2,
          "each screen's planes go on its own CRTC (masks %u %u %u %u)", masks[0], masks[1], masks[2], masks[3]);
    CHECK(types[0] == 1 && types[1] == 2 && types[2] == 1 && types[3] == 2, "primary, cursor, primary, cursor");
    drmModeEncoder *enc = drmModeGetEncoder(fd, res->encoders[1]);
    CHECK(enc && enc->possible_crtcs == 2, "the second encoder drives the second CRTC");
    drmModeFreeEncoder(enc);
    pipe_props(fd, &a);
    pipe_props(fd, &b);

    drmModeConnector *conn = drmModeGetConnector(fd, a.conn);
    CHECK(conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes == 1 &&
              conn->modes[0].hdisplay == 1080 && conn->modes[0].vdisplay == 1800 && conn->connector_type_id == 1,
          "DSI-1: connected, the first window's mode");
    drmModeModeInfo mode_a = conn && conn->count_modes ? conn->modes[0] : (drmModeModeInfo){0};
    drmModeFreeConnector(conn);
    conn = drmModeGetConnector(fd, b.conn);
    uint64_t edid = 99;
    uint32_t p_edid = prop_id(fd, b.conn, DRM_MODE_OBJECT_CONNECTOR, "EDID", &edid);
    CHECK(conn && conn->connection == DRM_MODE_DISCONNECTED && conn->count_modes == 0 && conn->mmWidth == 0 &&
              conn->connector_type == DRM_MODE_CONNECTOR_DSI && conn->connector_type_id == 2,
          "DSI-2: disconnected, no modes (its window isn't open)");
    CHECK(p_edid && edid == 0, "...and no EDID");
    drmModeFreeConnector(conn);
    CHECK(second.hellos == 0 && !second.connected, "nothing was sent to a window that isn't there");

    printf("the first screen alone\n");
    struct gbm_device *gbm = gbm_create_device(fd);
    for (int i = 0; i < 2; i++) {
        a.fbs[i] = make_fb(fd, gbm, a.w, a.h);
        b.fbs[i] = make_fb(fd, gbm, b.w, b.h);
    }
    CHECK(a.fbs[0] && a.fbs[1] && b.fbs[0] && b.fbs[1], "framebuffers for both");
    drmModeCreatePropertyBlob(fd, &mode_a, sizeof mode_a, &a.blob);
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    add_modeset(req, &a);
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0, "modeset on the first screen");
    drmModeAtomicFree(req);
    CHECK(flip(fd, &a, 1) == 0 && wait_flips(fd, 1, 2000) == 1 && g_flip_crtc[0] == a.crtc,
          "a page flip there: its event names the first CRTC");
    CHECK(wait_for(&first.presents, 2, 2000) && second.presents == 0, "its frames went to the first window (%d)",
          first.presents);
    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, b.plane, b.p_pcrtc, a.crtc);
    drmModeAtomicAddProperty(req, b.plane, b.p_fb, b.fbs[0]);
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL) != 0 && errno == EINVAL,
          "the second screen's plane on the first CRTC: EINVAL");
    drmModeAtomicFree(req);

    printf("the second window opens\n");
    second.open = 1;
    int plugged = 0;
    for (int i = 0; i < 400 && !plugged; i++) { /* the service looks for it once a second */
        usleep(10000);
        conn = drmModeGetConnector(fd, b.conn);
        plugged = conn && conn->connection == DRM_MODE_CONNECTED;
        if (!plugged)
            drmModeFreeConnector(conn);
    }
    CHECK(plugged && conn->count_modes == 1 && conn->modes[0].hdisplay == 1080 && conn->modes[0].vdisplay == 1500 &&
              conn->modes[0].vrefresh == 60,
          "DSI-2: connected, with its window's mode (1080x1500 @ 60)");
    CHECK(plugged && conn->mmWidth == 69 && conn->mmHeight == 152, "its size is its whole window's (%ux%u mm)",
          plugged ? conn->mmWidth : 0, plugged ? conn->mmHeight : 0);
    drmModeModeInfo mode_b = plugged && conn->count_modes ? conn->modes[0] : (drmModeModeInfo){0};
    if (plugged)
        drmModeFreeConnector(conn);
    CHECK(second.hellos == 1 && !(second.hello_flags & CHAM_HELLO_SCREENS), "its own connection, a plain hello");
    edid = 0;
    prop_id(fd, b.conn, DRM_MODE_OBJECT_CONNECTOR, "EDID", &edid);
    drmModePropertyBlobRes *eb = edid ? drmModeGetPropertyBlob(fd, (uint32_t)edid) : NULL;
    const unsigned char *e = eb ? eb->data : NULL;
    uint64_t edid_a = 0;
    prop_id(fd, a.conn, DRM_MODE_OBJECT_CONNECTOR, "EDID", &edid_a);
    CHECK(e && eb->length == 128 && e[10] == 2 && memcmp(e + 77, "Chameleon 2\n", 12) == 0 && edid != edid_a,
          "its EDID: another monitor, \"Chameleon 2\"");
    unsigned char sum = 0;
    for (int i = 0; e && i < 128; i++)
        sum = (unsigned char)(sum + e[i]);
    CHECK(e && sum == 0, "...with a valid checksum");
    drmModeFreePropertyBlob(eb);

    printf("both screens\n");
    drmModeCreatePropertyBlob(fd, &mode_b, sizeof mode_b, &b.blob);
    int before_a = first.presents;
    req = drmModeAtomicAlloc();
    add_modeset(req, &b);
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0, "modeset on the second screen");
    drmModeAtomicFree(req);
    CHECK(wait_for(&second.presents, 1, 2000) && second.buffers == 1, "its frame went to the second window");
    CHECK(first.presents == before_a, "...and nothing new to the first");
    int both = 1; /* the first flip after a modeset may complete at once */
    for (int i = 0; i < 3; i++)
        both &= flip(fd, &a, i & 1) == 0 && flip(fd, &b, !(i & 1)) == 0 && wait_flips(fd, 2, 2000) == 2 &&
                flipped_on(a.crtc) && flipped_on(b.crtc);
    CHECK(both, "flips on both, an event from each CRTC every time");
    CHECK(flip(fd, &a, 0) == 0 && flip(fd, &b, 1) == 0, "a flip on each while the other's is pending");
    CHECK(flip(fd, &b, 0) != 0 && errno == EBUSY, "a second on the same CRTC: EBUSY");
    CHECK(wait_flips(fd, 2, 2000) == 2 && flipped_on(a.crtc) && flipped_on(b.crtc), "an event from each CRTC");
    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, a.plane, a.p_fb, a.fbs[1]);
    drmModeAtomicAddProperty(req, b.plane, b.p_fb, b.fbs[0]);
    before_a = first.presents;
    int before_b = second.presents;
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0,
          "one commit for both");
    drmModeAtomicFree(req);
    CHECK(wait_flips(fd, 2, 2000) == 2 && flipped_on(a.crtc) && flipped_on(b.crtc), "...completes on both");
    CHECK(wait_for(&first.presents, before_a + 1, 2000) && wait_for(&second.presents, before_b + 1, 2000),
          "one frame to each window");
    drmModeCrtc *crtc = drmModeGetCrtc(fd, b.crtc);
    CHECK(crtc && crtc->mode_valid && crtc->mode.vdisplay == 1500 && crtc->buffer_id == b.fbs[0],
          "GETCRTC of the second: its mode and framebuffer");
    drmModeFreeCrtc(crtc);

    printf("the second window closes\n");
    second.open = 0;
    int unplugged = 0;
    for (int i = 0; i < 300 && !unplugged; i++) {
        usleep(10000);
        conn = drmModeGetConnector(fd, b.conn);
        unplugged = conn && conn->connection == DRM_MODE_DISCONNECTED && conn->count_modes == 0;
        drmModeFreeConnector(conn);
    }
    CHECK(unplugged, "DSI-2: disconnected again");
    before_b = second.presents;
    CHECK(flip(fd, &b, 1) == 0 && wait_flips(fd, 1, 2000) == 1 && g_flip_crtc[0] == b.crtc &&
              second.presents == before_b,
          "a flip on its CRTC still completes (nothing shown), as on a monitor pulled out");
    before_a = first.presents;
    CHECK(flip(fd, &a, 0) == 0 && wait_flips(fd, 1, 2000) == 1 && g_flip_crtc[0] == a.crtc &&
              wait_for(&first.presents, before_a + 1, 2000),
          "the first screen goes on as before");

    printf("...and opens again\n");
    before_b = second.presents;
    second.open = 1;
    plugged = 0;
    for (int i = 0; i < 400 && !plugged; i++) {
        usleep(10000);
        conn = drmModeGetConnector(fd, b.conn);
        plugged = conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes == 1;
        drmModeFreeConnector(conn);
    }
    CHECK(plugged, "DSI-2: connected");
    CHECK(wait_for(&second.presents, before_b + 1, 2000), "the frame on its CRTC is shown in the new window");
    CHECK(flip(fd, &b, 0) == 0 && wait_flips(fd, 1, 2000) == 1 && g_flip_crtc[0] == b.crtc,
          "its page flips complete again");

    printf("a program of the session asks for windows\n");
    CHECK(ask_screen(CARD_SCREEN_TITLE, 1, "Kate \xe2\x80\x94 notes") == 0 && wait_for(&second.titles, 1, 2000) &&
              strcmp(second.title, "Kate \xe2\x80\x94 notes") == 0 && first.titles == 0,
          "a title for the second screen's window reaches it (%s)", second.title);
    CHECK(ask_screen(CARD_SCREEN_TITLE, 0, "first") == 0 && wait_for(&first.titles, 1, 2000) &&
              strcmp(first.title, "first") == 0,
          "...and one for the first's");
    char icon_path[160];
    snprintf(icon_path, sizeof icon_path, "%s/icon-test.png", getenv("XDG_RUNTIME_DIR"));
    FILE *icon_file = fopen(icon_path, "w");
    if (icon_file) {
        fputs("a picture's bytes", icon_file);
        fclose(icon_file);
    }
    CHECK(ask_screen(CARD_SCREEN_ICON, 1, icon_path) == 0 && wait_for(&second.icons, 1, 2000) &&
              strcmp(second.icon, "a picture's bytes") == 0 && first.icons == 0,
          "an icon's file for the second screen's window: it gets the file to read (%s)", second.icon);
    CHECK(ask_screen(CARD_SCREEN_ICON, 1, "/nowhere/icon.png") == -ENOENT &&
              ask_screen(CARD_SCREEN_ICON, 1, getenv("XDG_RUNTIME_DIR")) == -EINVAL &&
              ask_screen(CARD_SCREEN_ICON, 7, icon_path) == -EINVAL && second.icons == 1,
          "no such file ENOENT, a directory EINVAL, no such screen EINVAL");
    CHECK(ask_screen(CARD_SCREEN_OPEN, 0, NULL) == -EBUSY && first.asked_open == 0,
          "open: every screen has its window, EBUSY");
    CHECK(ask_screen(CARD_SCREEN_CLOSE, 0, NULL) == -EINVAL && ask_screen(CARD_SCREEN_CLOSE, 2, NULL) == -EINVAL &&
              ask_screen(CARD_SCREEN_TITLE, 7, "x") == -EINVAL,
          "the first screen's window isn't closed; a screen the card hasn't: EINVAL");
    CHECK(ask_screen(CARD_SCREEN_CLOSE, 1, NULL) == 0 && wait_for(&second.asked_close, 1, 2000),
          "close: the second window is told to");
    unplugged = 0;
    for (int i = 0; i < 300 && !unplugged; i++) {
        usleep(10000);
        conn = drmModeGetConnector(fd, b.conn);
        unplugged = conn && conn->connection == DRM_MODE_DISCONNECTED;
        drmModeFreeConnector(conn);
    }
    CHECK(unplugged, "...and its connector is disconnected");
    CHECK(ask_screen(CARD_SCREEN_CLOSE, 1, NULL) == -ENOTCONN, "close again: no window there, ENOTCONN");
    CHECK(ask_screen(CARD_SCREEN_OPEN, 0, NULL) == 1 && wait_for(&first.asked_open, 1, 2000),
          "open: the first window is asked for the second screen's, which is the answer");
    CHECK(ask_screen(CARD_SCREEN_OPEN, 0, NULL) == -EBUSY, "open again before it came: EBUSY");
    second.open = 1; /* the app opens it */
    plugged = 0;
    for (int i = 0; i < 400 && !plugged; i++) {
        usleep(10000);
        conn = drmModeGetConnector(fd, b.conn);
        plugged = conn && conn->connection == DRM_MODE_CONNECTED;
        drmModeFreeConnector(conn);
    }
    CHECK(plugged, "the window comes: DSI-2 connected");

    printf("a finger on each window\n");
    static struct cham_evdev_info devs[CHAM_EVDEV_MAX_DEVICES];
    uint32_t count = 0;
    open_input(-1, devs, &count);
    CHECK(count == 6 && strcmp(devs[5].name, "Chameleon touchscreen 2") == 0 &&
              cham_bit_test(devs[5].prop, INPUT_PROP_DIRECT),
          "the card's second screen has a touchscreen of its own (%u devices, the last: %s)", count,
          count ? devs[count - 1].name : "");
    CHECK(strcmp(devs[0].output, "DSI-1") == 0 && strcmp(devs[5].output, "DSI-2") == 0 && !devs[3].output[0],
          "each touchscreen names its connector (%s, %s); the touchpad names none", devs[0].output, devs[5].output);
    int touch1 = open_input(0, NULL, NULL), touch2 = open_input(5, NULL, NULL);
    CHECK(touch1 >= 0 && touch2 >= 0, "both can be read");
    second.touch = 1;
    CHECK(wait_for(&second.touch, 2, 2000) && touched_at(touch2, 0x4000, 0x8000, 2000) == 1,
          "a finger on the second window: a contact on the second touchscreen, at its place");
    CHECK(touched_at(touch1, 0x4000, 0x8000, 200) == 0, "...and nothing on the first");
    while (touched_at(touch2, 0, 0, 100)) /* its lift */
        ;
    first.touch = 1;
    CHECK(wait_for(&first.touch, 2, 2000) && touched_at(touch1, 0x4000, 0x8000, 2000) == 1,
          "a finger on the first window: a contact on the first touchscreen");
    CHECK(touched_at(touch2, 0x4000, 0x8000, 200) == 0, "...and nothing on the second");
    close(touch1);
    close(touch2);

    /* Through chameleon-screens, over the session's bus, as a compositor's
     * script calls it (run-host-test.sh starts both). */
    if (getenv("SCREENS_TEST_BUS")) {
        printf("...over the session's bus\n");
#define BUS_CALL "dbus-send --session --print-reply --dest=io.github.harryl0l.Chameleon /Screens " \
                 "io.github.harryl0l.Chameleon.Screens."
        int there = 0;
        for (int i = 0; i < 50 && !there; i++) { /* until it has the name */
            there = system(BUS_CALL "SetTitle string:2 'string:Dolphin \xe2\x80\x94 Home' >/dev/null 2>&1") == 0;
            if (!there)
                usleep(100000);
        }
        CHECK(there && wait_for(&second.titles, 2, 2000) && strcmp(second.title, "Dolphin \xe2\x80\x94 Home") == 0,
              "SetTitle(\"2\", ...) names the second screen's window (%s)", second.title);
        char call[512];
        snprintf(call, sizeof call, BUS_CALL "SetIcon string:2 string:%s >/dev/null", icon_path);
        int icons = second.icons;
        CHECK(system(call) == 0 && wait_for(&second.icons, icons + 1, 2000),
              "SetIcon(\"2\", file) gives it the icon's file");
        CHECK(system(BUS_CALL "SetApp string:2 string:org.kde.kate >/dev/null") == 0 && second.icons == icons + 1,
              "SetApp(\"2\", app) is answered and nothing more");
        first.asked_open = 0;
        CHECK(system(BUS_CALL "Close string:2 >/dev/null") == 0 && wait_for(&second.asked_close, 2, 2000),
              "Close(\"2\") has it closed");
        for (int i = 0; i < 300 && second.connected; i++)
            usleep(10000);
        usleep(200000); /* the service has seen it go */
        FILE *out = popen(BUS_CALL "Open", "r");
        char reply[512] = "";
        size_t got = out ? fread(reply, 1, sizeof reply - 1, out) : 0;
        reply[got] = 0;
        if (out)
            pclose(out);
        CHECK(strstr(reply, "int32 2") && wait_for(&first.asked_open, 1, 2000),
              "Open() asks for a window and answers its connector's number, 2");
        out = popen(BUS_CALL "Open", "r");
        got = out ? fread(reply, 1, sizeof reply - 1, out) : 0;
        reply[got] = 0;
        if (out)
            pclose(out);
        CHECK(strstr(reply, "int32 0"), "Open() with no screen left: 0");
    }

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
