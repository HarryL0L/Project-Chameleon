/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host tests' stand-in for Android's /system/lib64/libvulkan.so: a real
 * Vulkan driver (Mesa's lavapipe, or FAKE_VULKAN_DRIVER) behind Android's
 * entry point vkGetInstanceProcAddr, with the things in which Android's
 * loader differs from a plain driver:
 *  - it is a loader itself: another loader's structures in a create call's
 *    chain aren't for it (it is stricter about them here than Android),
 *  - its lists hold extensions that only mean something on Android,
 *  - its windows are Android's: VK_KHR_android_surface on an ANativeWindow
 *    (fake_android_gl.c's AImageReader, CHAMELEON_ANDROID_MEDIANDK), with a
 *    swapchain whose presented images arrive in that reader. The images are
 *    the driver's, with memory this file can read; a present copies one
 *    into a buffer of the window. Like Android's, a surface knows no
 *    Wayland, only "inherit" for its alpha, and no size but its window's.
 * And it checks what no driver would: that a device group names the driver's
 * own physical device.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <vulkan/vulkan_core.h>

struct ANativeWindow;
struct AHardwareBuffer;
#include <vulkan/vulkan_android.h>

static PFN_vkGetInstanceProcAddr driver;
static VkInstance g_instance;           /* the driver's, for its functions */
static VkPhysicalDevice g_physical;     /* of the last device */
static PFN_vkGetDeviceProcAddr real_device_proc;
/* Taken from the driver when an instance is made, as Android does; a driver
 * keeping to a 1.0 instance (lavapipe, like any Mesa driver) gives none. */
static PFN_vkGetPhysicalDeviceImageFormatProperties2 image_format2;

/* fake_android_gl.c's window system */
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} ahb_desc;
typedef struct {
    int version, numFds, numInts;
    int data[];
} native_handle;
static struct {
    struct AHardwareBuffer *(*window_queue)(struct ANativeWindow *);
    int (*set_geometry)(struct ANativeWindow *, int32_t, int32_t, int32_t);
    void (*describe)(const struct AHardwareBuffer *, ahb_desc *);
    const native_handle *(*get_native_handle)(const struct AHardwareBuffer *);
} win;

static void load(void)
{
    const char *path = getenv("FAKE_VULKAN_DRIVER");
    void *lib = dlopen(path ? path : "libvulkan_lvp.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "fake_android_vulkan: %s\n", dlerror());
        abort();
    }
    VkResult (*negotiate)(uint32_t *) = dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
    uint32_t version = 5;
    if (negotiate)
        negotiate(&version);
    *(void **)&driver = dlsym(lib, "vk_icdGetInstanceProcAddr");
    const char *windows = getenv("CHAMELEON_ANDROID_MEDIANDK");
    void *wlib = windows ? dlopen(windows, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (wlib) {
        *(void **)&win.window_queue = dlsym(wlib, "fake_window_queue");
        *(void **)&win.set_geometry = dlsym(wlib, "ANativeWindow_setBuffersGeometry");
        *(void **)&win.describe = dlsym(wlib, "AHardwareBuffer_describe");
        *(void **)&win.get_native_handle = dlsym(wlib, "AHardwareBuffer_getNativeHandle");
    }
}

static int for_another_loader(const void *chain)
{
    for (const VkBaseInStructure *s = chain; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
            s->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            fprintf(stderr, "fake_android_vulkan: another loader's structure in the chain\n");
            return 1;
        }
    }
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *info,
                                                     const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    if (for_another_loader(info->pNext))
        return VK_ERROR_INITIALIZATION_FAILED;
    /* Android's own extension isn't the driver's; another window system's
     * isn't Android's. */
    VkInstanceCreateInfo mine = *info;
    const char *names[64];
    mine.enabledExtensionCount = 0;
    mine.ppEnabledExtensionNames = names;
    for (uint32_t i = 0; i < info->enabledExtensionCount && i < 64; i++) {
        const char *name = info->ppEnabledExtensionNames[i];
        if (strcmp(name, "VK_KHR_wayland_surface") == 0 || strcmp(name, "VK_KHR_xcb_surface") == 0) {
            fprintf(stderr, "fake_android_vulkan: %s isn't Android's\n", name);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        if (strcmp(name, "VK_KHR_android_surface") != 0)
            names[mine.enabledExtensionCount++] = name;
    }
    VkResult result = ((PFN_vkCreateInstance)driver(NULL, "vkCreateInstance"))(&mine, allocator, instance);
    if (result == VK_SUCCESS) {
        g_instance = *instance;
        image_format2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)driver(
            *instance, "vkGetPhysicalDeviceImageFormatProperties2");
    }
    return result;
}

static PFN_vkCreateDevice real_create_device;

static VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                                   const VkAllocationCallbacks *allocator, VkDevice *device)
{
    if (for_another_loader(info->pNext))
        return VK_ERROR_INITIALIZATION_FAILED;
    for (const VkBaseInStructure *s = info->pNext; s; s = s->pNext) {
        const VkDeviceGroupDeviceCreateInfo *group = (const void *)s;
        if (s->sType != VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO)
            continue;
        for (uint32_t i = 0; i < group->physicalDeviceCount; i++) {
            if (group->pPhysicalDevices[i] != physical) {
                fprintf(stderr, "fake_android_vulkan: the device group names something else\n");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
    }
    g_physical = physical;
    return real_create_device(physical, info, allocator, device);
}

/* The driver's list and one more name, the way the driver gives lists. */
static VkResult with_one_more(VkResult result, const char *name, uint32_t driver_count, uint32_t *count,
                              VkExtensionProperties *out, uint32_t room)
{
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        return result;
    if (!out) {
        *count = driver_count + 1;
        return VK_SUCCESS;
    }
    if (result == VK_INCOMPLETE || *count >= room)
        return VK_INCOMPLETE;
    memset(&out[*count], 0, sizeof *out);
    strcpy(out[*count].extensionName, name);
    out[(*count)++].specVersion = 1;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL instance_extensions(const char *layer, uint32_t *count,
                                                         VkExtensionProperties *out)
{
    PFN_vkEnumerateInstanceExtensionProperties real =
        (PFN_vkEnumerateInstanceExtensionProperties)driver(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t room = *count, n = 0;
    real(layer, &n, NULL);
    return with_one_more(real(layer, count, out), "VK_KHR_android_surface", n, count, out, room);
}

static PFN_vkEnumerateDeviceExtensionProperties real_device_extensions;

static VKAPI_ATTR VkResult VKAPI_CALL device_extensions(VkPhysicalDevice physical, const char *layer,
                                                       uint32_t *count, VkExtensionProperties *out)
{
    uint32_t room = *count, n = 0;
    real_device_extensions(physical, layer, &n, NULL);
    return with_one_more(real_device_extensions(physical, layer, count, out),
                         "VK_ANDROID_external_memory_android_hardware_buffer", n, count, out, room);
}

/* ---- Android's windows ---- */

struct fake_surface {
    struct ANativeWindow *window;
};

static VKAPI_ATTR VkResult VKAPI_CALL create_android_surface(VkInstance instance,
                                                            const VkAndroidSurfaceCreateInfoKHR *info,
                                                            const VkAllocationCallbacks *allocator,
                                                            VkSurfaceKHR *surface)
{
    (void)instance, (void)allocator;
    if (!win.window_queue || !info->window)
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    struct fake_surface *s = calloc(1, sizeof *s);
    s->window = info->window;
    *surface = (VkSurfaceKHR)s;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL destroy_surface(VkInstance instance, VkSurfaceKHR surface,
                                                 const VkAllocationCallbacks *allocator)
{
    (void)instance, (void)allocator;
    free((struct fake_surface *)surface);
}

static VKAPI_ATTR VkResult VKAPI_CALL surface_support(VkPhysicalDevice physical, uint32_t family,
                                                     VkSurfaceKHR surface, VkBool32 *supported)
{
    (void)physical, (void)family, (void)surface;
    *supported = VK_TRUE;
    return VK_SUCCESS;
}

/* What a Mali phone answers for an AImageReader's window. */
static VKAPI_ATTR VkResult VKAPI_CALL surface_capabilities(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                                          VkSurfaceCapabilitiesKHR *caps)
{
    (void)physical, (void)surface;
    *caps = (VkSurfaceCapabilitiesKHR){
        .minImageCount = 3, .maxImageCount = 64, .currentExtent = {1, 1}, .minImageExtent = {1, 1},
        .maxImageExtent = {4096, 4096}, .maxImageArrayLayers = 1,
        .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR,
        .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, .supportedUsageFlags = 0x9f};
    return VK_SUCCESS;
}

static VkResult list(const void *all, uint32_t n, size_t size, uint32_t *count, void *out)
{
    if (!out) {
        *count = n;
        return VK_SUCCESS;
    }
    VkResult result = *count < n ? VK_INCOMPLETE : VK_SUCCESS;
    if (*count > n)
        *count = n;
    memcpy(out, all, *count * size);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL surface_formats(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                                     uint32_t *count, VkSurfaceFormatKHR *formats)
{
    (void)surface;
    /* Android 17 asks the driver about an RGB format here, through a
     * function it never checks. */
    if (!image_format2) {
        fprintf(stderr, "fake_android_vulkan: the window formats call the driver's "
                        "vkGetPhysicalDeviceImageFormatProperties2, and it has none (Android 17 crashes)\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkPhysicalDeviceImageFormatInfo2 format_info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                                    .format = VK_FORMAT_R8G8B8A8_UNORM, .type = VK_IMAGE_TYPE_2D,
                                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImageFormatProperties2 format_props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    image_format2(physical, &format_info, &format_props);
    static const VkSurfaceFormatKHR k_formats[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R5G6B5_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    return list(k_formats, 5, sizeof *formats, count, formats);
}

static VKAPI_ATTR VkResult VKAPI_CALL surface_modes(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count,
                                                   VkPresentModeKHR *modes)
{
    (void)physical, (void)surface;
    static const VkPresentModeKHR k_modes[] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR,
                                               VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR,
                                               VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR};
    return list(k_modes, 4, sizeof *modes, count, modes);
}

#define IMAGES_MAX 8
struct fake_swapchain {
    VkDevice device;
    struct fake_surface *surface;
    uint32_t n, width, height, next;
    VkImage image[IMAGES_MAX];
    VkDeviceMemory memory[IMAGES_MAX];
    const uint8_t *pixels[IMAGES_MAX];
    VkSubresourceLayout layout[IMAGES_MAX];
    int held[IMAGES_MAX]; /* by the program */
};

#define DEVICE_FN(device, name) ((PFN_##name)real_device_proc(device, #name))

static VKAPI_ATTR void VKAPI_CALL destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
                                                   const VkAllocationCallbacks *allocator)
{
    (void)allocator;
    struct fake_swapchain *sc = (struct fake_swapchain *)swapchain;
    if (!sc)
        return;
    DEVICE_FN(device, vkDeviceWaitIdle)(device);
    for (uint32_t i = 0; i < sc->n; i++) {
        DEVICE_FN(device, vkDestroyImage)(device, sc->image[i], NULL);
        DEVICE_FN(device, vkFreeMemory)(device, sc->memory[i], NULL);
    }
    free(sc);
}

static VKAPI_ATTR VkResult VKAPI_CALL create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                                      const VkAllocationCallbacks *allocator,
                                                      VkSwapchainKHR *swapchain)
{
    (void)allocator;
    struct fake_surface *surface = (struct fake_surface *)info->surface;
    if (info->compositeAlpha != VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR ||
        info->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
        (info->imageFormat != VK_FORMAT_R8G8B8A8_UNORM && info->imageFormat != VK_FORMAT_R8G8B8A8_SRGB)) {
        fprintf(stderr, "fake_android_vulkan: a swapchain Android's surface can't have (alpha 0x%x, format %d)\n",
                info->compositeAlpha, info->imageFormat);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkPhysicalDeviceMemoryProperties mem;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)driver(g_instance, "vkGetPhysicalDeviceMemoryProperties"))(g_physical,
                                                                                                         &mem);
    struct fake_swapchain *sc = calloc(1, sizeof *sc);
    sc->device = device;
    sc->surface = surface;
    sc->width = info->imageExtent.width;
    sc->height = info->imageExtent.height;
    /* Android adds the buffers its window's reader may hold. */
    sc->n = (info->minImageCount < 3 ? 3 : info->minImageCount) + 3;
    if (sc->n > IMAGES_MAX)
        sc->n = IMAGES_MAX;
    for (uint32_t i = 0; i < sc->n; i++) {
        VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                 .format = info->imageFormat, .extent = {sc->width, sc->height, 1}, .mipLevels = 1,
                                 .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_LINEAR, .usage = info->imageUsage};
        VkMemoryRequirements need;
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        void *mapped = NULL;
        VkResult r = DEVICE_FN(device, vkCreateImage)(device, &ici, NULL, &sc->image[i]);
        if (r == VK_SUCCESS) {
            DEVICE_FN(device, vkGetImageMemoryRequirements)(device, sc->image[i], &need);
            mai.allocationSize = need.size;
            for (uint32_t t = 0; t < mem.memoryTypeCount; t++) {
                if ((need.memoryTypeBits & 1u << t) &&
                    (mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                    mai.memoryTypeIndex = t;
                    break;
                }
            }
            r = DEVICE_FN(device, vkAllocateMemory)(device, &mai, NULL, &sc->memory[i]);
        }
        if (r == VK_SUCCESS)
            r = DEVICE_FN(device, vkBindImageMemory)(device, sc->image[i], sc->memory[i], 0);
        if (r == VK_SUCCESS)
            r = DEVICE_FN(device, vkMapMemory)(device, sc->memory[i], 0, VK_WHOLE_SIZE, 0, &mapped);
        if (r != VK_SUCCESS) {
            fprintf(stderr, "fake_android_vulkan: no swapchain image (%d)\n", r);
            abort();
        }
        VkImageSubresource whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        DEVICE_FN(device, vkGetImageSubresourceLayout)(device, sc->image[i], &whole, &sc->layout[i]);
        sc->pixels[i] = mapped;
    }
    win.set_geometry(surface->window, (int32_t)sc->width, (int32_t)sc->height, 0);
    *swapchain = (VkSwapchainKHR)sc;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL swapchain_images(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count,
                                                      VkImage *images)
{
    (void)device;
    struct fake_swapchain *sc = (struct fake_swapchain *)swapchain;
    return list(sc->image, sc->n, sizeof *images, count, images);
}

static VKAPI_ATTR VkResult VKAPI_CALL acquire_image(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                   VkSemaphore semaphore, VkFence fence, uint32_t *index)
{
    (void)timeout;
    struct fake_swapchain *sc = (struct fake_swapchain *)swapchain;
    uint32_t tried = 0;
    while (tried < sc->n && sc->held[sc->next]) {
        sc->next = (sc->next + 1) % sc->n;
        tried++;
    }
    if (tried == sc->n)
        return VK_TIMEOUT;
    *index = sc->next;
    sc->held[sc->next] = 1;
    sc->next = (sc->next + 1) % sc->n;
    if (semaphore || fence) { /* the image is ready at once */
        VkQueue queue;
        DEVICE_FN(device, vkGetDeviceQueue)(device, 0, 0, &queue);
        VkSubmitInfo signal = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = semaphore ? 1 : 0,
                               .pSignalSemaphores = &semaphore};
        DEVICE_FN(device, vkQueueSubmit)(queue, 1, &signal, fence);
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
    VkResult result = VK_SUCCESS;
    for (uint32_t k = 0; k < info->swapchainCount; k++) {
        struct fake_swapchain *sc = (struct fake_swapchain *)info->pSwapchains[k];
        uint32_t i = info->pImageIndices[k];
        VkDevice device = sc->device;
        if (k == 0 && info->waitSemaphoreCount) { /* the frame must be drawn first */
            VkPipelineStageFlags stages[8];
            for (int s = 0; s < 8; s++)
                stages[s] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .waitSemaphoreCount = info->waitSemaphoreCount,
                                 .pWaitSemaphores = info->pWaitSemaphores, .pWaitDstStageMask = stages};
            DEVICE_FN(device, vkQueueSubmit)(queue, 1, &wait, VK_NULL_HANDLE);
        }
        DEVICE_FN(device, vkQueueWaitIdle)(queue);
        /* Into the window's queue, as Android's swapchain queues its buffer. */
        struct AHardwareBuffer *buffer = win.window_queue(sc->surface->window);
        VkResult one = VK_SUCCESS;
        if (!buffer) {
            fprintf(stderr, "fake_android_vulkan: the window's reader holds every buffer (Android would block)\n");
            one = VK_ERROR_DEVICE_LOST;
        } else {
            ahb_desc desc;
            win.describe(buffer, &desc);
            size_t size = (size_t)desc.stride * 4 * desc.height;
            uint8_t *to = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, win.get_native_handle(buffer)->data[0], 0);
            uint32_t width = desc.width < sc->width ? desc.width : sc->width;
            for (uint32_t y = 0; to != MAP_FAILED && y < desc.height && y < sc->height; y++)
                memcpy(to + (size_t)y * desc.stride * 4,
                       sc->pixels[i] + sc->layout[i].offset + y * sc->layout[i].rowPitch, (size_t)width * 4);
            if (to != MAP_FAILED)
                munmap(to, size);
        }
        sc->held[i] = 0;
        if (info->pResults)
            info->pResults[k] = one;
        if (one != VK_SUCCESS)
            result = one;
    }
    return result;
}

static PFN_vkVoidFunction own_device_function(const char *name)
{
    static const struct {
        const char *name;
        PFN_vkVoidFunction fn;
    } k_own[] = {
        {"vkCreateSwapchainKHR", (PFN_vkVoidFunction)create_swapchain},
        {"vkDestroySwapchainKHR", (PFN_vkVoidFunction)destroy_swapchain},
        {"vkGetSwapchainImagesKHR", (PFN_vkVoidFunction)swapchain_images},
        {"vkAcquireNextImageKHR", (PFN_vkVoidFunction)acquire_image},
        {"vkQueuePresentKHR", (PFN_vkVoidFunction)queue_present},
    };
    for (size_t i = 0; i < sizeof k_own / sizeof *k_own; i++)
        if (strcmp(name, k_own[i].name) == 0)
            return k_own[i].fn;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice device, const char *name)
{
    PFN_vkVoidFunction own = own_device_function(name);
    return own ? own : real_device_proc(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load);
    if (instance) { /* Android's window system, which the driver doesn't have */
        static const struct {
            const char *name;
            PFN_vkVoidFunction fn;
        } k_own[] = {
            {"vkCreateAndroidSurfaceKHR", (PFN_vkVoidFunction)create_android_surface},
            {"vkDestroySurfaceKHR", (PFN_vkVoidFunction)destroy_surface},
            {"vkGetPhysicalDeviceSurfaceSupportKHR", (PFN_vkVoidFunction)surface_support},
            {"vkGetPhysicalDeviceSurfaceCapabilitiesKHR", (PFN_vkVoidFunction)surface_capabilities},
            {"vkGetPhysicalDeviceSurfaceFormatsKHR", (PFN_vkVoidFunction)surface_formats},
            {"vkGetPhysicalDeviceSurfacePresentModesKHR", (PFN_vkVoidFunction)surface_modes},
        };
        for (size_t i = 0; i < sizeof k_own / sizeof *k_own; i++)
            if (strcmp(name, k_own[i].name) == 0)
                return k_own[i].fn;
        if (own_device_function(name))
            return own_device_function(name);
    }
    PFN_vkVoidFunction fn = driver(instance, name);
    if (!fn)
        return NULL;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
        real_device_proc = (PFN_vkGetDeviceProcAddr)fn;
        return (PFN_vkVoidFunction)device_proc;
    }
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)create_instance;
    if (strcmp(name, "vkCreateDevice") == 0) {
        real_create_device = (PFN_vkCreateDevice)fn;
        return (PFN_vkVoidFunction)create_device;
    }
    if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)instance_extensions;
    if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0) {
        real_device_extensions = (PFN_vkEnumerateDeviceExtensionProperties)fn;
        return (PFN_vkVoidFunction)device_extensions;
    }
    return fn;
}
