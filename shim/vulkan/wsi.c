/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Wayland windows for the Vulkan driver: VK_KHR_wayland_surface and
 * VK_KHR_swapchain on Android's own window system.
 *
 * Android's Vulkan draws into ANativeWindows. A Wayland surface here is a
 * window of vendor/window.c - an AImageReader - with an Android surface
 * (vkCreateAndroidSurfaceKHR) on its ANativeWindow, and the program's
 * swapchain is Android's swapchain on that surface: its images, their
 * acquiring and the waiting for the GPU are Android's. A presented image
 * arrives in the reader and is committed on the program's wl_surface, shared
 * with the session like an EGL window's buffer, without a copy.
 *
 * Surfaces and swapchains keep Android's handles, so every function that
 * only passes one on is a generated forwarder. What is written here is what
 * a Wayland window answers differently:
 *  - its size is the swapchain's (no current extent), never rotated;
 *  - its alpha: opaque or pre-multiplied, which Android leaves to its
 *    compositor ("inherit") and which the wl_buffer's format tells ours;
 *  - its formats: 8-bit RGBA, plain or sRGB, the buffers the session shows;
 *  - its pace: FIFO waits for the compositor's frame callback before the
 *    next frame, mailbox doesn't. Android's swapchain itself never waits
 *    for a display here (the reader takes every frame at once).
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "vulkan.h"

struct ANativeWindow;
struct AHardwareBuffer;
#include <vulkan/vulkan_android.h>

#include "../core/cham_shim.h"
#include "../vendor/window.h"

struct surface {
    VkSurfaceKHR real; /* Android's, which the program holds too */
    struct cham_window *w;
    pthread_mutex_t lock; /* serializes the window */
    struct surface *next;
};

struct swapchain {
    VkSwapchainKHR real; /* Android's, which the program holds too */
    struct surface *surface;
    int fifo;
    struct swapchain *next;
};

int cham_wsi_available(void)
{
    return cham_window_available();
}

static struct surface *surface_for(struct instance *inst, VkSurfaceKHR real, int unlink)
{
    pthread_mutex_lock(&inst->lock);
    struct surface **at = &inst->surfaces;
    while (*at && (*at)->real != real)
        at = &(*at)->next;
    struct surface *s = *at;
    if (s && unlink)
        *at = s->next;
    pthread_mutex_unlock(&inst->lock);
    return s;
}

static struct swapchain *swapchain_for(struct device *dev, VkSwapchainKHR real, int unlink)
{
    pthread_mutex_lock(&dev->lock);
    struct swapchain **at = &dev->swapchains;
    while (*at && (*at)->real != real)
        at = &(*at)->next;
    struct swapchain *sc = *at;
    if (sc && unlink)
        *at = sc->next;
    pthread_mutex_unlock(&dev->lock);
    return sc;
}

/* ---- surfaces ---- */

VKAPI_ATTR VkResult VKAPI_CALL cham_vkCreateWaylandSurfaceKHR(VkInstance instance,
                                                             const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
                                                             const VkAllocationCallbacks *pAllocator,
                                                             VkSurfaceKHR *pSurface)
{
    struct instance *inst = (struct instance *)instance;
    PFN_vkCreateAndroidSurfaceKHR create = (PFN_vkCreateAndroidSurfaceKHR)inst->android_surface;
    struct surface *s = calloc(1, sizeof *s);
    if (!s)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    /* The reader's size is the swapchain's later; with alpha, which the
     * swapchain may ask to have ignored. */
    s->w = create ? cham_window_create(pCreateInfo->display, pCreateInfo->surface, 1, 1, 0) : NULL;
    if (!s->w) {
        free(s);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkAndroidSurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                          .window = cham_window_native(s->w)};
    VkResult result = create(inst->o.real, &info, pAllocator, &s->real);
    if (result != VK_SUCCESS) {
        cham_window_destroy(s->w);
        free(s);
        return result;
    }
    pthread_mutex_init(&s->lock, NULL);
    pthread_mutex_lock(&inst->lock);
    s->next = inst->surfaces;
    inst->surfaces = s;
    pthread_mutex_unlock(&inst->lock);
    *pSurface = s->real;
    return VK_SUCCESS;
}

