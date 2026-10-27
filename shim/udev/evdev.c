/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The /dev/input/eventN nodes of Chameleon's input devices (udev.c lists
 * them), behaving like the kernel's evdev for each open file:
 *  - a queue of the device's events, as many as evdev keeps for it; when it
 *    overflows, the oldest events give way to SYN_DROPPED. read() returns
 *    whole frames only, from the queue;
 *  - the device's state (keys, axes, multitouch slots) for the state ioctls,
 *    which, as evdev does, also remove the queued events of their type;
 *  - per-file timestamps' clock, EVIOCGRAB (through the server, since it
 *    involves every reader), EVIOCREVOKE.
 * The events come from chameleon-kernel over a socket (one per open
 * file; common/chameleon_evdev.h), read by one helper thread per process.
 * The program's fd is a pipe's read end that holds a byte while the queue
 * holds a whole frame, so poll()/epoll work on it natively; read(), close(),
 * dup()/fcntl(F_DUPFD) and the ioctls on it are answered here.
 * Known differences: a revoked or removed node polls POLLHUP without
 * POLLERR (no pipe or socket can show both), and an fd that crosses fork()
 * or exec() isn't served there (libinput opens its devices close-on-exec
 * and doesn't fork with them).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
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
        *(void **)&next_##name = cham_next(#name)

/* Every libc function the library overrides (evdev.c, sysfs.c), looked up
 * once at load (internal.h). */
static const char *const k_next_names[] = {
    "open", "openat", "__open_2", "__openat_2", "read", "__read_chk", "close", "dup", "dup2", "dup3",
    "fcntl", "ioctl", "stat", "lstat", "fstat", "fstatat", "fopen", "opendir", "access", "faccessat",
    "readlink", "readlinkat", "__readlink_chk", "__readlinkat_chk", "realpath",
};
static void *g_next[sizeof k_next_names / sizeof k_next_names[0]];

__attribute__((constructor)) static void resolve_next(void)
{
    for (size_t i = 0; i < sizeof k_next_names / sizeof k_next_names[0]; i++)
        g_next[i] = dlsym(RTLD_NEXT, k_next_names[i]);
}

void *cham_next(const char *name)
{
    for (size_t i = 0; i < sizeof k_next_names / sizeof k_next_names[0]; i++)
        if (g_next[i] && strcmp(k_next_names[i], name) == 0)
            return g_next[i];
    return dlsym(RTLD_NEXT, name); /* not looked up yet, or not in the list */
}

#define MAX_OPEN 64
#define MAX_FD 65536 /* fds beyond this aren't recognised */
#define IN_MAX 4096
#define DEVNODE_PREFIX "/dev/input/event"

