/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chameleon-ime: the Android keyboard as a Linux on-screen keyboard. The
 * compositor (KWin, Weston) starts it as its input method, with a private
 * Wayland connection (WAYLAND_SOCKET), and through zwp_input_method_v1 gives
 * it a context while the focused app has a text field that takes text
 * (text-input). The keyboard's text reaches it from libchameleon.so over
 * Chameleon's input socket and goes into that context: the word being
 * composed, committed text, deletions. It tells libchameleon.so whether a
 * text field takes text; while none does, libchameleon.so types the text as
 * keys instead.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../../common/chameleon_evdev.h"
#include "../core/wlclient.h"

struct cham_wl wl;
const struct wl_interface *const k_wl_null[8];

void wl_ignore_event(void)
{
}

#define LOG(...) fprintf(stderr, "chameleon-ime: " __VA_ARGS__)

/* ---- zwp_input_method_v1 / zwp_input_method_context_v1 (input-method-unstable-v1.xml) ---- */

static const struct wl_interface k_context_iface;
static const struct wl_interface *k_context_types[] = {&k_context_iface};
static const struct wl_message k_im_events[] = {
    {"activate", "n", k_context_types},
    {"deactivate", "o", k_context_types},
};
static const struct wl_interface k_im_iface = {"zwp_input_method_v1", 1, 0, NULL, 2, k_im_events};

enum {
    CTX_DESTROY = 0,
    CTX_COMMIT_STRING = 1,
    CTX_PREEDIT_STRING = 2,
    CTX_PREEDIT_CURSOR = 4,
    CTX_DELETE_SURROUNDING_TEXT = 5,
};
static const struct wl_message k_context_requests[] = {
    {"destroy", "", k_null},
    {"commit_string", "us", k_null},
    {"preedit_string", "uss", k_null},
    {"preedit_styling", "uuu", k_null},
    {"preedit_cursor", "i", k_null},
    {"delete_surrounding_text", "iu", k_null},
    {"cursor_position", "ii", k_null},
    {"modifiers_map", "a", k_null},
    {"keysym", "uuuuu", k_null},
    {"grab_keyboard", "n", k_null}, /* never sent */
    {"key", "uuuu", k_null},
    {"modifiers", "uuuuu", k_null},
    {"language", "us", k_null},
    {"text_direction", "uu", k_null},
};
static const struct wl_message k_context_events[] = {
    {"surrounding_text", "suu", k_null},
    {"reset", "", k_null},
    {"content_type", "uu", k_null},
    {"invoke_button", "uu", k_null},
    {"commit_state", "u", k_null},
    {"preferred_language", "s", k_null},
};
static const struct wl_interface k_context_iface = {
    "zwp_input_method_context_v1", 1, 14, k_context_requests, 6, k_context_events,
};

/* ---- state ---- */

static struct wl_display *g_display;
static struct wl_proxy *g_registry, *g_im, *g_ctx;
static uint32_t g_serial;          /* the context's last commit_state */
static char *g_surrounding;        /* the text field's text around the cursor, if it says */
static uint32_t g_cursor;          /* byte offset of the cursor in it */
static int g_shim = -1;            /* connection to libchameleon.so */

/* ---- libchameleon.so ---- */

static void tell_shim(void)
{
    struct {
        struct cham_evdev_record h;
        uint32_t active;
    } r = {{CHAM_EVDEV_IME_ACTIVE, sizeof r.active}, g_ctx != NULL};
    if (g_shim >= 0 && send(g_shim, &r, sizeof r, MSG_NOSIGNAL) != (ssize_t)sizeof r) {
        close(g_shim);
        g_shim = -1;
    }
}

static int read_all(int fd, void *buf, size_t len)
{
    return recv(fd, buf, len, MSG_WAITALL) == (ssize_t)len;
}

/* The input socket: skip the device list, say who we are. */
static void connect_shim(void)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", dir ? dir : "", CHAM_EVDEV_SOCKET_NAME);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    uint32_t count = 0;
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || !read_all(fd, &count, sizeof count) ||
        count > CHAM_EVDEV_MAX_DEVICES) {
        if (fd >= 0)
            close(fd);
        return;
    }
    static struct cham_evdev_info info;
    for (uint32_t i = 0; i < count; i++) {
        if (!read_all(fd, &info, sizeof info)) {
            close(fd);
            return;
        }
    }
    uint32_t index = CHAM_EVDEV_IME;
    if (send(fd, &index, sizeof index, MSG_NOSIGNAL) != (ssize_t)sizeof index) {
        close(fd);
        return;
    }
    g_shim = fd;
    LOG("connected; the Android keyboard's text goes to text fields\n");
    tell_shim();
}

/* Bytes taken by `n` code points before (dir < 0) or after the cursor in the
 * surrounding text; without it, as if the text were ASCII. */
static uint32_t bytes_for(uint32_t n, int dir)
{
    if (!g_surrounding)
        return n;
    const unsigned char *t = (const unsigned char *)g_surrounding;
    size_t len = strlen(g_surrounding), at = g_cursor < len ? g_cursor : len, end = at;
    for (; n > 0; n--) {
        if (dir < 0 && end > 0) {
            do
                end--;
            while (end > 0 && (t[end] & 0xc0) == 0x80); /* back over continuation bytes */
        } else if (dir > 0 && end < len) {
            do
                end++;
            while (end < len && (t[end] & 0xc0) == 0x80);
        }
    }
    return (uint32_t)(dir < 0 ? at - end : end - at);
}

