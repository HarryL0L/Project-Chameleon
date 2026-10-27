/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host test of the typed-text path: a stub presenter sends the Android
 * keyboard's text, and libchameleon.so (LD_PRELOADed) must type it on the
 * evdev keyboard through the layout of the Wayland socket this process
 * serves (as inside kwin_wayland, whose socket it learns from bind()), or
 * hand it to chameleon-ime while a text field takes text.
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
#include <sys/wait.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server.h>

#include "../../common/cham_io.h"

#include "check.h"

static const struct wl_interface *k_null[8];

/* ---- a seat whose keyboard has a US layout (or another, later) ---- */

static struct wl_resource *g_keyboard;

/* Sends the keyboard the keymap of `layout` (evdev rules), as a compositor
 * does on bind and when the user changes the layout. */
static void send_keymap(const char *layout)
{
    void *lib = dlopen("libxkbcommon.so.0", RTLD_NOW);
    void *(*context_new)(int) = lib ? dlsym(lib, "xkb_context_new") : NULL;
    void *(*from_names)(void *, const void *, int) = lib ? dlsym(lib, "xkb_keymap_new_from_names") : NULL;
    char *(*as_string)(void *, int) = lib ? dlsym(lib, "xkb_keymap_get_as_string") : NULL;
    const char *names[5] = {"evdev", "pc105", layout, "", ""}; /* struct xkb_rule_names */
    void *km = context_new && from_names ? from_names(context_new(0), names, 0) : NULL;
    char *text = km && as_string ? as_string(km, 1) : NULL;
    if (!text || !g_keyboard) {
        fprintf(stderr, "no libxkbcommon keymap\n");
        return;
    }
    int fd = memfd_create("keymap", MFD_CLOEXEC);
    size_t size = strlen(text) + 1;
    if (fd >= 0 && write(fd, text, size) == (ssize_t)size)
        wl_keyboard_send_keymap(g_keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    if (fd >= 0)
        close(fd);
}

static void get_keyboard(struct wl_client *client, struct wl_resource *seat, uint32_t id)
{
    g_keyboard = wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(seat), id);
    send_keymap("us");
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

/* ---- an input method global, for chameleon-ime ---- */

static const struct wl_interface k_context;
static const struct wl_interface *k_context_types[] = {&k_context};
static const struct wl_message k_im_events[] = {{"activate", "n", k_context_types}, {"deactivate", "o", k_context_types}};
static const struct wl_interface k_im = {"zwp_input_method_v1", 1, 0, NULL, 2, k_im_events};
static const struct wl_message k_context_requests[] = {
    {"destroy", "", k_null},           {"commit_string", "us", k_null},  {"preedit_string", "uss", k_null},
    {"preedit_styling", "uuu", k_null}, {"preedit_cursor", "i", k_null}, {"delete_surrounding_text", "iu", k_null},
    {"cursor_position", "ii", k_null}, {"modifiers_map", "a", k_null},   {"keysym", "uuuuu", k_null},
    {"grab_keyboard", "n", k_null},    {"key", "uuuu", k_null},          {"modifiers", "uuuuu", k_null},
    {"language", "us", k_null},        {"text_direction", "uu", k_null},
};
static const struct wl_message k_context_events[] = {
    {"surrounding_text", "suu", k_null}, {"reset", "", k_null},       {"content_type", "uu", k_null},
    {"invoke_button", "uu", k_null},     {"commit_state", "u", k_null}, {"preferred_language", "s", k_null},
};
static const struct wl_interface k_context = {"zwp_input_method_context_v1", 1, 14, k_context_requests, 6,
                                              k_context_events};

static struct {
    struct wl_resource *im;
    char commit[64], preedit[64];
    int delete_index;
    unsigned delete_length;
} ime;

static void ctx_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void ctx_commit_string(struct wl_client *c, struct wl_resource *r, uint32_t serial, const char *text)
{
    (void)c;
    (void)r;
    (void)serial;
    snprintf(ime.commit, sizeof ime.commit, "%s", text);
}

static void ctx_preedit_string(struct wl_client *c, struct wl_resource *r, uint32_t serial, const char *text,
                               const char *commit)
{
    (void)c;
    (void)r;
    (void)serial;
    (void)commit;
    snprintf(ime.preedit, sizeof ime.preedit, "%s", text);
}

static void ctx_delete(struct wl_client *c, struct wl_resource *r, int32_t index, uint32_t length)
{
    (void)c;
    (void)r;
    ime.delete_index = index;
    ime.delete_length = length;
}

static void ctx_other(void)
{
}

static const struct {
    void *fns[14];
} k_context_impl = {{ctx_destroy, ctx_commit_string, ctx_preedit_string, ctx_other, ctx_other, ctx_delete, ctx_other,
                     ctx_other, ctx_other, ctx_other, ctx_other, ctx_other, ctx_other, ctx_other}};

static void bind_im(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    ime.im = wl_resource_create(client, &k_im, (int)version, id);
}

/* ---- ext_data_control_v1, the clipboard protocol KWin offers ---- */

static const struct wl_interface k_dc_source, k_dc_device, k_dc_offer;
static const struct wl_interface *k_dc_source_types[] = {&k_dc_source};
static const struct wl_interface *k_dc_device_types[] = {&k_dc_device, &wl_seat_interface};
static const struct wl_interface *k_dc_offer_types[] = {&k_dc_offer};
static const struct wl_message k_dc_manager_requests[] = {
    {"create_data_source", "n", k_dc_source_types}, {"get_data_device", "no", k_dc_device_types}, {"destroy", "", k_null}};
static const struct wl_interface k_dc_manager = {"ext_data_control_manager_v1", 1, 3, k_dc_manager_requests, 0, NULL};
static const struct wl_message k_dc_device_requests[] = {
    {"set_selection", "?o", k_dc_source_types}, {"destroy", "", k_null}, {"set_primary_selection", "?o", k_dc_source_types}};
static const struct wl_message k_dc_device_events[] = {{"data_offer", "n", k_dc_offer_types},
                                                       {"selection", "?o", k_dc_offer_types},
                                                       {"finished", "", k_null},
                                                       {"primary_selection", "?o", k_dc_offer_types}};
static const struct wl_interface k_dc_device = {"ext_data_control_device_v1", 1, 3, k_dc_device_requests, 4,
                                                k_dc_device_events};
static const struct wl_message k_dc_source_requests[] = {{"offer", "s", k_null}, {"destroy", "", k_null}};
static const struct wl_message k_dc_source_events[] = {{"send", "sh", k_null}, {"cancelled", "", k_null}};
static const struct wl_interface k_dc_source = {"ext_data_control_source_v1", 1, 2, k_dc_source_requests, 2,
                                                k_dc_source_events};
static const struct wl_message k_dc_offer_requests[] = {{"receive", "sh", k_null}, {"destroy", "", k_null}};
static const struct wl_message k_dc_offer_events[] = {{"offer", "s", k_null}};
static const struct wl_interface k_dc_offer = {"ext_data_control_offer_v1", 1, 2, k_dc_offer_requests, 1,
                                               k_dc_offer_events};

static struct {
    struct wl_resource *device, *selection; /* the shim's device, and the source it made the selection */
    char offered[256];                      /* the types that source offers */
    int received;                           /* times the shim asked an offer of ours for its text */
    const char *initial;                    /* the clipboard's text when a device is made, or NULL */
} dc;

static void dc_copy(const char *text, const char *mimes);

static void dc_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void dc_source_offer(struct wl_client *c, struct wl_resource *r, const char *mime)
{
    (void)c;
    (void)r;
    snprintf(dc.offered + strlen(dc.offered), sizeof dc.offered - strlen(dc.offered), "%s ", mime);
}

static const struct {
    void *fns[2];
} k_dc_source_impl = {{dc_source_offer, dc_destroy}};

static void dc_set_selection(struct wl_client *c, struct wl_resource *r, struct wl_resource *source)
{
    (void)c;
    (void)r;
    dc.selection = source;
}

static void dc_set_primary(struct wl_client *c, struct wl_resource *r, struct wl_resource *source)
{
    (void)c;
    (void)r;
    (void)source;
}

static const struct {
    void *fns[3];
} k_dc_device_impl = {{dc_set_selection, dc_destroy, dc_set_primary}};

static void dc_create_source(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *s = wl_resource_create(c, &k_dc_source, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s, &k_dc_source_impl, NULL, NULL);
}

static void dc_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat)
{
    (void)seat;
    dc.device = wl_resource_create(c, &k_dc_device, wl_resource_get_version(r), id);
    wl_resource_set_implementation(dc.device, &k_dc_device_impl, NULL, NULL);
    if (dc.initial) /* the first selection, sent upon binding */
        dc_copy(dc.initial, "text/plain");
    else
        wl_resource_post_event(dc.device, 1 /* selection */, NULL);
}

