/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The app's input as evdev devices (common/chameleon_evdev.h), served on
 * $XDG_RUNTIME_DIR/chameleon-evdev to libchameleon_udev.so, which shows
 * them to libinput (the compositor's own, here) and to any other program:
 *  0 touchscreen: the app's direct touch, multitouch protocol B;
 *  1 keyboard: hardware keyboards, by their Linux key codes;
 *  2 mouse: a relative mouse, fed from the mouse the app captures
 *    (Android's pointer capture: the mouse's own counts and buttons), with
 *    high-resolution wheels;
 *  3 touchpad: the app's trackpad mode, the screen as a laptop's touchpad
 *    without buttons (so libinput taps to click), in screen pixels.
 * Touch positions are fractions of the screen, so those axes run 0..65535
 * and the compositor maps them onto its output. As in the kernel, a device
 * changes whether or not a program reads it; its events reach only the
 * programs reading it at the time.
 *
 * The same socket carries udev monitors' events (evdev_uevent): kms.c's
 * card hotplug when the app's window changes.
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
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../../common/chameleon_evdev.h"
#include "../../common/chameleon_proto.h"
#include "internal.h"

enum { DEV_TOUCH, DEV_KEYBOARD, DEV_MOUSE, DEV_TOUCHPAD, DEV_TABLET, DEV_COUNT };

#define TOUCH_SLOTS 10
#define MAX_READERS 8
#define FRAME_EVENTS (TOUCH_SLOTS * 5 + 4)
#define AXIS_MAX 65535

/* Bytes queued for a reader whose socket is full. The kernel puts events
 * straight into each reader's queue, so none are lost on the way; here a
 * reader's process takes them from a socket, and frames wait for it rather
 * than being lost when it lags. Only a reader this far behind (a stopped
 * program) loses frames. */
#define OUT_MAX (1 << 20)

struct reader {
    int fd;   /* 0 = free; closed by the accept thread only */
    int dead; /* the program went away; the accept thread closes it */
    int lost; /* frames didn't fit: it gets a fresh STATE once the queue drains */
    size_t out_len, out_cap;
    char *out; /* grown as needed, up to OUT_MAX */
    size_t in_len; /* a request read in part */
    char in[sizeof(struct cham_evdev_record) + sizeof(uint32_t) + sizeof(struct input_absinfo)];
};

static struct device {
    struct cham_evdev_info info; /* under g_evdev_lock once served (EVIOCSABS) */
    struct reader readers[MAX_READERS]; /* under g_evdev_lock */
    int reader_count;
    int grab; /* the reader holding EVIOCGRAB, or -1 */
    struct cham_evdev_state state; /* under g_evdev_lock: the device as readers' frames left it */
    int32_t staged_slot; /* input thread: ABS_MT_SLOT not yet passed on */
    struct input_event frame[FRAME_EVENTS]; /* input thread only */
    int frame_len;
    int64_t last_time; /* ns, of the last frame sent */
} g_dev[DEV_COUNT];
static pthread_mutex_t g_evdev_lock = PTHREAD_MUTEX_INITIALIZER;
#define MAX_MONITORS 16
static struct {
    int fd;              /* 0 = free; closed by the accept thread */
    uint32_t subsystems; /* CHAM_UEVENT_SUBSYSTEM_* it wants */
} g_monitors[MAX_MONITORS]; /* under g_evdev_lock */
static uint64_t g_seqnum;            /* under g_evdev_lock */
static int g_server_wake[2] = {-1, -1}; /* a reader's socket filled up: the accept thread sends the rest */

/* Device state, on the input thread only. */
static struct mt {         /* a multitouch device (protocol B) */
    int slot_id[TOUCH_SLOTS]; /* the app's touch id per slot, -1 = free */
    int32_t x[TOUCH_SLOTS], y[TOUCH_SLOTS], cur_slot, abs_x, abs_y; /* as last sent */
    int tracking_id;
    int touching;
    int tool; /* the touchpad's BTN_TOOL_* finger count, as last sent */
} g_touch, g_pad;
static int32_t g_pad_size; /* the touchpad's axes: the screen's longer side, in pixels */
static uint32_t g_window_w, g_window_h; /* input thread: the app's whole window, 0 = not reported */
static int32_t g_wheel_rest[2]; /* hi-res wheel movement short of a whole notch */
static int64_t g_event_time; /* ns, from CHAM_INPUT_TIME; 0 = none yet */
static struct device *g_pointer = &g_dev[DEV_MOUSE]; /* the mouse or the tablet, whichever moved last */

static void set_axis(struct cham_evdev_info *d, unsigned axis, int32_t max)
{
    cham_bit_set(d->abs, axis);
    d->absinfo[axis].maximum = max;
}

static void describe(void)
{
    struct cham_evdev_info *d = &g_dev[DEV_TOUCH].info;
    snprintf(d->name, sizeof d->name, "Chameleon touchscreen");
    snprintf(d->udev_type, sizeof d->udev_type, "ID_INPUT_TOUCHSCREEN");
    d->id.bustype = BUS_VIRTUAL;
    cham_bit_set(d->prop, INPUT_PROP_DIRECT);
    cham_bit_set(d->ev, EV_SYN);
    cham_bit_set(d->ev, EV_KEY);
    cham_bit_set(d->ev, EV_ABS);
    cham_bit_set(d->key, BTN_TOUCH);
    set_axis(d, ABS_X, AXIS_MAX);
    set_axis(d, ABS_Y, AXIS_MAX);
    set_axis(d, ABS_MT_SLOT, TOUCH_SLOTS - 1);
    set_axis(d, ABS_MT_POSITION_X, AXIS_MAX);
    set_axis(d, ABS_MT_POSITION_Y, AXIS_MAX);
    set_axis(d, ABS_MT_TRACKING_ID, 65535);

    d = &g_dev[DEV_KEYBOARD].info;
    snprintf(d->name, sizeof d->name, "Chameleon keyboard");
    snprintf(d->udev_type, sizeof d->udev_type, "ID_INPUT_KEY ID_INPUT_KEYBOARD");
    d->id.bustype = BUS_VIRTUAL;
    cham_bit_set(d->ev, EV_SYN);
    cham_bit_set(d->ev, EV_KEY);
    for (unsigned k = KEY_ESC; k <= KEY_MICMUTE; k++)
        if (k != 84 && (k < 195 || k > 199)) /* codes the kernel doesn't define: no keyboard has them */
            cham_bit_set(d->key, k);

    d = &g_dev[DEV_MOUSE].info;
    snprintf(d->name, sizeof d->name, "Chameleon mouse");
    snprintf(d->udev_type, sizeof d->udev_type, "ID_INPUT_MOUSE");
    d->id.bustype = BUS_VIRTUAL;
    cham_bit_set(d->ev, EV_SYN);
    cham_bit_set(d->ev, EV_KEY);
    cham_bit_set(d->ev, EV_REL);
    for (unsigned b = BTN_LEFT; b <= BTN_EXTRA; b++)
        cham_bit_set(d->key, b);
    cham_bit_set(d->rel, REL_X);
    cham_bit_set(d->rel, REL_Y);
    cham_bit_set(d->rel, REL_WHEEL);
    cham_bit_set(d->rel, REL_HWHEEL);
    cham_bit_set(d->rel, REL_WHEEL_HI_RES);
    cham_bit_set(d->rel, REL_HWHEEL_HI_RES);

    /* A clickpad would have BTN_LEFT; without it, libinput taps to click. */
    d = &g_dev[DEV_TOUCHPAD].info;
    snprintf(d->name, sizeof d->name, "Chameleon touchpad");
    snprintf(d->udev_type, sizeof d->udev_type, "ID_INPUT_TOUCHPAD");
    d->id.bustype = BUS_VIRTUAL;
    cham_bit_set(d->prop, INPUT_PROP_POINTER);
    cham_bit_set(d->ev, EV_SYN);
    cham_bit_set(d->ev, EV_KEY);
    cham_bit_set(d->ev, EV_ABS);
    const unsigned pad_keys[] = {BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
                                 BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP};
    for (size_t i = 0; i < sizeof pad_keys / sizeof pad_keys[0]; i++)
        cham_bit_set(d->key, pad_keys[i]);
    set_axis(d, ABS_X, AXIS_MAX);
    set_axis(d, ABS_Y, AXIS_MAX);
    set_axis(d, ABS_MT_SLOT, TOUCH_SLOTS - 1);
    set_axis(d, ABS_MT_POSITION_X, AXIS_MAX);
    set_axis(d, ABS_MT_POSITION_Y, AXIS_MAX);
    set_axis(d, ABS_MT_TRACKING_ID, 65535);

    /* The mouse while the app hasn't captured it: absolute, like the tablet
     * a virtual machine gives its guest (QEMU's usb-tablet, virtio-tablet). */
    d = &g_dev[DEV_TABLET].info;
    snprintf(d->name, sizeof d->name, "Chameleon tablet");
    snprintf(d->udev_type, sizeof d->udev_type, "ID_INPUT_MOUSE");
    d->id.bustype = BUS_VIRTUAL;
    cham_bit_set(d->ev, EV_SYN);
    cham_bit_set(d->ev, EV_KEY);
    cham_bit_set(d->ev, EV_REL);
    cham_bit_set(d->ev, EV_ABS);
    for (unsigned b = BTN_LEFT; b <= BTN_EXTRA; b++)
        cham_bit_set(d->key, b);
    set_axis(d, ABS_X, AXIS_MAX);
    set_axis(d, ABS_Y, AXIS_MAX);
    cham_bit_set(d->rel, REL_WHEEL);
    cham_bit_set(d->rel, REL_HWHEEL);
    cham_bit_set(d->rel, REL_WHEEL_HI_RES);
    cham_bit_set(d->rel, REL_HWHEEL_HI_RES);
}

/* Units per mm from the size the connector reports, so programs that
 * measure in mm get sensible numbers. The touchscreen's axes span the
 * screen. The touchpad's are pixels, the same per mm both ways whichever
 * way the phone is turned (libinput's touchpad gestures are measured in mm),
 * and as long as the screen's longer side either way. */
static void set_resolution(void)
{
    uint32_t w, h, mm_w, mm_h;
    kms_screen_size(&w, &h, &mm_w, &mm_h);
    if (!w || !h || !mm_w || !mm_h)
        return;
    struct cham_evdev_info *t = &g_dev[DEV_TOUCH].info;
    t->absinfo[ABS_X].resolution = t->absinfo[ABS_MT_POSITION_X].resolution = AXIS_MAX / (int32_t)mm_w;
    t->absinfo[ABS_Y].resolution = t->absinfo[ABS_MT_POSITION_Y].resolution = AXIS_MAX / (int32_t)mm_h;
    struct cham_evdev_info *p = &g_dev[DEV_TOUCHPAD].info;
    int32_t size = (int32_t)(w > h ? w : h), res = size / (int32_t)(w > h ? mm_w : mm_h);
    const unsigned axes[] = {ABS_X, ABS_Y, ABS_MT_POSITION_X, ABS_MT_POSITION_Y};
    for (size_t i = 0; i < sizeof axes / sizeof axes[0]; i++) {
        p->absinfo[axes[i]].maximum = size - 1;
        p->absinfo[axes[i]].resolution = res > 0 ? res : 1;
    }
    pthread_mutex_lock(&g_evdev_lock);
    g_pad_size = size;
    pthread_mutex_unlock(&g_evdev_lock);
}

/* ---- server ---- */

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ---- the readers' streams (under g_evdev_lock) ---- */

/* Room for `len` more bytes in r's queue, up to OUT_MAX. */
static int reserve(struct reader *r, size_t len)
{
    size_t need = r->out_len + len;
    if (need > OUT_MAX)
        return 0;
    if (need <= r->out_cap)
        return 1;
    size_t cap = r->out_cap ? r->out_cap : 16384;
    while (cap < need)
        cap *= 2;
    char *out = realloc(r->out, cap < OUT_MAX ? cap : OUT_MAX);
    if (!out)
        return 0;
    r->out = out;
    r->out_cap = cap < OUT_MAX ? cap : OUT_MAX;
    return 1;
}

static void append(struct reader *r, uint32_t type, const void *a, size_t a_len, const void *b, size_t b_len)
{
    struct cham_evdev_record h = {type, (uint32_t)(a_len + b_len)};
    memcpy(r->out + r->out_len, &h, sizeof h);
    memcpy(r->out + r->out_len + sizeof h, a, a_len);
    if (b_len)
        memcpy(r->out + r->out_len + sizeof h + a_len, b, b_len);
    r->out_len += sizeof h + a_len + b_len;
}

/* Sends what it can of r's queue; once a reader that lost frames has taken
 * it all, it gets the device's state again, which tells it events were
 * lost. A program that went away is marked for the accept thread. */
static void send_out(struct device *d, struct reader *r)
{
    while (r->out_len && !r->dead) {
        ssize_t n = send(r->fd, r->out, r->out_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            r->dead = errno != EAGAIN;
            return;
        }
        r->out_len -= (size_t)n;
        memmove(r->out, r->out + n, r->out_len);
        if (!r->out_len && r->lost) {
            r->lost = 0;
            if (reserve(r, sizeof(struct cham_evdev_record) + sizeof d->state))
                append(r, CHAM_EVDEV_STATE, &d->state, sizeof d->state, NULL, 0);
        }
    }
}

/* Queues a record (header, a then b) and sends what it can. A frame that
 * doesn't fit in the queue is lost; the other records are small and rare,
 * and a program that can't take even those is given up on. */
static void queue_record(struct device *d, struct reader *r, uint32_t type, const void *a, size_t a_len,
                         const void *b, size_t b_len)
{
    if (r->dead)
        return;
    int frame = type == CHAM_EVDEV_FRAME || type == CHAM_EVDEV_FRAME_GRABBED;
    int fits = reserve(r, sizeof(struct cham_evdev_record) + a_len + b_len);
    if (frame && (r->lost || !fits))
        r->lost = 1;
    else if (fits)
        append(r, type, a, a_len, b, b_len);
    else
        r->dead = 1;
    send_out(d, r);
    if ((r->out_len || r->dead) && write(g_server_wake[1], "", 1) < 0) {
        /* full: the accept thread is awake anyway */
    }
}

/* A request from reader i: EVIOCGRAB or EVIOCSABS, as the kernel answers them. */
static void request(struct device *d, int i, const struct cham_evdev_record *h, const void *payload)
{
    struct reader *r = &d->readers[i];
    if (h->type == CHAM_EVDEV_GRAB && h->size == sizeof(int32_t)) {
        int32_t on, result = 0;
        memcpy(&on, payload, sizeof on);
        if (on && d->grab >= 0)
            result = -EBUSY;
        else if (!on && d->grab != i)
            result = -EINVAL;
        else
            d->grab = on ? i : -1;
        queue_record(d, r, CHAM_EVDEV_GRAB_RESULT, &result, sizeof result, NULL, 0);
    } else if (h->type == CHAM_EVDEV_SET_ABS && h->size == sizeof(uint32_t) + sizeof(struct input_absinfo)) {
        uint32_t axis;
        struct input_absinfo ai;
        memcpy(&axis, payload, sizeof axis);
        memcpy(&ai, (const char *)payload + sizeof axis, sizeof ai);
        if (axis >= ABS_CNT || axis == ABS_MT_SLOT)
            return;
        d->info.absinfo[axis] = ai;
        d->state.abs[axis] = ai.value;
        for (int j = 0; j < MAX_READERS; j++)
            if (d->readers[j].fd)
                queue_record(d, &d->readers[j], CHAM_EVDEV_ABSINFO, &axis, sizeof axis, &ai, sizeof ai);
    }
}

/* Reads reader i's requests; 0 once it has gone away. */
static int take_requests(struct device *d, int i)
{
    struct reader *r = &d->readers[i];
    for (;;) {
        ssize_t n = recv(r->fd, r->in + r->in_len, sizeof r->in - r->in_len, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
            return 0;
        if (n < 0)
            return 1;
        r->in_len += (size_t)n;
        const struct cham_evdev_record *h = (const void *)r->in;
        if (r->in_len >= sizeof *h && h->size > sizeof r->in - sizeof *h)
            return 0; /* not one of ours */
        if (r->in_len >= sizeof *h && r->in_len >= sizeof *h + h->size) {
            request(d, i, h, r->in + sizeof *h);
            r->in_len -= sizeof *h + h->size;
            memmove(r->in, r->in + sizeof *h + h->size, r->in_len);
        }
    }
}

static void serve(int c)
{
    uint32_t count = DEV_COUNT, index = UINT32_MAX;
    set_resolution();
    if (write_all(c, &count, sizeof count))
        goto done;
    for (int i = 0; i < DEV_COUNT; i++)
        if (write_all(c, &g_dev[i].info, sizeof g_dev[i].info))
            goto done;
    /* A reader asks for its device right away; a lister just closes. */
    struct pollfd p = {c, POLLIN, 0};
    if (poll(&p, 1, 1000) != 1 || recv(c, &index, sizeof index, MSG_WAITALL) != (ssize_t)sizeof index)
        goto done;
    fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
    if (index == CHAM_EVDEV_MONITOR) {
        uint32_t subsystems;
        if (poll(&p, 1, 1000) != 1 || recv(c, &subsystems, sizeof subsystems, MSG_WAITALL) != (ssize_t)sizeof subsystems)
            goto done;
        pthread_mutex_lock(&g_evdev_lock);
        for (int i = 0; i < MAX_MONITORS && c >= 0; i++)
            if (!g_monitors[i].fd) {
                g_monitors[i].fd = c;
                g_monitors[i].subsystems = subsystems;
                c = -1;
                cham_log("udev: a program watches %s devices",
                         subsystems == CHAM_UEVENT_SUBSYSTEM_DRM     ? "drm"
                         : subsystems == CHAM_UEVENT_SUBSYSTEM_INPUT ? "input"
                         : subsystems                                ? "drm and input"
                                                                     : "other");
            }
        pthread_mutex_unlock(&g_evdev_lock);
        goto done;
    }
    if (index == CHAM_EVDEV_IME) {
        input_ime_connected(c); /* the input thread owns it from here */
        c = -1;
        goto done;
    }
    if (index >= DEV_COUNT)
        goto done;
    struct device *d = &g_dev[index];
    pthread_mutex_lock(&g_evdev_lock);
    for (int i = 0; i < MAX_READERS; i++) {
        if (!d->readers[i].fd) {
            /* It learns the device as it is now; every frame from here on follows. */
            d->readers[i] = (struct reader){.fd = c};
            queue_record(d, &d->readers[i], CHAM_EVDEV_STATE, &d->state, sizeof d->state, NULL, 0);
            d->reader_count++;
            c = -1;
            break;
        }
    }
    int readers = d->reader_count;
    pthread_mutex_unlock(&g_evdev_lock);
    if (c < 0)
        cham_log("evdev: a program opened the %s (%d reader%s)", d->info.name + strlen("Chameleon "), readers,
                 readers == 1 ? "" : "s");
done:
    if (c >= 0)
        close(c);
}

/* New connections, and the readers: their requests, the rest of their
 * queues, their going away. */
static void *server_main(void *arg)
{
    int s = (int)(intptr_t)arg;
    for (;;) {
        struct pollfd p[2 + DEV_COUNT * MAX_READERS + MAX_MONITORS] = {{s, POLLIN, 0}, {g_server_wake[0], POLLIN, 0}};
        int n = 2;
        pthread_mutex_lock(&g_evdev_lock);
        for (int i = 0; i < MAX_MONITORS; i++) /* only their going away */
            p[n++] = (struct pollfd){g_monitors[i].fd ? g_monitors[i].fd : -1, POLLIN, 0};
        for (int d = 0; d < DEV_COUNT; d++)
            for (int i = 0; i < MAX_READERS; i++) {
                struct reader *r = &g_dev[d].readers[i];
                p[n++] = (struct pollfd){r->fd ? r->fd : -1, (short)(POLLIN | (r->out_len ? POLLOUT : 0)), 0};
            }
        pthread_mutex_unlock(&g_evdev_lock);
        if (poll(p, (nfds_t)n, -1) < 0 && errno != EINTR)
            break;
        char b[64];
        if (p[1].revents & POLLIN)
            while (read(g_server_wake[0], b, sizeof b) == (ssize_t)sizeof b)
                ;
        if (p[0].revents & POLLIN) {
            int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0)
                serve(c);
        }
        pthread_mutex_lock(&g_evdev_lock);
        for (int i = 0; i < MAX_MONITORS; i++) {
            if (g_monitors[i].fd && p[2 + i].fd == g_monitors[i].fd && p[2 + i].revents) {
                close(g_monitors[i].fd); /* monitors don't write: readable means gone */
                g_monitors[i].fd = 0;
            }
        }
        for (int k = 2 + MAX_MONITORS; k < n; k++) {
            struct device *d = &g_dev[(k - 2 - MAX_MONITORS) / MAX_READERS];
            int i = (k - 2 - MAX_MONITORS) % MAX_READERS;
            struct reader *r = &d->readers[i];
            if (!r->fd || p[k].fd != r->fd)
                continue;
            if (p[k].revents & POLLOUT)
                send_out(d, r);
            if ((p[k].revents & (POLLIN | POLLHUP | POLLERR)) && !take_requests(d, i))
                r->dead = 1;
            if (!r->dead)
                continue;
            close(r->fd);
            free(r->out);
            *r = (struct reader){0};
            d->reader_count--;
            if (d->grab == i)
                d->grab = -1;
            cham_log("evdev: a program closed the %s", d->info.name + strlen("Chameleon "));
        }
        pthread_mutex_unlock(&g_evdev_lock);
    }
    return NULL;
}

void evdev_start(void)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (!dir || snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", dir, CHAM_EVDEV_SOCKET_NAME) >=
                    (int)sizeof addr.sun_path)
        return;
    describe();
    for (int i = 0; i < TOUCH_SLOTS; i++)
        g_touch.slot_id[i] = g_pad.slot_id[i] = -1;
    for (int d = 0; d < DEV_COUNT; d++) {
        g_dev[d].grab = -1;
        cham_evdev_state_init(&g_dev[d].state);
    }
    g_touch.cur_slot = g_touch.abs_x = g_touch.abs_y = -1;
    g_pad.cur_slot = g_pad.abs_x = g_pad.abs_y = -1;
    if (pipe2(g_server_wake, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s >= 0 && connect(s, (struct sockaddr *)&addr, sizeof addr) == 0) {
        cham_log("evdev: another server already answers at %s", addr.sun_path); /* a second copy of us */
        close(s);
        return;
    }
    if (s >= 0) { /* a socket can't connect twice */
        close(s);
        s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    }
    unlink(addr.sun_path); /* left by a server that is gone */
    if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s, 8) != 0) {
        cham_log("evdev: cannot serve input devices at %s: %s", addr.sun_path, strerror(errno));
        if (s >= 0)
            close(s);
        return;
    }
    chmod(addr.sun_path, 0600);
    pthread_t t;
    if (pthread_create(&t, NULL, server_main, (void *)(intptr_t)s) == 0)
        pthread_detach(t);
}

