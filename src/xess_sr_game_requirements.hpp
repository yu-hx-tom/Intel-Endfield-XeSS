#pragma once
#include "device_bridge_requirements.hpp"
#include <xess/xess_vk.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace xess_vk::sr_game {
struct RuntimeIdentity {
    VkInstance instance{};
    VkPhysicalDevice physicalDevice{};
    VkDevice device{};
    std::uint64_t incarnation{};
    friend bool operator==(const RuntimeIdentity&,const RuntimeIdentity&)=default;
};
struct RequiredQueries {
    decltype(&::xessVKGetRequiredInstanceExtensions) instanceExtensions{};
    decltype(&::xessVKGetRequiredDeviceExtensions) deviceExtensions{};
    decltype(&::xessVKGetRequiredDeviceFeatures) deviceFeatures{};
    friend bool operator==(const RequiredQueries&,const RequiredQueries&)=default;
};
struct DeviceSupportQueries {
    // Resolve from the same instance as the SDK requirement query. These execute
    // only for an opted-in device plan, on its recorded physical device.
    PFN_vkEnumerateDeviceExtensionProperties extensions{};
    PFN_vkGetPhysicalDeviceFeatures2 features{};
};
enum class RequirementsStatus {
    Unqueried, NotRequested, InstanceReady, Ready, InvalidInput, QueryFailed, UnsupportedFeatureStructure,
    InvalidFeatureChain, InvalidBoolean, IdentityMismatch, CreationFailed,
    InstanceApiTooLow, MissingInstanceExtension, MissingDeviceExtension,
    MissingEnabledFeature, UnsupportedApplicationChain, BorrowedLoaderChainNotAllowed,
    CapabilityQueryMissing, CapabilityQueryFailed, RequiredExtensionUnsupported, RequiredFeatureUnsupported, OutOfMemory
};
class RuntimeRequirementsAdmission;
class InstanceRequirementsPlan;
class DeviceRequirementsPlan;
class RuntimeRequirements {
public:
    RuntimeRequirements() noexcept;
    ~RuntimeRequirements();
    RuntimeRequirements(RuntimeRequirements&&) noexcept;
    RuntimeRequirements& operator=(RuntimeRequirements&&) noexcept;
    RuntimeRequirements(const RuntimeRequirements&)=delete;
    RuntimeRequirements& operator=(const RuntimeRequirements&)=delete;
    bool queried() const noexcept;
    bool instanceQueried() const noexcept;
    RequirementsStatus status() const noexcept;
    const char* reason() const noexcept;
    std::uint32_t minimumApiVersion() const noexcept;
    std::span<const std::string> instanceExtensions() const noexcept;
    std::span<const std::string> deviceExtensions() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RequirementsStatus status_=RequirementsStatus::Unqueried;
    const char* reason_="Runtime SDK requirements have not been queried";
    friend RuntimeRequirements queryRuntimeRequirements(const RequiredQueries&,VkInstance,VkPhysicalDevice) noexcept;
    friend RuntimeRequirements queryInstanceRequirements(const RequiredQueries&) noexcept;
    friend RuntimeRequirements queryDeviceRequirements(RuntimeRequirements&&,VkInstance,VkPhysicalDevice) noexcept;
    friend RuntimeRequirements queryDeviceRequirements(const RuntimeRequirements&,VkInstance,VkPhysicalDevice) noexcept;
    friend InstanceRequirementsPlan planInstanceRequirements(const RuntimeRequirements&,const VkInstanceCreateInfo&,bool) noexcept;
    friend DeviceRequirementsPlan planDeviceRequirements(const RuntimeRequirements&,const VkDeviceCreateInfo&,bool,AllowBorrowedLoaderChain,DeviceSupportQueries) noexcept;
    friend RuntimeRequirementsAdmission admitRuntimeRequirements(const RuntimeRequirements&,const RuntimeIdentity&,
        const VkInstanceCreateInfo&,const VkDeviceCreateInfo&,VkResult,VkResult) noexcept;
};

// Calls only the three supplied SDK requirements APIs; creates no Vulkan object.
// Every SDK-owned name and feature boolean is copied before returning. Keep the
// SDK module loaded and use the same exports for the admitted context's lifetime.
RuntimeRequirements queryRuntimeRequirements(const RequiredQueries&,VkInstance,VkPhysicalDevice) noexcept;
// Creation path: query instance requirements BEFORE vkCreateInstance, then query
// the selected device requirements BEFORE vkCreateDevice. The combined function
// above is also useful to compare an already-created device's recorded facts.
RuntimeRequirements queryInstanceRequirements(const RequiredQueries&) noexcept;
RuntimeRequirements queryDeviceRequirements(RuntimeRequirements&&,VkInstance,VkPhysicalDevice) noexcept;
// Reuse the pre-instance SDK snapshot for each selected physical device.
RuntimeRequirements queryDeviceRequirements(const RuntimeRequirements&,VkInstance,VkPhysicalDevice) noexcept;

