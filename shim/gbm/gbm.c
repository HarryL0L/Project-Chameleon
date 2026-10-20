/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libgbm.so replacement: every gbm_bo is an AHardwareBuffer from
 * libchameleon.so, with the gralloc handle's dmabuf as its fd, and a
 * gbm_surface an Android AImageReader. Termux's Mesa libgbm has the same
 * soname and is shadowed via LD_LIBRARY_PATH.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* ---- gbm_surface: an EGL window on the GBM platform ----
 *
 * As with Mesa, the EGL driver renders a gbm_surface's buffers and
 * gbm_surface_lock_front_buffer() hands the one just swapped to the program
 * as a gbm_bo for KMS. Here the surface is an Android AImageReader: its
 * window, registered with libchameleon.so, is what Android's EGL renders
 * into (eglCreate*WindowSurface on a gbm_surface, egl/egl.c), and each
 * swapped image's AHardwareBuffer becomes a gbm_bo, the same one every time that buffer comes round again (programs
 * keep their framebuffer in its user data). */

typedef struct AImageReader AImageReader;
typedef struct AImage AImage;
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} ahb_desc;

#define AHB_FORMAT_R8G8B8A8 1
#define AHB_FORMAT_R8G8B8X8 2
#define AHB_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)
#define SURFACE_IMAGES 4 /* Mesa's gbm surfaces have four color buffers too */

static struct {
    void (*acquire)(AHardwareBuffer *);
    void (*release)(AHardwareBuffer *);
    void (*describe)(const AHardwareBuffer *, ahb_desc *);
    const void *(*get_native_handle)(const AHardwareBuffer *);
    int (*reader_new)(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **);
    int (*reader_get_window)(AImageReader *, void **);
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
    A(nw, acquire, "AHardwareBuffer_acquire");
    A(nw, release, "AHardwareBuffer_release");
    A(nw, describe, "AHardwareBuffer_describe");
    A(nw, get_native_handle, "AHardwareBuffer_getNativeHandle");
    A(media, reader_new, "AImageReader_newWithUsage");
    A(media, reader_get_window, "AImageReader_getWindow");
    A(media, reader_acquire_next, "AImageReader_acquireNextImageAsync");
    A(media, reader_delete, "AImageReader_delete");
    A(media, image_get_ahb, "AImage_getHardwareBuffer");
    A(media, image_delete, "AImage_delete");
#undef A
    loaded = android.acquire && android.release && android.describe && android.get_native_handle &&
             android.reader_new && android.reader_get_window && android.reader_acquire_next &&
             android.reader_delete && android.image_get_ahb && android.image_delete;
    if (!loaded)
        cham_log("gbm: AImageReader unavailable, no gbm surfaces");
    return loaded;
}

struct gbm_surface {
    struct gbm_device *device;
    uint32_t width, height, format;
    AImageReader *reader;
    void *window;
    struct gbm_bo *bos[SURFACE_IMAGES]; /* one per buffer the reader has shown us */
    AImage *images[SURFACE_IMAGES];     /* the image behind bos[i] while locked */
};

struct gbm_surface *gbm_surface_create(struct gbm_device *gbm, uint32_t width, uint32_t height, uint32_t format,
                                       uint32_t flags)
{
    (void)flags;
    /* AImageReader takes RGBA/RGBX, which are ABGR8888/XBGR8888 in DRM terms. */
    int32_t reader_format = format == DRM_FORMAT_ABGR8888   ? AHB_FORMAT_R8G8B8A8
                            : format == DRM_FORMAT_XBGR8888 ? AHB_FORMAT_R8G8B8X8
                                                            : 0;
    if (!reader_format || !width || !height || !load_android()) {
        errno = EINVAL;
        return NULL;
    }
    struct gbm_surface *s = calloc(1, sizeof *s);
    s->device = gbm;
    s->width = width;
    s->height = height;
    s->format = format;
    if (android.reader_new((int32_t)width, (int32_t)height, reader_format, AHB_USAGE_GPU_SAMPLED_IMAGE,
                           SURFACE_IMAGES, &s->reader) != 0 ||
        android.reader_get_window(s->reader, &s->window) != 0 || !s->window) {
        cham_log("gbm: AImageReader %ux%u failed", width, height);
        if (s->reader)
            android.reader_delete(s->reader);
        free(s);
        errno = ENOMEM;
        return NULL;
    }
    cham_gbm_window_set(s, s->window, format);
    return s;
}

