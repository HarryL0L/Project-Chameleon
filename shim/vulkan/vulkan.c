/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libvulkan_chameleon.so: the card's Vulkan driver, for the Vulkan loader (an
 * "ICD", named to the loader by the launcher), on Android's own Vulkan
 * driver - as libEGL_chameleon.so is the card's EGL on Android's EGL.
 *
 * The loader writes its dispatch pointer into the first word of every
 * instance, physical device, device, queue and command buffer a driver hands
 * out, and Android's libvulkan.so keeps its own there. So programs get
 * wrappers of those five kinds of object, and every function unwraps: the
 * forwarders gen_vk.py generates (vk_forward.h) for a first argument, the
 * functions here wherever else one travels. gen_vk.py finds those and fails
 * if one isn't written.
 *
 * Offered are the driver's extensions that vk_forward.h knows, minus what
 * gen_vk.py leaves out (windows, for now), and Vulkan up to the header's
 * version. VK_EXT_physical_device_drm names the card's nodes, which is how
 * a compositor matches this driver to the card it shows on.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <vulkan/vulkan_core.h>

#include "../../common/chameleon_drm.h"

#define COUNT(a) (sizeof(a) / sizeof(*(a)))
#define ICD_EXPORT __attribute__((visibility("default"))) VKAPI_ATTR
#define ICD_LOADER_MAGIC 0x01CDC0DE /* the loader's vk_icd.h: a new object's first word */

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

#include "vk_forward.h"

#define INST_FN(obj, name) ((PFN_##name)(obj)->t->fn[I_##name])
#define DEV_FN(obj, name) ((PFN_##name)(obj)->t->fn[D_##name])

struct instance {
    struct inst_obj o;
    struct instance_table table;
    PFN_vkGetDeviceProcAddr device_proc;
    pthread_mutex_t lock; /* guards the list */
    struct physical **physical;
    uint32_t physical_count;
};

struct physical {
    struct inst_obj o;
    struct instance *instance;
};

struct queue {
    struct dev_obj o;
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
    pthread_mutex_t lock; /* guards the lists */
    struct queue *queues;
    struct cmd *cmds; /* every command buffer: a pool's go with the pool */
};

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "chameleon: vulkan: %s\n", buf);
}

/* ---- Android's driver ---- */

static PFN_vkGetInstanceProcAddr g_android;

static void load(void)
{
    /* CHAMELEON_ANDROID_VULKAN: a stand-in library for the host tests */
    const char *path = getenv("CHAMELEON_ANDROID_VULKAN");
    if (!path)
        path = sizeof(void *) == 8 ? "/system/lib64/libvulkan.so" : "/system/lib/libvulkan.so";
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        say("cannot load Android's libvulkan: %s", dlerror());
        return;
    }
    *(void **)&g_android = dlsym(lib, "vkGetInstanceProcAddr");
}

/* One of Android's functions that need no instance, or NULL. */
static PFN_vkVoidFunction android(const char *name)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load);
    return g_android ? g_android(VK_NULL_HANDLE, name) : NULL;
}

/* ---- names ---- */

static int by_name(const void *name, const void *entry)
{
    return strcmp(name, ((const struct entry *)entry)->name);
}

static const struct entry *find(const char *name)
{
    return bsearch(name, entries, COUNT(entries), sizeof *entries, by_name);
}

static int by_string(const void *name, const void *string)
{
    return strcmp(name, *(const char *const *)string);
}

static int known_extension(const char *name)
{
    return bsearch(name, known_extensions, COUNT(known_extensions), sizeof *known_extensions, by_string) != NULL;
}

/* A driver's version, no newer than the functions here. */
static uint32_t known_version(uint32_t version)
{
    if (version >> 12 > CHAM_VK_VERSION >> 12)
        return CHAM_VK_VERSION | VK_API_VERSION_PATCH(version);
    return version;
}

/* The loader leaves its own structures at the start of a create call's
 * chain, made for its layers; Android's loader uses the same kind for its
 * own layers, and must not take these for them. */