/* One open file of a node (shared by its dup()s), as evdev's client. */
struct client {
    int used, refs;   /* refs: the program's fds for it */
    int index;        /* the device */
    int sock;         /* to the server; -1 once revoked or gone */
    int rfd, wfd;     /* the program's pipe end, and ours */
    dev_t dev;        /* rfd's identity */
    ino_t ino;
    struct cham_evdev_info info;
    struct cham_evdev_state state;
    struct input_event *buf; /* the queue: buf[tail..packet_head) is readable */
    unsigned size, head, tail, packet_head;
    int token;        /* a byte is in the pipe */
    int revoked, gone;
    clockid_t clk;
    int grab_pending;
    int32_t grab_result;
    size_t in_len;
    char in[IN_MAX];
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static struct client g_clients[MAX_OPEN];
static unsigned char g_fd[MAX_FD]; /* the program's fd -> client + 1 */
static int g_wake[2] = {-1, -1};   /* tells the helper thread its sockets changed */

static int real_fstat(int fd, struct stat *st)
{
    NEXT(fstat);
    return next_fstat(fd, st);
}

static ssize_t real_read(int fd, void *buf, size_t count)
{
    NEXT(read);
    return next_read(fd, buf, count);
}

static int real_close(int fd)
{
    NEXT(close);
    return next_close(fd);
}

/* N if path is the node of one of the server's devices, else -1. */
static int index_of_node(const char *path)
{
    struct cham_evdev_info list[CHAM_EVDEV_MAX_DEVICES];
    int i = cham_evdev_index(path, DEVNODE_PREFIX);
    return i >= 0 && i < cham_evdev_list(list, CHAM_EVDEV_MAX_DEVICES) ? i : -1;
}

/* The client behind the program's fd, or NULL. */
static struct client *client_of(int fd)
{
    if (fd < 0 || fd >= MAX_FD || !g_fd[fd])
        return NULL;
    struct client *c = &g_clients[g_fd[fd] - 1];
    struct stat st;
    if (real_fstat(fd, &st) != 0 || st.st_ino != c->ino || st.st_dev != c->dev || !c->used) {
        g_fd[fd] = 0; /* closed behind our back */
        return NULL;
    }
    return c;
}

/* The device index behind fd if it is one of our nodes, else -1. */
static int index_of_fd(int fd, const struct stat *st)
{
    (void)st;
    struct client *c = client_of(fd);
    return c ? c->index : -1;
}

/* ---- the queue (under g_lock), as evdev keeps it ---- */

/* The pipe holds a byte exactly while the queue holds a whole frame or the
 * node can't be read any more (revoked, removed: the pipe is closed then). */
static void update_token(struct client *c)
{
    int want = c->packet_head != c->tail && !c->revoked && !c->gone;
    char b = 0;
    if (want && !c->token && c->wfd >= 0)
        c->token = write(c->wfd, "", 1) == 1;
    else if (!want && c->token)
        c->token = real_read(c->rfd, &b, 1) != 1;
}

static void stamp(struct client *c, struct input_event *e)
{
    if (c->clk == CLOCK_MONOTONIC)
        return;
    struct timespec mono, other;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(c->clk, &other);
    int64_t t = (int64_t)e->input_event_sec * 1000000000 + (int64_t)e->input_event_usec * 1000 +
                ((int64_t)other.tv_sec - mono.tv_sec) * 1000000000 + (other.tv_nsec - mono.tv_nsec);
    e->input_event_sec = t / 1000000000;
    e->input_event_usec = t % 1000000000 / 1000;
}

/* An event into the queue; when it's full, the oldest events give way to a
 * SYN_DROPPED and the newest event. */
static void pass_event(struct client *c, const struct input_event *e)
{
    unsigned mask = c->size - 1;
    c->buf[c->head++] = *e;
    c->head &= mask;
    if (c->head == c->tail) {
        c->tail = (c->head - 2) & mask;
        c->buf[c->tail] = (struct input_event){.input_event_sec = e->input_event_sec,
                                               .input_event_usec = e->input_event_usec,
                                               .type = EV_SYN,
                                               .code = SYN_DROPPED};
        c->packet_head = c->tail;
    }
    if (e->type == EV_SYN && e->code == SYN_REPORT)
        c->packet_head = c->head;
}

/* A SYN_DROPPED after what's queued (readable with the next frame). */
static void queue_dropped(struct client *c)
{
    struct timespec ts;
    clock_gettime(c->clk, &ts);
    unsigned mask = c->size - 1;
    c->buf[c->head++] = (struct input_event){.input_event_sec = ts.tv_sec,
                                             .input_event_usec = ts.tv_nsec / 1000,
                                             .type = EV_SYN,
                                             .code = SYN_DROPPED};
    c->head &= mask;
    if (c->head == c->tail) {
        c->tail = (c->head - 1) & mask;
        c->packet_head = c->tail;
    }
}

/* Removes the queued events of `type`, and frames left empty by that (but a
 * SYN_REPORT at the very start stays): what evdev does on a state query. */
static void flush_type(struct client *c, uint16_t type)
{
    unsigned mask = c->size - 1, head = c->tail, kept = 1;
    c->packet_head = c->tail;
    for (unsigned i = c->tail; i != c->head; i = (i + 1) & mask) {
        struct input_event e = c->buf[i];
        int report = e.type == EV_SYN && e.code == SYN_REPORT;
        if (e.type == type || (report && !kept))
            continue;
        c->buf[head] = e;
        head = (head + 1) & mask;
        kept++;
        if (report) {
            kept = 0;
            c->packet_head = head;
        }
    }
    c->head = head;
}

/* The server's socket closed: the device is gone (or chameleon-kernel is). */
static void gone(struct client *c)
{
    c->gone = 1;
    if (c->sock >= 0)
        real_close(c->sock);
    c->sock = -1;
    update_token(c);
    if (c->wfd >= 0)
        real_close(c->wfd); /* the program's fd polls POLLHUP */
    c->wfd = -1;
    pthread_cond_broadcast(&g_cond);
}

static void handle_record(struct client *c, const struct cham_evdev_record *h, const char *p)
{
    switch (h->type) {
    case CHAM_EVDEV_STATE: /* again: events were lost on the way */
        if (h->size != sizeof c->state)
            break;
        memcpy(&c->state, p, sizeof c->state);
        queue_dropped(c);
        break;
    case CHAM_EVDEV_FRAME:
    case CHAM_EVDEV_FRAME_GRABBED:
        for (size_t i = 0; i + sizeof(struct input_event) <= h->size; i += sizeof(struct input_event)) {
            struct input_event e;
            memcpy(&e, p + i, sizeof e);
            cham_evdev_state_apply(&c->state, &e);
            if (h->type == CHAM_EVDEV_FRAME) {
                stamp(c, &e);
                pass_event(c, &e);
            }
        }
        break;
    case CHAM_EVDEV_GRAB_RESULT:
        if (h->size == sizeof c->grab_result) {
            memcpy(&c->grab_result, p, sizeof c->grab_result);
            c->grab_pending = 0;
        }
        break;
    case CHAM_EVDEV_ABSINFO: {
        uint32_t axis;
        if (h->size != sizeof axis + sizeof(struct input_absinfo))
            break;
        memcpy(&axis, p, sizeof axis);
        if (axis < ABS_CNT)
            memcpy(&c->info.absinfo[axis], p + sizeof axis, sizeof(struct input_absinfo));
        break;
    }
    }
}

/* Takes what the server sent. */
static void receive(struct client *c)
{
    for (;;) {
        ssize_t n = recv(c->sock, c->in + c->in_len, sizeof c->in - c->in_len, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            gone(c);
            return;
        }
        if (n < 0)
            break;
        c->in_len += (size_t)n;
        size_t used = 0;
        for (;;) {
            const struct cham_evdev_record *h = (const void *)(c->in + used);
            if (c->in_len - used < sizeof *h || c->in_len - used < sizeof *h + h->size)
                break;
            handle_record(c, h, c->in + used + sizeof *h);
            used += sizeof *h + h->size;
        }
        c->in_len -= used;
        memmove(c->in, c->in + used, c->in_len);
        if (c->in_len == sizeof c->in) { /* not one of ours */
            gone(c);
            return;
        }
    }
    update_token(c);
    pthread_cond_broadcast(&g_cond);
}

static void *helper_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct pollfd p[1 + MAX_OPEN] = {{g_wake[0], POLLIN, 0}};
        int which[1 + MAX_OPEN], n = 1;
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_OPEN; i++) {
            if (g_clients[i].used && g_clients[i].sock >= 0) {
                which[n] = i;
                p[n++] = (struct pollfd){g_clients[i].sock, POLLIN, 0};
            }
        }
        pthread_mutex_unlock(&g_lock);
        if (poll(p, (nfds_t)n, -1) < 0 && errno != EINTR)
            continue;
        char b[64];
        if (p[0].revents & POLLIN)
            while (real_read(g_wake[0], b, sizeof b) == (ssize_t)sizeof b)
                ;
        pthread_mutex_lock(&g_lock);
        for (int k = 1; k < n; k++) {
            struct client *c = &g_clients[which[k]];
            if (p[k].revents && c->used && c->sock == p[k].fd)
                receive(c);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

/* fork() copies g_lock as it is, but not the thread holding it: a child
 * that then closes or dup2()s one of the inherited fds (as a process spawn
 * does before exec) would wait forever. So fork happens with g_lock taken. */
static void fork_lock(void)
{
    pthread_mutex_lock(&g_lock);
}

static void fork_unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}

