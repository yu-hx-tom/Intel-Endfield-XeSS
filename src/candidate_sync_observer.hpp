#pragma once
#include "image_inventory.hpp"
#include "image_sync_tracker.hpp"
#include <vulkan/vulkan_core.h>

namespace xess_vk::candidate_sync {
// Production sink only copies/writes one bounded line. It must not re-enter this
// context: submit/summary sinks run while the helper's metadata mutex is held.
using Sink = void (*)(const char*);
enum class Role : unsigned { Motion = 0, Depth = 1, RealColor = 2 };
struct Context;
struct SubmitTicket { std::uint64_t queue{}, token{}; };
struct SelectionNotice {
    bool emit{}, accepted{};
    std::uint64_t device{}, selectionGeneration{}, previousSelectionGeneration{};
    Role role{};
    image_inventory::Record record;
};
Context* create(VkDevice, bool synchronization2Enabled, Sink) noexcept;
void destroy(Context*) noexcept;
void retire(Context*) noexcept;
void summary(Context*) noexcept;
SelectionNotice select(Context*, Role, const image_inventory::Record&) noexcept;
void reportSelection(const SelectionNotice&, Sink) noexcept; // copied values, after stateLock release
void destroyingImage(Context*, std::uint64_t image) noexcept;
void queue(Context*, VkQueue, std::uint32_t family) noexcept;
void pool(Context*, VkCommandPool, std::uint32_t family) noexcept;
void allocate(Context*, const VkCommandBufferAllocateInfo*, const VkCommandBuffer*) noexcept;
void free(Context*, std::uint32_t, const VkCommandBuffer*) noexcept;
void resetPool(Context*, VkCommandPool, bool destroy) noexcept;
void begin(Context*, VkCommandBuffer, const VkCommandBufferBeginInfo*, VkResult) noexcept;
void end(Context*, VkCommandBuffer, VkResult) noexcept;
void reset(Context*, VkCommandBuffer, VkResult) noexcept;
void barriers(Context*, VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
              std::uint32_t, const VkImageMemoryBarrier*) noexcept;
void barriers2(Context*, VkCommandBuffer, const VkDependencyInfo*) noexcept;
void opaque(Context*, VkCommandBuffer, image_sync::Issue) noexcept;
SubmitTicket beginSubmit(Context*, VkQueue) noexcept;
void submit(Context*, SubmitTicket, std::uint32_t, const VkSubmitInfo*, VkFence, VkResult) noexcept;
void submit2(Context*, SubmitTicket, std::uint32_t, const VkSubmitInfo2*, VkFence, VkResult) noexcept;
// Pure fixture access: copied metadata, no exported Vulkan symbols or GPU calls.
image_sync::Stats stats(Context*) noexcept;
image_sync::Evidence evidence(Context*, image_sync::ImageKey) noexcept;
} // namespace xess_vk::candidate_sync
