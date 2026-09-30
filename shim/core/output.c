/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * KWin's screen follows the Chameleon app's window.
 *
 * When the app's surface changes size (the phone rotates, or the keyboard
 * opens and the app shrinks the desktop above it), the presenter sends a new
 * CONFIG. KWin can't see that as a hotplug (no udev here), so the shim asks
 * KWin itself, the way System Settings does: over its own Wayland connection
 * (input.c) it binds kde_output_management_v2 and kde_output_device_registry_v2
 * (which hands out the kde_output_device_v2 objects), and
 *
 *   1. picks an existing mode of the output with the window's size, or else
 *   2. adds a custom mode of that size (set_custom_modes, replacing the one
 *      it added before), waits for KWin to list it, then picks it.
 *
 * KWin then does a normal modeset on the fake KMS device (kms.c accepts any
 * mode) and lays out panels and windows for the new size. Custom modes are
 * CVT modes, whose width is a multiple of 8, so the width is rounded up and
 * the app crops the few extra pixels (and output_visible() tells input.c).
 *
 * The display's refresh rate is followed the same way (battery saver
 * switching 120 Hz to 60 Hz, say): a mode of the window's size at that rate.
 *
 * Only a change of the window or the rate is acted on (and the first size
 * once KWin is up), so a mode picked by hand in System Settings stays until
 * they change again. CHAMELEON_RESIZE=0 turns this off (the app then scales).
 *
 * Everything here runs on input.c's thread.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "wlclient.h"

#include "output_proto.h"

/* ---- state ---- */

struct mode {
    struct wl_proxy *proxy;
    int32_t width, height, refresh_mhz;
    struct mode *next;
};

static struct {
    uint32_t width, height, refresh_mhz;
    unsigned serial;      /* bumped when the size changes */
} g_want;
static unsigned g_handled; /* g_want.serial is done (or given up) */
static uint64_t g_due_ns;  /* debounce: act once the size settles */
static int g_steps;        /* requests sent for g_want.serial */
static int g_disabled = -1;

static struct wl_display *g_display;
static struct wl_proxy *g_manager, *g_device_registry;
static uint32_t g_manager_version;
static struct {
    struct wl_proxy *proxy;
    struct mode *modes;
    struct mode *current;
    uint32_t capabilities;
    int done;
} g_dev;
static enum { IDLE, WAIT_CUSTOM, WAIT_MODE } g_step;
static struct wl_proxy *g_config;
static char g_failure[256];

/* ---- mode objects ---- */

static void mode_size(void *data, struct wl_proxy *p, int32_t w, int32_t h)
{
    (void)p;
    struct mode *m = data;
    m->width = w;
    m->height = h;
}

static void mode_refresh(void *data, struct wl_proxy *p, int32_t mhz)
{
    (void)p;
    ((struct mode *)data)->refresh_mhz = mhz;
}

static void mode_removed(void *data, struct wl_proxy *p)
{
    struct mode *m = data;
    for (struct mode **it = &g_dev.modes; *it; it = &(*it)->next) {
        if (*it == m) {
            *it = m->next;
            break;
        }
    }
    if (g_dev.current == m)
        g_dev.current = NULL;
    wl.proxy_destroy(p); /* no release request before version 26 */
    free(m);
}

static void (*k_mode_listener[])(void) = {
    [MODE_EV_SIZE] = (void (*)(void))mode_size,
    [MODE_EV_REFRESH] = (void (*)(void))mode_refresh,
    [MODE_EV_PREFERRED] = wl_ignore_event,
    [MODE_EV_REMOVED] = (void (*)(void))mode_removed,
    [MODE_EV_FLAGS] = wl_ignore_event,
};

/* ---- the output device ---- */

/* Called before the connection closes, so the proxies are still valid. */
static void forget_device(void)
{
    while (g_dev.modes) {
        struct mode *m = g_dev.modes;
        g_dev.modes = m->next;
        wl.proxy_destroy(m->proxy); /* no more events into freed memory */
        free(m);
    }
    if (g_dev.proxy)
        wl.proxy_destroy(g_dev.proxy);
    memset(&g_dev, 0, sizeof g_dev);
}

