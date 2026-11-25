/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host test of the Vulkan driver's X11 windows (vulkan/wsi.c, vendor/
 * window.c), end to end: a Vulkan app on the system's Vulkan loader ->
 * libvulkan_chameleon.so -> fake_android_vulkan.c (a real driver drawing,
 * an Android-like swapchain presenting into a fake AImageReader), with its
 * window on an X server.
 *
 * The X server is a stand-in of this file, on a socketpair the app's xcb
 * connection uses (xcb_connect_to_fd): just enough of the core protocol for
 * xcb (the connection setup, QueryExtension, GetGeometry, GetInputFocus,
 * FreePixmap) plus DRI3 1.0 (QueryVersion, PixmapFromBuffer) and Present 1.0
 * (QueryVersion, SelectInput, Pixmap with its events) as Xwayland answers
 * them: a presented pixmap is shown (its pixels looked at) and complete at
 * once, and the one shown before it is idle then, as after a flip. Like
 * Xwayland's gbm, it only takes a dmabuf that chameleon-kernel knows as a
 * session buffer.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <xcb/xcb.h>

#define VK_USE_PLATFORM_XCB_KHR
#include <vulkan/vulkan.h>

#include "check.h"

/* ---- the X server ---- */

#define DRI3_OPCODE 140
#define PRESENT_OPCODE 141
#define ROOT 0x100
#define WINDOW 0x200 /* any id is a window of this server's */
#define MAX_PIXMAPS 32

struct server {
    int fd;
    int dri3; /* offers DRI3 */
    pthread_mutex_t write_lock;
    pthread_t thread;
    uint16_t seq;
    int fds[8], nfds; /* fds that came with requests */
    int width, height, depth;
    uint32_t eid, event_mask, window_of_eid;
    struct {
        uint32_t id;
        int fd, width, height, stride;
        uint32_t size;
        int depth, bpp;
    } pixmaps[MAX_PIXMAPS];
    uint32_t shown_pixmap;
    int pixmaps_made, pixmaps_known, pixmaps_freed, presents, idles_sent, select_off;
    int last_width, last_height, last_stride, last_depth, last_bpp;
    uint8_t shown[4];
};

static void *(*buffer_ahb_acquire)(int);
static void (*buffer_ahb_release)(void *);

