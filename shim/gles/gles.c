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
 *    Two may be added, for names programs demand that the driver only
 *    provides under another name: GL_EXT_unpack_subimage on OpenGL ES 3.0+,
 *    where its GL_UNPACK_ROW_LENGTH/SKIP_* are core (same enums) - KWin
 *    demands it on any GLES, but most ES 3 drivers (all Adreno, PowerVR,
 *    newer Mali) no longer list it; and GL_OES_texture_border_clamp, which
 *    Xwayland's glamor demands, on OpenGL ES 3.2 or with
 *    GL_EXT_texture_border_clamp (same enums; Adreno lists only those).
 *  - glTex(Sub)Image2D/3D: two precautions from the same hunt. Source data
 *    at a *tagged* heap pointer (0xb4...: Android sets the top byte, which
 *    the CPU ignores but a driver importing the memory may not) is copied to
 *    an untagged bounce buffer first (CHAMELEON_GL_BOUNCE=0 disables it),
 *    and uploads run with the framebuffer unbound, restored right after
 *    (CHAMELEON_GL_UPLOAD_UNBIND=0 disables it).
 *  - glBindTexture/glDeleteTextures/glFlush/glFinish: textures standing in
 *    for another driver's buffer are refreshed from it when bound, once
 *    between flushes (see "buffers of other drivers" below).
 *    CHAMELEON_FOREIGN_UNLOCK=0 keeps their AHardwareBuffer locked instead of
 *    unlocking it after each copy: faster, but some drivers only show what
 *    was written once it is unlocked.
 */
#define _GNU_SOURCE
#include <GLES3/gl32.h>
#include <dlfcn.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../core/cham_shim.h"
#include "../vendor/vendor.h"
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
static void (GL_APIENTRY *p_glBindTexture)(GLenum, GLuint);
static void (GL_APIENTRY *p_glDeleteTextures)(GLsizei, const GLuint *);
static void (GL_APIENTRY *p_glPixelStorei)(GLenum, GLint);
static void (GL_APIENTRY *p_glBindBuffer)(GLenum, GLuint);
static void (GL_APIENTRY *p_glFlush)(void);
static void (GL_APIENTRY *p_glFinish)(void);
static GLsync (GL_APIENTRY *p_glFenceSync)(GLenum, GLbitfield);
static GLenum (GL_APIENTRY *p_glClientWaitSync)(GLsync, GLbitfield, GLuint64);
static void (GL_APIENTRY *p_glDeleteSync)(GLsync);
static int g_bounce = 1;
static int g_unbind = 1;
static int g_foreign_unlock = 1;

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
    *(void **)&p_glBindTexture = dlsym(lib, "glBindTexture");
    *(void **)&p_glDeleteTextures = dlsym(lib, "glDeleteTextures");
    *(void **)&p_glPixelStorei = dlsym(lib, "glPixelStorei");
    *(void **)&p_glBindBuffer = dlsym(lib, "glBindBuffer");
    *(void **)&p_glFlush = dlsym(lib, "glFlush");
    *(void **)&p_glFinish = dlsym(lib, "glFinish");
    *(void **)&p_glFenceSync = dlsym(lib, "glFenceSync");
    *(void **)&p_glClientWaitSync = dlsym(lib, "glClientWaitSync");
    *(void **)&p_glDeleteSync = dlsym(lib, "glDeleteSync");
    const char *unbind = getenv("CHAMELEON_GL_UPLOAD_UNBIND");
    g_unbind = !(unbind && *unbind == '0');
    const char *unlock = getenv("CHAMELEON_FOREIGN_UNLOCK");
    g_foreign_unlock = !(unlock && *unlock == '0');
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

/* Extensions the driver provides under another name (the context's core
 * version or another extension) that programs ask for by this one. */
static const struct {
    const char *name, *alias;
    int major, minor; /* core since OpenGL ES major.minor */
} k_implied[] = {
    {"GL_EXT_unpack_subimage", NULL, 3, 0},
    {"GL_OES_texture_border_clamp", "GL_EXT_texture_border_clamp", 3, 2},
};
#define N_IMPLIED (sizeof k_implied / sizeof k_implied[0])

