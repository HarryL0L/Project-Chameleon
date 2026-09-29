/*
 * The fake KMS device KWin drives with atomic mode setting.
 *
 * One connector (DSI-1, the Chameleon app's surface) -> one encoder -> one
 * CRTC -> one primary plane. Everything is answered at the ioctl level, so
 * the real libdrm on top of it behaves as with a kernel driver. An atomic
 * commit that puts a framebuffer on the plane becomes a PRESENT to the app;
 * the app's FRAME_DONE becomes the page-flip event.
 *
 * The connector's one mode is the app's surface size and refresh rate when
 * the device is first opened. KWin may set any other mode (output.c adds
 * custom modes to follow the app's window as it rotates or resizes); the app
 * scales or crops whatever size arrives.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

#include "internal.h"

#define ID_CONNECTOR 31
#define ID_ENCODER 32
#define ID_CRTC 33
#define ID_PLANE 34

enum {
    P_CONN_CRTC_ID = 100,
    P_CONN_DPMS,
    P_CONN_NON_DESKTOP,
    P_CRTC_ACTIVE,
    P_CRTC_MODE_ID,
    P_PLANE_TYPE,
    P_PLANE_FB_ID,
    P_PLANE_CRTC_ID,
    P_SRC_X,
    P_SRC_Y,
    P_SRC_W,
    P_SRC_H,
    P_CRTC_X,
    P_CRTC_Y,
    P_CRTC_W,
    P_CRTC_H,
    P_IN_FENCE_FD,
    P_END,
};

struct propdef {
    const char *name;
    uint32_t flags;
    uint64_t values[2]; /* range min/max, or object type */
    int nvalues;
    const char *const *enums; /* enum value i has name enums[i] */
    int nenums;
};

static const char *const k_dpms[] = {"On", "Standby", "Suspend", "Off"};
static const char *const k_plane_type[] = {"Overlay", "Primary", "Cursor"};

#define RANGE(lo, hi) DRM_MODE_PROP_RANGE, {(lo), (hi)}, 2
#define SRANGE(lo, hi) DRM_MODE_PROP_SIGNED_RANGE, {(uint64_t)(int64_t)(lo), (uint64_t)(int64_t)(hi)}, 2
#define ATOMIC DRM_MODE_PROP_ATOMIC

static const struct propdef k_props[P_END - 100] = {
    [P_CONN_CRTC_ID - 100] = {"CRTC_ID", DRM_MODE_PROP_OBJECT | ATOMIC, {DRM_MODE_OBJECT_CRTC}, 1},
    [P_CONN_DPMS - 100] = {"DPMS", DRM_MODE_PROP_ENUM, {0}, 0, k_dpms, 4},
    [P_CONN_NON_DESKTOP - 100] = {"non-desktop", DRM_MODE_PROP_IMMUTABLE | RANGE(0, 1)},
    [P_CRTC_ACTIVE - 100] = {"ACTIVE", ATOMIC | RANGE(0, 1)},
    [P_CRTC_MODE_ID - 100] = {"MODE_ID", DRM_MODE_PROP_BLOB | ATOMIC, {0}, 0},
    [P_PLANE_TYPE - 100] = {"type", DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE, {0}, 0, k_plane_type, 3},
    [P_PLANE_FB_ID - 100] = {"FB_ID", DRM_MODE_PROP_OBJECT | ATOMIC, {DRM_MODE_OBJECT_FB}, 1},
    [P_PLANE_CRTC_ID - 100] = {"CRTC_ID", DRM_MODE_PROP_OBJECT | ATOMIC, {DRM_MODE_OBJECT_CRTC}, 1},
    [P_SRC_X - 100] = {"SRC_X", ATOMIC | RANGE(0, UINT32_MAX)},
    [P_SRC_Y - 100] = {"SRC_Y", ATOMIC | RANGE(0, UINT32_MAX)},
    [P_SRC_W - 100] = {"SRC_W", ATOMIC | RANGE(0, UINT32_MAX)},
    [P_SRC_H - 100] = {"SRC_H", ATOMIC | RANGE(0, UINT32_MAX)},
    [P_CRTC_X - 100] = {"CRTC_X", ATOMIC | SRANGE(INT32_MIN, INT32_MAX)},
    [P_CRTC_Y - 100] = {"CRTC_Y", ATOMIC | SRANGE(INT32_MIN, INT32_MAX)},
    [P_CRTC_W - 100] = {"CRTC_W", ATOMIC | RANGE(0, INT32_MAX)},
    [P_CRTC_H - 100] = {"CRTC_H", ATOMIC | RANGE(0, INT32_MAX)},
    [P_IN_FENCE_FD - 100] = {"IN_FENCE_FD", ATOMIC | SRANGE(-1, INT32_MAX)},
};