static void device_current_mode(void *data, struct wl_proxy *p, struct wl_proxy *mode)
{
    (void)data;
    (void)p;
    g_dev.current = mode ? wl.get_user_data(mode) : NULL;
}

static void device_mode(void *data, struct wl_proxy *p, struct wl_proxy *proxy)
{
    (void)data;
    (void)p;
    struct mode *m = calloc(1, sizeof *m);
    if (!m)
        return;
    m->proxy = proxy;
    m->next = g_dev.modes;
    g_dev.modes = m;
    wl.add_listener(proxy, k_mode_listener, m);
}

static void device_done(void *data, struct wl_proxy *p)
{
    (void)data;
    (void)p;
    g_dev.done = 1;
}

static void device_capabilities(void *data, struct wl_proxy *p, uint32_t flags)
{
    (void)data;
    (void)p;
    g_dev.capabilities = flags;
}

static void device_removed(void *data, struct wl_proxy *p)
{
    (void)data;
    cham_log("output: KWin removed its screen");
    g_dev.proxy = NULL;
    forget_device();
    wl.marshal_flags(p, OD_REQ_RELEASE, NULL, wl.get_version(p), WL_MARSHAL_FLAG_DESTROY);
    g_step = IDLE;
}

static void (*k_device_listener[OD_EV_COUNT])(void);

static void registry_output(void *data, struct wl_proxy *registry, struct wl_proxy *device)
{
    (void)data;
    (void)registry;
    if (g_dev.proxy)
        return; /* one screen; others (there are none) stay unconfigured */
    for (int i = 0; i < OD_EV_COUNT; i++)
        k_device_listener[i] = wl_ignore_event;
    k_device_listener[OD_EV_CURRENT_MODE] = (void (*)(void))device_current_mode;
    k_device_listener[OD_EV_MODE] = (void (*)(void))device_mode;
    k_device_listener[OD_EV_DONE] = (void (*)(void))device_done;
    k_device_listener[OD_EV_CAPABILITIES] = (void (*)(void))device_capabilities;
    k_device_listener[OD_EV_REMOVED] = (void (*)(void))device_removed;
    g_dev.proxy = device;
    wl.add_listener(device, k_device_listener, NULL);
}

static void (*k_registry_listener[])(void) = {
    [OD_REGISTRY_EV_FINISHED] = wl_ignore_event,
    [OD_REGISTRY_EV_OUTPUT] = (void (*)(void))registry_output,
};

/* ---- configurations ---- */

static void config_finish(void)
{
    if (g_config)
        wl.marshal_flags(g_config, OC_REQ_DESTROY, NULL, wl.get_version(g_config), WL_MARSHAL_FLAG_DESTROY);
    g_config = NULL;
}

static void config_applied(void *data, struct wl_proxy *p)
{
    (void)data;
    (void)p;
    config_finish();
    g_step = IDLE; /* output_tick() checks the result and goes on */
}

static void config_failed(void *data, struct wl_proxy *p)
{
    (void)data;
    (void)p;
    cham_log("output: KWin refused the change to %ux%u%s%s", g_want.width, g_want.height,
             g_failure[0] ? ": " : "", g_failure);
    config_finish();
    g_step = IDLE;
    g_handled = g_want.serial;
}

static void config_failure_reason(void *data, struct wl_proxy *p, const char *reason)
{
    (void)data;
    (void)p;
    snprintf(g_failure, sizeof g_failure, "%s", reason ? reason : "");
}

static void (*k_config_listener[])(void) = {
    [OC_EV_APPLIED] = (void (*)(void))config_applied,
    [OC_EV_FAILED] = (void (*)(void))config_failed,
    [OC_EV_FAILURE_REASON] = (void (*)(void))config_failure_reason,
};

static struct wl_proxy *config_begin(void)
{
    g_failure[0] = 0;
    g_config = wl.marshal_flags(g_manager, OM_REQ_CREATE_CONFIGURATION, &k_oc_iface, g_manager_version, 0, NULL);
    if (g_config)
        wl.add_listener(g_config, k_config_listener, NULL);
    return g_config;
}

