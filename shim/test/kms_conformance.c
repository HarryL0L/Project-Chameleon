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
static uint32_t buffer(int fd, uint32_t *pitch)
{
    struct drm_mode_create_dumb d = {.width = g_mode.hdisplay, .height = g_mode.vdisplay, .bpp = 32};
    ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d);
    *pitch = d.pitch;
    return d.handle;
}
#else
#include <gbm.h>
static uint32_t buffer(int fd, uint32_t *pitch)
{
    static struct gbm_device *gbm;
    if (!gbm)
        gbm = gbm_create_device(fd);
    struct gbm_bo *bo = gbm_bo_create(gbm, g_mode.hdisplay, g_mode.vdisplay, DRM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT);
    *pitch = gbm_bo_get_stride(bo);
    return gbm_bo_get_handle(bo).u32;
}
#endif

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
    return 0;
}
