/*
 * libc / libdrm entry points libchameleon.so overrides when LD_PRELOADed
 * into kwin_wayland.
 *
 * Opening the fake device path ($CHAMELEON_DRM_PATH, what KWIN_DRM_DEVICES
 * points at) returns the read end of a pipe: KWin polls and read()s DRM
 * events from it, and every DRM ioctl on it is answered by kms.c. stat() and
 * fstat() make it look like a DRM character device, and libdrm's
 * drmGetDevice*() describe it. bind() is watched to learn KWin's Wayland
 * socket (input.c), and the constructor hands KWin's children their
 * original LD_PRELOAD / LD_LIBRARY_PATH. (crash.c overrides sigaction.)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <xf86drm.h>

#include "internal.h"

/* Minor < 64 is a primary node; real Android cards are 226:0. */
static const dev_t fake_rdev = (dev_t)((226u << 8) | 60u);

/* The overrides of libc/libdrm symbols below must be exported. */
#pragma GCC visibility push(default)

static pthread_mutex_t g_fd_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fake_fd *g_fake_fds;

#define REAL(name) next_##name
#define DECLARE_REAL(ret, name, ...)                                   \
    static ret (*next_##name)(__VA_ARGS__);                            \
    static void resolve_##name(void)                                   \
    {                                                                  \
        if (!next_##name)                                              \
            *(void **)&next_##name = dlsym(RTLD_NEXT, #name);          \
    }

DECLARE_REAL(int, open, const char *, int, ...)
DECLARE_REAL(int, openat, int, const char *, int, ...)
DECLARE_REAL(int, __open_2, const char *, int)
DECLARE_REAL(int, __openat_2, int, const char *, int)
DECLARE_REAL(int, stat, const char *, struct stat *)
DECLARE_REAL(int, lstat, const char *, struct stat *)
DECLARE_REAL(int, fstatat, int, const char *, struct stat *, int)
DECLARE_REAL(int, fstat, int, struct stat *)
DECLARE_REAL(int, ioctl, int, int, ...)
DECLARE_REAL(int, drmGetDevice2, int, uint32_t, drmDevicePtr *)
DECLARE_REAL(int, drmGetDevice, int, drmDevicePtr *)
DECLARE_REAL(char *, drmGetDeviceNameFromFd2, int)
DECLARE_REAL(int, bind, int, const struct sockaddr *, socklen_t)

static const char *fake_path(void)
{
    const char *p = getenv("CHAMELEON_DRM_PATH");
    return p && *p ? p : "/data/data/com.termux/files/usr/tmp/chameleon-card0";
}

static int is_fake_path(const char *path)
{
    return path && strcmp(path, fake_path()) == 0;
}

__attribute__((visibility("hidden"))) int real_fstat(int fd, struct stat *st)
{
    resolve_fstat();
    return REAL(fstat)(fd, st);
}

static struct fake_fd *fake_fd_lookup(int fd)
{
    struct stat st;
    if (fd < 0 || real_fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode))
        return NULL;
    pthread_mutex_lock(&g_fd_lock);
    struct fake_fd *f = g_fake_fds;
    while (f && !(f->dev == st.st_dev && f->ino == st.st_ino))
        f = f->next;
    pthread_mutex_unlock(&g_fd_lock);
    return f;
}

static void fake_stat(struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR | 0660;
    st->st_rdev = fake_rdev;
    st->st_nlink = 1;
    st->st_uid = getuid();
    st->st_gid = getgid();
}

static int fake_open(void)
{
    kms_init_once();
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0)
        return -1;
    struct stat st;
    real_fstat(p[0], &st);
    struct fake_fd *f = calloc(1, sizeof *f);
    f->dev = st.st_dev;
    f->ino = st.st_ino;
    f->event_wfd = p[1];
    pthread_mutex_lock(&g_fd_lock);
    f->next = g_fake_fds;
    g_fake_fds = f;
    pthread_mutex_unlock(&g_fd_lock);
    cham_log("opened fake DRM device %s (fd %d)", fake_path(), p[0]);
    return p[0];
}

/* ---- open ---- */

int open(const char *path, int flags, ...)
{
    if (is_fake_path(path))
        return fake_open();
    resolve_open();
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return REAL(open)(path, flags, mode);
}

int open64(const char *path, int flags, ...) __attribute__((alias("open")));

