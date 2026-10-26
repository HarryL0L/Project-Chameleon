/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libchameleon_udev.so: the udev side of the device layer, LD_PRELOADed
 * over the libudev the programs link (libudev-zero on Termux).
 *
 * Chameleon is the device manager here. An Android app sees none of the
 * kernel's devices and can't open its uevent socket, so:
 *  - every udev monitor is Chameleon's own: a connection to the server that
 *    receives its uevents (so far the card's hotplug when the app's window
 *    changes), or, while no server runs, an eventfd that stays quiet. KWin's
 *    GpuManager and libinput both need a working monitor to start at all.
 *  - subsystems "input" and "drm" are Chameleon's: the input devices KWin's
 *    libchameleon.so serves (common/chameleon_evdev.h), as devices eventN
 *    with node /dev/input/eventN (evdev.c answers for those nodes), and
 *    the fake KMS card it answers for in KWin's process, card0, with its
 *    node /dev/dri/card0 - all while that server runs. Enumerations list
 *    them, followed by the real libudev's devices of every other subsystem
 *    (power_supply, backlight, ...), so a program can run with this
 *    library preloaded whatever it looks for.
 *  - devices and property lists that aren't Chameleon's (looked up by a
 *    path of the real system) pass through to the real libudev. Chameleon's
 *    objects start with a magic number to tell them apart.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "internal.h"

struct udev;
struct udev_device;
struct udev_enumerate;
struct udev_list_entry;

#define NEXT(name)                                     \
    static __typeof__(name) *next_##name;              \
    if (!next_##name)                                  \
        *(void **)&next_##name = cham_next(#name)

#define MAGIC_ENTRY 0x43484d4c454e5452ull  /* "CHMLENTR" */
#define MAGIC_DEVICE 0x43484d4c44455643ull /* "CHMLDEVC" */
#define MAX_PROPS 13
#define MAX_TAGS 3
#define MAX_FILTERS 4
#define MAX_LISTED (CHAM_EVDEV_MAX_DEVICES + 1) /* the input devices and the card */

#define CARD_SYSPATH "/sys/devices/platform/chameleon/drm/card0"

/* The card's tags, as systemd's rules give a GPU's card node (71-seat,
 * 70-uaccess); its rules tag no keyboard, mouse or touch event node. */
static const char *const k_card_tags[MAX_TAGS] = {"seat", "master-of-seat", "uaccess"};

struct entry {
    uint64_t magic;
    const char *name, *value;
    struct entry *next;
};

struct dev {
    uint64_t magic;
    int refs;
    struct udev *udev;
    const char *subsystem, *devtype, *sysnum;
    const char *action; /* a monitor's event, else NULL */
    unsigned long long seqnum;
    char seqnum_s[24];
    char sysname[16], syspath[4 + 64], devpath[64], devnode[108], major[8], minor[8], type[32];
    dev_t devnum;
    struct entry props[MAX_PROPS];
    int nprops;
    struct entry tags[MAX_TAGS];
    int ntags;
};

static int is_entry(const void *p)
{
    return p && *(const uint64_t *)p == MAGIC_ENTRY;
}

static int is_dev(const void *p)
{
    return p && *(const uint64_t *)p == MAGIC_DEVICE;
}

static void link_entries(struct entry *e, int n)
{
    for (int i = 0; i < n; i++)
        e[i].next = i + 1 < n ? &e[i + 1] : NULL;
}

/* ---- Chameleon's input devices, from KWin's libchameleon.so ---- */

static int connect_server(void)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (!dir || snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", dir, CHAM_EVDEV_SOCKET_NAME) >=
                    (int)sizeof addr.sun_path)
        return -1;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s >= 0 && connect(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(s);
        s = -1;
    }
    return s;
}

/* Reads the device list the server sends on connect; returns the count. */
static int read_list(int s, struct cham_evdev_info *out, int max)
{
    uint32_t count = 0;
    if (recv(s, &count, sizeof count, MSG_WAITALL) != (ssize_t)sizeof count || count > CHAM_EVDEV_MAX_DEVICES)
        return 0;
    for (uint32_t i = 0; i < count; i++) {
        struct cham_evdev_info info;
        if (recv(s, &info, sizeof info, MSG_WAITALL) != (ssize_t)sizeof info)
            return 0;
        info.name[sizeof info.name - 1] = info.udev_type[sizeof info.udev_type - 1] = '\0';
        if ((int)i < max)
            out[i] = info;
    }
    return (int)count < max ? (int)count : max;
}

