/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A Wayland window fed by Android's GPU driver, for the EGL vendor's Wayland
 * platform (wayland.c) and the Vulkan driver's windows (vulkan/wsi.c).
 *
 * Android's drivers only render into ANativeWindows, and an AImageReader is
 * one a process can create for itself. So a window is an AImageReader whose
 * ANativeWindow the driver draws on (an EGL window surface, a Vulkan
 * swapchain). Each finished frame is taken out of the reader as an
 * AHardwareBuffer and shown on the app's wl_surface:
 *
 *   1. shared with the session as a GEM handle on this process's render
 *      node, once per buffer, because the compositor can't turn a bare
 *      dmabuf back into an AHB (chameleon-kernel hands it the AHB);
 *   2. wrapped in a zwp_linux_dmabuf_v1 wl_buffer (its gralloc dmabuf);
 *   3. attached, damaged and committed on the app's wl_surface.
 *
 * wl_buffer.release gives the image back to the reader, and frame callbacks
 * pace the frames of a caller that asks for them. All Wayland objects of
 * ours live on a private event queue, so the app's own event handling is
 * not disturbed. A window isn't locked here: its caller serializes.
 *
 * An X11 window (for the Vulkan driver's xcb and Xlib surfaces) is fed the
 * same way; its frames go to the X server instead, the way Mesa's drivers
 * show theirs: each buffer is a DRI3 pixmap (Xwayland's gbm, ours, finds
 * the session's buffer behind the dmabuf), shown with PresentPixmap;
 * IdleNotify gives a buffer back to the reader and CompleteNotify paces
 * FIFO. Present's events come on a queue of ours (xcb's special events).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include "../core/cham_shim.h"
#include "vendor.h"
#include "window.h"

/* ---- Android: AHardwareBuffer + AImageReader (dlopen) ---- */

typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} ahb_desc;
typedef struct {
    int version, numFds, numInts;
    int data[];
} native_handle;
typedef struct AImageReader AImageReader;
typedef struct AImage AImage;
typedef struct ANativeWindow ANativeWindow;

#define AHB_FORMAT_R8G8B8A8 1
#define AHB_FORMAT_R8G8B8X8 2
#define AHB_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)
#define AMEDIA_OK 0
#define AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE (-30001)
#define AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED (-30002)

static struct {
    void (*describe)(const AHardwareBuffer *, ahb_desc *);
    const native_handle *(*get_native_handle)(const AHardwareBuffer *);
    void (*acquire)(AHardwareBuffer *);
    void (*release)(AHardwareBuffer *);
    int (*set_geometry)(ANativeWindow *, int32_t, int32_t, int32_t);
    int (*reader_new)(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **);
    int (*reader_get_window)(AImageReader *, ANativeWindow **);
    int (*reader_acquire_next)(AImageReader *, AImage **, int *);
    void (*reader_delete)(AImageReader *);
    int (*image_get_ahb)(const AImage *, AHardwareBuffer **);
    void (*image_delete)(AImage *);
} android;

static void *open_system_lib(const char *env, const char *name)
{
    const char *path = getenv(env); /* stand-ins for the host tests */
    char buf[128];
    if (!path) {
        snprintf(buf, sizeof buf, "%s/%s", sizeof(void *) == 8 ? "/system/lib64" : "/system/lib", name);
        path = buf;
    }
    return cham_android_dlopen(path);
}

static int load_android(void)
{
    static int loaded = -1;
    if (loaded >= 0)
        return loaded;
    void *nw = open_system_lib("CHAMELEON_ANDROID_NATIVEWINDOW", "libnativewindow.so");
    void *media = open_system_lib("CHAMELEON_ANDROID_MEDIANDK", "libmediandk.so");
#define A(lib, field, name) (*(void **)&android.field = lib ? dlsym(lib, name) : NULL)
    A(nw, describe, "AHardwareBuffer_describe");
    A(nw, get_native_handle, "AHardwareBuffer_getNativeHandle");
    A(nw, acquire, "AHardwareBuffer_acquire");
    A(nw, release, "AHardwareBuffer_release");
    A(nw, set_geometry, "ANativeWindow_setBuffersGeometry");
    A(media, reader_new, "AImageReader_newWithUsage");
    A(media, reader_get_window, "AImageReader_getWindow");
    A(media, reader_acquire_next, "AImageReader_acquireNextImageAsync");
    A(media, reader_delete, "AImageReader_delete");
    A(media, image_get_ahb, "AImage_getHardwareBuffer");
    A(media, image_delete, "AImage_delete");
#undef A
    loaded = android.describe && android.get_native_handle && android.acquire && android.release &&
             android.set_geometry &&
             android.reader_new &&
             android.reader_get_window && android.reader_acquire_next && android.reader_delete &&
             android.image_get_ahb && android.image_delete;
    if (!loaded)
        cham_log("wayland: AImageReader/AHardwareBuffer unavailable; Wayland windows left to other vendors");
    return loaded;
}

/* ---- libwayland-client (dlopen) and the protocol bits we use ---- */

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
struct wl_event_queue;

#define WL_MARSHAL_FLAG_DESTROY 1

