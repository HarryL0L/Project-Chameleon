/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private to libchameleon.so and chameleon-kernel. */
#ifndef CHAM_INTERNAL_H
#define CHAM_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cham_shim.h"

/* One lock for all shim state: buffers, fake KMS objects, presenter link. */
extern pthread_mutex_t g_lock;

uint64_t now_ns(void);

/* ---- ahb.c ---- */
void bo_ref_locked(struct cham_bo *bo);
void bo_unref_locked(struct cham_bo *bo);
/* Waits (briefly) for every pending presenter release fence. */
void bo_wait_release_fences_locked(void);
/* An AHardwareBuffer sent by another process (clients.c, card.c). */
AHardwareBuffer *ahb_recv(int sock);
/* The bo for a buffer another process sent (card.c), one per buffer however
 * often it arrives (matched by dmabuf). Takes over `b`'s reference; returns
 * the bo with a reference for the caller, or NULL. */
struct cham_bo *ahb_import(AHardwareBuffer *b, uint32_t format);
void ahb_release(AHardwareBuffer *b);
int ahb_dmabuf_identity(const AHardwareBuffer *b, dev_t *dev, ino_t *ino);
/* AHardwareBuffer_sendHandleToUnixSocket (the presenter link). */
int ahb_send(const AHardwareBuffer *b, int sock);
/* The bo's AHardwareBuffer, as ahb_send (to chameleon-kernel). */
int bo_send(const struct cham_bo *bo, int sock);

/* ---- link.c: connection to the presenter app ---- */
void link_start(void);
/* Blocks until the presenter reported a surface size, or the timeout. */
int link_wait_config(uint32_t *width, uint32_t *height, uint32_t *refresh_mhz, int timeout_ms);
/* Shows `bo` (acquire fence borrowed, may be -1). Returns 1 if the presenter
 * will answer with FRAME_DONE for `frame`, 0 if nothing is on screen. */
int link_present_locked(struct cham_bo *bo, int in_fence, uint64_t frame);
void link_forget_bo_locked(struct cham_bo *bo);
/* Tells the presenter the compositor turned its screen off (1) or on (0). */
void link_screen_off_locked(int off);
/* Page-flip event bookkeeping (one pending flip, like KMS). */
int link_flip_pending_locked(void);
/* The pending flip's event would go to a file being closed: drop it. */
void link_cancel_flip_locked(int event_fd);
void link_queue_flip_locked(uint64_t user_data, uint32_t crtc_id, int event_fd, uint64_t frame, int presented,
                            uint32_t refresh_mhz);
/* CHAM_CLIPBOARD_DATA/REQUEST with a duplicate of fd; dropped without a presenter. */
void link_clipboard(uint32_t type, uint64_t a, int fd);

/* ---- input.c: the app's input, window size and clipboard (one thread) ---- */
struct cham_msg;
/* CHAM_INPUT, CHAM_CONFIG (the connector's mode) and CHAM_CLIPBOARD_OFFER
 * for clipboard.c; handled on the input thread. */
void input_post(const struct cham_msg *m);

/* ---- wlpath.c: the compositor's Wayland socket ---- */
/* A process bound a Wayland socket (card.c). */
void input_note_socket(pid_t pid, const char *path);
/* The compositor's socket ($CHAMELEON_WAYLAND_SOCKET, or the one bound by
 * the DRM master's process or an ancestor); "" if none yet. */
void input_socket_path(char *out, size_t size);

/* ---- clients.c: Wayland apps' GPU buffers (in chameleon-kernel) ---- */
void clients_start(void);
/* Calls fn with the registered app buffer whose dmabuf `fd` is, while it
 * stays registered; returns fn's result, or -ENOENT. */
int clients_with_buffer(int fd, int (*fn)(const AHardwareBuffer *b, void *arg), void *arg);

/* ---- evdev.c: the app's input as evdev devices for libchameleon_udev.so ---- */
void evdev_start(void);
/* A uevent (struct cham_uevent's what) to every udev monitor. */
void evdev_uevent(uint32_t what);
/* Takes an input message for a device (input thread); 0 if none. */
int evdev_input(const struct cham_msg *m);
/* Whether the keyboard device has this Linux key code. */
int evdev_keyboard_has(unsigned code);

/* ---- clipboard.c: the desktop's clipboard <-> Android's (input thread) ---- */
struct wl_proxy;
void clipboard_global(struct wl_proxy *registry, uint32_t name, const char *iface, uint32_t version);
void clipboard_connected(void);
void clipboard_disconnected(void);
void clipboard_offer(int secret); /* Android's clipboard has new text */

/* ---- keymap.c: typed text as keys of the desktop's layout (input thread) ---- */
void keymap_global(struct wl_proxy *registry, uint32_t name, const char *iface);
void keymap_disconnected(void);
/* Presses or releases the keys typing `sym` on the evdev keyboard; 0 if the
 * layout has no key for it. */
int keymap_type(uint32_t sym, int down);

/* ---- text.c: the Android keyboard's text (input thread) ---- */
int text_input(const struct cham_msg *m); /* takes CHAM_INPUT_TEXT_* */
void text_ime_attach(int fd);             /* chameleon-ime connected (input.c's message) */
int text_ime_fd(void);                    /* -1 = none */
void text_ime_readable(void);
/* input.c: a keysym typed on the evdev keyboard (dropped if the layout lacks it). */
void input_keysym(uint32_t sym, int down);
/* evdev.c's server: chameleon-ime connected (any thread). */
void input_ime_connected(int fd);

