#include "image_sync_tracker.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace xess_vk::image_sync;
unsigned checks{}, cases{};
void require(bool value, const char* message) {
    ++checks; if (!value) throw std::runtime_error(message);
}
Config covered() { Config c; c.completeHookCoverage = true; return c; }
Candidate candidate(std::uint64_t image = 10, std::uint64_t generation = 1) {
    return {{image, generation}, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_SHARING_MODE_EXCLUSIVE};
}
Barrier barrier(ImageKey key = {10, 1}, VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                VkImageLayout newLayout = VK_IMAGE_LAYOUT_GENERAL) {
    Barrier b; b.image = key; b.range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = oldLayout; b.newLayout = newLayout;
    b.srcStages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b.dstStages = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.srcAccess = VK_ACCESS_2_SHADER_WRITE_BIT; b.dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT; return b;
}
void initialize(Tracker& tracker, bool selected = true) {
    if (selected) require(tracker.select(candidate()), "select candidate");
    tracker.allocate(100, 200, 3, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    tracker.begin(100, 0, VK_SUCCESS);
}
void submit(Tracker& tracker, VkResult result = VK_SUCCESS, Handle queue = 300, Handle fence = 400) {
    tracker.end(100, VK_SUCCESS); const std::array<Handle, 1> commands{100};
    tracker.submit(queue, 3, commands, fence, result);
}
void noCopyClaims(const Evidence& e) {
    require(!e.gpuComplete && !e.copyPermitted && !e.resourceLifetimePinned && !e.lastWriterKnown,
            "submission never grants copy/GPU completion/writer/lifetime");
}
void issue(const Evidence& e, Issue expected) {
    require((e.issues & bit(expected)) != 0, "expected refusal issue");
    require(!e.submittedLayoutKnown, "refused layout cannot be declared known"); noCopyClaims(e);
}
void happyPath() {
    ++cases; Tracker tracker(covered()); initialize(tracker);
    tracker.barrier(100, barrier());
    auto before = tracker.snapshot({10, 1}); require(!before.barrierSubmitted, "recording is not submission");
    submit(tracker);
    auto e = tracker.snapshot({10, 1}); require(e.barrierSubmitted && e.submittedLayoutKnown, "submitted full barrier");
    require(e.submittedLayout == VK_IMAGE_LAYOUT_GENERAL && e.queue == 300 && e.queueFamily == 3,
            "exact layout and queue recorded");
    require(e.fence == 400 && e.commandGeneration == 1 && e.submissionSerial == 1, "provenance retained");
    require(e.lastBarrier.srcAccess == VK_ACCESS_2_SHADER_WRITE_BIT &&
            e.lastBarrier.dstStages == VK_PIPELINE_STAGE_2_TRANSFER_BIT, "scope retained"); noCopyClaims(e);
    tracker.begin(100, 0, VK_SUCCESS);
    tracker.barrier(100, barrier({10, 1}, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    submit(tracker, VK_SUCCESS, 300, 0);
    e = tracker.snapshot({10, 1}); require(e.submittedLayoutKnown && e.commandGeneration == 2 && e.fence == 0,
                                      "command reuse new recording and null fence supported");
    noCopyClaims(e);
}
void layoutRules() {
    ++cases;
    for (bool useSync2Command : {false, true}) {
        auto config = covered(); config.synchronization2Enabled = !useSync2Command;
        Tracker tracker(config); initialize(tracker);
        auto b = barrier({10, 1}, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED);
        b.synchronization2Command = useSync2Command; tracker.barrier(100, b); submit(tracker);
        auto e = tracker.snapshot({10, 1}); require(e.barrierSubmitted && !e.submittedLayoutKnown && !e.issues,
                                                "sync2 equal layouts are ignored, not an anchor");
        require(e.lastBarrierLayoutFieldsIgnored, "raw ignored-layout barrier explicitly marked");
        tracker.begin(100, 0, VK_SUCCESS); tracker.barrier(100, barrier()); submit(tracker);
        tracker.begin(100, 0, VK_SUCCESS); tracker.barrier(100, b); submit(tracker);
        e = tracker.snapshot({10, 1}); require(e.submittedLayoutKnown && e.submittedLayout == VK_IMAGE_LAYOUT_GENERAL,
                                            "ignored layout fields preserve known layout");
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier());
        tracker.barrier(100, barrier({10, 1}, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
        submit(tracker); issue(tracker.snapshot({10, 1}), Issue::LayoutChainMismatch);
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier());
        tracker.barrier(100, barrier({10, 1}, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
        submit(tracker); require(tracker.snapshot({10, 1}).submittedLayoutKnown, "undefined old layout discards, not mismatch");
    }
    for (auto newLayout : {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT}) {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier({10, 1}, VK_IMAGE_LAYOUT_GENERAL, newLayout));
        submit(tracker); issue(tracker.snapshot({10, 1}), Issue::InvalidLayout);
    }
}
void rejectedRangesAndOwnership() {
    ++cases;
    for (unsigned variant = 0; variant != 7; ++variant) {
        Tracker tracker(covered()); initialize(tracker); auto b = barrier();
        Issue expected = Issue::PartialRange;
        if (variant == 0) b.range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (variant == 1) b.range.baseMipLevel = UINT32_MAX;
        if (variant == 2) b.range.levelCount = 0;
        if (variant == 3) b.range.layerCount = 2;
        if (variant == 4) { b.srcQueueFamily = 3; b.dstQueueFamily = 4; expected = Issue::OwnershipTransfer; }
        if (variant == 5) { b.srcQueueFamily = b.dstQueueFamily = 4; expected = Issue::QueueFamilyMismatch; }
        if (variant == 6) { b.unsupportedExtension = true; expected = Issue::UnsupportedExtension; }
        tracker.barrier(100, b); submit(tracker); issue(tracker.snapshot({10, 1}), expected);
    }
    Tracker tracker(covered()); initialize(tracker); auto b = barrier();
    b.range.levelCount = VK_REMAINING_MIP_LEVELS; b.range.layerCount = VK_REMAINING_ARRAY_LAYERS;
    tracker.barrier(100, b); submit(tracker); require(tracker.snapshot({10, 1}).submittedLayoutKnown, "remaining range normalized");
    Tracker depth(covered()); auto c = candidate(); c.aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    require(depth.select(c), "depth stencil candidate"); initialize(depth, false); b = barrier(); b.range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth.barrier(100, b); submit(depth); issue(depth.snapshot(c.key), Issue::PartialRange);
}
void lifecycleAndSelection() {
    ++cases;
    {
        Tracker tracker(covered()); initialize(tracker, false); require(tracker.select(candidate()), "late select");
        tracker.barrier(100, barrier()); submit(tracker); issue(tracker.snapshot({10, 1}), Issue::NotSelectedAtBegin);
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); tracker.destroying({10, 1});
        require(tracker.select(candidate(10, 2)), "new generation same handle"); submit(tracker);
        require(!tracker.snapshot({10, 1}).selected && !tracker.snapshot({10, 2}).barrierSubmitted, "stale record never aliases new image");
        require(tracker.stats().retiredOperationsSkipped == 1 && !tracker.snapshot({10, 2}).issues,
                "retired observation does not poison new generation");
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier());
        tracker.deselect({10, 1}); require(tracker.select(candidate()), "same live image can be reselected");
        submit(tracker); require(!tracker.snapshot({10, 1}).barrierSubmitted && tracker.stats().retiredOperationsSkipped == 1,
                               "same-key reselection cannot revive old recording");
        tracker.begin(100, 0, VK_SUCCESS); tracker.barrier(100, barrier()); submit(tracker);
        require(tracker.snapshot({10, 1}).submittedLayoutKnown, "fresh recording after reselection supported");
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); tracker.end(100, VK_ERROR_OUT_OF_HOST_MEMORY);
        const std::array<Handle, 1> cb{100}; tracker.submit(300, 3, cb, 400, VK_SUCCESS);
        issue(tracker.snapshot({10, 1}), Issue::InvalidLifecycle);
    }
    for (bool destroyPool : {false, true}) {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); tracker.end(100, VK_SUCCESS);
        tracker.resetPool(200, destroyPool); const std::array<Handle, 1> cb{100}; tracker.submit(300, 3, cb, 400, VK_SUCCESS);
        issue(tracker.snapshot({10, 1}), destroyPool ? Issue::UnknownCommand : Issue::InvalidLifecycle);
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); tracker.end(100, VK_SUCCESS);
        tracker.free(100); tracker.allocate(100, 200, 3, VK_COMMAND_BUFFER_LEVEL_PRIMARY); tracker.begin(100, 0, VK_SUCCESS);
        submit(tracker); require(!tracker.snapshot({10, 1}).barrierSubmitted, "reallocated command has no stale barrier");
    }
}
void opaqueAndSubmissionFailure() {
    ++cases;
    for (auto opaque : {Issue::SecondaryCommands, Issue::RenderPass, Issue::Events, Issue::UnsupportedExtension}) {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); tracker.opaque(100, opaque);
        submit(tracker); issue(tracker.snapshot({10, 1}), opaque);
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); submit(tracker, VK_ERROR_OUT_OF_HOST_MEMORY);
        require(!tracker.snapshot({10, 1}).barrierSubmitted && tracker.stats().failedSubmissions == 1, "failed submit never commits evidence");
        submit(tracker, VK_ERROR_DEVICE_LOST); issue(tracker.snapshot({10, 1}), Issue::DeviceLost);
    }
    {
        Tracker tracker; initialize(tracker); tracker.barrier(100, barrier()); submit(tracker);
        issue(tracker.snapshot({10, 1}), Issue::CoverageIncomplete);
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); submit(tracker);
        tracker.begin(100, 0, VK_SUCCESS); tracker.barrier(100, barrier()); submit(tracker, VK_SUCCESS, 301);
        issue(tracker.snapshot({10, 1}), Issue::MultipleQueues);
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); submit(tracker);
        tracker.untrackedDeviceOperation(Issue::HostImageTransition); issue(tracker.snapshot({10, 1}), Issue::HostImageTransition);
        tracker.stop(); issue(tracker.snapshot({10, 1}), Issue::Stopped);
        require(!tracker.select(candidate(11, 2)), "stop cannot reactivate");
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); submit(tracker);
        const std::array<Handle, 1> unknown{999}; tracker.submit(300, 3, unknown, 0, VK_SUCCESS);
        issue(tracker.snapshot({10, 1}), Issue::UnknownCommand);
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); submit(tracker);
        tracker.submit(300, 3, {}, 0, VK_SUCCESS, false);
        issue(tracker.snapshot({10, 1}), Issue::UnsupportedExtension);
    }
    {
        Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); submit(tracker);
        tracker.allocate(101, 200, 3, VK_COMMAND_BUFFER_LEVEL_PRIMARY); tracker.begin(101, 0, VK_SUCCESS);
        tracker.opaque(101, Issue::RenderPass); tracker.end(101, VK_SUCCESS);
        const std::array<Handle, 1> noSelectedBarriers{101}; tracker.submit(300, 3, noSelectedBarriers, 0, VK_SUCCESS);
        issue(tracker.snapshot({10, 1}), Issue::RenderPass);
    }
}
void limitsAndFlags() {
    ++cases;
    auto config = covered(); config.candidates = 1; config.commandBuffers = 1; config.barriersPerCommand = 1;
    {
        Tracker tracker(config); initialize(tracker); require(!tracker.select(candidate(11, 2)), "candidate capacity bounded");
        issue(tracker.snapshot({10, 1}), Issue::Capacity);
    }
    {
        Tracker tracker(config); initialize(tracker); tracker.allocate(101, 200, 3, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        issue(tracker.snapshot({10, 1}), Issue::Capacity);
    }
    {
        Tracker tracker(config); initialize(tracker); tracker.barrier(100, barrier()); tracker.barrier(100, barrier());
        submit(tracker); issue(tracker.snapshot({10, 1}), Issue::Capacity);
    }
    {
        Tracker tracker(config); initialize(tracker);
        std::array<Handle, Tracker::maxCommands + 1> excess{};
        tracker.submit(300, 3, excess, 0, VK_SUCCESS);
        issue(tracker.snapshot({10, 1}), Issue::Capacity);
    }
    for (auto flags : {VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}) {
        Tracker tracker(covered()); initialize(tracker); tracker.begin(100, flags, VK_SUCCESS); tracker.barrier(100, barrier()); submit(tracker);
        const std::array<Handle, 1> cb{100}; tracker.submit(300, 3, cb, 400, VK_SUCCESS);
        issue(tracker.snapshot({10, 1}), flags == VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT ? Issue::SimultaneousUse : Issue::ReusedOneTimeCommand);
    }
    bool threw = false; try { auto bad = covered(); bad.barriersPerCommand = SIZE_MAX; Tracker invalid(bad); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "extreme config is rejected before arrays touched");
}
void concurrentMetadataOnly() {
    ++cases; Tracker tracker(covered()); initialize(tracker); tracker.barrier(100, barrier()); submit(tracker);
    std::array<std::thread, 4> threads;
    for (auto& thread : threads) thread = std::thread([&] {
        for (unsigned i = 0; i < 2000; ++i) { (void)tracker.snapshot({10, 1}); (void)tracker.stats(); }
    });
    for (auto& thread : threads) thread.join();
    require(tracker.snapshot({10, 1}).submissionSerial == 1, "concurrent snapshots preserve metadata");
    noCopyClaims(tracker.snapshot({10, 1}));
}
}
int main() {
    try {
        happyPath(); layoutRules(); rejectedRangesAndOwnership(); lifecycleAndSelection();
        opaqueAndSubmissionFailure(); limitsAndFlags(); concurrentMetadataOnly();
        std::cout << "{\"passed\":true,\"cases\":" << cases << ",\"checks\":" << checks
                  << ",\"allocatedStorageBytes\":" << Tracker::allocatedStorageBytes()
                  << ",\"vulkanCalls\":0,\"gpuCompletionVerified\":false,\"copyPermissionGranted\":false}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
