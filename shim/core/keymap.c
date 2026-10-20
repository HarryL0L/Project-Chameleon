/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Text from the Android keyboard, typed on the evdev keyboard the way a real
 * one would type it: each keysym the app sends becomes the key, with Shift
 * and AltGr as needed, that produces it in the desktop's layout. The layout
 * is the compositor's keymap, which any Wayland client gets on its
 * wl_keyboard; libxkbcommon (loaded at run time) finds the keys in it. A
 * keysym the layout can't produce is not typed, as a real keyboard couldn't
 * type it either. Runs on input.c's thread.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../common/chameleon_proto.h"
#include "internal.h"
#include "wlclient.h"

/* ---- libxkbcommon, loaded at run time ---- */

struct xkb_context;
struct xkb_keymap;
static struct {
    struct xkb_context *(*context_new)(int flags);
    struct xkb_keymap *(*keymap_new_from_string)(struct xkb_context *, const char *, int format, int flags);
    void (*keymap_unref)(struct xkb_keymap *);
    uint32_t (*min_keycode)(struct xkb_keymap *);
    uint32_t (*max_keycode)(struct xkb_keymap *);
    uint32_t (*num_levels_for_key)(struct xkb_keymap *, uint32_t key, uint32_t layout);
    int (*key_get_syms_by_level)(struct xkb_keymap *, uint32_t key, uint32_t layout, uint32_t level,
                                 const uint32_t **syms);
    size_t (*key_get_mods_for_level)(struct xkb_keymap *, uint32_t key, uint32_t layout, uint32_t level,
                                     uint32_t *masks, size_t size);
    uint32_t (*mod_get_index)(struct xkb_keymap *, const char *name);
} xkb;
static struct xkb_context *g_xkb;

static int load_xkb(void)
{
    static int loaded = -1;
    if (loaded >= 0)
        return loaded;
    char path[512];
    const char *prefix = getenv("PREFIX");
    snprintf(path, sizeof path, "%s/lib/libxkbcommon.so", prefix && *prefix ? prefix : "/data/data/com.termux/files/usr");
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL);
#define X(field, name) (*(void **)&xkb.field = lib ? dlsym(lib, name) : NULL)
    X(context_new, "xkb_context_new");
    X(keymap_new_from_string, "xkb_keymap_new_from_string");
    X(keymap_unref, "xkb_keymap_unref");
    X(min_keycode, "xkb_keymap_min_keycode");
    X(max_keycode, "xkb_keymap_max_keycode");
    X(num_levels_for_key, "xkb_keymap_num_levels_for_key");
    X(key_get_syms_by_level, "xkb_keymap_key_get_syms_by_level");
    X(key_get_mods_for_level, "xkb_keymap_key_get_mods_for_level");
    X(mod_get_index, "xkb_keymap_mod_get_index");
#undef X
    if (!xkb.key_get_mods_for_level || !xkb.mod_get_index || !(g_xkb = xkb.context_new(0))) {
        cham_log("input: typed text is dropped: cannot use libxkbcommon (1.0+)");
        return loaded = 0;
    }
    return loaded = 1;
}

/* ---- the compositor's keymap ---- */

#define XKB_KEYMAP_FORMAT_TEXT_V1 1
#define WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 1
#define XK_SHIFT_L 0xffe1
#define XK_ISO_LEVEL3_SHIFT 0xfe03

static struct wl_proxy *g_seat, *g_keyboard;
static struct xkb_keymap *g_keymap;
static uint32_t g_shift_mask, g_level3_mask;
static unsigned g_shift_key, g_level3_key; /* Linux key codes, 0 = none in the layout */

/* A key of the layout's first group whose plain level produces `sym`. */
static unsigned plain_key(uint32_t sym)
{
    for (uint32_t k = xkb.min_keycode(g_keymap); k <= xkb.max_keycode(g_keymap); k++) {
        const uint32_t *syms;
        if (xkb.key_get_syms_by_level(g_keymap, k, 0, 0, &syms) == 1 && syms[0] == sym)
            return k - 8;
    }
    return 0;
}

static uint32_t mod_bit(const char *name)
{
    uint32_t i = xkb.mod_get_index(g_keymap, name);
    return i < 32 ? 1u << i : 0;
}

