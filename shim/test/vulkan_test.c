/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The card's Vulkan driver (shim/vulkan), driven by the system's Vulkan
 * loader like any driver, on a real driver standing in for Android's
 * (fake_android_vulkan.c on Mesa's lavapipe): what it lists, that every kind
 * of wrapped object reaches the driver as the driver's own - in arguments,
 * arrays, structures and chains - and that what is drawn that way is right.
 *
 * VULKAN_TEST_ICD: the driver's library. VULKAN_TEST_CARD=1: the session's
 * card is there (libchameleon.so preloaded, chameleon-kernel running).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <vulkan/vulkan.h>

#include "check.h"

#define SIZE 64
#define MANY 40 /* more than the driver translates in one piece */

static int has(const VkExtensionProperties *list, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(list[i].extensionName, name) == 0)
            return 1;
    return 0;
}

static VkDevice dev;
static VkQueue queue;
static VkRenderPass pass;
static VkFramebuffer fb;
static VkImage image;
static VkBuffer buffer;
static const uint8_t *pixels;

static const uint8_t *pixel(int x, int y)
{
    return pixels + (y * SIZE + x) * 4;
}

static int is(const uint8_t *p, int r, int g, int b)
{
    return p[0] == r && p[1] == g && p[2] == b && p[3] == 255;
}

/* A secondary command buffer that paints a rectangle inside the pass. */
static void paint(VkCommandBuffer cmd, int x, int y, int w, int h, float r, float g, float b)
{
    VkCommandBufferInheritanceInfo inherit = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
                                              .renderPass = pass, .framebuffer = fb};
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
                                      .pInheritanceInfo = &inherit};
    vkBeginCommandBuffer(cmd, &begin);
    VkClearAttachment what = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .clearValue.color.float32 = {r, g, b, 1}};
    VkClearRect where = {.rect = {{x, y}, {w, h}}, .layerCount = 1};
    vkCmdClearAttachments(cmd, 1, &what, 1, &where);
    vkEndCommandBuffer(cmd);
}

/* The frame: cleared, the secondaries' rectangles on it, copied out. */
static void record(VkCommandBuffer cmd, float r, float g, float b, uint32_t count, const VkCommandBuffer *secondaries)
{
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &begin);
    VkClearValue clear = {.color.float32 = {r, g, b, 1}};
    VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
                                .framebuffer = fb, .renderArea = {{0, 0}, {SIZE, SIZE}},
                                .clearValueCount = 1, .pClearValues = &clear};
    vkCmdBeginRenderPass(cmd, &rp, count ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);
    if (count)
        vkCmdExecuteCommands(cmd, count, secondaries);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {SIZE, SIZE, 1}};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    vkEndCommandBuffer(cmd);
}

