/*
 * The slice of libwayland-client's ABI used by the shim's own Wayland client
 * connection to KWin (input.c owns the connection and its thread, output.c
 * uses it too). libwayland-client is dlopen()ed, and protocol tables are
 * written out by hand, so nothing is needed at build time.
 */
#ifndef CHAM_WLCLIENT_H
#define CHAM_WLCLIENT_H

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
    const struct wl_interface *registry_iface, *output_iface;
};
extern struct cham_wl wl;

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
