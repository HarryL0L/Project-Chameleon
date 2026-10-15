/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Chameleon's evdev devices against the real kernel's evdev.
 *
 * The same reader-side checks run twice and print the same transcript:
 *  - on the host (default build) against Chameleon's keyboard and
 *    touchscreen, driven through the app protocol (run with both shim
 *    libraries preloaded, as evdev_test); `dump` first saves those devices'
 *    capabilities;
 *  - in a real kernel (built with -DREF_KERNEL, run in the reference VM)
 *    against uinput devices created with exactly those capabilities and fed
 *    the same events.
 * run-conformance.sh diffs the two transcripts: every difference is
 * Chameleon not behaving like the kernel.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#ifdef REF_KERNEL
#include <linux/uinput.h>
#else
#include <sys/socket.h>
#include <sys/un.h>

#include "../../common/cham_io.h"
#include "../../common/chameleon_proto.h"
#endif

#define BITS(n) (((n) + 7) / 8)

struct caps {
    char name[80];
    struct input_id id;
    uint8_t ev[BITS(EV_CNT)], key[BITS(KEY_CNT)], rel[BITS(REL_CNT)], abs[BITS(ABS_CNT)], prop[BITS(INPUT_PROP_CNT)];
    struct input_absinfo absinfo[ABS_CNT];
};

static int bit(const uint8_t *b, unsigned n)
{
    return (b[n / 8] >> (n % 8)) & 1;
}

static const char *err(int ret)
{
    static char buf[32];
    if (ret >= 0)
        snprintf(buf, sizeof buf, "%d", ret);
    else
        snprintf(buf, sizeof buf, "-1 %s", strerrorname_np(errno) ? strerrorname_np(errno) : "?");
    return buf;
}

static void hex(const char *what, const uint8_t *b, int n)
{
    printf("%s:", what);
    int last = n - 1;
    while (last > 0 && !b[last])
        last--;
    for (int i = 0; i <= last; i++)
        printf("%02x", b[i]);
    printf("\n");
}

/* ---- driving the device ---- */

#ifdef REF_KERNEL
static int g_uinput[2]; /* keyboard, touchscreen */

static int create(const struct caps *c)
{
    int u = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    for (unsigned t = 0; t < EV_CNT; t++)
        if (bit(c->ev, t))
            ioctl(u, UI_SET_EVBIT, t);
    for (unsigned k = 0; k < KEY_CNT; k++)
        if (bit(c->key, k))
            ioctl(u, UI_SET_KEYBIT, k);
    for (unsigned r = 0; r < REL_CNT; r++)
        if (bit(c->rel, r))
            ioctl(u, UI_SET_RELBIT, r);
    for (unsigned p = 0; p < INPUT_PROP_CNT; p++)
        if (bit(c->prop, p))
            ioctl(u, UI_SET_PROPBIT, p);
    for (unsigned a = 0; a < ABS_CNT; a++) {
        if (!bit(c->abs, a))
            continue;
        ioctl(u, UI_SET_ABSBIT, a);
        struct uinput_abs_setup s = {.code = (uint16_t)a, .absinfo = c->absinfo[a]};
        ioctl(u, UI_ABS_SETUP, &s);
    }
    struct uinput_setup s = {.id = c->id};
    memcpy(s.name, c->name, sizeof s.name);
    ioctl(u, UI_DEV_SETUP, &s);
    if (ioctl(u, UI_DEV_CREATE) != 0) {
        perror("UI_DEV_CREATE");
        exit(2);
    }
    return u;
}

/* The /dev/input/eventN of the uinput device u. */
static void node_of(int u, char *path, size_t size)
{
    char sys[64], dir[128];
    ioctl(u, UI_GET_SYSNAME(sizeof sys), sys);
    snprintf(dir, sizeof dir, "/sys/class/input/%s", sys);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strncmp(e->d_name, "event", 5) == 0)
            snprintf(path, size, "/dev/input/%s", e->d_name);
    if (d)
        closedir(d);
}

static void emit(int dev, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev = {.type = type, .code = code, .value = value};
    if (write(g_uinput[dev], &ev, sizeof ev) != (ssize_t)sizeof ev)
        perror("uinput write");
}

static void settle(void)
{
}
#else
static int g_presenter;

static void send_input(uint32_t kind, uint64_t a, uint64_t b)
{
    struct cham_msg m = {CHAM_INPUT, kind, a, b};
    cham_send(g_presenter, &m, -1);
}

/* Input passes through KWin's process before a reader sees it. */
static void settle(void)
{
    usleep(150000);
}
#endif

enum { KBD, TOUCH };

