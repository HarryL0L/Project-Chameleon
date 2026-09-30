/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Wire protocol between a Termux-side producer (the KWin shim, or the demo)
 * and the presenter app.
 *
 * Transport: AF_UNIX SOCK_SEQPACKET. The presenter app is installed with
 * sharedUserId="com.termux" (signed with Termux's public GitHub test key), so
 * it runs as the Termux user and simply listens on CHAM_SOCKET_PATH.
 * Every message is one struct cham_msg packet, optionally carrying one fd via
 * SCM_RIGHTS. BUFFER_ADD is immediately followed by one packet written by
 * AHardwareBuffer_sendHandleToUnixSocket().
 *
 * Frame flow (mirrors a KMS page flip):
 *   producer: render into buffer N, export a native fence, PRESENT(N, fence)
 *   presenter: ASurfaceTransaction_setBuffer(N, fence) -> apply
 *   presenter: on transaction complete, RELEASE(previous buffer, release fence)
 *              then FRAME_DONE(frame)   <- the "page flip event"
 */
#ifndef CHAMELEON_PROTO_H
#define CHAMELEON_PROTO_H

#include <stdint.h>

#define CHAM_PROTO_VERSION 1

enum cham_msg_type {
    /* producer -> presenter */
    CHAM_HELLO = 1,         /* a = CHAM_PROTO_VERSION, b = CHAM_HELLO_* flags */
    CHAM_BUFFER_ADD = 2,    /* id; next packet is the AHardwareBuffer */
    CHAM_BUFFER_REMOVE = 3, /* id */
    CHAM_PRESENT = 4,       /* id, a = frame number, b = producer CLOCK_MONOTONIC ns;
                               fd = acquire fence (optional) */

    /* presenter -> producer */
    CHAM_CONFIG = 100,      /* a = width | height << 32 (0x0 = no surface, stop rendering),
                               b = refresh rate in mHz */
    CHAM_RELEASE = 101,     /* id; fd = release fence (optional); buffer may be reused
                               once the fence signals */
    CHAM_FRAME_DONE = 102,  /* a = frame number, b = SurfaceFlinger latch time (ns) */
    CHAM_BYE = 103,         /* a = CHAM_BYE_*; sent right before the presenter closes */
    CHAM_INPUT = 104,       /* id = CHAM_INPUT_*, a/b as described there */
};

/* Input events, presenter -> producer (the KWin shim feeds them to KWin's
 * org_kde_kwin_fake_input). Positions are fractions of the surface so they
 * map onto the producer's screen whatever its size or scale:
 *   pos   = CHAM_INPUT_POS(x, y), x/y in [0, 1] as 16.16 fixed point
 *   delta = CHAM_INPUT_DELTA(dx, dy), dx/dy as signed multiples of
 *           surface width/height in 12.20 fixed point */
enum cham_input_kind {
    CHAM_INPUT_TOUCH_DOWN = 1,     /* a = touch id, b = pos */
    CHAM_INPUT_TOUCH_MOTION = 2,   /* a = touch id, b = pos */
    CHAM_INPUT_TOUCH_UP = 3,       /* a = touch id */
    CHAM_INPUT_TOUCH_CANCEL = 4,
    CHAM_INPUT_TOUCH_FRAME = 5,    /* ends a group of touch events */
    CHAM_INPUT_POINTER_MOTION = 6, /* b = delta */
    CHAM_INPUT_POINTER_ABS = 7,    /* b = pos */
    CHAM_INPUT_BUTTON = 8,         /* a = evdev button (BTN_LEFT = 0x110), b = 1 pressed / 0 released */
    CHAM_INPUT_AXIS = 9,           /* a = 0 vertical / 1 horizontal, b = (int64) value * 256 */
    CHAM_INPUT_KEYSYM = 10,        /* a = X keysym (0x01000000 | code point for Unicode), b = 1 / 0 */
    CHAM_INPUT_KEY = 11,           /* a = evdev key code, b = 1 / 0 */
};

#define CHAM_INPUT_POS(x, y) ((uint64_t)(uint32_t)(x) | (uint64_t)(uint32_t)(y) << 32)
#define CHAM_INPUT_DELTA(dx, dy) ((uint64_t)(uint32_t)(int32_t)(dx) | (uint64_t)(uint32_t)(int32_t)(dy) << 32)
#define CHAM_INPUT_X(v) ((uint32_t)((v) & 0xffffffffu))
#define CHAM_INPUT_Y(v) ((uint32_t)((v) >> 32))
#define CHAM_INPUT_DX(v) ((int32_t)(uint32_t)((v) & 0xffffffffu))
#define CHAM_INPUT_DY(v) ((int32_t)(uint32_t)((v) >> 32))

/* BYE reasons. A replaced producer must not reconnect by itself, or two
 * producers would keep taking the screen from each other. */
#define CHAM_BYE_REPLACED 1u

/* HELLO flags.
 *
 * CHAM_HELLO_COPY: the producer follows KMS rules - it reuses a buffer as soon
 * as the *next* frame's FRAME_DONE arrives (like a page flip), not when
 * RELEASE arrives. SurfaceFlinger still scans that buffer out for one more
 * vsync, so the presenter blits each frame into its own buffer pool and
 * sends RELEASE (with the blit fence) right after the copy is queued. KWin
 * via the shim uses this; the demo stays zero-copy. */
#define CHAM_HELLO_COPY 1u

struct cham_msg {
    uint32_t type;
    uint32_t id;
    uint64_t a;
    uint64_t b;
};

#define CHAM_CONFIG_WIDTH(m) ((uint32_t)((m)->a & 0xffffffffu))
#define CHAM_CONFIG_HEIGHT(m) ((uint32_t)((m)->a >> 32))

/* ---- client buffers: Wayland apps -> KWin's shim ----
 *
 * An app rendering through the Chameleon EGL vendor hands KWin its buffers
 * as zwp_linux_dmabuf_v1 wl_buffers. A dmabuf fd alone can't be turned back
 * into an AHardwareBuffer on Android, so the app first registers each buffer
 * with libchameleon.so inside KWin over a second socket,
 * "<KWin's Wayland socket path>.chameleon" (SOCK_SEQPACKET), and KWin's EGL
 * import then finds the AHardwareBuffer by the dmabuf's inode. */
enum cham_client_msg_type {
    CHAM_CLIENT_BUFFER_ADD = 200,    /* id = app's buffer id; next packet is the AHardwareBuffer */
    CHAM_CLIENT_BUFFER_ACK = 201,    /* id, a = 1 registered / 0 failed */
    CHAM_CLIENT_BUFFER_REMOVE = 202, /* id */
};
#define CHAM_CLIENT_SOCKET_SUFFIX ".chameleon"

/* Filesystem socket the presenter app listens on; override with
 * $CHAMELEON_SOCKET on the producer side. */
#define CHAM_SOCKET_PATH "/data/data/com.termux/files/usr/tmp/chameleon-0"

#endif
