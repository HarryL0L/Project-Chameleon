/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The card's open files, served to every process of the session by
 * chameleon-kernel, as the kernel serves /dev/dri/card0.
 *
 * libchameleon.so (interpose.c) answers open/fstat/ioctl on the card node
 * by asking here, over $XDG_RUNTIME_DIR/chameleon-card: one SOCK_SEQPACKET
 * connection per process, one request at a time (the protocol is in
 * internal.h). OPEN makes a file of the card's primary node or of its
 * render node with kms.c's per-file state, and a pipe:
 * the program gets the read end and reads DRM events from it, the service
 * keeps the write end. Once every copy of the read end is closed (in any
 * process, even a dead one) the write end reports POLLERR and the file is
 * released, as the kernel releases a file on its last close.
 *
 * An ioctl travels as the system call does: the file, the request number
 * and the argument's address. kms.c reads and writes the caller's memory
 * with process_vm_readv/writev, the kernel's copy_from_user/copy_to_user,
 * and asks the caller for an fd it names (IN_FENCE_FD, PRIME import) or to
 * take a new one (PRIME export), the kernel's fget and fd_install. A process
 * that made itself non-dumpable can't be read; its calls get EFAULT.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../common/cham_io.h"
#include "internal.h"

#define CALLBACK_TIMEOUT_MS 1000 /* the caller waits in its ioctl; only a stopped process is slower */

struct caller {
    int sock;
    pid_t pid;
    uid_t uid;
    int broken; /* a callback went unanswered: the connection is out of step */
    int sent_fd, sent_num; /* an fd that came with the request, and its number in the caller */
};
/* The caller of the call running on a thread. A pthread key, not __thread:
 * bionic refuses executables whose TLS segment is aligned below 64 bytes,
 * which an NDK build for API levels below 29 gives. */
static pthread_key_t g_caller_key;
static pthread_once_t g_caller_once = PTHREAD_ONCE_INIT;
static void caller_key(void) { pthread_key_create(&g_caller_key, NULL); }
static struct caller *current(void) { return pthread_getspecific(g_caller_key); }

static pthread_mutex_t g_card_lock = PTHREAD_MUTEX_INITIALIZER;
/* Held while closed files are released, so a request that checks for them
 * waits until a release another thread started is done (a closed master
 * must not still hold the master when the next request runs). */
static pthread_mutex_t g_reap_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fake_fd *g_card_files; /* open files (next), each holding one ref */
static uint32_t g_next_file_id;
static int g_wake[2] = {-1, -1};     /* a file was opened: the server thread watches it too */

/* ---- the caller ---- */

pid_t card_caller_pid(void)
{
    struct caller *c = current();
    return c ? c->pid : getpid();
}

uid_t card_caller_uid(void)
{
    struct caller *c = current();
    return c ? c->uid : geteuid();
}

int card_copy_from(void *to, uint64_t from, size_t n)
{
    struct iovec local = {to, n}, remote = {(void *)(uintptr_t)from, n};
    struct caller *c = current();
    return c && process_vm_readv(c->pid, &local, 1, &remote, 1, 0) == (ssize_t)n ? 0 : -EFAULT;
}

int card_copy_to(uint64_t to, const void *from, size_t n)
{
    struct iovec local = {(void *)from, n}, remote = {(void *)(uintptr_t)to, n};
    struct caller *c = current();
    return c && process_vm_writev(c->pid, &local, 1, &remote, 1, 0) == (ssize_t)n ? 0 : -EFAULT;
}

/* The caller's answer to a callback, CARD_FD; fd_out gets a passed fd. */
static int callback_answer(struct caller *c, struct cham_msg *m, int *fd_out)
{
    struct pollfd pfd = {c->sock, POLLIN, 0};
    if (poll(&pfd, 1, CALLBACK_TIMEOUT_MS) != 1 || cham_recv(c->sock, m, fd_out) != 1 || m->type != CARD_FD) {
        c->broken = 1;
        return -1;
    }
    return 0;
}

int card_fd_get(int n)
{
    struct caller *c = current();
    if (c && c->sent_fd >= 0 && n == c->sent_num)
        return fcntl(c->sent_fd, F_DUPFD_CLOEXEC, 0); /* came with the request */
    struct cham_msg m = {CARD_GET_FD, 0, (uint64_t)(int64_t)n, 0};
    int fd = -1;
    if (!c || c->broken || cham_send(c->sock, &m, -1) != 0 || callback_answer(c, &m, &fd) != 0 ||
        (int64_t)m.a < 0 || fd < 0) {
        if (fd >= 0)
            close(fd);
        return -EBADF;
    }
    return fd;
}

int card_fd_install(int fd, int cloexec)
{
    struct caller *c = current();
    struct cham_msg m = {CARD_PUT_FD, 0, (uint64_t)(cloexec != 0), 0};
    int unused = -1;
    if (!c || c->broken || cham_send(c->sock, &m, fd) != 0 || callback_answer(c, &m, &unused) != 0)
        return -EMFILE;
    if (unused >= 0)
        close(unused);
    return (int)(int64_t)m.a;
}

