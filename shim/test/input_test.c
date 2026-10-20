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

    close(c);
    close(fd);
    wl_display_destroy(display);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
