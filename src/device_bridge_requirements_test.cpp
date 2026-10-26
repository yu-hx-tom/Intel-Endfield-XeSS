#include "device_bridge_requirements.hpp"
#include <vulkan/vk_layer.h>

#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace xess_vk;
unsigned checks = 0, cases = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
const VkBaseInStructure* find(const VkDeviceCreateInfo& info, VkStructureType type) {
    auto* node = static_cast<const VkBaseInStructure*>(info.pNext);
    for (unsigned count = 0; node && count < 64; ++count, node = node->pNext)
        if (node->sType == type) return node;
    return nullptr;
}
template<class T> const T* findAs(const VkDeviceCreateInfo& info, VkStructureType type) {
    return reinterpret_cast<const T*>(find(info, type));
}
unsigned count(const VkDeviceCreateInfo& info, VkStructureType type) {
    unsigned result = 0;
    auto* node = static_cast<const VkBaseInStructure*>(info.pNext);
    for (unsigned i = 0; node && i < 64; ++i, node = node->pNext) result += node->sType == type;
    return result;
}
bool extension(const VkDeviceCreateInfo& info, const char* name) {
    for (std::uint32_t i = 0; i < info.enabledExtensionCount; ++i)
        if (std::strcmp(info.ppEnabledExtensionNames[i], name) == 0) return true;
    return false;
}
struct Fixture {
    std::array<VkExtensionProperties, 3> supported{};
    std::array<float, 2> priorities{1.0f, 0.25f};
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    std::array<const char*, 1> originalExtensions{"VK_KHR_swapchain"};
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    DeviceBridgeSupport support;
    Fixture() {
        std::strcpy(supported[0].extensionName, "VK_KHR_external_memory_win32");
        std::strcpy(supported[1].extensionName, "VK_KHR_external_semaphore_win32");
        std::strcpy(supported[2].extensionName, "VK_KHR_swapchain");
        queue.queueFamilyIndex = 3; queue.queueCount = 2; queue.pQueuePriorities = priorities.data();
        info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queue;
        info.enabledExtensionCount = 1; info.ppEnabledExtensionNames = originalExtensions.data();
        support.instanceApiVersion = VK_API_VERSION_1_3;
        support.physicalDeviceApiVersion = VK_API_VERSION_1_3;
        support.extensionQueryComplete = true; support.timelineFeatureQueryComplete = true;
        support.timelineSemaphore = VK_TRUE; support.extensions = supported;
    }
    DeviceBridgePlan plan() const { return planDeviceBridgeRequirements(info, support, {true}); }
};
void refused(const DeviceBridgePlan& plan, DeviceBridgePlanStatus status) {
    require(!plan.ready(), "Refused plan became ready");
    require(plan.status() == status, plan.reason());
    require(plan.createInfo() == nullptr, "Refused plan exposed partially modified info");
}
template<class T> std::array<unsigned char, sizeof(T)> snapshot(const T& value) {
    std::array<unsigned char, sizeof(T)> result{};
    std::memcpy(result.data(), &value, sizeof(T)); return result;
}
template<class T> void unchanged(const T& value, const std::array<unsigned char, sizeof(T)>& before) {
    require(std::memcmp(&value, before.data(), sizeof(T)) == 0, "Caller-owned structure was modified");
}
VkResult VKAPI_PTR fakeLoaderData(VkDevice, void*) { return VK_SUCCESS; }

