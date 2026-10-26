#define XESS_PRESENTER_EMBEDDED
#include "color_presenter.hpp"
#include <iostream>
#include "presenter.cpp"
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace xess_vk {
namespace {
std::atomic<std::uint64_t> contextSerial{};
}
struct ColorPresenter::Impl {
    using Timing = detail::PresenterFrameTimings::Timing;
    using Handle = detail::PresenterFrameTimings::Handle;
    InteropBridge &bridge;
    Presenter presenter;
    std::mutex mutex;
    std::timed_mutex presentationMutex;
    std::condition_variable operationsDone;
    unsigned operations{};
    bool stopping{}, operationsShutdownFailed{}, sdkFailed{}, shutdownCompleted{}, shutdownInProgress{};
    bool lastUntimedOff{}, lastOccluded{};
    const char* onBlockedReason{};
    struct Lease {
        Impl &owner;
        explicit Lease(Impl &value) : owner(value) {
            std::lock_guard lock(owner.mutex);
            if (owner.stopping)
                throw std::logic_error("SDK owner is stopping");
            ++owner.operations;
        }
        ~Lease() {
            std::lock_guard lock(owner.mutex);
            --owner.operations;
            owner.operationsDone.notify_all();
        }
    };
    std::uint64_t generation{++contextSerial}, presents{};
    detail::PresenterFrameTimings timings{generation};
    CpuWallTime xellSleepCpu;
    Timing lastTiming;
    void clearStatus() {
        presenter.lastStatusValid = false;
        presenter.lastStatusQueryAttempted = false;
        presenter.lastStatusQueryResult = 0;
        presenter.lastStatusPresentId = 0;
        presenter.lastStatus = {};
        lastTiming = {};
        lastUntimedOff = false;
        lastOccluded = false;
        onBlockedReason = nullptr;
        presenter.borrowedSleepFrame = presenter.borrowedSleepCalls = presenter.presentMarkerCalls = 0;
        presenter.borrowedSleepResult = presenter.presentStartResult = presenter.presentEndResult = 0;
    }
    Impl(InteropBridge &b, const std::filesystem::path &sdk, HWND hwnd, bool continuous, bool initiallyQuiescent, bool gpuTimestamps)
        : bridge(b), presenter(b, sdk, false, hwnd, continuous, initiallyQuiescent, gpuTimestamps) {}
    void failedSdk() {
        std::lock_guard lock(mutex);
        sdkFailed = stopping = true;
        timings.abandonAll();
    }
    void marker(FrameTimingTicket ticket, unsigned index, xell_latency_marker_type_t type) {
        Handle handle;
        {
            std::lock_guard lock(mutex);
            handle = timings.marker(ticket, index);
            if (!handle)
                throw std::logic_error("XeLL frame marker has no matching start or was duplicated");
        }
        int result = -1;
        try {
            result = presenter.sdk.xellAddMarkerData(presenter.latency, ticket.sdkId, type);
        }
        catch (...) {
            {
                std::lock_guard lock(mutex);
                timings.completeMarker(handle, index, result);
            }
            failedSdk();
            throw;
        }
        bool accepted{};
        {
            std::lock_guard lock(mutex);
            accepted = timings.completeMarker(handle, index, result);
        }
        if (result != 0) failedSdk();
        sdkCheck(result, "xellAddMarkerData(real frame)");
        if (!accepted) throw std::logic_error("XeLL marker completed for a cancelled frame");
    }
    Handle presenting;
    bool presentingUntimed{};
    std::uint32_t admitPresent(FrameTimingTicket ticket, std::uint32_t bootstrapId = 0, bool untimed = false,
                               bool frameSlot = false) {
        if (ticket) {
            Timing timing;
            {
                std::lock_guard lock(mutex);
                presenting = timings.admit(ticket);
                timing = timings.snapshot(presenting);
            }
            if (!presenting)
                throw ColorPresenterUnavailable(
                    "Present requires the same completed CPU/recording timing ticket");
            if (!frameSlot) presenter.waitCallerCommands();
            presenter.borrowedSleepFrame = ticket.sdkId;
            presenter.borrowedSleepCalls = 1;
            presenter.borrowedSleepResult = timing.sleepResult;
            presenter.presentMarkerCalls = 0;
            presenter.presentStartResult = presenter.presentEndResult = 0;
            lastTiming = timing;
            lastUntimedOff = false;
            return ticket.sdkId;
        }
        std::uint32_t id{};
        {
            std::lock_guard lock(mutex);
            id = untimed ? timings.admitUntimed() : timings.admitBootstrap(bootstrapId);
        }
        if (!id) throw ColorPresenterUnavailable("Untimed OFF is unavailable while a real timing reservation remains or IDs are exhausted");
        presenting = {};
        presentingUntimed = true;
        lastTiming = {};
        lastUntimedOff = untimed;
        if (!frameSlot) presenter.waitFrame();
        presenter.borrowedSleepFrame = presenter.borrowedSleepCalls = presenter.presentMarkerCalls =
            0;
        presenter.borrowedSleepResult = presenter.presentStartResult = presenter.presentEndResult =
            0;
        return id;
    }
    void finishPresent(std::uint32_t id, bool success = true) {
        std::lock_guard lock(mutex);
        timings.finish(presenting, id, success);
        presenting = {};
        presentingUntimed = false;
        if (success) ++presents;
    }
};
ColorPresenter::ColorPresenter(InteropBridge &b, const std::filesystem::path &sdk, HWND hwnd,
                               bool vsync, bool continuous, bool initiallyQuiescent,
                               std::uint32_t interpolatedFrames, bool gpuTimestamps)
    : impl_(std::make_unique<Impl>(b, sdk, hwnd, continuous, initiallyQuiescent, gpuTimestamps)) {
    impl_->presenter.measurementContext=impl_->generation;
    impl_->presenter.syncInterval = vsync ? 1 : 0;
    impl_->presenter.interpolatedFramesRequest = interpolatedFrames;
    try {
        impl_->presenter.initialize();
        // No callbacks can borrow this owner until construction succeeds.
        if (continuous && initiallyQuiescent)
            impl_->timings = detail::PresenterFrameTimings{impl_->generation, true};
    } catch (...) {
        bool drained{};
        try {
            drained = impl_->presenter.shutdown();
        } catch (...) {
        }
        if (!drained) {
            impl_.release();
            throw ColorPresenterRetained("XeFG initialization failed and cleanup is unproven");
        }
        throw;
    }
}
ColorPresenter::~ColorPresenter() {
    if (!impl_) return;
    if (!impl_->shutdownCompleted) {
        try { shutdown(); }
        catch (...) { impl_.release(); }
    }
}
FrameTimingTicket ColorPresenter::beginGameFrame(std::uint32_t mappedSdkId) {
    auto &p = *impl_;
    Impl::Lease operation(p);
    if (!p.presenter.continuousMode)
        return {};
    Impl::Handle handle;
    {
        std::lock_guard lock(p.mutex);
        handle = p.timings.begin(mappedSdkId);
    }
    if (!handle) return {};
    const auto ticket = handle.ticket;
    int sleep = -1, simulation = -1;
    try {
        // Sleep mode is fixed at fresh-context initialization, never changed
        // while these CPU callbacks or rendering work may be in flight.
        const auto sleepStarted = CpuWallClock::now();
        sleep = p.presenter.sdk.xellSleep(p.presenter.latency, ticket.sdkId);
        const auto sleepNs = cpuWallElapsedNs(sleepStarted);
        {
            std::lock_guard lock(p.mutex);
            p.xellSleepCpu.add(sleepNs);
        }
        sdkCheck(sleep, "xellSleep(real frame start)");
        {
            std::lock_guard lock(p.mutex);
            if (p.timings.snapshot(handle).cancelled) {
                p.timings.completeBegin(handle, sleep, -1);
                return {};
            }
        }
        simulation = p.presenter.sdk.xellAddMarkerData(p.presenter.latency, ticket.sdkId, XELL_SIMULATION_START);
        sdkCheck(simulation, "XeLL real simulation start");
    } catch (...) {
        {
            std::lock_guard lock(p.mutex);
            p.timings.completeBegin(handle, sleep, simulation);
        }
        p.failedSdk();
        throw;
    }
    std::lock_guard lock(p.mutex);
    return p.timings.completeBegin(handle, sleep, simulation) ? ticket : FrameTimingTicket{};
}
void ColorPresenter::abandonFrameTiming(FrameTimingTicket ticket) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->timings.abandon(ticket);
}
detail::PresenterTimingStats ColorPresenter::timingStats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->timings.stats();
}
void ColorPresenter::setVsync(bool enabled) {
    std::lock_guard lock(impl_->presentationMutex);
    impl_->presenter.syncInterval = enabled ? 1 : 0;
}
void ColorPresenter::resizeOutput(std::uint32_t width, std::uint32_t height) {
    auto& p = *impl_;
    Impl::Lease operation(p);
    std::lock_guard lock(p.presentationMutex);
    auto wanted = p.bridge.config();
    wanted.width = width;
    wanted.height = height;
    wanted.validate();
    try {
        p.presenter.resizeOutput(width, height);
    } catch (...) {
        p.failedSdk();
        throw;
    }
}
void ColorPresenter::simulationEnd(FrameTimingTicket ticket) {
    Impl::Lease operation(*impl_);
    impl_->marker(ticket, 1, XELL_SIMULATION_END);
}
void ColorPresenter::renderSubmitBegin(FrameTimingTicket ticket) {
    Impl::Lease operation(*impl_);
    impl_->marker(ticket, 2, XELL_RENDERSUBMIT_START);
}
void ColorPresenter::renderSubmitEnd(FrameTimingTicket ticket) {
    Impl::Lease operation(*impl_);
    impl_->marker(ticket, 3, XELL_RENDERSUBMIT_END);
}
void ColorPresenter::present(const NativeImageSource &source, VkQueue queue, VkFence fence) {
    present(source, queue, fence, {});
}
void ColorPresenter::present(const NativeImageSource &source, VkQueue queue, VkFence fence,
                             FrameTimingTicket timing, std::uint32_t bootstrapSdkId) {
    presentColor(source, queue, fence, timing, bootstrapSdkId, false);
}
void ColorPresenter::presentQueuedOff(const NativeImageSource &source, VkQueue queue, VkFence fence,
                                      FrameTimingTicket timing) {
    presentColor(source, queue, fence, timing, 0, true);
}
void ColorPresenter::presentQueuedOff(const NativeImageSource& source, VkQueue queue, VkFence fence,
                                      FrameTimingTicket timing, FrameSlotKey key, FrameCompletion& receipt) {
    if (key != receipt.key) throw std::logic_error("Queued OFF slot and receipt differ");
    presentColor(source, queue, fence, timing, 0, true, &receipt);
}
void ColorPresenter::presentSharedQueuedOff(SharedPlaneKey shared, VkQueue queue, VkFence fence,
                                           FrameTimingTicket timing, FrameSlotKey key, FrameCompletion& receipt) {
    presentShared(shared, queue, fence, nullptr, false, timing, {}, key, receipt);
}
void ColorPresenter::presentSharedBundleQueued(SharedPlaneKey shared, VkQueue queue, VkFence fence,
                                              const frame_input::FrameConstantsSample& sample, bool resetHistory,
                                              FrameTimingTicket timing, VkRect2D rect, FrameSlotKey key,
                                              FrameCompletion& receipt) {
    presentShared(shared, queue, fence, &sample, resetHistory, timing, rect, key, receipt);
}
void ColorPresenter::presentShared(SharedPlaneKey shared, VkQueue queue, VkFence fence,
                                   const frame_input::FrameConstantsSample* sample, bool resetHistory,
                                   FrameTimingTicket timing, VkRect2D rect, FrameSlotKey key, FrameCompletion& receipt) {
    const bool enabled = sample != nullptr;
    if (!shared.valid() || key != receipt.key || !detail::freshFrameCompletion(receipt) ||
        (receipt.sharedKey != SharedPlaneKey{} && receipt.sharedKey != shared) || !timing ||
        (enabled && !timing.latencyEnabled))
        throw std::logic_error("Shared presentation requires a fresh exact frame and real timing ticket");
    receipt.sharedKey = shared; receipt.sharedAuxiliary = enabled;
    auto& p = *impl_;
    Impl::Lease operation(p);
    std::lock_guard lock(p.presentationMutex);
    p.clearStatus();
    if (!p.presenter.continuousMode) throw std::logic_error("Shared presentation requires a continuous owner");
    xefg_swapchain_frame_constant_data_t constants{};
    if (enabled) {
        bool recovery{};
        { std::lock_guard lock(p.mutex); recovery = p.timings.recoveryNeeded(); }
        constants = nativeFrameConstants(*sample, resetHistory || recovery);
        const auto config = p.bridge.config(shared);
        if (config.colorOnly) throw std::logic_error("Shared ON requires allocated auxiliary planes");
        rect = nativeOutputInterpolationRect(*sample, {config.width, config.height},
            config.extent(Format::DepthR32F), config.extent(Format::MotionRG16F), rect);
    }
    std::uint32_t id{};
    bool prepared{};
    try {
        p.presenter.prepareFrameSlot(receipt); prepared = true;
        id = p.admitPresent(timing, 0, false, true);
        p.presenter.setEnabled(enabled);
        const auto offBefore = p.presenter.validOffPresents;
        p.bridge.publishQueuedSharedFrame(shared, queue, fence, key, receipt);
        p.lastOccluded = !p.presenter.renderPublishedFrame(id, 0, enabled ? &constants : nullptr,
            !enabled, true, rect, &receipt);
        if (p.lastOccluded) { p.finishPresent(id); return; }
        if (p.presenter.statusQueryFailures || (!enabled && p.presenter.validOffPresents != offBefore + 1))
            throw std::runtime_error("Shared presentation did not report the requested SDK output");
        p.finishPresent(id);
        if (enabled) { std::lock_guard lock(p.mutex); p.timings.recovered(); }
    } catch (...) {
        p.presenter.poisonFrame(receipt, prepared);
        if (id || p.presenting || p.presentingUntimed) { p.finishPresent(id, false); p.failedSdk(); }
        throw;
    }
}
void ColorPresenter::presentColor(const NativeImageSource &source, VkQueue queue, VkFence fence,
                                 FrameTimingTicket timing, std::uint32_t bootstrapSdkId, bool queued,
                                 FrameCompletion* receipt) {
    auto &p = *impl_;
    Impl::Lease operation(p);
    std::lock_guard lock(p.presentationMutex);
    p.clearStatus();
    std::uint32_t id{};
    bool framePrepared{};
    try {
        if (queued && p.presenter.continuousMode && !timing)
            throw std::logic_error("Queued continuous OFF requires a real frame timing ticket");
        if (receipt) { p.presenter.prepareFrameSlot(*receipt); framePrepared = true; }
        id = p.admitPresent(timing, bootstrapSdkId, false, receipt != nullptr);
        p.presenter.setEnabled(false);
        const auto offBefore = p.presenter.validOffPresents;
        if (receipt) p.bridge.publishQueuedNativeColor(source, queue, fence, receipt->key, *receipt);
        else if (queued) p.bridge.publishQueuedNativeColor(source, queue, fence);
        else p.bridge.publishNativeColor(source, queue, fence);
        p.lastOccluded = !p.presenter.renderPublishedFrame(id, 0, nullptr, true, bool(timing), {}, receipt);
        if (p.lastOccluded) {
            p.finishPresent(id);
            return;
        }
        if (p.presenter.statusQueryFailures || p.presenter.validOffPresents != offBefore + 1)
            throw std::runtime_error("XeFG OFF did not report exactly one non-generated frame");
        p.finishPresent(id);
    } catch (...) {
        if (receipt) p.presenter.poisonFrame(*receipt, framePrepared);
        if (id || p.presenting || p.presentingUntimed) {
            p.finishPresent(id, false);
            p.failedSdk();
        }
        throw;
    }
}
void ColorPresenter::presentBootstrap(const NativeImageSource &source, VkQueue queue, VkFence fence,
                                      std::uint32_t mappedSdkId) {
    present(source, queue, fence, {}, mappedSdkId);
}
std::uint32_t ColorPresenter::presentUntimedOff(const NativeImageSource &source, VkQueue queue, VkFence fence) {
    auto &p = *impl_;
    Impl::Lease operation(p);
    std::lock_guard lock(p.presentationMutex);
    p.clearStatus();
    p.lastUntimedOff = true;
    std::uint32_t id{};
    try {
        id = p.admitPresent({}, 0, true);
        p.presenter.setEnabled(false);
        const auto before = p.presenter.validOffPresents;
        p.bridge.publishNativeColor(source, queue, fence);
        p.lastOccluded = !p.presenter.renderPublishedFrame(id, 0, nullptr, true, false);
        if (p.lastOccluded) {
            p.finishPresent(id);
            return id;
        }
        if (p.presenter.statusQueryFailures || p.presenter.validOffPresents != before + 1)
            throw std::runtime_error("Untimed OFF did not report one non-generated frame");
        p.finishPresent(id);
        return id;
    } catch (...) {
        if (id || p.presentingUntimed) {
            p.finishPresent(id, false);
            p.failedSdk();
        }
        throw;
    }
}
void ColorPresenter::presentBundle(const NativeFrame &frame,
                                   const frame_input::FrameConstantsSample &sample, bool enabled,
                                   bool resetHistory) {
    presentBundle(frame, sample, enabled, resetHistory, {});
}
void ColorPresenter::presentBundle(const NativeFrame &frame,
                                   const frame_input::FrameConstantsSample &sample, bool enabled,
                                   bool resetHistory, FrameTimingTicket timing) {
    presentBundle(frame, sample, enabled, resetHistory, timing, {});
}
void ColorPresenter::presentBundle(const NativeFrame &frame,
                                   const frame_input::FrameConstantsSample &sample, bool enabled,
                                   bool resetHistory, FrameTimingTicket timing, VkRect2D outputInterpolationRect) {
    presentBundleImpl(frame, sample, enabled, resetHistory, timing, outputInterpolationRect, false);
}
void ColorPresenter::presentBundleQueued(const NativeFrame &frame,
                                         const frame_input::FrameConstantsSample &sample,
                                         bool resetHistory, FrameTimingTicket timing, VkRect2D outputInterpolationRect) {
    if (!timing || !timing.latencyEnabled)
        throw std::logic_error("Queued ON requires a real latency-enabled continuous frame ticket");
    presentBundleImpl(frame, sample, true, resetHistory, timing, outputInterpolationRect, true);
}
void ColorPresenter::presentBundleQueued(const NativeFrame& frame,
                                         const frame_input::FrameConstantsSample& sample,
                                         bool resetHistory, FrameTimingTicket timing, VkRect2D rect,
                                         FrameSlotKey key, FrameCompletion& receipt) {
    if (key != receipt.key || !timing || !timing.latencyEnabled)
        throw std::logic_error("Queued ON requires matching slot receipt and latency-enabled real ticket");
    presentBundleImpl(frame, sample, true, resetHistory, timing, rect, true, &receipt);
}
void ColorPresenter::presentBundleImpl(const NativeFrame &frame,
                                       const frame_input::FrameConstantsSample &sample, bool enabled,
                                       bool resetHistory, FrameTimingTicket timing, VkRect2D outputInterpolationRect, bool queued,
                                       FrameCompletion* receipt) {
    if (!enabled || (timing && !timing.latencyEnabled)) {
        present(frame.planes[0], frame.producerQueue, frame.producerCompleteFence, timing);
        if (enabled) {
            std::lock_guard lock(impl_->presentationMutex);
            impl_->onBlockedReason = "ll_mode_change_requires_gpu_quiescence";
        }
        return;
    }
    auto &p = *impl_;
    Impl::Lease operation(p);
    std::lock_guard lock(p.presentationMutex);
    p.clearStatus();
    if (!p.presenter.continuousMode || !timing)
        throw std::logic_error("Native XeFG ON requires a real frame timing ticket");
    bool recovery{};
    {
        std::lock_guard lock(p.mutex);
        recovery = p.timings.recoveryNeeded();
    }
    const auto constants = nativeFrameConstants(sample, resetHistory || recovery);
    const auto input = frame.planes[1].successfulImageCreateInfo.extent;
    const auto motion = frame.planes[2].successfulImageCreateInfo.extent;
    const auto &config = p.bridge.config();
    outputInterpolationRect = nativeOutputInterpolationRect(sample, {config.width, config.height},
                                                           {input.width, input.height},
                                                           {motion.width, motion.height}, outputInterpolationRect);
    std::uint32_t id{};
    bool framePrepared{};
    try {
        if (receipt) { p.presenter.prepareFrameSlot(*receipt); framePrepared = true; }
        id = p.admitPresent(timing, 0, false, receipt != nullptr);
        p.bridge.enableAuxiliaryInputs(input.width, input.height);
        p.presenter.setEnabled(true);
        if (receipt) p.bridge.publishQueuedNativeFrame(frame, receipt->key, *receipt);
        else if (queued) p.bridge.publishQueuedNativeFrame(frame);
        else p.bridge.publishNativeFrame(frame);
        p.lastOccluded = !p.presenter.renderPublishedFrame(id, 0, &constants, false, true, outputInterpolationRect, receipt);
        if (p.lastOccluded) {
            p.finishPresent(id);
            return;
        }
        if (p.presenter.statusQueryFailures)
            throw std::runtime_error("XeFG native status query failed");
        p.finishPresent(id);
        {
            std::lock_guard lock(p.mutex);
            p.timings.recovered();
        }
    } catch (...) {
        if (receipt) p.presenter.poisonFrame(*receipt, framePrepared);
        if (id || p.presenting) {
            p.finishPresent(id, false);
            p.failedSdk();
        }
        throw;
    }
}
FrameCompletionState ColorPresenter::frameComplete(const FrameCompletion& receipt) const noexcept {
    try {
        std::unique_lock lock(impl_->presentationMutex, std::try_to_lock);
        if (!lock.owns_lock()) return FrameCompletionState::Pending;
        return impl_->presenter.frameComplete(receipt);
    } catch (...) { return FrameCompletionState::Invalid; }
}
void ColorPresenter::waitFrame(const FrameCompletion& receipt) {
    const auto deadline = GetTickCount64() + 10000;
    std::unique_lock lock(impl_->presentationMutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(10)))
        throw ColorPresenterRetained("A Presenter call still owns the frame slot after the finite wait");
    const auto now = GetTickCount64();
    impl_->presenter.waitFrame(receipt, now < deadline ? static_cast<std::uint32_t>(deadline - now) : 0);
}
void ColorPresenter::requestStop() {
    std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
    impl_->sdkFailed = true;
    impl_->timings.abandonAll();
}
void ColorPresenter::stopAdmission() {
    std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
}
void ColorPresenter::shutdown() {
    std::unique_lock operations(impl_->mutex);
    impl_->stopping = true;
    const auto admission = detail::admitPresenterShutdown(impl_->shutdownCompleted, impl_->shutdownInProgress,
                                                         impl_->sdkFailed || impl_->operationsShutdownFailed);
    if (admission == detail::ShutdownAdmission::AlreadyCompleted) return;
    if (admission == detail::ShutdownAdmission::Retain)
        throw ColorPresenterRetained("Failed or concurrently closing SDK owner is retained without another vendor destruction");
    if (!detail::waitForPresenterOperations(operations, impl_->operationsDone,
                                           impl_->operations, std::chrono::seconds(10),
                                           impl_->operationsShutdownFailed))
        throw ColorPresenterRetained("SDK operations have not drained; all owners must remain live");
    operations.unlock();
    std::lock_guard lock(impl_->presentationMutex);
    bool failed{};
    try {
        if (!detail::runPresenterShutdownStages(failed,
                [&] {
                    std::cerr << "Presentation shutdown: bridge consumed enter\n";
                    impl_->bridge.waitUntilConsumed();
                    std::cerr << "Presentation shutdown: bridge consumed return\n";
                },
                [] {}, [&] { return impl_->presenter.shutdown(); }))
            throw ColorPresenterRetained("XeFG shutdown failed after shared GPU drain");
        std::lock_guard completed(impl_->mutex);
        impl_->shutdownCompleted = true;
    } catch (...) {
        std::lock_guard retained(impl_->mutex);
        impl_->sdkFailed = true;
        throw;
    }
}
std::uint64_t ColorPresenter::frames() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->presents;
}
std::uint64_t ColorPresenter::timingContextGeneration() const {
    return impl_->generation;
}
std::uint32_t ColorPresenter::nextSdkId() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->timings.nextId();
}
ColorPresentStatus ColorPresenter::lastPresentStatus() const {
    std::lock_guard lock(impl_->presentationMutex);
    const auto callerGpu = impl_->presenter.callerTimestampSample();
    const auto &p = *impl_;
    const auto &presenter = p.presenter;
    std::lock_guard timingLock(impl_->mutex);
    ColorPresentStatus status;
    status.bridgePublishCpu = p.bridge.stats().nativePublishCpu;
    status.previousD3DWaitCpu = presenter.previousD3DWaitCpu;
    status.dxgiPresentCpu = presenter.dxgiPresentCpu;
    status.xellSleepCpu = p.xellSleepCpu;
    status.d3dCallerCopyTagGpu = callerGpu;
    if (status.d3dCallerCopyTagGpu.requested)
        status.d3dCallerCopyTagGpu.contextGeneration = p.generation;
    status.presentId = presenter.lastStatusQueryAttempted ? presenter.lastStatusPresentId :
        (p.lastTiming.ticket.sdkId ? p.lastTiming.ticket.sdkId : p.timings.lastPresented());
    status.sdkPresentId = status.presentId;
    status.untimedOff = p.lastUntimedOff;
    status.latencyTransitionAdmitted = p.timings.latencyCapability();
    status.onBlockedReason = p.onBlockedReason;
    status.framesPresented = presenter.lastStatus.framesPresented;
    status.fgTuning=presenter.tuning;status.fgOutcomes=presenter.tuningTotals;
    status.interpolatedFrames = presenter.appliedInterpolatedFrames;
    status.multiplierRequested=presenter.multiplierRequested;
    status.multiplierMaximum=presenter.appliedInterpolatedFrames+1;
    status.frameGenResult = presenter.lastStatus.frameGenResult;
    status.queryResult = presenter.lastStatusQueryResult;
    status.queryAttempted = presenter.lastStatusQueryAttempted;
    status.requestedEnabled = presenter.lastRequestedEnabled;
    status.isFrameGenEnabled = presenter.lastStatus.isFrameGenEnabled != 0;
    status.valid = presenter.lastStatusValid;
    status.occluded = p.lastOccluded;
    status.sleepFrameId = presenter.borrowedSleepFrame;
    status.sleepCalls = presenter.borrowedSleepCalls;
    status.presentMarkerCalls = presenter.presentMarkerCalls;
    status.consecutiveEnabledPresents = presenter.enabledRunFrames;
    status.sleepResult = presenter.borrowedSleepResult;
    status.presentStartResult = presenter.presentStartResult;
    status.presentEndResult = presenter.presentEndResult;
    status.completeFrameTiming = bool(p.lastTiming.ticket);
    if (p.lastUntimedOff) status.timingPolicy = "borrowed-untimed-off-no-markers";
    if (status.completeFrameTiming) {
        status.totalMarkerCalls = 4 + presenter.presentMarkerCalls;
        status.simulationStartResult = p.lastTiming.results[0];
        status.simulationEndResult = p.lastTiming.results[1];
        status.renderStartResult = p.lastTiming.results[2];
        status.renderEndResult = p.lastTiming.results[3];
        status.timingPolicy = "borrowed-real-frame-ticket";
    }
    return status;
}
} // namespace xess_vk