/* ---- files ---- */

static struct fake_fd *file_get(uint32_t id)
{
    pthread_mutex_lock(&g_card_lock);
    struct fake_fd *f = g_card_files;
    while (f && f->id != id)
        f = f->next;
    if (f)
        f->refs++;
    pthread_mutex_unlock(&g_card_lock);
    return f;
}

static void file_put(struct fake_fd *f)
{
    pthread_mutex_lock(&g_card_lock);
    int last = --f->refs == 0;
    pthread_mutex_unlock(&g_card_lock);
    if (!last)
        return;
    kms_file_close(f);
    close(f->event_wfd);
    free(f);
}

/* Releases the files whose read ends are all closed. Run before every
 * request too, so a program's close() comes before its next call here, as
 * with the kernel's last close. */
static void reap_closed(void)
{
    struct fake_fd *dead = NULL;
    pthread_mutex_lock(&g_reap_lock);
    pthread_mutex_lock(&g_card_lock);
    for (struct fake_fd **q = &g_card_files; *q;) {
        struct pollfd pfd = {(*q)->event_wfd, 0, 0};
        if (poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLERR)) {
            struct fake_fd *f = *q;
            *q = f->next;
            f->next = dead;
            dead = f;
        } else {
            q = &(*q)->next;
        }
    }
    pthread_mutex_unlock(&g_card_lock);
    while (dead) {
        struct fake_fd *f = dead;
        dead = f->next;
        file_put(f); /* now, or when the call in progress ends, as the kernel's last fput */
    }
    pthread_mutex_unlock(&g_reap_lock);
}

static int64_t file_open(struct caller *c, int render, int *read_end)
{
    kms_init_once(); /* the first open waits for the app, as the card's driver probing a panel */
    int p[2];
    struct stat st;
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0)
        return -errno;
    if (fcntl(p[0], F_SETPIPE_SZ, CHAM_CARD_PIPE_SIZE) != CHAM_CARD_PIPE_SIZE)
        cham_log("card: cannot make the event pipe one page: passed copies won't be recognised");
    struct fake_fd *f = calloc(1, sizeof *f);
    if (!f || fstat(p[0], &st) != 0) {
        free(f);
        close(p[0]);
        close(p[1]);
        return -ENOMEM;
    }
    f->dev = st.st_dev;
    f->ino = st.st_ino;
    f->event_wfd = p[1];
    f->pid = c->pid;
    f->render = render;
    f->refs = 1;
    kms_file_open(f);
    pthread_mutex_lock(&g_card_lock);
    f->id = ++g_next_file_id;
    f->next = g_card_files;
    g_card_files = f;
    pthread_mutex_unlock(&g_card_lock);
    if (write(g_wake[1], "", 1) < 0 && errno != EAGAIN)
        cham_log("card: cannot wake the server thread: %s", strerror(errno));

    char comm[32] = "?", path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)c->pid);
    FILE *fp = fopen(path, "re");
    if (fp) {
        if (fgets(comm, sizeof comm, fp))
            comm[strcspn(comm, "\n")] = 0;
        fclose(fp);
    }
    cham_log("%s (pid %d) opened the %s%s", comm, (int)c->pid, render ? "render node" : "card",
             f->master ? ", DRM master" : "");
    *read_end = p[0];
    return CARD_FILE(f->id, render);
}

/* Which open file `fd` (a copy of a read end) is. */
static int64_t file_attach(int fd)
{
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode))
        return -EBADF;
    int64_t ret = -EBADF;
    pthread_mutex_lock(&g_card_lock);
    for (struct fake_fd *f = g_card_files; f; f = f->next)
        if (f->dev == st.st_dev && f->ino == st.st_ino)
            ret = CARD_FILE(f->id, f->render);
    pthread_mutex_unlock(&g_card_lock);
    return ret;
}

/* ---- connections ---- */

static int answer(struct caller *c, int64_t ret, int fd)
{
    struct cham_msg m = {CARD_DONE, 0, (uint64_t)ret, 0};
    return cham_send(c->sock, &m, fd);
}

/* CARD_CLIENT_BUFFER's answer: the done, then the app's buffer. */
static int send_client_buffer(const AHardwareBuffer *b, void *arg)
{
    struct caller *c = arg;
    if (answer(c, 1, -1) != 0 || ahb_send(b, c->sock) != 0)
        c->broken = 1;
    return 0;
}