static int implied(const char *all, size_t i)
{
    const char *version = (const char *)p_glGetString(GL_VERSION);
    int major = 0, minor = 0;
    if (!version || sscanf(version, "OpenGL ES %d.%d", &major, &minor) < 1)
        return 0;
    if (has_extension(all, k_implied[i].name))
        return 0;
    return major > k_implied[i].major || (major == k_implied[i].major && minor >= k_implied[i].minor) ||
           (k_implied[i].alias && has_extension(all, k_implied[i].alias));
}

static void build_extensions_locked(void)
{
    if (g_ext_string)
        return;
    const char *all = (const char *)p_glGetString(GL_EXTENSIONS);
    if (!all)
        return; /* no context yet: try again next time */
    int added[N_IMPLIED];
    size_t extra = 0;
    for (size_t i = 0; i < N_IMPLIED; i++) {
        added[i] = implied(all, i);
        extra += added[i] ? strlen(k_implied[i].name) + 1 : 0;
    }
    g_ext_string = calloc(1, strlen(all) + extra + 1);
    size_t max = 1 + N_IMPLIED;
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
    for (size_t i = 0; i < N_IMPLIED; i++) {
        if (!added[i])
            continue;
        if (out != g_ext_string)
            *out++ = ' ';
        strcpy(out, k_implied[i].name);
        out += strlen(out);
        g_ext_count++;
        cham_log("adding GL extension %s (provided by the driver under another name)", k_implied[i].name);
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

/* ---- buffers of other drivers ----
 * gbm_bo_import (Xwayland's DRI3 clients: panfrost, panvk, ... rendering on
 * their own) gives another driver's linear dmabuf an AHardwareBuffer of its
 * own (cham_bo.src_fd). Android's driver can't read that dmabuf, so a
 * texture made from the stand-in gets the client's pixels when it is bound,
 * which is when glamor copies a presented frame into the window: copied
 * straight into the AHardwareBuffer (same format and layout, linear), as
 * Android's glTexSubImage2D is slow at it (rearranges the pixels on the
 * CPU), else with glTexSubImage2D. The dmabuf is read without its cache
 * sync calls, as Termux:X11 has long read these buffers. Once between
 * flushes is enough: glamor may bind it several times for one copy, and a
 * client can only present a new frame in it after glamor flushed (Present
 * flushes right after each copy). */

struct foreign {
    void *image;    /* NULL once destroyed */
    GLuint texture; /* 0 until the image is bound to one, or once deleted */
    void *map;
    size_t size, offset;
    GLsizei width, height;
    GLint row_length; /* pixels */
    GLenum format;
    AHardwareBuffer *ahb; /* the stand-in, held; NULL: glTexSubImage2D instead */
    size_t ahb_stride;    /* bytes */
    void *locked;         /* its pixels while kept locked (CHAMELEON_FOREIGN_UNLOCK=0) */
    GLsync drawn;         /* the GPU's done with what was drawn from it before */
    int fresh; /* copied in since the last flush */
    struct foreign *next;
};

static pthread_mutex_t g_foreign_lock = PTHREAD_MUTEX_INITIALIZER;
static struct foreign *g_foreign;

static struct {
    void (*acquire)(AHardwareBuffer *);
    void (*release)(AHardwareBuffer *);
    int (*lock)(AHardwareBuffer *, uint64_t usage, int32_t fence, const void *rect, void **out);
    int (*unlock)(AHardwareBuffer *, int32_t *fence);
} g_ahb;

static int ahb_load(void)
{
    static int loaded = -1;
    if (loaded >= 0)
        return loaded;
    void *lib = dlopen(sizeof(void *) == 8 ? "/system/lib64/libnativewindow.so" : "/system/lib/libnativewindow.so",
                       RTLD_NOW | RTLD_LOCAL);
    if (lib) {
        *(void **)&g_ahb.acquire = dlsym(lib, "AHardwareBuffer_acquire");
        *(void **)&g_ahb.release = dlsym(lib, "AHardwareBuffer_release");
        *(void **)&g_ahb.lock = dlsym(lib, "AHardwareBuffer_lock");
        *(void **)&g_ahb.unlock = dlsym(lib, "AHardwareBuffer_unlock");
    }
    loaded = g_ahb.acquire && g_ahb.release && g_ahb.lock && g_ahb.unlock;
    return loaded;
}

void cham_gl_foreign_image(void *image, const struct cham_bo *bo)
{
    struct foreign *f = calloc(1, sizeof *f);
    if (!f)
        return;
    f->image = image;
    f->offset = bo->src_offset;
    f->width = (GLsizei)bo->width;
    f->height = (GLsizei)bo->height;
    f->row_length = (GLint)(bo->src_stride / 4);
    f->format = bo->format == DRM_FORMAT_ARGB8888 || bo->format == DRM_FORMAT_XRGB8888 ? 0x80E1 /* GL_BGRA_EXT */
                                                                                     : GL_RGBA;
    f->size = bo->src_offset + (size_t)bo->src_stride * bo->height;
    f->map = mmap(NULL, f->size, PROT_READ, MAP_SHARED, bo->src_fd, 0);
    if (f->map == MAP_FAILED) {
        cham_log("cannot map a %dx%d buffer from another driver: %s", f->width, f->height, strerror(errno));
        free(f);
        return;
    }
    if (bo->ahb && ahb_load()) {
        g_ahb.acquire(bo->ahb);
        f->ahb = bo->ahb;
        f->ahb_stride = bo->stride;
    }
    static int logged;
    if (!logged++)
        cham_log("buffers from another driver: copied into textures as they are drawn, %s%s (first: %dx%d)",
                 f->ahb ? "into their AHardwareBuffer" : "with glTexSubImage2D",
                 f->ahb && !g_foreign_unlock ? ", kept locked" : "", f->width, f->height);
    pthread_mutex_lock(&g_foreign_lock);
    f->next = g_foreign;
    g_foreign = f;
    pthread_mutex_unlock(&g_foreign_lock);
}

static void foreign_free(struct foreign *f)
{
    if (f->drawn)
        p_glDeleteSync(f->drawn);
    if (f->locked)
        g_ahb.unlock(f->ahb, NULL);
    if (f->ahb)
        g_ahb.release(f->ahb);
    munmap(f->map, f->size);
    free(f);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Copy times, logged every 5 s while copies happen. Under g_foreign_lock. */
static struct {
    uint64_t since, ns;
    unsigned count;
} g_copies;

static void note_copy(uint64_t start)
{
    uint64_t end = now_ns();
    if (!g_copies.since)
        g_copies.since = start;
    g_copies.ns += end - start;
    g_copies.count++;
    if (end - g_copies.since >= 5000000000ull) {
        cham_log("buffers from another driver: %u copies in the last %.0f s, %.2f ms each", g_copies.count,
                 (end - g_copies.since) / 1e9, g_copies.ns / 1e6 / g_copies.count);
        memset(&g_copies, 0, sizeof g_copies);
    }
}

/* Into the AHardwareBuffer, once the GPU is done with what was drawn from it
 * before (the last copy into a window, fenced at the flush after it). 0 if
 * it can't be locked. */
static int upload_ahb(struct foreign *f)
{
    if (f->drawn) {
        p_glClientWaitSync(f->drawn, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        p_glDeleteSync(f->drawn);
        f->drawn = NULL;
    }
    void *dst = f->locked;
    if (!dst && (g_ahb.lock(f->ahb, 0x30 /* AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN */, -1, NULL, &dst) != 0 || !dst))
        return 0;
    const char *src = (const char *)f->map + f->offset;
    size_t src_stride = (size_t)f->row_length * 4, row = (size_t)f->width * 4;
    if (src_stride == f->ahb_stride) {
        memcpy(dst, src, src_stride * (size_t)(f->height - 1) + row);
    } else {
        for (GLsizei y = 0; y < f->height; y++)
            memcpy((char *)dst + (size_t)y * f->ahb_stride, src + (size_t)y * src_stride, row);
    }
    if (g_foreign_unlock)
        g_ahb.unlock(f->ahb, NULL);
    else
        f->locked = dst;
    return 1;
}

/* Into the texture bound to GL_TEXTURE_2D, under default unpack state. */
static void upload_gl(struct foreign *f)
{
    GLint row_length = 0, alignment = 4, skip_pixels = 0, skip_rows = 0, pbo = 0;
    p_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    p_glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    p_glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
    p_glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    p_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, f->row_length);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    p_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    if (pbo)
        p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width, f->height, f->format, GL_UNSIGNED_BYTE,
                    (const char *)f->map + f->offset);
    if (pbo)
        p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)pbo);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    p_glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
    p_glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
}

