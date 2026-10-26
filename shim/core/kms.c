/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The fake KMS device KWin drives with atomic mode setting, in
 * chameleon-kernel (card.c serves it to every process).
 *
 * One connector (DSI-1, the Chameleon app's surface) -> one encoder -> one
 * CRTC -> one primary plane. Everything is answered at the ioctl level, so
 * the real libdrm on top of it behaves as with a kernel driver. An atomic
 * commit that puts a framebuffer on the plane becomes a PRESENT to the app;
 * the app's FRAME_DONE becomes the page-flip event.
 *
 * Each open file has the kernel's per-file state (Linux 6.8, checked against
 * vkms with shim/test/refvm.sh): the first opener while there is no DRM
 * master becomes master and only it may commit; client caps, GEM handles,
 * framebuffers and property blobs belong to the file, and go when it closes.
 *
 * The connector's one mode is the app's surface: its size and the display's
 * refresh rate. When the window changes (the phone rotates, the keyboard
 * opens, battery saver drops to 60 Hz) the mode changes with it and the
 * card sends a hotplug uevent (HOTPLUG=1), as a monitor's driver does when
 * the monitor's modes change; the compositor re-reads the connector and
 * switches to the new mode. The EDID describes the panel as first seen (a
 * standard EDID 1.4 block: name, physical size, that mode as its preferred
 * timing) and stays the same, so the compositor keeps seeing the same
 * monitor. Any other mode may be set too; the app scales or crops whatever
 * size arrives.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

#include "internal.h"

#define ID_CONNECTOR 31
#define ID_ENCODER 32
#define ID_CRTC 33
#define ID_PLANE 34
#define ID_EDID_BLOB 35

enum {
    P_CONN_CRTC_ID = 100,
    P_CONN_DPMS,
    P_CONN_NON_DESKTOP,
    P_CONN_EDID,
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
    [P_CONN_EDID - 100] = {"EDID", DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE, {0}, 0},
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

static const uint32_t k_conn_props[] = {P_CONN_CRTC_ID, P_CONN_DPMS, P_CONN_NON_DESKTOP, P_CONN_EDID};
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
    struct fake_fd *owner; /* the file that created it; NULL once it let go (CLOSEFB) */
    struct fb *next;
};

struct blob {
    uint32_t id;
    uint32_t length;
    void *data;
    struct fake_fd *owner; /* NULL: the card's own (EDID) */
    int destroyed; /* the client let go of it; kept while the CRTC uses it, as the kernel does */
    struct blob *next;
};

static struct kms_state g_state = {.in_fence = -1};
static struct cham_bo *g_plane_bo; /* what's on screen; holds a ref */
static int g_plane_shown;           /* ...and it fills the active CRTC */
static int g_screen_off;            /* KWin turned its screen off (idle, DPMS) */
static int g_screen_was_on;         /* ...after it had been on */
static struct fb *g_fbs;
static struct blob *g_blobs;
static uint32_t g_next_object_id = 1000;
static uint64_t g_frame;
static struct fake_fd *g_files;     /* every open file (kms_next) */
static struct fake_fd *g_master;    /* the file that is DRM master, if any */
static uint32_t g_next_magic = 1;

static struct drm_mode_modeinfo g_mode;
static uint32_t g_mode_mhz;
static uint8_t g_edid[128];
static struct blob g_edid_blob = {.id = ID_EDID_BLOB, .length = sizeof g_edid, .data = g_edid};
static uint32_t g_screen_width, g_screen_height, g_mm_width, g_mm_height;

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

/* An EDID 1.4 base block for the panel: vendor "CML" (not a registered PNP
 * ID), name "Chameleon", physical size, sRGB, and g_mode as the preferred
 * detailed timing. */
static void make_edid(uint32_t mm_w, uint32_t mm_h)
{
    static const uint8_t header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    static const uint8_t srgb[] = {0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54};
    const struct drm_mode_modeinfo *m = &g_mode;
    uint8_t *e = g_edid;
    memset(e, 0, sizeof g_edid);
    memcpy(e, header, sizeof header);
    uint16_t vendor = (uint16_t)(('C' - '@') << 10 | ('M' - '@') << 5 | ('L' - '@'));
    e[8] = (uint8_t)(vendor >> 8);
    e[9] = (uint8_t)vendor;
    e[10] = 1;                /* product code */
    e[17] = 2026 - 1990;      /* model year */
    e[18] = 1, e[19] = 4;     /* EDID 1.4 */
    e[20] = 0x80;             /* digital input */
    e[21] = (uint8_t)(mm_w + 5 < 2550 ? (mm_w + 5) / 10 : 255); /* cm */
    e[22] = (uint8_t)(mm_h + 5 < 2550 ? (mm_h + 5) / 10 : 255);
    e[23] = 120;              /* gamma 2.2 */
    e[24] = 0x06;             /* sRGB, RGB 4:4:4, preferred timing is native */
    memcpy(e + 25, srgb, sizeof srgb);
    memset(e + 38, 0x01, 16); /* no standard timings */

    uint8_t *t = e + 54;      /* detailed timing: the mode */
    uint32_t clock = m->clock / 10 < 0xffff ? m->clock / 10 : 0xffff; /* 10 kHz */
    uint32_t hblank = m->htotal - m->hdisplay, vblank = m->vtotal - m->vdisplay;
    uint32_t hso = m->hsync_start - m->hdisplay, hsw = m->hsync_end - m->hsync_start;
    uint32_t vso = m->vsync_start - m->vdisplay, vsw = m->vsync_end - m->vsync_start;
    t[0] = (uint8_t)clock, t[1] = (uint8_t)(clock >> 8);
    t[2] = (uint8_t)m->hdisplay, t[3] = (uint8_t)hblank;
    t[4] = (uint8_t)((m->hdisplay >> 8) << 4 | (hblank >> 8));
    t[5] = (uint8_t)m->vdisplay, t[6] = (uint8_t)vblank;
    t[7] = (uint8_t)((m->vdisplay >> 8) << 4 | (vblank >> 8));
    t[8] = (uint8_t)hso, t[9] = (uint8_t)hsw;
    t[10] = (uint8_t)((vso & 15) << 4 | (vsw & 15));
    t[11] = (uint8_t)((hso >> 8) << 6 | (hsw >> 8) << 4 | (vso >> 4) << 2 | (vsw >> 4));
    t[12] = (uint8_t)mm_w, t[13] = (uint8_t)mm_h;
    t[14] = (uint8_t)((mm_w >> 8) << 4 | (mm_h >> 8));
    t[17] = 0x1e;             /* digital separate sync, +hsync +vsync */

    static const char name[13] = "Chameleon\n   ";
    uint8_t *d = e + 72;      /* display name */
    d[3] = 0xfc;
    memcpy(d + 5, name, sizeof name);
    e[90 + 3] = e[108 + 3] = 0x10; /* unused descriptors */

    uint8_t sum = 0;
    for (int i = 0; i < 127; i++)
        sum = (uint8_t)(sum + e[i]);
    e[127] = (uint8_t)-sum;
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
    g_screen_width = w;
    g_screen_height = h;
    g_mm_width = (uint32_t)(w * 25.4 / dpi + 0.5);
    g_mm_height = (uint32_t)(h * 25.4 / dpi + 0.5);
    make_edid(g_mm_width, g_mm_height);
}