/* Gralloc chooses the layout: only "implicit" or linear can be honoured. */
struct gbm_surface *gbm_surface_create_with_modifiers2(struct gbm_device *gbm, uint32_t width, uint32_t height,
                                                       uint32_t format, const uint64_t *modifiers,
                                                       const unsigned int count, uint32_t flags)
{
    int usable = count == 0;
    for (unsigned int i = 0; i < count; i++)
        usable |= modifiers[i] == DRM_FORMAT_MOD_INVALID || modifiers[i] == DRM_FORMAT_MOD_LINEAR;
    if (!usable) {
        errno = ENOSYS;
        return NULL;
    }
    return gbm_surface_create(gbm, width, height, format, flags);
}

struct gbm_surface *gbm_surface_create_with_modifiers(struct gbm_device *gbm, uint32_t width, uint32_t height,
                                                      uint32_t format, const uint64_t *modifiers,
                                                      const unsigned int count)
{
    return gbm_surface_create_with_modifiers2(gbm, width, height, format, modifiers, count,
                                              GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
}

/* The bo for a buffer of the reader's, made the first time it comes round. */
static struct gbm_bo *surface_bo(struct gbm_surface *s, AHardwareBuffer *ahb, int *index)
{
    int free_index = -1;
    for (int i = 0; i < SURFACE_IMAGES; i++) {
        if (s->bos[i] && s->bos[i]->cb->ahb == ahb) {
            *index = i;
            return s->bos[i];
        }
        if (!s->bos[i] && free_index < 0)
            free_index = i;
    }
    if (free_index < 0)
        return NULL;
    ahb_desc desc;
    android.describe(ahb, &desc);
    android.acquire(ahb);
    struct cham_bo *cb = cham_bo_wrap(ahb, android.get_native_handle(ahb), desc.width, desc.height,
                                      desc.stride * 4, s->format, android.release);
    if (!cb) {
        android.release(ahb);
        return NULL;
    }
    struct gbm_bo *bo = calloc(1, sizeof *bo);
    bo->device = s->device;
    bo->cb = cb;
    bo->modifier = DRM_FORMAT_MOD_INVALID;
    s->bos[free_index] = bo;
    *index = free_index;
    return bo;
}

struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *s)
{
    AImage *image = NULL;
    int fence = -1;
    if (android.reader_acquire_next(s->reader, &image, &fence) != 0 || !image)
        return NULL; /* nothing swapped since the last lock */
    if (fence >= 0) { /* scanout has no fence of ours: wait for the GPU here */
        struct pollfd p = {.fd = fence, .events = POLLIN};
        while (poll(&p, 1, -1) < 0 && errno == EINTR)
            ;
        close(fence);
    }
    AHardwareBuffer *ahb = NULL;
    int i;
    struct gbm_bo *bo = android.image_get_ahb(image, &ahb) == 0 && ahb ? surface_bo(s, ahb, &i) : NULL;
    if (!bo) {
        cham_log("gbm: cannot use the surface's buffer");
        android.image_delete(image);
        return NULL;
    }
    s->images[i] = image;
    return bo;
}

void gbm_surface_release_buffer(struct gbm_surface *s, struct gbm_bo *bo)
{
    for (int i = 0; i < SURFACE_IMAGES; i++) {
        if (s->bos[i] == bo && s->images[i]) {
            android.image_delete(s->images[i]); /* back to the reader for EGL to render into */
            s->images[i] = NULL;
        }
    }
}

int gbm_surface_has_free_buffers(struct gbm_surface *s)
{
    int locked = 0;
    for (int i = 0; i < SURFACE_IMAGES; i++)
        locked += s->images[i] != NULL;
    return locked < SURFACE_IMAGES - 1; /* EGL needs one to render the next frame into */
}

void gbm_surface_destroy(struct gbm_surface *s)
{
    if (!s)
        return;
    cham_gbm_window_set(s, NULL, 0);
    for (int i = 0; i < SURFACE_IMAGES; i++) {
        if (s->images[i])
            android.image_delete(s->images[i]);
        if (s->bos[i])
            gbm_bo_destroy(s->bos[i]);
    }
    android.reader_delete(s->reader);
    free(s);
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