static void surface_free(struct surface *s)
{
    cham_window_destroy(s->w);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

VKAPI_ATTR void VKAPI_CALL cham_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                   const VkAllocationCallbacks *pAllocator)
{
    struct instance *inst = (struct instance *)instance;
    struct surface *s = surface_for(inst, surface, 1);
    /* Android's surface holds the window: it goes first. */
    INST_FN(&inst->o, vkDestroySurfaceKHR)(inst->o.real, surface, pAllocator);
    if (s)
        surface_free(s);
}

void cham_wsi_instance_gone(struct instance *inst)
{
    while (inst->surfaces) {
        struct surface *s = inst->surfaces;
        inst->surfaces = s->next;
        INST_FN(&inst->o, vkDestroySurfaceKHR)(inst->o.real, s->real, NULL);
        surface_free(s);
    }
}

/* Any queue of the driver can present: presenting is handing a buffer to
 * the compositor that takes them. */
VKAPI_ATTR VkBool32 VKAPI_CALL cham_vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                                                    uint32_t queueFamilyIndex,
                                                                                    struct wl_display *display)
{
    (void)physicalDevice, (void)queueFamilyIndex;
    return cham_window_connect(display) == 0;
}

VKAPI_ATTR VkResult VKAPI_CALL cham_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
    struct inst_obj *p = (struct inst_obj *)physicalDevice;
    VkResult result =
        INST_FN(p, vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(p->real, surface, pSurfaceCapabilities);
    if (result != VK_SUCCESS)
        return result;
    pSurfaceCapabilities->currentExtent = (VkExtent2D){UINT32_MAX, UINT32_MAX};
    pSurfaceCapabilities->minImageExtent = (VkExtent2D){1, 1};
    pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->supportedCompositeAlpha =
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    return VK_SUCCESS;
}

/* Gives the caller `kept` items of `size` bytes, the way Vulkan's
 * two-call lists do. Frees `all`. */
static VkResult give(void *all, uint32_t kept, size_t size, uint32_t *count, void *out)
{
    VkResult result = VK_SUCCESS;
    if (out) {
        if (*count < kept) {
            kept = *count;
            result = VK_INCOMPLETE;
        }
        memcpy(out, all, kept * size);
    }
    *count = kept;
    free(all);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL cham_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice,
                                                                        VkSurfaceKHR surface,
                                                                        uint32_t *pSurfaceFormatCount,
                                                                        VkSurfaceFormatKHR *pSurfaceFormats)
{
    struct inst_obj *p = (struct inst_obj *)physicalDevice;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR real = INST_FN(p, vkGetPhysicalDeviceSurfaceFormatsKHR);
    uint32_t n = 0, kept = 0;
    VkResult result = real(p->real, surface, &n, NULL);
    if (result != VK_SUCCESS)
        return result;
    VkSurfaceFormatKHR *all = malloc((n + 1) * sizeof *all);
    if (!all)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = real(p->real, surface, &n, all);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        free(all);
        return result;
    }
    for (uint32_t i = 0; i < n; i++)
        if ((all[i].format == VK_FORMAT_R8G8B8A8_UNORM || all[i].format == VK_FORMAT_R8G8B8A8_SRGB) &&
            all[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            all[kept++] = all[i];
    return give(all, kept, sizeof *all, pSurfaceFormatCount, pSurfaceFormats);
}

VKAPI_ATTR VkResult VKAPI_CALL cham_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice,
                                                                             VkSurfaceKHR surface,
                                                                             uint32_t *pPresentModeCount,
                                                                             VkPresentModeKHR *pPresentModes)
{
    (void)physicalDevice, (void)surface;
    VkPresentModeKHR *all = malloc(2 * sizeof *all);
    if (!all)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    all[0] = VK_PRESENT_MODE_FIFO_KHR;
    all[1] = VK_PRESENT_MODE_MAILBOX_KHR;
    return give(all, 2, sizeof *all, pPresentModeCount, pPresentModes);
}

