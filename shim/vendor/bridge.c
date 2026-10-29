/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The vendor library's link to libchameleon.so, resolved at run time.
 *
 * libEGL_chameleon.so is loaded into every EGL program that glvnd routes to
 * it, while libchameleon.so (the card node, gbm buffers, crash reporter) is
 * LD_PRELOADed only into the programs of a Chameleon session. So the vendor
 * does not link against it: the few core functions the EGL code uses are
 * looked up with dlsym(RTLD_DEFAULT) and simply absent elsewhere (no dmabuf
 * import, which only makes sense for Chameleon's own gbm buffers anyway).
 *
 * The crash breadcrumbs (last GL/EGL calls) live here and are handed to
 * libchameleon's crash reporter once the process renders with Android's
 * driver.
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
    AHardwareBuffer *(*buffer_ahb_acquire)(int);
    void (*buffer_ahb_release)(AHardwareBuffer *);
    void *(*gbm_window_get)(void *, uint32_t *);
} core;

static void resolve(void)
{
    *(void **)&core.bo_from_fd = dlsym(RTLD_DEFAULT, "cham_bo_from_fd");
    *(void **)&core.bo_ahb = dlsym(RTLD_DEFAULT, "cham_bo_ahb");
    *(void **)&core.formats = dlsym(RTLD_DEFAULT, "cham_formats");
    *(void **)&core.buffer_ahb_acquire = dlsym(RTLD_DEFAULT, "cham_buffer_ahb_acquire");
    *(void **)&core.buffer_ahb_release = dlsym(RTLD_DEFAULT, "cham_buffer_ahb_release");
    *(void **)&core.gbm_window_get = dlsym(RTLD_DEFAULT, "cham_gbm_window_get");
    if (!core.bo_from_fd || !core.bo_ahb || !core.formats || !core.buffer_ahb_acquire || !core.buffer_ahb_release)
        memset(&core, 0, sizeof core);
}

static void arm(void)
{
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

void cham_crash_arm(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, arm);
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

AHardwareBuffer *cham_buffer_ahb_acquire(int fd)
{
    return cham_core_present() ? core.buffer_ahb_acquire(fd) : NULL;
}

void cham_buffer_ahb_release(AHardwareBuffer *b)
{
    if (b && cham_core_present())
        core.buffer_ahb_release(b);
}

void *cham_gbm_window(void *native, uint32_t *format)
{
    return cham_core_present() && core.gbm_window_get ? core.gbm_window_get(native, format) : NULL;
}

const uint32_t *cham_formats(int *count)
{
    if (cham_core_present())
        return core.formats(count);
    *count = 0;
    return NULL;
}