/* A key, as the app sends it / as a keyboard driver reports it. */
static void key(unsigned code, int down)
{
#ifdef REF_KERNEL
    emit(KBD, EV_KEY, (uint16_t)code, down);
    emit(KBD, EV_SYN, SYN_REPORT, 0);
#else
    send_input(CHAM_INPUT_KEY, code, (uint64_t)down);
#endif
}

/* Touch, in Chameleon's touchscreen's units (0..65535): finger `id` in
 * slot `slot`. The reference side reports it the way Chameleon's server
 * does (protocol B), so the comparison is about the node, not the mapping. */
#ifdef REF_KERNEL
static int g_cur_slot = -1, g_tracking, g_touching;
#endif

static void touch_down(int slot, int id, int x, int y)
{
#ifdef REF_KERNEL
    (void)id;
    if (slot != g_cur_slot)
        emit(TOUCH, EV_ABS, ABS_MT_SLOT, slot);
    g_cur_slot = slot;
    emit(TOUCH, EV_ABS, ABS_MT_TRACKING_ID, g_tracking++);
    if (g_touching++ == 0)
        emit(TOUCH, EV_KEY, BTN_TOUCH, 1);
    emit(TOUCH, EV_ABS, ABS_MT_POSITION_X, x);
    emit(TOUCH, EV_ABS, ABS_MT_POSITION_Y, y);
    if (slot == 0) {
        emit(TOUCH, EV_ABS, ABS_X, x);
        emit(TOUCH, EV_ABS, ABS_Y, y);
    }
    emit(TOUCH, EV_SYN, SYN_REPORT, 0);
#else
    (void)slot;
    send_input(CHAM_INPUT_TOUCH_DOWN, (uint64_t)id, CHAM_INPUT_POS(x, y));
    send_input(CHAM_INPUT_TOUCH_FRAME, 0, 0);
#endif
}

static void touch_move(int slot, int id, int x, int y)
{
#ifdef REF_KERNEL
    (void)id;
    if (slot != g_cur_slot)
        emit(TOUCH, EV_ABS, ABS_MT_SLOT, slot);
    g_cur_slot = slot;
    emit(TOUCH, EV_ABS, ABS_MT_POSITION_X, x);
    emit(TOUCH, EV_ABS, ABS_MT_POSITION_Y, y);
    if (slot == 0) {
        emit(TOUCH, EV_ABS, ABS_X, x);
        emit(TOUCH, EV_ABS, ABS_Y, y);
    }
    emit(TOUCH, EV_SYN, SYN_REPORT, 0);
#else
    (void)slot;
    send_input(CHAM_INPUT_TOUCH_MOTION, (uint64_t)id, CHAM_INPUT_POS(x, y));
    send_input(CHAM_INPUT_TOUCH_FRAME, 0, 0);
#endif
}

static void touch_up(int slot, int id)
{
#ifdef REF_KERNEL
    (void)id;
    if (slot != g_cur_slot)
        emit(TOUCH, EV_ABS, ABS_MT_SLOT, slot);
    g_cur_slot = slot;
    emit(TOUCH, EV_ABS, ABS_MT_TRACKING_ID, -1);
    if (--g_touching == 0)
        emit(TOUCH, EV_KEY, BTN_TOUCH, 0);
    emit(TOUCH, EV_SYN, SYN_REPORT, 0);
#else
    (void)slot;
    send_input(CHAM_INPUT_TOUCH_UP, (uint64_t)id, 0);
    send_input(CHAM_INPUT_TOUCH_FRAME, 0, 0);
#endif
}

/* ---- the reader's view ---- */

static __attribute__((unused)) void dump_caps(int fd, struct caps *c)
{
    memset(c, 0, sizeof *c);
    ioctl(fd, EVIOCGNAME(sizeof c->name), c->name);
    ioctl(fd, EVIOCGID, &c->id);
    ioctl(fd, EVIOCGBIT(0, sizeof c->ev), c->ev);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof c->key), c->key);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof c->rel), c->rel);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof c->abs), c->abs);
    ioctl(fd, EVIOCGPROP(sizeof c->prop), c->prop);
    for (unsigned a = 0; a < ABS_CNT; a++)
        if (bit(c->abs, a))
            ioctl(fd, EVIOCGABS(a), &c->absinfo[a]);
}

