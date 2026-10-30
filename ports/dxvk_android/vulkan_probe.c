/* Read-only Vulkan capability probe for the upstream DXVK 3.1 migration.
 * SPDX-License-Identifier: Zlib
 * Compare output with VP_DXVK_requirements.json at upstream commit
 * 70d7508c01201ed3d4bfb33da42ba834eafe3857.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FEATURE(s, f) printf("FEATURE %s.%s=%u\n", #s, #f, s.f)
#define INIT(t, n, st) t n = { .sType = st }
#define REQUIRE(s, f) do { if (!s.f) { printf("MISSING %s.%s\n", #s, #f); ++missing; } } while (0)

int main(void) {
    uint32_t version = VK_API_VERSION_1_0;
    VkResult vr = vkEnumerateInstanceVersion(&version);
    if (vr) { fprintf(stderr, "vkEnumerateInstanceVersion=%d\n", vr); return 1; }
    printf("LOADER %u.%u.%u\n", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "AC1 DXVK feasibility", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance instance;
    vr = vkCreateInstance(&ci, NULL, &instance);
    if (vr) { fprintf(stderr, "vkCreateInstance=%d\n", vr); return 1; }
    uint32_t count = 0;
    vr = vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (vr || !count) { fprintf(stderr, "vkEnumeratePhysicalDevices=%d count=%u\n", vr, count); return 1; }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) return 1;
    vr = vkEnumeratePhysicalDevices(instance, &count, devices);
    if (vr) { fprintf(stderr, "vkEnumeratePhysicalDevices=%d\n", vr); return 1; }
    unsigned suitable = 0;
    for (uint32_t i = 0; i < count; ++i) {
        unsigned missing = 0;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        printf("DEVICE %s vendor=%x device=%x api=%u.%u.%u driver=%u\n", props.deviceName, props.vendorID, props.deviceID,
            VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
        printf("LIMIT maxPushConstantsSize=%u\n", props.limits.maxPushConstantsSize);
        INIT(VkPhysicalDeviceFeatures2, core, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        INIT(VkPhysicalDeviceVulkan11Features, v11, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
        INIT(VkPhysicalDeviceVulkan12Features, v12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        INIT(VkPhysicalDeviceVulkan13Features, v13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        INIT(VkPhysicalDeviceMaintenance5FeaturesKHR, m5, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR);
        INIT(VkPhysicalDeviceMaintenance6FeaturesKHR, m6, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR);
        INIT(VkPhysicalDeviceRobustness2FeaturesEXT, robust, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT);
        INIT(VkPhysicalDeviceDepthClipEnableFeaturesEXT, clip, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT);
        core.pNext=&v11; v11.pNext=&v12; v12.pNext=&v13; v13.pNext=&m5; m5.pNext=&m6; m6.pNext=&robust; robust.pNext=&clip;
        vkGetPhysicalDeviceFeatures2(devices[i], &core);
        FEATURE(core.features, robustBufferAccess); FEATURE(core.features, fragmentStoresAndAtomics);
        FEATURE(core.features, dualSrcBlend); FEATURE(core.features, multiDrawIndirect); FEATURE(core.features, multiViewport);
        FEATURE(core.features, samplerAnisotropy); FEATURE(core.features, shaderInt16); FEATURE(core.features, shaderInt64);
        FEATURE(core.features, shaderSampledImageArrayDynamicIndexing);
        FEATURE(core.features, geometryShader); FEATURE(core.features, imageCubeArray); FEATURE(core.features, depthClamp);
        FEATURE(core.features, depthBiasClamp); FEATURE(core.features, fillModeNonSolid); FEATURE(core.features, sampleRateShading);
        FEATURE(core.features, shaderClipDistance); FEATURE(core.features, shaderCullDistance); FEATURE(core.features, textureCompressionBC);
        FEATURE(core.features, occlusionQueryPrecise); FEATURE(core.features, independentBlend); FEATURE(core.features, fullDrawIndexUint32);
        FEATURE(core.features, shaderImageGatherExtended);
        FEATURE(v11, multiview); FEATURE(v11, storageBuffer16BitAccess); FEATURE(v11, shaderDrawParameters);
        FEATURE(v12, uniformBufferStandardLayout); FEATURE(v12, subgroupBroadcastDynamicId); FEATURE(v12, imagelessFramebuffer);
        FEATURE(v12, separateDepthStencilLayouts); FEATURE(v12, hostQueryReset); FEATURE(v12, timelineSemaphore);
        FEATURE(v12, shaderSubgroupExtendedTypes); FEATURE(v12, vulkanMemoryModel); FEATURE(v12, vulkanMemoryModelDeviceScope);
        FEATURE(v12, bufferDeviceAddress); FEATURE(v12, storageBuffer8BitAccess); FEATURE(v12, shaderInt8); FEATURE(v12, descriptorIndexing);
        FEATURE(v12, descriptorBindingSampledImageUpdateAfterBind); FEATURE(v12, descriptorBindingUpdateUnusedWhilePending);
        FEATURE(v12, descriptorBindingPartiallyBound); FEATURE(v12, runtimeDescriptorArray); FEATURE(v12, samplerMirrorClampToEdge);
        FEATURE(v12, scalarBlockLayout);
        FEATURE(v13, robustImageAccess); FEATURE(v13, shaderTerminateInvocation); FEATURE(v13, shaderZeroInitializeWorkgroupMemory);
        FEATURE(v13, synchronization2); FEATURE(v13, shaderIntegerDotProduct); FEATURE(v13, maintenance4);
        FEATURE(v13, pipelineCreationCacheControl); FEATURE(v13, subgroupSizeControl); FEATURE(v13, computeFullSubgroups);
        FEATURE(v13, shaderDemoteToHelperInvocation); FEATURE(v13, inlineUniformBlock); FEATURE(v13, dynamicRendering);
        FEATURE(m5, maintenance5); FEATURE(m6, maintenance6); FEATURE(robust, nullDescriptor); FEATURE(robust, robustBufferAccess2);
        FEATURE(clip, depthClipEnable);
        /* Required features from dxvk_device_info.cpp::getFeatureList at the
         * pinned 3.1 commit. JSON profiles alone omit some common requirements. */
        REQUIRE(core.features, depthBiasClamp);
        REQUIRE(core.features, depthClamp);
        REQUIRE(core.features, dualSrcBlend);
        REQUIRE(core.features, fillModeNonSolid);
        REQUIRE(core.features, fragmentStoresAndAtomics);
        REQUIRE(core.features, fullDrawIndexUint32);
        REQUIRE(core.features, geometryShader);
        REQUIRE(core.features, imageCubeArray);
        REQUIRE(core.features, independentBlend);
        REQUIRE(core.features, multiDrawIndirect);
        REQUIRE(core.features, multiViewport);
        REQUIRE(core.features, occlusionQueryPrecise);
        REQUIRE(core.features, robustBufferAccess);
        REQUIRE(core.features, sampleRateShading);
        REQUIRE(core.features, samplerAnisotropy);
        REQUIRE(core.features, shaderClipDistance);
        REQUIRE(core.features, shaderCullDistance);
        REQUIRE(core.features, shaderImageGatherExtended);
        REQUIRE(core.features, shaderInt16);
        REQUIRE(core.features, shaderInt64);
        REQUIRE(core.features, shaderSampledImageArrayDynamicIndexing);
        REQUIRE(core.features, textureCompressionBC);
        REQUIRE(v11, shaderDrawParameters);
        REQUIRE(v11, storageBuffer16BitAccess);
        REQUIRE(v12, bufferDeviceAddress);
        REQUIRE(v12, descriptorIndexing);
        REQUIRE(v12, storageBuffer8BitAccess);
        REQUIRE(v12, descriptorBindingSampledImageUpdateAfterBind);
        REQUIRE(v12, descriptorBindingUpdateUnusedWhilePending);
        REQUIRE(v12, descriptorBindingPartiallyBound);
        REQUIRE(v12, hostQueryReset);
        REQUIRE(v12, runtimeDescriptorArray);
        REQUIRE(v12, samplerMirrorClampToEdge);
        REQUIRE(v12, scalarBlockLayout);
        REQUIRE(v12, shaderInt8);
        REQUIRE(v12, timelineSemaphore);
        REQUIRE(v12, uniformBufferStandardLayout);
        REQUIRE(v12, vulkanMemoryModel);
        REQUIRE(v13, inlineUniformBlock);
        REQUIRE(v13, computeFullSubgroups);
        REQUIRE(v13, dynamicRendering);
        REQUIRE(v13, maintenance4);
        REQUIRE(v13, shaderDemoteToHelperInvocation);
        REQUIRE(v13, shaderZeroInitializeWorkgroupMemory);
        REQUIRE(v13, subgroupSizeControl);
        REQUIRE(v13, synchronization2);
        REQUIRE(m5, maintenance5); REQUIRE(m6, maintenance6);
        REQUIRE(robust, robustBufferAccess2); REQUIRE(robust, nullDescriptor); REQUIRE(clip, depthClipEnable);
        if (props.apiVersion < VK_API_VERSION_1_3) { puts("MISSING Vulkan 1.3"); ++missing; }
        if (props.limits.maxPushConstantsSize < 256) { puts("MISSING maxPushConstantsSize>=256"); ++missing; }
        uint32_t n = 0;
        vr = vkEnumerateDeviceExtensionProperties(devices[i], NULL, &n, NULL);
        if (vr) { fprintf(stderr, "vkEnumerateDeviceExtensionProperties=%d\n", vr); return 1; }
        VkExtensionProperties *ext = calloc(n, sizeof(*ext));
        if (!ext) return 1;
        vr = vkEnumerateDeviceExtensionProperties(devices[i], NULL, &n, ext);
        if (vr) { fprintf(stderr, "vkEnumerateDeviceExtensionProperties=%d\n", vr); return 1; }
        for (uint32_t j = 0; j < n; ++j) printf("EXT %s=%u\n", ext[j].extensionName, ext[j].specVersion);
        const char *required[] = { "VK_KHR_maintenance5", "VK_KHR_maintenance6", "VK_KHR_load_store_op_none",
            "VK_EXT_robustness2", "VK_EXT_depth_clip_enable", "VK_KHR_swapchain" };
        for (size_t k = 0; k < sizeof(required)/sizeof(required[0]); ++k) {
            int found = 0;
            for (uint32_t j = 0; j < n; ++j) if (!strcmp(required[k], ext[j].extensionName)) found = 1;
            if (!found) { printf("MISSING %s\n", required[k]); ++missing; }
        }
        printf("RESULT %s missing=%u (capability query only; no device or surface created)\n", missing ? "INCOMPATIBLE" : "CANDIDATE", missing);
        if (!missing) ++suitable;
        free(ext);
    }
    free(devices);
    vkDestroyInstance(instance, NULL);
    return suitable ? 0 : 2;
}