static const uint32_t k_conn_props[] = {P_CONN_CRTC_ID, P_CONN_DPMS, P_CONN_NON_DESKTOP};
static const uint32_t k_crtc_props[] = {P_CRTC_ACTIVE, P_CRTC_MODE_ID};
static const uint32_t k_plane_props[] = {P_PLANE_TYPE, P_PLANE_FB_ID, P_PLANE_CRTC_ID, P_SRC_X, P_SRC_Y,
                                         P_SRC_W, P_SRC_H, P_CRTC_X, P_CRTC_Y, P_CRTC_W, P_CRTC_H,
                                         P_IN_FENCE_FD};

struct kms_state {
    uint32_t conn_crtc;
    uint64_t dpms;
    uint64_t active;
    uint32_t mode_blob;
    uint32_t fb;
    uint32_t plane_crtc;
    uint32_t src_x, src_y, src_w, src_h;
    int32_t crtc_x, crtc_y;
    uint32_t crtc_w, crtc_h;
    int64_t in_fence;
};

struct fb {
    uint32_t id;
    struct cham_bo *bo; /* holds a ref */
    uint32_t width, height, format;
    struct fb *next;
};

struct blob {
    uint32_t id;
    uint32_t length;
    void *data;
    struct blob *next;
};

static struct kms_state g_state = {.in_fence = -1};
static struct cham_bo *g_plane_bo; /* what's on screen; holds a ref */
static struct fb *g_fbs;
static struct blob *g_blobs;
static uint32_t g_next_object_id = 1000;
static uint64_t g_frame;

static struct drm_mode_modeinfo g_mode;
static uint32_t g_mode_mhz;
static uint32_t g_mm_width, g_mm_height;

/* ---- setup ---- */

static void make_mode(uint32_t w, uint32_t h, uint32_t mhz)
{
    struct drm_mode_modeinfo *m = &g_mode;
    memset(m, 0, sizeof *m);
    m->hdisplay = (uint16_t)w;
    m->hsync_start = (uint16_t)(w + 48);
    m->hsync_end = (uint16_t)(w + 80);
    m->htotal = (uint16_t)(w + 160);
    m->vdisplay = (uint16_t)h;
    m->vsync_start = (uint16_t)(h + 3);
    m->vsync_end = (uint16_t)(h + 13);
    m->vtotal = (uint16_t)(h + 40);
    m->clock = (uint32_t)(((uint64_t)m->htotal * m->vtotal * mhz + 500000) / 1000000); /* kHz */
    m->vrefresh = (mhz + 500) / 1000;
    m->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    m->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
    snprintf(m->name, sizeof m->name, "%ux%u", w, h);
    g_mode_mhz = mhz;
}

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void kms_init(void)
{
    uint32_t w = 0, h = 0, mhz = 0;
    link_start();
    const char *wait = getenv("CHAMELEON_WAIT");
    int timeout_ms = (wait ? atoi(wait) : 30) * 1000;
    cham_log("waiting up to %d s for the Chameleon app to report its screen", timeout_ms / 1000);
    if (link_wait_config(&w, &h, &mhz, timeout_ms)) {
        cham_log("screen %ux%u @ %.2f Hz", w, h, mhz / 1000.0);
    } else {
        const char *mode = getenv("CHAMELEON_MODE");
        double hz = 60;
        if (!mode || sscanf(mode, "%ux%u@%lf", &w, &h, &hz) < 2)
            w = 1080, h = 1920;
        mhz = (uint32_t)(hz * 1000);
        cham_log("no app yet; using %ux%u @ %.2f Hz (set CHAMELEON_MODE=WxH@Hz to change)", w, h, hz);
    }
    make_mode(w, h, mhz);

    /* Physical size decides KWin's default scale; phones are ~400 dpi. */
    const char *dpi_env = getenv("CHAMELEON_DPI");
    double dpi = dpi_env ? atof(dpi_env) : 400.0;
    if (dpi < 50)
        dpi = 400.0;
    g_mm_width = (uint32_t)(w * 25.4 / dpi + 0.5);
    g_mm_height = (uint32_t)(h * 25.4 / dpi + 0.5);
}

