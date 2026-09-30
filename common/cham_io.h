/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Send/receive one cham_msg packet with an optional fd (SCM_RIGHTS). */
#ifndef CHAMELEON_IO_H
#define CHAMELEON_IO_H

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "chameleon_proto.h"

/* Returns 0 on success, -errno on failure. Does not take ownership of fd. */
static inline int cham_send(int sock, const struct cham_msg *msg, int fd)
{
    struct iovec iov = {.iov_base = (void *)msg, .iov_len = sizeof *msg};
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } control;
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
    if (fd >= 0) {
        memset(&control, 0, sizeof control);
        mh.msg_control = control.buf;
        mh.msg_controllen = sizeof control.buf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof fd);
    }
    ssize_t n;
    do {
        n = sendmsg(sock, &mh, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return -errno;
    return n == (ssize_t)sizeof *msg ? 0 : -EPROTO;
}

/* Returns 1 on a message, 0 on EOF, -errno on failure. *fd_out is -1 unless
 * the packet carried a descriptor, which the caller then owns. */
static inline int cham_recv(int sock, struct cham_msg *msg, int *fd_out)
{
    struct iovec iov = {.iov_base = msg, .iov_len = sizeof *msg};
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int) * 4)];
    } control;
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = control.buf, .msg_controllen = sizeof control.buf};
    *fd_out = -1;
    ssize_t n;
    do {
        n = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return -errno;
    if (n == 0)
        return 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
        int count = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        int fds[4];
        memcpy(fds, CMSG_DATA(c), sizeof(int) * (count > 4 ? 4 : count));
        for (int i = 0; i < count && i < 4; i++) {
            if (*fd_out < 0)
                *fd_out = fds[i];
            else
                close(fds[i]);
        }
    }
    if (n != (ssize_t)sizeof *msg) {
        if (*fd_out >= 0)
            close(*fd_out);
        *fd_out = -1;
        return -EPROTO;
    }
    return 1;
}

#endif
