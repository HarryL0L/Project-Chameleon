/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A stand-in for Termux's libtermux-platform-ns.so on a desktop: the one
 * function Chameleon opens Android's libraries with. It writes down the name
 * of each library it is asked for ($FAKE_PLATFORM_NS_LOG) and opens it the
 * plain way, where the real one opens it in a linker namespace of its own.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((visibility("default")))
void *platform_dlopen(const char *path, int flags)
{
    const char *log = getenv("FAKE_PLATFORM_NS_LOG");
    FILE *f = log ? fopen(log, "a") : NULL;
    if (f) {
        const char *name = strrchr(path, '/');
        fprintf(f, "%s\n", name ? name + 1 : path);
        fclose(f);
    }
    return dlopen(path, flags);
}
