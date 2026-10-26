#pragma once
#include "image_inventory.hpp"
#include "render_pass_metadata.hpp"
#include "command_recording_facts.hpp"
#include <array>
#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace xess_vk::render_pass_observer {
using Sink=void (*)(const char*);
enum class Role : unsigned { Motion, Depth, RealColor };
struct Context;
using InventoryLookup=image_inventory::Snapshot(*)(void*,std::uint64_t) noexcept;
struct InventoryReader {InventoryLookup lookup{};void* user{};};
inline constexpr std::size_t maxSubmittedCommands=128;
inline constexpr std::size_t maxSubmittedSemaphores=256; // waits + signals across one Submit2 call
struct SubmitTicket {
    std::uint64_t queue{}, token{}, issues{};
    std::uint64_t device{},contextIncarnation{};
    std::uint32_t queueFamily{VK_QUEUE_FAMILY_IGNORED};
    bool queueKnown{};
    std::uint32_t commandCount{};
    std::array<render_pass_metadata::Key,maxSubmittedCommands> commands{};
    std::array<render_pass_metadata::ProjectionTicket,maxSubmittedCommands> projections{};
    std::array<CommandRecordingFacts,maxSubmittedCommands> recordingFacts{};
};
struct ProjectionStats {
    std::uint64_t selectionChanges{},selectionRejections{},capturedCommands{},completedCommands{};
    std::uint64_t acceptedSlots{},refusedSlots{},disabledSlots{},noDirectSlots{},layoutOperations{};
    std::uint64_t emittedOperations{},omittedOperations{},emittedSummaries{},omittedSummaries{},issues{};
    std::uint64_t swapchainImagesRegistered{},swapchainImageRejections{},swapchainImageDuplicates{};
    std::uint64_t swapchainSamplesEmitted{},swapchainSamplesOmitted{};
    std::uint64_t swapchainImagesRetired{},swapchainRetirementsIgnored{};
    // Per submitted command, not per role. Captured Begin calls are not frames.
    std::uint64_t journalObservedPasses{},journalRetainedPasses{},journalDroppedPasses{},journalMaxObservedPasses{};
};
struct Stats {
    render_pass_metadata::Stats metadata;
    std::uint64_t objectRejections{}, recordingRejections{}, ignoredNonAttachmentImages{}, ignoredUntrackedViews{};
    std::uint64_t successfulDriverSubmissions{}, failedDriverSubmissions{}, acceptedRecordings{}, refusedRecordings{}, acceptedLayoutOperations{};
    std::uint64_t emittedOperations{}, omittedOperations{}, emittedSummaries{}, omittedSummaries{};
    std::uint64_t observedIssues{}, fatalIssues{}, failures{}, queueOverlaps{};
    std::uint64_t diagnosticObservations{},diagnosticSamples{},diagnosticOmitted{},diagnosticEmitted{},diagnosticIgnoredViewEvictions{};
    std::uint64_t recordingFactsStaleSubmissions{},recordingFactsUnknownSubmissions{};
    ProjectionStats projection; // independent of all legacy R0 accounting above
};

// One Context per device, created before observing its children. All functions
// preserve Win32 LastError and never call Vulkan. Owner serializes destruction
// against concurrent callbacks. Sink is invoked ONLY by finishSubmit/summary,
// after releasing the Context mutex; it must be nonblocking and must not re-enter
// mutating hooks. Read-only stats() is permitted from a sink.
Context* create(VkDevice,Sink,InventoryReader={}) noexcept;
void destroy(Context*) noexcept;
void retire(Context*) noexcept;
void summary(Context*) noexcept;
Stats stats(Context*) noexcept;
struct DiagnosticPassSnapshot {
    CommandRecordingFacts recording;
    render_pass_metadata::CommandSnapshot command;
    render_pass_metadata::RenderPassSnapshot pass;
    render_pass_metadata::FramebufferSnapshot framebuffer;
    std::array<render_pass_metadata::ViewSnapshot, render_pass_metadata::maxAttachments> views{};
    std::array<render_pass_metadata::ImageSnapshot, render_pass_metadata::maxAttachments> images{};
    std::size_t count{};
};
// Current Begin input only; accumulated admission failures must not hide its
// diagnostic objects. Does not alter recording order or any admission.
DiagnosticPassSnapshot diagnosticPassSnapshot(Context*, VkCommandBuffer, const VkRenderPassBeginInfo*) noexcept;
render_pass_metadata::ViewSnapshot diagnosticViewSnapshot(Context*,VkImageView) noexcept;

