#pragma once
#include "command_recording_facts.hpp"
#include "frame_timing.hpp"
#include "gpu_timestamp_sample.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace xess_vk::sr_gpu_timestamps {
inline constexpr std::size_t recordingCapacity=16,passCapacity=8,submitCapacity=128;
enum class Pass {Unknown,Accumulate,Rcas,WholeFsr,XeSS};
enum class Completion {RecordingRetired,SequentialReplay,DeviceIdle};
enum class Skip : std::size_t {Disabled,Identity,Recording,RecordingCapacity,PassCapacity,
    CreateFailed,OpenPass,StaleTicket,Submission,UnknownAssociation,Count};
struct Functions {
    PFN_vkCreateQueryPool create{};PFN_vkDestroyQueryPool destroy{};
    PFN_vkCmdResetQueryPool reset{};PFN_vkCmdWriteTimestamp write{};PFN_vkGetQueryPoolResults read{};
};
struct Identity {
    std::uint64_t window{},ordinal{},callback{},pipeline{},pipelineGeneration{};
    Pass pass{Pass::Unknown};
    FrameTimingAssociation frame;
    VkExtent2D render{},output{};
    std::array<std::uint32_t,3> groups{};
    // Supplied from the actual matched callback/resource facts, never inferred
    // from quality labels, dispatch groups, or a presumed descriptor role.
    bool extentsKnown{};
    bool nativeCoreSuppressed{}; // Same bracket, explicitly not native FSR GPU cost.
};
struct Sample {
    CommandRecordingFacts recording;
    Identity identity;
    GpuTimestampSample gpu;
    std::uint64_t execution{},queue{};
    Completion completion{Completion::RecordingRetired};
    bool replay{};
    // The interval brackets a candidate pass; it is not isolated shader cost,
    // whole-FSR coverage, final-writer evidence, or calibrated GPU/QPC time.
};
using Sink=void(*)(void*,const Sample&) noexcept;
struct Config {
    VkDevice device{};
    std::uint64_t incarnation{},window{};
    std::uint32_t queueFamily{VK_QUEUE_FAMILY_IGNORED},validBits{};
    VkQueueFlags queueFlags{};
    double periodNs{};
    Functions functions;
    Sink sink{};void* sinkUser{};
    bool enabled{};
};
struct WriteTicket {
    std::uint64_t recordingSerial{},passSerial{};
    std::uint32_t slot{},pass{};
    explicit operator bool() const noexcept{return recordingSerial&&passSerial;}
};
struct SubmitTicket {
    struct Entry {std::uint64_t recordingSerial{};std::uint32_t slot{};};
    std::uint64_t serial{},queue{},submitQpcTicks{};
    std::size_t count{};
    std::array<Entry,recordingCapacity> entries{};
};
struct Stats {
    std::array<std::uint64_t,std::size_t(Skip::Count)> skipped{};
    std::uint64_t rejectedRecordingReasons{};
    std::uint64_t recordings{},passes{},creates{},destroys{},successfulExecutions{},replays{},failedSubmits{},
        reads{},samples{},unavailable{},durationUnknown{},retired{},discardedUnsubmitted{},retainedAtStop{};
};

// One opt-in SrObservation window on one ordinary compute-capable family.
// Caller serializes all methods, preserves its normal queue-call serialization,
// and calls original Vulkan APIs with unchanged arguments/results. Functions
// must be UNHOOKED. The sink must not re-enter this object. No method submits,
// waits, polls an application fence, or resets an application command buffer.
class Observer final {
    struct Pair {Identity identity;std::uint64_t serial{};bool ended{},suppressed{};};
    struct Recording {
        VkQueryPool pool{};
        CommandRecordingFacts facts;
        std::uint64_t serial{},executions{},readExecution{},queue{},submitQpcTicks{},inFlight{};
        std::size_t count{};
        std::array<Pair,passCapacity> pairs{};
        bool associationLost{},executableObserved{};
    };
    Config config_;
    std::array<Recording,recordingCapacity> recordings_{};
    Stats stats_;
    std::uint64_t serial_{},passSerial_{},submitSerial_{},qpcFrequency_{};
    bool active_{};
    void skip(Skip) noexcept;
    void collect(Recording&,Completion) noexcept;
    void release(Recording&) noexcept;
    void loseAssociations() noexcept;
public:
    explicit Observer(const Config&) noexcept;
    Observer(const Observer&)=delete;
    Observer& operator=(const Observer&)=delete;
    // Deliberately no Vulkan destruction in the destructor. Keep the owner
    // alive until drained(), or releaseForDeviceDestruction succeeds.
    WriteTicket beforeDispatch(const CommandRecordingFacts&,const Identity&) noexcept;
    void afterDispatch(WriteTicket,bool emitSample=true) noexcept;
    bool finishSpan(WriteTicket,const Identity& terminal) noexcept;
    // Complete membership is required, including unrelated command buffers.
    // For a legal non-SIMULTANEOUS replay, the previous execution is already
    // nonpending at this invocation. Read it BEFORE the new original submit;
    // after that call an old available value could masquerade as the new run.
    SubmitTicket beforeSubmit(VkQueue,std::uint32_t family,
        std::span<const CommandRecordingFacts>,bool structureKnown) noexcept;
    void submitted(const SubmitTicket&,VkResult originalResult) noexcept;
    // Actual successful reset/rebegin, actual free, or actual pool reset/free
    // only: valid Vulkan usage guarantees nonpending. Capture old generation
    // before the original; call retirement after it succeeds. Free/destroy
    // have no result. A recorded-but-never-submitted pool is discarded here.
    void retire(std::uint64_t command,std::uint64_t recording) noexcept;
    void retirePool(std::uint64_t pool,std::uint64_t generation=0) noexcept;
    void deviceIdle(VkResult originalResult) noexcept;
    void stop() noexcept; // Stop new writes; retain all executable/pending refs.
    // Only immediately before actual device destruction, with no possible
    // future CB submission, after an observed SUCCESS native device idle.
    // Ordinary window close/deviceIdle must not invalidate executable app CBs.
    bool releaseForDeviceDestruction(VkResult observedIdleResult) noexcept;
    bool drained() const noexcept;
    bool active() const noexcept{return active_;}
    Stats stats() const noexcept{return stats_;}
};
}