void kms_init_once(void)
{
    pthread_once(&g_init_once, kms_init);
}

/* ---- helpers ---- */

#define U2P(u) ((void *)(uintptr_t)(u))

static struct fb *fb_find(uint32_t id)
{
    for (struct fb *f = g_fbs; f; f = f->next)
        if (f->id == id)
            return f;
    return NULL;
}

static struct blob *blob_find(uint32_t id)
{
    for (struct blob *b = g_blobs; b; b = b->next)
        if (b->id == id)
            return b;
    return NULL;
}

static uint64_t prop_value(uint32_t obj, uint32_t prop)
{
    const struct kms_state *s = &g_state;
    switch (prop) {
    case P_CONN_CRTC_ID: return s->conn_crtc;
    case P_CONN_DPMS: return s->dpms;
    case P_CONN_NON_DESKTOP: return 0;
    case P_CRTC_ACTIVE: return s->active;
    case P_CRTC_MODE_ID: return s->mode_blob;
    case P_PLANE_TYPE: return 1; /* Primary */
    case P_PLANE_FB_ID: return s->fb;
    case P_PLANE_CRTC_ID: return s->plane_crtc;
    case P_SRC_X: return s->src_x;
    case P_SRC_Y: return s->src_y;
    case P_SRC_W: return s->src_w;
    case P_SRC_H: return s->src_h;
    case P_CRTC_X: return (uint64_t)(int64_t)s->crtc_x;
    case P_CRTC_Y: return (uint64_t)(int64_t)s->crtc_y;
    case P_CRTC_W: return s->crtc_w;
    case P_CRTC_H: return s->crtc_h;
    case P_IN_FENCE_FD: return (uint64_t)(int64_t)-1;
    }
    (void)obj;
    return 0;
}

static const uint32_t *object_props(uint32_t obj, uint32_t *type, uint32_t *count)
{
    switch (obj) {
    case ID_CONNECTOR:
        *type = DRM_MODE_OBJECT_CONNECTOR;
        *count = sizeof k_conn_props / sizeof k_conn_props[0];
        return k_conn_props;
    case ID_CRTC:
        *type = DRM_MODE_OBJECT_CRTC;
        *count = sizeof k_crtc_props / sizeof k_crtc_props[0];
        return k_crtc_props;
    case ID_PLANE:
        *type = DRM_MODE_OBJECT_PLANE;
        *count = sizeof k_plane_props / sizeof k_plane_props[0];
        return k_plane_props;
    }
    return NULL;
}

/* Copies an array out if the caller's buffer is big enough (kernel rules),
 * and always reports the real count. */
#define COPY_OUT(ptr, user_count, src, n)                                  \
    do {                                                                   \
        if ((ptr) && (user_count) >= (n) && (n))                           \
            memcpy(U2P(ptr), (src), sizeof((src)[0]) * (n));               \
        (user_count) = (n);                                                \
    } while (0)

/* ---- mode objects ---- */

static int get_resources(struct drm_mode_card_res *r)
{
    static const uint32_t crtcs[] = {ID_CRTC}, conns[] = {ID_CONNECTOR}, encs[] = {ID_ENCODER};
    r->count_fbs = 0;
    COPY_OUT(r->crtc_id_ptr, r->count_crtcs, crtcs, 1u);
    COPY_OUT(r->connector_id_ptr, r->count_connectors, conns, 1u);
    COPY_OUT(r->encoder_id_ptr, r->count_encoders, encs, 1u);
    r->min_width = r->min_height = 1;
    r->max_width = r->max_height = 16384;
    return 0;
}

