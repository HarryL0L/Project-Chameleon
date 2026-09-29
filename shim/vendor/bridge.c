/*
 * The vendor library's link to libchameleon.so, resolved at run time.
 *
 * libEGL_chameleon.so is loaded into every EGL program that glvnd routes to
 * it, while libchameleon.so (fake KMS device, gbm buffers, crash reporter)
 * is only LD_PRELOADed into kwin_wayland by chameleon-kwin. So the vendor
 * does not link against it: the few core functions the EGL code uses are
 * looked up with dlsym(RTLD_DEFAULT) and simply absent elsewhere (no dmabuf
 * import, which only makes sense for Chameleon's own gbm buffers anyway).
 *
 * The crash breadcrumbs (last GL/EGL calls) live here and are handed to
 * libchameleon's crash reporter when it is there.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../core/cham_shim.h"
#include "vendor.h"

const char *volatile cham_last_gl;
const char *volatile cham_call_ring[CHAM_CALL_RING];
volatile unsigned cham_call_pos;
static char g_note[256];

void cham_crash_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_note, sizeof g_note, fmt, ap);
    va_end(ap);
}

void cham_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "chameleon: %s\n", buf);
}

static struct {
    struct cham_bo *(*bo_from_fd)(int);
    AHardwareBuffer *(*bo_ahb)(struct cham_bo *);
    const uint32_t *(*formats)(int *);
} core;

static void resolve(void)
{
    *(void **)&core.bo_from_fd = dlsym(RTLD_DEFAULT, "cham_bo_from_fd");
    *(void **)&core.bo_ahb = dlsym(RTLD_DEFAULT, "cham_bo_ahb");
    *(void **)&core.formats = dlsym(RTLD_DEFAULT, "cham_formats");
    if (!core.bo_from_fd || !core.bo_ahb || !core.formats)
        memset(&core, 0, sizeof core);

    void (*attach)(const struct cham_crash_breadcrumbs *);
    *(void **)&attach = dlsym(RTLD_DEFAULT, "cham_crash_attach");
    if (attach) {
        static const struct cham_crash_breadcrumbs crumbs = {
            .last_call = &cham_last_gl,
            .ring = cham_call_ring,
            .pos = &cham_call_pos,
            .note = g_note,
        };
        attach(&crumbs);
    }
}

static void ensure_resolved(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, resolve);
}

int cham_core_present(void)
{
    ensure_resolved();
    return core.bo_from_fd != NULL;
}

struct cham_bo *cham_bo_from_fd(int fd)
{
    return cham_core_present() ? core.bo_from_fd(fd) : NULL;
}

AHardwareBuffer *cham_bo_ahb(struct cham_bo *bo)
{
    return cham_core_present() ? core.bo_ahb(bo) : NULL;
}

const uint32_t *cham_formats(int *count)
{
    if (cham_core_present())
        return core.formats(count);
    *count = 0;
    return NULL;
}
