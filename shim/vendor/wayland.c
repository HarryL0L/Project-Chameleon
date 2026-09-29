/*
 * EGL_PLATFORM_WAYLAND_KHR for apps: Wayland windows rendered by Android's
 * GPU driver and handed to KWin without copies.
 *
 * Android's EGL only renders into ANativeWindows, and an AImageReader is one
 * a process can create for itself. So each wl_egl_window gets an
 * AImageReader and a real Android window surface on it: the app renders
 * into "framebuffer 0" as usual, with the driver's own buffer handling
 * (buffer age, damage, fences). After every eglSwapBuffers the frame is taken
 * out of the reader as an AHardwareBuffer and shown on the app's wl_surface:
 *
 *   1. registered with libchameleon.so inside KWin (clients.c), once per
 *      buffer, because KWin can't turn a bare dmabuf back into an AHB;
 *   2. wrapped in a zwp_linux_dmabuf_v1 wl_buffer (its gralloc dmabuf);
 *   3. attached, damaged and committed on the app's wl_surface.
 *
 * wl_buffer.release gives the image back to the reader, and frame callbacks
 * pace swaps for swap interval >= 1. All Wayland objects of ours live on a
 * private event queue, so the app's own event handling is not disturbed.
 *
 * The surfaces the app sees are ours (struct surface); egl.c swaps in the
 * Android surface wherever one is passed to the driver.
 *
 * Only offered when KWin runs with the Chameleon shim (its
 * "<wayland socket>.chameleon" exists); elsewhere glvnd moves on to Mesa.
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include "../../common/cham_io.h"
#include "../core/cham_shim.h"
#include "vendor.h"

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
    int (*send_handle)(const AHardwareBuffer *, int);
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
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
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
    A(nw, send_handle, "AHardwareBuffer_sendHandleToUnixSocket");
    A(media, reader_new, "AImageReader_newWithUsage");
    A(media, reader_get_window, "AImageReader_getWindow");
    A(media, reader_acquire_next, "AImageReader_acquireNextImageAsync");
    A(media, reader_delete, "AImageReader_delete");
    A(media, image_get_ahb, "AImage_getHardwareBuffer");
    A(media, image_delete, "AImage_delete");
#undef A
    loaded = android.describe && android.get_native_handle && android.send_handle && android.reader_new &&
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
    /* Usually loaded by the app already; any copy of the same library will do. */
    void *lib = dlopen("libwayland-client.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib)
        lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!lib)
        lib = dlopen("libwayland-client.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
#define W(field, name) (*(void **)&wl.field = lib ? dlsym(lib, name) : NULL)
    W(marshal_flags, "wl_proxy_marshal_flags");
    W(add_listener, "wl_proxy_add_listener");
    W(get_version, "wl_proxy_get_version");
    W(create_wrapper, "wl_proxy_create_wrapper");
    W(wrapper_destroy, "wl_proxy_wrapper_destroy");
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

/* libwayland-egl's struct, WL_EGL_WINDOW_VERSION 3 (wayland-egl-backend.h) */
struct wl_egl_window {
    const intptr_t version;
    int width;
    int height;
    int dx;
    int dy;
    int attached_width;
    int attached_height;
    void *driver_private;
    void (*resize_callback)(struct wl_egl_window *, void *);
    void (*destroy_window_callback)(void *);
    struct wl_proxy *surface;
};

/* ---- the connection to KWin ---- */

struct display {
    struct wl_display *wl;
    struct wl_event_queue *queue;
    struct wl_proxy *dmabuf;
    int registry_sock; /* to clients.c in KWin */
    uint32_t next_buffer_id;
    pthread_mutex_t lock;
    struct display *next;
};

static pthread_mutex_t g_displays_lock = PTHREAD_MUTEX_INITIALIZER;
static struct display *g_displays;
static int g_wayland_used;

/* "<KWin's Wayland socket>.chameleon" for the display this process uses. */
static int registry_path(char *out, size_t size)
{
    const char *name = getenv("WAYLAND_DISPLAY");
    if (!name || !*name)
        name = "wayland-0";
    if (name[0] == '/')
        return snprintf(out, size, "%s%s", name, CHAM_CLIENT_SOCKET_SUFFIX) < (int)size;
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        return 0;
    return snprintf(out, size, "%s/%s%s", dir, name, CHAM_CLIENT_SOCKET_SUFFIX) < (int)size;
}

int cham_wl_platform_available(void)
{
    static int available = -1;
    if (available >= 0)
        return available;
    char path[108];
    struct stat st;
    available = registry_path(path, sizeof path) && stat(path, &st) == 0 && S_ISSOCK(st.st_mode) &&
                load_android() && load_wayland();
    return available;
}

static int connect_registry(void)
{
    char path[108];
    if (!registry_path(path, sizeof path))
        return -1;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (s >= 0 && connect(s, (struct sockaddr *)&addr, sizeof addr) == 0)
        return s;
    if (s >= 0)
        close(s);
    return -1;
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

/* The EGLDisplay for a wl_display: Android's, once we know we can serve it. */
EGLDisplay cham_wl_get_display(void *native)
{
    struct wl_display *wd = native;
    if (!wd || !cham_wl_platform_available())
        return EGL_NO_DISPLAY;
    pthread_mutex_lock(&g_displays_lock);
    struct display *d = g_displays;
    while (d && d->wl != wd)
        d = d->next;
    if (!d) {
        d = calloc(1, sizeof *d);
        d->wl = wd;
        d->registry_sock = -1;
        d->next_buffer_id = 1;
        pthread_mutex_init(&d->lock, NULL);
        d->queue = wl.create_queue(wd);
        struct wl_proxy *wrapper = wl.create_wrapper(wd);
        wl.set_queue(wrapper, d->queue);
        struct wl_proxy *registry =
            wl.marshal_flags(wrapper, 1 /* get_registry */, wl.registry_iface, wl.get_version(wrapper), 0, NULL);
        wl.wrapper_destroy(wrapper);
        wl.add_listener(registry, k_registry_listener, d);
        wl.roundtrip_queue(wd, d->queue);
        wl.proxy_destroy(registry);
        d->registry_sock = connect_registry();
        if (!d->dmabuf || d->registry_sock < 0) {
            cham_log("wayland: KWin offers no %s; Wayland windows left to other vendors",
                     d->dmabuf ? "Chameleon buffer socket" : "zwp_linux_dmabuf_v1");
            if (d->dmabuf)
                wl.proxy_destroy(d->dmabuf);
            if (d->registry_sock >= 0)
                close(d->registry_sock);
            wl.queue_destroy(d->queue);
            free(d);
            pthread_mutex_unlock(&g_displays_lock);
            return EGL_NO_DISPLAY;
        }
        d->next = g_displays;
        g_displays = d;
        g_wayland_used = 1;
    }
    pthread_mutex_unlock(&g_displays_lock);
    return cham_egl_android_display();
}

/* Registers an AHardwareBuffer with KWin; returns its id, 0 on failure. */
static uint32_t register_buffer(struct display *d, const AHardwareBuffer *ahb)
{
    pthread_mutex_lock(&d->lock);
    uint32_t id = d->next_buffer_id++;
    struct cham_msg m = {CHAM_CLIENT_BUFFER_ADD, id, 0, 0};
    int ok = cham_send(d->registry_sock, &m, -1) == 0 && android.send_handle(ahb, d->registry_sock) == 0;
    while (ok) {
        struct cham_msg ack;
        int fd = -1;
        if (cham_recv(d->registry_sock, &ack, &fd) <= 0) {
            ok = 0;
            break;
        }
        if (fd >= 0)
            close(fd);
        if (ack.type == CHAM_CLIENT_BUFFER_ACK && ack.id == id) {
            ok = ack.a == 1;
            break;
        }
    }
    pthread_mutex_unlock(&d->lock);
    return ok ? id : 0;
}

static void unregister_buffer(struct display *d, uint32_t id)
{
    pthread_mutex_lock(&d->lock);
    struct cham_msg m = {CHAM_CLIENT_BUFFER_REMOVE, id, 0, 0};
    cham_send(d->registry_sock, &m, -1);
    pthread_mutex_unlock(&d->lock);
}

/* ---- window surfaces ---- */

#define MAX_BUFFERS 6
#define READER_IMAGES 3

struct buffer {
    AHardwareBuffer *ahb; /* identity only; the reader owns it */
    struct wl_proxy *wl_buffer;
    uint32_t id;          /* registration with KWin */
    AImage *image;        /* held while KWin uses the buffer */
    struct surface *owner;
};

/* One AImageReader + Android window surface at a given size. */
struct generation {
    AImageReader *reader;
    EGLSurface android;
    int width, height;
    struct buffer buffers[MAX_BUFFERS];
    int nbuffers;
    struct generation *next; /* retired ones, still in use by KWin */
};

struct surface {
    struct display *d;
    struct wl_event_queue *queue; /* this surface's events only */
    struct wl_proxy *dmabuf;      /* wrapper on `queue`: its wl_buffers land there */
    EGLDisplay dpy;
    EGLConfig config;
    struct wl_egl_window *window;
    struct wl_proxy *wl_surface; /* wrapper on our queue */
    uint32_t surface_version;
    int ahb_format;
    uint32_t drm_format;
    struct generation *gen;
    struct generation *retired;
    struct wl_proxy *frame_callback;
    int swap_interval;
    pthread_mutex_t lock;
};

/* Our surfaces. EGLSurface handles from Android are never dereferenced:
 * a handle is ours only if it is in this table. */
static pthread_mutex_t g_surfaces_lock = PTHREAD_MUTEX_INITIALIZER;
static struct surface *g_surfaces[256];

static struct surface *as_surface(EGLSurface handle)
{
    if (!g_wayland_used || !handle)
        return NULL;
    struct surface *found = NULL;
    pthread_mutex_lock(&g_surfaces_lock);
    for (size_t i = 0; i < sizeof g_surfaces / sizeof g_surfaces[0]; i++)
        if ((EGLSurface)g_surfaces[i] == handle)
            found = g_surfaces[i];
    pthread_mutex_unlock(&g_surfaces_lock);
    return found;
}

static int track(struct surface *s, int add)
{
    int ok = 0;
    pthread_mutex_lock(&g_surfaces_lock);
    for (size_t i = 0; i < sizeof g_surfaces / sizeof g_surfaces[0]; i++) {
        if (add && !g_surfaces[i]) {
            g_surfaces[i] = s;
            ok = 1;
            break;
        }
        if (!add && g_surfaces[i] == s) {
            g_surfaces[i] = NULL;
            ok = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_surfaces_lock);
    return ok;
}

static EGLSurface (*real_create_window_surface)(EGLDisplay, EGLConfig, void *, const EGLint *);
static EGLBoolean (*real_destroy_surface)(EGLDisplay, EGLSurface);
static EGLBoolean (*real_get_config_attrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
static EGLSurface (*real_get_current_surface)(EGLint);
static EGLContext (*real_get_current_context)(void);
static EGLBoolean (*real_make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*real_swap_interval)(EGLDisplay, EGLint);

static int load_egl(void)
{
    if (real_make_current)
        return 1;
    *(void **)&real_create_window_surface = cham_egl_real("eglCreateWindowSurface");
    *(void **)&real_destroy_surface = cham_egl_real("eglDestroySurface");
    *(void **)&real_get_config_attrib = cham_egl_real("eglGetConfigAttrib");
    *(void **)&real_get_current_surface = cham_egl_real("eglGetCurrentSurface");
    *(void **)&real_get_current_context = cham_egl_real("eglGetCurrentContext");
    *(void **)&real_swap_interval = cham_egl_real("eglSwapInterval");
    *(void **)&real_make_current = cham_egl_real("eglMakeCurrent");
    return real_create_window_surface && real_destroy_surface && real_get_config_attrib && real_get_current_surface &&
           real_get_current_context && real_swap_interval && real_make_current;
}

static struct generation *generation_new(struct surface *s, int width, int height)
{
    struct generation *g = calloc(1, sizeof *g);
    g->width = width > 0 ? width : 1;
    g->height = height > 0 ? height : 1;
    ANativeWindow *window = NULL;
    if (android.reader_new(g->width, g->height, s->ahb_format, AHB_USAGE_GPU_SAMPLED_IMAGE, READER_IMAGES,
                           &g->reader) != AMEDIA_OK ||
        android.reader_get_window(g->reader, &window) != AMEDIA_OK || !window) {
        cham_log("wayland: AImageReader %dx%d failed", g->width, g->height);
        if (g->reader)
            android.reader_delete(g->reader);
        free(g);
        return NULL;
    }
    g->android = real_create_window_surface(s->dpy, s->config, window, NULL);
    if (g->android == EGL_NO_SURFACE) {
        cham_log("wayland: eglCreateWindowSurface on the AImageReader failed");
        android.reader_delete(g->reader);
        free(g);
        return NULL;
    }
    return g;
}

/* Destroys a generation. KWin keeps what it shows through its own
 * references, so only our side is released here. */
static void generation_free(struct surface *s, struct generation *g)
{
    for (int i = 0; i < g->nbuffers; i++) {
        struct buffer *b = &g->buffers[i];
        if (b->wl_buffer)
            wl.marshal_flags(b->wl_buffer, 0 /* destroy */, NULL, wl.get_version(b->wl_buffer),
                             WL_MARSHAL_FLAG_DESTROY);
        if (b->id)
            unregister_buffer(s->d, b->id);
        if (b->image)
            android.image_delete(b->image);
    }
    if (g->android != EGL_NO_SURFACE)
        real_destroy_surface(s->dpy, g->android);
    android.reader_delete(g->reader);
    free(g);
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
    struct surface *s = data;
    if (s->frame_callback == callback)
        s->frame_callback = NULL;
    wl.proxy_destroy(callback);
}

static void (*k_frame_listener[])(void) = {(void (*)(void))frame_done};

static void follow_resize(struct surface *s);

/* wl_egl_window_resize(). Apps resize right before drawing the next frame
 * (and glvnd skips eglMakeCurrent when nothing changes), so switch to the new
 * size now if the surface is current on this thread; otherwise at the next
 * make-current or swap. */
static void window_resized(struct wl_egl_window *window, void *data)
{
    (void)window;
    struct surface *s = data;
    pthread_mutex_lock(&s->lock);
    if (real_get_current_surface(EGL_DRAW) == s->gen->android)
        follow_resize(s);
    pthread_mutex_unlock(&s->lock);
}

static void window_destroyed(void *data)
{
    struct surface *s = data;
    s->window = NULL;
}

/* Called by egl.c for eglCreate(Platform)WindowSurface. EGL_NO_SURFACE with
 * *handled = 0 means "not a wl_egl_window, let Android have it". */
EGLSurface cham_wl_create_window_surface(EGLDisplay dpy, EGLConfig config, void *native, int *handled)
{
    *handled = 0;
    struct wl_egl_window *window = native;
    if (!g_wayland_used || !window || !load_egl())
        return EGL_NO_SURFACE;
    /* A wl_egl_window starts with its version; an ANativeWindow with a
     * magic number far larger than any version. */
    if (window->version < 3 || window->version > 16 || !window->surface)
        return EGL_NO_SURFACE;
    *handled = 1;
    if (window->driver_private) {
        cham_setEGLError(EGL_BAD_ALLOC); /* already has a surface */
        return EGL_NO_SURFACE;
    }
    struct display *d = g_displays; /* the display this surface's wl_surface belongs to */
    if (!d) {
        cham_setEGLError(EGL_BAD_NATIVE_WINDOW);
        return EGL_NO_SURFACE;
    }

    struct surface *s = calloc(1, sizeof *s);
    s->d = d;
    s->dpy = dpy;
    s->config = config;
    s->window = window;
    s->swap_interval = 1;
    pthread_mutex_init(&s->lock, NULL);
    EGLint alpha = 8, visual = AHB_FORMAT_R8G8B8A8;
    real_get_config_attrib(dpy, config, EGL_ALPHA_SIZE, &alpha);
    real_get_config_attrib(dpy, config, EGL_NATIVE_VISUAL_ID, &visual);
    int opaque = visual == AHB_FORMAT_R8G8B8X8 || alpha == 0;
    s->ahb_format = opaque ? AHB_FORMAT_R8G8B8X8 : AHB_FORMAT_R8G8B8A8;
    s->drm_format = opaque ? DRM_FORMAT_XBGR8888 : DRM_FORMAT_ABGR8888;
    s->surface_version = wl.get_version(window->surface);
    s->queue = wl.create_queue(d->wl);
    s->wl_surface = wl.create_wrapper(window->surface);
    wl.set_queue(s->wl_surface, s->queue);
    s->dmabuf = wl.create_wrapper(d->dmabuf);
    wl.set_queue(s->dmabuf, s->queue);
    s->gen = generation_new(s, window->width, window->height);
    if (!s->gen || !track(s, 1)) {
        if (s->gen)
            generation_free(s, s->gen);
        wl.wrapper_destroy(s->wl_surface);
        wl.wrapper_destroy(s->dmabuf);
        wl.queue_destroy(s->queue);
        free(s);
        cham_setEGLError(EGL_BAD_ALLOC);
        return EGL_NO_SURFACE;
    }
    window->driver_private = s;
    window->resize_callback = window_resized;
    window->destroy_window_callback = window_destroyed;
    return (EGLSurface)s;
}

int cham_wl_is_surface(EGLSurface surface)
{
    return as_surface(surface) != NULL;
}

/* The Android surface to hand the driver for `surface` (itself if not ours). */
EGLSurface cham_wl_android_surface(EGLSurface surface)
{
    struct surface *s = as_surface(surface);
    return s ? s->gen->android : surface;
}

/* Follows wl_egl_window_resize(): a new reader at the new size; the old one
 * is kept until KWin has the first new frame. Called where the app is about
 * to draw (make current) and after a swap. */
static void follow_resize(struct surface *s)
{
    if (!s->window || (s->window->width == s->gen->width && s->window->height == s->gen->height))
        return;
    if (s->window->width <= 0 || s->window->height <= 0)
        return;
    struct generation *g = generation_new(s, s->window->width, s->window->height);
    if (!g)
        return;
    struct generation *old = s->gen;
    s->gen = g;
    old->next = s->retired;
    s->retired = old;
    /* Keep rendering on this thread going into the new surface. */
    EGLContext ctx = real_get_current_context();
    if (ctx != EGL_NO_CONTEXT && real_get_current_surface(EGL_DRAW) == old->android) {
        EGLSurface read = real_get_current_surface(EGL_READ);
        real_make_current(s->dpy, g->android, read == old->android ? g->android : read, ctx);
        real_swap_interval(s->dpy, 0);
    }
}

void cham_wl_before_make_current(EGLSurface draw)
{
    struct surface *s = as_surface(draw);
    if (s) {
        pthread_mutex_lock(&s->lock);
        follow_resize(s);
        pthread_mutex_unlock(&s->lock);
    }
}

void cham_wl_after_make_current(EGLDisplay dpy, EGLSurface draw)
{
    /* The reader is not a display: never let the driver wait for "vsync";
     * pacing comes from Wayland frame callbacks. */
    if (as_surface(draw))
        real_swap_interval(dpy, 0);
}

/* eglSwapInterval for the current surface, if it is ours. */
int cham_wl_swap_interval(EGLint interval)
{
    if (!g_wayland_used || !load_egl())
        return 0;
    EGLSurface cur = real_get_current_surface(EGL_DRAW);
    pthread_mutex_lock(&g_surfaces_lock);
    struct surface *found = NULL;
    for (size_t i = 0; i < sizeof g_surfaces / sizeof g_surfaces[0]; i++)
        if (g_surfaces[i] && g_surfaces[i]->gen->android == cur)
            found = g_surfaces[i];
    pthread_mutex_unlock(&g_surfaces_lock);
    if (!found)
        return 0;
    found->swap_interval = interval < 0 ? 0 : interval;
    return 1;
}

static struct buffer *buffer_for(struct surface *s, AHardwareBuffer *ahb)
{
    struct generation *g = s->gen;
    for (int i = 0; i < g->nbuffers; i++)
        if (g->buffers[i].ahb == ahb)
            return &g->buffers[i];
    if (g->nbuffers == MAX_BUFFERS)
        return NULL;

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
    uint32_t id = dmabuf >= 0 ? register_buffer(s->d, ahb) : 0;
    if (!id) {
        cham_log("wayland: KWin did not take a buffer");
        return NULL;
    }

    struct wl_proxy *params =
        wl.marshal_flags(s->dmabuf, 1 /* create_params */, &k_params_iface, wl.get_version(s->dmabuf), 0, NULL);
    /* implicit layout: DRM_FORMAT_MOD_INVALID */
    wl.marshal_flags(params, PARAMS_ADD, NULL, wl.get_version(params), 0, dmabuf, 0u, 0u, desc.stride * 4,
                     (uint32_t)(DRM_FORMAT_MOD_INVALID >> 32), (uint32_t)(DRM_FORMAT_MOD_INVALID & 0xffffffff));
    struct wl_proxy *wl_buffer = wl.marshal_flags(params, PARAMS_CREATE_IMMED, wl.buffer_iface,
                                                  wl.get_version(params), 0, NULL, (int32_t)g->width,
                                                  (int32_t)g->height, s->drm_format, 0u);
    wl.marshal_flags(params, PARAMS_DESTROY, NULL, wl.get_version(params), WL_MARSHAL_FLAG_DESTROY);

    struct buffer *b = &g->buffers[g->nbuffers++];
    b->ahb = ahb;
    b->id = id;
    b->wl_buffer = wl_buffer;
    b->owner = s;
    wl.add_listener(wl_buffer, k_buffer_listener, b);
    return b;
}

/* Frees retired generations none of whose buffers KWin still holds. */
static void reap_retired(struct surface *s)
{
    for (struct generation **p = &s->retired; *p;) {
        struct generation *g = *p;
        int busy = 0;
        for (int i = 0; i < g->nbuffers; i++)
            busy |= g->buffers[i].image != NULL;
        if (busy) {
            p = &g->next;
            continue;
        }
        *p = g->next;
        generation_free(s, g);
    }
}

static int dispatch(struct surface *s)
{
    if (wl.dispatch_queue(s->d->wl, s->queue) < 0) {
        cham_log("wayland: lost the connection (error %d)", wl.get_error(s->d->wl));
        return -1;
    }
    return 0;
}

/* eglSwapBuffers(WithDamage) on one of our surfaces. `swap` does the
 * driver's swap on the Android surface. */
EGLBoolean cham_wl_swap(EGLDisplay dpy, EGLSurface surface, const EGLint *rects, EGLint n,
                        EGLBoolean (*swap)(EGLDisplay, EGLSurface, const EGLint *, EGLint))
{
    struct surface *s = as_surface(surface);
    if (!s)
        return swap(dpy, surface, rects, n);
    pthread_mutex_lock(&s->lock);
    EGLBoolean ok = EGL_FALSE;

    /* Swap interval >= 1: at most one frame ahead of the compositor. */
    while (s->swap_interval > 0 && s->frame_callback)
        if (dispatch(s) < 0)
            goto out;

    struct generation *g = s->gen;
    if (!swap(dpy, g->android, rects, n))
        goto out;

    AImage *image = NULL;
    int fence = -1;
    for (int tries = 0;; tries++) {
        int r = android.reader_acquire_next(g->reader, &image, &fence);
        if (r == AMEDIA_OK && image)
            break;
        if (r == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED && dispatch(s) == 0)
            continue; /* KWin still holds them all: wait for a release */
        if (r == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE && tries < 100) {
            usleep(1000);
            continue;
        }
        cham_log("wayland: no frame from the AImageReader (%d)", r);
        goto out;
    }
    AHardwareBuffer *ahb = NULL;
    android.image_get_ahb(image, &ahb);
    struct buffer *b = ahb ? buffer_for(s, ahb) : NULL;
    if (!b) {
        if (fence >= 0)
            close(fence);
        android.image_delete(image);
        goto out;
    }
    if (b->image) /* can't happen: the reader hands out a buffer once */
        android.image_delete(b->image);
    b->image = image;
    /* KWin samples the buffer with no fence of ours: wait for the GPU here. */
    if (fence >= 0) {
        struct pollfd p = {.fd = fence, .events = POLLIN};
        while (poll(&p, 1, -1) < 0 && errno == EINTR)
            ;
        close(fence);
    }

    if (s->swap_interval > 0) {
        s->frame_callback = wl.marshal_flags(s->wl_surface, SURFACE_FRAME, wl.callback_iface,
                                             s->surface_version, 0, NULL);
        wl.add_listener(s->frame_callback, k_frame_listener, s);
    }
    int dx = s->window ? s->window->dx : 0, dy = s->window ? s->window->dy : 0;
    if (s->surface_version >= 5) {
        wl.marshal_flags(s->wl_surface, SURFACE_ATTACH, NULL, s->surface_version, 0, b->wl_buffer, 0, 0);
        if (dx || dy)
            wl.marshal_flags(s->wl_surface, SURFACE_OFFSET, NULL, s->surface_version, 0, dx, dy);
    } else {
        wl.marshal_flags(s->wl_surface, SURFACE_ATTACH, NULL, s->surface_version, 0, b->wl_buffer, dx, dy);
    }
    if (s->window) {
        s->window->dx = s->window->dy = 0;
        s->window->attached_width = g->width;
        s->window->attached_height = g->height;
    }
    int damage_op = s->surface_version >= 4 ? SURFACE_DAMAGE_BUFFER : SURFACE_DAMAGE;
    if (rects && n > 0) {
        for (EGLint i = 0; i < n; i++) {
            const EGLint *r = &rects[i * 4]; /* EGL: origin bottom-left */
            wl.marshal_flags(s->wl_surface, damage_op, NULL, s->surface_version, 0, r[0], g->height - r[1] - r[3],
                             r[2], r[3]);
        }
    } else {
        wl.marshal_flags(s->wl_surface, damage_op, NULL, s->surface_version, 0, 0, 0, INT32_MAX, INT32_MAX);
    }
    wl.marshal_flags(s->wl_surface, SURFACE_COMMIT, NULL, s->surface_version, 0);
    wl.flush(s->d->wl);
    ok = EGL_TRUE;

    reap_retired(s);
    follow_resize(s);
out:
    pthread_mutex_unlock(&s->lock);
    return ok;
}

EGLBoolean cham_wl_destroy_surface(EGLDisplay dpy, EGLSurface surface)
{
    (void)dpy;
    struct surface *s = as_surface(surface);
    if (!s)
        return EGL_FALSE;
    track(s, 0);
    pthread_mutex_lock(&s->lock);
    if (s->frame_callback)
        wl.proxy_destroy(s->frame_callback);
    while (s->retired) {
        struct generation *g = s->retired;
        s->retired = g->next;
        generation_free(s, g);
    }
    generation_free(s, s->gen);
    if (s->window) {
        s->window->driver_private = NULL;
        s->window->resize_callback = NULL;
        s->window->destroy_window_callback = NULL;
    }
    wl.wrapper_destroy(s->wl_surface);
    wl.wrapper_destroy(s->dmabuf);
    wl.flush(s->d->wl);
    wl.queue_destroy(s->queue);
    pthread_mutex_unlock(&s->lock);
    free(s);
    return EGL_TRUE;
}
