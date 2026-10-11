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
 * device index and from then on reads that device's struct input_event
 * stream, one whole SYN_REPORT frame per write.
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
    char udev_type[32];       /* "ID_INPUT_TOUCHSCREEN", ... (set to "1" in udev) */
    struct input_id id;
    uint8_t ev[CHAM_BITS(EV_CNT)];
    uint8_t key[CHAM_BITS(KEY_CNT)];
    uint8_t rel[CHAM_BITS(REL_CNT)];
    uint8_t abs[CHAM_BITS(ABS_CNT)];
    uint8_t prop[CHAM_BITS(INPUT_PROP_CNT)];
    struct input_absinfo absinfo[ABS_CNT];
};

static inline void cham_bit_set(uint8_t *bits, unsigned n)
{
    bits[n / 8] |= (uint8_t)(1u << (n % 8));
}

static inline int cham_bit_test(const uint8_t *bits, unsigned n)
{
    return (bits[n / 8] >> (n % 8)) & 1;
}

#endif
