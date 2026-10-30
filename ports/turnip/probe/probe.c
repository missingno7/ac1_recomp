/* Standalone loader/feature probe. No app, surface, or game data access. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static VkExtensionProperties *extensions;
static uint32_t extension_count;
static int has(const char *name) {
    for (uint32_t i = 0; i < extension_count; ++i)
        if (!strcmp(name, extensions[i].extensionName)) return 1;
    return 0;
}
static void version(const char *name, uint32_t v) {
    printf("%s=%u.%u.%u (0x%08x)\n", name, VK_API_VERSION_MAJOR(v),
           VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v), v);
}
#define LOAD(type, name) type name = (type)gipa(instance, #name); \
    if (!name) { fprintf(stderr, "Missing entry: %s\n", #name); return 3; }
#define CHAIN(obj) do { (obj).pNext = f.pNext; f.pNext = &(obj); } while (0)

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    printf("probe_pid=%d\n", getpid());
    if (argc < 2 || (strcmp(argv[1], "system") && strcmp(argv[1], "turnip") && strcmp(argv[1], "direct"))) {
        fprintf(stderr, "Usage: %s system|turnip|direct [absolute library directory ending in /]\n", argv[0]);
        return 2;
    }
    printf("requested_driver=%s\n", argv[1]);
    void *library = NULL;
    if (!strcmp(argv[1], "turnip")) {
        if (argc != 3) return 2;
        printf("hook_dir=%s (shell namespace; no app-private classloader namespace)\n", argv[2]);
        dlerror();
        library = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
            NULL, argv[2], argv[2], "libvulkan_freedreno.so", NULL, NULL);
    } else if (!strcmp(argv[1], "direct")) {
        if (argc != 3) return 2;
        char path[4096];
        snprintf(path, sizeof(path), "%slibvulkan_freedreno.so", argv[2]);
        library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    } else library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        const char *error = dlerror();
        fprintf(stderr, "Loader failed: %s (errno is not a complete adrenotools diagnostic)\n", error ? error : "no dlerror; check linkernsbypass status and hooks");
        return 3;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
    if (!gipa) { fprintf(stderr, "Missing vkGetInstanceProcAddr/vk_icdGetInstanceProcAddr: %s\n", dlerror()); return 3; }
    VkInstance instance = VK_NULL_HANDLE;
    LOAD(PFN_vkCreateInstance, vkCreateInstance);
    LOAD(PFN_vkEnumerateInstanceExtensionProperties, vkEnumerateInstanceExtensionProperties);
    uint32_t instance_ext_count = 0;
    VkResult ir = vkEnumerateInstanceExtensionProperties(NULL, &instance_ext_count, NULL);
    if (ir) { printf("vkEnumerateInstanceExtensionProperties=%d\n", ir); return 4; }
    VkExtensionProperties *instance_ext = calloc(instance_ext_count, sizeof(*instance_ext));
    if (!instance_ext) return 9;
    ir = vkEnumerateInstanceExtensionProperties(NULL, &instance_ext_count, instance_ext);
    if (ir) return 4;
    const char *surface_exts[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    for (unsigned j = 0; j < 2; ++j) {
        int found = 0;
        for (uint32_t k = 0; k < instance_ext_count; ++k)
            if (!strcmp(instance_ext[k].extensionName, surface_exts[j])) found = 1;
        printf("instance_extension %s=%d\n", surface_exts[j], found);
        if (!found) return 4;
    }
    free(instance_ext);
    PFN_vkEnumerateInstanceVersion enumVersion = (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
    uint32_t api = VK_API_VERSION_1_0;
    if (enumVersion) { VkResult r = enumVersion(&api); if (r) { printf("vkEnumerateInstanceVersion=%d\n", r); return 4; } }
    version("loader_api", api);
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName="AC1 standalone driver probe",
                            .apiVersion=api < VK_API_VERSION_1_3 ? api : VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&app,
        .enabledExtensionCount=2, .ppEnabledExtensionNames=surface_exts};
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    printf("vkCreateInstance=%d\n", r);
    if (r) return 4;
    LOAD(PFN_vkDestroyInstance, vkDestroyInstance);
    LOAD(PFN_vkEnumeratePhysicalDevices, vkEnumeratePhysicalDevices);
    LOAD(PFN_vkGetPhysicalDeviceProperties2, vkGetPhysicalDeviceProperties2);
    LOAD(PFN_vkGetPhysicalDeviceFeatures2, vkGetPhysicalDeviceFeatures2);
    LOAD(PFN_vkEnumerateDeviceExtensionProperties, vkEnumerateDeviceExtensionProperties);
    LOAD(PFN_vkGetPhysicalDeviceQueueFamilyProperties, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(PFN_vkCreateDevice, vkCreateDevice);
    LOAD(PFN_vkGetDeviceProcAddr, vkGetDeviceProcAddr);
    uint32_t count = 0;
    r = vkEnumeratePhysicalDevices(instance, &count, NULL);
    printf("vkEnumeratePhysicalDevices=%d count=%u\n", r, count);
    if (r || !count) return 5;
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) return 9;
    r = vkEnumeratePhysicalDevices(instance, &count, devices);
    if (r) return 5;
    int result = 0;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceDriverProperties driver = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext=&driver};
        vkGetPhysicalDeviceProperties2(devices[i], &props);
        printf("device=%s driver=%s info=%s driverID=%u vendor=0x%x\n", props.properties.deviceName,
               driver.driverName, driver.driverInfo, driver.driverID, props.properties.vendorID);
        version("device_api", props.properties.apiVersion);
        printf("driver_version_raw=0x%08x\n", props.properties.driverVersion);
        if (driver.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY)
            printf("qualcomm_driver_version=%u.%u.%u\n", props.properties.driverVersion >> 22,
                   (props.properties.driverVersion >> 12) & 0x3ff, props.properties.driverVersion & 0xfff);
        else version("driver_version", props.properties.driverVersion);
        if (!strcmp(argv[1], "turnip") && driver.driverID != VK_DRIVER_ID_MESA_TURNIP) {
            puts("ERROR: adrenotools returned a non-Turnip driver (silent fallback detected)"); result = 6;
        }
        extension_count = 0;
        r = vkEnumerateDeviceExtensionProperties(devices[i], NULL, &extension_count, NULL);
        if (r) return 5;
        extensions = calloc(extension_count, sizeof(*extensions));
        if (!extensions) return 9;
        r = vkEnumerateDeviceExtensionProperties(devices[i], NULL, &extension_count, extensions);
        if (r) return 5;
        VkPhysicalDeviceFeatures2 f = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan11Features vk11 = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features vk12 = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features vk13 = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        CHAIN(vk11); CHAIN(vk12);
        if (props.properties.apiVersion >= VK_API_VERSION_1_3) CHAIN(vk13);
        VkPhysicalDeviceRobustness2FeaturesEXT robustness = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        VkPhysicalDeviceDepthClipEnableFeaturesEXT depth = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
        VkPhysicalDeviceMaintenance5FeaturesKHR m5 = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        VkPhysicalDeviceMaintenance6FeaturesKHR m6 = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        if (has("VK_EXT_robustness2")) CHAIN(robustness);
        if (has("VK_EXT_depth_clip_enable")) CHAIN(depth);
        if (has("VK_KHR_maintenance5")) CHAIN(m5);
        if (has("VK_KHR_maintenance6")) CHAIN(m6);
        if (has("VK_EXT_extended_dynamic_state")) CHAIN(eds);
        if (has("VK_EXT_extended_dynamic_state2")) CHAIN(eds2);
        if (has("VK_EXT_extended_dynamic_state3")) CHAIN(eds3);
        vkGetPhysicalDeviceFeatures2(devices[i], &f);
        unsigned missing = 0;
#define REQUIRE(label, value) do { printf("required %-58s %u\n", label, (unsigned)(value)); if (!(value)) ++missing; } while (0)
#define CORE(group, member) REQUIRE(#group "." #member, group.member)
        REQUIRE("Vulkan >= 1.3", props.properties.apiVersion >= VK_API_VERSION_1_3);
#include "required_core.inc"
        const char *required_exts[] = {"VK_EXT_depth_clip_enable", "VK_EXT_robustness2", "VK_KHR_maintenance5",
            "VK_KHR_maintenance6", "VK_KHR_load_store_op_none", "VK_KHR_swapchain"};
        for (unsigned j = 0; j < sizeof(required_exts)/sizeof(*required_exts); ++j) REQUIRE(required_exts[j], has(required_exts[j]));
        REQUIRE("depthClipEnable", depth.depthClipEnable);
        REQUIRE("robustBufferAccess2", robustness.robustBufferAccess2);
        REQUIRE("nullDescriptor", robustness.nullDescriptor);
        REQUIRE("maintenance5", m5.maintenance5);
        REQUIRE("maintenance6", m6.maintenance6);
        printf("optional robustImageAccess2=%u extendedDynamicState=%u extendedDynamicState2=%u\n",
               robustness.robustImageAccess2, eds.extendedDynamicState, eds2.extendedDynamicState2);
        printf("optional EDS3 alphaToCoverage=%u depthClip=%u rasterizationSamples=%u sampleMask=%u lineRasterizationMode=%u sampleLocations=%u\n",
               eds3.extendedDynamicState3AlphaToCoverageEnable, eds3.extendedDynamicState3DepthClipEnable,
               eds3.extendedDynamicState3RasterizationSamples, eds3.extendedDynamicState3SampleMask,
               eds3.extendedDynamicState3LineRasterizationMode, eds3.extendedDynamicState3SampleLocationsEnable);
        printf("maxPushConstantsSize=%u\n", props.properties.limits.maxPushConstantsSize);
        REQUIRE("maxPushConstantsSize >= 256 (without descriptorHeap)", props.properties.limits.maxPushConstantsSize >= 256);
        uint32_t queues = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queues, NULL);
        VkQueueFamilyProperties *q = calloc(queues, sizeof(*q));
        if (!q) return 9;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queues, q);
        uint32_t family = 0;
        while (family < queues && (!q[family].queueCount || (q[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))) ++family;
        REQUIRE("graphics + compute queue", family < queues);
        printf("DXVK_3_1_missing_requirements=%u\n", missing);
        if (missing) result = result ? result : 7;
        if (family < queues) {
            float priority = 1.f;
            VkDeviceQueueCreateInfo qi = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex=family, .queueCount=1, .pQueuePriorities=&priority};
            /* Create a minimal device even when DXVK cannot run; otherwise enable the queried
             * feature set, to validate the required feature chain and extension combination. */
            VkDeviceCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .queueCreateInfoCount=1, .pQueueCreateInfos=&qi};
            if (!missing) {
                ci.pNext = &f;
                ci.enabledExtensionCount = sizeof(required_exts)/sizeof(*required_exts);
                ci.ppEnabledExtensionNames = required_exts;
                /* Extra EDS features need their extensions when enabled. They were query-only. */
                f.pNext = &m6; m6.pNext = &m5; m5.pNext = &depth; depth.pNext = &robustness;
                robustness.pNext = &vk13; vk13.pNext = &vk12; vk12.pNext = &vk11; vk11.pNext = NULL;
            }
            VkDevice device = VK_NULL_HANDLE;
            r = vkCreateDevice(devices[i], &ci, NULL, &device);
            printf("vkCreateDevice(%s)=%d\n", missing ? "minimal" : "queried DXVK requirements", r);
            if (r) result = 8;
            else {
                PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(device, "vkDestroyDevice");
                if (!destroy) return 3;
                destroy(device, NULL);
            }
        }
        free(q); free(extensions);
    }
    free(devices);
    vkDestroyInstance(instance, NULL);
    /* Keep loader handles alive for the whole process, as the app must. */
    return result;
}