static const void *without_loader_info(const void *chain, VkStructureType type)
{
    const VkBaseInStructure *s = chain;
    while (s && s->sType == type)
        s = s->pNext;
    return s;
}

/* ---- the card's nodes ---- */

static VkBool32 card_node(const char *path, int64_t *node_major, int64_t *node_minor)
{
    struct stat st;
    int there = stat(path, &st) == 0 && S_ISCHR(st.st_mode);
    *node_major = there ? major(st.st_rdev) : 0;
    *node_minor = there ? minor(st.st_rdev) : 0;
    return there;
}

static int card_present(void)
{
    int64_t node_major, node_minor;
    return card_node(CHAM_RENDER_PATH, &node_major, &node_minor);
}

/* ---- extension lists ---- */

/* Gives the caller, the way vkEnumerate*ExtensionProperties does, the n
 * extensions of `all` that are known here, plus the card's with with_card
 * (`all` has room for it). Frees `all`. */
static VkResult give_extensions(VkExtensionProperties *all, uint32_t n, int with_card, uint32_t *count,
                                VkExtensionProperties *out)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++)
        if (known_extension(all[i].extensionName))
            all[kept++] = all[i];
    if (with_card) {
        memset(&all[kept], 0, sizeof *all);
        strcpy(all[kept].extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
        all[kept++].specVersion = VK_EXT_PHYSICAL_DEVICE_DRM_SPEC_VERSION;
    }
    VkResult result = VK_SUCCESS;
    if (out) {
        if (*count < kept) {
            kept = *count;
            result = VK_INCOMPLETE;
        }
        memcpy(out, all, kept * sizeof *all);
    }
    *count = kept;
    free(all);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkEnumerateInstanceExtensionProperties(const char *pLayerName,
                                                                                 uint32_t *pPropertyCount,
                                                                                 VkExtensionProperties *pProperties)
{
    PFN_vkEnumerateInstanceExtensionProperties real =
        (PFN_vkEnumerateInstanceExtensionProperties)android("vkEnumerateInstanceExtensionProperties");
    if (pLayerName || !real)
        return VK_ERROR_LAYER_NOT_PRESENT;
    uint32_t n = 0;
    VkResult result = real(NULL, &n, NULL);
    if (result != VK_SUCCESS)
        return result;
    VkExtensionProperties *all = malloc((n + 1) * sizeof *all);
    if (!all)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = real(NULL, &n, all);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        free(all);
        return result;
    }
    return give_extensions(all, n, 0, pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                               const char *pLayerName,
                                                                               uint32_t *pPropertyCount,
                                                                               VkExtensionProperties *pProperties)
{
    struct inst_obj *p = (struct inst_obj *)physicalDevice;
    PFN_vkEnumerateDeviceExtensionProperties real = INST_FN(p, vkEnumerateDeviceExtensionProperties);
    if (pLayerName)
        return VK_ERROR_LAYER_NOT_PRESENT;
    uint32_t n = 0;
    VkResult result = real(p->real, NULL, &n, NULL);
    if (result != VK_SUCCESS)
        return result;
    VkExtensionProperties *all = malloc((n + 1) * sizeof *all);
    if (!all)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = real(p->real, NULL, &n, all);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        free(all);
        return result;
    }
    return give_extensions(all, n, card_present(), pPropertyCount, pProperties);
}