static struct {
    struct wl_proxy *(*marshal_flags)(struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t,
                                      ...);
    int (*add_listener)(struct wl_proxy *, void (**)(void), void *);
    uint32_t (*get_version)(struct wl_proxy *);
    void *(*create_wrapper)(void *);
    void (*wrapper_destroy)(void *);
    struct wl_display *(*proxy_get_display)(struct wl_proxy *); /* 1.23+, optional */
    void (*set_queue)(struct wl_proxy *, struct wl_event_queue *);
    void (*proxy_destroy)(struct wl_proxy *);
    struct wl_event_queue *(*create_queue)(struct wl_display *);
    void (*queue_destroy)(struct wl_event_queue *);
    int (*roundtrip_queue)(struct wl_display *, struct wl_event_queue *);
    int (*dispatch_queue)(struct wl_display *, struct wl_event_queue *);
    int (*flush)(struct wl_display *);
    int (*get_error)(struct wl_display *);
    const struct wl_interface *registry_iface, *buffer_iface, *callback_iface;
} wl;

static int load_wayland(void)
{
    static int loaded = -1;
    if (loaded >= 0)
        return loaded;
    /* Usually loaded by the app already; any copy of the same library will
     * do. A program that loads it only once it knows it has a Wayland window
     * (vkcube) asks for the window extensions first: then by its path, which
     * Termux's own programs find by their RUNPATH and this library has none. */
    void *lib = dlopen("libwayland-client.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib)
        lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) {
        char path[512];
        const char *prefix = getenv("PREFIX");
        snprintf(path, sizeof path, "%s/lib/libwayland-client.so",
                 prefix && *prefix ? prefix : "/data/data/com.termux/files/usr");
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!lib)
        lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
#define W(field, name) (*(void **)&wl.field = lib ? dlsym(lib, name) : NULL)
    W(marshal_flags, "wl_proxy_marshal_flags");
    W(add_listener, "wl_proxy_add_listener");
    W(get_version, "wl_proxy_get_version");
    W(create_wrapper, "wl_proxy_create_wrapper");
    W(wrapper_destroy, "wl_proxy_wrapper_destroy");
    W(proxy_get_display, "wl_proxy_get_display");
    W(set_queue, "wl_proxy_set_queue");
    W(proxy_destroy, "wl_proxy_destroy");
    W(create_queue, "wl_display_create_queue");
    W(queue_destroy, "wl_event_queue_destroy");
    W(roundtrip_queue, "wl_display_roundtrip_queue");
    W(dispatch_queue, "wl_display_dispatch_queue");
    W(flush, "wl_display_flush");
    W(get_error, "wl_display_get_error");
    W(registry_iface, "wl_registry_interface");
    W(buffer_iface, "wl_buffer_interface");
    W(callback_iface, "wl_callback_interface");
#undef W
    loaded = wl.marshal_flags && wl.add_listener && wl.get_version && wl.create_wrapper && wl.wrapper_destroy &&
             wl.set_queue && wl.proxy_destroy && wl.create_queue && wl.queue_destroy && wl.roundtrip_queue &&
             wl.dispatch_queue && wl.flush && wl.get_error && wl.registry_iface && wl.buffer_iface &&
             wl.callback_iface;
    if (!loaded)
        cham_log("wayland: libwayland-client 1.20+ not found");
    return loaded;
}

/* ---- libxcb with DRI3 and Present (dlopen), for X11 windows ----
 * Declared here as the protocol defines them; the X11 libraries aren't part
 * of the build. */

typedef struct xcb_connection_t xcb_connection_t;
typedef struct xcb_special_event xcb_special_event_t;
typedef struct xcb_extension_t xcb_extension_t;
typedef struct {
    unsigned int sequence;
} xcb_cookie_t;
typedef struct {
    uint8_t response_type, error_code;
    uint16_t sequence;
    uint32_t resource_id;
    uint16_t minor_code;
    uint8_t major_code, pad0;
    uint32_t pad[5];
    uint32_t full_sequence;
} xcb_generic_error_t;
typedef struct {
    uint8_t response_type, depth;
    uint16_t sequence;
    uint32_t length, root;
    int16_t x, y;
    uint16_t width, height, border_width;
    uint8_t pad0[2];
} xcb_get_geometry_reply_t;
typedef struct {
    uint8_t response_type, pad0;
    uint16_t sequence;
    uint32_t length;
    uint8_t present, major_opcode, first_event, first_error;
} xcb_query_extension_reply_t;
typedef struct { /* DRI3's and Present's QueryVersion replies alike */
    uint8_t response_type, pad0;
    uint16_t sequence;
    uint32_t length, major_version, minor_version;
} xcb_version_reply_t;

#define PRESENT_EVENT_CONFIGURE_NOTIFY 0
#define PRESENT_EVENT_COMPLETE_NOTIFY 1
#define PRESENT_EVENT_IDLE_NOTIFY 2
#define PRESENT_EVENT_MASK_ALL 7 /* configure, complete, idle */
#define PRESENT_COMPLETE_KIND_PIXMAP 0

static struct {
    uint32_t (*generate_id)(xcb_connection_t *);
    int (*flush)(xcb_connection_t *);
    int (*has_error)(xcb_connection_t *);
    xcb_cookie_t (*get_geometry)(xcb_connection_t *, uint32_t);
    xcb_get_geometry_reply_t *(*get_geometry_reply)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **);
    xcb_cookie_t (*free_pixmap)(xcb_connection_t *, uint32_t);
    const xcb_query_extension_reply_t *(*get_extension_data)(xcb_connection_t *, xcb_extension_t *);
    xcb_special_event_t *(*register_special)(xcb_connection_t *, xcb_extension_t *, uint32_t, uint32_t *);
    void (*unregister_special)(xcb_connection_t *, xcb_special_event_t *);
    void *(*wait_special)(xcb_connection_t *, xcb_special_event_t *);
    void *(*poll_special)(xcb_connection_t *, xcb_special_event_t *);
    xcb_generic_error_t *(*request_check)(xcb_connection_t *, xcb_cookie_t);
    void (*discard_reply)(xcb_connection_t *, unsigned int);
    xcb_extension_t *dri3_id;
    xcb_cookie_t (*dri3_query_version)(xcb_connection_t *, uint32_t, uint32_t);
    xcb_version_reply_t *(*dri3_query_version_reply)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **);
    xcb_cookie_t (*dri3_pixmap_from_buffer)(xcb_connection_t *, uint32_t, uint32_t, uint32_t, uint16_t, uint16_t,
                                            uint16_t, uint8_t, uint8_t, int32_t);
    xcb_extension_t *present_id;
    xcb_cookie_t (*present_query_version)(xcb_connection_t *, uint32_t, uint32_t);
    xcb_version_reply_t *(*present_query_version_reply)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **);
    xcb_cookie_t (*present_select_input)(xcb_connection_t *, uint32_t, uint32_t, uint32_t);
    xcb_cookie_t (*present_pixmap)(xcb_connection_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int16_t,
                                   int16_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t,
                                   uint32_t, const void *);
} xcb;

