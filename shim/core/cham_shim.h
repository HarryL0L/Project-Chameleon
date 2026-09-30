/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Internal interface of libchameleon.so, shared by the fake libgbm.so and
 * EGL vendor (which finds it at run time). Not a public ABI.
 */
#ifndef CHAM_SHIM_H
#define CHAM_SHIM_H

#include <stdint.h>
#include <sys/types.h>

#define CHAM_EXPORT __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AHardwareBuffer AHardwareBuffer;

/* An AHardwareBuffer standing in for a GEM buffer object. */
struct cham_bo {
    uint32_t handle;   /* GEM handle as seen through the fake DRM device */
    uint32_t width, height;
    uint32_t format;   /* DRM fourcc */
    uint32_t stride;   /* bytes */
    AHardwareBuffer *ahb;
    int dmabuf_fd;     /* our own dup of the gralloc handle's pixel dmabuf */
    dev_t dmabuf_dev;
    ino_t dmabuf_ino;

    /* private to the core */
    int refs;
    int slot;          /* presenter buffer id, -1 if not registered */
    uint64_t slot_gen; /* presenter connection the slot belongs to */
    int release_fence; /* last RELEASE fence from the presenter */
    struct cham_bo *next;
};

/* GBM usage bits the core understands (same values as GBM_BO_USE_*). */
#define CHAM_USE_SCANOUT (1u << 0)
#define CHAM_USE_CURSOR (1u << 1)
#define CHAM_USE_RENDERING (1u << 2)
#define CHAM_USE_WRITE (1u << 3)
#define CHAM_USE_LINEAR (1u << 4)

CHAM_EXPORT struct cham_bo *cham_bo_create(uint32_t width, uint32_t height, uint32_t format, uint32_t usage);
CHAM_EXPORT void cham_bo_unref(struct cham_bo *bo);
/* Finds the buffer whose dmabuf `fd` refers to (any dup of it). No new ref. */
CHAM_EXPORT struct cham_bo *cham_bo_from_fd(int fd);
/* New fd (dup) of the buffer's dmabuf, caller owns it. */
CHAM_EXPORT int cham_bo_export_fd(struct cham_bo *bo);
CHAM_EXPORT int cham_format_supported(uint32_t format);
/* CPU access for gbm_bo_map(); returns NULL if the allocation forbids it. */
CHAM_EXPORT void *cham_bo_lock(struct cham_bo *bo);
CHAM_EXPORT void cham_bo_unlock(struct cham_bo *bo);

/* Crash-report breadcrumbs: the last GL/EGL entry points called, recorded
 * by the glvnd vendor library (libEGL_chameleon.so, vendor/bridge.c) and
 * handed to libchameleon's crash reporter with cham_crash_attach(). */
#define CHAM_CALL_RING 64
struct cham_crash_breadcrumbs {
    const char *volatile *last_call;
    const char *volatile *ring; /* CHAM_CALL_RING entries */
    volatile unsigned *pos;
    const char *note;           /* last texture upload */
};
CHAM_EXPORT void cham_crash_attach(const struct cham_crash_breadcrumbs *crumbs);
CHAM_EXPORT extern const char *volatile cham_last_gl;
CHAM_EXPORT extern const char *volatile cham_call_ring[CHAM_CALL_RING];
CHAM_EXPORT extern volatile unsigned cham_call_pos;
#define CHAM_NOTE_CALL(name) \
    (cham_last_gl = (name), cham_call_ring[cham_call_pos++ % CHAM_CALL_RING] = (name))
CHAM_EXPORT void cham_crash_note(const char *fmt, ...);

/* Formats scanned out and imported by EGL (DRM fourcc). */
CHAM_EXPORT const uint32_t *cham_formats(int *count);

/* AHB -> EGLClientBuffer is done by the EGL shim with the real driver. */
CHAM_EXPORT AHardwareBuffer *cham_bo_ahb(struct cham_bo *bo);
/* An app's buffer registered through clients.c whose dmabuf `fd` refers to
 * (any dup of it); NULL if none. Borrowed reference. */
CHAM_EXPORT AHardwareBuffer *cham_client_ahb_from_fd(int fd);

CHAM_EXPORT void cham_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