int openat(int dirfd, const char *path, int flags, ...)
{
    if (is_fake_path(path))
        return fake_open();
    resolve_openat();
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return REAL(openat)(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...) __attribute__((alias("openat")));

int __open_2(const char *path, int flags)
{
    if (is_fake_path(path))
        return fake_open();
    resolve___open_2();
    return REAL(__open_2)(path, flags);
}

int __openat_2(int dirfd, const char *path, int flags)
{
    if (is_fake_path(path))
        return fake_open();
    resolve___openat_2();
    return REAL(__openat_2)(dirfd, path, flags);
}

/* ---- stat ---- */

int stat(const char *path, struct stat *st)
{
    if (is_fake_path(path)) {
        fake_stat(st);
        return 0;
    }
    resolve_stat();
    return REAL(stat)(path, st);
}

int lstat(const char *path, struct stat *st)
{
    if (is_fake_path(path)) {
        fake_stat(st);
        return 0;
    }
    resolve_lstat();
    return REAL(lstat)(path, st);
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    if (is_fake_path(path)) {
        fake_stat(st);
        return 0;
    }
    resolve_fstatat();
    return REAL(fstatat)(dirfd, path, st, flags);
}

int fstat(int fd, struct stat *st)
{
    int ret = real_fstat(fd, st);
    if (ret == 0 && S_ISFIFO(st->st_mode) && fake_fd_lookup(fd))
        fake_stat(st);
    return ret;
}

/* bionic's *64 variants are the same functions with a different struct name. */
int stat64(const char *path, struct stat64 *st)
{
    return stat(path, (struct stat *)st);
}

int lstat64(const char *path, struct stat64 *st)
{
    return lstat(path, (struct stat *)st);
}

int fstatat64(int dirfd, const char *path, struct stat64 *st, int flags)
{
    return fstatat(dirfd, path, (struct stat *)st, flags);
}

int fstat64(int fd, struct stat64 *st)
{
    return fstat(fd, (struct stat *)st);
}

/* ---- ioctl ---- */

int ioctl(int fd, int request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (_IOC_TYPE((unsigned int)request) == DRM_IOCTL_BASE) {
        struct fake_fd *f = fake_fd_lookup(fd);
        if (f) {
            int ret = kms_ioctl(f, (unsigned int)request, arg);
            if (ret < 0) {
                errno = -ret;
                return -1;
            }
            return ret;
        }
    }
    resolve_ioctl();
    return REAL(ioctl)(fd, request, arg);
}

/* ---- KWin's Wayland socket ---- */

/* libwayland-server binds "$XDG_RUNTIME_DIR/wayland-N"; input.c connects
 * to it to inject input through KWin's fake-input protocol. */
int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    resolve_bind();
    int ret = REAL(bind)(fd, addr, len);
    if (ret == 0 && addr && addr->sa_family == AF_UNIX && len > offsetof(struct sockaddr_un, sun_path)) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        char path[sizeof un->sun_path + 1];
        size_t n = len - offsetof(struct sockaddr_un, sun_path);
        if (n > sizeof un->sun_path)
            n = sizeof un->sun_path;
        memcpy(path, un->sun_path, n);
        path[n] = 0;
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (path[0] == '/' && strncmp(base, "wayland-", 8) == 0 && !strchr(base, '.')) /* not .lock / .chameleon */
            input_note_socket(path);
    }
    return ret;
}

/* ---- libdrm device identity ---- */

/* Laid out the way libdrm's drmFreeDevice() frees it: one block for the
 * device, bus/device info and node paths, plus a separately allocated
 * `compatible` array whose strings are each freed on their own. */
static drmDevicePtr make_device(void)
{
    const char *path = fake_path();
    size_t path_len = strlen(path) + 1;
    size_t size = sizeof(drmDevice) + DRM_NODE_MAX * sizeof(char *) + sizeof(drmPlatformBusInfo) +
                  sizeof(drmPlatformDeviceInfo) + path_len;
    char *mem = calloc(1, size);
    char **compatible = calloc(2, sizeof(char *));
    char *name = strdup("chameleon");
    if (!mem || !compatible || !name) {
        free(mem);
        free(compatible);
        free(name);
        return NULL;
    }
    drmDevicePtr dev = (drmDevicePtr)mem;
    char *p = mem + sizeof(drmDevice);
    dev->nodes = (char **)p;
    p += DRM_NODE_MAX * sizeof(char *);
    dev->businfo.platform = (drmPlatformBusInfoPtr)p;
    p += sizeof(drmPlatformBusInfo);
    dev->deviceinfo.platform = (drmPlatformDeviceInfoPtr)p;
    p += sizeof(drmPlatformDeviceInfo);
    memcpy(p, path, path_len);
    dev->nodes[DRM_NODE_PRIMARY] = p;
    compatible[0] = name;
    dev->deviceinfo.platform->compatible = compatible;
    strcpy(dev->businfo.platform->fullname, "/chameleon");
    dev->available_nodes = 1 << DRM_NODE_PRIMARY;
    dev->bustype = DRM_BUS_PLATFORM;
    return dev;
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
    if (fake_fd_lookup(fd)) {
        *device = make_device();
        return *device ? 0 : -ENOMEM;
    }
    resolve_drmGetDevice2();
    return REAL(drmGetDevice2) ? REAL(drmGetDevice2)(fd, flags, device) : -ENOSYS;
}

int drmGetDevice(int fd, drmDevicePtr *device)
{
    if (fake_fd_lookup(fd)) {
        *device = make_device();
        return *device ? 0 : -ENOMEM;
    }
    resolve_drmGetDevice();
    return REAL(drmGetDevice) ? REAL(drmGetDevice)(fd, device) : -ENOSYS;
}

char *drmGetDeviceNameFromFd2(int fd)
{
    if (fake_fd_lookup(fd))
        return strdup(fake_path());
    resolve_drmGetDeviceNameFromFd2();
    return REAL(drmGetDeviceNameFromFd2) ? REAL(drmGetDeviceNameFromFd2)(fd) : NULL;
}

/* ---- environment ----
 * The launcher saves the user's LD_PRELOAD / LD_LIBRARY_PATH. The dynamic
 * linker has already read ours, so restore theirs: Xwayland and every app
 * KWin starts must get Termux's normal Mesa, not this shim. */
__attribute__((constructor)) static void restore_environment(void)
{
    const char *saved = getenv("CHAMELEON_ORIG_LD_PRELOAD");
    if (saved) {
        if (*saved)
            setenv("LD_PRELOAD", saved, 1);
        else
            unsetenv("LD_PRELOAD");
        unsetenv("CHAMELEON_ORIG_LD_PRELOAD");
    }
    saved = getenv("CHAMELEON_ORIG_LD_LIBRARY_PATH");
    if (saved) {
        if (*saved)
            setenv("LD_LIBRARY_PATH", saved, 1);
        else
            unsetenv("LD_LIBRARY_PATH");
        unsetenv("CHAMELEON_ORIG_LD_LIBRARY_PATH");
    }
}

#pragma GCC visibility pop
