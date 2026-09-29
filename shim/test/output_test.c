/*
 * Host test of output.c: a stub presenter reports window sizes, and
 * libchameleon.so (LD_PRELOADed) must switch "KWin's" screen to match over
 * kde_output_management_v2, as it does inside kwin_wayland. This process
 * plays KWin: fake input (so the shim connects), one output device with a
 * native 1000x500 mode, and custom modes generated like KWin's CVT modes
 * (width rounded down to a multiple of 8).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server.h>

#include "../../common/cham_io.h"

static const struct wl_interface *k_null[8];
#include "../core/output_proto.h"

static int failures;
#define CHECK(cond, ...)                          \
    do {                                          \
        int ok_ = (cond);                         \
        printf("  %s  ", ok_ ? "ok  " : "FAIL"); \
        printf(__VA_ARGS__);                      \
        printf("\n");                             \
        failures += !ok_;                         \
    } while (0)

/* ---- fake input: only so the shim's connection is accepted ---- */

static const struct wl_message k_fi_requests[] = {
    {"authenticate", "ss", k_null},
};
static const struct wl_interface k_fake_input = {"org_kde_kwin_fake_input", 6, 1, k_fi_requests, 0, NULL};

static void fi_authenticate(struct wl_client *c, struct wl_resource *r, const char *app, const char *reason)
{
    (void)c;
    (void)r;
    (void)app;
    (void)reason;
}
static const struct {
    void *fns[1];
} k_fi_impl = {{fi_authenticate}};

static void bind_fake_input(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &k_fake_input, (int)version, id);
    wl_resource_set_implementation(r, &k_fi_impl, NULL, NULL);
}

/* ---- output device ---- */

struct smode {
    int w, h, mhz, custom, removed;
    struct wl_resource *res;
};

#define MAX_MODES 16
static struct {
    struct smode modes[MAX_MODES];
    int count;
    struct smode *current;
    struct wl_resource *device;
    struct wl_client *client;
    /* what the shim asked for */
    int configs, applied, custom_sets, mode_sets;
    int custom_w, custom_h, custom_mhz;
    /* pending, per configuration */
    struct smode *pending_mode;
    int pending_custom, pending_w, pending_h, pending_mhz;
} g;

static struct smode *add_mode(int w, int h, int mhz, int custom)
{
    struct smode *m = &g.modes[g.count++];
    *m = (struct smode){w, h, mhz, custom, 0, NULL};
    return m;
}

static void send_mode(struct smode *m)
{
    m->res = wl_resource_create(g.client, &k_od_mode_iface, wl_resource_get_version(g.device), 0);
    wl_resource_set_user_data(m->res, m);
    wl_resource_post_event(g.device, OD_EV_MODE, m->res);
    wl_resource_post_event(m->res, MODE_EV_SIZE, m->w, m->h);
    wl_resource_post_event(m->res, MODE_EV_REFRESH, m->mhz);
}

static void send_current_and_done(void)
{
    wl_resource_post_event(g.device, OD_EV_CURRENT_MODE, g.current->res);
    wl_resource_post_event(g.device, OD_EV_DONE);
}

