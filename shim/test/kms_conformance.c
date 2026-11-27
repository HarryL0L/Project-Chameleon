/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Chameleon's card against the real kernel's DRM ioctl handling.
 *
 * The same checks run twice and print the same transcript:
 *  - on the host (default build) against Chameleon's card, buffers from
 *    Chameleon's gbm (run with libchameleon.so preloaded);
 *  - in a real kernel (built with -DREF_KERNEL, run in the reference VM)
 *    against vkms, buffers from dumb buffers.
 * Only device-independent outcomes are printed (errors, counts, what was
 * written), so run-conformance.sh can diff the two: argument copying and
 * struct sizes, partly filled arrays, EFAULT, blob reads, PRIME flags, the
 * atomic ioctl's argument checks, framebuffer lifetime.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

static uint32_t g_crtc, g_conn, g_enc, g_plane;
static struct drm_mode_modeinfo g_mode;

static void result(const char *what, int ret)
{
    printf("%-60s %s\n", what, ret == 0 ? "ok" : strerror(errno));
}

static const char *yes(int c)
{
    return c ? "yes" : "no";
}

/* The id (and value) of an object's property by name. */
static uint32_t prop(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *value)
{
    uint32_t ids[64];
    uint64_t values[64];
    struct drm_mode_obj_get_properties o = {.obj_id = obj, .obj_type = type, .props_ptr = (uintptr_t)ids,
                                            .prop_values_ptr = (uintptr_t)values, .count_props = 64};
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &o))
        return 0;
    for (uint32_t i = 0; i < o.count_props; i++) {
        struct drm_mode_get_property p = {.prop_id = ids[i]};
        ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p);
        if (strcmp(p.name, name) == 0) {
            if (value)
                *value = values[i];
            return ids[i];
        }
    }
    return 0;
}

#ifdef REF_KERNEL
static uint32_t sized_buffer(int fd, uint32_t w, uint32_t h, uint32_t format, uint32_t *pitch)
{
    (void)format;
    struct drm_mode_create_dumb d = {.width = w, .height = h, .bpp = 32};
    ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d);
    *pitch = d.pitch;
    return d.handle;
}
#else
#include <gbm.h>
static uint32_t sized_buffer(int fd, uint32_t w, uint32_t h, uint32_t format, uint32_t *pitch)
{
    static struct gbm_device *gbm;
    if (!gbm)
        gbm = gbm_create_device(fd);
    struct gbm_bo *bo = gbm_bo_create(gbm, w, h, format, GBM_BO_USE_SCANOUT);
    *pitch = gbm_bo_get_stride(bo);
    return gbm_bo_get_handle(bo).u32;
}
#endif

static uint32_t buffer(int fd, uint32_t *pitch)
{
    return sized_buffer(fd, g_mode.hdisplay, g_mode.vdisplay, DRM_FORMAT_XRGB8888, pitch);
}

/* A w x h framebuffer in `format`. */
static uint32_t sized_framebuffer(int fd, uint32_t w, uint32_t h, uint32_t format)
{
    uint32_t pitch, handle = sized_buffer(fd, w, h, format, &pitch);
    struct drm_mode_fb_cmd2 c = {.width = w, .height = h, .pixel_format = format, .handles = {handle},
                                 .pitches = {pitch}};
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &c))
        result("ADDFB2", -1);
    return c.fb_id;
}

static int commit(int fd, uint32_t flags, uint32_t n, const uint32_t *objs, const uint32_t *counts,
                  const uint32_t *props, const uint64_t *values, uint64_t reserved);

/* The cursor plane, as vkms has one: any size and position, no scaling,
 * commits of it alone complete like page flips. */
static void cursor_checks(int fd)
{
    uint32_t planes[8], cursor = 0;
    struct drm_mode_get_plane_res pr = {.plane_id_ptr = (uintptr_t)planes, .count_planes = 8};
    ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
    for (uint32_t i = 0; i < pr.count_planes && i < 8; i++) {
        uint64_t type = 0;
        prop(fd, planes[i], DRM_MODE_OBJECT_PLANE, "type", &type);
        if (type == 2)
            cursor = planes[i];
    }
    printf("%-60s %s\n", "  a cursor plane", yes(cursor));
    if (!cursor)
        return;
    struct drm_get_cap cw = {.capability = DRM_CAP_CURSOR_WIDTH}, ch = {.capability = DRM_CAP_CURSOR_HEIGHT};
    ioctl(fd, DRM_IOCTL_GET_CAP, &cw);
    ioctl(fd, DRM_IOCTL_GET_CAP, &ch);
    printf("%-60s %llux%llu\n", "  cursor size", (unsigned long long)cw.value, (unsigned long long)ch.value);
    static const char *const names[] = {"FB_ID", "CRTC_ID", "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H",
                                        "SRC_X", "SRC_Y", "SRC_W", "SRC_H"};
    uint32_t props[10], counts[1] = {10};
    for (int i = 0; i < 10; i++)
        props[i] = prop(fd, cursor, DRM_MODE_OBJECT_PLANE, names[i], NULL);
    uint32_t fb64 = sized_framebuffer(fd, 64, 64, DRM_FORMAT_ARGB8888);
    uint32_t fb128 = sized_framebuffer(fd, 128, 128, DRM_FORMAT_ARGB8888);
    uint32_t fbx = sized_framebuffer(fd, 64, 64, DRM_FORMAT_XRGB8888);
#define CURSOR(flags, fb, x, y, w, h, sx, sw, sh)                                                         \
    commit(fd, (flags), 1, &cursor, counts, props,                                                       \
           (uint64_t[]){(fb), (fb) ? g_crtc : 0, (uint64_t)(int64_t)(x), (uint64_t)(int64_t)(y), (w), (h),  \
                        (uint64_t)(sx) << 16, 0, (uint64_t)(sw) << 16, (uint64_t)(sh) << 16},          \
           0)
    const uint32_t t = DRM_MODE_ATOMIC_TEST_ONLY, flip = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    /* The plane is on no CRTC yet: a commit of it alone has none for an event. */
    result("PAGE_FLIP_EVENT, only a plane on no CRTC",
           commit(fd, flip, 1, &cursor, (uint32_t[]){1}, &props[2], (uint64_t[]){5}, 0));
    result("cursor 64x64 at 100,100", CURSOR(0, fb64, 100, 100, 64, 64, 0, 64, 64));
    result("cursor alone, NONBLOCK | PAGE_FLIP_EVENT", CURSOR(flip, fb64, 120, 110, 64, 64, 0, 64, 64));
    result("...again while it is pending", CURSOR(flip, fb64, 130, 110, 64, 64, 0, 64, 64));
    char ev[64];
    struct pollfd pfd = {fd, POLLIN, 0};
    printf("%-60s %s\n", "  its event arrives", yes(poll(&pfd, 1, 1000) == 1 && read(fd, ev, sizeof ev) > 0));
    result("cursor partly off the top left (test)", CURSOR(t, fb64, -20, -30, 64, 64, 0, 64, 64));
    result("cursor past the right edge (test)", CURSOR(t, fb64, g_mode.hdisplay - 10, 10, 64, 64, 0, 64, 64));
    result("cursor off the screen (test)", CURSOR(t, fb64, g_mode.hdisplay + 10, 10, 64, 64, 0, 64, 64));
    result("cursor 128x128 (test)", CURSOR(t, fb128, 10, 10, 128, 128, 0, 128, 128));
    result("cursor scaled 64x64 to 32x32 (test)", CURSOR(t, fb64, 10, 10, 32, 32, 0, 64, 64));
    result("cursor 32x32 part of its buffer (test)", CURSOR(t, fb64, 10, 10, 32, 32, 0, 32, 32));
    result("cursor part past its buffer's edge (test)", CURSOR(t, fb64, 10, 10, 32, 32, 48, 32, 32));
    result("cursor XRGB8888 (test)", CURSOR(t, fbx, 10, 10, 64, 64, 0, 64, 64));
    uint32_t one = 1;
    uint64_t zero = 0, crtc = g_crtc;
    result("cursor framebuffer without a CRTC (test)", commit(fd, t, 1, &cursor, &one, &props[1], &zero, 0));
    result("cursor off", CURSOR(0, 0, 0, 0, 0, 0, 0, 0, 0));
    result("cursor CRTC without a framebuffer (test)", commit(fd, t, 1, &cursor, &one, &props[1], &crtc, 0));
    uint64_t fb_id = 9, crtc_id = 9;
    prop(fd, cursor, DRM_MODE_OBJECT_PLANE, "FB_ID", &fb_id);
    prop(fd, cursor, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &crtc_id);
    printf("%-60s %s, %s\n", "  cursor FB_ID / CRTC_ID when off", fb_id ? "set" : "0", crtc_id ? "set" : "0");
#undef CURSOR
}

