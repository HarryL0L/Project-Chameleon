/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host test of GPU rendering for Wayland apps (vendor/wayland.c, whose
 * buffers reach chameleon-kernel as GEM handles on the render node), end to
 * end:
 *
 *  - a compositor thread (real libwayland-server) with wl_compositor and a
 *    zwp_linux_dmabuf_v1 that, like KWin's EGL import, only accepts a dmabuf
 *    that chameleon-kernel knows as a session buffer (asked through
 *    libchameleon.so, which also tells it the socket from our bind()), and
 *    that imports each one on the render node too, as wlroots does;
 *  - the app: real libwayland-client + libwayland-egl, EGL through the
 *    system's glvnd -> libEGL_chameleon.so -> fake Android driver whose
 *    window surfaces render into (fake) AImageReaders;
 *  - or, with VULKAN_WINDOW_TEST, a Vulkan app: the system's Vulkan loader
 *    -> libvulkan_chameleon.so -> fake_android_vulkan.c, whose swapchain
 *    presents into the same kind of reader. Its frames are drawn by a real
 *    driver, and the compositor looks at their pixels.
 */
#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-server.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include "check.h"

/* ---- compositor ---- */

static const struct wl_interface *k_null[8];
static const struct wl_interface k_params_iface;
static const struct wl_interface *k_create_params_types[] = {&k_params_iface};
static const struct wl_message k_dmabuf_requests[] = {
    {"destroy", "", k_null},
    {"create_params", "n", k_create_params_types},
};
static const struct wl_message k_dmabuf_events[] = {
    {"format", "u", k_null},
    {"modifier", "3uuu", k_null},
};
static const struct wl_interface k_dmabuf_iface = {"zwp_linux_dmabuf_v1", 3, 2, k_dmabuf_requests, 2,
                                                   k_dmabuf_events};
static const struct wl_interface *k_create_immed_types[] = {&wl_buffer_interface, NULL, NULL, NULL, NULL};
static const struct wl_interface *k_created_types[] = {&wl_buffer_interface};
static const struct wl_message k_params_requests[] = {
    {"destroy", "", k_null},
    {"add", "huuuuu", k_null},
    {"create", "iiuu", k_null},
    {"create_immed", "2niiuu", k_create_immed_types},
};
static const struct wl_message k_params_events[] = {
    {"created", "n", k_created_types},
    {"failed", "", k_null},
};
static const struct wl_interface k_params_iface = {"zwp_linux_buffer_params_v1", 3, 4, k_params_requests, 2,
                                                   k_params_events};

static int g_render = -1; /* the compositor's render node */
static struct {
    int buffers_created, buffers_known_to_kwin, buffers_unknown, buffers_prime;
    int commits, releases, frames_done;
    int last_width, last_height, last_stride;
    uint8_t shown[4]; /* the first pixel of the buffer on screen */
    uint32_t last_format;
    uint64_t last_modifier;
    volatile int stop;
} srv;

static void *(*buffer_ahb_acquire)(int); /* libchameleon: AHardwareBuffer * or NULL */
static void (*buffer_ahb_release)(void *);

struct buffer {
    struct wl_resource *resource;
    int width, height;
    int fd;
};

struct params {
    int fd;
    uint32_t stride;
    uint64_t modifier;
};

static void buffer_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static const struct wl_buffer_interface k_buffer_impl = {.destroy = buffer_destroy};
static void buffer_free(struct wl_resource *r)
{
    struct buffer *b = wl_resource_get_user_data(r);
    close(b->fd);
    free(b);
}

