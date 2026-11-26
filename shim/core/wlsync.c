/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Implicit sync at the Wayland boundary, for the session's Wayland clients.
 *
 * On Linux a GPU driver attaches a fence to each buffer a GPU job writes,
 * and a compositor that gets a dmabuf from a client waits on it (poll() on
 * the dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE) before it reads the buffer.
 * Android's drivers attach no such fences, so a client that hands a dmabuf
 * to the compositor right after rendering into it (Xwayland's glamor, any
 * client rendering into gbm buffers) would have its buffer read half drawn.
 *
 * So at the point where the protocol needs the buffer finished, when the
 * client attaches a dmabuf wl_buffer to a surface (wl_surface.attach), the
 * fence of the GPU work its current context submitted so far is given to
 * the session for that buffer (core/dmabuf.c), and waited for here if the
 * compositor isn't one that asks for it. Which context, and how, is the EGL
 * vendor's business (cham_set_present_wait()); without a current context of
 * Android's driver on the thread nothing is done. The vendor's own EGL and
 * Vulkan windows do the same in vendor/window.c and don't come through here.
 *
 * libwayland-client sends every request through wl_proxy_marshal_flags()
 * (the code wayland-scanner generates since 1.20), which this library, being
 * preloaded, provides: the arguments are read by the request's signature
 * exactly as libwayland does and passed on unchanged to
 * wl_proxy_marshal_array_flags(). Watched: zwp_linux_buffer_params_v1 add
 * (the dmabuf), create_immed (the wl_buffer it returns is a dmabuf buffer)
 * and destroy, wl_buffer destroy, wl_surface attach.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cham_shim.h"
#include "wlclient.h"

union wl_argument {
    int32_t i;
    uint32_t u;
    int32_t f;
    const char *s;
    void *o;
    uint32_t n;
    void *a;
    int32_t h;
};
#define MAX_ARGS 20 /* WL_CLOSURE_MAX_ARGS */

static struct wl_proxy *(*p_marshal_array_flags)(struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t,
                                                 uint32_t, union wl_argument *);
static const struct wl_interface *(*p_get_interface)(struct wl_proxy *); /* 1.24+ */

/* The caller has libwayland-client loaded (it called us from there). */
static void resolve(void)
{
    static const char *const names[] = {"libwayland-client.so", "libwayland-client.so.0"};
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !p_marshal_array_flags; i++) {
        void *lib = dlopen(names[i], RTLD_NOW | RTLD_NOLOAD);
        if (!lib)
            continue;
        *(void **)&p_marshal_array_flags = dlsym(lib, "wl_proxy_marshal_array_flags");
        *(void **)&p_get_interface = dlsym(lib, "wl_proxy_get_interface");
    }
    if (!p_marshal_array_flags)
        *(void **)&p_marshal_array_flags = dlsym(RTLD_NEXT, "wl_proxy_marshal_array_flags");
    if (!p_marshal_array_flags) {
        cham_log("wayland: cannot find libwayland-client's wl_proxy_marshal_array_flags");
        abort(); /* the request can't be sent at all */
    }
}

static const struct wl_interface *interface_of(struct wl_proxy *proxy)
{
    if (p_get_interface)
        return p_get_interface(proxy);
    /* Before 1.24: a proxy starts with its wl_object, which starts with
     * its interface. */
    return *(const struct wl_interface *const *)proxy;
}

/* ---- the dmabuf wl_buffers this process made ---- */

/* A wl_buffer, or while it is being made its zwp_linux_buffer_params_v1,
 * with our own fd of its first plane's dmabuf (-1 if none was kept). */
struct made {
    struct wl_proxy *proxy;
    int fd;
};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct made *g_made;
static size_t g_count, g_cap;

