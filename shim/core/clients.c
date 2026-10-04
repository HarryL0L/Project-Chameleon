/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Buffers of Wayland apps that render through the Chameleon EGL vendor.
 *
 * Such an app draws into AHardwareBuffers and hands them to KWin as
 * zwp_linux_dmabuf_v1 wl_buffers. KWin imports those with
 * eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT), but Android can't turn a bare
 * dmabuf fd back into an AHardwareBuffer. So before creating the wl_buffer
 * the app sends the AHardwareBuffer itself here, over
 * "<KWin's Wayland socket>.chameleon" (common/chameleon_proto.h), and the
 * EGL vendor's dmabuf import looks it up by the dmabuf's inode
 * (cham_client_ahb_from_fd).
 *
 * A thread waits until KWin's Wayland socket is known, then listens there.
 * A buffer stays registered until the app removes it or disconnects; KWin's
 * EGLImages hold their own references, so what is on screen stays valid.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../common/cham_io.h"
#include "internal.h"

#define MAX_CLIENTS 64

struct entry {
    int client;       /* connection fd it came from */
    uint32_t id;      /* the app's buffer id */
    dev_t dev;
    ino_t ino;
    AHardwareBuffer *ahb;
    struct entry *next;
};

static pthread_mutex_t g_entries_lock = PTHREAD_MUTEX_INITIALIZER;
static struct entry *g_entries;

CHAM_EXPORT AHardwareBuffer *cham_client_ahb_from_fd(int fd)
{
    struct stat st;
    if (fd < 0 || real_fstat(fd, &st) != 0)
        return NULL;
    AHardwareBuffer *found = NULL;
    pthread_mutex_lock(&g_entries_lock);
    for (struct entry *e = g_entries; e; e = e->next) {
        if (e->dev == st.st_dev && e->ino == st.st_ino) {
            found = e->ahb;
            break;
        }
    }
    pthread_mutex_unlock(&g_entries_lock);
    return found;
}

/* Drops `client`'s buffer `id`, or all of its buffers if all != 0. */
static void forget(int client, uint32_t id, int all)
{
    pthread_mutex_lock(&g_entries_lock);
    for (struct entry **p = &g_entries; *p;) {
        struct entry *e = *p;
        if (e->client == client && (all || e->id == id)) {
            *p = e->next;
            ahb_release(e->ahb);
            free(e);
        } else {
            p = &e->next;
        }
    }
    pthread_mutex_unlock(&g_entries_lock);
}

static void reply(int client, uint32_t id, int ok)
{
    struct cham_msg ack = {CHAM_CLIENT_BUFFER_ACK, id, (uint64_t)ok, 0};
    cham_send(client, &ack, -1);
}

/* Returns 0 when the connection should be closed. */
static int handle(int client)
{
    struct cham_msg m;
    int fd = -1;
    if (cham_recv(client, &m, &fd) <= 0)
        return 0;
    if (fd >= 0)
        close(fd);
    switch (m.type) {
    case CHAM_CLIENT_BUFFER_ADD: {
        AHardwareBuffer *b = ahb_recv(client);
        struct entry *e = b ? calloc(1, sizeof *e) : NULL;
        if (!e || ahb_dmabuf_identity(b, &e->dev, &e->ino) != 0) {
            cham_log("clients: could not take an app's buffer");
            free(e);
            ahb_release(b);
            reply(client, m.id, 0);
            return b != NULL; /* a missing packet means a confused peer */
        }
        forget(client, m.id, 0); /* an id is reused only after a remove */
        e->client = client;
        e->id = m.id;
        e->ahb = b;
        pthread_mutex_lock(&g_entries_lock);
        e->next = g_entries;
        g_entries = e;
        pthread_mutex_unlock(&g_entries_lock);
        reply(client, m.id, 1);
        return 1;
    }
    case CHAM_CLIENT_BUFFER_REMOVE:
        forget(client, m.id, 0);
        return 1;
    }
    return 1; /* unknown messages are ignored */
}

static int listen_at(const char *path)
{
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (s < 0 || strlen(path) >= sizeof addr.sun_path) {
        if (s >= 0)
            close(s);
        return -1;
    }
    strcpy(addr.sun_path, path);
    unlink(path); /* left over by a KWin that didn't exit cleanly */
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s, 16) != 0) {
        cham_log("clients: cannot listen on %s: %s", path, strerror(errno));
        close(s);
        return -1;
    }
    return s;
}