static int get_connector(struct drm_mode_get_connector *c)
{
    if (c->connector_id != ID_CONNECTOR)
        return -ENOENT;
    static const uint32_t encs[] = {ID_ENCODER};
    uint64_t values[3];
    for (int i = 0; i < 3; i++)
        values[i] = prop_value(ID_CONNECTOR, k_conn_props[i]);
    COPY_OUT(c->modes_ptr, c->count_modes, &g_mode, 1u);
    COPY_OUT(c->encoders_ptr, c->count_encoders, encs, 1u);
    uint32_t n = 3;
    if (c->props_ptr && c->prop_values_ptr && c->count_props >= n) {
        memcpy(U2P(c->props_ptr), k_conn_props, sizeof k_conn_props);
        memcpy(U2P(c->prop_values_ptr), values, sizeof values);
    }
    c->count_props = n;
    c->encoder_id = ID_ENCODER;
    c->connector_type = DRM_MODE_CONNECTOR_DSI;
    c->connector_type_id = 1;
    c->connection = 1; /* DRM_MODE_CONNECTED */
    c->mm_width = g_mm_width;
    c->mm_height = g_mm_height;
    c->subpixel = 1; /* DRM_MODE_SUBPIXEL_UNKNOWN */
    return 0;
}

static int get_encoder(struct drm_mode_get_encoder *e)
{
    if (e->encoder_id != ID_ENCODER)
        return -ENOENT;
    e->encoder_type = DRM_MODE_ENCODER_DSI;
    e->crtc_id = g_state.conn_crtc;
    e->possible_crtcs = 1;
    e->possible_clones = 0;
    return 0;
}

static int get_crtc(struct drm_mode_crtc *c)
{
    if (c->crtc_id != ID_CRTC)
        return -ENOENT;
    c->fb_id = g_state.fb;
    c->x = c->y = 0;
    c->gamma_size = 0;
    struct blob *b = blob_find(g_state.mode_blob);
    c->mode_valid = g_state.active && b && b->length == sizeof(struct drm_mode_modeinfo);
    if (c->mode_valid)
        memcpy(&c->mode, b->data, sizeof c->mode);
    else
        memset(&c->mode, 0, sizeof c->mode);
    return 0;
}

static int get_plane_resources(struct drm_mode_get_plane_res *r)
{
    static const uint32_t planes[] = {ID_PLANE};
    COPY_OUT(r->plane_id_ptr, r->count_planes, planes, 1u);
    return 0;
}

static int get_plane(struct drm_mode_get_plane *p)
{
    if (p->plane_id != ID_PLANE)
        return -ENOENT;
    int n;
    const uint32_t *formats = cham_formats(&n);
    p->crtc_id = g_state.plane_crtc;
    p->fb_id = g_state.fb;
    p->possible_crtcs = 1;
    p->gamma_size = 0;
    COPY_OUT(p->format_type_ptr, p->count_format_types, formats, (uint32_t)n);
    return 0;
}

/* ---- properties ---- */

static int obj_get_properties(struct drm_mode_obj_get_properties *o)
{
    uint32_t type, n;
    const uint32_t *props = object_props(o->obj_id, &type, &n);
    if (!props || (o->obj_type && o->obj_type != type))
        return -ENOENT;
    if (o->props_ptr && o->prop_values_ptr && o->count_props >= n) {
        uint32_t *ids = U2P(o->props_ptr);
        uint64_t *values = U2P(o->prop_values_ptr);
        for (uint32_t i = 0; i < n; i++) {
            ids[i] = props[i];
            values[i] = prop_value(o->obj_id, props[i]);
        }
    }
    o->count_props = n;
    return 0;
}