/* Layers are the loader's. */
static VKAPI_ATTR VkResult VKAPI_CALL cham_vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                                                             VkLayerProperties *pProperties)
{
    (void)pProperties;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                                                           uint32_t *pPropertyCount,
                                                                           VkLayerProperties *pProperties)
{
    (void)physicalDevice;
    (void)pProperties;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkEnumerateInstanceVersion(uint32_t *pApiVersion)
{
    PFN_vkEnumerateInstanceVersion real = (PFN_vkEnumerateInstanceVersion)android("vkEnumerateInstanceVersion");
    *pApiVersion = VK_API_VERSION_1_0;
    VkResult result = real ? real(pApiVersion) : VK_SUCCESS;
    *pApiVersion = known_version(*pApiVersion);
    return result;
}

/* ---- instances ---- */

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                           const VkAllocationCallbacks *pAllocator,
                                                           VkInstance *pInstance)
{
    PFN_vkCreateInstance real = (PFN_vkCreateInstance)android("vkCreateInstance");
    if (!real)
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    struct instance *inst = calloc(1, sizeof *inst);
    if (!inst)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkInstanceCreateInfo info = *pCreateInfo;
    info.pNext = without_loader_info(info.pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    VkInstance created;
    VkResult result = real(&info, pAllocator, &created);
    if (result != VK_SUCCESS) {
        free(inst);
        return result;
    }
    inst->o.loader = ICD_LOADER_MAGIC;
    inst->o.real = created;
    inst->o.t = &inst->table;
    for (size_t i = 0; i < COUNT(entries); i++)
        if (entries[i].level == LEVEL_INSTANCE || entries[i].level == LEVEL_PHYSICAL)
            inst->table.fn[entries[i].slot] = g_android(created, entries[i].name);
    inst->device_proc = (PFN_vkGetDeviceProcAddr)g_android(created, "vkGetDeviceProcAddr");
    pthread_mutex_init(&inst->lock, NULL);
    *pInstance = (VkInstance)inst;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL cham_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
    struct instance *inst = (struct instance *)instance;
    if (!inst)
        return;
    INST_FN(&inst->o, vkDestroyInstance)(inst->o.real, pAllocator);
    for (uint32_t i = 0; i < inst->physical_count; i++)
        free(inst->physical[i]);
    free(inst->physical);
    pthread_mutex_destroy(&inst->lock);
    free(inst);
}

/* What the loader asks a driver for a function with: an instance's, a
 * device's as the instance reaches it, or (no instance) one that needs none. */
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL cham_vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    struct instance *inst = (struct instance *)instance;
    const struct entry *e = find(pName);
    if (!e)
        return NULL;
    if (e->level == LEVEL_GLOBAL || e->fn == (PFN_vkVoidFunction)cham_vkGetInstanceProcAddr)
        return android("vkCreateInstance") ? e->fn : NULL;
    if (!inst)
        return NULL;
    if (e->level == LEVEL_DEVICE)
        return g_android(inst->o.real, pName) ? e->fn : NULL;
    return inst->table.fn[e->slot] ? e->fn : NULL;
}

ICD_EXPORT PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    return cham_vkGetInstanceProcAddr(instance, pName);
}

/* A physical device's function the loader doesn't know by name (a newer
 * extension's). */
ICD_EXPORT PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{
    struct instance *inst = (struct instance *)instance;
    const struct entry *e = find(pName);
    return e && e->level == LEVEL_PHYSICAL && inst->table.fn[e->slot] ? e->fn : NULL;
}

/* Version 5 of the loader's driver interface: these three entry points, the
 * loader's word in each object, and Vulkan versions above 1.0 handed through. */