static int server_running(void)
{
    int s = connect_server();
    if (s >= 0)
        close(s);
    return s >= 0;
}

int cham_card_present(void)
{
    return server_running();
}

int cham_evdev_list(struct cham_evdev_info *out, int max)
{
    int s = connect_server();
    if (s < 0)
        return 0;
    int n = read_list(s, out, max);
    close(s);
    return n;
}

int cham_evdev_open(int index, struct cham_evdev_info *info)
{
    struct cham_evdev_info list[CHAM_EVDEV_MAX_DEVICES];
    uint32_t want = (uint32_t)index;
    int s = connect_server();
    if (s < 0)
        return -1;
    if (index < 0 || index >= read_list(s, list, CHAM_EVDEV_MAX_DEVICES) ||
        send(s, &want, sizeof want, MSG_NOSIGNAL) != (ssize_t)sizeof want) {
        close(s);
        return -1;
    }
    *info = list[index];
    return s;
}

int cham_evdev_index(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);
    if (!path || strncmp(path, prefix, n) != 0)
        return -1;
    char *end;
    long i = strtol(path + n, &end, 10);
    return end != path + n && !*end && i >= 0 && i < CHAM_EVDEV_MAX_DEVICES ? (int)i : -1;
}

static int index_of_syspath(const char *syspath)
{
    for (int i = 0; syspath && i < CHAM_EVDEV_MAX_DEVICES; i++) {
        char p[64];
        snprintf(p, sizeof p, "/sys/devices/virtual/input/input%d/event%d", i, i);
        if (strcmp(p, syspath) == 0)
            return i;
    }
    return -1;
}

static void describe(struct dev *d, struct udev *udev, int index, const struct cham_evdev_info *info)
{
    memset(d, 0, sizeof *d);
    d->magic = MAGIC_DEVICE;
    d->refs = 1;
    d->udev = udev;
    d->subsystem = "input";
    d->devnum = cham_evdev_devnum(index);
    snprintf(d->sysname, sizeof d->sysname, "event%d", index);
    d->sysnum = d->sysname + 5;
    snprintf(d->devpath, sizeof d->devpath, "/devices/virtual/input/input%d/event%d", index, index);
    snprintf(d->syspath, sizeof d->syspath, "/sys%s", d->devpath);
    snprintf(d->devnode, sizeof d->devnode, "/dev/input/event%d", index);
    snprintf(d->major, sizeof d->major, "%u", major(d->devnum));
    snprintf(d->minor, sizeof d->minor, "%u", minor(d->devnum));
    snprintf(d->type, sizeof d->type, "%s", info->udev_type);
    struct entry *p = d->props;
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "DEVPATH", d->devpath, NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "SUBSYSTEM", "input", NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "DEVNAME", d->devnode, NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "MAJOR", d->major, NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "MINOR", d->minor, NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "ID_INPUT", "1", NULL};
    char *save, *type = strtok_r(d->type, " ", &save);
    for (; type && d->nprops < MAX_PROPS; type = strtok_r(NULL, " ", &save))
        p[d->nprops++] = (struct entry){MAGIC_ENTRY, type, "1", NULL};
    link_entries(p, d->nprops);
}

/* The card, as the kernel and udev describe a GPU's primary node: under its
 * platform device, tagged for seat0 (logind and libseat look for these). */
