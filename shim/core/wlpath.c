/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * KWin's Wayland socket: libchameleon.so in KWin sees it (KWin's bind(), or
 * $CHAMELEON_WAYLAND_SOCKET from bin/kwin_wayland) for clients.c, and tells
 * chameleon-kernel, whose input thread connects to it (input.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>

#include "internal.h"

static pthread_mutex_t g_path_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_wayland_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

void input_note_socket(const char *path)
{
    pthread_mutex_lock(&g_path_lock); /* the latest: a KWin started again after one died */
    strncpy(g_wayland_path, path, sizeof g_wayland_path - 1);
    pthread_mutex_unlock(&g_path_lock);
}

void input_socket_path(char *out, size_t size)
{
    /* kwin_wayland_wrapper (Plasma) creates the socket and passes KWin only
     * its fd, so KWin never binds it; bin/kwin_wayland then names it. */
    const char *env = getenv("CHAMELEON_WAYLAND_SOCKET");
    if (env && *env) {
        snprintf(out, size, "%s", env);
        return;
    }
    pthread_mutex_lock(&g_path_lock);
    snprintf(out, size, "%s", g_wayland_path);
    pthread_mutex_unlock(&g_path_lock);
}
