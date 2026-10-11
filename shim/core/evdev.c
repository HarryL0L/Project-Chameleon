/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The app's input as evdev devices (common/chameleon_evdev.h), served on
 * $XDG_RUNTIME_DIR/chameleon-evdev to libchameleon_udev.so, which shows
 * them to libinput (KWin's own, here) and to any other program.
 *
 * One device so far: a touchscreen, fed from the app's touch messages.
 * Positions are fractions of the screen, so the axes run 0..65535 and the
 * compositor maps them onto its output as for any touchscreen. While a
 * program reads the device, touches go there; otherwise input.c sends them
 * through KWin's fake input as before.
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

#define TOUCH_SLOTS 10
#define MAX_READERS 8
#define FRAME_EVENTS (TOUCH_SLOTS * 5 + 4)

static struct cham_evdev_info g_touch_info;
static pthread_mutex_t g_evdev_lock = PTHREAD_MUTEX_INITIALIZER;
static struct reader {
    int fd; /* 0 = free */
    size_t pending; /* bytes of a partly sent frame still to send */
    char rest[FRAME_EVENTS * sizeof(struct input_event)];
} g_readers[MAX_READERS]; /* the touchscreen's */
static int g_reader_count;

/* Touch state, on the input thread only. */
static int g_slot_id[TOUCH_SLOTS]; /* the app's touch id per slot, -1 = free */
static int g_tracking_id;
static int g_touching;
static struct input_event g_frame[FRAME_EVENTS];
static int g_frame_len;

static void describe_touchscreen(void)
{
    struct cham_evdev_info *d = &g_touch_info;
    snprintf(d->name, sizeof d->name, "Chameleon touchscreen");
    snprintf(d->udev_type, sizeof d->udev_type, "ID_INPUT_TOUCHSCREEN");
    d->id.bustype = BUS_VIRTUAL;
    cham_bit_set(d->prop, INPUT_PROP_DIRECT);
    cham_bit_set(d->ev, EV_SYN);
    cham_bit_set(d->ev, EV_KEY);
    cham_bit_set(d->ev, EV_ABS);
    cham_bit_set(d->key, BTN_TOUCH);
    const unsigned axes[] = {ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID};
    for (size_t i = 0; i < sizeof axes / sizeof axes[0]; i++)
        cham_bit_set(d->abs, axes[i]);
    d->absinfo[ABS_X].maximum = d->absinfo[ABS_Y].maximum = 65535;
    d->absinfo[ABS_MT_POSITION_X].maximum = d->absinfo[ABS_MT_POSITION_Y].maximum = 65535;
    d->absinfo[ABS_MT_SLOT].maximum = TOUCH_SLOTS - 1;
    d->absinfo[ABS_MT_TRACKING_ID].maximum = 65535;
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
    uint32_t count = 1, index = UINT32_MAX;
    if (write_all(c, &count, sizeof count) || write_all(c, &g_touch_info, sizeof g_touch_info))
        goto done;
    /* A reader asks for its device right away; a lister just closes. */
    struct pollfd p = {c, POLLIN, 0};
    if (poll(&p, 1, 1000) != 1 || recv(c, &index, sizeof index, MSG_WAITALL) != (ssize_t)sizeof index || index != 0)
        goto done;
    fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
    pthread_mutex_lock(&g_evdev_lock);
    for (int i = 0; i < MAX_READERS; i++) {
        if (!g_readers[i].fd) {
            g_readers[i] = (struct reader){.fd = c};
            g_reader_count++;
            c = -1;
            break;
        }
    }
    pthread_mutex_unlock(&g_evdev_lock);
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
    describe_touchscreen();
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

/* ---- touchscreen events (input thread) ---- */

static void add(uint16_t type, uint16_t code, int32_t value)
{
    if (g_frame_len < FRAME_EVENTS)
        g_frame[g_frame_len++] = (struct input_event){.type = type, .code = code, .value = value};
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

/* Sends the queued events and a SYN_REPORT to every reader, as one write. */
static void flush(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    add(EV_SYN, SYN_REPORT, 0);
    for (int i = 0; i < g_frame_len; i++) {
        g_frame[i].input_event_sec = ts.tv_sec;
        g_frame[i].input_event_usec = ts.tv_nsec / 1000;
    }
    pthread_mutex_lock(&g_evdev_lock);
    for (int i = 0; i < MAX_READERS; i++) {
        struct reader *r = &g_readers[i];
        if (!r->fd)
            continue;
        /* A reader too slow to take a whole frame loses it, like a full evdev buffer. */
        int ret = r->pending ? send_some(r, r->rest, r->pending) : 0;
        if (ret == 0 && !r->pending)
            ret = send_some(r, (const char *)g_frame, g_frame_len * sizeof *g_frame);
        if (ret < 0) {
            close(r->fd);
            r->fd = 0;
            g_reader_count--;
        }
    }
    pthread_mutex_unlock(&g_evdev_lock);
    g_frame_len = 0;
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

static void position(int slot, uint64_t pos)
{
    add(EV_ABS, ABS_MT_SLOT, slot);
    add(EV_ABS, ABS_MT_POSITION_X, (int32_t)CHAM_INPUT_X(pos));
    add(EV_ABS, ABS_MT_POSITION_Y, (int32_t)CHAM_INPUT_Y(pos));
    if (slot == 0) { /* single-touch axes follow the first finger */
        add(EV_ABS, ABS_X, (int32_t)CHAM_INPUT_X(pos));
        add(EV_ABS, ABS_Y, (int32_t)CHAM_INPUT_Y(pos));
    }
}

static void lift(int slot)
{
    add(EV_ABS, ABS_MT_SLOT, slot);
    add(EV_ABS, ABS_MT_TRACKING_ID, -1);
    g_slot_id[slot] = -1;
    if (--g_touching == 0)
        add(EV_KEY, BTN_TOUCH, 0);
}

int evdev_touch(const struct cham_msg *m)
{
    pthread_mutex_lock(&g_evdev_lock);
    int readers = g_reader_count;
    pthread_mutex_unlock(&g_evdev_lock);
    if (!readers && !g_touching)
        return 0;
    int slot;
    switch (m->id) {
    case CHAM_INPUT_TOUCH_DOWN:
        if ((slot = slot_of((int)m->a, 1)) < 0)
            break;
        add(EV_ABS, ABS_MT_SLOT, slot);
        add(EV_ABS, ABS_MT_TRACKING_ID, g_tracking_id++ & 0xffff);
        if (g_touching++ == 0)
            add(EV_KEY, BTN_TOUCH, 1);
        position(slot, m->b);
        break;
    case CHAM_INPUT_TOUCH_MOTION:
        if ((slot = slot_of((int)m->a, 0)) >= 0)
            position(slot, m->b);
        break;
    case CHAM_INPUT_TOUCH_UP:
        if ((slot = slot_of((int)m->a, 0)) >= 0)
            lift(slot);
        break;
    case CHAM_INPUT_TOUCH_CANCEL:
        for (int i = 0; i < TOUCH_SLOTS; i++)
            if (g_slot_id[i] >= 0)
                lift(i);
        flush();
        break;
    case CHAM_INPUT_TOUCH_FRAME:
        flush();
        break;
    default:
        return 0;
    }
    return 1;
}
