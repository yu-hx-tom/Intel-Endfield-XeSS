#pragma once
#include "cpu_wall_time.hpp"
#include "wsi_acquire_observer.hpp"
#include "gpu_timestamp_sample.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan_core.h>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace xess_vk {
using QueueGate = std::recursive_mutex;
inline constexpr std::uint32_t frameSlotCount = 2;
inline constexpr std::uint32_t sharedPlaneSlotCount = 3;
struct SharedPlaneKey {
    std::uint32_t index = UINT32_MAX;
    std::uint64_t ringGeneration = 0, leaseGeneration = 0;
    bool valid() const noexcept {
        return index < sharedPlaneSlotCount && ringGeneration && leaseGeneration;
    }
    friend bool operator==(const SharedPlaneKey&, const SharedPlaneKey&) = default;
};
struct FrameSlotKey {
    std::uint32_t index = UINT32_MAX;
    std::uint64_t generation = 0;
    bool valid() const noexcept { return index < frameSlotCount && generation != 0; }
    friend bool operator==(const FrameSlotKey&, const FrameSlotKey&) = default;
};
// Caller-owned receipt: update immediately after each successful queue operation,
// including before a later SDK/Present operation can throw. Poison never proves idle.
struct FrameCompletion {
    FrameSlotKey key;
    // Valid only for direct shared publication; legacy shared sources are bound
    // by NativeFrame::sharedSourceKey to this receipt's actual publication.
    SharedPlaneKey sharedKey;
    bool sharedAuxiliary = false;
    std::uint64_t producedValue = 0, consumedValue = 0, sdkTailValue = 0;
    bool producedValid = false, consumedValid = false, sdkTailValid = false;
    bool d3dWaitQueued = false, callerSubmitted = false;
    bool postPresentSubmitted = false;
    bool poisoned = false;
};
enum class FrameCompletionState { Pending, Complete, Invalid, DeviceLost };
namespace detail {
inline bool freshFrameCompletion(const FrameCompletion& value) noexcept {
    return value.key.valid() && !value.producedValue && !value.consumedValue && !value.sdkTailValue &&
        !value.producedValid && !value.consumedValid && !value.sdkTailValid &&
        !value.d3dWaitQueued && !value.callerSubmitted && !value.postPresentSubmitted && !value.poisoned &&
        (value.sharedKey.valid() || (value.sharedKey == SharedPlaneKey{} && !value.sharedAuxiliary));
}
inline bool frameSlotCanReuse(FrameSlotKey previous, FrameSlotKey next, FrameCompletionState state) noexcept {
    return next.valid() && (!previous.valid() || (previous.index == next.index &&
        next.generation > previous.generation && state == FrameCompletionState::Complete));
}
}
enum class Format : unsigned { ColorRGBA8 = 0, DepthR32F = 1, MotionRG16F = 2 };
using FramePlanes = std::array<std::vector<std::uint8_t>, 3>;
struct InteropConfig {
    std::uint32_t width = 127;
    std::uint32_t height = 73;
    std::uint32_t timeoutMs = 10000;
    // Depth and motion share this fixed input extent. Both zero preserves the
    // original equal-resolution contract and three-value aggregate initializers.
    std::uint32_t inputWidth = 0;
    std::uint32_t inputHeight = 0;
    // Presentation takeover OFF mode only needs color. Other planes are neither
    // allocated nor published; this does not authorize frame generation.
    bool colorOnly = false;
    void validate() const;
    VkExtent2D extent(Format format) const;
    std::size_t byteSize(Format format) const;
};
struct TransferStats {
    std::uint64_t framesVerified = 0;
    std::uint64_t bytesVerified = 0;
    std::string adapter;
    std::uint64_t cpuUploadFrames = 0;
    std::uint64_t nativeImageFrames = 0;
    CpuWallTime nativePublishCpu;
};

