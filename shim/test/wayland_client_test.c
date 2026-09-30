/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host test of GPU rendering for Wayland apps (vendor/wayland.c + the
 * buffer registry in core/clients.c), end to end in one process:
 *
 *  - a compositor thread (real libwayland-server) with wl_compositor and a
 *    zwp_linux_dmabuf_v1 that, like KWin's EGL import, only accepts a dmabuf
 *    that libchameleon.so knows as a registered app buffer;
 *  - the app: real libwayland-client + libwayland-egl, EGL through the
 *    system's glvnd -> libEGL_chameleon.so -> fake Android driver whose
 *    window surfaces render into (fake) AImageReaders.
 */
#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-server.h>

#include <drm_fourcc.h>

#include "check.h"

/* ---- compositor ---- */

static const struct wl_interface *k_null[8];
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
static const struct wl_interface k_dmabuf_iface = {"zwp_linux_dmabuf_v1", 3, 2, k_dmabuf_requests, 2,
                                                   k_dmabuf_events};
static const struct wl_interface *k_create_immed_types[] = {&wl_buffer_interface, NULL, NULL, NULL, NULL};
static const struct wl_interface *k_created_types[] = {&wl_buffer_interface};
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
static const struct wl_interface k_params_iface = {"zwp_linux_buffer_params_v1", 3, 4, k_params_requests, 2,
                                                   k_params_events};

static struct {
    int buffers_created, buffers_known_to_kwin, buffers_unknown;
    int commits, releases, frames_done;
    int last_width, last_height, last_stride;
    uint32_t last_format;
    uint64_t last_modifier;
    volatile int stop;
} srv;

static void *(*client_ahb_from_fd)(int); /* libchameleon: AHardwareBuffer * or NULL */

struct buffer {
    struct wl_resource *resource;
    int width, height;
};

struct params {
    int fd;
    uint32_t stride;
    uint64_t modifier;
};

static void buffer_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static const struct wl_buffer_interface k_buffer_impl = {.destroy = buffer_destroy};
static void buffer_free(struct wl_resource *r)
{
    free(wl_resource_get_user_data(r));
}

static void params_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static void params_add(struct wl_client *c, struct wl_resource *r, int32_t fd, uint32_t plane, uint32_t offset,
                       uint32_t stride, uint32_t mod_hi, uint32_t mod_lo)
{
    (void)c, (void)plane, (void)offset;
    struct params *p = wl_resource_get_user_data(r);
    if (p->fd >= 0)
        close(p->fd);
    p->fd = fd;
    p->stride = stride;
    p->modifier = (uint64_t)mod_hi << 32 | mod_lo;
}
static void params_create(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h, uint32_t f, uint32_t fl)
{
    (void)c, (void)r, (void)w, (void)h, (void)f, (void)fl;
}
static void params_create_immed(struct wl_client *client, struct wl_resource *r, uint32_t id, int32_t w, int32_t h,
                                uint32_t format, uint32_t flags)
{
    (void)flags;
    struct params *p = wl_resource_get_user_data(r);
    /* What KWin's EGL import does through the vendor: find the AHB. */
    if (p->fd >= 0 && client_ahb_from_fd && client_ahb_from_fd(p->fd))
        srv.buffers_known_to_kwin++;
    else
        srv.buffers_unknown++;
    srv.buffers_created++;
    srv.last_width = w;
    srv.last_height = h;
    srv.last_stride = (int)p->stride;
    srv.last_format = format;
    srv.last_modifier = p->modifier;
    struct buffer *b = calloc(1, sizeof *b);
    b->width = w;
    b->height = h;
    b->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    wl_resource_set_implementation(b->resource, &k_buffer_impl, b, buffer_free);
}
static const struct {
    void *fns[4];
} k_params_impl = {{params_destroy, params_add, params_create, params_create_immed}};
static void params_free(struct wl_resource *r)
{
    struct params *p = wl_resource_get_user_data(r);
    if (p->fd >= 0)
        close(p->fd);
    free(p);
}

static void dmabuf_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static void dmabuf_create_params(struct wl_client *client, struct wl_resource *r, uint32_t id)
{
    struct params *p = calloc(1, sizeof *p);
    p->fd = -1;
    struct wl_resource *res = wl_resource_create(client, &k_params_iface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(res, &k_params_impl, p, params_free);
}
static const struct {
    void *fns[2];
} k_dmabuf_impl = {{dmabuf_destroy, dmabuf_create_params}};
static void bind_dmabuf(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &k_dmabuf_iface, (int)version, id);
    wl_resource_set_implementation(r, &k_dmabuf_impl, NULL, NULL);
    wl_resource_post_event(r, 0 /* format */, DRM_FORMAT_ABGR8888);
}