static void describe_card(struct dev *d, struct udev *udev)
{
    memset(d, 0, sizeof *d);
    d->magic = MAGIC_DEVICE;
    d->refs = 1;
    d->udev = udev;
    d->subsystem = "drm";
    d->devtype = "drm_minor";
    d->devnum = makedev(226, CHAM_DRM_MINOR);
    const char *node = getenv("CHAMELEON_DRM_PATH");
    snprintf(d->devnode, sizeof d->devnode, "%s", node && *node ? node : CHAM_DRM_PATH);
    snprintf(d->sysname, sizeof d->sysname, "card0");
    d->sysnum = d->sysname + 4;
    snprintf(d->syspath, sizeof d->syspath, "%s", CARD_SYSPATH);
    snprintf(d->devpath, sizeof d->devpath, "%s", &CARD_SYSPATH[4]); /* without "/sys" */
    snprintf(d->major, sizeof d->major, "%u", major(d->devnum));
    snprintf(d->minor, sizeof d->minor, "%u", minor(d->devnum));
    struct entry *p = d->props;
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "DEVPATH", d->devpath, NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "SUBSYSTEM", "drm", NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "DEVNAME", d->devnode, NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "DEVTYPE", "drm_minor", NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "MAJOR", d->major, NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "MINOR", d->minor, NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "ID_PATH", "platform-chameleon", NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "ID_FOR_SEAT", "drm-platform-chameleon", NULL};
    p[d->nprops++] = (struct entry){MAGIC_ENTRY, "TAGS", ":seat:master-of-seat:uaccess:", NULL};
    link_entries(p, d->nprops);
    for (; d->ntags < MAX_TAGS; d->ntags++)
        d->tags[d->ntags] = (struct entry){MAGIC_ENTRY, k_card_tags[d->ntags], NULL, NULL};
    link_entries(d->tags, d->ntags);
}

static struct udev_device *new_card(struct udev *udev)
{
    if (!server_running())
        return NULL;
    struct dev *d = malloc(sizeof *d);
    if (d)
        describe_card(d, udev);
    return (struct udev_device *)d;
}

/* Device `index` if KWin serves it now, else NULL. */
static struct udev_device *new_dev(struct udev *udev, int index)
{
    struct cham_evdev_info list[CHAM_EVDEV_MAX_DEVICES];
    if (index < 0 || index >= cham_evdev_list(list, CHAM_EVDEV_MAX_DEVICES))
        return NULL;
    struct dev *d = malloc(sizeof *d);
    if (d)
        describe(d, udev, index, &list[index]);
    return (struct udev_device *)d;
}

static const char *prop(const struct dev *d, const char *key)
{
    for (int i = 0; key && i < d->nprops; i++)
        if (strcmp(d->props[i].name, key) == 0)
            return d->props[i].value;
    return NULL;
}

/* ---- udev_device ---- */

EXPORT struct udev_device *udev_device_new_from_syspath(struct udev *udev, const char *syspath)
{
    if (syspath && strcmp(syspath, CARD_SYSPATH) == 0)
        return new_card(udev);
    int i = index_of_syspath(syspath);
    if (i >= 0)
        return new_dev(udev, i);
    NEXT(udev_device_new_from_syspath);
    return next_udev_device_new_from_syspath ? next_udev_device_new_from_syspath(udev, syspath) : NULL;
}

EXPORT struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type, dev_t devnum)
{
    if (type == 'c' && major(devnum) == 13 && minor(devnum) >= CHAM_EVDEV_MINOR_BASE &&
        minor(devnum) < CHAM_EVDEV_MINOR_BASE + CHAM_EVDEV_MAX_DEVICES)
        return new_dev(udev, (int)minor(devnum) - CHAM_EVDEV_MINOR_BASE);
    if (type == 'c' && devnum == makedev(226, CHAM_DRM_MINOR))
        return new_card(udev);
    NEXT(udev_device_new_from_devnum);
    return next_udev_device_new_from_devnum ? next_udev_device_new_from_devnum(udev, type, devnum) : NULL;
}

EXPORT struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *udev, const char *subsystem,
                                                                  const char *sysname)
{
    int i = subsystem && strcmp(subsystem, "input") == 0 ? cham_evdev_index(sysname, "event") : -1;
    if (i >= 0)
        return new_dev(udev, i);
    if (subsystem && sysname && strcmp(subsystem, "drm") == 0 && strcmp(sysname, "card0") == 0)
        return new_card(udev);
    NEXT(udev_device_new_from_subsystem_sysname);
    return next_udev_device_new_from_subsystem_sysname
               ? next_udev_device_new_from_subsystem_sysname(udev, subsystem, sysname)
               : NULL;
}