/* Sends a uevent to every udev monitor. A monitor that isn't reading misses
 * it, as a full netlink socket drops uevents. */
void evdev_uevent(uint32_t what)
{
    pthread_mutex_lock(&g_evdev_lock);
    struct {
        struct cham_evdev_record h;
        struct cham_uevent u;
    } r = {{CHAM_EVDEV_UEVENT, sizeof r.u}, {what, 0, ++g_seqnum}};
    const uint32_t subsystem = CHAM_UEVENT_SUBSYSTEM_DRM; /* the card's, so far the only uevent */
    int sent = 0;
    for (int i = 0; i < MAX_MONITORS; i++) {
        if (!g_monitors[i].fd || !(g_monitors[i].subsystems & subsystem))
            continue;
        ssize_t n = send(g_monitors[i].fd, &r, sizeof r, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n == (ssize_t)sizeof r) {
            sent++;
            continue;
        }
        cham_log("evdev: a udev monitor missed an event");
        if (n > 0) /* half a record: its stream is broken, it ends here */
            shutdown(g_monitors[i].fd, SHUT_RDWR);
    }
    cham_log("udev: card hotplug sent to %d monitor%s", sent, sent == 1 ? "" : "s");
    pthread_mutex_unlock(&g_evdev_lock);
}

/* ---- events (input thread) ---- */