/* A library of the program's (already loaded), else Termux's by its path,
 * else by name (the host's). */
static void *open_x11_lib(const char *name, const char *versioned)
{
    void *lib = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    if (!lib)
        lib = dlopen(versioned, RTLD_NOW | RTLD_NOLOAD);
    if (!lib) {
        char path[512];
        const char *prefix = getenv("PREFIX");
        snprintf(path, sizeof path, "%s/lib/%s", prefix && *prefix ? prefix : "/data/data/com.termux/files/usr",
                 name);
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!lib)
        lib = dlopen(versioned, RTLD_NOW | RTLD_LOCAL);
    return lib;
}

static int load_xcb(void)
{
    static int loaded = -1;
    if (loaded >= 0)
        return loaded;
    void *core = open_x11_lib("libxcb.so", "libxcb.so.1");
    void *dri3 = open_x11_lib("libxcb-dri3.so", "libxcb-dri3.so.0");
    void *present = open_x11_lib("libxcb-present.so", "libxcb-present.so.0");
#define X(lib, field, name) (*(void **)&xcb.field = lib ? dlsym(lib, name) : NULL)
    X(core, generate_id, "xcb_generate_id");
    X(core, flush, "xcb_flush");
    X(core, has_error, "xcb_connection_has_error");
    X(core, get_geometry, "xcb_get_geometry");
    X(core, get_geometry_reply, "xcb_get_geometry_reply");
    X(core, free_pixmap, "xcb_free_pixmap");
    X(core, get_extension_data, "xcb_get_extension_data");
    X(core, register_special, "xcb_register_for_special_xge");
    X(core, unregister_special, "xcb_unregister_for_special_event");
    X(core, wait_special, "xcb_wait_for_special_event");
    X(core, poll_special, "xcb_poll_for_special_event");
    X(core, request_check, "xcb_request_check");
    X(core, discard_reply, "xcb_discard_reply");
    X(dri3, dri3_id, "xcb_dri3_id");
    X(dri3, dri3_query_version, "xcb_dri3_query_version");
    X(dri3, dri3_query_version_reply, "xcb_dri3_query_version_reply");
    X(dri3, dri3_pixmap_from_buffer, "xcb_dri3_pixmap_from_buffer_checked");
    X(present, present_id, "xcb_present_id");
    X(present, present_query_version, "xcb_present_query_version");
    X(present, present_query_version_reply, "xcb_present_query_version_reply");
    X(present, present_select_input, "xcb_present_select_input_checked");
    X(present, present_pixmap, "xcb_present_pixmap_checked");
#undef X
    loaded = xcb.generate_id && xcb.flush && xcb.has_error && xcb.get_geometry && xcb.get_geometry_reply &&
             xcb.free_pixmap && xcb.get_extension_data && xcb.register_special && xcb.unregister_special &&
             xcb.wait_special && xcb.poll_special && xcb.request_check && xcb.discard_reply && xcb.dri3_id && xcb.dri3_query_version &&
             xcb.dri3_query_version_reply && xcb.dri3_pixmap_from_buffer && xcb.present_id &&
             xcb.present_query_version && xcb.present_query_version_reply && xcb.present_select_input &&
             xcb.present_pixmap;
    if (!loaded)
        cham_log("x11: libxcb with DRI3 and Present not found; no X11 windows");
    return loaded;
}

static const struct wl_interface *k_null[8];

/* zwp_linux_dmabuf_v1 (bound at version 3) / zwp_linux_buffer_params_v1 */
static const struct wl_interface k_params_iface;
static const struct wl_interface *k_create_params_types[] = {&k_params_iface};
static const struct wl_message k_dmabuf_requests[] = {
    {"destroy", "", k_null},
    {"create_params", "n", k_create_params_types},
};
static const struct wl_message k_dmabuf_events[] = {
    {"format", "u", k_null},
    {"modifier", "3uuu", k_null},
};
static const struct wl_interface k_dmabuf_iface = {
    "zwp_linux_dmabuf_v1", 3, 2, k_dmabuf_requests, 2, k_dmabuf_events,
};
static const struct wl_interface *k_create_immed_types[] = {NULL /* wl_buffer */, NULL, NULL, NULL, NULL};
static const struct wl_interface *k_created_types[] = {NULL /* wl_buffer */};
static const struct wl_message k_params_requests[] = {
    {"destroy", "", k_null},
    {"add", "huuuuu", k_null},
    {"create", "iiuu", k_null},
    {"create_immed", "2niiuu", k_create_immed_types},
};
static const struct wl_message k_params_events[] = {
    {"created", "n", k_created_types},
    {"failed", "", k_null},
};
static const struct wl_interface k_params_iface = {
    "zwp_linux_buffer_params_v1", 3, 4, k_params_requests, 2, k_params_events,
};