/* For a getter with one device argument: Chameleon's answer, else the real one. */
#define DEVICE_GETTER(ret, name, ours)               \
    EXPORT ret name(struct udev_device *device)      \
    {                                                \
        if (is_dev(device)) {                        \
            const struct dev *d = (const void *)device; \
            (void)d;                                 \
            return ours;                             \
        }                                            \
        NEXT(name);                                  \
        return next_##name(device);                  \
    }

DEVICE_GETTER(struct udev *, udev_device_get_udev, d->udev)
DEVICE_GETTER(const char *, udev_device_get_syspath, d->syspath)
DEVICE_GETTER(const char *, udev_device_get_sysname, d->sysname)
DEVICE_GETTER(const char *, udev_device_get_sysnum, d->sysnum)
DEVICE_GETTER(const char *, udev_device_get_devpath, d->devpath)
DEVICE_GETTER(const char *, udev_device_get_devnode, d->devnode)
DEVICE_GETTER(dev_t, udev_device_get_devnum, d->devnum)
DEVICE_GETTER(const char *, udev_device_get_subsystem, d->subsystem)
DEVICE_GETTER(const char *, udev_device_get_devtype, d->devtype)
DEVICE_GETTER(const char *, udev_device_get_driver, NULL)
DEVICE_GETTER(const char *, udev_device_get_action, d->action)
DEVICE_GETTER(unsigned long long, udev_device_get_seqnum, d->seqnum)
DEVICE_GETTER(struct udev_device *, udev_device_get_parent, NULL)
DEVICE_GETTER(int, udev_device_get_is_initialized, 1)
DEVICE_GETTER(struct udev_list_entry *, udev_device_get_properties_list_entry, (struct udev_list_entry *)d->props)
DEVICE_GETTER(struct udev_list_entry *, udev_device_get_devlinks_list_entry, NULL)
DEVICE_GETTER(struct udev_list_entry *, udev_device_get_tags_list_entry,
              d->ntags ? (struct udev_list_entry *)d->tags : NULL)
DEVICE_GETTER(struct udev_list_entry *, udev_device_get_sysattr_list_entry, NULL)

EXPORT struct udev_device *udev_device_ref(struct udev_device *device)
{
    if (is_dev(device)) {
        ((struct dev *)device)->refs++;
        return device;
    }
    NEXT(udev_device_ref);
    return next_udev_device_ref(device);
}

EXPORT struct udev_device *udev_device_unref(struct udev_device *device)
{
    if (is_dev(device)) {
        struct dev *d = (struct dev *)device;
        if (--d->refs == 0) {
            d->magic = 0;
            free(d);
        }
        return NULL;
    }
    NEXT(udev_device_unref);
    return next_udev_device_unref ? next_udev_device_unref(device) : NULL;
}

EXPORT struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *device,
                                                                         const char *subsystem, const char *devtype)
{
    if (is_dev(device))
        return NULL;
    NEXT(udev_device_get_parent_with_subsystem_devtype);
    return next_udev_device_get_parent_with_subsystem_devtype(device, subsystem, devtype);
}

EXPORT const char *udev_device_get_property_value(struct udev_device *device, const char *key)
{
    if (is_dev(device))
        return prop((struct dev *)device, key);
    NEXT(udev_device_get_property_value);
    return next_udev_device_get_property_value(device, key);
}

EXPORT const char *udev_device_get_sysattr_value(struct udev_device *device, const char *sysattr)
{
    if (is_dev(device))
        return NULL;
    NEXT(udev_device_get_sysattr_value);
    return next_udev_device_get_sysattr_value(device, sysattr);
}

EXPORT int udev_device_has_tag(struct udev_device *device, const char *tag)
{
    if (is_dev(device)) {
        const struct dev *d = (const void *)device;
        for (int i = 0; tag && i < d->ntags; i++)
            if (strcmp(d->tags[i].name, tag) == 0)
                return 1;
        return 0;
    }
    NEXT(udev_device_has_tag);
    return next_udev_device_has_tag(device, tag);
}

/* ---- udev_list_entry ---- */

EXPORT struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *entry)
{
    if (is_entry(entry))
        return (struct udev_list_entry *)((struct entry *)entry)->next;
    NEXT(udev_list_entry_get_next);
    return next_udev_list_entry_get_next(entry);
}