static void push(struct device *d, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event e = {.type = type, .code = code, .value = value};
    if (d->frame_len < FRAME_EVENTS) {
        d->frame[d->frame_len++] = e;
        cham_evdev_state_apply(&d->state, &e);
    }
}

/* Queues an event for the next frame the way the kernel's input core passes
 * events on: a key only when it changes, a relative motion only when it
 * moves, an axis only when its value changes (a multitouch axis: in the
 * current slot), and ABS_MT_SLOT held back until a value of another slot
 * than the last one passed on follows. */
static void add(struct device *d, uint16_t type, uint16_t code, int32_t value)
{
    pthread_mutex_lock(&g_evdev_lock);
    struct cham_evdev_state *s = &d->state;
    int pass = 1;
    if (type == EV_KEY) {
        pass = code < KEY_CNT && cham_bit_test(d->info.key, code) &&
               (value == 2 || cham_bit_test(s->key, code) != !!value);
    } else if (type == EV_REL) {
        pass = value != 0;
    } else if (type == EV_ABS && code == ABS_MT_SLOT) {
        if (value >= 0 && value <= d->info.absinfo[ABS_MT_SLOT].maximum)
            d->staged_slot = value;
        pass = 0;
    } else if (type == EV_ABS && code < ABS_CNT) {
        int mt = cham_is_mt_value(code) && cham_bit_test(d->info.abs, ABS_MT_SLOT);
        int32_t old = mt ? s->slot[d->staged_slot][code - ABS_MT_TOUCH_MAJOR] : s->abs[code];
        pass = old != value;
        if (pass && mt && d->staged_slot != s->abs[ABS_MT_SLOT])
            push(d, EV_ABS, ABS_MT_SLOT, d->staged_slot);
    }
    if (pass)
        push(d, type, code, value);
    pthread_mutex_unlock(&g_evdev_lock);
}

