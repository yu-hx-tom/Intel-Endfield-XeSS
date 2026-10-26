#include "interop.hpp"
#include "fence_wait.hpp"
#include "interop_gpu_timestamps.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace xess_vk {
namespace {
void check(HRESULT hr, const char* operation) {
    if(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET||hr==DXGI_ERROR_DEVICE_HUNG)
        measurement::increment(measurement::Counter::DeviceLost);
    if (FAILED(hr)) {
        std::ostringstream s; s << operation << " failed: HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
        throw std::runtime_error(s.str());
    }
}
void checkVk(VkResult result, const char* operation) {
    if(result==VK_ERROR_DEVICE_LOST)measurement::increment(measurement::Counter::DeviceLost);
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(result));
}
std::string utf8(const wchar_t* s) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, result.data(), count, nullptr, nullptr);
    result.pop_back(); return result;
}
template<class T> void release(T*& value) { if (value) { value->Release(); value = nullptr; } }
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
constexpr std::array<VkFormat, 3> vkFormats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT};
constexpr std::array<DXGI_FORMAT, 3> dxFormats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT};
constexpr std::array<const char*, 3> names{"RGBA8", "R32F", "RG16F"};
}

void InteropConfig::validate() const {
    if (!width || !height || width > 16384 || height > 16384 || !timeoutMs || timeoutMs > 60000)
        throw std::invalid_argument("width/height must be 1..16384 and timeoutMs 1..60000");
    if ((inputWidth == 0) != (inputHeight == 0) || inputWidth > 16384 || inputHeight > 16384)
        throw std::invalid_argument("inputWidth/inputHeight must both be zero or both be 1..16384");
}
VkExtent2D InteropConfig::extent(Format format) const {
    validate();
    switch (format) {
    case Format::ColorRGBA8: return {width, height};
    case Format::DepthR32F:
    case Format::MotionRG16F: return inputWidth ? VkExtent2D{inputWidth, inputHeight} : VkExtent2D{width, height};
    default: throw std::invalid_argument("Unknown interop plane format");
    }
}
std::size_t InteropConfig::byteSize(Format format) const {
    const auto size = extent(format);
    return static_cast<std::size_t>(size.width) * size.height * 4;
}
void detail::validateNativeFrameMetadata(const InteropConfig& config, const NativeFrame& frame,
                                        VkDevice expectedDevice, VkQueue expectedQueue, uint32_t expectedQueueFamily) {
    config.validate();
    if (!expectedDevice || !expectedQueue || expectedQueueFamily >= VK_QUEUE_FAMILY_FOREIGN_EXT)
        throw std::invalid_argument("Native validation requires a known device, queue and concrete queue family");
    if (frame.producerQueue != expectedQueue || !frame.producerCompleteFence)
        throw std::invalid_argument("Native frame requires the borrowed graphics queue and its actual producer completion fence");
    for (unsigned i = 0; i < (config.colorOnly ? 1u : frame.planes.size()); ++i) {
        const auto& source = frame.planes[i]; const auto& info = source.successfulImageCreateInfo;
        const auto size = config.extent(static_cast<Format>(i));
        if (source.device != expectedDevice || !source.image || source.ownerQueueFamily != expectedQueueFamily)
            throw std::invalid_argument("Native source device/image/queue-family ownership does not match borrowed context");
        if (info.sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || info.pNext || info.flags || info.imageType != VK_IMAGE_TYPE_2D ||
            info.format != vkFormats[i] || info.extent.width != size.width || info.extent.height != size.height || info.extent.depth != 1 ||
            info.mipLevels != 1 || info.arrayLayers != 1 || info.samples != VK_SAMPLE_COUNT_1_BIT || info.tiling != VK_IMAGE_TILING_OPTIMAL ||
            info.sharingMode != VK_SHARING_MODE_EXCLUSIVE || !(info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            throw std::invalid_argument("Native source must match fixed per-plane format/extent, single mip/layer/sample, optimal exclusive TRANSFER_SRC contract");
        switch (source.currentLayout) {
        case VK_IMAGE_LAYOUT_GENERAL:
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            if (!(info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) throw std::invalid_argument("TRANSFER_DST layout requires TRANSFER_DST usage");
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            if (!(info.usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))) throw std::invalid_argument("Shader-read layout requires sampled/input-attachment usage");
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            if (!(info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) throw std::invalid_argument("Color-attachment layout requires color-attachment usage");
            break;
        default: throw std::invalid_argument("Native source has undefined, preinitialized, present, depth, or unsupported layout");
        }
        for (unsigned j = 0; j < i; ++j) if (source.image == frame.planes[j].image)
            throw std::invalid_argument("Native source planes must be distinct images");
    }
}

struct InteropBridge::Impl {
    InteropConfig cfg;
    TransferStats stats;
    HMODULE loader = nullptr;
    bool borrowedVulkan = false;
    PFN_vkGetInstanceProcAddr get = nullptr;
    PFN_vkGetDeviceProcAddr getDevice = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue vkQueue = VK_NULL_HANDLE;
    QueueGate* queueGate = nullptr;
    wsi_acquire::QueueObserver queueObserver = nullptr;
    void* queueObserverContext = nullptr;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkSemaphore producedSemaphore = VK_NULL_HANDLE;
    VkSemaphore consumedSemaphore = VK_NULL_HANDLE;
    ID3D12Device* d3d = nullptr;
    ID3D12CommandQueue* d3dQueue = nullptr;
    ID3D12Fence* produced = nullptr;
    ID3D12Fence* consumed = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    HANDLE event = nullptr;
    uint64_t value = 0;
    bool d3dWaitQueued = false;
    bool d3dSignalQueued = true;
    bool workOutstanding = false;
    struct Plane {
        ID3D12Resource* texture = nullptr;
        ID3D12Resource* readback = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        uint64_t readbackSize = 0;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory imageMemory = VK_NULL_HANDLE;
        VkImageView storageView = VK_NULL_HANDLE;
        VkImageUsageFlags usage = 0;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        bool coherent = false;
        bool initialized = false;
        std::uint32_t ownerFamily = VK_QUEUE_FAMILY_EXTERNAL;
        std::uint64_t allocationBytes = 0;
    };
    std::array<Plane, 3> planes{};
    enum class SharedState { Free, Reserved, Submitted, Published };
    struct SharedSlot {
        std::array<Plane, 3> planes{};
        SharedPlaneKey key;
        SharedState state = SharedState::Free;
        std::uint32_t writes = 0;
        bool auxiliary = false, publicationAuxiliary = false, direct = false, poisoned = false;
        FrameSlotKey frameKey;
        std::uint64_t producedValue = 0, consumedValue = 0;
    };
    struct SharedRing {
        InteropConfig config;
        std::uint64_t generation = 0;
        bool ready = false;
        bool depthStorage = false;
        std::array<SharedSlot, sharedPlaneSlotCount> slots;
    };
    mutable std::mutex sharedMutex;
    std::unique_ptr<SharedRing> sharedRing;
    SharedPlaneStats sharedStats;
    std::uint64_t sharedRingSerial = 0, sharedLeaseSerial = 0;
    detail::InteropGpuTimestamps gpuTimestamps;
    GpuTimestampSample gpuTimestampFrame;
    BorrowedVulkanContext timestampContext;
    struct CommandSlot {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        detail::InteropGpuTimestamps timestamps;
        FrameCompletion receipt;
    };
    std::array<CommandSlot, frameSlotCount> commandSlots;
    std::uint64_t timestampSequence = 0;
    std::uint64_t timestampResourceGeneration = 1;
    GpuTimestampSample latestGpuTimestamp;
    std::atomic<bool> uncertainSubmission = false;

    template<class T> T vk(const char* name) const {
        const auto result = vkDevice ? getDevice(vkDevice, name) : get(instance, name);
        if (!result) throw std::runtime_error(std::string("Vulkan function unavailable: ") + name);
        return reinterpret_cast<T>(result);
    }
    template<class T> T instanceVk(const char* name) const {
        const auto result = get(instance, name);
        if (!result) throw std::runtime_error(std::string("Vulkan instance function unavailable: ") + name);
        return reinterpret_cast<T>(result);
    }
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) const {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & (required | preferred)) == (required | preferred)) return i;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & required) == required) return i;
        throw std::runtime_error("No compatible Vulkan memory type");
    }
    void validateConfig() const { cfg.validate(); }
    void initialize() {
        validateConfig();
        loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) throw std::runtime_error("Cannot load system Vulkan loader");
        get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        if (!get) throw std::runtime_error("Cannot resolve vkGetInstanceProcAddr");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Endfield XeSS Vulkan shared resource experiment"; app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
        checkVk(vk<PFN_vkCreateInstance>("vkCreateInstance")(&ci, nullptr, &instance), "vkCreateInstance");
        uint32_t count = 0;
        checkVk(vk<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        checkVk(vk<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        IDXGIFactory4* factory = nullptr;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        try {
            for (const auto candidate : devices) {
                VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
                vk<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(candidate, &props);
                if (props.properties.vendorID != 0x8086 || !id.deviceLUIDValid) continue;
                LUID luid{}; std::memcpy(&luid, id.deviceLUID, sizeof(luid));
                IDXGIAdapter1* adapter = nullptr;
                if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) continue;
                const auto hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d));
                DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc); release(adapter);
                if (SUCCEEDED(hr)) { physical = candidate; stats.adapter = utf8(desc.Description); break; }
            }
        } catch (...) { release(factory); throw; }
        release(factory);
        if (!physical) throw std::runtime_error("No same-LUID Intel Vulkan/D3D12 adapter supporting feature level 12_0");
        uint32_t extCount = 0;
        checkVk(vk<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties")(physical, nullptr, &extCount, nullptr), "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> extensions(extCount);
        checkVk(vk<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties")(physical, nullptr, &extCount, extensions.data()), "vkEnumerateDeviceExtensionProperties");
        const char* enabled[]{VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
        for (const char* required : enabled)
            if (std::none_of(extensions.begin(), extensions.end(), [&](const auto& e) { return std::strcmp(e.extensionName, required) == 0; }))
                throw std::runtime_error(std::string("Required Vulkan device extension missing: ") + required);
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline};
        vk<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical, &features);
        if (!timeline.timelineSemaphore) throw std::runtime_error("Vulkan timeline semaphores unsupported");
        uint32_t familyCount = 0;
        vk<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vk<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical, &familyCount, families.data());
        auto queueFamily = std::find_if(families.begin(), families.end(), [](const auto& f) { return f.queueCount && (f.queueFlags & VK_QUEUE_GRAPHICS_BIT); });
        if (queueFamily == families.end()) throw std::runtime_error("No Vulkan graphics queue");
        family = static_cast<uint32_t>(queueFamily - families.begin());
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qc.queueFamilyIndex = family; qc.queueCount = 1; qc.pQueuePriorities = &priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &timeline}; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qc;
        dc.enabledExtensionCount = 2; dc.ppEnabledExtensionNames = enabled;
        checkVk(vk<PFN_vkCreateDevice>("vkCreateDevice")(physical, &dc, nullptr, &vkDevice), "vkCreateDevice");
        getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get(instance, "vkGetDeviceProcAddr"));
        vk<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(vkDevice, family, 0, &vkQueue);
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get(instance, "vkGetPhysicalDeviceMemoryProperties"))(physical, &memory);
        initializeSharedResources();
    }
    void initializeBorrowed(const BorrowedVulkanContext& context) {
        queueGate = context.queueGate;
        queueObserver = context.queueObserver; queueObserverContext = context.queueObserverContext;
        validateConfig();
        if (!context.instance || !context.physicalDevice || !context.device || !context.graphicsQueue ||
            !context.getInstanceProcAddr || !context.getDeviceProcAddr ||
            !context.successfulInstanceCreateInfo || !context.successfulDeviceCreateInfo)
            throw std::invalid_argument("Borrowed context requires handles, dispatch and actual successful creation records");
        const auto& instanceInfo = *context.successfulInstanceCreateInfo;
        const auto& deviceInfo = *context.successfulDeviceCreateInfo;
        if (instanceInfo.sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO || !instanceInfo.pApplicationInfo ||
            instanceInfo.pApplicationInfo->apiVersion < VK_API_VERSION_1_2 || deviceInfo.sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO)
            throw std::invalid_argument("Borrowed context requires an actual Vulkan 1.2+ instance/device creation record");
        if (deviceInfo.flags || deviceInfo.enabledExtensionCount == 0 || !deviceInfo.ppEnabledExtensionNames)
            throw std::invalid_argument("Borrowed device creation record has no enabled external extensions or unsupported flags");
        for (const char* required : {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME}) {
            bool enabled = false;
            for (uint32_t i = 0; i < deviceInfo.enabledExtensionCount; ++i)
                if (deviceInfo.ppEnabledExtensionNames[i] && std::strcmp(required, deviceInfo.ppEnabledExtensionNames[i]) == 0) enabled = true;
            if (!enabled) throw std::invalid_argument(std::string("Borrowed device did not enable required extension: ") + required);
        }
        bool timelineEnabled = false;
        const auto* node = reinterpret_cast<const VkBaseInStructure*>(deviceInfo.pNext);
        for (uint32_t depth = 0; node && depth < 256; ++depth, node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
                timelineEnabled = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(node)->timelineSemaphore == VK_TRUE;
            else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
                timelineEnabled = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node)->timelineSemaphore == VK_TRUE;
            else if (node->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO)
                throw std::invalid_argument("Borrowed device groups are outside the single-device contract");
        }
        if (node || !timelineEnabled) throw std::invalid_argument("Borrowed device creation record must enable timelineSemaphore");
        bool queueEnabled = false;
        if (deviceInfo.queueCreateInfoCount && !deviceInfo.pQueueCreateInfos) throw std::invalid_argument("Missing device queue creation records");
        for (uint32_t i = 0; i < deviceInfo.queueCreateInfoCount; ++i) {
            const auto& queueInfo = deviceInfo.pQueueCreateInfos[i];
            if (queueInfo.queueFamilyIndex == context.queueFamilyIndex && context.queueIndex < queueInfo.queueCount && queueInfo.flags == 0)
                queueEnabled = true;
        }
        if (!queueEnabled) throw std::invalid_argument("Borrowed graphics queue is absent from device creation record or protected");

        // Ownership mode is set before storing any caller handles, including on
        // constructor failure. Their lifetime always remains with the caller.
        borrowedVulkan = true;
        instance = context.instance; physical = context.physicalDevice; vkDevice = context.device;
        vkQueue = context.graphicsQueue; family = context.queueFamilyIndex;
        get = context.getInstanceProcAddr; getDevice = context.getDeviceProcAddr;
        uint32_t familyCount = 0;
        instanceVk<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        instanceVk<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical, &familyCount, families.data());
        if (family >= families.size() || context.queueIndex >= families[family].queueCount || !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            throw std::invalid_argument("Borrowed queue family is not graphics capable");
        VkQueue actualQueue = VK_NULL_HANDLE;
        vk<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(vkDevice, family, context.queueIndex, &actualQueue);
        if (actualQueue != vkQueue) throw std::invalid_argument("Borrowed queue does not match the given device/family/index");
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        instanceVk<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(physical, &properties);
        if (properties.properties.vendorID != 0x8086 || !id.deviceLUIDValid || properties.properties.apiVersion < VK_API_VERSION_1_2)
            throw std::invalid_argument("Borrowed physical device must be Intel Vulkan 1.2+ with a valid adapter LUID");
        LUID luid{}; std::memcpy(&luid, id.deviceLUID, sizeof(luid));
        IDXGIFactory4* factory = nullptr; IDXGIAdapter1* adapter = nullptr;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1(borrowed)");
        try {
            check(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid(borrowed)");
            check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d)), "D3D12CreateDevice(borrowed)");
            const LUID actualLuid = d3d->GetAdapterLuid();
            if (std::memcmp(&actualLuid, &luid, sizeof(luid))) throw std::runtime_error("D3D12 device LUID does not match borrowed Vulkan physical device");
            DXGI_ADAPTER_DESC1 desc{}; check(adapter->GetDesc1(&desc), "GetDesc1(borrowed)"); stats.adapter = utf8(desc.Description);
        } catch (...) { release(adapter); release(factory); throw; }
        release(adapter); release(factory);

        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkPhysicalDeviceExternalSemaphoreInfo fenceInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &type};
        fenceInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        VkExternalSemaphoreProperties fenceProps{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        instanceVk<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>("vkGetPhysicalDeviceExternalSemaphoreProperties")(physical, &fenceInfo, &fenceProps);
        if (!(fenceProps.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) ||
            !(fenceProps.compatibleHandleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT))
            throw std::runtime_error("Borrowed physical device cannot import timeline D3D12 fences");
        for (unsigned i = 0; i < (cfg.colorOnly ? 1u : vkFormats.size()); ++i) {
            const auto size = cfg.extent(static_cast<Format>(i));
            validateExternalPlane(i, size);
        }
        // Confirm dispatch entry points as well as recorded enablement before any import.
        (void)vk<PFN_vkImportSemaphoreWin32HandleKHR>("vkImportSemaphoreWin32HandleKHR");
        (void)vk<PFN_vkGetMemoryWin32HandlePropertiesKHR>("vkGetMemoryWin32HandlePropertiesKHR");
        instanceVk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
        initializeSharedResources();
        gpuTimestamps.initialize(context);
        timestampContext = context;
        latestGpuTimestamp.requested = context.gpuTimestamps;
    }
    bool externalPlaneSupported(unsigned index, VkExtent2D size, VkImageUsageFlags extraUsage = 0) const {
            VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
            external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
            VkPhysicalDeviceImageFormatInfo2 imageInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &external};
            imageInfo.format = vkFormats[index]; imageInfo.type = VK_IMAGE_TYPE_2D; imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | extraUsage;
            VkExternalImageFormatProperties externalProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
            VkImageFormatProperties2 imageProps{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &externalProps};
            const auto result = instanceVk<PFN_vkGetPhysicalDeviceImageFormatProperties2>("vkGetPhysicalDeviceImageFormatProperties2")(physical, &imageInfo, &imageProps);
            if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) return false;
            checkVk(result, "vkGetPhysicalDeviceImageFormatProperties2(borrowed)");
            return (externalProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
                (externalProps.externalMemoryProperties.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT) &&
                imageProps.imageFormatProperties.maxExtent.width >= size.width && imageProps.imageFormatProperties.maxExtent.height >= size.height;
    }
    void validateExternalPlane(unsigned index, VkExtent2D size) const {
        if (!externalPlaneSupported(index, size))
            throw std::runtime_error("Borrowed physical device cannot import required D3D12 texture format/extent");
    }
    bool sharedDepthStorageSupported(VkExtent2D size) const {
        VkFormatProperties properties{};
        instanceVk<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical, vkFormats[1], &properties);
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return false;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{dxFormats[1]};
        const auto result = d3d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
        if (result == E_INVALIDARG || result == DXGI_ERROR_UNSUPPORTED) return false;
        check(result, "CheckFeatureSupport(shared depth UAV)");
        return (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
            (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) &&
            externalPlaneSupported(1, size, VK_IMAGE_USAGE_STORAGE_BIT);
    }
    void initializeSharedResources() {
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(d3d->CreateCommandQueue(&qd, IID_PPV_ARGS(&d3dQueue)), "CreateCommandQueue");
        check(d3d->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&produced)), "CreateFence(produced)");
        check(d3d->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&consumed)), "CreateFence(consumed)");
        producedSemaphore = importFence(produced); consumedSemaphore = importFence(consumed);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("CreateEvent failed");
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pc.queueFamilyIndex = family; pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        checkVk(vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(vkDevice, &pc, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        checkVk(vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(vkDevice, &ca, &commands), "vkAllocateCommandBuffers");
        check(d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        check(d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
        check(list->Close(), "Close(initial command list)");
        for (unsigned i = 0; i < (cfg.colorOnly ? 1u : planes.size()); ++i) createPlane(i);
    }
    VkSemaphore importFence(ID3D12Fence* fence) {
        Handle shared;
        check(d3d->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &shared.value), "CreateSharedHandle(fence)");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        VkSemaphore semaphore = VK_NULL_HANDLE;
        checkVk(vk<PFN_vkCreateSemaphore>("vkCreateSemaphore")(vkDevice, &sc, nullptr, &semaphore), "vkCreateSemaphore(timeline)");
        VkImportSemaphoreWin32HandleInfoKHR ic{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        ic.semaphore = semaphore; ic.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT; ic.handle = shared.value;
        const auto result = vk<PFN_vkImportSemaphoreWin32HandleKHR>("vkImportSemaphoreWin32HandleKHR")(vkDevice, &ic);
        if (result != VK_SUCCESS) { vk<PFN_vkDestroySemaphore>("vkDestroySemaphore")(vkDevice, semaphore, nullptr); checkVk(result, "vkImportSemaphoreWin32HandleKHR"); }
        return semaphore; // NT handle ownership remains with us; import keeps its own reference.
    }
    void createPlane(unsigned index) {
        createPlane(index, planes[index], cfg);
    }
    void createPlane(unsigned index, Plane& p, const InteropConfig& planeConfig,
                     bool verificationBuffers = true, bool depthStorage = false) {
        const bool storage = depthStorage && index == 1;
        const auto size = planeConfig.extent(static_cast<Format>(index));
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = size.width; rd.Height = size.height; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = dxFormats[index];
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (storage) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p.texture)), "CreateCommittedResource(shared texture)");
        Handle shared;
        check(d3d->CreateSharedHandle(p.texture, nullptr, GENERIC_ALL, nullptr, &shared.value), "CreateSharedHandle(texture)");
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external}; image.imageType = VK_IMAGE_TYPE_2D;
        image.format = vkFormats[index]; image.extent = {size.width, size.height, 1}; image.mipLevels = 1; image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (storage) image.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        p.usage = image.usage;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE; image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        checkVk(vk<PFN_vkCreateImage>("vkCreateImage")(vkDevice, &image, nullptr, &p.image), "vkCreateImage(external)");
        VkMemoryRequirements req{};
        vk<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(vkDevice, p.image, &req);
        VkMemoryWin32HandlePropertiesKHR handleProps{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        checkVk(vk<PFN_vkGetMemoryWin32HandlePropertiesKHR>("vkGetMemoryWin32HandlePropertiesKHR")(vkDevice, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, shared.value, &handleProps), "vkGetMemoryWin32HandlePropertiesKHR");
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicated.image = p.image;
        VkImportMemoryWin32HandleInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated};
        imported.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT; imported.handle = shared.value;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imported};
        allocation.allocationSize = req.size; allocation.memoryTypeIndex = memoryType(req.memoryTypeBits & handleProps.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checkVk(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(vkDevice, &allocation, nullptr, &p.imageMemory), "vkAllocateMemory(import texture)");
        p.allocationBytes = allocation.allocationSize;
        checkVk(vk<PFN_vkBindImageMemory>("vkBindImageMemory")(vkDevice, p.image, p.imageMemory, 0), "vkBindImageMemory");
        if (storage) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = p.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = vkFormats[index];
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            checkVk(vk<PFN_vkCreateImageView>("vkCreateImageView")(vkDevice, &view, nullptr, &p.storageView),
                    "vkCreateImageView(shared storage depth)");
        }
        if (!verificationBuffers) return;
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.size = planeConfig.byteSize(static_cast<Format>(index));
        bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVk(vk<PFN_vkCreateBuffer>("vkCreateBuffer")(vkDevice, &bc, nullptr, &p.staging), "vkCreateBuffer(staging)");
        vk<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(vkDevice, p.staging, &req);
        VkMemoryAllocateInfo staging{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; staging.allocationSize = req.size;
        staging.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        p.coherent = (memory.memoryTypes[staging.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        checkVk(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(vkDevice, &staging, nullptr, &p.stagingMemory), "vkAllocateMemory(staging)");
        checkVk(vk<PFN_vkBindBufferMemory>("vkBindBufferMemory")(vkDevice, p.staging, p.stagingMemory, 0), "vkBindBufferMemory");
        checkVk(vk<PFN_vkMapMemory>("vkMapMemory")(vkDevice, p.stagingMemory, 0, VK_WHOLE_SIZE, 0, &p.mapped), "vkMapMemory(staging)");
        d3d->GetCopyableFootprints(&rd, 0, 1, 0, &p.footprint, nullptr, nullptr, &p.readbackSize);
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rb{}; rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rb.Width = p.readbackSize;
        rb.Height = 1; rb.DepthOrArraySize = 1; rb.MipLevels = 1; rb.SampleDesc.Count = 1; rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.readback)), "CreateCommittedResource(readback)");
    }
    void destroyPlane(Plane& p) {
        if (p.mapped) vk<PFN_vkUnmapMemory>("vkUnmapMemory")(vkDevice, p.stagingMemory);
        if (p.staging) vk<PFN_vkDestroyBuffer>("vkDestroyBuffer")(vkDevice, p.staging, nullptr);
        if (p.stagingMemory) vk<PFN_vkFreeMemory>("vkFreeMemory")(vkDevice, p.stagingMemory, nullptr);
        if (p.storageView) vk<PFN_vkDestroyImageView>("vkDestroyImageView")(vkDevice, p.storageView, nullptr);
        if (p.image) vk<PFN_vkDestroyImage>("vkDestroyImage")(vkDevice, p.image, nullptr);
        if (p.imageMemory) vk<PFN_vkFreeMemory>("vkFreeMemory")(vkDevice, p.imageMemory, nullptr);
        release(p.readback); release(p.texture); p = {};
    }
    SharedSlot* sharedSlot(SharedPlaneKey key) const noexcept {
        if (!sharedRing || !key.valid() || key.ringGeneration != sharedRing->generation) return nullptr;
        auto& slot = sharedRing->slots[key.index];
        return slot.key == key && slot.state != SharedState::Free ? &slot : nullptr;
    }
    bool sharedLeases() const noexcept {
        return sharedRing && std::any_of(sharedRing->slots.begin(), sharedRing->slots.end(),
            [](const auto& slot) { return slot.state != SharedState::Free || slot.poisoned; });
    }
    void pauseSharedRing() {
        std::lock_guard lock(sharedMutex);
        if (sharedRing) sharedRing->ready = false;
    }
    NativeImageSource sharedSource(const Plane& plane, unsigned index, const InteropConfig& config) const {
        NativeImageSource result;
        result.device = vkDevice; result.image = plane.image; result.ownerQueueFamily = plane.ownerFamily;
        result.currentLayout = plane.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        auto& info = result.successfulImageCreateInfo;
        info.imageType = VK_IMAGE_TYPE_2D; info.format = vkFormats[index];
        const auto extent = config.extent(static_cast<Format>(index)); info.extent = {extent.width, extent.height, 1};
        info.mipLevels = info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.usage = plane.usage;
        return result;
    }
    bool prepareSharedPlanes(std::uint32_t width, std::uint32_t height) {
        if (!borrowedVulkan || uncertainSubmission) throw std::logic_error("Shared planes require a healthy borrowed bridge");
        auto wanted = cfg; wanted.inputWidth = width; wanted.inputHeight = height; wanted.colorOnly = !width && !height;
        wanted.validate();
        std::lock_guard lock(sharedMutex);
        if (sharedRing) {
            const auto& old = sharedRing->config;
            if (old.width == wanted.width && old.height == wanted.height && old.inputWidth == width &&
                old.inputHeight == height && old.colorOnly == wanted.colorOnly) {
                sharedRing->ready = true;
                return true;
            }
            sharedRing->ready = false;
            if (sharedLeases()) return false;
        }
        if (sharedRingSerial == UINT64_MAX) throw std::overflow_error("Shared plane ring generation exhausted");
        const unsigned count = wanted.colorOnly ? 1u : 3u;
        for (unsigned i = 0; i < count; ++i) validateExternalPlane(i, wanted.extent(static_cast<Format>(i)));
        auto replacement = std::make_unique<SharedRing>();
        replacement->config = wanted; replacement->generation = sharedRingSerial + 1;
        replacement->depthStorage = !wanted.colorOnly && sharedDepthStorageSupported(wanted.extent(Format::DepthR32F));
        std::uint64_t bytes = 0;
        try {
            for (auto& slot : replacement->slots) for (unsigned i = 0; i < count; ++i) {
                createPlane(i, slot.planes[i], wanted, false, replacement->depthStorage);
                bytes += slot.planes[i].allocationBytes;
            }
        } catch (...) {
            for (auto& slot : replacement->slots) for (auto& plane : slot.planes) destroyPlane(plane);
            throw;
        }
        std::swap(sharedRing, replacement);
        if (replacement) for (auto& slot : replacement->slots) for (auto& plane : slot.planes) destroyPlane(plane);
        sharedRingSerial = sharedRing->generation; sharedRing->ready = true;
        sharedStats.ringGeneration = sharedRingSerial; sharedStats.resourceBytes = bytes;
        sharedStats.depthStorage = sharedRing->depthStorage;
        return true;
    }
    SharedBorrowResult tryReserveSharedFrame(VkExtent2D expected, bool auxiliary, SharedPlaneLease& out) noexcept {
        out = {};
        try {
            std::unique_lock lock(sharedMutex, std::try_to_lock);
            if (!lock.owns_lock()) return SharedBorrowResult::Unavailable;
            if (!borrowedVulkan || uncertainSubmission || !consumed) return SharedBorrowResult::Invalid;
            const auto done = consumed->GetCompletedValue();
            if (done == UINT64_MAX) return SharedBorrowResult::DeviceLost;
            if (!sharedRing || !sharedRing->ready || (auxiliary && (sharedRing->config.colorOnly ||
                expected.width != sharedRing->config.extent(Format::DepthR32F).width ||
                expected.height != sharedRing->config.extent(Format::DepthR32F).height))) {
                ++sharedStats.extentFallbacks; return SharedBorrowResult::Unavailable;
            }
            if (sharedLeaseSerial == UINT64_MAX) return SharedBorrowResult::Invalid;
            for (unsigned i = 0; i < sharedRing->slots.size(); ++i) {
                auto& slot = sharedRing->slots[i];
                if (slot.state != SharedState::Free || slot.poisoned || done < slot.consumedValue) continue;
                slot.key = {i, sharedRing->generation, ++sharedLeaseSerial};
                slot.state = SharedState::Reserved; slot.writes = 0; slot.auxiliary = auxiliary; slot.direct = false;
                slot.frameKey = {}; slot.producedValue = 0;
                out.key = slot.key; out.auxiliary = auxiliary;
                out.depthStorage = auxiliary && sharedRing->depthStorage;
                if (out.depthStorage) out.depthStorageView = slot.planes[1].storageView;
                for (unsigned p = 0; p < (sharedRing->config.colorOnly ? 1u : 3u); ++p) {
                    out.planes[p] = sharedSource(slot.planes[p], p, sharedRing->config);
                    if (slot.planes[p].initialized) out.initializedMask |= 1u << p;
                }
                ++sharedStats.reservations; return SharedBorrowResult::Acquired;
            }
            ++sharedStats.noFreeFallbacks; return SharedBorrowResult::Unavailable;
        } catch (...) { return SharedBorrowResult::Invalid; }
    }
    void sharedWriteSubmitted(SharedPlaneKey key, std::uint32_t mask) {
        std::lock_guard lock(sharedMutex);
        auto* slot = sharedSlot(key);
        if (!slot || slot->poisoned || (slot->state != SharedState::Reserved && slot->state != SharedState::Submitted) ||
            !mask || (mask & ~(slot->auxiliary ? 7u : 1u)) || (mask & slot->writes))
            throw std::logic_error("Shared write does not identify a fresh reserved plane");
        slot->writes |= mask; slot->state = SharedState::Submitted;
        for (unsigned i = 0; i < 3; ++i) if (mask & (1u << i)) {
            slot->planes[i].initialized = true; slot->planes[i].ownerFamily = family;
        }
    }
    bool cancelRecordedSharedFrame(SharedPlaneKey key) noexcept {
        try {
            std::lock_guard lock(sharedMutex); auto* slot = sharedSlot(key);
            if (!slot || slot->poisoned || slot->state != SharedState::Reserved || slot->writes) return false;
            slot->state = SharedState::Free; ++sharedStats.cancelled; return true;
        } catch (...) { return false; }
    }
    bool abandonSubmittedSharedFrame(SharedPlaneKey key, VkFence fence) noexcept {
        try {
            std::lock_guard lock(sharedMutex); auto* slot = sharedSlot(key);
            if (!slot || slot->poisoned || slot->state != SharedState::Submitted || !fence) return false;
            const auto result = vk<PFN_vkGetFenceStatus>("vkGetFenceStatus")(vkDevice, fence);
            if (result == VK_NOT_READY) return false;
            if (result != VK_SUCCESS) { slot->poisoned = true; ++sharedStats.poisoned; return false; }
            slot->state = SharedState::Free; ++sharedStats.abandoned; return true;
        } catch (...) { return false; }
    }
    bool releaseSharedFrame(SharedPlaneKey key, const FrameCompletion& receipt) noexcept {
        try {
            std::lock_guard lock(sharedMutex); auto* slot = sharedSlot(key);
            if (!slot || slot->poisoned || slot->state != SharedState::Published || receipt.poisoned ||
                receipt.key != slot->frameKey || !receipt.producedValid || receipt.producedValue != slot->producedValue ||
                !receipt.consumedValid || receipt.consumedValue != slot->consumedValue ||
                !receipt.sdkTailValid || !receipt.sdkTailValue || receipt.sdkTailValue == UINT64_MAX ||
                !receipt.d3dWaitQueued || !receipt.callerSubmitted ||
                (slot->direct ? (receipt.sharedKey != key || receipt.sharedAuxiliary != slot->publicationAuxiliary ||
                                !receipt.postPresentSubmitted) :
                    (receipt.sharedKey != SharedPlaneKey{} || receipt.sharedAuxiliary || receipt.postPresentSubmitted)) ||
                consumedComplete(receipt.consumedValue) != FrameCompletionState::Complete) return false;
            slot->state = SharedState::Free; ++sharedStats.released; return true;
        } catch (...) { return false; }
    }
    void poisonSharedFrame(SharedPlaneKey key) noexcept {
        try {
            std::lock_guard lock(sharedMutex);
            if (auto* slot = sharedSlot(key); slot && !slot->poisoned) {
                slot->poisoned = true; ++sharedStats.poisoned;
            }
        } catch (...) {}
    }
    SharedPlaneStats sharedPlaneStats() const noexcept {
        try {
            std::unique_lock lock(sharedMutex, std::try_to_lock);
            if (!lock.owns_lock()) return {};
            auto result = sharedStats; result.available = true;
            result.ready = sharedRing && sharedRing->ready;
            if (sharedRing) for (const auto& slot : sharedRing->slots) result.leases += slot.state != SharedState::Free;
            return result;
        } catch (...) { return {}; }
    }
    void enableAuxiliaryInputs(std::uint32_t width, std::uint32_t height) {
        if (!width || !height) throw std::invalid_argument("Auxiliary input extent must be nonzero");
        auto wanted = cfg; wanted.inputWidth = width; wanted.inputHeight = height; wanted.colorOnly = false;
        wanted.validate();
        const auto current = cfg.extent(Format::MotionRG16F);
        if (planes[1].image && planes[2].image && current.width == width && current.height == height) return;
        pauseSharedRing();
        validateExternalPlane(1, {width, height}); validateExternalPlane(2, {width, height});
        waitConsumed();
        std::array<Plane, 2> replacements{};
        try {
            createPlane(1, replacements[0], wanted); createPlane(2, replacements[1], wanted);
        } catch (...) {
            for (auto& plane : replacements) destroyPlane(plane);
            throw;
        }
        for (unsigned i = 0; i < replacements.size(); ++i) {
            std::swap(planes[i + 1], replacements[i]); destroyPlane(replacements[i]);
        }
        cfg = wanted;
        gpuTimestamps.rebuilt();
        for (auto& slot : commandSlots) slot.timestamps.rebuilt();
        if (timestampResourceGeneration != UINT64_MAX) ++timestampResourceGeneration;
    }
    void resizeOutput(std::uint32_t width, std::uint32_t height) {
        if (cfg.width == width && cfg.height == height) return;
        pauseSharedRing();
        auto wanted = cfg;
        wanted.width = width;
        wanted.height = height;
        wanted.inputWidth = wanted.inputHeight = 0;
        wanted.colorOnly = true;
        wanted.validate();
        validateExternalPlane(0, {width, height});
        waitConsumed();
        Plane replacement{};
        try {
            createPlane(0, replacement, wanted);
        } catch (...) {
            destroyPlane(replacement);
            throw;
        }
        std::swap(planes[0], replacement);
        destroyPlane(replacement);
        for (unsigned i = 1; i < planes.size(); ++i) destroyPlane(planes[i]);
        cfg = wanted;
        gpuTimestamps.rebuilt();
        for (auto& slot : commandSlots) slot.timestamps.rebuilt();
        if (timestampResourceGeneration != UINT64_MAX) ++timestampResourceGeneration;
    }
    void waitFence(ID3D12Fence* fence, uint64_t target, const char* description) {
        detail::waitForD3D12Fence(fence, target, event, cfg.timeoutMs, description);
    }
    void waitConsumed() {
        if (uncertainSubmission) throw std::logic_error("Uncertain bridge submission is quarantined");
        if (!value) return;
        if (!d3dSignalQueued) throw std::logic_error("queueSignalConsumed must follow published frame before reuse");
        waitFence(consumed, value, "Waiting for D3D12 consumed fence");
        gpuTimestamps.consumed();
        acceptGpuTimestamp(gpuTimestamps.latest());
        // This global consumed proof covers every earlier bridge command, also
        // a partial frame whose normal receipt never acquired a consumed signal.
        for (auto& slot : commandSlots) {
            slot.timestamps.consumed(); acceptGpuTimestamp(slot.timestamps.latest());
        }
        workOutstanding = false;
    }
    FrameCompletionState consumedComplete(std::uint64_t target) const noexcept {
        if (uncertainSubmission) return FrameCompletionState::Invalid;
        if (!consumed || !target || target > value || target == UINT64_MAX) return FrameCompletionState::Invalid;
        const auto done = consumed->GetCompletedValue();
        if (done == UINT64_MAX) return FrameCompletionState::DeviceLost;
        return done >= target ? FrameCompletionState::Complete : FrameCompletionState::Pending;
    }
    void acceptGpuTimestamp(const GpuTimestampSample& sample) noexcept {
        if (sample.sequence > latestGpuTimestamp.sequence) latestGpuTimestamp = sample;
    }
    void pollGpuTimestamps() noexcept {
        acceptGpuTimestamp(gpuTimestamps.latest());
        for (auto& slot : commandSlots) {
            if (slot.receipt.consumedValid && !slot.receipt.poisoned &&
                consumedComplete(slot.receipt.consumedValue) == FrameCompletionState::Complete) {
                slot.timestamps.consumed();
                acceptGpuTimestamp(slot.timestamps.latest());
            }
        }
    }
    CommandSlot& prepareCommandSlot(FrameSlotKey key, const FrameCompletion& receipt) {
        if (receipt.key != key || !detail::freshFrameCompletion(receipt))
            throw std::logic_error("Queued bridge requires a fresh caller-owned frame slot receipt");
        auto& slot = commandSlots[key.index];
        const auto previousState = slot.receipt.consumedValid && !slot.receipt.poisoned
            ? consumedComplete(slot.receipt.consumedValue) : FrameCompletionState::Invalid;
        if (!detail::frameSlotCanReuse(slot.receipt.key, key, previousState))
            throw std::logic_error("Queued bridge slot is stale or its actual consumed value is incomplete");
        if (slot.receipt.key.valid()) {
            slot.timestamps.consumed(); acceptGpuTimestamp(slot.timestamps.latest());
        }
        if (!slot.commands) {
            VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            info.commandPool = pool; info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; info.commandBufferCount = 1;
            checkVk(vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(vkDevice, &info, &slot.commands), "vkAllocateCommandBuffers(frame slot)");
            slot.timestamps.initialize(timestampContext);
        }
        slot.receipt = receipt;
        return slot;
    }
    void publish(const std::array<std::span<const uint8_t>, 3>& data) {
        if (cfg.colorOnly) throw std::logic_error("Color-only bridge accepts only native GPU color publication");
        for (unsigned i = 0; i < data.size(); ++i)
            if (data[i].size() != cfg.byteSize(static_cast<Format>(i)))
                throw std::invalid_argument("Every frame plane must exactly match its configured tightly packed byte count");
        waitConsumed();
        if (value == UINT64_MAX - 1) throw std::overflow_error("Frame fence value exhausted");
        for (unsigned i = 0; i < planes.size(); ++i) {
            auto& p = planes[i]; std::memcpy(p.mapped, data[i].data(), data[i].size());
            if (!p.coherent) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = p.stagingMemory; range.size = VK_WHOLE_SIZE;
                checkVk(vk<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(vkDevice, 1, &range), "vkFlushMappedMemoryRanges");
            }
        }
        checkVk(vk<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands, 0), "vkResetCommandBuffer");
        gpuTimestamps.resetRecording();
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer");
        std::array<VkImageMemoryBarrier, 3> barriers{};
        for (unsigned i = 0; i < planes.size(); ++i) {
            auto& b = barriers[i]; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = planes[i].initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; b.dstQueueFamilyIndex = family; b.image = planes[i].image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers.data());
        for (unsigned i = 0; i < planes.size(); ++i) {
            auto& p = planes[i]; const auto size = cfg.extent(static_cast<Format>(i));
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {size.width, size.height, 1};
            vk<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, p.staging, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
        for (auto& b : barriers) {
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = family; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        }
        vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers.data());
        checkVk(vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands), "vkEndCommandBuffer");
        submitRecorded("vkQueueSubmit(shared upload)");
        for (auto& plane : planes) plane.initialized = true;
        ++stats.cpuUploadFrames;
    }
    void submitRecorded(const char* operation, CommandSlot* slot = nullptr, FrameCompletion* receipt = nullptr) {
        const uint64_t previous = value, next = value + 1;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = previous ? 1 : 0; timeline.pWaitSemaphoreValues = &previous;
        timeline.signalSemaphoreValueCount = 1; timeline.pSignalSemaphoreValues = &next;
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
        submit.waitSemaphoreCount = previous ? 1 : 0; submit.pWaitSemaphores = &consumedSemaphore; submit.pWaitDstStageMask = &waitStage;
        const auto command = slot ? slot->commands : commands;
        auto& timestamps = slot ? slot->timestamps : gpuTimestamps;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &producedSemaphore;
        VkResult submitted;
        const auto entryError = queueObserver ? GetLastError() : 0;
        wsi_acquire::QueueCall timing;
        if (queueObserver) timing.entryNs = cpuWallNowNs();
        DWORD originalError;
        timestamps.beforeSubmit();
        {
            std::unique_lock<QueueGate> gate;
            if(queueGate)gate=std::unique_lock<QueueGate>(*queueGate);
            if (queueObserver) timing.driverEntryNs = cpuWallNowNs();
            if (queueObserver) SetLastError(entryError);
            submitted=vk<PFN_vkQueueSubmit>("vkQueueSubmit")(vkQueue, 1, &submit, VK_NULL_HANDLE);
            if (submitted == VK_SUCCESS) {
                value = next; d3dWaitQueued = false; d3dSignalQueued = false; workOutstanding = true;
                if (receipt) {
                    receipt->producedValue = next; receipt->producedValid = true;
                    slot->receipt = *receipt;
                }
            } else if (submitted != VK_ERROR_OUT_OF_HOST_MEMORY && submitted != VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                uncertainSubmission = true;
                if (receipt) { receipt->poisoned = true; slot->receipt = *receipt; }
            }
            originalError = queueObserver ? GetLastError() : 0;
            if (queueObserver) timing.returnNs = cpuWallNowNs();
        }
        timestamps.submitted(submitted);
        timing.result = submitted;
        if (queueObserver && timing.slow())
            queueObserver(queueObserverContext, "interop_copy_submit", reinterpret_cast<std::uintptr_t>(vkQueue), timing);
        if (queueObserver) SetLastError(originalError);
        checkVk(submitted, operation);
    }
    void publishShared(SharedPlaneKey key, VkQueue queue, VkFence fence, FrameCompletion& receipt) {
        CpuWallScope elapsed(stats.nativePublishCpu);
        std::lock_guard lock(sharedMutex);
        auto* shared = sharedSlot(key);
        if (uncertainSubmission || !borrowedVulkan || queue != vkQueue || !fence || !sharedRing || !sharedRing->ready ||
            !shared || shared->poisoned || shared->state != SharedState::Submitted ||
            (receipt.sharedAuxiliary ? (!shared->auxiliary || shared->writes != 7u) : !(shared->writes & 1u)) ||
            receipt.sharedKey != key ||
            (value && !d3dSignalQueued))
            throw std::logic_error("Direct shared publication requires its exact submitted reservation");
        auto& slot = prepareCommandSlot(receipt.key, receipt);
        if (value == UINT64_MAX - 1) throw std::overflow_error("Frame fence value exhausted");
        checkVk(vk<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(slot.commands, 0), "Reset shared release command");
        slot.timestamps.resetRecording();
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(slot.commands, &begin), "Begin shared release command");
        if (timestampSequence != UINT64_MAX)
            slot.timestamps.begin(slot.commands, gpuTimestampFrame, ++timestampSequence, gpuTimestamps.contextGeneration(), key.ringGeneration);
        std::array<VkImageMemoryBarrier, 3> barriers{};
        const unsigned count = receipt.sharedAuxiliary ? 3u : 1u;
        for (unsigned i = 0; i < count; ++i) {
            auto& barrier = barriers[i]; barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = family; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            barrier.image = shared->planes[i].image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(slot.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, count, barriers.data());
        slot.timestamps.end(slot.commands);
        checkVk(vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(slot.commands), "End shared release command");
        submitRecorded("vkQueueSubmit(direct shared release)", &slot, &receipt);
        shared->state = SharedState::Published; shared->direct = true;
        shared->publicationAuxiliary = receipt.sharedAuxiliary;
        shared->frameKey = receipt.key; shared->producedValue = receipt.producedValue;
        for (unsigned i = 0; i < count; ++i) shared->planes[i].ownerFamily = VK_QUEUE_FAMILY_EXTERNAL;
        ++sharedStats.directFrames;
    }
    void publishNative(const NativeFrame& frame, bool colorOnly = false, bool queued = false,
                       FrameCompletion* receipt = nullptr) {
        CpuWallScope elapsed(stats.nativePublishCpu);
        if (uncertainSubmission) throw std::logic_error("Uncertain bridge submission is quarantined");
        if (!borrowedVulkan) throw std::logic_error("publishNativeFrame requires a borrowed Vulkan context");
        if ((frame.sharedSourceKey != SharedPlaneKey{} && !frame.sharedSourceKey.valid()) ||
            (receipt && (receipt->sharedKey != SharedPlaneKey{} || receipt->sharedAuxiliary)))
            throw std::logic_error("Legacy publication cannot substitute a direct shared receipt");
        std::unique_lock<std::mutex> sharedLock(sharedMutex, std::defer_lock);
        SharedSlot* shared = nullptr;
        if (frame.sharedSourceKey.valid()) {
            sharedLock.lock(); shared = sharedSlot(frame.sharedSourceKey);
            if (!queued || !receipt || colorOnly || !shared || shared->poisoned ||
                !shared->auxiliary || shared->state != SharedState::Submitted || shared->writes != 6 ||
                frame.planes[1].image != shared->planes[1].image || frame.planes[2].image != shared->planes[2].image ||
                frame.planes[1].currentLayout != VK_IMAGE_LAYOUT_GENERAL || frame.planes[2].currentLayout != VK_IMAGE_LAYOUT_GENERAL)
                throw std::logic_error("Legacy shared sources require their exact submitted auxiliary lease");
        } else {
            std::lock_guard lock(sharedMutex);
            if (sharedRing) for (const auto& source : frame.planes) if (source.image)
                for (const auto& candidate : sharedRing->slots) for (const auto& plane : candidate.planes)
                    if (source.image == plane.image)
                        throw std::logic_error("Shared source images require an exact retained lease");
        }
        auto validationConfig = cfg; validationConfig.colorOnly = colorOnly || cfg.colorOnly;
        detail::validateNativeFrameMetadata(validationConfig, frame, vkDevice, vkQueue, family);
        if (queued && colorOnly && frame.planes[0].currentLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            throw std::invalid_argument("Queued OFF requires persistent private TRANSFER_SRC color on the borrowed queue");
        const unsigned planeCount = validationConfig.colorOnly ? 1u : 3u;
        for (unsigned i = 0; i < planeCount; ++i) {
            const auto& source = frame.planes[i];
            for (const auto& plane : planes) if (source.image == plane.image)
                throw std::invalid_argument("Native sources cannot alias the bridge's imported destination images");
        }
        if (receipt && (!queued || (value && !d3dSignalQueued)))
            throw std::logic_error("Slot publication requires queued mode and the preceding consumed signal already queued");
        auto* slot = receipt ? &prepareCommandSlot(receipt->key, *receipt) : nullptr;
        if (!slot) waitConsumed();
        const auto commands = slot ? slot->commands : this->commands;
        auto& timestamps = slot ? slot->timestamps : gpuTimestamps;
        if (value == UINT64_MAX - 1) throw std::overflow_error("Frame fence value exhausted");
        if (!queued) checkVk(vk<PFN_vkWaitForFences>("vkWaitForFences")(vkDevice, 1, &frame.producerCompleteFence, VK_TRUE,
                static_cast<uint64_t>(cfg.timeoutMs) * 1'000'000), "vkWaitForFences(native producer)");
        checkVk(vk<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands, 0), "vkResetCommandBuffer(native)");
        timestamps.resetRecording();
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer(native)");
        if (timestampSequence != UINT64_MAX && timestampResourceGeneration != UINT64_MAX)
            timestamps.begin(commands, gpuTimestampFrame, ++timestampSequence, gpuTimestamps.contextGeneration(), timestampResourceGeneration);
        std::array<VkImageMemoryBarrier, 6> barriers{};
        for (unsigned i = 0; i < planeCount; ++i) {
            auto& source = barriers[i]; source.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            source.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            source.oldLayout = frame.planes[i].currentLayout; source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            source.image = frame.planes[i].image; source.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            auto& destination = barriers[i + planeCount]; destination.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            destination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            destination.oldLayout = planes[i].initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            destination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            destination.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; destination.dstQueueFamilyIndex = family;
            destination.image = planes[i].image; destination.subresourceRange = source.subresourceRange;
        }
        vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                             0, 0, nullptr, 0, nullptr, planeCount * 2, barriers.data());
        for (unsigned i = 0; i < planeCount; ++i) {
            VkImageCopy copy{}; copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.dstSubresource = copy.srcSubresource;
            const auto size = cfg.extent(static_cast<Format>(i));
            copy.extent = {size.width, size.height, 1};
            vk<PFN_vkCmdCopyImage>("vkCmdCopyImage")(commands, frame.planes[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                       planes[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            measurement::command(measurement::Command::ImageCopy);
        }
        for (unsigned i = 0; i < planeCount; ++i) {
            auto& source = barriers[i]; source.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            source.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; source.newLayout = frame.planes[i].currentLayout;
            auto& destination = barriers[i + planeCount]; destination.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; destination.dstAccessMask = 0;
            destination.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; destination.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            destination.srcQueueFamilyIndex = family; destination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        }
        vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                             0, 0, nullptr, 0, nullptr, planeCount * 2, barriers.data());
        timestamps.end(commands);
        checkVk(vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands), "vkEndCommandBuffer(native)");
        try { submitRecorded("vkQueueSubmit(native GPU image copy)", slot, receipt); }
        catch(...) { measurement::increment(measurement::Counter::CopyFailures);throw; }
        for(unsigned i=0;i<planeCount;++i)
            measurement::submittedCommand(i==0?measurement::CopyRole::Color:i==1?measurement::CopyRole::Depth:measurement::CopyRole::Motion,
                                          measurement::Command::ImageCopy);
        if (shared) {
            shared->state = SharedState::Published; shared->direct = false;
            shared->frameKey = receipt->key; shared->producedValue = receipt->producedValue;
            ++sharedStats.legacySourceFrames;
        }
        for (unsigned i = 0; i < planeCount; ++i) planes[i].initialized = true;
        ++stats.nativeImageFrames;
        // Synchronous entry points return completed source reads/restoration;
        // Queued entries retain the caller's source lifetime through real GPU use.
        if (!queued) waitFence(produced, value, "Waiting for native source copy/layout restoration");
    }
    void queueWait(FrameCompletion* receipt = nullptr) {
        if (!value || d3dWaitQueued || d3dSignalQueued) throw std::logic_error("queueWaitForProduced requires a newly published frame");
        if (receipt) validateCurrentReceipt(*receipt);
        check(d3dQueue->Wait(produced, value), "D3D12 queue Wait(produced)"); d3dWaitQueued = true;
        if (receipt) { receipt->d3dWaitQueued = true; commandSlots[receipt->key.index].receipt = *receipt; }
    }
    void queueSignal(FrameCompletion* receipt = nullptr, SharedPlaneKey postPresentShared = {}) {
        if (!d3dWaitQueued || d3dSignalQueued) throw std::logic_error("queueSignalConsumed requires queued produced wait and may only run once per frame");
        if (receipt) validateCurrentReceipt(*receipt);
        if ((receipt && receipt->sharedKey.valid()) != postPresentShared.valid() ||
            (postPresentShared.valid() && receipt->sharedKey != postPresentShared))
            throw std::logic_error("Direct shared consumed requires the post-Present queue path");
        std::lock_guard lock(sharedMutex);
        if (postPresentShared.valid()) {
            const auto* shared = sharedSlot(postPresentShared);
            if (!shared || shared->poisoned || !shared->direct || shared->state != SharedState::Published ||
                shared->frameKey != receipt->key || shared->producedValue != value || !receipt->callerSubmitted ||
                receipt->sharedAuxiliary != shared->publicationAuxiliary ||
                (shared->publicationAuxiliary && !receipt->postPresentSubmitted))
                throw std::logic_error("Post-Present consumed does not identify its direct shared publication");
        }
        check(d3dQueue->Signal(consumed, value), "D3D12 queue Signal(consumed)"); d3dSignalQueued = true;
        if (receipt) {
            receipt->consumedValue = value; receipt->consumedValid = true;
            if (postPresentShared.valid()) receipt->postPresentSubmitted = true;
            commandSlots[receipt->key.index].receipt = *receipt;
        }
        if (receipt && sharedRing) for (auto& shared : sharedRing->slots)
            if (shared.state == SharedState::Published && shared.frameKey == receipt->key && shared.producedValue == value)
                shared.consumedValue = value;
    }
    void validateCurrentReceipt(const FrameCompletion& receipt) const {
        if (!receipt.key.valid() || receipt.poisoned || !receipt.producedValid || receipt.producedValue != value ||
            commandSlots[receipt.key.index].receipt.key != receipt.key ||
            commandSlots[receipt.key.index].receipt.producedValue != receipt.producedValue ||
            commandSlots[receipt.key.index].receipt.sharedKey != receipt.sharedKey ||
            commandSlots[receipt.key.index].receipt.sharedAuxiliary != receipt.sharedAuxiliary)
            throw std::logic_error("Bridge receipt does not identify its actual current slot publication");
    }
    void verify(const FramePlanes& expected) {
        if (cfg.colorOnly) throw std::logic_error("Three-plane verification is unavailable for color-only publication");
        for (unsigned i = 0; i < expected.size(); ++i)
            if (expected[i].size() != cfg.byteSize(static_cast<Format>(i))) throw std::invalid_argument("Verification plane byte count mismatch");
        if (!value || d3dWaitQueued) throw std::logic_error("verifyFrame requires an unconsumed published frame");
        check(allocator->Reset(), "Reset(D3D12 allocator)");
        check(list->Reset(allocator, nullptr), "Reset(D3D12 command list)");
        for (auto& p : planes) {
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = p.texture; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = p.texture; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = p.readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = p.footprint;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter); list->ResourceBarrier(1, &b);
        }
        check(list->Close(), "Close(D3D12 readback list)");
        queueWait(); ID3D12CommandList* submitted[]{list}; d3dQueue->ExecuteCommandLists(1, submitted); queueSignal(); waitConsumed();
        for (unsigned i = 0; i < planes.size(); ++i) {
            auto& p = planes[i]; void* mapped = nullptr;
            const auto size = cfg.extent(static_cast<Format>(i));
            const size_t rowBytes = static_cast<size_t>(size.width) * 4;
            D3D12_RANGE read{0, static_cast<SIZE_T>(p.readbackSize)};
            check(p.readback->Map(0, &read, &mapped), "Map(D3D12 readback)");
            const auto* actual = static_cast<const uint8_t*>(mapped) + p.footprint.Offset;
            for (uint32_t y = 0; y < size.height; ++y) {
                const auto* row = actual + static_cast<size_t>(y) * p.footprint.Footprint.RowPitch;
                const auto* want = expected[i].data() + static_cast<size_t>(y) * rowBytes;
                for (size_t byte = 0; byte < rowBytes; ++byte) if (row[byte] != want[byte]) {
                    std::ostringstream s; s << "Pixel mismatch: fence=" << value << ", format=" << names[i] << ", x=" << byte / 4 << ", y=" << y << ", byte=" << byte % 4 << ", expected=" << unsigned(want[byte]) << ", actual=" << unsigned(row[byte]);
                    D3D12_RANGE written{0, 0}; p.readback->Unmap(0, &written); throw std::runtime_error(s.str());
                }
            }
            D3D12_RANGE written{0, 0}; p.readback->Unmap(0, &written);
            stats.bytesVerified += rowBytes * size.height;
        }
        ++stats.framesVerified;
    }
    bool canCleanUp() noexcept {
        if (uncertainSubmission) return false;
        try {
            {
                std::lock_guard lock(sharedMutex);
                if (sharedRing) sharedRing->ready = false;
                if (sharedLeases()) return false;
            }
            if (!workOutstanding) return true;
            if (!d3dWaitQueued) queueWait();
            if (!d3dSignalQueued) queueSignal();
            waitConsumed(); return true;
        } catch (...) { return false; }
    }
    ~Impl() {
        // The owner abandons this object on an uncompleted GPU timeout rather
        // than freeing resources still in GPU use or calling unbounded WaitIdle.
        if (vkDevice) {
            gpuTimestamps.releaseCompleted();
            for (auto& slot : commandSlots) slot.timestamps.releaseCompleted();
            for (auto& p : planes) destroyPlane(p);
            if (sharedRing) for (auto& slot : sharedRing->slots) for (auto& p : slot.planes) destroyPlane(p);
            if (pool) vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(vkDevice, pool, nullptr);
            if (producedSemaphore) vk<PFN_vkDestroySemaphore>("vkDestroySemaphore")(vkDevice, producedSemaphore, nullptr);
            if (consumedSemaphore) vk<PFN_vkDestroySemaphore>("vkDestroySemaphore")(vkDevice, consumedSemaphore, nullptr);
            if (!borrowedVulkan) vk<PFN_vkDestroyDevice>("vkDestroyDevice")(vkDevice, nullptr);
        }
        for (auto& p : planes) { release(p.readback); release(p.texture); }
        release(list); release(allocator); release(produced); release(consumed); release(d3dQueue); release(d3d);
        if (event) CloseHandle(event);
        if (!borrowedVulkan && instance && get) reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"))(instance, nullptr);
        if (loader) FreeLibrary(loader);
    }
};