// Both plans are opt-in and create no Vulkan object. Their changed metadata and
// feature nodes are owned at stable heap addresses. Unchanged application-name,
// layer, queue and allocator payloads remain borrowed for one synchronous call.
class InstanceRequirementsPlan {
public:
    InstanceRequirementsPlan() noexcept;
    ~InstanceRequirementsPlan();
    InstanceRequirementsPlan(InstanceRequirementsPlan&&) noexcept;
    InstanceRequirementsPlan& operator=(InstanceRequirementsPlan&&) noexcept;
    InstanceRequirementsPlan(const InstanceRequirementsPlan&)=delete;
    InstanceRequirementsPlan& operator=(const InstanceRequirementsPlan&)=delete;
    bool ready() const noexcept;
    RequirementsStatus status() const noexcept;
    const char* reason() const noexcept;
    const VkInstanceCreateInfo* createInfo() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RequirementsStatus status_=RequirementsStatus::NotRequested;
    const char* reason_="Instance requirements plan was not requested";
    friend InstanceRequirementsPlan planInstanceRequirements(const RuntimeRequirements&,const VkInstanceCreateInfo&,bool) noexcept;
};
class DeviceRequirementsPlan {
public:
    DeviceRequirementsPlan() noexcept;
    ~DeviceRequirementsPlan();
    DeviceRequirementsPlan(DeviceRequirementsPlan&&) noexcept;
    DeviceRequirementsPlan& operator=(DeviceRequirementsPlan&&) noexcept;
    DeviceRequirementsPlan(const DeviceRequirementsPlan&)=delete;
    DeviceRequirementsPlan& operator=(const DeviceRequirementsPlan&)=delete;
    bool ready() const noexcept;
    RequirementsStatus status() const noexcept;
    const char* reason() const noexcept;
    const VkDeviceCreateInfo* createInfo() const noexcept;
    bool requiresSynchronousLoaderCall() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RequirementsStatus status_=RequirementsStatus::NotRequested;
    const char* reason_="Device requirements plan was not requested";
    friend DeviceRequirementsPlan planDeviceRequirements(const RuntimeRequirements&,const VkDeviceCreateInfo&,bool,AllowBorrowedLoaderChain,DeviceSupportQueries) noexcept;
};
InstanceRequirementsPlan planInstanceRequirements(const RuntimeRequirements&,const VkInstanceCreateInfo&,bool optIn=false) noexcept;
DeviceRequirementsPlan planDeviceRequirements(const RuntimeRequirements&,const VkDeviceCreateInfo&,bool optIn=false,
    AllowBorrowedLoaderChain=AllowBorrowedLoaderChain::No,DeviceSupportQueries={}) noexcept;

class RuntimeRequirementsAdmission {
public:
    bool admitted() const noexcept {return status_==RequirementsStatus::Ready;}
    bool matches(const RuntimeIdentity& identity) const noexcept {return admitted()&&identity_==identity;}
    bool matchesQueries(const RequiredQueries& queries) const noexcept {return admitted()&&queries_==queries;}
    RequirementsStatus status() const noexcept {return status_;}
    const char* reason() const noexcept {return reason_;}
private:
    RequirementsStatus status_=RequirementsStatus::Unqueried;
    const char* reason_="Runtime SDK requirements have not been admitted";
    RuntimeIdentity identity_{};
    RequiredQueries queries_{};
    friend RuntimeRequirementsAdmission admitRuntimeRequirements(const RuntimeRequirements&,const RuntimeIdentity&,
        const VkInstanceCreateInfo&,const VkDeviceCreateInfo&,VkResult,VkResult) noexcept;
};

// Supply the effective parameters of the exact successful creation calls, while
// their borrowed chains are alive. Supported capabilities are not enabled facts.
// This only checks requirements; it neither merges chains nor modifies the app.
RuntimeRequirementsAdmission admitRuntimeRequirements(const RuntimeRequirements&,const RuntimeIdentity&,
    const VkInstanceCreateInfo&,const VkDeviceCreateInfo&,VkResult instanceResult,VkResult deviceResult) noexcept;
} // namespace xess_vk::sr_game