EXPORT const char *udev_list_entry_get_name(struct udev_list_entry *entry)
{
    if (is_entry(entry))
        return ((struct entry *)entry)->name;
    NEXT(udev_list_entry_get_name);
    return next_udev_list_entry_get_name(entry);
}

EXPORT const char *udev_list_entry_get_value(struct udev_list_entry *entry)
{
    if (is_entry(entry))
        return ((struct entry *)entry)->value;
    NEXT(udev_list_entry_get_value);
    return next_udev_list_entry_get_value(entry);
}

EXPORT struct udev_list_entry *udev_list_entry_get_by_name(struct udev_list_entry *entry, const char *name)
{
    for (; is_entry(entry); entry = (struct udev_list_entry *)((struct entry *)entry)->next)
        if (name && strcmp(((struct entry *)entry)->name, name) == 0)
            return entry;
    if (!entry)
        return NULL;
    NEXT(udev_list_entry_get_by_name); /* the real entries after Chameleon's */
    return next_udev_list_entry_get_by_name(entry, name);
}

/* ---- udev_enumerate: Chameleon's input devices and card, the real system's others ---- */

struct udev_enumerate {
    int refs;
    struct udev *udev;
    struct udev_enumerate *real; /* the real libudev's, which never lists "input" or "drm" */
    const char *subsystem[MAX_FILTERS], *nosubsystem[MAX_FILTERS], *sysname[MAX_FILTERS];
    const char *prop_key[MAX_FILTERS], *prop_value[MAX_FILTERS], *tag[MAX_FILTERS];
    int nsubsystem, nnosubsystem, nsysname, nprop, ntag;
    int unsupported; /* a filter Chameleon's devices can't pass (attributes, parents) */
    char paths[MAX_LISTED][sizeof ((struct dev *)0)->syspath];
    struct entry entries[MAX_LISTED];
    int n;
};

EXPORT struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *e);
EXPORT int udev_enumerate_add_nomatch_subsystem(struct udev_enumerate *e, const char *subsystem);

/* The real libudev's `name`(e->real, ...), or 0 without a real enumerate. */
#define REAL(name, ...)                                                        \
    ({                                                                         \
        NEXT(name);                                                            \
        e->real && next_##name ? next_##name(e->real, ##__VA_ARGS__) : 0;      \
    })

/* Chameleon's result, unless the real libudev failed. */
static int both(int real, int ours)
{
    return real < 0 ? real : ours;
}

static int add_filter(const char **list, int *n, const char *value)
{
    if (!value || *n >= MAX_FILTERS)
        return -EINVAL;
    list[(*n)++] = strdup(value);
    return 0;
}

EXPORT struct udev_enumerate *udev_enumerate_new(struct udev *udev)
{
    if (!udev)
        return NULL;
    struct udev_enumerate *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->refs = 1;
    e->udev = udev;
    NEXT(udev_enumerate_new);
    e->real = next_udev_enumerate_new ? next_udev_enumerate_new(udev) : NULL;
    /* The system's own input devices and GPUs: their nodes aren't Chameleon's. */
    if (REAL(udev_enumerate_add_nomatch_subsystem, "input") < 0 ||
        REAL(udev_enumerate_add_nomatch_subsystem, "drm") < 0) {
        NEXT(udev_enumerate_unref);
        next_udev_enumerate_unref(e->real);
        e->real = NULL;
    }
    return e;
}

EXPORT struct udev_enumerate *udev_enumerate_ref(struct udev_enumerate *e)
{
    if (e)
        e->refs++;
    return e;
}

EXPORT struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *e)
{
    if (e && --e->refs == 0) {
        REAL(udev_enumerate_unref);
        for (int i = 0; i < MAX_FILTERS; i++) {
            free((void *)e->subsystem[i]);
            free((void *)e->nosubsystem[i]);
            free((void *)e->sysname[i]);
            free((void *)e->prop_key[i]);
            free((void *)e->prop_value[i]);
            free((void *)e->tag[i]);
        }
        free(e);
    }
    return NULL;
}

EXPORT struct udev *udev_enumerate_get_udev(struct udev_enumerate *e)
{
    return e ? e->udev : NULL;
}