static void params_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static void params_add(struct wl_client *c, struct wl_resource *r, int32_t fd, uint32_t plane, uint32_t offset,
                       uint32_t stride, uint32_t mod_hi, uint32_t mod_lo)
{
    (void)c, (void)plane, (void)offset;
    struct params *p = wl_resource_get_user_data(r);
    if (p->fd >= 0)
        close(p->fd);
    p->fd = fd;
    p->stride = stride;
    p->modifier = (uint64_t)mod_hi << 32 | mod_lo;
}
static void params_create(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h, uint32_t f, uint32_t fl)
{
    (void)c, (void)r, (void)w, (void)h, (void)f, (void)fl;
}
static void params_create_immed(struct wl_client *client, struct wl_resource *r, uint32_t id, int32_t w, int32_t h,
                                uint32_t format, uint32_t flags)
{
    (void)flags;
    struct params *p = wl_resource_get_user_data(r);
    /* What KWin's EGL import does through the vendor: find the AHB. */
    void *ahb = p->fd >= 0 && buffer_ahb_acquire ? buffer_ahb_acquire(p->fd) : NULL;
    if (ahb)
        srv.buffers_known_to_kwin++;
    else
        srv.buffers_unknown++;
    if (ahb)
        buffer_ahb_release(ahb);
    struct drm_prime_handle prime = {.fd = p->fd};
    if (p->fd >= 0 && ioctl(g_render, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) == 0 && prime.handle) {
        srv.buffers_prime++;
        struct drm_gem_close close_handle = {.handle = prime.handle};
        ioctl(g_render, DRM_IOCTL_GEM_CLOSE, &close_handle);
    }
    srv.buffers_created++;
    srv.last_width = w;
    srv.last_height = h;
    srv.last_stride = (int)p->stride;
    srv.last_format = format;
    srv.last_modifier = p->modifier;
    struct buffer *b = calloc(1, sizeof *b);
    b->width = w;
    b->height = h;
    b->fd = dup(p->fd);
    b->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    wl_resource_set_implementation(b->resource, &k_buffer_impl, b, buffer_free);
}
static const struct {
    void *fns[4];
} k_params_impl = {{params_destroy, params_add, params_create, params_create_immed}};
static void params_free(struct wl_resource *r)
{
    struct params *p = wl_resource_get_user_data(r);
    if (p->fd >= 0)
        close(p->fd);
    free(p);
}

static void dmabuf_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static void dmabuf_create_params(struct wl_client *client, struct wl_resource *r, uint32_t id)
{
    struct params *p = calloc(1, sizeof *p);
    p->fd = -1;
    struct wl_resource *res = wl_resource_create(client, &k_params_iface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(res, &k_params_impl, p, params_free);
}
static const struct {
    void *fns[2];
} k_dmabuf_impl = {{dmabuf_destroy, dmabuf_create_params}};
static void bind_dmabuf(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &k_dmabuf_iface, (int)version, id);
    wl_resource_set_implementation(r, &k_dmabuf_impl, NULL, NULL);
    wl_resource_post_event(r, 0 /* format */, DRM_FORMAT_ABGR8888);
}

struct surface {
    struct wl_resource *pending, *current;
    struct wl_list frames;
};