enum { PARAMS_DESTROY = 0, PARAMS_ADD = 1, PARAMS_CREATE_IMMED = 3 };
enum {
    SURFACE_ATTACH = 1,
    SURFACE_DAMAGE = 2,
    SURFACE_FRAME = 3,
    SURFACE_COMMIT = 6,
    SURFACE_DAMAGE_BUFFER = 9,
    SURFACE_OFFSET = 10,
};

/* ---- the connection to the compositor ---- */

struct display {
    struct wl_display *wl;
    struct wl_event_queue *queue;
    struct wl_proxy *dmabuf;
    struct display *next;
};

static pthread_mutex_t g_displays_lock = PTHREAD_MUTEX_INITIALIZER;
static struct display *g_displays;

/* Buffers can be shared with the session and Android draws on readers. */
static int base_available(void)
{
    static int available = -1;
    if (available < 0)
        available = cham_bo_sharing() && cham_render_fd() >= 0 && load_android();
    return available;
}

int cham_window_available(void)
{
    return base_available() && load_wayland();
}

int cham_window_x11_available(void)
{
    return base_available() && load_xcb();
}

static void registry_global(void *data, struct wl_proxy *registry, uint32_t name, const char *iface,
                            uint32_t version)
{
    struct display *d = data;
    if (strcmp(iface, "zwp_linux_dmabuf_v1") == 0 && !d->dmabuf && version >= 2) {
        uint32_t v = version < 3 ? version : 3;
        d->dmabuf = wl.marshal_flags(registry, 0 /* bind */, &k_dmabuf_iface, v, 0, name, iface, v, NULL);
    }
}

static void registry_global_remove(void *data, struct wl_proxy *registry, uint32_t name)
{
    (void)data, (void)registry, (void)name;
}

static void (*k_registry_listener[])(void) = {
    (void (*)(void))registry_global,
    (void (*)(void))registry_global_remove,
};

/* The connection's entry, made the first time it is seen: a queue of ours
 * and the compositor's zwp_linux_dmabuf_v1. NULL (said once) without one. */
static struct display *display_for(struct wl_display *wd)
{
    pthread_mutex_lock(&g_displays_lock);
    struct display *d = g_displays;
    while (d && d->wl != wd)
        d = d->next;
    if (!d) {
        d = calloc(1, sizeof *d);
        d->wl = wd;
        d->queue = wl.create_queue(wd);
        struct wl_proxy *wrapper = wl.create_wrapper(wd);
        wl.set_queue(wrapper, d->queue);
        struct wl_proxy *registry =
            wl.marshal_flags(wrapper, 1 /* get_registry */, wl.registry_iface, wl.get_version(wrapper), 0, NULL);
        wl.wrapper_destroy(wrapper);
        wl.add_listener(registry, k_registry_listener, d);
        wl.roundtrip_queue(wd, d->queue);
        wl.proxy_destroy(registry);
        if (!d->dmabuf) {
            cham_log("wayland: the compositor offers no zwp_linux_dmabuf_v1; Wayland windows left to other drivers");
            wl.queue_destroy(d->queue);
            free(d);
            d = NULL;
        } else {
            d->next = g_displays;
            g_displays = d;
        }
    }
    pthread_mutex_unlock(&g_displays_lock);
    return d;
}

int cham_window_connect(void *wl_display)
{
    return wl_display && cham_window_available() && display_for(wl_display) ? 0 : -1;
}

/* Shares an AHardwareBuffer (the reader's) with the session: a GEM handle on
 * this process's render node file, so the compositor's dmabuf import finds
 * it, as a driver's buffers are kernel objects. chameleon-kernel keeps it
 * while the handle or its dmabuf is held anywhere. NULL on failure. */
static struct cham_bo *share_buffer(AHardwareBuffer *ahb, const native_handle *nh, const ahb_desc *desc,
                                    uint32_t format, uint32_t *handle)
{
    int fd = cham_render_fd();
    if (fd < 0)
        return NULL;
    android.acquire(ahb); /* the bo's reference */
    struct cham_bo *bo = cham_bo_wrap(ahb, nh, desc->width, desc->height, desc->stride * 4, format, android.release);
    if (!bo) {
        android.release(ahb);
        return NULL;
    }
    *handle = cham_bo_handle_open(bo, fd);
    if (!*handle) {
        cham_bo_unref(bo);
        return NULL;
    }
    return bo;
}

/* ---- windows ---- */

/* How many of a window's frames can be with the compositor (handed over, not
 * released yet) at once. With one fewer, a program drawing faster than the
 * compositor takes frames waits for a release in nearly every frame. */
#define READER_IMAGES 4
/* All the slots of an Android buffer queue (a Vulkan swapchain may ask for
 * as many images), and an older set's buffers still at the compositor. */
#define MAX_BUFFERS (64 + READER_IMAGES)

struct buffer {
    AHardwareBuffer *ahb; /* the reader owns it */
    dev_t dev;
    ino_t ino;            /* its dmabuf: what tells buffers apart */
    int width, height;
    unsigned serial;      /* the window's buffer set when it was seen last */
    uint32_t drm_format;  /* what the compositor was told */
    struct wl_proxy *wl_buffer;
    uint32_t pixmap;      /* an X11 window's: the buffer as a DRI3 pixmap */
    struct cham_bo *bo;   /* shared with the session... */
    uint32_t handle;      /* ...as this GEM handle on the render node */
    AImage *image;        /* held while the compositor uses the buffer */
};