static void identity(int fd)
{
    char s[256];
    int v = 0;
    int ret = ioctl(fd, EVIOCGVERSION, &v);
    printf("GVERSION %s %#x\n", err(ret), v);
    struct input_id id = {0};
    ret = ioctl(fd, EVIOCGID, &id);
    printf("GID %s %04x:%04x:%04x:%04x\n", err(ret), id.bustype, id.vendor, id.product, id.version);
    memset(s, 0, sizeof s);
    ret = ioctl(fd, EVIOCGNAME(sizeof s), s);
    printf("GNAME %s '%s'\n", err(ret), s);
    printf("GNAME(4) %s\n", err(ioctl(fd, EVIOCGNAME(4), s)));
    printf("GPHYS %s\n", err(ioctl(fd, EVIOCGPHYS(sizeof s), s)));
    printf("GUNIQ %s\n", err(ioctl(fd, EVIOCGUNIQ(sizeof s), s)));
    uint8_t b[BITS(KEY_CNT)];
    static const struct {
        const char *name;
        unsigned type, max;
    } types[] = {{"GBIT(0)", 0, EV_MAX},       {"GBIT(KEY)", EV_KEY, KEY_MAX}, {"GBIT(REL)", EV_REL, REL_MAX},
                 {"GBIT(ABS)", EV_ABS, ABS_MAX}, {"GBIT(MSC)", EV_MSC, MSC_MAX}, {"GBIT(LED)", EV_LED, LED_MAX},
                 {"GBIT(SND)", EV_SND, SND_MAX}, {"GBIT(REP)", EV_REP, REP_MAX}, {"GBIT(FF)", EV_FF, FF_MAX},
                 {"GBIT(SW)", EV_SW, SW_MAX}};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        memset(b, 0, sizeof b);
        ret = ioctl(fd, EVIOCGBIT(types[i].type, sizeof b), b);
        printf("%s ret %s ", types[i].name, err(ret));
        hex("", b, BITS(types[i].max + 1));
    }
    memset(b, 0, sizeof b);
    printf("GPROP %s ", err(ioctl(fd, EVIOCGPROP(sizeof b), b)));
    hex("", b, BITS(INPUT_PROP_CNT));
    uint8_t abs[BITS(ABS_CNT)] = {0};
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    for (unsigned a = 0; a < ABS_CNT; a++) {
        if (!bit(abs, a))
            continue;
        struct input_absinfo ai = {0};
        ret = ioctl(fd, EVIOCGABS(a), &ai);
        printf("GABS(%#x) %s value %d min %d max %d fuzz %d flat %d res %d\n", a, err(ret), ai.value, ai.minimum,
               ai.maximum, ai.fuzz, ai.flat, ai.resolution);
    }
    printf("GABS(unset ABS_PRESSURE) %s\n", err(bit(abs, ABS_PRESSURE) ? 0 : ioctl(fd, EVIOCGABS(ABS_PRESSURE), &(struct input_absinfo){0})));
    unsigned rep[2] = {0};
    ret = ioctl(fd, EVIOCGREP, rep);
    printf("GREP %s %u %u\n", err(ret), rep[0], rep[1]);
    int effects = -1;
    ret = ioctl(fd, EVIOCGEFFECTS, &effects);
    printf("GEFFECTS %s %d\n", err(ret), effects);
    printf("GKEYCODE %s\n", err(ioctl(fd, EVIOCGKEYCODE, (unsigned[2]){30, 0})));
}

static void state(int fd, const char *when)
{
    uint8_t b[BITS(KEY_CNT)] = {0};
    int ret = ioctl(fd, EVIOCGKEY(sizeof b), b);
    printf("%s GKEY ret %s ", when, err(ret));
    hex("", b, sizeof b);
    memset(b, 0, sizeof b);
    printf("%s GLED ret %s\n", when, err(ioctl(fd, EVIOCGLED(sizeof b), b)));
    uint8_t abs[BITS(ABS_CNT)] = {0};
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    for (unsigned a = 0; a < ABS_CNT; a++) {
        struct input_absinfo ai = {0};
        if (bit(abs, a) && ioctl(fd, EVIOCGABS(a), &ai) == 0)
            printf("%s GABS(%#x).value %d\n", when, a, ai.value);
    }
    if (bit(abs, ABS_MT_SLOT)) {
        const unsigned codes[] = {ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y};
        for (size_t i = 0; i < 3; i++) {
            int32_t v[1 + 4] = {(int32_t)codes[i]};
            int r = ioctl(fd, EVIOCGMTSLOTS(sizeof v), v);
            printf("%s GMTSLOTS(%#x) %s %d %d %d %d\n", when, codes[i], err(r), v[1], v[2], v[3], v[4]);
        }
    }
}

static int64_t ts_ns(const struct input_event *e)
{
    return (int64_t)e->input_event_sec * 1000000000 + (int64_t)e->input_event_usec * 1000;
}

