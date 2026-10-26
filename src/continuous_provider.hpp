#pragma once
#include "command_recording_facts.hpp"
#include "descriptor_image_metadata.hpp"
#include "interop.hpp"
#include "upscale_frame_constants.hpp"
#include "frame_timing.hpp"
#include <array>
#include <memory>
#include <span>

namespace xess_vk::continuous {
inline constexpr std::size_t slotCount = 3;
inline constexpr std::size_t maximumSignals = 16;
inline constexpr std::size_t invocationCapacity = 16;
enum class State { Free, Recorded, Submitted, Consuming, Uncertain };
struct SourcePair {
    descriptor_image_metadata::Key depth{}, motion{};
    std::uint64_t invocation{};
    std::uint32_t thread{};
    frame_input::FrameConstantsSample constants;
    FrameTimingTicket timing;
    std::uint32_t engineFrame{};
    bool frameValid{};
    bool valid{};
};
struct RecordingKey {
    std::uint64_t command{}, generation{};
    friend bool operator==(const RecordingKey &, const RecordingKey &) = default;
};
struct Ticket {
    std::uint32_t slot{};
    std::uint64_t generation{};
    bool shared{};
    friend bool operator==(const Ticket &, const Ticket &) = default;
};
struct Bundle {
    Ticket ticket;
    NativeImageSource depth, motion;
    frame_input::FrameConstantsSample constants;
    FrameTimingTicket timing;
    std::uint32_t engineFrame{};
    std::uint64_t invocation{}, submission{};
    SharedPlaneLease shared;
    bool ready{},depthCompute{};
    measurement::Commands commands{};
};
struct Stats {
    std::uint64_t invocations{}, rejectedInvocations{}, matchedDispatches{}, captured{}, skippedOffCaptures{},
        saturated{};
    std::uint64_t submissions{}, presentMatches{}, missingPresent{}, ambiguousPresent{}, consumed{},
        cancelled{};
    std::uint64_t missingInvocationFrame{}, invocationCapacityRefusals{}, missingRecordingFrame{},
        unmatchedDispatches{}, ambiguousDispatches{}, invalidRecording{}, invalidSources{}, copyFailures{};
    std::array<State, slotCount> states{};
    std::array<State, sharedPlaneSlotCount> sharedStates{};
    std::uint64_t sharedCaptures{}, sharedFallbacks{}, sharedFailures{};
    std::uint64_t depthComputeCaptures{},depthTransferCaptures{},depthComputeFallbacks{},depthComputeFailures{};
    std::uint64_t timestampFailures{},timestampUnavailable{},timestampCompleted{},submissionIntervalResets{};
    GpuTimestampSample depthGpu,auxGpu;
};
// Concrete borrowed shared-input operations; Provider never owns a D3D device.
struct SharedInputAccess {
    void* context{};
    SharedBorrowResult (*reserve)(void*,VkExtent2D,SharedPlaneLease&) noexcept{};
    void (*submitted)(void*,SharedPlaneKey,std::uint32_t){};
    bool (*cancel)(void*,SharedPlaneKey) noexcept{};
    bool (*abandon)(void*,SharedPlaneKey,VkFence) noexcept{};
    bool (*release)(void*,SharedPlaneKey,const FrameCompletion&) noexcept{};
    void (*poison)(void*,SharedPlaneKey) noexcept{};
};
struct Config {
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queueFamily{};
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkGetDeviceProcAddr functions{};
    bool separateDepthStencilLayouts{};
    bool gpuTimestamps{};
    float timestampPeriodNs{};
    std::uint32_t timestampValidBits{};
    bool depthCompute{}; // Independent opt-in from shared input borrowing.
};
class Provider {
  public:
    explicit Provider(Config);
    ~Provider();
    // Clearing disables new borrows immediately; false retains a live context.
    bool setSharedInputs(SharedInputAccess);
    // Present-boundary allocation only; record() falls back if a carrier is not ready.
    bool prepareSharedInputs(VkExtent2D,bool depthStorage = false);
    // Keep bounded immutable callback records. Their actual frame/ticket and
    // resource generations identify a dispatch across recording threads.
    void invocation(SourcePair);
    bool record(std::uint32_t thread, const CommandRecordingFacts &,
                const descriptor_image_metadata::DispatchEvidence &, FrameTimingAssociation recordingFrame,
                bool captureInputs = true);
    // A real completed present retires unused records through its frame key.
    void retireFrame(FrameTimingAssociation);
    bool containsRecording(RecordingKey) const;
    void submitted(RecordingKey, std::span<const VkSemaphore> signals, VkQueue,
                   std::uint64_t submission, VkResult,std::uint64_t entryNs=0,std::uint64_t entryQpcTicks=0);
    // Snapshot before the private submit; only pre-cutoff captures are covered.
    // Later successful submissions cannot borrow that copy fence's proof.
    std::uint64_t submittedCutoff(VkQueue) const;
    // Only called after the private present-copy fence completed on this queue.
    // An exact ticket/frame selects one completed producer across semaphore
    // forwarding. Without a ticket, retain the original unique-wait OFF path.
    Bundle acquireForPresent(VkQueue, std::span<const VkSemaphore>, VkFence completedCopyFence,
                             std::uint64_t submittedCutoff, FrameTimingAssociation presentFrame = {});
    // Exact same-queue queued use only. This pins a successful pre-cutoff
    // submission; it does not prove GPU completion or recycle any other slot.
    Bundle acquireQueuedForPresent(VkQueue, std::uint64_t submittedCutoff, FrameTimingAssociation);
    // Only before a Bridge publication can have read this reservation.
    // Return it to Submitted, retaining every resource and synchronization identity.
    void rollbackQueuedAcquire(Ticket);
    void retireCompletedForPresent(VkQueue, VkFence, std::uint64_t submittedCutoff, FrameTimingAssociation);
    // Real Bridge source reads/restoration completed: synchronous return, or
    // successful queued publication followed by its actual consumed completion.
    bool releaseAfterBridge(Ticket,const FrameCompletion* = nullptr);
    void retireUnsubmitted(RecordingKey);
    void retirePool(std::uint64_t pool);
    VkResult stop(VkFence actualQueueDrainFence);
    Stats stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool requested() noexcept;
} // namespace xess_vk::continuous