/* Nothing if already done since the last flush. */
static void foreign_upload(struct foreign *f)
{
    if (f->fresh)
        return;
    f->fresh = 1;
    uint64_t start = now_ns();
    if (!f->ahb || !upload_ahb(f))
        upload_gl(f);
    note_copy(start);
}

/* Before a flush: a fence after what was drawn from the buffers copied in
 * since the last one, for their next copy to wait on. */
static void foreign_fence(void)
{
    if (!__atomic_load_n(&g_foreign, __ATOMIC_RELAXED) || !p_glFenceSync)
        return;
    pthread_mutex_lock(&g_foreign_lock);
    for (struct foreign *f = g_foreign; f; f = f->next) {
        if (f->fresh && f->ahb) {
            if (f->drawn)
                p_glDeleteSync(f->drawn);
            f->drawn = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
    }
    pthread_mutex_unlock(&g_foreign_lock);
}

/* After a flush, the clients may have drawn new frames. */
static void foreign_stale(void)
{
    if (!__atomic_load_n(&g_foreign, __ATOMIC_RELAXED))
        return;
    pthread_mutex_lock(&g_foreign_lock);
    for (struct foreign *f = g_foreign; f; f = f->next)
        f->fresh = 0;
    pthread_mutex_unlock(&g_foreign_lock);
}

void cham_gl_foreign_target(void *image)
{
    if (!__atomic_load_n(&g_foreign, __ATOMIC_RELAXED))
        return;
    pthread_mutex_lock(&g_foreign_lock);
    for (struct foreign *f = g_foreign; f; f = f->next) {
        if (f->image == image) {
            GLint texture = 0;
            p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            f->texture = (GLuint)texture; /* one texture per image, as glamor does */
            foreign_upload(f);
            break;
        }
    }
    pthread_mutex_unlock(&g_foreign_lock);
}

/* Entries matching `image` or one of `textures` lose that link; an entry
 * with neither left is freed. */
static void foreign_forget(void *image, GLsizei n, const GLuint *textures)
{
    if (!__atomic_load_n(&g_foreign, __ATOMIC_RELAXED))
        return;
    pthread_mutex_lock(&g_foreign_lock);
    for (struct foreign **p = &g_foreign; *p;) {
        struct foreign *f = *p;
        if (image && f->image == image)
            f->image = NULL;
        for (GLsizei i = 0; i < n; i++)
            if (f->texture && f->texture == textures[i])
                f->texture = 0;
        if (!f->image && !f->texture) {
            *p = f->next;
            foreign_free(f);
        } else {
            p = &f->next;
        }
    }
    pthread_mutex_unlock(&g_foreign_lock);
}

void cham_gl_foreign_image_gone(void *image)
{
    foreign_forget(image, 0, NULL);
}

GL_APICALL void GL_APIENTRY glBindTexture(GLenum target, GLuint texture)
{
    CHAM_NOTE_CALL("glBindTexture");
    p_glBindTexture(target, texture);
    if (target != GL_TEXTURE_2D || !texture || !__atomic_load_n(&g_foreign, __ATOMIC_RELAXED))
        return;
    pthread_mutex_lock(&g_foreign_lock);
    for (struct foreign *f = g_foreign; f; f = f->next) {
        if (f->texture == texture) {
            foreign_upload(f);
            break;
        }
    }
    pthread_mutex_unlock(&g_foreign_lock);
}

GL_APICALL void GL_APIENTRY glDeleteTextures(GLsizei n, const GLuint *textures)
{
    CHAM_NOTE_CALL("glDeleteTextures");
    foreign_forget(NULL, n, textures);
    p_glDeleteTextures(n, textures);
}

GL_APICALL void GL_APIENTRY glFlush(void)
{
    CHAM_NOTE_CALL("glFlush");
    foreign_fence();
    p_glFlush();
    foreign_stale();
}

GL_APICALL void GL_APIENTRY glFinish(void)
{
    CHAM_NOTE_CALL("glFinish");
    foreign_fence();
    p_glFinish();
    foreign_stale();
}

/* ---- entry points for glvnd ---- */

#define OWN(name) {#name, (void *)name}
static const struct {
    const char *name;
    void *fn;
} k_own[] = {
    OWN(glShaderSource), OWN(glGetString),  OWN(glGetStringi),    OWN(glGetIntegerv),
    OWN(glTexImage2D),   OWN(glTexSubImage2D), OWN(glTexImage3D), OWN(glTexSubImage3D),
    OWN(glBindTexture),  OWN(glDeleteTextures), OWN(glFlush), OWN(glFinish),
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
