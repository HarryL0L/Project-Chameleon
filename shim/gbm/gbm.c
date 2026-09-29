/*
 * libgbm.so replacement: every gbm_bo is an AHardwareBuffer from
 * libchameleon.so, with the gralloc handle's dmabuf as its fd. Termux's Mesa
 * libgbm has the same soname and is shadowed via LD_LIBRARY_PATH.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <drm_fourcc.h>
#include <gbm.h>

#include "../core/cham_shim.h"

#pragma GCC visibility push(default)

struct gbm_device {
    int fd;
};

struct gbm_bo {
    struct gbm_device *device;
    struct cham_bo *cb;
    uint64_t modifier;
    void *user_data;
    void (*destroy_user_data)(struct gbm_bo *, void *);
};

struct gbm_device *gbm_create_device(int fd)
{
    struct gbm_device *dev = calloc(1, sizeof *dev);
    if (dev)
        dev->fd = fd;
    return dev;
}

void gbm_device_destroy(struct gbm_device *gbm)
{
    free(gbm);
}

int gbm_device_get_fd(struct gbm_device *gbm)
{
    return gbm->fd;
}

const char *gbm_device_get_backend_name(struct gbm_device *gbm)
{
    (void)gbm;
    return "chameleon";
}

int gbm_device_is_format_supported(struct gbm_device *gbm, uint32_t format, uint32_t flags)
{
    (void)gbm;
    (void)flags;
    return cham_format_supported(format);
}

int gbm_device_get_format_modifier_plane_count(struct gbm_device *gbm, uint32_t format, uint64_t modifier)
{
    (void)gbm;
    if (!cham_format_supported(format))
        return -1;
    return modifier == DRM_FORMAT_MOD_INVALID || modifier == DRM_FORMAT_MOD_LINEAR ? 1 : -1;
}

static struct gbm_bo *create(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format,
                             uint32_t flags, uint64_t modifier)
{
    if (modifier == DRM_FORMAT_MOD_LINEAR)
        flags |= GBM_BO_USE_LINEAR;
    struct cham_bo *cb = cham_bo_create(width, height, format, flags);
    if (!cb)
        return NULL;
    struct gbm_bo *bo = calloc(1, sizeof *bo);
    bo->device = gbm;
    bo->cb = cb;
    bo->modifier = modifier;
    return bo;
}

struct gbm_bo *gbm_bo_create(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format,
                             uint32_t flags)
{
    return create(gbm, width, height, format, flags,
                  (flags & GBM_BO_USE_LINEAR) ? DRM_FORMAT_MOD_LINEAR : DRM_FORMAT_MOD_INVALID);
}

/* Gralloc chooses the layout, so only "implicit" (or linear, which we can
 * ask gralloc for via CPU usage) can be honoured. */
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *gbm, uint32_t width, uint32_t height,
                                             uint32_t format, const uint64_t *modifiers, const unsigned int count,
                                             uint32_t flags)
{
    int implicit = count == 0, linear = 0;
    for (unsigned int i = 0; i < count; i++) {
        implicit |= modifiers[i] == DRM_FORMAT_MOD_INVALID;
        linear |= modifiers[i] == DRM_FORMAT_MOD_LINEAR;
    }
    if (implicit)
        return create(gbm, width, height, format, flags, DRM_FORMAT_MOD_INVALID);
    if (linear)
        return create(gbm, width, height, format, flags, DRM_FORMAT_MOD_LINEAR);
    errno = ENOSYS;
    return NULL;
}

struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *gbm, uint32_t width, uint32_t height,
                                            uint32_t format, const uint64_t *modifiers, const unsigned int count)
{
    return gbm_bo_create_with_modifiers2(gbm, width, height, format, modifiers, count,
                                         GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
}

struct gbm_bo *gbm_bo_import(struct gbm_device *gbm, uint32_t type, void *buffer, uint32_t flags)
{
    (void)gbm;
    (void)type;
    (void)buffer;
    (void)flags;
    errno = ENOSYS; /* foreign dmabufs have no AHardwareBuffer to show */
    return NULL;
}

void gbm_bo_destroy(struct gbm_bo *bo)
{
    if (!bo)
        return;
    if (bo->destroy_user_data)
        bo->destroy_user_data(bo, bo->user_data);
    cham_bo_unref(bo->cb);
    free(bo);
}

void *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t flags,
                 uint32_t *stride, void **map_data)
{
    (void)width;
    (void)height;
    (void)flags;
    char *base = cham_bo_lock(bo->cb);
    if (!base) {
        errno = EINVAL;
        return NULL;
    }
    *stride = bo->cb->stride;
    *map_data = bo;
    return base + (size_t)y * bo->cb->stride + (size_t)x * 4;
}

