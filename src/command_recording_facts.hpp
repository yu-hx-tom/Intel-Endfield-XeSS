#pragma once
#include <cstdint>
#include <type_traits>
#include <vulkan/vulkan_core.h>

namespace xess_vk {
// These reasons describe only observed command lifecycle/control flow. They
// never inherit attachment metadata errors and never grant GPU/copy permission.
enum class CommandRecordingReason : std::uint64_t {
    MissingObservation=1ull<<0, UnknownPool=1ull<<1, InvalidInput=1ull<<2,
    Capacity=1ull<<3, Lifecycle=1ull<<4, DriverStateUnknown=1ull<<5,
    AllocationPNext=1ull<<6, PoolPNext=1ull<<7, BeginPNext=1ull<<8,
    Secondary=1ull<<9, SimultaneousUse=1ull<<10, NotOneTime=1ull<<11,
    UnsupportedFlags=1ull<<12, ScopeUnknown=1ull<<13, DynamicRendering=1ull<<14,
    NextSubpass=1ull<<15, Events=1ull<<16, SecondaryExecution=1ull<<17,
    UnknownControl=1ull<<18, NotRecording=1ull<<19, NotExecutable=1ull<<20,
    UnknownQueue=1ull<<21, QueueFamilyMismatch=1ull<<22,
    SubmissionStructureUnknown=1ull<<23, DuplicateSubmission=1ull<<24,
    OneTimeAlreadySubmitted=1ull<<25, CompletionUnknown=1ull<<26,
    Stopped=1ull<<27, DeviceLost=1ull<<28, IdentityMismatch=1ull<<29
};
constexpr std::uint64_t bit(CommandRecordingReason r) noexcept {return static_cast<std::uint64_t>(r);}
struct CommandRecordingFacts {
    std::uint64_t device{},contextIncarnation{},command{},rpRecordingGeneration{};
    std::uint64_t pool{},poolGeneration{},commonRecordOrder{};
    VkCommandPoolCreateFlags poolFlags{};
    std::uint32_t queueFamily{VK_QUEUE_FAMILY_IGNORED};
    VkCommandBufferLevel level{VK_COMMAND_BUFFER_LEVEL_MAX_ENUM};
    VkCommandBufferUsageFlags beginUsageFlags{};
    std::uint64_t reasons{bit(CommandRecordingReason::MissingObservation)};
    std::uint64_t successfulSubmissionCount{},lastSubmitQueue{},lastSubmitSerial{};
    bool allocationKnown{},poolKnown{},beginFlagsKnown{},stateKnown{};
    bool recording{},executable{},endSucceeded{},scopeKnown{},inRenderPass{},inDynamicRendering{};
    bool lastSubmitQueueKnown{};
    // The common order covers currently hooked control/barrier/dispatch events,
    // not every vkCmd. Submission is historical; no pending/completion inference.
    bool completeHookCoverage{},gpuComplete{},layoutVerified{},copyPermitted{};
    bool supported() const noexcept {return reasons==0;}
};
static_assert(std::is_trivially_copyable_v<CommandRecordingFacts>);
} // namespace xess_vk
