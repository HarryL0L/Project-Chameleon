/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Chameleon's render node against the real kernel's.
 *
 * The same checks run twice and print the same transcript:
 *  - on the host (default build) against Chameleon's /dev/dri/renderD128,
 *    the shared buffer from Chameleon's gbm (run with libchameleon.so
 *    preloaded);
 *  - in a real kernel (built with -DREF_KERNEL, run in the reference VM
 *    with REFVM_DRM=virtio) against virtio-gpu's, the buffer a dumb buffer
 *    of its primary node.
 * Only device-independent outcomes are printed: which ioctls a render file
 * may use (DRM_RENDER_ALLOW), its node, and PRIME sharing between files.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

static void result(const char *what, int ret)
{
    printf("%-60s %s\n", what, ret == 0 ? "ok" : strerror(errno));
}

static const char *yes(int c)
{
    return c ? "yes" : "no";
}

/* A buffer of the primary node, as a dmabuf. */
#ifdef REF_KERNEL
static int shared_buffer(int card)
{
    struct drm_mode_create_dumb d = {.width = 64, .height = 64, .bpp = 32};
    struct drm_prime_handle p = {.flags = DRM_CLOEXEC, .fd = -1};
    if (ioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &d))
        return -1;
    p.handle = d.handle;
    ioctl(card, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p);
    struct drm_gem_close c = {.handle = d.handle};
    ioctl(card, DRM_IOCTL_GEM_CLOSE, &c); /* the dmabuf keeps the buffer */
    return p.fd;
}
#else
#include <gbm.h>
static int shared_buffer(int card)
{
    struct gbm_device *gbm = gbm_create_device(card);
    struct gbm_bo *bo = gbm ? gbm_bo_create(gbm, 64, 64, DRM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING) : NULL;
    return bo ? gbm_bo_get_fd(bo) : -1; /* bo and device stay: the process's */
}
#endif

static int send_fd(int s, int fd)
{
    char c = 0, buf[CMSG_SPACE(sizeof(int))];
    struct iovec io = {&c, 1};
    struct msghdr m = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = sizeof buf};
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof fd);
    return sendmsg(s, &m, 0) == 1 ? 0 : -1;
}

static int recv_fd(int s)
{
    char c, buf[CMSG_SPACE(sizeof(int))];
    struct iovec io = {&c, 1};
    struct msghdr m = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = sizeof buf};
    int fd = -1;
    if (recvmsg(s, &m, 0) == 1 && CMSG_FIRSTHDR(&m))
        memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&m)), sizeof fd);
    return fd;
}

static int import(int fd, int dmabuf, uint32_t *handle)
{
    struct drm_prime_handle p = {.fd = dmabuf};
    int ret = ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &p);
    *handle = p.handle;
    return ret;
}