static void start_helper(void)
{
    pthread_atfork(fork_lock, fork_unlock, fork_unlock);
    if (pipe2(g_wake, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    pthread_t t;
    if (pthread_create(&t, NULL, helper_main, NULL) == 0)
        pthread_detach(t);
}

static void wake_helper(void)
{
    if (g_wake[1] >= 0 && write(g_wake[1], "", 1) < 0) {
        /* full: it's awake anyway */
    }
}

/* Releases the client once the program has closed its last fd for it. */
static void release(struct client *c)
{
    if (--c->refs > 0)
        return;
    if (c->sock >= 0)
        real_close(c->sock); /* the server sees it go (and drops its grab) */
    if (c->wfd >= 0)
        real_close(c->wfd);
    free(c->buf);
    *c = (struct client){0};
    wake_helper();
}

/* ---- opening a node ---- */

static int recv_all(int fd, void *buf, size_t len)
{
    struct pollfd p = {fd, POLLIN, 0};
    while (len) {
        if (poll(&p, 1, 2000) != 1)
            return -1;
        ssize_t n = recv(fd, buf, len, 0);
        if (n <= 0)
            return -1;
        buf = (char *)buf + n;
        len -= (size_t)n;
    }
    return 0;
}

static int open_node(int index, int flags)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, start_helper);
    struct cham_evdev_info info;
    struct cham_evdev_record h;
    struct cham_evdev_state state;
    int p[2] = {-1, -1}, s = cham_evdev_open(index, &info);
    struct stat st;
    /* The server starts with the device's state. */
    if (s < 0 || recv_all(s, &h, sizeof h) != 0 || h.type != CHAM_EVDEV_STATE || h.size != sizeof state ||
        recv_all(s, &state, sizeof state) != 0 || pipe2(p, O_CLOEXEC) != 0 || real_fstat(p[0], &st) != 0 ||
        p[0] >= MAX_FD) {
        if (s >= 0)
            real_close(s);
        if (p[0] >= 0) {
            real_close(p[0]);
            real_close(p[1]);
        }
        errno = ENODEV;
        return -1;
    }
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    if (flags & O_NONBLOCK)
        fcntl(p[0], F_SETFL, O_NONBLOCK);
    if (!(flags & O_CLOEXEC))
        fcntl(p[0], F_SETFD, 0);
    pthread_mutex_lock(&g_lock);
    struct client *c = NULL;
    for (int i = 0; i < MAX_OPEN && !c; i++)
        if (!g_clients[i].used)
            c = &g_clients[i];
    unsigned size = cham_evdev_queue_size(&info);
    struct input_event *buf = c ? calloc(size, sizeof *buf) : NULL;
    if (!buf) {
        pthread_mutex_unlock(&g_lock);
        real_close(s);
        real_close(p[0]);
        real_close(p[1]);
        errno = c ? ENOMEM : EMFILE;
        return -1;
    }
    *c = (struct client){.used = 1, .refs = 1, .index = index, .sock = s, .rfd = p[0], .wfd = p[1],
                         .dev = st.st_dev, .ino = st.st_ino, .info = info, .state = state, .buf = buf,
                         .size = size, .clk = CLOCK_MONOTONIC};
    g_fd[p[0]] = (unsigned char)(c - g_clients + 1);
    pthread_mutex_unlock(&g_lock);
    wake_helper();
    return p[0];
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
    char buf[PATH_MAX];
    return next_open(cham_sys_path(path, buf, sizeof buf), flags, mode);
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
    char buf[PATH_MAX];
    return next_openat(dirfd, cham_sys_path(path, buf, sizeof buf), flags, mode);
}

