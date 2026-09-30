/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GBM buffer objects backed by AHardwareBuffers.
 *
 * The pixel memory is the dmabuf inside the gralloc native handle. Vendors
 * order the handle's fds differently (MediaTek puts a gralloc_extra metadata
 * fd first), so the dmabuf is found by what /proc says it is, not by index.
 * Any dup of that dmabuf identifies the buffer through (st_dev, st_ino).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include "internal.h"

typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} ahb_desc;

typedef struct {
    int version, numFds, numInts;
    int data[];
} native_handle;

#define AHB_FORMAT_R8G8B8A8 1
#define AHB_FORMAT_R8G8B8X8 2
#define HAL_FORMAT_BGRA_8888 5 /* not in the NDK enum, but gralloc takes it */
#define AHB_USAGE_CPU_READ_OFTEN 3ULL
#define AHB_USAGE_CPU_WRITE_OFTEN (3ULL << 4)
#define AHB_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)
#define AHB_USAGE_GPU_COLOR_OUTPUT (1ULL << 9)

static struct {
    int (*allocate)(const ahb_desc *, AHardwareBuffer **);
    void (*release)(AHardwareBuffer *);
    void (*describe)(const AHardwareBuffer *, ahb_desc *);
    int (*lock)(AHardwareBuffer *, uint64_t, int32_t, const void *, void **);
    int (*unlock)(AHardwareBuffer *, int32_t *);
    const native_handle *(*get_native_handle)(const AHardwareBuffer *);
    int (*recv_handle)(int, AHardwareBuffer **);
    int (*send_handle)(const AHardwareBuffer *, int);
} ahb;

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cham_bo *g_bos;
static uint32_t g_next_handle = 1;

void cham_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "chameleon: %s\n", buf);
}

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#ifdef CHAM_HOST_TEST
/* Desktop test build: memfd-backed stand-ins, so the fake KMS device can be
 * exercised with the real libdrm (shim/test/kms_test.c). */
#include <sys/mman.h>
struct AHardwareBuffer {
    ahb_desc desc;
    native_handle *nh;
    void *map;
};
static int host_allocate(const ahb_desc *d, AHardwareBuffer **out)
{
    AHardwareBuffer *b = calloc(1, sizeof *b);
    b->desc = *d;
    b->desc.stride = d->width;
    b->nh = calloc(1, sizeof(native_handle) + sizeof(int));
    b->nh->numFds = 1;
    b->nh->data[0] = memfd_create("fake-ahb", MFD_CLOEXEC);
    if (ftruncate(b->nh->data[0], (off_t)d->width * d->height * 4) != 0)
        return -1;
    *out = b;
    return 0;
}
static void host_release(AHardwareBuffer *b)
{
    close(b->nh->data[0]);
    free(b->nh);
    free(b);
}
static void host_describe(const AHardwareBuffer *b, ahb_desc *d) { *d = b->desc; }
static const native_handle *host_handle(const AHardwareBuffer *b) { return b->nh; }
/* Stand-in for AHardwareBuffer_recvHandleFromUnixSocket: one packet holding
 * the description and the buffer's (memfd) fd, as the tests' fake
 * AHardwareBuffer_sendHandleToUnixSocket writes it. */