static const struct {
    void *fns[3];
} k_dc_manager_impl = {{dc_create_source, dc_get_device, dc_destroy}};

static void bind_dc(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &k_dc_manager, (int)version, id);
    wl_resource_set_implementation(r, &k_dc_manager_impl, NULL, NULL);
}

/* An app's copied text: the offer writes `text` when the shim receives it. */
static void dc_offer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd)
{
    (void)c;
    (void)mime;
    const char *text = wl_resource_get_user_data(r);
    if (write(fd, text, strlen(text)) < 0)
        perror("write");
    close(fd);
    dc.received++;
}

static const struct {
    void *fns[2];
} k_dc_offer_impl = {{dc_offer_receive, dc_destroy}};

/* An app copies `text`, offered as the types in `mimes` (space-separated). */
static void dc_copy(const char *text, const char *mimes)
{
    struct wl_resource *o = wl_resource_create(wl_resource_get_client(dc.device), &k_dc_offer, 1, 0);
    wl_resource_set_implementation(o, &k_dc_offer_impl, (void *)text, NULL);
    wl_resource_post_event(dc.device, 0 /* data_offer */, o);
    char list[128];
    snprintf(list, sizeof list, "%s", mimes);
    for (char *m = strtok(list, " "); m; m = strtok(NULL, " "))
        wl_resource_post_event(o, 0 /* offer */, m);
    wl_resource_post_event(dc.device, 1 /* selection */, o);
}

