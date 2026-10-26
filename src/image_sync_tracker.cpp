#include "image_sync_tracker.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace xess_vk::image_sync {
struct Tracker::Impl {
    struct Image {
        Candidate description;
        std::uint64_t selectionSerial{};
        Evidence evidence;
    };
    struct Operation { Barrier barrier; std::uint64_t issues{}, selectionSerial{}; bool layoutIgnored{}; };
    enum class State { Initial, Recording, Executable, Invalid };
    struct Command {
        Handle handle{}, pool{};
        std::uint32_t family{VK_QUEUE_FAMILY_IGNORED};
        VkCommandBufferLevel level{VK_COMMAND_BUFFER_LEVEL_PRIMARY};
        State state{State::Initial};
        VkCommandBufferUsageFlags flags{};
        std::uint64_t generation{}, beginSelectionSerial{}, issues{}, submissions{};
        std::size_t count{};
        std::array<Operation, maxBarriers> operations{};
    };
    Config config;
    mutable std::mutex mutex;
    std::array<Image, maxCandidates> images{};
    std::array<Command, maxCommands> commands{};
    std::uint64_t selectionSerial{}, commandSerial{};
    Stats totals;
    explicit Impl(Config c) : config(c) {
        if (!c.candidates || c.candidates > maxCandidates || !c.commandBuffers ||
            c.commandBuffers > maxCommands || !c.barriersPerCommand || c.barriersPerCommand > maxBarriers)
            throw std::invalid_argument("image sync tracker capacity outside hard bounds");
        if (!c.completeHookCoverage) totals.issues |= bit(Issue::CoverageIncomplete);
    }
    Image* image(ImageKey key) {
        for (std::size_t i = 0; i < config.candidates; ++i)
            if (images[i].description.key == key && key.image && key.generation) return &images[i];
        return nullptr;
    }
    Command* command(Handle handle) {
        if (handle) for (std::size_t i = 0; i < config.commandBuffers; ++i)
            if (commands[i].handle == handle) return &commands[i];
        return nullptr;
    }
    static bool whole(const Candidate& c, const VkImageSubresourceRange& r) {
        return r.aspectMask == c.aspects && r.baseMipLevel == 0 && r.baseArrayLayer == 0 &&
            (r.levelCount == c.mipLevels || r.levelCount == VK_REMAINING_MIP_LEVELS) &&
            (r.layerCount == c.arrayLayers || r.layerCount == VK_REMAINING_ARRAY_LAYERS);
    }
    void invalidate(Command& command) {
        command.state = State::Initial; command.count = 0; command.issues = 0;
        command.flags = 0; command.submissions = 0; command.generation = 0;
    }
};
Tracker::Tracker(Config config) : impl_(std::make_unique<Impl>(config)) {}
Tracker::~Tracker() = default;
std::size_t Tracker::allocatedStorageBytes() noexcept { return sizeof(Impl); }
bool Tracker::select(const Candidate& candidate) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (!candidate.key.image || !candidate.key.generation || !candidate.aspects ||
        !candidate.mipLevels || !candidate.arrayLayers || (p.totals.issues & bit(Issue::Stopped))) return false;
    if (auto* found = p.image(candidate.key)) {
        const auto& old = found->description;
        if (old.aspects == candidate.aspects && old.mipLevels == candidate.mipLevels &&
            old.arrayLayers == candidate.arrayLayers && old.sharingMode == candidate.sharingMode) return true;
        p.totals.issues |= bit(Issue::InvalidLifecycle); return false;
    }
    for (std::size_t i = 0; i < p.config.candidates; ++i)
        if (p.images[i].description.key.image == candidate.key.image) {
            p.totals.issues |= bit(Issue::StaleImageGeneration); return false;
        }
    if (p.selectionSerial == UINT64_MAX) { p.totals.issues |= bit(Issue::Capacity); return false; }
    for (std::size_t i = 0; i < p.config.candidates; ++i) if (!p.images[i].description.key.image) {
        auto& image = p.images[i]; image.description = candidate;
        image.selectionSerial = ++p.selectionSerial; image.evidence.image = candidate.key;
        image.evidence.selected = true; ++p.totals.selected; return true;
    }
    p.totals.issues |= bit(Issue::Capacity); return false;
}
void Tracker::deselect(ImageKey key) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (auto* image = p.image(key)) { *image = {}; --p.totals.selected; }
}
void Tracker::destroying(ImageKey key) { deselect(key); }
void Tracker::allocate(Handle handle, Handle pool, std::uint32_t family, VkCommandBufferLevel level) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (!handle || !pool || p.command(handle)) { p.totals.issues |= bit(Issue::InvalidLifecycle); return; }
    for (std::size_t i = 0; i < p.config.commandBuffers; ++i) if (!p.commands[i].handle) {
        auto& command = p.commands[i]; command.handle = handle; command.pool = pool;
        command.family = family; command.level = level; ++p.totals.commands; return;
    }
    p.totals.issues |= bit(Issue::Capacity);
}
void Tracker::free(Handle handle) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (auto* command = p.command(handle)) { *command = {}; --p.totals.commands; }
}
void Tracker::resetPool(Handle pool, bool destroy) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    for (std::size_t i = 0; i < p.config.commandBuffers; ++i) {
        auto& command = p.commands[i];
        if (command.handle && command.pool == pool) {
            if (destroy) { command = {}; --p.totals.commands; }
            else p.invalidate(command);
        }
    }
}
void Tracker::begin(Handle handle, VkCommandBufferUsageFlags flags, VkResult result) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    auto* command = p.command(handle);
    if (!command) { p.totals.issues |= bit(Issue::UnknownCommand); return; }
    p.invalidate(*command);
    if (result != VK_SUCCESS) { command->state = Impl::State::Invalid; return; }
    if (p.commandSerial == UINT64_MAX) { p.totals.issues |= bit(Issue::Capacity); return; }
    command->generation = ++p.commandSerial; command->beginSelectionSerial = p.selectionSerial;
    command->flags = flags; command->state = Impl::State::Recording;
    if (command->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) command->issues |= bit(Issue::SecondaryCommands);
    if (flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) command->issues |= bit(Issue::SimultaneousUse);
}
void Tracker::end(Handle handle, VkResult result) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (auto* command = p.command(handle)) {
        if (command->state != Impl::State::Recording) command->issues |= bit(Issue::InvalidLifecycle);
        command->state = result == VK_SUCCESS ? Impl::State::Executable : Impl::State::Invalid;
    } else p.totals.issues |= bit(Issue::UnknownCommand);
}
void Tracker::reset(Handle handle, VkResult result) {
    if (result != VK_SUCCESS) return;
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (auto* command = p.command(handle)) p.invalidate(*command);
    else p.totals.issues |= bit(Issue::UnknownCommand);
}
void Tracker::barrier(Handle handle, const Barrier& barrier) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    auto* image = p.image(barrier.image);
    if (!image) { ++p.totals.ignoredBarriers; return; }
    auto* command = p.command(handle);
    if (!command) { p.totals.issues |= bit(Issue::UnknownCommand); return; }
    if (command->state != Impl::State::Recording) { command->issues |= bit(Issue::InvalidLifecycle); return; }
    if (command->count >= p.config.barriersPerCommand) { command->issues |= bit(Issue::Capacity); return; }
    auto& op = command->operations[command->count++]; op = {}; op.barrier = barrier;
    op.selectionSerial = image->selectionSerial;
    ++p.totals.recordedBarriers;
    const auto& candidate = image->description;
    if (image->selectionSerial > command->beginSelectionSerial) op.issues |= bit(Issue::NotSelectedAtBegin);
    if (!Impl::whole(candidate, barrier.range)) op.issues |= bit(Issue::PartialRange);
    if (candidate.sharingMode != VK_SHARING_MODE_EXCLUSIVE) op.issues |= bit(Issue::ConcurrentSharing);
    if (barrier.srcQueueFamily != barrier.dstQueueFamily) op.issues |= bit(Issue::OwnershipTransfer);
    else if (barrier.srcQueueFamily != VK_QUEUE_FAMILY_IGNORED &&
             barrier.srcQueueFamily != command->family) op.issues |= bit(Issue::QueueFamilyMismatch);
    if (barrier.unsupportedExtension) op.issues |= bit(Issue::UnsupportedExtension);
    op.layoutIgnored = (barrier.synchronization2Command || p.config.synchronization2Enabled) &&
        barrier.srcQueueFamily == barrier.dstQueueFamily && barrier.oldLayout == barrier.newLayout;
    if (!op.layoutIgnored && (barrier.newLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
        barrier.newLayout == VK_IMAGE_LAYOUT_PREINITIALIZED ||
        barrier.newLayout == VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT)) op.issues |= bit(Issue::InvalidLayout);
}
void Tracker::opaque(Handle handle, Issue reason) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (auto* command = p.command(handle)) command->issues |= bit(reason);
    else p.totals.issues |= bit(Issue::UnknownCommand);
}
void Tracker::untrackedDeviceOperation(Issue reason) {
    auto& p = *impl_; std::lock_guard lock(p.mutex); p.totals.issues |= bit(reason);
}
void Tracker::submit(Handle queue, std::uint32_t family, std::span<const Handle> commands,
                     Handle fence, VkResult result, bool audited) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (result != VK_SUCCESS) {
        ++p.totals.failedSubmissions;
        if (result == VK_ERROR_DEVICE_LOST) p.totals.issues |= bit(Issue::DeviceLost);
        return;
    }
    if (p.totals.successfulSubmissions == UINT64_MAX) { p.totals.issues |= bit(Issue::Capacity); return; }
    const auto serial = ++p.totals.successfulSubmissions;
    if (!audited) p.totals.issues |= bit(Issue::UnsupportedExtension);
    if (!queue || family >= VK_QUEUE_FAMILY_FOREIGN_EXT) p.totals.issues |= bit(Issue::QueueFamilyMismatch);
    if (commands.size() > maxCommands) { p.totals.issues |= bit(Issue::Capacity); return; }
    for (auto handle : commands) {
        auto* command = p.command(handle);
        if (!command) { p.totals.issues |= bit(Issue::UnknownCommand); continue; }
        auto issues = command->issues;
        if (command->state != Impl::State::Executable) issues |= bit(Issue::InvalidLifecycle);
        if (command->family != family) issues |= bit(Issue::QueueFamilyMismatch);
        if (command->submissions && (command->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))
            issues |= bit(Issue::ReusedOneTimeCommand);
        ++command->submissions;
        // Unresolved commands can affect any selected image, even if no selected
        // image barrier appears in this particular command buffer.
        p.totals.issues |= issues;
        for (std::size_t i = 0; i < command->count; ++i) {
            const auto& op = command->operations[i];
            auto* image = p.image(op.barrier.image);
            // A role can retire/reselect a still-live image. Old recordings must
            // not alias that new observation generation or taint unrelated roles.
            if (!image || image->selectionSerial != op.selectionSerial) {
                ++p.totals.retiredOperationsSkipped; continue;
            }
            auto& e = image->evidence;
            e.issues |= issues | op.issues;
            e.lastRecordingIssues = issues | op.issues;
            if (e.barrierSubmitted && e.queue != queue) e.issues |= bit(Issue::MultipleQueues);
            if (!op.layoutIgnored) {
                if (e.submittedLayoutKnown && op.barrier.oldLayout != VK_IMAGE_LAYOUT_UNDEFINED &&
                    e.submittedLayout != op.barrier.oldLayout) e.issues |= bit(Issue::LayoutChainMismatch);
                e.submittedLayout = op.barrier.newLayout;
                // Partial ranges cannot be collapsed to one whole-image layout.
                e.submittedLayoutKnown = !(e.issues | p.totals.issues);
            }
            e.barrierSubmitted = true; e.lastBarrier = op.barrier;
            e.lastBarrierLayoutFieldsIgnored = op.layoutIgnored;
            e.command = handle; e.commandGeneration = command->generation;
            e.queue = queue; e.queueFamily = family; e.fence = fence; e.submissionSerial = serial;
        }
    }
}
Evidence Tracker::snapshot(ImageKey key) const {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    Evidence evidence; evidence.image = key;
    if (const auto* image = p.image(key)) evidence = image->evidence;
    evidence.issues |= p.totals.issues;
    evidence.latestDeviceSubmission = p.totals.successfulSubmissions;
    if (evidence.issues) evidence.submittedLayoutKnown = false;
    return evidence;
}
Stats Tracker::stats() const { auto& p = *impl_; std::lock_guard lock(p.mutex); return p.totals; }
void Tracker::stop() { untrackedDeviceOperation(Issue::Stopped); }
} // namespace xess_vk::image_sync
