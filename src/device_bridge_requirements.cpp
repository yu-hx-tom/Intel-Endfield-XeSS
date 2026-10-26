#include "device_bridge_requirements.hpp"
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace xess_vk {
namespace {
constexpr const char* memoryWin32 = "VK_KHR_external_memory_win32";
constexpr const char* semaphoreWin32 = "VK_KHR_external_semaphore_win32";
constexpr std::uint32_t maxNames = 512, maxQueues = 64, maxPriorities = 4096, maxNodes = 64;

struct Refusal {
    DeviceBridgePlanStatus status;
    const char* reason;
    VkStructureType structure = VK_STRUCTURE_TYPE_MAX_ENUM;
};
[[noreturn]] void refuse(DeviceBridgePlanStatus status, const char* reason,
                        VkStructureType structure = VK_STRUCTURE_TYPE_MAX_ENUM) {
    throw Refusal{status, reason, structure};
}

struct Node {
    virtual ~Node() = default;
    virtual const void* data() const noexcept = 0;
    virtual void next(const void*) noexcept = 0;
    virtual VkStructureType type() const noexcept = 0;
    virtual VkBool32* timeline() noexcept { return nullptr; }
    unsigned aggregateGroup = 0, memberGroup = 0;
};
template<class T> struct FlatNode final : Node {
    T value;
    explicit FlatNode(const T& original) : value(original) { value.pNext = nullptr; }
    const void* data() const noexcept override { return &value; }
    void next(const void* pointer) noexcept override {
        if constexpr (std::is_same_v<decltype(value.pNext), void*>) value.pNext = const_cast<void*>(pointer);
        else value.pNext = pointer;
    }
    VkStructureType type() const noexcept override { return value.sType; }
    VkBool32* timeline() noexcept override {
        if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan12Features>
                      || std::is_same_v<T, VkPhysicalDeviceTimelineSemaphoreFeatures>) return &value.timelineSemaphore;
        else return nullptr;
    }
};
using Chain = std::vector<std::unique_ptr<Node>>;

template<class T> std::unique_ptr<Node> copyNode(const VkBaseInStructure* original,
                                              unsigned aggregate = 0, unsigned member = 0) {
    auto node = std::make_unique<FlatNode<T>>(*reinterpret_cast<const T*>(original));
    node->aggregateGroup = aggregate;
    node->memberGroup = member;
    return node;
}

// Every whitelisted node is explicitly typed and has no nested data pointers
// other than pNext. Pointer-rich/unknown structures are deliberately refused.
#define FLAT_DEVICE_NODES(X) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, VkPhysicalDeviceFeatures2, 0, 0) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features, 11, 0) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features, 12, 0) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features, 13, 0) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, VkPhysicalDeviceVulkan14Features, 14, 0) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures, 0, 11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, VkPhysicalDeviceMultiviewFeatures, 0, 11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, VkPhysicalDeviceVariablePointersFeatures, 0, 11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, VkPhysicalDeviceProtectedMemoryFeatures, 0, 11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, VkPhysicalDeviceSamplerYcbcrConversionFeatures, 0, 11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, VkPhysicalDeviceShaderDrawParametersFeatures, 0, 11) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, VkPhysicalDeviceShaderAtomicInt64Features, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, VkPhysicalDeviceImagelessFramebufferFeatures, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, VkPhysicalDeviceUniformBufferStandardLayoutFeatures, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VkPhysicalDeviceHostQueryResetFeatures, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VkPhysicalDeviceTimelineSemaphoreFeatures, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VkPhysicalDeviceVulkanMemoryModelFeatures, 0, 12) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, VkPhysicalDeviceImageRobustnessFeatures, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, VkPhysicalDeviceInlineUniformBlockFeatures, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, VkPhysicalDevicePipelineCreationCacheControlFeatures, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, VkPhysicalDevicePrivateDataFeatures, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, VkPhysicalDeviceShaderIntegerDotProductFeatures, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, VkPhysicalDeviceShaderTerminateInvocationFeatures, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VkPhysicalDeviceSubgroupSizeControlFeatures, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES, VkPhysicalDeviceTextureCompressionASTCHDRFeatures, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES, VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures, 0, 13) \
 X(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR, VkPhysicalDeviceFragmentShadingRateFeaturesKHR, 0, 0) \
 X(VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO, VkDevicePrivateDataCreateInfo, 0, 0)

const void* link(Chain& chain) noexcept {
    const void* next = nullptr;
    for (auto at = chain.rbegin(); at != chain.rend(); ++at) {
        (*at)->next(next);
        next = (*at)->data();
    }
    return next;
}

Chain copyChain(const void* first, bool queueChain, AllowBorrowedLoaderChain allowLoader, bool& borrowedLoader) {
    Chain chain;
    std::vector<const void*> visited;
    std::vector<VkStructureType> types;
    std::vector<VkLayerFunction> loaderFunctions;
    auto source = static_cast<const VkBaseInStructure*>(first);
    while (source) {
        if (visited.size() >= maxNodes || std::find(visited.begin(), visited.end(), source) != visited.end())
            refuse(DeviceBridgePlanStatus::InvalidCreateInfo, "pNext chain is cyclic or exceeds 64 nodes");
        visited.push_back(source);
        const auto type = source->sType;
        if (type != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && std::find(types.begin(), types.end(), type) != types.end())
            refuse(DeviceBridgePlanStatus::ConflictingFeatureChain, "Duplicate pNext structure type", type);
        types.push_back(type);
        std::unique_ptr<Node> copy;
        if (queueChain) {
            if (type != VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO)
                refuse(DeviceBridgePlanStatus::UnsupportedPNext, "Queue pNext structure is not in the typed whitelist", type);
            copy = copyNode<VkDeviceQueueGlobalPriorityCreateInfo>(source);
        } else if (type == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            if (allowLoader != AllowBorrowedLoaderChain::SynchronousCreateDeviceCallOnly)
                refuse(DeviceBridgePlanStatus::BorrowedLoaderChainNotAllowed, "Loader payload requires explicit synchronous borrowing", type);
            const auto* loader = reinterpret_cast<const VkLayerDeviceCreateInfo*>(source);
            if (loader->function != VK_LAYER_LINK_INFO && loader->function != VK_LOADER_DATA_CALLBACK)
                refuse(DeviceBridgePlanStatus::UnsupportedPNext, "Unknown loader device-create function payload", type);
            if (std::find(loaderFunctions.begin(), loaderFunctions.end(), loader->function) != loaderFunctions.end())
                refuse(DeviceBridgePlanStatus::ConflictingFeatureChain, "Duplicate loader device-create function", type);
            loaderFunctions.push_back(loader->function);
            copy = copyNode<VkLayerDeviceCreateInfo>(source);
            borrowedLoader = true;
        } else {
            switch (type) {
#define COPY_NODE(stype, ctype, aggregate, member) case stype: copy = copyNode<ctype>(source, aggregate, member); break;
                FLAT_DEVICE_NODES(COPY_NODE)
#undef COPY_NODE
            default: refuse(DeviceBridgePlanStatus::UnsupportedPNext, "Device pNext structure is not in the typed whitelist", type);
            }
        }
        chain.push_back(std::move(copy));
        source = source->pNext;
    }
    return chain;
}

std::vector<std::string> copyNames(std::uint32_t count, const char* const* names) {
    if (count > maxNames || (count && !names))
        refuse(DeviceBridgePlanStatus::InvalidCreateInfo, "Invalid extension/layer name array or more than 512 names");
    std::vector<std::string> copied;
    copied.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!names[i]) refuse(DeviceBridgePlanStatus::InvalidCreateInfo, "Null extension/layer name");
        std::size_t length = 0;
        while (length < VK_MAX_EXTENSION_NAME_SIZE && names[i][length]) ++length;
        if (!length || length == VK_MAX_EXTENSION_NAME_SIZE)
            refuse(DeviceBridgePlanStatus::InvalidCreateInfo, "Empty or unterminated extension/layer name");
        copied.emplace_back(names[i], length);
    }
    return copied;
}
std::vector<const char*> namePointers(const std::vector<std::string>& names) {
    std::vector<const char*> pointers;
    pointers.reserve(names.size());
    for (const auto& name : names) pointers.push_back(name.c_str());
    return pointers;
}
bool supported(const DeviceBridgeSupport& support, const char* name) noexcept {
    for (const auto& extension : support.extensions) {
        if (std::memchr(extension.extensionName, 0, VK_MAX_EXTENSION_NAME_SIZE)
            && std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
}
bool apiAtLeast(std::uint32_t api, std::uint32_t minor) noexcept {
    return VK_API_VERSION_VARIANT(api) == 0 && (VK_API_VERSION_MAJOR(api) > 1
        || (VK_API_VERSION_MAJOR(api) == 1 && VK_API_VERSION_MINOR(api) >= minor));
}
} // namespace

struct DeviceBridgePlan::Impl {
    VkDeviceCreateInfo info{};
    VkPhysicalDeviceFeatures features{};
    std::vector<VkDeviceQueueCreateInfo> queues;
    std::vector<std::vector<float>> priorities;
    std::vector<Chain> queueChains;
    std::vector<std::string> extensionNames, layerNames, addedExtensions;
    std::vector<const char*> extensionPointers, layerPointers;
    Chain chain;
    bool changedTimeline = false, borrowedLoader = false;
};

DeviceBridgePlan::DeviceBridgePlan() noexcept = default;
DeviceBridgePlan::~DeviceBridgePlan() = default;
DeviceBridgePlan::DeviceBridgePlan(DeviceBridgePlan&&) noexcept = default;
DeviceBridgePlan& DeviceBridgePlan::operator=(DeviceBridgePlan&&) noexcept = default;
DeviceBridgePlanStatus DeviceBridgePlan::status() const noexcept {
    return status_ == DeviceBridgePlanStatus::Ready && !impl_ ? DeviceBridgePlanStatus::NotRequested : status_;
}
bool DeviceBridgePlan::ready() const noexcept { return status_ == DeviceBridgePlanStatus::Ready && impl_; }
const char* DeviceBridgePlan::reason() const noexcept {
    return status_ == DeviceBridgePlanStatus::Ready && !impl_ ? "Plan storage was moved to another owner" : reason_;
}
VkStructureType DeviceBridgePlan::refusedStructureType() const noexcept { return refusedStructureType_; }
const VkDeviceCreateInfo* DeviceBridgePlan::createInfo() const noexcept { return ready() ? &impl_->info : nullptr; }
std::span<const std::string> DeviceBridgePlan::addedExtensions() const noexcept {
    return impl_ ? std::span<const std::string>(impl_->addedExtensions) : std::span<const std::string>{};
}
bool DeviceBridgePlan::timelineFeatureChanged() const noexcept { return impl_ && impl_->changedTimeline; }
bool DeviceBridgePlan::requiresSynchronousLoaderCall() const noexcept { return impl_ && impl_->borrowedLoader; }

DeviceBridgePlan planDeviceBridgeRequirements(const VkDeviceCreateInfo& original,
    const DeviceBridgeSupport& support, DeviceBridgePlanOptions options) noexcept {
    DeviceBridgePlan result;
    if (!options.optIn) return result;
    try {
        if (!apiAtLeast(support.instanceApiVersion, 2) || !apiAtLeast(support.physicalDeviceApiVersion, 2))
            refuse(DeviceBridgePlanStatus::UnsupportedApi, "The existing bridge requires instance and physical-device Vulkan 1.2 or later");
        if (!support.extensionQueryComplete || !support.timelineFeatureQueryComplete)
            refuse(DeviceBridgePlanStatus::CapabilityQueryMissing, "Completed extension and timeline-feature queries are required");
        if (!supported(support, memoryWin32) || !supported(support, semaphoreWin32))
            refuse(DeviceBridgePlanStatus::MissingRequiredExtension, "Both Win32 external-memory and external-semaphore extensions must be supported");
        if (support.timelineSemaphore != VK_TRUE)
            refuse(DeviceBridgePlanStatus::TimelineUnsupported, "Queried timelineSemaphore support is not VK_TRUE");
        if (original.sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || original.flags
            || !original.queueCreateInfoCount || original.queueCreateInfoCount > maxQueues || !original.pQueueCreateInfos)
            refuse(DeviceBridgePlanStatus::InvalidCreateInfo, "Invalid device create-info header or queue array");

        auto owned = std::make_unique<DeviceBridgePlan::Impl>();
        owned->info = original;
        owned->extensionNames = copyNames(original.enabledExtensionCount, original.ppEnabledExtensionNames);
        owned->layerNames = copyNames(original.enabledLayerCount, original.ppEnabledLayerNames);
        for (const char* required : {memoryWin32, semaphoreWin32}) {
            if (std::find(owned->extensionNames.begin(), owned->extensionNames.end(), required) == owned->extensionNames.end()) {
                owned->extensionNames.emplace_back(required);
                owned->addedExtensions.emplace_back(required);
            }
        }
        if (options.calibratedTimestamps) {
            constexpr const char* khr = "VK_KHR_calibrated_timestamps";
            constexpr const char* ext = "VK_EXT_calibrated_timestamps";
            const auto enabled = [&](const char* name) {
                return std::find(owned->extensionNames.begin(), owned->extensionNames.end(), name) != owned->extensionNames.end();
            };
            if (!enabled(khr) && !enabled(ext)) {
                const char* optional = supported(support, khr) ? khr : supported(support, ext) ? ext : nullptr;
                if (optional) {
                    owned->extensionNames.emplace_back(optional);
                    owned->addedExtensions.emplace_back(optional);
                }
            }
        }
        owned->extensionPointers = namePointers(owned->extensionNames);
        owned->layerPointers = namePointers(owned->layerNames);
        owned->info.enabledExtensionCount = static_cast<std::uint32_t>(owned->extensionPointers.size());
        owned->info.ppEnabledExtensionNames = owned->extensionPointers.data();
        owned->info.ppEnabledLayerNames = owned->layerPointers.empty() ? nullptr : owned->layerPointers.data();
        if (original.pEnabledFeatures) {
            owned->features = *original.pEnabledFeatures;
            owned->info.pEnabledFeatures = &owned->features;
        }
        owned->queues.assign(original.pQueueCreateInfos, original.pQueueCreateInfos + original.queueCreateInfoCount);
        owned->priorities.resize(owned->queues.size());
        owned->queueChains.resize(owned->queues.size());
        std::uint32_t totalPriorities = 0;
        for (std::size_t i = 0; i < owned->queues.size(); ++i) {
            auto& queue = owned->queues[i];
            if (queue.sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || !queue.queueCount
                || queue.queueCount > maxPriorities || !queue.pQueuePriorities
                || totalPriorities > maxPriorities - queue.queueCount)
                refuse(DeviceBridgePlanStatus::InvalidCreateInfo, "Invalid queue header, priorities, or bounded queue count");
            totalPriorities += queue.queueCount;
            owned->priorities[i].assign(queue.pQueuePriorities, queue.pQueuePriorities + queue.queueCount);
            for (const auto priority : owned->priorities[i]) if (!std::isfinite(priority) || priority < 0 || priority > 1)
                refuse(DeviceBridgePlanStatus::InvalidCreateInfo, "Queue priority must be finite and within [0, 1]");
            queue.pQueuePriorities = owned->priorities[i].data();
            owned->queueChains[i] = copyChain(queue.pNext, true, AllowBorrowedLoaderChain::No, owned->borrowedLoader);
            queue.pNext = link(owned->queueChains[i]);
        }
        owned->info.pQueueCreateInfos = owned->queues.data();
        owned->chain = copyChain(original.pNext, false, options.loaderChain, owned->borrowedLoader);
        VkBool32* timeline = nullptr;
        for (const auto& node : owned->chain) {
            if (node->type() == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 && original.pEnabledFeatures)
                refuse(DeviceBridgePlanStatus::ConflictingFeatureChain, "Features2 cannot coexist with pEnabledFeatures", node->type());
            if (node->aggregateGroup >= 13 && (!apiAtLeast(support.instanceApiVersion, node->aggregateGroup - 10)
                || !apiAtLeast(support.physicalDeviceApiVersion, node->aggregateGroup - 10)))
                refuse(DeviceBridgePlanStatus::UnsupportedApi, "Aggregate feature structure exceeds the supplied Vulkan API version", node->type());
            if (node->aggregateGroup) for (const auto& other : owned->chain) if (other->memberGroup == node->aggregateGroup)
                refuse(DeviceBridgePlanStatus::ConflictingFeatureChain, "Aggregate and promoted individual feature structures must not coexist", other->type());
            if (auto* feature = node->timeline()) {
                if (timeline) refuse(DeviceBridgePlanStatus::ConflictingFeatureChain, "More than one timeline feature declaration", node->type());
                timeline = feature;
            }
        }
        if (timeline) {
            if (*timeline != VK_FALSE && *timeline != VK_TRUE)
                refuse(DeviceBridgePlanStatus::InvalidCreateInfo, "timelineSemaphore has an invalid VkBool32 value");
            owned->changedTimeline = *timeline != VK_TRUE;
            *timeline = VK_TRUE;
        } else {
            VkPhysicalDeviceTimelineSemaphoreFeatures added{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
            added.timelineSemaphore = VK_TRUE;
            owned->chain.push_back(std::make_unique<FlatNode<VkPhysicalDeviceTimelineSemaphoreFeatures>>(added));
            owned->changedTimeline = true;
        }
        owned->info.pNext = link(owned->chain);
        result.impl_ = std::move(owned);
        result.status_ = DeviceBridgePlanStatus::Ready;
        result.reason_ = "Owned device create-info plan prepared; no Vulkan call was made";
    } catch (const Refusal& refusal) {
        result.status_ = refusal.status;
        result.reason_ = refusal.reason;
        result.refusedStructureType_ = refusal.structure;
    } catch (const std::bad_alloc&) {
        result.status_ = DeviceBridgePlanStatus::OutOfMemory;
        result.reason_ = "Unable to allocate an owned device create-info plan";
    } catch (...) {
        result.status_ = DeviceBridgePlanStatus::InvalidCreateInfo;
        result.reason_ = "Unable to copy bounded device creation parameters";
    }
    return result;
}

} // namespace xess_vk
