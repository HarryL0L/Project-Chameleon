/* SPDX-License-Identifier: GPL-2.0-or-later */
/* window.c: a Wayland or X11 window fed by Android's GPU driver through an
 * AImageReader, for EGL's Wayland platform and Vulkan's surfaces.
 * The caller serializes the calls on one window. */
#ifndef CHAM_WINDOW_H
#define CHAM_WINDOW_H

#include <stdint.h>

struct cham_window;

/* A Chameleon session (windows' buffers are shared through its render node)
 * with AImageReader and libwayland-client at hand. */
int cham_window_available(void);
/* 0 if the compositor behind this wl_display takes the windows' buffers. */
int cham_window_connect(void *wl_display);
/* A window on a wl_surface (wl_display NULL: the surface's own connection).
 * opaque: its buffers have no alpha. NULL on failure. */
struct cham_window *cham_window_create(void *wl_display, void *wl_surface, int width, int height, int opaque);
/* The ANativeWindow the driver draws on. */
void *cham_window_native(struct cham_window *w);
/* Buffers of this size from the next one on. 0 or -1. */
int cham_window_resize(struct cham_window *w, int width, int height);
/* The driver starts another set of buffers on the window (a new swapchain),
 * with or without alpha for the compositor. */
void cham_window_renew(struct cham_window *w, int opaque);
/* Waits until the compositor has asked for the frame after the last one
 * committed with `callback`. 0, or -1 when the connection is lost. */
int cham_window_wait_frame(struct cham_window *w);
/* Takes the frame the driver just finished and commits it on the wl_surface:
 * n damage rectangles of x, y, width, height (none: all of it), from the
 * bottom-left corner with bottom_left; with a frame callback if asked.
 * Gives the buffer's size. 0 or -1. */
int cham_window_commit(struct cham_window *w, int callback, int dx, int dy, const int *rects, int n, int bottom_left,
                       int *width, int *height);
/* After the driver's surface on it is gone. */
void cham_window_destroy(struct cham_window *w);

/* X11 windows: libxcb with DRI3 and Present at hand. */
int cham_window_x11_available(void);
/* 0 if the X server behind this xcb connection shows its clients' buffers
 * (DRI3 and Present). */
int cham_window_x11_connect(void *xcb_connection);
/* A window on an X11 window of 24 or 32 bits. NULL on failure. The other
 * calls above work on it alike (wait_frame: until the server has shown the
 * last frame; commit: the damage and offset arguments are not used). */
struct cham_window *cham_window_create_x11(void *xcb_connection, uint32_t window);
/* The X11 window's size (asked of the server, or as it last told) and depth.
 * 0 or -1. */
int cham_window_x11_size(struct cham_window *w, int ask, int *width, int *height, int *depth);
/* Xlib's displays: libX11-xcb at hand, and a display's xcb connection. */
int cham_window_xlib_available(void);
void *cham_window_xlib_connection(void *display);

#endif