EXPORT int openat64(int dirfd, const char *path, int flags, ...) __attribute__((alias("openat")));

EXPORT int __open_2(const char *path, int flags)
{
    int i = index_of_node(path);
    if (i >= 0)
        return open_node(i, flags);
    NEXT(__open_2);
    char buf[PATH_MAX];
    return next___open_2(cham_sys_path(path, buf, sizeof buf), flags);
}

EXPORT int __openat_2(int dirfd, const char *path, int flags)
{
    int i = index_of_node(path);
    if (i >= 0)
        return open_node(i, flags);
    NEXT(__openat_2);
    char buf[PATH_MAX];
    return next___openat_2(dirfd, cham_sys_path(path, buf, sizeof buf), flags);
}

/* ---- stat ---- */

EXPORT int stat(const char *path, struct stat *st)
{
    int i = index_of_node(path);
    if (i >= 0) {
        node_stat(i, st);
        return 0;
    }
    if (cham_sys_node_stat(path, st))
        return 0;
    NEXT(stat);
    char buf[PATH_MAX];
    return next_stat(cham_sys_path(path, buf, sizeof buf), st);
}

EXPORT int lstat(const char *path, struct stat *st)
{
    int i = index_of_node(path);
    if (i >= 0) {
        node_stat(i, st);
        return 0;
    }
    if (cham_sys_node_stat(path, st))
        return 0;
    NEXT(lstat);
    char buf[PATH_MAX];
    return next_lstat(cham_sys_path(path, buf, sizeof buf), st);
}

