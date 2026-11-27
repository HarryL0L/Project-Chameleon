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
 *    extensions whose driver paths misbehave: GL_EXT_disjoint_timer_query
 *    when its timestamps can't be made right (see "GPU timestamps").
 *    Override with CHAMELEON_GL_HIDE="ext1 ext2" (empty = hide nothing).
 *    Some are added, for names programs demand that the driver only
 *    provides under another name (the context's core version or another
 *    extension, same enums and functions): GL_EXT_unpack_subimage on OpenGL
 *    ES 3.0+ (GL_UNPACK_ROW_LENGTH/SKIP_*) - KWin demands it on any GLES,
 *    but most ES 3 drivers (all Adreno, PowerVR, newer Mali) no longer list
 *    it; and, for Xwayland's glamor, GL_OES_texture_border_clamp on OpenGL
 *    ES 3.2 or with GL_EXT_texture_border_clamp (Adreno lists only those)
 *    and GL_OES_vertex_array_object on OpenGL ES 3.0+ (its *OES functions
 *    are the core ones then, see cham_gl_alias()).
 *  - GPU timestamps (GL_EXT_disjoint_timer_query): answered from Android
 *    fences, not the driver's clock (see "GPU timestamps").
 *  - glTex(Sub)Image2D/3D: two precautions from a crash hunt. Source data
 *    at a *tagged* heap pointer (0xb4...: Android sets the top byte, which
 *    the CPU ignores but a driver importing the memory may not) is copied to
 *    an untagged bounce buffer first (CHAMELEON_GL_BOUNCE=0 disables it),
 *    and uploads run with the framebuffer unbound, restored right after
 *    (CHAMELEON_GL_UPLOAD_UNBIND=0 disables it).
 */
#define _GNU_SOURCE
#include <GLES3/gl32.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

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
static int g_bounce = 1;
static int g_unbind = 1;
static void *g_lib;

