#pragma once
#include "image_inventory.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vulkan/vulkan_core.h>

namespace xess_vk::resource_memory_facts {
using Handle=std::uint64_t;
struct Key {Handle handle{};std::uint64_t generation{};friend bool operator==(const Key&,const Key&)=default;};
enum class ResourceKind {Image,Buffer};
struct Resource {ResourceKind kind{ResourceKind::Image};Key key{};friend bool operator==(const Resource&,const Resource&)=default;};
enum class Issue:std::uint64_t {
    None=0,InvalidInput=1ull<<0,DriverFailed=1ull<<1,Capacity=1ull<<2,UnknownObject=1ull<<3,
    StaleGeneration=1ull<<4,Lifecycle=1ull<<5,UnsupportedResource=1ull<<6,UnsupportedAllocation=1ull<<7,
    UnsupportedBinding=1ull<<8,MissingRequirements=1ull<<9,InvalidRequirements=1ull<<10,
    OutOfBounds=1ull<<11,Alignment=1ull<<12,MemoryType=1ull<<13,DedicatedMismatch=1ull<<14,
    BindingUnknown=1ull<<15,MissingBinding=1ull<<16,Overlap=1ull<<17,CoverageIncomplete=1ull<<18,
    IsolationUnknown=1ull<<19,GenerationExhausted=1ull<<20,Stopped=1ull<<21,UnsupportedDeviceScope=1ull<<22,
    AliasGranularityUnknown=1ull<<23,AliasRangeOverflow=1ull<<24
};
constexpr std::uint64_t bit(Issue issue)noexcept{return static_cast<std::uint64_t>(issue);}
struct Result {Key key{};std::uint64_t issues{};bool accepted()const noexcept{return issues==0;}};
enum class DeviceScope {Unverified,SinglePhysicalDevice,DeviceGroup};
enum class BindingCoverage {NotEstablished,CompleteOrdinaryCalls};
// These are explicit adapter contracts, not facts discovered by this component.
// CompleteOrdinaryCalls covers all resource/memory creation, requirements,
// ordinary core/2/KHR binds, and retirement from device creation onward. Sparse
// or another unknown binding mechanism must be reported via coverageLost().
struct Config {
    std::size_t images{16384},buffers{16384},memories{16384},pendingBinds{32};
    DeviceScope deviceScope{DeviceScope::Unverified};
    BindingCoverage bindingCoverage{BindingCoverage::NotEstablished};
    VkDeviceSize bufferImageGranularity{}; // actual physical-device limit, zero means unknown
};
inline constexpr std::size_t maxBindElements=64,maxPendingBinds=32;
enum class BindApi {ImageCore,ImageCore2,ImageKhr2,BufferCore,BufferCore2,BufferKhr2};
struct BindTicket {std::uint32_t slot{};std::uint64_t generation{},issues{};bool tracked()const noexcept{return generation!=0;}};
struct ObjectUsage {std::size_t live{},highWater{},capacity{};std::uint64_t capacityRefusals{};};
struct Stats {
    std::size_t storageBytes{};
    ObjectUsage images{},buffers{},memories{},pendingBinds{};
    std::uint64_t registrations{},retirements{},requirementsObserved{},bindAttempts{},successfulBinds{},failedBinds{};
    std::uint64_t uncertainBatches{},refusals{},globalIssues{},queries{},overlapQueries{},bindingListSteps{};
    std::array<std::uint64_t,6> bindCalls{};
};
struct BindingFacts {
    VkDevice device{};Resource resource{};Key memory{};
    VkDeviceSize offset{},size{},end{}; // checked half-open [offset,end), actual requirements.size
    VkDeviceSize bufferImageGranularity{},paddedAliasBegin{},paddedAliasEnd{};
    bool linearResource{},paddedAliasRangeKnown{}; // padding matters for different linearity only
    std::uint64_t issues{};std::size_t overlapCount{};Resource firstOverlap{};
    bool bindingBoundsKnown{},observedAliasFree{};
    // Always false. CPU observations do not retain/pin Vulkan resources or
    // establish layout, ownership, synchronization, GPU work, or copy admission.
    bool resourceLifetimePinned{},ownerKnown{},gpuComplete{},copyPermitted{};
};

class Tracker {
public:
    // One fixed allocation at construction, none in methods. All methods and
    // destruction are serialized by the caller; do not hold that lock across
    // an original Vulkan call. The supplied device/physical memory properties
    // must be the actual single-physical-device creation facts for this context.
    Tracker(VkDevice,const VkPhysicalDeviceMemoryProperties&,Config={});~Tracker();
    Tracker(const Tracker&)=delete;Tracker& operator=(const Tracker&)=delete;
    // Base bookkeeping only; storageBytes() also includes allocated payload pages.
    static std::size_t allocatedStorageBytes()noexcept;
    std::size_t storageBytes() const noexcept;
    Stats stats()const noexcept;
    // Unknown loss that cannot be assigned to an exact memory generation is
    // permanent context failure. A localized failure retires with that memory.
    void coverageLost(Issue=Issue::BindingUnknown)noexcept;
    void memoryBindingUnknown(Key,Issue=Issue::BindingUnknown)noexcept;
    void stop()noexcept;

    // Successful ordinary vkCreateImage plus its exact common-inventory key.
    // inventory.created -> lookup -> this registration must share one metadata
    // stateLock transaction after the driver returns, in global creation order;
    // an identical same-key observation is idempotent. No second image serial.
    Result imageCreated(const image_inventory::Record&,const VkImageCreateInfo*,VkResult)noexcept;
    Result bufferCreated(VkBuffer,const VkBufferCreateInfo*,VkResult)noexcept;
    Result memoryAllocated(VkDeviceMemory,const VkMemoryAllocateInfo*,VkResult)noexcept;
    Result imageKey(VkImage)const noexcept;
    Result bufferKey(VkBuffer)const noexcept;
    Result memoryKey(VkDeviceMemory)const noexcept;
    // Actual successful driver query for this exact live resource generation;
    // ordinary whole-resource size/alignment/typeBits, never a size estimate or
    // a device-create requirement for a different image. Query2 adapters may
    // normalize its actual VkMemoryRequirements; unknown/plane queries refuse.
    Result requirementsObserved(Resource,const VkMemoryRequirements&)noexcept;
    void imageRetiring(Key)noexcept;
    void bufferRetiring(Key)noexcept;
    void memoryRetiring(Key)noexcept; // BEFORE forwarding vkFreeMemory

    // Snapshot keys/requirements before forwarding the exact original call.
    // Core adapters synthesize one ordinary info with null pNext. Both arrays
    // are copied now; no application pointer is retained across the call.
    // Unsupported bind pNext cannot be localized safely and fails the context.
    BindTicket beforeImageBind(BindApi,std::uint32_t,const VkBindImageMemoryInfo*)noexcept;
    BindTicket beforeBufferBind(BindApi,std::uint32_t,const VkBindBufferMemoryInfo*)noexcept;
    // Publish only after actual VK_SUCCESS, with no partially published batch.
    // Any failed bind2/KHR batch makes ALL its addressed memory/resource states
    // unknown: a failing result does not imply that every bind was ineffective.
    // A failed ordinary core call leaves its prior binding unchanged. Duplicate
    // or stale tickets do not publish or affect reused numeric handles.
    Result afterBind(BindTicket,VkResult)noexcept;
    BindingFacts query(Resource)const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace xess_vk::resource_memory_facts