/* ---- stub presenter ---- */

static void send_input(int c, uint32_t kind, uint64_t a, uint64_t b)
{
    struct cham_msg m = {CHAM_INPUT, kind, a, b};
    cham_send(c, &m, -1);
}

static struct wl_display *g_display;

static void pump(int times)
{
    for (int i = 0; i < times; i++) {
        wl_event_loop_dispatch(wl_display_get_event_loop(g_display), 20);
        wl_display_flush_clients(g_display);
    }
}

/* The next message of `type` from the shim within ~1 s (others are skipped),
 * keeping the compositor running meanwhile; its fd, or -1. */
static int wait_msg(int c, uint32_t type, struct cham_msg *out)
{
    for (int i = 0; i < 50; i++) {
        struct pollfd p = {c, POLLIN, 0};
        while (poll(&p, 1, 0) == 1) {
            struct cham_msg m;
            int fd = -1;
            if (cham_recv(c, &m, &fd) <= 0)
                return -1;
            if (m.type == type) {
                *out = m;
                return fd;
            }
            if (fd >= 0)
                close(fd);
        }
        pump(1);
    }
    return -1;
}

static void read_all(int fd, char *buf, size_t size)
{
    size_t n = 0;
    ssize_t r;
    while (n < size - 1 && (r = read(fd, buf + n, size - 1 - n)) > 0)
        n += (size_t)r;
    buf[n] = 0;
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
    g_display = display;
    CHECK(wl_display_add_socket(display, "wayland-chameleon-test") == 0, "Wayland socket");
    wl_global_create(display, &wl_seat_interface, 1, NULL, bind_seat);
    wl_global_create(display, &k_dc_manager, 1, NULL, bind_dc);
    struct wl_event_loop *loop = wl_display_get_event_loop(display);

    int l = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, sock_path, sizeof addr.sun_path - 1);
    unlink(sock_path);
    bind(l, (struct sockaddr *)&addr, sizeof addr);
    listen(l, 1);

    int fd = open(dev_path, O_RDWR | O_CLOEXEC); /* starts the shim's presenter link */
    CHECK(fd >= 0, "open fake DRM device");
    /* Another program binding a Wayland socket (Firefox's proxy, a nested
     * compositor) is neither the DRM master nor its ancestor: the input
     * thread must still connect to ours. */
    int proxy_go[2], proxy_bound[2];
    if (pipe(proxy_go) != 0 || pipe(proxy_bound) != 0)
        return 1;
    pid_t proxy = fork();
    if (proxy == 0) {
        struct sockaddr_un pa = {.sun_family = AF_UNIX};
        snprintf(pa.sun_path, sizeof pa.sun_path, "%s/wayland-proxy-1", getenv("XDG_RUNTIME_DIR"));
        int s = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        unlink(pa.sun_path);
        if (s < 0 || bind(s, (struct sockaddr *)&pa, sizeof pa) != 0 || listen(s, 4) != 0)
            _exit(2);
        char ch = 0;
        if (write(proxy_bound[1], &ch, 1) != 1 || read(proxy_go[0], &ch, 1) != 1) /* the parent has typed */
            _exit(2);
        _exit(accept(s, NULL, NULL) >= 0); /* 1: the input thread came here */
    }
    char ch;
    CHECK(read(proxy_bound[0], &ch, 1) == 1, "another program binds wayland-proxy-1");
    usleep(200 * 1000); /* chameleon-kernel has heard of it */
    struct pollfd p = {l, POLLIN, 0};
    CHECK(poll(&p, 1, 5000) == 1, "shim connects to the presenter");
    int c = accept(l, NULL, NULL);
    struct cham_msg cfg = {CHAM_CONFIG, 0, 1000ull | (500ull << 32), 60000};
    cham_send(c, &cfg, -1);

    /* Text goes to the keyboard as the keys that type it in the desktop's
     * layout; a character the layout lacks is not typed. */
    printf("typed text on the evdev keyboard\n");
    int kfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    CHECK(kfd >= 0, "keyboard opened");
    for (int i = 0; i < 10; i++) { /* the reader registers, the shim gets the keymap */
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(display);
    }
    send_input(c, CHAM_INPUT_KEYSYM, 0x010003bb, 1); /* U+03BB GREEK SMALL LETTER LAMDA */
    send_input(c, CHAM_INPUT_KEYSYM, 0x010003bb, 0);
    send_input(c, CHAM_INPUT_KEYSYM, 'A', 1);
    send_input(c, CHAM_INPUT_KEYSYM, 'A', 0);
    send_input(c, CHAM_INPUT_KEYSYM, '$', 1);
    send_input(c, CHAM_INPUT_KEYSYM, '$', 0);
    char got[96] = "";
    struct input_event ev;
    for (int i = 0; i < 50 && strlen(got) < sizeof got - 8; i++) {
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(display);
        while (kfd >= 0 && read(kfd, &ev, sizeof ev) == (ssize_t)sizeof ev)
            if (ev.type == EV_KEY)
                snprintf(got + strlen(got), sizeof got - strlen(got), "%d=%d ", ev.code, ev.value);
    }
    CHECK(strcmp(got, "42=1 30=1 30=0 42=0 42=1 5=1 5=0 42=0 ") == 0,
          "'A' typed as Shift+A, '$' as Shift+4 (not KEY_DOLLAR), no key for the lambda (%s)", got);
    int status = -1;
    CHECK(write(proxy_go[1], "", 1) == 1, "tell it the typing is done");
    waitpid(proxy, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "a program binding wayland-proxy-1 doesn't take the compositor's place (%d)", status);

    /* A layout change: characters on the AltGr level are typed with it. */
    send_keymap("de");
    pump(10);
    send_input(c, CHAM_INPUT_KEYSYM, '@', 1);
    send_input(c, CHAM_INPUT_KEYSYM, '@', 0);
    got[0] = 0;
    for (int i = 0; i < 25 && strlen(got) < sizeof got - 8; i++) {
        pump(1);
        while (kfd >= 0 && read(kfd, &ev, sizeof ev) == (ssize_t)sizeof ev)
            if (ev.type == EV_KEY)
                snprintf(got + strlen(got), sizeof got - strlen(got), "%d=%d ", ev.code, ev.value);
    }
    CHECK(strcmp(got, "100=1 16=1 16=0 100=0 ") == 0, "German layout: '@' typed as AltGr+Q (%s)", got);
    send_keymap("us");
    pump(10);

    /* The clipboard, both ways, through ext_data_control_v1. */
    printf("clipboard\n");
    CHECK(dc.device != NULL, "the shim takes part in the clipboard (ext_data_control_v1 device)");
    struct cham_msg cm;
    int pfd = -1;
    if (dc.device) {
        dc_copy("from the desktop", "text/plain TEXT text/plain;charset=utf-8");
        pfd = wait_msg(c, CHAM_CLIPBOARD_DATA, &cm);
    }
    char text[64] = "";
    if (pfd >= 0) {
        read_all(pfd, text, sizeof text);
        close(pfd);
    }
    CHECK(strcmp(text, "from the desktop") == 0, "copied on the desktop: the app gets a pipe with the text (\"%s\")",
          text);
    struct cham_msg offer = {CHAM_CLIPBOARD_OFFER, 0, 0, 0};
    cham_send(c, &offer, -1);
    for (int i = 0; i < 25 && !dc.selection; i++)
        pump(1);
    CHECK(dc.selection && strstr(dc.offered, "text/plain;charset=utf-8 ") &&
              strstr(dc.offered, "application/x-chameleon-android "),
          "copied on Android: the shim owns the desktop's clipboard (%s)", dc.offered);
    int paste[2] = {-1, -1};
    text[0] = 0;
    if (dc.selection && pipe(paste) == 0) {
        wl_resource_post_event(dc.selection, 0 /* send */, "text/plain;charset=utf-8", paste[1]);
        close(paste[1]);
        int wfd = wait_msg(c, CHAM_CLIPBOARD_REQUEST, &cm);
        if (wfd >= 0) {
            if (write(wfd, "from Android", 12) < 0)
                perror("write");
            close(wfd);
        }
        read_all(paste[0], text, sizeof text);
        close(paste[0]);
    }
    CHECK(strcmp(text, "from Android") == 0, "a desktop app pastes it: the app writes into its pipe (\"%s\")", text);
    int before = dc.received;
    if (dc.device) {
        dc_copy("echo", "text/plain;charset=utf-8 application/x-chameleon-android");
        pfd = wait_msg(c, CHAM_CLIPBOARD_DATA, &cm);
        if (pfd >= 0)
            close(pfd);
    }
    CHECK(pfd < 0 && dc.received == before, "the shim's own clipboard isn't sent back to Android");

    /* The shim's connection to the compositor goes (as when it restarts) and
     * can't be made again at once: Android's text copied meanwhile becomes
     * the clipboard once connected, in place of the desktop's clipboard from
     * before; again after the next restart; but not after the desktop
     * copied. */
    char wl_path[256], off_path[300];
    snprintf(wl_path, sizeof wl_path, "%s/wayland-chameleon-test", getenv("XDG_RUNTIME_DIR"));
    snprintf(off_path, sizeof off_path, "%s.off", wl_path);
    dc.initial = "the desktop's clipboard from before";
    if (dc.device) {
        wl_client_destroy(wl_resource_get_client(dc.device));
        dc.device = dc.selection = NULL;
    }
    CHECK(rename(wl_path, off_path) == 0, "the compositor's socket is gone for a moment");
    pump(10);
    cham_send(c, &offer, -1); /* copied on Android: the shim tries to connect, and can't */
    pump(10);
    CHECK(rename(off_path, wl_path) == 0 && !dc.device, "...back again, the shim not connected yet");
    usleep(1100 * 1000); /* the shim tries once a second */
    dc.offered[0] = 0;
    cham_send(c, &cfg, -1); /* any message: the shim connects */
    for (int i = 0; i < 50 && !dc.selection; i++)
        pump(1);
    CHECK(dc.selection && strstr(dc.offered, "application/x-chameleon-android "),
          "Android's text copied while not connected is the clipboard once connected");
    pfd = wait_msg(c, CHAM_CLIPBOARD_DATA, &cm);
    if (pfd >= 0)
        close(pfd);
    CHECK(pfd < 0, "the desktop's clipboard from before isn't sent to Android over it");
    if (dc.device) {
        wl_client_destroy(wl_resource_get_client(dc.device));
        dc.device = dc.selection = NULL;
    }
    pump(10);
    cham_send(c, &cfg, -1);
    for (int i = 0; i < 50 && !dc.selection; i++)
        pump(1);
    CHECK(dc.selection != NULL, "after the next restart too");
    if (dc.device) {
        dc_copy("copied on the desktop since", "text/plain");
        pfd = wait_msg(c, CHAM_CLIPBOARD_DATA, &cm);
        if (pfd >= 0)
            close(pfd);
        wl_client_destroy(wl_resource_get_client(dc.device));
        dc.device = dc.selection = NULL;
    }
    dc.initial = "copied on the desktop since";
    pump(10);
    cham_send(c, &cfg, -1);
    pfd = wait_msg(c, CHAM_CLIPBOARD_DATA, &cm);
    text[0] = 0;
    if (pfd >= 0) {
        read_all(pfd, text, sizeof text);
        close(pfd);
    }
    CHECK(!dc.selection && strcmp(text, "copied on the desktop since") == 0,
          "but not once the desktop copied: its clipboard stays and goes to Android (\"%s\")", text);
    dc.initial = NULL;

    /* Many programs binding Wayland sockets of their own (each browser
     * launch binds a proxy's) don't push the compositor's out of the
     * service's list: the shim still finds it to connect again. */
    for (int i = 0; i < 20; i++) {
        pid_t k = fork();
        if (k == 0) {
            struct sockaddr_un ka = {.sun_family = AF_UNIX};
            snprintf(ka.sun_path, sizeof ka.sun_path, "%s/wayland-many-%d", getenv("XDG_RUNTIME_DIR"), i);
            int ks = socket(AF_UNIX, SOCK_STREAM, 0);
            unlink(ka.sun_path);
            _exit(ks < 0 || bind(ks, (struct sockaddr *)&ka, sizeof ka) != 0);
        }
        waitpid(k, NULL, 0);
    }
    usleep(300 * 1000); /* chameleon-kernel has heard of them */
    if (dc.device) {
        wl_client_destroy(wl_resource_get_client(dc.device));
        dc.device = dc.selection = NULL;
    }
    pump(10);
    cham_send(c, &cfg, -1);
    for (int i = 0; i < 50 && !dc.device; i++)
        pump(1);
    CHECK(dc.device != NULL, "20 programs binding Wayland sockets don't hide the compositor's (reconnected)");

    /* Chameleon's input method, started the way KWin starts one: with a
     * private connection that offers zwp_input_method_v1. While a text field
     * takes text (a context), the text goes there; without one, as keys. */
    printf("input method\n");
    wl_global_create(display, &k_im, 1, NULL, bind_im);
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv);
    pid_t ime_pid = fork();
    if (ime_pid == 0) {
        char fd[16];
        snprintf(fd, sizeof fd, "%d", sv[1]);
        fcntl(sv[1], F_SETFD, 0);
        setenv("WAYLAND_SOCKET", fd, 1);
        unsetenv("LD_PRELOAD"); /* KWin's children don't get the shim either */
        execl(getenv("CHAMELEON_IME"), "chameleon-ime", (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    struct wl_client *ime_client = wl_client_create(display, sv[0]);
    for (int i = 0; i < 50 && !ime.im; i++) {
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(display);
    }
    CHECK(ime_client && ime.im, "chameleon-ime binds zwp_input_method_v1");
    struct wl_resource *ctx = ime.im ? wl_resource_create(ime_client, &k_context, 1, 0) : NULL;
    if (ctx) {
        wl_resource_set_implementation(ctx, &k_context_impl, NULL, NULL);
        wl_resource_post_event(ime.im, 0 /* activate */, ctx);
    }
    for (int i = 0; i < 25; i++) { /* it connects to the shim and says a text field takes text */
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(display);
    }
    const char *word = "hi";
    for (const char *p = word; *p; p++)
        send_input(c, CHAM_INPUT_TEXT_CHAR, (unsigned char)*p, 0);
    send_input(c, CHAM_INPUT_TEXT_COMMIT, 0, 0);
    send_input(c, CHAM_INPUT_TEXT_CHAR, 0x00e9, 0); /* é */
    send_input(c, CHAM_INPUT_TEXT_CHAR, 'x', 0);
    send_input(c, CHAM_INPUT_TEXT_PREEDIT, 0, 0);
    send_input(c, CHAM_INPUT_TEXT_DELETE, 2, 0);
    for (int i = 0; i < 25 && !ime.delete_length; i++) {
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(display);
    }
    CHECK(strcmp(ime.commit, "hi") == 0, "committed text reaches the text field (\"%s\")", ime.commit);
    CHECK(strcmp(ime.preedit, "\xc3\xa9x") == 0, "composing text too (\"%s\")", ime.preedit);
    CHECK(ime.delete_index == -2 && ime.delete_length == 2, "deleting 2 before the cursor (%d, %u)",
          ime.delete_index, ime.delete_length);
    if (ctx)
        wl_resource_post_event(ime.im, 1 /* deactivate */, ctx);
    for (int i = 0; i < 25; i++) {
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(display);
    }
    while (kfd >= 0 && read(kfd, &ev, sizeof ev) == (ssize_t)sizeof ev)
        ;
    send_input(c, CHAM_INPUT_TEXT_CHAR, 'a', 0); /* composing "ab", then "ac" committed */
    send_input(c, CHAM_INPUT_TEXT_CHAR, 'b', 0);
    send_input(c, CHAM_INPUT_TEXT_PREEDIT, 0, 0);
    send_input(c, CHAM_INPUT_TEXT_CHAR, 'a', 0);
    send_input(c, CHAM_INPUT_TEXT_CHAR, 'c', 0);
    send_input(c, CHAM_INPUT_TEXT_COMMIT, 0, 0);
    got[0] = 0;
    for (int i = 0; i < 25 && strlen(got) < sizeof got - 8; i++) {
        wl_event_loop_dispatch(loop, 20);
        wl_display_flush_clients(display);
        while (kfd >= 0 && read(kfd, &ev, sizeof ev) == (ssize_t)sizeof ev)
            if (ev.type == EV_KEY)
                snprintf(got + strlen(got), sizeof got - strlen(got), "%d=%d ", ev.code, ev.value);
    }
    CHECK(strcmp(got, "30=1 30=0 48=1 48=0 14=1 14=0 46=1 46=0 ") == 0,
          "without a text field, text is typed as keys, composing text corrected with Backspace (%s)", got);
    if (ime_client)
        wl_client_destroy(ime_client);
    if (ime_pid > 0)
        waitpid(ime_pid, NULL, 0);
    if (kfd >= 0)
        close(kfd);

    /* An app built from another protocol version says so and closes; the
     * shim must not keep reconnecting. */
    printf("app of another protocol version\n");
    struct cham_msg bye = {CHAM_BYE, 0, CHAM_BYE_VERSION, CHAM_PROTO_VERSION + 1};
    cham_send(c, &bye, -1);
    close(c);
    p = (struct pollfd){l, POLLIN, 0};
    CHECK(poll(&p, 1, 2500) == 0, "the shim doesn't reconnect");
    close(fd);
    close(l);
    wl_display_destroy(display);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