void kms_init_once(void)
{
    pthread_once(&g_init_once, kms_init);
}

#define REFRESH_SLACK_MHZ 1000 /* the display's rate wobbles a little */

int kms_panel(uint32_t width, uint32_t height, uint32_t refresh_mhz)
{
    kms_init_once();
    pthread_mutex_lock(&g_lock);
    int changed = width != g_mode.hdisplay || height != g_mode.vdisplay ||
                  abs((int32_t)refresh_mhz - (int32_t)g_mode_mhz) > REFRESH_SLACK_MHZ;
    if (changed) {
        make_mode(width, height, refresh_mhz);
        cham_log("connector mode now %ux%u @ %.2f Hz", width, height, refresh_mhz / 1000.0);
    }
    pthread_mutex_unlock(&g_lock);
    return changed;
}

/* ---- helpers ---- */

/* The kernel's copies to and from the caller's memory (card.c). */
static int copy_to_user(uint64_t to, const void *from, size_t n)
{
    return n ? card_copy_to(to, from, n) : 0;
}

static int copy_from_user(void *to, uint64_t from, size_t n)
{
    return n ? card_copy_from(to, from, n) : 0;
}

/* An id list as most getters return it: as many as fit, then the count. */
static int put_ids(uint64_t ptr, uint32_t *count, const uint32_t *ids, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (i < *count && copy_to_user(ptr + i * sizeof ids[0], &ids[i], sizeof ids[0]))
            return -EFAULT;
    *count = n;
    return 0;
}

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

static uint64_t prop_value(uint32_t prop)
{
    const struct kms_state *s = &g_state;
    switch (prop) {
    case P_CONN_CRTC_ID: return s->conn_crtc;
    case P_CONN_DPMS: return s->dpms;
    case P_CONN_NON_DESKTOP: return 0;
    case P_CONN_EDID: return ID_EDID_BLOB;
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

/* ---- mode objects ---- */

static int get_resources(const struct fake_fd *f, struct drm_mode_card_res *r)
{
    static const uint32_t crtcs[] = {ID_CRTC}, conns[] = {ID_CONNECTOR}, encs[] = {ID_ENCODER};
    uint32_t n = 0;
    for (const struct fb *fb = g_fbs; fb; fb = fb->next) { /* the file's own */
        if (fb->owner != f)
            continue;
        if (n < r->count_fbs && copy_to_user(r->fb_id_ptr + n * sizeof fb->id, &fb->id, sizeof fb->id))
            return -EFAULT;
        n++;
    }
    r->count_fbs = n;
    r->min_width = r->min_height = 1;
    r->max_width = r->max_height = 16384;
    if (put_ids(r->crtc_id_ptr, &r->count_crtcs, crtcs, 1) ||
        put_ids(r->encoder_id_ptr, &r->count_encoders, encs, 1) ||
        put_ids(r->connector_id_ptr, &r->count_connectors, conns, 1))
        return -EFAULT;
    return 0;
}

/* An object's properties as `f` sees them (atomic ones only with the
 * atomic client cap): as many as fit, then the count. */
static int put_props(const struct fake_fd *f, const uint32_t *props, uint32_t n, uint64_t ids_ptr,
                     uint64_t values_ptr, uint32_t *count)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if ((k_props[props[i] - 100].flags & DRM_MODE_PROP_ATOMIC) && !f->atomic)
            continue;
        if (*count > k) {
            uint64_t v = prop_value(props[i]);
            if (copy_to_user(ids_ptr + k * sizeof props[i], &props[i], sizeof props[i]) ||
                copy_to_user(values_ptr + k * sizeof v, &v, sizeof v))
                return -EFAULT;
        }
        k++;
    }
    *count = k;
    return 0;
}

static int get_connector(struct fake_fd *f, struct drm_mode_get_connector *c)
{
    if (c->connector_id != ID_CONNECTOR)
        return -ENOENT;
    static const uint32_t enc = ID_ENCODER;
    /* Encoders and modes: all of them if they fit, else none. */
    if (c->count_encoders >= 1 && copy_to_user(c->encoders_ptr, &enc, sizeof enc))
        return -EFAULT;
    c->count_encoders = 1;
    c->connector_type = DRM_MODE_CONNECTOR_DSI;
    c->connector_type_id = 1;
    c->connection = 1; /* DRM_MODE_CONNECTED */
    c->mm_width = g_mm_width;
    c->mm_height = g_mm_height;
    c->subpixel = 1; /* DRM_MODE_SUBPIXEL_UNKNOWN */
    if (c->count_modes >= 1 && copy_to_user(c->modes_ptr, &g_mode, sizeof g_mode))
        return -EFAULT;
    c->count_modes = 1;
    c->encoder_id = g_state.conn_crtc ? ID_ENCODER : 0; /* the one it drives now */
    return put_props(f, k_conn_props, sizeof k_conn_props / sizeof k_conn_props[0], c->props_ptr,
                     c->prop_values_ptr, &c->count_props);
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
    c->x = g_state.src_x >> 16;
    c->y = g_state.src_y >> 16;
    c->gamma_size = 0;
    /* A CRTC with a mode is enabled, active or not (DPMS off keeps it). */
    struct blob *b = blob_find(g_state.mode_blob);
    c->mode_valid = b && b->length == sizeof(struct drm_mode_modeinfo);
    if (c->mode_valid)
        memcpy(&c->mode, b->data, sizeof c->mode);
    else
        memset(&c->mode, 0, sizeof c->mode);
    return 0;
}