EXPORT int udev_enumerate_add_match_subsystem(struct udev_enumerate *e, const char *subsystem)
{
    if (!e)
        return -EINVAL;
    return both(REAL(udev_enumerate_add_match_subsystem, subsystem), add_filter(e->subsystem, &e->nsubsystem, subsystem));
}

EXPORT int udev_enumerate_add_nomatch_subsystem(struct udev_enumerate *e, const char *subsystem)
{
    if (!e)
        return -EINVAL;
    return both(REAL(udev_enumerate_add_nomatch_subsystem, subsystem),
                add_filter(e->nosubsystem, &e->nnosubsystem, subsystem));
}

EXPORT int udev_enumerate_add_match_sysname(struct udev_enumerate *e, const char *sysname)
{
    if (!e)
        return -EINVAL;
    return both(REAL(udev_enumerate_add_match_sysname, sysname), add_filter(e->sysname, &e->nsysname, sysname));
}

EXPORT int udev_enumerate_add_match_property(struct udev_enumerate *e, const char *key, const char *value)
{
    if (!e || !key)
        return -EINVAL;
    int r = REAL(udev_enumerate_add_match_property, key, value);
    if (r < 0 || add_filter(e->prop_key, &e->nprop, key) != 0)
        return r < 0 ? r : -EINVAL;
    e->prop_value[e->nprop - 1] = value ? strdup(value) : NULL;
    return 0;
}

EXPORT int udev_enumerate_add_match_is_initialized(struct udev_enumerate *e)
{
    if (!e)
        return -EINVAL;
    return both(REAL(udev_enumerate_add_match_is_initialized), 0); /* all of Chameleon's devices are */
}

EXPORT int udev_enumerate_add_match_sysattr(struct udev_enumerate *e, const char *sysattr, const char *value)
{
    if (!e)
        return -EINVAL;
    e->unsupported = 1;
    return both(REAL(udev_enumerate_add_match_sysattr, sysattr, value), 0);
}

EXPORT int udev_enumerate_add_nomatch_sysattr(struct udev_enumerate *e, const char *sysattr, const char *value)
{
    if (!e)
        return -EINVAL;
    e->unsupported = 1;
    return both(REAL(udev_enumerate_add_nomatch_sysattr, sysattr, value), 0);
}

EXPORT int udev_enumerate_add_match_tag(struct udev_enumerate *e, const char *tag)
{
    if (!e)
        return -EINVAL;
    return both(REAL(udev_enumerate_add_match_tag, tag), add_filter(e->tag, &e->ntag, tag));
}

EXPORT int udev_enumerate_add_match_parent(struct udev_enumerate *e, struct udev_device *parent)
{
    if (!e)
        return -EINVAL;
    e->unsupported = 1;
    if (is_dev(parent)) { /* Chameleon's devices have no children, real ones included */
        REAL(udev_enumerate_unref);
        e->real = NULL;
        return 0;
    }
    return both(REAL(udev_enumerate_add_match_parent, parent), 0);
}

EXPORT int udev_enumerate_add_syspath(struct udev_enumerate *e, const char *syspath)
{
    if (!e)
        return -EINVAL;
    if (index_of_syspath(syspath) < 0 && !(syspath && strcmp(syspath, CARD_SYSPATH) == 0))
        return e->real ? REAL(udev_enumerate_add_syspath, syspath) : -EINVAL;
    if (e->n >= MAX_LISTED)
        return -EINVAL;
    snprintf(e->paths[e->n], sizeof e->paths[e->n], "%s", syspath);
    e->entries[e->n] = (struct entry){MAGIC_ENTRY, e->paths[e->n], NULL, NULL};
    e->n++;
    link_entries(e->entries, e->n);
    return 0;
}

static int matches(const struct udev_enumerate *e, const struct dev *d)
{
    int ok = !e->nsubsystem;
    for (int i = 0; i < e->nsubsystem; i++)
        ok |= strcmp(e->subsystem[i], d->subsystem) == 0;
    for (int i = 0; i < e->nnosubsystem; i++)
        ok &= strcmp(e->nosubsystem[i], d->subsystem) != 0;
    int named = !e->nsysname;
    for (int i = 0; i < e->nsysname; i++)
        named |= fnmatch(e->sysname[i], d->sysname, 0) == 0;
    for (int i = 0; i < e->nprop; i++) {
        const char *v = prop(d, e->prop_key[i]);
        ok &= v && (!e->prop_value[i] || fnmatch(e->prop_value[i], v, 0) == 0);
    }
    for (int i = 0; i < e->ntag; i++) /* every tag, as with systemd's libudev */
        ok &= udev_device_has_tag((struct udev_device *)d, e->tag[i]);
    return ok && named && !e->unsupported;
}