__attribute__((constructor)) static void load(void)
{
    const char *path = getenv("CHAMELEON_ANDROID_GLES"); /* host tests */
    if (!path)
        path = sizeof(void *) == 8 ? "/system/lib64/libGLESv2.so" : "/system/lib/libGLESv2.so";
    void *lib = g_lib = cham_android_dlopen(path);
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

static int stamps_check(void);
static int g_stamps; /* timestamps come from fences (stamps_check passed) */

#define TIMER_QUERY "GL_EXT_disjoint_timer_query"

static int is_hidden(const char *name, size_t len)
{
    const char *list = getenv("CHAMELEON_GL_HIDE");
    if (!list) /* by default only the timer query, if its timestamps can't be made right */
        return len == strlen(TIMER_QUERY) && memcmp(name, TIMER_QUERY, len) == 0 && !g_stamps;
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
    {"GL_OES_vertex_array_object", NULL, 3, 0},
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
    if (has_extension(all, TIMER_QUERY))
        stamps_check(); /* decides whose timestamps apps get, listed or not */
    size_t extra = 0;
    int added[N_IMPLIED];
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
    if (pname == 0x8FBB /* GL_GPU_DISJOINT_EXT */ && g_stamps) {
        *data = 0; /* the monotonic clock doesn't jump */
        return;
    }
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

/* ---- GPU timestamps ----
 * Some drivers' GL_EXT_disjoint_timer_query timestamps are unusable. Mali-G77
 * r32p1 (MediaTek) answers glGetInteger64v(GL_TIMESTAMP) in raw ticks of the
 * 13 MHz system counter, and gives timestamp query results in nanoseconds
 * but through a float, in 4.2 ms steps after 16 hours of uptime. A
 * compositor that measures its GPU time with the two (KWin) gets hours.
 *
 * So timestamps are answered here: glQueryCounterEXT(GL_TIMESTAMP) puts an
 * EGL_ANDROID_native_fence_sync fence at that point of the command stream,
 * and the query's result is the time its fence signalled (the kernel's
 * dma-fence timestamp, CLOCK_MONOTONIC, from SYNC_IOC_FILE_INFO); the current
 * time is CLOCK_MONOTONIC, which never jumps (no disjoint). GL_TIME_ELAPSED
 * queries stay the driver's. All of it only if a fence reports a plausible
 * time once (stamps_check); otherwise the extension is hidden. Query names
 * are per context, so a timestamp is kept per (context, name), until
 * glDeleteQueries (a context destroyed without it leaves a few bytes). */

#define GL_TIMESTAMP_EXT 0x8E28

static struct {
    EGLDisplay (*display)(void);
    EGLContext (*context)(void);
    const char *(*query_string)(EGLDisplay, EGLint);
    EGLSyncKHR (*create)(EGLDisplay, EGLenum, const EGLint *);
    EGLBoolean (*destroy)(EGLDisplay, EGLSyncKHR);
    EGLint (*dup)(EGLDisplay, EGLSyncKHR);
    void (GL_APIENTRY *flush)(void);
    void (GL_APIENTRY *get_i64)(GLenum, GLint64 *);
    void (GL_APIENTRY *get_uiv)(GLuint, GLenum, GLuint *);
    void (GL_APIENTRY *delete)(GLsizei, const GLuint *);
    /* the extension's own entry points */
    void (GL_APIENTRY *counter)(GLuint, GLenum);
    void (GL_APIENTRY *get_i64_ext)(GLuint, GLenum, GLint64 *);
    void (GL_APIENTRY *get_ui64_ext)(GLuint, GLenum, GLuint64 *);
    void (GL_APIENTRY *get_iv_ext)(GLuint, GLenum, GLint *);
    void (GL_APIENTRY *get_uiv_ext)(GLuint, GLenum, GLuint *);
    void (GL_APIENTRY *get_int64_ext)(GLenum, GLint64 *);
    void (GL_APIENTRY *delete_ext)(GLsizei, const GLuint *);
    /* the host tests' stand-in for Android's GLES provides this; Android doesn't */
    int (*test_fence_time)(int fd, uint64_t *ns);
} G;

static void resolve_stamps(void)
{
    *(void **)&G.display = cham_egl_real("eglGetCurrentDisplay");
    *(void **)&G.context = cham_egl_real("eglGetCurrentContext");
    *(void **)&G.query_string = cham_egl_real("eglQueryString");
    *(void **)&G.create = cham_egl_real("eglCreateSyncKHR");
    *(void **)&G.destroy = cham_egl_real("eglDestroySyncKHR");
    *(void **)&G.dup = cham_egl_real("eglDupNativeFenceFDANDROID");
    *(void **)&G.counter = cham_egl_real("glQueryCounterEXT");
    *(void **)&G.get_i64_ext = cham_egl_real("glGetQueryObjecti64vEXT");
    *(void **)&G.get_ui64_ext = cham_egl_real("glGetQueryObjectui64vEXT");
    *(void **)&G.get_iv_ext = cham_egl_real("glGetQueryObjectivEXT");
    *(void **)&G.get_uiv_ext = cham_egl_real("glGetQueryObjectuivEXT");
    *(void **)&G.get_int64_ext = cham_egl_real("glGetInteger64vEXT");
    *(void **)&G.delete_ext = cham_egl_real("glDeleteQueriesEXT");
    if (g_lib) {
        *(void **)&G.flush = dlsym(g_lib, "glFlush");
        *(void **)&G.get_i64 = dlsym(g_lib, "glGetInteger64v");
        *(void **)&G.get_uiv = dlsym(g_lib, "glGetQueryObjectuiv");
        *(void **)&G.delete = dlsym(g_lib, "glDeleteQueries");
        *(void **)&G.test_fence_time = dlsym(g_lib, "cham_test_fence_time");
    }
}

static pthread_once_t g_stamps_once = PTHREAD_ONCE_INIT;

static uint64_t monotonic_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* A signalled sync file's signal time: 1 with *ns, 0 if not signalled, -1
 * if it can't be told. */
static int fence_time(int fd, uint64_t *ns)
{
    if (G.test_fence_time)
        return G.test_fence_time(fd, ns);
    struct sync_fence_info fences[8];
    struct sync_file_info info;
    memset(&info, 0, sizeof info);
    memset(fences, 0, sizeof fences);
    info.num_fences = 8;
    info.sync_fence_info = (uint64_t)(uintptr_t)fences;
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.status < 0)
        return -1;
    if (info.status == 0)
        return 0;
    uint64_t latest = 0;
    for (uint32_t i = 0; i < info.num_fences && i < 8; i++)
        if (fences[i].timestamp_ns > latest)
            latest = fences[i].timestamp_ns;
    *ns = latest;
    return latest ? 1 : -1;
}

/* Once, with a context current: do fences of this driver report when they
 * signalled, on CLOCK_MONOTONIC? */
static int stamps_check(void)
{
    static int checked, ok;
    if (checked)
        return ok;
    checked = 1;
    pthread_once(&g_stamps_once, resolve_stamps);
    EGLDisplay dpy = G.display ? G.display() : EGL_NO_DISPLAY;
    const char *exts = dpy != EGL_NO_DISPLAY && G.query_string ? G.query_string(dpy, EGL_EXTENSIONS) : NULL;
    if (!exts || !strstr(exts, "EGL_ANDROID_native_fence_sync") || !G.create || !G.destroy || !G.dup || !G.flush ||
        !G.counter || !G.get_i64 || !(G.get_i64_ext || G.get_ui64_ext)) {
        cham_log("hiding GL extension " TIMER_QUERY ": no native fences to time the GPU with");
        return 0;
    }
    uint64_t before = monotonic_ns(), ns = 0;
    EGLSyncKHR sync = G.create(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    G.flush();
    int fd = sync != EGL_NO_SYNC_KHR ? G.dup(dpy, sync) : -1;
    if (sync != EGL_NO_SYNC_KHR)
        G.destroy(dpy, sync);
    int r = -1;
    if (fd >= 0) {
        struct pollfd p = {fd, POLLIN, 0};
        r = poll(&p, 1, 1000) == 1 ? fence_time(fd, &ns) : -1;
        close(fd);
    }
    uint64_t after = monotonic_ns();
    ok = r == 1 && ns + 1000000 >= before && ns <= after + 1000000;
    if (ok)
        cham_log("GPU timestamps come from Android fences (not the driver's clock)");
    else
        cham_log("hiding GL extension " TIMER_QUERY ": fences report no usable time");
    g_stamps = ok;
    return ok;
}

struct stamp {
    struct stamp *next;
    EGLContext context;
    GLuint name;
    EGLDisplay dpy;
    EGLSyncKHR sync; /* until its time is known */
    uint64_t time;   /* 0 until known */
};
static pthread_mutex_t g_stamp_lock = PTHREAD_MUTEX_INITIALIZER;
static struct stamp *g_stamp_list;

/* The current context's timestamp of query `name`. A context is current on
 * one thread at a time, so only that thread uses or frees its stamps; the
 * lock guards the list. */
static struct stamp *stamp_find(GLuint name, int create)
{
    EGLContext ctx = G.context ? G.context() : EGL_NO_CONTEXT;
    pthread_mutex_lock(&g_stamp_lock);
    struct stamp *s = g_stamp_list;
    while (s && (s->context != ctx || s->name != name))
        s = s->next;
    if (!s && create && (s = calloc(1, sizeof *s))) {
        s->context = ctx;
        s->name = name;
        s->next = g_stamp_list;
        g_stamp_list = s;
    }
    pthread_mutex_unlock(&g_stamp_lock);
    return s;
}

static void stamp_drop_sync(struct stamp *s)
{
    if (s->sync != EGL_NO_SYNC_KHR)
        G.destroy(s->dpy, s->sync);
    s->sync = EGL_NO_SYNC_KHR;
}

/* Its time, waiting for the GPU if `wait`; 0 if not there yet. */
static uint64_t stamp_time(struct stamp *s, int wait)
{
    if (s->time || s->sync == EGL_NO_SYNC_KHR)
        return s->time;
    int fd = G.dup(s->dpy, s->sync);
    if (fd < 0) { /* the fence exists once the commands before it are flushed */
        G.flush();
        fd = G.dup(s->dpy, s->sync);
    }
    uint64_t ns = 0;
    if (fd >= 0) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, wait ? 2000 : 0) == 1 && fence_time(fd, &ns) != 1)
            ns = monotonic_ns(); /* signalled, time unknown: now is close */
        close(fd);
    }
    if (!ns && wait)
        ns = monotonic_ns(); /* no fence (or a hung GPU): don't block forever */
    if (ns) {
        s->time = ns;
        stamp_drop_sync(s);
    }
    return ns;
}