ICD_EXPORT VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion)
{
    if (!android("vkCreateInstance")) {
        say("Android's Vulkan isn't usable here; leaving Vulkan to the other drivers");
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    if (*pSupportedVersion > 5)
        *pSupportedVersion = 5;
    return VK_SUCCESS;
}

/* ---- physical devices ---- */

/* The wrapper of one of the driver's physical devices, made at first sight
 * and kept with the instance. Under inst->lock; NULL without memory. */
static struct physical *physical_for(struct instance *inst, VkPhysicalDevice real)
{
    for (uint32_t i = 0; i < inst->physical_count; i++)
        if (inst->physical[i]->o.real == real)
            return inst->physical[i];
    struct physical **list = realloc(inst->physical, (inst->physical_count + 1) * sizeof *list);
    if (!list)
        return NULL;
    inst->physical = list;
    struct physical *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->o.loader = ICD_LOADER_MAGIC;
    p->o.real = real;
    p->o.t = &inst->table;
    p->instance = inst;
    list[inst->physical_count++] = p;
    return p;
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkEnumeratePhysicalDevices(VkInstance instance,
                                                                     uint32_t *pPhysicalDeviceCount,
                                                                     VkPhysicalDevice *pPhysicalDevices)
{
    struct instance *inst = (struct instance *)instance;
    VkResult result =
        INST_FN(&inst->o, vkEnumeratePhysicalDevices)(inst->o.real, pPhysicalDeviceCount, pPhysicalDevices);
    if (!pPhysicalDevices || (result != VK_SUCCESS && result != VK_INCOMPLETE))
        return result;
    pthread_mutex_lock(&inst->lock);
    for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++) {
        struct physical *p = physical_for(inst, pPhysicalDevices[i]);
        if (!p) {
            memset(pPhysicalDevices, 0, *pPhysicalDeviceCount * sizeof *pPhysicalDevices);
            *pPhysicalDeviceCount = 0;
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            break;
        }
        pPhysicalDevices[i] = (VkPhysicalDevice)p;
    }
    pthread_mutex_unlock(&inst->lock);
    return result;
}

static VkResult physical_groups(struct instance *inst, PFN_vkEnumeratePhysicalDeviceGroups real, uint32_t *count,
                                VkPhysicalDeviceGroupProperties *groups)
{
    VkResult result = real(inst->o.real, count, groups);
    if (!groups || (result != VK_SUCCESS && result != VK_INCOMPLETE))
        return result;
    pthread_mutex_lock(&inst->lock);
    for (uint32_t i = 0; i < *count; i++) {
        for (uint32_t j = 0; j < groups[i].physicalDeviceCount; j++) {
            struct physical *p = physical_for(inst, groups[i].physicalDevices[j]);
            if (!p) {
                for (uint32_t k = 0; k < *count; k++)
                    groups[k].physicalDeviceCount = 0;
                *count = 0;
                pthread_mutex_unlock(&inst->lock);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            groups[i].physicalDevices[j] = (VkPhysicalDevice)p;
        }
    }
    pthread_mutex_unlock(&inst->lock);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkEnumeratePhysicalDeviceGroups(
    VkInstance instance, uint32_t *pPhysicalDeviceGroupCount, VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties)
{
    struct instance *inst = (struct instance *)instance;
    return physical_groups(inst, INST_FN(&inst->o, vkEnumeratePhysicalDeviceGroups), pPhysicalDeviceGroupCount,
                           pPhysicalDeviceGroupProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkEnumeratePhysicalDeviceGroupsKHR(
    VkInstance instance, uint32_t *pPhysicalDeviceGroupCount, VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties)
{
    struct instance *inst = (struct instance *)instance;
    return physical_groups(inst, INST_FN(&inst->o, vkEnumeratePhysicalDeviceGroupsKHR), pPhysicalDeviceGroupCount,
                           pPhysicalDeviceGroupProperties);
}

static VKAPI_ATTR void VKAPI_CALL cham_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                                    VkPhysicalDeviceProperties *pProperties)
{
    struct inst_obj *p = (struct inst_obj *)physicalDevice;
    INST_FN(p, vkGetPhysicalDeviceProperties)(p->real, pProperties);
    pProperties->apiVersion = known_version(pProperties->apiVersion);
}

/* The card's nodes are answered here: Android's driver never sees the
 * question, which is taken out of the chain for its call. */
static void properties2(struct inst_obj *p, PFN_vkGetPhysicalDeviceProperties2 real,
                        VkPhysicalDeviceProperties2 *properties)
{
    VkBaseOutStructure *before = (VkBaseOutStructure *)properties;
    VkPhysicalDeviceDrmPropertiesEXT *drm = NULL;
    for (; before->pNext; before = before->pNext) {
        if (before->pNext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT) {
            drm = (VkPhysicalDeviceDrmPropertiesEXT *)before->pNext;
            before->pNext = before->pNext->pNext;
            break;
        }
    }
    real(p->real, properties);
    properties->properties.apiVersion = known_version(properties->properties.apiVersion);
    if (drm) {
        before->pNext = (VkBaseOutStructure *)drm;
        drm->hasPrimary = card_node(CHAM_DRM_PATH, &drm->primaryMajor, &drm->primaryMinor);
        drm->hasRender = card_node(CHAM_RENDER_PATH, &drm->renderMajor, &drm->renderMinor);
    }
}

static VKAPI_ATTR void VKAPI_CALL cham_vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                                     VkPhysicalDeviceProperties2 *pProperties)
{
    struct inst_obj *p = (struct inst_obj *)physicalDevice;
    properties2(p, INST_FN(p, vkGetPhysicalDeviceProperties2), pProperties);
}

static VKAPI_ATTR void VKAPI_CALL cham_vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice,
                                                                        VkPhysicalDeviceProperties2 *pProperties)
{
    struct inst_obj *p = (struct inst_obj *)physicalDevice;
    properties2(p, INST_FN(p, vkGetPhysicalDeviceProperties2KHR), pProperties);
}

/* ---- devices ---- */

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                         const VkDeviceCreateInfo *pCreateInfo,
                                                         const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
    struct physical *p = (struct physical *)physicalDevice;
    VkDeviceCreateInfo info = *pCreateInfo;
    info.pNext = without_loader_info(info.pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);

    /* A device group names physical devices: the driver gets a copy of
     * that structure with its own, in the chain's same place. Only the
     * structure before it can point to the copy; when that one is the
     * program's, its pointer is put back after the call. */
    VkBaseOutStructure *before = (VkBaseOutStructure *)&info, *group_before = NULL;
    VkDeviceGroupDeviceCreateInfo group;
    VkPhysicalDevice group_devices[VK_MAX_DEVICE_GROUP_SIZE];
    for (; before->pNext; before = before->pNext) {
        if (before->pNext->sType != VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO)
            continue;
        group = *(const VkDeviceGroupDeviceCreateInfo *)before->pNext;
        if (group.physicalDeviceCount > VK_MAX_DEVICE_GROUP_SIZE)
            return VK_ERROR_INITIALIZATION_FAILED;
        for (uint32_t i = 0; i < group.physicalDeviceCount; i++)
            group_devices[i] = ((struct inst_obj *)group.pPhysicalDevices[i])->real;
        group.pPhysicalDevices = group_devices;
        group_before = before;
        break;
    }

    struct device *dev = calloc(1, sizeof *dev);
    /* The card's extension is this driver's, not Android's. */
    const char **names = malloc((info.enabledExtensionCount + 1) * sizeof *names);
    if (!dev || !names) {
        free(dev);
        free(names);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < info.enabledExtensionCount; i++)
        if (strcmp(info.ppEnabledExtensionNames[i], VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) != 0)
            names[kept++] = info.ppEnabledExtensionNames[i];
    info.enabledExtensionCount = kept;
    info.ppEnabledExtensionNames = names;

    VkBaseOutStructure *theirs = NULL;
    if (group_before) {
        theirs = group_before->pNext;
        group_before->pNext = (VkBaseOutStructure *)&group;
    }
    VkDevice created;
    VkResult result = INST_FN(&p->o, vkCreateDevice)(p->o.real, &info, pAllocator, &created);
    if (group_before)
        group_before->pNext = theirs;
    free(names);
    if (result != VK_SUCCESS) {
        free(dev);
        return result;
    }
    dev->o.loader = ICD_LOADER_MAGIC;
    dev->o.real = created;
    dev->o.t = &dev->table;
    for (size_t i = 0; i < COUNT(entries); i++)
        if (entries[i].level == LEVEL_DEVICE)
            dev->table.fn[entries[i].slot] = p->instance->device_proc(created, entries[i].name);
    pthread_mutex_init(&dev->lock, NULL);
    *pDevice = (VkDevice)dev;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL cham_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    struct device *dev = (struct device *)device;
    if (!dev)
        return;
    DEV_FN(&dev->o, vkDestroyDevice)(dev->o.real, pAllocator);
    while (dev->queues) {
        struct queue *q = dev->queues;
        dev->queues = q->next;
        free(q);
    }
    while (dev->cmds) {
        struct cmd *c = dev->cmds;
        dev->cmds = c->next;
        free(c);
    }
    pthread_mutex_destroy(&dev->lock);
    free(dev);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL cham_vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    struct device *dev = (struct device *)device;
    const struct entry *e = find(pName);
    return e && e->level == LEVEL_DEVICE && dev->table.fn[e->slot] ? e->fn : NULL;
}

/* ---- queues ---- */

/* A queue's one wrapper, made when it is first asked for. */
static VkQueue queue_for(struct device *dev, VkQueue real)
{
    if (!real)
        return VK_NULL_HANDLE;
    pthread_mutex_lock(&dev->lock);
    struct queue *q = dev->queues;
    while (q && q->o.real != real)
        q = q->next;
    if (!q && (q = calloc(1, sizeof *q))) {
        q->o.loader = ICD_LOADER_MAGIC;
        q->o.real = real;
        q->o.t = &dev->table;
        q->next = dev->queues;
        dev->queues = q;
    }
    pthread_mutex_unlock(&dev->lock);
    return (VkQueue)q;
}

static VKAPI_ATTR void VKAPI_CALL cham_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                                       uint32_t queueIndex, VkQueue *pQueue)
{
    struct device *dev = (struct device *)device;
    VkQueue real = VK_NULL_HANDLE;
    DEV_FN(&dev->o, vkGetDeviceQueue)(dev->o.real, queueFamilyIndex, queueIndex, &real);
    *pQueue = queue_for(dev, real);
}

