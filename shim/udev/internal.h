/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private to libchameleon_udev.so. */
#ifndef CHAM_UDEV_INTERNAL_H
#define CHAM_UDEV_INTERNAL_H

#include <stddef.h>
#include <sys/stat.h>
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
/* Whether the card exists: the compositor's libchameleon.so is serving. */
int cham_card_present(void);

/* evdev.c: the next definition of a function this library overrides
 * (dlsym(RTLD_NEXT)), looked up when the library loads: dlsym takes the
 * dynamic linker's lock, which a forked child may have inherited held by a
 * thread that no longer exists there. */
void *cham_next(const char *name);

/* sysfs.c: `path` inside the card's sysfs entries or /dev/dri, as its place
 * in the tree standing for them (in buf); else `path` itself. */
const char *cham_sys_path(const char *path, char *buf, size_t size);
/* 1 and fills *st if `path` is the card's node, else 0. */
int cham_sys_node_stat(const char *path, struct stat *st);

#endif