static int get_plane_resources(const struct fake_fd *f, struct drm_mode_get_plane_res *r)
{
    static const uint32_t planes[] = {ID_PLANE};
    /* The one plane is the primary: listed only for universal-planes clients. */
    return put_ids(r->plane_id_ptr, &r->count_planes, planes, f->universal_planes ? 1u : 0u);
}

/* The plane's formats: what gbm allocates (RGBA and BGRA Android buffers),
 * which the app copies by their own layout. XRGB8888 first, the format every
 * driver offers by convention. */
static const uint32_t k_plane_formats[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888,
                                           DRM_FORMAT_ABGR8888};

static int plane_has_format(uint32_t format)
{
    for (size_t i = 0; i < sizeof k_plane_formats / sizeof k_plane_formats[0]; i++)
        if (k_plane_formats[i] == format)
            return 1;
    return 0;
}

static int get_plane(struct drm_mode_get_plane *p)
{
    if (p->plane_id != ID_PLANE)
        return -ENOENT;
    const uint32_t n = sizeof k_plane_formats / sizeof k_plane_formats[0];
    p->crtc_id = g_state.plane_crtc;
    p->fb_id = g_state.fb;
    p->possible_crtcs = 1;
    p->gamma_size = 0;
    if (p->count_format_types >= n && copy_to_user(p->format_type_ptr, k_plane_formats, sizeof k_plane_formats))
        return -EFAULT;
    p->count_format_types = n;
    return 0;
}

/* ---- properties ---- */

static int obj_get_properties(const struct fake_fd *f, struct drm_mode_obj_get_properties *o)
{
    uint32_t type, n;
    const uint32_t *props = object_props(o->obj_id, &type, &n);
    if (o->obj_id == ID_ENCODER && (!o->obj_type || o->obj_type == DRM_MODE_OBJECT_ENCODER))
        return -EINVAL; /* an object without properties */
    if (!props || (o->obj_type && o->obj_type != type))
        return -ENOENT;
    return put_props(f, props, n, o->props_ptr, o->prop_values_ptr, &o->count_props);
}

static int get_property(struct drm_mode_get_property *p)
{
    if (p->prop_id < 100 || p->prop_id >= P_END)
        return -ENOENT;
    const struct propdef *d = &k_props[p->prop_id - 100];
    p->flags = d->flags;
    memset(p->name, 0, sizeof p->name);
    snprintf(p->name, sizeof p->name, "%s", d->name);
    int is_enum = (d->flags & DRM_MODE_PROP_ENUM) != 0;
    uint32_t n = is_enum ? (uint32_t)d->nenums : (uint32_t)d->nvalues; /* an enum's values are 0..n-1 */
    for (uint32_t i = 0; i < n; i++) {
        uint64_t v = is_enum ? i : d->values[i];
        if (i < p->count_values && copy_to_user(p->values_ptr + i * sizeof v, &v, sizeof v))
            return -EFAULT;
    }
    p->count_values = n;
    if (is_enum) {
        for (uint32_t i = 0; i < n; i++) {
            if (p->count_enum_blobs < i + 1)
                continue;
            struct drm_mode_property_enum e = {.value = i};
            snprintf(e.name, sizeof e.name, "%s", d->enums[i]);
            if (copy_to_user(p->enum_blob_ptr + i * sizeof e, &e, sizeof e))
                return -EFAULT;
        }
        p->count_enum_blobs = n;
    }
    if (d->flags & DRM_MODE_PROP_BLOB)
        p->count_enum_blobs = 0;
    return 0;
}

static int get_blob(struct drm_mode_get_blob *g)
{
    struct blob *b = g->blob_id == ID_EDID_BLOB ? &g_edid_blob : blob_find(g->blob_id);
    if (!b)
        return -ENOENT;
    if (g->length == b->length && copy_to_user(g->data, b->data, b->length)) /* only an exact fit */
        return -EFAULT;
    g->length = b->length;
    return 0;
}

static int create_blob(struct fake_fd *f, struct drm_mode_create_blob *c)
{
    if (!c->length || c->length > INT32_MAX - 64)
        return -EINVAL;
    struct blob *b = calloc(1, sizeof *b);
    b->data = b ? malloc(c->length) : NULL;
    if (!b || !b->data) {
        free(b);
        return -ENOMEM;
    }
    if (copy_from_user(b->data, c->data, c->length)) {
        free(b->data);
        free(b);
        return -EFAULT;
    }
    b->length = c->length;
    b->owner = f;
    b->id = g_next_object_id++;
    b->next = g_blobs;
    g_blobs = b;
    c->blob_id = b->id;
    return 0;
}

/* Frees the blobs the client destroyed that the CRTC no longer uses. */
static void free_destroyed_blobs(void)
{
    for (struct blob **p = &g_blobs; *p;) {
        struct blob *b = *p;
        if (b->destroyed && b->id != g_state.mode_blob) {
            *p = b->next;
            free(b->data);
            free(b);
        } else {
            p = &b->next;
        }
    }
}

static int destroy_blob(struct fake_fd *f, struct drm_mode_destroy_blob *d)
{
    struct blob *b = blob_find(d->blob_id);
    if (!b || b->destroyed)
        return -ENOENT;
    if (b->owner != f)
        return -EPERM;
    b->destroyed = 1;
    free_destroyed_blobs();
    return 0;
}

/* ---- buffers: GEM handles, per file ---- */

static struct cham_bo *handle_bo(const struct fake_fd *f, uint32_t handle)
{
    for (int i = 0; handle && i < f->nhandles; i++)
        if (f->handles[i].handle == handle)
            return f->handles[i].bo;
    return NULL;
}

/* The file's handle for `bo`, made if it has none (the lowest free number,
 * as the kernel's idr). The handle holds a reference. */
