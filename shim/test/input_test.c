/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host test of the input path: a stub presenter sends CHAM_INPUT messages,
 * libchameleon.so (LD_PRELOADed) must turn them into org_kde_kwin_fake_input
 * requests on the Wayland socket this process serves - exactly as it does
 * inside kwin_wayland, whose socket it learns from bind().
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server.h>

#include "../../common/cham_io.h"

#include "check.h"

/* ---- fake-input server ---- */

static const struct wl_interface *k_null[8];
static const struct wl_message k_requests[] = {
    {"authenticate", "ss", k_null},     {"pointer_motion", "ff", k_null},
    {"button", "uu", k_null},           {"axis", "uf", k_null},
    {"touch_down", "2uff", k_null},     {"touch_motion", "2uff", k_null},
    {"touch_up", "2u", k_null},         {"touch_cancel", "2", k_null},
    {"touch_frame", "2", k_null},       {"pointer_motion_absolute", "3ff", k_null},
    {"keyboard_key", "4uu", k_null},    {"destroy", "5", k_null},
    {"keyboard_keysym", "6uu", k_null},
};
static const struct wl_interface k_fake_input = {"org_kde_kwin_fake_input", 6, 13, k_requests, 0, NULL};

static struct {
    int authenticated, same_pid;
    int touch_down, touch_id, touch_frames, touch_up;
    double touch_x, touch_y;
    double motion_dx, motion_dy;
    double abs_x, abs_y;
    int button, button_state;
    int axis;
    double axis_value;
    unsigned keysym, keysym_state;
    int events;
} g;

static void authenticate(struct wl_client *c, struct wl_resource *r, const char *app, const char *reason)
{
    (void)r;
    (void)reason;
    pid_t pid;
    wl_client_get_credentials(c, &pid, NULL, NULL);
    g.same_pid = pid == getpid();
    g.authenticated = app && strcmp(app, "Chameleon") == 0;
}
static void pointer_motion(struct wl_client *c, struct wl_resource *r, wl_fixed_t dx, wl_fixed_t dy)
{
    (void)c;
    (void)r;
    g.motion_dx = wl_fixed_to_double(dx);
    g.motion_dy = wl_fixed_to_double(dy);
    g.events++;
}
static void button(struct wl_client *c, struct wl_resource *r, uint32_t b, uint32_t s)
{
    (void)c;
    (void)r;
    g.button = (int)b;
    g.button_state = (int)s;
    g.events++;
}
static void axis(struct wl_client *c, struct wl_resource *r, uint32_t a, wl_fixed_t v)
{
    (void)c;
    (void)r;
    g.axis = (int)a;
    g.axis_value = wl_fixed_to_double(v);
    g.events++;
}
static void touch_down(struct wl_client *c, struct wl_resource *r, uint32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)c;
    (void)r;
    g.touch_down++;
    g.touch_id = (int)id;
    g.touch_x = wl_fixed_to_double(x);
    g.touch_y = wl_fixed_to_double(y);
    g.events++;
}
static void touch_motion(struct wl_client *c, struct wl_resource *r, uint32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)c;
    (void)r;
    (void)id;
    (void)x;
    (void)y;
}
static void touch_up(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    (void)c;
    (void)r;
    g.touch_up += id == (uint32_t)g.touch_id;
    g.events++;
}
static void touch_cancel(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    (void)r;
}
static void touch_frame(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    (void)r;
    g.touch_frames++;
    g.events++;
}
static void motion_absolute(struct wl_client *c, struct wl_resource *r, wl_fixed_t x, wl_fixed_t y)
{
    (void)c;
    (void)r;
    g.abs_x = wl_fixed_to_double(x);
    g.abs_y = wl_fixed_to_double(y);
    g.events++;
}
static void keyboard_key(struct wl_client *c, struct wl_resource *r, uint32_t k, uint32_t s)
{
    (void)c;
    (void)r;
    (void)k;
    (void)s;
}
static void destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static void keyboard_keysym(struct wl_client *c, struct wl_resource *r, uint32_t sym, uint32_t s)
{
    (void)c;
    (void)r;
    g.keysym = sym;
    g.keysym_state = s;
    g.events++;
}

static const struct {
    void *fns[13];
} k_impl = {{authenticate, pointer_motion, button, axis, touch_down, touch_motion, touch_up, touch_cancel, touch_frame,
             motion_absolute, keyboard_key, destroy, keyboard_keysym}};

static void bind_fake_input(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &k_fake_input, (int)version, id);
    wl_resource_set_implementation(r, &k_impl, NULL, NULL);
}

/* ---- a seat whose keyboard has a US layout ---- */

static void get_keyboard(struct wl_client *client, struct wl_resource *seat, uint32_t id)
{
    struct wl_resource *kb = wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(seat), id);
    void *lib = dlopen("libxkbcommon.so.0", RTLD_NOW);
    void *(*context_new)(int) = lib ? dlsym(lib, "xkb_context_new") : NULL;
    void *(*from_names)(void *, const void *, int) = lib ? dlsym(lib, "xkb_keymap_new_from_names") : NULL;
    char *(*as_string)(void *, int) = lib ? dlsym(lib, "xkb_keymap_get_as_string") : NULL;
    void *km = context_new && from_names ? from_names(context_new(0), NULL, 0) : NULL; /* evdev rules, us */
    char *text = km && as_string ? as_string(km, 1) : NULL;
    if (!text) {
        fprintf(stderr, "no libxkbcommon keymap\n");
        return;
    }
    int fd = memfd_create("keymap", MFD_CLOEXEC);
    size_t size = strlen(text) + 1;
    if (fd >= 0 && write(fd, text, size) == (ssize_t)size)
        wl_keyboard_send_keymap(kb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    if (fd >= 0)
        close(fd);
}

