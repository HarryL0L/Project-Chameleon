/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The app's input as evdev devices (common/chameleon_evdev.h), served on
 * $XDG_RUNTIME_DIR/chameleon-evdev to libchameleon_udev.so, which shows
 * them to libinput (KWin's own, here) and to any other program:
 *  0 touchscreen: the app's direct touch, multitouch protocol B;
 *  1 keyboard: hardware keyboards, by their Linux key codes;
 *  2 mouse: a relative mouse, fed from the mouse the app captures
 *    (Android's pointer capture: the mouse's own counts and buttons), with
 *    high-resolution wheels.
 * Touch positions are fractions of the screen, so those axes run 0..65535
 * and the compositor maps them onto its output. While a program reads a
 * device, its input goes there; otherwise input.c sends it through KWin's
 * fake input as before (and so does everything else: trackpad mode, the soft
 * keyboard, a mouse the app hasn't captured).
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

enum { DEV_TOUCH, DEV_KEYBOARD, DEV_MOUSE, DEV_COUNT };

#define TOUCH_SLOTS 10
#define MAX_READERS 8
#define FRAME_EVENTS (TOUCH_SLOTS * 5 + 4)
#define AXIS_MAX 65535

struct reader {
    int fd; /* 0 = free */
    size_t pending; /* bytes of a partly sent frame still to send */
    char rest[FRAME_EVENTS * sizeof(struct input_event)];
};

static struct device {
    struct cham_evdev_info info;   /* accept thread only, after evdev_start() */
    struct reader readers[MAX_READERS]; /* under g_evdev_lock */
    int reader_count;
    int fresh; /* a reader came: send whole state again, it doesn't know ours */
    struct input_event frame[FRAME_EVENTS]; /* input thread only */
    int frame_len;
} g_dev[DEV_COUNT];
static pthread_mutex_t g_evdev_lock = PTHREAD_MUTEX_INITIALIZER;

/* Device state, on the input thread only. */
static int g_slot_id[TOUCH_SLOTS]; /* the app's touch id per slot, -1 = free */
static int32_t g_slot_x[TOUCH_SLOTS], g_slot_y[TOUCH_SLOTS], g_cur_slot = -1, g_abs_x = -1, g_abs_y = -1;
static int g_tracking_id;
static int g_touching;
static uint8_t g_keys_down[CHAM_BITS(KEY_CNT)]; /* keys and buttons pressed on the devices */
static int32_t g_wheel_rest[2]; /* hi-res wheel movement short of a whole notch */

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
}

/* Units per mm from the size the connector reports (the touch axes span
 * the screen), so programs that measure in mm get sensible numbers. */
static void set_resolution(void)
{
    uint32_t mm_w, mm_h;
    kms_physical_size(&mm_w, &mm_h);
    if (!mm_w || !mm_h)
        return;
    struct cham_evdev_info *t = &g_dev[DEV_TOUCH].info;
    t->absinfo[ABS_X].resolution = t->absinfo[ABS_MT_POSITION_X].resolution = AXIS_MAX / (int32_t)mm_w;
    t->absinfo[ABS_Y].resolution = t->absinfo[ABS_MT_POSITION_Y].resolution = AXIS_MAX / (int32_t)mm_h;
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
    if (poll(&p, 1, 1000) != 1 || recv(c, &index, sizeof index, MSG_WAITALL) != (ssize_t)sizeof index ||
        index >= DEV_COUNT)
        goto done;
    fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
    struct device *d = &g_dev[index];
    pthread_mutex_lock(&g_evdev_lock);
    for (int i = 0; i < MAX_READERS; i++) {
        if (!d->readers[i].fd) {
            d->readers[i] = (struct reader){.fd = c};
            d->reader_count++;
            d->fresh = 1;
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

static void *server_main(void *arg)
{
    int s = (int)(intptr_t)arg;
    for (;;) {
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c >= 0)
            serve(c);
        else if (errno != EINTR && errno != ECONNABORTED)
            break;
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
        g_slot_id[i] = -1;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    unlink(addr.sun_path);
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

/* ---- events (input thread) ---- */

static void add(struct device *d, uint16_t type, uint16_t code, int32_t value)
{
    if (d->frame_len < FRAME_EVENTS)
        d->frame[d->frame_len++] = (struct input_event){.type = type, .code = code, .value = value};
}

/* Sends what it can of buf; keeps the rest for next time so the stream stays
 * whole events. -1 if the reader is gone. */
static int send_some(struct reader *r, const char *buf, size_t len)
{
    ssize_t n = send(r->fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0 && errno != EAGAIN)
        return -1;
    n = n < 0 ? 0 : n;
    r->pending = len - (size_t)n;
    memmove(r->rest, buf + n, r->pending);
    return 0;
}

/* Sends the queued events and a SYN_REPORT to every reader, as one write;
 * nothing if no event is queued (the kernel drops unchanged values too). */
static void flush(struct device *d)
{
    if (!d->frame_len)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    add(d, EV_SYN, SYN_REPORT, 0);
    for (int i = 0; i < d->frame_len; i++) {
        d->frame[i].input_event_sec = ts.tv_sec;
        d->frame[i].input_event_usec = ts.tv_nsec / 1000;
    }
    pthread_mutex_lock(&g_evdev_lock);
    for (int i = 0; i < MAX_READERS; i++) {
        struct reader *r = &d->readers[i];
        if (!r->fd)
            continue;
        /* A reader too slow to take a whole frame loses it, like a full evdev buffer. */
        int ret = r->pending ? send_some(r, r->rest, r->pending) : 0;
        if (ret == 0 && !r->pending)
            ret = send_some(r, (const char *)d->frame, d->frame_len * sizeof *d->frame);
        if (ret < 0) {
            close(r->fd);
            r->fd = 0;
            d->reader_count--;
            cham_log("evdev: a program closed the %s", d->info.name + strlen("Chameleon "));
        }
    }
    pthread_mutex_unlock(&g_evdev_lock);
    d->frame_len = 0;
}

static int has_readers(const struct device *d)
{
    pthread_mutex_lock(&g_evdev_lock);
    int n = d->reader_count;
    pthread_mutex_unlock(&g_evdev_lock);
    return n > 0;
}

static int take_fresh(struct device *d)
{
    pthread_mutex_lock(&g_evdev_lock);
    int fresh = d->fresh;
    d->fresh = 0;
    pthread_mutex_unlock(&g_evdev_lock);
    return fresh;
}

/* A key or button event, unless it would leave the device stuck: a press
 * goes to the device only while it has readers, a release wherever its
 * press went. */
static int key(struct device *d, unsigned code, int down)
{
    if (code >= KEY_CNT || !cham_bit_test(d->info.key, code))
        return 0;
    int held = cham_bit_test(g_keys_down, code);
    if (down ? !has_readers(d) : !held)
        return 0;
    if (down == held)
        return 1;
    g_keys_down[code / 8] ^= (uint8_t)(1u << (code % 8));
    add(d, EV_KEY, (uint16_t)code, down);
    flush(d);
    return 1;
}

static void abs_value(struct device *d, uint16_t code, int32_t value, int32_t *last)
{
    if (value != *last)
        add(d, EV_ABS, code, value);
    *last = value;
}

static int slot_of(int id, int claim)
{
    for (int i = 0; i < TOUCH_SLOTS; i++)
        if (g_slot_id[i] == id)
            return i;
    for (int i = 0; claim && i < TOUCH_SLOTS; i++) {
        if (g_slot_id[i] < 0) {
            g_slot_id[i] = id;
            return i;
        }
    }
    return -1;
}

static void select_slot(struct device *d, int slot)
{
    abs_value(d, ABS_MT_SLOT, slot, &g_cur_slot);
}

static void position(struct device *d, int slot, uint64_t pos)
{
    int32_t x = (int32_t)CHAM_INPUT_X(pos), y = (int32_t)CHAM_INPUT_Y(pos);
    if (x == g_slot_x[slot] && y == g_slot_y[slot])
        return;
    select_slot(d, slot);
    abs_value(d, ABS_MT_POSITION_X, x, &g_slot_x[slot]);
    abs_value(d, ABS_MT_POSITION_Y, y, &g_slot_y[slot]);
    if (slot == 0) { /* single-touch axes follow the first finger */
        abs_value(d, ABS_X, x, &g_abs_x);
        abs_value(d, ABS_Y, y, &g_abs_y);
    }
}

static void lift(struct device *d, int slot)
{
    select_slot(d, slot);
    add(d, EV_ABS, ABS_MT_TRACKING_ID, -1);
    g_slot_id[slot] = -1;
    if (--g_touching == 0)
        add(d, EV_KEY, BTN_TOUCH, 0);
}

static int touch(const struct cham_msg *m)
{
    struct device *d = &g_dev[DEV_TOUCH];
    if (!g_touching && !has_readers(d))
        return 0;
    if (take_fresh(d)) {
        g_cur_slot = g_abs_x = g_abs_y = -1;
        for (int i = 0; i < TOUCH_SLOTS; i++)
            g_slot_x[i] = g_slot_y[i] = -1;
    }
    int slot;
    switch (m->id) {
    case CHAM_INPUT_TOUCH_DOWN:
        if ((slot = slot_of((int)m->a, 1)) < 0)
            break;
        select_slot(d, slot);
        add(d, EV_ABS, ABS_MT_TRACKING_ID, g_tracking_id++ & 0xffff);
        if (g_touching++ == 0)
            add(d, EV_KEY, BTN_TOUCH, 1);
        g_slot_x[slot] = g_slot_y[slot] = -1; /* a new contact reports its position */
        position(d, slot, m->b);
        break;
    case CHAM_INPUT_TOUCH_MOTION:
        if ((slot = slot_of((int)m->a, 0)) >= 0)
            position(d, slot, m->b);
        break;
    case CHAM_INPUT_TOUCH_UP:
        if ((slot = slot_of((int)m->a, 0)) >= 0)
            lift(d, slot);
        break;
    case CHAM_INPUT_TOUCH_CANCEL:
        for (int i = 0; i < TOUCH_SLOTS; i++)
            if (g_slot_id[i] >= 0)
                lift(d, i);
        flush(d);
        break;
    case CHAM_INPUT_TOUCH_FRAME:
        flush(d);
        break;
    }
    return 1;
}

static int wheel(struct device *d, int horizontal, int32_t hi_res)
{
    if (!has_readers(d))
        return 0;
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

int evdev_input(const struct cham_msg *m)
{
    struct device *mouse = &g_dev[DEV_MOUSE];
    switch (m->id) {
    case CHAM_INPUT_TOUCH_DOWN:
    case CHAM_INPUT_TOUCH_MOTION:
    case CHAM_INPUT_TOUCH_UP:
    case CHAM_INPUT_TOUCH_CANCEL:
    case CHAM_INPUT_TOUCH_FRAME:
        return touch(m);
    case CHAM_INPUT_KEY:
        return key(&g_dev[DEV_KEYBOARD], (unsigned)m->a, m->b != 0);
    case CHAM_INPUT_MOUSE_BUTTON:
        return key(mouse, (unsigned)m->a, m->b != 0);
    case CHAM_INPUT_MOUSE_WHEEL:
        return wheel(mouse, m->a != 0, (int32_t)(int64_t)m->b);
    case CHAM_INPUT_MOUSE_MOTION:
        if (!has_readers(mouse))
            return 0;
        if (CHAM_INPUT_DX(m->b))
            add(mouse, EV_REL, REL_X, CHAM_INPUT_DX(m->b));
        if (CHAM_INPUT_DY(m->b))
            add(mouse, EV_REL, REL_Y, CHAM_INPUT_DY(m->b));
        flush(mouse);
        return 1;
    }
    return 0;
}