static int64_t clock_ns(clockid_t c)
{
    struct timespec ts;
    clock_gettime(c, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Reads everything pending (non-blocking), printing each frame; returns frames. */
static int drain(int fd, const char *label, int print)
{
    struct input_event ev[64];
    int frames = 0, events = 0;
    char line[2048] = "";
    int64_t last = 0;
    int ts_bad = 0;
    for (;;) {
        ssize_t n = read(fd, ev, sizeof ev);
        if (n < 0) {
            if (errno != EAGAIN)
                printf("%s read %s\n", label, err(-1));
            break;
        }
        if (n == 0) {
            printf("%s read 0 (EOF)\n", label);
            break;
        }
        if (n % sizeof ev[0])
            printf("%s read returned a partial event (%zd bytes)\n", label, n);
        for (size_t i = 0; i < (size_t)n / sizeof ev[0]; i++) {
            const struct input_event *e = &ev[i];
            events++;
            ts_bad |= ts_ns(e) < last;
            last = ts_ns(e);
            if (e->type == EV_SYN && e->code == SYN_DROPPED) {
                printf("%s frame %d: SYN_DROPPED (after %d events)\n", label, frames, events - 1);
                continue;
            }
            if (print && strlen(line) < sizeof line - 32)
                snprintf(line + strlen(line), sizeof line - strlen(line), " %x:%x:%d", e->type, e->code, e->value);
            if (e->type == EV_SYN && e->code == SYN_REPORT) {
                if (print)
                    printf("%s frame %d:%s\n", label, frames, line);
                line[0] = 0;
                frames++;
            }
        }
    }
    if (line[0])
        printf("%s unfinished frame:%s\n", label, line);
    printf("%s %d frames, %d events, timestamps %s\n", label, frames, events, ts_bad ? "go backwards" : "in order");
    return frames;
}

static int open_node(const char *path)
{
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        printf("open %s: %s\n", path, strerror(errno));
        exit(2);
    }
    return fd;
}

static void keyboard(const char *path)
{
    printf("== keyboard\n");
    int fd = open_node(path);
    identity(fd);
    state(fd, "idle");

    struct input_event ev;
    printf("read(empty) %s\n", err((int)read(fd, &ev, sizeof ev)));
    key(KEY_A, 1);
    settle();
    state(fd, "A held, unread");
    char small[10];
    printf("read(10 bytes) %s\n", err((int)read(fd, small, sizeof small)));
    drain(fd, "A down", 1);
    key(KEY_A, 0);
    settle();
    drain(fd, "A up", 1);

    /* A reader that doesn't keep up. */
    for (int i = 0; i < 400; i++) {
        key(KEY_B, 1);
        key(KEY_B, 0);
    }
    key(KEY_C, 1);
    settle();
    state(fd, "C held after flood");
    drain(fd, "flood", 0);
    key(KEY_C, 0);
    settle();
    drain(fd, "C up", 1);

    /* Clock per reader. */
    int clk = CLOCK_REALTIME;
    printf("SCLOCKID(REALTIME) %s\n", err(ioctl(fd, EVIOCSCLOCKID, &clk)));
    key(KEY_D, 1);
    key(KEY_D, 0);
    settle();
    struct input_event e2[8];
    ssize_t n = read(fd, e2, sizeof e2);
    if (n >= (ssize_t)sizeof e2[0]) {
        int64_t t = ts_ns(&e2[0]);
        int64_t rt = clock_ns(CLOCK_REALTIME), mono = clock_ns(CLOCK_MONOTONIC);
        printf("REALTIME reader's timestamp is %s\n",
               llabs(t - rt) < 5000000000ll ? "realtime" : llabs(t - mono) < 5000000000ll ? "monotonic" : "other");
    }
    drain(fd, "D", 0);
    clk = CLOCK_MONOTONIC;
    ioctl(fd, EVIOCSCLOCKID, &clk);
    clk = 12345;
    printf("SCLOCKID(bogus) %s\n", err(ioctl(fd, EVIOCSCLOCKID, &clk)));

    /* Exclusive grab. */
    int fd2 = open_node(path);
    printf("GRAB(fd1) %s\n", err(ioctl(fd, EVIOCGRAB, (void *)1)));
    printf("GRAB(fd2) %s\n", err(ioctl(fd2, EVIOCGRAB, (void *)1)));
    printf("GRAB(fd1 again) %s\n", err(ioctl(fd, EVIOCGRAB, (void *)1)));
    key(KEY_E, 1);
    key(KEY_E, 0);
    settle();
    drain(fd, "grabbed fd1", 0);
    drain(fd2, "other fd2", 0);
    printf("UNGRAB(fd2) %s\n", err(ioctl(fd2, EVIOCGRAB, (void *)0)));
    printf("UNGRAB(fd1) %s\n", err(ioctl(fd, EVIOCGRAB, (void *)0)));
    key(KEY_F, 1);
    key(KEY_F, 0);
    settle();
    drain(fd2, "fd2 after ungrab", 0);
    drain(fd, "fd1 after ungrab", 0);

    /* Revoke. */
    printf("REVOKE(fd2) %s\n", err(ioctl(fd2, EVIOCREVOKE, NULL)));
    key(KEY_G, 1);
    key(KEY_G, 0);
    settle();
    printf("revoked read %s\n", err((int)read(fd2, &ev, sizeof ev)));
    struct input_id id;
    printf("revoked GID %s\n", err(ioctl(fd2, EVIOCGID, &id)));
    struct pollfd p = {fd2, POLLIN, 0};
    poll(&p, 1, 0);
    printf("revoked poll revents %#x\n", p.revents);
    drain(fd, "fd1 after revoke", 0);
    close(fd2);
    close(fd);
}

static void touchscreen(const char *path)
{
    printf("== touchscreen\n");
    int fd = open_node(path);
    identity(fd);
    state(fd, "idle");
    touch_down(0, 7, 1000, 2000);
    settle();
    state(fd, "finger, unread");
    drain(fd, "down", 1);
    touch_down(1, 9, 3000, 4000);
    touch_move(0, 7, 1100, 2000);
    settle();
    drain(fd, "second finger, move", 1);
    state(fd, "two fingers");
    touch_move(0, 7, 1100, 2000); /* nothing changed */
    settle();
    drain(fd, "unchanged", 1);
    touch_up(0, 7);
    touch_up(1, 9);
    settle();
    drain(fd, "up", 1);
    state(fd, "lifted");
    close(fd);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *capsfile = argc > 2 ? argv[2] : "caps.bin";
    struct caps caps[2];
#ifdef REF_KERNEL
    FILE *f = fopen(capsfile, "rb");
    if (!f || fread(caps, sizeof caps, 1, f) != 1) {
        fprintf(stderr, "no %s\n", capsfile);
        return 2;
    }
    fclose(f);
    char kbd[64] = "", ts[64] = "";
    g_uinput[KBD] = create(&caps[KBD]);
    g_uinput[TOUCH] = create(&caps[TOUCH]);
    usleep(200000);
    node_of(g_uinput[KBD], kbd, sizeof kbd);
    node_of(g_uinput[TOUCH], ts, sizeof ts);
    keyboard(kbd);
    touchscreen(ts);
    /* Removal, kernel side only for now. */
    int fd = open_node(kbd);
    ioctl(g_uinput[KBD], UI_DEV_DESTROY);
    struct pollfd p = {fd, POLLIN, 0};
    poll(&p, 1, 100);
    struct input_event ev;
    printf("== removal\nremoved poll revents %#x\nremoved read %s\n", p.revents, err((int)read(fd, &ev, sizeof ev)));
    return 0;
#else
    const char *sock_path = getenv("CHAMELEON_SOCKET"), *dev_path = getenv("CHAMELEON_DRM_PATH");
    if (!sock_path || !dev_path) {
        fprintf(stderr, "run via run-conformance.sh\n");
        return 2;
    }
    int l = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, sock_path, sizeof addr.sun_path - 1);
    unlink(sock_path);
    bind(l, (struct sockaddr *)&addr, sizeof addr);
    listen(l, 1);
    int card = open(dev_path, O_RDWR | O_CLOEXEC); /* starts the shim's presenter link */
    struct pollfd p = {l, POLLIN, 0};
    if (card < 0 || poll(&p, 1, 5000) != 1) {
        fprintf(stderr, "no presenter link\n");
        return 2;
    }
    g_presenter = accept(l, NULL, NULL);
    struct cham_msg cfg = {CHAM_CONFIG, 0, 1080 | (uint64_t)1800 << 32, 60000};
    cham_send(g_presenter, &cfg, -1);
    usleep(200000);
    const char *kbd = "/dev/input/event1", *ts = "/dev/input/event0";
    if (argc > 1 && strcmp(argv[1], "dump") == 0) {
        for (int i = 0; i < 2; i++) {
            int fd = open_node(i == KBD ? kbd : ts);
            dump_caps(fd, &caps[i]);
            close(fd);
        }
        FILE *f = fopen(capsfile, "wb");
        fwrite(caps, sizeof caps, 1, f);
        fclose(f);
        return 0;
    }
    keyboard(kbd);
    touchscreen(ts);
    printf("== removal\n(Chameleon's devices can't be removed yet)\n");
    return 0;
#endif
}
