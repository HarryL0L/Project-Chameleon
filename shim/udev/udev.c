/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libchameleon_udev.so: the udev side of the device layer, LD_PRELOADed
 * over the libudev the programs link (libudev-zero on Termux).
 *
 * Chameleon is the device manager here. An Android app sees none of the
 * kernel's devices and can't open its uevent socket, so:
 *  - every udev monitor is Chameleon's own: an eventfd only Chameleon will
 *    signal (nothing does yet). KWin's GpuManager and libinput both need a
 *    working monitor to start at all.
 *  - enumerations list Chameleon's devices only: the input devices KWin's
 *    libchameleon.so serves (common/chameleon_evdev.h), as subsystem
 *    "input" devices eventN with node /dev/input/eventN (evdev.c answers
 *    for those nodes).
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
        *(void **)&next_##name = dlsym(RTLD_NEXT, #name)

#define MAGIC_ENTRY 0x43484d4c454e5452ull  /* "CHMLENTR" */
#define MAGIC_DEVICE 0x43484d4c44455643ull /* "CHMLDEVC" */
#define MAX_PROPS 8
#define MAX_FILTERS 4

struct entry {
    uint64_t magic;
    const char *name, *value;
    struct entry *next;
};

struct dev {
    uint64_t magic;
    int refs;
    struct udev *udev;
    char sysname[16], syspath[4 + 64], devpath[64], devnode[32], major[8], minor[8], type[32];
    dev_t devnum;
    struct entry props[MAX_PROPS];
    int nprops;
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

int cham_evdev_list(struct cham_evdev_info *out, int max)
{
    int s = connect_server();
    if (s < 0)
        return 0;
    int n = read_list(s, out, max);
    close(s);
    return n;
}

int cham_evdev_open(int index, int nonblock, struct cham_evdev_info *info)
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
    if (nonblock)
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
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
    d->devnum = cham_evdev_devnum(index);
    snprintf(d->sysname, sizeof d->sysname, "event%d", index);
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
    if (d->type[0])
        p[d->nprops++] = (struct entry){MAGIC_ENTRY, d->type, "1", NULL};
    link_entries(p, d->nprops);
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
    NEXT(udev_device_new_from_devnum);
    return next_udev_device_new_from_devnum ? next_udev_device_new_from_devnum(udev, type, devnum) : NULL;
}

EXPORT struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *udev, const char *subsystem,
                                                                  const char *sysname)
{
    int i = subsystem && strcmp(subsystem, "input") == 0 ? cham_evdev_index(sysname, "event") : -1;
    if (i >= 0)
        return new_dev(udev, i);
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
DEVICE_GETTER(const char *, udev_device_get_sysnum, d->sysname + 5)
DEVICE_GETTER(const char *, udev_device_get_devpath, d->devpath)
DEVICE_GETTER(const char *, udev_device_get_devnode, d->devnode)
DEVICE_GETTER(dev_t, udev_device_get_devnum, d->devnum)
DEVICE_GETTER(const char *, udev_device_get_subsystem, "input")
DEVICE_GETTER(const char *, udev_device_get_devtype, NULL)
DEVICE_GETTER(const char *, udev_device_get_driver, NULL)
DEVICE_GETTER(const char *, udev_device_get_action, NULL)
DEVICE_GETTER(struct udev_device *, udev_device_get_parent, NULL)
DEVICE_GETTER(int, udev_device_get_is_initialized, 1)
DEVICE_GETTER(struct udev_list_entry *, udev_device_get_properties_list_entry, (struct udev_list_entry *)d->props)
DEVICE_GETTER(struct udev_list_entry *, udev_device_get_devlinks_list_entry, NULL)
DEVICE_GETTER(struct udev_list_entry *, udev_device_get_tags_list_entry, NULL)
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
    if (is_dev(device))
        return 0;
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
    if (is_entry(entry)) {
        for (struct entry *e = (struct entry *)entry; e; e = e->next)
            if (name && strcmp(e->name, name) == 0)
                return (struct udev_list_entry *)e;
        return NULL;
    }
    NEXT(udev_list_entry_get_by_name);
    return next_udev_list_entry_get_by_name(entry, name);
}

/* ---- udev_enumerate: Chameleon's devices only ---- */