/* Sends the queued events and a SYN_REPORT to every reader, as one write;
 * nothing if no event is queued (the kernel drops unchanged values too). */
static void flush(struct device *d)
{
    if (!d->frame_len)
        return;
    /* When it happened, by the app's clock (the same CLOCK_MONOTONIC): never
     * later than now, never earlier than the device's last frame. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    int64_t t = g_event_time && g_event_time < now ? g_event_time : now;
    t = t > d->last_time ? t : d->last_time;
    d->last_time = t;
    pthread_mutex_lock(&g_evdev_lock);
    push(d, EV_SYN, SYN_REPORT, 0);
    for (int i = 0; i < d->frame_len; i++) {
        d->frame[i].input_event_sec = t / 1000000000;
        d->frame[i].input_event_usec = t % 1000000000 / 1000;
    }
    /* While a reader holds the grab, the others' devices still change. */
    for (int i = 0; i < MAX_READERS; i++)
        if (d->readers[i].fd)
            queue_record(d, &d->readers[i], d->grab < 0 || d->grab == i ? CHAM_EVDEV_FRAME : CHAM_EVDEV_FRAME_GRABBED,
                         d->frame, d->frame_len * sizeof *d->frame, NULL, 0);
    pthread_mutex_unlock(&g_evdev_lock);
    d->frame_len = 0;
}


