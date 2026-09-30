/* SPDX-License-Identifier: GPL-2.0-or-later */
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
/* An AHardwareBuffer sent by another process (clients.c). */
AHardwareBuffer *ahb_recv(int sock);
void ahb_release(AHardwareBuffer *b);
int ahb_dmabuf_identity(const AHardwareBuffer *b, dev_t *dev, ino_t *ino);
/* AHardwareBuffer_sendHandleToUnixSocket (the presenter link). */
int ahb_send(const AHardwareBuffer *b, int sock);

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
/* The app's surface size (last CONFIG), 0x0 while it has none. */
void link_app_size(uint32_t *width, uint32_t *height);

/* ---- input.c: app input -> KWin's fake input ---- */
struct cham_msg;
/* CHAM_INPUT, and CHAM_CONFIG for output.c; handled on the input thread. */
void input_post(const struct cham_msg *m);
/* KWin's Wayland socket, seen when it bind()s it. */
void input_note_socket(const char *path);
/* KWin's Wayland socket path, once known (env or bind()); "" if not yet. */
void input_socket_path(char *out, size_t size);

/* ---- output.c: KWin's screen follows the app's window (input thread) ---- */
struct wl_proxy;
struct wl_display;
void output_config(uint32_t width, uint32_t height, uint32_t refresh_mhz);
void output_global(struct wl_proxy *registry, uint32_t name, const char *iface, uint32_t version);
void output_connected(struct wl_display *display);
void output_disconnected(void);
int output_pending(void);    /* work that needs a connection to KWin */
int output_timeout_ms(void); /* until output_tick() has work, or -1 */
void output_tick(void);
/* Fraction of KWin's screen the app shows (it crops up to 7 pixels). */
void output_visible(double *fx, double *fy);

/* ---- kms.c: the fake KMS device ---- */
struct fake_fd {
    dev_t dev;
    ino_t ino;
    int event_wfd; /* write end of the pipe KWin reads DRM events from */
    struct fake_fd *next;
};
void kms_init_once(void);
/* Size of the mode KWin has set, 0x0 before the first modeset. */
void kms_mode_size(uint32_t *width, uint32_t *height);
int kms_ioctl(struct fake_fd *f, unsigned int request, void *arg); /* 0 / >=0 or -errno */

/* ---- interpose.c ---- */
int real_fstat(int fd, struct stat *st);

#endif
