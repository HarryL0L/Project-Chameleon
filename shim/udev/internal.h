/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private to libchameleon_udev.so. */
#ifndef CHAM_UDEV_INTERNAL_H
#define CHAM_UDEV_INTERNAL_H

#include <sys/sysmacros.h>
#include <sys/types.h>

#include "../../common/chameleon_drm.h"
#include "../../common/chameleon_evdev.h"

#define EXPORT __attribute__((visibility("default")))

/* Device N is /dev/input/eventN with this number: evdev's major, and a minor
 * far above the kernel's own evdev devices. */
#define CHAM_EVDEV_MINOR_BASE 512
static inline dev_t cham_evdev_devnum(int index)
{
    return makedev(13, CHAM_EVDEV_MINOR_BASE + index);
}

/* The devices KWin's libchameleon.so serves; 0 if it isn't running. */
int cham_evdev_list(struct cham_evdev_info *out, int max);
/* A connection to the server for device `index` (its records follow:
 * common/chameleon_evdev.h), or -1; fills *info with the device's
 * description. */
int cham_evdev_open(int index, struct cham_evdev_info *info);
/* N for "/dev/input/eventN" (devnode) or ".../eventN" (syspath), else -1. */
int cham_evdev_index(const char *path, const char *prefix);

#endif