static void put(struct server *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n) {
        ssize_t w = write(s->fd, p, n);
        if (w <= 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

static void reply(struct server *s, uint8_t data, const void *body, size_t n)
{
    uint8_t r[32] = {1, data};
    memcpy(r + 2, &s->seq, 2);
    memcpy(r + 8, body, n > 24 ? 24 : n);
    pthread_mutex_lock(&s->write_lock);
    put(s, r, sizeof r);
    pthread_mutex_unlock(&s->write_lock);
}

/* A Present event: GenericEvent of `size` bytes, fields from offset 8. */
static void present_event(struct server *s, uint16_t type, const uint8_t *fields, size_t size)
{
    uint8_t e[64] = {35, PRESENT_OPCODE};
    memcpy(e + 2, &s->seq, 2);
    uint32_t length = (uint32_t)(size - 32) / 4;
    memcpy(e + 4, &length, 4);
    memcpy(e + 8, &type, 2);
    memcpy(e + 12, &s->eid, 4);
    memcpy(e + 16, &s->window_of_eid, 4);
    memcpy(e + 20, fields + 20, size - 20);
    pthread_mutex_lock(&s->write_lock);
    put(s, e, size);
    pthread_mutex_unlock(&s->write_lock);
}

static void configure_notify(struct server *s, int width, int height)
{
    s->width = width;
    s->height = height;
    uint8_t f[44] = {0};
    uint16_t w = (uint16_t)width, h = (uint16_t)height;
    memcpy(f + 24, &w, 2);
    memcpy(f + 26, &h, 2);
    present_event(s, 0, f, sizeof f);
}

static void handle(struct server *s, const uint8_t *req, size_t len)
{
    uint8_t op = req[0], minor = req[1];
    s->seq++;
    if (op == 98) { /* QueryExtension */
        uint16_t n;
        memcpy(&n, req + 4, 2);
        const char *name = (const char *)req + 8;
        uint8_t body[4] = {0};
        if (n == 4 && memcmp(name, "DRI3", 4) == 0 && s->dri3)
            body[0] = 1, body[1] = DRI3_OPCODE;
        else if (n == 7 && memcmp(name, "Present", 7) == 0)
            body[0] = 1, body[1] = PRESENT_OPCODE;
        reply(s, 0, body, sizeof body);
    } else if (op == 43) { /* GetInputFocus: xcb's sync for checked requests */
        uint32_t none = 0;
        reply(s, 0, &none, 4);
    } else if (op == 14) { /* GetGeometry */
        uint8_t body[16] = {0};
        uint32_t root = ROOT;
        uint16_t w = (uint16_t)s->width, h = (uint16_t)s->height;
        memcpy(body, &root, 4);
        memcpy(body + 8, &w, 2);
        memcpy(body + 10, &h, 2);
        reply(s, (uint8_t)s->depth, body, sizeof body);
    } else if (op == 54) { /* FreePixmap */
        uint32_t id;
        memcpy(&id, req + 4, 4);
        for (int i = 0; i < MAX_PIXMAPS; i++)
            if (s->pixmaps[i].id == id) {
                close(s->pixmaps[i].fd);
                s->pixmaps[i].id = 0;
                s->pixmaps_freed++;
            }
    } else if ((op == DRI3_OPCODE || op == PRESENT_OPCODE) && minor == 0) { /* QueryVersion: 1.0 */
        uint32_t v[2] = {1, 0};
        reply(s, 0, v, sizeof v);
    } else if (op == DRI3_OPCODE && minor == 2) { /* PixmapFromBuffer */
        int fd = s->nfds ? s->fds[0] : -1;
        if (s->nfds)
            memmove(s->fds, s->fds + 1, --s->nfds * sizeof *s->fds);
        int i = 0;
        while (i < MAX_PIXMAPS && s->pixmaps[i].id)
            i++;
        if (fd < 0 || i == MAX_PIXMAPS)
            return;
        memcpy(&s->pixmaps[i].id, req + 4, 4);
        memcpy(&s->pixmaps[i].size, req + 12, 4);
        uint16_t w, h, stride;
        memcpy(&w, req + 16, 2);
        memcpy(&h, req + 18, 2);
        memcpy(&stride, req + 20, 2);
        s->pixmaps[i].fd = fd;
        s->pixmaps[i].width = s->last_width = w;
        s->pixmaps[i].height = s->last_height = h;
        s->pixmaps[i].stride = s->last_stride = stride;
        s->pixmaps[i].depth = s->last_depth = req[22];
        s->pixmaps[i].bpp = s->last_bpp = req[23];
        s->pixmaps_made++;
        /* as Xwayland's gbm: only a buffer of the session */
        void *ahb = buffer_ahb_acquire ? buffer_ahb_acquire(fd) : NULL;
        if (ahb) {
            s->pixmaps_known++;
            buffer_ahb_release(ahb);
        }
    } else if (op == PRESENT_OPCODE && minor == 3) { /* SelectInput */
        uint32_t eid, window, mask;
        memcpy(&eid, req + 4, 4);
        memcpy(&window, req + 8, 4);
        memcpy(&mask, req + 12, 4);
        if (mask == 0)
            s->select_off++;
        s->eid = eid;
        s->window_of_eid = window;
        s->event_mask = mask;
    } else if (op == PRESENT_OPCODE && minor == 1) { /* Pixmap */
        uint32_t pixmap, serial;
        memcpy(&pixmap, req + 8, 4);
        memcpy(&serial, req + 12, 4);
        for (int i = 0; i < MAX_PIXMAPS; i++)
            if (s->pixmaps[i].id == pixmap) {
                uint8_t *pixels = mmap(NULL, 4, PROT_READ, MAP_SHARED, s->pixmaps[i].fd, 0);
                if (pixels != MAP_FAILED) {
                    memcpy(s->shown, pixels, 4);
                    munmap(pixels, 4);
                }
            }
        uint8_t f[44] = {0};
        if (s->shown_pixmap && (s->event_mask & 4)) { /* the one shown before: idle */
            memcpy(f + 20, &serial, 4);
            memcpy(f + 24, &s->shown_pixmap, 4);
            present_event(s, 2, f, 36);
            s->idles_sent++;
        }
        s->shown_pixmap = pixmap;
        if (s->event_mask & 2) { /* complete, kind pixmap, mode flip */
            memset(f, 0, sizeof f);
            f[10] = 0;
            f[11] = 1;
            memcpy(f + 20, &serial, 4);
            present_event(s, 1, f, 44);
        }
        /* last: the test's thread waits for this count, then looks */
        __atomic_add_fetch(&s->presents, 1, __ATOMIC_RELEASE);
    }
    (void)len;
}

/* The connection setup, then requests. */
static void *server_main(void *arg)
{
    struct server *s = arg;
    uint8_t buf[65536];
    size_t have = 0;
    int setup_done = 0;
    for (;;) {
        char control[CMSG_SPACE(4 * sizeof(int))];
        struct iovec iov = {buf + have, sizeof buf - have};
        struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
                            .msg_controllen = sizeof control};
        ssize_t n = recvmsg(s->fd, &mh, 0);
        if (n <= 0)
            return NULL;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
            if (c->cmsg_type == SCM_RIGHTS)
                for (size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int) && s->nfds < 8; i++)
                    memcpy(&s->fds[s->nfds++], CMSG_DATA(c) + i * sizeof(int), sizeof(int));
        have += (size_t)n;
        if (!setup_done) {
            if (have < 12)
                continue;
            uint16_t an, ad;
            memcpy(&an, buf + 6, 2);
            memcpy(&ad, buf + 8, 2);
            size_t need = 12 + ((an + 3u) & ~3u) + ((ad + 3u) & ~3u);
            if (have < need)
                continue;
            /* one screen, depth 24, one TrueColor visual */
            uint8_t r[8 + 112] = {1, 0, 11, 0, 0, 0};
            uint16_t length = 112 / 4;
            memcpy(r + 6, &length, 2);
            uint8_t *b = r + 8;
            uint32_t base = 0x00400000, mask = 0x001fffff;
            memcpy(b + 4, &base, 4);
            memcpy(b + 8, &mask, 4);
            uint16_t max_request = 65535;
            memcpy(b + 18, &max_request, 2);
            b[20] = 1; /* roots */
            b[21] = 1; /* pixmap formats */
            b[26] = 8, b[27] = 255;
            uint8_t *f = b + 32; /* depth 24, 32 bits a pixel */
            f[0] = 24, f[1] = 32, f[2] = 32;
            uint8_t *sc = f + 8;
            uint32_t root = ROOT, visual = 0x21;
            memcpy(sc, &root, 4);
            uint16_t sw = 1920, sh = 1080;
            memcpy(sc + 20, &sw, 2);
            memcpy(sc + 22, &sh, 2);
            memcpy(sc + 32, &visual, 4);
            sc[38] = 24; /* root depth */
            sc[39] = 1;  /* allowed depths */
            uint8_t *d = sc + 40;
            d[0] = 24;
            uint16_t nv = 1;
            memcpy(d + 2, &nv, 2);
            uint8_t *v = d + 8;
            memcpy(v, &visual, 4);
            v[4] = 4; /* TrueColor */
            v[5] = 8;
            uint32_t rm = 0xff0000, gm = 0xff00, bm = 0xff;
            memcpy(v + 8, &rm, 4);
            memcpy(v + 12, &gm, 4);
            memcpy(v + 16, &bm, 4);
            pthread_mutex_lock(&s->write_lock);
            put(s, r, sizeof r);
            pthread_mutex_unlock(&s->write_lock);
            memmove(buf, buf + need, have - need);
            have -= need;
            setup_done = 1;
        }
        while (have >= 4) {
            uint16_t units;
            memcpy(&units, buf + 2, 2);
            size_t len = (size_t)units * 4;
            if (len < 4 || have < len)
                break;
            handle(s, buf, len);
            memmove(buf, buf + len, have - len);
            have -= len;
        }
    }
}