/* ---- deciding ---- */

/* The app shows a mode 1:1 if it's the window's size, or up to 7 pixels
 * bigger (it crops those); see place() in presenter.cpp. */
static int fits(const struct mode *m)
{
    return m->width >= (int32_t)g_want.width && m->height >= (int32_t)g_want.height &&
           m->width - (int32_t)g_want.width < 8 && m->height - (int32_t)g_want.height < 8;
}

/* CVT modes land a little off the requested rate (119.93 Hz for 120). */
#define REFRESH_SLACK_MHZ 1000

static int refresh_ok(const struct mode *m)
{
    return !g_want.refresh_mhz || abs(m->refresh_mhz - (int32_t)g_want.refresh_mhz) <= REFRESH_SLACK_MHZ;
}

static const struct mode *best_fit(void)
{
    const struct mode *best = NULL;
    for (const struct mode *m = g_dev.modes; m; m = m->next) {
        if (!fits(m) || !refresh_ok(m))
            continue;
        if (!best) {
            best = m;
            continue;
        }
        int32_t extra = m->width - (int32_t)g_want.width + m->height - (int32_t)g_want.height;
        int32_t best_extra = best->width - (int32_t)g_want.width + best->height - (int32_t)g_want.height;
        int32_t dr = abs(m->refresh_mhz - (int32_t)g_want.refresh_mhz);
        int32_t best_dr = abs(best->refresh_mhz - (int32_t)g_want.refresh_mhz);
        if (extra < best_extra || (extra == best_extra && dr < best_dr))
            best = m;
    }
    return best;
}

static void decide(void)
{
    if (g_dev.current && fits(g_dev.current) && refresh_ok(g_dev.current)) {
        if (g_steps)
            cham_log("output: KWin's screen is now %dx%d @ %.2f Hz", g_dev.current->width, g_dev.current->height,
                     g_dev.current->refresh_mhz / 1000.0);
        g_handled = g_want.serial;
        return;
    }
    if (g_steps >= 3) {
        cham_log("output: could not switch KWin to %ux%u; the app scales instead", g_want.width, g_want.height);
        g_handled = g_want.serial;
        return;
    }
    g_steps++;

    const struct mode *m = best_fit();
    if (m) {
        cham_log("output: switching KWin's screen to %dx%d @ %.2f Hz", m->width, m->height, m->refresh_mhz / 1000.0);
        if (!config_begin()) {
            g_handled = g_want.serial;
            return;
        }
        wl.marshal_flags(g_config, OC_REQ_MODE, NULL, g_manager_version, 0, g_dev.proxy, m->proxy);
        wl.marshal_flags(g_config, OC_REQ_APPLY, NULL, g_manager_version, 0);
        g_step = WAIT_MODE;
        return;
    }
    if (g_manager_version < OC_SET_CUSTOM_MODES_SINCE || !(g_dev.capabilities & OD_CAPABILITY_CUSTOM_MODES)) {
        cham_log("output: KWin has no %ux%u mode and can't add one; the app scales instead", g_want.width,
                 g_want.height);
        g_handled = g_want.serial;
        return;
    }
    /* CVT (libxcvt) rounds the width down to a multiple of 8. */
    uint32_t w = (g_want.width + 7) & ~7u, h = g_want.height;
    uint32_t mhz = g_want.refresh_mhz ? g_want.refresh_mhz : 60000;
    cham_log("output: adding a %ux%u @ %.2f Hz mode to KWin's screen", w, h, mhz / 1000.0);
    struct wl_proxy *list =
        wl.marshal_flags(g_manager, OM_REQ_CREATE_MODE_LIST, &k_mode_list_iface, g_manager_version, 0, NULL);
    if (!list || !config_begin()) {
        g_handled = g_want.serial;
        return;
    }
    wl.marshal_flags(list, ML_REQ_SET_RESOLUTION, NULL, g_manager_version, 0, w, h);
    wl.marshal_flags(list, ML_REQ_SET_REFRESH_RATE, NULL, g_manager_version, 0, mhz);
    wl.marshal_flags(list, ML_REQ_ADD_MODE, NULL, g_manager_version, 0);
    wl.marshal_flags(g_config, OC_REQ_SET_CUSTOM_MODES, NULL, g_manager_version, 0, g_dev.proxy, list);
    wl.marshal_flags(g_config, OC_REQ_APPLY, NULL, g_manager_version, 0);
    wl.marshal_flags(list, ML_REQ_DESTROY, NULL, g_manager_version, WL_MARSHAL_FLAG_DESTROY);
    g_step = WAIT_CUSTOM;
}

