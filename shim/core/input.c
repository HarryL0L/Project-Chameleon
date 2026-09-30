/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Touch, pointer and keyboard input from the Chameleon app into KWin.
 *
 * KWin has no input devices of its own here (no libinput), but it always
 * runs its fake-input backend: org_kde_kwin_fake_input, which it offers to
 * any client in its own process (allowInterface() allows pid == getpid(),
 * and authenticate is accepted unconditionally). So a helper thread in this
 * process connects to KWin's Wayland socket as an ordinary client, binds
 * fake input plus xdg_output (for the screen's logical geometry), and turns
 * the app's CHAM_INPUT messages into fake-input requests. The same
 * connection carries output.c's screen-size changes (CHAM_CONFIG).
 *
 * The socket path is taken from KWin's own bind() (interpose.c), or from
 * $CHAMELEON_WAYLAND_SOCKET when kwin_wayland_wrapper made the socket, and
 * libwayland-client is loaded with dlopen(), so nothing here depends on how
 * KWin was started. The link reader thread only writes messages into a pipe;
 * all Wayland calls happen on the input thread.
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

#include "../../common/chameleon_proto.h"
#include "internal.h"
#include "wlclient.h"

/* ---- libwayland-client, loaded at run time (wlclient.h) ---- */

struct cham_wl wl;
const struct wl_interface *const k_wl_null[8];

void wl_ignore_event(void)
{
}

static wl_fixed_t fixed(double d)
{
    return (wl_fixed_t)(d * 256.0);
}

/* ---- protocol definitions (what wayland-scanner would generate) ---- */

/* org_kde_kwin_fake_input v6, plasma-wayland-protocols fake-input.xml */
enum {
    FI_AUTHENTICATE = 0,
    FI_POINTER_MOTION = 1,
    FI_BUTTON = 2,
    FI_AXIS = 3,
    FI_TOUCH_DOWN = 4,
    FI_TOUCH_MOTION = 5,
    FI_TOUCH_UP = 6,
    FI_TOUCH_CANCEL = 7,
    FI_TOUCH_FRAME = 8,
    FI_POINTER_MOTION_ABSOLUTE = 9,
    FI_KEYBOARD_KEY = 10,
    FI_DESTROY = 11,
    FI_KEYBOARD_KEYSYM = 12,
};
static const struct wl_message k_fake_input_requests[] = {
    {"authenticate", "ss", k_null},
    {"pointer_motion", "ff", k_null},
    {"button", "uu", k_null},
    {"axis", "uf", k_null},
    {"touch_down", "2uff", k_null},
    {"touch_motion", "2uff", k_null},
    {"touch_up", "2u", k_null},
    {"touch_cancel", "2", k_null},
    {"touch_frame", "2", k_null},
    {"pointer_motion_absolute", "3ff", k_null},
    {"keyboard_key", "4uu", k_null},
    {"destroy", "5", k_null},
    {"keyboard_keysym", "6uu", k_null},
};
static const struct wl_interface k_fake_input_iface = {
    "org_kde_kwin_fake_input", 6, 13, k_fake_input_requests, 0, NULL,
};

/* zxdg_output_manager_v1 / zxdg_output_v1, bound at version 1 */
static const struct wl_interface k_xdg_output_iface;
static const struct wl_interface *k_get_xdg_output_types[2] = {&k_xdg_output_iface, NULL /* wl_output */};
static const struct wl_message k_xdg_output_manager_requests[] = {
    {"destroy", "", k_null},
    {"get_xdg_output", "no", k_get_xdg_output_types},
};
static const struct wl_interface k_xdg_output_manager_iface = {
    "zxdg_output_manager_v1", 1, 2, k_xdg_output_manager_requests, 0, NULL,
};
static const struct wl_message k_xdg_output_requests[] = {{"destroy", "", k_null}};
static const struct wl_message k_xdg_output_events[] = {
    {"logical_position", "ii", k_null},
    {"logical_size", "ii", k_null},
    {"done", "", k_null},
};
static const struct wl_interface k_xdg_output_iface = {
    "zxdg_output_v1", 1, 1, k_xdg_output_requests, 3, k_xdg_output_events,
};

/* ---- state (input thread only, except the pipe) ---- */

static int g_pipe[2] = {-1, -1};
static pthread_mutex_t g_path_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_wayland_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

static struct wl_display *g_display;
static struct wl_proxy *g_registry, *g_fake_input, *g_output, *g_xdg_manager, *g_xdg_output;
static uint32_t g_fake_input_version;
static struct {
    int32_t x, y, w, h;
    int known;
} g_logical;

void input_note_socket(const char *path)
{
    pthread_mutex_lock(&g_path_lock);
    if (!g_wayland_path[0]) {
        strncpy(g_wayland_path, path, sizeof g_wayland_path - 1);
        cham_log("KWin's Wayland socket: %s", g_wayland_path);
    }
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
        cham_log("input and screen resizing disabled: cannot load libwayland-client: %s", dlerror());
        return loaded = 0;
    }
#define W(field, name) (*(void **)&wl.field = dlsym(lib, name))
    W(connect_to_fd, "wl_display_connect_to_fd");
    W(disconnect, "wl_display_disconnect");
    W(roundtrip, "wl_display_roundtrip");
    W(flush, "wl_display_flush");
    W(get_fd, "wl_display_get_fd");
    W(prepare_read, "wl_display_prepare_read");
    W(read_events, "wl_display_read_events");
    W(cancel_read, "wl_display_cancel_read");
    W(dispatch_pending, "wl_display_dispatch_pending");
    W(get_error, "wl_display_get_error");
    W(marshal_flags, "wl_proxy_marshal_flags");
    W(add_listener, "wl_proxy_add_listener");
    W(get_version, "wl_proxy_get_version");
    W(get_user_data, "wl_proxy_get_user_data");
    W(proxy_destroy, "wl_proxy_destroy");
    W(registry_iface, "wl_registry_interface");
    W(output_iface, "wl_output_interface");
#undef W
    if (!wl.connect_to_fd || !wl.marshal_flags || !wl.add_listener || !wl.registry_iface || !wl.output_iface ||
        !wl.prepare_read || !wl.get_version || !wl.get_user_data || !wl.proxy_destroy) {
        cham_log("input and screen resizing disabled: libwayland-client is too old (need 1.20+)");
        return loaded = 0;
    }
    k_get_xdg_output_types[1] = wl.output_iface;
    return loaded = 1;
}