EXPORT int udev_enumerate_scan_devices(struct udev_enumerate *e)
{
    if (!e)
        return -EINVAL;
    struct cham_evdev_info list[CHAM_EVDEV_MAX_DEVICES];
    int count = cham_evdev_list(list, CHAM_EVDEV_MAX_DEVICES);
    int card = server_running();
    e->n = 0;
    for (int i = 0; i < count + card; i++) {
        struct dev d;
        if (i < count)
            describe(&d, e->udev, i, &list[i]);
        else
            describe_card(&d, e->udev);
        if (!matches(e, &d))
            continue;
        snprintf(e->paths[e->n], sizeof e->paths[e->n], "%s", d.syspath);
        e->entries[e->n] = (struct entry){MAGIC_ENTRY, e->paths[e->n], NULL, NULL};
        e->n++;
    }
    link_entries(e->entries, e->n);
    /* libudev-zero fails on the parts of /sys Android hides; a real system
     * can read them all and succeeds, keeping what it found. */
    REAL(udev_enumerate_scan_devices);
    return 0;
}

EXPORT int udev_enumerate_scan_subsystems(struct udev_enumerate *e)
{
    if (!e)
        return -EINVAL;
    e->n = 0;
    return REAL(udev_enumerate_scan_subsystems);
}

/* Chameleon's entries, then the real ones (udev_list_entry_get_next tells them apart). */
EXPORT struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *e)
{
    if (!e)
        return NULL;
    struct udev_list_entry *real = REAL(udev_enumerate_get_list_entry);
    if (!e->n)
        return real;
    e->entries[e->n - 1].next = (struct entry *)real;
    return (struct udev_list_entry *)e->entries;
}

/* ---- udev_monitor: Chameleon's events only ---- */

struct udev_monitor {
    int refs;
    int fd;       /* an eventfd that stays quiet; the server connection from enable_receiving */
    int server;
    struct udev *udev;
    const char *subsystems[MAX_FILTERS]; /* none: everything */
    const char *tags[MAX_FILTERS];       /* none: everything, else devices with any of them */
    int nsubsystems, ntags;
};

EXPORT int udev_monitor_filter_remove(struct udev_monitor *m);

EXPORT struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev, const char *name)
{
    (void)name; /* "udev" or "kernel": Chameleon's events either way */
    if (!udev) {
        errno = EINVAL;
        return NULL;
    }
    struct udev_monitor *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m->fd < 0) {
        free(m);
        return NULL;
    }
    m->refs = 1;
    m->udev = udev;
    return m;
}

EXPORT struct udev_monitor *udev_monitor_ref(struct udev_monitor *m)
{
    if (m)
        m->refs++;
    return m;
}

EXPORT struct udev_monitor *udev_monitor_unref(struct udev_monitor *m)
{
    if (m && --m->refs == 0) {
        close(m->fd);
        udev_monitor_filter_remove(m);
        free(m);
    }
    return NULL;
}

EXPORT struct udev *udev_monitor_get_udev(struct udev_monitor *m)
{
    return m ? m->udev : NULL;
}

EXPORT int udev_monitor_get_fd(struct udev_monitor *m)
{
    return m ? m->fd : -EINVAL;
}

/* Connects to the server with the subsystems the filters let through; the
 * connection takes over the fd number the program already has. Filters
 * changed after this don't apply. */
