/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The input devices libchameleon.so serves, seen by the system's real
 * libinput through libchameleon_udev.so: a stub presenter sends touch, key
 * and mouse messages, and libinput's udev backend must find the Chameleon
 * touchscreen, keyboard and mouse and report the input. Run with LD_PRELOAD=libchameleon.so:libchameleon_udev.so, as
 * kwin_wayland is.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../common/cham_io.h"

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

static void send_input(int c, uint32_t kind, uint64_t a, uint64_t b)
{
    struct cham_msg m = {CHAM_INPUT, kind, a, b};
    cham_send(c, &m, -1);
}

/* The next libinput event of `type` within 2 s, other events skipped. */
static struct libinput_event *wait_event(struct libinput *li, enum libinput_event_type type)
{
    struct pollfd p = {libinput_get_fd(li), POLLIN, 0};
    for (int tries = 0; tries < 100; tries++) {
        libinput_dispatch(li);
        struct libinput_event *ev;
        while ((ev = libinput_get_event(li))) {
            if (libinput_event_get_type(ev) == type)
                return ev;
            libinput_event_destroy(ev);
        }
        poll(&p, 1, 20);
    }
    return NULL;
}

int main(void)
{
    const char *dev_path = getenv("CHAMELEON_DRM_PATH");
    const char *sock_path = getenv("CHAMELEON_SOCKET");
    if (!dev_path || !sock_path || !getenv("XDG_RUNTIME_DIR")) {
        fprintf(stderr, "run via run-host-test.sh\n");
        return 2;
    }

    int l = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, sock_path, sizeof addr.sun_path - 1);
    unlink(sock_path);
    bind(l, (struct sockaddr *)&addr, sizeof addr);
    listen(l, 1);
    int fd = open(dev_path, O_RDWR | O_CLOEXEC); /* starts the shim's presenter link */
    CHECK(fd >= 0, "open fake DRM device");
    struct pollfd p = {l, POLLIN, 0};
    CHECK(poll(&p, 1, 5000) == 1, "shim connects to the presenter");
    int c = accept(l, NULL, NULL);

    printf("udev\n");
    struct udev *udev = udev_new();
    struct udev_enumerate *e = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(e, "input");
    udev_enumerate_add_match_property(e, "ID_INPUT_TOUCHSCREEN", "1");
    udev_enumerate_scan_devices(e);
    struct udev_list_entry *first = udev_enumerate_get_list_entry(e);
    const char *syspath = first ? udev_list_entry_get_name(first) : NULL;
    CHECK(syspath && !udev_list_entry_get_next(first), "one touchscreen listed (%s)", syspath ? syspath : "none");
    struct udev_device *d = syspath ? udev_device_new_from_syspath(udev, syspath) : NULL;
    const char *node = d ? udev_device_get_devnode(d) : NULL;
    CHECK(node && strcmp(node, "/dev/input/event0") == 0, "its node is /dev/input/event0");
    struct stat st;
    CHECK(node && stat(node, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == udev_device_get_devnum(d),
          "stat: a character device with udev's number");
    int nfd = node ? open(node, O_RDONLY | O_NONBLOCK) : -1;
    CHECK(nfd >= 0 && fstat(nfd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == udev_device_get_devnum(d),
          "open + fstat: the same device");
    if (nfd >= 0)
        close(nfd);
    if (d)
        udev_device_unref(d);
    udev_enumerate_unref(e);

    printf("libinput\n");
    struct libinput *li = libinput_udev_create_context(&iface, NULL, udev);
    CHECK(li && libinput_udev_assign_seat(li, "seat0") == 0, "libinput_udev_assign_seat");
    int touchscreen = 0, keyboard = 0, mouse = 0;
    struct libinput_event *ev;
    for (int i = 0; li && i < 3 && (ev = wait_event(li, LIBINPUT_EVENT_DEVICE_ADDED)); i++) {
        struct libinput_device *dev = libinput_event_get_device(ev);
        const char *name = libinput_device_get_name(dev);
        touchscreen |= !strcmp(name, "Chameleon touchscreen") &&
                       libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_TOUCH);
        keyboard |= !strcmp(name, "Chameleon keyboard") &&
                    libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD);
        mouse |= !strcmp(name, "Chameleon mouse") && libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER);
        libinput_event_destroy(ev);
    }
    CHECK(touchscreen, "libinput adds the touchscreen as a touch device");
    CHECK(keyboard, "libinput adds the keyboard as a keyboard");
    CHECK(mouse, "libinput adds the mouse as a pointer");

    send_input(c, CHAM_INPUT_TOUCH_DOWN, 3, CHAM_INPUT_POS(32768, 16384)); /* (0.5, 0.25) */
    send_input(c, CHAM_INPUT_TOUCH_FRAME, 0, 0);
    send_input(c, CHAM_INPUT_TOUCH_DOWN, 7, CHAM_INPUT_POS(0, 65535)); /* second finger */
    send_input(c, CHAM_INPUT_TOUCH_FRAME, 0, 0);
    send_input(c, CHAM_INPUT_TOUCH_MOTION, 3, CHAM_INPUT_POS(16384, 16384));
    send_input(c, CHAM_INPUT_TOUCH_FRAME, 0, 0);
    send_input(c, CHAM_INPUT_TOUCH_UP, 3, 0);
    send_input(c, CHAM_INPUT_TOUCH_UP, 7, 0);
    send_input(c, CHAM_INPUT_TOUCH_FRAME, 0, 0);

    double x = -1, y = -1;
    int slot0 = -1, slot1 = -1;
    if (li && (ev = wait_event(li, LIBINPUT_EVENT_TOUCH_DOWN))) {
        struct libinput_event_touch *t = libinput_event_get_touch_event(ev);
        x = libinput_event_touch_get_x_transformed(t, 1000);
        y = libinput_event_touch_get_y_transformed(t, 1000);
        slot0 = libinput_event_touch_get_slot(t);
        libinput_event_destroy(ev);
    }
    CHECK(x > 499 && x < 501 && y > 249 && y < 251, "touch down at %.1f,%.1f (want 500,250)", x, y);
    if (li && (ev = wait_event(li, LIBINPUT_EVENT_TOUCH_DOWN))) {
        slot1 = libinput_event_touch_get_slot(libinput_event_get_touch_event(ev));
        libinput_event_destroy(ev);
    }
    CHECK(slot0 >= 0 && slot1 >= 0 && slot0 != slot1, "second finger in its own slot (%d, %d)", slot0, slot1);
    x = -1;
    if (li && (ev = wait_event(li, LIBINPUT_EVENT_TOUCH_MOTION))) {
        x = libinput_event_touch_get_x_transformed(libinput_event_get_touch_event(ev), 1000);
        libinput_event_destroy(ev);
    }
    CHECK(x > 249 && x < 251, "touch motion to x %.1f (want 250)", x);
    int ups = 0;
    while (li && ups < 2 && (ev = wait_event(li, LIBINPUT_EVENT_TOUCH_UP))) {
        ups++;
        libinput_event_destroy(ev);
    }
    CHECK(ups == 2, "both fingers up (%d)", ups);

    send_input(c, CHAM_INPUT_KEY, KEY_A, 1);
    send_input(c, CHAM_INPUT_KEY, KEY_A, 0);
    int key = -1, pressed = 0, released = 0;
    for (int i = 0; li && i < 2 && (ev = wait_event(li, LIBINPUT_EVENT_KEYBOARD_KEY)); i++) {
        struct libinput_event_keyboard *k = libinput_event_get_keyboard_event(ev);
        key = (int)libinput_event_keyboard_get_key(k);
        if (libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED)
            pressed++;
        else
            released++;
        libinput_event_destroy(ev);
    }
    CHECK(key == KEY_A && pressed == 1 && released == 1, "key A pressed and released (key %d)", key);

    send_input(c, CHAM_INPUT_MOUSE_MOTION, 0, CHAM_INPUT_DELTA(7, -3));
    send_input(c, CHAM_INPUT_MOUSE_BUTTON, BTN_RIGHT, 1);
    send_input(c, CHAM_INPUT_MOUSE_BUTTON, BTN_RIGHT, 0);
    send_input(c, CHAM_INPUT_MOUSE_WHEEL, 0, 120); /* a notch up */
    x = y = 0;
    if (li && (ev = wait_event(li, LIBINPUT_EVENT_POINTER_MOTION))) {
        struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
        x = libinput_event_pointer_get_dx_unaccelerated(p);
        y = libinput_event_pointer_get_dy_unaccelerated(p);
        libinput_event_destroy(ev);
    }
    CHECK(x == 7 && y == -3, "mouse moved %.1f,%.1f (want 7,-3)", x, y);
    int button = -1;
    if (li && (ev = wait_event(li, LIBINPUT_EVENT_POINTER_BUTTON))) {
        button = (int)libinput_event_pointer_get_button(libinput_event_get_pointer_event(ev));
        libinput_event_destroy(ev);
    }
    CHECK(button == BTN_RIGHT, "right button (button 0x%x)", button);
    double v120 = 0;
    if (li && (ev = wait_event(li, LIBINPUT_EVENT_POINTER_SCROLL_WHEEL))) {
        v120 = libinput_event_pointer_get_scroll_value_v120(libinput_event_get_pointer_event(ev),
                                                            LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        libinput_event_destroy(ev);
    }
    CHECK(v120 == -120, "wheel a notch up (v120 %.0f, want -120)", v120);

    if (li)
        libinput_unref(li);
    udev_unref(udev);
    close(c);
    close(fd);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