void basicOwnedCopy() {
    ++cases;
    Fixture f;
    VkPhysicalDeviceFeatures legacy{}; legacy.samplerAnisotropy = VK_TRUE; legacy.shaderInt64 = VK_TRUE;
    f.info.pEnabledFeatures = &legacy;
    const auto before = snapshot(f.info);
    const auto queueBefore = snapshot(f.queue);
    const auto featuresBefore = snapshot(legacy);
    auto plan = f.plan();
    require(plan.ready(), plan.reason());
    unchanged(f.info, before); unchanged(f.queue, queueBefore); unchanged(legacy, featuresBefore);
    const auto* out = plan.createInfo();
    require(out != &f.info && out->pQueueCreateInfos != &f.queue, "Device/queue info aliases caller memory");
    require(out->queueCreateInfoCount == 1 && out->pQueueCreateInfos[0].queueFamilyIndex == 3
        && out->pQueueCreateInfos[0].queueCount == 2, "Planner changed the requested queues");
    require(out->pQueueCreateInfos[0].pQueuePriorities != f.priorities.data()
        && out->pQueueCreateInfos[0].pQueuePriorities[1] == 0.25f, "Queue priorities were not owned/preserved");
    require(out->pEnabledFeatures != &legacy && out->pEnabledFeatures->shaderInt64 == VK_TRUE
        && out->pEnabledFeatures->samplerAnisotropy == VK_TRUE, "Legacy features not preserved");
    require(extension(*out, "VK_KHR_swapchain") && extension(*out, "VK_KHR_external_memory_win32")
        && extension(*out, "VK_KHR_external_semaphore_win32"), "Required extensions absent");
    require(plan.addedExtensions().size() == 2 && plan.timelineFeatureChanged(), "Change metadata incorrect");
    const auto* timeline = findAs<VkPhysicalDeviceTimelineSemaphoreFeatures>(*out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
    require(timeline && timeline->timelineSemaphore == VK_TRUE, "Timeline was not added");
    require(!plan.requiresSynchronousLoaderCall(), "Ordinary plan unexpectedly borrows loader payloads");
    auto moved = std::move(plan);
    require(!plan.ready() && plan.createInfo() == nullptr && moved.createInfo() == out, "Moving plan invalidated owned pointers");
    f.priorities[1] = 0.9f; legacy.shaderInt64 = VK_FALSE;
    require(out->pQueueCreateInfos[0].pQueuePriorities[1] == 0.25f && out->pEnabledFeatures->shaderInt64 == VK_TRUE,
        "Output changed after modifying caller-owned buffers");
}

void existingTimelineAndFeatures2() {
    ++cases;
    Fixture f;
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR shading{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    shading.attachmentFragmentShadingRate = VK_TRUE;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.pNext = &shading;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &timeline; features.features.fillModeNonSolid = VK_TRUE;
    f.info.pNext = &features;
    const auto a = snapshot(features);
    const auto b = snapshot(timeline);
    const auto c = snapshot(shading);
    auto plan = f.plan(); require(plan.ready(), plan.reason());
    unchanged(features, a); unchanged(timeline, b); unchanged(shading, c);
    const auto& out = *plan.createInfo();
    require(count(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) == 1, "Timeline node was duplicated");
    auto* clonedFeatures = findAs<VkPhysicalDeviceFeatures2>(out, features.sType);
    auto* clonedTimeline = findAs<VkPhysicalDeviceTimelineSemaphoreFeatures>(out, timeline.sType);
    auto* clonedShading = findAs<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>(out, shading.sType);
    require(clonedFeatures && clonedFeatures != &features && clonedFeatures->features.fillModeNonSolid == VK_TRUE,
        "Features2 core features were lost");
    require(clonedTimeline && clonedTimeline != &timeline && clonedTimeline->timelineSemaphore == VK_TRUE,
        "Existing timeline node was not updated in its copy");
    require(clonedShading && clonedShading != &shading && clonedShading->attachmentFragmentShadingRate == VK_TRUE,
        "Unrelated feature node was lost/changed");
    require(clonedFeatures->pNext == clonedTimeline && clonedTimeline->pNext == clonedShading && !clonedShading->pNext,
        "Copied chain ordering or termination changed");
}

void aggregatesAndAlreadyEnabled() {
    ++cases;
    Fixture f;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.dynamicRendering = VK_TRUE; v13.synchronization2 = VK_FALSE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.pNext = &v13; v12.bufferDeviceAddress = VK_TRUE; v12.runtimeDescriptorArray = VK_TRUE;
    f.info.pNext = &v12;
    const auto before = snapshot(v12);
    const auto after = snapshot(v13);
    auto plan = f.plan(); require(plan.ready(), plan.reason()); unchanged(v12, before); unchanged(v13, after);
    const auto& out = *plan.createInfo();
    require(count(out, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) == 0, "Conflicting standalone timeline was added to Vulkan12Features");
    auto* cloned12 = findAs<VkPhysicalDeviceVulkan12Features>(out, v12.sType);
    auto* cloned13 = findAs<VkPhysicalDeviceVulkan13Features>(out, v13.sType);
    require(cloned12 && cloned12->timelineSemaphore == VK_TRUE && cloned12->bufferDeviceAddress == VK_TRUE
        && cloned12->runtimeDescriptorArray == VK_TRUE, "Vulkan12 feature values not preserved");
    require(cloned13 && cloned13->dynamicRendering == VK_TRUE && cloned13->synchronization2 == VK_FALSE,
        "Planner changed unrelated Vulkan13 features");
    const char* names[]{"VK_KHR_external_memory_win32", "VK_KHR_swapchain", "VK_KHR_external_semaphore_win32"};
    f.info.enabledExtensionCount = 3; f.info.ppEnabledExtensionNames = names; v12.timelineSemaphore = VK_TRUE;
    auto already = f.plan(); require(already.ready(), already.reason());
    require(already.addedExtensions().empty() && !already.timelineFeatureChanged()
        && already.createInfo()->enabledExtensionCount == 3, "Already-enabled requirements were duplicated");
    for (unsigned i = 0; i < 3; ++i) require(std::strcmp(already.createInfo()->ppEnabledExtensionNames[i], names[i]) == 0,
        "Existing extension ordering changed");
}

void ownershipOutlivesInputs() {
    ++cases;
    auto make = [] {
        Fixture f;
        std::string name = "VK_KHR_swapchain";
        const char* names[]{name.c_str()}; f.info.ppEnabledExtensionNames = names;
        VkDeviceQueueGlobalPriorityCreateInfo priority{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO};
        priority.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH;
        f.queue.pNext = &priority;
        VkDevicePrivateDataCreateInfo privateData{VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO};
        privateData.privateDataSlotRequestCount = 3; f.info.pNext = &privateData;
        return f.plan();
    };
    auto plan = make(); require(plan.ready(), plan.reason());
    const auto& out = *plan.createInfo();
    require(std::strcmp(out.ppEnabledExtensionNames[0], "VK_KHR_swapchain") == 0 && out.pQueueCreateInfos[0].pQueuePriorities[1] == 0.25f,
        "Owned arrays/strings did not outlive input storage");
    const auto* priority = static_cast<const VkDeviceQueueGlobalPriorityCreateInfo*>(out.pQueueCreateInfos[0].pNext);
    require(priority && priority->globalPriority == VK_QUEUE_GLOBAL_PRIORITY_HIGH && priority->pNext == nullptr,
        "Queue pNext did not survive input lifetime");
    const auto* privateData = findAs<VkDevicePrivateDataCreateInfo>(out, VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO);
    require(privateData && privateData->privateDataSlotRequestCount == 3, "Unrelated scalar create node was not preserved");
}

void rejectionCases() {
    Fixture f;
    auto reject = [&](DeviceBridgePlanStatus status) { ++cases; auto plan = f.plan(); refused(plan, status); };
    f.support.extensionQueryComplete = false; reject(DeviceBridgePlanStatus::CapabilityQueryMissing); f.support.extensionQueryComplete = true;
    f.support.timelineFeatureQueryComplete = false; reject(DeviceBridgePlanStatus::CapabilityQueryMissing); f.support.timelineFeatureQueryComplete = true;
    f.support.extensions = std::span(f.supported).subspan(1); reject(DeviceBridgePlanStatus::MissingRequiredExtension); f.support.extensions = f.supported;
    f.support.timelineSemaphore = VK_FALSE; reject(DeviceBridgePlanStatus::TimelineUnsupported); f.support.timelineSemaphore = VK_TRUE;
    f.support.instanceApiVersion = VK_API_VERSION_1_1; reject(DeviceBridgePlanStatus::UnsupportedApi); f.support.instanceApiVersion = VK_API_VERSION_1_3;
    f.support.physicalDeviceApiVersion = VK_API_VERSION_1_1; reject(DeviceBridgePlanStatus::UnsupportedApi); f.support.physicalDeviceApiVersion = VK_API_VERSION_1_3;
    VkBaseInStructure unknown{static_cast<VkStructureType>(0x7ffffffe), nullptr};
    f.info.pNext = &unknown; reject(DeviceBridgePlanStatus::UnsupportedPNext);
    auto unknownPlan = f.plan(); require(unknownPlan.refusedStructureType() == unknown.sType, "Unknown sType was not reported");
    f.info.pNext = nullptr; f.queue.pNext = &unknown; reject(DeviceBridgePlanStatus::UnsupportedPNext); f.queue.pNext = nullptr;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.pNext = &timeline; f.info.pNext = &timeline; reject(DeviceBridgePlanStatus::InvalidCreateInfo); timeline.pNext = nullptr;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.pNext = &timeline; f.info.pNext = &v12; reject(DeviceBridgePlanStatus::ConflictingFeatureChain);
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceSynchronization2Features sync{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    v13.pNext = &sync; f.info.pNext = &v13; reject(DeviceBridgePlanStatus::ConflictingFeatureChain);
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceFeatures features{};
    f.info.pNext = &features2; f.info.pEnabledFeatures = &features; reject(DeviceBridgePlanStatus::ConflictingFeatureChain);
    f.info.pNext = nullptr; f.info.pEnabledFeatures = nullptr;
    f.queue.queueCount = 0; reject(DeviceBridgePlanStatus::InvalidCreateInfo); f.queue.queueCount = 2;
    f.priorities[0] = std::numeric_limits<float>::quiet_NaN(); reject(DeviceBridgePlanStatus::InvalidCreateInfo); f.priorities[0] = 1;
    timeline.timelineSemaphore = 7; f.info.pNext = &timeline; reject(DeviceBridgePlanStatus::InvalidCreateInfo);
    timeline.timelineSemaphore = VK_FALSE;
    VkPhysicalDeviceTimelineSemaphoreFeatures second{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.pNext = &second; reject(DeviceBridgePlanStatus::ConflictingFeatureChain);
    f.info.pNext = nullptr;
    VkDeviceGroupDeviceCreateInfo pointerRich{VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO};
    f.info.pNext = &pointerRich; reject(DeviceBridgePlanStatus::UnsupportedPNext);
    ++cases;
    // Opt-out must not inspect unsupported chains, query evidence, or queue data.
    f.support = {}; f.queue.pQueuePriorities = nullptr;
    refused(planDeviceBridgeRequirements(f.info, f.support), DeviceBridgePlanStatus::NotRequested);
}

void loaderBorrowing() {
    ++cases;
    Fixture f;
    VkLayerDeviceLink link{};
    VkLayerDeviceCreateInfo callback{}; callback.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
    callback.function = VK_LOADER_DATA_CALLBACK; callback.u.pfnSetDeviceLoaderData = fakeLoaderData;
    VkLayerDeviceCreateInfo loader{}; loader.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
    loader.pNext = &callback; loader.function = VK_LAYER_LINK_INFO; loader.u.pLayerInfo = &link;
    f.info.pNext = &loader;
    const auto before = snapshot(loader);
    const auto callbackBefore = snapshot(callback);
    const auto linkBefore = snapshot(link);
    refused(f.plan(), DeviceBridgePlanStatus::BorrowedLoaderChainNotAllowed);
    auto plan = planDeviceBridgeRequirements(f.info, f.support,
        {true, AllowBorrowedLoaderChain::SynchronousCreateDeviceCallOnly});
    require(plan.ready(), plan.reason());
    require(plan.requiresSynchronousLoaderCall(), "Borrowed loader lifetime constraint was not exposed");
    unchanged(loader, before); unchanged(callback, callbackBefore); unchanged(link, linkBefore);
    const auto* clone = static_cast<const VkLayerDeviceCreateInfo*>(plan.createInfo()->pNext);
    const auto* clonedCallback = static_cast<const VkLayerDeviceCreateInfo*>(clone->pNext);
    require(clone != &loader && clone->function == VK_LAYER_LINK_INFO && clone->u.pLayerInfo == &link,
        "Loader union payload changed or top-level node not copied");
    require(clonedCallback != &callback && clonedCallback->function == VK_LOADER_DATA_CALLBACK
        && clonedCallback->u.pfnSetDeviceLoaderData == fakeLoaderData, "Loader data callback payload changed");
    require(count(*plan.createInfo(), VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) == 2,
        "Multiple known loader nodes did not survive");
    loader.function = VK_LOADER_FEATURES;
    refused(planDeviceBridgeRequirements(f.info, f.support,
        {true, AllowBorrowedLoaderChain::SynchronousCreateDeviceCallOnly}), DeviceBridgePlanStatus::UnsupportedPNext);
}

void optionalTimestampCalibration() {
    ++cases;
    Fixture f;
    const auto before = snapshot(f.info);
    const DeviceBridgePlanOptions diagnostic{true, AllowBorrowedLoaderChain::No, true};
    auto missing = planDeviceBridgeRequirements(f.info, f.support, diagnostic);
    require(missing.ready() && missing.addedExtensions().size() == 2,
        "Missing optional calibration support must retain the ready bridge");
    std::vector<VkExtensionProperties> supported(f.supported.begin(), f.supported.end());
    VkExtensionProperties ext{}, khr{};
    std::strcpy(ext.extensionName, "VK_EXT_calibrated_timestamps");
    std::strcpy(khr.extensionName, "VK_KHR_calibrated_timestamps");
    supported.push_back(ext); f.support.extensions = supported;
    auto disabled = f.plan();
    require(disabled.ready() && disabled.addedExtensions().size() == 2 &&
        !extension(*disabled.createInfo(), ext.extensionName), "Calibration extension remains opt-in");
    auto legacy = planDeviceBridgeRequirements(f.info, f.support, diagnostic);
    require(legacy.ready() && legacy.addedExtensions().size() == 3 &&
        extension(*legacy.createInfo(), ext.extensionName), "EXT-only drivers receive the advertised calibration extension");
    supported.push_back(khr); f.support.extensions = supported;
    auto preferred = planDeviceBridgeRequirements(f.info, f.support, diagnostic);
    require(preferred.ready() && preferred.addedExtensions().size() == 3 &&
        extension(*preferred.createInfo(), khr.extensionName) && !extension(*preferred.createInfo(), ext.extensionName),
        "KHR is preferred when neither calibration alias was originally enabled");
    unchanged(f.info, before);
    std::array<const char*, 2> enabled{"VK_KHR_swapchain", ext.extensionName};
    f.info.enabledExtensionCount = enabled.size(); f.info.ppEnabledExtensionNames = enabled.data();
    auto existing = planDeviceBridgeRequirements(f.info, f.support, diagnostic);
    require(existing.ready() && existing.addedExtensions().size() == 2 &&
        extension(*existing.createInfo(), ext.extensionName) && !extension(*existing.createInfo(), khr.extensionName),
        "The application's existing calibration alias is preserved without adding the other alias");
    supported.clear();
    require(extension(*preferred.createInfo(), "VK_KHR_calibrated_timestamps") &&
        extension(*legacy.createInfo(), "VK_EXT_calibrated_timestamps"), "Calibration extension names must have owned lifetimes");
    f.support = {};
    refused(planDeviceBridgeRequirements(f.info, f.support, {false, AllowBorrowedLoaderChain::No, true}),
        DeviceBridgePlanStatus::NotRequested);
}
} // namespace

int main() {
    try {
        basicOwnedCopy(); existingTimelineAndFeatures2(); aggregatesAndAlreadyEnabled();
        ownershipOutlivesInputs(); rejectionCases(); loaderBorrowing(); optionalTimestampCalibration();
        std::cout << "{\"passed\":true,\"cases\":" << cases << ",\"checks\":" << checks
                  << ",\"vulkanCallsMade\":0,\"gameDeviceModified\":false,\"gameXeFGVerified\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Device requirement planner test failed: " << error.what() << '\n';
        return 1;
    }
}
