#ifndef CHAM_GLES_INTERNAL_H
#define CHAM_GLES_INTERNAL_H

struct cham_gl_entry {
    const char *name;
    void **fn;
};
extern const struct cham_gl_entry cham_gl_entries[];

/* Crash-report breadcrumbs, defined in libchameleon.so (core/crash.c). */
extern const char *volatile cham_last_gl;
void cham_crash_note(const char *fmt, ...);

/* Returns a malloc'd copy of a GLSL source with Mesa-only leniencies made
 * portable (see glsl_fix.c). */
char *cham_fix_glsl(const char *src);

#endif
