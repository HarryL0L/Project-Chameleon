/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The app's input as evdev devices (common/chameleon_evdev.h), served on
 * $XDG_RUNTIME_DIR/chameleon-evdev to libchameleon_udev.so, which shows
 * them to libinput (KWin's own, here) and to any other program:
 *  0 touchscreen: the app's direct touch, multitouch protocol B;
 *  1 keyboard: hardware keyboards, by their Linux key codes;
 *  2 mouse: a relative mouse, fed from the mouse the app captures
 *    (Android's pointer capture: the mouse's own counts and buttons), with
 *    high-resolution wheels;
 *  3 touchpad: the app's trackpad mode, the screen as a laptop's touchpad
 *    without buttons (so libinput taps to click), in screen pixels.
 * Touch positions are fractions of the screen, so those axes run 0..65535
 * and the compositor maps them onto its output. While a program reads a
 * device, its input goes there; otherwise input.c sends it through KWin's
 * fake input as before (and so does everything else: trackpad mode, the soft
 * keyboard, a mouse the app hasn't captured; the touchpad has no fallback).
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

enum { DEV_TOUCH, DEV_KEYBOARD, DEV_MOUSE, DEV_TOUCHPAD, DEV_COUNT };

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
static struct mt {         /* a multitouch device (protocol B) */
    int slot_id[TOUCH_SLOTS]; /* the app's touch id per slot, -1 = free */
    int32_t x[TOUCH_SLOTS], y[TOUCH_SLOTS], cur_slot, abs_x, abs_y; /* as last sent */
    int tracking_id;
    int touching;
    int tool; /* the touchpad's BTN_TOOL_* finger count, as last sent */
} g_touch, g_pad;
static int32_t g_pad_size; /* the touchpad's axes: the screen's longer side, in pixels */
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
        g_touch.slot_id[i] = g_pad.slot_id[i] = -1;
    g_touch.cur_slot = g_touch.abs_x = g_touch.abs_y = -1;
    g_pad.cur_slot = g_pad.abs_x = g_pad.abs_y = -1;
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

/* pos as fractions of the screen; the touchpad's axes are its pixels. */
static void position(struct device *d, struct mt *t, int slot, uint64_t pos)
{
    int32_t x = (int32_t)CHAM_INPUT_X(pos), y = (int32_t)CHAM_INPUT_Y(pos);
    if (t == &g_pad) {
        uint32_t w, h, mm_w, mm_h;
        kms_mode_size(&w, &h);
        if (!w || !h) /* before KWin's first modeset */
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
    if (!t->touching && !has_readers(d))
        return 0;
    if (take_fresh(d)) {
        t->cur_slot = t->abs_x = t->abs_y = -1;
        for (int i = 0; i < TOUCH_SLOTS; i++)
            t->x[i] = t->y[i] = -1;
    }
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
        return touch(&g_dev[DEV_TOUCH], &g_touch, m->id, m);
    case CHAM_INPUT_PAD_DOWN:
    case CHAM_INPUT_PAD_MOTION:
    case CHAM_INPUT_PAD_UP:
    case CHAM_INPUT_PAD_CANCEL:
    case CHAM_INPUT_PAD_FRAME:
        touch(&g_dev[DEV_TOUCHPAD], &g_pad, m->id - CHAM_INPUT_PAD_DOWN + CHAM_INPUT_TOUCH_DOWN, m);
        return 1; /* no fallback: fake input has no touchpad */
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