EXPORT int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    int i = index_of_node(path);
    if (i >= 0) {
        node_stat(i, st);
        return 0;
    }
    if (cham_sys_node_stat(path, st))
        return 0;
    NEXT(fstatat);
    char buf[PATH_MAX];
    return next_fstatat(dirfd, cham_sys_path(path, buf, sizeof buf), st, flags);
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


/* ---- read(), close(), dup() on the program's fd ---- */

/* evdev's read(): whole events of whole frames, ENODEV once revoked or
 * removed, EAGAIN or a wait when nothing is queued. */
static ssize_t node_read(struct client *c, int fd, void *buf, size_t count)
{
    if (count && count < sizeof(struct input_event)) {
        errno = EINVAL;
        return -1;
    }
    size_t done = 0;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        if (c->revoked || c->gone) {
            pthread_mutex_unlock(&g_lock);
            errno = ENODEV;
            return -1;
        }
        int nonblock = fcntl(fd, F_GETFL) & O_NONBLOCK;
        if (c->packet_head == c->tail && nonblock) {
            pthread_mutex_unlock(&g_lock);
            errno = EAGAIN;
            return -1;
        }
        if (!count)
            break;
        while (done + sizeof(struct input_event) <= count && c->packet_head != c->tail) {
            memcpy((char *)buf + done, &c->buf[c->tail], sizeof(struct input_event));
            c->tail = (c->tail + 1) & (c->size - 1);
            done += sizeof(struct input_event);
        }
        if (done)
            break;
        pthread_cond_wait(&g_cond, &g_lock);
    }
    update_token(c);
    pthread_mutex_unlock(&g_lock);
    return (ssize_t)done;
}

EXPORT ssize_t read(int fd, void *buf, size_t count)
{
    struct client *c = client_of(fd);
    return c ? node_read(c, fd, buf, count) : real_read(fd, buf, count);
}

EXPORT ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size)
{
    struct client *c = count <= buf_size ? client_of(fd) : NULL;
    if (c)
        return node_read(c, fd, buf, count);
    NEXT(__read_chk);
    return next___read_chk(fd, buf, count, buf_size);
}

EXPORT int close(int fd)
{
    struct client *c = client_of(fd);
    int ret = real_close(fd);
    if (c && ret == 0) {
        pthread_mutex_lock(&g_lock);
        g_fd[fd] = 0;
        release(c);
        pthread_mutex_unlock(&g_lock);
    }
    return ret;
}

