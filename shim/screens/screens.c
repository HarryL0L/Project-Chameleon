/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chameleon-screens: a session's programs ask for windows of the Chameleon
 * app here, over the session's D-Bus (what a compositor's script can call):
 *
 *   name       io.github.harryl0l.Chameleon
 *   object     /Screens
 *   interface  io.github.harryl0l.Chameleon.Screens
 *     Open() -> i            asks the app for the window of a screen of the
 *                            card that has none: the number N of its
 *                            connector (DSI-N), which gets connected once the
 *                            window is there; 0 if there is no such screen
 *     Close(s N)             has that window closed (its connector is then
 *                            disconnected); not the first screen's
 *     SetTitle(s N, s text)  names the window in Android's recent apps
 *
 * The numbers are strings going in, since a script's numbers have no fixed
 * D-Bus type. Each call becomes one request to chameleon-kernel (CARD_SCREEN,
 * core/card.c), which talks to the app. Started by the `chameleon` launcher
 * where the card has several screens; exits with the bus.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../common/cham_io.h"
#include "../core/internal.h"

#define LOG(...) fprintf(stderr, "chameleon-screens: " __VA_ARGS__)

#define BUS_NAME "io.github.harryl0l.Chameleon"
#define INTERFACE BUS_NAME ".Screens"

/* ---- libdbus, loaded by name: only what is called here (dbus/dbus.h) ---- */

typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
enum { DBUS_BUS_SESSION = 0, DBUS_NAME_FLAG_DO_NOT_QUEUE = 4, DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER = 1 };
enum { DBUS_TYPE_INVALID = 0, DBUS_TYPE_INT32 = 'i', DBUS_TYPE_STRING = 's' };

static struct {
    DBusConnection *(*bus_get)(int type, void *error);
    int (*bus_request_name)(DBusConnection *, const char *name, unsigned flags, void *error);
    void (*connection_set_exit_on_disconnect)(DBusConnection *, uint32_t exit_on_disconnect);
    uint32_t (*connection_read_write)(DBusConnection *, int timeout_ms);
    DBusMessage *(*connection_pop_message)(DBusConnection *);
    uint32_t (*connection_send)(DBusConnection *, DBusMessage *, uint32_t *serial);
    void (*connection_flush)(DBusConnection *);
    uint32_t (*message_is_method_call)(DBusMessage *, const char *interface, const char *method);
    uint32_t (*message_get_args)(DBusMessage *, void *error, int first_type, ...);
    uint32_t (*message_get_no_reply)(DBusMessage *);
    DBusMessage *(*message_new_method_return)(DBusMessage *call);
    uint32_t (*message_append_args)(DBusMessage *, int first_type, ...);
    void (*message_unref)(DBusMessage *);
} dbus;