struct udev_enumerate {
    int refs;
    struct udev *udev;
    const char *subsystem[MAX_FILTERS], *nosubsystem[MAX_FILTERS], *sysname[MAX_FILTERS];
    const char *prop_key[MAX_FILTERS], *prop_value[MAX_FILTERS];
    int nsubsystem, nnosubsystem, nsysname, nprop;
    int unsupported; /* a filter Chameleon's devices can't pass (tags, attributes, parents) */
    char paths[CHAM_EVDEV_MAX_DEVICES][sizeof ((struct dev *)0)->syspath];
    struct entry entries[CHAM_EVDEV_MAX_DEVICES];
    int n;
};

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
    if (e) {
        e->refs = 1;
        e->udev = udev;
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
        for (int i = 0; i < MAX_FILTERS; i++) {
            free((void *)e->subsystem[i]);
            free((void *)e->nosubsystem[i]);
            free((void *)e->sysname[i]);
            free((void *)e->prop_key[i]);
            free((void *)e->prop_value[i]);
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
    return e ? add_filter(e->subsystem, &e->nsubsystem, subsystem) : -EINVAL;
}

EXPORT int udev_enumerate_add_nomatch_subsystem(struct udev_enumerate *e, const char *subsystem)
{
    return e ? add_filter(e->nosubsystem, &e->nnosubsystem, subsystem) : -EINVAL;
}

EXPORT int udev_enumerate_add_match_sysname(struct udev_enumerate *e, const char *sysname)
{
    return e ? add_filter(e->sysname, &e->nsysname, sysname) : -EINVAL;
}

EXPORT int udev_enumerate_add_match_property(struct udev_enumerate *e, const char *key, const char *value)
{
    if (!e || !key || add_filter(e->prop_key, &e->nprop, key) != 0)
        return -EINVAL;
    e->prop_value[e->nprop - 1] = value ? strdup(value) : NULL;
    return 0;
}

EXPORT int udev_enumerate_add_match_is_initialized(struct udev_enumerate *e)
{
    return e ? 0 : -EINVAL; /* all of Chameleon's devices are */
}

#define UNUSED __attribute__((unused))
#define UNSUPPORTED_FILTER(name, ...)                     \
    EXPORT int name(struct udev_enumerate *e, __VA_ARGS__) \
    {                                                      \
        if (!e)                                            \
            return -EINVAL;                                \
        e->unsupported = 1;                                \
        return 0;                                          \
    }
UNSUPPORTED_FILTER(udev_enumerate_add_match_sysattr, const char *sysattr UNUSED, const char *value UNUSED)
UNSUPPORTED_FILTER(udev_enumerate_add_nomatch_sysattr, const char *sysattr UNUSED, const char *value UNUSED)
UNSUPPORTED_FILTER(udev_enumerate_add_match_tag, const char *tag UNUSED)
UNSUPPORTED_FILTER(udev_enumerate_add_match_parent, struct udev_device *parent UNUSED)

EXPORT int udev_enumerate_add_syspath(struct udev_enumerate *e, const char *syspath)
{
    if (!e || index_of_syspath(syspath) < 0 || e->n >= CHAM_EVDEV_MAX_DEVICES)
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
        ok |= strcmp(e->subsystem[i], "input") == 0;
    for (int i = 0; i < e->nnosubsystem; i++)
        ok &= strcmp(e->nosubsystem[i], "input") != 0;
    int named = !e->nsysname;
    for (int i = 0; i < e->nsysname; i++)
        named |= fnmatch(e->sysname[i], d->sysname, 0) == 0;
    for (int i = 0; i < e->nprop; i++) {
        const char *v = prop(d, e->prop_key[i]);
        ok &= v && (!e->prop_value[i] || fnmatch(e->prop_value[i], v, 0) == 0);
    }
    return ok && named && !e->unsupported;
}

EXPORT int udev_enumerate_scan_devices(struct udev_enumerate *e)
{
    if (!e)
        return -EINVAL;
    struct cham_evdev_info list[CHAM_EVDEV_MAX_DEVICES];
    int count = cham_evdev_list(list, CHAM_EVDEV_MAX_DEVICES);
    e->n = 0;
    for (int i = 0; i < count; i++) {
        struct dev d;
        describe(&d, e->udev, i, &list[i]);
        if (!matches(e, &d))
            continue;
        snprintf(e->paths[e->n], sizeof e->paths[e->n], "%s", d.syspath);
        e->entries[e->n] = (struct entry){MAGIC_ENTRY, e->paths[e->n], NULL, NULL};
        e->n++;
    }
    link_entries(e->entries, e->n);
    return 0;
}

EXPORT int udev_enumerate_scan_subsystems(struct udev_enumerate *e)
{
    if (!e)
        return -EINVAL;
    e->n = 0;
    return 0;
}

EXPORT struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *e)
{
    return e && e->n ? (struct udev_list_entry *)e->entries : NULL;
}

/* ---- udev_monitor: Chameleon's events only ---- */

struct udev_monitor {
    int refs;
    int fd;
    struct udev *udev;
};

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

EXPORT int udev_monitor_enable_receiving(struct udev_monitor *m)
{
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_set_receive_buffer_size(struct udev_monitor *m, int size)
{
    (void)size;
    return m ? 0 : -EINVAL;
}

/* Filters narrow down Chameleon's events, which are few and all wanted. */
EXPORT int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *m, const char *subsystem,
                                                           const char *devtype)
{
    (void)subsystem, (void)devtype;
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_filter_add_match_tag(struct udev_monitor *m, const char *tag)
{
    (void)tag;
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_filter_update(struct udev_monitor *m)
{
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_filter_remove(struct udev_monitor *m)
{
    return m ? 0 : -EINVAL;
}

EXPORT struct udev_device *udev_monitor_receive_device(struct udev_monitor *m)
{
    (void)m;
    errno = EAGAIN;
    return NULL;
}