/* newfd now refers to the same open file as oldfd (dup2/dup3 closed it first). */
static void dup_done(struct client *old, struct client *replaced, int newfd)
{
    pthread_mutex_lock(&g_lock);
    if (replaced) {
        g_fd[newfd] = 0;
        release(replaced);
    }
    if (old && old->used && newfd >= 0 && newfd < MAX_FD) {
        old->refs++;
        g_fd[newfd] = (unsigned char)(old - g_clients + 1);
    }
    pthread_mutex_unlock(&g_lock);
}

EXPORT int dup(int oldfd)
{
    struct client *c = client_of(oldfd);
    NEXT(dup);
    int fd = next_dup(oldfd);
    if (c && fd >= 0)
        dup_done(c, NULL, fd);
    return fd;
}

EXPORT int dup2(int oldfd, int newfd)
{
    struct client *c = client_of(oldfd), *replaced = oldfd != newfd ? client_of(newfd) : NULL;
    NEXT(dup2);
    int fd = next_dup2(oldfd, newfd);
    if (fd >= 0 && oldfd != newfd && (c || replaced))
        dup_done(c, replaced, fd);
    return fd;
}

EXPORT int dup3(int oldfd, int newfd, int flags)
{
    struct client *c = client_of(oldfd), *replaced = client_of(newfd);
    NEXT(dup3);
    int fd = next_dup3(oldfd, newfd, flags);
    if (fd >= 0 && (c || replaced))
        dup_done(c, replaced, fd);
    return fd;
}

EXPORT int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    static int (*next_fcntl)(int, int, ...);
    if (!next_fcntl)
        *(void **)&next_fcntl = cham_next("fcntl");
    int ret = next_fcntl(fd, cmd, arg);
    if (ret >= 0 && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) && fd >= 0 && fd < MAX_FD && g_fd[fd]) {
        struct client *c = client_of(fd);
        if (c)
            dup_done(c, NULL, ret);
    }
    return ret;
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

/* A state ioctl: the state now, and the queued events of its type go. */
static int state_out(struct client *c, void *arg, unsigned size, const uint8_t *bits, unsigned bits_len,
                     unsigned max, uint16_t type)
{
    flush_type(c, type);
    update_token(c);
    return bits_out(arg, size, bits, bits_len, max);
}

static int send_request(struct client *c, uint32_t type, const void *a, size_t a_len, const void *b, size_t b_len)
{
    struct cham_evdev_record h = {type, (uint32_t)(a_len + b_len)};
    char msg[sizeof h + sizeof(uint32_t) + sizeof(struct input_absinfo)];
    memcpy(msg, &h, sizeof h);
    memcpy(msg + sizeof h, a, a_len);
    if (b_len)
        memcpy(msg + sizeof h + a_len, b, b_len);
    size_t len = sizeof h + a_len + b_len;
    return send(c->sock, msg, len, MSG_NOSIGNAL) == (ssize_t)len ? 0 : -ENODEV;
}

/* EVIOCGRAB: the server decides, since it involves every reader. */
static int grab(struct client *c, int on)
{
    int32_t v = on;
    c->grab_pending = 1;
    if (send_request(c, CHAM_EVDEV_GRAB, &v, sizeof v, NULL, 0) != 0)
        return -ENODEV;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 2;
    while (c->grab_pending && !c->gone)
        if (pthread_cond_timedwait(&g_cond, &g_lock, &until) != 0)
            break;
    return c->grab_pending || c->gone ? -ENODEV : c->grab_result;
}

