// Owned Vulkan pixels through the production continuous Provider. Descriptor
// evidence is constructed from this fixture's actual creation/recording facts;
// no game, hook, descriptor tracker, XeFG or installer participates.
#include "continuous_provider.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace cp = xess_vk::continuous;
namespace dm = xess_vk::descriptor_image_metadata;
constexpr VkExtent2D extent{31, 19};
constexpr std::size_t pixels = extent.width * extent.height;
constexpr std::uint64_t timeout = 5'000'000'000;
template<class T> std::uint64_t bits(T value) { return reinterpret_cast<std::uint64_t>(value); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
DWORD WINAPI watchdog(void* event) {
    if (WaitForSingleObject(static_cast<HANDLE>(event), 28000) == WAIT_TIMEOUT) {
        const char message[] = "Continuous provider GPU fixture exceeded 28 seconds.\n";
        DWORD written{};
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, sizeof(message) - 1, &written, nullptr);
        TerminateProcess(GetCurrentProcess(), 124);
    }
    return 0;
}
struct Watchdog {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = event ? CreateThread(nullptr, 0, watchdog, event, 0, nullptr) : nullptr;
    Watchdog() { require(event && thread, "fixture watchdog creation"); }
    ~Watchdog() { SetEvent(event); WaitForSingleObject(thread, INFINITE); CloseHandle(thread); CloseHandle(event); }
};

