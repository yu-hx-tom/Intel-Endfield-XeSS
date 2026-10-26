#pragma once
// Self-owned synthetic input fixture shared by native-copy and XeFG tests.
// This creates its own Vulkan instance/device and three real device-local images.
// It is not a game image finder, rendering hook, or WSI replacement.
#include "interop.hpp"
#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace xess_vk::testing {
inline void check(VkResult value, const char* operation) {
    if (value != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(value));
}
constexpr std::array<VkFormat, 3> kFormats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT};
constexpr std::array<float, 4> kDepth{0.25f, 0.5f, 0.75f, 1.0f};
constexpr std::array<float, 4> kMotion{0.5f, -0.25f, 1.0f, -1.0f};
constexpr std::array<uint16_t, 4> kMotionBits{0x3800, 0xb400, 0x3c00, 0xbc00};

struct SourceOwner {
    SourceOwner() = default;
    SourceOwner(const SourceOwner&) = delete;
    SourceOwner& operator=(const SourceOwner&) = delete;
    xess_vk::InteropConfig cfg;
    HMODULE loader = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence completion = VK_NULL_HANDLE;
    VkFence drainFence = VK_NULL_HANDLE;
    bool drainSubmissionFailed = false;
    // Tests set this before constructing a borrower. Clear only after a normal
    // explicit consumed wait and full borrower destruction. An exception may
    // leave intentionally retained bridge child objects on this VkDevice.
    bool retainForPossibleBorrowedChildren = false;
    std::array<VkDeviceMemory, 3> allocation{};
    xess_vk::NativeFrame frame;
    bool initializedImages = false;
    bool depthStencilSource = false;
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    const char* enabledExtensions[2]{VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};