static uint32_t framebuffer(int fd)
{
    uint32_t pitch, handle = buffer(fd, &pitch);
    struct drm_mode_fb_cmd2 c = {.width = g_mode.hdisplay, .height = g_mode.vdisplay,
                                 .pixel_format = DRM_FORMAT_XRGB8888, .handles = {handle}, .pitches = {pitch}};
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &c))
        result("ADDFB2", -1);
    return c.fb_id;
}

static int commit(int fd, uint32_t flags, uint32_t n, const uint32_t *objs, const uint32_t *counts,
                  const uint32_t *props, const uint64_t *values, uint64_t reserved)
{
    struct drm_mode_atomic a = {.flags = flags, .count_objs = n, .objs_ptr = (uintptr_t)objs,
                                .count_props_ptr = (uintptr_t)counts, .props_ptr = (uintptr_t)props,
                                .prop_values_ptr = (uintptr_t)values, .reserved = reserved};
    return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &a);
}

/* ---- the legacy calls (the kernel serves them through atomic helpers) ---- */

static int set_crtc(int fd, uint32_t crtc, uint32_t fb, uint32_t x, uint32_t y, const uint32_t *conns, uint32_t n,
                    const struct drm_mode_modeinfo *mode)
{
    struct drm_mode_crtc c = {.set_connectors_ptr = (uintptr_t)conns, .count_connectors = n, .crtc_id = crtc,
                              .fb_id = fb, .x = x, .y = y, .mode_valid = mode != NULL};
    if (mode)
        c.mode = *mode;
    return ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &c);
}

static int page_flip(int fd, uint32_t crtc, uint32_t fb, uint32_t flags, uint64_t user_data)
{
    struct drm_mode_crtc_page_flip f = {.crtc_id = crtc, .fb_id = fb, .flags = flags, .user_data = user_data};
    return ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &f);
}

/* Waits for the card's next event and prints what kind it is. */
static void event(int fd, const char *what, uint64_t user_data)
{
    struct drm_event_vblank ev = {0};
    struct pollfd pfd = {fd, POLLIN, 0};
    int got = poll(&pfd, 1, 2000) == 1 && read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev;
    printf("%-60s %s, type %u, user data %s, crtc %s, sequence %s\n", what, got ? "arrives" : "MISSING",
           ev.base.type, yes(ev.user_data == user_data), yes(ev.crtc_id == g_crtc), ev.sequence ? "set" : "0");
}

static void no_event(int fd, const char *what)
{
    struct pollfd pfd = {fd, POLLIN, 0};
    printf("%-60s %s\n", what, yes(poll(&pfd, 1, 200) == 0));
}

static void crtc_state(int fd, const char *when, uint32_t fb)
{
    struct drm_mode_crtc c = {.crtc_id = g_crtc};
    ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c);
    uint64_t dpms = 9, active = 9, pfb = 9, pcrtc = 9, ccrtc = 9;
    prop(fd, g_conn, DRM_MODE_OBJECT_CONNECTOR, "DPMS", &dpms);
    prop(fd, g_conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &ccrtc);
    prop(fd, g_crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", &active);
    prop(fd, g_plane, DRM_MODE_OBJECT_PLANE, "FB_ID", &pfb);
    prop(fd, g_plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &pcrtc);
    struct drm_mode_get_encoder e = {.encoder_id = g_enc};
    ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e);
    struct drm_mode_get_plane pl = {.plane_id = g_plane};
    ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &pl);
    printf("  %s: mode_valid %u, crtc fb %s, x %u y %u, ACTIVE %llu, DPMS %llu, connector crtc %s, encoder crtc %s, "
           "plane fb %s crtc %s, GETPLANE fb %s crtc %s\n",
           when, c.mode_valid, !c.fb_id ? "0" : c.fb_id == fb ? "it" : "another", c.x, c.y,
           (unsigned long long)active, (unsigned long long)dpms, ccrtc ? "set" : "0", e.crtc_id ? "set" : "0",
           !pfb ? "0" : pfb == fb ? "it" : "another", pcrtc ? "set" : "0",
           !pl.fb_id ? "0" : pl.fb_id == fb ? "it" : "another", pl.crtc_id ? "set" : "0");
}