/* A timestamp query's GL_QUERY_RESULT(_AVAILABLE); 0 if `name` isn't one. */
static int stamp_value(GLuint name, GLenum pname, uint64_t *v)
{
    if (!g_stamps || (pname != GL_QUERY_RESULT && pname != GL_QUERY_RESULT_AVAILABLE))
        return 0;
    struct stamp *s = stamp_find(name, 0);
    if (!s)
        return 0;
    *v = pname == GL_QUERY_RESULT ? stamp_time(s, 1) : stamp_time(s, 0) != 0;
    return 1;
}

static void stamps_forget(GLsizei n, const GLuint *names)
{
    if (!g_stamps || n <= 0 || !names)
        return;
    EGLContext ctx = G.context ? G.context() : EGL_NO_CONTEXT;
    pthread_mutex_lock(&g_stamp_lock);
    for (GLsizei i = 0; i < n; i++) {
        for (struct stamp **p = &g_stamp_list; *p; p = &(*p)->next) {
            if ((*p)->context == ctx && (*p)->name == names[i]) {
                struct stamp *s = *p;
                *p = s->next;
                stamp_drop_sync(s);
                free(s);
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_stamp_lock);
}

static void GL_APIENTRY stamp_QueryCounterEXT(GLuint name, GLenum target)
{
    CHAM_NOTE_CALL("glQueryCounterEXT");
    EGLDisplay dpy = g_stamps && target == GL_TIMESTAMP_EXT ? G.display() : EGL_NO_DISPLAY;
    struct stamp *s = dpy != EGL_NO_DISPLAY ? stamp_find(name, 1) : NULL;
    if (!s) {
        G.counter(name, target);
        return;
    }
    stamp_drop_sync(s);
    s->dpy = dpy;
    s->time = 0;
    s->sync = G.create(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (s->sync == EGL_NO_SYNC_KHR)
        s->time = monotonic_ns();
}

static void GL_APIENTRY stamp_GetQueryObjecti64vEXT(GLuint name, GLenum pname, GLint64 *v)
{
    CHAM_NOTE_CALL("glGetQueryObjecti64vEXT");
    uint64_t value;
    if (stamp_value(name, pname, &value))
        *v = (GLint64)value;
    else if (G.get_i64_ext)
        G.get_i64_ext(name, pname, v);
}

static void GL_APIENTRY stamp_GetQueryObjectui64vEXT(GLuint name, GLenum pname, GLuint64 *v)
{
    CHAM_NOTE_CALL("glGetQueryObjectui64vEXT");
    uint64_t value;
    if (stamp_value(name, pname, &value))
        *v = value;
    else if (G.get_ui64_ext)
        G.get_ui64_ext(name, pname, v);
}

static void GL_APIENTRY stamp_GetQueryObjectivEXT(GLuint name, GLenum pname, GLint *v)
{
    CHAM_NOTE_CALL("glGetQueryObjectivEXT");
    uint64_t value;
    if (stamp_value(name, pname, &value))
        *v = (GLint)value; /* a 64-bit result in 32 bits: the low bits */
    else if (G.get_iv_ext)
        G.get_iv_ext(name, pname, v);
}

static void GL_APIENTRY stamp_GetQueryObjectuivEXT(GLuint name, GLenum pname, GLuint *v)
{
    CHAM_NOTE_CALL("glGetQueryObjectuivEXT");
    uint64_t value;
    if (stamp_value(name, pname, &value))
        *v = (GLuint)value;
    else if (G.get_uiv_ext)
        G.get_uiv_ext(name, pname, v);
}

static void GL_APIENTRY stamp_GetQueryObjectuiv(GLuint name, GLenum pname, GLuint *v)
{
    CHAM_NOTE_CALL("glGetQueryObjectuiv");
    uint64_t value;
    if (stamp_value(name, pname, &value))
        *v = (GLuint)value;
    else
        G.get_uiv(name, pname, v);
}

static void GL_APIENTRY stamp_GetInteger64v(GLenum pname, GLint64 *v)
{
    CHAM_NOTE_CALL("glGetInteger64v");
    if (pname == GL_TIMESTAMP_EXT && g_stamps)
        *v = (GLint64)monotonic_ns();
    else
        G.get_i64(pname, v);
}

static void GL_APIENTRY stamp_GetInteger64vEXT(GLenum pname, GLint64 *v)
{
    CHAM_NOTE_CALL("glGetInteger64vEXT");
    if (pname == GL_TIMESTAMP_EXT && g_stamps)
        *v = (GLint64)monotonic_ns();
    else
        G.get_int64_ext(pname, v);
}

static void GL_APIENTRY stamp_DeleteQueries(GLsizei n, const GLuint *names)
{
    CHAM_NOTE_CALL("glDeleteQueries");
    stamps_forget(n, names);
    G.delete(n, names);
}

static void GL_APIENTRY stamp_DeleteQueriesEXT(GLsizei n, const GLuint *names)
{
    CHAM_NOTE_CALL("glDeleteQueriesEXT");
    stamps_forget(n, names);
    G.delete_ext(n, names);
}

/* Ours for a core or extension entry point of the above, NULL if the driver
 * lacks it (or it's another function). */
static void *stamp_proc(const char *name)
{
    static const struct {
        const char *name;
        size_t real; /* offset of the driver's function in G */
        void *fn;
    } k[] = {
        {"glQueryCounterEXT", offsetof(__typeof__(G), counter), (void *)stamp_QueryCounterEXT},
        {"glGetQueryObjecti64vEXT", offsetof(__typeof__(G), get_i64_ext), (void *)stamp_GetQueryObjecti64vEXT},
        {"glGetQueryObjectui64vEXT", offsetof(__typeof__(G), get_ui64_ext), (void *)stamp_GetQueryObjectui64vEXT},
        {"glGetQueryObjectivEXT", offsetof(__typeof__(G), get_iv_ext), (void *)stamp_GetQueryObjectivEXT},
        {"glGetQueryObjectuivEXT", offsetof(__typeof__(G), get_uiv_ext), (void *)stamp_GetQueryObjectuivEXT},
        {"glGetInteger64vEXT", offsetof(__typeof__(G), get_int64_ext), (void *)stamp_GetInteger64vEXT},
        {"glDeleteQueriesEXT", offsetof(__typeof__(G), delete_ext), (void *)stamp_DeleteQueriesEXT},
        {"glGetQueryObjectuiv", offsetof(__typeof__(G), get_uiv), (void *)stamp_GetQueryObjectuiv},
        {"glGetInteger64v", offsetof(__typeof__(G), get_i64), (void *)stamp_GetInteger64v},
        {"glDeleteQueries", offsetof(__typeof__(G), delete), (void *)stamp_DeleteQueries},
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (strcmp(name, k[i].name) == 0) {
            pthread_once(&g_stamps_once, resolve_stamps);
            return *(void **)((char *)&G + k[i].real) ? k[i].fn : NULL;
        }
    }
    return NULL;
}

/* The extension's entry points, for the EGL side's eglGetProcAddress. */
void *cham_gl_stamp_proc(const char *name)
{
    return name[0] == 'g' && name[1] == 'l' ? stamp_proc(name) : NULL;
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
    void *stamp = stamp_proc(name);
    if (stamp)
        return stamp;
    for (const struct cham_gl_entry *e = cham_gl_entries; e->name; e++)
        if (strcmp(name, e->name) == 0)
            return *e->fn ? e->wrapper : NULL;
    return NULL;
}

/* The core function behind an *OES function of GL_OES_vertex_array_object
 * (added above on OpenGL ES 3.0, where the driver need not export the OES
 * names), or NULL. */
void *cham_gl_alias(const char *name)
{
    static const char *const vao[] = {"glBindVertexArrayOES", "glDeleteVertexArraysOES", "glGenVertexArraysOES",
                                      "glIsVertexArrayOES"};
    for (size_t i = 0; i < sizeof vao / sizeof vao[0]; i++)
        if (strcmp(name, vao[i]) == 0) {
            char core[32];
            snprintf(core, sizeof core, "%.*s", (int)(strlen(name) - 3), name);
            return cham_gl_get_proc(core);
        }
    return NULL;
}

/* Android's own GLES function, for the vendor's internal use. */
void *cham_gl_real(const char *name)
{
    return g_lib ? dlsym(g_lib, name) : NULL;
}
