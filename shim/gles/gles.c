/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GLES side of the Chameleon glvnd vendor (libEGL_chameleon.so). glvnd's
 * libGLESv2.so.2 dispatches every GL call to the function cham_gl_get_proc()
 * returns: a forwarder to Android's libGLESv2.so (the vendor driver) that
 * records the call for the crash report, or one of these overrides:
 *
 *  - glShaderSource: makes KWin's Mesa-tested GLSL acceptable to strict
 *    drivers (glsl_fix.c).
 *  - glGetString/glGetStringi/glGetIntegerv(GL_NUM_EXTENSIONS): hide
 *    extensions whose driver paths misbehave. Default:
 *    GL_EXT_disjoint_timer_query - after KWin's per-frame GL_TIMESTAMP
 *    query, Mali-G77 (r32p1) dies with SIGBUS (NULL+0x29) in the next write
 *    into texture memory (the launcher also sets KWIN_NO_TIMER_QUERY=1), and
 *    GL_EXT_texture_format_BGRA8888, so KWin uploads plain RGBA (hidden
 *    while that crash was being hunted, kept as the well-trodden path).
 *    Override with CHAMELEON_GL_HIDE="ext1 ext2" (empty = hide nothing).
 *    One is added: GL_EXT_unpack_subimage on OpenGL ES 3.0+, where its
 *    GL_UNPACK_ROW_LENGTH/SKIP_* are core (same enums). KWin demands the
 *    name on any GLES, but most ES 3 drivers (all Adreno, PowerVR, newer
 *    Mali) no longer list it.
 *  - glTex(Sub)Image2D/3D: two precautions from the same hunt. Source data
 *    at a *tagged* heap pointer (0xb4...: Android sets the top byte, which
 *    the CPU ignores but a driver importing the memory may not) is copied to
 *    an untagged bounce buffer first (CHAMELEON_GL_BOUNCE=0 disables it),
 *    and uploads run with the framebuffer unbound, restored right after
 *    (CHAMELEON_GL_UPLOAD_UNBIND=0 disables it).
 */
#define _GNU_SOURCE
#include <GLES3/gl32.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

#include "../core/cham_shim.h"
#include "gles_internal.h"