static int load_dbus(void)
{
    char path[512];
    const char *prefix = getenv("PREFIX");
    snprintf(path, sizeof path, "%s/lib/libdbus-1.so", prefix ? prefix : "/data/data/com.termux/files/usr");
    void *lib = dlopen(path, RTLD_NOW);
    if (!lib)
        lib = dlopen("libdbus-1.so.3", RTLD_NOW);
    if (!lib) {
        LOG("no libdbus-1 (%s)\n", dlerror());
        return -1;
    }
#define SYM(name)                                             \
    if (!(*(void **)&dbus.name = dlsym(lib, "dbus_" #name))) { \
        LOG("libdbus-1 has no dbus_" #name "\n");             \
        return -1;                                            \
    }
    SYM(bus_get)
    SYM(bus_request_name)
    SYM(connection_set_exit_on_disconnect)
    SYM(connection_read_write)
    SYM(connection_pop_message)
    SYM(connection_send)
    SYM(connection_flush)
    SYM(message_is_method_call)
    SYM(message_get_args)
    SYM(message_get_no_reply)
    SYM(message_new_method_return)
    SYM(message_append_args)
    SYM(message_unref)
#undef SYM
    return 0;
}

/* ---- the request to chameleon-kernel ---- */

static int g_card = -1;

static int64_t ask_once(uint32_t what, int screen, const char *text)
{
    if (g_card < 0) {
        const char *dir = getenv("XDG_RUNTIME_DIR");
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        if (!dir || snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", dir, CHAM_CARD_SOCKET_NAME) >=
                        (int)sizeof addr.sun_path)
            return -ENOENT;
        g_card = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (g_card >= 0 && connect(g_card, (struct sockaddr *)&addr, sizeof addr) != 0) {
            close(g_card);
            g_card = -1;
        }
        if (g_card < 0)
            return -ECONNREFUSED;
    }
    struct cham_msg req = {CARD_SCREEN, what, (uint64_t)(int64_t)screen, 0}, done;
    int fd = -1;
    if (cham_send(g_card, &req, -1) != 0 || (text && send(g_card, text, strlen(text), MSG_NOSIGNAL) < 0) ||
        cham_recv(g_card, &done, &fd) != 1 || done.type != CARD_DONE) {
        close(g_card);
        g_card = -1;
        return -EPIPE;
    }
    if (fd >= 0)
        close(fd);
    return (int64_t)done.a;
}

/* Once more on a connection of before that the service has closed. */
static int64_t ask(uint32_t what, int screen, const char *text)
{
    int had = g_card >= 0;
    int64_t ret = ask_once(what, screen, text);
    return ret == -EPIPE && had ? ask_once(what, screen, text) : ret;
}

int main(void)
{
    if (load_dbus() != 0)
        return 1;
    DBusConnection *bus = dbus.bus_get(DBUS_BUS_SESSION, NULL);
    if (!bus) {
        LOG("no session bus\n");
        return 1;
    }
    dbus.connection_set_exit_on_disconnect(bus, 0);
    if (dbus.bus_request_name(bus, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL) !=
        DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        LOG("the name " BUS_NAME " is taken\n");
        return 1;
    }
    while (dbus.connection_read_write(bus, -1)) {
        DBusMessage *call;
        while ((call = dbus.connection_pop_message(bus))) {
            const char *n = NULL, *text = NULL;
            int handled = 1;
            int32_t ret = 0;
            if (dbus.message_is_method_call(call, INTERFACE, "Open")) {
                int64_t screen = ask(CARD_SCREEN_OPEN, 0, NULL);
                ret = screen > 0 ? (int32_t)screen + 1 : 0; /* the connector's number */
                if (screen <= 0)
                    LOG("no window to open: %s\n", strerror((int)-screen));
            } else if (dbus.message_is_method_call(call, INTERFACE, "Close") &&
                       dbus.message_get_args(call, NULL, DBUS_TYPE_STRING, &n, DBUS_TYPE_INVALID)) {
                ret = (int32_t)ask(CARD_SCREEN_CLOSE, atoi(n) - 1, NULL);
            } else if (dbus.message_is_method_call(call, INTERFACE, "SetTitle") &&
                       dbus.message_get_args(call, NULL, DBUS_TYPE_STRING, &n, DBUS_TYPE_STRING, &text,
                                             DBUS_TYPE_INVALID)) {
                char cut[240]; /* one packet's worth; whole UTF-8 sequences */
                size_t len = strlen(text) < sizeof cut ? strlen(text) : sizeof cut - 1;
                while (len && len < strlen(text) && ((unsigned char)text[len] & 0xc0) == 0x80)
                    len--;
                memcpy(cut, text, len);
                cut[len] = 0;
                ret = (int32_t)ask(CARD_SCREEN_TITLE, atoi(n) - 1, cut);
            } else {
                handled = 0; /* the bus's own signals, a method this hasn't */
            }
            if (handled && !dbus.message_get_no_reply(call)) {
                DBusMessage *reply = dbus.message_new_method_return(call);
                if (reply && dbus.message_append_args(reply, DBUS_TYPE_INT32, &ret, DBUS_TYPE_INVALID))
                    dbus.connection_send(bus, reply, NULL);
                if (reply)
                    dbus.message_unref(reply);
                dbus.connection_flush(bus);
            }
            dbus.message_unref(call);
        }
    }
    return 0; /* the bus is gone: the session ended */
}
