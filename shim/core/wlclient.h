/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The slice of libwayland-client's ABI used by the shim's own Wayland client
 * connection to the compositor (input.c owns the connection and its thread,
 * clipboard.c uses it too) and by chameleon-ime. libwayland-client is
 * dlopen()ed, and protocol tables are written out by hand, so nothing is
 * needed at build time.
 */
#ifndef CHAM_WLCLIENT_H
#define CHAM_WLCLIENT_H

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

struct wl_message {
    const char *name;
    const char *signature;
    const struct wl_interface **types;
};
struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};
struct wl_proxy;
struct wl_display;

#define WL_MARSHAL_FLAG_DESTROY (1 << 0)

struct cham_wl {
    struct wl_display *(*connect)(const char *);
    struct wl_display *(*connect_to_fd)(int);
    void (*disconnect)(struct wl_display *);
    int (*roundtrip)(struct wl_display *);
    int (*flush)(struct wl_display *);
    int (*get_fd)(struct wl_display *);
    int (*prepare_read)(struct wl_display *);
    int (*read_events)(struct wl_display *);
    void (*cancel_read)(struct wl_display *);
    int (*dispatch_pending)(struct wl_display *);
    int (*get_error)(struct wl_display *);
    struct wl_proxy *(*marshal_flags)(struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t,
                                      ...);
    int (*add_listener)(struct wl_proxy *, void (**)(void), void *);
    uint32_t (*get_version)(struct wl_proxy *);
    void *(*get_user_data)(struct wl_proxy *);
    void (*proxy_destroy)(struct wl_proxy *);
    const struct wl_interface *registry_iface, *seat_iface, *keyboard_iface;
};
extern struct cham_wl wl;

/* Fills `wl` from a dlopen()ed libwayland-client; 0 if it is too old (1.20+
 * has wl_proxy_marshal_flags). */
static inline int cham_wl_load(void *lib)
{
#define W(field, name) (*(void **)&wl.field = dlsym(lib, name))
    W(connect, "wl_display_connect");
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
    W(seat_iface, "wl_seat_interface");
    W(keyboard_iface, "wl_keyboard_interface");
#undef W
    return wl.connect_to_fd && wl.marshal_flags && wl.add_listener && wl.registry_iface &&
           wl.prepare_read && wl.get_version && wl.get_user_data && wl.proxy_destroy;
}

/* An all-NULL types array for messages without object arguments. */
extern const struct wl_interface *const k_wl_null[8];
#define k_null ((const struct wl_interface **)k_wl_null)

/* wl_registry.bind */
static inline struct wl_proxy *wl_bind(struct wl_proxy *registry, uint32_t name, const struct wl_interface *iface,
                                       uint32_t version)
{
    return wl.marshal_flags(registry, 0 /* bind */, iface, version, 0, name, iface->name, version, NULL);
}

typedef int32_t wl_fixed_t;

/* A listener slot for events we don't care about. libwayland calls it with
 * the event's arguments, which it ignores (fine with the C calling
 * conventions of every ABI Android runs on). */
void wl_ignore_event(void);

#endif