struct cham_window {
    struct display *d;
    struct wl_event_queue *queue; /* this window's events only */
    struct wl_proxy *dmabuf;      /* wrapper on `queue`: its wl_buffers land there */
    struct wl_proxy *wl_surface;  /* wrapper on our queue */
    uint32_t surface_version;
    uint32_t drm_format;
    /* One AImageReader for the window's life, as a SurfaceView's window: a
     * new size asks its window for buffers of that size, and the driver's
     * surface follows from its next buffer. */
    AImageReader *reader;
    ANativeWindow *native;
    unsigned serial; /* counts the buffer sets: sizes, swapchains */
    struct buffer buffers[MAX_BUFFERS];
    struct wl_proxy *frame_callback;
    /* An X11 window instead (d NULL): its frames are its buffers as DRI3
     * pixmaps, shown with Present; Present's events for it come on a queue
     * of ours, not the program's. */
    struct {
        xcb_connection_t *c;
        uint32_t window, eid;
        xcb_special_event_t *events;
        int depth;
        int width, height;   /* as the X server last told */
        uint32_t presented;  /* serial of the last frame presented... */
        uint32_t completed;  /* ...and of the last one the server is done showing */
    } x;
};

static void buffer_free(struct cham_window *s, struct buffer *b)
{
    if (b->wl_buffer)
        wl.marshal_flags(b->wl_buffer, 0 /* destroy */, NULL, wl.get_version(b->wl_buffer), WL_MARSHAL_FLAG_DESTROY);
    if (b->pixmap)
        xcb.free_pixmap(s->x.c, b->pixmap);
    if (b->bo) {
        cham_bo_handle_close(cham_render_fd(), b->handle);
        cham_bo_unref(b->bo);
    }
    if (b->image)
        android.image_delete(b->image);
    memset(b, 0, sizeof *b);
}

static void buffer_release(void *data, struct wl_proxy *wl_buffer)
{
    (void)wl_buffer;
    struct buffer *b = data;
    if (b->image) {
        android.image_delete(b->image);
        b->image = NULL;
    }
}

static void (*k_buffer_listener[])(void) = {(void (*)(void))buffer_release};

static void frame_done(void *data, struct wl_proxy *callback, uint32_t time)
{
    (void)time;
    struct cham_window *s = data;
    if (s->frame_callback == callback)
        s->frame_callback = NULL;
    wl.proxy_destroy(callback);
}

static void (*k_frame_listener[])(void) = {(void (*)(void))frame_done};

struct cham_window *cham_window_create(void *wl_display, void *wl_surface, int width, int height, int opaque)
{
    if (!cham_window_available() || !wl_surface)
        return NULL;
    /* The connection the wl_surface belongs to. Without one named and
     * without wl_proxy_get_display (libwayland < 1.23), the last one seen. */
    struct display *d = NULL;
    if (wl_display || wl.proxy_get_display) {
        d = display_for(wl_display ? wl_display : wl.proxy_get_display(wl_surface));
    } else {
        pthread_mutex_lock(&g_displays_lock);
        d = g_displays;
        pthread_mutex_unlock(&g_displays_lock);
    }
    struct cham_window *s = d ? calloc(1, sizeof *s) : NULL;
    if (!s)
        return NULL;
    s->d = d;
    s->drm_format = opaque ? DRM_FORMAT_XBGR8888 : DRM_FORMAT_ABGR8888;
    if (android.reader_new(width > 0 ? width : 1, height > 0 ? height : 1,
                           opaque ? AHB_FORMAT_R8G8B8X8 : AHB_FORMAT_R8G8B8A8, AHB_USAGE_GPU_SAMPLED_IMAGE,
                           READER_IMAGES, &s->reader) != AMEDIA_OK ||
        android.reader_get_window(s->reader, &s->native) != AMEDIA_OK || !s->native) {
        cham_log("wayland: AImageReader %dx%d failed", width, height);
        if (s->reader)
            android.reader_delete(s->reader);
        free(s);
        return NULL;
    }
    s->surface_version = wl.get_version(wl_surface);
    s->queue = wl.create_queue(d->wl);
    s->wl_surface = wl.create_wrapper(wl_surface);
    wl.set_queue(s->wl_surface, s->queue);
    s->dmabuf = wl.create_wrapper(d->dmabuf);
    wl.set_queue(s->dmabuf, s->queue);
    return s;
}

/* ---- X11 windows ---- */

static void x11_poll(struct cham_window *s);

/* The X server behind a connection shows its clients' buffers (DRI3 1.0 to
 * make them pixmaps, Present 1.0 to show those): asked once per connection. */
static struct x11_conn {
    xcb_connection_t *c;
    int ok;
    struct x11_conn *next;
} *g_x11_conns; /* under g_displays_lock */

static int x11_version(xcb_connection_t *c, xcb_extension_t *ext,
                       xcb_cookie_t (*query)(xcb_connection_t *, uint32_t, uint32_t),
                       xcb_version_reply_t *(*reply)(xcb_connection_t *, xcb_cookie_t, xcb_generic_error_t **))
{
    const xcb_query_extension_reply_t *data = xcb.get_extension_data(c, ext);
    if (!data || !data->present)
        return 0;
    xcb_generic_error_t *e = NULL;
    xcb_version_reply_t *r = reply(c, query(c, 1, 0), &e);
    int ok = r && r->major_version >= 1;
    free(r);
    free(e);
    return ok;
}

