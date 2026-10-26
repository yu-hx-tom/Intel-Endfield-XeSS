#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vulkan/vulkan_core.h>

// One borrowed recording/capture lease. Callers serialize the methods and keep
// the actual application's externally synchronized queue-call window open
// across preSubmit, its unmodified original call, and afterOriginalResult.
namespace xess_vk::borrowed_capture_completion {
inline constexpr std::size_t maxMembership=64;
struct RecordingKey {
    VkDevice device{};
    std::uint64_t contextIncarnation{};
    VkCommandBuffer command{};
    std::uint64_t rpRecordingGeneration{};
    friend bool operator==(const RecordingKey&,const RecordingKey&)=default;
};
enum class Mode { Readback, FenceMarkerOnly };
enum class Reason : std::uint64_t {
    None=0, InvalidInput=1ull<<0, MissingFunction=1ull<<1,
    FenceNotUnsignaled=1ull<<2, UnsupportedRecording=1ull<<3,
    UnsupportedOwnerRecipe=1ull<<4, WrongState=1ull<<5,
    UnknownQueue=1ull<<6, WrongQueue=1ull<<7,
    UnknownMembership=1ull<<8, MembershipCapacity=1ull<<9,
    DuplicateRecording=1ull<<10, StaleTicket=1ull<<11,
    SubmissionInProgress=1ull<<12, OriginalFailed=1ull<<13,
    PrivateSubmitFailed=1ull<<14, DeviceLost=1ull<<15,
    FenceFailed=1ull<<16, Pending=1ull<<17,
    OneTimeReplay=1ull<<18, RetiredRecording=1ull<<19,
    GenerationExhausted=1ull<<20, OwnerNotReleasable=1ull<<21,
    ApplicationFenceAliasesPrivate=1ull<<22, SourceRecordingFailed=1ull<<23,
    DeviceTerminated=1ull<<24
};
constexpr std::uint64_t bit(Reason value) noexcept {return static_cast<std::uint64_t>(value);}
struct Result {
    std::uint64_t reasons{};
    VkResult driver{VK_SUCCESS};
    bool accepted() const noexcept {return reasons==0;}
};
struct PreparedOwner {
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queueFamily{VK_QUEUE_FAMILY_IGNORED};
    VkImage image{};
    VkBuffer staging{};
    VkCommandBuffer readbackCommand{};
    VkFence completionFence{};
    PFN_vkQueueSubmit originalQueueSubmit{};
    PFN_vkGetFenceStatus originalFenceStatus{};
    Mode mode{Mode::Readback};
    // Facts established by the preparer from real allocations/recording. The
    // module neither creates them nor verifies their contents from handles.
    bool singlePhysicalDeviceKnown{},ordinaryUnprotectedQueueKnown{};
    bool uniqueExternallySynchronizedQueueKnown{};
    bool imageAndStagingOwned{},readbackExecutableKnown{},readbackSameFamilyKnown{};
    bool readbackWritesStagingOnce{},readbackRestoresOwnedFinalLayout{};
    bool transferToHostBarrierKnown{},hostCoherenceResponsibilityKnown{};
    bool inlineTargetStartsUndefined{},inlineTargetRestoresFinalLayout{};
    bool inlineSourceRestoresOriginalLayout{};
};
struct RecordingAdmission {
    RecordingKey key{};
    std::uint64_t poolGeneration{};
    std::uint32_t queueFamily{VK_QUEUE_FAMILY_IGNORED};
    VkCommandBufferLevel level{VK_COMMAND_BUFFER_LEVEL_MAX_ENUM};
    VkCommandBufferUsageFlags actualBeginFlags{};
    bool allocationKnown{},poolKnown{},beginFlagsKnown{},currentlyRecordingKnown{};
    bool outsideRenderingKnown{},ordinaryUnprotectedPoolKnown{},controlSupported{};
};
struct QueueWindow {
    VkQueue queue{};
    std::uint32_t queueFamily{VK_QUEUE_FAMILY_IGNORED};
    bool queueKnown{},externallySynchronizedWindowKnown{};
};
struct SubmitObservation {
    // The caller routes observations from THIS context's VkDevice, including
    // unrelated command buffers on another queue of that same device.
    QueueWindow window{};
    // A complete, caller-owned normalization of all original CB occurrences,
    // not only the captured CB. Ordinary VkTimelineSemaphoreSubmitInfo is
    // allowed by caller normalization. Unknown/device-group/protected structure
    // must make structureKnown false. This span is never retained.
    std::span<const RecordingKey> completeMembership{};
    VkFence applicationFence{};
    bool structureKnown{};
};
struct SubmitTicket {
    std::uint64_t contextSerial{},callSerial{};
    std::uint64_t reasons{};
    QueueWindow window{};
    VkFence applicationFence{};
    std::array<RecordingKey,maxMembership> membership{};
    std::size_t membershipCount{};
    bool relevant{},associationKnown{};
};
enum class ReleaseDisposition { Retain, DiscardRetiredNeverSubmitted, ReleaseCompleted, DropAfterDeviceTerminated };
struct Status {
    RecordingAdmission borrowed{};
    std::uint64_t reasons{},successfulOriginalSubmissions{},replays{};
    bool initialized{},armed{},sourceCommandsMayExist{},recordingRetired{};
    bool originalCallInProgress{},privateCallInProgress{},privateSubmitSucceeded{};
    bool ownFenceSuccessObserved{},readbackReady{},readbackConsumed{};
    bool uncertain{},deviceTerminated{},ownerReleased{};
    ReleaseDisposition release{ReleaseDisposition::Retain};
};
struct Stats {
    std::uint64_t admissionRefusals{},matchingOriginalCalls{},originalFailures{};
    std::uint64_t privateSubmitCalls{},privateSubmitSuccesses{},privateSubmitFailures{};
    std::uint64_t fenceQueries{},pendingFenceQueries{},fenceCompletions{};
    std::uint64_t replaySubmissions{},associationRefusals{},retirements{},releases{};
};
class Context final {
public:
    Context() noexcept;
    Context(const Context&)=delete;
    Context& operator=(const Context&)=delete;
    // Queries the real private fence once; only NOT_READY is an initial state.
    Result initialize(const PreparedOwner&) noexcept;
    // Must be called BEFORE the first source vkCmd insertion. flags0 and
    // ONE_TIME_SUBMIT are allowed; primary/known ordinary family is required.
    Result armBeforeSourceCommands(const RecordingAdmission&) noexcept;
    Result sourceCommandsRecorded() noexcept;
    // Partial/unknown command emission is retained, never advertised as zero.
    Result sourceRecordingFailed(bool anyCommandsMayHaveBeenIssued) noexcept;
    SubmitTicket preSubmit(const SubmitObservation&) noexcept;
    // Caller has already executed its original exactly once, with unchanged
    // arrays/fence. A successful exact association appends one private submit
    // using the supplied UNHOOKED original, inside the same queue-call window.
    // Its Result describes observer/private work. The app wrapper MUST return
    // its saved original VkResult, even if the private append fails.
    Result afterOriginalResult(const SubmitTicket&,VkResult,const QueueWindow&) noexcept;
    Result pollOwnCompletion() noexcept;
    // Actual owner invalidation/host read/consumption must precede this event.
    Result readbackConsumed() noexcept;
    void observeRecordingRetired(const RecordingKey&) noexcept;
    // Observation only: these NEVER call vkQueueWaitIdle/vkDeviceWaitIdle.
    void observeQueueIdle(VkQueue,VkResult) noexcept;
    void observeDeviceIdle(VkDevice,VkResult) noexcept;
    void observeDeviceTerminated(VkDevice) noexcept;
    ReleaseDisposition releaseDisposition() const noexcept;
    // Metadata transition only, no reset/free/destroy/cancel Vulkan operation.
    Result markOwnerReleased() noexcept;
    Status status() const noexcept;
    Stats stats() const noexcept {return stats_;}
private:
    Result refuse(Reason,VkResult=VK_SUCCESS,bool uncertain=false) noexcept;
    bool exactWindow(const QueueWindow&) const noexcept;
    PreparedOwner owner_{};
    RecordingAdmission borrowed_{};
    SubmitTicket activeTicket_{};
    Stats stats_{};
    std::uint64_t contextSerial_{},callSerial_{},activeCallSerial_{},reasons_{};
    std::uint64_t successfulOriginals_{},replays_{},idleAfterSubmission_{};
    bool initialized_{},armed_{},sourceCommands_{},retired_{},originalInProgress_{};
    bool privateInProgress_{},privateAttempted_{},privateSucceeded_{},ownFenceSuccess_{};
    bool consumed_{},uncertain_{},terminated_{},released_{},idleKnown_{};
};
} // namespace xess_vk::borrowed_capture_completion