static void legacy_checks(int fd)
{
    const uint32_t w = g_mode.hdisplay, h = g_mode.vdisplay;
    uint32_t fb_a = framebuffer(fd), fb_b = framebuffer(fd);
    uint32_t fb_small = sized_framebuffer(fd, w / 2, h / 2, DRM_FORMAT_XRGB8888);
    uint32_t fb_big = sized_framebuffer(fd, w + 64, h + 64, DRM_FORMAT_XRGB8888);
    uint32_t fb_argb = sized_framebuffer(fd, w, h, DRM_FORMAT_ARGB8888);
    uint32_t unknown = 999999;
    struct drm_mode_modeinfo bad_mode = g_mode;
    bad_mode.hdisplay = 0;

    printf("== SETCRTC\n");
    crtc_state(fd, "before", fb_a);
    result("mode, framebuffer, connector", set_crtc(fd, g_crtc, fb_a, 0, 0, &g_conn, 1, &g_mode));
    crtc_state(fd, "after", fb_a);
    result("unknown CRTC", set_crtc(fd, unknown, fb_a, 0, 0, &g_conn, 1, &g_mode));
    result("unknown framebuffer", set_crtc(fd, g_crtc, unknown, 0, 0, &g_conn, 1, &g_mode));
    result("unknown connector", set_crtc(fd, g_crtc, fb_a, 0, 0, &unknown, 1, &g_mode));
    result("a mode without connectors", set_crtc(fd, g_crtc, fb_a, 0, 0, NULL, 0, &g_mode));
    result("connectors without a mode", set_crtc(fd, g_crtc, fb_a, 0, 0, &g_conn, 1, NULL));
    result("connector count 1, NULL pointer", set_crtc(fd, g_crtc, fb_a, 0, 0, NULL, 1, &g_mode));
    result("a mode 0 wide", set_crtc(fd, g_crtc, fb_a, 0, 0, &g_conn, 1, &bad_mode));
    result("a framebuffer smaller than the mode", set_crtc(fd, g_crtc, fb_small, 0, 0, &g_conn, 1, &g_mode));
    result("x 1 with a framebuffer of the mode's size", set_crtc(fd, g_crtc, fb_a, 1, 0, &g_conn, 1, &g_mode));
    result("x 64 y 64 into a bigger framebuffer", set_crtc(fd, g_crtc, fb_big, 64, 64, &g_conn, 1, &g_mode));
    crtc_state(fd, "after", fb_big);
    result("x 65 into it", set_crtc(fd, g_crtc, fb_big, 65, 0, &g_conn, 1, &g_mode));
    result("framebuffer -1 (the current one)", set_crtc(fd, g_crtc, (uint32_t)-1, 0, 0, &g_conn, 1, &g_mode));
    crtc_state(fd, "after", fb_big);
    result("back to the first framebuffer", set_crtc(fd, g_crtc, fb_a, 0, 0, &g_conn, 1, &g_mode));

    printf("== PAGE_FLIP\n");
    result("unknown flag", page_flip(fd, g_crtc, fb_b, 0x100, 0));
    result("ASYNC", page_flip(fd, g_crtc, fb_b, DRM_MODE_PAGE_FLIP_ASYNC, 0));
    result("TARGET_ABSOLUTE", page_flip(fd, g_crtc, fb_b, DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE, 0));
    result("unknown CRTC", page_flip(fd, unknown, fb_b, 0, 0));
    result("unknown framebuffer", page_flip(fd, g_crtc, unknown, 0, 0));
    result("a smaller framebuffer", page_flip(fd, g_crtc, fb_small, 0, 0));
    result("a bigger framebuffer", page_flip(fd, g_crtc, fb_big, 0, 0));
    result("...another at once (the first hasn't completed)", page_flip(fd, g_crtc, fb_a, 0, 0));
    result("a framebuffer of another format", page_flip(fd, g_crtc, fb_argb, 0, 0));
    usleep(200000);
    crtc_state(fd, "after", fb_big);
    result("back to the first framebuffer", page_flip(fd, g_crtc, fb_a, 0, 0));
    usleep(200000);
    result("with an event", page_flip(fd, g_crtc, fb_b, DRM_MODE_PAGE_FLIP_EVENT, 0x1234));
    result("...again while it is pending", page_flip(fd, g_crtc, fb_a, DRM_MODE_PAGE_FLIP_EVENT, 1));
    result("...and without an event", page_flip(fd, g_crtc, fb_a, 0, 0));
    event(fd, "  its event", 0x1234);
    crtc_state(fd, "after", fb_b);
    result("without an event", page_flip(fd, g_crtc, fb_a, 0, 0));
    no_event(fd, "  no event comes");
    crtc_state(fd, "after", fb_a);
    result("the framebuffer already shown, with an event", page_flip(fd, g_crtc, fb_a, DRM_MODE_PAGE_FLIP_EVENT, 7));
    event(fd, "  its event", 7);
    usleep(200000);

    printf("== vblanks\n");
    union drm_wait_vblank vb = {.request = {.type = _DRM_VBLANK_RELATIVE, .sequence = 0}};
    result("WAIT_VBLANK relative 0", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vb));
    uint32_t seq0 = vb.reply.sequence;
    printf("%-60s %s, %s\n", "  sequence / time set", yes(seq0 > 0), yes(vb.reply.tval_sec || vb.reply.tval_usec));
    vb = (union drm_wait_vblank){.request = {.type = _DRM_VBLANK_RELATIVE, .sequence = 2}};
    result("WAIT_VBLANK relative 2", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vb));
    printf("%-60s %s\n", "  sequence is 2 or 3 later", yes(vb.reply.sequence - seq0 >= 2 && vb.reply.sequence - seq0 <= 3));
    seq0 = vb.reply.sequence;
    vb = (union drm_wait_vblank){.request = {.type = _DRM_VBLANK_ABSOLUTE, .sequence = seq0 - 5}};
    result("WAIT_VBLANK absolute, in the past", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vb));
    printf("%-60s %s\n", "  answered with the current sequence", yes(vb.reply.sequence - seq0 <= 1));
    vb = (union drm_wait_vblank){.request = {.type = _DRM_VBLANK_RELATIVE | _DRM_VBLANK_EVENT, .sequence = 1,
                                             .signal = 0x77}};
    result("WAIT_VBLANK with an event", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vb));
    event(fd, "  its event", 0x77);
    vb = (union drm_wait_vblank){.request = {.type = _DRM_VBLANK_RELATIVE | (1 << _DRM_VBLANK_HIGH_CRTC_SHIFT)}};
    result("WAIT_VBLANK on a second CRTC", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vb));
    vb = (union drm_wait_vblank){.request = {.type = _DRM_VBLANK_RELATIVE | _DRM_VBLANK_SECONDARY}};
    result("WAIT_VBLANK SECONDARY", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vb));
    vb = (union drm_wait_vblank){.request = {.type = _DRM_VBLANK_RELATIVE | 0x00800000}};
    result("WAIT_VBLANK unknown flag", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vb));
    vb = (union drm_wait_vblank){.request = {.type = _DRM_VBLANK_RELATIVE | _DRM_VBLANK_SIGNAL}};
    result("WAIT_VBLANK SIGNAL", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vb));
    struct drm_crtc_get_sequence gs = {.crtc_id = g_crtc};
    result("CRTC_GET_SEQUENCE", ioctl(fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &gs));
    printf("%-60s %u, %s, %s\n", "  active / sequence set / time set", gs.active, yes(gs.sequence > 0),
           yes(gs.sequence_ns > 0));
    result("CRTC_GET_SEQUENCE unknown CRTC",
           ioctl(fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &(struct drm_crtc_get_sequence){.crtc_id = unknown}));
    struct drm_crtc_queue_sequence qs = {.crtc_id = g_crtc, .flags = DRM_CRTC_SEQUENCE_RELATIVE, .sequence = 1,
                                         .user_data = 0x99};
    result("CRTC_QUEUE_SEQUENCE relative 1", ioctl(fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &qs));
    printf("%-60s %s\n", "  sequence answered is later", yes(qs.sequence > gs.sequence));
    struct drm_event_crtc_sequence se = {0};
    struct pollfd pfd = {fd, POLLIN, 0};
    int got = poll(&pfd, 1, 2000) == 1 && read(fd, &se, sizeof se) == (ssize_t)sizeof se;
    printf("%-60s %s, type %u, user data %s, sequence %s\n", "  its event", got ? "arrives" : "MISSING", se.base.type,
           yes(se.user_data == 0x99), yes(se.sequence == qs.sequence));
    qs = (struct drm_crtc_queue_sequence){.crtc_id = g_crtc, .flags = 0x10, .sequence = 1};
    result("CRTC_QUEUE_SEQUENCE unknown flag", ioctl(fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &qs));
    qs = (struct drm_crtc_queue_sequence){.crtc_id = g_crtc, .flags = 0, .sequence = 1};
    result("CRTC_QUEUE_SEQUENCE absolute, in the past", ioctl(fd, DRM_IOCTL_CRTC_QUEUE_SEQUENCE, &qs));
    got = poll(&pfd, 1, 2000) == 1 && read(fd, &se, sizeof se) == (ssize_t)sizeof se;
    printf("%-60s %s\n", "  its event", got ? "arrives" : "MISSING");

    printf("== ADDFB, GETFB\n");
    uint32_t pitch, handle = buffer(fd, &pitch);
    struct drm_mode_fb_cmd a = {.width = w, .height = h, .pitch = pitch, .bpp = 32, .depth = 24, .handle = handle};
    result("ADDFB depth 24, 32 bits a pixel", ioctl(fd, DRM_IOCTL_MODE_ADDFB, &a));
    struct drm_mode_fb_cmd2 g2 = {.fb_id = a.fb_id};
    result("GETFB2 of it", ioctl(fd, DRM_IOCTL_MODE_GETFB2, &g2));
    printf("%-60s %s, %ux%u %s, handle %s, pitch %s\n", "  format / size / handle / pitch",
           g2.pixel_format == DRM_FORMAT_XRGB8888 ? "XRGB8888" : "another", g2.width == w ? 1 : 0, g2.height == h ? 1 : 0,
           "as made", !g2.handles[0] ? "0" : g2.handles[0] == handle ? "the same" : "a new one",
           yes(g2.pitches[0] == pitch));
    struct drm_mode_fb_cmd a32 = {.width = w, .height = h, .pitch = pitch, .bpp = 32, .depth = 32, .handle = handle};
    result("ADDFB depth 32, 32 bits a pixel", ioctl(fd, DRM_IOCTL_MODE_ADDFB, &a32));
    g2 = (struct drm_mode_fb_cmd2){.fb_id = a32.fb_id};
    ioctl(fd, DRM_IOCTL_MODE_GETFB2, &g2);
    printf("%-60s %s\n", "  its format", g2.pixel_format == DRM_FORMAT_ARGB8888 ? "ARGB8888" : "another");
    struct drm_mode_fb_cmd a15 = {.width = w, .height = h, .pitch = pitch, .bpp = 16, .depth = 15, .handle = handle};
    result("ADDFB depth 15", ioctl(fd, DRM_IOCTL_MODE_ADDFB, &a15));
    struct drm_mode_fb_cmd a7 = {.width = w, .height = h, .pitch = pitch, .bpp = 7, .depth = 7, .handle = handle};
    result("ADDFB 7 bits a pixel", ioctl(fd, DRM_IOCTL_MODE_ADDFB, &a7));
    struct drm_mode_fb_cmd a0 = {.width = w, .height = h, .pitch = pitch, .bpp = 32, .depth = 24, .handle = 7777};
    result("ADDFB unknown handle", ioctl(fd, DRM_IOCTL_MODE_ADDFB, &a0));
    struct drm_mode_fb_cmd g = {.fb_id = fb_a};
    result("GETFB", ioctl(fd, DRM_IOCTL_MODE_GETFB, &g));
    printf("%-60s %s, %u, %u, handle %s\n", "  size as made / bpp / depth / handle", yes(g.width == w && g.height == h),
           g.bpp, g.depth, g.handle ? "set" : "0");
    struct drm_mode_fb_cmd gg = {.fb_id = fb_a};
    ioctl(fd, DRM_IOCTL_MODE_GETFB, &gg);
    printf("%-60s %s\n", "  a second GETFB gives the same handle", yes(g.handle && g.handle == gg.handle));
    result("GETFB unknown framebuffer", ioctl(fd, DRM_IOCTL_MODE_GETFB, &(struct drm_mode_fb_cmd){.fb_id = unknown}));
    result("GETFB2 unknown framebuffer", ioctl(fd, DRM_IOCTL_MODE_GETFB2, &(struct drm_mode_fb_cmd2){.fb_id = unknown}));

    printf("== SETPROPERTY, DPMS\n");
    uint32_t p_dpms = prop(fd, g_conn, DRM_MODE_OBJECT_CONNECTOR, "DPMS", NULL);
    uint32_t p_edid = prop(fd, g_conn, DRM_MODE_OBJECT_CONNECTOR, "EDID", NULL);
    uint32_t p_active = prop(fd, g_crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
    uint32_t p_fb = prop(fd, g_plane, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
#define SETPROP(conn, p, v) \
    ioctl(fd, DRM_IOCTL_MODE_SETPROPERTY, &(struct drm_mode_connector_set_property){.value = (v), .prop_id = (p), .connector_id = (conn)})
#define OBJPROP(obj, type, p, v) \
    ioctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &(struct drm_mode_obj_set_property){.value = (v), .prop_id = (p), .obj_id = (obj), .obj_type = (type)})
    result("DPMS 7 (not a value of it)", SETPROP(g_conn, p_dpms, 7));
    crtc_state(fd, "after", fb_a);
    result("DPMS on", SETPROP(g_conn, p_dpms, DRM_MODE_DPMS_ON));
    result("unknown connector", SETPROP(unknown, p_dpms, 0));
    result("unknown property", SETPROP(g_conn, unknown, 0));
    result("a property of another object", SETPROP(g_conn, p_active, 0));
    result("EDID (immutable)", SETPROP(g_conn, p_edid, 0));
    result("DPMS off", SETPROP(g_conn, p_dpms, DRM_MODE_DPMS_OFF));
    crtc_state(fd, "after", fb_a);
    result("a page flip with an event while off", page_flip(fd, g_crtc, fb_b, DRM_MODE_PAGE_FLIP_EVENT, 3));
    result("a page flip without one while off", page_flip(fd, g_crtc, fb_b, 0, 0));
    union drm_wait_vblank voff = {.request = {.type = _DRM_VBLANK_RELATIVE, .sequence = 0}};
    result("WAIT_VBLANK relative 0 while off", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &voff));
    voff = (union drm_wait_vblank){.request = {.type = _DRM_VBLANK_RELATIVE, .sequence = 1}};
    result("WAIT_VBLANK relative 1 while off", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &voff));
    gs = (struct drm_crtc_get_sequence){.crtc_id = g_crtc};
    result("CRTC_GET_SEQUENCE while off", ioctl(fd, DRM_IOCTL_CRTC_GET_SEQUENCE, &gs));
    printf("%-60s %u\n", "  active", gs.active);
    result("DPMS standby", SETPROP(g_conn, p_dpms, DRM_MODE_DPMS_STANDBY));
    crtc_state(fd, "after", fb_a);
    result("DPMS on", SETPROP(g_conn, p_dpms, DRM_MODE_DPMS_ON));
    crtc_state(fd, "after", fb_a);
    result("OBJ_SETPROPERTY DPMS off", OBJPROP(g_conn, DRM_MODE_OBJECT_CONNECTOR, p_dpms, DRM_MODE_DPMS_OFF));
    result("OBJ_SETPROPERTY DPMS on", OBJPROP(g_conn, DRM_MODE_OBJECT_CONNECTOR, p_dpms, DRM_MODE_DPMS_ON));
    result("OBJ_SETPROPERTY wrong object type", OBJPROP(g_conn, DRM_MODE_OBJECT_CRTC, p_dpms, 0));
    result("OBJ_SETPROPERTY the plane's framebuffer", OBJPROP(g_plane, DRM_MODE_OBJECT_PLANE, p_fb, fb_b));
    crtc_state(fd, "after", fb_b);
    result("OBJ_SETPROPERTY ACTIVE 0", OBJPROP(g_crtc, DRM_MODE_OBJECT_CRTC, p_active, 0));
    crtc_state(fd, "after", fb_b);
    result("OBJ_SETPROPERTY ACTIVE 1", OBJPROP(g_crtc, DRM_MODE_OBJECT_CRTC, p_active, 1));
    crtc_state(fd, "after", fb_b);

    printf("== SETPLANE\n");
