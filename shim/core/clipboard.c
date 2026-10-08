/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Clipboard sharing with Android (the app's "Clipboard sharing" setting).
 *
 * On the input thread's connection to KWin (input.c), the shim takes part in
 * the desktop's clipboard like a clipboard manager, through
 * ext_data_control_v1, which KWin offers to any client. Only pipes pass
 * through here, never the text:
 *  - the desktop copies text: the copying app is asked to write it into a
 *    pipe whose read end goes to the Chameleon app (CHAM_CLIPBOARD_DATA),
 *    which puts it on Android's clipboard;
 *  - Android's clipboard has new text (CHAM_CLIPBOARD_OFFER): the shim
 *    becomes the desktop's clipboard owner, and each paste's pipe goes to the
 *    app (CHAM_CLIPBOARD_REQUEST), which writes the text into it.
 * The shim's own offers carry a private type, so they don't go back to
 * Android. Passwords carry KDE's password manager hint on the desktop and
 * Android's "sensitive" flag on Android, either way, so neither side shows
 * them or keeps them in its clipboard history. Primary selections (middle-click paste) are left alone.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../common/chameleon_proto.h"
#include "internal.h"
#include "wlclient.h"

/* ---- protocol definitions: ext-data-control-v1 ---- */

static const struct wl_interface k_dc_source_iface, k_dc_device_iface, k_dc_offer_iface;
static const struct wl_interface *k_create_source_types[] = {&k_dc_source_iface};
static const struct wl_interface *k_get_device_types[] = {&k_dc_device_iface, NULL /* wl_seat */};
static const struct wl_interface *k_source_types[] = {&k_dc_source_iface};
static const struct wl_interface *k_offer_types[] = {&k_dc_offer_iface};

static const struct wl_message k_dc_manager_requests[] = {
    {"create_data_source", "n", k_create_source_types},
    {"get_data_device", "no", k_get_device_types},
    {"destroy", "", k_null},
};
static const struct wl_interface k_dc_manager_iface = {
    "ext_data_control_manager_v1", 1, 3, k_dc_manager_requests, 0, NULL,
};
static const struct wl_message k_dc_device_requests[] = {
    {"set_selection", "?o", k_source_types},
    {"destroy", "", k_null},
    {"set_primary_selection", "?o", k_source_types},
};
static const struct wl_message k_dc_device_events[] = {
    {"data_offer", "n", k_offer_types},
    {"selection", "?o", k_offer_types},
    {"finished", "", k_null},
    {"primary_selection", "?o", k_offer_types},
};
static const struct wl_interface k_dc_device_iface = {
    "ext_data_control_device_v1", 1, 3, k_dc_device_requests, 4, k_dc_device_events,
};
static const struct wl_message k_dc_source_requests[] = {{"offer", "s", k_null}, {"destroy", "", k_null}};
static const struct wl_message k_dc_source_events[] = {{"send", "sh", k_null}, {"cancelled", "", k_null}};
static const struct wl_interface k_dc_source_iface = {
    "ext_data_control_source_v1", 1, 2, k_dc_source_requests, 2, k_dc_source_events,
};
static const struct wl_message k_dc_offer_requests[] = {{"receive", "sh", k_null}, {"destroy", "", k_null}};
static const struct wl_message k_dc_offer_events[] = {{"offer", "s", k_null}};
static const struct wl_interface k_dc_offer_iface = {
    "ext_data_control_offer_v1", 1, 2, k_dc_offer_requests, 1, k_dc_offer_events,
};

/* Text types, best first; what the shim offers and accepts. */
static const char *const k_text_types[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT",
                                           "STRING"};
#define N_TEXT_TYPES (sizeof k_text_types / sizeof k_text_types[0])
/* Marks the shim's own offers. */
static const char k_ours[] = "application/x-chameleon-android";
/* Set by password managers (KeePassXC, KDE's) on passwords they copy, with
 * the value "secret"; Klipper leaves those out of its history. */
static const char k_secret[] = "x-kde-passwordManagerHint";

/* ---- state (input thread only) ---- */

struct offer {
    struct wl_proxy *proxy;
    int text; /* 1 + index of the best text type offered, 0 for none */
    int ours;
    int secret;
};

static struct wl_proxy *g_manager, *g_seat, *g_device, *g_source;
static struct offer *g_selection;

static void offer_free(struct offer *o)
{
    if (!o)
        return;
    wl.marshal_flags(o->proxy, 1 /* destroy */, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
    free(o);
}

static void offer_type(void *data, struct wl_proxy *proxy, const char *mime)
{
    (void)proxy;
    struct offer *o = data;
    if (strcmp(mime, k_ours) == 0)
        o->ours = 1;
    else if (strcmp(mime, k_secret) == 0)
        o->secret = 1;
    for (size_t i = 0; i < N_TEXT_TYPES; i++)
        if (strcmp(mime, k_text_types[i]) == 0 && (!o->text || (int)i + 1 < o->text))
            o->text = (int)i + 1;
}

static void (*k_offer_listener[])(void) = {(void (*)(void))offer_type};

/* ---- the desktop's clipboard -> Android ---- */

static void device_data_offer(void *data, struct wl_proxy *device, struct wl_proxy *proxy)
{
    (void)data;
    (void)device;
    struct offer *o = calloc(1, sizeof *o);
    if (!o) {
        wl.marshal_flags(proxy, 1 /* destroy */, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
        return;
    }
    o->proxy = proxy;
    wl.add_listener(proxy, k_offer_listener, o);
}

static void device_selection(void *data, struct wl_proxy *device, struct wl_proxy *proxy)
{
    (void)data;
    (void)device;
    struct offer *o = proxy ? wl.get_user_data(proxy) : NULL;
    if (g_selection != o)
        offer_free(g_selection);
    g_selection = o;
    if (!o || !o->text || o->ours)
        return;
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0)
        return;
    /* libwayland sends a duplicate of p[1]. */
    wl.marshal_flags(o->proxy, 0 /* receive */, NULL, 1, 0, k_text_types[o->text - 1], p[1]);
    close(p[1]);
    link_clipboard(CHAM_CLIPBOARD_DATA, o->secret, p[0]);
    close(p[0]);
}

static void device_finished(void *data, struct wl_proxy *device)
{
    (void)data;
    (void)device;
    wl.marshal_flags(g_device, 1 /* destroy */, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
    g_device = NULL;
}

static void device_primary_selection(void *data, struct wl_proxy *device, struct wl_proxy *proxy)
{
    (void)data;
    (void)device;
    if (proxy)
        offer_free(wl.get_user_data(proxy));
}

static void (*k_device_listener[])(void) = {
    (void (*)(void))device_data_offer,
    (void (*)(void))device_selection,
    (void (*)(void))device_finished,
    (void (*)(void))device_primary_selection,
};

/* ---- Android's clipboard -> the desktop ---- */

static void source_send(void *data, struct wl_proxy *source, const char *mime, int32_t fd)
{
    (void)data;
    if (strcmp(mime, k_secret) == 0)
        write(fd, "secret", 6); /* fits the pipe's buffer: doesn't block */
    else if (source == g_source && strcmp(mime, k_ours) != 0)
        link_clipboard(CHAM_CLIPBOARD_REQUEST, 0, fd);
    close(fd);
}

static void source_cancelled(void *data, struct wl_proxy *source)
{
    (void)data;
    if (source == g_source)
        g_source = NULL;
    wl.marshal_flags(source, 1 /* destroy */, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
}

static void (*k_source_listener[])(void) = {
    (void (*)(void))source_send,
    (void (*)(void))source_cancelled,
};

void clipboard_offer(int secret)
{
    if (!g_device)
        return;
    /* The previous source is cancelled by the new selection. */
    g_source = wl.marshal_flags(g_manager, 0 /* create_data_source */, &k_dc_source_iface, 1, 0, NULL);
    if (!g_source)
        return;
    wl.add_listener(g_source, k_source_listener, NULL);
    for (size_t i = 0; i < N_TEXT_TYPES; i++)
        wl.marshal_flags(g_source, 0 /* offer */, NULL, 1, 0, k_text_types[i]);
    wl.marshal_flags(g_source, 0 /* offer */, NULL, 1, 0, k_ours);
    if (secret)
        wl.marshal_flags(g_source, 0 /* offer */, NULL, 1, 0, k_secret);
    wl.marshal_flags(g_device, 0 /* set_selection */, NULL, 1, 0, g_source);
}

/* ---- connection ---- */

void clipboard_global(struct wl_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    (void)version;
    if (strcmp(iface, "ext_data_control_manager_v1") == 0 && !g_manager)
        g_manager = wl_bind(registry, name, &k_dc_manager_iface, 1);
    else if (strcmp(iface, "wl_seat") == 0 && !g_seat && wl.seat_iface)
        g_seat = wl_bind(registry, name, wl.seat_iface, 1); /* no listener: its events are dropped */
}

void clipboard_connected(void)
{
    if (!g_manager || !g_seat) {
        cham_log("clipboard: KWin offers no %s; no clipboard sharing",
                 g_manager ? "seat" : "ext_data_control_manager_v1");
        return;
    }
    k_get_device_types[1] = wl.seat_iface;
    g_device = wl.marshal_flags(g_manager, 1 /* get_data_device */, &k_dc_device_iface, 1, 0, NULL, g_seat);
    if (g_device)
        wl.add_listener(g_device, k_device_listener, NULL);
}

/* The connection is gone, and its proxies with it. */
void clipboard_disconnected(void)
{
    free(g_selection);
    g_selection = NULL;
    g_manager = g_seat = g_device = g_source = NULL;
}
