/*
 * libGLESv2.so.2 replacement for kwin_wayland. Termux's libepoxy dlopen()s
 * this name (glvnd's soname); every entry point forwards to Android's
 * libGLESv2.so (the vendor driver), except:
 *
 *  - glShaderSource: makes KWin's Mesa-tested GLSL acceptable to strict
 *    drivers (glsl_fix.c).
 *  - glGetString/glGetStringi/glGetIntegerv(GL_NUM_EXTENSIONS): hide
 *    extensions whose driver paths misbehave. Default:
 *    GL_EXT_texture_format_BGRA8888 - KWin then uploads textures as plain
 *    RGBA instead of GL_BGRA_EXT, after Mali-G77 crashed (SIGBUS, NULL+0x29)
 *    inside glTexImage2D on the BGRA path. Override with
 *    CHAMELEON_GL_HIDE="ext1 ext2" (empty = hide nothing).
 *  - glTexImage2D: only to log uploads when CHAMELEON_GL_TRACE=1.
 */
#define _GNU_SOURCE
#include <GLES3/gl32.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../core/cham_shim.h"
#include "gles_internal.h"

static void (GL_APIENTRY *p_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
static const GLubyte *(GL_APIENTRY *p_glGetString)(GLenum);
static const GLubyte *(GL_APIENTRY *p_glGetStringi)(GLenum, GLuint);
static void (GL_APIENTRY *p_glGetIntegerv)(GLenum, GLint *);
static void (GL_APIENTRY *p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static int g_trace;

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
    *(void **)&p_glGetString = dlsym(lib, "glGetString");
    *(void **)&p_glGetStringi = dlsym(lib, "glGetStringi");
    *(void **)&p_glGetIntegerv = dlsym(lib, "glGetIntegerv");
    *(void **)&p_glTexImage2D = dlsym(lib, "glTexImage2D");
    const char *trace = getenv("CHAMELEON_GL_TRACE");
    g_trace = trace && *trace == '1';
}

/* ---- hidden extensions ---- */

static int is_hidden(const char *name, size_t len)
{
    const char *list = getenv("CHAMELEON_GL_HIDE");
    if (!list)
        list = "GL_EXT_texture_format_BGRA8888";
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        const char *e = p;
        while (*e && *e != ' ' && *e != ',')
            e++;
        if ((size_t)(e - p) == len && len && memcmp(p, name, len) == 0)
            return 1;
        p = e;
    }
    return 0;
}

/* Filled once, with a current context; the driver's list doesn't change. */
static pthread_mutex_t g_ext_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_ext_string;
static const GLubyte **g_ext_list;
static GLint g_ext_count = -1;

static void build_extensions_locked(void)
{
    if (g_ext_string)
        return;
    const char *all = (const char *)p_glGetString(GL_EXTENSIONS);
    if (!all)
        return; /* no context yet: try again next time */
    g_ext_string = calloc(1, strlen(all) + 1);
    size_t max = 1;
    for (const char *c = all; *c; c++)
        max += *c == ' ';
    g_ext_list = calloc(max, sizeof *g_ext_list);
    g_ext_count = 0;
    char *out = g_ext_string;
    for (const char *p = all; *p;) {
        while (*p == ' ')
            p++;
        const char *e = p;
        while (*e && *e != ' ')
            e++;
        size_t n = (size_t)(e - p);
        if (n && !is_hidden(p, n)) {
            if (out != g_ext_string)
                *out++ = ' ';
            g_ext_count++;
            memcpy(out, p, n);
            out += n;
        } else if (n) {
            cham_log("hiding GL extension %.*s", (int)n, p);
        }
        p = e;
    }
    /* glGetStringi needs NUL-terminated names: split a private copy */
    char *names = strdup(g_ext_string);
    char *save = NULL;
    GLint i = 0;
    for (char *t = strtok_r(names, " ", &save); t; t = strtok_r(NULL, " ", &save))
        g_ext_list[i++] = (const GLubyte *)t;
}

__attribute__((visibility("default")))
GL_APICALL const GLubyte *GL_APIENTRY glGetString(GLenum name)
{
    if (name != GL_EXTENSIONS)
        return p_glGetString(name);
    pthread_mutex_lock(&g_ext_lock);
    build_extensions_locked();
    const GLubyte *ret = g_ext_string ? (const GLubyte *)g_ext_string : p_glGetString(name);
    pthread_mutex_unlock(&g_ext_lock);
    return ret;
}

__attribute__((visibility("default")))
GL_APICALL const GLubyte *GL_APIENTRY glGetStringi(GLenum name, GLuint index)
{
    if (name != GL_EXTENSIONS)
        return p_glGetStringi(name, index);
    pthread_mutex_lock(&g_ext_lock);
    build_extensions_locked();
    const GLubyte *ret = g_ext_count >= 0 && index < (GLuint)g_ext_count ? g_ext_list[index] : NULL;
    pthread_mutex_unlock(&g_ext_lock);
    return ret;
}

__attribute__((visibility("default")))
GL_APICALL void GL_APIENTRY glGetIntegerv(GLenum pname, GLint *data)
{
    if (pname == GL_NUM_EXTENSIONS) {
        pthread_mutex_lock(&g_ext_lock);
        build_extensions_locked();
        GLint count = g_ext_count;
        pthread_mutex_unlock(&g_ext_lock);
        if (count >= 0) {
            *data = count;
            return;
        }
    }
    p_glGetIntegerv(pname, data);
}

__attribute__((visibility("default")))
GL_APICALL void GL_APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                         GLsizei height, GLint border, GLenum format, GLenum type,
                                         const void *pixels)
{
    if (g_trace) {
        GLint row_length = 0;
        p_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
        cham_log("glTexImage2D target 0x%x level %d internal 0x%x %dx%d format 0x%x type 0x%x row_length %d data %p",
                 target, level, internalformat, width, height, format, type, row_length, pixels);
    }
    p_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
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