static void get_none(struct wl_client *client, struct wl_resource *seat, uint32_t id)
{
    (void)client;
    (void)seat;
    (void)id;
}

static const struct {
    void *fns[3];
} k_seat_impl = {{get_none, get_keyboard, get_none}};

static void bind_seat(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wl_seat_interface, (int)version, id);
    wl_resource_set_implementation(r, &k_seat_impl, NULL, NULL);
}

/* ---- stub presenter ---- */

static void send_input(int c, uint32_t kind, uint64_t a, uint64_t b)
{
    struct cham_msg m = {CHAM_INPUT, kind, a, b};
    cham_send(c, &m, -1);
}

int main(void)
{
    const char *dev_path = getenv("CHAMELEON_DRM_PATH");
    const char *sock_path = getenv("CHAMELEON_SOCKET");
    if (!dev_path || !sock_path || !getenv("XDG_RUNTIME_DIR")) {
        fprintf(stderr, "run via run-host-test.sh\n");
        return 2;
    }

    struct wl_display *display = wl_display_create();
    CHECK(wl_display_add_socket(display, "wayland-chameleon-test") == 0, "Wayland socket");
    wl_global_create(display, &k_fake_input, 6, NULL, bind_fake_input);
    wl_global_create(display, &wl_seat_interface, 1, NULL, bind_seat);
    struct wl_event_loop *loop = wl_display_get_event_loop(display);

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
    struct cham_msg cfg = {CHAM_CONFIG, 0, 1000ull | (500ull << 32), 60000};
    cham_send(c, &cfg, -1);

    send_input(c, CHAM_INPUT_TOUCH_DOWN, 3, CHAM_INPUT_POS(32768, 16384)); /* (0.5, 0.25) */
    send_input(c, CHAM_INPUT_TOUCH_FRAME, 0, 0);
    send_input(c, CHAM_INPUT_TOUCH_UP, 3, 0);
    send_input(c, CHAM_INPUT_TOUCH_FRAME, 0, 0);
    send_input(c, CHAM_INPUT_POINTER_MOTION, 0, CHAM_INPUT_DELTA(104858, -209715)); /* (+0.1, -0.2) */
    send_input(c, CHAM_INPUT_POINTER_ABS, 0, CHAM_INPUT_POS(16384, 32768));        /* (0.25, 0.5) */
    send_input(c, CHAM_INPUT_BUTTON, 0x110, 1);
    send_input(c, CHAM_INPUT_AXIS, 0, (uint64_t)(int64_t)(-15 * 256));
    send_input(c, CHAM_INPUT_KEYSYM, 0x010003bb, 1); /* U+03BB GREEK SMALL LETTER LAMDA */

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(display);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (g.events < 9 && now.tv_sec - start.tv_sec < 5);

    printf("fake input\n");
    CHECK(g.authenticated && g.same_pid, "shim authenticated from KWin's own process");
    CHECK(g.touch_down == 1 && g.touch_id == 3 && g.touch_x == 500 && g.touch_y == 125,
          "touch down id %d at %.1f,%.1f (want 3 at 500,125)", g.touch_id, g.touch_x, g.touch_y);
    CHECK(g.touch_up == 1 && g.touch_frames == 2, "touch up + frames (%d up, %d frames)", g.touch_up,
          g.touch_frames);
    CHECK(g.motion_dx > 99 && g.motion_dx < 101 && g.motion_dy > -101 && g.motion_dy < -99,
          "relative motion %.2f,%.2f (want 100,-100)", g.motion_dx, g.motion_dy);
    CHECK(g.abs_x == 250 && g.abs_y == 250, "absolute motion %.1f,%.1f (want 250,250)", g.abs_x, g.abs_y);
    CHECK(g.button == 0x110 && g.button_state == 1, "left button pressed");
    CHECK(g.axis == 0 && g.axis_value == -15, "vertical scroll %.2f (want -15)", g.axis_value);
    CHECK(g.keysym == 0x010003bb && g.keysym_state == 1, "unicode keysym 0x%x", g.keysym);

    /* Once something reads the keyboard, text goes there as the keys that
     * type it in the desktop's layout (a keysym the layout lacks, as above,
     * stays on fake input). */
    printf("typed text on the evdev keyboard\n");
    int kfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    CHECK(kfd >= 0, "keyboard opened");
    for (int i = 0; i < 10; i++) { /* the reader registers with the shim */
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(display);
    }
    send_input(c, CHAM_INPUT_KEYSYM, 'A', 1);
    send_input(c, CHAM_INPUT_KEYSYM, 'A', 0);
    char got[64] = "";
    struct input_event ev;
    for (int i = 0; i < 50 && strlen(got) < sizeof got - 8; i++) {
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(display);
        while (kfd >= 0 && read(kfd, &ev, sizeof ev) == (ssize_t)sizeof ev)
            if (ev.type == EV_KEY)
                snprintf(got + strlen(got), sizeof got - strlen(got), "%d=%d ", ev.code, ev.value);
    }
    CHECK(strcmp(got, "42=1 30=1 30=0 42=0 ") == 0, "'A' typed as Shift+A (%s)", got);
    CHECK(g.keysym == 0x010003bb, "...not through fake input (last keysym 0x%x)", g.keysym);
    if (kfd >= 0)
        close(kfd);

    close(c);
    close(fd);
    wl_display_destroy(display);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