static xcb_connection_t *server_start(struct server *s, int dri3, int width, int height)
{
    memset(s, 0, sizeof *s);
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
        return NULL;
    s->fd = pair[0];
    s->dri3 = dri3;
    s->width = width;
    s->height = height;
    s->depth = 24;
    pthread_mutex_init(&s->write_lock, NULL);
    pthread_create(&s->thread, NULL, server_main, s);
    xcb_connection_t *c = xcb_connect_to_fd(pair[1], NULL);
    return xcb_connection_has_error(c) ? NULL : c;
}

/* ---- the Vulkan app ---- */

static VkDevice vk_dev;
static VkQueue vk_queue;
static VkCommandBuffer vk_cmd;
static VkSemaphore vk_acquired, vk_drawn;

/* One frame: the next image, filled with a colour, presented. */
static struct server srv;

static VkResult vk_frame(VkSwapchainKHR swapchain, VkImage *images, float r, float g, float b)
{
    uint32_t i = 0;
    int before = __atomic_load_n(&srv.presents, __ATOMIC_ACQUIRE);
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
    /* The request is on its way: the server's thread may not have read it
     * yet. */
    for (int t = 0; (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && t < 2000 &&
                    __atomic_load_n(&srv.presents, __ATOMIC_ACQUIRE) == before;
         t++)
        usleep(1000);
    return result;
}

static int has_extension(const VkExtensionProperties *list, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(list[i].extensionName, name) == 0)
            return 1;
    return 0;
}


