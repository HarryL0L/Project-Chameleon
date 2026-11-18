/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Android keyboard's text (CHAM_INPUT_TEXT_*), the way a Linux on-screen
 * keyboard delivers it: while chameleon-ime, the input method the compositor
 * started, says a text field takes text, the text goes to it (committed text,
 * the word being composed) and from there to the app through text-input;
 * Enter and deletions are keys (Return, BackSpace, Delete) it sends in the
 * same stream, which every app takes, unlike Enter as text or a request to
 * delete surrounding text; the line breaks of pasted text stay in the text.
 * Otherwise it is all typed as keys, like on a
 * real keyboard (keymap.c): composing text is typed as it changes and
 * corrected with Backspace. Runs on input.c's thread.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../common/chameleon_evdev.h"
#include "../../common/chameleon_proto.h"
#include "internal.h"

#define XK_BACKSPACE 0xff08
#define XK_TAB 0xff09
#define XK_RETURN 0xff0d
#define XK_DELETE 0xffff
#define MAX_TEXT 256 /* code points */

static int g_ime = -1;  /* connection from chameleon-ime */
static int g_ime_active; /* it has a text field */
static uint32_t g_text[MAX_TEXT]; /* code points since the last COMMIT / PREEDIT */
static size_t g_text_len;
static uint32_t g_typed[MAX_TEXT]; /* composing text typed as keys, without a text field */
static size_t g_typed_len;

/* ---- chameleon-ime ---- */

static void ime_closed(void)
{
    close(g_ime);
    g_ime = -1;
    g_ime_active = 0;
    cham_log("text: the input method went away; text is typed as keys");
}

void text_ime_attach(int fd)
{
    if (g_ime >= 0)
        close(g_ime); /* a new one replaces it (the compositor restarted it) */
    g_ime = fd;
    g_ime_active = 0;
    cham_log("text: Chameleon's input method connected");
}

int text_ime_fd(void)
{
    return g_ime;
}

/* Its messages: whether a text field takes text. */
void text_ime_readable(void)
{
    struct {
        struct cham_evdev_record h;
        uint32_t active;
    } r;
    ssize_t n = recv(g_ime, &r, sizeof r, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        ime_closed();
        return;
    }
    if (n == (ssize_t)sizeof r && r.h.type == CHAM_EVDEV_IME_ACTIVE && r.h.size == sizeof r.active) {
        g_ime_active = r.active != 0;
        g_typed_len = 0;
    }
}

static size_t utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3f));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* COMMIT / PREEDIT with text[0..len), or KEYSYM tapped count times; whether
 * the input method has it. */
static int ime_send(uint32_t kind, const uint32_t *text, size_t len, uint32_t keysym, uint32_t count)
{
    struct {
        struct cham_evdev_record h;
        struct cham_ime_text t;
        char text[MAX_TEXT * 4];
    } r = {.t = {kind, keysym, count}};
    size_t bytes = 0;
    for (size_t i = 0; i < len; i++)
        bytes += utf8(text[i], r.text + bytes);
    r.h = (struct cham_evdev_record){CHAM_EVDEV_IME_TEXT, (uint32_t)(sizeof r.t + bytes)};
    size_t size = sizeof r.h + r.h.size;
    if (g_ime >= 0 && send(g_ime, &r, size, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)size)
        ime_closed(); /* gone or stuck: its socket holds far more than one message */
    return g_ime >= 0;
}

static void tap(uint32_t keysym);
static uint32_t keysym_for(uint32_t cp);

/* Committed text to the text field. A line break alone is the keyboard's
 * Enter: Return, after an empty commit that ends the text being composed.
 * Line breaks inside text (pasted lines) stay text: an app takes committed
 * text at once but queues keys, so a Return between two pieces of text of
 * one burst would act after both. If the input method goes away on the way,
 * the text is typed as keys. */
static void ime_commit(void)
{
    size_t sent = 0;
    if (g_text_len == 1 && (g_text[0] == '\n' || g_text[0] == '\r')) {
        sent = ime_send(CHAM_IME_COMMIT, g_text, 0, 0, 0) && ime_send(CHAM_IME_KEYSYM, NULL, 0, XK_RETURN, 1);
    } else {
        for (size_t i = 0; i < g_text_len; i++)
            if (g_text[i] == '\r')
                g_text[i] = '\n';
        if (ime_send(CHAM_IME_COMMIT, g_text, g_text_len, 0, 0))
            sent = g_text_len;
    }
    for (size_t i = sent; g_ime < 0 && i < g_text_len; i++)
        tap(keysym_for(g_text[i]));
}

/* ---- as keys ---- */

static uint32_t keysym_for(uint32_t cp)
{
    if (cp == '\n' || cp == '\r')
        return XK_RETURN;
    if (cp == '\t')
        return XK_TAB;
    if ((cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xff))
        return cp; /* Latin-1 keysyms are the code points */
    return 0x01000000 | cp;
}

static void tap(uint32_t keysym)
{
    input_keysym(keysym, 1);
    input_keysym(keysym, 0);
}

/* Turns the composing text typed so far into the pending text: erases what
 * differs, types the rest. */
static void retype(void)
{
    size_t same = 0;
    while (same < g_typed_len && same < g_text_len && g_typed[same] == g_text[same])
        same++;
    for (size_t i = same; i < g_typed_len; i++)
        tap(XK_BACKSPACE);
    for (size_t i = same; i < g_text_len; i++)
        tap(keysym_for(g_text[i]));
}

/* ---- the app's messages ---- */

int text_input(const struct cham_msg *m)
{
    switch (m->id) {
    case CHAM_INPUT_TEXT_CHAR:
        if (g_text_len < MAX_TEXT)
            g_text[g_text_len++] = (uint32_t)m->a;
        return 1;
    case CHAM_INPUT_TEXT_COMMIT:
    case CHAM_INPUT_TEXT_PREEDIT:
        if (g_ime >= 0 && g_ime_active && m->id == CHAM_INPUT_TEXT_COMMIT) {
            ime_commit();
        } else if (g_ime >= 0 && g_ime_active) {
            ime_send(CHAM_IME_PREEDIT, g_text, g_text_len, 0, 0);
        } else {
            retype();
            g_typed_len = 0;
            if (m->id == CHAM_INPUT_TEXT_PREEDIT) {
                memcpy(g_typed, g_text, g_text_len * sizeof g_text[0]);
                g_typed_len = g_text_len;
            }
        }
        g_text_len = 0;
        return 1;
    case CHAM_INPUT_TEXT_DELETE:
        if (g_ime >= 0 && g_ime_active) {
            if (m->a)
                ime_send(CHAM_IME_KEYSYM, NULL, 0, XK_BACKSPACE, (uint32_t)(m->a < MAX_TEXT ? m->a : MAX_TEXT));
            if (m->b)
                ime_send(CHAM_IME_KEYSYM, NULL, 0, XK_DELETE, (uint32_t)(m->b < MAX_TEXT ? m->b : MAX_TEXT));
        } else {
            for (uint64_t i = 0; i < m->a && i < MAX_TEXT; i++)
                tap(XK_BACKSPACE);
            for (uint64_t i = 0; i < m->b && i < MAX_TEXT; i++)
                tap(XK_DELETE);
        }
        return 1;
    }
    return 0;
}