static int get_property(struct drm_mode_get_property *p)
{
    if (p->prop_id < 100 || p->prop_id >= P_END)
        return -ENOENT;
    const struct propdef *d = &k_props[p->prop_id - 100];
    p->flags = d->flags;
    snprintf(p->name, sizeof p->name, "%s", d->name);
    if (d->flags & DRM_MODE_PROP_ENUM) {
        uint32_t n = (uint32_t)d->nenums;
        if (p->values_ptr && p->count_values >= n)
            for (uint32_t i = 0; i < n; i++)
                ((uint64_t *)U2P(p->values_ptr))[i] = i;
        if (p->enum_blob_ptr && p->count_enum_blobs >= n) {
            struct drm_mode_property_enum *e = U2P(p->enum_blob_ptr);
            for (uint32_t i = 0; i < n; i++) {
                e[i].value = i;
                snprintf(e[i].name, sizeof e[i].name, "%s", d->enums[i]);
            }
        }
        p->count_values = n;
        p->count_enum_blobs = n;
    } else {
        uint32_t n = (uint32_t)d->nvalues;
        COPY_OUT(p->values_ptr, p->count_values, d->values, n);
        p->count_enum_blobs = 0;
    }
    return 0;
}

static int get_blob(struct drm_mode_get_blob *g)
{
    struct blob *b = blob_find(g->blob_id);
    if (!b)
        return -ENOENT;
    if (g->data && g->length >= b->length)
        memcpy(U2P(g->data), b->data, b->length);
    g->length = b->length;
    return 0;
}

static int create_blob(struct drm_mode_create_blob *c)
{
    if (!c->length || c->length > (1u << 20))
        return -EINVAL;
    struct blob *b = calloc(1, sizeof *b);
    b->data = malloc(c->length);
    memcpy(b->data, U2P(c->data), c->length);
    b->length = c->length;
    b->id = g_next_object_id++;
    b->next = g_blobs;
    g_blobs = b;
    c->blob_id = b->id;
    return 0;
}

static int destroy_blob(struct drm_mode_destroy_blob *d)
{
    for (struct blob **p = &g_blobs; *p; p = &(*p)->next) {
        if ((*p)->id == d->blob_id) {
            struct blob *b = *p;
            *p = b->next;
            free(b->data);
            free(b);
            return 0;
        }
    }
    return -ENOENT;
}

/* ---- framebuffers & buffers ---- */

static int add_fb2(struct drm_mode_fb_cmd2 *c)
{
    struct cham_bo *bo = bo_by_handle_locked(c->handles[0]);
    if (!bo)
        return -ENOENT;
    if (c->width > bo->width || c->height > bo->height)
        return -EINVAL;
    struct fb *f = calloc(1, sizeof *f);
    f->id = g_next_object_id++;
    f->bo = bo;
    f->width = c->width;
    f->height = c->height;
    f->format = c->pixel_format;
    bo_ref_locked(bo);
    f->next = g_fbs;
    g_fbs = f;
    c->fb_id = f->id;
    return 0;
}

static int remove_fb(uint32_t id)
{
    for (struct fb **p = &g_fbs; *p; p = &(*p)->next) {
        if ((*p)->id == id) {
            struct fb *f = *p;
            *p = f->next;
            bo_unref_locked(f->bo); /* the plane keeps its own ref */
            free(f);
            return 0;
        }
    }
    return -ENOENT;
}

static int prime_fd_to_handle(struct drm_prime_handle *p)
{
    /* cham_bo_from_fd takes the lock itself. */
    pthread_mutex_unlock(&g_lock);
    struct cham_bo *bo = cham_bo_from_fd(p->fd);
    pthread_mutex_lock(&g_lock);
    if (!bo)
        return -EINVAL; /* not one of ours, e.g. a client buffer: no direct scanout */
    p->handle = bo->handle;
    return 0;
}

static int prime_handle_to_fd(struct drm_prime_handle *p)
{
    struct cham_bo *bo = bo_by_handle_locked(p->handle);
    if (!bo)
        return -ENOENT;
    p->fd = cham_bo_export_fd(bo);
    return p->fd < 0 ? -errno : 0;
}

/* ---- atomic ---- */

