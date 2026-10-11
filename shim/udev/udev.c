/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libchameleon_udev.so: the udev side of the device layer, LD_PRELOADed
 * over the libudev the programs link (libudev-zero on Termux).
 *
 * Chameleon is the device manager here: device events come from it, never
 * from the kernel, whose uevent netlink socket an Android app can't open
 * anyway. So every udev monitor is Chameleon's own, and its fd only ever
 * becomes readable for Chameleon's events. KWin's GpuManager and libinput
 * both need a working monitor to start at all.
 *
 * Nothing sends events yet, so a monitor's fd never becomes readable and
 * udev_monitor_receive_device() has nothing to return.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

struct udev;
struct udev_device;

struct udev_monitor {
    int refs;
    int fd;
    struct udev *udev;
};

EXPORT struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev, const char *name)
{
    (void)name; /* "udev" or "kernel": Chameleon's events either way */
    if (!udev) {
        errno = EINVAL;
        return NULL;
    }
    struct udev_monitor *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m->fd < 0) {
        free(m);
        return NULL;
    }
    m->refs = 1;
    m->udev = udev;
    return m;
}

EXPORT struct udev_monitor *udev_monitor_ref(struct udev_monitor *m)
{
    if (m)
        m->refs++;
    return m;
}

EXPORT struct udev_monitor *udev_monitor_unref(struct udev_monitor *m)
{
    if (m && --m->refs == 0) {
        close(m->fd);
        free(m);
    }
    return NULL;
}

EXPORT struct udev *udev_monitor_get_udev(struct udev_monitor *m)
{
    return m ? m->udev : NULL;
}

EXPORT int udev_monitor_get_fd(struct udev_monitor *m)
{
    return m ? m->fd : -EINVAL;
}

EXPORT int udev_monitor_enable_receiving(struct udev_monitor *m)
{
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_set_receive_buffer_size(struct udev_monitor *m, int size)
{
    (void)size;
    return m ? 0 : -EINVAL;
}

/* Filters narrow down Chameleon's events, which are few and all wanted. */
EXPORT int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *m, const char *subsystem,
                                                           const char *devtype)
{
    (void)subsystem, (void)devtype;
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_filter_add_match_tag(struct udev_monitor *m, const char *tag)
{
    (void)tag;
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_filter_update(struct udev_monitor *m)
{
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_filter_remove(struct udev_monitor *m)
{
    return m ? 0 : -EINVAL;
}

EXPORT struct udev_device *udev_monitor_receive_device(struct udev_monitor *m)
{
    (void)m;
    errno = EAGAIN;
    return NULL;
}