static uint32_t handle_for(struct fake_fd *f, struct cham_bo *bo)
{
    uint32_t h = 1;
    for (int i = 0; i < f->nhandles; i++)
        if (f->handles[i].bo == bo)
            return f->handles[i].handle;
    for (int i = 0; i < f->nhandles; i++)
        if (f->handles[i].handle == h)
            h++, i = -1; /* taken: look again from the start */
    struct gem_handle *t = realloc(f->handles, sizeof *t * (size_t)(f->nhandles + 1));
    if (!t)
        return 0;
    f->handles = t;
    f->handles[f->nhandles++] = (struct gem_handle){h, bo};
    bo_ref_locked(bo);
    return h;
}

static int gem_close(struct fake_fd *f, uint32_t handle)
{
    for (int i = 0; handle && i < f->nhandles; i++) {
        if (f->handles[i].handle == handle) {
            struct cham_bo *bo = f->handles[i].bo;
            f->handles[i] = f->handles[--f->nhandles];
            bo_unref_locked(bo); /* framebuffers keep their own references */
            return 0;
        }
    }
    return -EINVAL;
}

/* Our gbm's buffers: a gbm device on the card makes a handle for each
 * buffer it creates (GEM_CLOSE closes it), as Mesa's asks its driver. */
uint32_t kms_gem_import(struct fake_fd *f, struct cham_bo *bo)
{
    pthread_mutex_lock(&g_lock);
    uint32_t h = handle_for(f, bo);
    pthread_mutex_unlock(&g_lock);
    return h;
}

/* ---- framebuffers ---- */

static int add_fb2(struct fake_fd *f, struct drm_mode_fb_cmd2 *c)
{
    if (!plane_has_format(c->pixel_format))
        return -EINVAL; /* as the kernel: a format no plane can show */
    struct cham_bo *bo = handle_bo(f, c->handles[0]);
    if (!bo)
        return -ENOENT;
    if (c->width > bo->width || c->height > bo->height)
        return -EINVAL;
    struct fb *fb = calloc(1, sizeof *fb);
    fb->id = g_next_object_id++;
    fb->bo = bo;
    fb->owner = f;
    bo_ref_locked(bo);
    fb->next = g_fbs;
    g_fbs = fb;
    c->fb_id = fb->id;
    return 0;
}

/* Frees the framebuffers no file holds that the plane doesn't show either
 * (the kernel's last reference going). */
static void free_unused_fbs(void)
{
    for (struct fb **p = &g_fbs; *p;) {
        struct fb *fb = *p;
        if (!fb->owner && fb->id != g_state.fb) {
            *p = fb->next;
            bo_unref_locked(fb->bo);
            free(fb);
        } else {
            p = &fb->next;
        }
    }
}

/* The plane lets go of a framebuffer that is being removed (RMFB, or its
 * file closing): the kernel takes it off the plane; the CRTC stays on. */
static void plane_drop_fb(uint32_t id)
{
    if (!id || g_state.fb != id)
        return;
    g_state.fb = 0;
    g_state.plane_crtc = 0;
    if (g_plane_bo)
        bo_unref_locked(g_plane_bo);
    g_plane_bo = NULL;
    g_plane_shown = 0;
}

/* The file lets go of its framebuffer. RMFB also takes it off the plane;
 * CLOSEFB leaves it showing (and usable by id) until something replaces it. */
static int remove_fb(struct fake_fd *f, uint32_t id, int off_plane)
{
    struct fb *fb = fb_find(id);
    if (!fb || fb->owner != f)
        return -ENOENT;
    fb->owner = NULL;
    if (off_plane)
        plane_drop_fb(id);
    free_unused_fbs();
    return 0;
}

static int prime_fd_to_handle(struct fake_fd *f, struct drm_prime_handle *p)
{
    /* Asking the caller and cham_bo_from_fd go without the lock. */
    pthread_mutex_unlock(&g_lock);
    int fd = card_fd_get(p->fd);
    struct cham_bo *bo = fd >= 0 ? cham_bo_from_fd(fd) : NULL;
    if (fd >= 0)
        close(fd);
    pthread_mutex_lock(&g_lock);
    if (fd < 0)
        return -EBADF;
    if (!bo)
        return -EINVAL; /* not one of ours, e.g. a client buffer: no direct scanout */
    p->handle = handle_for(f, bo);
    return p->handle ? 0 : -ENOMEM;
}

static int prime_handle_to_fd(struct fake_fd *f, struct drm_prime_handle *p)
{
    if (p->flags & ~(uint32_t)(DRM_CLOEXEC | DRM_RDWR))
        return -EINVAL;
    struct cham_bo *bo = handle_bo(f, p->handle);
    if (!bo)
        return -ENOENT;
    int fd = cham_bo_export_fd(bo);
    if (fd < 0)
        return -errno;
    pthread_mutex_unlock(&g_lock); /* the caller takes it */
    int ret = card_fd_install(fd, (p->flags & DRM_CLOEXEC) != 0); /* close-on-exec only when asked */
    pthread_mutex_lock(&g_lock);
    close(fd);
    if (ret < 0)
        return ret;
    p->fd = ret;
    return 0;
}

static int g_in_fence = -1; /* our copy of the commit's IN_FENCE_FD, under g_lock */

/* ---- atomic ---- */

static int set_prop(struct kms_state *s, uint32_t obj, uint32_t prop, uint64_t v)
{
    switch (obj) {
    case ID_CONNECTOR:
        if (prop == P_CONN_CRTC_ID && (v == 0 || v == ID_CRTC)) {
            s->conn_crtc = (uint32_t)v;
            return 0;
        }
        return -EINVAL; /* DPMS too: the kernel takes it only through the legacy SETPROPERTY */
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
            s->fb = (uint32_t)v; /* checked to exist by prop_value_ok */
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
        case P_IN_FENCE_FD:
            if (g_in_fence >= 0)
                close(g_in_fence);
            g_in_fence = (int64_t)v >= 0 ? card_fd_get((int)v) : -1;
            if (g_in_fence < 0 && (int64_t)v >= 0)
                return -EINVAL; /* not an open file */
            s->in_fence = g_in_fence;
            return 0;
        }
        return -EINVAL;
    }
    return -ENOENT;
}

