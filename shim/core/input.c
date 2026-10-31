/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The app's input, window size and clipboard, on one thread.
 *
 * CHAM_INPUT messages become events of the evdev devices (evdev.c) and the
 * Android keyboard's text goes to chameleon-ime or is typed as keys
 * (text.c, keymap.c). The app's window size (CHAM_CONFIG) becomes the
 * connector's mode and a card hotplug (kms.c, evdev.c). The thread also
 * connects to the compositor's Wayland socket as an ordinary client, for
 * clipboard.c's clipboard sharing and keymap.c's view of the desktop's
 * keyboard layout.
 *
 * The socket path comes from the process that bound it (its bind(),
 * reported by libchameleon.so) or $CHAMELEON_WAYLAND_SOCKET (wlpath.c), and
 * libwayland-client is loaded with dlopen(), so nothing here depends on how
 * the compositor was started. The link reader thread only writes messages
 * into a pipe; all Wayland calls happen on the input thread.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../common/chameleon_evdev.h"
#include "../../common/chameleon_proto.h"
#include "internal.h"
#include "wlclient.h"

/* ---- libwayland-client, loaded at run time (wlclient.h) ---- */

struct cham_wl wl;
const struct wl_interface *const k_wl_null[8];

void wl_ignore_event(void)
{
}

/* ---- state (input thread only, except the pipe) ---- */

static int g_pipe[2] = {-1, -1};
static struct wl_display *g_display;
static struct wl_proxy *g_registry;

static int load_wayland(void)
{
    static int loaded = -1;
    if (loaded >= 0)
        return loaded;
    char path[512];
    const char *prefix = getenv("PREFIX");
    snprintf(path, sizeof path, "%s/lib/libwayland-client.so",
             prefix && *prefix ? prefix : "/data/data/com.termux/files/usr");
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        cham_log("clipboard and typed text disabled: cannot load libwayland-client: %s", dlerror());
        return loaded = 0;
    }
    if (!cham_wl_load(lib)) {
        cham_log("clipboard and typed text disabled: libwayland-client is too old (need 1.20+)");
        return loaded = 0;
    }
    return loaded = 1;
}

/* ---- registry ---- */

static void registry_global(void *data, struct wl_proxy *registry, uint32_t name, const char *iface,
                            uint32_t version)
{
    (void)data;
    (void)registry;
    clipboard_global(g_registry, name, iface, version);
    keymap_global(g_registry, name, iface);
}