/* ---- swapchains ---- */

VKAPI_ATTR VkResult VKAPI_CALL cham_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                        const VkAllocationCallbacks *pAllocator,
                                                        VkSwapchainKHR *pSwapchain)
{
    struct device *dev = (struct device *)device;
    struct inst_obj *p = &dev->physical->o;
    struct surface *s = surface_for(dev->physical->instance, pCreateInfo->surface, 0);
    struct swapchain *sc = calloc(1, sizeof *sc);
    if (!sc)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkSwapchainCreateInfoKHR info = *pCreateInfo;
    /* The reader takes each frame as it is presented: Android's own mode
     * only decides how its queue behaves, and FIFO loses no frame. */
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    /* Alpha is the compositor's to honour: Android passes it on as it is. */
    VkSurfaceCapabilitiesKHR caps;
    if (INST_FN(p, vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(p->real, info.surface, &caps) == VK_SUCCESS &&
        !(caps.supportedCompositeAlpha & info.compositeAlpha))
        info.compositeAlpha = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                                  ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                                  : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha &
                                                                  -caps.supportedCompositeAlpha);
    VkResult result = DEV_FN(&dev->o, vkCreateSwapchainKHR)(dev->o.real, &info, pAllocator, &sc->real);
    if (result != VK_SUCCESS) {
        free(sc);
        return result;
    }
    sc->surface = s;
    sc->fifo = pCreateInfo->presentMode != VK_PRESENT_MODE_MAILBOX_KHR;
    if (s) {
        pthread_mutex_lock(&s->lock);
        cham_window_renew(s->w, pCreateInfo->compositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
        pthread_mutex_unlock(&s->lock);
    }
    pthread_mutex_lock(&dev->lock);
    sc->next = dev->swapchains;
    dev->swapchains = sc;
    pthread_mutex_unlock(&dev->lock);
    *pSwapchain = sc->real;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL cham_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                     const VkAllocationCallbacks *pAllocator)
{
    struct device *dev = (struct device *)device;
    free(swapchain_for(dev, swapchain, 1));
    DEV_FN(&dev->o, vkDestroySwapchainKHR)(dev->o.real, swapchain, pAllocator);
}

void cham_wsi_device_gone(struct device *dev)
{
    while (dev->swapchains) {
        struct swapchain *sc = dev->swapchains;
        dev->swapchains = sc->next;
        free(sc);
    }
}

/* Android queues each image into the reader of its window; from there it
 * goes to the compositor. */
VKAPI_ATTR VkResult VKAPI_CALL cham_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    struct queue *q = (struct queue *)queue;
    /* FIFO: at most one frame ahead of the compositor. */
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        struct swapchain *sc = swapchain_for(q->device, pPresentInfo->pSwapchains[i], 0);
        if (sc && sc->surface && sc->fifo) {
            pthread_mutex_lock(&sc->surface->lock);
            int lost = cham_window_wait_frame(sc->surface->w) != 0;
            pthread_mutex_unlock(&sc->surface->lock);
            if (lost)
                return VK_ERROR_SURFACE_LOST_KHR;
        }
    }
    VkResult result = DEV_FN(&q->o, vkQueuePresentKHR)(q->o.real, pPresentInfo);
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        VkResult one = pPresentInfo->pResults ? pPresentInfo->pResults[i] : result;
        struct swapchain *sc = swapchain_for(q->device, pPresentInfo->pSwapchains[i], 0);
        if (!sc || !sc->surface || (one != VK_SUCCESS && one != VK_SUBOPTIMAL_KHR))
            continue;
        pthread_mutex_lock(&sc->surface->lock);
        int lost = cham_window_commit(sc->surface->w, sc->fifo, 0, 0, NULL, 0, 0, NULL, NULL) != 0;
        pthread_mutex_unlock(&sc->surface->lock);
        if (lost) {
            if (pPresentInfo->pResults)
                pPresentInfo->pResults[i] = VK_ERROR_SURFACE_LOST_KHR;
            if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
                result = VK_ERROR_SURFACE_LOST_KHR;
        }
    }
    return result;
}