static int set_prop(struct kms_state *s, uint32_t obj, uint32_t prop, uint64_t v)
{
    switch (obj) {
    case ID_CONNECTOR:
        if (prop == P_CONN_CRTC_ID && (v == 0 || v == ID_CRTC)) {
            s->conn_crtc = (uint32_t)v;
            return 0;
        }
        if (prop == P_CONN_DPMS && v <= 3) {
            s->dpms = v;
            return 0;
        }
        return -EINVAL;
    case ID_CRTC:
        if (prop == P_CRTC_ACTIVE && v <= 1) {
            s->active = v;
            return 0;
        }
        if (prop == P_CRTC_MODE_ID) {
            struct blob *b = v ? blob_find((uint32_t)v) : NULL;
            if (v && (!b || b->length != sizeof(struct drm_mode_modeinfo)))
                return -EINVAL;
            s->mode_blob = (uint32_t)v;
            return 0;
        }
        return -EINVAL;
    case ID_PLANE:
        switch (prop) {
        case P_PLANE_FB_ID:
            if (v && !fb_find((uint32_t)v))
                return -ENOENT;
            s->fb = (uint32_t)v;
            return 0;
        case P_PLANE_CRTC_ID:
            if (v != 0 && v != ID_CRTC)
                return -EINVAL;
            s->plane_crtc = (uint32_t)v;
            return 0;
        case P_SRC_X: s->src_x = (uint32_t)v; return 0;
        case P_SRC_Y: s->src_y = (uint32_t)v; return 0;
        case P_SRC_W: s->src_w = (uint32_t)v; return 0;
        case P_SRC_H: s->src_h = (uint32_t)v; return 0;
        case P_CRTC_X: s->crtc_x = (int32_t)v; return 0;
        case P_CRTC_Y: s->crtc_y = (int32_t)v; return 0;
        case P_CRTC_W: s->crtc_w = (uint32_t)v; return 0;
        case P_CRTC_H: s->crtc_h = (uint32_t)v; return 0;
        case P_IN_FENCE_FD: s->in_fence = (int64_t)(int32_t)v; return 0;
        }
        return -EINVAL;
    }
    return -ENOENT;
}

static const struct drm_mode_modeinfo *state_mode(const struct kms_state *s)
{
    struct blob *b = s->mode_blob ? blob_find(s->mode_blob) : NULL;
    return b ? b->data : NULL;
}