#include <sys/socket.h>
static int host_recv(int sock, AHardwareBuffer **out)
{
    ahb_desc d;
    struct iovec iov = {.iov_base = &d, .iov_len = sizeof d};
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } control;
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.buf,
                        .msg_controllen = sizeof control.buf};
    if (recvmsg(sock, &mh, MSG_CMSG_CLOEXEC) != (ssize_t)sizeof d)
        return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (!c || c->cmsg_type != SCM_RIGHTS)
        return -1;
    AHardwareBuffer *b = calloc(1, sizeof *b);
    b->desc = d;
    b->nh = calloc(1, sizeof(native_handle) + sizeof(int));
    b->nh->numFds = 1;
    memcpy(&b->nh->data[0], CMSG_DATA(c), sizeof(int));
    *out = b;
    return 0;
}
/* Stands in for the AHardwareBuffer packet that follows BUFFER_ADD. */
static int host_send(const AHardwareBuffer *b, int sock)
{
    (void)b;
    char packet[16] = "fake-ahb";
    return send(sock, packet, sizeof packet, MSG_NOSIGNAL) == (ssize_t)sizeof packet ? 0 : -1;
}
static int ahb_load(void)
{
    ahb.send_handle = host_send;
    ahb.allocate = host_allocate;
    ahb.release = host_release;
    ahb.describe = host_describe;
    ahb.get_native_handle = host_handle;
    ahb.recv_handle = host_recv;
    return 1;
}
#else
static int ahb_load(void)
{
    static int loaded = -1;
    if (loaded >= 0)
        return loaded;
    void *lib = dlopen(sizeof(void *) == 8 ? "/system/lib64/libnativewindow.so" : "/system/lib/libnativewindow.so",
                       RTLD_NOW | RTLD_LOCAL);
    if (lib) {
        *(void **)&ahb.allocate = dlsym(lib, "AHardwareBuffer_allocate");
        *(void **)&ahb.release = dlsym(lib, "AHardwareBuffer_release");
        *(void **)&ahb.describe = dlsym(lib, "AHardwareBuffer_describe");
        *(void **)&ahb.lock = dlsym(lib, "AHardwareBuffer_lock");
        *(void **)&ahb.unlock = dlsym(lib, "AHardwareBuffer_unlock");
        *(void **)&ahb.get_native_handle = dlsym(lib, "AHardwareBuffer_getNativeHandle");
        *(void **)&ahb.recv_handle = dlsym(lib, "AHardwareBuffer_recvHandleFromUnixSocket");
        *(void **)&ahb.send_handle = dlsym(lib, "AHardwareBuffer_sendHandleToUnixSocket");
    }
    loaded = lib && ahb.allocate && ahb.release && ahb.describe && ahb.get_native_handle;
    if (!loaded)
        cham_log("cannot use AHardwareBuffer from libnativewindow: %s", lib ? "missing symbols" : dlerror());
    return loaded;
}
#endif

static const uint32_t k_formats[] = {DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888};

const uint32_t *cham_formats(int *count)
{
    *count = (int)(sizeof k_formats / sizeof k_formats[0]);
    return k_formats;
}

static uint32_t ahb_format(uint32_t drm_format)
{
    switch (drm_format) {
    case DRM_FORMAT_ABGR8888:
        return AHB_FORMAT_R8G8B8A8;
    case DRM_FORMAT_XBGR8888:
        return AHB_FORMAT_R8G8B8X8;
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
        return HAL_FORMAT_BGRA_8888;
    default:
        return 0;
    }
}

int cham_format_supported(uint32_t format)
{
    return ahb_format(format) != 0;
}

/* The handle fd that /proc calls a dmabuf, else the first one. */
static int find_dmabuf_fd(const native_handle *nh)
{
    for (int i = 0; i < nh->numFds; i++) {
        char path[64], link[128];
        snprintf(path, sizeof path, "/proc/self/fd/%d", nh->data[i]);
        ssize_t n = readlink(path, link, sizeof link - 1);
        if (n > 0) {
            link[n] = '\0';
            if (strstr(link, "dmabuf"))
                return nh->data[i];
        }
    }
    return nh->numFds > 0 ? nh->data[0] : -1;
}

/* ---- buffers from other processes (clients.c) ---- */

int ahb_send(const AHardwareBuffer *b, int sock)
{
    return ahb_load() && ahb.send_handle ? ahb.send_handle(b, sock) : -1;
}

AHardwareBuffer *ahb_recv(int sock)
{
    AHardwareBuffer *b = NULL;
    if (!ahb_load() || !ahb.recv_handle || ahb.recv_handle(sock, &b) != 0)
        return NULL;
    return b;
}

void ahb_release(AHardwareBuffer *b)
{
    if (b)
        ahb.release(b);
}

int ahb_dmabuf_identity(const AHardwareBuffer *b, dev_t *dev, ino_t *ino)
{
    const native_handle *nh = ahb.get_native_handle(b);
    int fd = nh ? find_dmabuf_fd(nh) : -1;
    struct stat st;
    if (fd < 0 || real_fstat(fd, &st) != 0)
        return -1;
    *dev = st.st_dev;
    *ino = st.st_ino;
    return 0;
}

struct cham_bo *cham_bo_create(uint32_t width, uint32_t height, uint32_t format, uint32_t usage)
{
    uint32_t afmt = ahb_format(format);
    if (!afmt || !width || !height || !ahb_load()) {
        errno = EINVAL;
        return NULL;
    }
    ahb_desc desc = {.width = width, .height = height, .layers = 1, .format = afmt,
                     .usage = AHB_USAGE_GPU_SAMPLED_IMAGE | AHB_USAGE_GPU_COLOR_OUTPUT};
    if (usage & (CHAM_USE_LINEAR | CHAM_USE_WRITE | CHAM_USE_CURSOR))
        desc.usage |= AHB_USAGE_CPU_READ_OFTEN | AHB_USAGE_CPU_WRITE_OFTEN;