int cham_window_x11_connect(void *connection)
{
    xcb_connection_t *c = connection;
    if (!c || !cham_window_x11_available() || xcb.has_error(c))
        return -1;
    pthread_mutex_lock(&g_displays_lock);
    struct x11_conn *x = g_x11_conns;
    while (x && x->c != c)
        x = x->next;
    if (!x && (x = calloc(1, sizeof *x))) {
        x->c = c;
        x->ok = x11_version(c, xcb.dri3_id, xcb.dri3_query_version, xcb.dri3_query_version_reply) &&
                x11_version(c, xcb.present_id, xcb.present_query_version, xcb.present_query_version_reply);
        if (!x->ok)
            cham_log("x11: the X server has no DRI3 or Present; its windows are left to other drivers");
        x->next = g_x11_conns;
        g_x11_conns = x;
    }
    int ok = x && x->ok;
    pthread_mutex_unlock(&g_displays_lock);
    return ok ? 0 : -1;
}

/* The window's size and depth, asked of the server. 0 or -1. */
static int x11_geometry(xcb_connection_t *c, uint32_t window, int *width, int *height, int *depth)
{
    xcb_generic_error_t *e = NULL;
    xcb_get_geometry_reply_t *r = xcb.get_geometry_reply(c, xcb.get_geometry(c, window), &e);
    free(e);
    if (!r)
        return -1;
    *width = r->width;
    *height = r->height;
    *depth = r->depth;
    free(r);
    return 0;
}

struct cham_window *cham_window_create_x11(void *connection, uint32_t window)
{
    xcb_connection_t *c = connection;
    int width, height, depth;
    if (cham_window_x11_connect(c) != 0)
        return NULL;
    if (x11_geometry(c, window, &width, &height, &depth) != 0 || (depth != 24 && depth != 32)) {
        cham_log("x11: window 0x%x has no depth of 24 or 32 bits", window);
        return NULL;
    }
    struct cham_window *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    /* Its buffers have alpha either way: a 24-bit window's pixmaps ignore it. */
    s->drm_format = depth == 32 ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_XBGR8888;
    if (android.reader_new(width ? width : 1, height ? height : 1, AHB_FORMAT_R8G8B8A8, AHB_USAGE_GPU_SAMPLED_IMAGE,
                           READER_IMAGES, &s->reader) != AMEDIA_OK ||
        android.reader_get_window(s->reader, &s->native) != AMEDIA_OK || !s->native) {
        cham_log("x11: AImageReader %dx%d failed", width, height);
        if (s->reader)
            android.reader_delete(s->reader);
        free(s);
        return NULL;
    }
    s->x.c = c;
    s->x.window = window;
    s->x.depth = depth;
    s->x.width = width;
    s->x.height = height;
    s->x.eid = xcb.generate_id(c);
    xcb_generic_error_t *e =
        xcb.request_check(c, xcb.present_select_input(c, s->x.eid, window, PRESENT_EVENT_MASK_ALL));
    if (e) {
        cham_log("x11: Present takes no events for window 0x%x (error %d)", window, e->error_code);
        free(e);
        android.reader_delete(s->reader);
        free(s);
        return NULL;
    }
    s->x.events = xcb.register_special(c, xcb.present_id, s->x.eid, NULL);
    return s;
}

/* XGetXCBConnection, for Xlib's displays: libX11-xcb. */
static void *(*p_xlib_xcb)(void *);

int cham_window_xlib_available(void)
{
    static int loaded = -1;
    if (loaded < 0) {
        void *lib = open_x11_lib("libX11-xcb.so", "libX11-xcb.so.1");
        *(void **)&p_xlib_xcb = lib ? dlsym(lib, "XGetXCBConnection") : NULL;
        loaded = cham_window_x11_available() && p_xlib_xcb;
    }
    return loaded;
}

void *cham_window_xlib_connection(void *display)
{
    return display && cham_window_xlib_available() ? p_xlib_xcb(display) : NULL;
}

int cham_window_x11_size(struct cham_window *s, int ask, int *width, int *height, int *depth)
{
    if (ask) {
        int w, h, d;
        if (x11_geometry(s->x.c, s->x.window, &w, &h, &d) != 0)
            return -1;
        s->x.width = w;
        s->x.height = h;
    } else {
        x11_poll(s); /* ConfigureNotify events already here */
    }
    *width = s->x.width;
    *height = s->x.height;
    if (depth)
        *depth = s->x.depth;
    return 0;
}

void *cham_window_native(struct cham_window *s)
{
    return s->native;
}

int cham_window_resize(struct cham_window *s, int width, int height)
{
    if (android.set_geometry(s->native, width, height, 0) != 0) {
        cham_log("wayland: cannot resize the window to %dx%d", width, height);
        return -1;
    }
    s->serial++;
    return 0;
}

void cham_window_renew(struct cham_window *s, int opaque)
{
    s->drm_format = opaque ? DRM_FORMAT_XBGR8888 : DRM_FORMAT_ABGR8888;
    s->serial++;
}