static void shim_readable(void)
{
    struct cham_evdev_record h;
    static char buf[sizeof(struct cham_ime_text) + 4096 + 1];
    if (!read_all(g_shim, &h, sizeof h) || h.size >= sizeof buf || !read_all(g_shim, buf, h.size)) {
        LOG("lost libchameleon.so\n");
        close(g_shim);
        g_shim = -1;
        return;
    }
    if (h.type != CHAM_EVDEV_IME_TEXT || h.size < sizeof(struct cham_ime_text) || !g_ctx)
        return;
    struct cham_ime_text t;
    memcpy(&t, buf, sizeof t);
    char *text = buf + sizeof t;
    text[h.size - sizeof t] = 0;
    switch (t.kind) {
    case CHAM_IME_COMMIT:
        wl.marshal_flags(g_ctx, CTX_COMMIT_STRING, NULL, 1, 0, g_serial, text);
        break;
    case CHAM_IME_PREEDIT:
        wl.marshal_flags(g_ctx, CTX_PREEDIT_CURSOR, NULL, 1, 0, (int32_t)strlen(text));
        wl.marshal_flags(g_ctx, CTX_PREEDIT_STRING, NULL, 1, 0, g_serial, text, text);
        break;
    case CHAM_IME_DELETE: {
        uint32_t before = bytes_for(t.before, -1), after = bytes_for(t.after, 1);
        wl.marshal_flags(g_ctx, CTX_DELETE_SURROUNDING_TEXT, NULL, 1, 0, -(int32_t)before, before + after);
        break;
    }
    }
}

/* ---- the input method ---- */

static void ctx_surrounding_text(void *data, struct wl_proxy *ctx, const char *text, uint32_t cursor, uint32_t anchor)
{
    (void)data;
    (void)ctx;
    (void)anchor;
    free(g_surrounding);
    g_surrounding = text ? strdup(text) : NULL;
    g_cursor = cursor;
}

static void ctx_commit_state(void *data, struct wl_proxy *ctx, uint32_t serial)
{
    (void)data;
    (void)ctx;
    g_serial = serial;
}

static void (*k_context_listener[])(void) = {
    (void (*)(void))ctx_surrounding_text,
    wl_ignore_event, /* reset */
    wl_ignore_event, /* content_type */
    wl_ignore_event, /* invoke_button */
    (void (*)(void))ctx_commit_state,
    wl_ignore_event, /* preferred_language */
};

static void drop_context(void)
{
    if (g_ctx)
        wl.marshal_flags(g_ctx, CTX_DESTROY, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
    g_ctx = NULL;
    free(g_surrounding);
    g_surrounding = NULL;
}

static void im_activate(void *data, struct wl_proxy *im, struct wl_proxy *ctx)
{
    (void)data;
    (void)im;
    drop_context();
    g_ctx = ctx;
    g_serial = 0;
    wl.add_listener(g_ctx, k_context_listener, NULL);
    tell_shim();
}

static void im_deactivate(void *data, struct wl_proxy *im, struct wl_proxy *ctx)
{
    (void)data;
    (void)im;
    if (ctx == g_ctx) {
        drop_context();
        tell_shim();
    }
}

static void (*k_im_listener[])(void) = {
    (void (*)(void))im_activate,
    (void (*)(void))im_deactivate,
};

static void registry_global(void *data, struct wl_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    (void)data;
    (void)version;
    if (strcmp(iface, "zwp_input_method_v1") == 0 && !g_im) {
        g_im = wl_bind(registry, name, &k_im_iface, 1);
        wl.add_listener(g_im, k_im_listener, NULL);
    }
}

static void (*k_registry_listener[])(void) = {
    (void (*)(void))registry_global,
    wl_ignore_event, /* global_remove */
};

int main(void)
{
    char path[512];
    const char *prefix = getenv("PREFIX");
    snprintf(path, sizeof path, "%s/lib/libwayland-client.so", prefix && *prefix ? prefix : "/data/data/com.termux/files/usr");
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib || !cham_wl_load(lib) || !wl.connect) {
        LOG("cannot load libwayland-client (1.20+)\n");
        return 1;
    }
    g_display = wl.connect(NULL); /* WAYLAND_SOCKET from the compositor */
    if (!g_display) {
        LOG("no Wayland connection (the compositor starts this as its input method)\n");
        return 1;
    }
    g_registry = wl.marshal_flags((struct wl_proxy *)g_display, 1 /* get_registry */, wl.registry_iface,
                                  wl.get_version((struct wl_proxy *)g_display), 0, NULL);
    wl.add_listener(g_registry, k_registry_listener, NULL);
    wl.roundtrip(g_display);
    if (!g_im) {
        LOG("the compositor offers no zwp_input_method_v1 to this connection\n");
        return 1;
    }

    for (;;) {
        if (g_shim < 0)
            connect_shim();
        while (wl.prepare_read(g_display) != 0)
            wl.dispatch_pending(g_display);
        wl.flush(g_display);
        struct pollfd p[2] = {{wl.get_fd(g_display), POLLIN, 0}, {g_shim, POLLIN, 0}};
        int n = poll(p, 2, g_shim < 0 ? 1000 : -1);
        if (n > 0 && (p[0].revents & (POLLIN | POLLERR | POLLHUP)))
            wl.read_events(g_display);
        else
            wl.cancel_read(g_display);
        if (wl.dispatch_pending(g_display) < 0 || wl.get_error(g_display)) {
            LOG("the compositor closed the connection\n");
            return 0;
        }
        if (n > 0 && g_shim >= 0 && (p[1].revents & (POLLIN | POLLERR | POLLHUP)))
            shim_readable();
    }
}
