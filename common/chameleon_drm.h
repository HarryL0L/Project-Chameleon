/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The fake DRM card chameleon-kernel serves, which libchameleon_udev.so lists
 * while chameleon-kernel runs (chameleon_evdev.h), and its two nodes, numbered
 * as the kernel numbers a first card's: the primary node card0 (minor 0;
 * CHAMELEON_DRM_PATH may name another path for it) and the render node
 * renderD128 (minor 128), the same device for programs that only render and
 * share buffers. libchameleon_udev.so shows their sysfs entries under those
 * numbers (shim/udev/sysfs.c), in place of a real card's.
 */
#ifndef CHAMELEON_DRM_H
#define CHAMELEON_DRM_H

#define CHAM_DRM_NAME "card0"
#define CHAM_DRM_PATH "/dev/dri/" CHAM_DRM_NAME
#define CHAM_DRM_MINOR 0
#define CHAM_RENDER_NAME "renderD128"
#define CHAM_RENDER_PATH "/dev/dri/" CHAM_RENDER_NAME
#define CHAM_RENDER_MINOR 128

#endif