/* The buffer of a frame: known (by its dmabuf), or registered now. */
static struct buffer *buffer_for(struct cham_window *s, AHardwareBuffer *ahb)
{
    ahb_desc desc;
    android.describe(ahb, &desc);
    const native_handle *nh = android.get_native_handle(ahb);
    int dmabuf = -1;
    for (int i = 0; nh && i < nh->numFds && dmabuf < 0; i++) {
        char path[64], link[128];
        snprintf(path, sizeof path, "/proc/self/fd/%d", nh->data[i]);
        ssize_t n = readlink(path, link, sizeof link - 1);
        if (n > 0) {
            link[n] = 0;
            if (strstr(link, "dmabuf"))
                dmabuf = nh->data[i];
        }
    }
    if (dmabuf < 0 && nh && nh->numFds > 0)
        dmabuf = nh->data[0];
    struct stat st;
    if (dmabuf < 0 || fstat(dmabuf, &st) != 0)
        return NULL;
    struct buffer *free_slot = NULL;
    for (int i = 0; i < MAX_BUFFERS; i++) {
        struct buffer *b = &s->buffers[i];
        if (b->ahb && b->dev == st.st_dev && b->ino == st.st_ino) {
            if (b->drm_format == s->drm_format) {
                b->serial = s->serial; /* one of the set in use */
                return b;
            }
            buffer_free(s, b); /* the compositor must hear of it anew */
        }
        if (!b->ahb && !free_slot)
            free_slot = b;
    }
    if (!free_slot) {
        cham_log("wayland: more than %d buffers in use", MAX_BUFFERS);
        return NULL;
    }
    uint32_t handle = 0;
    struct cham_bo *bo = share_buffer(ahb, nh, &desc, s->drm_format, &handle);
    if (!bo) {
        cham_log("wayland: cannot share a buffer with the session");
        return NULL;
    }
    int width = (int)desc.width, height = (int)desc.height;
    struct wl_proxy *wl_buffer = NULL;
    uint32_t pixmap = 0;

    if (s->d) {
        struct wl_proxy *params = wl.marshal_flags(s->dmabuf, 1 /* create_params */, &k_params_iface,
                                                   wl.get_version(s->dmabuf), 0, NULL);
        /* implicit layout: DRM_FORMAT_MOD_INVALID */
        wl.marshal_flags(params, PARAMS_ADD, NULL, wl.get_version(params), 0, dmabuf, 0u, 0u, desc.stride * 4,
                         (uint32_t)(DRM_FORMAT_MOD_INVALID >> 32),
                         (uint32_t)(DRM_FORMAT_MOD_INVALID & 0xffffffff));
        wl_buffer = wl.marshal_flags(params, PARAMS_CREATE_IMMED, wl.buffer_iface, wl.get_version(params), 0, NULL,
                                     (int32_t)width, (int32_t)height, s->drm_format, 0u);
        wl.marshal_flags(params, PARAMS_DESTROY, NULL, wl.get_version(params), WL_MARSHAL_FLAG_DESTROY);
    } else {
        /* DRI3 1.0: one plane in the driver's own layout, which the X
         * server's gbm (ours, in Xwayland) finds by the dmabuf. The fd is
         * libxcb's to close. */
        uint32_t stride = desc.stride * 4;
        int fd = stride <= UINT16_MAX ? fcntl(dmabuf, F_DUPFD_CLOEXEC, 0) : -1;
        xcb_generic_error_t *e = NULL;
        if (fd >= 0) {
            pixmap = xcb.generate_id(s->x.c);
            e = xcb.request_check(s->x.c, xcb.dri3_pixmap_from_buffer(s->x.c, pixmap, s->x.window, stride * height,
                                                                      width, height, stride, s->x.depth, 32, fd));
        }
        if (fd < 0 || e) {
            cham_log("x11: the X server takes no %dx%d buffer as a pixmap (error %d)", width, height,
                     e ? e->error_code : 0);
            free(e);
            cham_bo_handle_close(cham_render_fd(), handle);
            cham_bo_unref(bo);
            return NULL;
        }
    }

    struct buffer *b = free_slot;
    b->ahb = ahb;
    b->dev = st.st_dev;
    b->ino = st.st_ino;
    b->width = width;
    b->height = height;
    b->serial = s->serial;
    b->drm_format = s->drm_format;
    b->bo = bo;
    b->handle = handle;
    b->wl_buffer = wl_buffer;
    b->pixmap = pixmap;
    if (wl_buffer)
        wl.add_listener(wl_buffer, k_buffer_listener, b);
    return b;
}

/* Forgets buffers of earlier sets the compositor has released: the window
 * replaces them with buffers of the new one. */
static void reap_old_sets(struct cham_window *s)
{
    for (int i = 0; i < MAX_BUFFERS; i++) {
        struct buffer *b = &s->buffers[i];
        if (b->ahb && !b->image && b->serial != s->serial)
            buffer_free(s, b);
    }
}

/* One of Present's events for an X11 window. */
static void x11_event(struct cham_window *s, const uint8_t *ev)
{
    uint16_t type;
    memcpy(&type, ev + 8, 2);
    if (type == PRESENT_EVENT_CONFIGURE_NOTIFY) {
        uint16_t width, height;
        memcpy(&width, ev + 24, 2);
        memcpy(&height, ev + 26, 2);
        s->x.width = width;
        s->x.height = height;
    } else if (type == PRESENT_EVENT_COMPLETE_NOTIFY) {
        uint32_t serial;
        memcpy(&serial, ev + 20, 4);
        if (ev[10] == PRESENT_COMPLETE_KIND_PIXMAP && (int32_t)(serial - s->x.completed) > 0)
            s->x.completed = serial;
    } else if (type == PRESENT_EVENT_IDLE_NOTIFY) {
        uint32_t pixmap;
        memcpy(&pixmap, ev + 24, 4);
        for (int i = 0; i < MAX_BUFFERS; i++) {
            struct buffer *b = &s->buffers[i];
            if (b->pixmap == pixmap && b->image) { /* the server is done with it: back to the reader */
                android.image_delete(b->image);
                b->image = NULL;
            }
        }
    }
}

/* The events already here, without waiting. */
static void x11_poll(struct cham_window *s)
{
    void *ev;
    while ((ev = xcb.poll_special(s->x.c, s->x.events))) {
        x11_event(s, ev);
        free(ev);
    }
}

