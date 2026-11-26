/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Internal interface of libchameleon.so, shared by the fake libgbm.so and
 * EGL vendor (which finds it at run time). Not a public ABI.
 */
#ifndef CHAM_SHIM_H
#define CHAM_SHIM_H

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#define CHAM_EXPORT __attribute__((visibility("default")))

/* dlopen() for one of Android's own libraries. Loaded the plain way, such a
 * library and all it needs sit among the program's own: there they answer to
 * their names, so that a program asking for "libvulkan.so" gets Android's
 * where it means Termux's Vulkan loader, and they take Termux's libraries
 * for theirs of the same name (libz.so). Termux has a helper against that,
 * libtermux-platform-ns.so of its libandroid-stub package: it loads Android's
 * libraries into one linker namespace of their own per process, the one
 * Termux's own stand-ins for Android's libraries use too. Without the helper,
 * or with CHAMELEON_ANDROID_NAMESPACE=0, the plain way. */
static inline void *cham_android_dlopen(const char *path)
{
    static uintptr_t helper; /* 0: not looked for yet, 1: none, else its platform_dlopen() */
    uintptr_t h = __atomic_load_n(&helper, __ATOMIC_ACQUIRE);
    if (!h) {
        const char *use = getenv("CHAMELEON_ANDROID_NAMESPACE");
        void *fn = NULL;
        if (!use || use[0] != '0' || use[1]) {
            const char *prefix = getenv("PREFIX");
            char lib[4096];
            snprintf(lib, sizeof lib, "%s/lib/libtermux-platform-ns.so",
                     prefix && *prefix ? prefix : "/data/data/com.termux/files/usr");
            void *ns = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
            fn = ns ? dlsym(ns, "platform_dlopen") : NULL;
        }
        h = fn ? (uintptr_t)fn : 1;
        __atomic_store_n(&helper, h, __ATOMIC_RELEASE);
    }
    if (h != 1)
        return ((void *(*)(const char *, int))h)(path, RTLD_NOW | RTLD_LOCAL);
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AHardwareBuffer AHardwareBuffer;
struct cham_dumb;

/* An AHardwareBuffer standing in for a GEM buffer object. */
struct cham_bo {
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
    void (*release)(AHardwareBuffer *); /* for cham_bo_wrap()'s buffers */
    int base_refs;     /* cham_bo_create(): references to the dmabuf the buffer holds itself */
    int exported;      /* its dmabuf was handed out (gbm_bo_get_fd, PRIME) */
    int in_service;    /* chameleon-kernel has it (a GEM handle): its dmabuf keeps it alive there */
    struct cham_dumb *dumb; /* chameleon-kernel: a dumb buffer's memory; such a bo has no AHardwareBuffer */
    int fence[2];      /* chameleon-kernel (dmabuf.c): GPU work pending on it, [0] writing, [1] reading; -1 none */
    int watched;       /* ...and whether who reads it has asked for them (poll, EXPORT_SYNC_FILE) */
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
/* A buffer someone else allocated (an AImageReader's, for gbm_surface), with
 * its native handle and size; the bo takes over one reference, which it
 * drops with `release`. */
CHAM_EXPORT struct cham_bo *cham_bo_wrap(AHardwareBuffer *ahb, const void *native_handle, uint32_t width,
                                         uint32_t height, uint32_t stride, uint32_t format,
                                         void (*release)(AHardwareBuffer *));
/* A GEM handle for the buffer in the card file `fd` (a gbm device's), made
 * if the file has none; 0 if fd isn't the card. Close it with
 * cham_bo_handle_close, as Mesa's gbm closes its handles. */
CHAM_EXPORT uint32_t cham_bo_handle_open(struct cham_bo *bo, int fd);
CHAM_EXPORT void cham_bo_handle_close(int fd, uint32_t handle);
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
 * handed to libchameleon's crash reporter with cham_crash_attach(), which
 * installs it: only programs rendering with Android's driver get one. */
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

/* gbm surfaces (libgbm.so) and the Android windows behind them, for the
 * EGL vendor: libgbm may be loaded privately (a compositor's backend
 * module), so they are kept here. window NULL forgets the surface. */
CHAM_EXPORT void cham_gbm_window_set(void *surface, void *window, uint32_t format);
/* The window behind `surface` and its DRM format; NULL if not a gbm surface. */
CHAM_EXPORT void *cham_gbm_window_get(void *surface, uint32_t *format);

/* Formats gbm allocates and EGL imports (DRM fourcc). */
CHAM_EXPORT const uint32_t *cham_formats(int *count);

/* AHB -> EGLClientBuffer is done by the EGL shim with the real driver. */
CHAM_EXPORT AHardwareBuffer *cham_bo_ahb(struct cham_bo *bo);
/* The session's buffer whose dmabuf `fd` refers to (any dup of it): another
 * process's gbm buffer or a Wayland app's registered one, as chameleon-kernel
 * knows them; with a reference for the caller to give back with
 * cham_buffer_ahb_release; NULL if none. */
CHAM_EXPORT AHardwareBuffer *cham_buffer_ahb_acquire(int fd);
CHAM_EXPORT void cham_buffer_ahb_release(AHardwareBuffer *b);
/* The session's buffer whose dmabuf `fd` refers to, as a bo of this process
 * (with a reference for the caller, cham_bo_unref): gbm_bo_import's.
 * NULL with errno ENODEV for another dmabuf, EINVAL for anything else. */
CHAM_EXPORT struct cham_bo *cham_bo_import(int fd);
/* Tells the session that what was just drawn into the buffer `dmabuf` is
 * done when `fence` (a sync file, the caller's still) signals: what a GPU
 * driver does on Linux. 1 if whoever reads the buffer asks for its fences
 * (poll(), DMA_BUF_IOCTL_EXPORT_SYNC_FILE); if 0 the caller waits for the
 * fence itself before it hands the buffer on. */
CHAM_EXPORT int cham_dmabuf_write_fence(int dmabuf, int fence);

CHAM_EXPORT void cham_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
