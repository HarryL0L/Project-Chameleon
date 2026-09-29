/*
 * libGLESv2.so.2 replacement for kwin_wayland. Termux's libepoxy dlopen()s
 * this name (glvnd's soname); every entry point forwards to Android's
 * libGLESv2.so (the vendor driver), except glShaderSource, which first makes
 * KWin's Mesa-tested GLSL acceptable to strict drivers (glsl_fix.c).
 */
#define _GNU_SOURCE
#include <GLES3/gl32.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include "../core/cham_shim.h"
#include "gles_internal.h"

static void (GL_APIENTRY *p_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);

__attribute__((constructor)) static void load(void)
{
    void *lib = dlopen(sizeof(void *) == 8 ? "/system/lib64/libGLESv2.so" : "/system/lib/libGLESv2.so",
                       RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        cham_log("cannot load Android's libGLESv2: %s", dlerror());
        return;
    }
    for (const struct cham_gl_entry *e = cham_gl_entries; e->name; e++)
        *e->fn = dlsym(lib, e->name);
    *(void **)&p_glShaderSource = dlsym(lib, "glShaderSource");
}

__attribute__((visibility("default")))
GL_APICALL void GL_APIENTRY glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                                           const GLint *length)
{
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++)
        total += length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
    char *joined = malloc(total + 1), *p = joined;
    if (!joined) {
        p_glShaderSource(shader, count, string, length);
        return;
    }
    for (GLsizei i = 0; i < count; i++) {
        size_t n = length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
        memcpy(p, string[i], n);
        p += n;
    }
    *p = '\0';
    char *fixed = cham_fix_glsl(joined);
    const GLchar *src = fixed ? fixed : joined;
    p_glShaderSource(shader, 1, &src, NULL);
    free(fixed);
    free(joined);
}