/* A key or button event, unless it would leave the device stuck: a press
 * goes to the device only while it has readers, a release wherever its
 * press went. */
static int key(struct device *d, unsigned code, int down)
{
    if (code >= KEY_CNT || !cham_bit_test(d->info.key, code))
        return 0;
    add(d, EV_KEY, (uint16_t)code, down);
    flush(d);
    return 1;
}

int evdev_keyboard_has(unsigned code)
{
    return code < KEY_CNT && cham_bit_test(g_dev[DEV_KEYBOARD].info.key, code);
}

static void abs_value(struct device *d, uint16_t code, int32_t value, int32_t *last)
{
    if (value != *last)
        add(d, EV_ABS, code, value);
    *last = value;
}

static int slot_of(struct mt *t, int id, int claim)
{
    for (int i = 0; i < TOUCH_SLOTS; i++)
        if (t->slot_id[i] == id)
            return i;
    for (int i = 0; claim && i < TOUCH_SLOTS; i++) {
        if (t->slot_id[i] < 0) {
            t->slot_id[i] = id;
            return i;
        }
    }
    return -1;
}

static void select_slot(struct device *d, struct mt *t, int slot)
{
    abs_value(d, ABS_MT_SLOT, slot, &t->cur_slot);
}

void evdev_window(uint32_t width, uint32_t height)
{
    g_window_w = width;
    g_window_h = height;
}