static void handle(struct caller *c, const struct cham_msg *m, int fd)
{
    switch (m->type) {
    case CARD_OPEN: {
        int read_end = -1;
        int64_t ret = file_open(c, m->a == 1, &read_end);
        answer(c, ret, read_end);
        if (read_end >= 0)
            close(read_end); /* the program's from now on */
        break;
    }
    case CARD_ATTACH:
        answer(c, file_attach(fd), -1);
        break;
    case CARD_IOCTL: {
        struct fake_fd *f = file_get(m->id);
        int ret = -EBADF;
        if (f) {
            c->sent_fd = fd; /* an fd of the caller's this call names (IN_FENCE_FD) */
            c->sent_num = (int)(m->a >> 32) - 1;
            ret = kms_ioctl(f, (unsigned int)m->a, (void *)(uintptr_t)m->b);
            c->sent_fd = -1;
            file_put(f);
        }
        answer(c, ret, -1);
        break;
    }
    case CARD_IMPORT: {
        AHardwareBuffer *b = ahb_recv(c->sock); /* read it whatever happens next */
        struct fake_fd *f = file_get(m->id);
        struct cham_bo *bo = f && b ? ahb_import(b, (uint32_t)m->a) : NULL;
        if (!bo && b && !f)
            ahb_release(b);
        uint32_t handle = bo ? kms_gem_import(f, bo) : 0;
        if (bo)
            cham_bo_unref(bo); /* the handle holds it */
        if (f)
            file_put(f);
        answer(c, handle ? (int64_t)handle : !f ? -EBADF : -EINVAL, -1);
        break;
    }
    case CARD_CLIENT_BUFFER:
        if (clients_with_buffer(fd, send_client_buffer, c) != 0)
            answer(c, -ENOENT, -1);
        break;
    case CARD_WAYLAND: {
        char path[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
        ssize_t n = recv(c->sock, path, sizeof path - 1, 0);
        path[n > 0 ? n : 0] = 0;
        if (path[0] == '/')
            input_note_socket(c->pid, path);
        break;
    }
    default:
        c->broken = 1;
    }
}

static void *conn_main(void *arg)
{
    struct caller c = {.sock = (int)(intptr_t)arg, .sent_fd = -1};
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(c.sock, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        c.pid = cred.pid;
        c.uid = cred.uid;
        pthread_setspecific(g_caller_key, &c);
        struct cham_msg m;
        int fd;
        while (!c.broken && cham_recv(c.sock, &m, &fd) == 1) {
            reap_closed();
            handle(&c, &m, fd);
            if (fd >= 0)
                close(fd);
        }
    }
    close(c.sock);
    return NULL;
}

/* Accepts connections and releases the files whose read ends are all closed. */
static void *server_main(void *arg)
{
    int l = (int)(intptr_t)arg;
    for (;;) {
        pthread_mutex_lock(&g_card_lock);
        int n = 2;
        for (struct fake_fd *f = g_card_files; f; f = f->next)
            n++;
        struct pollfd *p = calloc((size_t)n, sizeof *p);
        if (!p) {
            pthread_mutex_unlock(&g_card_lock);
            sleep(1);
            continue;
        }
        p[0] = (struct pollfd){l, POLLIN, 0};
        p[1] = (struct pollfd){g_wake[0], POLLIN, 0};
        int k = 2;
        for (struct fake_fd *f = g_card_files; f; f = f->next, k++)
            p[k] = (struct pollfd){f->event_wfd, 0, 0}; /* POLLERR: no reader left */
        pthread_mutex_unlock(&g_card_lock);

        if (poll(p, (nfds_t)n, -1) > 0) {
            if (p[1].revents & POLLIN) {
                char buf[64];
                while (read(g_wake[0], buf, sizeof buf) > 0) {
                }
            }
            if (p[0].revents & POLLIN) {
                int c = accept4(l, NULL, NULL, SOCK_CLOEXEC);
                pthread_t t;
                if (c >= 0 && pthread_create(&t, NULL, conn_main, (void *)(intptr_t)c) == 0)
                    pthread_detach(t);
                else if (c >= 0)
                    close(c);
            }
            for (k = 2; k < n; k++)
                if (p[k].revents & POLLERR)
                    k = n, reap_closed();
        }
        free(p);
    }
    return NULL;
}

int card_start(void)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (!dir || snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", dir, CHAM_CARD_SOCKET_NAME) >=
                    (int)sizeof addr.sun_path) {
        cham_log("card: XDG_RUNTIME_DIR is missing or too long");
        return -1;
    }
    pthread_once(&g_caller_once, caller_key);
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    unlink(addr.sun_path); /* left by a service that is gone */
    if (s < 0 || pipe2(g_wake, O_CLOEXEC | O_NONBLOCK) != 0 ||
        bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s, 16) != 0) {
        cham_log("card: cannot serve the card at %s: %s", addr.sun_path, strerror(errno));
        if (s >= 0)
            close(s);
        return -1;
    }
    chmod(addr.sun_path, 0600);
    pthread_t t;
    if (pthread_create(&t, NULL, server_main, (void *)(intptr_t)s) != 0)
        return -1;
    pthread_detach(t);
    return 0;
}