static void *clients_main(void *arg)
{
    (void)arg;
    char wayland[sizeof(((struct sockaddr_un *)0)->sun_path)] = "";
    while (!wayland[0]) {
        input_socket_path(wayland, sizeof wayland);
        if (!wayland[0])
            usleep(200 * 1000);
    }
    char path[128];
    snprintf(path, sizeof path, "%s%s", wayland, CHAM_CLIENT_SOCKET_SUFFIX);
    int l = listen_at(path);
    if (l < 0)
        return NULL;
    cham_log("clients: GPU buffers from apps accepted at %s", path);

    struct pollfd p[1 + MAX_CLIENTS];
    int n = 0; /* clients */
    for (;;) {
        p[0] = (struct pollfd){.fd = l, .events = POLLIN};
        if (poll(p, 1 + (nfds_t)n, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int i = 1; i <= n; i++) {
            if (!p[i].revents)
                continue;
            if ((p[i].revents & POLLIN) && handle(p[i].fd))
                continue;
            forget(p[i].fd, 0, 1);
            close(p[i].fd);
            p[i] = p[n--];
            i--;
        }
        if (p[0].revents & POLLIN) {
            int c = accept4(l, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0 && n < MAX_CLIENTS)
                p[++n] = (struct pollfd){.fd = c, .events = POLLIN};
            else if (c >= 0)
                close(c);
        }
    }
    close(l);
    return NULL;
}

/* ---- Xwayland's side ----
 * Xwayland with glamor runs on libchameleon.so too (bin/Xwayland): its
 * window buffers are gbm buffers, i.e. AHardwareBuffers, which it hands to
 * KWin as dmabufs. Each one is registered with KWin like an app's buffer,
 * under its GEM handle as id, for as long as it exists. */

static pthread_mutex_t g_share_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_share_sock = -2; /* -2: not connected yet, -1: failed */

/* "<KWin's Wayland socket>.chameleon"; KWin sets WAYLAND_DISPLAY for Xwayland. */
static int share_connect(void)
{
    char wayland[sizeof(((struct sockaddr_un *)0)->sun_path)];
    input_socket_path(wayland, sizeof wayland); /* CHAMELEON_WAYLAND_SOCKET */
    if (!wayland[0]) {
        const char *name = getenv("WAYLAND_DISPLAY");
        const char *dir = getenv("XDG_RUNTIME_DIR");
        if (!name || !*name)
            name = "wayland-0";
        if (name[0] == '/')
            snprintf(wayland, sizeof wayland, "%s", name);
        else if (dir && *dir)
            snprintf(wayland, sizeof wayland, "%s/%s", dir, name);
    }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (snprintf(addr.sun_path, sizeof addr.sun_path, "%s%s", wayland, CHAM_CLIENT_SOCKET_SUFFIX) >=
        (int)sizeof addr.sun_path)
        return -1;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s >= 0 && connect(s, (struct sockaddr *)&addr, sizeof addr) == 0)
        return s;
    cham_log("xwayland: cannot reach KWin at %s: %s", addr.sun_path, strerror(errno));
    if (s >= 0)
        close(s);
    return -1;
}

void clients_share(struct cham_bo *bo)
{
    pthread_mutex_lock(&g_share_lock);
    if (g_share_sock == -2)
        g_share_sock = share_connect();
    struct cham_msg m = {CHAM_CLIENT_BUFFER_ADD, bo->handle, 0, 0};
    int ok = g_share_sock >= 0 && cham_send(g_share_sock, &m, -1) == 0 && ahb_send(bo->ahb, g_share_sock) == 0;
    while (ok) {
        struct cham_msg ack;
        int fd = -1;
        if (cham_recv(g_share_sock, &ack, &fd) <= 0) {
            ok = 0;
            break;
        }
        if (fd >= 0)
            close(fd);
        if (ack.type == CHAM_CLIENT_BUFFER_ACK && ack.id == bo->handle) {
            ok = ack.a == 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_share_lock);
    if (!ok)
        cham_log("xwayland: KWin did not take buffer %u (%ux%u); that window won't show", bo->handle,
                 bo->width, bo->height);
}

void clients_unshare(uint32_t handle)
{
    pthread_mutex_lock(&g_share_lock);
    struct cham_msg m = {CHAM_CLIENT_BUFFER_REMOVE, handle, 0, 0};
    if (g_share_sock >= 0)
        cham_send(g_share_sock, &m, -1);
    pthread_mutex_unlock(&g_share_lock);
}

__attribute__((constructor)) static void clients_start(void)
{
    if (role_xwayland())
        return; /* KWin's registry, not ours to serve */
    pthread_t t;
    if (pthread_create(&t, NULL, clients_main, NULL) == 0)
        pthread_detach(t);
}