static void keyboard_keymap(void *data, struct wl_proxy *kb, uint32_t format, int fd, uint32_t size)
{
    (void)data;
    (void)kb;
    char *map = format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && load_xkb()
                    ? mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0)
                    : MAP_FAILED;
    close(fd);
    if (map == MAP_FAILED)
        return;
    struct xkb_keymap *km = xkb.keymap_new_from_string(g_xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(map, size);
    if (!km) {
        cham_log("input: cannot read the compositor's keymap");
        return;
    }
    if (g_keymap)
        xkb.keymap_unref(g_keymap);
    g_keymap = km;
    g_shift_mask = mod_bit("Shift");
    g_level3_mask = mod_bit("Mod5") | mod_bit("LevelThree");
    g_shift_key = plain_key(XK_SHIFT_L);
    g_level3_key = plain_key(XK_ISO_LEVEL3_SHIFT);
    /* KWin sends a keymap again whenever it changes. */
    static int logged;
    if (!logged++)
        cham_log("input: typed text goes to the evdev keyboard through the desktop's layout");
}

static void (*k_keyboard_listener[])(void) = {
    (void (*)(void))keyboard_keymap,
    wl_ignore_event, /* enter */
    wl_ignore_event, /* leave */
    wl_ignore_event, /* key */
    wl_ignore_event, /* modifiers */
    wl_ignore_event, /* repeat_info */
};

void keymap_global(struct wl_proxy *registry, uint32_t name, const char *iface)
{
    if (strcmp(iface, "wl_seat") != 0 || g_seat || !wl.seat_iface || !wl.keyboard_iface)
        return;
    g_seat = wl_bind(registry, name, wl.seat_iface, 1); /* no listener: its events are dropped */
    g_keyboard = wl.marshal_flags(g_seat, 1 /* get_keyboard */, wl.keyboard_iface, 1, 0, NULL);
    wl.add_listener(g_keyboard, k_keyboard_listener, NULL);
}

void keymap_disconnected(void)
{
    g_seat = g_keyboard = NULL; /* destroyed with the display */
}

/* ---- typing ---- */

static struct {
    uint32_t sym;
    unsigned key, shift, level3;
} g_down[8]; /* keysyms pressed and not yet released */

/* The key and modifiers producing `sym`: the lowest level of the first group
 * reachable with Shift and AltGr only. */
static int lookup(uint32_t sym, unsigned *key, int *shift, int *level3)
{
    int best = 3;
    for (uint32_t k = xkb.min_keycode(g_keymap); best && k <= xkb.max_keycode(g_keymap); k++) {
        for (uint32_t lv = 0; lv < xkb.num_levels_for_key(g_keymap, k, 0); lv++) {
            const uint32_t *syms;
            if (xkb.key_get_syms_by_level(g_keymap, k, 0, lv, &syms) != 1 || syms[0] != sym)
                continue;
            uint32_t masks[8];
            size_t n = xkb.key_get_mods_for_level(g_keymap, k, 0, lv, masks, 8);
            for (size_t i = 0; i < n; i++) {
                int s = (masks[i] & g_shift_mask) != 0, l3 = (masks[i] & g_level3_mask) != 0;
                if ((masks[i] & ~(g_shift_mask | g_level3_mask)) || (s && !g_shift_key) || (l3 && !g_level3_key) ||
                    s + l3 >= best)
                    continue;
                best = s + l3;
                *key = k - 8;
                *shift = s;
                *level3 = l3;
            }
        }
    }
    return best < 3;
}

static int send_key(unsigned key, int down)
{
    struct cham_msg m = {CHAM_INPUT, CHAM_INPUT_KEY, key, (uint64_t)down};
    return evdev_input(&m);
}

int keymap_type(uint32_t sym, int down)
{
    if (!down) {
        for (size_t i = 0; i < sizeof g_down / sizeof g_down[0]; i++) {
            if (g_down[i].key && g_down[i].sym == sym) {
                send_key(g_down[i].key, 0);
                if (g_down[i].level3)
                    send_key(g_level3_key, 0);
                if (g_down[i].shift)
                    send_key(g_shift_key, 0);
                g_down[i].key = 0;
                return 1;
            }
        }
        return 0;
    }
    unsigned key;
    int shift, level3;
    size_t slot = 0;
    while (slot < sizeof g_down / sizeof g_down[0] && g_down[slot].key)
        slot++;
    if (!g_keymap || slot == sizeof g_down / sizeof g_down[0] || !lookup(sym, &key, &shift, &level3))
        return 0;
    if ((shift && !send_key(g_shift_key, 1)) || (level3 && !send_key(g_level3_key, 1)) || !send_key(key, 1)) {
        if (level3) /* a key the evdev keyboard lacks */
            send_key(g_level3_key, 0);
        if (shift)
            send_key(g_shift_key, 0);
        return 0;
    }
    g_down[slot].sym = sym;
    g_down[slot].key = key;
    g_down[slot].shift = (unsigned)shift;
    g_down[slot].level3 = (unsigned)level3;
    return 1;
}
