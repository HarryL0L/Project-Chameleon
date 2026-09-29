/*
 * Desktop test for the fake KMS device, using the *real* libdrm the way
 * KWin 6.7's DRM backend does. Run through shim/test/run-host-test.sh, which
 * builds libchameleon.so with CHAM_HOST_TEST (memfd instead of
 * AHardwareBuffer) and LD_PRELOADs it.
 *
 * A stub presenter thread plays the Chameleon app: it reports a
 * 1080x1800 @ 120 Hz surface and answers each PRESENT with RELEASE and a
 * FRAME_DONE 8.3 ms later.
 */
#define _GNU_SOURCE
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
    while (!g_stop_presenter) {
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
        } else if (m.type == CHAM_PRESENT) {
            g_presents++;
            usleep(8333);
            struct cham_msg rel = {CHAM_RELEASE, (uint32_t)m.id, 0, 0};
            cham_send(c, &rel, -1); /* copy mode: released right after the "blit" */
            (void)displayed;
            displayed = m.id;
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

int main(void)
{
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
    drmDevicePtr dev = NULL;
    CHECK(drmGetDevice2(fd, 0, &dev) == 0 && dev && dev->bustype == DRM_BUS_PLATFORM &&
              strcmp(dev->nodes[DRM_NODE_PRIMARY], dev_path) == 0,
          "drmGetDevice2: platform device at the fake path");
    drmFreeDevice(&dev);
    drm_magic_t magic;
    CHECK(drmGetMagic(fd, &magic) == 0 && drmAuthMagic(fd, magic) == 0, "magic / auth (RenderDevice::open)");
    CHECK(drmIsMaster(fd), "drmIsMaster");

    printf("objects\n");
    drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
    CHECK(pres && pres->count_planes == 1, "one plane");
    uint32_t plane_id = pres ? pres->planes[0] : 0;
    drmModeFreePlaneResources(pres);
    drmModePlane *plane = drmModeGetPlane(fd, plane_id);
    CHECK(plane && plane->count_formats == 2 && plane->possible_crtcs == 1, "plane: 2 formats, crtc mask 1");
    drmModeFreePlane(plane);
    uint64_t type = 99;
    uint32_t p_type = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
    drmModePropertyRes *tp = drmModeGetProperty(fd, p_type);
    CHECK(tp && tp->count_enums == 3 && strcmp(tp->enums[type].name, "Primary") == 0, "plane type enum = Primary");
    drmModeFreeProperty(tp);

    drmModeRes *res = drmModeGetResources(fd);
    CHECK(res && res->count_crtcs == 1 && res->count_connectors == 1 && res->count_encoders == 1,
          "resources: 1 crtc / connector / encoder");
    uint32_t crtc_id = res->crtcs[0], conn_id = res->connectors[0];
    drmModeConnector *conn = drmModeGetConnector(fd, conn_id);
    CHECK(conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes == 1, "connector connected, 1 mode");
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
    int foreign = memfd_create("not-ours", 0);
    uint32_t h;
    CHECK(drmPrimeFDToHandle(fd, foreign, &h) != 0, "foreign dmabuf rejected (no direct scanout)");
    close(foreign);

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

    printf("page flips (presenter at 120 Hz)\n");
    int ok = 1, busy_checked = 0;
    uint64_t t0 = now_ns();
    for (int i = 1; i <= 60; i++) {
        req = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(req, plane_id, p_fb, fbs[i % 3]);
        drmModeAtomicAddProperty(req, plane_id, p_fence, (uint64_t)-1);
        if (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0)
            ok = 0;
        if (!busy_checked) {
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

    printf("second producer takes over\n");
    int intruder = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, g_sock_path, sizeof addr.sun_path - 1);
    connect(intruder, (struct sockaddr *)&addr, sizeof addr);
    usleep(2500000);
    CHECK(g_accepts == 2, "replaced shim does not reconnect (%d connections, want 2)", g_accepts);
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

    printf("teardown\n");
    struct drm_mode_closefb cfb = {.fb_id = fbs[0]};
    CHECK(drmIoctl(fd, DRM_IOCTL_MODE_CLOSEFB, &cfb) == 0, "CLOSEFB of the fb on screen");
    for (int i = 1; i < 3; i++)
        drmModeRmFB(fd, fbs[i]);
    for (int i = 0; i < 3; i++)
        gbm_bo_destroy(bos[i]);
    gbm_device_destroy(gbm);
    CHECK(drmModeDestroyPropertyBlob(fd, blob) == 0, "destroy mode blob");
    close(fd);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
