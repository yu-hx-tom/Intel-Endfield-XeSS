#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vulkan/vulkan_core.h>

namespace xess_vk::image_sync {
using Handle = std::uint64_t;
struct ImageKey {
    Handle image{};
    std::uint64_t generation{}; // successful-create inventory generation
    friend bool operator==(const ImageKey&, const ImageKey&) = default;
};
struct Candidate {
    ImageKey key;
    VkImageAspectFlags aspects{};
    std::uint32_t mipLevels{1}, arrayLayers{1};
    VkSharingMode sharingMode{VK_SHARING_MODE_EXCLUSIVE};
};
enum class Issue : std::uint64_t {
    None = 0, CoverageIncomplete = 1ull << 0, Capacity = 1ull << 1,
    InvalidLifecycle = 1ull << 2, NotSelectedAtBegin = 1ull << 3,
    PartialRange = 1ull << 4, OwnershipTransfer = 1ull << 5,
    ConcurrentSharing = 1ull << 6, LayoutChainMismatch = 1ull << 7,
    UnknownCommand = 1ull << 8, SecondaryCommands = 1ull << 9,
    RenderPass = 1ull << 10, Events = 1ull << 11,
    UnsupportedExtension = 1ull << 12, MultipleQueues = 1ull << 13,
    QueueFamilyMismatch = 1ull << 14, DeviceLost = 1ull << 15,
    StaleImageGeneration = 1ull << 16, SimultaneousUse = 1ull << 17,
    ReusedOneTimeCommand = 1ull << 18, InvalidLayout = 1ull << 19,
    Stopped = 1ull << 20, HostImageTransition = 1ull << 21,
    SubmitOrderAmbiguous = 1ull << 22
};
constexpr std::uint64_t bit(Issue value) noexcept { return static_cast<std::uint64_t>(value); }
struct Config {
    // Hard bounds cannot be raised by configuration. No per-call allocations.
    std::size_t candidates{16}, commandBuffers{512}, barriersPerCommand{64};
    bool synchronization2Enabled{};
    // Must remain false until every integration hook in the accompanying doc
    // is installed from device creation and every unsupported path is marked.
    bool completeHookCoverage{};
};
struct Barrier {
    ImageKey image;
    VkImageSubresourceRange range{};
    VkImageLayout oldLayout{VK_IMAGE_LAYOUT_UNDEFINED}, newLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    std::uint32_t srcQueueFamily{VK_QUEUE_FAMILY_IGNORED}, dstQueueFamily{VK_QUEUE_FAMILY_IGNORED};
    VkPipelineStageFlags2 srcStages{}, dstStages{};
    VkAccessFlags2 srcAccess{}, dstAccess{};
    bool synchronization2Command{};
    bool unsupportedExtension{}; // pNext/dependency flags not audited by adapter
};
struct Evidence {
    ImageKey image;
    bool selected{}, barrierSubmitted{}, submittedLayoutKnown{}, lastBarrierLayoutFieldsIgnored{};
    std::uint64_t issues{}, lastRecordingIssues{}, commandGeneration{}, submissionSerial{}, latestDeviceSubmission{};
    Handle command{}, queue{}, fence{};
    std::uint32_t queueFamily{VK_QUEUE_FAMILY_IGNORED};
    Barrier lastBarrier;
    VkImageLayout submittedLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    // These are deliberately never inferred from queue submission or barriers.
    bool gpuComplete{}, lastWriterKnown{}, resourceLifetimePinned{}, copyPermitted{};
};
struct Stats {
    std::uint64_t successfulSubmissions{}, failedSubmissions{}, recordedBarriers{}, ignoredBarriers{}, retiredOperationsSkipped{}, issues{};
    std::size_t selected{}, commands{};
};

// Device-scoped metadata only. Handles are never dereferenced, Vulkan is never
// called, and no lock or snapshot pins a Vulkan resource. Methods are serialized
// internally; the owner must additionally serialize Tracker destruction.
class Tracker {
public:
    static constexpr std::size_t maxCandidates = 16, maxCommands = 512, maxBarriers = 64;
    static std::size_t allocatedStorageBytes() noexcept;
    explicit Tracker(Config config = {});
    ~Tracker();
    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;
    bool select(const Candidate&);
    void deselect(ImageKey); // observation retirement only; image may still be alive
    void destroying(ImageKey); // before forwarding vkDestroyImage
    void allocate(Handle command, Handle pool, std::uint32_t queueFamily, VkCommandBufferLevel);
    void free(Handle command); // before forwarding free/pool destruction
    void resetPool(Handle pool, bool destroy); // successful reset, or before destroy
    void begin(Handle command, VkCommandBufferUsageFlags, VkResult); // after driver result
    void end(Handle command, VkResult);
    void reset(Handle command, VkResult);
    void barrier(Handle command, const Barrier&);
    void opaque(Handle command, Issue); // do not claim render-pass/secondary/event support
    void untrackedDeviceOperation(Issue); // host image transitions, bind-sparse, missed hooks
    // Flatten primary buffers in batch order only after successful forwarding.
    // A null fence is valid observation. It is NOT replaced, waited or queried.
    // Unsupported submit pNext/device-group/protected paths must set audited=false.
    void submit(Handle queue, std::uint32_t family, std::span<const Handle> commands,
                Handle fence, VkResult, bool audited = true);
    Evidence snapshot(ImageKey) const;
    Stats stats() const;
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xess_vk::image_sync