static int ready(void)
{
    return g_display && g_manager && g_dev.proxy && g_dev.done && g_step == IDLE;
}

/* ---- entry points (input thread) ---- */

void output_config(uint32_t width, uint32_t height, uint32_t refresh_mhz)
{
    if (g_disabled < 0) {
        const char *env = getenv("CHAMELEON_RESIZE");
        g_disabled = env && strcmp(env, "0") == 0;
        if (g_disabled)
            cham_log("output: CHAMELEON_RESIZE=0, KWin's screen keeps its size");
    }
    if (g_disabled || !width || !height)
        return;
    /* The display's rate changes too (battery saver, adaptive refresh). */
    int rate_changed = abs((int32_t)refresh_mhz - (int32_t)g_want.refresh_mhz) > REFRESH_SLACK_MHZ;
    if (width == g_want.width && height == g_want.height && !rate_changed)
        return;
    g_want.width = width;
    g_want.height = height;
    g_want.refresh_mhz = refresh_mhz;
    g_want.serial++;
    g_steps = 0;
    /* Keyboards and rotations can report a few sizes in a row. */
    g_due_ns = now_ns() + 250000000ull;
}

void output_global(struct wl_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    if (strcmp(iface, "kde_output_management_v2") == 0 && !g_manager) {
        g_manager_version = version < OUTPUT_MANAGEMENT_VERSION ? version : OUTPUT_MANAGEMENT_VERSION;
        g_manager = wl_bind(registry, name, &k_om_iface, g_manager_version);
    } else if (strcmp(iface, "kde_output_device_registry_v2") == 0 && !g_device_registry) {
        if (version < OUTPUT_PROTO_VERSION) /* the registry exists from version 21 on */
            return;
        g_device_registry = wl_bind(registry, name, &k_od_registry_iface, OUTPUT_PROTO_VERSION);
        if (g_device_registry)
            wl.add_listener(g_device_registry, k_registry_listener, NULL);
    }
}

void output_connected(struct wl_display *display)
{
    g_display = display;
    if (!g_manager || !g_device_registry)
        cham_log("output: KWin offers no output management (v%u+); the app scales instead", OUTPUT_PROTO_VERSION);
}

void output_disconnected(void)
{
    forget_device();
    g_display = NULL;
    g_manager = g_device_registry = g_config = NULL;
    g_manager_version = 0;
    g_step = IDLE;
}

int output_pending(void)
{
    return g_want.serial != g_handled;
}

int output_timeout_ms(void)
{
    if (!output_pending() || !ready())
        return -1; /* KWin's events (or a new size) wake the thread */
    uint64_t now = now_ns();
    return now >= g_due_ns ? 0 : (int)((g_due_ns - now) / 1000000ull) + 1;
}

void output_tick(void)
{
    if (!output_pending() || !g_display)
        return;
    if (!g_manager || !g_device_registry) {
        g_handled = g_want.serial; /* nothing we can do */
        return;
    }
    if (ready() && now_ns() >= g_due_ns)
        decide();
}

void output_visible(double *fx, double *fy)
{
    *fx = *fy = 1.0;
    const struct mode *m = g_dev.current;
    if (!m || !g_want.width || !g_want.height || !fits(m))
        return;
    *fx = (double)g_want.width / m->width;
    *fy = (double)g_want.height / m->height;
}