/* drm_property_change_valid_get: the value fits the property's type. */
static int prop_value_ok(uint32_t prop, uint64_t v)
{
    const struct propdef *d = &k_props[prop - 100];
    if (d->flags & DRM_MODE_PROP_IMMUTABLE)
        return 0;
    if (d->flags & DRM_MODE_PROP_RANGE)
        return v >= d->values[0] && v <= d->values[1];
    if (d->flags & DRM_MODE_PROP_SIGNED_RANGE)
        return (int64_t)v >= (int64_t)d->values[0] && (int64_t)v <= (int64_t)d->values[1];
    if (d->flags & DRM_MODE_PROP_BLOB)
        return v == 0 || blob_find((uint32_t)v) != NULL;
    if (d->flags & DRM_MODE_PROP_OBJECT) { /* 0, or an object of the property's type */
        if (v == 0)
            return 1;
        if (d->values[0] == DRM_MODE_OBJECT_FB)
            return fb_find((uint32_t)v) != NULL;
        return d->values[0] == DRM_MODE_OBJECT_CRTC && v == ID_CRTC;
    }
    return v < (uint64_t)d->nenums; /* enum values are 0..n-1 */
}

static const struct drm_mode_modeinfo *state_mode(const struct kms_state *s)
{
    struct blob *b = s->mode_blob ? blob_find(s->mode_blob) : NULL;
    return b ? b->data : NULL;
}

/* Refresh rate of a mode in mHz (KWin's custom modes follow the display). */
static uint32_t mode_mhz(const struct drm_mode_modeinfo *m)
{
    uint64_t pixels = m ? (uint64_t)m->htotal * m->vtotal : 0;
    return pixels ? (uint32_t)((uint64_t)m->clock * 1000000ull / pixels) : g_mode_mhz;
}

void kms_screen_size(uint32_t *width, uint32_t *height, uint32_t *mm_width, uint32_t *mm_height)
{
    kms_init_once();
    *width = g_screen_width;
    *height = g_screen_height;
    *mm_width = g_mm_width;
    *mm_height = g_mm_height;
}

void kms_mode_size(uint32_t *width, uint32_t *height)
{
    pthread_mutex_lock(&g_lock);
    const struct drm_mode_modeinfo *m = state_mode(&g_state);
    *width = m ? m->hdisplay : 0;
    *height = m ? m->vdisplay : 0;
    pthread_mutex_unlock(&g_lock);
}

struct cham_bo *kms_screen_bo_locked(void)
{
    return g_plane_shown ? g_plane_bo : NULL;
}

int kms_screen_off_locked(void)
{
    return g_screen_off;
}