/* pos as fractions of the screen; the touchpad's axes are its pixels. The
 * touchpad is the app's whole window, also where the keyboard covers the
 * desktop, so its pixels don't change with the desktop's size; an app that
 * doesn't report the window gives fractions of the desktop. */
static void position(struct device *d, struct mt *t, int slot, uint64_t pos)
{
    int32_t x = (int32_t)CHAM_INPUT_X(pos), y = (int32_t)CHAM_INPUT_Y(pos);
    if (t == &g_pad) {
        uint32_t w = g_window_w, h = g_window_h, mm_w, mm_h;
        if (!w || !h)
            kms_mode_size(&w, &h);
        if (!w || !h) /* before the compositor's first modeset */
            kms_screen_size(&w, &h, &mm_w, &mm_h);
        pthread_mutex_lock(&g_evdev_lock);
        int32_t size = g_pad_size;
        pthread_mutex_unlock(&g_evdev_lock);
        x = (int32_t)((int64_t)x * (int32_t)w / 65536);
        y = (int32_t)((int64_t)y * (int32_t)h / 65536);
        if (size > 0) {
            x = x < size ? x : size - 1;
            y = y < size ? y : size - 1;
        }
    }
    if (x == t->x[slot] && y == t->y[slot])
        return;
    select_slot(d, t, slot);
    abs_value(d, ABS_MT_POSITION_X, x, &t->x[slot]);
    abs_value(d, ABS_MT_POSITION_Y, y, &t->y[slot]);
    if (slot == 0) { /* single-touch axes follow the first finger */
        abs_value(d, ABS_X, x, &t->abs_x);
        abs_value(d, ABS_Y, y, &t->abs_y);
    }
}