EXPORT int udev_monitor_enable_receiving(struct udev_monitor *m)
{
    if (!m)
        return -EINVAL;
    if (m->server)
        return 0;
    uint32_t subsystems = m->nsubsystems ? 0 : CHAM_UEVENT_SUBSYSTEM_DRM | CHAM_UEVENT_SUBSYSTEM_INPUT;
    for (int i = 0; i < m->nsubsystems; i++)
        subsystems |= strcmp(m->subsystems[i], "drm") == 0     ? CHAM_UEVENT_SUBSYSTEM_DRM
                      : strcmp(m->subsystems[i], "input") == 0 ? CHAM_UEVENT_SUBSYSTEM_INPUT
                                                               : 0;
    int card_tagged = !m->ntags;
    for (int i = 0; i < m->ntags; i++)
        for (int t = 0; t < MAX_TAGS; t++)
            card_tagged |= strcmp(m->tags[i], k_card_tags[t]) == 0;
    if (m->ntags) /* input devices have no tags */
        subsystems &= card_tagged ? CHAM_UEVENT_SUBSYSTEM_DRM : 0;
    const uint32_t req[2] = {CHAM_EVDEV_MONITOR, subsystems};
    struct cham_evdev_info list[CHAM_EVDEV_MAX_DEVICES];
    int s = connect_server(); /* none running: the eventfd stays */
    if (s < 0)
        return 0;
    read_list(s, list, CHAM_EVDEV_MAX_DEVICES); /* the device list comes first */
    if (send(s, req, sizeof req, MSG_NOSIGNAL) == (ssize_t)sizeof req && dup3(s, m->fd, O_CLOEXEC) == m->fd) {
        fcntl(m->fd, F_SETFL, fcntl(m->fd, F_GETFL) | O_NONBLOCK);
        m->server = 1;
    }
    close(s);
    return 0;
}

EXPORT int udev_monitor_set_receive_buffer_size(struct udev_monitor *m, int size)
{
    (void)size;
    return m ? 0 : -EINVAL;
}

/* Filters narrow down Chameleon's events, which are few and all wanted. */
/* Chameleon's devices have one devtype per subsystem, so the subsystem
 * decides. */
EXPORT int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *m, const char *subsystem,
                                                           const char *devtype)
{
    (void)devtype;
    if (!m || !subsystem)
        return -EINVAL;
    return add_filter(m->subsystems, &m->nsubsystems, subsystem);
}

EXPORT int udev_monitor_filter_add_match_tag(struct udev_monitor *m, const char *tag)
{
    if (!m || !tag)
        return -EINVAL;
    return add_filter(m->tags, &m->ntags, tag);
}

EXPORT int udev_monitor_filter_update(struct udev_monitor *m)
{
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_filter_remove(struct udev_monitor *m)
{
    if (!m)
        return -EINVAL;
    for (int i = 0; i < m->nsubsystems; i++)
        free((void *)m->subsystems[i]);
    for (int i = 0; i < m->ntags; i++)
        free((void *)m->tags[i]);
    m->nsubsystems = m->ntags = 0;
    return 0;
}

/* The next event (the server applied the filters), or NULL (EAGAIN) once
 * none is waiting. */
EXPORT struct udev_device *udev_monitor_receive_device(struct udev_monitor *m)
{
    struct cham_evdev_record h;
    struct cham_uevent u;
    while (m && m->server && recv(m->fd, &h, sizeof h, 0) == (ssize_t)sizeof h) {
        if (h.type != CHAM_EVDEV_UEVENT || h.size != sizeof u || recv(m->fd, &u, sizeof u, MSG_WAITALL) != (ssize_t)sizeof u)
            break; /* not a stream we understand: no more events */
        if (u.what != CHAM_UEVENT_CARD_HOTPLUG)
            continue;
        struct dev *d = malloc(sizeof *d);
        if (!d)
            break;
        describe_card(d, m->udev);
        d->action = "change";
        d->seqnum = u.seqnum;
        snprintf(d->seqnum_s, sizeof d->seqnum_s, "%llu", (unsigned long long)u.seqnum);
        struct entry *p = d->props;
        p[d->nprops++] = (struct entry){MAGIC_ENTRY, "ACTION", "change", NULL};
        p[d->nprops++] = (struct entry){MAGIC_ENTRY, "SEQNUM", d->seqnum_s, NULL};
        p[d->nprops++] = (struct entry){MAGIC_ENTRY, "HOTPLUG", "1", NULL};
        link_entries(p, d->nprops);
        return (struct udev_device *)d;
    }
    errno = EAGAIN;
    return NULL;
}