static void surface_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static void surface_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buffer, int32_t x,
                           int32_t y)
{
    (void)c, (void)x, (void)y;
    struct surface *s = wl_resource_get_user_data(r);
    s->pending = buffer;
}
static void surface_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c, (void)r, (void)x, (void)y, (void)w, (void)h;
}
static void surface_frame(struct wl_client *client, struct wl_resource *r, uint32_t id)
{
    struct surface *s = wl_resource_get_user_data(r);
    struct wl_resource *cb = wl_resource_create(client, &wl_callback_interface, 1, id);
    wl_resource_set_implementation(cb, NULL, NULL, NULL);
    wl_list_insert(s->frames.prev, wl_resource_get_link(cb));
}
static void surface_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *region)
{
    (void)c, (void)r, (void)region;
}
static void surface_commit(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    struct surface *s = wl_resource_get_user_data(r);
    if (s->pending && s->pending != s->current) {
        if (s->current) {
            wl_resource_post_event(s->current, 0 /* release */);
            srv.releases++;
        }
        s->current = s->pending;
    }
    s->pending = NULL;
    if (s->current) { /* what a compositor would sample */
        struct buffer *b = wl_resource_get_user_data(s->current);
        uint8_t *pixels = mmap(NULL, 4, PROT_READ, MAP_SHARED, b->fd, 0);
        if (pixels != MAP_FAILED) {
            memcpy(srv.shown, pixels, 4);
            munmap(pixels, 4);
        }
    }
    srv.commits++;
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, &s->frames) {
        wl_resource_post_event(cb, 0 /* done */, 0u);
        wl_list_remove(wl_resource_get_link(cb));
        wl_resource_destroy(cb);
        srv.frames_done++;
    }
}
static void surface_int(struct wl_client *c, struct wl_resource *r, int32_t v)
{
    (void)c, (void)r, (void)v;
}
static void surface_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{
    (void)c, (void)r, (void)x, (void)y;
}
static const struct wl_surface_interface k_surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_region,
    .set_input_region = surface_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_int,
    .set_buffer_scale = surface_int,
    .damage_buffer = surface_damage,
    .offset = surface_offset,
};
static void surface_free(struct wl_resource *r)
{
    free(wl_resource_get_user_data(r));
}
static void compositor_create_surface(struct wl_client *client, struct wl_resource *r, uint32_t id)
{
    struct surface *s = calloc(1, sizeof *s);
    wl_list_init(&s->frames);
    struct wl_resource *res = wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(res, &k_surface_impl, s, surface_free);
}
static void compositor_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    (void)c, (void)r, (void)id;
}
static const struct wl_compositor_interface k_compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};
static void bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wl_compositor_interface, (int)version, id);
    wl_resource_set_implementation(r, &k_compositor_impl, NULL, NULL);
}

static void *server_main(void *arg)
{
    struct wl_display *display = arg;
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    while (!srv.stop) {
        wl_event_loop_dispatch(loop, 10);
        wl_display_flush_clients(display);
    }
    return NULL;
}

/* ---- the app ---- */

static struct wl_compositor *g_compositor;
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version)
{
    (void)data, (void)version;
    if (strcmp(iface, "wl_compositor") == 0)
        g_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data, (void)registry, (void)name;
}
static const struct wl_registry_listener k_registry_listener = {global, global_remove};

/* ---- the Vulkan app ---- */

static VkDevice vk_dev;
static VkQueue vk_queue;
static VkCommandBuffer vk_cmd;
static VkSemaphore vk_acquired, vk_drawn;

/* One frame: the next image, filled with a colour, presented. */
static VkResult vk_frame(VkSwapchainKHR swapchain, VkImage *images, float r, float g, float b)
{
    uint32_t i = 0;
    VkResult result = vkAcquireNextImageKHR(vk_dev, swapchain, UINT64_MAX, vk_acquired, VK_NULL_HANDLE, &i);
    if (result != VK_SUCCESS)
        return result;
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(vk_cmd, &begin);
    VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_clear = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[i],
                                     .subresourceRange = whole};
    vkCmdPipelineBarrier(vk_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &to_clear);
    VkClearColorValue colour = {.float32 = {r, g, b, 1}};
    vkCmdClearColorImage(vk_cmd, images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &whole);
    VkImageMemoryBarrier to_present = to_clear;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(vk_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                         0, NULL, 1, &to_present);
    vkEndCommandBuffer(vk_cmd);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &vk_acquired, .pWaitDstStageMask = &stage, .commandBufferCount = 1,
                           .pCommandBuffers = &vk_cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &vk_drawn};
    result = vkQueueSubmit(vk_queue, 1, &submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS)
        return result;
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &vk_drawn, .swapchainCount = 1, .pSwapchains = &swapchain,
                                .pImageIndices = &i};
    result = vkQueuePresentKHR(vk_queue, &present);
    vkQueueWaitIdle(vk_queue); /* the one command buffer is free again */
    return result;
}

static int shown(int r, int g, int b)
{
    return srv.shown[0] == r && srv.shown[1] == g && srv.shown[2] == b;
}

static int has_extension(const VkExtensionProperties *list, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(list[i].extensionName, name) == 0)
            return 1;
    return 0;
}