// Caller holds its stateLock over inventory.created -> lookup -> imageCreated
// so inventory generation arrives in global creation order. Never hold it over
// downstream Vulkan. Adapter never acquires stateLock or logs from this method.
// On success record must identify the exact returned image; info is the original
// CreateImage input, including imageType and pNext missing from older inventory.
void imageCreated(Context*,const VkImageCreateInfo*,VkImage,VkResult,const image_inventory::Record*) noexcept;
// A separate, already audited successful swapchain image enumeration observation.
// Caller provides creation-equivalent immutable facts and the SAME inventory's
// globally ordered generation under stateLock. This is not a vkCreateImage event.
// Unknown flags/extensions remain unsupported; no WSI usage/lifetime is inferred.
void swapchainImageObserved(Context*,const render_pass_metadata::ImageDescription&) noexcept;
// Consume an inventory removal by exact source/key. A stale removal must not
// erase a newer generation that reused the numeric image handle.
void swapchainImageRetiring(Context*,render_pass_metadata::Key) noexcept;
// Before forwarding driver destroy, under the same outer inventory/selection lock.
void imageDestroying(Context*,VkImage) noexcept;
void select(Context*,Role,const image_inventory::Record&) noexcept; // metadata selection only
void viewCreated(Context*,const VkImageViewCreateInfo*,VkImageView,VkResult) noexcept;
void viewDestroying(Context*,VkImageView) noexcept;
void renderPassCreated(Context*,const VkRenderPassCreateInfo*,VkRenderPass,VkResult) noexcept;
void renderPass2Created(Context*,const VkRenderPassCreateInfo2*,VkRenderPass,VkResult) noexcept; // core/KHR
void renderPassDestroying(Context*,VkRenderPass) noexcept;
void framebufferCreated(Context*,const VkFramebufferCreateInfo*,VkFramebuffer,VkResult) noexcept;
void framebufferDestroying(Context*,VkFramebuffer) noexcept;

void queue(Context*,VkQueue,std::uint32_t family) noexcept;
void poolCreated(Context*,VkCommandPool,const VkCommandPoolCreateInfo*,VkResult) noexcept;
void allocate(Context*,const VkCommandBufferAllocateInfo*,const VkCommandBuffer*,VkResult) noexcept;
void free(Context*,std::uint32_t,const VkCommandBuffer*) noexcept; // before actual free
void resetPool(Context*,VkCommandPool,bool destroy,VkResult) noexcept; // after successful reset / before destroy
void begin(Context*,VkCommandBuffer,const VkCommandBufferBeginInfo*,VkResult) noexcept;
void end(Context*,VkCommandBuffer,VkResult) noexcept;
void reset(Context*,VkCommandBuffer,VkResult) noexcept;
// Feed each original explicit barrier entry point once to keep common ordinals.
// This emits no invented barrier and does not modify image_sync_tracker.
void explicitBarrier(Context*,VkCommandBuffer) noexcept;
void explicitImageBarriers(Context*,VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,
    std::uint32_t,const VkImageMemoryBarrier*) noexcept;
void explicitImageBarriers2(Context*,VkCommandBuffer,const VkDependencyInfo*) noexcept; // core/KHR
// Advances the SAME order as explicitBarrier/RP/control hooks. Call immediately
// before preDispatch/original vkCmdDispatch; the returned owned copy is frozen.
CommandRecordingFacts dispatchPoint(Context*,VkCommandBuffer) noexcept;
CommandRecordingFacts recordingFacts(Context*,VkCommandBuffer) noexcept; // read-only
// Query-path counters. lockContended covers only recordingFacts finding the
// Context mutex held, not barrier/pass/lifetime writers or dispatchPoint.
struct RecordingFactsCounters {std::uint64_t queries{},lockContended{},lookupScans{};};
RecordingFactsCounters recordingFactsCounters(Context*) noexcept; // lock-free, monotonic
RecordedSourceLayout resolveSourceAt(Context*,const CommandRecordingFacts&,RecordedImageKey,const VkImageSubresourceRange&,VkImageLayout) noexcept;
RecordedSourceLayoutQuery sourceLayoutQuery(Context*) noexcept;
void imageTouched(Context*,VkCommandBuffer,VkImage,RecordedImageTouch) noexcept;
RecordedImageInterval imageInterval(Context*,const CommandRecordingFacts& read,const CommandRecordingFacts& use,RecordedImageKey) noexcept;
RecordedImageIntervalQuery imageIntervalQuery(Context*) noexcept;
void unsupported(Context*,VkCommandBuffer,render_pass_metadata::Issue) noexcept; // events / secondary / unknown
void beginRenderPass(Context*,VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents) noexcept;
void beginRenderPass2(Context*,VkCommandBuffer,const VkRenderPassBeginInfo*,const VkSubpassBeginInfo*) noexcept;
void endRenderPass(Context*,VkCommandBuffer) noexcept;
void endRenderPass2(Context*,VkCommandBuffer,const VkSubpassEndInfo*) noexcept;
void nextSubpass(Context*,VkCommandBuffer) noexcept; // all three forms explicitly unsupported
void dynamicRendering(Context*,VkCommandBuffer) noexcept; // Begin/End, core/KHR unsupported
void beginDynamicRendering(Context*,VkCommandBuffer,const VkRenderingInfo*) noexcept;
void endDynamicRendering(Context*,VkCommandBuffer) noexcept;

// Capture before the original driver call and finish afterward with its literal
// return value. No adapter lock is held across that call. Ticket captures exact
// command generations so racing reset/free cannot be relabelled as old work.
// Zero-command submissions are valid: they count driver outcomes, not recordings.
// Submit2 semaphore entries are bounded and checked for the supported structure,
// core stage mask and device index 0. Values are not interpreted as sync proof.
SubmitTicket beginSubmit(Context*,VkQueue,std::uint32_t,const VkSubmitInfo*) noexcept;
SubmitTicket beginSubmit2(Context*,VkQueue,std::uint32_t,const VkSubmitInfo2*) noexcept;
void finishSubmit(Context*,const SubmitTicket&,VkFence,VkResult) noexcept;
} // namespace xess_vk::render_pass_observer