static void lift(struct device *d, struct mt *t, int slot)
{
    select_slot(d, t, slot);
    add(d, EV_ABS, ABS_MT_TRACKING_ID, -1);
    t->slot_id[slot] = -1;
    if (--t->touching == 0)
        add(d, EV_KEY, BTN_TOUCH, 0);
}

/* The touchpad's finger count, as the one BTN_TOOL_* key held. */
static void tool(struct device *d, struct mt *t)
{
    static const uint16_t keys[] = {0, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP,
                                    BTN_TOOL_QUINTTAP};
    int n = t->touching < 5 ? t->touching : 5;
    if (n == t->tool)
        return;
    if (t->tool)
        add(d, EV_KEY, keys[t->tool], 0);
    if (n)
        add(d, EV_KEY, keys[n], 1);
    t->tool = n;
}

/* Touch messages, kind as CHAM_INPUT_TOUCH_* (the touchpad's mapped). */
static int touch(struct device *d, struct mt *t, unsigned kind, const struct cham_msg *m)
{
    int slot;
    switch (kind) {
    case CHAM_INPUT_TOUCH_DOWN:
        if ((slot = slot_of(t, (int)m->a, 1)) < 0)
            break;
        select_slot(d, t, slot);
        add(d, EV_ABS, ABS_MT_TRACKING_ID, t->tracking_id++ & 0xffff);
        if (t->touching++ == 0)
            add(d, EV_KEY, BTN_TOUCH, 1);
        t->x[slot] = t->y[slot] = -1; /* a new contact reports its position */
        position(d, t, slot, m->b);
        break;
    case CHAM_INPUT_TOUCH_MOTION:
        if ((slot = slot_of(t, (int)m->a, 0)) >= 0)
            position(d, t, slot, m->b);
        break;
    case CHAM_INPUT_TOUCH_UP:
        if ((slot = slot_of(t, (int)m->a, 0)) >= 0)
            lift(d, t, slot);
        break;
    case CHAM_INPUT_TOUCH_CANCEL:
        for (int i = 0; i < TOUCH_SLOTS; i++)
            if (t->slot_id[i] >= 0)
                lift(d, t, i);
        __attribute__((fallthrough)); /* and a frame */
    case CHAM_INPUT_TOUCH_FRAME:
        if (t == &g_pad)
            tool(d, t);
        flush(d);
        break;
    }
    return 1;
}