#define SETPLANE(plane, crtc, fb, x, y, cw, ch, sx, sy, sw, sh)                                              \
    ioctl(fd, DRM_IOCTL_MODE_SETPLANE,                                                                       \
          &(struct drm_mode_set_plane){.plane_id = (plane), .crtc_id = (crtc), .fb_id = (fb), .crtc_x = (x),  \
                                       .crtc_y = (y), .crtc_w = (cw), .crtc_h = (ch), .src_x = (sx) << 16,    \
                                       .src_y = (sy) << 16, .src_w = (uint32_t)(sw) << 16,                    \
                                       .src_h = (uint32_t)(sh) << 16})
    result("the primary plane, another framebuffer", SETPLANE(g_plane, g_crtc, fb_a, 0, 0, w, h, 0, 0, w, h));
    crtc_state(fd, "after", fb_a);
    result("unknown plane", SETPLANE(unknown, g_crtc, fb_a, 0, 0, w, h, 0, 0, w, h));
    result("unknown CRTC", SETPLANE(g_plane, unknown, fb_a, 0, 0, w, h, 0, 0, w, h));
    result("unknown framebuffer", SETPLANE(g_plane, g_crtc, unknown, 0, 0, w, h, 0, 0, w, h));
    result("a source past the framebuffer", SETPLANE(g_plane, g_crtc, fb_a, 0, 0, w, h, 1, 0, w, h));
    result("a size past INT_MAX", SETPLANE(g_plane, g_crtc, fb_a, 0, 0, 0x80000000u, h, 0, 0, w, h));
    result("an x and width past INT_MAX", SETPLANE(g_plane, g_crtc, fb_a, 0x7fffff00, 0, w, h, 0, 0, w, h));
    result("the plane off (framebuffer 0)", SETPLANE(g_plane, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    crtc_state(fd, "after", fb_a);
    result("a page flip with the plane off", page_flip(fd, g_crtc, fb_a, 0, 0));
    result("the plane on again", SETPLANE(g_plane, g_crtc, fb_a, 0, 0, w, h, 0, 0, w, h));

    printf("== the legacy cursor\n");
    uint32_t cpitch, chandle = sized_buffer(fd, 64, 64, DRM_FORMAT_ARGB8888, &cpitch);
#define CURSOR(f, x_, y_, w_, h_, hnd) \
    ioctl(fd, DRM_IOCTL_MODE_CURSOR, &(struct drm_mode_cursor){.flags = (f), .crtc_id = g_crtc, .x = (x_), .y = (y_), .width = (w_), .height = (h_), .handle = (hnd)})
    result("no flags", CURSOR(0, 0, 0, 0, 0, 0));
    result("unknown flag", CURSOR(4, 0, 0, 0, 0, 0));
    result("unknown CRTC", ioctl(fd, DRM_IOCTL_MODE_CURSOR,
                                 &(struct drm_mode_cursor){.flags = DRM_MODE_CURSOR_MOVE, .crtc_id = unknown}));
    result("an image, unknown handle", CURSOR(DRM_MODE_CURSOR_BO, 0, 0, 64, 64, 7777));
    result("an image 64x64", CURSOR(DRM_MODE_CURSOR_BO, 0, 0, 64, 64, chandle));
    result("moved to 100,200", CURSOR(DRM_MODE_CURSOR_MOVE, 100, 200, 0, 0, 0));
    result("moved off the top left", CURSOR(DRM_MODE_CURSOR_MOVE, -30, -30, 0, 0, 0));
    result("an image and a move at once", CURSOR(DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE, 10, 10, 64, 64, chandle));
    result("an image of a size the buffer doesn't hold", CURSOR(DRM_MODE_CURSOR_BO, 0, 0, 128, 128, chandle));
    result("an image wider than the cursor limit", CURSOR(DRM_MODE_CURSOR_BO, 0, 0, 4096, 64, chandle));
    result("CURSOR2 with a hot spot",
           ioctl(fd, DRM_IOCTL_MODE_CURSOR2,
                 &(struct drm_mode_cursor2){.flags = DRM_MODE_CURSOR_BO, .crtc_id = g_crtc, .width = 64, .height = 64,
                                            .handle = chandle, .hot_x = 3, .hot_y = 4}));
    result("the image off (handle 0)", CURSOR(DRM_MODE_CURSOR_BO, 0, 0, 0, 0, 0));
    result("moved while off", CURSOR(DRM_MODE_CURSOR_MOVE, 50, 50, 0, 0, 0));

    printf("== DIRTYFB\n");
    struct drm_clip_rect clip = {0, 0, 16, 16};
    result("the shown framebuffer, no rectangles", ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &(struct drm_mode_fb_dirty_cmd){.fb_id = fb_a}));
    result("one rectangle",
           ioctl(fd, DRM_IOCTL_MODE_DIRTYFB,
                 &(struct drm_mode_fb_dirty_cmd){.fb_id = fb_a, .num_clips = 1, .clips_ptr = (uintptr_t)&clip}));
    result("a framebuffer not shown", ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &(struct drm_mode_fb_dirty_cmd){.fb_id = fb_b}));
    result("unknown framebuffer", ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &(struct drm_mode_fb_dirty_cmd){.fb_id = unknown}));
    result("rectangles counted, NULL pointer",
           ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &(struct drm_mode_fb_dirty_cmd){.fb_id = fb_a, .num_clips = 1}));
    result("unknown flag", ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &(struct drm_mode_fb_dirty_cmd){.fb_id = fb_a, .flags = 8}));

    printf("== the CRTC off\n");
    result("SETCRTC without a mode", set_crtc(fd, g_crtc, 0, 0, 0, NULL, 0, NULL));
    crtc_state(fd, "after", fb_a);
    result("a page flip", page_flip(fd, g_crtc, fb_a, 0, 0));
    result("SETCRTC framebuffer -1", set_crtc(fd, g_crtc, (uint32_t)-1, 0, 0, &g_conn, 1, &g_mode));
    result("DPMS on", SETPROP(g_conn, p_dpms, DRM_MODE_DPMS_ON));
    crtc_state(fd, "after", fb_a);
    result("on again", set_crtc(fd, g_crtc, fb_a, 0, 0, &g_conn, 1, &g_mode));
    crtc_state(fd, "after", fb_a);
    result("RMFB of the shown framebuffer", ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_a));
    crtc_state(fd, "after", fb_a);
}