/* ---- registry / xdg_output listeners ---- */

static void registry_global(void *data, struct wl_proxy *registry, uint32_t name, const char *iface,
                            uint32_t version)
{
    (void)data;
    (void)registry;
    if (strcmp(iface, "org_kde_kwin_fake_input") == 0 && !g_fake_input) {
        g_fake_input_version = version < 6 ? version : 6;
        g_fake_input = wl_bind(g_registry, name, &k_fake_input_iface, g_fake_input_version);
    } else if (strcmp(iface, "wl_output") == 0 && !g_output) {
        g_output = wl_bind(g_registry, name, wl.output_iface, 1); /* no listener: its events are dropped */
    } else if (strcmp(iface, "zxdg_output_manager_v1") == 0 && !g_xdg_manager) {
        g_xdg_manager = wl_bind(g_registry, name, &k_xdg_output_manager_iface, 1);
    } else {
        output_global(g_registry, name, iface, version);
    }
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

static void xdg_logical_position(void *data, struct wl_proxy *o, int32_t x, int32_t y)
{
    (void)data;
    (void)o;
    g_logical.x = x;
    g_logical.y = y;
}

static void xdg_logical_size(void *data, struct wl_proxy *o, int32_t w, int32_t h)
{
    (void)data;
    (void)o;
    g_logical.w = w;
    g_logical.h = h;
}

static void xdg_done(void *data, struct wl_proxy *o)
{
    (void)data;
    (void)o;
    if (g_logical.w > 0 && g_logical.h > 0) {
        cham_log("input: screen is %dx%d logical pixels at %d,%d", g_logical.w, g_logical.h, g_logical.x,
                 g_logical.y);
        g_logical.known = 1;
    }
}

static void (*k_xdg_output_listener[])(void) = {
    (void (*)(void))xdg_logical_position,
    (void (*)(void))xdg_logical_size,
    (void (*)(void))xdg_done,
};

/* ---- connection ---- */

static void disconnect_wayland(void)
{
    output_disconnected();
    if (g_display)
        wl.disconnect(g_display);
    g_display = NULL;
    g_registry = g_fake_input = g_output = g_xdg_manager = g_xdg_output = NULL;
    memset(&g_logical, 0, sizeof g_logical);
}

static int connect_wayland(void)
{
    char path[sizeof g_wayland_path];
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
    if (!g_fake_input) {
        cham_log("input: KWin offers no org_kde_kwin_fake_input");
        disconnect_wayland();
        return 0;
    }
    wl.marshal_flags(g_fake_input, FI_AUTHENTICATE, NULL, g_fake_input_version, 0, "Chameleon",
                     "Touch and keyboard input from the Chameleon app");
    if (g_xdg_manager && g_output) {
        g_xdg_output = wl.marshal_flags(g_xdg_manager, 1 /* get_xdg_output */, &k_xdg_output_iface, 1, 0, NULL,
                                        g_output);
        wl.add_listener(g_xdg_output, k_xdg_output_listener, NULL);
    }
    wl.roundtrip(g_display);
    cham_log("input: connected to KWin (fake input v%u)", g_fake_input_version);
    output_connected(g_display);
    return 1;
}

/* ---- event translation ---- */

static void screen_geometry(double *x, double *y, double *w, double *h)
{
    if (g_logical.known) {
        /* The app may crop a few pixels off a mode (output.c). */
        double fx, fy;
        output_visible(&fx, &fy);
        *x = g_logical.x;
        *y = g_logical.y;
        *w = g_logical.w * fx;
        *h = g_logical.h * fy;
        return;
    }
    uint32_t mw = 0, mh = 0;
    kms_mode_size(&mw, &mh);
    if (!mw || !mh)
        link_app_size(&mw, &mh);
    *x = *y = 0;
    *w = mw ? mw : 1;
    *h = mh ? mh : 1;
}

static void to_logical(uint64_t pos, wl_fixed_t *fx, wl_fixed_t *fy)
{
    double x, y, w, h;
    screen_geometry(&x, &y, &w, &h);
    *fx = fixed(x + CHAM_INPUT_X(pos) / 65536.0 * w);
    *fy = fixed(y + CHAM_INPUT_Y(pos) / 65536.0 * h);
}

static void forward(const struct cham_msg *m)
{
    struct wl_proxy *fi = g_fake_input;
    uint32_t v = g_fake_input_version;
    wl_fixed_t x, y;
    double sx, sy, w, h;
    switch (m->id) {
    case CHAM_INPUT_TOUCH_DOWN:
    case CHAM_INPUT_TOUCH_MOTION:
        if (v < 2)
            break;
        to_logical(m->b, &x, &y);
        wl.marshal_flags(fi, m->id == CHAM_INPUT_TOUCH_DOWN ? FI_TOUCH_DOWN : FI_TOUCH_MOTION, NULL, v, 0,
                         (uint32_t)m->a, x, y);
        break;
    case CHAM_INPUT_TOUCH_UP:
        if (v >= 2)
            wl.marshal_flags(fi, FI_TOUCH_UP, NULL, v, 0, (uint32_t)m->a);
        break;
    case CHAM_INPUT_TOUCH_CANCEL:
        if (v >= 2)
            wl.marshal_flags(fi, FI_TOUCH_CANCEL, NULL, v, 0);
        break;
    case CHAM_INPUT_TOUCH_FRAME:
        if (v >= 2)
            wl.marshal_flags(fi, FI_TOUCH_FRAME, NULL, v, 0);
        break;
    case CHAM_INPUT_POINTER_MOTION:
        screen_geometry(&sx, &sy, &w, &h);
        wl.marshal_flags(fi, FI_POINTER_MOTION, NULL, v, 0, fixed(CHAM_INPUT_DX(m->b) / 1048576.0 * w),
                         fixed(CHAM_INPUT_DY(m->b) / 1048576.0 * h));
        break;
    case CHAM_INPUT_POINTER_ABS:
        if (v < 3)
            break;
        to_logical(m->b, &x, &y);
        wl.marshal_flags(fi, FI_POINTER_MOTION_ABSOLUTE, NULL, v, 0, x, y);
        break;
    case CHAM_INPUT_BUTTON:
        wl.marshal_flags(fi, FI_BUTTON, NULL, v, 0, (uint32_t)m->a, (uint32_t)(m->b ? 1 : 0));
        break;
    case CHAM_INPUT_AXIS:
        wl.marshal_flags(fi, FI_AXIS, NULL, v, 0, (uint32_t)(m->a ? 1 : 0), (wl_fixed_t)(int64_t)m->b);
        break;
    case CHAM_INPUT_KEYSYM:
        if (v >= 6)
            wl.marshal_flags(fi, FI_KEYBOARD_KEYSYM, NULL, v, 0, (uint32_t)m->a, (uint32_t)(m->b ? 1 : 0));
        break;
    case CHAM_INPUT_KEY:
        if (v >= 4)
            wl.marshal_flags(fi, FI_KEYBOARD_KEY, NULL, v, 0, (uint32_t)m->a, (uint32_t)(m->b ? 1 : 0));
        break;
    }
}

/* ---- thread ---- */

static void *input_main(void *arg)
{
    (void)arg;
    int warned = 0;
    uint64_t next_connect = 0;
    for (;;) {
        struct pollfd p[2] = {{.fd = g_pipe[0], .events = POLLIN}, {.fd = -1, .events = POLLIN}};
        if (g_display) {
            while (wl.prepare_read(g_display) != 0)
                wl.dispatch_pending(g_display);
            wl.flush(g_display);
            p[1].fd = wl.get_fd(g_display);
        }
        /* Output work may be waiting for a deadline, or for KWin's socket. */
        int timeout = output_timeout_ms();
        if (!g_display && output_pending() && (timeout < 0 || timeout > 1000))
            timeout = 1000;
        int n = poll(p, 2, timeout);
        if (g_display) {
            if (n > 0 && (p[1].revents & (POLLIN | POLLERR | POLLHUP)))
                wl.read_events(g_display);
            else
                wl.cancel_read(g_display);
            wl.dispatch_pending(g_display);
            if (wl.get_error(g_display)) {
                cham_log("input: lost the connection to KWin (error %d)", wl.get_error(g_display));
                disconnect_wayland();
            }
        }

        struct cham_msg batch[64];
        ssize_t got = 0;
        if (n > 0 && (p[0].revents & POLLIN))
            got = read(g_pipe[0], batch, sizeof batch);
        int have_input = 0;
        for (size_t i = 0; got > 0 && i < (size_t)got / sizeof batch[0]; i++) {
            if (batch[i].type == CHAM_CONFIG)
                output_config(CHAM_CONFIG_WIDTH(&batch[i]), CHAM_CONFIG_HEIGHT(&batch[i]), (uint32_t)batch[i].b);
            else
                have_input = 1;
        }

        if (!g_display && (have_input || output_pending()) && now_ns() >= next_connect) {
            if (!connect_wayland()) {
                next_connect = now_ns() + 1000000000ull;
                if (have_input && !warned++)
                    cham_log("input: dropping input until KWin's Wayland socket is reachable");
            }
        }
        if (!g_display)
            continue;
        warned = 0;
        for (size_t i = 0; have_input && i < (size_t)got / sizeof batch[0]; i++)
            if (batch[i].type == CHAM_INPUT)
                forward(&batch[i]);
        output_tick();
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

/* CHAM_INPUT and CHAM_CONFIG messages, from the presenter link's thread. */
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
