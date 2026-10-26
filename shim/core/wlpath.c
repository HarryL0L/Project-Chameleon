/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The compositor's Wayland socket, in chameleon-kernel. libchameleon.so in
 * every program tells it each "wayland-*" socket a program binds (card.c),
 * and the compositor's is the one bound by the DRM master's process or by
 * one of its ancestors: KWin's comes from kwin_wayland_wrapper, its parent,
 * Weston and sway bind their own, and a proxy (Firefox's wayland-proxy-N)
 * or a nested compositor binding sockets of its own doesn't count.
 * $CHAMELEON_WAYLAND_SOCKET names it instead. The input thread connects to
 * it (input.c) and apps' buffers come next to it (clients.c).
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>

#include "internal.h"

#define BOUND 16

static pthread_mutex_t g_path_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    pid_t pid;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} g_bound[BOUND]; /* under g_path_lock */
static unsigned g_next;
static char g_chosen[sizeof g_bound[0].path];

void input_note_socket(pid_t pid, const char *path)
{
    pthread_mutex_lock(&g_path_lock);
    unsigned i = 0;
    while (i < BOUND && g_bound[i].pid != pid)
        i++;
    if (i == BOUND) /* a new binder takes the oldest slot */
        i = g_next++ % BOUND;
    g_bound[i].pid = pid;
    snprintf(g_bound[i].path, sizeof g_bound[i].path, "%s", path);
    pthread_mutex_unlock(&g_path_lock);
}

/* The parent of `pid` from /proc, or 0. */
static pid_t parent_of(pid_t pid)
{
    char path[32], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "re");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f)
        fclose(f);
    buf[n] = 0;
    const char *p = strrchr(buf, ')'); /* after the command name, which may hold anything */
    int ppid = 0;
    return p && sscanf(p + 1, " %*c %d", &ppid) == 1 ? (pid_t)ppid : 0;
}

void input_socket_path(char *out, size_t size)
{
    /* For a compositor given its socket from outside the session. */
    const char *env = getenv("CHAMELEON_WAYLAND_SOCKET");
    if (env && *env) {
        snprintf(out, size, "%s", env);
        return;
    }
    pid_t master = kms_master_pid();
    pthread_mutex_lock(&g_path_lock);
    const char *found = "";
    for (pid_t p = master; p > 1 && !*found; p = parent_of(p)) /* the master itself first */
        for (int i = 0; i < BOUND; i++)
            if (g_bound[i].pid == p && (kill(p, 0) == 0 || errno == EPERM))
                found = g_bound[i].path;
    if (strcmp(found, g_chosen) != 0) {
        snprintf(g_chosen, sizeof g_chosen, "%s", found);
        if (*found)
            cham_log("the compositor's Wayland socket: %s", found);
    }
    snprintf(out, size, "%s", g_chosen);
    pthread_mutex_unlock(&g_path_lock);
}
