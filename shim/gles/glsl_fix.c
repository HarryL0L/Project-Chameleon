/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GLSL source fix-ups for strict (non-Mesa) drivers.
 *
 * GLSL ES says an identifier in #if/#elif that isn't a defined macro is an
 * error. Mesa treats it as 0, so KWin's shaders use e.g.
 *     #if GL_OES_standard_derivatives && TRAIT_ROUNDED_CORNERS
 * which Mali rejects in "#version 300 es" (where that extension macro isn't
 * defined, derivatives being core). Every bare GL_* name in an #if/#elif is
 * rewritten to defined(GL_*): for extension macros (defined as 1) that's
 * the same value, and it is valid everywhere.
 *
 * GLSL ES 3.00 also has no texture2D(), only texture(). KWin's blur
 * (downsample.frag) and colour-blindness shaders still call texture2D(),
 * which Mali accepts but Adreno rightly rejects, and a failed blur shader
 * leaves KWin crashing on its next reconfigure. So in "#version 300 es"
 * shaders texture2D( becomes texture(, unless the shader uses
 * samplerExternalOES, where texture2D() is what GL_OES_EGL_image_external
 * defines.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "gles_internal.h"

static int is_ident(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* Copies [s, e) into out with each texture2D( call renamed texture(. */
static char *fix_texture2d(char *out, const char *s, const char *e)
{
    while (s < e) {
        if ((size_t)(e - s) > 9 && memcmp(s, "texture2D", 9) == 0 && !is_ident(s[9])) {
            const char *t = s + 9;
            while (t < e && (*t == ' ' || *t == '\t'))
                t++;
            if (t < e && *t == '(') {
                memcpy(out, "texture", 7);
                out += 7;
                s += 9;
                continue;
            }
        }
        if (is_ident(*s)) { /* copy the whole identifier, so foo_texture2D stays */
            while (s < e && is_ident(*s))
                *out++ = *s++;
            continue;
        }
        *out++ = *s++;
    }
    return out;
}

/* Rewrites one #if/#elif expression [s, e) into out. */
static char *fix_condition(char *out, const char *s, const char *e)
{
    int after_defined = 0; /* previous token was `defined` (maybe with '(') */
    while (s < e) {
        if (is_ident(*s) && !isdigit((unsigned char)*s)) {
            const char *t = s;
            while (t < e && is_ident(*t))
                t++;
            size_t n = (size_t)(t - s);
            if (n == 7 && memcmp(s, "defined", 7) == 0) {
                after_defined = 1;
            } else if (!after_defined && n > 3 && memcmp(s, "GL_", 3) == 0) {
                memcpy(out, "defined(", 8);
                out += 8;
                memcpy(out, s, n);
                out += n;
                *out++ = ')';
                s = t;
                continue;
            } else {
                after_defined = 0;
            }
            memcpy(out, s, n);
            out += n;
            s = t;
            continue;
        }
        if (*s != '(' && !isspace((unsigned char)*s))
            after_defined = 0;
        *out++ = *s++;
    }
    return out;
}

char *cham_fix_glsl(const char *src)
{
    size_t len = strlen(src);
    /* worst case every 4-char "GL_x" grows by 9 ("defined()") */
    char *out = malloc(len * 4 + 1), *o = out;
    if (!out)
        return NULL;
    const int texture2d = strncmp(src, "#version 300 es", 15) == 0 && !strstr(src, "samplerExternalOES");
    const char *line = src;
    while (*line) {
        const char *end = strchr(line, '\n');
        end = end ? end + 1 : line + strlen(line);
        const char *p = line;
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        size_t kw = 0;
        if (end - p >= 3 && *p == '#') {
            const char *q = p + 1;
            while (q < end && (*q == ' ' || *q == '\t'))
                q++;
            if (end - q >= 3 && memcmp(q, "if", 2) == 0 && (q[2] == ' ' || q[2] == '\t' || q[2] == '('))
                kw = (size_t)(q + 2 - line);
            else if (end - q >= 5 && memcmp(q, "elif", 4) == 0 && (q[4] == ' ' || q[4] == '\t' || q[4] == '('))
                kw = (size_t)(q + 4 - line);
        }
        if (kw) {
            memcpy(o, line, kw);
            o = fix_condition(o + kw, line + kw, end);
        } else if (texture2d && *p != '#') {
            o = fix_texture2d(o, line, end);
        } else {
            memcpy(o, line, (size_t)(end - line));
            o += end - line;
        }
        line = end;
    }
    *o = '\0';
    return out;
}
