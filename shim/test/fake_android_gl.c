/*
 * Stand-in for Android's /system/lib64/libEGL.so + libGLESv2.so in the host
 * test of the glvnd vendor: just enough EGL/GLES to be driven through
 * glvnd, recording what reaches "the driver" in `fake_state`.
 */
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>

#define API __attribute__((visibility("default")))

API struct {
    int robust_attrib_seen, client_version;
    char shader[512];
    int tex_uploads;
    int create_image_target;
    int query_counter_calls;
} fake_state;

static EGLDisplay const k_display = (EGLDisplay)0x1234;
static EGLContext const k_context = (EGLContext)0x5678;

API EGLDisplay eglGetDisplay(EGLNativeDisplayType d)
{
    return d == EGL_DEFAULT_DISPLAY ? k_display : EGL_NO_DISPLAY;
}
API EGLBoolean eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    if (major)
        *major = 1;
    if (minor)
        *minor = 5;
    return d == k_display;
}
API EGLint eglGetError(void) { return EGL_SUCCESS; }
API EGLBoolean eglBindAPI(EGLenum api) { return api == EGL_OPENGL_ES_API; }
API const char *eglQueryString(EGLDisplay d, EGLint name)
{
    (void)d;
    switch (name) {
    case EGL_EXTENSIONS:
        return "EGL_KHR_image_base EGL_ANDROID_native_fence_sync EGL_KHR_no_config_context "
               "EGL_KHR_surfaceless_context";
    case EGL_VENDOR:
        return "fake Android";
    case EGL_VERSION:
        return "1.5 fake";
    case EGL_CLIENT_APIS:
        return "OpenGL_ES";
    }
    return NULL;
}
API EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attribs)
{
    (void)d, (void)c, (void)share;
    for (const EGLint *a = attribs; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT)
            fake_state.robust_attrib_seen = 1;
        if (a[0] == EGL_CONTEXT_CLIENT_VERSION)
            fake_state.client_version = a[1];
    }
    return k_context;
}
API EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c)
{
    (void)dr, (void)rd, (void)c;
    return d == k_display;
}
API EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c)
{
    (void)d, (void)c;
    return EGL_TRUE;
}
API EGLImageKHR eglCreateImageKHR(EGLDisplay d, EGLContext c, EGLenum target, EGLClientBuffer b, const EGLint *a)
{
    (void)d, (void)c, (void)b, (void)a;
    fake_state.create_image_target = (int)target;
    return (EGLImageKHR)0x42;
}
API EGLint eglDupNativeFenceFDANDROID(EGLDisplay d, EGLSyncKHR s)
{
    (void)d, (void)s;
    return 42;
}
static void fake_query_counter(GLuint id, GLenum target)
{
    (void)id, (void)target;
    fake_state.query_counter_calls++;
}
API void (*eglGetProcAddress(const char *name))(void)
{
    if (strcmp(name, "glQueryCounterEXT") == 0)
        return (void (*)(void))fake_query_counter;
    return NULL;
}

/* ---- GLES ---- */

API const GLubyte *glGetString(GLenum name)
{
    if (name == GL_EXTENSIONS)
        return (const GLubyte *)"GL_OES_a GL_EXT_texture_format_BGRA8888 GL_EXT_disjoint_timer_query GL_OES_b";
    if (name == GL_VERSION)
        return (const GLubyte *)"OpenGL ES 3.2 fake";
    return (const GLubyte *)"fake";
}
API void glGetIntegerv(GLenum pname, GLint *v)
{
    (void)pname;
    *v = 0;
}
API void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *src, const GLint *len)
{
    (void)shader, (void)len;
    fake_state.shader[0] = 0;
    for (GLsizei i = 0; i < count; i++)
        strncat(fake_state.shader, src[i], sizeof fake_state.shader - strlen(fake_state.shader) - 1);
}
API void glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *p)
{
    (void)t, (void)l, (void)i, (void)w, (void)h, (void)b, (void)f, (void)ty, (void)p;
    fake_state.tex_uploads++;
}
API void glBindFramebuffer(GLenum t, GLuint f) { (void)t, (void)f; }
API void glClear(GLbitfield m) { (void)m; }