/* ---- dumb buffers: memory the program draws into itself ---- */

static void dumb_checks(int fd)
{
    printf("== dumb buffers\n");
    struct drm_get_cap cap = {.capability = DRM_CAP_DUMB_BUFFER};
    ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
    printf("%-60s %llu\n", "DRM_CAP_DUMB_BUFFER", (unsigned long long)cap.value);
#define CREATE(w, h, b) ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &(struct drm_mode_create_dumb){.width = (w), .height = (h), .bpp = (b)})
    result("no width", CREATE(0, 64, 32));
    result("no height", CREATE(64, 0, 32));
    result("no bits a pixel", CREATE(64, 64, 0));
    result("a size that overflows", CREATE(0x40000000, 0x40000000, 32));
    result("a pitch that overflows", CREATE(0xffffffff, 1, 32));
#undef CREATE
    static const uint32_t sizes[][3] = {{64, 64, 32}, {100, 10, 32}, {50, 50, 16}, {33, 3, 24}, {1, 1, 8}};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        struct drm_mode_create_dumb d = {.width = sizes[i][0], .height = sizes[i][1], .bpp = sizes[i][2]};
        char what[64];
        snprintf(what, sizeof what, "%ux%u at %u bits", d.width, d.height, d.bpp);
        int ret = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d);
        result(what, ret);
        printf("%-60s pitch %u, size %llu, handle %s\n", "", d.pitch, (unsigned long long)d.size, yes(d.handle));
        if (!ret)
            ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &(struct drm_mode_destroy_dumb){d.handle});
    }

    struct drm_mode_create_dumb d = {.width = 64, .height = 64, .bpp = 32};
    ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d);
    struct drm_mode_map_dumb m = {.handle = d.handle + 77};
    result("MAP_DUMB of an unknown handle", ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m));
    m = (struct drm_mode_map_dumb){.handle = d.handle};
    result("MAP_DUMB", ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m));
    struct drm_mode_map_dumb m2 = {.handle = d.handle};
    ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m2);
    printf("%-60s %s, %s\n", "  an offset, the same each time", yes(m.offset != 0), yes(m.offset == m2.offset));
    printf("%-60s %s\n", "  a whole number of pages", yes(m.offset % 4096 == 0));

#define MAP(len, prot, flags, off) mmap(NULL, (len), (prot), (flags), fd, (off_t)(off))
#define TRY(what, len, prot, flags, off)                         \
    do {                                                         \
        void *t_ = MAP(len, prot, flags, off);                   \
        result(what, t_ == MAP_FAILED ? -1 : 0);                 \
        if (t_ != MAP_FAILED)                                    \
            munmap(t_, (len));                                   \
    } while (0)
    uint32_t *a = MAP(d.size, PROT_READ | PROT_WRITE, MAP_SHARED, m.offset);
    result("mmap of the card at that offset", a == MAP_FAILED ? -1 : 0);
    if (a == MAP_FAILED)
        return;
    int zero = 1;
    for (unsigned i = 0; i < d.size / 4; i++)
        zero &= a[i] == 0;
    printf("%-60s %s\n", "  new memory is zero", yes(zero));
    a[0] = 0x11223344;
    a[64 * 64 - 1] = 0x55667788;
    uint32_t *b = MAP(d.size, PROT_READ, MAP_SHARED, m.offset);
    result("a second mapping", b == MAP_FAILED ? -1 : 0);
    if (b != MAP_FAILED) {
        printf("%-60s %s\n", "  shows what the first wrote", yes(b[0] == 0x11223344 && b[64 * 64 - 1] == 0x55667788));
        munmap(b, d.size);
    }
    TRY("a mapping of its first page only", 4096, PROT_READ, MAP_SHARED, m.offset);
    TRY("a mapping longer than the buffer", d.size + 4096, PROT_READ, MAP_SHARED, m.offset);
    TRY("a mapping from its second page", 4096, PROT_READ, MAP_SHARED, m.offset + 4096);
    TRY("a mapping at an offset nothing has", 4096, PROT_READ, MAP_SHARED, m.offset + (1ull << 30));
    TRY("a mapping at offset 0", 4096, PROT_READ, MAP_SHARED, 0);
    TRY("a private writable mapping", d.size, PROT_READ | PROT_WRITE, MAP_PRIVATE, m.offset);
    TRY("a private read-only mapping", d.size, PROT_READ, MAP_PRIVATE, m.offset);
    const char *path = getenv("CHAMELEON_DRM_PATH");
    int other = open(path ? path : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    void *o = mmap(NULL, d.size, PROT_READ, MAP_SHARED, other, (off_t)m.offset);
    result("a mapping through another open file of the card", o == MAP_FAILED ? -1 : 0);
    if (o != MAP_FAILED)
        munmap(o, d.size);
    struct drm_mode_map_dumb om = {.handle = d.handle};
    result("MAP_DUMB of the handle in that file", ioctl(other, DRM_IOCTL_MODE_MAP_DUMB, &om));

    printf("== a dumb buffer through PRIME\n");
    struct drm_prime_handle ex = {.handle = d.handle, .flags = DRM_CLOEXEC};
    result("PRIME_HANDLE_TO_FD of a dumb buffer", ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ex));
    printf("%-60s %s\n", "  as long as the buffer", yes(lseek(ex.fd, 0, SEEK_END) == (off_t)d.size));
    uint32_t *e = mmap(NULL, d.size, PROT_READ, MAP_SHARED, ex.fd, 0);
    result("a mapping of what it gave", e == MAP_FAILED ? -1 : 0);
    if (e != MAP_FAILED) {
        printf("%-60s %s\n", "  shows the buffer's memory", yes(e[0] == 0x11223344 && e[64 * 64 - 1] == 0x55667788));
        munmap(e, d.size);
    }
    struct pollfd pe = {.fd = ex.fd, .events = POLLIN};
    printf("%-60s %d\n", "  poll for reading: ready at once", poll(&pe, 1, 0));
    struct drm_prime_handle im = {.fd = ex.fd};
    result("PRIME_FD_TO_HANDLE of it", ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &im));
    printf("%-60s %s\n", "  the same handle", yes(im.handle == d.handle));
    struct drm_prime_handle oi = {.fd = ex.fd};
    result("PRIME_FD_TO_HANDLE in another open file", ioctl(other, DRM_IOCTL_PRIME_FD_TO_HANDLE, &oi));
    om = (struct drm_mode_map_dumb){.handle = oi.handle};
    result("MAP_DUMB of the handle it got", ioctl(other, DRM_IOCTL_MODE_MAP_DUMB, &om));
    uint32_t *oe = mmap(NULL, d.size, PROT_READ, MAP_SHARED, other, (off_t)om.offset);
    result("a mapping through that file", oe == MAP_FAILED ? -1 : 0);
    if (oe != MAP_FAILED) {
        printf("%-60s %s\n", "  shows the buffer's memory", yes(oe[0] == 0x11223344));
        munmap(oe, d.size);
    }
    /* What Mesa's software driver does: the handles go, the file stays. */
    struct drm_mode_create_dumb k = {.width = 32, .height = 32, .bpp = 32};
    ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &k);
    struct drm_prime_handle kx = {.handle = k.handle, .flags = DRM_CLOEXEC};
    ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &kx);
    result("DESTROY_DUMB of an exported buffer", ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &(struct drm_mode_destroy_dumb){k.handle}));
    struct drm_prime_handle ki = {.fd = kx.fd};
    result("PRIME_FD_TO_HANDLE of its file after that", ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ki));
    struct drm_mode_map_dumb km = {.handle = ki.handle};
    result("MAP_DUMB of the handle it got", ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &km));
    struct drm_mode_fb_cmd kf = {.width = 32, .height = 32, .pitch = 128, .bpp = 32, .depth = 24, .handle = ki.handle};
    result("ADDFB of it", ioctl(fd, DRM_IOCTL_MODE_ADDFB, &kf));
    ioctl(fd, DRM_IOCTL_MODE_RMFB, &kf.fb_id);
    ioctl(fd, DRM_IOCTL_GEM_CLOSE, &(struct drm_gem_close){.handle = ki.handle});
    close(kx.fd);
    close(ex.fd);
    close(other);

    printf("== framebuffers of dumb buffers\n");