static int atomic_commit(struct fake_fd *f, struct drm_mode_atomic *a)
{
    if (a->flags & ~(uint32_t)DRM_MODE_ATOMIC_FLAGS)
        return -EINVAL;
    if (a->flags & DRM_MODE_PAGE_FLIP_ASYNC)
        return -EINVAL; /* no tearing updates */

    struct kms_state s = g_state;
    s.in_fence = -1;
    /* A refused real commit is a bug somewhere; say why (tests fail often). */
    static int refusals;
#define REFUSE(err, ...)                                                                 \
    do {                                                                                 \
        if (!(a->flags & DRM_MODE_ATOMIC_TEST_ONLY) && refusals < 20) {                 \
            refusals++;                                                                  \
            cham_log("refusing KWin's commit: " __VA_ARGS__);                            \
        }                                                                                \
        return (err);                                                                    \
    } while (0)

    const uint32_t *objs = U2P(a->objs_ptr), *counts = U2P(a->count_props_ptr), *props = U2P(a->props_ptr);
    const uint64_t *values = U2P(a->prop_values_ptr);
    uint64_t k = 0;
    for (uint32_t i = 0; i < a->count_objs; i++) {
        for (uint32_t j = 0; j < counts[i]; j++, k++) {
            int ret = set_prop(&s, objs[i], props[k], values[k]);
            if (ret)
                REFUSE(ret, "object %u property %u = %llu", objs[i], props[k], (unsigned long long)values[k]);
        }
    }

    const struct drm_mode_modeinfo *mode = state_mode(&s);
    const struct drm_mode_modeinfo *old_mode = state_mode(&g_state);
    int on = s.active && mode;
    if (mode && (!mode->hdisplay || !mode->vdisplay || mode->hdisplay > 16384 || mode->vdisplay > 16384))
        REFUSE(-EINVAL, "mode %ux%u", mode->hdisplay, mode->vdisplay);
    if (s.active && !mode)
        REFUSE(-EINVAL, "active CRTC without a mode");
    int modeset = s.active != g_state.active || (!mode != !old_mode) ||
                  (mode && old_mode && memcmp(mode, old_mode, sizeof *mode));
    if (modeset && !(a->flags & DRM_MODE_ATOMIC_ALLOW_MODESET))
        REFUSE(-EINVAL, "modeset without ALLOW_MODESET");
    if (s.fb && !fb_find(s.fb) && s.fb != g_state.fb)
        REFUSE(-ENOENT, "unknown framebuffer %u", s.fb);
    /* A plane bigger than the CRTC is clipped, as with real drivers: KWin's
     * first commit after a mode change can still carry the previous frame's
     * size. A smaller one is only KWin putting its cursor alone on an empty
     * screen (nothing else ever reaches this plane: client buffers can't be
     * scanned out); it is accepted but not shown, since the app shows whole
     * buffers - the previous frame stays until a full one comes. */
    int partial = 0;
    if (s.fb) {
        if (!on || s.plane_crtc != ID_CRTC)
            REFUSE(-EINVAL, "framebuffer on a CRTC that is off");
        partial = s.crtc_x > 0 || s.crtc_y > 0 || (int64_t)s.crtc_x + s.crtc_w < mode->hdisplay ||
                  (int64_t)s.crtc_y + s.crtc_h < mode->vdisplay;
    } else if (s.plane_crtc) {
        REFUSE(-EINVAL, "plane on a CRTC without a framebuffer");
    }
    if (a->flags & DRM_MODE_PAGE_FLIP_EVENT) {
        if (!on)
            REFUSE(-EINVAL, "page-flip event with the CRTC off");
        if (link_flip_pending_locked())
            return -EBUSY;
    }
#undef REFUSE
    if (a->flags & DRM_MODE_ATOMIC_TEST_ONLY)
        return 0;

    /* Commit. The kernel doesn't take ownership of IN_FENCE_FD either. */
    int in_fence = (int)s.in_fence;
    s.in_fence = -1;
    /* CLOSEFB may have dropped the id of the fb that's still on the plane. */
    struct fb *fb = s.fb ? fb_find(s.fb) : NULL;
    struct cham_bo *bo = fb ? fb->bo : (s.fb && s.fb == g_state.fb ? g_plane_bo : NULL);
    if (bo)
        bo_ref_locked(bo);
    if (g_plane_bo)
        bo_unref_locked(g_plane_bo);
    g_plane_bo = bo;
    g_state = s;

    if (modeset && mode && (!old_mode || mode->hdisplay != old_mode->hdisplay ||
                            mode->vdisplay != old_mode->vdisplay))
        cham_log("KWin set mode %ux%u", mode->hdisplay, mode->vdisplay);

    uint64_t frame = ++g_frame;
    int presented = 0;
    if (bo && on && s.dpms == 0 && !partial)
        presented = link_present_locked(bo, in_fence, frame);
    if (a->flags & DRM_MODE_PAGE_FLIP_EVENT)
        link_queue_flip_locked(a->user_data, ID_CRTC, f->event_wfd, frame, presented, g_mode_mhz);
    return 0;
}

/* ---- misc ---- */

static int get_cap(struct drm_get_cap *c)
{
    switch (c->capability) {
    case DRM_CAP_DUMB_BUFFER: c->value = 0; return 0;
    case DRM_CAP_VBLANK_HIGH_CRTC: c->value = 1; return 0;
    case DRM_CAP_DUMB_PREFERRED_DEPTH: c->value = 24; return 0;
    case DRM_CAP_DUMB_PREFER_SHADOW: c->value = 0; return 0;
    case DRM_CAP_PRIME: c->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; return 0;
    case DRM_CAP_TIMESTAMP_MONOTONIC: c->value = 1; return 0;
    case DRM_CAP_ASYNC_PAGE_FLIP: c->value = 0; return 0;
    case DRM_CAP_CURSOR_WIDTH: c->value = 64; return 0;
    case DRM_CAP_CURSOR_HEIGHT: c->value = 64; return 0;
    /* Gralloc picks the layout (maybe AFBC); expose implicit modifiers only. */
    case DRM_CAP_ADDFB2_MODIFIERS: c->value = 0; return 0;
    case DRM_CAP_PAGE_FLIP_TARGET: c->value = 0; return 0;
    case DRM_CAP_CRTC_IN_VBLANK_EVENT: c->value = 1; return 0;
    case DRM_CAP_SYNCOBJ: c->value = 0; return 0;
    case DRM_CAP_SYNCOBJ_TIMELINE: c->value = 0; return 0;
    case DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP: c->value = 0; return 0;
    }
    return -EINVAL;
}