static int wheel(struct device *d, int horizontal, int32_t hi_res)
{
    int32_t *rest = &g_wheel_rest[horizontal];
    *rest += hi_res;
    int32_t notches = *rest / 120;
    *rest -= notches * 120;
    if (hi_res)
        add(d, EV_REL, horizontal ? REL_HWHEEL_HI_RES : REL_WHEEL_HI_RES, hi_res);
    if (notches)
        add(d, EV_REL, horizontal ? REL_HWHEEL : REL_WHEEL, notches);
    flush(d);
    return 1;
}

/* A mouse button: on the pointer device that moved last; a release where
 * its press went. */
static struct device *button_device(unsigned code, int down)
{
    struct device *t = &g_dev[DEV_TABLET];
    if (down || code >= KEY_CNT)
        return g_pointer;
    pthread_mutex_lock(&g_evdev_lock);
    int on_tablet = cham_bit_test(t->state.key, code);
    pthread_mutex_unlock(&g_evdev_lock);
    return on_tablet ? t : &g_dev[DEV_MOUSE];
}

int evdev_input(const struct cham_msg *m)
{
    struct device *mouse = &g_dev[DEV_MOUSE];
    switch (m->id) {
    case CHAM_INPUT_TIME:
        g_event_time = (int64_t)m->b;
        return 1;
    case CHAM_INPUT_TOUCH_DOWN:
    case CHAM_INPUT_TOUCH_MOTION:
    case CHAM_INPUT_TOUCH_UP:
    case CHAM_INPUT_TOUCH_CANCEL:
    case CHAM_INPUT_TOUCH_FRAME:
        return touch(&g_dev[DEV_TOUCH], &g_touch, m->id, m);
    case CHAM_INPUT_PAD_DOWN:
    case CHAM_INPUT_PAD_MOTION:
    case CHAM_INPUT_PAD_UP:
    case CHAM_INPUT_PAD_CANCEL:
    case CHAM_INPUT_PAD_FRAME:
        return touch(&g_dev[DEV_TOUCHPAD], &g_pad, m->id - CHAM_INPUT_PAD_DOWN + CHAM_INPUT_TOUCH_DOWN, m);
    case CHAM_INPUT_KEY:
        return key(&g_dev[DEV_KEYBOARD], (unsigned)m->a, m->b != 0);
    case CHAM_INPUT_MOUSE_BUTTON:
        return key(button_device((unsigned)m->a, m->b != 0), (unsigned)m->a, m->b != 0);
    case CHAM_INPUT_MOUSE_WHEEL:
        return wheel(g_pointer, m->a != 0, (int32_t)(int64_t)m->b);
    case CHAM_INPUT_POINTER_ABS: {
        struct device *t = &g_dev[DEV_TABLET];
        g_pointer = t;
        add(t, EV_ABS, ABS_X, (int32_t)CHAM_INPUT_X(m->b));
        add(t, EV_ABS, ABS_Y, (int32_t)CHAM_INPUT_Y(m->b));
        flush(t);
        return 1;
    }
    case CHAM_INPUT_MOUSE_MOTION:
        g_pointer = mouse;
        if (CHAM_INPUT_DX(m->b))
            add(mouse, EV_REL, REL_X, CHAM_INPUT_DX(m->b));
        if (CHAM_INPUT_DY(m->b))
            add(mouse, EV_REL, REL_Y, CHAM_INPUT_DY(m->b));
        flush(mouse);
        return 1;
    }
    return 0;
}
