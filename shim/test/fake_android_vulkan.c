/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host tests' stand-in for Android's /system/lib64/libvulkan.so: a real
 * Vulkan driver (Mesa's lavapipe, or FAKE_VULKAN_DRIVER) behind Android's
 * entry point vkGetInstanceProcAddr, with the things in which Android's
 * loader differs from a plain driver:
 *  - it is a loader itself: another loader's structures in a create call's
 *    chain aren't for it (it is stricter about them here than Android),
 *  - its lists hold extensions that only mean something on Android.
 * And it checks what no driver would: that a device group names the driver's
 * own physical device.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

static PFN_vkGetInstanceProcAddr driver;

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
    return ((PFN_vkCreateInstance)driver(NULL, "vkCreateInstance"))(info, allocator, instance);
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

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load);
    PFN_vkVoidFunction fn = driver(instance, name);
    if (!fn)
        return NULL;
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