// These are successful creation records, not a list of supported capabilities.
// Vulkan cannot query enabled device features/extensions after creation. Records
// and handles must come from the same actual successful caller creation calls.
struct BorrowedVulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex = UINT32_MAX;
    std::uint32_t queueIndex = 0;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    const VkInstanceCreateInfo* successfulInstanceCreateInfo = nullptr;
    const VkDeviceCreateInfo* successfulDeviceCreateInfo = nullptr;
    QueueGate* queueGate = nullptr; // Non-owning, device-owned; never held during GPU waits.
    wsi_acquire::QueueObserver queueObserver = nullptr;
    void* queueObserverContext = nullptr; // Same device lifetime as queueGate, including quarantine.
    bool gpuTimestamps = false; // Frozen device opt-in; no capability/query work when false.
};
struct NativeImageSource {
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageCreateInfo successfulImageCreateInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t ownerQueueFamily = UINT32_MAX;
};
struct NativeFrame {
    std::array<NativeImageSource, 3> planes;
    VkQueue producerQueue = VK_NULL_HANDLE;
    // Actual fence of the producer submission, not an initially signaled token.
    // The caller serializes all access and keeps the sources alive and unchanged
    // until publishNativeFrame returns successfully. It waits for GPU copy and
    // source-layout restoration; D3D12 consumption thereafter uses copies only.
    VkFence producerCompleteFence = VK_NULL_HANDLE;
    // Queued legacy fallback may read this reservation's GENERAL/Vulkan-owned
    // auxiliary images. Its lease remains pinned through the exact receipt.
    SharedPlaneKey sharedSourceKey;
};
struct SharedPlaneLease {
    SharedPlaneKey key;
    // Previous layout/owner for recording the next write, not a GPU completion
    // claim. Writers leave GENERAL/Vulkan-owned; direct publication releases it.
    std::array<NativeImageSource, 3> planes;
    std::uint32_t initializedMask = 0;
    bool auxiliary = false;
    bool depthStorage = false;
    // Precreated at the extent boundary; borrowed for exactly this lease.
    VkImageView depthStorageView = VK_NULL_HANDLE;
};
enum class SharedBorrowResult { Acquired, Unavailable, Invalid, DeviceLost };
struct SharedPlaneStats {
    bool available = false, ready = false, depthStorage = false;
    std::uint64_t ringGeneration = 0, resourceBytes = 0;
    std::uint64_t reservations = 0, noFreeFallbacks = 0, extentFallbacks = 0;
    std::uint64_t cancelled = 0, abandoned = 0, released = 0, poisoned = 0;
    std::uint64_t directFrames = 0, legacySourceFrames = 0;
    std::uint32_t leases = 0;
};

namespace detail {
// Pure structural rejection used before the real GPU waits/copies. It cannot
// authenticate handles, creation records, producer-fence provenance or lifetime;
// those remain caller contracts. Tests may use inert handles only in this helper.
void validateNativeFrameMetadata(const InteropConfig& config, const NativeFrame& frame,
                                VkDevice expectedDevice, VkQueue expectedQueue,
                                std::uint32_t expectedQueueFamily);
}

