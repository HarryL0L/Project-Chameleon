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
 *  - glTex(Sub)Image2D/3D: Mali-G77 crashed (SIGBUS, NULL+0x29) in
 *    glTexImage2D on KWin's very first, perfectly ordinary RGBA upload whose
 *    data was a *tagged* heap pointer (0xb4...: Android sets the top byte,
 *    which the CPU ignores but a kernel driver importing the memory may
 *    not). Tagged source data is first copied to an untagged (mmap'd)
 *    bounce buffer. CHAMELEON_GL_BOUNCE=0 disables that.
 *    The crash kept happening intermittently even so. KWin uploads in the
 *    middle of a frame, with its AHB-backed output framebuffer bound (the
 *    probe's clean uploads never do), so uploads run with the framebuffer
 *    unbound and it is restored right after. CHAMELEON_GL_UPLOAD_UNBIND=0
 *    disables that; CHAMELEON_GL_TRACE=1 logs every upload.
 */
#define _GNU_SOURCE
#include <GLES3/gl32.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
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
static int g_trace;
static int g_bounce = 1;
static int g_unbind = 1;

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
    *(void **)&p_glTexSubImage2D = dlsym(lib, "glTexSubImage2D");
    *(void **)&p_glTexImage3D = dlsym(lib, "glTexImage3D");
    *(void **)&p_glTexSubImage3D = dlsym(lib, "glTexSubImage3D");
    const char *trace = getenv("CHAMELEON_GL_TRACE");
    g_trace = trace && *trace == '1';
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

/* Returns data itself, or an untagged copy of everything GL will read from
 * it under the current unpack state. The copy lives until the next upload
 * on this thread (GL has consumed it by then: uploads copy synchronously). */
static const void *untagged(const void *data, GLsizei width, GLsizei height, GLsizei depth, GLenum format,
                            GLenum type)
{
    if (!g_bounce || !data || ((uintptr_t)data >> 56) == 0)
        return data; /* nothing to do, or already untagged */
    GLint pbo = 0;
    p_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    if (pbo)
        return data; /* an offset into a buffer object, not a pointer */
    size_t bpp = pixel_size(format, type);
    if (!bpp || width <= 0 || height <= 0 || depth <= 0)
        return data;
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

static void trace_upload(const char *fn, GLenum internal, GLsizei w, GLsizei h, GLenum format, GLenum type,
                         const void *data, const void *used, const struct fb_binding *fb)
{
    GLint row_length = 0, texture = 0;
    p_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    cham_log("%s internal 0x%x %dx%d format 0x%x type 0x%x row_length %d data %p%s texture %d framebuffer %d%s", fn,
             internal, w, h, format, type, row_length, data, used != data ? " (bounced)" : "", texture, fb->draw,
             fb->unbound ? " (unbound for the upload)" : "");
}

__attribute__((visibility("default")))
GL_APICALL void GL_APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                         GLsizei height, GLint border, GLenum format, GLenum type,
                                         const void *pixels)
{
    const void *data = untagged(pixels, width, height, 1, format, type);
    struct fb_binding fb;
    unbind_framebuffer(&fb);
    if (g_trace)
        trace_upload("glTexImage2D", (GLenum)internalformat, width, height, format, type, pixels, data, &fb);
    cham_last_gl = "glTexImage2D";
    cham_crash_note("glTexImage2D %dx%d internal 0x%x format 0x%x type 0x%x data %p (from %p) framebuffer %d%s",
                    width, height, internalformat, format, type, data, pixels, fb.draw,
                    fb.unbound ? " (unbound)" : "");
    p_glTexImage2D(target, level, internalformat, width, height, border, format, type, data);
    restore_framebuffer(&fb);
}

__attribute__((visibility("default")))
GL_APICALL void GL_APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                            GLsizei height, GLenum format, GLenum type, const void *pixels)
{
    const void *data = untagged(pixels, width, height, 1, format, type);
    struct fb_binding fb;
    unbind_framebuffer(&fb);
    if (g_trace)
        trace_upload("glTexSubImage2D", 0, width, height, format, type, pixels, data, &fb);
    cham_last_gl = "glTexSubImage2D";
    cham_crash_note("glTexSubImage2D %dx%d at %d,%d format 0x%x type 0x%x data %p (from %p) framebuffer %d%s",
                    width, height, xoffset, yoffset, format, type, data, pixels, fb.draw,
                    fb.unbound ? " (unbound)" : "");
    p_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, data);
    restore_framebuffer(&fb);
}

__attribute__((visibility("default")))
GL_APICALL void GL_APIENTRY glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                         GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type,
                                         const void *pixels)
{
    const void *data = untagged(pixels, width, height, depth, format, type);
    struct fb_binding fb;
    unbind_framebuffer(&fb);
    if (g_trace)
        trace_upload("glTexImage3D", (GLenum)internalformat, width, height, format, type, pixels, data, &fb);
    cham_last_gl = "glTexImage3D";
    p_glTexImage3D(target, level, internalformat, width, height, depth, border, format, type, data);
    restore_framebuffer(&fb);
}

__attribute__((visibility("default")))
GL_APICALL void GL_APIENTRY glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                            GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                            const void *pixels)
{
    const void *data = untagged(pixels, width, height, depth, format, type);
    struct fb_binding fb;
    unbind_framebuffer(&fb);
    if (g_trace)
        trace_upload("glTexSubImage3D", 0, width, height, format, type, pixels, data, &fb);
    cham_last_gl = "glTexSubImage3D";
    p_glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, data);
    restore_framebuffer(&fb);
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