static void vulkan_app(struct wl_display *display, struct wl_surface *surface)
{
    printf("Vulkan windows\n");
    VkExtensionProperties exts[256];
    uint32_t n = 256;
    vkEnumerateInstanceExtensionProperties(NULL, &n, exts);
    CHECK(has_extension(exts, n, "VK_KHR_surface") && has_extension(exts, n, "VK_KHR_wayland_surface") &&
              !has_extension(exts, n, "VK_KHR_android_surface") && !has_extension(exts, n, "VK_KHR_xcb_surface"),
          "Wayland surfaces offered, and no other kind");
    const char *instance_exts[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = 2, .ppEnabledExtensionNames = instance_exts};
    VkInstance inst;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    CHECK(r == VK_SUCCESS, "an instance with them (%d)", r);
    if (r != VK_SUCCESS)
        return;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &phys);
    VkWaylandSurfaceCreateInfoKHR wsci = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                                          .display = display, .surface = surface};
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    r = vkCreateWaylandSurfaceKHR(inst, &wsci, NULL, &vk_surface);
    CHECK(r == VK_SUCCESS && vk_surface, "a surface on a wl_surface (%d)", r);
    if (r != VK_SUCCESS)
        return;
    VkBool32 can = VK_FALSE;
    r = vkGetPhysicalDeviceSurfaceSupportKHR(phys, 0, vk_surface, &can);
    CHECK(r == VK_SUCCESS && can && vkGetPhysicalDeviceWaylandPresentationSupportKHR(phys, 0, display),
          "the device's queue presents to it and to the compositor");
    VkSurfaceCapabilitiesKHR caps;
    r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, vk_surface, &caps);
    CHECK(r == VK_SUCCESS && caps.currentExtent.width == UINT32_MAX && caps.minImageExtent.width == 1 &&
              caps.maxImageExtent.width >= 2048,
          "a Wayland window: its size is the swapchain's");
    CHECK(caps.supportedCompositeAlpha == (VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) &&
              caps.supportedTransforms == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR && caps.minImageCount >= 2,
          "opaque or pre-multiplied alpha, never rotated, at least %u images", caps.minImageCount);
    VkSurfaceFormatKHR formats[16];
    n = 16;
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(phys, vk_surface, &n, formats);
    CHECK(r == VK_SUCCESS && n == 2 && formats[0].format == VK_FORMAT_R8G8B8A8_UNORM &&
              formats[1].format == VK_FORMAT_R8G8B8A8_SRGB,
          "formats: 8-bit RGBA, plain and sRGB (%u)", n);
    n = 1;
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(phys, vk_surface, &n, formats);
    CHECK(r == VK_INCOMPLETE && n == 1, "...a list that doesn't fit is cut and says so");
    VkPresentModeKHR modes[8];
    n = 8;
    r = vkGetPhysicalDeviceSurfacePresentModesKHR(phys, vk_surface, &n, modes);
    CHECK(r == VK_SUCCESS && n == 2 && modes[0] == VK_PRESENT_MODE_FIFO_KHR && modes[1] == VK_PRESENT_MODE_MAILBOX_KHR,
          "present modes: FIFO and mailbox");

    n = 256;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, exts);
    CHECK(has_extension(exts, n, "VK_KHR_swapchain"), "swapchains offered");
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                   .pQueuePriorities = &priority};
    const char *device_exts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 1,
                              .ppEnabledExtensionNames = device_exts};
    r = vkCreateDevice(phys, &dci, NULL, &vk_dev);
    CHECK(r == VK_SUCCESS, "a device with them (%d)", r);
    if (r != VK_SUCCESS)
        return;
    vkGetDeviceQueue(vk_dev, 0, 0, &vk_queue);
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool;
    vkCreateCommandPool(vk_dev, &cpci, NULL, &pool);
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    vkAllocateCommandBuffers(vk_dev, &cbai, &vk_cmd);
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(vk_dev, &sci, NULL, &vk_acquired);
    vkCreateSemaphore(vk_dev, &sci, NULL, &vk_drawn);

    VkSwapchainCreateInfoKHR scci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vk_surface,
                                     .minImageCount = caps.minImageCount, .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
                                     .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = {64, 32},
                                     .imageArrayLayers = 1,
                                     .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                     .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                     .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(vk_dev, &scci, NULL, &swapchain);
    CHECK(r == VK_SUCCESS, "an opaque FIFO swapchain of 64x32, which Android only knows as \"inherit\" (%d)", r);
    if (r != VK_SUCCESS)
        return;
    VkImage images[16];
    uint32_t image_count = 16;
    vkGetSwapchainImagesKHR(vk_dev, swapchain, &image_count, images);

    static const float k_colours[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}};
    int right = 0;
    r = VK_SUCCESS;
    for (int i = 0; i < 6 && r == VK_SUCCESS; i++) {
        r = vk_frame(swapchain, images, k_colours[i][0], k_colours[i][1], k_colours[i][2]);
        wl_display_roundtrip(display);
        right += shown((int)k_colours[i][0] * 255, (int)k_colours[i][1] * 255, (int)k_colours[i][2] * 255);
    }
    CHECK(r == VK_SUCCESS && srv.commits == 6 && right == 6,
          "6 frames -> %d commits, each with its colour on screen (%d)", srv.commits, right);
    CHECK(srv.buffers_created >= 2 && srv.buffers_created <= 3 && srv.buffers_unknown == 0 &&
              srv.buffers_known_to_kwin == srv.buffers_created && srv.buffers_prime == srv.buffers_created,
          "%d zero-copy buffers, all known to KWin and its render node", srv.buffers_created);
    CHECK(srv.last_width == 64 && srv.last_height == 32 && srv.last_stride == 256 &&
              srv.last_format == DRM_FORMAT_XBGR8888 && srv.last_modifier == DRM_FORMAT_MOD_INVALID,
          "dmabuf %dx%d stride %d, XBGR8888 (opaque), implicit modifier", srv.last_width, srv.last_height,
          srv.last_stride);
    CHECK(srv.releases == 5 && srv.frames_done == 6, "buffers released (%d) and frames paced (%d callbacks)",
          srv.releases, srv.frames_done);

    /* The window gets another size: a new swapchain, this one unpaced and
     * with alpha. */
    VkSwapchainKHR old = swapchain;
    scci.imageExtent = (VkExtent2D){80, 40};
    scci.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    scci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    scci.oldSwapchain = old;
    r = vkCreateSwapchainKHR(vk_dev, &scci, NULL, &swapchain);
    vkDestroySwapchainKHR(vk_dev, old, NULL);
    image_count = 16;
    vkGetSwapchainImagesKHR(vk_dev, swapchain, &image_count, images);
    int created = srv.buffers_created;
    right = 0;
    for (int i = 0; i < 4 && r == VK_SUCCESS; i++) {
        r = vk_frame(swapchain, images, k_colours[i][0], k_colours[i][1], k_colours[i][2]);
        wl_display_roundtrip(display);
        right += shown((int)k_colours[i][0] * 255, (int)k_colours[i][1] * 255, (int)k_colours[i][2] * 255);
    }
    CHECK(r == VK_SUCCESS && srv.commits == 10 && right == 4, "a mailbox swapchain of 80x40: 4 more frames shown (%d)",
          r);
    CHECK(srv.last_width == 80 && srv.last_height == 40 && srv.last_format == DRM_FORMAT_ABGR8888 &&
              srv.buffers_created > created && srv.buffers_unknown == 0,
          "new buffers at %dx%d with alpha reach KWin", srv.last_width, srv.last_height);
    CHECK(srv.frames_done == 6, "and nothing waited for the compositor's frame callbacks");
    struct { int robust_attrib_seen, client_version; char shader[512]; int tex_uploads, create_image_target,
             query_counter_calls, swaps, swap_interval_calls, last_swap_interval, readers, geometry_calls; } *fake =
        dlsym(dlopen(getenv("CHAMELEON_ANDROID_MEDIANDK"), RTLD_NOW | RTLD_NOLOAD), "fake_state");
    CHECK(fake && fake->readers == 1, "one window for the surface's life (%d)", fake ? fake->readers : -1);

    vkDestroySwapchainKHR(vk_dev, swapchain, NULL);
    vkDestroySemaphore(vk_dev, vk_acquired, NULL);
    vkDestroySemaphore(vk_dev, vk_drawn, NULL);
    vkDestroyCommandPool(vk_dev, pool, NULL);
    vkDestroyDevice(vk_dev, NULL);
    vkDestroySurfaceKHR(inst, vk_surface, NULL);
    vkDestroyInstance(inst, NULL);
    wl_display_roundtrip(display);
    CHECK(1, "swapchain, device, surface and instance gone");
}

