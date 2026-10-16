/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Chameleon's input devices, as evdev devices: libchameleon.so inside KWin
 * (which gets the app's input over the presenter link) serves them on a
 * local socket, and libchameleon_udev.so makes them appear in any process as
 * udev devices with /dev/input/eventN nodes.
 *
 * Socket: $XDG_RUNTIME_DIR/chameleon-evdev (SOCK_STREAM). On connect the
 * server writes a uint32_t device count and that many struct
 * cham_evdev_info; the client then either closes, or writes a uint32_t
 * device index and from then on exchanges records (struct
 * cham_evdev_record, then `size` bytes) with the server: first a STATE,
 * then the device's frames as they happen. A udev monitor writes
 * CHAM_EVDEV_MONITOR instead of an index, then a uint32_t of
 * CHAM_UEVENT_SUBSYSTEM_* bits (the subsystems its filters let through), and
 * receives UEVENT records, one per uevent that passes, as the netlink
 * socket of a real monitor would.
 *
 * The server applies the kernel's input core rules (events that change
 * nothing are dropped, ABS_MT_SLOT is held back until a slot's value
 * changes), so the frames are what the kernel's evdev would pass on; the
 * client keeps each open file's queue the way evdev does.
 */
#ifndef CHAMELEON_EVDEV_H
#define CHAMELEON_EVDEV_H

#include <linux/input.h>
#include <stdint.h>

#define CHAM_EVDEV_SOCKET_NAME "chameleon-evdev"
#define CHAM_EVDEV_MAX_DEVICES 8

#define CHAM_BITS(n) (((n) + 7) / 8)

struct cham_evdev_info {
    char name[80];
    char udev_type[32];       /* "ID_INPUT_TOUCHSCREEN", ... space-separated (each "1" in udev) */
    struct input_id id;
    uint8_t ev[CHAM_BITS(EV_CNT)];
    uint8_t key[CHAM_BITS(KEY_CNT)];
    uint8_t rel[CHAM_BITS(REL_CNT)];
    uint8_t abs[CHAM_BITS(ABS_CNT)];
    uint8_t prop[CHAM_BITS(INPUT_PROP_CNT)];
    struct input_absinfo absinfo[ABS_CNT];
};

#define CHAM_EVDEV_SLOTS 10 /* multitouch slots at most */
#define CHAM_MT_CODES (ABS_MT_TOOL_Y - ABS_MT_TOUCH_MAJOR + 1)

/* What evdev's state ioctls report: the device as it is now. */
struct cham_evdev_state {
    uint8_t key[CHAM_BITS(KEY_CNT)];        /* keys and buttons held */
    int32_t abs[ABS_CNT];                   /* axis values; ABS_MT_SLOT's is the current slot */
    int32_t slot[CHAM_EVDEV_SLOTS][CHAM_MT_CODES]; /* each slot's multitouch values */
};

struct cham_evdev_record {
    uint32_t type;
    uint32_t size; /* bytes after this header */
};

enum cham_evdev_record_type {
    /* server -> client */
    CHAM_EVDEV_STATE = 1,     /* struct cham_evdev_state; again later = events were lost */
    CHAM_EVDEV_FRAME,         /* struct input_event[], the last a SYN_REPORT */
    CHAM_EVDEV_FRAME_GRABBED, /* the same while another client holds the grab: state only */
    CHAM_EVDEV_GRAB_RESULT,   /* int32_t: 0 or -errno, for the client's last GRAB */
    CHAM_EVDEV_ABSINFO,       /* uint32_t axis, struct input_absinfo: EVIOCSABS by any client */
    CHAM_EVDEV_UEVENT,        /* struct cham_uevent, to monitors */
    /* client -> server */
    CHAM_EVDEV_GRAB = 16,     /* int32_t: 1 grab, 0 release (EVIOCGRAB) */
    CHAM_EVDEV_SET_ABS,       /* uint32_t axis, struct input_absinfo (EVIOCSABS) */
};

/* The "index" a udev monitor connects with. */
#define CHAM_EVDEV_MONITOR 0x4d4f4e49u /* "MONI" */

/* A uevent: so far only the card's "change" with HOTPLUG=1, which the kernel
 * sends when a connector's modes or status changed. */
enum { CHAM_UEVENT_CARD_HOTPLUG = 1 };
enum { CHAM_UEVENT_SUBSYSTEM_DRM = 1, CHAM_UEVENT_SUBSYSTEM_INPUT = 2 };
struct cham_uevent {
    uint32_t what;
    uint32_t pad;
    uint64_t seqnum;
};

static inline void cham_bit_set(uint8_t *bits, unsigned n)
{
    bits[n / 8] |= (uint8_t)(1u << (n % 8));
}

static inline int cham_bit_test(const uint8_t *bits, unsigned n)
{
    return (bits[n / 8] >> (n % 8)) & 1;
}

static inline int cham_is_mt_value(unsigned code)
{
    return code >= ABS_MT_TOUCH_MAJOR && code <= ABS_MT_TOOL_Y;
}

static inline void cham_evdev_state_init(struct cham_evdev_state *s)
{
    *s = (struct cham_evdev_state){0};
    for (int i = 0; i < CHAM_EVDEV_SLOTS; i++)
        s->slot[i][ABS_MT_TRACKING_ID - ABS_MT_TOUCH_MAJOR] = -1; /* no contact */
}

/* Applies an event the kernel passed on to the device state. */
static inline void cham_evdev_state_apply(struct cham_evdev_state *s, const struct input_event *e)
{
    if (e->type == EV_KEY && e->code < KEY_CNT) {
        if (e->value == 1)
            cham_bit_set(s->key, e->code);
        else if (e->value == 0)
            s->key[e->code / 8] &= (uint8_t) ~(1u << (e->code % 8));
    } else if (e->type == EV_ABS && e->code < ABS_CNT) {
        int32_t slot = s->abs[ABS_MT_SLOT];
        if (!cham_is_mt_value(e->code))
            s->abs[e->code] = e->value;
        else if (slot >= 0 && slot < CHAM_EVDEV_SLOTS)
            s->slot[slot][e->code - ABS_MT_TOUCH_MAJOR] = e->value;
    }
}

/* How many events evdev queues per open file of this device: the kernel's
 * estimate of a frame's size, times 8 frames, at least 64, rounded up to a
 * power of two. */
static inline unsigned cham_evdev_queue_size(const struct cham_evdev_info *d)
{
    unsigned slots = 0, events, n = 1;
    if (cham_bit_test(d->abs, ABS_MT_SLOT))
        slots = (unsigned)d->absinfo[ABS_MT_SLOT].maximum + 1;
    events = slots + 1;
    if (cham_bit_test(d->ev, EV_ABS))
        for (unsigned a = 0; a < ABS_CNT; a++)
            if (cham_bit_test(d->abs, a))
                events += a == ABS_MT_SLOT || cham_is_mt_value(a) ? slots : 1;
    if (cham_bit_test(d->ev, EV_REL))
        for (unsigned r = 0; r < REL_CNT; r++)
            events += cham_bit_test(d->rel, r);
    events += 7; /* keys and MSC */
    events = events * 8 > 64 ? events * 8 : 64;
    while (n < events)
        n <<= 1;
    return n;
}

#endif