    AHardwareBuffer *buffer = NULL;
    if (ahb.allocate(&desc, &buffer) != 0 || !buffer) {
        cham_log("AHardwareBuffer_allocate %ux%u format 0x%x failed", width, height, format);
        errno = ENOMEM;
        return NULL;
    }
    ahb.describe(buffer, &desc);

    const native_handle *nh = ahb.get_native_handle(buffer);
    int src = nh ? find_dmabuf_fd(nh) : -1;
    int fd = src >= 0 ? fcntl(src, F_DUPFD_CLOEXEC, 0) : -1;
    struct stat st;
    if (fd < 0 || real_fstat(fd, &st) != 0) {
        cham_log("gralloc handle has no usable dmabuf fd");
        if (fd >= 0)
            close(fd);
        ahb.release(buffer);
        errno = ENODEV;
        return NULL;
    }

    struct cham_bo *bo = calloc(1, sizeof *bo);
    bo->width = width;
    bo->height = height;
    bo->format = format;
    bo->stride = desc.stride * 4;
    bo->ahb = buffer;
    bo->dmabuf_fd = fd;
    bo->dmabuf_dev = st.st_dev;
    bo->dmabuf_ino = st.st_ino;
    bo->refs = 1;
    bo->slot = -1;
    bo->release_fence = -1;

    pthread_mutex_lock(&g_lock);
    bo->handle = g_next_handle++;
    bo->next = g_bos;
    g_bos = bo;
    pthread_mutex_unlock(&g_lock);
    return bo;
}

void bo_ref_locked(struct cham_bo *bo)
{
    bo->refs++;
}

void bo_unref_locked(struct cham_bo *bo)
{
    if (--bo->refs > 0)
        return;
    link_forget_bo_locked(bo);
    for (struct cham_bo **p = &g_bos; *p; p = &(*p)->next) {
        if (*p == bo) {
            *p = bo->next;
            break;
        }
    }
    if (bo->release_fence >= 0)
        close(bo->release_fence);
    close(bo->dmabuf_fd);
    ahb.release(bo->ahb);
    free(bo);
}

void cham_bo_unref(struct cham_bo *bo)
{
    if (!bo)
        return;
    pthread_mutex_lock(&g_lock);
    bo_unref_locked(bo);
    pthread_mutex_unlock(&g_lock);
}

struct cham_bo *bo_by_handle_locked(uint32_t handle)
{
    for (struct cham_bo *bo = g_bos; bo; bo = bo->next)
        if (bo->handle == handle)
            return bo;
    return NULL;
}

struct cham_bo *cham_bo_from_fd(int fd)
{
    struct stat st;
    if (fd < 0 || real_fstat(fd, &st) != 0)
        return NULL;
    struct cham_bo *found = NULL;
    pthread_mutex_lock(&g_lock);
    for (struct cham_bo *bo = g_bos; bo; bo = bo->next) {
        if (bo->dmabuf_dev == st.st_dev && bo->dmabuf_ino == st.st_ino) {
            found = bo;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

int cham_bo_export_fd(struct cham_bo *bo)
{
    return fcntl(bo->dmabuf_fd, F_DUPFD_CLOEXEC, 0);
}

AHardwareBuffer *cham_bo_ahb(struct cham_bo *bo)
{
    return bo->ahb;
}

void *cham_bo_lock(struct cham_bo *bo)
{
    void *ptr = NULL;
    if (!ahb.lock || ahb.lock(bo->ahb, AHB_USAGE_CPU_READ_OFTEN | AHB_USAGE_CPU_WRITE_OFTEN, -1, NULL, &ptr) != 0)
        return NULL;
    return ptr;
}

void cham_bo_unlock(struct cham_bo *bo)
{
    if (ahb.unlock)
        ahb.unlock(bo->ahb, NULL);
}

void bo_wait_release_fences_locked(void)
{
    for (struct cham_bo *bo = g_bos; bo; bo = bo->next) {
        if (bo->release_fence < 0)
            continue;
        struct pollfd pfd = {bo->release_fence, POLLIN, 0};
        poll(&pfd, 1, 100);
        close(bo->release_fence);
        bo->release_fence = -1;
    }
}