static void (GL_APIENTRY *p_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
static const GLubyte *(GL_APIENTRY *p_glGetString)(GLenum);
static const GLubyte *(GL_APIENTRY *p_glGetStringi)(GLenum, GLuint);
static void (GL_APIENTRY *p_glGetIntegerv)(GLenum, GLint *);
static void (GL_APIENTRY *p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (GL_APIENTRY *p_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                             const void *);
static void (GL_APIENTRY *p_glTexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                          const void *);
static void (GL_APIENTRY *p_glTexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum,
                                             GLenum, const void *);
static void (GL_APIENTRY *p_glBindFramebuffer)(GLenum, GLuint);
static int g_bounce = 1;
static int g_unbind = 1;

__attribute__((constructor)) static void load(void)
{
    const char *path = getenv("CHAMELEON_ANDROID_GLES"); /* host tests */
    if (!path)
        path = sizeof(void *) == 8 ? "/system/lib64/libGLESv2.so" : "/system/lib/libGLESv2.so";
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
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
    *(void **)&p_glTexSubImage2D = dlsym(lib, "glTexSubImage2D");
    *(void **)&p_glTexImage3D = dlsym(lib, "glTexImage3D");
    *(void **)&p_glTexSubImage3D = dlsym(lib, "glTexSubImage3D");
    const char *bounce = getenv("CHAMELEON_GL_BOUNCE");
    g_bounce = !(bounce && *bounce == '0');
    *(void **)&p_glBindFramebuffer = dlsym(lib, "glBindFramebuffer");
    const char *unbind = getenv("CHAMELEON_GL_UPLOAD_UNBIND");
    g_unbind = !(unbind && *unbind == '0');
}

/* ---- hidden extensions ---- */

static int is_hidden(const char *name, size_t len)
{
    const char *list = getenv("CHAMELEON_GL_HIDE");
    if (!list)
        list = "GL_EXT_texture_format_BGRA8888 GL_EXT_disjoint_timer_query";
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

static int has_extension(const char *list, const char *name)
{
    size_t len = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += len)
        if ((p == list || p[-1] == ' ') && (p[len] == ' ' || p[len] == 0))
            return 1;
    return 0;
}

/* Extensions implied by the context's core version that KWin still asks
 * for by name. */
static const char *implied_extension(const char *all)
{
    const char *version = (const char *)p_glGetString(GL_VERSION);
    int major = 0;
    if (version && sscanf(version, "OpenGL ES %d", &major) == 1 && major >= 3 &&
        !has_extension(all, "GL_EXT_unpack_subimage"))
        return "GL_EXT_unpack_subimage";
    return NULL;
}

static void build_extensions_locked(void)
{
    if (g_ext_string)
        return;
    const char *all = (const char *)p_glGetString(GL_EXTENSIONS);
    if (!all)
        return; /* no context yet: try again next time */
    const char *added = implied_extension(all);
    g_ext_string = calloc(1, strlen(all) + (added ? strlen(added) + 1 : 0) + 1);
    size_t max = 2;
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
    if (added) {
        if (out != g_ext_string)
            *out++ = ' ';
        strcpy(out, added);
        g_ext_count++;
        cham_log("adding GL extension %s (core in OpenGL ES 3.0)", added);
    }
    /* glGetStringi needs NUL-terminated names: split a private copy */
    char *names = strdup(g_ext_string);
    char *save = NULL;
    GLint i = 0;
    for (char *t = strtok_r(names, " ", &save); t; t = strtok_r(NULL, " ", &save))
        g_ext_list[i++] = (const GLubyte *)t;
}

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

GL_APICALL const GLubyte *GL_APIENTRY glGetStringi(GLenum name, GLuint index)
{
    if (name != GL_EXTENSIONS)
        return p_glGetStringi(name, index);
    pthread_mutex_lock(&g_ext_lock);
    build_extensions_locked();
    const GLubyte *ret = g_ext_count < 0                   ? p_glGetStringi(name, index) /* no context yet */
                         : index < (GLuint)g_ext_count ? g_ext_list[index]
                                                       : NULL;
    pthread_mutex_unlock(&g_ext_lock);
    return ret;
}

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

/* ---- texture uploads ---- */

static size_t pixel_size(GLenum format, GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    }
    size_t component;
    switch (type) {
    case GL_UNSIGNED_BYTE:
    case GL_BYTE:
        component = 1;
        break;
    case GL_UNSIGNED_SHORT:
    case GL_SHORT:
    case GL_HALF_FLOAT:
    case 0x8D61: /* GL_HALF_FLOAT_OES */
        component = 2;
        break;
    case GL_UNSIGNED_INT:
    case GL_INT:
    case GL_FLOAT:
        component = 4;
        break;
    default:
        return 0;
    }
    switch (format) {
    case GL_RGBA:
    case GL_RGBA_INTEGER:
    case 0x80E1: /* GL_BGRA_EXT */
        return 4 * component;
    case GL_RGB:
    case GL_RGB_INTEGER:
        return 3 * component;
    case GL_RG:
    case GL_RG_INTEGER:
    case GL_LUMINANCE_ALPHA:
        return 2 * component;
    case GL_RED:
    case GL_RED_INTEGER:
    case GL_ALPHA:
    case GL_LUMINANCE:
    case GL_DEPTH_COMPONENT:
        return component;
    }
    return 0;
}

static int unpack_buffer_bound(void)
{
    GLint pbo = 0;
    p_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    return pbo != 0;
}

/* Bytes GL reads from client memory for an upload under the current unpack
 * state, 0 if unknown. */
static size_t upload_span(GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type)
{
    size_t bpp = pixel_size(format, type);
    if (!bpp || width <= 0 || height <= 0 || depth <= 0)
        return 0;
    GLint row_length = 0, skip_pixels = 0, skip_rows = 0, alignment = 4, image_height = 0, skip_images = 0;
    p_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    p_glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
    p_glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    p_glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    if (depth > 1) {
        p_glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &image_height);
        p_glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skip_images);
    }
    size_t row_pixels = (size_t)(row_length > 0 ? row_length : width);
    size_t stride = row_pixels * bpp;
    if (alignment > 1)
        stride = (stride + (size_t)alignment - 1) / (size_t)alignment * (size_t)alignment;
    size_t image = (size_t)(image_height > 0 ? image_height : height) * stride;
    size_t span = (size_t)(skip_images + depth - 1) * image + (size_t)(skip_rows + height - 1) * stride +
                  (size_t)(skip_pixels + width) * bpp;
    return span;
}

/* Returns data itself, or an untagged copy of everything GL will read from
 * it under the current unpack state. The copy lives until the next upload
 * on this thread (GL has consumed it by then: uploads copy synchronously). */
