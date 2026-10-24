/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libc entry points libchameleon.so overrides when LD_PRELOADed
 * into kwin_wayland.
 *
 * The card's node (/dev/dri/card0, or $CHAMELEON_DRM_PATH;
 * libchameleon_udev.so lists it as a drm device) is served by
 * chameleon-kernel (card.c), as a device node by the kernel: opening it
 * returns the read end of a pipe the program polls and read()s DRM events
 * from, and a DRM ioctl on it is sent there as the system call's
 * arguments (the service reads and writes our memory itself). Each open is
 * one file with its own state (DRM master, client caps, GEM handles),
 * shared by its dups and by other processes it is passed to, and released
 * once no copy of the pipe's read end is left. stat() and fstat() make it
 * look like a DRM character device; libdrm learns the rest from the card's
 * sysfs entries (shim/udev/sysfs.c). bind() is watched to learn KWin's
 * Wayland socket (wlpath.c), and the constructor hands KWin's children
 * their original LD_PRELOAD / LD_LIBRARY_PATH. (crash.c overrides
 * sigaction.)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <xf86drm.h>

#include "../../common/cham_io.h"
#include "../../common/chameleon_drm.h"
#include "internal.h"

static const dev_t fake_rdev = (dev_t)((226u << 8) | CHAM_DRM_MINOR);

/* The overrides of libc symbols below must be exported. */
#pragma GCC visibility push(default)


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
DECLARE_REAL(int, bind, int, const struct sockaddr *, socklen_t)

static const char *fake_path(void)
{
    const char *p = getenv("CHAMELEON_DRM_PATH");
    return p && *p ? p : CHAM_DRM_PATH;
}

/* Its node as udev names it, or as libdrm does from sysfs. */
static int is_fake_path(const char *path)
{
    return path && (strcmp(path, fake_path()) == 0 || strcmp(path, CHAM_DRM_PATH) == 0);
}

__attribute__((visibility("hidden"))) int real_fstat(int fd, struct stat *st)
{
    resolve_fstat();
    return REAL(fstat)(fd, st);
}

/* ---- chameleon-kernel ---- */

/* One connection per process, one call at a time (the service learns the
 * caller from it, so a forked child makes its own). */
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_conn = -1;
static pid_t g_conn_pid;

/* Pipes known to be card files here, and pipes known not to be: pipe inode
 * numbers aren't reused, so neither answer goes stale. Under g_conn_lock. */
#define KNOWN 64
static struct known {
    dev_t dev;
    ino_t ino;
    uint32_t id; /* 0: not the card */
} g_known[KNOWN];
static unsigned g_known_next;

static void lock_conn(void) { pthread_mutex_lock(&g_conn_lock); }
static void unlock_conn(void) { pthread_mutex_unlock(&g_conn_lock); }

__attribute__((constructor)) static void fork_safety(void)
{
    /* Nobody may hold the connection across fork: the child would inherit it locked. */
    pthread_atfork(lock_conn, unlock_conn, unlock_conn);
}

static void disconnect_locked(void)
{
    if (g_conn >= 0)
        close(g_conn);
    g_conn = -1;
    memset(g_known, 0, sizeof g_known); /* a new service has new files */
}

