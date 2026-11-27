/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A stand-in for libadrenotools.so on a desktop: the one function the card's
 * Vulkan driver calls for CHAMELEON_VULKAN_DRIVER. It writes down what it was
 * asked ($FAKE_ADRENOTOOLS_LOG) and, like the real one, wants the driver file
 * to be at the directory and name given, joined as they are; then it opens
 * the stand-in for Android's Vulkan ($CHAMELEON_ANDROID_VULKAN) where the
 * real one opens its own copy of libvulkan.so. $FAKE_ADRENOTOOLS_FAIL: it
 * fails as the real one does where the linker can't load a hook library.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define S(s) ((s) ? (s) : "(none)")

__attribute__((visibility("default")))
void *adrenotools_open_libvulkan(int mode, int features, const char *tmp_dir, const char *hook_dir,
                                 const char *driver_dir, const char *driver_name, const char *redirect_dir,
                                 void **mapping)
{
    const char *log = getenv("FAKE_ADRENOTOOLS_LOG");
    FILE *f = log ? fopen(log, "a") : NULL;
    if (f) {
        fprintf(f, "features %d tmp %s hooks %s dir %s name %s redirect %s mapping %s\n", features, S(tmp_dir),
                S(hook_dir), S(driver_dir), S(driver_name), S(redirect_dir), mapping ? "given" : "(none)");
        fclose(f);
    }
    if (getenv("FAKE_ADRENOTOOLS_FAIL"))
        return dlopen("/nowhere/libhook_impl.so", mode);
    if (!driver_dir || !driver_name)
        return NULL;
    char path[4096];
    struct stat st;
    snprintf(path, sizeof path, "%s%s", driver_dir, driver_name);
    if (stat(path, &st) != 0)
        return NULL;
    const char *vulkan = getenv("CHAMELEON_ANDROID_VULKAN");
    return vulkan ? dlopen(vulkan, mode) : NULL;
}
