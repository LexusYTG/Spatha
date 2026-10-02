#ifndef SPATHA_CHAIN_H
#define SPATHA_CHAIN_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#define SPATHA_CHAIN_HDR sizeof(VkBaseOutStructure)
#define SPATHA_CHAIN_LIST(X) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,                  VkPhysicalDeviceVulkan11Features) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,                  VkPhysicalDeviceVulkan12Features) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,               VkPhysicalDevice16BitStorageFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,                   VkPhysicalDeviceMultiviewFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,           VkPhysicalDeviceVariablePointersFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,            VkPhysicalDeviceProtectedMemoryFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,    VkPhysicalDeviceSamplerYcbcrConversionFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,      VkPhysicalDeviceShaderDrawParametersFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,                VkPhysicalDevice8BitStorageFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES,         VkPhysicalDeviceShaderAtomicInt64Features) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,         VkPhysicalDeviceShaderFloat16Int8Features) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,         VkPhysicalDeviceDescriptorIndexingFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,         VkPhysicalDeviceScalarBlockLayoutFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES,       VkPhysicalDeviceImagelessFramebufferFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, VkPhysicalDeviceUniformBufferStandardLayoutFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,            VkPhysicalDeviceHostQueryResetFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,          VkPhysicalDeviceTimelineSemaphoreFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,       VkPhysicalDeviceBufferDeviceAddressFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,         VkPhysicalDeviceVulkanMemoryModelFeatures) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,                VkPhysicalDeviceVulkan11Properties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,                VkPhysicalDeviceVulkan12Properties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,                        VkPhysicalDeviceIDProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,                  VkPhysicalDeviceSubgroupProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES,            VkPhysicalDevicePointClippingProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,                 VkPhysicalDeviceMultiviewProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES,          VkPhysicalDeviceProtectedMemoryProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES,             VkPhysicalDeviceMaintenance3Properties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,                    VkPhysicalDeviceDriverProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES,            VkPhysicalDeviceFloatControlsProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES,       VkPhysicalDeviceDescriptorIndexingProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,     VkPhysicalDeviceDepthStencilResolveProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES,     VkPhysicalDeviceSamplerFilterMinmaxProperties) \
    X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES,        VkPhysicalDeviceTimelineSemaphoreProperties)
static inline size_t spatha_chain_size(VkStructureType t) {
    switch ((int)t) {
#define X(ST, T) case ST: return sizeof(T);
    SPATHA_CHAIN_LIST(X)
#undef X
    default: return 0;
    }
}
static inline const void *spatha_chain_find(const void *next, VkStructureType t) {
    for (const VkBaseInStructure *s = (const VkBaseInStructure *)next; s; s = s->pNext)
        if (s->sType == t) return s;
    return NULL;
}
static inline void spatha_mask_feat10(VkPhysicalDeviceFeatures *f) {
    f->sparseBinding = VK_FALSE;            f->sparseResidencyBuffer = VK_FALSE;
    f->sparseResidencyImage2D = VK_FALSE;   f->sparseResidencyImage3D = VK_FALSE;
    f->sparseResidency2Samples = VK_FALSE;  f->sparseResidency4Samples = VK_FALSE;
    f->sparseResidency8Samples = VK_FALSE;  f->sparseResidency16Samples = VK_FALSE;
    f->sparseResidencyAliased = VK_FALSE;
}
static inline void spatha_mask_chain_struct(void *p) {
    VkBaseOutStructure *s = (VkBaseOutStructure *)p;
    switch ((int)s->sType) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
        VkPhysicalDeviceVulkan11Features *f = (VkPhysicalDeviceVulkan11Features *)p;
        f->protectedMemory = VK_FALSE; f->samplerYcbcrConversion = VK_FALSE; break; }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
        VkPhysicalDeviceVulkan12Features *f = (VkPhysicalDeviceVulkan12Features *)p;
        f->imagelessFramebuffer = VK_FALSE; f->separateDepthStencilLayouts = VK_FALSE;
        f->bufferDeviceAddressCaptureReplay = VK_FALSE; f->bufferDeviceAddressMultiDevice = VK_FALSE; break; }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
        ((VkPhysicalDeviceProtectedMemoryFeatures *)p)->protectedMemory = VK_FALSE; break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
        ((VkPhysicalDeviceSamplerYcbcrConversionFeatures *)p)->samplerYcbcrConversion = VK_FALSE; break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:
        ((VkPhysicalDeviceImagelessFramebufferFeatures *)p)->imagelessFramebuffer = VK_FALSE; break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES:
        ((VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures *)p)->separateDepthStencilLayouts = VK_FALSE; break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: {
        VkPhysicalDeviceBufferDeviceAddressFeatures *f = (VkPhysicalDeviceBufferDeviceAddressFeatures *)p;
        f->bufferDeviceAddressCaptureReplay = VK_FALSE; f->bufferDeviceAddressMultiDevice = VK_FALSE; break; }
    default: break;
    }
}
#endif
