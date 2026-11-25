/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private to libvulkan_chameleon.so: what vulkan.c and wsi.c share. */
#ifndef CHAM_VULKAN_H
#define CHAM_VULKAN_H

#include <pthread.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* What a program holds for Android's object `real`. The loader owns the
 * first word. */
struct instance_table;
struct device_table;
struct inst_obj { /* an instance or a physical device */
    uintptr_t loader;
    void *real;
    const struct instance_table *t;
};
struct dev_obj { /* a device, a queue or a command buffer */
    uintptr_t loader;
    void *real;
    const struct device_table *t;
};

/* VK_KHR_wayland_surface: this driver's own functions (wsi.c), which
 * vk_forward.h's name table lists. */
struct wl_display;
struct wl_surface;
#ifndef VK_KHR_wayland_surface /* vulkan_wayland.h, which the NDK doesn't have */
#define VK_KHR_wayland_surface 1
#define VK_KHR_WAYLAND_SURFACE_SPEC_VERSION 6
#define VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME "VK_KHR_wayland_surface"
typedef VkFlags VkWaylandSurfaceCreateFlagsKHR;
typedef struct VkWaylandSurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkWaylandSurfaceCreateFlagsKHR flags;
    struct wl_display *display;
    struct wl_surface *surface;
} VkWaylandSurfaceCreateInfoKHR;
#endif

VKAPI_ATTR VkResult VKAPI_CALL cham_vkCreateWaylandSurfaceKHR(VkInstance instance,
                                                             const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
                                                             const VkAllocationCallbacks *pAllocator,
                                                             VkSurfaceKHR *pSurface);
VKAPI_ATTR VkBool32 VKAPI_CALL cham_vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                                                    uint32_t queueFamilyIndex,
                                                                                    struct wl_display *display);

/* VK_KHR_xcb_surface and VK_KHR_xlib_surface: this driver's own functions
 * too (wsi.c). Their structures as vulkan_xcb.h and vulkan_xlib.h define
 * them; X11's own headers aren't part of the build. */
typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_window_t;
typedef uint32_t xcb_visualid_t;
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long VisualID;
#ifndef VK_KHR_xcb_surface
#define VK_KHR_xcb_surface 1
#define VK_KHR_XCB_SURFACE_SPEC_VERSION 6
#define VK_KHR_XCB_SURFACE_EXTENSION_NAME "VK_KHR_xcb_surface"
typedef VkFlags VkXcbSurfaceCreateFlagsKHR;
typedef struct VkXcbSurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkXcbSurfaceCreateFlagsKHR flags;
    xcb_connection_t *connection;
    xcb_window_t window;
} VkXcbSurfaceCreateInfoKHR;
#endif
#ifndef VK_KHR_xlib_surface
#define VK_KHR_xlib_surface 1
#define VK_KHR_XLIB_SURFACE_SPEC_VERSION 6
#define VK_KHR_XLIB_SURFACE_EXTENSION_NAME "VK_KHR_xlib_surface"
typedef VkFlags VkXlibSurfaceCreateFlagsKHR;
typedef struct VkXlibSurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkXlibSurfaceCreateFlagsKHR flags;
    Display *dpy;
    Window window;
} VkXlibSurfaceCreateInfoKHR;
#endif

VKAPI_ATTR VkResult VKAPI_CALL cham_vkCreateXcbSurfaceKHR(VkInstance instance,
                                                         const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
                                                         const VkAllocationCallbacks *pAllocator,
                                                         VkSurfaceKHR *pSurface);
VKAPI_ATTR VkBool32 VKAPI_CALL cham_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                                                uint32_t queueFamilyIndex,
                                                                                xcb_connection_t *connection,
                                                                                xcb_visualid_t visual_id);
VKAPI_ATTR VkResult VKAPI_CALL cham_vkCreateXlibSurfaceKHR(VkInstance instance,
                                                          const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
                                                          const VkAllocationCallbacks *pAllocator,
                                                          VkSurfaceKHR *pSurface);
VKAPI_ATTR VkBool32 VKAPI_CALL cham_vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                                                 uint32_t queueFamilyIndex,
                                                                                 Display *dpy, VisualID visualID);

#include "vk_forward.h"

#define INST_FN(obj, name) ((PFN_##name)(obj)->t->fn[I_##name])
#define DEV_FN(obj, name) ((PFN_##name)(obj)->t->fn[D_##name])

struct surface;
struct swapchain;

struct instance {
    struct inst_obj o;
    struct instance_table table;
    PFN_vkGetDeviceProcAddr device_proc;
    /* Windows: asked for with VK_KHR_wayland_surface, VK_KHR_xcb_surface or
     * VK_KHR_xlib_surface, made on Android's kind of surface
     * (vkCreateAndroidSurfaceKHR). */
    int windows;
    PFN_vkVoidFunction android_surface;
    pthread_mutex_t lock; /* guards the lists */
    struct physical **physical;
    uint32_t physical_count;
    struct surface *surfaces;
};

struct physical {
    struct inst_obj o;
    struct instance *instance;
};

struct queue {
    struct dev_obj o;
    struct device *device;
    struct queue *next;
};

struct cmd {
    struct dev_obj o;
    VkCommandPool pool;
    struct cmd *prev, *next;
};

struct device {
    struct dev_obj o;
    struct device_table table;
    struct physical *physical;
    pthread_mutex_t lock; /* guards the lists */
    struct queue *queues;
    struct cmd *cmds; /* every command buffer: a pool's go with the pool */
    struct swapchain *swapchains;
};

/* ---- wsi.c: Wayland and X11 windows ---- */

/* Windows can be shown from here: a Chameleon session. */
int cham_wsi_available(void);
/* ...X11 ones too (libxcb with DRI3 and Present; Xlib's with libX11-xcb). */
int cham_wsi_xcb_available(void);
int cham_wsi_xlib_available(void);
/* What a program left behind of an instance's surfaces, a device's swapchains. */
void cham_wsi_instance_gone(struct instance *inst);
void cham_wsi_device_gone(struct device *dev);

#endif