static int connect_locked(void)
{
    if (g_conn >= 0 && g_conn_pid == getpid())
        return g_conn;
    if (g_conn >= 0) { /* the parent's, inherited: never speak for it */
        close(g_conn);
        g_conn = -1;
    }
    const char *dir = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (!dir || snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", dir, CHAM_CARD_SOCKET_NAME) >=
                    (int)sizeof addr.sun_path)
        return -1;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s >= 0 && connect(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(s);
        s = -1;
    }
    /* The service copies to and from our memory as the kernel would; let it
     * where Yama lets only ancestors do that (ptrace_scope 1). */
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (s >= 0 && getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
        prctl(PR_SET_PTRACER, (unsigned long)cred.pid, 0, 0, 0);
    g_conn = s;
    g_conn_pid = getpid();
    return s;
}

/* Sends a request (with fd, or followed by a bo's AHardwareBuffer) and answers
 * the service's questions until its CARD_DONE; returns that result, and its
 * fd in *fd_out. -ENODEV when there is no service. */
static int64_t call_locked(struct cham_msg req, int fd, const struct cham_bo *bo, int *fd_out)
{
    if (fd_out)
        *fd_out = -1;
    int s = connect_locked();
    if (s < 0 || cham_send(s, &req, fd) != 0 || (bo && bo_send(bo, s) != 0)) {
        disconnect_locked();
        return -ENODEV;
    }
    for (;;) {
        struct cham_msg m;
        int got = -1;
        if (cham_recv(s, &m, &got) != 1) {
            disconnect_locked();
            return -ENODEV;
        }
        if (m.type == CARD_DONE) {
            if (fd_out)
                *fd_out = got;
            else if (got >= 0)
                close(got);
            return (int64_t)m.a;
        }
        struct cham_msg reply = {CARD_FD, 0, 0, 0};
        int send_fd = -1;
        if (m.type == CARD_GET_FD) { /* fget: a copy of our fd */
            send_fd = (int)(int64_t)m.a;
            if (send_fd < 0 || fcntl(send_fd, F_GETFD) < 0)
                reply.a = (uint64_t)(int64_t)-EBADF, send_fd = -1;
        } else if (m.type == CARD_PUT_FD) { /* fd_install: it arrived close-on-exec */
            if (got >= 0 && !m.a)
                fcntl(got, F_SETFD, 0);
            reply.a = got >= 0 ? (uint64_t)got : (uint64_t)(int64_t)-EMFILE;
            got = -1; /* the program's now */
        }
        if (got >= 0)
            close(got);
        if (cham_send(s, &reply, send_fd) != 0) {
            disconnect_locked();
            return -ENODEV;
        }
    }
}

static void remember_locked(const struct stat *st, uint32_t id)
{
    g_known[g_known_next++ % KNOWN] = (struct known){st->st_dev, st->st_ino, id};
}

/* The card file `fd` is (its id), or 0. Only a pipe can be one. */
static uint32_t card_file(int fd)
{
    struct stat st;
    if (fd < 0 || real_fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode))
        return 0;
    lock_conn();
    for (int i = 0; i < KNOWN; i++) {
        if (g_known[i].ino == st.st_ino && g_known[i].dev == st.st_dev) { /* st_ino is never 0 */
            uint32_t id = g_known[i].id;
            unlock_conn();
            return id;
        }
    }
    /* A copy we haven't seen here (inherited, passed): the service knows. */
    struct cham_msg req = {CARD_ATTACH, 0, 0, 0};
    int64_t ret = call_locked(req, fd, NULL, NULL);
    uint32_t id = ret > 0 ? (uint32_t)ret : 0;
    if (ret != -ENODEV)
        remember_locked(&st, id);
    unlock_conn();
    return id;
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
    struct cham_msg req = {CARD_OPEN, 0, 0, 0};
    int fd;
    struct stat st;
    lock_conn();
    int64_t ret = call_locked(req, -1, NULL, &fd);
    if (ret > 0 && fd >= 0 && real_fstat(fd, &st) == 0)
        remember_locked(&st, (uint32_t)ret);
    unlock_conn();
    if (ret <= 0 || fd < 0) {
        if (fd >= 0)
            close(fd);
        cham_log("cannot open the card: %s", ret == -ENODEV ? "chameleon-kernel isn't running" : strerror((int)-ret));
        errno = ret < 0 ? (int)-ret : ENODEV;
        return -1;
    }
    return fd;
}

/* ---- our gbm's buffers (cham_shim.h) ---- */

uint32_t cham_bo_handle_open(struct cham_bo *bo, int fd)
{
    uint32_t id = card_file(fd);
    if (!id)
        return 0;
    struct cham_msg req = {CARD_IMPORT, id, bo->format, 0};
    lock_conn();
    int64_t ret = call_locked(req, -1, bo, NULL);
    unlock_conn();
    return ret > 0 ? (uint32_t)ret : 0;
}

int ioctl(int fd, int request, ...); /* ours, below */

/* ---- a commit's fence travels with it ----
 * An atomic commit's IN_FENCE_FD is an fd of ours the service would have to
 * ask for (the kernel's fget), a second round trip on every commit. So the
 * one fence a commit names goes along with the request instead. Which
 * property is IN_FENCE_FD is learnt the standard way, by its name, once per
 * id. The commit is read with the same checks the service applies: a bad
 * pointer only means nothing is sent ahead, and the service asks as before. */

static pthread_mutex_t g_prop_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint32_t id;
    int fence;
} g_props[64];
static int g_nprops;

static int is_fence_prop(int fd, uint32_t id)
{
    pthread_mutex_lock(&g_prop_lock);
    for (int i = 0; i < g_nprops; i++) {
        if (g_props[i].id == id) {
            int fence = g_props[i].fence;
            pthread_mutex_unlock(&g_prop_lock);
            return fence;
        }
    }
    pthread_mutex_unlock(&g_prop_lock);
    struct drm_mode_get_property p = {.prop_id = id};
    int fence = ioctl(fd, (int)DRM_IOCTL_MODE_GETPROPERTY, &p) == 0 && strcmp(p.name, "IN_FENCE_FD") == 0;
    pthread_mutex_lock(&g_prop_lock);
    if (g_nprops < (int)(sizeof g_props / sizeof g_props[0]))
        g_props[g_nprops].id = id, g_props[g_nprops++].fence = fence;
    pthread_mutex_unlock(&g_prop_lock);
    return fence;
}

/* Our own memory, read without trusting the pointer (the service would
 * answer EFAULT for a bad one, so must we not crash on it). */
static int read_own(void *to, uint64_t from, size_t n)
{
    struct iovec local = {to, n}, remote = {(void *)(uintptr_t)from, n};
    return n == 0 || process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n ? 0 : -1;
}

