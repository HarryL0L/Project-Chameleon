/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Desktop test for the fake KMS device, using the *real* libdrm the way
 * KWin 6.7's DRM backend does. Run through shim/test/run-host-test.sh, which
 * builds chameleon-kernel and libchameleon.so with CHAM_HOST_TEST (memfd
 * instead of AHardwareBuffer), starts the one and LD_PRELOADs the other.
 *
 * A stub presenter thread plays the Chameleon app: it reports a
 * 1080x1800 @ 120 Hz surface and answers each PRESENT with RELEASE and a
 * FRAME_DONE 8.3 ms later.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE
#define DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE 7
#endif

#include "../../common/cham_io.h"

static int failures;
#define CHECK(cond, ...)                                           \
    do {                                                           \
        if (cond) {                                                \
            printf("  ok   " __VA_ARGS__);                         \
        } else {                                                   \
            printf("  FAIL " __VA_ARGS__);                         \
            printf("       (%s, errno %s)\n", #cond, strerror(errno)); \
            failures++;                                            \
        }                                                          \
        printf("\n");                                              \
    } while (0)

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- stub presenter ---- */

static const char *g_sock_path;
static int g_presents, g_buffers_added, g_copy_mode, g_accepts;
static volatile int g_stop_presenter;
/* Set by the test: Android holds the frames it got (no FRAME_DONE until
 * cleared), the app is frozen (presents get no answer), the app's connection
 * drops, the app sends a CONFIG with these flags (-1: none). */
static volatile int g_hold_latch, g_frozen, g_drop, g_send_flags = -1;

static void *presenter_main(void *arg)
{
    (void)arg;
    int l = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, g_sock_path, sizeof addr.sun_path - 1);
    unlink(g_sock_path);
    bind(l, (struct sockaddr *)&addr, sizeof addr);
    listen(l, 1);
    int c = accept(l, NULL, NULL);
    g_accepts++;
    struct cham_msg cfg = {CHAM_CONFIG, 0, 1080ull | (1800ull << 32), 120000};
    cham_send(c, &cfg, -1);
    int64_t displayed = -1;
    uint64_t held = 0; /* the newest frame Android holds, 0 for none */
    while (!g_stop_presenter) {
        if (g_send_flags >= 0) {
            struct cham_msg flags = cfg;
            flags.id = (uint32_t)g_send_flags;
            cham_send(c, &flags, -1);
            g_send_flags = -1;
        }
        if (held && !g_hold_latch) {
            struct cham_msg done = {CHAM_FRAME_DONE, 0, held, now_ns()};
            cham_send(c, &done, -1);
            held = 0;
        }
        if (g_drop) {
            close(c);
            c = accept(l, NULL, NULL);
            g_accepts++;
            cham_send(c, &cfg, -1);
            g_drop = 0;
            continue;
        }
        struct pollfd p[2] = {{c, POLLIN, 0}, {l, POLLIN, 0}};
        if (poll(p, 2, 50) <= 0)
            continue;
        if (p[1].revents & POLLIN) {
            /* like the app: the newest producer wins, the old one is told */
            struct cham_msg bye = {CHAM_BYE, 0, CHAM_BYE_REPLACED, 0};
            cham_send(c, &bye, -1);
            close(c);
            c = accept(l, NULL, NULL);
            g_accepts++;
            continue;
        }
        if (!(p[0].revents & POLLIN))
            continue;
        struct cham_msg m;
        int fd;
        if (cham_recv(c, &m, &fd) <= 0)
            break;
        if (fd >= 0)
            close(fd);
        if (m.type == CHAM_HELLO) {
            g_copy_mode = (m.b & CHAM_HELLO_COPY) != 0;
        } else if (m.type == CHAM_BUFFER_ADD) {
            char packet[64];
            recv(c, packet, sizeof packet, 0); /* the (fake) AHardwareBuffer */
            g_buffers_added++;
        } else if (m.type == CHAM_PRESENT && !g_frozen) {
            g_presents++;
            struct cham_msg queued = {CHAM_FRAME_QUEUED, 0, m.a, now_ns()};
            cham_send(c, &queued, -1); /* "copied": the flip may complete on the next tick */
            usleep(8333);
            struct cham_msg rel = {CHAM_RELEASE, (uint32_t)m.id, 0, 0};
            cham_send(c, &rel, -1); /* copy mode: released right after the "blit" */
            (void)displayed;
            displayed = m.id;
            if (g_hold_latch) {
                held = m.a;
                continue;
            }
            struct cham_msg done = {CHAM_FRAME_DONE, 0, m.a, now_ns()};
            cham_send(c, &done, -1);
        }
    }
    close(c);
    close(l);
    return NULL;
}

/* ---- helpers ---- */

static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t *value)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    for (uint32_t i = 0; props && i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p && strcmp(p->name, name) == 0) {
            id = p->prop_id;
            if (value)
                *value = props->prop_values[i];
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static volatile int g_stop_calls;

static void *calls_main(void *arg) /* a thread in card calls while another forks */
{
    int fd = (int)(intptr_t)arg;
    while (!g_stop_calls)
        drmModeFreeResources(drmModeGetResources(fd));
    return NULL;
}

static int g_flips;
static uint64_t g_last_flip_ns, g_max_gap_ns;
static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, unsigned crtc, void *data)
{
    (void)fd, (void)seq, (void)crtc, (void)data;
    uint64_t t = (uint64_t)sec * 1000000000ull + (uint64_t)usec * 1000ull;
    if (g_last_flip_ns && t - g_last_flip_ns > g_max_gap_ns)
        g_max_gap_ns = t - g_last_flip_ns;
    g_last_flip_ns = t;
    g_flips++;
}

/* This process's fds of the file with inode `ino`. */
/* Fds of the file in every process: the test's and chameleon-kernel's,
 * which keeps a buffer while its dmabuf is held, as the kernel. */
static int fds_anywhere(dev_t dev, ino_t ino)
{
    DIR *procs = opendir("/proc");
    int n = 0;
    for (struct dirent *p; procs && (p = readdir(procs));) {
        char dir[300];
        if (p->d_name[0] < '1' || p->d_name[0] > '9')
            continue;
        snprintf(dir, sizeof dir, "/proc/%s/fd", p->d_name);
        DIR *fds = opendir(dir);
        for (struct dirent *e; fds && (e = readdir(fds));) {
            char path[600];
            struct stat st;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            if (e->d_name[0] != '.' && stat(path, &st) == 0 && st.st_dev == dev && st.st_ino == ino)
                n++;
        }
        if (fds)
            closedir(fds);
    }
    if (procs)
        closedir(procs);
    return n;
}

