#pragma once
#include "descriptor_image_observer.hpp"
#include "borrowed_capture_completion.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace xess_vk::rawcolor_capture_probe {
inline constexpr VkDeviceSize maxCaptureBytes=64ull*1024*1024;
struct Functions {
    PFN_vkCreateImage createImage{};PFN_vkDestroyImage destroyImage{};PFN_vkGetImageMemoryRequirements imageRequirements{};
    PFN_vkCreateBuffer createBuffer{};PFN_vkDestroyBuffer destroyBuffer{};PFN_vkGetBufferMemoryRequirements bufferRequirements{};
    PFN_vkAllocateMemory allocateMemory{};PFN_vkFreeMemory freeMemory{};PFN_vkBindImageMemory bindImageMemory{};PFN_vkBindBufferMemory bindBufferMemory{};
    PFN_vkCreateCommandPool createPool{};PFN_vkDestroyCommandPool destroyPool{};PFN_vkAllocateCommandBuffers allocateCommands{};
    PFN_vkBeginCommandBuffer beginCommand{};PFN_vkEndCommandBuffer endCommand{};
    PFN_vkCreateFence createFence{};PFN_vkDestroyFence destroyFence{};PFN_vkGetFenceStatus fenceStatus{};PFN_vkWaitForFences waitFences{};
    PFN_vkCmdPipelineBarrier barrier{};PFN_vkCmdCopyImage copyImage{};PFN_vkCmdCopyImageToBuffer copyImageToBuffer{};
    PFN_vkQueueSubmit submit{};PFN_vkMapMemory mapMemory{};PFN_vkUnmapMemory unmapMemory{};PFN_vkInvalidateMappedMemoryRanges invalidate{};
};
using Sink=void(*)(const char*);
struct Config {
    VkDevice device{};VkQueue queue{};std::uint32_t queueFamily{VK_QUEUE_FAMILY_IGNORED};VkQueueFlags queueFlags{};
    VkPhysicalDeviceMemoryProperties memory{};VkDeviceSize nonCoherentAtomSize{};Functions functions;
    bool singlePhysicalDeviceKnown{},uniqueOrdinaryExternallySynchronizedQueueKnown{};
    bool defaultDeviceAndInstanceAllocatorsKnown{},applicationDeviceMemoryCallbackAbsentKnown{};
    bool applicationInstanceCallbacksAbsentKnown{};
    bool targetFormatTransferSupported{};VkImageFormatProperties targetFormatProperties{}; // actual fixed format-122 queries
    bool (*claimProcessLease)(void*) noexcept{};void* leaseUser{};
    bool enabled{};std::array<wchar_t,32768> outputBmpPath{};Sink sink{};
};
enum class Reason:std::uint64_t {Disabled=1ull<<0,MissingFunction=1ull<<1,UnsupportedDeviceQueue=1ull<<2,NotRawPattern=1ull<<3,
    UnsupportedRecording=1ull<<4,UnsupportedSource=1ull<<5,LayoutUnknown=1ull<<6,MemoryUnknown=1ull<<7,Capacity=1ull<<8,
    DriverFailed=1ull<<9,AlreadyCaptured=1ull<<10,CompletionRefused=1ull<<11,OutputFailed=1ull<<12,Pending=1ull<<13,
    UnsafeDeferredLifetime=1ull<<14,Quarantined=1ull<<15,ObservationUnsafe=1ull<<16};
constexpr std::uint64_t bit(Reason r) noexcept{return static_cast<std::uint64_t>(r);}
struct Result {std::uint64_t reasons{};VkResult driver{VK_SUCCESS};bool commandsInserted{};bool accepted()const noexcept{return !reasons;}};
struct Status {
    borrowed_capture_completion::Status completion;
    VkImage ownedImage{};VkDeviceMemory imageMemory{},stagingMemory{};VkBuffer staging{};VkFence ownFence{},drainFence{};
    VkExtent2D extent{};VkDeviceSize pixelBytes{};std::uint64_t insertionAttempts{},sourceCopies{},stagingCopies{},fencePolls{},filesWritten{};
    bool prepared{},sourceInserted{},pixelsWritten{},released{};std::uint64_t reasons{};
    bool drainSubmitted{},shutdownCleanupComplete{},shutdownQuarantined{},observationUnsafe{};VkResult shutdownDriver{VK_NOT_READY};
    std::uint64_t rawCandidates{},sourceQueryAttempts{};
};
struct Context;
Context* create(const Config&) noexcept;
// All methods are serialized by the caller. No metadata observer lock may span
// calls. prepare+arm use UNHOOKED functions and never reset a borrowed app CB.
Result preDispatch(Context*,const descriptor_image_observer::DispatchSourceFacts&) noexcept;
borrowed_capture_completion::SubmitTicket preSubmit(Context*,const borrowed_capture_completion::SubmitObservation&) noexcept;
Result afterOriginalResult(Context*,const borrowed_capture_completion::SubmitTicket&,VkResult,const borrowed_capture_completion::QueueWindow&) noexcept;
Result poll(Context*) noexcept;
void recordingRetired(Context*,const borrowed_capture_completion::RecordingKey&) noexcept;
void queueIdle(Context*,VkQueue,VkResult) noexcept;
void deviceIdle(Context*,VkDevice,VkResult) noexcept;
// Actual missing lifecycle evidence is terminal for this one capture lease.
// Ordinary cross-thread contention is serialized and must not call this API.
// Retain private children until the existing explicit own drain/wait teardown.
void observationUnsafe(Context*) noexcept;
// Explicit shutdown result must be checked BEFORE original vkDestroyDevice.
// A finite wait uses only the probe's own fence, never app fences or force-idle.
Result beforeDeviceDestroy(Context*,std::uint64_t ownFenceTimeoutNanoseconds) noexcept;
void deviceTerminated(Context*) noexcept;
bool destroy(Context*) noexcept; // false retains an owner that cannot yet release
Status status(Context*) noexcept;
bool canAttempt(Context*) noexcept;
bool requestSourceQuery(void*) noexcept; // first 16, then every 1200, max 64
// The returned packed format-122 pixels belong to this Context's mapped private
// staging memory and remain readable until successful physical teardown.
std::span<const std::uint32_t> pixels(Context*) noexcept;
} // namespace xess_vk::rawcolor_capture_probe