/* ---- kms.c: the fake KMS device ---- */
struct gem_handle {
    uint32_t handle;
    struct cham_bo *bo; /* holds a ref */
};
/* One open file of the card (struct drm_file): dups, fork and fd passing
 * share it. */
struct fake_fd {
    uint32_t id;      /* card.c's name for it in requests */
    int refs;         /* card.c: the file list's, and each call in progress */
    dev_t dev;
    ino_t ino;        /* the read end the program holds */
    int event_wfd;    /* write end of the pipe the program reads DRM events from */
    pid_t pid;        /* the opener, for SET_MASTER's permission check */
    int master, was_master;
    uint32_t magic;   /* 0 until GET_MAGIC */
    int magic_used;   /* AUTH_MAGIC took it */
    int universal_planes, atomic, writeback;
    struct gem_handle *handles; /* under g_lock */
    int nhandles;
    struct fake_fd *next;     /* card.c's list */
    struct fake_fd *kms_next; /* kms.c's list, under g_lock */
};
void kms_init_once(void);
/* A file was opened (it may become master) / every copy of it is closed:
 * its master, handles, framebuffers, blobs and pending event go. */
void kms_file_open(struct fake_fd *f);
void kms_file_close(struct fake_fd *f);
/* The app's screen when the compositor started, in pixels and as the
 * connector reports its physical size. */
void kms_screen_size(uint32_t *width, uint32_t *height, uint32_t *mm_width, uint32_t *mm_height);
/* The process that opened the file that is DRM master, 0 if none. */
pid_t kms_master_pid(void);
/* Size of the mode the compositor has set, 0x0 before the first modeset. */
void kms_mode_size(uint32_t *width, uint32_t *height);
/* The app's surface as the connector's mode; 1 if that changed it. */
int kms_panel(uint32_t width, uint32_t height, uint32_t refresh_mhz);
/* The buffer on the compositor's screen, or NULL if the screen shows none. */
struct cham_bo *kms_screen_bo_locked(void);
/* The compositor turned its screen off (it wakes on input). */
int kms_screen_off_locked(void);
/* `arg` is an address in the calling process. 0 / >=0 or -errno. */
int kms_ioctl(struct fake_fd *f, unsigned int request, void *arg);
/* A GEM handle in `f` for a buffer its process sent (card.c); 0 if none. */
uint32_t kms_gem_import(struct fake_fd *f, struct cham_bo *bo);

/* ---- card.c: the card's files, served to every process ---- */
#define CHAM_CARD_SOCKET_NAME "chameleon-card" /* in $XDG_RUNTIME_DIR, SOCK_SEQPACKET */
/* Requests (struct cham_msg type), one at a time per connection, each
 * answered by CARD_DONE (a = the result, or -errno): */
enum {
    CARD_OPEN = 1, /* -> a = file id, and the read end of its event pipe */
    CARD_ATTACH,   /* + an fd: -> a = the id of the file it is, or -EBADF */
    CARD_IOCTL,    /* id = file, a = request (+ the number of an fd sent with it << 32,
                    * plus one), b = the argument's address */
    CARD_IMPORT,   /* id = file, a = DRM format, then the AHardwareBuffer: -> a = GEM handle */
    CARD_WAYLAND,  /* then a packet with a Wayland socket path a process bound; no answer */
    CARD_CLIENT_BUFFER, /* + a dmabuf fd: -> a = 1 and then an app's registered
                         * AHardwareBuffer behind it (clients.c), or -ENOENT */
    CARD_DONE = 100,
    /* While an ioctl runs the service may ask its caller, as the kernel's
     * fget and fd_install use the caller's fd table: */
    CARD_GET_FD,   /* a = the caller's fd number: CARD_FD with a dup of it (a = 0) or a = -EBADF */
    CARD_PUT_FD,   /* + an fd, a = 1 for close-on-exec: CARD_FD, a = its number in the caller */
    CARD_FD,
};
int card_start(void); /* 0 once it listens */
/* The process whose ioctl is running on this thread. */
pid_t card_caller_pid(void);
uid_t card_caller_uid(void);
/* copy_from_user / copy_to_user on its memory: 0 or -EFAULT. */
int card_copy_from(void *to, uint64_t from, size_t n);
int card_copy_to(uint64_t to, const void *from, size_t n);
/* fget: our own copy of its fd `n` (the caller closes it), or -EBADF. */
int card_fd_get(int n);
/* fd_install: `fd` (still ours) as a new fd of the caller; its number, or -errno. */
int card_fd_install(int fd, int cloexec);

/* ---- interpose.c (libchameleon.so), main.c (chameleon-kernel) ---- */
/* A card file's event pipe holds one page, as the kernel gives a DRM file
 * 4 KiB of event space; ordinary pipes hold more, so only a one-page pipe
 * can be a card file. */
#define CHAM_CARD_PIPE_SIZE ((int)sysconf(_SC_PAGESIZE))
int real_fstat(int fd, struct stat *st);

#endif
