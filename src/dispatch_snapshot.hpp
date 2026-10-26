#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan_core.h>

// Isolated recorder, deliberately not linked to the production observer.
// Callers serialize all methods and supply actual Vulkan object/recording facts.
namespace xess_vk::dispatch_snapshot {
inline constexpr std::size_t maxPlanes=4,slotCount=2;
enum class Error { None,InvalidInput,UnsupportedImage,UnsupportedLayout,UnsupportedScope,WrongQueue,
    Alias,Capacity,MissingFunction,DriverFailed,StaleTicket,WrongState,Pending,DeviceLost,GenerationExhausted };
const char* errorName(Error) noexcept;
struct Ticket {std::uint32_t slot{};std::uint64_t generation{};friend bool operator==(const Ticket&,const Ticket&)=default;};
struct Result {Error error{Error::None};VkResult driver{VK_SUCCESS};bool accepted()const noexcept{return error==Error::None;}};
struct PlaneSpec {VkFormat format{VK_FORMAT_UNDEFINED};VkExtent2D extent{};};
struct Functions {
    PFN_vkCreateImage createImage{};PFN_vkDestroyImage destroyImage{};
    PFN_vkGetImageMemoryRequirements imageMemoryRequirements{};PFN_vkAllocateMemory allocateMemory{};
    PFN_vkFreeMemory freeMemory{};PFN_vkBindImageMemory bindImageMemory{};
    PFN_vkCmdPipelineBarrier pipelineBarrier{};PFN_vkCmdCopyImage copyImage{};
    PFN_vkResetCommandBuffer resetCommandBuffer{};PFN_vkGetFenceStatus fenceStatus{};
};
struct Config {
    VkDevice device{};std::uint32_t queueFamily{};VkQueueFlags queueFlags{};
    VkPhysicalDeviceMemoryProperties memory{};Functions functions{};
    std::array<PlaneSpec,maxPlanes> planes{};std::size_t planeCount{};
};
// Caller owns a primary CB that is currently recording outside render passes
// and dynamic rendering, allocated from this family and a RESET_COMMAND_BUFFER
// pool. generation changes after every real reset/begin. These are caller facts,
// not states that this module discovers from a handle.
struct CommandRecording {VkCommandBuffer command{};std::uint64_t generation{};std::uint32_t queueFamily{};};
struct Source {
    VkDevice device{};VkImage image{};
    // Exact successful creation description supplied by the owner. No pointer
    // from this structure is retained; opaque pNext/flags are unsupported.
    VkImageCreateInfo successfulCreateInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    // For D32S8 this layout applies to both depth and stencil together, including
    // SHADER_READ_ONLY_OPTIMAL. Separate depth/stencil layouts are unsupported.
    // Only depth contents are copied; the source restores this original layout.
    VkImageLayout currentLayout{VK_IMAGE_LAYOUT_UNDEFINED};std::uint32_t ownerQueueFamily{};
    VkImageSubresourceRange range{};
    // Real same-command producer and resumed-use scopes, supplied by the owner.
    // These fields cannot discover a game producer or extend a semaphore wait.
    VkPipelineStageFlags precedingStages{},resumedStages{};
    VkAccessFlags precedingAccess{},resumedAccess{};
};
struct Target {VkImage image{};VkFormat format{};VkExtent2D extent{};VkImageAspectFlags copiedAspect{};VkImageLayout finalLayout{};};
struct Capture {Result result{};Ticket ticket{};std::array<Target,maxPlanes> targets{};std::size_t count{};};
enum class State {Free,Recorded,Submitted,Captured,Uncertain};
struct Stats {std::uint64_t records{},refusals{},capacityRefusals{},copies{},cancellations{},submissions{},completions{},consumed{};std::array<State,slotCount> states{};};
struct Context;
// Allocates two independent target images/memory allocations per plane. Targets
// have TRANSFER_DST|TRANSFER_SRC|SAMPLED usage. No GPU command is issued here.
Context* create(const Config&,Result* failure=nullptr) noexcept;
// Busy contexts are retained, including their Vulkan resources and owner. The
// caller must retain its VkDevice/runtime too; no unbounded idle wait occurs.
Result destroy(Context*) noexcept;
// Complete preflight precedes every vkCmd call. Failure emits zero commands;
// success records source transitions/copies/restoration without shader state
// changes. No heap allocation occurs on any lifecycle/recording method.
Capture record(Context*,CommandRecording,std::size_t,const Source*) noexcept;
// Actual reset of the exact never-submitted command buffer. On successful reset
// all Recorded tickets for that recording are cancelled. Submitted/uncertain
// tickets cannot use this path, even if an external reset appeared to succeed.
Result cancelUnsubmitted(Context*,Ticket) noexcept;
// Caller contract: this is the actual result/fence of the queue submission that
// contains the registered command recording. The fence was correctly reset and
// cannot be reassigned/reset until every ticket is released. The module cannot
// prove this association from arbitrary handles. A failed submit is retained.
Result trackSubmission(Context*,Ticket,CommandRecording,VkFence,VkResult) noexcept;
// Queries the registered actual capture fence. NOT_READY/timeout/device loss
// never release a slot. Success records capture completion, not consumption.
Result pollCapture(Context*,Ticket) noexcept;
// Caller contract: actualCompletion covers every last downstream use of this
// target. For fixture capture+readback in one batch this is the capture fence.
// Every consumer must restore each target's advertised finalLayout before this
// fence signals; the next recording uses that layout. A fence cannot verify it.
// A cross-API consumer requires its own real completion mechanism; this narrow
// Vulkan API cannot infer that completion. Successful fence query frees a slot.
Result releaseConsumed(Context*,Ticket,VkFence actualCompletion) noexcept;
Stats stats(const Context*) noexcept;
} // namespace xess_vk::dispatch_snapshot
