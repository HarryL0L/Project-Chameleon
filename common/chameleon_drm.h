/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The fake DRM card libchameleon.so answers for in KWin's process, which
 * libchameleon_udev.so lists while that process serves Chameleon's devices
 * (chameleon_evdev.h): its node, unless CHAMELEON_DRM_PATH names another,
 * and its number: a first card's, primary minor 0 for card0, as the kernel
 * numbers them. libchameleon_udev.so shows its sysfs entries under that
 * number (shim/udev/sysfs.c), in place of a real card's.
 */
#ifndef CHAMELEON_DRM_H
#define CHAMELEON_DRM_H

#define CHAM_DRM_PATH "/dev/dri/card0"
#define CHAM_DRM_MINOR 0

#endif
