#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates vk_forward.h for vulkan.c: for every Vulkan function of the
vulkan_core.h (Khronos's, in shim/include) a forwarder that swaps the caller's instance, physical
device, device, queue or command buffer for Android's own and calls Android's
driver, the table vk_icdGetInstanceProcAddr() looks names up in, and the list
of extensions the header knows. What the files of shim/vulkan share comes
first (the tables' slots, the prototypes of the functions written by hand);
the forwarders and the table are vulkan.c's, which defines
CHAM_VK_FORWARDERS.

The Vulkan loader writes into the first word of those five kinds of handle,
and so does Android's loader, so a program never gets Android's handles: it
gets vulkan.c's wrappers. A forwarder only unwraps its first argument. A
function that carries such a handle anywhere else (another argument, an
array, a structure, an object type with a 64 bit handle) must be written by
hand, in vulkan.c: this script finds them all and stops if one is in neither
HAND nor an extension left out in LEFT_OUT, so a newer header can't add one
unnoticed.

usage: gen_vk.py shim/include/vulkan/vulkan_core.h > vk_forward.h"""
import re
import sys

DISPATCHABLE = {"VkInstance": "INSTANCE", "VkPhysicalDevice": "PHYSICAL", "VkDevice": "DEVICE",
                "VkQueue": "DEVICE", "VkCommandBuffer": "DEVICE"}
# An object named by its type and a 64 bit number.
OBJECT_TYPES = {"VkObjectType", "VkDebugReportObjectTypeEXT"}

# Written by hand in vulkan.c (cham_<name>): the loader's own entry points
# and every function that carries a wrapped handle outside its first argument.
HAND = {
    "vkCreateInstance", "vkDestroyInstance", "vkEnumerateInstanceVersion",
    "vkEnumerateInstanceExtensionProperties", "vkEnumerateInstanceLayerProperties",
    "vkGetInstanceProcAddr", "vkGetDeviceProcAddr",
    "vkEnumeratePhysicalDevices", "vkEnumeratePhysicalDeviceGroups", "vkEnumeratePhysicalDeviceGroupsKHR",
    "vkEnumerateDeviceExtensionProperties", "vkEnumerateDeviceLayerProperties",
    "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR",
    "vkCreateDevice", "vkDestroyDevice", "vkGetDeviceQueue", "vkGetDeviceQueue2",
    "vkAllocateCommandBuffers", "vkFreeCommandBuffers", "vkDestroyCommandPool", "vkCmdExecuteCommands",
    "vkCmdPreprocessGeneratedCommandsEXT",
    "vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR",
    "vkSetPrivateData", "vkGetPrivateData", "vkSetPrivateDataEXT", "vkGetPrivateDataEXT",
    # Windows (wsi.c): what Android answers for its own kind of window is
    # turned into a Wayland window's answers, and frames go to the compositor.
    "vkDestroySurfaceKHR", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", "vkGetPhysicalDeviceSurfaceFormatsKHR",
    "vkGetPhysicalDeviceSurfacePresentModesKHR", "vkCreateSwapchainKHR", "vkDestroySwapchainKHR",
    "vkQueuePresentKHR",
}
# Functions of this driver alone, from a platform's header (their prototypes
# are in vulkan.h): Android has none by these names.
OWN = {"vkCreateWaylandSurfaceKHR": "INSTANCE", "vkGetPhysicalDeviceWaylandPresentationSupportKHR": "PHYSICAL"}
# Extensions with all their functions here, but listed by vulkan.c only where
# windows can be shown.
WINDOWS = {"VK_KHR_surface", "VK_KHR_swapchain"}
# Structures with a wrapped handle that reach the driver through a pNext
# chain, and the function of HAND that translates each.
CHAINED = {"VkDeviceGroupDeviceCreateInfo": "vkCreateDevice"}
# Structures that name an object by type and number, but only ever objects
# that aren't wrapped (the driver reports them to the program's callback).
PLAIN_OBJECTS = {"VkDeviceMemoryReportCallbackDataEXT"}

# Extensions of the header that aren't offered, with their functions.
LEFT_OUT = {
    # Windows beyond plain Wayland surfaces and swapchains.
    "VK_KHR_display", "VK_KHR_display_swapchain",
    "VK_KHR_get_surface_capabilities2", "VK_KHR_get_display_properties2", "VK_KHR_incremental_present",
    "VK_KHR_shared_presentable_image", "VK_KHR_swapchain_mutable_format", "VK_KHR_surface_protected_capabilities",
    "VK_KHR_present_wait", "VK_KHR_present_id", "VK_EXT_swapchain_colorspace", "VK_EXT_display_surface_counter",
    "VK_EXT_display_control", "VK_EXT_direct_mode_display", "VK_EXT_acquire_drm_display",
    "VK_EXT_headless_surface", "VK_EXT_hdr_metadata", "VK_GOOGLE_display_timing", "VK_GOOGLE_surfaceless_query",
    "VK_EXT_surface_maintenance1", "VK_EXT_swapchain_maintenance1", "VK_EXT_image_compression_control_swapchain",
    "VK_AMD_display_native_hdr", "VK_NV_present_barrier",
    "VK_KHR_surface_maintenance1", "VK_KHR_swapchain_maintenance1", "VK_KHR_present_id2", "VK_KHR_present_wait2",
    "VK_KHR_present_mode_fifo_latest_ready", "VK_EXT_present_mode_fifo_latest_ready", "VK_EXT_present_timing",
    "VK_EXT_multisampled_render_to_swapchain", "VK_NV_display_stereo", "VK_NV_present_metering",
    # Messages about objects: the driver's handles in them would need
    # translating both ways. The loader and the validation layer provide
    # these for programs without the driver.
    "VK_EXT_debug_report", "VK_EXT_debug_marker", "VK_EXT_debug_utils",
    # dma-bufs: only the card's own buffers can be shown or shared in the
    # session, not the dma-bufs Android's driver makes by itself.
    "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier",
    # Answered by vulkan.c for the card's nodes, never by Android's driver.
    "VK_EXT_physical_device_drm",
    # A queue of the driver's handed to another API: nothing here uses one.
    "VK_NV_external_compute_queue",
}

text = open(sys.argv[1]).read()
text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
text = re.sub(r"//[^\n]*", "", text)

# Sections: the core versions and the extensions, in the header's order.
guard = re.compile(r"^#define (VK_VERSION_\d+_\d+|VK_[A-Z0-9]+_[a-z0-9_]+) 1$", re.M)
marks = [(m.start(), m.group(1)) for m in guard.finditer(text)]


def section(pos):
    name = None
    for start, n in marks:
        if start > pos:
            break
        name = n
    return name


versions = sorted((tuple(map(int, n.split("_")[2:])) for _, n in marks if n.startswith("VK_VERSION_")))
extensions = sorted(n for _, n in marks if not n.startswith("VK_VERSION_"))
unknown = LEFT_OUT - set(extensions)
if unknown:
    sys.exit(f"gen_vk.py: LEFT_OUT names extensions the header doesn't have: {sorted(unknown)}")


windows = [e for e in extensions if e not in LEFT_OUT and e not in WINDOWS and
           {"surface", "swapchain", "display", "present"} & set(e.split("_"))]
if windows:
    sys.exit(f"gen_vk.py: these look like window extensions and aren't in LEFT_OUT: {windows}")


def offered(pos):
    return section(pos) not in LEFT_OUT


# Structures and unions: which contain a wrapped handle or an object type,
# directly or through another structure.
structs = {}
for m in re.finditer(r"typedef (?:struct|union) (\w+) \{(.*?)\} \1;", text, re.S):
    members = [re.findall(r"\w+", line) for line in m.group(2).split(";") if line.strip()]
    structs[m.group(1)] = (m.start(), [[w for w in words if w not in ("const", "struct")][0] for words in members])


def carries(type_name, seen=()):
    """Why a value of this type names a wrapped object, or None."""
    if type_name in DISPATCHABLE:
        return type_name
    if type_name in OBJECT_TYPES:
        return type_name
    if type_name in structs and type_name not in seen and type_name not in PLAIN_OBJECTS:
        for member in structs[type_name][1]:
            why = carries(member, seen + (type_name,))
            if why:
                return f"{type_name} ({why})"
    return None


proto = re.compile(r"VKAPI_ATTR\s+(\w[\w\s\*]*?)\s+VKAPI_CALL\s+(vk\w+)\(\s*(.*?)\);", re.S)
funcs = []
problems = []
for m in proto.finditer(text):
    ret, name, params = m.group(1), m.group(2), m.group(3)
    if not offered(m.start()):
        continue
    params = [" ".join(p.split()) for p in params.split(",")]
    types = [[w for w in re.findall(r"\w+", p) if w not in ("const", "struct")][0] for p in params]
    names = [re.findall(r"\w+", p.split("[")[0])[-1] for p in params]
    level = DISPATCHABLE.get(types[0], "GLOBAL")
    if name not in HAND:
        if level == "GLOBAL":
            problems.append(f"{name}: no instance or device to send it to")
        for t in types[1:]:
            why = carries(t)
            if why:
                problems.append(f"{name}: carries {why}")
    funcs.append((name, ret, params, names, types[0], level))

# A structure with a wrapped handle that no function's arguments reach is
# handed over in a pNext chain (it starts with sType).
reached = set()


def reach(type_name):
    if type_name in structs and type_name not in reached:
        reached.add(type_name)
        for member in structs[type_name][1]:
            reach(member)


for m in proto.finditer(text):
    for p in m.group(3).split(","):
        for w in re.findall(r"\w+", p):
            reach(w)
for name, (pos, members) in structs.items():
    if name in reached or not offered(pos) or name in PLAIN_OBJECTS:
        continue
    why = carries(name)
    if why and name not in CHAINED:
        problems.append(f"{name}: a pNext structure that carries {why}")
for name, by in CHAINED.items():
    if name not in structs or by not in HAND:
        problems.append(f"CHAINED: {name} / {by} isn't in the header or in HAND")
missing = HAND - {f[0] for f in funcs}
if missing:
    problems.append(f"HAND names functions the header doesn't offer: {sorted(missing)}")
if problems:
    sys.exit("gen_vk.py: these need code in vulkan.c (HAND) or belong in LEFT_OUT:\n  " + "\n  ".join(problems))

out = ['/* SPDX-License-Identifier: GPL-2.0-or-later */',
       "/* Generated by gen_vk.py from shim/include/vulkan/vulkan_core.h - do not edit. */",
       '',
       '/* The newest Vulkan version whose functions are all here. */',
       f'#define CHAM_VK_VERSION VK_MAKE_API_VERSION(0, {versions[-1][0]}, {versions[-1][1]}, 0)',
       '',
       "/* Android's functions, by the wrapper's kind: an instance's and its",
       " * physical devices', and a device's with its queues and command buffers. */"]
for kind, levels in (("I", ("INSTANCE", "PHYSICAL")), ("D", ("DEVICE",))):
    out.append("enum {")
    out += [f"    {kind}_{f[0]}," for f in funcs if f[5] in levels]
    out += [f"    {kind}_COUNT", "};"]
out += ['struct instance_table {', '    PFN_vkVoidFunction fn[I_COUNT];', '};',
        'struct device_table {', '    PFN_vkVoidFunction fn[D_COUNT];', '};', '']
hand = [f for f in funcs if f[0] in HAND]
out += ['/* Written by hand (vulkan.c, wsi.c). */']
out += [f"VKAPI_ATTR {ret} VKAPI_CALL cham_{name}({', '.join(params)});" for name, ret, params, names, first, level in hand]
out += ['', '#ifdef CHAM_VK_FORWARDERS', '']
for name, ret, params, names, first, level in funcs:
    if name in HAND:
        continue
    kind, obj = ("I", "inst_obj") if level in ("INSTANCE", "PHYSICAL") else ("D", "dev_obj")
    args = ", ".join([f"({first})o->real"] + names[1:])
    out.append(f"static VKAPI_ATTR {ret} VKAPI_CALL cham_{name}({', '.join(params)})\n{{\n"
               f"    struct {obj} *o = (struct {obj} *){names[0]};\n"
               f"    {'' if ret == 'void' else 'return '}((PFN_{name})o->t->fn[{kind}_{name}])({args});\n}}")
out += ['', '/* Every function by name (sorted), with its slot in the table of its level;',
        ' * `own` ones are this driver\'s alone. */',
        'enum { LEVEL_GLOBAL, LEVEL_INSTANCE, LEVEL_PHYSICAL, LEVEL_DEVICE };',
        'static const struct entry {', '    const char *name;', '    PFN_vkVoidFunction fn;',
        '    unsigned short level, slot;', '    unsigned char own;', '} entries[] = {']
rows = [(name, level, "0" if level == "GLOBAL" else f"{'D' if level == 'DEVICE' else 'I'}_{name}", 0)
        for name, ret, params, names, first, level in funcs]
rows += [(name, level, "0", 1) for name, level in OWN.items()]
for name, level, slot, own in sorted(rows):
    out.append(f'    {{"{name}", (PFN_vkVoidFunction)cham_{name}, LEVEL_{level}, {slot}, {own}}},')
out += ['};', '', "/* The extensions whose functions and structures are all known here (sorted). */",
        'static const char *const known_extensions[] = {']
out += [f'    "{e}",' for e in extensions if e not in LEFT_OUT and e not in WINDOWS]
out += ['};', '', '#endif', '']
print("\n".join(out))
