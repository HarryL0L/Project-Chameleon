/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The card's sysfs entries and its /dev/dri directory, as the kernel shows a
 * platform card with a primary and a render node (virtio-gpu on 6.8 has the
 * same nodes):
 *
 *   /sys/dev/char/226:0 -> ../../devices/platform/chameleon/drm/card0
 *   /sys/dev/char/226:128 -> ../../devices/platform/chameleon/drm/renderD128
 *   /sys/devices/platform/chameleon/{uevent,subsystem -> bus/platform}
 *   .../drm/{card0,renderD128}/{uevent,dev,subsystem -> class/drm,device -> chameleon}
 *   /dev/dri listing card0 and renderD128
 *
 * libdrm reads them to name and type the card (drmGetPrimaryDeviceNameFromFd,
 * drmGetNodeTypeFromFd, drmGetDevice2...). They are a real tree under
 * $XDG_RUNTIME_DIR/chameleon-sys, which these paths lead to while the card
 * exists (the compositor's libchameleon.so serving), as udev lists it. The
 * node itself is libchameleon.so's; here it only stats and is accessible.
 *
 * Only the /dev/dri directory itself is the tree's: it lists the card's
 * nodes next to the real entries the program can see, and those stay the
 * real ones. A real card0 or renderD128 (a GPU's, on the few devices that
 * let apps reach one) is hidden behind ours: neither can drive Android's
 * screen or share buffers with it.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "internal.h"

#define NEXT(name)                                     \
    static __typeof__(name) *next_##name;              \
    if (!next_##name)                                  \
        *(void **)&next_##name = cham_next(#name)

#define STR(x) #x
#define XSTR(x) STR(x)
#define CARD_CHAR_DEV "/sys/dev/char/226:" XSTR(CHAM_DRM_MINOR)
#define RENDER_CHAR_DEV "/sys/dev/char/226:" XSTR(CHAM_RENDER_MINOR)
#define DEVICE "/sys/devices/platform/chameleon"

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static char g_root[PATH_MAX], g_real_root[PATH_MAX]; /* "" if the tree couldn't be made */

static int write_file(const char *path, const char *text)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    size_t n = strlen(text);
    int ok = write(fd, text, n) == (ssize_t)n;
    close(fd);
    if (!ok || rename(tmp, path) != 0) { /* whole files only, for processes building it at once */
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int make_dirs(const char *path)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, 0700) != 0 && errno != EEXIST)
                return -1;
            *s = '/';
        }
    }
    return mkdir(p, 0700) != 0 && errno != EEXIST ? -1 : 0;
}

static int link_to(const char *target, const char *path)
{
    return symlink(target, path) != 0 && errno != EEXIST ? -1 : 0;
}

/* One node's entries: its directory under the card's drm directory, its
 * /sys/dev/char link and its name in /dev/dri. */
static int make_node(const char *name, unsigned minor)
{
    char p[PATH_MAX + 64], dir[PATH_MAX + 64], text[128]; /* g_root leaves room for the longest entry */
#define AT(rel, ...) (snprintf(p, sizeof p, "%s" rel, g_root, __VA_ARGS__), p)
    snprintf(dir, sizeof dir, "%s" DEVICE "/drm/%s", g_root, name);
    snprintf(text, sizeof text, "MAJOR=226\nMINOR=%u\nDEVNAME=dri/%s\nDEVTYPE=drm_minor\n", minor, name);
    int ok = make_dirs(dir) == 0 && write_file(AT(DEVICE "/drm/%s/uevent", name), text) == 0;
    snprintf(text, sizeof text, "226:%u\n", minor);
    return ok && write_file(AT(DEVICE "/drm/%s/dev", name), text) == 0 &&
                   link_to("../../../../../class/drm", AT(DEVICE "/drm/%s/subsystem", name)) == 0 &&
                   link_to("../../../chameleon", AT(DEVICE "/drm/%s/device", name)) == 0 &&
                   (snprintf(text, sizeof text, "../../devices/platform/chameleon/drm/%s", name),
                    link_to(text, AT("/sys/dev/char/226:%u", minor)) == 0) &&
                   write_file(AT("/dev/dri/%s", name), "") == 0 /* for the listing; the node is libchameleon.so's */
               ? 0
               : -1;
#undef AT
}

static void build_tree(void)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || snprintf(g_root, sizeof g_root, "%s/chameleon-sys", dir) >= (int)sizeof g_root - 64) {
        g_root[0] = 0;
        return;
    }
    char p[PATH_MAX + 64];
#define AT(rel) (snprintf(p, sizeof p, "%s%s", g_root, rel), p)
    int ok = make_dirs(AT("/sys/dev/char")) == 0 && make_dirs(AT("/sys/bus/platform")) == 0 &&
             make_dirs(AT("/sys/class/drm")) == 0 && make_dirs(AT("/dev/dri")) == 0 &&
             make_dirs(AT(DEVICE "/drm")) == 0 &&
             write_file(AT(DEVICE "/uevent"), "MODALIAS=platform:chameleon\n") == 0 &&
             link_to("../../../bus/platform", AT(DEVICE "/subsystem")) == 0 &&
             make_node(CHAM_DRM_NAME, CHAM_DRM_MINOR) == 0 && make_node(CHAM_RENDER_NAME, CHAM_RENDER_MINOR) == 0;
    /* Past our own open() (bionic's opendir() calls it, and /dev/dri leads
     * back here while the tree is being built). */
    NEXT(openat);
    int real_fd = ok ? next_openat(AT_FDCWD, "/dev/dri", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    DIR *real = real_fd >= 0 ? fdopendir(real_fd) : NULL;
    if (real_fd >= 0 && !real)
        close(real_fd);
    for (struct dirent *e; real && (e = readdir(real));) {
        if (e->d_name[0] == '.' || strcmp(e->d_name, CHAM_DRM_NAME) == 0 || strcmp(e->d_name, CHAM_RENDER_NAME) == 0)
            continue;
        char name[16 + sizeof e->d_name];
        snprintf(name, sizeof name, "/dev/dri/%s", e->d_name);
        if (e->d_type == DT_DIR)
            make_dirs(AT(name)); /* by-path */
        else
            write_file(AT(name), "");
    }
    if (real)
        closedir(real);
#undef AT
    NEXT(realpath);
    if (!ok || !next_realpath(g_root, g_real_root))
        g_root[0] = 0;
}

