/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The fences of the session's buffers: what the kernel keeps in a dmabuf's
 * reservation object (dma-buf.c, dma-resv.c).
 *
 * On Linux a GPU driver attaches a fence to a buffer for each job that
 * writes or reads it, and whoever gets the buffer's dmabuf can wait for
 * them: poll() on the dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, and
 * DMA_BUF_IOCTL_IMPORT_SYNC_FILE to add one. Android's drivers attach none,
 * and its older kernels lack the two calls, so they are kept here, per
 * buffer, and libchameleon answers the three calls from them
 * (interpose.c).
 *
 * Who draws into a buffer and hands it on gives the fence of that work
 * (DMABUF_WRITE: the session's app windows, a program handing a compositor
 * dmabufs of its own). A driver would also make the next job that reads the
 * buffer wait; nothing does that here, so the answer tells whether the
 * buffer's reader has ever asked for its fences. If not, the writer waits
 * for its work itself before it hands the buffer on, as it always did.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#ifdef CHAM_HOST_TEST
#include <sys/eventfd.h>
#endif

#include "cham_shim.h"
#include "internal.h"

/* A fence that has signalled, for a buffer with nothing pending
 * (dma_fence_get_stub()): the first one seen to signal is kept. */
static int g_stub = -1;

static int signalled(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    return poll(&p, 1, 0) > 0;
}

/* Done with `fd` (ours). */
static void retire_locked(int fd)
{
    if (fd < 0)
        return;
    if (g_stub < 0 && signalled(fd))
        g_stub = fd;
    else
        close(fd);
}

/* A new fd of a signalled fence, or -1 when none has been seen yet. */
static int stub_locked(void)
{
#ifdef CHAM_HOST_TEST
    if (g_stub < 0)
        g_stub = eventfd(1, EFD_CLOEXEC); /* the host tests' kind of fence: readable = signalled */
#endif
    return g_stub >= 0 ? fcntl(g_stub, F_DUPFD_CLOEXEC, 0) : -1;
}

/* One fence for both (sync_file_merge), or -1. */
static int merge(int a, int b)
{
    struct sync_merge_data m = {.name = "chameleon", .fd2 = b};
    return ioctl(a, SYNC_IOC_MERGE, &m) == 0 ? m.fence : -1;
}

/* What is still pending in a slot, or -1. */
static int pending_locked(int *slot)
{
    if (*slot >= 0 && signalled(*slot)) {
        retire_locked(*slot);
        *slot = -1;
    }
    return *slot;
}

/* Adds `fence` (taken over) to what the slot waits for. */
static void add_locked(int *slot, int fence)
{
    if (pending_locked(slot) >= 0) {
        int both = merge(*slot, fence);
        if (both >= 0) {
            close(*slot);
            close(fence);
            *slot = both;
            return;
        }
        close(*slot); /* can't be merged (not sync files): the newer one stands */
    }
    *slot = fence;
}

int64_t dmabuf_sync(dev_t dev, ino_t ino, int op, unsigned arg, int fence, int *out)
{
    *out = -1;
    struct cham_bo *bo = ahb_bo_for_dmabuf(dev, ino);
    if (!bo) {
        if (fence >= 0)
            close(fence);
        return -ENOENT;
    }
    int64_t ret = 0;
    pthread_mutex_lock(&g_lock);
    int w = pending_locked(&bo->fence[0]), r = pending_locked(&bo->fence[1]);
    switch (op) {
    case DMABUF_POLL: {
        /* dma_buf_poll: reading waits for the writers, writing for everyone. */
        unsigned ev = arg & (POLLIN | POLLOUT);
        ret = (w < 0 ? ev & POLLIN : 0) | (w < 0 && r < 0 ? ev & POLLOUT : 0);
        if ((unsigned)ret != ev)
            *out = fcntl(w >= 0 ? w : r, F_DUPFD_CLOEXEC, 0);
        bo->watched = 1;
        break;
    }
    case DMABUF_EXPORT: {
        /* dma_buf_export_sync_file: the fences a reader (DMA_BUF_SYNC_READ)
         * or a writer must wait for, as one. */
        if (!(arg & DMA_BUF_SYNC_RW) || (arg & ~(unsigned)DMA_BUF_SYNC_RW)) {
            ret = -EINVAL;
            break;
        }
        if (!(arg & DMA_BUF_SYNC_WRITE))
            r = -1;
        if (w >= 0 && r >= 0 && (*out = merge(w, r)) >= 0)
            ;
        else if (w >= 0 || r >= 0)
            *out = fcntl(w >= 0 ? w : r, F_DUPFD_CLOEXEC, 0);
        else
            *out = stub_locked();
        if (*out < 0)
            ret = -ENOTTY; /* nothing to give: as a kernel without the call */
        bo->watched = 1;
        break;
    }
    case DMABUF_IMPORT:
        /* dma_buf_import_sync_file */
        if (!(arg & DMA_BUF_SYNC_RW) || (arg & ~(unsigned)DMA_BUF_SYNC_RW) || fence < 0 || !is_sync_file(fence)) {
            ret = -EINVAL;
            break;
        }
        add_locked(&bo->fence[arg & DMA_BUF_SYNC_WRITE ? 0 : 1], fence);
        fence = -1;
        break;
    case DMABUF_WRITE:
        if (fence >= 0)
            add_locked(&bo->fence[0], fence);
        fence = -1;
        ret = bo->watched;
        break;
    default:
        ret = -EINVAL;
    }
    pthread_mutex_unlock(&g_lock);
    if (fence >= 0)
        close(fence);
    cham_bo_unref(bo);
    return ret;
}