int main(void)
{
    const char *path = getenv("CHAMELEON_DRM_PATH");
    int card = open(path ? path : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (card < 0) {
        perror("open the card");
        return 1;
    }

    printf("== node\n");
    struct stat st;
    result("stat /dev/dri/renderD128", stat("/dev/dri/renderD128", &st));
    printf("%-60s %s, %u:%u\n", "  character device / number", yes(S_ISCHR(st.st_mode)), major(st.st_rdev),
           minor(st.st_rdev));
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    result("open", fd < 0 ? -1 : 0);
    result("fstat", fstat(fd, &st));
    printf("%-60s %s, %u:%u\n", "  character device / number", yes(S_ISCHR(st.st_mode)), major(st.st_rdev),
           minor(st.st_rdev));

    printf("== render ioctls\n");
    char name[64] = "";
    struct drm_version v = {.name = name, .name_len = sizeof name - 1};
    result("VERSION", ioctl(fd, DRM_IOCTL_VERSION, &v));
    struct drm_get_cap cap = {.capability = DRM_CAP_PRIME};
    result("GET_CAP PRIME", ioctl(fd, DRM_IOCTL_GET_CAP, &cap));
    printf("%-60s %llu\n", "  value", (unsigned long long)cap.value);
    cap = (struct drm_get_cap){.capability = DRM_CAP_TIMESTAMP_MONOTONIC};
    result("GET_CAP TIMESTAMP_MONOTONIC", ioctl(fd, DRM_IOCTL_GET_CAP, &cap));
    struct drm_gem_close gc = {.handle = 12345};
    result("GEM_CLOSE of no handle", ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc));

    printf("== ioctls render files may not use\n");
    struct drm_set_client_cap ccap = {DRM_CLIENT_CAP_ATOMIC, 1};
    result("SET_CLIENT_CAP", ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &ccap));
    struct drm_auth auth = {0};
    result("GET_MAGIC", ioctl(fd, DRM_IOCTL_GET_MAGIC, &auth));
    result("AUTH_MAGIC", ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth));
    result("SET_MASTER", ioctl(fd, DRM_IOCTL_SET_MASTER, 0));
    result("DROP_MASTER", ioctl(fd, DRM_IOCTL_DROP_MASTER, 0));
    struct drm_unique u = {0};
    result("GET_UNIQUE", ioctl(fd, DRM_IOCTL_GET_UNIQUE, &u));
    struct drm_client client = {0};
    result("GET_CLIENT", ioctl(fd, DRM_IOCTL_GET_CLIENT, &client));
    result("GETRESOURCES", ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &(struct drm_mode_card_res){0}));
    result("GETPLANERESOURCES", ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &(struct drm_mode_get_plane_res){0}));
    result("ATOMIC", ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &(struct drm_mode_atomic){0}));
    result("CREATE_DUMB", ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &(struct drm_mode_create_dumb){.width = 64, .height = 64, .bpp = 32}));
    uint32_t data = 0;
    struct drm_mode_create_blob blob = {.data = (uintptr_t)&data, .length = sizeof data};
    result("CREATEPROPBLOB", ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob));
    result("ADDFB2", ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &(struct drm_mode_fb_cmd2){0}));
    struct drm_gem_flink flink = {.handle = 1};
    result("GEM_FLINK", ioctl(fd, DRM_IOCTL_GEM_FLINK, &flink));
    struct drm_gem_open gopen = {.name = 1};
    result("GEM_OPEN", ioctl(fd, DRM_IOCTL_GEM_OPEN, &gopen));
    union drm_wait_vblank vbl = {.request = {.type = _DRM_VBLANK_RELATIVE}};
    result("WAIT_VBLANK", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl));

    printf("== events\n");
    struct pollfd p = {fd, POLLIN, 0};
    int polled = poll(&p, 1, 0);
    printf("%-60s %d, 0x%x\n", "poll: ready / revents", polled, p.revents);
    char buf[64];
    result("read (non-blocking)", read(fd, buf, sizeof buf) < 0 ? -1 : 0);

    printf("== sharing\n");
    int dmabuf = shared_buffer(card);
    printf("%-60s %s\n", "a buffer of the primary node as a dmabuf", yes(dmabuf >= 0));
    uint32_t h = 0, h2 = 0, h3 = 0;
    result("PRIME_FD_TO_HANDLE", import(fd, dmabuf, &h));
    result("...again", import(fd, dmabuf, &h2));
    printf("%-60s %s\n", "  the same handle", yes(h && h == h2));
    struct drm_prime_handle ex = {.handle = h, .flags = DRM_CLOEXEC | DRM_RDWR, .fd = -1};
    result("PRIME_HANDLE_TO_FD", ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ex));
    printf("%-60s %s\n", "  close-on-exec", yes(fcntl(ex.fd, F_GETFD) == FD_CLOEXEC));
    result("the export imports", import(fd, ex.fd, &h3));
    printf("%-60s %s\n", "  as the same handle", yes(h3 == h));
    struct drm_prime_handle bad_flags = {.handle = h, .flags = 0x1234, .fd = -1};
    result("PRIME_HANDLE_TO_FD with unknown flags", ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &bad_flags));
    struct drm_prime_handle no_handle = {.handle = 999, .flags = DRM_CLOEXEC, .fd = -1};
    result("PRIME_HANDLE_TO_FD of no handle", ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &no_handle));
    gc.handle = h;
    result("GEM_CLOSE", ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc));
    result("GEM_CLOSE again", ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc));
    close(fd);
    close(dmabuf);
    int fd2 = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    result("a new render file imports the export once the rest is gone", import(fd2, ex.fd, &h));
    close(ex.fd);
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return 1;
    result("PRIME_FD_TO_HANDLE of a pipe", import(fd2, pipefd[0], &h));
    result("PRIME_FD_TO_HANDLE of a closed fd", import(fd2, 999, &h));
    close(fd2);

    printf("== another process's buffer\n");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
        return 1;
    pid_t pid = fork();
    if (pid == 0) { /* makes a buffer on the primary node, sends its dmabuf, exits */
        int c = open(path ? path : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
        int d = shared_buffer(c);
        _exit(d >= 0 && send_fd(pair[1], d) == 0 ? 0 : 1);
    }
    int theirs = recv_fd(pair[0]);
    int status = -1;
    waitpid(pid, &status, 0);
    printf("%-60s %s\n", "its dmabuf arrived, the process exited", yes(theirs >= 0 && WIFEXITED(status)));
    int fd3 = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    result("PRIME_FD_TO_HANDLE on the render node", import(fd3, theirs, &h));
    struct drm_prime_handle again = {.handle = h, .flags = DRM_CLOEXEC, .fd = -1};
    result("PRIME_HANDLE_TO_FD", ioctl(fd3, DRM_IOCTL_PRIME_HANDLE_TO_FD, &again));
    struct stat a, b;
    printf("%-60s %s\n", "  the same dmabuf",
           yes(fstat(theirs, &a) == 0 && fstat(again.fd, &b) == 0 && a.st_ino == b.st_ino));
    close(again.fd);
    close(theirs);
    close(fd3);
    return 0;
}