int main(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *socket_name = getenv("WAYLAND_DISPLAY");
    if (!runtime || !socket_name || !getenv("__EGL_VENDOR_LIBRARY_FILENAMES")) {
        fprintf(stderr, "run via run-host-test.sh\n");
        return 2;
    }
    *(void **)&buffer_ahb_acquire = dlsym(RTLD_DEFAULT, "cham_buffer_ahb_acquire");
    *(void **)&buffer_ahb_release = dlsym(RTLD_DEFAULT, "cham_buffer_ahb_release");

    /* As a compositor: DRM master first, then its socket, and a second one
     * the app uses below like a proxy's. */
    const char *card_path = getenv("CHAMELEON_DRM_PATH");
    int card = open(card_path ? card_path : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    g_render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct wl_display *server = wl_display_create();
    if (card < 0 || wl_display_add_socket(server, socket_name) != 0 ||
        wl_display_add_socket(server, "wayland-proxy-cwl") != 0) {
        fprintf(stderr, "cannot create the Wayland socket\n");
        return 1;
    }
    wl_global_create(server, &wl_compositor_interface, 4, NULL, bind_compositor);
    wl_global_create(server, &k_dmabuf_iface, 3, NULL, bind_dmabuf);
    pthread_t thread;
    pthread_create(&thread, NULL, server_main, server);

    printf("GPU rendering for Wayland apps\n");
    char old_socket[256];
    snprintf(old_socket, sizeof old_socket, "%s/chameleon-buffers", runtime);
    struct stat st;
    CHECK(buffer_ahb_acquire && stat(old_socket, &st) != 0,
          "no socket of its own for app buffers: they come through the render node");

    /* The app reaches the compositor through another socket, as behind
     * Firefox's wayland-proxy-N: its buffers are shared all the same. */
    setenv("WAYLAND_DISPLAY", "wayland-proxy-cwl", 1);
    socket_name = "wayland-proxy-cwl";

    struct wl_display *display = wl_display_connect(socket_name);
    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &k_registry_listener, NULL);
    wl_display_roundtrip(display);
    struct wl_surface *surface = wl_compositor_create_surface(g_compositor);
    if (getenv("VULKAN_WINDOW_TEST")) {
        vulkan_app(display, surface);
        wl_surface_destroy(surface);
        wl_display_roundtrip(display);
        wl_display_disconnect(display);
        srv.stop = 1;
        pthread_join(thread, NULL);
        wl_display_destroy(server);
        printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
        return failures != 0;
    }
    struct wl_egl_window *window = wl_egl_window_create(surface, 64, 32);

    if (getenv("EXPECT_DESKTOP_GL_QT")) {
        /* fake_qtgui.so is preloaded: a Qt built for desktop OpenGL */
        const char *mode = getenv("CHAMELEON_EGL_QT");
        int mesa = mode && strcmp(mode, "mesa") == 0;
        printf("desktop-OpenGL Qt%s\n", mesa ? ", CHAMELEON_EGL_QT=mesa" : "");
        EGLDisplay qdpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
        if (mesa) {
            CHECK(qdpy == EGL_NO_DISPLAY, "its Wayland display is left to the other vendors (Mesa)");
        } else {
            eglInitialize(qdpy, NULL, NULL);
            const char *vendor = eglQueryString(qdpy, EGL_VENDOR);
            CHECK(qdpy != EGL_NO_DISPLAY && vendor && strstr(vendor, "NVIDIA") && strcmp(vendor, "NVIDIA") != 0,
                  "GPU display, EGL_VENDOR steers Qt to OpenGL ES: '%s'", vendor ? vendor : "(null)");
        }
        srv.stop = 1;
        pthread_join(thread, NULL);
        printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
        return failures != 0;
    }

    const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    CHECK(client_exts && strstr(client_exts, "EGL_KHR_platform_wayland"), "Wayland platform advertised");
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
    CHECK(dpy != EGL_NO_DISPLAY, "Wayland EGLDisplay from the Chameleon vendor");
    CHECK(eglInitialize(dpy, NULL, NULL), "eglInitialize");
    EGLConfig config;
    EGLint n = 0;
    const EGLint config_attribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
    eglChooseConfig(dpy, config_attribs, &config, 1, &n);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    EGLSurface surf = eglCreatePlatformWindowSurface(dpy, config, window, NULL);
    CHECK(surf != EGL_NO_SURFACE, "window surface for a wl_egl_window");
    CHECK(eglMakeCurrent(dpy, surf, surf, ctx), "eglMakeCurrent on it");
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    CHECK(w == 64 && h == 32, "eglQuerySurface: %dx%d", w, h);
    CHECK(eglSwapInterval(dpy, 1), "eglSwapInterval(1)");

    int ok = 1;
    for (int i = 0; i < 6; i++)
        ok &= eglSwapBuffers(dpy, surf) == EGL_TRUE;
    wl_display_roundtrip(display);
    CHECK(ok && srv.commits == 6, "6 swaps -> %d commits", srv.commits);
    CHECK(srv.buffers_created >= 2 && srv.buffers_created <= 3 && srv.buffers_unknown == 0 &&
              srv.buffers_known_to_kwin == srv.buffers_created,
          "%d zero-copy buffers, all known to KWin (%d unknown)", srv.buffers_created, srv.buffers_unknown);
    CHECK(srv.buffers_prime == srv.buffers_created, "...and each imports on the render node by PRIME (%d of %d)",
          srv.buffers_prime, srv.buffers_created);
    CHECK(srv.last_width == 64 && srv.last_height == 32 && srv.last_stride == 256 &&
              srv.last_format == DRM_FORMAT_ABGR8888 && srv.last_modifier == DRM_FORMAT_MOD_INVALID,
          "dmabuf %dx%d stride %d, ABGR8888, implicit modifier", srv.last_width, srv.last_height, srv.last_stride);
    CHECK(srv.releases == 5 && srv.frames_done >= 5, "buffers released (%d) and frames paced (%d callbacks)",
          srv.releases, srv.frames_done);

    wl_egl_window_resize(window, 80, 40, 0, 0);
    eglMakeCurrent(dpy, surf, surf, ctx); /* where apps start drawing the next frame */
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    CHECK(w == 80 && h == 40, "resized wl_egl_window: surface now %dx%d", w, h);
    CHECK(eglSwapBuffers(dpy, surf), "swap after resize");
    wl_display_roundtrip(display);
    CHECK(srv.last_width == 80 && srv.last_height == 40 && srv.buffers_unknown == 0,
          "new buffers at %dx%d reach KWin", srv.last_width, srv.last_height);
    struct { int robust_attrib_seen, client_version; char shader[512]; int tex_uploads, create_image_target,
             query_counter_calls, swaps, swap_interval_calls, last_swap_interval, readers, geometry_calls; } *fake =
        getenv("CHAMELEON_ANDROID_MEDIANDK") /* the fake driver, loaded by the vendor */
            ? dlsym(dlopen(getenv("CHAMELEON_ANDROID_MEDIANDK"), RTLD_NOW | RTLD_NOLOAD), "fake_state")
            : NULL;
    CHECK(fake && fake->readers == 1 && fake->geometry_calls == 1,
          "the window was resized in place: %d reader, %d resize", fake ? fake->readers : -1,
          fake ? fake->geometry_calls : -1);

    CHECK(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), "release the surface");
    CHECK(eglDestroySurface(dpy, surf), "eglDestroySurface");
    wl_egl_window_destroy(window);
    wl_surface_destroy(surface);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);

    srv.stop = 1;
    pthread_join(thread, NULL);
    wl_display_destroy(server);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
