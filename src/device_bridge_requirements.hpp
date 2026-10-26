#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vulkan/vulkan_core.h>

namespace xess_vk {

// Evidence from completed queries for the SAME physical device being created.
// These are supported capabilities, not evidence of enabled device capabilities.
struct DeviceBridgeSupport {
    std::uint32_t instanceApiVersion = 0;
    std::uint32_t physicalDeviceApiVersion = 0;
    bool extensionQueryComplete = false;
    bool timelineFeatureQueryComplete = false;
    VkBool32 timelineSemaphore = VK_FALSE;
    std::span<const VkExtensionProperties> extensions;
};

enum class AllowBorrowedLoaderChain { No, SynchronousCreateDeviceCallOnly };
struct DeviceBridgePlanOptions {
    bool optIn = false;
    AllowBorrowedLoaderChain loaderChain = AllowBorrowedLoaderChain::No;
    bool calibratedTimestamps = false; // Optional diagnostics; absence never refuses the bridge.
};

enum class DeviceBridgePlanStatus {
    NotRequested, Ready, UnsupportedApi, CapabilityQueryMissing, MissingRequiredExtension,
    TimelineUnsupported, InvalidCreateInfo, UnsupportedPNext, ConflictingFeatureChain,
    BorrowedLoaderChainNotAllowed, OutOfMemory
};

class DeviceBridgePlan;
DeviceBridgePlan planDeviceBridgeRequirements(const VkDeviceCreateInfo& original,
    const DeviceBridgeSupport& support, DeviceBridgePlanOptions options = {}) noexcept;

// Moving this object preserves the addresses of its owned create-info and nodes.
// It never creates a device, calls a driver, or modifies caller-owned memory.
class DeviceBridgePlan {
public:
    DeviceBridgePlan() noexcept;
    ~DeviceBridgePlan();
    DeviceBridgePlan(DeviceBridgePlan&&) noexcept;
    DeviceBridgePlan& operator=(DeviceBridgePlan&&) noexcept;
    DeviceBridgePlan(const DeviceBridgePlan&) = delete;
    DeviceBridgePlan& operator=(const DeviceBridgePlan&) = delete;

    DeviceBridgePlanStatus status() const noexcept;
    bool ready() const noexcept;
    const char* reason() const noexcept;
    VkStructureType refusedStructureType() const noexcept;
    // Null for refusal, opt-out, or moved-from plans; never returns the original.
    const VkDeviceCreateInfo* createInfo() const noexcept;
    std::span<const std::string> addedExtensions() const noexcept;
    bool timelineFeatureChanged() const noexcept;

    // If true, union pointer targets in known VkLayerDeviceCreateInfo nodes are
    // borrowed from the loader. Use only for the immediate synchronous downstream
    // vkCreateDevice while the original call stack/payload remain alive. Do NOT
    // cache this plan or use its createInfo as a long-lived successful record.
    // The planner does not advance pLayerInfo or make layer dispatch decisions.
    bool requiresSynchronousLoaderCall() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    DeviceBridgePlanStatus status_ = DeviceBridgePlanStatus::NotRequested;
    const char* reason_ = "Bridge device requirements were not requested";
    VkStructureType refusedStructureType_ = VK_STRUCTURE_TYPE_MAX_ENUM;
    friend DeviceBridgePlan planDeviceBridgeRequirements(const VkDeviceCreateInfo&,
        const DeviceBridgeSupport&, DeviceBridgePlanOptions) noexcept;
};

} // namespace xess_vk