InteropBridge::InteropBridge(InteropConfig config) : impl_(std::make_unique<Impl>()) { impl_->cfg = config; impl_->initialize(); }
InteropBridge::InteropBridge(InteropConfig config, const BorrowedVulkanContext& borrowed) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = config; impl_->initializeBorrowed(borrowed);
}
InteropBridge::~InteropBridge() { if (impl_ && !impl_->canCleanUp()) (void)impl_.release(); }
ID3D12Device* InteropBridge::device() const { return impl_->d3d; }
ID3D12CommandQueue* InteropBridge::queue() const { return impl_->d3dQueue; }
ID3D12Resource* InteropBridge::texture(Format format) const { return impl_->planes.at(static_cast<unsigned>(format)).texture; }
ID3D12Resource* InteropBridge::texture(Format format, SharedPlaneKey key) const {
    std::lock_guard lock(impl_->sharedMutex); const auto* slot = impl_->sharedSlot(key);
    if (!slot || slot->poisoned) throw std::logic_error("Shared texture key is stale or poisoned");
    auto* texture = slot->planes.at(static_cast<unsigned>(format)).texture;
    if (!texture) throw std::logic_error("Shared texture plane is unavailable");
    return texture;
}
InteropConfig InteropBridge::config(SharedPlaneKey key) const {
    std::lock_guard lock(impl_->sharedMutex);
    if (!impl_->sharedSlot(key)) throw std::logic_error("Shared configuration key is stale");
    return impl_->sharedRing->config;
}
uint64_t InteropBridge::frameValue() const { return impl_->value; }
const InteropConfig& InteropBridge::config() const { return impl_->cfg; }
const TransferStats& InteropBridge::stats() const { return impl_->stats; }
void InteropBridge::associateGpuTimestampFrame(const GpuTimestampSample& identity) noexcept { impl_->gpuTimestampFrame = identity; }
GpuTimestampSample InteropBridge::gpuTimestampSample() const noexcept { impl_->pollGpuTimestamps(); return impl_->latestGpuTimestamp; }
void InteropBridge::publishFrame(const FramePlanes& planes) { publishFrame(std::array<std::span<const uint8_t>, 3>{planes[0], planes[1], planes[2]}); }
void InteropBridge::publishFrame(const std::array<std::span<const uint8_t>, 3>& planes) { impl_->publish(planes); }
void InteropBridge::publishPattern(uint64_t patternFrame) { publishFrame(makePattern(impl_->cfg, patternFrame)); }
void InteropBridge::publishNativeFrame(const NativeFrame& frame) {
    if (impl_->cfg.colorOnly) throw std::logic_error("Color-only bridge requires publishNativeColor");
    impl_->publishNative(frame);
}
void InteropBridge::publishQueuedNativeFrame(const NativeFrame& frame) {
    if (impl_->cfg.colorOnly) throw std::logic_error("Queued ON requires all three bridge planes");
    impl_->publishNative(frame, false, true);
}
void InteropBridge::publishQueuedNativeFrame(const NativeFrame& frame, FrameSlotKey key, FrameCompletion& receipt) {
    if (impl_->cfg.colorOnly || key != receipt.key) throw std::logic_error("Queued ON requires matching slot and three planes");
    impl_->publishNative(frame, false, true, &receipt);
}
bool InteropBridge::prepareSharedPlanes(std::uint32_t width, std::uint32_t height) { return impl_->prepareSharedPlanes(width, height); }
SharedBorrowResult InteropBridge::tryReserveSharedFrame(VkExtent2D extent, bool auxiliary, SharedPlaneLease& lease) noexcept {
    return impl_->tryReserveSharedFrame(extent, auxiliary, lease);
}
void InteropBridge::sharedWriteSubmitted(SharedPlaneKey key, std::uint32_t mask) { impl_->sharedWriteSubmitted(key, mask); }
bool InteropBridge::cancelRecordedSharedFrame(SharedPlaneKey key) noexcept { return impl_->cancelRecordedSharedFrame(key); }
bool InteropBridge::abandonSubmittedSharedFrame(SharedPlaneKey key, VkFence fence) noexcept { return impl_->abandonSubmittedSharedFrame(key, fence); }
void InteropBridge::publishQueuedSharedFrame(SharedPlaneKey key, VkQueue queue, VkFence fence,
                                            FrameSlotKey frame, FrameCompletion& receipt) {
    if (frame != receipt.key) throw std::logic_error("Direct shared publication frame key differs from its receipt");
    impl_->publishShared(key, queue, fence, receipt);
}
bool InteropBridge::releaseSharedFrame(SharedPlaneKey key, const FrameCompletion& receipt) noexcept { return impl_->releaseSharedFrame(key, receipt); }
void InteropBridge::poisonSharedFrame(SharedPlaneKey key) noexcept { impl_->poisonSharedFrame(key); }
SharedPlaneStats InteropBridge::sharedPlaneStats() const noexcept { return impl_->sharedPlaneStats(); }
void InteropBridge::publishNativeColor(const NativeImageSource& color, VkQueue queue, VkFence fence) {
    NativeFrame frame{}; frame.planes[0] = color; frame.producerQueue = queue; frame.producerCompleteFence = fence;
    impl_->publishNative(frame, true);
}
void InteropBridge::publishQueuedNativeColor(const NativeImageSource& color, VkQueue queue, VkFence fence) {
    NativeFrame frame{}; frame.planes[0] = color; frame.producerQueue = queue; frame.producerCompleteFence = fence;
    impl_->publishNative(frame, true, true);
}
void InteropBridge::publishQueuedNativeColor(const NativeImageSource& color, VkQueue queue, VkFence fence, FrameSlotKey key, FrameCompletion& receipt) {
    if (key != receipt.key) throw std::logic_error("Queued OFF requires matching slot receipt");
    NativeFrame frame{}; frame.planes[0] = color; frame.producerQueue = queue; frame.producerCompleteFence = fence;
    impl_->publishNative(frame, true, true, &receipt);
}
void InteropBridge::enableAuxiliaryInputs(std::uint32_t width, std::uint32_t height) {
    impl_->enableAuxiliaryInputs(width, height);
}
void InteropBridge::resizeOutput(std::uint32_t width, std::uint32_t height) {
    impl_->resizeOutput(width, height);
}
void InteropBridge::queueWaitForProduced() { impl_->queueWait(); }
void InteropBridge::queueWaitForProduced(FrameCompletion& receipt) { impl_->queueWait(&receipt); }
void InteropBridge::queueSignalConsumed() { impl_->queueSignal(); }
void InteropBridge::queueSignalConsumed(FrameCompletion& receipt) { impl_->queueSignal(&receipt); }
void InteropBridge::queueSignalSharedConsumed(SharedPlaneKey key, FrameCompletion& receipt) {
    if (!key.valid()) throw std::logic_error("Post-Present consumed requires a valid shared key");
    impl_->queueSignal(&receipt, key);
}
void InteropBridge::waitUntilConsumed() { impl_->waitConsumed(); }
FrameCompletionState InteropBridge::consumedComplete(std::uint64_t target) const noexcept { return impl_->consumedComplete(target); }
void InteropBridge::verifyFrame(const FramePlanes& expected) { impl_->verify(expected); }
void InteropBridge::verifyPattern(uint64_t patternFrame) { impl_->verify(makePattern(impl_->cfg, patternFrame)); }
FramePlanes InteropBridge::makePattern(const InteropConfig& config, uint64_t frame) {
    config.validate();
    FramePlanes result;
    for (unsigned i = 0; i < result.size(); ++i) {
        const auto format = static_cast<Format>(i);
        const auto size = config.extent(format);
        result[i].resize(config.byteSize(format));
        for (uint32_t y = 0; y < size.height; ++y) for (uint32_t x = 0; x < size.width; ++x) {
            const auto offset = (static_cast<size_t>(y) * size.width + x) * 4;
            if (format == Format::ColorRGBA8) {
                const uint32_t color = static_cast<uint32_t>(frame * 2654435761ULL) ^ (x * 2246822519u) ^ (y * 3266489917u);
                std::memcpy(result[i].data() + offset, &color, 4);
            } else if (format == Format::DepthR32F) {
                const float depth = static_cast<float>((frame * 131 + x + y * size.width) % 8192) / 8192.0f;
                std::memcpy(result[i].data() + offset, &depth, 4);
            } else {
                const uint16_t motion[2]{static_cast<uint16_t>(0x3000u | ((frame * 17 + x * 3 + y) & 0x7ff)), static_cast<uint16_t>(0xb000u | ((frame * 19 + x + y * 7) & 0x7ff))};
                std::memcpy(result[i].data() + offset, motion, 4);
            }
        }
    }
    return result;
}
}