static int commit(struct fake_fd *f, struct drm_mode_atomic *a)
{
    if (!f->atomic)
        return -EINVAL;
    if (a->flags & ~(uint32_t)DRM_MODE_ATOMIC_FLAGS)
        return -EINVAL;
    if (a->reserved)
        return -EINVAL;
    if (a->flags & DRM_MODE_PAGE_FLIP_ASYNC)
        return -EINVAL; /* no tearing updates */
    if ((a->flags & DRM_MODE_ATOMIC_TEST_ONLY) && (a->flags & DRM_MODE_PAGE_FLIP_EVENT))
        return -EINVAL;

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

    uint64_t k = 0;
    for (uint32_t i = 0; i < a->count_objs; i++) {
        uint32_t obj, count, type, nprops;
        if (copy_from_user(&obj, a->objs_ptr + i * sizeof obj, sizeof obj))
            return -EFAULT;
        const uint32_t *obj_props = object_props(obj, &type, &nprops);
        if (!obj_props)
            REFUSE(-ENOENT, "object %u has no properties", obj);
        if (copy_from_user(&count, a->count_props_ptr + i * sizeof count, sizeof count))
            return -EFAULT;
        for (uint32_t j = 0; j < count; j++, k++) {
            uint32_t prop;
            uint64_t v;
            if (copy_from_user(&prop, a->props_ptr + k * sizeof prop, sizeof prop))
                return -EFAULT;
            uint32_t m = 0;
            while (m < nprops && obj_props[m] != prop)
                m++;
            if (m == nprops)
                REFUSE(-ENOENT, "object %u has no property %u", obj, prop);
            if (copy_from_user(&v, a->prop_values_ptr + k * sizeof v, sizeof v))
                return -EFAULT;
            int ret = prop_value_ok(prop, v) ? set_prop(&s, obj, prop, v) : -EINVAL;
            if (ret)
                REFUSE(ret, "object %u property %u = %llu", obj, prop, (unsigned long long)v);
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
    /* A plane bigger than the CRTC is clipped, as with real drivers: KWin's
     * first commit after a mode change can still carry the previous frame's
     * size. A smaller one is only KWin putting its cursor alone on an empty
     * screen (nothing else ever reaches this plane: client buffers can't be
     * scanned out); it is accepted but not shown, since the app shows whole
     * buffers - the previous frame stays until a full one comes. */
    int partial = 0;
    if (s.fb && !s.plane_crtc)
        REFUSE(-EINVAL, "framebuffer on a plane without a CRTC");
    if (s.plane_crtc && !s.fb)
        REFUSE(-EINVAL, "plane on a CRTC without a framebuffer");
    /* A plane on a CRTC that is off or has no mode stays attached, unseen. */
    if (s.fb && on)
        partial = s.crtc_x > 0 || s.crtc_y > 0 || (int64_t)s.crtc_x + s.crtc_w < mode->hdisplay ||
                  (int64_t)s.crtc_y + s.crtc_h < mode->vdisplay;
    if (a->flags & DRM_MODE_PAGE_FLIP_EVENT) {
        if (!s.active && !g_state.active) /* an event for turning it off is fine */
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
    struct fb *fb = s.fb ? fb_find(s.fb) : NULL;
    struct cham_bo *bo = fb ? fb->bo : NULL;
    if (bo)
        bo_ref_locked(bo);
    if (g_plane_bo)
        bo_unref_locked(g_plane_bo);
    g_plane_bo = bo;
    g_state = s;
    free_destroyed_blobs();
    free_unused_fbs(); /* a closed framebuffer the plane just let go of */

    if (modeset && mode && (!old_mode || mode->hdisplay != old_mode->hdisplay ||
                            mode->vdisplay != old_mode->vdisplay))
        cham_log("KWin set mode %ux%u", mode->hdisplay, mode->vdisplay);
    /* Off after having been on: KWin's DPMS off (CRTC inactive, mode
     * cleared) or the DPMS property; e.g. Plasma's power management after a
     * while without input. */
    if (on && s.dpms == 0)
        g_screen_was_on = 1;
    int screen_off = g_screen_was_on && !(on && s.dpms == 0);
    if (screen_off != g_screen_off) {
        cham_log("KWin turned its screen %s", screen_off ? "off" : "on");
        link_screen_off_locked(screen_off);
    }
    g_screen_off = screen_off;

    uint64_t frame = ++g_frame;
    int presented = 0;
    g_plane_shown = bo && on && s.dpms == 0 && !partial;
    if (g_plane_shown)
        presented = link_present_locked(bo, in_fence, frame);
    if (a->flags & DRM_MODE_PAGE_FLIP_EVENT)
        link_queue_flip_locked(a->user_data, ID_CRTC, f->event_wfd, frame, presented, mode_mhz(mode));
    return 0;
}

static int atomic_commit(struct fake_fd *f, struct drm_mode_atomic *a)
{
    int ret = commit(f, a);
    if (g_in_fence >= 0)
        close(g_in_fence); /* the caller's fence stays the caller's */
    g_in_fence = -1;
    return ret;
}

/* ---- misc ---- */

static int get_cap(struct drm_get_cap *c)
{
    c->value = 0;
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

/* The kernel refuses atomic to Xorg's modesetting driver by process name
 * ("broken atomic modeset userspace"). */
static int comm_starts_with_x(void)
{
    char comm[32] = "", path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)card_caller_pid());
    FILE *fp = fopen(path, "re");
    if (fp) {
        if (!fgets(comm, sizeof comm, fp))
            comm[0] = 0;
        fclose(fp);
    }
    return comm[0] == 'X';
}

static int set_client_cap(struct fake_fd *f, struct drm_set_client_cap *c)
{
    switch (c->capability) {
    case DRM_CLIENT_CAP_STEREO_3D:
    case DRM_CLIENT_CAP_ASPECT_RATIO:
        return c->value <= 1 ? 0 : -EINVAL;
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
        if (c->value > 1)
            return -EINVAL;
        f->universal_planes = (int)c->value;
        return 0;
    case DRM_CLIENT_CAP_ATOMIC:
        if (c->value == 1 && comm_starts_with_x())
            return -EOPNOTSUPP;
        if (c->value > 2)
            return -EINVAL;
        f->atomic = f->universal_planes = (int)c->value; /* and aspect ratios */
        return 0;
    case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS:
        if (!f->atomic || c->value > 1)
            return -EINVAL;
        f->writeback = (int)c->value; /* there are none to show */
        return 0;
    case 6: /* DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT: a feature of virtual GPUs' drivers */
        return -EOPNOTSUPP;
    }
    return -EINVAL;
}

/* ---- DRM master and authentication (6.8's drm_auth.c) ---- */

void kms_file_open(struct fake_fd *f)
{
    pthread_mutex_lock(&g_lock);
    f->kms_next = g_files;
    g_files = f;
    if (!g_master) /* the first opener while there is none becomes master */
        g_master = f, f->master = f->was_master = 1;
    pthread_mutex_unlock(&g_lock);
}

pid_t kms_master_pid(void)
{
    pthread_mutex_lock(&g_lock);
    pid_t pid = g_master ? g_master->pid : 0;
    pthread_mutex_unlock(&g_lock);
    return pid;
}

void kms_file_close(struct fake_fd *f)
{
    pthread_mutex_lock(&g_lock);
    for (struct fake_fd **p = &g_files; *p; p = &(*p)->kms_next) {
        if (*p == f) {
            *p = f->kms_next;
            break;
        }
    }
    if (g_master == f)
        g_master = NULL; /* nobody inherits it; the next opener takes it */
    link_cancel_flip_locked(f->event_wfd);
    for (struct fb *fb = g_fbs; fb; fb = fb->next) { /* drm_fb_release */
        if (fb->owner == f) {
            fb->owner = NULL;
            plane_drop_fb(fb->id);
        }
    }
    free_unused_fbs();
    for (struct blob *b = g_blobs; b; b = b->next)
        if (b->owner == f)
            b->destroyed = 1, b->owner = NULL;
    free_destroyed_blobs();
    while (f->nhandles)
        gem_close(f, f->handles[0].handle);
    free(f->handles);
    f->handles = NULL;
    pthread_mutex_unlock(&g_lock);
}

/* Who may SET_MASTER/DROP_MASTER: a file that was master, from the process
 * that opened it, or anyone with CAP_SYS_ADMIN (root here). */
static int master_perm(const struct fake_fd *f)
{
    return (f->was_master && f->pid == card_caller_pid()) || card_caller_uid() == 0 ? 0 : -EACCES;
}

static int set_master(struct fake_fd *f)
{
    int ret = master_perm(f);
    if (ret || g_master == f)
        return ret;
    if (g_master)
        return -EBUSY;
    g_master = f;
    f->master = f->was_master = 1;
    return 0;
}

static int drop_master(struct fake_fd *f)
{
    int ret = master_perm(f);
    if (ret)
        return ret;
    if (g_master != f)
        return -EINVAL;
    g_master = NULL;
    f->master = 0;
    return 0;
}

static int get_magic(struct fake_fd *f, struct drm_auth *a)
{
    if (!f->magic)
        f->magic = g_next_magic++;
    a->magic = f->magic;
    return 0;
}

/* Called by the master (checked by the caller): marks the file holding the
 * magic authenticated; a magic works once. */
static int auth_magic(const struct drm_auth *a);

/* drm_copy_field: as much as fits, the full length reported. */
static int copy_string(char *dst, __kernel_size_t *len, const char *src)
{
    size_t n = strlen(src), fit = n < *len ? n : *len;
    *len = n;
    return dst && fit ? copy_to_user((uint64_t)(uintptr_t)dst, src, fit) : 0;
}

static int get_version(struct drm_version *v)
{
    v->version_major = 1;
    v->version_minor = 0;
    v->version_patchlevel = 0;
    if (copy_string(v->name, &v->name_len, "chameleon") || copy_string(v->date, &v->date_len, "20260929") ||
        copy_string(v->desc, &v->desc_len, "Chameleon: KMS on an Android surface"))
        return -EFAULT;
    return 0;
}

static int auth_magic(const struct drm_auth *a)
{
    struct fake_fd *holder = g_files;
    while (holder && (!a->magic || holder->magic != a->magic))
        holder = holder->kms_next;
    if (!holder || holder->magic_used)
        return -EINVAL;
    holder->magic_used = 1;
    return 0;
}

/* ---- the ioctl entry (6.8's drm_ioctl) ---- */

static int op_version(struct fake_fd *f, void *d) { (void)f; return get_version(d); }
static int op_get_cap(struct fake_fd *f, void *d) { (void)f; return get_cap(d); }
static int op_set_client_cap(struct fake_fd *f, void *d) { return set_client_cap(f, d); }
static int op_get_magic(struct fake_fd *f, void *d) { return get_magic(f, d); }
static int op_auth_magic(struct fake_fd *f, void *d) { (void)f; return auth_magic(d); }
static int op_set_master(struct fake_fd *f, void *d) { (void)d; return set_master(f); }
static int op_drop_master(struct fake_fd *f, void *d) { (void)d; return drop_master(f); }
static int op_get_unique(struct fake_fd *f, void *d)
{
    (void)f;
    ((struct drm_unique *)d)->unique_len = 0; /* a platform device's unique is empty, as vkms' */
    return 0;
}
static int op_get_resources(struct fake_fd *f, void *d) { return get_resources(f, d); }
static int op_get_connector(struct fake_fd *f, void *d) { return get_connector(f, d); }
static int op_get_encoder(struct fake_fd *f, void *d) { (void)f; return get_encoder(d); }
static int op_get_crtc(struct fake_fd *f, void *d) { (void)f; return get_crtc(d); }
static int op_get_plane_resources(struct fake_fd *f, void *d) { return get_plane_resources(f, d); }
static int op_get_plane(struct fake_fd *f, void *d) { (void)f; return get_plane(d); }
static int op_obj_get_properties(struct fake_fd *f, void *d) { return obj_get_properties(f, d); }
static int op_get_property(struct fake_fd *f, void *d) { (void)f; return get_property(d); }
static int op_get_blob(struct fake_fd *f, void *d) { (void)f; return get_blob(d); }
static int op_create_blob(struct fake_fd *f, void *d) { return create_blob(f, d); }
static int op_destroy_blob(struct fake_fd *f, void *d) { return destroy_blob(f, d); }
static int op_add_fb2(struct fake_fd *f, void *d) { return add_fb2(f, d); }
static int op_rmfb(struct fake_fd *f, void *d) { return remove_fb(f, *(uint32_t *)d, 1); }
static int op_closefb(struct fake_fd *f, void *d) { return remove_fb(f, ((struct drm_mode_closefb *)d)->fb_id, 0); }
static int op_prime_fd_to_handle(struct fake_fd *f, void *d) { return prime_fd_to_handle(f, d); }
static int op_prime_handle_to_fd(struct fake_fd *f, void *d) { return prime_handle_to_fd(f, d); }
static int op_gem_close(struct fake_fd *f, void *d) { return gem_close(f, ((struct drm_gem_close *)d)->handle); }
static int op_atomic(struct fake_fd *f, void *d) { return atomic_commit(f, d); }
static int op_list_lessees(struct fake_fd *f, void *d)
{
    (void)f;
    struct drm_mode_list_lessees *l = d;
    if (l->pad)
        return -EINVAL;
    l->count_lessees = 0; /* nothing is leased */
    return 0;
}
static int op_noop(struct fake_fd *f, void *d) { (void)f; (void)d; return 0; }
/* The syncobj ioctls of a driver without DRIVER_SYNCOBJ. */
static int op_no_syncobj(struct fake_fd *f, void *d) { (void)f; (void)d; return -EOPNOTSUPP; }

#define DRM_CORE_IOCTL_COUNT 0xd1 /* up to DRM_IOCTL_MODE_CLOSEFB */
#define MASTER 1
struct ioctl_def {
    unsigned int cmd;
    int (*fn)(struct fake_fd *, void *);
    int flags;
};
#define DEF(c, f, fl) [_IOC_NR(c)] = {(unsigned int)(c), (f), (fl)}
/* What the card answers; core ioctls it doesn't implement are listed with
 * no function when the kernel needs the master to reach them, so a
 * non-master file still gets EACCES as on Linux. */
static const struct ioctl_def k_ioctls[DRM_CORE_IOCTL_COUNT] = {
    DEF(DRM_IOCTL_VERSION, op_version, 0),
    DEF(DRM_IOCTL_GET_UNIQUE, op_get_unique, 0),
    DEF(DRM_IOCTL_GET_MAGIC, op_get_magic, 0),
    DEF(DRM_IOCTL_GET_CAP, op_get_cap, 0),
    DEF(DRM_IOCTL_SET_CLIENT_CAP, op_set_client_cap, 0),
    DEF(DRM_IOCTL_SET_VERSION, NULL, MASTER),
    DEF(DRM_IOCTL_AUTH_MAGIC, op_auth_magic, MASTER),
    DEF(DRM_IOCTL_SET_MASTER, op_set_master, 0),
    DEF(DRM_IOCTL_DROP_MASTER, op_drop_master, 0),
    DEF(DRM_IOCTL_GEM_CLOSE, op_gem_close, 0),
    DEF(DRM_IOCTL_PRIME_HANDLE_TO_FD, op_prime_handle_to_fd, 0),
    DEF(DRM_IOCTL_PRIME_FD_TO_HANDLE, op_prime_fd_to_handle, 0),
    DEF(DRM_IOCTL_MODE_GETRESOURCES, op_get_resources, 0),
    DEF(DRM_IOCTL_MODE_GETPLANERESOURCES, op_get_plane_resources, 0),
    DEF(DRM_IOCTL_MODE_GETCRTC, op_get_crtc, 0),
    DEF(DRM_IOCTL_MODE_SETCRTC, NULL, MASTER),
    DEF(DRM_IOCTL_MODE_GETPLANE, op_get_plane, 0),
    DEF(DRM_IOCTL_MODE_SETPLANE, NULL, MASTER),
    DEF(DRM_IOCTL_MODE_CURSOR, NULL, MASTER),
    DEF(DRM_IOCTL_MODE_SETGAMMA, NULL, MASTER),
    DEF(DRM_IOCTL_MODE_GETENCODER, op_get_encoder, 0),
    DEF(DRM_IOCTL_MODE_GETCONNECTOR, op_get_connector, 0),
    DEF(DRM_IOCTL_MODE_ATTACHMODE, op_noop, MASTER),
    DEF(DRM_IOCTL_MODE_DETACHMODE, op_noop, MASTER),
    DEF(DRM_IOCTL_MODE_GETPROPERTY, op_get_property, 0),
    DEF(DRM_IOCTL_MODE_SETPROPERTY, NULL, MASTER),
    DEF(DRM_IOCTL_MODE_GETPROPBLOB, op_get_blob, 0),
    DEF(DRM_IOCTL_MODE_ADDFB2, op_add_fb2, 0),
    DEF(DRM_IOCTL_MODE_RMFB, op_rmfb, 0),
    DEF(DRM_IOCTL_MODE_PAGE_FLIP, NULL, MASTER),
    DEF(DRM_IOCTL_MODE_DIRTYFB, NULL, MASTER),
    DEF(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, op_obj_get_properties, 0),
    DEF(DRM_IOCTL_MODE_OBJ_SETPROPERTY, NULL, MASTER),
    DEF(DRM_IOCTL_MODE_CURSOR2, NULL, MASTER),
    DEF(DRM_IOCTL_MODE_ATOMIC, op_atomic, MASTER),
    DEF(DRM_IOCTL_MODE_CREATEPROPBLOB, op_create_blob, 0),
    DEF(DRM_IOCTL_MODE_DESTROYPROPBLOB, op_destroy_blob, 0),
    DEF(DRM_IOCTL_SYNCOBJ_CREATE, op_no_syncobj, 0),
    DEF(DRM_IOCTL_SYNCOBJ_DESTROY, op_no_syncobj, 0),
    DEF(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, op_no_syncobj, 0),
    DEF(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, op_no_syncobj, 0),
    DEF(DRM_IOCTL_SYNCOBJ_WAIT, op_no_syncobj, 0),
    DEF(DRM_IOCTL_SYNCOBJ_RESET, op_no_syncobj, 0),
    DEF(DRM_IOCTL_SYNCOBJ_SIGNAL, op_no_syncobj, 0),
    DEF(DRM_IOCTL_MODE_CREATE_LEASE, NULL, MASTER),
    DEF(DRM_IOCTL_MODE_LIST_LESSEES, op_list_lessees, MASTER),
    DEF(DRM_IOCTL_MODE_GET_LEASE, NULL, MASTER),
    DEF(DRM_IOCTL_MODE_REVOKE_LEASE, NULL, MASTER),
    DEF(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, op_no_syncobj, 0),
    DEF(DRM_IOCTL_SYNCOBJ_QUERY, op_no_syncobj, 0),
    DEF(DRM_IOCTL_SYNCOBJ_TRANSFER, op_no_syncobj, 0),
    DEF(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, op_no_syncobj, 0),
    DEF(DRM_IOCTL_SYNCOBJ_EVENTFD, op_no_syncobj, 0),
    DEF(DRM_IOCTL_MODE_CLOSEFB, op_closefb, 0),
};

/* As the kernel: the ioctl is found by number, the argument is copied in
 * and out by the size the caller declares (the rest zero-filled), and only
 * then are permission and the handler applied. */
int kms_ioctl(struct fake_fd *f, unsigned int cmd, void *user_arg)
{
    static int unknown_warnings, non_master_commits;
    if (_IOC_TYPE(cmd) != DRM_IOCTL_BASE)
        return -ENOTTY;
    unsigned int nr = _IOC_NR(cmd);
    const struct ioctl_def *def = nr < DRM_CORE_IOCTL_COUNT && !(nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END)
                                      ? &k_ioctls[nr]
                                      : NULL; /* driver ioctls: this driver has none */
    if (!def || (!def->fn && !(def->flags & MASTER))) {
        if (unknown_warnings++ < 20)
            cham_log("unsupported DRM ioctl 0x%08x (nr 0x%02x)", cmd, nr);
        return -EINVAL;
    }
    unsigned int drv_size = _IOC_SIZE(def->cmd), in_size = _IOC_SIZE(cmd), out_size = in_size;
    if (!(cmd & def->cmd & IOC_IN))
        in_size = 0;
    if (!(cmd & def->cmd & IOC_OUT))
        out_size = 0;
    unsigned int ksize = in_size > out_size ? in_size : out_size;
    if (drv_size > ksize)
        ksize = drv_size;
    char stack[256], *kdata = ksize <= sizeof stack ? stack : malloc(ksize);
    if (!kdata)
        return -ENOMEM;
    int ret = copy_from_user(kdata, (uint64_t)(uintptr_t)user_arg, in_size);
    if (!ret) {
        memset(kdata + in_size, 0, ksize - in_size);
        pthread_mutex_lock(&g_lock);
        if ((def->flags & MASTER) && g_master != f) {
            if (nr == _IOC_NR(DRM_IOCTL_MODE_ATOMIC) && non_master_commits++ < 5)
                cham_log("refusing a commit from a file that isn't DRM master (EACCES)");
            ret = -EACCES;
        } else if (!def->fn) {
            if (unknown_warnings++ < 20)
                cham_log("unsupported DRM ioctl 0x%08x (nr 0x%02x)", cmd, nr);
            ret = -EINVAL;
        } else {
            ret = def->fn(f, kdata);
        }
        pthread_mutex_unlock(&g_lock);
        if (copy_to_user((uint64_t)(uintptr_t)user_arg, kdata, out_size))
            ret = -EFAULT;
    }
    if (kdata != stack)
        free(kdata);
    return ret;
}