// These wrappers forward every actual command exactly once. Only the Provider
// receives this resolver, so the trace observes its implementation directly.
PFN_vkGetDeviceProcAddr actualResolver{};
PFN_vkCmdPipelineBarrier actualBarrier{};
PFN_vkCmdCopyImageToBuffer actualImageToBuffer{};
PFN_vkCmdCopyBufferToImage actualBufferToImage{};
PFN_vkCmdCopyImage actualCopyImage{};
struct Trace {
    std::uint64_t commands{};
    VkImage depth{}, motion{};
    VkBuffer intermediate{};
    unsigned depthCopies{}, convertedCopies{}, motionCopies{}, depthRestores{}, motionRestores{};
    bool valid{true};
} trace;
constexpr VkImageLayout depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
constexpr VkImageLayout motionLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
VKAPI_ATTR void VKAPI_CALL trackedBarrier(VkCommandBuffer command, VkPipelineStageFlags source,
    VkPipelineStageFlags destination, VkDependencyFlags flags, std::uint32_t memoryCount,
    const VkMemoryBarrier* memory, std::uint32_t bufferCount, const VkBufferMemoryBarrier* buffers,
    std::uint32_t imageCount, const VkImageMemoryBarrier* images) {
    ++trace.commands;
    for (std::uint32_t i = 0; i < imageCount; ++i) {
        const auto& image = images[i];
        if (image.image == trace.depth && image.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            ++trace.depthRestores;
            trace.valid &= image.newLayout == depthLayout && image.subresourceRange.aspectMask ==
                (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        }
        if (image.image == trace.motion && image.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            ++trace.motionRestores;
            trace.valid &= image.newLayout == motionLayout && image.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }
    actualBarrier(command, source, destination, flags, memoryCount, memory, bufferCount, buffers, imageCount, images);
}
VKAPI_ATTR void VKAPI_CALL trackedImageToBuffer(VkCommandBuffer command, VkImage source,
    VkImageLayout layout, VkBuffer destination, std::uint32_t count, const VkBufferImageCopy* regions) {
    ++trace.commands; ++trace.depthCopies;
    trace.intermediate = destination;
    trace.valid &= source == trace.depth && layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && count == 1;
    if (count == 1) trace.valid &= regions[0].imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT &&
        regions[0].imageExtent.width == extent.width && regions[0].imageExtent.height == extent.height;
    actualImageToBuffer(command, source, layout, destination, count, regions);
}
VKAPI_ATTR void VKAPI_CALL trackedBufferToImage(VkCommandBuffer command, VkBuffer source,
    VkImage destination, VkImageLayout layout, std::uint32_t count, const VkBufferImageCopy* regions) {
    ++trace.commands; ++trace.convertedCopies;
    trace.valid &= source == trace.intermediate && destination != trace.depth && destination != trace.motion &&
        layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count == 1;
    if (count == 1) trace.valid &= regions[0].imageSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
    actualBufferToImage(command, source, destination, layout, count, regions);
}
VKAPI_ATTR void VKAPI_CALL trackedCopyImage(VkCommandBuffer command, VkImage source, VkImageLayout sourceLayout,
    VkImage destination, VkImageLayout destinationLayout, std::uint32_t count, const VkImageCopy* regions) {
    ++trace.commands; ++trace.motionCopies;
    trace.valid &= source == trace.motion && destination != trace.depth && destination != trace.motion &&
        sourceLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && destinationLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count == 1;
    actualCopyImage(command, source, sourceLayout, destination, destinationLayout, count, regions);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL providerResolver(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkCmdPipelineBarrier") == 0) return reinterpret_cast<PFN_vkVoidFunction>(trackedBarrier);
    if (std::strcmp(name, "vkCmdCopyImageToBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(trackedImageToBuffer);
    if (std::strcmp(name, "vkCmdCopyBufferToImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(trackedBufferToImage);
    if (std::strcmp(name, "vkCmdCopyImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(trackedCopyImage);
    return actualResolver(device, name);
}
struct Source { VkImage image{}; VkDeviceMemory memory{}; VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; };
struct Fixture {
    HMODULE loader{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetDeviceProcAddr gdpa{};
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    std::uint32_t family{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{};
    // Four producers, one capacity refusal recording, one completed consumer.
    std::array<VkCommandBuffer, 6> commands{};
    std::array<VkSemaphore, 4> ready{};
    std::array<VkFence, 4> producerFences{};
    VkSemaphore gate{};
    VkFence presentFence{}, readbackFence{}, drainFence{};
    VkShaderModule shader{}; VkPipelineLayout pipelineLayout{}; VkPipeline pipeline{};
    Source depth, motion;
    VkBuffer readback{}; VkDeviceMemory readbackMemory{}; bool coherent{};
    void* mapped{};
    std::unique_ptr<cp::Provider> provider;
    std::array<cp::Bundle, 4> bundles{};
    bool gateClosed{}, queueUsed{}, drainSubmitted{}, cleanupDone{}, cleanupSucceeded{};
    bool pendingVerified{}, saturationVerified{}, restorationVerified{}, generationAdvanced{};
    std::uint64_t verifiedPixels{}, producerCommands{};
    unsigned submissions{}, presentWaits{}, consumerReadbacks{};
    bool injectMismatch{};
    ~Fixture() { (void)cleanup(); }
    template<class T> T inst(const char* name) const {
        auto function = gipa(instance, name); require(function != nullptr, name); return reinterpret_cast<T>(function);
    }
    template<class T> T dev(const char* name) const {
        auto function = gdpa(device, name); require(function != nullptr, name); return reinterpret_cast<T>(function);
    }
    std::uint32_t memoryType(std::uint32_t mask, VkMemoryPropertyFlags wanted) const {
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((mask & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        throw std::runtime_error("fixture memory type unavailable");
    }
    static VkImageAspectFlags aspect(VkFormat format) {
        return format == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    }
    VkImageMemoryBarrier barrier(VkImage image, VkImageAspectFlags range, VkImageLayout before,
                                VkImageLayout after, VkAccessFlags source, VkAccessFlags destination) const {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = image;
        b.oldLayout = before; b.newLayout = after; b.srcAccessMask = source; b.dstAccessMask = destination;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {range, 0, 1, 0, 1}; return b;
    }
    void transition(VkCommandBuffer command, const VkImageMemoryBarrier& b) const {
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    void createSource(Source& source, VkFormat format) {
        VkFormatProperties properties{};
        inst<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical, format, &properties);
        constexpr auto needed = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        require((properties.optimalTilingFeatures & needed) == needed, "source format transfer/sample features unavailable");
        auto& info = source.info; info.imageType = VK_IMAGE_TYPE_2D; info.format = format;
        info.extent = {extent.width, extent.height, 1}; info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (format == VK_FORMAT_D32_SFLOAT_S8_UINT) info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(dev<PFN_vkCreateImage>("vkCreateImage")(device, &info, nullptr, &source.image), "create owned source");
        VkMemoryRequirements requirements{};
        dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, source.image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &allocation, nullptr, &source.memory), "allocate owned source");
        check(dev<PFN_vkBindImageMemory>("vkBindImageMemory")(device, source.image, source.memory, 0), "bind owned source");
    }
    void initialize() {
        require(!GetModuleHandleW(L"xess-vulkan-observer.dll"), "fixture must not load production observer");
        loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(loader != nullptr, "system Vulkan loader unavailable");
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        require(gipa != nullptr, "vkGetInstanceProcAddr unavailable");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "Owned continuous provider conversion";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        check(inst<PFN_vkCreateInstance>("vkCreateInstance")(&create, nullptr, &instance), "create instance");
        std::uint32_t count{};
        check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance, &count, nullptr), "device count");
        std::vector<VkPhysicalDevice> devices(count);
        check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance, &count, devices.data()), "devices");
        VkPhysicalDeviceProperties selected{};
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            inst<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate, &properties);
            if (properties.apiVersion >= VK_API_VERSION_1_2 && (!physical || properties.vendorID == 0x8086)) {
                physical = candidate; selected = properties; if (properties.vendorID == 0x8086) break;
            }
        }
        require(physical != VK_NULL_HANDLE, "Vulkan 1.2 device unavailable");
        VkPhysicalDeviceTimelineSemaphoreFeatures available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &available;
        inst<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical, &features);
        require(available.timelineSemaphore, "timeline semaphore feature unavailable");
        inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical, &count, families.data());
        const auto chosen = std::find_if(families.begin(), families.end(), [](const auto& f) {
            return f.queueCount && (f.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
        });
        require(chosen != families.end(), "graphics/compute queue unavailable"); family = static_cast<std::uint32_t>(chosen - families.begin());
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceTimelineSemaphoreFeatures enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES}; enabled.timelineSemaphore = VK_TRUE;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; deviceInfo.pNext = &enabled;
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
        check(inst<PFN_vkCreateDevice>("vkCreateDevice")(physical, &deviceInfo, nullptr, &device), "create device");
        gdpa = inst<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr"); actualResolver = gdpa;
        actualBarrier = dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        actualImageToBuffer = dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
        actualBufferToImage = dev<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
        actualCopyImage = dev<PFN_vkCmdCopyImage>("vkCmdCopyImage");
        dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, family, 0, &queue);
        inst<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; poolInfo.queueFamilyIndex = family;
        check(dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &poolInfo, nullptr, &pool), "create command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = commands.size();
        check(dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &allocation, commands.data()), "allocate commands");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (auto& fence : producerFences) check(dev<PFN_vkCreateFence>("vkCreateFence")(device, &fenceInfo, nullptr, &fence), "producer fence");
        for (auto* fence : {&presentFence, &readbackFence, &drainFence})
            check(dev<PFN_vkCreateFence>("vkCreateFence")(device, &fenceInfo, nullptr, fence), "consumer fence");
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& semaphore : ready) check(dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device, &semaphoreInfo, nullptr, &semaphore), "producer signal");
        VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        semaphoreInfo.pNext = &timeline;
        check(dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device, &semaphoreInfo, nullptr, &gate), "timeline gate");
        createSource(depth, VK_FORMAT_D32_SFLOAT_S8_UINT); createSource(motion, VK_FORMAT_R16G16_SFLOAT);
        trace.depth = depth.image; trace.motion = motion.image;
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; buffer.size = ((pixels * 17) + 3) & ~VkDeviceSize(3);
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(dev<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &buffer, nullptr, &readback), "readback buffer");
        VkMemoryRequirements requirements{};
        dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, readback, &requirements);
        VkMemoryAllocateInfo host{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; host.allocationSize = requirements.size;
        try { host.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); coherent = true; }
        catch (...) { host.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT); }
        check(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &host, nullptr, &readbackMemory), "readback memory");
        check(dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, readback, readbackMemory, 0), "bind readback");
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        check(dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device, &layout, nullptr, &pipelineLayout), "no-op layout");
        constexpr std::uint32_t code[]{0x07230203,0x00010000,0,5,0,(2u<<16)|17,1,(3u<<16)|14,0,1,(5u<<16)|15,5,3,0x6e69616d,0,
            (6u<<16)|16,3,17,1,1,1,(2u<<16)|19,1,(3u<<16)|33,2,1,(5u<<16)|54,1,3,0,2,(2u<<16)|248,4,(1u<<16)|253,(1u<<16)|56};
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; module.codeSize = sizeof(code); module.pCode = code;
        check(dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(device, &module, nullptr, &shader), "no-op shader");
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; compute.layout = pipelineLayout;
        compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = shader; compute.stage.pName = "main";
        check(dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline), "no-op pipeline");
        provider = std::make_unique<cp::Provider>(cp::Config{device, queue, family, memory, providerResolver, false});
        std::printf("{\"event\":\"continuous_runtime\",\"vendorId\":%u,\"deviceId\":%u,\"queueFamily\":%u,\"observerLoaded\":false,\"separateDepthStencilLayoutsEnabled\":false}\n", selected.vendorID, selected.deviceID, family);
    }
    void begin(VkCommandBuffer command) const {
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &info), "begin command");
    }
    static float depthValue(unsigned frame) { return 0.125f + 0.25f * frame; }
    static std::array<float, 2> motionValue(unsigned frame) {
        constexpr std::array<std::array<float, 2>, 4> values{{{-0.25f, 0.5f}, {0.75f, -0.125f}, {-0.5f, -0.75f}, {0.125f, 0.25f}}};
        return values.at(frame);
    }
    static std::uint32_t motionBits(unsigned frame) {
        constexpr std::array<std::uint32_t, 4> values{0x3800b400u, 0xb0003a00u, 0xba00b800u, 0x34003000u};
        return values.at(frame);
    }
    void clear(VkCommandBuffer command, unsigned frame) const {
        for (const auto* source : {&depth, &motion}) {
            const auto layout = source == &depth ? depthLayout : motionLayout;
            const auto range = aspect(source->info.format);
            auto b = barrier(source->image, range, frame ? layout : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, frame ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            transition(command, b);
            if (source == &depth) {
                VkClearDepthStencilValue value{depthValue(frame), 17u + frame * 23u};
                dev<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(command, source->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &b.subresourceRange);
            } else {
                VkClearColorValue value{}; const auto xy = motionValue(frame); value.float32[0] = xy[0]; value.float32[1] = xy[1];
                dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command, source->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &b.subresourceRange);
            }
            transition(command, barrier(source->image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
        }
    }
    dm::DispatchEvidence evidence(VkCommandBuffer command, std::uint64_t generation) const {
        dm::DispatchEvidence e; e.command = {bits(command), generation}; e.pipeline = {bits(pipeline), 1};
        e.pipelineLayout = {bits(pipelineLayout), 1}; e.metadataAccepted = true; e.count = 2; e.groups = {1, 1, 1};
        for (std::size_t i = 0; i < e.count; ++i) {
            auto& b = e.bindings[i]; const auto& source = i ? motion : depth; const auto& info = source.info;
            b.setIndex = 0; b.binding = i ? 7 : 5; b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            b.image = {bits(source.image), i + 1}; b.viewType = VK_IMAGE_VIEW_TYPE_2D; b.viewFormat = info.format;
            b.range = {i ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            b.declaredLayout = i ? motionLayout : depthLayout;
            auto& d = b.imageDescription; d.format = info.format; d.width = info.extent.width; d.height = info.extent.height;
            d.depth = info.extent.depth; d.mipLevels = info.mipLevels; d.arrayLayers = info.arrayLayers;
            d.samples = info.samples; d.usage = info.usage; d.tiling = info.tiling; d.sharingMode = info.sharingMode;
        }
        return e;
    }
    bool capture(VkCommandBuffer command, unsigned frame, std::uint64_t generation) {
        const auto e = evidence(command, generation);
        cp::SourcePair pair; pair.depth = e.bindings[0].image; pair.motion = e.bindings[1].image;
        pair.invocation = 100 + frame; pair.thread = GetCurrentThreadId(); pair.valid = true;
        pair.engineFrame = frame;
        pair.frameValid = true;
        pair.timing = {frame + 1, 47, false};
        pair.constants.producerToken = pair.invocation; pair.constants.frameIndex = static_cast<float>(frame);
        pair.constants.jitter = {0.125f * frame, -0.125f * frame};
        pair.constants.renderSize = pair.constants.upscaleSize = {static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height)};
        provider->invocation(pair);
        xess_vk::CommandRecordingFacts facts; facts.command = bits(command); facts.device = bits(device); facts.contextIncarnation = 23;
        facts.rpRecordingGeneration = generation; facts.queueFamily = family; facts.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        facts.beginUsageFlags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; facts.recording = facts.stateKnown = facts.scopeKnown = true;
        return provider->record(pair.thread, facts, e, {frame, pair.timing, 0, true});
    }
    void producer(unsigned frame, bool gated) {
        // The clear is recorded inside this producer, before Provider::record.
        // Once the real timeline gate opens, queue order gives each pending slot
        // its own pattern; the shared borrowed source ends with frame 2.
        const auto command = commands[frame]; begin(command); clear(command, frame);
        trace = {}; trace.depth = depth.image; trace.motion = motion.image;
        require(capture(command, frame, frame + 1), "production Provider record refused owned inputs");
        require(trace.valid && trace.commands == 6 && trace.depthCopies == 1 && trace.convertedCopies == 1 &&
            trace.motionCopies == 1 && trace.depthRestores == 1 && trace.motionRestores == 1, "Provider did not restore borrowed sources or use depth buffer conversion");
        producerCommands += trace.commands; restorationVerified = true;
        dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        dev<PFN_vkCmdDispatch>("vkCmdDispatch")(command, 1, 1, 1);
        check(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command), "end producer");
        require(provider->containsRecording({bits(command), frame + 1}), "actual recording not retained");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &ready[frame];
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        std::uint64_t waitValue = 1, signalValue = 0;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        if (gated) {
            timeline.waitSemaphoreValueCount = timeline.signalSemaphoreValueCount = 1;
            timeline.pWaitSemaphoreValues = &waitValue; timeline.pSignalSemaphoreValues = &signalValue;
            submit.pNext = &timeline; submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &gate; submit.pWaitDstStageMask = &stage;
            gateClosed = true;
        }
        queueUsed = true;
        const auto result = dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, producerFences[frame]);
        provider->submitted({bits(command), frame + 1}, std::span<const VkSemaphore>(&ready[frame], 1), queue, frame + 1, result);
        check(result, "actual producer submit"); ++submissions;
        std::printf("{\"event\":\"continuous_record_submit\",\"frame\":%u,\"recordingGeneration\":%u,\"providerCommands\":%llu,\"gated\":%s}\n", frame, frame + 1,
            static_cast<unsigned long long>(trace.commands), gated ? "true" : "false");
    }
    void checkSaturation() {
        const auto pending = dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device, producerFences[0]);
        const auto waited = dev<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &producerFences[0], VK_TRUE, 5'000'000);
        require(pending == VK_NOT_READY && waited == VK_TIMEOUT, "timeline gate did not hold actual producer fence");
        require(!provider->acquireForPresent(queue, std::span<const VkSemaphore>(&ready[0], 1), presentFence, provider->submittedCutoff(queue)).ready, "unsignaled actual consumer fence admitted a bundle");
        begin(commands[4]); const auto before = trace.commands;
        require(!capture(commands[4], 3, 90) && trace.commands == before, "three submitted slots must reject record without commands");
        check(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands[4]), "end capacity refusal command");
        const auto stats = provider->stats(); require(stats.saturated == 1 &&
            std::all_of(stats.states.begin(), stats.states.end(), [](auto state) { return state == cp::State::Submitted; }), "three submitted slots not retained");
        pendingVerified = saturationVerified = true;
        std::printf("{\"event\":\"continuous_three_slots_pending\",\"actualFenceStatus\":%d,\"finiteWaitResult\":%d,\"saturatedSlots\":3,\"refusedCommandsIssued\":0}\n", int(pending), int(waited));
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore = gate; signal.value = 1;
        check(dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(device, &signal), "open actual timeline gate"); gateClosed = false;
    }
    void readImage(VkCommandBuffer command, VkImage image, VkImageAspectFlags range, VkDeviceSize offset) const {
        VkBufferImageCopy region{}; region.bufferOffset = offset; region.imageSubresource = {range, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
    }
    void verifyPixels(unsigned frame, unsigned sourceFrame) {
        check(dev<PFN_vkMapMemory>("vkMapMemory")(device, readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "map completed readback");
        if (!coherent) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = readbackMemory; range.size = VK_WHOLE_SIZE;
            check(dev<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device, 1, &range), "invalidate completed readback");
        }
        std::uint32_t convertedDepth{}, originalDepth{}; const auto captured = depthValue(frame), original = depthValue(sourceFrame);
        std::memcpy(&convertedDepth, &captured, 4); std::memcpy(&originalDepth, &original, 4);
        const std::array<std::uint32_t, 4> wanted{convertedDepth, motionBits(frame), originalDepth, motionBits(sourceFrame)};
        std::array<std::uint32_t, 4> observed{};
        for (std::size_t plane = 0; plane < observed.size(); ++plane)
            std::memcpy(&observed[plane], static_cast<unsigned char*>(mapped) + plane * pixels * 4, 4);
        for (std::size_t plane = 0; plane < wanted.size(); ++plane) for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            std::uint32_t actual{}; std::memcpy(&actual, static_cast<unsigned char*>(mapped) + plane * pixels * 4 + pixel * 4, 4);
            const bool inject = injectMismatch && frame == 0 && plane == 0 && pixel == pixels / 2;
            const auto expected = inject ? wanted[plane] ^ 1u : wanted[plane];
            if (actual != expected) {
                std::printf("{\"event\":\"continuous_pixel_mismatch\",\"frame\":%u,\"plane\":%zu,\"x\":%zu,\"y\":%zu,\"expectedBits\":%u,\"actualBits\":%u,\"expectedMismatchInjected\":%s}\n",
                    frame, plane, pixel % extent.width, pixel / extent.width, expected, actual, inject ? "true" : "false");
                throw std::runtime_error("actual continuous Provider GPU pixel mismatch");
            }
            ++verifiedPixels;
        }
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            require(static_cast<unsigned char*>(mapped)[pixels * 16 + pixel] == 17 + sourceFrame * 23, "borrowed source stencil changed"); ++verifiedPixels;
        }
        dev<PFN_vkUnmapMemory>("vkUnmapMemory")(device, readbackMemory); mapped = nullptr;
        std::printf("{\"event\":\"continuous_pixels_verified\",\"frame\":%u,\"sourceFrame\":%u,\"convertedDepthFormat\":100,\"motionFormat\":83,\"width\":31,\"height\":19,\"verifiedPixels\":%zu,\"targetDepthBits\":%u,\"targetMotionBits\":%u,\"sourceDepthBits\":%u,\"sourceMotionBits\":%u,\"sourceStencilPreserved\":true}\n",
            frame, sourceFrame, pixels * 5, observed[0], observed[1], observed[2], observed[3]);
    }
    void consume(unsigned frame, unsigned sourceFrame) {
        check(dev<PFN_vkResetFences>("vkResetFences")(device, 1, &presentFence), "reset completed present-equivalent fence");
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo present{VK_STRUCTURE_TYPE_SUBMIT_INFO}; present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &ready[frame]; present.pWaitDstStageMask = &stage;
        // A real semaphore-wait submit/fence substitutes only for WSI present's
        // completion boundary. No swapchain or synthetic completion token.
        const auto cutoff = provider->submittedCutoff(queue);
        check(dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &present, presentFence), "submit present-equivalent wait");
        check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &presentFence, VK_TRUE, timeout), "present-equivalent completion"); ++presentWaits;
        require(dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device, producerFences[frame]) == VK_SUCCESS, "actual producer fence incomplete");
        auto& bundle = bundles[frame];
        bundle = provider->acquireForPresent(queue, std::span<const VkSemaphore>(&ready[frame], 1), presentFence, cutoff);
        require(bundle.ready && bundle.invocation == 100 + frame && bundle.submission == frame + 1 &&
            bundle.constants.producerToken == 100 + frame && bundle.constants.commandBuffer == bits(commands[frame]) &&
            bundle.constants.recordingGeneration == frame + 1 && bundle.constants.deviceIncarnation == 23 &&
            bundle.constants.frameIndex == static_cast<float>(frame), "same submitted invocation provenance lost");
        require(bundle.depth.successfulImageCreateInfo.format == VK_FORMAT_R32_SFLOAT &&
            bundle.motion.successfulImageCreateInfo.format == VK_FORMAT_R16G16_SFLOAT &&
            bundle.depth.currentLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && bundle.motion.currentLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            bundle.depth.image != depth.image && bundle.motion.image != motion.image, "bundle is not owned converted depth/motion");
        require(provider->stats().states[bundle.ticket.slot] == cp::State::Consuming, "acquired slot was released before consumer completion");
        if (frame == 3) {
            require(bundle.ticket.slot == bundles[0].ticket.slot && bundle.ticket.generation > bundles[0].ticket.generation,
                "completed slot must be reacquired with a newer generation");
            provider->releaseAfterBridge(bundles[0].ticket);
            require(provider->stats().states[bundle.ticket.slot] == cp::State::Consuming,
                "stale ticket released a newer capture before its actual readback fence");
        }
        check(dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands[5], 0), "reset completed readback command");
        begin(commands[5]);
        readImage(commands[5], bundle.depth.image, VK_IMAGE_ASPECT_COLOR_BIT, 0);
        readImage(commands[5], bundle.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, pixels * 4);
        transition(commands[5], barrier(depth.image, aspect(depth.info.format), depthLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT));
        transition(commands[5], barrier(motion.image, VK_IMAGE_ASPECT_COLOR_BIT, motionLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT));
        readImage(commands[5], depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, pixels * 8);
        readImage(commands[5], motion.image, VK_IMAGE_ASPECT_COLOR_BIT, pixels * 12);
        readImage(commands[5], depth.image, VK_IMAGE_ASPECT_STENCIL_BIT, pixels * 16);
        transition(commands[5], barrier(depth.image, aspect(depth.info.format), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depthLayout, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT));
        transition(commands[5], barrier(motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, motionLayout, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT));
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.buffer = readback; host.size = VK_WHOLE_SIZE;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands[5], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 1, &host, 0, nullptr);
        check(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands[5]), "end readback");
        check(dev<PFN_vkResetFences>("vkResetFences")(device, 1, &readbackFence), "reset completed readback fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &commands[5];
        check(dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, readbackFence), "submit actual readback");
        check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &readbackFence, VK_TRUE, timeout), "actual GPU readback completion");
        ++consumerReadbacks; verifyPixels(frame, sourceFrame);
        provider->releaseAfterBridge(bundle.ticket);
        require(provider->stats().states[bundle.ticket.slot] == cp::State::Free, "completed consumed slot not released");
        std::printf("{\"event\":\"continuous_consumed\",\"frame\":%u,\"slot\":%u,\"ticketGeneration\":%llu,\"actualReadbackFenceCompleted\":true}\n", frame, bundle.ticket.slot, static_cast<unsigned long long>(bundle.ticket.generation));
    }
    void run() {
        initialize();
        for (unsigned frame = 0; frame < 3; ++frame) producer(frame, true);
        checkSaturation();
        for (unsigned frame = 0; frame < 3; ++frame) consume(frame, 2);
        producer(3, false); consume(3, 3);
        generationAdvanced = bundles[3].ticket.slot == bundles[0].ticket.slot && bundles[3].ticket.generation > bundles[0].ticket.generation;
        require(generationAdvanced, "completed first slot did not reuse with a newer generation");
        const auto stats = provider->stats();
        require(stats.captured == 4 && stats.submissions == 4 && stats.presentMatches == 4 && stats.consumed == 4 && stats.saturated == 1 &&
            std::all_of(stats.states.begin(), stats.states.end(), [](auto state) { return state == cp::State::Free; }), "continuous lifecycle counts/states differ");
    }
    bool cleanup() noexcept {
        if (cleanupDone) return cleanupSucceeded;
        try {
            if (device && gdpa) {
                if (gateClosed) {
                    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore = gate; signal.value = 1;
                    if (dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(device, &signal) != VK_SUCCESS) return false;
                    gateClosed = false;
                }
                if (queueUsed || provider) {
                    if (!drainSubmitted) {
                        VkSubmitInfo drain{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                        if (dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &drain, drainFence) != VK_SUCCESS) return false;
                        drainSubmitted = true;
                    }
                    if (dev<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &drainFence, VK_TRUE, timeout) != VK_SUCCESS) return false;
                    if (provider && provider->stop(drainFence) != VK_SUCCESS) return false;
                }
                if (mapped) { dev<PFN_vkUnmapMemory>("vkUnmapMemory")(device, readbackMemory); mapped = nullptr; }
                provider.reset();
                if (pipeline) dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device, pipeline, nullptr);
                if (shader) dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device, shader, nullptr);
                if (pipelineLayout) dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device, pipelineLayout, nullptr);
                if (readback) dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, readback, nullptr);
                if (readbackMemory) dev<PFN_vkFreeMemory>("vkFreeMemory")(device, readbackMemory, nullptr);
                for (auto* source : {&depth, &motion}) {
                    if (source->image) dev<PFN_vkDestroyImage>("vkDestroyImage")(device, source->image, nullptr);
                    if (source->memory) dev<PFN_vkFreeMemory>("vkFreeMemory")(device, source->memory, nullptr);
                }
                for (auto semaphore : ready) if (semaphore) dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device, semaphore, nullptr);
                if (gate) dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device, gate, nullptr);
                for (auto fence : producerFences) if (fence) dev<PFN_vkDestroyFence>("vkDestroyFence")(device, fence, nullptr);
                for (auto fence : {presentFence, readbackFence, drainFence}) if (fence) dev<PFN_vkDestroyFence>("vkDestroyFence")(device, fence, nullptr);
                if (pool) dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, nullptr);
                dev<PFN_vkDestroyDevice>("vkDestroyDevice")(device, nullptr); device = VK_NULL_HANDLE;
            }
            if (instance) { inst<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr); instance = VK_NULL_HANDLE; }
            if (loader) { FreeLibrary(loader); loader = nullptr; }
            cleanupDone = cleanupSucceeded = true; return true;
        } catch (...) { return false; }
    }
};
void limits() {
    std::cout << ",\"descriptorEvidenceFromOwnedFacts\":true,\"descriptorApiObserved\":false,\"sourceCpuUploaded\":false,\"gameTested\":false,\"gamePixelsCaptured\":false,\"xefgIntegrated\":false,\"motionSemanticsVerified\":false,\"gameSameFrameVerified\":false,\"realWsiPresentTested\":false";
}
}
int main(int argc, char** argv) {
    Watchdog watchdog; Fixture fixture;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--inject-pixel-mismatch") == 0) fixture.injectMismatch = true;
        else { std::cerr << "unknown fixture option\n"; return 2; }
    }
    try {
        fixture.run(); require(fixture.cleanup(), "bounded GPU cleanup failed");
        std::cout << "{\"stage\":\"continuous-provider-gpu\",\"passed\":true,\"depthConverted\":true,\"selfOwnedPixelsCaptured\":true,\"pendingTimelineGateVerified\":" << (fixture.pendingVerified ? "true" : "false")
            << ",\"threeSlotSaturationVerified\":" << (fixture.saturationVerified ? "true" : "false") << ",\"sourceLayoutsRestored\":" << (fixture.restorationVerified ? "true" : "false")
            << ",\"ticketGenerationAdvanced\":" << (fixture.generationAdvanced ? "true" : "false") << ",\"producerSubmissions\":" << fixture.submissions
            << ",\"presentEquivalentWaits\":" << fixture.presentWaits << ",\"completedReadbacks\":" << fixture.consumerReadbacks << ",\"verifiedPixels\":" << fixture.verifiedPixels
            << ",\"providerCommands\":" << fixture.producerCommands << ",\"formats\":[130,100,83],\"cleanupSucceeded\":true";
        limits(); std::cout << "}\n"; return 0;
    } catch (const std::exception& error) {
        const auto clean = fixture.cleanup(); std::cerr << error.what() << '\n';
        std::cout << "{\"stage\":\"continuous-provider-gpu\",\"passed\":false,\"depthConverted\":false,\"selfOwnedPixelsCaptured\":false,\"cleanupSucceeded\":" << (clean ? "true" : "false");
        limits(); std::cout << "}\n"; return 1;
    }
}