/* Waits for the next of the window's events and handles it. */
static int dispatch(struct cham_window *s)
{
    if (!s->d) {
        void *ev = xcb.wait_special(s->x.c, s->x.events);
        if (!ev) {
            cham_log("x11: lost the connection to the X server");
            return -1;
        }
        x11_event(s, ev);
        free(ev);
        x11_poll(s);
        return 0;
    }
    if (wl.dispatch_queue(s->d->wl, s->queue) < 0) {
        cham_log("wayland: lost the connection (error %d)", wl.get_error(s->d->wl));
        return -1;
    }
    return 0;
}

int cham_window_wait_frame(struct cham_window *s)
{
    while (s->d ? s->frame_callback != NULL : (int32_t)(s->x.presented - s->x.completed) > 0)
        if (dispatch(s) < 0)
            return -1;
    return 0;
}

int cham_window_commit(struct cham_window *s, int callback, int dx, int dy, const int *rects, int n,
                       int bottom_left, int *width, int *height)
{
    AImage *image = NULL;
    int fence = -1;
    for (int tries = 0;; tries++) {
        int r = android.reader_acquire_next(s->reader, &image, &fence);
        if (r == AMEDIA_OK && image)
            break;
        if (r == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED && dispatch(s) == 0)
            continue; /* The compositor still holds them all: wait for a release */
        if (r == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE && tries < 100) {
            usleep(1000);
            continue;
        }
        cham_log("wayland: no frame from the AImageReader (%d)", r);
        return -1;
    }
    AHardwareBuffer *ahb = NULL;
    android.image_get_ahb(image, &ahb);
    struct buffer *b = ahb ? buffer_for(s, ahb) : NULL;
    if (!b) {
        if (fence >= 0)
            close(fence);
        android.image_delete(image);
        return -1;
    }
    if (b->image) /* can't happen: the reader hands out a buffer once */
        android.image_delete(b->image);
    b->image = image;
    /* The compositor samples the buffer with no fence of ours: wait for the
     * GPU here. */
    if (fence >= 0) {
        struct pollfd p = {.fd = fence, .events = POLLIN};
        while (poll(&p, 1, -1) < 0 && errno == EINTR)
            ;
        close(fence);
    }

    if (width)
        *width = b->width;
    if (height)
        *height = b->height;

    if (!s->d) {
        /* Shown when the server can (target MSC 0); its CompleteNotify
         * paces FIFO, its IdleNotify gives the buffer back. */
        uint32_t serial = s->x.presented + 1;
        xcb_cookie_t ck = xcb.present_pixmap(s->x.c, s->x.window, b->pixmap, serial, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                             0, 0, NULL);
        xcb.discard_reply(s->x.c, ck.sequence); /* errors (the window went) not to the program */
        s->x.presented = serial;
        xcb.flush(s->x.c);
        x11_poll(s);
        reap_old_sets(s);
        return xcb.has_error(s->x.c) ? -1 : 0;
    }

    if (callback) {
        s->frame_callback = wl.marshal_flags(s->wl_surface, SURFACE_FRAME, wl.callback_iface,
                                             s->surface_version, 0, NULL);
        wl.add_listener(s->frame_callback, k_frame_listener, s);
    }
    if (s->surface_version >= 5) {
        wl.marshal_flags(s->wl_surface, SURFACE_ATTACH, NULL, s->surface_version, 0, b->wl_buffer, 0, 0);
        if (dx || dy)
            wl.marshal_flags(s->wl_surface, SURFACE_OFFSET, NULL, s->surface_version, 0, dx, dy);
    } else {
        wl.marshal_flags(s->wl_surface, SURFACE_ATTACH, NULL, s->surface_version, 0, b->wl_buffer, dx, dy);
    }
    int damage_op = s->surface_version >= 4 ? SURFACE_DAMAGE_BUFFER : SURFACE_DAMAGE;
    if (rects && n > 0) {
        for (int i = 0; i < n; i++) {
            const int *r = &rects[i * 4];
            wl.marshal_flags(s->wl_surface, damage_op, NULL, s->surface_version, 0, r[0],
                             bottom_left ? b->height - r[1] - r[3] : r[1], r[2], r[3]);
        }
    } else {
        wl.marshal_flags(s->wl_surface, damage_op, NULL, s->surface_version, 0, 0, 0, INT32_MAX, INT32_MAX);
    }
    wl.marshal_flags(s->wl_surface, SURFACE_COMMIT, NULL, s->surface_version, 0);
    wl.flush(s->d->wl);
    reap_old_sets(s);
    return 0;
}

/* Our side of the window. The compositor keeps what it shows through its
 * own references. The driver's surface on it must be gone already. */
void cham_window_destroy(struct cham_window *s)
{
    if (s->frame_callback)
        wl.proxy_destroy(s->frame_callback);
    for (int i = 0; i < MAX_BUFFERS; i++)
        if (s->buffers[i].ahb)
            buffer_free(s, &s->buffers[i]);
    android.reader_delete(s->reader);
    if (!s->d) {
        /* The window may be gone already: no error for the program. */
        xcb_cookie_t ck = xcb.present_select_input(s->x.c, s->x.eid, s->x.window, 0);
        xcb.discard_reply(s->x.c, ck.sequence);
        xcb.unregister_special(s->x.c, s->x.events);
        xcb.flush(s->x.c);
        free(s);
        return;
    }
    wl.wrapper_destroy(s->wl_surface);
    wl.wrapper_destroy(s->dmabuf);
    wl.flush(s->d->wl);
    wl.queue_destroy(s->queue);
    free(s);
}