/* 1 for the card's primary node, as named in udev (CHAMELEON_DRM_PATH) or
 * by libdrm; 2 for its render node; 0 for any other path. */
static int is_node(const char *path)
{
    const char *p = getenv("CHAMELEON_DRM_PATH");
    if (!path)
        return 0;
    if (strcmp(path, CHAM_DRM_PATH) == 0 || (p && *p && strcmp(path, p) == 0))
        return 1;
    return strcmp(path, CHAM_RENDER_PATH) == 0 ? 2 : 0;
}

static int prefixed(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 && (path[n] == 0 || path[n] == '/');
}

const char *cham_sys_path(const char *path, char *buf, size_t size)
{
    if (!path || path[0] != '/' || is_node(path) ||
        (!prefixed(path, CARD_CHAR_DEV) && !prefixed(path, RENDER_CHAR_DEV) && !prefixed(path, DEVICE) &&
         strcmp(path, "/dev/dri") != 0 && strcmp(path, "/dev/dri/") != 0))
        return path;
    pthread_once(&g_once, build_tree);
    if (!g_root[0] || !cham_card_present() || snprintf(buf, size, "%s%s", g_root, path) >= (int)size)
        return path;
    return buf;
}

int cham_sys_node_stat(const char *path, struct stat *st)
{
    int node = is_node(path);
    if (!node || !cham_card_present())
        return 0;
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR | (node == 2 ? 0666 : 0660); /* the render node open to every user, by mode */
    st->st_rdev = makedev(226, node == 2 ? CHAM_RENDER_MINOR : CHAM_DRM_MINOR);
    st->st_nlink = 1;
    st->st_uid = getuid();
    st->st_gid = getgid();
    return 1;
}

EXPORT FILE *fopen(const char *path, const char *mode)
{
    char buf[PATH_MAX];
    NEXT(fopen);
    return next_fopen(cham_sys_path(path, buf, sizeof buf), mode);
}

EXPORT FILE *fopen64(const char *path, const char *mode) __attribute__((alias("fopen")));

EXPORT int access(const char *path, int mode)
{
    char buf[PATH_MAX];
    if (is_node(path) && cham_card_present()) /* uaccess, or the render node's mode: the user may use it */
        return 0;
    NEXT(access);
    return next_access(cham_sys_path(path, buf, sizeof buf), mode);
}

EXPORT int faccessat(int dirfd, const char *path, int mode, int flags)
{
    char buf[PATH_MAX];
    if (is_node(path) && cham_card_present())
        return 0;
    NEXT(faccessat);
    return next_faccessat(dirfd, cham_sys_path(path, buf, sizeof buf), mode, flags);
}

EXPORT DIR *opendir(const char *path)
{
    char buf[PATH_MAX];
    NEXT(opendir);
    return next_opendir(cham_sys_path(path, buf, sizeof buf));
}

EXPORT ssize_t readlink(const char *path, char *out, size_t size)
{
    char buf[PATH_MAX];
    NEXT(readlink);
    return next_readlink(cham_sys_path(path, buf, sizeof buf), out, size);
}

EXPORT ssize_t readlinkat(int dirfd, const char *path, char *out, size_t size)
{
    char buf[PATH_MAX];
    NEXT(readlinkat);
    return next_readlinkat(dirfd, cham_sys_path(path, buf, sizeof buf), out, size);
}

/* Resolved inside the tree, then named as the system path it stands for. */
EXPORT char *realpath(const char *path, char *resolved)
{
    char buf[PATH_MAX];
    const char *p = cham_sys_path(path, buf, sizeof buf);
    NEXT(realpath);
    char *r = next_realpath(p, resolved);
    size_t n = strlen(g_real_root);
    if (r && p != path && n && strncmp(r, g_real_root, n) == 0)
        memmove(r, r + n, strlen(r + n) + 1);
    return r;
}

/* The checked variants programs built with _FORTIFY_SOURCE call instead. */
EXPORT ssize_t __readlink_chk(const char *path, char *out, size_t size, size_t out_size)
{
    char buf[PATH_MAX];
    static ssize_t (*next)(const char *, char *, size_t, size_t);
    if (!next)
        *(void **)&next = cham_next("__readlink_chk");
    path = cham_sys_path(path, buf, sizeof buf);
    return next ? next(path, out, size, out_size) : readlink(path, out, size);
}

EXPORT ssize_t __readlinkat_chk(int dirfd, const char *path, char *out, size_t size, size_t out_size)
{
    char buf[PATH_MAX];
    static ssize_t (*next)(int, const char *, char *, size_t, size_t);
    if (!next)
        *(void **)&next = cham_next("__readlinkat_chk");
    path = cham_sys_path(path, buf, sizeof buf);
    return next ? next(dirfd, path, out, size, out_size) : readlinkat(dirfd, path, out, size);
}

EXPORT char *__realpath_chk(const char *path, char *resolved, size_t resolved_size)
{
    if (resolved && resolved_size < PATH_MAX)
        abort(); /* what the checked variant does */
    return realpath(path, resolved);
}