#define ADDFB(w, h, p, bpp_, depth_) \
    ioctl(fd, DRM_IOCTL_MODE_ADDFB, &(struct drm_mode_fb_cmd){.width = (w), .height = (h), .pitch = (p), .bpp = (bpp_), .depth = (depth_), .handle = d.handle})
    struct drm_mode_fb_cmd f = {.width = 64, .height = 64, .pitch = d.pitch, .bpp = 32, .depth = 24, .handle = d.handle};
    result("ADDFB 64x64", ioctl(fd, DRM_IOCTL_MODE_ADDFB, &f));
    result("ADDFB with a pitch below the width", ADDFB(64, 64, 128, 32, 24));
    result("ADDFB with a pitch above it, too tall for the memory", ADDFB(64, 64, 512, 32, 24));
    result("ADDFB with a pitch above it that fits", ADDFB(64, 32, 512, 32, 24));
    result("ADDFB taller than the buffer", ADDFB(64, 65, d.pitch, 32, 24));
    result("ADDFB wider than the buffer, in its memory", ADDFB(128, 32, 512, 32, 24));
    result("ADDFB of part of it", ADDFB(32, 32, d.pitch, 32, 24));
#undef ADDFB
    struct drm_mode_fb_cmd2 f2 = {.width = 64, .height = 64, .pixel_format = DRM_FORMAT_ARGB8888,
                                  .handles = {d.handle}, .pitches = {d.pitch}};
    result("ADDFB2 ARGB8888", ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f2));
    f2.offsets[0] = 4096;
    result("ADDFB2 from its second page", ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f2));

    printf("== a dumb buffer on the screen\n");
    struct drm_mode_create_dumb s = {.width = g_mode.hdisplay, .height = g_mode.vdisplay, .bpp = 32};
    ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &s);
    struct drm_mode_fb_cmd sf = {.width = s.width, .height = s.height, .pitch = s.pitch, .bpp = 32, .depth = 24,
                                 .handle = s.handle};
    result("ADDFB of a screen-sized one", ioctl(fd, DRM_IOCTL_MODE_ADDFB, &sf));
    result("SETCRTC with it", set_crtc(fd, g_crtc, sf.fb_id, 0, 0, &g_conn, 1, &g_mode));
    usleep(200000);
    result("a page flip to itself", page_flip(fd, g_crtc, sf.fb_id, DRM_MODE_PAGE_FLIP_EVENT, 7));
    event(fd, "its event", 7);
    result("DESTROY_DUMB while it is shown", ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &(struct drm_mode_destroy_dumb){s.handle}));
    usleep(200000);
    result("a page flip to its framebuffer after that", page_flip(fd, g_crtc, sf.fb_id, DRM_MODE_PAGE_FLIP_EVENT, 8));
    event(fd, "its event", 8);
    result("RMFB", ioctl(fd, DRM_IOCTL_MODE_RMFB, &sf.fb_id));

    printf("== destroying\n");
    result("DESTROY_DUMB of an unknown handle",
           ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &(struct drm_mode_destroy_dumb){d.handle + 77}));
    result("DESTROY_DUMB", ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &(struct drm_mode_destroy_dumb){d.handle}));
    result("again", ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &(struct drm_mode_destroy_dumb){d.handle}));
    printf("%-60s %s\n", "  the mapping still reads", yes(a[0] == 0x11223344));
    TRY("a new mapping (a framebuffer still has the buffer)", d.size, PROT_READ, MAP_SHARED, m.offset);
    result("MAP_DUMB of the closed handle", ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m));
    ioctl(fd, DRM_IOCTL_MODE_RMFB, &f.fb_id);
    f2.offsets[0] = 0;
    munmap(a, d.size);
    struct drm_mode_create_dumb g = {.width = 16, .height = 16, .bpp = 32};
    ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &g);
    result("GEM_CLOSE of a dumb buffer's handle", ioctl(fd, DRM_IOCTL_GEM_CLOSE, &(struct drm_gem_close){.handle = g.handle}));
#undef TRY
#undef MAP
}

static void find_objects(int fd)
{
    uint32_t crtcs[4], conns[4], encs[4], planes[8];
    struct drm_mode_card_res r = {.crtc_id_ptr = (uintptr_t)crtcs, .connector_id_ptr = (uintptr_t)conns,
                                  .encoder_id_ptr = (uintptr_t)encs, .count_crtcs = 4, .count_connectors = 4,
                                  .count_encoders = 4};
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r);
    g_crtc = crtcs[0], g_conn = conns[0], g_enc = encs[0];
    struct drm_mode_modeinfo modes[64];
    struct drm_mode_get_connector c = {.connector_id = g_conn, .modes_ptr = (uintptr_t)modes, .count_modes = 64};
    ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
    g_mode = modes[0];
    struct drm_mode_get_plane_res pr = {.plane_id_ptr = (uintptr_t)planes, .count_planes = 8};
    ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
    for (uint32_t i = 0; i < pr.count_planes; i++) {
        uint64_t type = 0;
        prop(fd, planes[i], DRM_MODE_OBJECT_PLANE, "type", &type);
        if (type == 1) /* "Primary" */
            g_plane = planes[i];
    }
}

