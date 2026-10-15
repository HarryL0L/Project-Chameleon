/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The /dev/input/eventN nodes of Chameleon's input devices (udev.c lists
 * them). Opening one returns a socket to KWin's libchameleon.so, which
 * streams the device's struct input_event frames: read() and poll() work on
 * it natively. stat()/fstat() make it the character device udev named, and
 * the evdev ioctls libevdev asks are answered from the device's description.
 *
 * The sockets are recognised by (st_dev, st_ino), confirmed by their peer
 * being the evdev server, so close() needs no override.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

/* bionic's ioctl takes an int request, glibc's an unsigned long. */
#ifdef __BIONIC__
typedef int ioctl_request;
#else
typedef unsigned long ioctl_request;
#endif

#define NEXT(name)                                     \
    static __typeof__(name) *next_##name;              \
    if (!next_##name)                                  \
        *(void **)&next_##name = dlsym(RTLD_NEXT, #name)

#define MAX_OPEN 64
#define DEVNODE_PREFIX "/dev/input/event"

struct node {
    dev_t dev;
    ino_t ino;
    int index;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct node g_nodes[MAX_OPEN]; /* open sockets; the oldest is reused when full */
static int g_next_node;
static struct cham_evdev_info g_info[CHAM_EVDEV_MAX_DEVICES]; /* as of the last open */

static int real_fstat(int fd, struct stat *st)
{
    NEXT(fstat);
    return next_fstat(fd, st);
}

/* N if path is the node of one of the server's devices, else -1. */
static int index_of_node(const char *path)
{
    struct cham_evdev_info list[CHAM_EVDEV_MAX_DEVICES];
    int i = cham_evdev_index(path, DEVNODE_PREFIX);
    return i >= 0 && i < cham_evdev_list(list, CHAM_EVDEV_MAX_DEVICES) ? i : -1;
}

static int peer_is_server(int fd)
{
    struct sockaddr_un peer, want = {.sun_family = AF_UNIX};
    socklen_t len = sizeof peer;
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || getpeername(fd, (struct sockaddr *)&peer, &len) != 0 || peer.sun_family != AF_UNIX)
        return 0;
    snprintf(want.sun_path, sizeof want.sun_path, "%s/%s", dir, CHAM_EVDEV_SOCKET_NAME);
    return len > offsetof(struct sockaddr_un, sun_path) &&
           strncmp(peer.sun_path, want.sun_path, len - offsetof(struct sockaddr_un, sun_path)) == 0;
}

/* The device index behind fd if it is one of our nodes, else -1. */
static int index_of_fd(int fd, const struct stat *st)
{
    int index = -1;
    if (!S_ISSOCK(st->st_mode))
        return -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_OPEN; i++)
        if (g_nodes[i].ino == st->st_ino && g_nodes[i].dev == st->st_dev)
            index = g_nodes[i].index;
    pthread_mutex_unlock(&g_lock);
    return index >= 0 && peer_is_server(fd) ? index : -1;
}

static int open_node(int index, int flags)
{
    struct cham_evdev_info info;
    int fd = cham_evdev_open(index, flags & O_NONBLOCK, &info);
    struct stat st;
    if (fd < 0 || real_fstat(fd, &st) != 0) {
        if (fd >= 0)
            close(fd);
        errno = ENODEV;
        return -1;
    }
    if (!(flags & O_CLOEXEC))
        fcntl(fd, F_SETFD, 0);
    pthread_mutex_lock(&g_lock);
    int slot = g_next_node;
    for (int i = 0; i < MAX_OPEN; i++) /* a closed socket's inode may come back */
        if (g_nodes[i].ino == st.st_ino && g_nodes[i].dev == st.st_dev)
            slot = i;
    if (slot == g_next_node)
        g_next_node = (g_next_node + 1) % MAX_OPEN;
    g_nodes[slot] = (struct node){st.st_dev, st.st_ino, index};
    g_info[index] = info;
    pthread_mutex_unlock(&g_lock);
    return fd;
}

static void node_stat(int index, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR | 0660;
    st->st_rdev = cham_evdev_devnum(index);
    st->st_nlink = 1;
    st->st_uid = getuid();
    st->st_gid = getgid();
}

/* ---- open ---- */

static mode_t mode_arg(int flags, va_list ap)
{
    return flags & (O_CREAT | O_TMPFILE) ? (mode_t)va_arg(ap, int) : 0;
}

EXPORT int open(const char *path, int flags, ...)
{
    int i = index_of_node(path);
    if (i >= 0)
        return open_node(i, flags);
    NEXT(open);
    va_list ap;
    va_start(ap, flags);
    mode_t mode = mode_arg(flags, ap);
    va_end(ap);
    return next_open(path, flags, mode);
}