struct surface {
    struct wl_resource *pending, *current;
    struct wl_list frames;
};

static void surface_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static void surface_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buffer, int32_t x,
                           int32_t y)
{
    (void)c, (void)x, (void)y;
    struct surface *s = wl_resource_get_user_data(r);
    s->pending = buffer;
}
static void surface_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c, (void)r, (void)x, (void)y, (void)w, (void)h;
}
static void surface_frame(struct wl_client *client, struct wl_resource *r, uint32_t id)
{
    struct surface *s = wl_resource_get_user_data(r);
    struct wl_resource *cb = wl_resource_create(client, &wl_callback_interface, 1, id);
    wl_resource_set_implementation(cb, NULL, NULL, NULL);
    wl_list_insert(s->frames.prev, wl_resource_get_link(cb));
}
static void surface_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *region)
{
    (void)c, (void)r, (void)region;
}
static void surface_commit(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    struct surface *s = wl_resource_get_user_data(r);
    if (s->pending && s->pending != s->current) {
        if (s->current) {
            wl_resource_post_event(s->current, 0 /* release */);
            srv.releases++;
        }
        s->current = s->pending;
    }
    s->pending = NULL;
    srv.commits++;
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, &s->frames) {
        wl_resource_post_event(cb, 0 /* done */, 0u);
        wl_list_remove(wl_resource_get_link(cb));
        wl_resource_destroy(cb);
        srv.frames_done++;
    }
}
static void surface_int(struct wl_client *c, struct wl_resource *r, int32_t v)
{
    (void)c, (void)r, (void)v;
}
static void surface_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{
    (void)c, (void)r, (void)x, (void)y;
}
static const struct wl_surface_interface k_surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_region,
    .set_input_region = surface_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_int,
    .set_buffer_scale = surface_int,
    .damage_buffer = surface_damage,
    .offset = surface_offset,
};
static void surface_free(struct wl_resource *r)
{
    free(wl_resource_get_user_data(r));
}
static void compositor_create_surface(struct wl_client *client, struct wl_resource *r, uint32_t id)
{
    struct surface *s = calloc(1, sizeof *s);
    wl_list_init(&s->frames);
    struct wl_resource *res = wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(res, &k_surface_impl, s, surface_free);
}
static void compositor_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    (void)c, (void)r, (void)id;
}
static const struct wl_compositor_interface k_compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};
static void bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wl_compositor_interface, (int)version, id);
    wl_resource_set_implementation(r, &k_compositor_impl, NULL, NULL);
}

static void *server_main(void *arg)
{
    struct wl_display *display = arg;
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    while (!srv.stop) {
        wl_event_loop_dispatch(loop, 10);
        wl_display_flush_clients(display);
    }
    return NULL;
}

/* ---- the app ---- */

static struct wl_compositor *g_compositor;
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version)
{
    (void)data, (void)version;
    if (strcmp(iface, "wl_compositor") == 0)
        g_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data, (void)registry, (void)name;
}
static const struct wl_registry_listener k_registry_listener = {global, global_remove};