int main(void)
{
    const char *path = getenv("CHAMELEON_DRM_PATH");
    int fd = open(path ? path : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open the card");
        return 1;
    }
    struct drm_set_client_cap cap = {DRM_CLIENT_CAP_ATOMIC, 1};
    ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    find_objects(fd);

    printf("== ioctl numbers\n");
    int n = 0;
    result("FIONREAD", ioctl(fd, FIONREAD, &n));
    result("TCGETS", ioctl(fd, TCGETS, &(struct termios){0}));
    result("driver range nr 0x40 (this driver has none)",
           ioctl(fd, DRM_IOWR(0x40, struct drm_version), &(struct drm_version){0}));
    result("SYNCOBJ_EVENTFD (no syncobjs)", ioctl(fd, _IOWR('d', 0xcf, int), &n));
    result("IRQ_BUSID (no function)", ioctl(fd, _IOWR('d', 0x03, struct drm_irq_busid), &(struct drm_irq_busid){0}));

    printf("== argument sizes\n");
    struct {
        uint64_t capability;
    } small = {DRM_CAP_TIMESTAMP_MONOTONIC};
    result("GET_CAP with an 8-byte struct", ioctl(fd, _IOWR('d', 0x0c, small), &small));
    struct {
        uint64_t capability, value, extra;
    } large = {DRM_CAP_TIMESTAMP_MONOTONIC, 0, 77};
    result("GET_CAP with a 24-byte struct", ioctl(fd, _IOWR('d', 0x0c, large), &large));
    printf("%-60s %llu, %llu\n", "  value / extra after", (unsigned long long)large.value,
           (unsigned long long)large.extra);
    result("GET_CAP NULL argument", ioctl(fd, DRM_IOCTL_GET_CAP, NULL));
    struct drm_get_cap unknown_cap = {.capability = 0x7777, .value = 5};
    result("GET_CAP unknown", ioctl(fd, DRM_IOCTL_GET_CAP, &unknown_cap));
    printf("%-60s %llu\n", "  value after", (unsigned long long)unknown_cap.value);

    printf("== strings and lists\n");
    char unique[64] = "";
    struct drm_unique u = {0};
    result("GET_UNIQUE length", ioctl(fd, DRM_IOCTL_GET_UNIQUE, &u));
    printf("%-60s %s\n", "  unique length > 0", yes(u.unique_len > 0));
    u.unique = unique, u.unique_len = sizeof unique - 1;
    result("GET_UNIQUE string", ioctl(fd, DRM_IOCTL_GET_UNIQUE, &u));
    result("GET_UNIQUE too short, NULL pointer",
           ioctl(fd, DRM_IOCTL_GET_UNIQUE, &(struct drm_unique){.unique_len = 1}));
    result("GETRESOURCES counts", ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &(struct drm_mode_card_res){0}));
    result("GETRESOURCES count_crtcs 4, NULL pointer",
           ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &(struct drm_mode_card_res){.count_crtcs = 4}));
    result("GETRESOURCES count_crtcs 4, unmapped pointer",
           ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &(struct drm_mode_card_res){.count_crtcs = 4, .crtc_id_ptr = 16}));
    result("GETRESOURCES, the argument itself unmapped", ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, (void *)16));
    uint32_t fb1 = framebuffer(fd);
    framebuffer(fd);
    uint32_t ids[4];
    memset(ids, 0xff, sizeof ids);
    struct drm_mode_card_res r = {.fb_id_ptr = (uintptr_t)ids, .count_fbs = 1};
    result("GETRESOURCES with 2 framebuffers, room for 1", ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r));
    printf("%-60s %u, %s, %s\n", "  count_fbs / first written / second untouched", r.count_fbs,
           yes(ids[0] != 0xffffffffu), yes(ids[1] == 0xffffffffu));
    result("GETCONNECTOR count_encoders 4, NULL pointer",
           ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &(struct drm_mode_get_connector){.connector_id = g_conn,
                                                                                   .count_encoders = 4}));
    uint32_t pids[2] = {0, 0xffffffffu};
    uint64_t pvals[2];
    struct drm_mode_obj_get_properties op = {.obj_id = g_crtc, .obj_type = DRM_MODE_OBJECT_CRTC,
                                             .props_ptr = (uintptr_t)pids, .prop_values_ptr = (uintptr_t)pvals,
                                             .count_props = 1};
    result("OBJ_GETPROPERTIES crtc, room for 1", ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op));
    printf("%-60s %s, %s\n", "  count > 1 / second untouched", yes(op.count_props > 1), yes(pids[1] == 0xffffffffu));
    result("OBJ_GETPROPERTIES of the encoder (no properties)",
           ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &(struct drm_mode_obj_get_properties){.obj_id = g_enc}));
    result("OBJ_GETPROPERTIES crtc id as connector type",
           ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES,
                 &(struct drm_mode_obj_get_properties){.obj_id = g_crtc, .obj_type = DRM_MODE_OBJECT_CONNECTOR}));
    result("OBJ_GETPROPERTIES unknown id",
           ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &(struct drm_mode_obj_get_properties){.obj_id = 999999}));
    result("OBJ_GETPROPERTIES count 3, NULL pointers",
           ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES,
                 &(struct drm_mode_obj_get_properties){.obj_id = g_crtc, .count_props = 3}));
    struct drm_mode_property_enum enums[3];
    memset(enums, 0xff, sizeof enums);
    struct drm_mode_get_property gp = {.prop_id = prop(fd, g_plane, DRM_MODE_OBJECT_PLANE, "type", NULL),
                                       .enum_blob_ptr = (uintptr_t)enums, .count_enum_blobs = 1};
    result("GETPROPERTY plane type, room for 1 enum", ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp));
    printf("%-60s %u, %s, %s\n", "  count_enum_blobs / first written / second untouched", gp.count_enum_blobs,
           yes(enums[0].name[0] != (char)0xff), yes(enums[1].value == ~0ull));
    uint32_t p_mode = prop(fd, g_crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
    struct drm_mode_get_property gb = {.prop_id = p_mode, .count_enum_blobs = 5};
    result("GETPROPERTY MODE_ID (blob)", ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gb));
    printf("%-60s %u, %u\n", "  count_values / count_enum_blobs", gb.count_values, gb.count_enum_blobs);
    result("GETPROPERTY unknown id", ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &(struct drm_mode_get_property){.prop_id = 999999}));

    printf("== blobs\n");
    result("CREATEPROPBLOB length 0",
           ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &(struct drm_mode_create_blob){.data = (uintptr_t)&g_mode}));
    result("CREATEPROPBLOB NULL data", ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &(struct drm_mode_create_blob){.length = 8}));
    struct drm_mode_create_blob mode_blob = {.data = (uintptr_t)&g_mode, .length = sizeof g_mode};
    result("CREATEPROPBLOB mode", ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &mode_blob));
    unsigned char big[sizeof g_mode + 8];
    memset(big, 0xab, sizeof big);
    struct drm_mode_get_blob get = {.blob_id = mode_blob.blob_id, .data = (uintptr_t)big, .length = sizeof big};
    result("GETPROPBLOB with a bigger buffer", ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &get));
    printf("%-60s %s, %s\n", "  length = blob / data untouched", yes(get.length == sizeof g_mode), yes(big[0] == 0xab));
    result("GETPROPBLOB exact length, NULL pointer",
           ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB,
                 &(struct drm_mode_get_blob){.blob_id = mode_blob.blob_id, .length = sizeof g_mode}));

    printf("== PRIME\n");
    result("PRIME_HANDLE_TO_FD bad flags", ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &(struct drm_prime_handle){.flags = 4}));
    uint32_t pitch, handle = buffer(fd, &pitch);
    struct drm_prime_handle p0 = {.handle = handle};
    result("PRIME_HANDLE_TO_FD flags 0", ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p0));
    printf("%-60s %s\n", "  FD_CLOEXEC set", yes(fcntl(p0.fd, F_GETFD) & FD_CLOEXEC));
    struct drm_prime_handle p1 = {.handle = handle, .flags = DRM_CLOEXEC};
    result("PRIME_HANDLE_TO_FD DRM_CLOEXEC", ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p1));
    printf("%-60s %s\n", "  FD_CLOEXEC set", yes(fcntl(p1.fd, F_GETFD) & FD_CLOEXEC));
    result("PRIME_HANDLE_TO_FD unknown handle",
           ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &(struct drm_prime_handle){.handle = 7777}));
    result("PRIME_FD_TO_HANDLE closed fd", ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &(struct drm_prime_handle){.fd = 999}));
    result("PRIME_FD_TO_HANDLE memfd",
           ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &(struct drm_prime_handle){.fd = memfd_create("x", 0)}));

    printf("== atomic arguments\n");
    uint32_t p_fb = prop(fd, g_plane, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
    uint32_t p_active = prop(fd, g_crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
    uint32_t p_dpms = prop(fd, g_conn, DRM_MODE_OBJECT_CONNECTOR, "DPMS", NULL);
    uint32_t one = 1, unknown = 999999;
    uint64_t v_fb = fb1, v0 = 0, v1 = 1, v2 = 2;
    const uint32_t t = DRM_MODE_ATOMIC_TEST_ONLY;
    result("reserved field set", commit(fd, t, 1, &g_plane, &one, &p_fb, &v_fb, 1));
    result("TEST_ONLY with PAGE_FLIP_EVENT",
           commit(fd, t | DRM_MODE_PAGE_FLIP_EVENT, 1, &g_plane, &one, &p_fb, &v_fb, 0));
    result("unknown flag", commit(fd, 0x80000000u, 1, &g_plane, &one, &p_fb, &v_fb, 0));
    result("ASYNC flip", commit(fd, t | DRM_MODE_PAGE_FLIP_ASYNC, 1, &g_plane, &one, &p_fb, &v_fb, 0));
    result("unknown object", commit(fd, t, 1, &unknown, &one, &p_fb, &v_fb, 0));
    result("the encoder (no properties)", commit(fd, t, 1, &g_enc, &one, &p_fb, &v_fb, 0));
    result("a property the object lacks", commit(fd, t, 1, &g_crtc, &one, &p_fb, &v_fb, 0));
    result("DPMS on the connector", commit(fd, t, 1, &g_conn, &one, &p_dpms, &v0, 0));
    result("ACTIVE = 2", commit(fd, t, 1, &g_crtc, &one, &p_active, &v2, 0));
    result("NULL objects pointer, 1 object", commit(fd, t, 1, NULL, &one, &p_fb, &v_fb, 0));
    result("nothing (0 objects)", commit(fd, t, 0, NULL, NULL, NULL, NULL, 0));
    /* An event is some CRTC's: a commit without one can't have it. */
    result("PAGE_FLIP_EVENT, nothing (0 objects)", commit(fd, DRM_MODE_PAGE_FLIP_EVENT, 0, NULL, NULL, NULL, NULL, 0));
    /* IN_FENCE_FD: -1 or a sync file, once per plane. */
    uint32_t p_fence = prop(fd, g_plane, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", NULL);
    uint32_t fence_props[2] = {p_fence, p_fence}, two = 2;
    uint64_t no_fences[2] = {(uint64_t)-1, (uint64_t)-1}, v_minus2 = (uint64_t)-2, v_closed = 9999;
    uint64_t v_null = (uint64_t)open("/dev/null", O_RDONLY | O_CLOEXEC);
    result("IN_FENCE_FD -1 twice", commit(fd, t, 1, &g_plane, &two, fence_props, no_fences, 0));
    result("IN_FENCE_FD -2", commit(fd, t, 1, &g_plane, &one, &p_fence, &v_minus2, 0));
    result("IN_FENCE_FD not an open file", commit(fd, t, 1, &g_plane, &one, &p_fence, &v_closed, 0));
    result("IN_FENCE_FD a file that is no fence", commit(fd, t, 1, &g_plane, &one, &p_fence, &v_null, 0));
    close((int)v_null);
    /* Which error a bad pointer gives depends on what was read before it. */
    const void *bad = (const void *)8;
    result("unknown object, bad other pointers", commit(fd, t, 1, &unknown, bad, bad, bad, 0));
    result("bad values pointer", commit(fd, t, 1, &g_plane, &one, &p_fb, bad, 0));
    result("a property the object lacks, bad values pointer", commit(fd, t, 1, &g_crtc, &one, &p_fb, bad, 0));
    uint32_t two_objs[2] = {g_plane, unknown}, two_counts[2] = {1, 1}, two_props[2] = {p_fb, p_fb};
    result("an unknown object after a bad values pointer", commit(fd, t, 2, two_objs, two_counts, two_props, bad, 0));
    result("LIST_LESSEES pad set", ioctl(fd, DRM_IOCTL_MODE_LIST_LESSEES, &(struct drm_mode_list_lessees){.pad = 1}));

    printf("== framebuffer lifetime, CRTC states\n");
    uint32_t p_conn_crtc = prop(fd, g_conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
    static const char *const plane_names[] = {"CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
                                              "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H"};
    uint64_t plane_values[] = {g_crtc, 0, 0, (uint64_t)g_mode.hdisplay << 16, (uint64_t)g_mode.vdisplay << 16,
                               0, 0, g_mode.hdisplay, g_mode.vdisplay};
    uint32_t objs[3] = {g_conn, g_crtc, g_plane}, counts[3] = {1, 2, 10}, props[13];
    uint64_t values[13] = {g_crtc, mode_blob.blob_id, 1, fb1};
    props[0] = p_conn_crtc, props[1] = p_mode, props[2] = p_active, props[3] = p_fb;
    for (int i = 0; i < 9; i++) {
        props[4 + i] = prop(fd, g_plane, DRM_MODE_OBJECT_PLANE, plane_names[i], NULL);
        values[4 + i] = plane_values[i];
    }
    result("modeset showing fb1", commit(fd, DRM_MODE_ATOMIC_ALLOW_MODESET, 3, objs, counts, props, values, 0));
    /* Long lists (fb1 is shown: setting it again changes nothing). */
    static uint32_t many_props[600];
    static uint64_t many_values[600];
    for (int i = 0; i < 600; i++) {
        many_props[i] = p_fb;
        many_values[i] = fb1;
    }
    uint32_t n_many = 600, n_some = 100;
    result("the same property 600 times", commit(fd, t, 1, &g_plane, &n_many, many_props, many_values, 0));
    result("the same property 100 times", commit(fd, t, 1, &g_plane, &n_some, many_props, many_values, 0));
    many_props[99] = p_active;
    result("100 properties, the last one the object lacks",
           commit(fd, t, 1, &g_plane, &n_some, many_props, many_values, 0));
    printf("== cursor plane\n");
    cursor_checks(fd);
    result("CLOSEFB fb1 (on screen)", ioctl(fd, DRM_IOCTL_MODE_CLOSEFB, &(struct drm_mode_closefb){.fb_id = fb1}));
    result("commit fb1 again after CLOSEFB", commit(fd, 0, 1, &g_plane, &one, &p_fb, &v_fb, 0));
    result("RMFB fb1 after CLOSEFB", ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb1));
    result("ACTIVE 0, mode kept", commit(fd, DRM_MODE_ATOMIC_ALLOW_MODESET, 1, &g_crtc, &one, &p_active, &v0, 0));
    struct drm_mode_crtc crtc = {.crtc_id = g_crtc};
    result("GETCRTC", ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc));
    printf("%-60s %u\n", "  mode_valid with the CRTC inactive", crtc.mode_valid);
    struct drm_mode_get_connector c = {.connector_id = g_conn};
    ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
    printf("%-60s %s\n", "  connector encoder_id (bound)", c.encoder_id ? "set" : "0");
    uint32_t off_objs[2] = {g_conn, g_crtc}, off_counts[2] = {1, 2}, off_props[3] = {p_conn_crtc, p_active, p_mode};
    uint64_t off_values[3] = {0, 0, 0};
    result("unbind the connector, CRTC off",
           commit(fd, DRM_MODE_ATOMIC_ALLOW_MODESET, 2, off_objs, off_counts, off_props, off_values, 0));
    c = (struct drm_mode_get_connector){.connector_id = g_conn};
    ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
    printf("%-60s %s\n", "  connector encoder_id (unbound)", c.encoder_id ? "set" : "0");
    uint64_t fb_id = 9, crtc_id = 9;
    prop(fd, g_plane, DRM_MODE_OBJECT_PLANE, "FB_ID", &fb_id);
    prop(fd, g_plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &crtc_id);
    printf("%-60s %s, %s\n", "  plane FB_ID / CRTC_ID after the CRTC went off", fb_id ? "set" : "0",
           crtc_id ? "set" : "0");
    result("ACTIVE 1 without a mode",
           commit(fd, DRM_MODE_ATOMIC_ALLOW_MODESET | t, 1, &g_crtc, &one, &p_active, &v1, 0));
    result("plane framebuffer on the CRTC without a mode",
           commit(fd, DRM_MODE_ATOMIC_ALLOW_MODESET | t, 1, &g_plane, &one, &p_fb, &v_fb, 0));
    struct drm_mode_get_encoder e = {.encoder_id = g_enc};
    ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e);
    printf("%-60s %s\n", "  encoder crtc_id (unbound)", e.crtc_id ? "set" : "0");
    legacy_checks(fd);
    dumb_checks(fd);
    return 0;
}