static const void *untagged(const void *data, GLsizei width, GLsizei height, GLsizei depth, GLenum format,
                            GLenum type)
{
    if (!g_bounce || !data || ((uintptr_t)data >> 56) == 0)
        return data; /* nothing to do, or already untagged */
    if (unpack_buffer_bound())
        return data; /* an offset into a buffer object, not a pointer */
    size_t span = upload_span(width, height, depth, format, type);
    if (!span)
        return data;

    static __thread void *buf;
    static __thread size_t buf_size;
    if (span > buf_size) {
        if (buf)
            munmap(buf, buf_size);
        size_t size = (span + 0xfffff) & ~(size_t)0xfffff;
        buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf == MAP_FAILED) {
            buf = NULL;
            buf_size = 0;
            return data;
        }
        buf_size = size;
    }
    memcpy(buf, data, span);
    return buf;
}

struct fb_binding {
    GLint draw, read;
    int unbound;
};

static void unbind_framebuffer(struct fb_binding *b)
{
    b->draw = b->read = 0;
    b->unbound = 0;
    p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &b->draw);
    p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &b->read);
    if (g_unbind && p_glBindFramebuffer && (b->draw || b->read)) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        b->unbound = 1;
    }
}

static void restore_framebuffer(const struct fb_binding *b)
{
    if (!b->unbound)
        return;
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)b->draw);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)b->read);
}

GL_APICALL void GL_APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                         GLsizei height, GLint border, GLenum format, GLenum type,
                                         const void *pixels)
{
    const void *data = untagged(pixels, width, height, 1, format, type);
    struct fb_binding fb;
    unbind_framebuffer(&fb);
    CHAM_NOTE_CALL("glTexImage2D");
    cham_crash_note("glTexImage2D %dx%d internal 0x%x format 0x%x type 0x%x data %p (from %p) framebuffer %d%s",
                    width, height, internalformat, format, type, data, pixels, fb.draw,
                    fb.unbound ? " (unbound)" : "");
    p_glTexImage2D(target, level, internalformat, width, height, border, format, type, data);
    restore_framebuffer(&fb);
}

GL_APICALL void GL_APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                            GLsizei height, GLenum format, GLenum type, const void *pixels)
{
    const void *data = untagged(pixels, width, height, 1, format, type);
    struct fb_binding fb;
    unbind_framebuffer(&fb);
    CHAM_NOTE_CALL("glTexSubImage2D");
    cham_crash_note("glTexSubImage2D %dx%d at %d,%d format 0x%x type 0x%x data %p (from %p) framebuffer %d%s",
                    width, height, xoffset, yoffset, format, type, data, pixels, fb.draw,
                    fb.unbound ? " (unbound)" : "");
    p_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, data);
    restore_framebuffer(&fb);
}

GL_APICALL void GL_APIENTRY glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                         GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type,
                                         const void *pixels)
{
    const void *data = untagged(pixels, width, height, depth, format, type);
    struct fb_binding fb;
    unbind_framebuffer(&fb);
    CHAM_NOTE_CALL("glTexImage3D");
    p_glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, data);
    restore_framebuffer(&fb);
}

GL_APICALL void GL_APIENTRY glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                            GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                            const void *pixels)
{
    const void *data = untagged(pixels, width, height, depth, format, type);
    struct fb_binding fb;
    unbind_framebuffer(&fb);
    CHAM_NOTE_CALL("glTexSubImage3D");
    p_glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, data);
    restore_framebuffer(&fb);
}

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

/* ---- entry points for glvnd ---- */

#define OWN(name) {#name, (void *)name}
static const struct {
    const char *name;
    void *fn;
} k_own[] = {
    OWN(glShaderSource), OWN(glGetString),  OWN(glGetStringi),    OWN(glGetIntegerv),
    OWN(glTexImage2D),   OWN(glTexSubImage2D), OWN(glTexImage3D), OWN(glTexSubImage3D),
};

/* Our function for a core GLES 3.2 entry point, NULL for anything else
 * (extensions are resolved through EGL). */
void *cham_gl_get_proc(const char *name)
{
    for (size_t i = 0; i < sizeof k_own / sizeof k_own[0]; i++)
        if (strcmp(name, k_own[i].name) == 0)
            return k_own[i].fn;
    for (const struct cham_gl_entry *e = cham_gl_entries; e->name; e++)
        if (strcmp(name, e->name) == 0)
            return *e->fn ? e->wrapper : NULL;
    return NULL;
}