// Single-slot, same-adapter Vulkan -> D3D12 bridge. This is not a Vulkan WSI hook.
// Each plane contains config.byteSize(format) bytes, with tightly packed rows.
// Color uses output width/height; depth and motion use the optional input extent.
// These copies do not rescale inputs or validate motion-vector meaning for XeFG.
// Vulkan uploads into imported images, then releases ownership and signals a
// shared D3D12 fence. The caller must enqueue its reads between queueWaitForProduced
// and queueSignalConsumed, returning every shared texture to D3D12 COMMON first.
// Calls and externally submitted queue work must be serialized by the caller.
class InteropBridge {
public:
    explicit InteropBridge(InteropConfig config = {});
    InteropBridge(InteropConfig config, const BorrowedVulkanContext& borrowed);
    ~InteropBridge();
    InteropBridge(const InteropBridge&) = delete;
    InteropBridge& operator=(const InteropBridge&) = delete;
    ID3D12Device* device() const;
    ID3D12CommandQueue* queue() const;
    ID3D12Resource* texture(Format format) const;
    std::uint64_t frameValue() const;
    const InteropConfig& config() const;
    const TransferStats& stats() const;
    void associateGpuTimestampFrame(const GpuTimestampSample& identity) noexcept;
    GpuTimestampSample gpuTimestampSample() const noexcept;
    void publishFrame(const FramePlanes& planes);
    void publishFrame(const std::array<std::span<const std::uint8_t>, 3>& planes);
    void publishPattern(std::uint64_t patternFrame);
    // Available only for a borrowed context. GPU image-to-image copy; no staging
    // upload. Restores each source layout and preserves its queue-family owner.
    // CPU-blocking first version: finite producer and produced-fence waits.
    void publishNativeFrame(const NativeFrame& frame);
    // Explicit asynchronous publication of three persistent private planes on
    // the same serialized queue. The real producer submission fence may still
    // be pending. Keep every source and its exact lease alive until consumed;
    // later same-queue accesses follow the queued reads/restoration barriers.
    // Current producer/produced CPU waits are skipped; previous reuse still waits.
    void publishQueuedNativeFrame(const NativeFrame& frame);
    void publishQueuedNativeFrame(const NativeFrame& frame, FrameSlotKey, FrameCompletion&);
    // Called only at a lifecycle/extent boundary. A mismatched ring with live
    // leases is paused and retained; no new allocation occurs at record time.
    bool prepareSharedPlanes(std::uint32_t inputWidth = 0, std::uint32_t inputHeight = 0);
    SharedBorrowResult tryReserveSharedFrame(VkExtent2D expectedInputExtent, bool auxiliary,
                                            SharedPlaneLease&) noexcept;
    // Actual successful same-queue writes only: bit0=color, bit1=depth, bit2=MV.
    void sharedWriteSubmitted(SharedPlaneKey, std::uint32_t planeMask);
    bool cancelRecordedSharedFrame(SharedPlaneKey) noexcept;
    bool abandonSubmittedSharedFrame(SharedPlaneKey, VkFence actualCompletedSameQueueFence) noexcept;
    void publishQueuedSharedFrame(SharedPlaneKey, VkQueue, VkFence producerSubmissionFence,
                                  FrameSlotKey, FrameCompletion&);
    // Native must first prove this exact receipt's full SDK tail. Bridge also
    // checks its bound publication and matching consumed value, without waiting.
    bool releaseSharedFrame(SharedPlaneKey, const FrameCompletion&) noexcept;
    void poisonSharedFrame(SharedPlaneKey) noexcept;
    SharedPlaneStats sharedPlaneStats() const noexcept;
    ID3D12Resource* texture(Format, SharedPlaneKey) const;
    InteropConfig config(SharedPlaneKey) const;
    // Add or resize auxiliary planes after the last published GPU work completed.
    // Keeps color, both devices, shared fences and the borrowed queue intact.
    void enableAuxiliaryInputs(std::uint32_t width, std::uint32_t height);
    // Replace size-dependent planes after their actual GPU consumers complete.
    // Preserves both devices, queues, shared fences and their monotonic values.
    void resizeOutput(std::uint32_t width, std::uint32_t height);
    void publishNativeColor(const NativeImageSource& color, VkQueue producerQueue, VkFence producerCompleteFence);
    // Takeover OFF only: persistent private TRANSFER_SRC color whose producer
    // is already submitted on this same serialized queue. No current-fence or
    // produced CPU wait; keep the source alive until waitUntilConsumed (or
    // completed cleanup), and order later GPU writes after these read barriers.
    void publishQueuedNativeColor(const NativeImageSource& color, VkQueue producerQueue, VkFence producerSubmissionFence);
    void publishQueuedNativeColor(const NativeImageSource&, VkQueue, VkFence, FrameSlotKey, FrameCompletion&);
    void queueWaitForProduced();
    void queueWaitForProduced(FrameCompletion&);
    void queueSignalConsumed();
    void queueSignalConsumed(FrameCompletion&);
    // Direct shared resources are restored to COMMON on the SDK queue after
    // their matching Present; the legacy pre-Present signal rejects this key.
    void queueSignalSharedConsumed(SharedPlaneKey, FrameCompletion&);
    void waitUntilConsumed();
    FrameCompletionState consumedComplete(std::uint64_t value) const noexcept;
    // Enqueues real D3D12 copies, signals consumed, waits and compares all bytes.
    // Invoke after publish; it performs both queueWait and queueSignal itself.
    void verifyFrame(const FramePlanes& expected);
    void verifyPattern(std::uint64_t patternFrame);
    static FramePlanes makePattern(const InteropConfig& config, std::uint64_t frame);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
