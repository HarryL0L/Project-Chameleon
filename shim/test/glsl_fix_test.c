/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unit test for shim/gles/glsl_fix.c, using lines from KWin 6.7's shaders. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gles/gles_internal.h"

static int failures;

static void check(const char *in, const char *want)
{
    char *got = cham_fix_glsl(in);
    int ok = strcmp(got, want) == 0;
    printf("  %s %s", ok ? "ok  " : "FAIL", in);
    if (!ok) {
        printf("       got:  %s       want: %s", got, want);
        failures++;
    }
    free(got);
}

int main(void)
{
    printf("glsl fix-ups\n");
    check("#if GL_OES_standard_derivatives && TRAIT_ROUNDED_CORNERS && TRAIT_BORDER\n",
          "#if defined(GL_OES_standard_derivatives) && TRAIT_ROUNDED_CORNERS && TRAIT_BORDER\n");
    check("#elif GL_EXT_foo || X\n", "#elif defined(GL_EXT_foo) || X\n");
    check("#if defined(GL_OES_x) && GL_OES_y\n", "#if defined(GL_OES_x) && defined(GL_OES_y)\n");
    check("#if defined GL_OES_x\n", "#if defined GL_OES_x\n");
    check("  #  if GL_ES\n", "  #  if defined(GL_ES)\n");
    check("#ifdef GL_ES\n", "#ifdef GL_ES\n");
    check("#extension GL_OES_standard_derivatives : enable\n", "#extension GL_OES_standard_derivatives : enable\n");
    check("#if __VERSION__ >= 130\n", "#if __VERSION__ >= 130\n");
    check("uniform float GL_like_name; // #if GL_X\n", "uniform float GL_like_name; // #if GL_X\n");
    check("#version 300 es\n#if GL_A\nx\n#endif\n", "#version 300 es\n#if defined(GL_A)\nx\n#endif\n");
    /* KWin 6.7's blur downsample.frag, as KWin hands it over on GLES 3 */
    check("#version 300 es\n    vec4 sum = texture2D(texUnit, uv) * 4.0;\n    sum += texture2D (texUnit, uv);\n",
          "#version 300 es\n    vec4 sum = texture(texUnit, uv) * 4.0;\n    sum += texture (texUnit, uv);\n");
    check("#version 300 es\nvec4 my_texture2D(int a);\nfloat texture2DLod;\nx = texture2D;\n",
          "#version 300 es\nvec4 my_texture2D(int a);\nfloat texture2DLod;\nx = texture2D;\n");
    check("#version 300 es\nuniform samplerExternalOES sampler;\nr = texture2D(sampler, c);\n",
          "#version 300 es\nuniform samplerExternalOES sampler;\nr = texture2D(sampler, c);\n");
    check("#version 140\nr = texture2D(sampler, c);\n", "#version 140\nr = texture2D(sampler, c);\n");
    check("vec4 texture(in sampler2D s, in vec2 c) {\n    return texture2D(s, c);\n",
          "vec4 texture(in sampler2D s, in vec2 c) {\n    return texture2D(s, c);\n");
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
