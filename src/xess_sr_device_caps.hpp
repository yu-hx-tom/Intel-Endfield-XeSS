#pragma once
#include <vulkan/vulkan.h>
#include <array>

namespace xess_vk::sr_caps {
struct Requested {
    bool complete=false;
    bool storageWriteWithoutFormat=false;
    bool mutableDescriptorType=false;
    bool shaderInt8=false;
    bool shaderIntegerDotProduct=false;
    bool scalarBlockLayout=false;
};

// Diagnostic only: borrowed creation parameters are never changed or retained.
inline Requested requested(const VkDeviceCreateInfo& info) noexcept {
    Requested result;
    unsigned seen=0;
    const auto read=[&](unsigned field,VkBool32 value,bool& output) {
        if((seen&field)||value>VK_TRUE)return false;
        seen|=field;output=value==VK_TRUE;return true;
    };
    if(info.pEnabledFeatures&&!read(1,info.pEnabledFeatures->shaderStorageImageWriteWithoutFormat,
                                  result.storageWriteWithoutFormat))return {};
    std::array<const VkBaseInStructure*,128> visited{};
    auto* node=static_cast<const VkBaseInStructure*>(info.pNext);
    std::size_t count=0;
    for(;node;node=node->pNext) {
        if(count==visited.size())return {};
        for(std::size_t n=0;n<count;++n)if(visited[n]==node)return {};
        visited[count++]=node;
        switch(node->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            if(!read(1,reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->features.shaderStorageImageWriteWithoutFormat,
                     result.storageWriteWithoutFormat))return {};
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            if(!read(2,reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node)->shaderInt8,result.shaderInt8))return {};
            if(!read(16,reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node)->scalarBlockLayout,result.scalarBlockLayout))return {};
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
            if(!read(16,reinterpret_cast<const VkPhysicalDeviceScalarBlockLayoutFeatures*>(node)->scalarBlockLayout,result.scalarBlockLayout))return {};
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
            if(!read(2,reinterpret_cast<const VkPhysicalDeviceShaderFloat16Int8Features*>(node)->shaderInt8,result.shaderInt8))return {};
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            if(!read(4,reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(node)->shaderIntegerDotProduct,
                     result.shaderIntegerDotProduct))return {};
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:
            if(!read(4,reinterpret_cast<const VkPhysicalDeviceShaderIntegerDotProductFeatures*>(node)->shaderIntegerDotProduct,
                     result.shaderIntegerDotProduct))return {};
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT:
            if(!read(8,reinterpret_cast<const VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT*>(node)->mutableDescriptorType,
                     result.mutableDescriptorType))return {};
            break;
        default: break; // Other valid application/loader structures are unrelated.
        }
    }
    result.complete=true;
    return result;
}
} // namespace xess_vk::sr_caps