int main(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *socket_name = getenv("WAYLAND_DISPLAY");
    if (!runtime || !socket_name || !getenv("__EGL_VENDOR_LIBRARY_FILENAMES")) {
        fprintf(stderr, "run via run-host-test.sh\n");
        return 2;
    }
    *(void **)&client_ahb_from_fd = dlsym(RTLD_DEFAULT, "cham_client_ahb_from_fd");

    struct wl_display *server = wl_display_create();
    if (wl_display_add_socket(server, socket_name) != 0) {
        fprintf(stderr, "cannot create the Wayland socket\n");
        return 1;
    }
    wl_global_create(server, &wl_compositor_interface, 4, NULL, bind_compositor);
    wl_global_create(server, &k_dmabuf_iface, 3, NULL, bind_dmabuf);
    pthread_t thread;
    pthread_create(&thread, NULL, server_main, server);

    printf("GPU rendering for Wayland apps\n");
    /* libchameleon.so (as in KWin) opens its buffer socket next to KWin's. */
    char registry[256];
    snprintf(registry, sizeof registry, "%s/%s.chameleon", runtime, socket_name);
    struct stat st;
    for (int i = 0; i < 100 && stat(registry, &st) != 0; i++)
        usleep(20 * 1000);
    CHECK(client_ahb_from_fd && stat(registry, &st) == 0, "KWin side accepts app buffers at %s.chameleon",
          socket_name);

    struct wl_display *display = wl_display_connect(socket_name);
    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &k_registry_listener, NULL);
    wl_display_roundtrip(display);
    struct wl_surface *surface = wl_compositor_create_surface(g_compositor);
    struct wl_egl_window *window = wl_egl_window_create(surface, 64, 32);

    if (getenv("EXPECT_DESKTOP_GL_QT")) {
        /* fake_qtgui.so is preloaded: a Qt built for desktop OpenGL */
        const char *mode = getenv("CHAMELEON_EGL_QT");
        int mesa = mode && strcmp(mode, "mesa") == 0;
        printf("desktop-OpenGL Qt%s\n", mesa ? ", CHAMELEON_EGL_QT=mesa" : "");
        EGLDisplay qdpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
        if (mesa) {
            CHECK(qdpy == EGL_NO_DISPLAY, "its Wayland display is left to the other vendors (Mesa)");
        } else {
            eglInitialize(qdpy, NULL, NULL);
            const char *vendor = eglQueryString(qdpy, EGL_VENDOR);
            CHECK(qdpy != EGL_NO_DISPLAY && vendor && strstr(vendor, "NVIDIA") && strcmp(vendor, "NVIDIA") != 0,
                  "GPU display, EGL_VENDOR steers Qt to OpenGL ES: '%s'", vendor ? vendor : "(null)");
        }
        srv.stop = 1;
        pthread_join(thread, NULL);
        printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
        return failures != 0;
    }

    const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    CHECK(client_exts && strstr(client_exts, "EGL_KHR_platform_wayland"), "Wayland platform advertised");
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
    CHECK(dpy != EGL_NO_DISPLAY, "Wayland EGLDisplay from the Chameleon vendor");
    CHECK(eglInitialize(dpy, NULL, NULL), "eglInitialize");
    EGLConfig config;
    EGLint n = 0;
    const EGLint config_attribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
    eglChooseConfig(dpy, config_attribs, &config, 1, &n);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    EGLSurface surf = eglCreatePlatformWindowSurface(dpy, config, window, NULL);
    CHECK(surf != EGL_NO_SURFACE, "window surface for a wl_egl_window");
    CHECK(eglMakeCurrent(dpy, surf, surf, ctx), "eglMakeCurrent on it");
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    CHECK(w == 64 && h == 32, "eglQuerySurface: %dx%d", w, h);
    CHECK(eglSwapInterval(dpy, 1), "eglSwapInterval(1)");

    int ok = 1;
    for (int i = 0; i < 6; i++)
        ok &= eglSwapBuffers(dpy, surf) == EGL_TRUE;
    wl_display_roundtrip(display);
    CHECK(ok && srv.commits == 6, "6 swaps -> %d commits", srv.commits);
    CHECK(srv.buffers_created >= 2 && srv.buffers_created <= 3 && srv.buffers_unknown == 0 &&
              srv.buffers_known_to_kwin == srv.buffers_created,
          "%d zero-copy buffers, all registered with KWin (%d unknown)", srv.buffers_created, srv.buffers_unknown);
    CHECK(srv.last_width == 64 && srv.last_height == 32 && srv.last_stride == 256 &&
              srv.last_format == DRM_FORMAT_ABGR8888 && srv.last_modifier == DRM_FORMAT_MOD_INVALID,
          "dmabuf %dx%d stride %d, ABGR8888, implicit modifier", srv.last_width, srv.last_height, srv.last_stride);
    CHECK(srv.releases == 5 && srv.frames_done >= 5, "buffers released (%d) and frames paced (%d callbacks)",
          srv.releases, srv.frames_done);

    wl_egl_window_resize(window, 80, 40, 0, 0);
    eglMakeCurrent(dpy, surf, surf, ctx); /* where apps start drawing the next frame */
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    CHECK(w == 80 && h == 40, "resized wl_egl_window: surface now %dx%d", w, h);
    CHECK(eglSwapBuffers(dpy, surf), "swap after resize");
    wl_display_roundtrip(display);
    CHECK(srv.last_width == 80 && srv.last_height == 40 && srv.buffers_unknown == 0,
          "new buffers at %dx%d reach KWin", srv.last_width, srv.last_height);

    CHECK(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "release the surface");
    CHECK(eglDestroySurface(dpy, surf), "eglDestroySurface");
    wl_egl_window_destroy(window);
    wl_surface_destroy(surface);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);

    srv.stop = 1;
    pthread_join(thread, NULL);
    wl_display_destroy(server);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
