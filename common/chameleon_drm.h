/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The fake DRM card libchameleon.so answers for in KWin's process, which
 * libchameleon_udev.so lists while that process serves Chameleon's devices
 * (chameleon_evdev.h): its node, unless CHAMELEON_DRM_PATH names another,
 * and its number: DRM's major with a primary-node minor no real card uses,
 * so nothing mistakes it for a real GPU's sysfs entry.
 */
#ifndef CHAMELEON_DRM_H
#define CHAMELEON_DRM_H

#define CHAM_DRM_PATH "/dev/dri/card0"
#define CHAM_DRM_MINOR 60

#endif