static uint32_t memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
        if ((bits & 1u << i) && (mem.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

int main(void)
{
    int card = getenv("VULKAN_TEST_CARD") != NULL;
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("the driver's names:\n");
    void *icd = dlopen(getenv("VULKAN_TEST_ICD"), RTLD_NOW | RTLD_LOCAL);
    PFN_vkGetInstanceProcAddr icd_proc = icd ? dlsym(icd, "vk_icdGetInstanceProcAddr") : NULL;
    CHECK(icd_proc != NULL, "the driver has the loader's entry point");
    if (!icd_proc)
        return 1;
    CHECK(icd_proc(NULL, "vkCreateInstance") != NULL, "it creates instances");
    CHECK(icd_proc(NULL, "vkCreateSwapchainKHR") == NULL && icd_proc(NULL, "vkCreateDebugUtilsMessengerEXT") == NULL &&
              icd_proc(NULL, "vkNoSuchFunction") == NULL,
          "no functions of what it leaves out");
    CHECK(icd_proc(NULL, "vkCmdDraw") == NULL, "no device's function without an instance");

    /* As the loader calls it, without the loader: it copies a device group
     * for the driver and puts its own copy back before the program sees. */
    printf("a device group, handed over directly:\n");
    VkInstanceCreateInfo direct_ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance direct_inst = VK_NULL_HANDLE;
    VkPhysicalDevice direct_phys = VK_NULL_HANDLE;
    VkDevice direct_dev = VK_NULL_HANDLE;
    uint32_t direct_n = 1;
    VkResult direct = ((PFN_vkCreateInstance)icd_proc(NULL, "vkCreateInstance"))(&direct_ici, NULL, &direct_inst);
    if (direct == VK_SUCCESS)
        direct = ((PFN_vkEnumeratePhysicalDevices)icd_proc(direct_inst, "vkEnumeratePhysicalDevices"))(
            direct_inst, &direct_n, &direct_phys);
    CHECK(direct == VK_SUCCESS && direct_phys && *(uintptr_t *)direct_phys == 0x01CDC0DE,
          "an instance and its physical device, each with the loader's word to write over");
    if (direct != VK_SUCCESS)
        return 1;
    VkDeviceGroupDeviceCreateInfo direct_group = {.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,
                                                  .physicalDeviceCount = 1, .pPhysicalDevices = &direct_phys};
    VkPhysicalDeviceFeatures2 direct_features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                                 .pNext = &direct_group};
    float direct_priority = 1;
    VkDeviceQueueCreateInfo direct_qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                          .pQueuePriorities = &direct_priority};
    VkDeviceCreateInfo direct_dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &direct_features,
                                     .queueCreateInfoCount = 1, .pQueueCreateInfos = &direct_qci};
    direct = ((PFN_vkCreateDevice)icd_proc(direct_inst, "vkCreateDevice"))(direct_phys, &direct_dci, NULL,
                                                                          &direct_dev);
    CHECK(direct == VK_SUCCESS, "the driver was given its own physical device in the group (%d)", direct);
    CHECK(direct_features.pNext == &direct_group && direct_group.pPhysicalDevices == &direct_phys,
          "the program's chain is as it was");
    if (direct == VK_SUCCESS)
        ((PFN_vkDestroyDevice)icd_proc(direct_inst, "vkDestroyDevice"))(direct_dev, NULL);
    ((PFN_vkDestroyInstance)icd_proc(direct_inst, "vkDestroyInstance"))(direct_inst, NULL);

    printf("instances:\n");
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *exts = calloc(n + 1, sizeof *exts);
    vkEnumerateInstanceExtensionProperties(NULL, &n, exts);
    CHECK(has(exts, n, "VK_KHR_get_physical_device_properties2"), "the driver's instance extensions are listed");
    CHECK(!has(exts, n, "VK_KHR_surface") && !has(exts, n, "VK_KHR_wayland_surface") &&
              !has(exts, n, "VK_KHR_android_surface"),
          "no window extensions, Android's or the driver's");
    free(exts);

    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    CHECK(r == VK_SUCCESS, "an instance (%d)", r);
    if (r != VK_SUCCESS)
        return 1;
    if (getenv("VK_INSTANCE_LAYERS")) { /* the run under the validation layer */
        VkLayerProperties layers[32];
        uint32_t count = 32, found = 0;
        vkEnumerateInstanceLayerProperties(&count, layers);
        for (uint32_t i = 0; i < count; i++)
            found |= strcmp(layers[i].layerName, getenv("VK_INSTANCE_LAYERS")) == 0;
        CHECK(found, "%s checks every call from here on", getenv("VK_INSTANCE_LAYERS"));
    }

    VkPhysicalDevice phys = VK_NULL_HANDLE, again = VK_NULL_HANDLE;
    n = 1;
    r = vkEnumeratePhysicalDevices(inst, &n, &phys);
    CHECK(r == VK_SUCCESS && n == 1 && phys, "one physical device (%d, %u)", r, n);
    n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &again);
    CHECK(again == phys, "the same one when asked again");
    VkPhysicalDeviceGroupProperties group = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES};
    n = 1;
    r = vkEnumeratePhysicalDeviceGroups(inst, &n, &group);
    CHECK(r == VK_SUCCESS && n == 1 && group.physicalDeviceCount == 1 && group.physicalDevices[0] == phys,
          "its device group names it");

    printf("the physical device:\n");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    CHECK(VK_API_VERSION_MAJOR(props.apiVersion) == 1 && VK_API_VERSION_MINOR(props.apiVersion) == 3,
          "Vulkan %u.%u.%u of the driver's newer one: what the driver's functions here reach",
          VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
          VK_API_VERSION_PATCH(props.apiVersion));
    n = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL);
    exts = calloc(n + 1, sizeof *exts);
    uint32_t one = 1;
    r = vkEnumerateDeviceExtensionProperties(phys, NULL, &one, exts);
    CHECK(r == VK_INCOMPLETE && one == 1, "a list that doesn't fit is cut and says so");
    r = vkEnumerateDeviceExtensionProperties(phys, NULL, &n, exts);
    CHECK(r == VK_SUCCESS && has(exts, n, "VK_KHR_maintenance4") && has(exts, n, "VK_KHR_external_memory_fd"),
          "the driver's device extensions are listed (%u)", n);
    CHECK(!has(exts, n, "VK_KHR_swapchain") && !has(exts, n, "VK_EXT_external_memory_dma_buf") &&
              !has(exts, n, "VK_ANDROID_external_memory_android_hardware_buffer"),
          "not windows, the driver's own dma-bufs or Android's buffers");
    CHECK(!has(exts, n, "VK_KHR_maintenance5"), "nor an extension newer than the functions here");

    /* The card's nodes, as the session shows them to this program too. */
    struct stat primary, render;
    int has_primary = stat("/dev/dri/card0", &primary) == 0 && S_ISCHR(primary.st_mode);
    int has_render = stat("/dev/dri/renderD128", &render) == 0 && S_ISCHR(render.st_mode);
    int drm_listed = has(exts, n, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
    free(exts);
    if (card)
        CHECK(has_render && major(render.st_rdev) == 226 && minor(render.st_rdev) == 128,
              "the session's card is there");
    CHECK(drm_listed == has_render, has_render ? "VK_EXT_physical_device_drm is listed" :
                                                 "no card here: no VK_EXT_physical_device_drm");
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceDrmPropertiesEXT drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
                                            .pNext = &driver, .renderMajor = -1, .primaryMinor = -1};
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = &drm};
    VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
    vkGetPhysicalDeviceProperties2(phys, &props2);
    CHECK(props2.pNext == &id && id.pNext == &drm && drm.pNext == &driver, "the program's chain is as it was");
    CHECK(driver.driverName[0] && props2.properties.apiVersion == props.apiVersion,
          "the driver answered what stands before and after the card's question (%s)", driver.driverName);
    CHECK(drm.hasRender == (VkBool32)has_render && drm.hasPrimary == (VkBool32)has_primary &&
              drm.renderMajor == (has_render ? (int64_t)major(render.st_rdev) : 0) &&
              drm.renderMinor == (has_render ? (int64_t)minor(render.st_rdev) : 0) &&
              drm.primaryMajor == (has_primary ? (int64_t)major(primary.st_rdev) : 0) &&
              drm.primaryMinor == (has_primary ? (int64_t)minor(primary.st_rdev) : 0),
          "the card's nodes: primary %s %lld:%lld, render %s %lld:%lld", drm.hasPrimary ? "yes" : "no",
          (long long)drm.primaryMajor, (long long)drm.primaryMinor, drm.hasRender ? "yes" : "no",
          (long long)drm.renderMajor, (long long)drm.renderMinor);

    printf("a device:\n");
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
    vkGetPhysicalDeviceFeatures2(phys, &features);
    CHECK(f13.synchronization2 && f13.privateData, "the stand-in driver has what the test needs");
    /* The device group after another structure, so the driver's copy of
     * it hangs on a structure of the program's. */
    VkDeviceGroupDeviceCreateInfo in_group = {.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,
                                              .physicalDeviceCount = 1, .pPhysicalDevices = &phys};
    VkPhysicalDeviceVulkan13Features want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                             .pNext = &in_group, .synchronization2 = VK_TRUE,
                                             .privateData = VK_TRUE};
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                   .pQueuePriorities = &priority};
    const char *enable[] = {"VK_KHR_maintenance4", VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &want,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = drm_listed ? 2 : 1, .ppEnabledExtensionNames = enable};
    r = vkCreateDevice(phys, &dci, NULL, &dev);
    CHECK(r == VK_SUCCESS, "a device, in a group, with the card's extension that the driver doesn't know (%d)", r);
    if (r != VK_SUCCESS)
        return 1;
    CHECK(want.pNext == &in_group && in_group.pPhysicalDevices == &phys, "the program's chain is as it was");
    VkQueue queue2 = VK_NULL_HANDLE, queue3 = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, 0, 0, &queue);
    vkGetDeviceQueue(dev, 0, 0, &queue2);
    VkDeviceQueueInfo2 qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    vkGetDeviceQueue2(dev, &qi, &queue3);
    CHECK(queue && queue2 == queue && queue3 == queue, "its queue, the same each time it is asked for");

    /* A picture to draw into and a buffer to read it from. */
    VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                              .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {SIZE, SIZE, 1}, .mipLevels = 1,
                              .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                              .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    vkCreateImage(dev, &imci, NULL, &image);
    VkMemoryRequirements need;
    vkGetImageMemoryRequirements(dev, image, &need);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = need.size,
                                .memoryTypeIndex = memory_type(phys, need.memoryTypeBits, 0)};
    VkDeviceMemory image_memory, buffer_memory;
    vkAllocateMemory(dev, &mai, NULL, &image_memory);
    vkBindImageMemory(dev, image, image_memory, 0);
    VkImageViewCreateInfo ivci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = imci.format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    vkCreateImageView(dev, &ivci, NULL, &view);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SIZE * SIZE * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    vkCreateBuffer(dev, &bci, NULL, &buffer);
    vkGetBufferMemoryRequirements(dev, buffer, &need);
    mai.allocationSize = need.size;
    mai.memoryTypeIndex = memory_type(phys, need.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev, &mai, NULL, &buffer_memory);
    vkBindBufferMemory(dev, buffer, buffer_memory, 0);
    void *mapped = NULL;
    vkMapMemory(dev, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    pixels = mapped;
    VkAttachmentDescription attachment = {.format = imci.format, .samples = VK_SAMPLE_COUNT_1_BIT,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                          .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                    .pColorAttachments = &color};
    VkSubpassDependency after = {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
                                 .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                   .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass,
                                   .dependencyCount = 1, .pDependencies = &after};
    vkCreateRenderPass(dev, &rpci, NULL, &pass);
    VkFramebufferCreateInfo fbci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
                                    .attachmentCount = 1, .pAttachments = &view, .width = SIZE, .height = SIZE,
                                    .layers = 1};
    vkCreateFramebuffer(dev, &fbci, NULL, &fb);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);

    printf("command buffers:\n");
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &cpci, NULL, &pool);
    VkCommandBuffer frame, idle[17], rects[MANY];
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    r = vkAllocateCommandBuffers(dev, &cbai, &frame);
    cbai.commandBufferCount = 17;
    r |= vkAllocateCommandBuffers(dev, &cbai, idle);
    cbai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    cbai.commandBufferCount = MANY;
    r |= vkAllocateCommandBuffers(dev, &cbai, rects);
    CHECK(r == VK_SUCCESS && frame && idle[16] && rects[MANY - 1] && rects[0] != rects[1], "%d command buffers",
          1 + 17 + MANY);
    VkCommandBufferBeginInfo plain = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    for (int i = 0; i < 17; i++) {
        vkBeginCommandBuffer(idle[i], &plain);
        vkEndCommandBuffer(idle[i]);
    }
    /* One pixel each in the top row, and the first one a square as well. */
    paint(rects[0], 16, 16, 32, 32, 0, 1, 0);
    for (int i = 1; i < MANY; i++)
        paint(rects[i], i, 0, 1, 1, 1, 1, 1);

    record(frame, 1, 0, 0, MANY, rects);
    VkSubmitInfo single = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &frame};
    r = vkQueueSubmit(queue, 1, &single, fence);
    r |= vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    CHECK(r == VK_SUCCESS && is(pixel(0, 0), 255, 0, 0) && is(pixel(63, 63), 255, 0, 0),
          "a submitted command buffer cleared the picture");
    CHECK(is(pixel(32, 32), 0, 255, 0) && is(pixel(MANY - 1, 0), 255, 255, 255) && is(pixel(MANY, 0), 255, 0, 0),
          "and ran %d secondary command buffers in it, all of them", MANY);

    /* Six submissions of three command buffers, the frame the very last. */
    VkCommandBuffer batch[18];
    for (int i = 0; i < 17; i++)
        batch[i] = idle[i];
    batch[17] = frame;
    VkSubmitInfo submits[6];
    for (int i = 0; i < 6; i++)
        submits[i] = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 3,
                                    .pCommandBuffers = &batch[i * 3]};
    record(frame, 0, 0, 1, 1, rects);
    vkResetFences(dev, 1, &fence);
    r = vkQueueSubmit(queue, 6, submits, fence);
    r |= vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    CHECK(r == VK_SUCCESS && is(pixel(0, 0), 0, 0, 255) && is(pixel(32, 32), 0, 255, 0) && is(pixel(5, 0), 0, 0, 255),
          "many submissions in one call");

    VkCommandBufferSubmitInfo infos[18];
    for (int i = 0; i < 18; i++)
        infos[i] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                               .commandBuffer = batch[i]};
    VkSubmitInfo2 submits2[6];
    for (int i = 0; i < 6; i++)
        submits2[i] = (VkSubmitInfo2){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 3,
                                      .pCommandBufferInfos = &infos[i * 3]};
    record(frame, 1, 1, 0, 0, NULL);
    vkResetFences(dev, 1, &fence);
    r = vkQueueSubmit2(queue, 6, submits2, fence);
    r |= vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    CHECK(r == VK_SUCCESS && is(pixel(0, 0), 255, 255, 0) && is(pixel(32, 32), 255, 255, 0),
          "the same through vkQueueSubmit2");
    record(frame, 0, 1, 1, 1, rects);
    vkResetFences(dev, 1, &fence);
    r = vkQueueSubmit2(queue, 1, &submits2[5], fence);
    r |= vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    CHECK(r == VK_SUCCESS && is(pixel(0, 0), 0, 255, 255) && is(pixel(32, 32), 0, 255, 0), "and a single one");

    printf("objects named by type and number:\n");
    VkPrivateDataSlotCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO};
    VkPrivateDataSlot slot;
    r = vkCreatePrivateDataSlot(dev, &sci, NULL, &slot);
    r |= vkSetPrivateData(dev, VK_OBJECT_TYPE_DEVICE, (uint64_t)(uintptr_t)dev, slot, 11);
    r |= vkSetPrivateData(dev, VK_OBJECT_TYPE_QUEUE, (uint64_t)(uintptr_t)queue, slot, 22);
    r |= vkSetPrivateData(dev, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(uintptr_t)frame, slot, 33);
    r |= vkSetPrivateData(dev, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(uintptr_t)rects[3], slot, 44);
    r |= vkSetPrivateData(dev, VK_OBJECT_TYPE_IMAGE, (uint64_t)image, slot, 55);
    uint64_t got[5] = {0};
    vkGetPrivateData(dev, VK_OBJECT_TYPE_DEVICE, (uint64_t)(uintptr_t)dev, slot, &got[0]);
    vkGetPrivateData(dev, VK_OBJECT_TYPE_QUEUE, (uint64_t)(uintptr_t)queue, slot, &got[1]);
    vkGetPrivateData(dev, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(uintptr_t)frame, slot, &got[2]);
    vkGetPrivateData(dev, VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(uintptr_t)rects[3], slot, &got[3]);
    vkGetPrivateData(dev, VK_OBJECT_TYPE_IMAGE, (uint64_t)image, slot, &got[4]);
    CHECK(r == VK_SUCCESS && got[0] == 11 && got[1] == 22 && got[2] == 33 && got[3] == 44 && got[4] == 55,
          "private data of the device, its queue, two command buffers and a picture");
    vkDestroyPrivateDataSlot(dev, slot, NULL);

    printf("the end of command buffers:\n");
    VkCommandBuffer some[5] = {rects[1], VK_NULL_HANDLE, rects[2], idle[0], VK_NULL_HANDLE};
    vkFreeCommandBuffers(dev, pool, 5, some);
    vkFreeCommandBuffers(dev, pool, MANY - 3, &rects[3]);
    record(frame, 1, 0, 1, 1, rects);
    vkResetFences(dev, 1, &fence);
    r = vkQueueSubmit(queue, 1, &single, fence);
    r |= vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    CHECK(r == VK_SUCCESS && is(pixel(0, 0), 255, 0, 255) && is(pixel(32, 32), 0, 255, 0),
          "some freed, a gap in the list among them: the others still draw");
    /* Another pool's go with their pool. */
    VkCommandPool pool2;
    vkCreateCommandPool(dev, &cpci, NULL, &pool2);
    cbai.commandPool = pool2;
    cbai.commandBufferCount = MANY;
    r = vkAllocateCommandBuffers(dev, &cbai, rects);
    vkDestroyCommandPool(dev, pool2, NULL);
    record(frame, 1, 1, 1, 0, NULL);
    vkResetFences(dev, 1, &fence);
    r |= vkQueueSubmit(queue, 1, &single, fence);
    r |= vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    CHECK(r == VK_SUCCESS && is(pixel(32, 32), 255, 255, 255), "a pool destroyed with its command buffers");

    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyFramebuffer(dev, fb, NULL);
    vkDestroyRenderPass(dev, pass, NULL);
    vkDestroyBuffer(dev, buffer, NULL);
    vkFreeMemory(dev, buffer_memory, NULL);
    vkDestroyImageView(dev, view, NULL);
    vkDestroyImage(dev, image, NULL);
    vkFreeMemory(dev, image_memory, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("vulkan_test: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