EXPORT int open64(const char *path, int flags, ...) __attribute__((alias("open")));

EXPORT int openat(int dirfd, const char *path, int flags, ...)
{
    int i = index_of_node(path);
    if (i >= 0)
        return open_node(i, flags);
    NEXT(openat);
    va_list ap;
    va_start(ap, flags);
    mode_t mode = mode_arg(flags, ap);
    va_end(ap);
    return next_openat(dirfd, path, flags, mode);
}

EXPORT int openat64(int dirfd, const char *path, int flags, ...) __attribute__((alias("openat")));

EXPORT int __open_2(const char *path, int flags)
{
    int i = index_of_node(path);
    if (i >= 0)
        return open_node(i, flags);
    NEXT(__open_2);
    return next___open_2(path, flags);
}

EXPORT int __openat_2(int dirfd, const char *path, int flags)
{
    int i = index_of_node(path);
    if (i >= 0)
        return open_node(i, flags);
    NEXT(__openat_2);
    return next___openat_2(dirfd, path, flags);
}

/* ---- stat ---- */

EXPORT int stat(const char *path, struct stat *st)
{
    int i = index_of_node(path);
    if (i >= 0) {
        node_stat(i, st);
        return 0;
    }
    NEXT(stat);
    return next_stat(path, st);
}

EXPORT int lstat(const char *path, struct stat *st)
{
    int i = index_of_node(path);
    if (i >= 0) {
        node_stat(i, st);
        return 0;
    }
    NEXT(lstat);
    return next_lstat(path, st);
}

EXPORT int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    int i = index_of_node(path);
    if (i >= 0) {
        node_stat(i, st);
        return 0;
    }
    NEXT(fstatat);
    return next_fstatat(dirfd, path, st, flags);
}

EXPORT int fstat(int fd, struct stat *st)
{
    int ret = real_fstat(fd, st), i;
    if (ret == 0 && (i = index_of_fd(fd, st)) >= 0)
        node_stat(i, st);
    return ret;
}

/* The *64 variants are the same functions with a different struct name. */
EXPORT int stat64(const char *path, struct stat64 *st)
{
    return stat(path, (struct stat *)st);
}

EXPORT int lstat64(const char *path, struct stat64 *st)
{
    return lstat(path, (struct stat *)st);
}

EXPORT int fstatat64(int dirfd, const char *path, struct stat64 *st, int flags)
{
    return fstatat(dirfd, path, (struct stat *)st, flags);
}

EXPORT int fstat64(int fd, struct stat64 *st)
{
    return fstat(fd, (struct stat *)st);
}

/* ---- evdev ioctls ---- */

/* Copies up to `size` bytes of a string, as the kernel does. */
static int copy_out(void *arg, unsigned size, const void *src, unsigned len)
{
    unsigned n = len < size ? len : size;
    memcpy(arg, src, n);
    return (int)n;
}

/* A bitmap of codes up to `max`, as the kernel copies it: whole longs (the
 * bitmap's own size), at most `size` bytes; returns the bytes copied. */
static int bits_out(void *arg, unsigned size, const uint8_t *bits, unsigned bits_len, unsigned max)
{
    unsigned char buf[CHAM_BITS(KEY_CNT) + sizeof(long)] = {0};
    unsigned len = (max + 8 * sizeof(long) - 1) / (8 * sizeof(long)) * sizeof(long); /* BITS_TO_LONGS */
    if (bits)
        memcpy(buf, bits, bits_len < len ? bits_len : len);
    return copy_out(arg, size, buf, len);
}

static int has_abs(const struct cham_evdev_info *d)
{
    return cham_bit_test(d->ev, EV_ABS);
}