/* The fd a commit gives as IN_FENCE_FD, or -1: none, more than one, or
 * anything unreadable (the service then asks, as for any other fd). */
static int commit_fence(int fd, const void *arg)
{
    struct drm_mode_atomic a;
    uint32_t counts[16], props[128];
    uint64_t values[128];
    if (read_own(&a, (uint64_t)(uintptr_t)arg, sizeof a) || a.count_objs > 16 ||
        read_own(counts, a.count_props_ptr, a.count_objs * sizeof counts[0]))
        return -1;
    uint32_t total = 0;
    for (uint32_t i = 0; i < a.count_objs; i++)
        total += counts[i] < 128 ? counts[i] : 128;
    if (total > 128 || read_own(props, a.props_ptr, total * sizeof props[0]) ||
        read_own(values, a.prop_values_ptr, total * sizeof values[0]))
        return -1;
    int found = -1;
    for (uint32_t k = 0; k < total; k++) {
        if ((int64_t)values[k] < 0 || values[k] > INT32_MAX || !is_fence_prop(fd, props[k]))
            continue;
        if (found >= 0)
            return -1;
        found = (int)values[k];
    }
    return found >= 0 && fcntl(found, F_GETFD) >= 0 ? found : -1;
}

void cham_bo_handle_close(int fd, uint32_t handle)
{
    struct drm_gem_close c = {.handle = handle};
    ioctl(fd, (int)DRM_IOCTL_GEM_CLOSE, &c);
}

/* No app link in this process: buffers here are never shown from here. */
void link_forget_bo_locked(struct cham_bo *bo)
{
    (void)bo;
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
    if (ret == 0 && S_ISFIFO(st->st_mode) && card_file(fd))
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

    /* DRM ioctls go to the card; every other ioctl passes untouched, at no
     * cost: the GPU driver's (many per frame) and FIONREAD, which Qt and
     * D-Bus call on every socket read. On the card the pipe answers those
     * (TCGETS: ENOTTY as a DRM device; FIONREAD: a byte count where a DRM
     * device says ENOTTY, which no program asks a card). */
    if (_IOC_TYPE((unsigned int)request) == DRM_IOCTL_BASE) {
        uint32_t id = card_file(fd);
        struct drm_prime_handle prime;
        if (id && (unsigned int)request == DRM_IOCTL_PRIME_FD_TO_HANDLE && read_own(&prime, (uint64_t)(uintptr_t)arg,
                                                                                     sizeof prime) == 0) {
            /* A dmabuf of a buffer this process allocated, as the kernel finds
             * its own: the service gets the buffer itself (it can't rebuild one
             * from a dmabuf), and gives the same handle each time. */
            struct cham_bo *bo = cham_bo_from_fd(prime.fd);
            uint32_t handle = bo ? cham_bo_handle_open(bo, fd) : 0;
            if (handle) {
                ((struct drm_prime_handle *)arg)->handle = handle;
                return 0;
            }
        }
        if (id) {
            struct cham_msg req = {CARD_IOCTL, id, (unsigned int)request, (uint64_t)(uintptr_t)arg};
            int fence = _IOC_NR((unsigned int)request) == _IOC_NR(DRM_IOCTL_MODE_ATOMIC) ? commit_fence(fd, arg) : -1;
            if (fence >= 0)
                req.a |= (uint64_t)(fence + 1) << 32; /* which of our fds it is */
            lock_conn();
            int64_t ret = call_locked(req, fence, NULL, NULL);
            unlock_conn();
            if (ret < 0) {
                errno = (int)-ret;
                return -1;
            }
            return (int)ret;
        }
    }
    resolve_ioctl();
    return REAL(ioctl)(fd, request, arg);
}

/* ---- KWin's Wayland socket ---- */

/* For clients.c here and chameleon-kernel's input thread (input.c), which
 * connects to it for the clipboard and the keyboard layout. */
static void note_socket(const char *path)
{
    input_note_socket(path);
    struct cham_msg req = {CARD_WAYLAND, 0, 0, 0};
    lock_conn();
    int s = connect_locked();
    if (s < 0 || cham_send(s, &req, -1) != 0 || send(s, path, strlen(path), MSG_NOSIGNAL) < 0)
        disconnect_locked();
    unlock_conn();
}

/* libwayland-server binds "$XDG_RUNTIME_DIR/wayland-N". */
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
            note_socket(path);
    }
    return ret;
}

/* ---- environment ----
 * The launcher saves the user's LD_PRELOAD / LD_LIBRARY_PATH. The dynamic
 * linker has already read ours, so restore theirs: Xwayland and every app
 * KWin starts must get Termux's normal Mesa, not this shim. */
__attribute__((constructor)) static void restore_environment(void)
{
    const char *socket = getenv("CHAMELEON_WAYLAND_SOCKET"); /* bin/kwin_wayland: KWin won't bind() it */
    if (socket && *socket == '/')
        note_socket(socket);
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