static void made_add(struct wl_proxy *proxy, int fd)
{
    pthread_mutex_lock(&g_lock);
    if (g_count == g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 64;
        struct made *grown = realloc(g_made, cap * sizeof *grown);
        if (grown) {
            g_made = grown;
            g_cap = cap;
        }
    }
    if (g_count < g_cap)
        g_made[g_count++] = (struct made){proxy, fd};
    else if (fd >= 0)
        close(fd);
    pthread_mutex_unlock(&g_lock);
}

/* Whether `proxy` is one of them; *fd gets a new fd of its dmabuf (the
 * caller's), or -1. With `remove` it is forgotten and *fd is our own. */
static int made_find(struct wl_proxy *proxy, int remove, int *fd)
{
    int found = 0;
    *fd = -1;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_count; i++) {
        if (g_made[i].proxy == proxy) {
            found = 1;
            *fd = remove || g_made[i].fd < 0 ? g_made[i].fd : fcntl(g_made[i].fd, F_DUPFD_CLOEXEC, 0);
            if (remove)
                g_made[i] = g_made[--g_count];
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

static void made_forget(struct wl_proxy *proxy)
{
    int fd;
    if (made_find(proxy, 1, &fd) && fd >= 0)
        close(fd);
}

static _Atomic(void (*)(int)) g_wait;

CHAM_EXPORT void cham_set_present_wait(void (*wait)(int dmabuf))
{
    atomic_store(&g_wait, wait);
}

/* ---- the request path ---- */

CHAM_EXPORT struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
                                                    const struct wl_interface *interface, uint32_t version,
                                                    uint32_t flags, ...)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, resolve);

    const struct wl_interface *own = interface_of(proxy);
    const char *sig = own->methods[opcode].signature;
    union wl_argument args[MAX_ARGS];
    int n = 0;
    va_list ap;
    va_start(ap, flags);
    for (const char *c = sig; *c && n < MAX_ARGS; c++) {
        switch (*c) {
        case 'i':
            args[n++].i = va_arg(ap, int32_t);
            break;
        case 'u':
            args[n++].u = va_arg(ap, uint32_t);
            break;
        case 'f':
            args[n++].f = va_arg(ap, int32_t);
            break;
        case 's':
            args[n++].s = va_arg(ap, const char *);
            break;
        case 'o':
        case 'n':
            args[n++].o = va_arg(ap, void *);
            break;
        case 'a':
            args[n++].a = va_arg(ap, void *);
            break;
        case 'h':
            args[n++].h = va_arg(ap, int32_t);
            break;
        default: /* '?' (nullable) and the version digits */
            break;
        }
    }
    va_end(ap);

    const char *name = own->name;
    int params = strcmp(name, "zwp_linux_buffer_params_v1") == 0, fd = -1;
    if (opcode == 1 && strcmp(name, "wl_surface") == 0) {
        /* attach(buffer, x, y): the buffer must be finished when the
         * compositor reads it, or the compositor know when it will be */
        void (*wait)(int) = atomic_load(&g_wait);
        if (wait && args[0].o && made_find(args[0].o, 0, &fd))
            wait(fd);
        if (fd >= 0)
            close(fd);
        fd = -1;
    } else if (opcode == 0 && strcmp(name, "wl_buffer") == 0) {
        made_forget(proxy); /* destroy: the proxy goes away */
    } else if (params && opcode == 1 && args[1].u == 0) {
        /* add(fd, plane 0, ...): the buffer's dmabuf */
        made_forget(proxy);
        made_add(proxy, fcntl(args[0].h, F_DUPFD_CLOEXEC, 0));
    } else if (params && opcode == 3) {
        made_find(proxy, 1, &fd); /* create_immed: its dmabuf goes on to the wl_buffer */
    } else if (params && opcode == 0) {
        made_forget(proxy); /* destroy */
    }

    struct wl_proxy *made = p_marshal_array_flags(proxy, opcode, interface, version, flags, args);

    if (made && params && opcode == 3)
        made_add(made, fd);
    else if (fd >= 0)
        close(fd);
    return made;
}