static int evdev_ioctl(struct client *c, unsigned req, void *arg)
{
    const struct cham_evdev_info *d = &c->info;
    unsigned nr = _IOC_NR(req), size = _IOC_SIZE(req);
    if (c->revoked || c->gone)
        return -ENODEV;
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
            if (!cham_bit_test(d->ev, EV_ABS))
                return -EINVAL;
            struct input_absinfo ai = d->absinfo[nr - 0x40];
            ai.value = c->state.abs[nr - 0x40];
            memcpy(arg, &ai, size < sizeof ai ? size : sizeof ai);
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
        case _IOC_NR(EVIOCGMTSLOTS(0)): {
            uint32_t code = size >= sizeof code ? *(uint32_t *)arg : 0;
            if (size < sizeof code || !cham_bit_test(d->abs, ABS_MT_SLOT) || !cham_is_mt_value(code))
                return -EINVAL;
            int32_t *values = (int32_t *)arg + 1;
            unsigned slots = (unsigned)d->absinfo[ABS_MT_SLOT].maximum + 1, n = (size - sizeof code) / sizeof *values;
            for (unsigned i = 0; i < n && i < slots && i < CHAM_EVDEV_SLOTS; i++)
                values[i] = c->state.slot[i][code - ABS_MT_TOUCH_MAJOR];
            return 0;
        }
        case _IOC_NR(EVIOCGKEY(0)):
            return state_out(c, arg, size, c->state.key, sizeof c->state.key, KEY_MAX, EV_KEY);
        case _IOC_NR(EVIOCGLED(0)):
            return state_out(c, arg, size, NULL, 0, LED_MAX, EV_LED);
        case _IOC_NR(EVIOCGSND(0)):
            return state_out(c, arg, size, NULL, 0, SND_MAX, EV_SND);
        case _IOC_NR(EVIOCGSW(0)):
            return state_out(c, arg, size, NULL, 0, SW_MAX, EV_SW);
        case _IOC_NR(EVIOCGREP):
            return -ENOSYS; /* no key repeat (EV_REP) */
        case _IOC_NR(EVIOCGEFFECTS):
            *(int *)arg = 0;
            return 0;
        }
        return -EINVAL;
    }
    if (_IOC_DIR(req) == _IOC_WRITE) {
        if (nr >= 0xc0 && nr < 0xc0 + ABS_CNT) { /* EVIOCSABS: for every reader */
            uint32_t axis = nr - 0xc0;
            if (!cham_bit_test(d->ev, EV_ABS) || axis == ABS_MT_SLOT)
                return -EINVAL;
            struct input_absinfo ai = {0};
            memcpy(&ai, arg, size < sizeof ai ? size : sizeof ai);
            c->info.absinfo[axis] = ai;
            c->state.abs[axis] = ai.value;
            return send_request(c, CHAM_EVDEV_SET_ABS, &axis, sizeof axis, &ai, sizeof ai);
        }
        switch (nr) {
        case _IOC_NR(EVIOCGRAB):
            return grab(c, arg != NULL);
        case _IOC_NR(EVIOCREVOKE):
            if (arg)
                return -EINVAL;
            c->revoked = 1;
            if (c->sock >= 0)
                real_close(c->sock); /* the server drops its grab, if it had one */
            c->sock = -1;
            update_token(c);
            if (c->wfd >= 0)
                real_close(c->wfd);
            c->wfd = -1;
            wake_helper();
            pthread_cond_broadcast(&g_cond);
            return 0;
        case _IOC_NR(EVIOCSCLOCKID): {
            int clk = *(const int *)arg;
            if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC && clk != CLOCK_BOOTTIME)
                return -EINVAL;
            if (clk != c->clk) { /* queued events have the old clock's times: dropped */
                c->clk = clk;
                if (c->head != c->tail) {
                    c->packet_head = c->head = c->tail;
                    queue_dropped(c);
                }
                update_token(c);
            }
            return 0;
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

    unsigned req = (unsigned)request;
    struct client *c = _IOC_TYPE(req) == 'E' ? client_of(fd) : NULL;
    if (c) {
        pthread_mutex_lock(&g_lock);
        int ret = evdev_ioctl(c, req, arg);
        pthread_mutex_unlock(&g_lock);
        if (ret < 0) {
            errno = -ret;
            return -1;
        }
        return ret;
    }
    static int (*next_ioctl)(int, ioctl_request, ...); /* bionic's ioctl is overloaded: no __typeof__ */
    if (!next_ioctl)
        *(void **)&next_ioctl = cham_next("ioctl");
    return next_ioctl(fd, request, arg);
}