static void flip_to(int fd, uint32_t plane_id, uint32_t p_fb, uint32_t fb)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, p_fb, fb);
    if (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0)
        printf("       (flip commit: %s)\n", strerror(errno));
    drmModeAtomicFree(req);
}

static double ms_since(uint64_t t)
{
    return (now_ns() - t) / 1e6;
}

static int wait_flip(int fd, int timeout_ms)
{
    int before = g_flips;
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0)
        return 0;
    drmEventContext ctx = {.version = 3, .page_flip_handler2 = on_flip};
    drmHandleEvent(fd, &ctx);
    return g_flips > before;
}

/* Run again by exec, with libchameleon.so preloaded as in every program of
 * a session: a card fd it inherited is still the card. */
static int exec_card(int fd)
{
    struct stat st;
    drmModeRes *r = drmModeGetResources(fd);
    return fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 226 && r && r->count_crtcs == 1 ? 0 : 1;
}

/* ...and an inherited render-node fd is still the render node (the service
 * tells which node a file is when it meets an unknown copy). */
static int exec_render(int fd)
{
    struct stat st;
    drmVersionPtr v = drmGetVersion(fd);
    int ok = fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == makedev(226, 128) && v;
    drmFreeVersion(v);
    errno = 0;
    return ok && drmModeGetResources(fd) == NULL && errno == EACCES ? 0 : 1;
}

static int count_sockets(void)
{
    int sockets = 0;
    char link[64], path[64];
    for (int fd = 0; fd < 256; fd++) {
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, link, sizeof link - 1);
        sockets += n > 0 && strncmp(link, "socket:", 7) == 0;
    }
    return sockets;
}

/* ...and a program using only ordinary pipes never talks to chameleon-kernel
 * (no socket appears). */
static int pipes_only(void)
{
    int before = count_sockets(); /* inherited ones */
    for (int i = 0; i < 50; i++) {
        int p[2];
        struct stat st;
        if (pipe(p) != 0 || fstat(p[0], &st) != 0 || fstat(p[1], &st) != 0 || !S_ISFIFO(st.st_mode))
            return 1;
        drmVersion *v = drmGetVersion(p[0]); /* a DRM ioctl on a pipe: ENOTTY, from the pipe */
        if (v)
            return 1;
    }
    return count_sockets() == before ? 0 : 1;
}