static void device_release(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static const struct {
    void *fns[1];
} k_device_impl = {{device_release}};

static void registry_stop(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_post_event(r, OD_REGISTRY_EV_FINISHED);
    wl_resource_destroy(r);
}
static const struct {
    void *fns[1];
} k_registry_impl = {{registry_stop}};

static void bind_device_registry(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *reg = wl_resource_create(client, &k_od_registry_iface, (int)version, id);
    wl_resource_set_implementation(reg, &k_registry_impl, NULL, NULL);
    g.client = client;
    g.device = wl_resource_create(client, &k_od_iface, (int)version, 0);
    wl_resource_set_implementation(g.device, &k_device_impl, NULL, NULL);
    wl_resource_post_event(reg, OD_REGISTRY_EV_OUTPUT, g.device);
    wl_resource_post_event(g.device, OD_EV_CAPABILITIES, OD_CAPABILITY_CUSTOM_MODES);
    for (int i = 0; i < g.count; i++)
        send_mode(&g.modes[i]);
    send_current_and_done();
}

/* ---- output management ---- */

static void ml_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static void ml_add_mode(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    (void)r;
    g.pending_custom++;
}
static void ml_set_resolution(struct wl_client *c, struct wl_resource *r, uint32_t w, uint32_t h)
{
    (void)c;
    (void)r;
    g.pending_w = (int)w;
    g.pending_h = (int)h;
}
static void ml_set_refresh_rate(struct wl_client *c, struct wl_resource *r, uint32_t mhz)
{
    (void)c;
    (void)r;
    g.pending_mhz = (int)mhz;
}
static void ml_set_reduced_blanking(struct wl_client *c, struct wl_resource *r, uint32_t v)
{
    (void)c;
    (void)r;
    (void)v;
}
static const struct {
    void *fns[5];
} k_ml_impl = {{ml_destroy, ml_add_mode, ml_set_resolution, ml_set_refresh_rate, ml_set_reduced_blanking}};

static void oc_mode(struct wl_client *c, struct wl_resource *r, struct wl_resource *dev, struct wl_resource *mode)
{
    (void)c;
    (void)r;
    (void)dev;
    g.pending_mode = wl_resource_get_user_data(mode);
    g.mode_sets++;
}
static void oc_set_custom_modes(struct wl_client *c, struct wl_resource *r, struct wl_resource *dev,
                                struct wl_resource *list)
{
    (void)c;
    (void)r;
    (void)dev;
    (void)list;
    g.custom_sets++;
    g.custom_w = g.pending_w;
    g.custom_h = g.pending_h;
    g.custom_mhz = g.pending_mhz;
}
static void oc_apply(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    g.applied++;
    if (g.pending_custom) {
        /* Like KWin: the old custom modes go, the current one stays until
         * replaced; CVT widths are multiples of 8. */
        for (int i = 0; i < g.count; i++) {
            struct smode *m = &g.modes[i];
            if (m->custom && !m->removed && m != g.current) {
                m->removed = 1;
                wl_resource_post_event(m->res, MODE_EV_REMOVED);
            }
        }
        if (g.count < MAX_MODES)
            send_mode(add_mode(g.custom_w - g.custom_w % 8, g.custom_h, g.custom_mhz - 50, 1));
    }
    if (g.pending_mode && !g.pending_mode->removed)
        g.current = g.pending_mode;
    send_current_and_done();
    wl_resource_post_event(r, OC_EV_APPLIED);
    g.pending_mode = NULL;
    g.pending_custom = 0;
}
static void oc_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static void *k_oc_impl[OC_REQ_COUNT] = {
    [OC_REQ_MODE] = oc_mode,
    [OC_REQ_APPLY] = oc_apply,
    [OC_REQ_DESTROY] = oc_destroy,
    [OC_REQ_SET_CUSTOM_MODES] = oc_set_custom_modes,
};

static void om_create_configuration(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *config = wl_resource_create(c, &k_oc_iface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(config, k_oc_impl, NULL, NULL);
    g.configs++;
}
static void om_create_mode_list(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *list = wl_resource_create(c, &k_mode_list_iface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(list, &k_ml_impl, NULL, NULL);
}
static const struct {
    void *fns[2];
} k_om_impl = {{om_create_configuration, om_create_mode_list}};

static void bind_management(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &k_om_iface, (int)version, id);
    wl_resource_set_implementation(r, &k_om_impl, NULL, NULL);
}

/* ---- test driver ---- */

static struct wl_display *g_display;

static void run_for(int ms)
{
    struct wl_event_loop *loop = wl_display_get_event_loop(g_display);
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        wl_event_loop_dispatch(loop, 10);
        wl_display_flush_clients(g_display);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 < ms);
}

static void send_size(int c, uint32_t w, uint32_t h)
{
    struct cham_msg m = {CHAM_CONFIG, 0, w | ((uint64_t)h << 32), 120000};
    cham_send(c, &m, -1);
}

static int current_is(int w, int h)
{
    return g.current && g.current->w == w && g.current->h == h;
}

int main(void)
{
    const char *dev_path = getenv("CHAMELEON_DRM_PATH");
    const char *sock_path = getenv("CHAMELEON_SOCKET");
    if (!dev_path || !sock_path || !getenv("XDG_RUNTIME_DIR")) {
        fprintf(stderr, "run via run-host-test.sh\n");
        return 2;
    }

    g_display = wl_display_create();
    CHECK(wl_display_add_socket(g_display, "wayland-chameleon-out") == 0, "Wayland socket");
    wl_global_create(g_display, &k_fake_input, 6, NULL, bind_fake_input);
    wl_global_create(g_display, &k_om_iface, 21, NULL, bind_management);
    wl_global_create(g_display, &k_od_registry_iface, 21, NULL, bind_device_registry);
    g.current = add_mode(1000, 500, 120000, 0);

    int l = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, sock_path, sizeof addr.sun_path - 1);
    unlink(sock_path);
    bind(l, (struct sockaddr *)&addr, sizeof addr);
    listen(l, 1);

    int fd = open(dev_path, O_RDWR | O_CLOEXEC); /* starts the shim's presenter link */
    CHECK(fd >= 0, "open fake DRM device");
    struct pollfd p = {l, POLLIN, 0};
    CHECK(poll(&p, 1, 5000) == 1, "shim connects to the presenter");
    int c = accept(l, NULL, NULL);

    printf("screen follows the window\n");
    send_size(c, 1000, 500);
    run_for(800);
    CHECK(g.configs == 0, "same size as KWin's screen: nothing to do (%d configurations)", g.configs);

    send_size(c, 500, 1000); /* rotated */
    run_for(800);
    CHECK(g.custom_sets == 1 && g.custom_w == 504 && g.custom_h == 1000 && g.custom_mhz == 120000,
          "rotation adds a custom mode %dx%d @ %d mHz (want 504x1000, width 8-aligned)", g.custom_w, g.custom_h,
          g.custom_mhz);
    CHECK(current_is(504, 1000) && g.mode_sets == 1, "...and switches to it (screen %dx%d)",
          g.current ? g.current->w : 0, g.current ? g.current->h : 0);

    send_size(c, 1000, 500); /* back */
    run_for(800);
    CHECK(current_is(1000, 500) && g.custom_sets == 1 && g.mode_sets == 2,
          "rotating back picks the existing native mode (%d custom mode lists)", g.custom_sets);

    send_size(c, 700, 400); /* keyboard animating: only the last size counts */
    run_for(50);
    send_size(c, 800, 400);
    run_for(800);
    CHECK(g.custom_sets == 2 && g.custom_w == 800 && current_is(800, 400),
          "sizes in quick succession: only the last one is applied (%dx%d)", g.custom_w, g.custom_h);
    int custom_left = 0;
    for (int i = 0; i < g.count; i++)
        custom_left += g.modes[i].custom && !g.modes[i].removed;
    CHECK(custom_left == 1, "the previous custom mode was replaced (%d left)", custom_left);
    CHECK(g.applied == g.configs, "every configuration was applied (%d/%d)", g.applied, g.configs);

    close(c);
    close(fd);
    wl_display_destroy(g_display);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