static VKAPI_ATTR void VKAPI_CALL cham_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo,
                                                        VkQueue *pQueue)
{
    struct device *dev = (struct device *)device;
    VkQueue real = VK_NULL_HANDLE;
    DEV_FN(&dev->o, vkGetDeviceQueue2)(dev->o.real, pQueueInfo, &real);
    *pQueue = queue_for(dev, real);
}

static VkCommandBuffer real_cmd(VkCommandBuffer cmd)
{
    return cmd ? ((struct dev_obj *)cmd)->real : VK_NULL_HANDLE;
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkQueueSubmit(VkQueue queue, uint32_t submitCount,
                                                        const VkSubmitInfo *pSubmits, VkFence fence)
{
    struct dev_obj *q = (struct dev_obj *)queue;
    VkSubmitInfo few_submits[4], *submits = few_submits;
    VkCommandBuffer few_cmds[16], *cmds = few_cmds;
    void *block = NULL;
    size_t total = 0;
    for (uint32_t i = 0; i < submitCount; i++)
        total += pSubmits[i].commandBufferCount;
    if (submitCount > COUNT(few_submits) || total > COUNT(few_cmds)) {
        block = malloc(submitCount * sizeof *submits + total * sizeof *cmds);
        if (!block)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        submits = block;
        cmds = (VkCommandBuffer *)(submits + submitCount);
    }
    for (uint32_t i = 0; i < submitCount; i++) {
        submits[i] = pSubmits[i];
        submits[i].pCommandBuffers = cmds;
        for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++)
            *cmds++ = real_cmd(pSubmits[i].pCommandBuffers[j]);
    }
    VkResult result = DEV_FN(q, vkQueueSubmit)(q->real, submitCount, submits, fence);
    free(block);
    return result;
}

