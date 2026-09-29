/* Private to libchameleon.so. */
#ifndef CHAM_INTERNAL_H
#define CHAM_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>

#include "cham_shim.h"

/* One lock for all shim state: buffers, fake KMS objects, presenter link. */
extern pthread_mutex_t g_lock;

uint64_t now_ns(void);

/* ---- ahb.c ---- */
struct cham_bo *bo_by_handle_locked(uint32_t handle);
void bo_ref_locked(struct cham_bo *bo);
void bo_unref_locked(struct cham_bo *bo);
/* Waits (briefly) for every pending presenter release fence. */
void bo_wait_release_fences_locked(void);

/* ---- link.c: connection to the presenter app ---- */
void link_start(void);
/* Blocks until the presenter reported a surface size, or the timeout. */
int link_wait_config(uint32_t *width, uint32_t *height, uint32_t *refresh_mhz, int timeout_ms);
/* Shows `bo` (acquire fence borrowed, may be -1). Returns 1 if the presenter
 * will answer with FRAME_DONE for `frame`, 0 if nothing is on screen. */
int link_present_locked(struct cham_bo *bo, int in_fence, uint64_t frame);
void link_forget_bo_locked(struct cham_bo *bo);
/* Page-flip event bookkeeping (one pending flip, like KMS). */
int link_flip_pending_locked(void);
void link_queue_flip_locked(uint64_t user_data, uint32_t crtc_id, int event_fd, uint64_t frame, int presented,
                            uint32_t refresh_mhz);

/* ---- kms.c: the fake KMS device ---- */
struct fake_fd {
    dev_t dev;
    ino_t ino;
    int event_wfd; /* write end of the pipe KWin reads DRM events from */
    struct fake_fd *next;
};
void kms_init_once(void);
int kms_ioctl(struct fake_fd *f, unsigned int request, void *arg); /* 0 / >=0 or -errno */

/* ---- interpose.c ---- */
int real_fstat(int fd, struct stat *st);
const char *fake_path(void);
extern const dev_t fake_rdev;

#endif