static int set_client_cap(struct drm_set_client_cap *c)
{
    switch (c->capability) {
    case DRM_CLIENT_CAP_STEREO_3D:
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
    case DRM_CLIENT_CAP_ATOMIC:
    case DRM_CLIENT_CAP_ASPECT_RATIO:
        return c->value <= 1 ? 0 : -EINVAL;
    }
    return -EINVAL; /* writeback, cursor hotspot, color pipeline */
}

static void copy_string(char *dst, __kernel_size_t *len, const char *src)
{
    size_t n = strlen(src);
    if (dst && *len)
        memcpy(dst, src, n < *len ? n : *len);
    *len = n;
}

static int get_version(struct drm_version *v)
{
    v->version_major = 1;
    v->version_minor = 0;
    v->version_patchlevel = 0;
    copy_string(v->name, &v->name_len, "chameleon");
    copy_string(v->date, &v->date_len, "20260929");
    copy_string(v->desc, &v->desc_len, "Chameleon: KMS on an Android surface");
    return 0;
}

int kms_ioctl(struct fake_fd *f, unsigned int request, void *arg)
{
    static int unknown_warnings;
    int ret;
    pthread_mutex_lock(&g_lock);
    switch (request) {
    case (unsigned int)DRM_IOCTL_VERSION: ret = get_version(arg); break;
    case (unsigned int)DRM_IOCTL_GET_CAP: ret = get_cap(arg); break;
    case (unsigned int)DRM_IOCTL_SET_CLIENT_CAP: ret = set_client_cap(arg); break;
    case (unsigned int)DRM_IOCTL_GET_MAGIC: ((struct drm_auth *)arg)->magic = 0x4348; ret = 0; break;
    case (unsigned int)DRM_IOCTL_AUTH_MAGIC: ret = 0; break;
    case (unsigned int)DRM_IOCTL_SET_MASTER:
    case (unsigned int)DRM_IOCTL_DROP_MASTER: ret = 0; break;
    case (unsigned int)DRM_IOCTL_GET_UNIQUE: ((struct drm_unique *)arg)->unique_len = 0; ret = 0; break;
    case (unsigned int)DRM_IOCTL_MODE_GETRESOURCES: ret = get_resources(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_GETCONNECTOR: ret = get_connector(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_GETENCODER: ret = get_encoder(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_GETCRTC: ret = get_crtc(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_GETPLANERESOURCES: ret = get_plane_resources(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_GETPLANE: ret = get_plane(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_OBJ_GETPROPERTIES: ret = obj_get_properties(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_GETPROPERTY: ret = get_property(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_GETPROPBLOB: ret = get_blob(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_CREATEPROPBLOB: ret = create_blob(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_DESTROYPROPBLOB: ret = destroy_blob(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_ADDFB2: ret = add_fb2(arg); break;
    case (unsigned int)DRM_IOCTL_MODE_RMFB: ret = remove_fb(*(uint32_t *)arg); break;
    case (unsigned int)DRM_IOCTL_MODE_CLOSEFB: ret = remove_fb(((struct drm_mode_closefb *)arg)->fb_id); break;
    case (unsigned int)DRM_IOCTL_PRIME_FD_TO_HANDLE: ret = prime_fd_to_handle(arg); break;
    case (unsigned int)DRM_IOCTL_PRIME_HANDLE_TO_FD: ret = prime_handle_to_fd(arg); break;
    case (unsigned int)DRM_IOCTL_GEM_CLOSE: ret = 0; break; /* handles are the bo's own ids */
    case (unsigned int)DRM_IOCTL_MODE_ATOMIC: ret = atomic_commit(f, arg); break;
    case (unsigned int)DRM_IOCTL_MODE_LIST_LESSEES: ((struct drm_mode_list_lessees *)arg)->count_lessees = 0; ret = 0; break;
    default:
        if (unknown_warnings++ < 20)
            cham_log("unsupported DRM ioctl 0x%08x (nr 0x%02x)", request, _IOC_NR(request));
        ret = -EINVAL;
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return ret;
}