static VkResult submit2(struct dev_obj *q, PFN_vkQueueSubmit2 real, uint32_t count, const VkSubmitInfo2 *theirs,
                        VkFence fence)
{
    VkSubmitInfo2 few_submits[4], *submits = few_submits;
    VkCommandBufferSubmitInfo few_cmds[16], *cmds = few_cmds;
    void *block = NULL;
    size_t total = 0;
    for (uint32_t i = 0; i < count; i++)
        total += theirs[i].commandBufferInfoCount;
    if (count > COUNT(few_submits) || total > COUNT(few_cmds)) {
        block = malloc(count * sizeof *submits + total * sizeof *cmds);
        if (!block)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        submits = block;
        cmds = (VkCommandBufferSubmitInfo *)(submits + count);
    }
    for (uint32_t i = 0; i < count; i++) {
        submits[i] = theirs[i];
        submits[i].pCommandBufferInfos = cmds;
        for (uint32_t j = 0; j < theirs[i].commandBufferInfoCount; j++) {
            *cmds = theirs[i].pCommandBufferInfos[j];
            cmds->commandBuffer = real_cmd(cmds->commandBuffer);
            cmds++;
        }
    }
    VkResult result = real(q->real, count, submits, fence);
    free(block);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkQueueSubmit2(VkQueue queue, uint32_t submitCount,
                                                         const VkSubmitInfo2 *pSubmits, VkFence fence)
{
    struct dev_obj *q = (struct dev_obj *)queue;
    return submit2(q, DEV_FN(q, vkQueueSubmit2), submitCount, pSubmits, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount,
                                                            const VkSubmitInfo2 *pSubmits, VkFence fence)
{
    struct dev_obj *q = (struct dev_obj *)queue;
    return submit2(q, DEV_FN(q, vkQueueSubmit2KHR), submitCount, pSubmits, fence);
}

/* ---- command buffers ---- */

static void cmd_drop(struct device *dev, struct cmd *c)
{
    if (c->prev)
        c->prev->next = c->next;
    else
        dev->cmds = c->next;
    if (c->next)
        c->next->prev = c->prev;
    free(c);
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkAllocateCommandBuffers(VkDevice device,
                                                                   const VkCommandBufferAllocateInfo *pAllocateInfo,
                                                                   VkCommandBuffer *pCommandBuffers)
{
    struct device *dev = (struct device *)device;
    uint32_t n = pAllocateInfo->commandBufferCount;
    struct cmd *made = NULL; /* n wrappers, before anything can fail halfway */
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < n && result == VK_SUCCESS; i++) {
        struct cmd *c = calloc(1, sizeof *c);
        if (c) {
            c->next = made;
            made = c;
        } else {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    if (result == VK_SUCCESS)
        result = DEV_FN(&dev->o, vkAllocateCommandBuffers)(dev->o.real, pAllocateInfo, pCommandBuffers);
    if (result != VK_SUCCESS) {
        while (made) {
            struct cmd *c = made;
            made = c->next;
            free(c);
        }
        memset(pCommandBuffers, 0, n * sizeof *pCommandBuffers);
        return result;
    }
    pthread_mutex_lock(&dev->lock);
    for (uint32_t i = 0; i < n; i++) {
        struct cmd *c = made;
        made = c->next;
        c->o.loader = ICD_LOADER_MAGIC;
        c->o.real = pCommandBuffers[i];
        c->o.t = &dev->table;
        c->pool = pAllocateInfo->commandPool;
        c->prev = NULL;
        c->next = dev->cmds;
        if (dev->cmds)
            dev->cmds->prev = c;
        dev->cmds = c;
        pCommandBuffers[i] = (VkCommandBuffer)c;
    }
    pthread_mutex_unlock(&dev->lock);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL cham_vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                                           uint32_t commandBufferCount,
                                                           const VkCommandBuffer *pCommandBuffers)
{
    struct device *dev = (struct device *)device;
    VkCommandBuffer real[32];
    for (uint32_t done = 0; done < commandBufferCount;) {
        uint32_t n = commandBufferCount - done < COUNT(real) ? commandBufferCount - done : COUNT(real);
        for (uint32_t i = 0; i < n; i++)
            real[i] = real_cmd(pCommandBuffers[done + i]);
        DEV_FN(&dev->o, vkFreeCommandBuffers)(dev->o.real, commandPool, n, real);
        pthread_mutex_lock(&dev->lock);
        for (uint32_t i = 0; i < n; i++)
            if (pCommandBuffers[done + i])
                cmd_drop(dev, (struct cmd *)pCommandBuffers[done + i]);
        pthread_mutex_unlock(&dev->lock);
        done += n;
    }
}

/* A pool's command buffers are freed with it. */
static VKAPI_ATTR void VKAPI_CALL cham_vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool,
                                                           const VkAllocationCallbacks *pAllocator)
{
    struct device *dev = (struct device *)device;
    DEV_FN(&dev->o, vkDestroyCommandPool)(dev->o.real, commandPool, pAllocator);
    pthread_mutex_lock(&dev->lock);
    for (struct cmd *c = dev->cmds, *next; c; c = next) {
        next = c->next;
        if (c->pool == commandPool)
            cmd_drop(dev, c);
    }
    pthread_mutex_unlock(&dev->lock);
}

/* Executing a list in pieces is executing it. */
static VKAPI_ATTR void VKAPI_CALL cham_vkCmdExecuteCommands(VkCommandBuffer commandBuffer,
                                                           uint32_t commandBufferCount,
                                                           const VkCommandBuffer *pCommandBuffers)
{
    struct dev_obj *c = (struct dev_obj *)commandBuffer;
    VkCommandBuffer real[32];
    for (uint32_t done = 0; done < commandBufferCount;) {
        uint32_t n = commandBufferCount - done < COUNT(real) ? commandBufferCount - done : COUNT(real);
        for (uint32_t i = 0; i < n; i++)
            real[i] = real_cmd(pCommandBuffers[done + i]);
        DEV_FN(c, vkCmdExecuteCommands)(c->real, n, real);
        done += n;
    }
}

/* ---- objects named by type and number ---- */

static uint64_t real_object(VkObjectType type, uint64_t handle)
{
    if (!handle)
        return 0;
    switch (type) {
    case VK_OBJECT_TYPE_INSTANCE:
    case VK_OBJECT_TYPE_PHYSICAL_DEVICE:
        return (uintptr_t)((struct inst_obj *)(uintptr_t)handle)->real;
    case VK_OBJECT_TYPE_DEVICE:
    case VK_OBJECT_TYPE_QUEUE:
    case VK_OBJECT_TYPE_COMMAND_BUFFER:
        return (uintptr_t)((struct dev_obj *)(uintptr_t)handle)->real;
    default:
        return handle;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkSetPrivateData(VkDevice device, VkObjectType objectType,
                                                           uint64_t objectHandle, VkPrivateDataSlot privateDataSlot,
                                                           uint64_t data)
{
    struct dev_obj *d = (struct dev_obj *)device;
    return DEV_FN(d, vkSetPrivateData)(d->real, objectType, real_object(objectType, objectHandle), privateDataSlot,
                                       data);
}

static VKAPI_ATTR void VKAPI_CALL cham_vkGetPrivateData(VkDevice device, VkObjectType objectType,
                                                       uint64_t objectHandle, VkPrivateDataSlot privateDataSlot,
                                                       uint64_t *pData)
{
    struct dev_obj *d = (struct dev_obj *)device;
    DEV_FN(d, vkGetPrivateData)(d->real, objectType, real_object(objectType, objectHandle), privateDataSlot, pData);
}

static VKAPI_ATTR VkResult VKAPI_CALL cham_vkSetPrivateDataEXT(VkDevice device, VkObjectType objectType,
                                                              uint64_t objectHandle,
                                                              VkPrivateDataSlot privateDataSlot, uint64_t data)
{
    struct dev_obj *d = (struct dev_obj *)device;
    return DEV_FN(d, vkSetPrivateDataEXT)(d->real, objectType, real_object(objectType, objectHandle),
                                          privateDataSlot, data);
}

static VKAPI_ATTR void VKAPI_CALL cham_vkGetPrivateDataEXT(VkDevice device, VkObjectType objectType,
                                                          uint64_t objectHandle, VkPrivateDataSlot privateDataSlot,
                                                          uint64_t *pData)
{
    struct dev_obj *d = (struct dev_obj *)device;
    DEV_FN(d, vkGetPrivateDataEXT)(d->real, objectType, real_object(objectType, objectHandle), privateDataSlot,
                                   pData);
}
