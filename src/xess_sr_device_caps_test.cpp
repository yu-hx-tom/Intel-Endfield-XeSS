#include "xess_sr_device_caps.hpp"
#include <cstdio>
#include <stdexcept>

int main() {
    unsigned checks=0;
    const auto check=[&](bool ok) {++checks;if(!ok)throw std::runtime_error("SR requested feature diagnostic regression");};
    using xess_vk::sr_caps::requested;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    auto result=requested(info);
    check(result.complete&&!result.storageWriteWithoutFormat&&!result.mutableDescriptorType&&!result.shaderInt8&&!result.shaderIntegerDotProduct&&!result.scalarBlockLayout);
    VkPhysicalDeviceFeatures legacy{};legacy.shaderStorageImageWriteWithoutFormat=VK_TRUE;
    info.pEnabledFeatures=&legacy;result=requested(info);check(result.complete&&result.storageWriteWithoutFormat);
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    core.features=legacy;info.pNext=&core;
    check(!requested(info).complete); // Both legacy and Features2 is invalid Vulkan.
    info.pEnabledFeatures=nullptr;
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableType{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    mutableType.mutableDescriptorType=VK_TRUE;v12.shaderInt8=VK_TRUE;v12.scalarBlockLayout=VK_TRUE;v13.shaderIntegerDotProduct=VK_TRUE;
    core.pNext=&mutableType;mutableType.pNext=&v12;v12.pNext=&v13;
    result=requested(info);check(result.complete&&result.storageWriteWithoutFormat&&result.mutableDescriptorType&&result.shaderInt8&&result.shaderIntegerDotProduct&&result.scalarBlockLayout);
    check(core.pNext==&mutableType&&mutableType.pNext==&v12&&v12.pNext==&v13&&!v13.pNext);
    VkPhysicalDeviceShaderFloat16Int8Features int8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceShaderIntegerDotProductFeatures dot{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES};
    int8.shaderInt8=VK_TRUE;dot.shaderIntegerDotProduct=VK_TRUE;mutableType.pNext=&int8;int8.pNext=&dot;
    result=requested(info);check(result.complete&&result.shaderInt8&&result.shaderIntegerDotProduct);
    dot.pNext=&v13;check(!requested(info).complete);dot.pNext=nullptr;
    int8.pNext=&v12;check(!requested(info).complete);int8.pNext=&dot;
    mutableType.mutableDescriptorType=2;check(!requested(info).complete);mutableType.mutableDescriptorType=VK_TRUE;
    core.features.shaderStorageImageWriteWithoutFormat=2;check(!requested(info).complete);core.features=legacy;
    dot.pNext=&core;check(!requested(info).complete);dot.pNext=nullptr;
    std::array<VkBaseInStructure,129> unrelated{};
    for(std::size_t n=0;n<unrelated.size();++n){unrelated[n].sType=VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO;unrelated[n].pNext=n+1<unrelated.size()?&unrelated[n+1]:nullptr;}
    info.pNext=unrelated.data();check(!requested(info).complete);
    unrelated[127].pNext=nullptr;check(requested(info).complete);
    info.pNext=&v12;v12.shaderInt8=VK_FALSE;v12.pNext=nullptr;result=requested(info);check(result.complete&&!result.shaderInt8);
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES};
    scalar.scalarBlockLayout=VK_TRUE;info.pNext=&scalar;result=requested(info);check(result.complete&&result.scalarBlockLayout);
    scalar.pNext=&v12;check(!requested(info).complete);scalar.pNext=nullptr;
    scalar.scalarBlockLayout=2;check(!requested(info).complete);
    std::printf("{\"event\":\"sr_device_caps_cpu\",\"passed\":true,\"checks\":%u,\"deviceParametersModified\":false}\n",checks);
}