static void registry_global_remove(void *data, struct wl_proxy *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static void (*k_registry_listener[])(void) = {
    (void (*)(void))registry_global,
    (void (*)(void))registry_global_remove,
};

/* ---- connection ---- */

static void disconnect_wayland(void)
{
    clipboard_disconnected();
    keymap_disconnected();
    if (g_display)
        wl.disconnect(g_display);
    g_display = NULL;
    g_registry = NULL;
}

static int connect_wayland(void)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    input_socket_path(path, sizeof path);
    if (!path[0] || !load_wayland())
        return 0;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    memcpy(addr.sun_path, path, sizeof addr.sun_path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        cham_log("input: cannot connect to %s: %s", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return 0;
    }
    g_display = wl.connect_to_fd(fd); /* takes the fd */
    if (!g_display) {
        cham_log("input: wl_display_connect_to_fd failed");
        return 0;
    }
    g_registry = wl.marshal_flags((struct wl_proxy *)g_display, 1 /* get_registry */, wl.registry_iface,
                                  wl.get_version((struct wl_proxy *)g_display), 0, NULL);
    wl.add_listener(g_registry, k_registry_listener, NULL);
    wl.roundtrip(g_display);
    wl.roundtrip(g_display); /* the globals' first events (keymap) */
    cham_log("input: connected to the compositor");
    clipboard_connected(g_display);
    return 1;
}

/* ---- thread ---- */

/* The app's window, as the panel: the compositor follows a hotplug. With
 * CHAMELEON_RESIZE=0 the panel keeps its first size (the app scales). */
static void config(uint32_t width, uint32_t height, uint32_t refresh_mhz)
{
    static int disabled = -1;
    if (disabled < 0) {
        const char *env = getenv("CHAMELEON_RESIZE");
        disabled = env && strcmp(env, "0") == 0;
        if (disabled)
            cham_log("CHAMELEON_RESIZE=0: the compositor's screen keeps its size");
    }
    if (!disabled && width && height && kms_panel(width, height, refresh_mhz))
        evdev_uevent(CHAM_UEVENT_CARD_HOTPLUG);
}

/* A keysym typed on the evdev keyboard; one the layout has no key for is
 * lost, as on a real keyboard. */
void input_keysym(uint32_t sym, int down)
{
    static int warned;
    if (!keymap_type(sym, down) && down && !warned++)
        cham_log("input: the desktop's layout has no key for some typed characters (keysym 0x%x); they are dropped",
                 sym);
}

/* Not a presenter message: chameleon-ime connected (input_ime_connected). */
#define IME_CONNECTED 0x494d45u

static void *input_main(void *arg)
{
    (void)arg;
    uint64_t next_connect = 0;
    for (;;) {
        struct pollfd p[3] = {{.fd = g_pipe[0], .events = POLLIN},
                              {.fd = -1, .events = POLLIN},
                              {.fd = text_ime_fd(), .events = POLLIN}};
        if (g_display) {
            while (wl.prepare_read(g_display) != 0)
                wl.dispatch_pending(g_display);
            wl.flush(g_display);
            p[1].fd = wl.get_fd(g_display);
        }
        int n = poll(p, 3, -1);
        if (g_display) {
            if (n > 0 && (p[1].revents & (POLLIN | POLLERR | POLLHUP)))
                wl.read_events(g_display);
            else
                wl.cancel_read(g_display);
            wl.dispatch_pending(g_display);
            if (wl.get_error(g_display)) {
                cham_log("input: lost the connection to the compositor (error %d)", wl.get_error(g_display));
                disconnect_wayland();
            }
        }
        if (n > 0 && p[2].fd >= 0 && (p[2].revents & (POLLIN | POLLERR | POLLHUP)))
            text_ime_readable();

        struct cham_msg batch[64];
        ssize_t got = 0;
        if (n > 0 && (p[0].revents & POLLIN))
            got = read(g_pipe[0], batch, sizeof batch);
        size_t count = got > 0 ? (size_t)got / sizeof batch[0] : 0;
        /* Typed text needs the connection (keymap.c's layout), and so does
         * the clipboard. */
        if (!g_display && count && now_ns() >= next_connect && !connect_wayland())
            next_connect = now_ns() + 1000000000ull;
        for (size_t i = 0; i < count; i++) {
            const struct cham_msg *m = &batch[i];
            if (m->type == CHAM_CONFIG) {
                config(CHAM_CONFIG_WIDTH(m), CHAM_CONFIG_HEIGHT(m), (uint32_t)m->b);
            } else if (m->type == CHAM_WINDOW) {
                evdev_window(CHAM_CONFIG_WIDTH(m), CHAM_CONFIG_HEIGHT(m));
            } else if (m->type == IME_CONNECTED) {
                text_ime_attach((int)m->a);
            } else if (m->type == CHAM_INPUT && m->id == CHAM_INPUT_KEYSYM) {
                input_keysym((uint32_t)m->a, m->b != 0);
            } else if (m->type == CHAM_INPUT && !text_input(m)) {
                evdev_input(m);
            } else if (m->type == CHAM_CLIPBOARD_OFFER) {
                clipboard_offer(m->a != 0); /* without a connection, kept for it */
            }
        }
        if (g_display)
            wl.flush(g_display);
    }
    return NULL;
}

static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void start(void)
{
    if (pipe2(g_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    /* The reader must block in poll(), not in read(). */
    fcntl(g_pipe[0], F_SETFL, fcntl(g_pipe[0], F_GETFL) & ~O_NONBLOCK);
    pthread_t t;
    if (pthread_create(&t, NULL, input_main, NULL) == 0)
        pthread_detach(t);
}

void input_ime_connected(int fd)
{
    struct cham_msg m = {IME_CONNECTED, 0, (uint64_t)fd, 0};
    input_post(&m);
}

/* CHAM_INPUT, CHAM_CONFIG, CHAM_WINDOW and CHAM_CLIPBOARD_OFFER messages, from the
 * presenter link's thread. */
void input_post(const struct cham_msg *m)
{
    pthread_once(&g_once, start);
    if (g_pipe[1] < 0)
        return;
    /* A pipe write of one message is atomic; if the thread is stuck the pipe
     * fills and input is dropped rather than blocking the presenter link. */
    if (write(g_pipe[1], m, sizeof *m) != (ssize_t)sizeof *m && errno != EAGAIN)
        cham_log("input: cannot queue event: %s", strerror(errno));
}
