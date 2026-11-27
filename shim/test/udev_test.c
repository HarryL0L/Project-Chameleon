/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libchameleon_udev.so against the system's real libudev and libinput:
 * udev monitors are Chameleon's own (KWin's GpuManager uses one
 * unchecked), and libinput's udev backend starts on them. Subsystems other
 * than "input" are the real system's.
 * Run with LD_PRELOAD=libchameleon_udev.so.
 */
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "check.h"

static int open_restricted(const char *path, int flags, void *data)
{
    (void)data;
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *data)
{
    (void)data;
    close(fd);
}

static const struct libinput_interface iface = {open_restricted, close_restricted};

int main(void)
{
    struct udev *udev = udev_new();
    CHECK(udev != NULL, "udev_new");

    printf("udev monitor\n");
    struct udev_monitor *m = udev_monitor_new_from_netlink(udev, "udev");
    CHECK(m != NULL, "udev_monitor_new_from_netlink gives a monitor");
    char link[PATH_MAX] = "", path[64];
    int fd = udev_monitor_get_fd(m);
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    ssize_t n = fd >= 0 ? readlink(path, link, sizeof link - 1) : -1;
    if (n > 0)
        link[n] = '\0';
    CHECK(strstr(link, "eventfd") != NULL, "it is Chameleon's, not the kernel's netlink socket (%s)", link);
    CHECK(udev_monitor_filter_add_match_subsystem_devtype(m, "drm", NULL) == 0 && udev_monitor_enable_receiving(m) == 0,
          "filter and enable succeed");
    CHECK(udev_monitor_get_udev(m) == udev, "udev_monitor_get_udev");
    struct pollfd pfd = {fd, POLLIN, 0};
    CHECK(poll(&pfd, 1, 0) == 0, "no events yet");
    errno = 0;
    CHECK(udev_monitor_receive_device(m) == NULL && errno == EAGAIN, "receive_device: nothing (EAGAIN)");
    CHECK(udev_monitor_ref(m) == m, "ref");
    udev_monitor_unref(m);
    CHECK(fcntl(fd, F_GETFD) != -1, "still open after unref with a ref left");
    udev_monitor_unref(m);
    CHECK(fcntl(fd, F_GETFD) == -1, "closed after the last unref");

    printf("udev enumerate\n");
    struct udev_enumerate *e = udev_enumerate_new(udev);
    CHECK(e && udev_enumerate_add_match_subsystem(e, "mem") == 0 && udev_enumerate_scan_devices(e) == 0,
          "enumerate subsystem mem");
    int found = 0;
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, e ? udev_enumerate_get_list_entry(e) : NULL)
        found |= strcmp(udev_list_entry_get_name(entry), "/sys/devices/virtual/mem/null") == 0;
    CHECK(found, "the real /dev/null's device is listed");
    udev_enumerate_unref(e);
    e = udev_enumerate_new(udev);
    CHECK(e && udev_enumerate_scan_devices(e) == 0, "enumerate everything");
    int all = 0, input = 0;
    udev_list_entry_foreach(entry, e ? udev_enumerate_get_list_entry(e) : NULL) {
        struct udev_device *d = udev_device_new_from_syspath(udev, udev_list_entry_get_name(entry));
        const char *subsystem = d ? udev_device_get_subsystem(d) : NULL;
        all++;
        input += subsystem && strcmp(subsystem, "input") == 0;
        udev_device_unref(d);
    }
    CHECK(all > 0 && input == 0, "real devices, none of the real input ones (%d listed, %d input)", all, input);
    udev_enumerate_unref(e);

    printf("libinput's udev backend\n");
    struct libinput *li = libinput_udev_create_context(&iface, NULL, udev);
    CHECK(li != NULL, "libinput_udev_create_context");
    CHECK(li && libinput_udev_assign_seat(li, "seat0") == 0, "libinput_udev_assign_seat starts on the monitor");
    CHECK(li && libinput_dispatch(li) == 0, "libinput_dispatch");
    if (li)
        libinput_unref(li);
    udev_unref(udev);

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