static int shown(const float *c)
{
    return srv.shown[0] == (int)(c[0] * 255) && srv.shown[1] == (int)(c[1] * 255) &&
           srv.shown[2] == (int)(c[2] * 255);
}

int main(void)
{
    alarm(60); /* a frame that never comes back fails instead of hanging */
    *(void **)&buffer_ahb_acquire = dlsym(RTLD_DEFAULT, "cham_buffer_ahb_acquire");
    *(void **)&buffer_ahb_release = dlsym(RTLD_DEFAULT, "cham_buffer_ahb_release");
    xcb_connection_t *c = server_start(&srv, 1, 64, 32);
    if (!c || !buffer_ahb_acquire) {
        fprintf(stderr, "run via run-host-test.sh\n");
        return 2;
    }
    printf("Vulkan windows on X11\n");
    VkExtensionProperties exts[256];
    uint32_t n = 256;
    vkEnumerateInstanceExtensionProperties(NULL, &n, exts);
    CHECK(has_extension(exts, n, "VK_KHR_surface") && has_extension(exts, n, "VK_KHR_xcb_surface") &&
              has_extension(exts, n, "VK_KHR_xlib_surface") && has_extension(exts, n, "VK_KHR_wayland_surface") &&
              !has_extension(exts, n, "VK_KHR_android_surface"),
          "xcb and Xlib surfaces offered next to Wayland ones (Android's own kind not)");
    const char *instance_exts[] = {"VK_KHR_surface", "VK_KHR_xcb_surface", "VK_KHR_wayland_surface"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = 3, .ppEnabledExtensionNames = instance_exts};
    VkInstance inst;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    CHECK(r == VK_SUCCESS, "an instance with xcb and Wayland surfaces both (%d)", r);
    if (r != VK_SUCCESS)
        return 1;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &phys);
    CHECK(vkGetPhysicalDeviceXcbPresentationSupportKHR(phys, 0, c, 0x21),
          "an X server with DRI3 and Present: the queue presents to it");

    VkXcbSurfaceCreateInfoKHR xsci = {.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, .connection = c,
                                      .window = WINDOW};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    r = vkCreateXcbSurfaceKHR(inst, &xsci, NULL, &surface);
    CHECK(r == VK_SUCCESS && surface && srv.event_mask == 7 && srv.window_of_eid == WINDOW,
          "a surface on an X11 window; Present's events for it asked for (%d)", r);
    if (r != VK_SUCCESS)
        return 1;
    VkBool32 can = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys, 0, surface, &can);
    VkSurfaceCapabilitiesKHR caps;
    r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    CHECK(can && r == VK_SUCCESS && caps.currentExtent.width == 64 && caps.currentExtent.height == 32 &&
              caps.minImageExtent.width == 64 && caps.maxImageExtent.height == 32,
          "an X11 window: its own size is the swapchain's (%ux%u)", caps.currentExtent.width,
          caps.currentExtent.height);
    CHECK(caps.supportedCompositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
          "a 24-bit window: opaque only");
    VkSurfaceFormatKHR formats[16];
    n = 16;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, formats);
    VkPresentModeKHR modes[8];
    uint32_t nm = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, modes);
    CHECK(n == 2 && formats[0].format == VK_FORMAT_R8G8B8A8_UNORM && nm == 2,
          "formats and present modes as a Wayland window's (%u, %u)", n, nm);

    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                   .pQueuePriorities = &priority};
    const char *device_exts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 1,
                              .ppEnabledExtensionNames = device_exts};
    r = vkCreateDevice(phys, &dci, NULL, &vk_dev);
    if (r != VK_SUCCESS)
        return 1;
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

    VkSwapchainCreateInfoKHR scci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
                                     .minImageCount = caps.minImageCount, .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
                                     .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                                     .imageExtent = caps.currentExtent, .imageArrayLayers = 1,
                                     .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                     .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                     .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(vk_dev, &scci, NULL, &swapchain);
    CHECK(r == VK_SUCCESS, "a FIFO swapchain of the window's size (%d)", r);
    if (r != VK_SUCCESS)
        return 1;
    VkImage images[16];
    uint32_t image_count = 16;
    vkGetSwapchainImagesKHR(vk_dev, swapchain, &image_count, images);

    static const float k_colours[10][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0, 1, 1},
                                           {1, 0, 1}, {1, 1, 1}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    int right = 0;
    r = VK_SUCCESS;
    for (int i = 0; i < 10 && r == VK_SUCCESS; i++) {
        r = vk_frame(swapchain, images, k_colours[i][0], k_colours[i][1], k_colours[i][2]);
        right += shown(k_colours[i]);
    }
    CHECK(r == VK_SUCCESS && srv.presents == 10 && right == 10,
          "10 frames -> %d PresentPixmaps, each with its colour on the X server (%d)", srv.presents, right);
    CHECK(srv.pixmaps_made >= 2 && srv.pixmaps_made <= 5 && srv.pixmaps_known == srv.pixmaps_made,
          "%d zero-copy pixmaps (DRI3), each a buffer of the session for the server's gbm", srv.pixmaps_made);
    CHECK(srv.last_width == 64 && srv.last_height == 32 && srv.last_stride == 256 && srv.last_depth == 24 &&
              srv.last_bpp == 32,
          "pixmaps %dx%d, stride %d, the window's depth %d, %d bits a pixel", srv.last_width, srv.last_height,
          srv.last_stride, srv.last_depth, srv.last_bpp);
    CHECK(srv.idles_sent == 9, "the buffers the server let go of went back to the window's reader (%d)",
          srv.idles_sent);

    /* The window gets another size: the next present says so, and the
     * program makes a swapchain of the new size. */
    configure_notify(&srv, 96, 48);
    r = vk_frame(swapchain, images, 0, 0, 1);
    CHECK(r == VK_SUBOPTIMAL_KHR, "after the window grew, a present is suboptimal (%d)", r);
    r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    CHECK(r == VK_SUCCESS && caps.currentExtent.width == 96 && caps.currentExtent.height == 48,
          "...and the surface is 96x48 now (%ux%u)", caps.currentExtent.width, caps.currentExtent.height);
    VkSwapchainKHR old = swapchain;
    scci.imageExtent = caps.currentExtent;
    scci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    scci.oldSwapchain = old;
    r = vkCreateSwapchainKHR(vk_dev, &scci, NULL, &swapchain);
    vkDestroySwapchainKHR(vk_dev, old, NULL);
    image_count = 16;
    vkGetSwapchainImagesKHR(vk_dev, swapchain, &image_count, images);
    int made = srv.pixmaps_made;
    right = 0;
    for (int i = 0; i < 4 && (r == VK_SUCCESS); i++) {
        r = vk_frame(swapchain, images, k_colours[i][0], k_colours[i][1], k_colours[i][2]);
        right += shown(k_colours[i]);
    }
    CHECK(r == VK_SUCCESS && right == 4 && srv.pixmaps_made > made && srv.last_width == 96 && srv.last_height == 48,
          "a mailbox swapchain of 96x48: 4 frames shown, as new pixmaps of that size (%d)", r);

    vkDestroySwapchainKHR(vk_dev, swapchain, NULL);
    vkDestroySemaphore(vk_dev, vk_acquired, NULL);
    vkDestroySemaphore(vk_dev, vk_drawn, NULL);
    vkDestroyCommandPool(vk_dev, pool, NULL);
    vkDestroyDevice(vk_dev, NULL);
    vkDestroySurfaceKHR(inst, surface, NULL);
    free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL)); /* everything sent has arrived */
    CHECK(srv.pixmaps_freed == srv.pixmaps_made && srv.select_off == 1,
          "the surface gone: its %d pixmaps freed, Present's events for it off", srv.pixmaps_freed);

    /* An X server without DRI3 can't take the buffers: no presenting, and
     * no surface. */
    static struct server plain;
    xcb_connection_t *c2 = server_start(&plain, 0, 64, 32);
    CHECK(c2 && !vkGetPhysicalDeviceXcbPresentationSupportKHR(phys, 0, c2, 0x21),
          "an X server without DRI3: the queue can't present to it");
    xsci.connection = c2;
    surface = VK_NULL_HANDLE;
    r = vkCreateXcbSurfaceKHR(inst, &xsci, NULL, &surface);
    CHECK(r == VK_ERROR_INITIALIZATION_FAILED && !surface, "...and no surface on its windows (%d)", r);
    vkDestroyInstance(inst, NULL);
    xcb_disconnect(c2);
    xcb_disconnect(c);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