void gbm_bo_unmap(struct gbm_bo *bo, void *map_data)
{
    (void)map_data;
    cham_bo_unlock(bo->cb);
}

int gbm_bo_write(struct gbm_bo *bo, const void *buf, size_t count)
{
    void *dst = cham_bo_lock(bo->cb);
    if (!dst)
        return -1;
    memcpy(dst, buf, count);
    cham_bo_unlock(bo->cb);
    return 0;
}

uint32_t gbm_bo_get_width(struct gbm_bo *bo) { return bo->cb->width; }
uint32_t gbm_bo_get_height(struct gbm_bo *bo) { return bo->cb->height; }
uint32_t gbm_bo_get_stride(struct gbm_bo *bo) { return bo->cb->stride; }
uint32_t gbm_bo_get_format(struct gbm_bo *bo) { return bo->cb->format; }
uint32_t gbm_bo_get_bpp(struct gbm_bo *bo) { (void)bo; return 32; }
uint64_t gbm_bo_get_modifier(struct gbm_bo *bo) { return bo->modifier; }
int gbm_bo_get_plane_count(struct gbm_bo *bo) { (void)bo; return 1; }
struct gbm_device *gbm_bo_get_device(struct gbm_bo *bo) { return bo->device; }

uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane)
{
    return plane == 0 ? bo->cb->stride : 0;
}

uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane)
{
    (void)bo;
    (void)plane;
    return 0;
}

union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo)
{
    union gbm_bo_handle h;
    memset(&h, 0, sizeof h);
    h.u32 = bo->cb->handle;
    return h;
}

union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane)
{
    union gbm_bo_handle h;
    memset(&h, 0, sizeof h);
    if (plane == 0)
        h.u32 = bo->cb->handle;
    else
        h.s32 = -1;
    return h;
}

int gbm_bo_get_fd(struct gbm_bo *bo)
{
    return cham_bo_export_fd(bo->cb);
}

int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane)
{
    if (plane != 0) {
        errno = EINVAL;
        return -1;
    }
    return cham_bo_export_fd(bo->cb);
}

void gbm_bo_set_user_data(struct gbm_bo *bo, void *data, void (*destroy_user_data)(struct gbm_bo *, void *))
{
    bo->user_data = data;
    bo->destroy_user_data = destroy_user_data;
}

void *gbm_bo_get_user_data(struct gbm_bo *bo)
{
    return bo->user_data;
}

/* gbm_surface is only for EGL's GBM window platform, which KWin doesn't use. */
struct gbm_surface *gbm_surface_create(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format,
                                       uint32_t flags)
{
    (void)gbm, (void)width, (void)height, (void)format, (void)flags;
    errno = ENOSYS;
    return NULL;
}

struct gbm_surface *gbm_surface_create_with_modifiers(struct gbm_device *gbm, uint32_t width, uint32_t height,
                                                      uint32_t format, const uint64_t *modifiers,
                                                      const unsigned int count)
{
    (void)modifiers, (void)count;
    return gbm_surface_create(gbm, width, height, format, 0);
}

struct gbm_surface *gbm_surface_create_with_modifiers2(struct gbm_device *gbm, uint32_t width, uint32_t height,
                                                       uint32_t format, const uint64_t *modifiers,
                                                       const unsigned int count, uint32_t flags)
{
    (void)modifiers, (void)count;
    return gbm_surface_create(gbm, width, height, format, flags);
}

struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *surface)
{
    (void)surface;
    return NULL;
}

void gbm_surface_release_buffer(struct gbm_surface *surface, struct gbm_bo *bo)
{
    (void)surface, (void)bo;
}

int gbm_surface_has_free_buffers(struct gbm_surface *surface)
{
    (void)surface;
    return 0;
}

void gbm_surface_destroy(struct gbm_surface *surface)
{
    (void)surface;
}

char *gbm_format_get_name(uint32_t gbm_format, struct gbm_format_name_desc *desc)
{
    if (gbm_format == 0) {
        strcpy(desc->name, "NONE");
        return desc->name;
    }
    desc->name[0] = (char)(gbm_format & 0xff);
    desc->name[1] = (char)((gbm_format >> 8) & 0xff);
    desc->name[2] = (char)((gbm_format >> 16) & 0xff);
    desc->name[3] = (char)((gbm_format >> 24) & 0xff);
    desc->name[4] = '\0';
    return desc->name;
}

#pragma GCC visibility pop