/* Runs this program again with `mode` (and `arg`) and returns its exit code. */
static int run_self(const char *mode, const char *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("/proc/self/exe", "kms_test", mode, arg, (char *)NULL);
        _exit(127);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "exec-card") == 0)
        return exec_card(atoi(argv[2]));
    if (argc == 3 && strcmp(argv[1], "exec-render") == 0)
        return exec_render(atoi(argv[2]));
    if (argc >= 2 && strcmp(argv[1], "pipes") == 0)
        return pipes_only();
    const char *dev_path = getenv("CHAMELEON_DRM_PATH");
    g_sock_path = getenv("CHAMELEON_SOCKET");
    if (!dev_path || !g_sock_path) {
        fprintf(stderr, "run via run-host-test.sh\n");
        return 2;
    }
    pthread_t presenter;
    pthread_create(&presenter, NULL, presenter_main, NULL);

    printf("device\n");
    struct stat st;
    char num_r[16];
    CHECK(stat(dev_path, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 226, "stat: DRM char device");
    int fd = open(dev_path, O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0, "open fake path");
    CHECK(fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 226, "fstat: DRM char device");
    CHECK(drmIsKMS(fd), "drmIsKMS");
    drmVersion *ver = drmGetVersion(fd);
    CHECK(ver && strcmp(ver->name, "chameleon") == 0, "drmGetVersion name '%s'", ver ? ver->name : "?");
    drmFreeVersion(ver);
    uint64_t cap = 0;
    CHECK(drmGetCap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap) == 0 && cap == 1, "monotonic timestamps");
    CHECK(drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap == 0, "implicit modifiers only");
    CHECK(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0, "atomic client cap");
    CHECK(drmSetClientCap(fd, DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE, 1) != 0, "color pipeline refused");
    drm_magic_t magic;
    CHECK(drmGetMagic(fd, &magic) == 0 && drmAuthMagic(fd, magic) == 0, "magic / auth (RenderDevice::open)");
    CHECK(drmIsMaster(fd), "drmIsMaster");

    printf("objects\n");
    drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
    CHECK(pres && pres->count_planes == 1, "one plane");
    uint32_t plane_id = pres ? pres->planes[0] : 0;
    drmModeFreePlaneResources(pres);
    drmModePlane *plane = drmModeGetPlane(fd, plane_id);
    CHECK(plane && plane->count_formats == 4 && plane->formats[0] == DRM_FORMAT_XRGB8888 &&
              plane->possible_crtcs == 1,
          "plane: XRGB8888 first of 4 formats, crtc mask 1");
    drmModeFreePlane(plane);
    uint64_t type = 99;
    uint32_t p_type = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
    drmModePropertyRes *tp = drmModeGetProperty(fd, p_type);
    CHECK(tp && tp->count_enums == 3 && strcmp(tp->enums[type].name, "Primary") == 0, "plane type enum = Primary");
    drmModeFreeProperty(tp);

    drmModeRes *res = drmModeGetResources(fd);
    CHECK(res && res->count_crtcs == 1 && res->count_connectors == 1 && res->count_encoders == 1,
          "resources: 1 crtc / connector / encoder");
    if (!res || !res->count_crtcs || !res->count_connectors) {
        printf("\nFAILED: no card objects to test\n");
        return 1;
    }
    uint32_t crtc_id = res->crtcs[0], conn_id = res->connectors[0];
    drmModeConnector *conn = drmModeGetConnector(fd, conn_id);
    CHECK(conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes == 1, "connector connected, 1 mode");
    if (!conn || !conn->count_modes) {
        printf("\nFAILED: no connector mode to test\n");
        return 1;
    }
    drmModeModeInfo mode = conn->modes[0];
    CHECK(mode.hdisplay == 1080 && mode.vdisplay == 1800 && mode.vrefresh == 120, "mode %s @ %u Hz (from the app)",
          mode.name, mode.vrefresh);
    double hz = mode.clock * 1000.0 / (mode.htotal * mode.vtotal);
    CHECK(hz > 119.9 && hz < 120.1, "mode timings give %.3f Hz", hz);
    CHECK(conn->mmWidth > 50 && conn->mmWidth < 100, "physical size %ux%u mm", conn->mmWidth, conn->mmHeight);
    CHECK(drmModeConnectorGetPossibleCrtcs(fd, conn) == 1, "possible crtcs via encoder");
    CHECK(drmModeGetConnectorTypeName(conn->connector_type) &&
              strcmp(drmModeGetConnectorTypeName(conn->connector_type), "DSI") == 0,
          "connector name DSI-%u", conn->connector_type_id);
    /* An EDID block a compositor can parse: header, checksum, name, timing. */
    uint32_t p_edid = prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "EDID", NULL);
    drmModeObjectProperties *cprops = drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
    uint64_t edid_id = 0;
    for (uint32_t i = 0; cprops && i < cprops->count_props; i++)
        if (cprops->props[i] == p_edid)
            edid_id = cprops->prop_values[i];
    drmModeFreeObjectProperties(cprops);
    drmModePropertyBlobRes *edid = edid_id ? drmModeGetPropertyBlob(fd, (uint32_t)edid_id) : NULL;
    const uint8_t *eb = edid ? edid->data : NULL;
    uint8_t sum = 0;
    for (int i = 0; eb && i < 128; i++)
        sum = (uint8_t)(sum + eb[i]);
    CHECK(edid && edid->length == 128 && eb[0] == 0 && eb[1] == 0xff && eb[7] == 0 && sum == 0,
          "EDID: a valid 128-byte block");
    CHECK(eb && eb[75] == 0xfc && memcmp(eb + 77, "Chameleon\n", 10) == 0, "EDID: named Chameleon");
    CHECK(eb && (eb[56] | (eb[58] >> 4) << 8) == mode.hdisplay && (eb[59] | (eb[61] >> 4) << 8) == mode.vdisplay &&
              (uint32_t)(eb[54] | eb[55] << 8) == mode.clock / 10,
          "EDID: the mode is its preferred timing");
    drmModeFreePropertyBlob(edid);
    drmModeFreeConnector(conn);
    drmModeCrtc *crtc = drmModeGetCrtc(fd, crtc_id);
    CHECK(crtc && !crtc->mode_valid, "crtc initially off");
    drmModeFreeCrtc(crtc);
    drmModeFreeResources(res);

    uint32_t p_fb = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
    uint32_t p_pcrtc = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
    uint32_t p_fence = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD", NULL);
    uint32_t p_src[4], p_dst[4];
    const char *src_names[] = {"SRC_X", "SRC_Y", "SRC_W", "SRC_H"}, *dst_names[] = {"CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H"};
    for (int i = 0; i < 4; i++) {
        p_src[i] = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, src_names[i], NULL);
        p_dst[i] = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, dst_names[i], NULL);
    }
    uint32_t p_active = prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
    uint32_t p_mode = prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
    uint32_t p_conn_crtc = prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
    CHECK(p_fb && p_pcrtc && p_fence && p_src[3] && p_dst[3] && p_active && p_mode && p_conn_crtc,
          "all properties KWin needs exist");

    printf("buffers\n");
    struct gbm_device *gbm = gbm_create_device(fd);
    struct gbm_bo *bos[3];
    uint32_t fbs[3];
    for (int i = 0; i < 3; i++) {
        bos[i] = gbm_bo_create(gbm, 1080, 1800, DRM_FORMAT_XBGR8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        int dmabuf = gbm_bo_get_fd(bos[i]);
        uint32_t handle = 0;
        int ok = drmPrimeFDToHandle(fd, dmabuf, &handle) == 0 && handle == gbm_bo_get_handle(bos[i]).u32;
        close(dmabuf);
        uint32_t handles[4] = {handle}, pitches[4] = {gbm_bo_get_stride(bos[i])}, offsets[4] = {0};
        ok = ok && drmModeAddFB2(fd, 1080, 1800, DRM_FORMAT_XBGR8888, handles, pitches, offsets, &fbs[i], 0) == 0;
        drmCloseBufferHandle(fd, handle);
        CHECK(ok, "bo %d: gbm -> dmabuf -> prime handle -> fb %u", i, fbs[i]);
    }
    {
        uint32_t handles[4] = {gbm_bo_get_handle(bos[0]).u32}, pitches[4] = {gbm_bo_get_stride(bos[0])},
                 offsets[4] = {0}, fb = 0;
        CHECK(drmModeAddFB2(fd, 1080, 1800, DRM_FORMAT_RGB565, handles, pitches, offsets, &fb, 0) != 0 &&
                  errno == EINVAL,
              "a framebuffer in a format no plane shows is refused (EINVAL)");
        struct gbm_bo *xrgb = gbm_bo_create(gbm, 640, 480, DRM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT);
        handles[0] = xrgb ? gbm_bo_get_handle(xrgb).u32 : 0;
        pitches[0] = xrgb ? gbm_bo_get_stride(xrgb) : 0;
        CHECK(xrgb && drmModeAddFB2(fd, 640, 480, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb, 0) == 0 &&
                  drmModeRmFB(fd, fb) == 0,
              "an XRGB8888 framebuffer is accepted");
        if (xrgb)
            gbm_bo_destroy(xrgb);
    }
    int foreign = memfd_create("not-ours", 0);
    uint32_t h;
    CHECK(drmPrimeFDToHandle(fd, foreign, &h) != 0, "foreign dmabuf rejected (no direct scanout)");
    close(foreign);
    /* As in the kernel, a dmabuf keeps its buffer after gbm_bo_destroy
     * (KWin 6.8 keeps only the fd), until nothing holds it any more. */
    {
        struct gbm_bo *bo = gbm_bo_create(gbm, 256, 256, DRM_FORMAT_XBGR8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        uint32_t stride = bo ? gbm_bo_get_stride(bo) : 0, kept = 0;
        int dmabuf = bo ? gbm_bo_get_fd(bo) : -1;
        struct stat st = {0};
        fstat(dmabuf, &st);
        if (bo)
            gbm_bo_destroy(bo);
        uint32_t handles[4] = {0}, pitches[4] = {stride}, offsets[4] = {0}, fb = 0;
        int ok = drmPrimeFDToHandle(fd, dmabuf, &handles[0]) == 0 && handles[0];
        ok = ok && drmModeAddFB2(fd, 256, 256, DRM_FORMAT_XBGR8888, handles, pitches, offsets, &fb, 0) == 0 &&
             drmModeRmFB(fd, fb) == 0;
        CHECK(ok, "after gbm_bo_destroy, its dmabuf still gives a handle and a framebuffer");
        uint32_t again = 0;
        CHECK(drmPrimeFDToHandle(fd, dmabuf, &again) == 0 && again == handles[0], "...the same handle again (%u, %u)",
              handles[0], again);
        int exported = -1;
        struct stat est = {0};
        CHECK(drmPrimeHandleToFD(fd, handles[0], DRM_CLOEXEC, &exported) == 0 && fstat(exported, &est) == 0 &&
                  est.st_ino == st.st_ino,
              "...exported again through the card, the same dmabuf");
        if (exported >= 0)
            close(exported);
        drmCloseBufferHandle(fd, handles[0]);
        kept = (uint32_t)fds_anywhere(st.st_dev, st.st_ino);
        close(dmabuf);
        struct gbm_bo *next = gbm_bo_create(gbm, 64, 64, DRM_FORMAT_XBGR8888, GBM_BO_USE_RENDERING); /* frees the rest */
        if (next)
            gbm_bo_destroy(next);
        CHECK(kept > 1 && fds_anywhere(st.st_dev, st.st_ino) == 0,
              "freed everywhere once its last fd is closed (%u fds while held, %d after)", kept,
              fds_anywhere(st.st_dev, st.st_ino));
    }

    printf("modeset\n");
    uint32_t blob = 0;
    CHECK(drmModeCreatePropertyBlob(fd, &mode, sizeof mode, &blob) == 0, "mode blob %u", blob);
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, conn_id, p_conn_crtc, crtc_id);
    drmModeAtomicAddProperty(req, crtc_id, p_active, 1);
    drmModeAtomicAddProperty(req, crtc_id, p_mode, blob);
    drmModeAtomicAddProperty(req, plane_id, p_fb, fbs[0]);
    drmModeAtomicAddProperty(req, plane_id, p_pcrtc, crtc_id);
    uint64_t src[4] = {0, 0, 1080ull << 16, 1800ull << 16}, dst[4] = {0, 0, 1080, 1800};
    for (int i = 0; i < 4; i++) {
        drmModeAtomicAddProperty(req, plane_id, p_src[i], src[i]);
        drmModeAtomicAddProperty(req, plane_id, p_dst[i], dst[i]);
    }
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_NONBLOCK, NULL) != 0,
          "modeset without ALLOW_MODESET refused");
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0,
          "test modeset");
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0, "blocking modeset commit");
    drmModeAtomicFree(req);
    crtc = drmModeGetCrtc(fd, crtc_id);
    CHECK(crtc && crtc->mode_valid && crtc->mode.hdisplay == 1080, "crtc on with our mode");
    drmModeFreeCrtc(crtc);

    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, p_dst[0], 100);
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL) == 0,
          "smaller plane (KWin's cursor alone on an empty screen) accepted");
    drmModeAtomicFree(req);

    /* KWin's custom modes (the screen following a rotated window). */
    drmModeModeInfo rotated = mode;
    rotated.hdisplay = 1800;
    rotated.vdisplay = 1080;
    uint32_t rotated_blob = 0;
    drmModeCreatePropertyBlob(fd, &rotated, sizeof rotated, &rotated_blob);
    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, crtc_id, p_mode, rotated_blob);
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0,
          "other mode with the old plane size accepted (clipped / not shown)");
    drmModeAtomicAddProperty(req, plane_id, p_dst[2], 1800);
    drmModeAtomicAddProperty(req, plane_id, p_dst[3], 1080);
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0,
          "any other mode size accepted (1800x1080)");
    drmModeAtomicFree(req);
    /* Right after a mode change KWin may still commit the old frame size. */
    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, p_dst[2], 1800);
    drmModeAtomicAddProperty(req, plane_id, p_dst[3], 1800);
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL) == 0,
          "plane bigger than the mode accepted (clipped, as real drivers do)");
    drmModeAtomicFree(req);
    drmModeDestroyPropertyBlob(fd, rotated_blob);

    /* A mode blob the CRTC uses outlives its destruction, as in the kernel:
     * after a hotplug KWin destroys the old mode's blob, then still commits
     * a frame on it before its modeset. */
    uint32_t used_blob = 0;
    drmModeCreatePropertyBlob(fd, &mode, sizeof mode, &used_blob);
    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, crtc_id, p_mode, used_blob);
    int used = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0;
    drmModeAtomicFree(req);
    used = used && drmModeDestroyPropertyBlob(fd, used_blob) == 0;
    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, p_dst[0], 0);
    CHECK(used && drmModeAtomicCommit(fd, req, 0, NULL) == 0,
          "the CRTC's mode blob, destroyed, still holds the mode for the next commit");
    drmModeAtomicFree(req);
    crtc = drmModeGetCrtc(fd, crtc_id);
    CHECK(crtc && crtc->mode_valid && crtc->mode.hdisplay == mode.hdisplay, "...and GETCRTC still shows it");
    drmModeFreeCrtc(crtc);
    CHECK(drmModeDestroyPropertyBlob(fd, used_blob) != 0, "destroying it twice fails");
    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, crtc_id, p_mode, blob);
    CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0, "back to the first mode blob");
    drmModeAtomicFree(req);
    int fence_pipe[2];
    if (pipe(fence_pipe) != 0)
        return 1;
    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, p_fb, fbs[0]);
    drmModeAtomicAddProperty(req, plane_id, p_fence, (uint64_t)fence_pipe[0]);
    CHECK(drmModeAtomicCommit(fd, req, 0, NULL) == 0, "a commit with an IN_FENCE_FD (sent along with it)");
    drmModeAtomicFree(req);
    CHECK(fcntl(fence_pipe[0], F_GETFD) >= 0, "...which stays the caller's");
    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, p_fb, fbs[0]);
    drmModeAtomicAddProperty(req, plane_id, p_fence, 9999);
    CHECK(drmModeAtomicCommit(fd, req, 0, NULL) != 0 && errno == EINVAL, "IN_FENCE_FD that isn't an open file: EINVAL");
    drmModeAtomicFree(req);
    close(fence_pipe[0]);
    close(fence_pipe[1]);

    printf("page flips (presenter at 120 Hz)\n");
    int ok = 1, busy_checked = 0;
    uint64_t t0 = now_ns();
    for (int i = 1; i <= 60; i++) {
        req = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(req, plane_id, p_fb, fbs[i % 3]);
        drmModeAtomicAddProperty(req, plane_id, p_fence, (uint64_t)-1);
        if (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0)
            ok = 0;
        if (!busy_checked && i == 3) { /* the first flip after a modeset may complete at once */
            busy_checked = 1;
            CHECK(drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0 &&
                      errno == EBUSY,
                  "second flip while one is pending -> EBUSY");
        }
        drmModeAtomicFree(req);
        if (!wait_flip(fd, 500))
            ok = 0;
    }
    double secs = (now_ns() - t0) / 1e9;
    CHECK(ok && g_flips == 60, "60 flips completed via FRAME_DONE (%.1f fps)", 60 / secs);
    CHECK(g_presents >= 61 && g_buffers_added == 3, "presenter saw %d presents of %d buffers", g_presents,
          g_buffers_added);
    CHECK(g_copy_mode, "shim asked for copy mode");

    /* A flip completes once Android latched the frames it must have by then:
     * the one before (one frame may wait in Android), or with back pressure
     * the one before that (two may), else the watchdog completes it. */
    printf("frames waiting in Android\n");
    usleep(50000); /* the last flip completed when its frame was copied; Android shows it now */
    g_hold_latch = 1;
    flip_to(fd, plane_id, p_fb, fbs[0]);
    CHECK(wait_flip(fd, 100), "a frame copied, not shown yet: its flip completes");
    flip_to(fd, plane_id, p_fb, fbs[1]);
    uint64_t t = now_ns();
    int early = wait_flip(fd, 120), late = !early && wait_flip(fd, 500);
    double ms = ms_since(t);
    CHECK(!early, "the next one waits for Android to show the first");
    CHECK(late && ms > 200 && ms < 400, "...until the watchdog completes it (%.0f ms)", ms);
    g_hold_latch = 0;
    usleep(150000);
    g_send_flags = CHAM_CONFIG_BACK_PRESSURE;
    usleep(150000);
    flip_to(fd, plane_id, p_fb, fbs[2]);
    wait_flip(fd, 100);
    usleep(50000);
    g_hold_latch = 1;
    flip_to(fd, plane_id, p_fb, fbs[2]);
    int first = wait_flip(fd, 100);
    flip_to(fd, plane_id, p_fb, fbs[0]);
    CHECK(first && wait_flip(fd, 60), "with back pressure, a second frame may wait in Android");
    flip_to(fd, plane_id, p_fb, fbs[1]);
    t = now_ns();
    early = wait_flip(fd, 120);
    late = !early && wait_flip(fd, 500);
    CHECK(!early && late, "...a third waits (%.0f ms)", ms_since(t));
    g_hold_latch = 0;
    usleep(150000);
    g_send_flags = 0;
    usleep(150000);

    /* A flip needs the frame before it copied, not its own: the first one
     * after the app froze still completes, the next waits for the watchdog. */
    printf("app frozen\n");
    g_frozen = 1;
    flip_to(fd, plane_id, p_fb, fbs[2]);
    first = wait_flip(fd, 100);
    flip_to(fd, plane_id, p_fb, fbs[1]);
    t = now_ns();
    ok = wait_flip(fd, 500);
    ms = ms_since(t);
    CHECK(first && ok && ms > 200 && ms < 400, "the watchdog completes the next flip (%.0f ms)", ms);
    g_frozen = 0;
    flip_to(fd, plane_id, p_fb, fbs[0]);
    CHECK(wait_flip(fd, 100), "the app answers again: flips complete at once");

    printf("app reconnects\n");
    int added = g_buffers_added, accepts = g_accepts;
    g_drop = 1;
    for (int i = 0; i < 30 && g_accepts == accepts; i++)
        usleep(100000);
    CHECK(g_accepts == accepts + 1, "the shim reconnects after the app's connection dropped");
    usleep(100000);
    ok = 1;
    t = now_ns();
    for (int i = 1; i <= 10; i++) {
        flip_to(fd, plane_id, p_fb, fbs[i % 3]);
        if (!wait_flip(fd, 100))
            ok = 0;
    }
    CHECK(ok, "10 flips completed by the app again (%.0f ms)", ms_since(t));
    CHECK(g_buffers_added == added + 3, "its 3 buffers sent again (%d)", g_buffers_added - added);

    printf("second producer takes over\n");
    accepts = g_accepts;
    int intruder = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, g_sock_path, sizeof addr.sun_path - 1);
    connect(intruder, (struct sockaddr *)&addr, sizeof addr);
    usleep(2500000);
    CHECK(g_accepts == accepts + 1, "replaced shim does not reconnect (%d connections, want %d)", g_accepts,
          accepts + 1);
    close(intruder);

    printf("presenter gone -> simulated vblank\n");
    g_stop_presenter = 1;
    pthread_join(presenter, NULL);
    usleep(100000);
    g_flips = 0;
    t0 = now_ns();
    ok = 1;
    for (int i = 0; i < 12; i++) {
        req = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(req, plane_id, p_fb, fbs[i % 3]);
        if (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0)
            ok = 0;
        drmModeAtomicFree(req);
        if (!wait_flip(fd, 500))
            ok = 0;
    }
    secs = (now_ns() - t0) / 1e9;
    CHECK(ok && g_flips == 12, "12 flips without an app (%.1f fps, KWin keeps running)", 12 / secs);

    /* As Linux 6.8 with vkms answers for a normal user (shim/test/refvm.sh). */
    printf("files: master, client caps, handles, ownership\n");
    int root = geteuid() == 0; /* CAP_SYS_ADMIN may take master */
    int fd2 = open(dev_path, O_RDWR | O_CLOEXEC);
    CHECK(fd2 >= 0 && !drmIsMaster(fd2), "a second open is not master");
    errno = 0;
    CHECK(drmSetMaster(fd2) != 0 && errno == (root ? EBUSY : EACCES), "...SET_MASTER: %s",
          root ? "EBUSY while another is master" : "EACCES (never was master)");
    pres = drmModeGetPlaneResources(fd2);
    CHECK(pres && pres->count_planes == 0, "no planes listed without the universal-planes cap");
    drmModeFreePlaneResources(pres);
    drmModeObjectProperties *op = drmModeObjectGetProperties(fd2, crtc_id, DRM_MODE_OBJECT_CRTC);
    CHECK(op && op->count_props == 0, "atomic properties hidden without the atomic cap");
    drmModeFreeObjectProperties(op);
    CHECK(drmSetClientCap(fd2, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1) != 0 && errno == EINVAL,
          "writeback cap needs the atomic cap first");
    CHECK(drmSetClientCap(fd2, DRM_CLIENT_CAP_ATOMIC, 3) != 0 && errno == EINVAL, "atomic cap 3: EINVAL");
    CHECK(drmSetClientCap(fd2, DRM_CLIENT_CAP_ATOMIC, 1) == 0, "atomic cap on the second file");
    pres = drmModeGetPlaneResources(fd2);
    CHECK(pres && pres->count_planes == 1, "...which lists the plane");
    drmModeFreePlaneResources(pres);
    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, p_fb, fbs[0]);
    CHECK(drmModeAtomicCommit(fd2, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL) != 0 && errno == EACCES,
          "a commit from a non-master file: EACCES");
    drmModeAtomicFree(req);
    uint32_t h1 = gbm_bo_get_handle(bos[1]).u32, fb2 = 0;
    uint32_t handles2[4] = {h1}, pitches2[4] = {gbm_bo_get_stride(bos[1])}, offsets2[4] = {0};
    CHECK(drmModeAddFB2(fd2, 1080, 1800, DRM_FORMAT_XBGR8888, handles2, pitches2, offsets2, &fb2, 0) != 0 &&
              errno == ENOENT,
          "another file's handle number means nothing here (ENOENT)");
    int dmabuf = gbm_bo_get_fd(bos[1]);
    uint32_t h2 = 0, h2again = 0;
    CHECK(drmPrimeFDToHandle(fd2, dmabuf, &h2) == 0 && drmPrimeFDToHandle(fd2, dmabuf, &h2again) == 0 &&
              h2 && h2 == h2again,
          "importing the same buffer twice gives the same handle");
    close(dmabuf);
    handles2[0] = h2;
    CHECK(drmModeAddFB2(fd2, 1080, 1800, DRM_FORMAT_XBGR8888, handles2, pitches2, offsets2, &fb2, 0) == 0,
          "a non-master file adds a framebuffer");
    CHECK(drmCloseBufferHandle(fd2, h2) == 0 && drmCloseBufferHandle(fd2, h2) != 0 && errno == EINVAL,
          "GEM_CLOSE twice: EINVAL the second time");
    CHECK(drmModeRmFB(fd, fb2) != 0 && errno == ENOENT, "RMFB of another file's framebuffer: ENOENT");
    uint32_t blob2 = 0;
    CHECK(drmModeCreatePropertyBlob(fd2, "abcd", 4, &blob2) == 0 && drmModeDestroyPropertyBlob(fd, blob2) != 0 &&
              errno == EPERM,
          "a non-master file creates a blob; another file can't destroy it (EPERM)");
    drm_magic_t magic2 = 0;
    CHECK(drmGetMagic(fd2, &magic2) == 0 && drmAuthMagic(fd2, magic2) != 0 && errno == EACCES,
          "AUTH_MAGIC from a non-master: EACCES");
    CHECK(drmAuthMagic(fd, magic2) == 0 && drmAuthMagic(fd, magic2) != 0 && errno == EINVAL,
          "the master authenticates the magic once");
    int dup2fd = dup(fd2);
    close(fd2);
    drmModePropertyBlobRes *pb = drmModeGetPropertyBlob(fd, blob2);
    CHECK(pb != NULL, "a dup keeps the file open");
    drmModeFreePropertyBlob(pb);
    close(dup2fd);
    pb = drmModeGetPropertyBlob(fd, blob2);
    CHECK(pb == NULL, "closing the last copy releases the file's blobs");
    drmModeFreePropertyBlob(pb);
    CHECK(drmModeRmFB(fd2, fb2) != 0, "...and framebuffers");
    CHECK(drmDropMaster(fd) == 0 && !drmIsMaster(fd), "the master drops master");
    int fd3 = open(dev_path, O_RDWR | O_CLOEXEC);
    CHECK(fd3 >= 0 && drmIsMaster(fd3), "the next open while there is none becomes master");
    close(fd3);
    CHECK(drmSetMaster(fd) == 0 && drmIsMaster(fd), "...and once it closed, the old master takes it back");

    printf("teardown\n");
    uint64_t on_plane = 0;
    prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &on_plane);
    struct drm_mode_closefb cfb = {.fb_id = (uint32_t)on_plane};
    CHECK(on_plane && drmIoctl(fd, DRM_IOCTL_MODE_CLOSEFB, &cfb) == 0, "CLOSEFB of the fb on screen");
    uint64_t still = 0;
    prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &still);
    CHECK(still == on_plane, "...leaves it on the plane");
    uint32_t rm = 0;
    for (int i = 0; i < 3; i++)
        if (fbs[i] != on_plane)
            rm = fbs[i];
    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, p_fb, rm);
    CHECK(drmModeAtomicCommit(fd, req, 0, NULL) == 0, "show another framebuffer");
    drmModeAtomicFree(req);
    uint64_t active = 0;
    CHECK(drmModeRmFB(fd, rm) == 0 && prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &still) &&
              prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &active) && still == 0 && active == 1,
          "RMFB of the fb on screen takes it off the plane, the CRTC stays on");
    for (int i = 0; i < 3; i++)
        if (fbs[i] != on_plane && fbs[i] != rm)
            drmModeRmFB(fd, fbs[i]);
    for (int i = 0; i < 3; i++)
        gbm_bo_destroy(bos[i]);
    gbm_device_destroy(gbm);
    CHECK(drmModeDestroyPropertyBlob(fd, blob) == 0, "destroy mode blob");
    close(fd);
    int fd4 = open(dev_path, O_RDWR | O_CLOEXEC);
    CHECK(fd4 >= 0 && drmIsMaster(fd4), "after the master closed, a new open becomes master");
    close(fd4);

    /* As Linux 6.8 with virtio-gpu answers on its render node (shim/test/refvm.sh). */
    printf("render node\n");
    CHECK(stat("/dev/dri/renderD128", &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == makedev(226, 128),
          "stat: /dev/dri/renderD128 is 226:128");
    int master = open(dev_path, O_RDWR | O_CLOEXEC);
    int rfd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    drmVersionPtr rv = rfd >= 0 ? drmGetVersion(rfd) : NULL;
    CHECK(rv && strcmp(rv->name, "chameleon") == 0, "it opens; VERSION names the same driver");
    drmFreeVersion(rv);
    cap = 0;
    CHECK(drmGetCap(rfd, DRM_CAP_PRIME, &cap) == 0 && (cap & DRM_PRIME_CAP_IMPORT) && (cap & DRM_PRIME_CAP_EXPORT),
          "GET_CAP: PRIME import and export");
    CHECK(fstat(rfd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == makedev(226, 128), "fstat: 226:128");
    CHECK(master >= 0 && drmIsMaster(master) && !drmIsMaster(rfd), "a render file is never master");
    errno = 0;
    CHECK(drmSetMaster(rfd) != 0 && errno == EACCES, "SET_MASTER: EACCES");
    errno = 0;
    CHECK(drmSetClientCap(rfd, DRM_CLIENT_CAP_ATOMIC, 1) != 0 && errno == EACCES, "SET_CLIENT_CAP: EACCES");
    drm_magic_t rmagic;
    errno = 0;
    CHECK(drmGetMagic(rfd, &rmagic) != 0 && errno == EACCES, "GET_MAGIC: EACCES");
    errno = 0;
    CHECK(drmModeGetResources(rfd) == NULL && errno == EACCES, "mode setting ioctls: EACCES");
    struct drm_mode_create_dumb dumb = {.width = 64, .height = 64, .bpp = 32};
    errno = 0;
    CHECK(drmIoctl(rfd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) != 0 && errno == EACCES, "CREATE_DUMB: EACCES");
    errno = 0;
    CHECK(drmIoctl(master, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) != 0 && errno == ENOSYS,
          "...and ENOSYS on the primary node (no dumb buffers)");
    struct drm_gem_flink flink = {.handle = 1};
    errno = 0;
    CHECK(drmIoctl(rfd, DRM_IOCTL_GEM_FLINK, &flink) != 0 && errno == EACCES, "GEM_FLINK: EACCES");
    struct drm_gem_close gclose = {.handle = 12345};
    errno = 0;
    CHECK(drmIoctl(rfd, DRM_IOCTL_GEM_CLOSE, &gclose) != 0 && errno == EINVAL, "GEM_CLOSE of no handle: EINVAL");
    struct pollfd rp = {rfd, POLLIN, 0};
    CHECK(poll(&rp, 1, 0) == 0, "no events to read");

    struct gbm_device *rgbm = gbm_create_device(rfd);
    struct gbm_bo *rbo = rgbm ? gbm_bo_create(rgbm, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING) : NULL;
    uint32_t rh = rbo ? gbm_bo_get_handle(rbo).u32 : 0;
    CHECK(rh, "gbm on the render node: a buffer with a handle");
    int rdmabuf = -1;
    uint32_t rh2 = 0, mh = 0, rfb = 0;
    CHECK(drmPrimeHandleToFD(rfd, rh, DRM_CLOEXEC, &rdmabuf) == 0 && rdmabuf >= 0 &&
              drmPrimeFDToHandle(rfd, rdmabuf, &rh2) == 0 && rh2 == rh,
          "PRIME export, and the export imports as the same handle");
    uint32_t rhandles[4] = {0}, rpitches[4] = {rbo ? gbm_bo_get_stride(rbo) : 0}, roffsets[4] = {0};
    CHECK(drmPrimeFDToHandle(master, rdmabuf, &mh) == 0 && mh && (rhandles[0] = mh) &&
              drmModeAddFB2(master, 64, 64, DRM_FORMAT_XRGB8888, rhandles, rpitches, roffsets, &rfb, 0) == 0,
          "the primary node imports the render node's buffer and makes a framebuffer of it");
    drmModeRmFB(master, rfb);
    if (rdmabuf >= 0)
        close(rdmabuf);
    gbm_bo_destroy(rbo);
    gbm_device_destroy(rgbm);

    struct drm_client client = {.idx = 0};
    CHECK(drmIoctl(master, DRM_IOCTL_GET_CLIENT, &client) == 0 && client.pid == (unsigned long)getpid() &&
              client.auth == 1,
          "GET_CLIENT 0: this process, authenticated");
    client.idx = 1;
    errno = 0;
    CHECK(drmIoctl(master, DRM_IOCTL_GET_CLIENT, &client) != 0 && errno == EINVAL, "GET_CLIENT 1: EINVAL");
    int plain = open(dev_path, O_RDWR | O_CLOEXEC);
    flink.handle = 12345;
    errno = 0;
    CHECK(drmIoctl(plain, DRM_IOCTL_GEM_FLINK, &flink) != 0 && errno == (root ? ENOENT : EACCES),
          "GEM_FLINK from a file not authenticated: %s", root ? "ENOENT (root is)" : "EACCES");
    drm_magic_t pmagic = 0;
    errno = 0;
    CHECK(drmGetMagic(plain, &pmagic) == 0 && drmAuthMagic(master, pmagic) == 0 &&
              drmIoctl(plain, DRM_IOCTL_GEM_FLINK, &flink) != 0 && errno == ENOENT,
          "...once authenticated, ENOENT for no handle");
    close(plain);
    char *busid = drmGetBusid(master);
    CHECK(busid && strcmp(busid, "") == 0, "GET_UNIQUE: empty before SET_VERSION 1.1");
    drmFreeBusid(busid);
    drmSetVersion sv = {1, 4, -1, -1};
    busid = drmSetInterfaceVersion(master, &sv) == 0 ? drmGetBusid(master) : NULL;
    CHECK(busid && strcmp(busid, "chameleon") == 0 && sv.drm_di_major == 1 && sv.drm_di_minor == 4,
          "SET_VERSION 1.4 names the master 'chameleon' (%s)", busid ? busid : "none");
    drmFreeBusid(busid);

    /* Another process's buffer, after that process destroyed it and exited:
     * its dmabuf keeps it, as a GEM object, for PRIME and for EGL. */
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
        return 1;
    pid_t creator = fork();
    if (creator == 0) {
        int c = open(dev_path, O_RDWR | O_CLOEXEC);
        struct gbm_device *g = gbm_create_device(c);
        struct gbm_bo *b = g ? gbm_bo_create(g, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING) : NULL;
        int d = b ? gbm_bo_get_fd(b) : -1;
        struct cham_msg m = {0};
        int sent = d >= 0 && cham_send(pair[1], &m, d) == 0;
        gbm_bo_destroy(b);
        gbm_device_destroy(g);
        _exit(sent ? 0 : 1);
    }
    struct cham_msg cm;
    int theirs = -1;
    int got_fd = cham_recv(pair[0], &cm, &theirs) == 1 && theirs >= 0;
    waitpid(creator, NULL, 0);
    close(pair[0]);
    close(pair[1]);
    uint32_t th = 0;
    CHECK(got_fd && drmPrimeFDToHandle(rfd, theirs, &th) == 0 && th,
          "a buffer of a process that destroyed it and exited imports on the render node");
    void *(*ahb_acquire)(int) = (void *(*)(int))dlsym(RTLD_DEFAULT, "cham_buffer_ahb_acquire");
    void (*ahb_release)(void *) = (void (*)(void *))dlsym(RTLD_DEFAULT, "cham_buffer_ahb_release");
    void *their_ahb = ahb_acquire && got_fd ? ahb_acquire(theirs) : NULL;
    CHECK(their_ahb != NULL, "...and its AHardwareBuffer comes from chameleon-kernel (EGL import)");
    if (their_ahb)
        ahb_release(their_ahb);
    if (theirs >= 0)
        close(theirs);
    int not_dmabuf = memfd_create("not-a-dmabuf", MFD_CLOEXEC);
    errno = 0;
    CHECK(drmPrimeFDToHandle(rfd, not_dmabuf, &th) != 0 && errno == EINVAL, "an fd that is no dmabuf: EINVAL");
    close(not_dmabuf);

    int rinherited = fcntl(rfd, F_DUPFD, 10); /* not close-on-exec */
    snprintf(num_r, sizeof num_r, "%d", rinherited);
    CHECK(rinherited >= 0 && run_self("exec-render", num_r) == 0,
          "a render fd passed across exec is still the render node, not master");
    close(rinherited);
    close(rfd);
    close(master);

    printf("other processes (chameleon-kernel)\n");
    int fda = open(dev_path, O_RDWR | O_CLOEXEC);
    struct gbm_device *gbm2 = gbm_create_device(fda);
    struct gbm_bo *bo2 = gbm2 ? gbm_bo_create(gbm2, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT) : NULL;
    int dmabuf2 = bo2 ? gbm_bo_get_fd(bo2) : -1;
    uint32_t pitch = bo2 ? gbm_bo_get_stride(bo2) : 0;
    pid_t pid = fork();
    if (pid == 0) { /* the file and the dmabuf, inherited */
        uint32_t h = 0, fb = 0, handles[4] = {0}, pitches[4] = {pitch}, offsets[4] = {0};
        int ok = drmPrimeFDToHandle(fda, dmabuf2, &h) == 0 && h;
        handles[0] = h;
        ok = ok && drmModeAddFB2(fda, 64, 64, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb, 0) == 0;
        _exit(ok ? 0 : 1);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    CHECK(dmabuf2 >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "a child imports a dmabuf on the card file it inherited and adds a framebuffer");
    struct drm_mode_card_res bad = {.count_crtcs = 1, .crtc_id_ptr = 16};
    CHECK(drmIoctl(fda, DRM_IOCTL_MODE_GETRESOURCES, &bad) != 0 && errno == EFAULT,
          "a bad pointer gets EFAULT, as from the kernel");

    CHECK(drmDropMaster(fda) == 0, "drop master for a child to take");
    int ready[2];
    if (pipe(ready) != 0)
        return 1;
    pid = fork();
    if (pid == 0) {
        int c = open(dev_path, O_RDWR | O_CLOEXEC);
        char m = c >= 0 && drmIsMaster(c) ? 'm' : '-';
        if (write(ready[1], &m, 1) != 1)
            _exit(1);
        for (;;)
            pause();
    }
    char m = 0;
    if (read(ready[0], &m, 1) != 1)
        m = 0;
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    CHECK(m == 'm' && drmSetMaster(fda) == 0 && drmIsMaster(fda),
          "a child became master; once it is killed the master is free at once");
    close(ready[0]);
    close(ready[1]);

    pthread_t calls;
    pthread_create(&calls, NULL, calls_main, (void *)(intptr_t)fda);
    int forks_ok = 0;
    for (int i = 0; i < 100; i++) {
        pid = fork();
        if (pid == 0) {
            alarm(5); /* a deadlock ends as SIGALRM */
            drmModeRes *r = drmModeGetResources(fda);
            _exit(r && r->count_crtcs == 1 ? 0 : 1);
        }
        status = -1;
        waitpid(pid, &status, 0);
        forks_ok += WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    g_stop_calls = 1;
    pthread_join(calls, NULL);
    CHECK(forks_ok == 100, "fork while another thread is in card calls: the child's calls work (%d of 100)", forks_ok);

    CHECK(fcntl(fda, F_GETPIPE_SZ) == (int)sysconf(_SC_PAGESIZE),
          "a card file's event pipe holds one page, the kernel's 4 KiB of event space");
    int inherited = fcntl(fda, F_DUPFD, 10); /* not close-on-exec */
    char num[16];
    snprintf(num, sizeof num, "%d", inherited);
    CHECK(inherited >= 0 && run_self("exec-card", num) == 0, "a card fd passed across exec is still the card");
    close(inherited);
    CHECK(run_self("pipes", NULL) == 0, "a program using only ordinary pipes never contacts chameleon-kernel");

    /* Last: chameleon-kernel goes away (its pid from the card socket). */
    struct sockaddr_un kaddr = {.sun_family = AF_UNIX};
    snprintf(kaddr.sun_path, sizeof kaddr.sun_path, "%s/chameleon-card", getenv("XDG_RUNTIME_DIR"));
    int ks = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct ucred cred = {0};
    socklen_t clen = sizeof cred;
    int got = connect(ks, (struct sockaddr *)&kaddr, sizeof kaddr) == 0 &&
              getsockopt(ks, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0 && cred.pid > 0;
    close(ks);
    if (got)
        kill(cred.pid, SIGKILL);
    usleep(100000);
    struct pollfd pfd = {fda, POLLIN, 0};
    CHECK(got && drmModeGetResources(fda) == NULL && errno == ENODEV,
          "chameleon-kernel gone: the card answers ENODEV, as an unplugged device");
    CHECK(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLHUP), "...and its event pipe hangs up");
    close(dmabuf2);
    gbm_bo_destroy(bo2);
    gbm_device_destroy(gbm2);
    close(fda);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