    template<class T> T inst(const char* name) const {
        const auto result = gipa(instance, name);
        if (!result) throw std::runtime_error(std::string("Missing instance function: ") + name);
        return reinterpret_cast<T>(result);
    }
    template<class T> T dev(const char* name) const {
        const auto result = gdpa(device, name);
        if (!result) throw std::runtime_error(std::string("Missing device function: ") + name);
        return reinterpret_cast<T>(result);
    }
    // A producer fence covers only our clear, not later bridge copies on the
    // borrowed queue. The tail fence covers every submission already queued.
    // Keep a timed-out fence alive for the pending submission and a later retry.
    bool drainQueue(uint32_t timeoutMs) noexcept {
        if (!device || !queue || !gdpa || drainSubmissionFailed) return false;
        try {
            if (!drainFence) {
                VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                if (dev<PFN_vkCreateFence>("vkCreateFence")(device, &create, nullptr, &drainFence) != VK_SUCCESS) return false;
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                if (dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, drainFence) != VK_SUCCESS) {
                    drainSubmissionFailed = true;
                    return false;
                }
            }
            if (dev<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &drainFence, VK_TRUE,
                    static_cast<uint64_t>(timeoutMs) * 1'000'000) != VK_SUCCESS) return false;
            dev<PFN_vkDestroyFence>("vkDestroyFence")(device, drainFence, nullptr);
            drainFence = VK_NULL_HANDLE;
            return true;
        } catch (...) { return false; }
    }
    ~SourceOwner() noexcept {
        if (retainForPossibleBorrowedChildren) return;
        try {
        if (device) {
            // A bridge can intentionally retain its Impl after GPU timeout.
            // Retain our device/images/loader too unless the entire borrowed
            // queue is known idle. Never guess that clear completion is enough.
            // Partial initialization without dispatch/queue is also retained.
            if (!drainQueue(cfg.timeoutMs)) return;
            if (pool) dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, nullptr);
            if (completion) dev<PFN_vkDestroyFence>("vkDestroyFence")(device, completion, nullptr);
            for (unsigned i = 0; i < frame.planes.size(); ++i) {
                if (frame.planes[i].image) dev<PFN_vkDestroyImage>("vkDestroyImage")(device, frame.planes[i].image, nullptr);
                if (allocation[i]) dev<PFN_vkFreeMemory>("vkFreeMemory")(device, allocation[i], nullptr);
            }
            dev<PFN_vkDestroyDevice>("vkDestroyDevice")(device, nullptr);
        }
        if (instance) inst<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (loader) FreeLibrary(loader);
        } catch (...) {
            // Best effort teardown must never throw or unload the runtime while
            // handles may remain alive. Process exit is the final owner here.
        }
    }
    void initialize(xess_vk::InteropConfig config, bool useDepthStencilSource = false) {
        config.validate();
        cfg = config;
        depthStencilSource = useDepthStencilSource;
        loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) throw std::runtime_error("No system Vulkan loader");
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        if (!gipa) throw std::runtime_error("No vkGetInstanceProcAddr");
        application.pApplicationName = "Native Vulkan source-image bridge self-test"; application.apiVersion = VK_API_VERSION_1_2;
        instanceInfo.pApplicationInfo = &application;
        check(inst<PFN_vkCreateInstance>("vkCreateInstance")(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        uint32_t count = 0;
        check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance, &count, nullptr), "vkEnumeratePhysicalDevices(count)");
        std::vector<VkPhysicalDevice> devices(count);
        check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            inst<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate, &properties);
            if (properties.vendorID == 0x8086) { physical = candidate; break; }
        }
        if (!physical) throw std::runtime_error("No Intel Vulkan device");
        uint32_t familyCount = 0;
        inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical, &familyCount, families.data());
        const auto selected = std::find_if(families.begin(), families.end(), [](const auto& value) { return value.queueCount && (value.queueFlags & VK_QUEUE_GRAPHICS_BIT); });
        if (selected == families.end()) throw std::runtime_error("No graphics queue");
        family = static_cast<uint32_t>(selected - families.begin());
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        timeline.timelineSemaphore = VK_TRUE;
        deviceInfo.pNext = &timeline; deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 2; deviceInfo.ppEnabledExtensionNames = enabledExtensions;
        check(inst<PFN_vkCreateDevice>("vkCreateDevice")(physical, &deviceInfo, nullptr, &device), "vkCreateDevice(source owner)");
        gdpa = inst<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, family, 0, &queue);
        inst<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family; poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cb.commandPool = pool; cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cb.commandBufferCount = 1;
        check(dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &cb, &command), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(dev<PFN_vkCreateFence>("vkCreateFence")(device, &fenceInfo, nullptr, &completion), "vkCreateFence");
        frame.producerQueue = queue; frame.producerCompleteFence = completion;
        for (unsigned i = 0; i < frame.planes.size(); ++i) {
            auto& source = frame.planes[i]; auto& image = source.successfulImageCreateInfo;
            const auto size = cfg.extent(static_cast<xess_vk::Format>(i));
            source.device = device; source.ownerQueueFamily = family; source.currentLayout = VK_IMAGE_LAYOUT_GENERAL;
            image.imageType = VK_IMAGE_TYPE_2D;
            image.format = i == 1 && depthStencilSource ? VK_FORMAT_D32_SFLOAT_S8_UINT : kFormats[i];
            image.extent = {size.width, size.height, 1};
            image.mipLevels = 1; image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            check(dev<PFN_vkCreateImage>("vkCreateImage")(device, &image, nullptr, &source.image), "vkCreateImage(source)");
            VkMemoryRequirements requirements{};
            dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, source.image, &requirements);
            uint32_t memoryIndex = UINT32_MAX;
            for (uint32_t j = 0; j < memory.memoryTypeCount; ++j)
                if ((requirements.memoryTypeBits & (1u << j)) && (memory.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { memoryIndex = j; break; }
            if (memoryIndex == UINT32_MAX) throw std::runtime_error("No local memory for source image");
            VkMemoryAllocateInfo memoryInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memoryInfo.allocationSize = requirements.size; memoryInfo.memoryTypeIndex = memoryIndex;
            check(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &memoryInfo, nullptr, &allocation[i]), "vkAllocateMemory(source)");
            check(dev<PFN_vkBindImageMemory>("vkBindImageMemory")(device, source.image, allocation[i], 0), "vkBindImageMemory(source)");
        }
    }
    xess_vk::BorrowedVulkanContext context() const {
        return {instance, physical, device, queue, family, 0, gipa, gdpa, &instanceInfo, &deviceInfo};
    }
    void wait() const {
        check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &completion, VK_TRUE,
                  static_cast<uint64_t>(cfg.timeoutMs) * 1'000'000), "vkWaitForFences(source owner)");
    }
    void render(uint64_t index) {
        std::array<VkClearColorValue, 3> clear{};
        clear[0].float32[0] = (index & 1) ? 1.0f : 0.0f;
        clear[0].float32[1] = (index & 2) ? 1.0f : 0.0f;
        clear[0].float32[2] = (index & 4) ? 1.0f : 0.0f; clear[0].float32[3] = 1.0f;
        clear[1].float32[0] = kDepth[index % 4];
        clear[2].float32[0] = kMotion[index % 4]; clear[2].float32[1] = kMotion[(index + 1) % 4];
        renderClear(clear);
    }
    // Real GPU clears only: these values are command parameters, not uploads.
    using RecordingCallback = void (*)(void*, VkCommandBuffer);
    void renderClear(const std::array<VkClearColorValue, 3>& clear,
                     RecordingCallback callback = nullptr, void* user = nullptr,
                     VkSemaphore signal = VK_NULL_HANDLE) {
        wait();
        check(dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command, 0), "vkResetCommandBuffer(source)");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin), "vkBeginCommandBuffer(source)");
        std::array<VkImageMemoryBarrier, 3> barriers{};
        for (unsigned i = 0; i < frame.planes.size(); ++i) {
            auto& barrier = barriers[i]; barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = initializedImages ? VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT : 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = initializedImages ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            const VkImageAspectFlags aspect = i == 1 && depthStencilSource
                                                 ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                 : VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.image = frame.planes[i].image;
            barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        }
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                                 0, 0, nullptr, 0, nullptr, 3, barriers.data());
        for (unsigned i = 0; i < frame.planes.size(); ++i) {
            if (i == 1 && depthStencilSource) {
                const VkClearDepthStencilValue depth{clear[i].float32[0], 17};
                dev<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(
                    command, frame.planes[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    &depth, 1, &barriers[i].subresourceRange);
            } else {
                dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(
                    command, frame.planes[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    &clear[i], 1, &barriers[i].subresourceRange);
            }
        }
        for (auto& barrier : barriers) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                                 0, 0, nullptr, 0, nullptr, 3, barriers.data());
        if (callback)
            callback(user, command);
        check(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command), "vkEndCommandBuffer(source)");
        check(dev<PFN_vkResetFences>("vkResetFences")(device, 1, &completion), "vkResetFences(source)");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        if (signal) {
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &signal;
        }
        check(dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, completion), "vkQueueSubmit(GPU clear source)");
        initializedImages = true;
    }
    xess_vk::FramePlanes expected(uint64_t index) const {
        const std::array<uint8_t, 4> color{static_cast<uint8_t>((index & 1) ? 255 : 0), static_cast<uint8_t>((index & 2) ? 255 : 0),
                                          static_cast<uint8_t>((index & 4) ? 255 : 0), 255};
        const float depth = kDepth[index % 4];
        const uint16_t motion[]{kMotionBits[index % 4], kMotionBits[(index + 1) % 4]};
        const void* words[]{color.data(), &depth, motion};
        xess_vk::FramePlanes result;
        for (unsigned i = 0; i < 3; ++i) {
            result[i].resize(cfg.byteSize(static_cast<xess_vk::Format>(i)));
            for (size_t offset = 0; offset < result[i].size(); offset += 4) std::memcpy(result[i].data() + offset, words[i], 4);
        }
        return result;
    }
};

} // namespace xess_vk::testing
