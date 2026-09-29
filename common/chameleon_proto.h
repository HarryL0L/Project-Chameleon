/*
 * Wire protocol between a Termux-side producer (demo, later the KWin shim)
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
    CHAM_HELLO = 1,         /* a = CHAM_PROTO_VERSION */
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
};

struct cham_msg {
    uint32_t type;
    uint32_t id;
    uint64_t a;
    uint64_t b;
};

#define CHAM_CONFIG_WIDTH(m) ((uint32_t)((m)->a & 0xffffffffu))
#define CHAM_CONFIG_HEIGHT(m) ((uint32_t)((m)->a >> 32))

/* Filesystem socket the presenter app listens on; override with
 * $CHAMELEON_SOCKET on the producer side. */
#define CHAM_SOCKET_PATH "/data/data/com.termux/files/usr/tmp/chameleon-0"

#endif