static int evdev_ioctl(const struct cham_evdev_info *d, unsigned req, void *arg)
{
    unsigned nr = _IOC_NR(req), size = _IOC_SIZE(req);
    if (_IOC_DIR(req) == _IOC_READ) {
        if (nr >= 0x20 && nr < 0x20 + EV_CNT) { /* EVIOCGBIT: the types evdev knows */
            switch (nr - 0x20) {
            case 0: return bits_out(arg, size, d->ev, sizeof d->ev, EV_MAX);
            case EV_KEY: return bits_out(arg, size, d->key, sizeof d->key, KEY_MAX);
            case EV_REL: return bits_out(arg, size, d->rel, sizeof d->rel, REL_MAX);
            case EV_ABS: return bits_out(arg, size, d->abs, sizeof d->abs, ABS_MAX);
            case EV_MSC: return bits_out(arg, size, NULL, 0, MSC_MAX);
            case EV_LED: return bits_out(arg, size, NULL, 0, LED_MAX);
            case EV_SND: return bits_out(arg, size, NULL, 0, SND_MAX);
            case EV_FF: return bits_out(arg, size, NULL, 0, FF_MAX);
            case EV_SW: return bits_out(arg, size, NULL, 0, SW_MAX);
            }
            return -EINVAL;
        }
        if (nr >= 0x40 && nr < 0x40 + ABS_CNT) { /* EVIOCGABS: zeros for an axis it lacks */
            if (!has_abs(d))
                return -EINVAL;
            memcpy(arg, &d->absinfo[nr - 0x40],
                   size < sizeof(struct input_absinfo) ? size : sizeof(struct input_absinfo));
            return 0;
        }
        switch (nr) {
        case _IOC_NR(EVIOCGVERSION):
            *(int *)arg = EV_VERSION;
            return 0;
        case _IOC_NR(EVIOCGID):
            memcpy(arg, &d->id, sizeof d->id);
            return 0;
        case _IOC_NR(EVIOCGNAME(0)):
            return copy_out(arg, size, d->name, (unsigned)strlen(d->name) + 1);
        case _IOC_NR(EVIOCGPHYS(0)):
        case _IOC_NR(EVIOCGUNIQ(0)):
            return -ENOENT;
        case _IOC_NR(EVIOCGPROP(0)):
            return bits_out(arg, size, d->prop, sizeof d->prop, INPUT_PROP_MAX);
        case _IOC_NR(EVIOCGMTSLOTS(0)): { /* nothing touching: every slot free */
            uint32_t code = size >= sizeof code ? *(uint32_t *)arg : 0;
            if (size < sizeof code || !cham_bit_test(d->abs, ABS_MT_SLOT) || code < ABS_MT_TOUCH_MAJOR ||
                code > ABS_MT_TOOL_Y) /* the MT axes */
                return -EINVAL;
            int32_t *values = (int32_t *)arg + 1;
            unsigned slots = (unsigned)d->absinfo[ABS_MT_SLOT].maximum + 1, n = (size - sizeof code) / sizeof *values;
            for (unsigned i = 0; i < n && i < slots; i++)
                values[i] = code == ABS_MT_TRACKING_ID ? -1 : 0;
            return 0;
        }
        case _IOC_NR(EVIOCGKEY(0)): /* nothing held */
            return bits_out(arg, size, NULL, 0, KEY_MAX);
        case _IOC_NR(EVIOCGLED(0)):
            return bits_out(arg, size, NULL, 0, LED_MAX);
        case _IOC_NR(EVIOCGSND(0)):
            return bits_out(arg, size, NULL, 0, SND_MAX);
        case _IOC_NR(EVIOCGSW(0)):
            return bits_out(arg, size, NULL, 0, SW_MAX);
        case _IOC_NR(EVIOCGREP):
            return -ENOSYS; /* no key repeat (EV_REP) */
        case _IOC_NR(EVIOCGEFFECTS):
            *(int *)arg = 0;
            return 0;
        }
        return -EINVAL;
    }
    if (_IOC_DIR(req) == _IOC_WRITE) {
        if (nr >= 0xc0 && nr < 0xc0 + ABS_CNT) /* EVIOCSABS: the axes are fixed */
            return !has_abs(d) || nr - 0xc0 == ABS_MT_SLOT ? -EINVAL : 0;
        switch (nr) {
        case _IOC_NR(EVIOCGRAB):
        case _IOC_NR(EVIOCREVOKE):
            return 0;
        case _IOC_NR(EVIOCSCLOCKID): { /* timestamps are CLOCK_MONOTONIC, libinput's choice */
            int clk = *(const int *)arg;
            return clk == CLOCK_REALTIME || clk == CLOCK_MONOTONIC || clk == CLOCK_BOOTTIME ? 0 : -EINVAL;
        }
        }
    }
    return -EINVAL;
}

EXPORT int ioctl(int fd, ioctl_request request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    struct stat st;
    unsigned req = (unsigned)request;
    if (_IOC_TYPE(req) == 'E' && real_fstat(fd, &st) == 0) {
        int i = index_of_fd(fd, &st);
        if (i >= 0) {
            pthread_mutex_lock(&g_lock);
            int ret = evdev_ioctl(&g_info[i], req, arg);
            pthread_mutex_unlock(&g_lock);
            if (ret < 0) {
                errno = -ret;
                return -1;
            }
            return ret;
        }
    }
    static int (*next_ioctl)(int, ioctl_request, ...); /* bionic's ioctl is overloaded: no __typeof__ */
    if (!next_ioctl)
        *(void **)&next_ioctl = dlsym(RTLD_NEXT, "ioctl");
    return next_ioctl(fd, request, arg);
}
