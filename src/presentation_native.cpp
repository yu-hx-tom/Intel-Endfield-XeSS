#include "color_presenter.hpp"
#include "continuous_provider.hpp"
#include "frame_camera_input.hpp"
#include "fsr_menu_control.hpp"
#include "diagnostic_flags.hpp"
#include "launcher_preferences.hpp"
#include "presentation_runtime.hpp"
#include "presentation_output_geometry.hpp"
#include "presentation_window_shutdown.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
namespace xess_vk::presentation {
#ifdef XESS_PRESENTATION_NATIVE_TEST
namespace testing {
std::chrono::steady_clock::time_point outputNow() noexcept;
bool longFrameFollowupReset() noexcept;
bool asyncCopyPath() noexcept;
std::uint32_t asyncCopyFrames() noexcept;
bool sharedCopyPath() noexcept;
bool sharedCopySync() noexcept;
}
#endif
namespace {
void check(VkResult r, const char *name) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(name) + " result=" + std::to_string(r));
}
LRESULT CALLBACK hiddenProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP + 17)
        return SetWindowPos(hwnd, nullptr, 0, 0, LOWORD(wp), HIWORD(wp),
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (msg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace
struct Surface::Impl {
    HWND original{}, hidden{};
    HANDLE ready{};
    bool startupReady{};
    std::thread thread;
    explicit Impl(HWND hwnd) : original(hwnd) {
        if (!IsWindow(hwnd))
            throw std::invalid_argument("Presentation requires a real application HWND");
        // A retained hidden HWND and its thread can outlive normal cleanup.
        // Keep the WNDPROC code resident for that entire process lifetime.
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&hiddenProc), &pinned))
            throw std::runtime_error("Could not pin hidden window procedure module");
        ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready)
            throw std::runtime_error("Hidden HWND ready event failed");
        try { thread = std::thread([this] {
            SetThreadDescription(GetCurrentThread(),L"XeSS Vulkan WSI bridge");
            WNDCLASSW wc{};
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpfnWndProc = hiddenProc;
            wc.lpszClassName = L"XessPrivateNativeWsi";
            RegisterClassW(&wc);
            RECT rect{};
            GetClientRect(original, &rect);
            hidden = CreateWindowExW(
                WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Private Vulkan WSI",
                WS_POPUP, 0, 0, rect.right > 0 ? rect.right : 1, rect.bottom > 0 ? rect.bottom : 1,
                nullptr, nullptr, wc.hInstance, nullptr);
            SetEvent(ready);
            if (!hidden)
                return;
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }); } catch (...) {
            CloseHandle(ready); ready = nullptr;
            throw;
        }
        startupReady = WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0;
        if (startupReady) {
            CloseHandle(ready); ready = nullptr;
        }
    }
    bool close() noexcept {
        if (!thread.joinable()) return true;
        return detail::closeWindowThread([&] {
            // hidden is readable only after the ready event published it.
            if (startupReady && hidden) PostMessageW(hidden, WM_CLOSE, 0, 0);
        }, [&](std::uint32_t timeout) {
            return WaitForSingleObject(thread.native_handle(), timeout) == WAIT_OBJECT_0;
        }, [&] { thread.join(); }, 10000);
    }
    ~Impl() {
        if (ready) CloseHandle(ready);
    }
};
Surface::Surface(HWND hwnd) : impl_(std::make_unique<Impl>(hwnd)) {
    if (!impl_->startupReady) {
        // The worker may still be writing hidden. Do not inspect it, destroy
        // its captured owner, cancel it, or run a joinable thread destructor.
        impl_.release();
        throw std::runtime_error("Private hidden HWND startup timed out; thread owner retained");
    }
    if (!impl_->hidden) {
        if (!impl_->close()) impl_.release();
        throw std::runtime_error("Private hidden HWND creation failed");
    }
}
Surface::~Surface() {
    if (impl_ && !impl_->close()) {
        OutputDebugStringA("Private hidden HWND thread did not finish; complete owner retained");
        impl_.release();
    }
}
HWND Surface::original() const {
    return impl_->original;
}
HWND Surface::hidden() const {
    return impl_->hidden;
}
bool Surface::resize(VkExtent2D size) {
    if (!size.width || !size.height || size.width > 16384 || size.height > 16384)
        return false;
    RECT actual{};
    DWORD_PTR resized{};
    return SendMessageTimeoutW(hidden(), WM_APP + 17, MAKEWPARAM(size.width, size.height), 0,
               SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &resized) != 0 && resized != 0 &&
           GetClientRect(hidden(), &actual) && actual.right == static_cast<LONG>(size.width) &&
           actual.bottom == static_cast<LONG>(size.height);
}
struct DeviceRecord::Impl {
    bool fake{};
    bool gpuTimestamps{};
    QueueGate* queueGate{};
    wsi_acquire::QueueObserver queueObserver{};
    void* queueObserverContext{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetDeviceProcAddr gdpa{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    float priority = 1;
    std::vector<std::string> extensionStorage;
    std::vector<const char *> extensions;
    template <class T> T vk(const char *n) const {
        auto p = gdpa(device, n);
        if (!p)
            throw std::runtime_error(std::string("Missing native dispatch ") + n);
        return reinterpret_cast<T>(p);
    }
    template <class T> T ivk(const char *n) const {
        auto p = gipa(instance, n);
        if (!p)
            throw std::runtime_error(std::string("Missing instance dispatch ") + n);
        return reinterpret_cast<T>(p);
    }
    BorrowedVulkanContext borrowed(VkQueue queue) {
        return {instance, physical, device, queue,         queueInfo.queueFamilyIndex,
                0,        gipa,     gdpa,   &instanceInfo, &deviceInfo,queueGate,queueObserver,queueObserverContext,gpuTimestamps};
    }
};
namespace {
// Open only after the current game's ordinary presentation/input acceptance.
constexpr bool gameOnAdmitted = true;
class NativeBackend final : public Backend {
    struct FrameContext {
        VkCommandBuffer command_{};
        VkFence fence_{};
        VkSemaphore queuedSemaphore_{};
        bool queuedOff_{}, queuedOn_{}, queuedBundleEntered_{}, queuedBundlePublished_{};
        fsr_menu_control::ControlIntent queuedIntent_;
        VkRect2D queuedRect_{};
        std::uint64_t queuedCutoff_{};
        FrameTimingAssociation queuedFrame_;
        std::array<VkSemaphore,16> pendingAppWaits_{};
        std::uint32_t pendingAppWaitCount_{};
        bool healthyQueuedWaits_{}, pending_{}, slotOwned_{}, nativeCompleted_{};
        CopyProgress copyProgress_{CopyProgress::NotSubmitted};
        continuous::Bundle bundle_;
        SharedPlaneLease shared_;
        CopyFenceTiming pendingCopyTiming_;
        FrameSlotKey key_;
        FrameCompletion completion_;
        VkQueryPool timestampPool_{};
        PFN_vkDestroyQueryPool destroyTimestampPool_{};
        PFN_vkCmdResetQueryPool resetTimestamps_{};
        PFN_vkCmdWriteTimestamp writeTimestamp_{};
        PFN_vkGetQueryPoolResults readTimestamps_{};
        PFN_vkGetCalibratedTimestampsKHR calibrateTimestamps_{};
        bool timestampActive_{}, timestampFailed_{}, timestampRecorded_{}, timestampPendingValid_{};
        std::uint64_t timestampQpcFrequency_{};
        std::uint32_t timestampValidBits_{};
        double timestampPeriodNs_{};
        GpuTimestampSample timestampRecording_, timestampPending_, timestampLatest_;
        GpuTimestampCalibration timestampCalibration_;
    };
    std::array<FrameContext,frameSlotCount> frames_;
    std::uint32_t frameIndex_{}, nextFrameIndex_{}, inflightHighWater_{}, asyncFrameLimit_{frameSlotCount};
    std::uint64_t frameGeneration_{};
    std::uint64_t timestampSerial_{}, timestampContext_{};
    bool asyncCopy_{},sharedCopy_{};
    VkExtent2D sharedInputExtent_{};
    const char* sharedFallbackReason_{};
    continuous::SharedInputAccess sharedInputAccess_;
    CpuWallTime inflightCapWaitCpu_;
    GpuTimestampSample latestPrivateTimestamp_;
    FrameContext& frame() noexcept { return frames_[frameIndex_]; }
    const FrameContext& frame() const noexcept { return frames_[frameIndex_]; }
    bool hasAsyncFrames() const noexcept {
        return std::any_of(frames_.begin(),frames_.end(),[](const auto& f){return f.slotOwned_;});
    }
    VkResult retireAsyncFrame(FrameContext& copy, bool wait, bool cap = false) {
        if (!copy.slotOwned_) return VK_SUCCESS;
        if (!presenter_ || copy.completion_.poisoned) return VK_ERROR_DEVICE_LOST;
        auto tail = presenter_->frameComplete(copy.completion_);
        if (tail == FrameCompletionState::Pending && wait) {
            try {
                if (cap) { CpuWallScope elapsed(inflightCapWaitCpu_,measurement::Metric::CapWait); presenter_->waitFrame(copy.completion_); }
                else presenter_->waitFrame(copy.completion_);
            } catch (const std::exception& e) { error_=e.what(); return VK_ERROR_DEVICE_LOST; }
            tail=presenter_->frameComplete(copy.completion_);
        }
        if (tail == FrameCompletionState::Invalid || tail == FrameCompletionState::DeviceLost)
            return VK_ERROR_DEVICE_LOST;
        if (!copy.nativeCompleted_) {
            const auto result=vk<PFN_vkGetFenceStatus>("vkGetFenceStatus")(record_.device,copy.fence_);
            if (result != VK_SUCCESS && result != VK_NOT_READY) return result;
            if (result == VK_SUCCESS) {
                copy.nativeCompleted_=true; copy.pending_=false;
                { std::lock_guard lock(sourceCompletionMutex_);
                  completedSourceCopy_=std::max(completedSourceCopy_,copy.pendingCopyTiming_.sequence); }
                const auto observed=cpuWallNowNs();
                copy.pendingCopyTiming_.waitEntryNs=copy.pendingCopyTiming_.waitReturnNs=observed;
                copy.pendingCopyTiming_.waitResult=VK_SUCCESS;
                copy.pendingCopyTiming_.privateCopyGpu={};
                copy.pendingCopyTiming_.privateCopyGpu.requested=record_.gpuTimestamps;
                copy.pendingCopyTiming_.privateCopyGpu.queryStateKnown=record_.gpuTimestamps;
                copy.pendingCopyTiming_.privateCopyGpu.diagnosticActive=copy.timestampActive_;
                copy.pendingCopyTiming_.privateCopyQueryMatch=false;
                if(readCompletedTimestamps(&copy)&&copy.timestampLatest_.sourceCopyTokenKnown&&
                    copy.timestampLatest_.sourceCopyToken==copy.pendingCopyTiming_.sequence&&
                    copy.timestampLatest_.generation==copy.pendingCopyTiming_.generation) {
                    copy.pendingCopyTiming_.privateCopyGpu=copy.timestampLatest_;
                    copy.pendingCopyTiming_.privateCopyQueryMatch=true;
                    if(copy.timestampLatest_.sourceCopyToken>=latestPrivateTimestamp_.sourceCopyToken)
                        latestPrivateTimestamp_=copy.timestampLatest_;
                }
                previousCopyTiming_=copy.pendingCopyTiming_;
                { std::lock_guard lock(copyDependencyMutex_); copy.pendingAppWaitCount_=0; copy.healthyQueuedWaits_=false; }
                if(provider_&&copy.queuedFrame_.valid)
                    provider_->retireCompletedForPresent(queue_,copy.fence_,copy.queuedCutoff_,copy.queuedFrame_);
            }
        }
        if(copy.bundle_.ready) {
            if(copy.queuedBundlePublished_) {
                if(!copy.completion_.consumedValid) return VK_ERROR_DEVICE_LOST;
                const auto consumed=bridge_->consumedComplete(copy.completion_.consumedValue);
                if(consumed==FrameCompletionState::Invalid||consumed==FrameCompletionState::DeviceLost) return VK_ERROR_DEVICE_LOST;
                if(consumed==FrameCompletionState::Complete&&
                   (!copy.bundle_.shared.key.valid()||tail==FrameCompletionState::Complete)) {
                    const bool released=provider_->releaseAfterBridge(copy.bundle_.ticket,&copy.completion_);
                    if(copy.bundle_.shared.key.valid()&&!released)return VK_ERROR_DEVICE_LOST;
                    copy.bundle_={};copy.shared_={};
                }
            } else if(copy.queuedBundleEntered_) return VK_ERROR_DEVICE_LOST;
            else if(copy.nativeCompleted_) { provider_->rollbackQueuedAcquire(copy.bundle_.ticket); copy.bundle_={}; }
        }
        if(copy.shared_.key.valid()&&!copy.bundle_.ready&&tail==FrameCompletionState::Complete&&copy.nativeCompleted_) {
            if(!bridge_->releaseSharedFrame(copy.shared_.key,copy.completion_))return VK_ERROR_DEVICE_LOST;
            copy.shared_={};
        }
        if(tail==FrameCompletionState::Complete&&copy.nativeCompleted_&&!copy.bundle_.ready) {
            copy.slotOwned_=false;
            copy.queuedOff_=copy.queuedOn_=copy.queuedBundleEntered_=copy.queuedBundlePublished_=false;
            copy.copyProgress_=CopyProgress::Completed;
        }
        return wait&&copy.slotOwned_ ? VK_NOT_READY : VK_SUCCESS;
    }
    VkResult retireAsyncFrames(bool wait = false) {
        for(auto& copy:frames_) { const auto result=retireAsyncFrame(copy,wait); if(result!=VK_SUCCESS)return result; }
        return VK_SUCCESS;
    }
    VkResult admitAsyncFrame() {
        auto result=retireAsyncFrames(); if(result!=VK_SUCCESS)return result;
        auto& copy=frames_[nextFrameIndex_];
        if(copy.slotOwned_) { result=retireAsyncFrame(copy,true,true); if(result!=VK_SUCCESS)return result; }
        if(copy.pending_||copy.bundle_.ready||copy.shared_.key.valid()||frameGeneration_==UINT64_MAX) return VK_ERROR_UNKNOWN;
        frameIndex_=nextFrameIndex_; nextFrameIndex_=(nextFrameIndex_+1)%asyncFrameLimit_;
        copy.key_={frameIndex_,++frameGeneration_}; copy.completion_={copy.key_};
        copy.slotOwned_=true; copy.nativeCompleted_=false;
        { std::lock_guard lock(sourceCompletionMutex_); copy.pendingCopyTiming_={}; }
        copy.queuedFrame_={presentEngineFrame_,presentTiming_,bootstrapId_,bool(presentTiming_)};
        const auto occupied=static_cast<std::uint32_t>(std::count_if(frames_.begin(),frames_.end(),[](const auto& f){return f.slotOwned_;}));
        inflightHighWater_=std::max(inflightHighWater_,occupied);
        return VK_SUCCESS;
    }
    struct FakeImage {
        VkImage image{};
        VkDeviceMemory memory{};
        std::uint64_t lastCopyToken{};
        std::uint64_t lastAcquireToken{};
    };
    struct FakePool {
        std::vector<FakeImage> images;
        VkFence retirementFence{};
        bool tailSubmitted{};
        std::uint64_t coveredCopyToken{};
    };
    std::map<std::uint64_t, FakePool> fakePools_;
    std::mutex fakePoolsMutex_;
    mutable std::mutex sourceCompletionMutex_;
    std::uint64_t submittedSourceCopy_{}, completedSourceCopy_{};
    DeviceRecord::Impl &record_;
    Surface &surface_;
    VkQueue queue_{};
    VkExtent2D extent_{};
    VkExtent2D wantedExtent_{};
    VkExtent2D colorExtent_{};
    VkRect2D lowResRegion_{};
    bool lowResRequested_{}, lowResActive_{}, linearBlitSupported_{};
    const char* lowResFallbackReason_{};
    // The last admitted low canvas and the WSI extent it was computed for.
    // Frames without their own geometry (game menus, missing or late inputs,
    // OFF) keep it: each canvas change resizes the proxy swapchain, a visible
    // hitch, and also rescales overlays drawn into its back buffers.
    VkExtent2D heldLowResCanvas_{}, heldLowResSource_{};
    bool lowResHeld_{};
    VkFormat format_{};
    VkFormat wantedFormat_{};
    VkPresentModeKHR presentMode_{};
    VkPresentModeKHR wantedPresentMode_{};
    VkImage stable_{};
    VkDeviceMemory memory_{};
    VkCommandPool pool_{};
    VkSemaphore copySemaphore_{};
    std::map<std::pair<std::uint64_t, VkImage>, VkSemaphore> queuedSemaphores_;
    mutable std::mutex copyDependencyMutex_;
    bool emergencyWaitHandedOff_{}, sdkFailed_{};
    ShutdownState shutdown_;
    bool initialized_{}, active_{}, unsafe_{};
    std::atomic<bool> timingStopped_{};
    std::unique_ptr<InteropBridge> bridge_;
    std::shared_ptr<ColorPresenter> presenter_;
    mutable std::mutex presenterMutex_;
    std::shared_ptr<ColorPresenter> borrowPresenter() const {
        std::lock_guard lock(presenterMutex_);
        return timingStopped_.load() ? nullptr : presenter_;
    }
    std::filesystem::path sdk_;
    std::string error_;
    continuous::Provider *provider_{};
    FrameTimingTicket presentTiming_;
    std::uint32_t presentEngineFrame_{}, bootstrapId_{};
    FrameStatus status_;
    std::uint32_t lastSdkResultId_{};
    void observeSdkResult(const ColorPresentStatus& sdk) noexcept {
        // Admission may throw before ColorPresenter clears its previous result.
        if(!sdk.queryAttempted||!sdk.sdkPresentId||sdk.sdkPresentId==lastSdkResultId_)return;
        lastSdkResultId_=sdk.sdkPresentId;
        status_.sdkResultKnown=true;
        status_.sdkStatusValid=sdk.valid;
        status_.queryResult=sdk.queryResult;
        status_.frameGenResult=sdk.frameGenResult;
        status_.framesPresented=sdk.framesPresented;
        status_.requestedEnabled=sdk.requestedEnabled;
        status_.sdkEnabled=sdk.isFrameGenEnabled;
        status_.sdkPresentId=sdk.sdkPresentId;
        status_.fgTuning=sdk.fgTuning;status_.fgOutcomes=sdk.fgOutcomes;
    }
    void observeFailedSdkResult() noexcept {
        if(presenter_)try {observeSdkResult(presenter_->lastPresentStatus());}catch(...) {}
    }
    bool previousEnabled_{}, historyGap_{true};
    std::uint64_t previousToken_{}, previousViewport_{}, previousEpoch_{};
    std::array<std::int32_t, 4> previousOutputRect_{};
    // Real-frame cadence used to skip interpolation across stalls (see output()).
    static constexpr double longFrameFloorMs = 40.0, longFrameFactor = 2.5;
    std::chrono::steady_clock::time_point previousOutputTime_{};
    double outputIntervalEmaMs_{16.7};
    bool previousOutputTimed_{};
    std::uint64_t longFrameResets_{};
    bool followupPending_{};
    LongFrameResetTrace::Trigger followupTrigger_, clearedFollowupTrigger_;
    LongFrameResetTrace resetTrace_;
    const char* followupClearReason_{};
    unsigned resetTraceTail_{};
    std::uint64_t resetTraceSequence_{};
    std::uint64_t previousFollowupContext_{}, previousFollowupCopyGeneration_{};
    void clearFollowup(const char* reason) noexcept {
        if (!followupPending_) return;
        followupPending_ = false;
        clearedFollowupTrigger_ = followupTrigger_;
        followupClearReason_ = reason;
    }
    void beginResetTrace(bool outputEntry) noexcept {
        resetTrace_ = {};
#ifdef XESS_PRESENTATION_NATIVE_TEST
        resetTrace_.policyEnabled = testing::longFrameFollowupReset();
#else
        resetTrace_.policyEnabled = diagnostic_flags::longFrameFollowupReset();
#endif
        resetTrace_.requested = resetTrace_.policyEnabled || record_.gpuTimestamps;
        if (!resetTrace_.requested) { followupPending_ = false; followupClearReason_ = nullptr; resetTraceTail_ = 0; return; }
        resetTrace_.traceSequence = ++resetTraceSequence_;
        resetTrace_.engineFrame = presentEngineFrame_;
        resetTrace_.sdkPresentId = presentTiming_.sdkId;
        resetTrace_.timingContext = presentTiming_.contextGeneration;
        resetTrace_.copyToken = frame().pendingCopyTiming_.sequence;
        resetTrace_.copyGeneration = frame().pendingCopyTiming_.generation;
        resetTrace_.pendingBefore = followupPending_;
        if (followupPending_) resetTrace_.triggerBefore = followupTrigger_;
        if (outputEntry) {
            LARGE_INTEGER now{}, frequency{};
            resetTrace_.outputEntryKnown = QueryPerformanceCounter(&now) && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0;
            if (resetTrace_.outputEntryKnown) {
                resetTrace_.outputEntryQpcTicks = now.QuadPart;
                resetTrace_.qpcFrequency = frequency.QuadPart;
            }
        }
    }
    void finishResetTrace() noexcept {
        if (!resetTrace_.requested) return;
        if (followupClearReason_) {
            resetTrace_.clearReason = followupClearReason_;
            if (!resetTrace_.pendingBefore) {
                resetTrace_.pendingBefore = true;
                resetTrace_.triggerBefore = clearedFollowupTrigger_;
            }
            followupClearReason_ = nullptr;
        }
        resetTrace_.pendingAfter = followupPending_;
        if (followupPending_) resetTrace_.triggerAfter = followupTrigger_;
        const bool trigger = resetTrace_.longFrame || resetTrace_.extraRequested || resetTrace_.clearReason;
        if (trigger) { resetTrace_.emit = true; resetTraceTail_ = 4; }
        else if (resetTraceTail_) { resetTrace_.emit = true; --resetTraceTail_; }
    }

    CpuWallTime copyFenceWaitCpu_, copyConsumedWaitCpu_;
    CopyFenceTiming previousCopyTiming_, immediateCopyTiming_;
    void disableTimestamps(FrameContext* selected = nullptr) noexcept {
        auto& copy = selected ? *selected : frame();
        copy.timestampActive_ = copy.timestampRecorded_ = copy.timestampPendingValid_ = false;
        copy.timestampFailed_ = true;
        copy.timestampLatest_.requested = record_.gpuTimestamps;
        copy.timestampLatest_.queryStateKnown = record_.gpuTimestamps;
        copy.timestampLatest_.diagnosticActive = false;
        copy.timestampLatest_.available = copy.timestampLatest_.durationKnown = false;
    }
    void calibrateTimestampClock(FrameContext* selected = nullptr) noexcept {
        auto& copy = selected ? *selected : frame();
        if (!copy.calibrateTimestamps_) return;
        try {
            VkCalibratedTimestampInfoKHR info[2]{{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR},
                                                {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}};
            info[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
            info[1].timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
            std::uint64_t values[2]{}, deviation{}; LARGE_INTEGER captured{};
            const auto result = copy.calibrateTimestamps_(record_.device, 2, info, values, &deviation);
            QueryPerformanceCounter(&captured);
            if (result == VK_SUCCESS) copy.timestampCalibration_ = {true, true, values[0], values[1],
                copy.timestampQpcFrequency_, static_cast<std::uint64_t>(captured.QuadPart), deviation};
            else copy.calibrateTimestamps_ = nullptr;
        } catch (...) { copy.calibrateTimestamps_ = nullptr; }
    }
    void initializeTimestamps() noexcept {
        if (!record_.gpuTimestamps || frame().timestampFailed_ || frame().timestampPool_) return;
        frame().timestampLatest_.requested = true;
        frame().timestampLatest_.queryStateKnown = true;
        try {
            const auto properties = record_.ivk<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
            const auto families = record_.ivk<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
            VkPhysicalDeviceProperties props{}; properties(record_.physical, &props);
            std::array<VkQueueFamilyProperties, 64> queues{}; std::uint32_t count{};
            families(record_.physical, &count, nullptr);
            if (!count || count > queues.size() || record_.queueInfo.queueFamilyIndex >= count) { disableTimestamps(); return; }
            families(record_.physical, &count, queues.data());
            if (count > queues.size() || record_.queueInfo.queueFamilyIndex >= count) { disableTimestamps(); return; }
            frame().timestampValidBits_ = queues[record_.queueInfo.queueFamilyIndex].timestampValidBits;
            frame().timestampPeriodNs_ = props.limits.timestampPeriod;
            if (!frame().timestampValidBits_ || frame().timestampValidBits_ > 64 || !(frame().timestampPeriodNs_ > 0) || !std::isfinite(frame().timestampPeriodNs_)) { disableTimestamps(); return; }
            const auto create = vk<PFN_vkCreateQueryPool>("vkCreateQueryPool");
            frame().destroyTimestampPool_ = vk<PFN_vkDestroyQueryPool>("vkDestroyQueryPool");
            frame().resetTimestamps_ = vk<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool");
            frame().writeTimestamp_ = vk<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp");
            frame().readTimestamps_ = vk<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults");
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; info.queryType = VK_QUERY_TYPE_TIMESTAMP; info.queryCount = 2;
            if (create(record_.device, &info, nullptr, &frame().timestampPool_) != VK_SUCCESS) { frame().timestampPool_ = {}; disableTimestamps(); return; }
            LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); frame().timestampQpcFrequency_ = frequency.QuadPart;
            static std::atomic<std::uint64_t> contextSerial{}; if (!timestampContext_) timestampContext_ = ++contextSerial;
            frame().timestampActive_ = true;
            frame().timestampLatest_.diagnosticActive = true;
            // Calibration is used only when the caller actually enabled its extension.
            const char* suffix{};
            for (const auto& extension : record_.extensionStorage) {
                if (extension == "VK_KHR_calibrated_timestamps") { suffix = "KHR"; break; }
                if (extension == "VK_EXT_calibrated_timestamps") suffix = "EXT";
            }
            if (suffix) {
                const auto domains = record_.ivk<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
                    suffix[0] == 'K' ? "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR" : "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
                std::array<VkTimeDomainKHR, 16> supported{}; std::uint32_t total{}; bool device{}, qpc{};
                if (domains(record_.physical, &total, nullptr) == VK_SUCCESS && total <= supported.size() &&
                    domains(record_.physical, &total, supported.data()) == VK_SUCCESS && total <= supported.size()) {
                    for (std::uint32_t i = 0; i < total; ++i) { device |= supported[i] == VK_TIME_DOMAIN_DEVICE_KHR; qpc |= supported[i] == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR; }
                    if (device && qpc) frame().calibrateTimestamps_ = vk<PFN_vkGetCalibratedTimestampsKHR>(suffix[0] == 'K' ? "vkGetCalibratedTimestampsKHR" : "vkGetCalibratedTimestampsEXT");
                }
                calibrateTimestampClock();
            }
        } catch (...) {
            // Optional calibration lookup must not disable a working raw query pool.
            if (!frame().timestampPool_) disableTimestamps();
        }
    }
    void beginCopyTimestamps(std::uint64_t generation) noexcept {
        frame().timestampRecorded_ = false;
        if (!frame().timestampActive_) return;
        try {
            frame().resetTimestamps_(frame().command_, frame().timestampPool_, 0, 2);
            frame().writeTimestamp_(frame().command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame().timestampPool_, 0);
            frame().timestampRecording_ = {}; frame().timestampRecording_.requested = true;
            frame().timestampRecording_.queryStateKnown = frame().timestampRecording_.diagnosticActive = true;
            frame().timestampRecording_.sequence = ++timestampSerial_; frame().timestampRecording_.contextGeneration = timestampContext_;
            frame().timestampRecording_.generation = generation; frame().timestampRecording_.sourceEngineFrame = presentEngineFrame_;
            frame().timestampRecording_.sourceEngineFrameKnown = presentTiming_ || presentEngineFrame_ != 0;
            frame().timestampRecording_.sdkPresentId = presentTiming_.sdkId;
            frame().timestampRecording_.sdkPresentIdKnown = bool(presentTiming_);
            frame().timestampRecording_.validBits = frame().timestampValidBits_; frame().timestampRecording_.timestampPeriodNs = frame().timestampPeriodNs_;
            frame().timestampRecording_.qpcFrequency = frame().timestampQpcFrequency_; frame().timestampRecorded_ = true;
        } catch (...) { disableTimestamps(); }
    }
    void endCopyTimestamps() noexcept {
        if (frame().timestampRecorded_) try { frame().writeTimestamp_(frame().command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame().timestampPool_, 1); }
        catch (...) { disableTimestamps(); }
    }
    bool readCompletedTimestamps(FrameContext* selected = nullptr) noexcept {
        auto& copy = selected ? *selected : frame();
        if (!copy.timestampActive_ || !copy.timestampPendingValid_) return false;
        LARGE_INTEGER observed{}; QueryPerformanceCounter(&observed);
        copy.timestampPending_.fenceObservedQpcTicks = observed.QuadPart;
        copy.timestampPendingValid_ = false;
        // Adopt this submission before reading: even an exception must never expose an older success.
        copy.timestampLatest_ = copy.timestampPending_;
        copy.timestampLatest_.queryReadAttempted = true;
        try {
            struct Query { std::uint64_t ticks{}, available{}; } results[2];
            const auto result = copy.readTimestamps_(record_.device, copy.timestampPool_, 0, 2, sizeof(results), results,
                sizeof(Query), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            copy.timestampLatest_.queryResultKnown = true;
            copy.timestampLatest_.queryResult = result;
            copy.timestampLatest_.queryAvailabilityKnown = result == VK_SUCCESS || result == VK_NOT_READY;
            if (copy.timestampLatest_.queryAvailabilityKnown) {
                copy.timestampLatest_.beginAvailability = results[0].available;
                copy.timestampLatest_.endAvailability = results[1].available;
            }
            if (result != VK_SUCCESS || !results[0].available || !results[1].available) { disableTimestamps(&copy); return true; }
            copy.timestampLatest_.beginTicks = results[0].ticks; copy.timestampLatest_.endTicks = results[1].ticks;
            copy.timestampLatest_.available = true;
            copy.timestampLatest_.calibration = copy.timestampCalibration_;
            timestampDuration(copy.timestampLatest_);
            if(copy.timestampLatest_.durationKnown&&copy.timestampLatest_.sdkPresentIdKnown)measurement::observe(measurement::Metric::NativeGpu,
                static_cast<std::uint64_t>(copy.timestampLatest_.durationNs),copy.timestampLatest_.sdkPresentId,copy.timestampLatest_.contextGeneration);
            if (copy.timestampLatest_.sequence % 300 == 0) calibrateTimestampClock(&copy); // Refresh the next sample's reference.
        } catch (...) { disableTimestamps(&copy); }
        return true;
    }
    VkResult submitQueue(const VkSubmitInfo &info, VkFence fence, const char* operation,
                         wsi_acquire::QueueCall* out = nullptr) {
        wsi_acquire::QueueCall timing;
        const bool observed = record_.queueObserver || out;
        const auto entryError = observed ? GetLastError() : 0;
        if (observed) timing.entryNs = cpuWallNowNs();
        VkResult result; DWORD originalError;
        {
            std::unique_lock<QueueGate> gate;
            if (record_.queueGate)
                gate = std::unique_lock<QueueGate>(*record_.queueGate);
            if (observed) timing.driverEntryNs = cpuWallNowNs();
            if (observed) SetLastError(entryError);
            result = vk<PFN_vkQueueSubmit>("vkQueueSubmit")(queue_, 1, &info, fence);
            originalError = observed ? GetLastError() : 0;
            if (observed) timing.returnNs = cpuWallNowNs();
        }
        timing.result = result;
        if (out) *out = timing;
        if (record_.queueObserver && timing.slow())
            record_.queueObserver(record_.queueObserverContext, operation,
                                  reinterpret_cast<std::uintptr_t>(queue_), timing);
        if (observed) SetLastError(originalError);
        return result;
    }
    void copyWaitBegin() {
        if (record_.fake) frame().pendingCopyTiming_.waitEntryNs = cpuWallNowNs();
    }
    void copyWaitEnd(VkResult result, bool previous = true) {
        if (record_.fake) {
            frame().pendingCopyTiming_.waitReturnNs = cpuWallNowNs();
            frame().pendingCopyTiming_.waitResult = result;
            frame().pendingCopyTiming_.privateCopyGpu = {};
            frame().pendingCopyTiming_.privateCopyGpu.requested = record_.gpuTimestamps;
            frame().pendingCopyTiming_.privateCopyGpu.queryStateKnown = record_.gpuTimestamps;
            frame().pendingCopyTiming_.privateCopyGpu.diagnosticActive = frame().timestampActive_;
            frame().pendingCopyTiming_.privateCopyQueryMatch = false;
            if (result == VK_SUCCESS && readCompletedTimestamps() && frame().timestampLatest_.sourceCopyTokenKnown &&
                frame().timestampLatest_.sourceCopyToken == frame().pendingCopyTiming_.sequence &&
                frame().timestampLatest_.generation == frame().pendingCopyTiming_.generation) {
                frame().pendingCopyTiming_.privateCopyGpu = frame().timestampLatest_;
                frame().pendingCopyTiming_.privateCopyQueryMatch = true;
            }
            (previous ? previousCopyTiming_ : immediateCopyTiming_) = frame().pendingCopyTiming_;
        }
    }
    void sourceCopyCompleted(std::uint64_t covered = UINT64_MAX) {
        if (record_.fake) {
            std::lock_guard lock(sourceCompletionMutex_);
            completedSourceCopy_ = std::max(completedSourceCopy_,
                                            covered == UINT64_MAX ? submittedSourceCopy_ : covered);
        }
    }
    VkResult resetCopyFence() {
        if (!record_.fake || frame().slotOwned_)
            return vk<PFN_vkResetFences>("vkResetFences")(record_.device, 1, &frame().fence_);
        std::lock_guard lock(sourceCompletionMutex_);
        if (completedSourceCopy_ < submittedSourceCopy_) {
            const auto status =
                vk<PFN_vkGetFenceStatus>("vkGetFenceStatus")(record_.device, frame().fence_);
            if (status != VK_SUCCESS)
                return status;
            completedSourceCopy_ = submittedSourceCopy_;
        }
        return vk<PFN_vkResetFences>("vkResetFences")(record_.device, 1, &frame().fence_);
    }
    void sourceCopySubmitted(const wsi_acquire::QueueCall& submit, VkImage source = VK_NULL_HANDLE,
                             std::uint64_t generation = 0) {
        if (record_.fake) {
            std::lock_guard lock(sourceCompletionMutex_);
            if(submittedSourceCopy_==UINT64_MAX) throw std::overflow_error("Private copy token exhausted");
            ++submittedSourceCopy_;
            frame().pendingCopyTiming_ = {submittedSourceCopy_, generation, reinterpret_cast<std::uintptr_t>(source),
                                  reinterpret_cast<std::uintptr_t>(frame().fence_), reinterpret_cast<std::uintptr_t>(queue_),
                                  presentEngineFrame_, submit};
            if(source&&generation) {
                std::lock_guard pools(fakePoolsMutex_);
                const auto found=fakePools_.find(generation);
                if(found==fakePools_.end())throw std::logic_error("Submitted private copy lost its image generation");
                const auto image=std::find_if(found->second.images.begin(),found->second.images.end(),[&](const auto& item){return item.image==source;});
                if(image==found->second.images.end())throw std::logic_error("Submitted private copy source is outside its image generation");
                image->lastCopyToken=submittedSourceCopy_;
            }
        }
    }
    template <class T> T vk(const char *n) const {
        return record_.vk<T>(n);
    }
    VkRect2D outputRectFor(const continuous::Bundle& bundle) const {
        const auto& region = bundle.constants.outputRegion;
        if (!region.observed || !region.valid || region.rect[0] < 0 || region.rect[1] < 0 ||
            region.rect[2] <= 0 || region.rect[3] <= 0 ||
            static_cast<std::uint32_t>(region.rect[0]) >= extent_.width ||
            static_cast<std::uint32_t>(region.rect[1]) >= extent_.height ||
            static_cast<std::uint32_t>(region.rect[2]) > extent_.width - static_cast<std::uint32_t>(region.rect[0]) ||
            static_cast<std::uint32_t>(region.rect[3]) > extent_.height - static_cast<std::uint32_t>(region.rect[1]))
            return {};
        return {{region.rect[0], region.rect[1]},
                {static_cast<std::uint32_t>(region.rect[2]), static_cast<std::uint32_t>(region.rect[3])}};
    }
    bool nativeSuppressedFor(const continuous::Bundle& bundle,
                             const fsr_menu_control::ControlIntent& intent) const {
        if (!intent.localFiOffRequested) return intent.nativeSuppressed;
        const auto current = fsr_menu_control::snapshot();
        const auto& region = bundle.constants.outputRegion;
        return current.localFiOffRequested && current.localFiOffActive && bundle.ready &&
            region.observed && region.valid && region.localFiOffVerified;
    }
    const char* onRefusalFor(const continuous::Bundle& bundle, const fsr_menu_control::ControlIntent& intent,
                             VkRect2D outputRect) const {
        if (!intent.requestedEnabled) return nullptr;
        if (!fsr_menu_control::gameplayAllowsInterpolation(intent)) return "not_in_gameplay";
        if (!gameOnAdmitted) return "game_on_not_admitted";
        RECT client{};
        if (IsIconic(surface_.original()) || !GetClientRect(surface_.original(), &client) ||
            client.right != static_cast<LONG>(extent_.width) || client.bottom != static_cast<LONG>(extent_.height))
            return "window_extent_transition";
        if (!intent.bothGatesReady) return "menu_gates_not_ready";
        if (!intent.inputObserved) return "menu_input_not_observed";
        if (!intent.paired) return "menu_gates_unpaired";
        if (!intent.gatesConsistent) return "menu_gates_inconsistent";
        if (!nativeSuppressedFor(bundle, intent)) return "native_fsr_not_suppressed";
        if (!presentTiming_) return "present_ticket_missing";
        if (!presentTiming_.latencyEnabled) return "timing_warmup_off";
        if (!bundle.ready) return "frame_inputs_missing";
        if (bundle.timing != presentTiming_ || bundle.engineFrame != presentEngineFrame_)
            return "frame_inputs_not_current";
        const auto& constants = bundle.constants;
        constexpr std::uint32_t required = frame_input::Extents | frame_input::Jitter |
            frame_input::MotionScale | frame_input::View | frame_input::Projection |
            frame_input::CameraPlanes | frame_input::Reset;
        if ((constants.validityMask & required) != required) return "frame_constants_incomplete";
        if (!frame_input::xefgJitterInRange(constants)) return "jitter_out_of_sdk_range";
        if (constants.motionVectorScale[0] == 0 || constants.motionVectorScale[1] == 0)
            return "motion_scale_zero";
        const auto& region = constants.outputRegion;
        const bool regionInBounds = outputRect.extent.width != 0;
        if ((region.observed || region.valid) && !regionInBounds) return "frame_output_region_invalid";
        const auto input = bundle.depth.successfulImageCreateInfo.extent;
        const auto motion = bundle.motion.successfulImageCreateInfo.extent;
        const auto target = regionInBounds ? outputRect.extent : extent_;
        if (!input.width || !input.height || input.width > target.width || input.height > target.height ||
            motion.width != input.width || motion.height != input.height ||
            constants.renderSize[0] != static_cast<std::int32_t>(input.width) ||
            constants.renderSize[1] != static_cast<std::int32_t>(input.height) ||
            constants.upscaleSize[0] <= 0 || constants.upscaleSize[1] <= 0 ||
            (!regionInBounds && (constants.upscaleSize[0] != static_cast<std::int32_t>(extent_.width) ||
                                 constants.upscaleSize[1] != static_cast<std::int32_t>(extent_.height))))
            return "frame_extent_mismatch";
        return nullptr;
    }
    void createStable(VkExtent2D size, VkImage& image, VkDeviceMemory& allocation) {
        try {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = VK_FORMAT_R8G8B8A8_UNORM;
            info.extent = {size.width, size.height, 1};
            info.mipLevels = info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vk<PFN_vkCreateImage>("vkCreateImage")(record_.device, &info, nullptr, &image),
                  "Create stable color");
            VkMemoryRequirements requirements{};
            vk<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(record_.device, image, &requirements);
            VkPhysicalDeviceMemoryProperties memory{};
            record_.ivk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
                record_.physical, &memory);
            std::uint32_t type = UINT32_MAX;
            for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
                if ((requirements.memoryTypeBits & (1u << i)) &&
                    (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    type = i;
                    break;
                }
            }
            if (type == UINT32_MAX) throw std::runtime_error("No stable device-local allocation");
            VkMemoryAllocateInfo memoryInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            memoryInfo.allocationSize = requirements.size;
            memoryInfo.memoryTypeIndex = type;
            check(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(record_.device, &memoryInfo, nullptr, &allocation),
                  "Allocate stable color");
            check(vk<PFN_vkBindImageMemory>("vkBindImageMemory")(record_.device, image, allocation, 0),
                  "Bind stable color");
        } catch (...) {
            if (image) vk<PFN_vkDestroyImage>("vkDestroyImage")(record_.device, image, nullptr);
            if (allocation) vk<PFN_vkFreeMemory>("vkFreeMemory")(record_.device, allocation, nullptr);
            image = VK_NULL_HANDLE;
            allocation = VK_NULL_HANDLE;
            throw;
        }
    }
    void applyPendingOutput(VkExtent2D colorExtent) {
        const bool physicalResized = extent_.width != wantedExtent_.width || extent_.height != wantedExtent_.height;
        const bool resized = colorExtent_.width != colorExtent.width || colorExtent_.height != colorExtent.height;
        if (physicalResized || resized || format_ != wantedFormat_ || presentMode_ != wantedPresentMode_) {
            if(hasAsyncFrames()&&retireAsyncFrames(true)!=VK_SUCCESS)
                throw std::runtime_error("In-flight frames did not retire before output transition");
            clearFollowup("output_transition");
        }
        if (resized) {
            VkImage replacement{};
            VkDeviceMemory replacementMemory{};
            createStable(colorExtent, replacement, replacementMemory);
            try {
                // This runs only at the application's serialized queue-present
                // boundary, never during vkCreateSwapchainKHR. The previous
                // private copy completed before its present returned; the SDK
                // owner additionally waits its real tagging/bridge consumers.
                if (presenter_) presenter_->resizeOutput(colorExtent.width, colorExtent.height);
                else bridge_->resizeOutput(colorExtent.width, colorExtent.height);
            } catch (...) {
                vk<PFN_vkDestroyImage>("vkDestroyImage")(record_.device, replacement, nullptr);
                vk<PFN_vkFreeMemory>("vkFreeMemory")(record_.device, replacementMemory, nullptr);
                throw;
            }
            vk<PFN_vkDestroyImage>("vkDestroyImage")(record_.device, stable_, nullptr);
            vk<PFN_vkFreeMemory>("vkFreeMemory")(record_.device, memory_, nullptr);
            stable_ = replacement;
            memory_ = replacementMemory;
            initialized_ = false;
            historyGap_ = true;
            previousEnabled_ = false;
            colorExtent_ = colorExtent;
        }
        if (physicalResized) { historyGap_ = true; previousEnabled_ = false; }
        extent_ = wantedExtent_;
        if (presenter_ && presentMode_ != wantedPresentMode_)
            presenter_->setVsync(wantedPresentMode_ == VK_PRESENT_MODE_FIFO_KHR);
        presentMode_ = wantedPresentMode_;
        if (format_ != wantedFormat_) historyGap_ = true;
        format_ = wantedFormat_;
    }
    void release() {
        if(provider_&&sharedCopy_&&!provider_->setSharedInputs({}))
            throw std::runtime_error("Shared input recordings still retain the bridge owner");
        sharedInputAccess_={};
        {
            std::lock_guard lock(presenterMutex_);
            presenter_.reset();
        }
        lastSdkResultId_=0;
        bridge_.reset();
        for(auto& copy:frames_) {
            if(copy.timestampPool_) try { copy.destroyTimestampPool_(record_.device,copy.timestampPool_,nullptr); } catch (...) {}
            if(copy.fence_) vk<PFN_vkDestroyFence>("vkDestroyFence")(record_.device,copy.fence_,nullptr);
            copy={};
        }
        if (stable_)
            vk<PFN_vkDestroyImage>("vkDestroyImage")(record_.device, stable_, nullptr);
        if (memory_)
            vk<PFN_vkFreeMemory>("vkFreeMemory")(record_.device, memory_, nullptr);
        if (pool_)
            vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(record_.device, pool_, nullptr);
        if (copySemaphore_)
            vk<PFN_vkDestroySemaphore>("vkDestroySemaphore")(record_.device, copySemaphore_, nullptr);
        // Per-image WSI waits outlive private cleanup until retireChain proves
        // their native tail retired; the layer retains their Vulkan parent.
        stable_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE;
        frame().fence_ = VK_NULL_HANDLE;
        copySemaphore_ = VK_NULL_HANDLE;
        initialized_ = false;
    }

  public:
    NativeBackend(DeviceRecord::Impl &r, Surface &s, VkQueue q, continuous::Provider *provider)
        : record_(r), surface_(s), queue_(q), provider_(provider) {
#ifdef XESS_PRESENTATION_NATIVE_TEST
        asyncCopy_=record_.fake&&testing::asyncCopyPath();
        if(asyncCopy_)asyncFrameLimit_=testing::asyncCopyFrames()==1 ? 1 : frameSlotCount;
        sharedCopy_=record_.fake&&(asyncCopy_||testing::sharedCopySync())&&testing::sharedCopyPath();
#else
        asyncCopy_=record_.fake&&diagnostic_flags::asyncCopyPath();
        if(asyncCopy_)asyncFrameLimit_=diagnostic_flags::asyncCopyFrames();
        sharedCopy_=record_.fake&&diagnostic_flags::sharedCopyPath();
#endif
        wchar_t lowRes[2]{};
        lowResRequested_ = GetEnvironmentVariableW(L"XESS_XEFG_LOW_RES_OUTPUT",lowRes,2)==1 && lowRes[0]==L'1';
        wchar_t path[32768]{};
        const auto count = GetEnvironmentVariableW(L"XESS_SDK_BIN", path, 32768);
        if (!count || count >= 32768 || !std::filesystem::path(path).is_absolute())
            throw std::invalid_argument("XESS_SDK_BIN must be an absolute SDK directory");
        sdk_ = path;
    }
    ~NativeBackend() override {
        if(provider_&&sharedCopy_&&!provider_->setSharedInputs({})) {
            // Live recordings retain the concrete callback target as well as its ring.
            bridge_.release();return;
        }
        if (!hasAsyncFrames() && std::none_of(frames_.begin(),frames_.end(),[](const auto& f){return f.pending_;}) && !unsafe_)
            release();
    }
    VkResult createFakeImages(const VkSwapchainCreateInfoKHR &source, std::uint64_t generation,
                              std::vector<VkImage> &output) override {
        if (!record_.fake)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        {
            std::lock_guard lock(fakePoolsMutex_);
            if (fakePools_.contains(generation))
                return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        FakePool pool;
        const auto cleanup = [&] {
            for (const auto &image : pool.images) {
                if (image.image)
                    vk<PFN_vkDestroyImage>("vkDestroyImage")(record_.device, image.image, nullptr);
                if (image.memory)
                    vk<PFN_vkFreeMemory>("vkFreeMemory")(record_.device, image.memory, nullptr);
            }
        };
        try {
            pool.images.resize(std::max<std::uint32_t>(3, source.minImageCount));
            output.reserve(pool.images.size());
            VkPhysicalDeviceMemoryProperties memory;
            record_.ivk<PFN_vkGetPhysicalDeviceMemoryProperties>(
                "vkGetPhysicalDeviceMemoryProperties")(record_.physical, &memory);
            for (auto &image : pool.images) {
                VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                info.imageType = VK_IMAGE_TYPE_2D;
                info.format = source.imageFormat;
                info.extent = {source.imageExtent.width, source.imageExtent.height, 1};
                info.mipLevels = info.arrayLayers = 1;
                info.samples = VK_SAMPLE_COUNT_1_BIT;
                info.tiling = VK_IMAGE_TILING_OPTIMAL;
                info.usage = source.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                auto result = vk<PFN_vkCreateImage>("vkCreateImage")(record_.device, &info, nullptr,
                                                                     &image.image);
                if (result != VK_SUCCESS) {
                    cleanup();
                    return result;
                }
                VkMemoryRequirements requirements;
                vk<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
                    record_.device, image.image, &requirements);
                std::uint32_t type = UINT32_MAX;
                for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
                    if ((requirements.memoryTypeBits & (1u << i)) &&
                        (memory.memoryTypes[i].propertyFlags &
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                        type = i;
                        break;
                    }
                if (type == UINT32_MAX) {
                    cleanup();
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                }
                VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocation.allocationSize = requirements.size;
                allocation.memoryTypeIndex = type;
                result = vk<PFN_vkAllocateMemory>("vkAllocateMemory")(record_.device, &allocation,
                                                                      nullptr, &image.memory);
                if (result != VK_SUCCESS) {
                    cleanup();
                    return result;
                }
                result = vk<PFN_vkBindImageMemory>("vkBindImageMemory")(record_.device, image.image,
                                                                        image.memory, 0);
                if (result != VK_SUCCESS) {
                    cleanup();
                    return result;
                }
                output.push_back(image.image);
            }
            {
                std::lock_guard lock(fakePoolsMutex_);
                fakePools_.emplace(generation, std::move(pool));
            }
            return VK_SUCCESS;
        } catch (const std::bad_alloc &) {
            cleanup();
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        } catch (...) {
            cleanup();
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    VkResult destroyFakeImages(std::uint64_t generation) override {
        FakePool *selected;
        {
            std::lock_guard lock(fakePoolsMutex_);
            const auto found = fakePools_.find(generation);
            if (found == fakePools_.end())
                return VK_SUCCESS;
            selected = &found->second;
        }
        auto &pool = *selected;
        if (!pool.retirementFence) {
            VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            const auto result = vk<PFN_vkCreateFence>("vkCreateFence")(
                record_.device, &create, nullptr, &pool.retirementFence);
            if (result != VK_SUCCESS)
                return result;
        }
        if (!pool.tailSubmitted) {
            pool.coveredCopyToken = copyCompletionToken();
            VkSubmitInfo tail{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            const auto submitted = submitQueue(tail, pool.retirementFence, "fake_pool_retire_submit");
            if (submitted != VK_SUCCESS) {
                if (submitted == VK_ERROR_OUT_OF_HOST_MEMORY ||
                    submitted == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                    vk<PFN_vkDestroyFence>("vkDestroyFence")(record_.device, pool.retirementFence,
                                                             nullptr);
                    pool.retirementFence = VK_NULL_HANDLE;
                } else {
                    pool.tailSubmitted = true;
                    unsafe_ = true;
                    shutdown_.failedQuarantined = true;
                }
                return submitted;
            }
            pool.tailSubmitted = true;
        }
        const auto completed = vk<PFN_vkWaitForFences>("vkWaitForFences")(
            record_.device, 1, &pool.retirementFence, VK_TRUE, 10'000'000'000ull);
        if (completed != VK_SUCCESS)
            return completed;
        sourceCopyCompleted(pool.coveredCopyToken);
        for (const auto &image : pool.images) {
            vk<PFN_vkDestroyImage>("vkDestroyImage")(record_.device, image.image, nullptr);
            vk<PFN_vkFreeMemory>("vkFreeMemory")(record_.device, image.memory, nullptr);
        }
        vk<PFN_vkDestroyFence>("vkDestroyFence")(record_.device, pool.retirementFence, nullptr);
        {
            std::lock_guard lock(fakePoolsMutex_);
            fakePools_.erase(generation);
        }
        return VK_SUCCESS;
    }
    VkResult signalAcquire(VkSemaphore semaphore, VkFence fence, wsi_acquire::Call* timing = nullptr) override {
        VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        signal.signalSemaphoreCount = semaphore ? 1 : 0;
        signal.pSignalSemaphores = semaphore ? &semaphore : nullptr;
        const auto result = submitQueue(signal, fence, "fake_acquire_signal_submit", timing ? &timing->signal : nullptr);
        return result;
    }
    bool asyncAcquireEnabled() const override { return asyncCopy_; }
    VkResult signalAcquireAfterCopy(VkImage image,std::uint64_t generation,std::uint64_t token,
                                    VkSemaphore semaphore,VkFence fence,wsi_acquire::Call* timing = nullptr) override {
#ifdef XESS_PRESENTATION_NATIVE_TEST
        struct ProbeOnExit {
            testing::AcquireProbe value;
            ~ProbeOnExit() {
                if(value.reason) { value.returnNs=cpuWallNowNs();testing::acquireProbe(value); }
            }
            void reject(const char* reason,VkResult result) { value.reason=reason;value.result=result;value.resultKnown=true; }
        } probe;
        probe.value.image=image;probe.value.generation=generation;probe.value.token=token;
        probe.value.async=asyncCopy_;probe.value.unsafe=unsafe_;probe.value.stopped=timingStopped_.load();probe.value.entryNs=cpuWallNowNs();
        const auto snapshot=[&] {
            probe.value.submitted=submittedSourceCopy_;
            const auto pool=fakePools_.find(generation);probe.value.poolFound=pool!=fakePools_.end();
            if(!probe.value.poolFound)return;
            const auto found=std::find_if(pool->second.images.begin(),pool->second.images.end(),[&](const auto& item){return item.image==image;});
            probe.value.imageFound=found!=pool->second.images.end();
            if(probe.value.imageFound) { probe.value.lastCopy=found->lastCopyToken;probe.value.lastAcquire=found->lastAcquireToken; }
        };
        const char* entryRefusal=!probe.value.async?"async_disabled":!image?"null_image":!generation?"zero_generation":!token?"zero_token":
            probe.value.unsafe?"unsafe_backend":probe.value.stopped?"timing_stopped":nullptr;
        if(entryRefusal) {
            {std::lock_guard source(sourceCompletionMutex_);std::lock_guard pools(fakePoolsMutex_);snapshot();}
            probe.reject(entryRefusal,VK_ERROR_UNKNOWN);return VK_ERROR_UNKNOWN;
        }
#else
        if(!asyncCopy_||!image||!generation||!token||unsafe_||timingStopped_.load())return VK_ERROR_UNKNOWN;
#endif
        std::lock_guard source(sourceCompletionMutex_);
        std::lock_guard pools(fakePoolsMutex_);
#ifdef XESS_PRESENTATION_NATIVE_TEST
        snapshot();
#endif
        const auto found=fakePools_.find(generation);
        if(found==fakePools_.end()||token>submittedSourceCopy_) {
#ifdef XESS_PRESENTATION_NATIVE_TEST
            probe.reject(found==fakePools_.end()?"pool_missing":"token_not_submitted",VK_ERROR_UNKNOWN);
#endif
            return VK_ERROR_UNKNOWN;
        }
        const auto binding=std::find_if(found->second.images.begin(),found->second.images.end(),[&](const auto& item){return item.image==image;});
        if(binding==found->second.images.end()||binding->lastCopyToken!=token||binding->lastAcquireToken>=token) {
#ifdef XESS_PRESENTATION_NATIVE_TEST
            probe.reject(binding==found->second.images.end()?"image_missing":binding->lastCopyToken!=token?"token_mismatch":"already_acquired",VK_ERROR_UNKNOWN);
#endif
            return VK_ERROR_UNKNOWN;
        }
        // This same queue already contains the exact successful private read.
        // The acquire signal follows it without observing GPU completion on CPU.
#ifdef XESS_PRESENTATION_NATIVE_TEST
        probe.value.reason="signal_submit_exception";probe.value.signalAttempted=true;
#endif
        const auto result=signalAcquire(semaphore,fence,timing);
        if(result==VK_SUCCESS)binding->lastAcquireToken=token;
#ifdef XESS_PRESENTATION_NATIVE_TEST
        if(result==VK_SUCCESS)probe.value.reason=nullptr;
        else probe.reject(result==VK_ERROR_OUT_OF_HOST_MEMORY||result==VK_ERROR_OUT_OF_DEVICE_MEMORY?"signal_submit_oom":"signal_submit_failed",result);
#endif
        return result;
    }
    VkResult windowStatus(VkExtent2D extent) const override {
        RECT client{};
        if (!IsWindow(surface_.original()) || !GetClientRect(surface_.original(), &client))
            return VK_ERROR_SURFACE_LOST_KHR;
        return IsIconic(surface_.original()) || client.right != static_cast<LONG>(extent.width) ||
                       client.bottom != static_cast<LONG>(extent.height)
                   ? VK_ERROR_OUT_OF_DATE_KHR
                   : VK_SUCCESS;
    }
    std::uint64_t copyCompletionToken() const override {
        std::lock_guard lock(sourceCompletionMutex_);
        return submittedSourceCopy_;
    }
    VkResult pollCopyCompletion(std::uint64_t token) override {
        std::lock_guard lock(sourceCompletionMutex_);
        if (!token || token > submittedSourceCopy_)
            return VK_ERROR_UNKNOWN;
        if (token <= completedSourceCopy_)
            return VK_SUCCESS;
        for(const auto& copy:frames_) if(copy.pendingCopyTiming_.sequence>=token&&copy.fence_) {
            const auto result=vk<PFN_vkGetFenceStatus>("vkGetFenceStatus")(record_.device,copy.fence_);
            if(result==VK_SUCCESS) { completedSourceCopy_=std::max(completedSourceCopy_,copy.pendingCopyTiming_.sequence); return VK_SUCCESS; }
            if(result!=VK_NOT_READY)return result;
        }
        return VK_NOT_READY;
    }
    VkResult consumePresentWaits(const VkPresentInfoKHR &info) override {
        return consumePresentWaits(info,VK_NULL_HANDLE,0);
    }
    VkResult consumePresentWaits(const VkPresentInfoKHR &info,VkImage source,std::uint64_t generation) override {
        if(hasAsyncFrames()) { const auto result=retireAsyncFrames(true); if(result!=VK_SUCCESS)return result; }
        frameIndex_=0;
        beginResetTrace(false);
        clearFollowup("window_unavailable");
        finishResetTrace();
        frame().copyProgress_ = CopyProgress::NotSubmitted;
        const auto reset = resetCopyFence();
        if (reset != VK_SUCCESS)
            return reset;
        std::vector<VkPipelineStageFlags> stages(info.waitSemaphoreCount,
                                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = info.waitSemaphoreCount;
        submit.pWaitSemaphores = info.pWaitSemaphores;
        submit.pWaitDstStageMask = stages.data();
        {
            std::lock_guard lock(copyDependencyMutex_);
            frame().pendingAppWaitCount_ = info.waitSemaphoreCount;
            std::copy_n(info.pWaitSemaphores, frame().pendingAppWaitCount_, frame().pendingAppWaits_.begin());
            frame().healthyQueuedWaits_ = false;
        }
        wsi_acquire::QueueCall timing;
        const auto result = submitQueue(submit, frame().fence_, "fake_wait_only_submit", record_.fake ? &timing : nullptr);
        if (result != VK_SUCCESS) {
            if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                std::lock_guard lock(copyDependencyMutex_);
                frame().pendingAppWaitCount_ = 0;
            } else {
                unsafe_ = true;
                frame().pending_ = true;
                frame().copyProgress_ = CopyProgress::SubmissionUncertain;
                shutdown_.failedQuarantined = true;
            }
            return result;
        }
        sourceCopySubmitted(timing,source,generation);
        frame().pending_ = true;
        frame().queuedOff_ = frame().queuedOn_ = false;
        frame().copyProgress_ = CopyProgress::SubmittedPending;
        {
            std::lock_guard lock(copyDependencyMutex_);
            frame().healthyQueuedWaits_ = true;
        }
        return VK_SUCCESS;
    }
    VkResult prepare(const VkSwapchainCreateInfoKHR &info) override {
        try {
            if (!surface_.resize(info.imageExtent))
                throw std::runtime_error("Hidden HWND extent resize failed");
            VkSurfaceCapabilitiesKHR caps{};
            check(record_.ivk<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
                      "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(record_.physical,
                                                                   surface_.handle, &caps),
                  "SurfaceCapabilities");
            if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
                caps.currentExtent.width != info.imageExtent.width ||
                caps.currentExtent.height != info.imageExtent.height)
                throw std::runtime_error("Hidden surface lacks requested extent / TRANSFER_SRC");
            VkBool32 supports{};
            check(record_.ivk<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
                      "vkGetPhysicalDeviceSurfaceSupportKHR")(record_.physical,
                                                              record_.queueInfo.queueFamilyIndex,
                                                              surface_.handle, &supports),
                  "SurfaceSupport");
            if (!supports)
                throw std::runtime_error(
                    "Recorded graphics queue cannot present to hidden surface");
            VkFormatProperties source{}, target{};
            auto props = record_.ivk<PFN_vkGetPhysicalDeviceFormatProperties>(
                "vkGetPhysicalDeviceFormatProperties");
            props(record_.physical, info.imageFormat, &source);
            props(record_.physical, VK_FORMAT_R8G8B8A8_UNORM, &target);
            constexpr auto linearSource = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if (!(source.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ||
                !(target.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ||
                (info.imageFormat != VK_FORMAT_R8G8B8A8_UNORM &&
                 (!(source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
                  !(target.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))))
                throw std::runtime_error("Swapchain copy/blit format features unavailable");
            linearBlitSupported_ = (source.optimalTilingFeatures & linearSource) == linearSource &&
                                   (target.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
            wantedExtent_ = info.imageExtent;
            wantedFormat_ = info.imageFormat;
            wantedPresentMode_ = info.presentMode;
            if (bridge_ && active_) {
                if(sharedCopy_&&provider_)provider_->setSharedInputs({});
                return VK_SUCCESS;
            }
            if (drain() != VK_SUCCESS) return VK_ERROR_UNKNOWN;
            release();
            shutdown_.privateCleanupCompleted = shutdown_.sdkReleased = shutdown_.bridgeReleased = false;
            shutdown_.failedQuarantined = false;
            presentTiming_ = {};
            presentEngineFrame_ = bootstrapId_ = 0;
            status_ = {};
            heldLowResCanvas_ = heldLowResSource_ = {};
            extent_ = info.imageExtent;
            colorExtent_ = extent_;
            format_ = info.imageFormat;
            presentMode_ = info.presentMode;
            createStable(extent_, stable_, memory_);
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = record_.queueInfo.queueFamilyIndex;
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            check(vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(record_.device, &pool, nullptr,
                                                                     &pool_),
                  "Create private copy pool");
            VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commands.commandPool = pool_;
            commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commands.commandBufferCount = 1;
            for(frameIndex_=0;frameIndex_<(asyncCopy_?frameSlotCount:1u);++frameIndex_) {
            check(vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
                      record_.device, &commands, &frame().command_),
                  "Allocate private copy command");
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            check(vk<PFN_vkCreateFence>("vkCreateFence")(record_.device, &fence, nullptr, &frame().fence_),
                  "Create private copy fence");
            initializeTimestamps();
            }
            frameIndex_=nextFrameIndex_=0;
            InteropConfig config{extent_.width, extent_.height, 10000};
            config.colorOnly = true;
            bridge_ = std::make_unique<InteropBridge>(config, record_.borrowed(queue_));
            if(sharedCopy_) {
                bridge_->prepareSharedPlanes();
                if(provider_) {
                    continuous::SharedInputAccess access;
                    access.context=bridge_.get();
                    access.reserve=[](void* context,VkExtent2D extent,SharedPlaneLease& lease) noexcept {
                        return static_cast<InteropBridge*>(context)->tryReserveSharedFrame(extent,true,lease);
                    };
                    access.submitted=[](void* context,SharedPlaneKey key,std::uint32_t mask) {
                        static_cast<InteropBridge*>(context)->sharedWriteSubmitted(key,mask);
                    };
                    access.cancel=[](void* context,SharedPlaneKey key) noexcept {
                        return static_cast<InteropBridge*>(context)->cancelRecordedSharedFrame(key);
                    };
                    access.abandon=[](void* context,SharedPlaneKey key,VkFence fence) noexcept {
                        return static_cast<InteropBridge*>(context)->abandonSubmittedSharedFrame(key,fence);
                    };
                    access.release=[](void* context,SharedPlaneKey key,const FrameCompletion& receipt) noexcept {
                        return static_cast<InteropBridge*>(context)->releaseSharedFrame(key,receipt);
                    };
                    access.poison=[](void* context,SharedPlaneKey key) noexcept {
                        static_cast<InteropBridge*>(context)->poisonSharedFrame(key);
                    };
                    sharedInputAccess_=access;
                    if(!provider_->setSharedInputs(access))
                        throw std::runtime_error("Shared input callback owner could not be attached");
                }
            }
            historyGap_ = true;
            active_ = true;
            return VK_SUCCESS;
        } catch (const ColorPresenterRetained &e) {
            unsafe_ = true;
            bridge_.release();
            error_ = e.what();
            return VK_ERROR_UNKNOWN;
        } catch (const std::exception &e) {
            error_ = e.what();
            OutputDebugStringA(e.what());
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    VkResult copy(const VkPresentInfoKHR &info, VkImage source, std::uint64_t generation) override {
        if(!asyncCopy_)frame().copyProgress_ = CopyProgress::NotSubmitted;
        if (!bridge_ || (!asyncCopy_ && frame().pending_) || unsafe_ || timingStopped_.load())
            return VK_ERROR_UNKNOWN;
        continuous::Bundle candidate;
        VkRect2D candidateRect{};
        RECT client{};
        if (!GetClientRect(surface_.original(), &client)) {
            error_ = "Original presentation HWND is unavailable";
            return VK_ERROR_UNKNOWN;
        }
        const auto intent = fsr_menu_control::snapshot();
        const bool transition = extent_.width != wantedExtent_.width || extent_.height != wantedExtent_.height ||
                                format_ != wantedFormat_ || presentMode_ != wantedPresentMode_;
        bool queueOff = presenter_ && !transition && !bootstrapId_ &&
                              (!provider_ || (presentTiming_ && intent.inputObserved && !intent.requestedEnabled));
        const auto submittedCutoff = provider_ ? provider_->submittedCutoff(queue_) : 0;
        // A queued bundle may still have a running producer. Every pre-submit
        // failure returns its lease to Submitted, never frees its input slot.
        struct UnpublishedLease {
            continuous::Provider* provider;
            continuous::Bundle* bundle;
            bool active{};
            ~UnpublishedLease() {
                if (active && bundle && bundle->ready) {
                    provider->rollbackQueuedAcquire(bundle->ticket);
                    *bundle = {};
                }
            }
        } lease{provider_, &candidate};
        bool queueOn{};
        lowResActive_ = lowResHeld_ = false;
        lowResRegion_ = {};
        lowResFallbackReason_ = lowResRequested_ ? "low_res_frame_not_admitted" : nullptr;
        if (provider_ && presenter_ && !transition && !bootstrapId_ && presentTiming_ && intent.requestedEnabled) {
            candidate = provider_->acquireQueuedForPresent(
                queue_, submittedCutoff, {presentEngineFrame_, presentTiming_, 0, true});
            lease.active = candidate.ready;
            candidateRect = outputRectFor(candidate);
            const auto refusal = onRefusalFor(candidate, intent, candidateRect);
            queueOn = candidate.ready && !refusal;
            if (!queueOn && candidate.ready) {
                provider_->rollbackQueuedAcquire(candidate.ticket);
                candidate = {};
                lease.active = false;
            }
            if (lowResRequested_ && refusal) lowResFallbackReason_ = refusal;
        }
        // A timed refusal still has ordinary queued color; missing inputs do
        // not require draining every otherwise healthy in-flight frame.
        if(asyncCopy_&&presenter_&&!transition&&!bootstrapId_&&presentTiming_&&!queueOn)queueOff=true;
        auto colorExtent = wantedExtent_;
        if (lowResRequested_ && queueOn) {
            const auto depth = candidate.depth.successfulImageCreateInfo.extent;
            const auto motion = candidate.motion.successfulImageCreateInfo.extent;
            const auto geometry = detail::lowResOutputGeometry(extent_, candidate.constants,
                {depth.width,depth.height}, {motion.width,motion.height});
            lowResFallbackReason_ = linearBlitSupported_ ? geometry.fallbackReason : "low_res_linear_blit_unsupported";
            if (linearBlitSupported_ && geometry) {
                colorExtent = geometry.canvas;
                lowResRegion_ = geometry.region;
                lowResActive_ = true;
                heldLowResCanvas_ = geometry.canvas;
                heldLowResSource_ = extent_;
            } else {
                // An ON frame decided against low output: later OFF frames
                // follow it at full resolution instead of flipping back.
                heldLowResCanvas_ = heldLowResSource_ = {};
            }
        } else if (lowResRequested_ && linearBlitSupported_ && !transition && heldLowResCanvas_.width &&
                   heldLowResSource_.width == extent_.width && heldLowResSource_.height == extent_.height) {
            colorExtent = heldLowResCanvas_;
            lowResHeld_ = true;
        }
        try {
            applyPendingOutput(colorExtent);
            if(sharedCopy_) {
                if(candidate.ready) {
                    const auto input=candidate.depth.successfulImageCreateInfo.extent;
                    sharedInputExtent_={input.width,input.height};
                }
                bridge_->prepareSharedPlanes(sharedInputExtent_.width,sharedInputExtent_.height);
                if(provider_) {
                    if(!provider_->setSharedInputs(sharedInputAccess_))throw std::runtime_error("Shared input owner changed during present");
                    provider_->prepareSharedInputs(sharedInputExtent_,bridge_->sharedPlaneStats().depthStorage);
                }
            }
        } catch (const std::exception& e) {
            error_ = e.what();
            return VK_ERROR_UNKNOWN;
        }
        if(asyncCopy_&&(queueOff||queueOn)) {
            const auto result=admitAsyncFrame(); if(result!=VK_SUCCESS)return result;
        } else if(asyncCopy_) {
            const auto result=retireAsyncFrames(true); if(result!=VK_SUCCESS)return result;
            frameIndex_=nextFrameIndex_=0;
        }
        frame().copyProgress_=CopyProgress::NotSubmitted;
        if(!frame().slotOwned_) { frame().key_={}; frame().completion_={}; }
        frame().bundle_=candidate; candidate={}; lease.bundle=&frame().bundle_;
        frame().shared_={};
        sharedFallbackReason_=sharedCopy_?"shared_requires_timed_frame":nullptr;
        if(sharedCopy_&&(queueOff||queueOn)) {
            if(!asyncCopy_&&frameGeneration_==UINT64_MAX)return VK_ERROR_UNKNOWN;
            if(frame().bundle_.shared.key.valid()) {
                const auto& color=frame().bundle_.shared.planes[0];
                const auto size=color.successfulImageCreateInfo.extent;
                if(size.width==colorExtent_.width&&size.height==colorExtent_.height)
                    frame().shared_=frame().bundle_.shared;
                else sharedFallbackReason_="shared_color_extent_mismatch";
            } else if(queueOff) {
                bridge_->tryReserveSharedFrame({},false,frame().shared_);
                sharedFallbackReason_="shared_color_unavailable";
            } else sharedFallbackReason_="shared_inputs_private_fallback";
            if(frame().shared_.key.valid())sharedFallbackReason_=nullptr;
            if(!asyncCopy_&&(frame().shared_.key.valid()||frame().bundle_.shared.key.valid())) {
                // Shared sync keeps one command context and retires this receipt at the existing fence/consumer wait boundary.
                frame().key_={0,++frameGeneration_}; frame().completion_={frame().key_};
            }
        }
        frame().queuedRect_=candidateRect;
        struct UnsubmittedFrame {
            FrameContext& copy;
            std::uint32_t& next;
            ~UnsubmittedFrame() {
                if(copy.slotOwned_&&!copy.pending_&&copy.copyProgress_==CopyProgress::NotSubmitted) {
                    next=copy.key_.index;
                    copy.slotOwned_=false; copy.completion_={}; copy.key_={};
                }
            }
        } reservation{frame(),nextFrameIndex_};
        struct UnsubmittedShared {
            InteropBridge* bridge;
            FrameContext& copy;
            ~UnsubmittedShared() {
                if(copy.pending_||copy.copyProgress_!=CopyProgress::NotSubmitted||!copy.shared_.key.valid())return;
                if(!copy.bundle_.shared.key.valid()&&!bridge->cancelRecordedSharedFrame(copy.shared_.key))
                    bridge->poisonSharedFrame(copy.shared_.key);
                copy.shared_={};
            }
        } sharedReservation{bridge_.get(),frame()};
        auto r = resetCopyFence();
        if (r != VK_SUCCESS)
            return r;
        r = vk<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(frame().command_, 0);
        if (r != VK_SUCCESS)
            return r;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        r = vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(frame().command_, &begin);
        if (r != VK_SUCCESS)
            return r;
        beginCopyTimestamps(generation);
        VkImageMemoryBarrier barriers[2]{};
        for (auto &b : barriers) {
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        barriers[0].image = source;
        barriers[0].oldLayout = record_.fake ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        const bool sharedColor=frame().shared_.key.valid();
        const auto colorTarget=sharedColor?frame().shared_.planes[0].image:stable_;
        barriers[1].image = colorTarget;
        barriers[1].oldLayout =
            sharedColor?frame().shared_.planes[0].currentLayout:
                (initialized_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED);
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcAccessMask = initialized_ ? VK_ACCESS_TRANSFER_READ_BIT : 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        if(sharedColor&&frame().shared_.planes[0].ownerQueueFamily==VK_QUEUE_FAMILY_EXTERNAL) {
            barriers[1].srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
            barriers[1].dstQueueFamilyIndex=record_.queueInfo.queueFamilyIndex;
            barriers[1].srcAccessMask=0;
        }
        auto barrier = vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        barrier(frame().command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                nullptr, 0, nullptr, 2, barriers);
        const bool scaled = colorExtent_.width != extent_.width || colorExtent_.height != extent_.height;
        if (!scaled && format_ == VK_FORMAT_R8G8B8A8_UNORM) {
            VkImageCopy c{};
            c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.extent = {extent_.width, extent_.height, 1};
            vk<PFN_vkCmdCopyImage>("vkCmdCopyImage")(frame().command_, source,
                                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colorTarget,
                                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            measurement::command(measurement::Command::ImageCopy);
        } else {
            VkImageBlit c{};
            c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            // A full-domain blit initializes every low-canvas pixel, including
            // the game's existing borders; no crop/clear or high-size copy.
            c.srcOffsets[1] = {static_cast<int>(extent_.width), static_cast<int>(extent_.height), 1};
            c.dstOffsets[1] = {static_cast<int>(colorExtent_.width), static_cast<int>(colorExtent_.height), 1};
            vk<PFN_vkCmdBlitImage>("vkCmdBlitImage")(
                frame().command_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colorTarget,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c, scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
            measurement::command(measurement::Command::Blit);
        }
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout = record_.fake ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = 0;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout = sharedColor?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].srcQueueFamilyIndex=barriers[1].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier(frame().command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                nullptr, 0, nullptr, 2, barriers);
        endCopyTimestamps();
        r = vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(frame().command_);
        if (r != VK_SUCCESS)
            return r;
        if (!record_.fake && (queueOff || queueOn)) {
            auto& semaphore = queuedSemaphores_[{generation, source}];
            if (!semaphore) {
                VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                r = vk<PFN_vkCreateSemaphore>("vkCreateSemaphore")(record_.device, &create, nullptr, &semaphore);
                if (r != VK_SUCCESS) return r;
            }
            frame().queuedSemaphore_ = semaphore;
        } else if (!record_.fake && !copySemaphore_) {
            VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            r = vk<PFN_vkCreateSemaphore>("vkCreateSemaphore")(record_.device, &create, nullptr, &copySemaphore_);
            if (r != VK_SUCCESS) return r;
        }
        std::vector<VkPipelineStageFlags> stages(info.waitSemaphoreCount,
                                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = info.waitSemaphoreCount;
        submit.pWaitSemaphores = info.pWaitSemaphores;
        submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame().command_;
        submit.signalSemaphoreCount = record_.fake ? 0 : 1;
        const auto signal = (queueOff || queueOn) ? frame().queuedSemaphore_ : copySemaphore_;
        submit.pSignalSemaphores = record_.fake ? nullptr : &signal;
        {
            std::lock_guard lock(copyDependencyMutex_);
            frame().pendingAppWaitCount_ = info.waitSemaphoreCount;
            std::copy_n(info.pWaitSemaphores, frame().pendingAppWaitCount_, frame().pendingAppWaits_.begin());
        }
        wsi_acquire::QueueCall timing;
        if (frame().timestampRecorded_) { LARGE_INTEGER submitted{}; QueryPerformanceCounter(&submitted); frame().timestampRecording_.submitQpcTicks = submitted.QuadPart; }
        try { r = submitQueue(submit,frame().fence_,"private_copy_submit",record_.fake ? &timing : nullptr); }
        catch(...) {
            frame().pending_=true;frame().copyProgress_=CopyProgress::SubmissionUncertain;
            unsafe_=true;shutdown_.failedQuarantined=true;
            if(sharedColor)bridge_->poisonSharedFrame(frame().shared_.key);
            throw;
        }
        if (r != VK_SUCCESS) {
            measurement::increment(measurement::Counter::CopyFailures);
            if(r==VK_ERROR_DEVICE_LOST)measurement::increment(measurement::Counter::DeviceLost);
            if(r==VK_ERROR_OUT_OF_HOST_MEMORY || r==VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                std::lock_guard lock(copyDependencyMutex_);
                frame().pendingAppWaitCount_ = 0;
            } else {
                // Only the OOM errors guarantee unchanged synchronization.
                // DEVICE_LOST/unknown submit failures cannot safely replay app
                // waits or claim that the emergency signal will be produced.
                frame().pending_=true;
                frame().copyProgress_=CopyProgress::SubmissionUncertain;
                unsafe_=true;
                shutdown_.failedQuarantined=true;
                if(sharedColor)bridge_->poisonSharedFrame(frame().shared_.key);
            }
            return r;
        }
        measurement::submittedCommand(measurement::CopyRole::Color,
            !scaled&&format_==VK_FORMAT_R8G8B8A8_UNORM?measurement::Command::ImageCopy:measurement::Command::Blit);
        frame().pending_ = true;
        frame().copyProgress_ = CopyProgress::SubmittedPending;
        sourceCopySubmitted(timing, source, generation);
        if(!sharedColor&&frame().slotOwned_)initialized_=true;
        if(sharedColor)try { bridge_->sharedWriteSubmitted(frame().shared_.key,1); }
        catch(...) {
            bridge_->poisonSharedFrame(frame().shared_.key);
            frame().copyProgress_=CopyProgress::SubmissionUncertain;
            unsafe_=true;shutdown_.failedQuarantined=true;return VK_ERROR_UNKNOWN;
        }
        if (frame().timestampRecorded_) {
            frame().timestampPending_ = frame().timestampRecording_; frame().timestampPendingValid_ = true;
            frame().timestampPending_.sourceCopyTokenKnown = record_.fake;
            if (record_.fake) frame().timestampPending_.sourceCopyToken = frame().pendingCopyTiming_.sequence;
        }
        frame().copyProgress_ = CopyProgress::SubmittedPending;
        if (queueOff || queueOn) {
            frame().queuedOff_ = queueOff;
            frame().queuedOn_ = queueOn;
            frame().queuedBundleEntered_ = frame().queuedBundlePublished_ = false;
            frame().queuedCutoff_ = submittedCutoff;
            frame().queuedFrame_ = {presentEngineFrame_, presentTiming_, 0, bool(presentTiming_)};
            frame().queuedIntent_ = provider_ ? intent : fsr_menu_control::ControlIntent{};
            {
                std::lock_guard lock(copyDependencyMutex_);
                frame().healthyQueuedWaits_ = true;
            }
            frame().copyProgress_ = queueOn ? CopyProgress::QueuedOn : CopyProgress::QueuedOff;
            lease.active = false;
            return VK_SUCCESS;
        }
        copyWaitBegin();
        r = vk<PFN_vkWaitForFences>("vkWaitForFences")(record_.device, 1, &frame().fence_, VK_TRUE,
                                                       10'000'000'000ull);
        copyWaitEnd(r, false);
        if(r==VK_ERROR_DEVICE_LOST) {
            frame().copyProgress_=CopyProgress::SubmissionUncertain;
            unsafe_=true;
            shutdown_.failedQuarantined=true;
            return r;
        }
        if (r == VK_SUCCESS) {
            readCompletedTimestamps();
            sourceCopyCompleted();
            frame().pending_ = false;
            initialized_ = true;
            frame().copyProgress_ = CopyProgress::Completed;
            {
                std::lock_guard lock(copyDependencyMutex_);
                frame().pendingAppWaitCount_ = 0;
            }
            // This semaphore was only signalled by the completed copy batch.
            // No WSI wait references it on the healthy zero-wait path. A
            // signalled binary semaphore may be destroyed once every referencing
            // submitted batch completed (vkDestroySemaphore VUID 05149).
            if(copySemaphore_)vk<PFN_vkDestroySemaphore>("vkDestroySemaphore")(record_.device, copySemaphore_, nullptr);
            copySemaphore_ = VK_NULL_HANDLE;
            if (!presenter_) {
                try {
                    // DeviceRecord admits one device queue. Its completed copy
                    // fence drains prior Vulkan work; this fresh bridge has not
                    // submitted D3D12 work and the owner is still unpublished.
                    // Game frame generation takes the maximum the user's Intel
                    // Graphics Software override allows; the OFF route keeps one.
                    auto owner = std::make_shared<ColorPresenter>(
                        *bridge_, sdk_, surface_.original(), presentMode_ == VK_PRESENT_MODE_FIFO_KHR,
                        provider_ != nullptr, provider_ != nullptr,
                        provider_ ? xess_vk::launcher_preferences::startupMultiplier()-1 : 1u, record_.gpuTimestamps);
                    std::lock_guard lock(presenterMutex_);
                    presenter_ = std::move(owner);
                } catch (const ColorPresenterRetained &e) {
                    unsafe_ = true;
                    bridge_.release();
                    error_ = e.what();
                    timingStopped_ = sdkFailed_ = true;
                    return VK_ERROR_UNKNOWN;
                } catch (const std::exception &e) {
                    error_ = e.what();
                    timingStopped_ = sdkFailed_ = true;
                    return VK_ERROR_UNKNOWN;
                }
            }
            if (provider_ && !timingStopped_.load())
                frame().bundle_ = provider_->acquireForPresent(
                    queue_, {info.pWaitSemaphores, info.waitSemaphoreCount}, frame().fence_, submittedCutoff,
                    {presentEngineFrame_, presentTiming_, 0, bool(presentTiming_)});
        }
        return r;
    }
    CopyProgress copyProgress() const override { return frame().copyProgress_; }
    VkResult finishQueuedCopy() override {
        if(hasAsyncFrames())return retireAsyncFrames();
        if(record_.fake&&frame().pending_&&!frame().queuedOff_&&!frame().queuedOn_){
            copyWaitBegin();
            const auto result=vk<PFN_vkWaitForFences>("vkWaitForFences")(record_.device,1,&frame().fence_,VK_TRUE,10'000'000'000ull);
            copyWaitEnd(result);if(result!=VK_SUCCESS)return result;
            readCompletedTimestamps();
            sourceCopyCompleted();frame().pending_=false;frame().copyProgress_=CopyProgress::Completed;
            std::lock_guard lock(copyDependencyMutex_);frame().pendingAppWaitCount_=0;frame().healthyQueuedWaits_=false;return VK_SUCCESS;
        }
        if ((!frame().queuedOff_ && !frame().queuedOn_) || !frame().pending_) return VK_SUCCESS;
        if (frame().queuedBundleEntered_ && !frame().queuedBundlePublished_) {
            shutdown_.failedQuarantined = true;
            return VK_ERROR_UNKNOWN;
        }
        VkResult result{};
        copyWaitBegin();
        {
            CpuWallScope elapsed(copyFenceWaitCpu_,measurement::Metric::CopyFenceWait);
            result = vk<PFN_vkWaitForFences>("vkWaitForFences")(
                record_.device, 1, &frame().fence_, VK_TRUE, 10'000'000'000ull);
        }
        copyWaitEnd(result);
        if (result != VK_SUCCESS) return result;
        readCompletedTimestamps();
        sourceCopyCompleted();
        if(frame().key_.valid()) {
            try {
                CpuWallScope elapsed(copyConsumedWaitCpu_,measurement::Metric::ConsumedWait);
                bridge_->waitUntilConsumed();
                const auto tail=presenter_->frameComplete(frame().completion_);
                if(tail==FrameCompletionState::Pending)presenter_->waitFrame(frame().completion_);
                if(presenter_->frameComplete(frame().completion_)!=FrameCompletionState::Complete)
                    return VK_ERROR_DEVICE_LOST;
            } catch(const std::exception& error) { error_=error.what(); return VK_ERROR_UNKNOWN; }
        }
        if (frame().queuedOn_ && frame().bundle_.ready) {
            if (frame().queuedBundlePublished_) {
                if(frame().key_.valid()) {
                    if(!provider_->releaseAfterBridge(frame().bundle_.ticket,&frame().completion_))return VK_ERROR_DEVICE_LOST;
                    frame().shared_={};
                } else try {
                    CpuWallScope elapsed(copyConsumedWaitCpu_,measurement::Metric::ConsumedWait);
                    bridge_->waitUntilConsumed();
                }
                catch (const std::exception& error) { error_ = error.what(); return VK_ERROR_UNKNOWN; }
                if(!frame().key_.valid())provider_->releaseAfterBridge(frame().bundle_.ticket);
            } else {
                provider_->rollbackQueuedAcquire(frame().bundle_.ticket);
            }
            frame().bundle_ = {};
        }
        if(frame().shared_.key.valid()) {
            if(!bridge_->releaseSharedFrame(frame().shared_.key,frame().completion_))return VK_ERROR_DEVICE_LOST;
            frame().shared_={};
        }
        frame().key_={}; frame().completion_={};
        if (frame().queuedOn_)
            provider_->retireCompletedForPresent(queue_, frame().fence_, frame().queuedCutoff_, frame().queuedFrame_);
        frame().pending_ = frame().queuedOff_ = frame().queuedOn_ = false;
        frame().queuedBundleEntered_ = frame().queuedBundlePublished_ = false;
        frame().queuedFrame_ = {};
        frame().queuedCutoff_ = 0;
        frame().queuedSemaphore_ = VK_NULL_HANDLE;
        initialized_ = true;
        frame().copyProgress_ = CopyProgress::Completed;
        std::lock_guard lock(copyDependencyMutex_);
        frame().pendingAppWaitCount_ = 0;
        frame().healthyQueuedWaits_ = false;
        return VK_SUCCESS;
    }
    void retireChain(std::uint64_t generation) override {
        for (auto it = queuedSemaphores_.begin(); it != queuedSemaphores_.end();) {
            if (it->first.first != generation) { ++it; continue; }
            // Only Runtime's real WSI retirement proof releases these waits.
            vk<PFN_vkDestroySemaphore>("vkDestroySemaphore")(record_.device, it->second, nullptr);
            it = queuedSemaphores_.erase(it);
        }
    }
    bool retainsAppSemaphore(VkSemaphore semaphore) const override {
        std::lock_guard lock(copyDependencyMutex_);
        for(const auto& copy:frames_) {
            const auto end=copy.pendingAppWaits_.begin()+copy.pendingAppWaitCount_;
            if(std::find(copy.pendingAppWaits_.begin(),end,semaphore)!=end&&
                (!copy.healthyQueuedWaits_||vk<PFN_vkGetFenceStatus>("vkGetFenceStatus")(record_.device,copy.fence_)!=VK_SUCCESS))return true;
        }
        return false;
    }
    VkSemaphore copyCompletionSemaphore() const override {
        if(record_.fake)return VK_NULL_HANDLE;
        if (frame().copyProgress_ == CopyProgress::QueuedOff || frame().copyProgress_ == CopyProgress::QueuedOn) return frame().queuedSemaphore_;
        return frame().copyProgress_ == CopyProgress::SubmittedPending ? copySemaphore_ : VK_NULL_HANDLE;
    }
    void nativePresentIssued(std::uint64_t, bool emergencyWait) override {
        shutdown_.nativeTailRetained = true;
        emergencyWaitHandedOff_ |= emergencyWait;
    }
    void stopAdmission() override {
        std::shared_ptr<ColorPresenter> owner;
        {
            std::lock_guard lock(presenterMutex_);
            timingStopped_ = true;
            owner = presenter_;
        }
        if (owner) owner->stopAdmission();
    }
    ShutdownState shutdownState() const override { return shutdown_; }
    bool discardOutput() override {
        if (frame().copyProgress_ != CopyProgress::Completed && frame().copyProgress_ != CopyProgress::QueuedOff &&
            frame().copyProgress_ != CopyProgress::QueuedOn) return false;
        if (frame().queuedBundleEntered_) return false;
        if (frame().bundle_.ready) {
            if (frame().queuedOn_) provider_->rollbackQueuedAcquire(frame().bundle_.ticket);
            else provider_->releaseAfterBridge(frame().bundle_.ticket);
        }
        frame().bundle_ = {};
        status_ = {};
        beginResetTrace(false);
        clearFollowup("output_discarded");
        finishResetTrace();
        historyGap_ = true;
        return true;
    }
    VkResult output() override {
        status_ = {};
        if (timingStopped_.load()) {
            beginResetTrace(false); clearFollowup("timing_stopped"); finishResetTrace();
            return VK_ERROR_UNKNOWN;
        }
        beginResetTrace(true);
        try {
            NativeImageSource color{};
            color.device = record_.device;
            color.image = stable_;
            color.ownerQueueFamily = record_.queueInfo.queueFamilyIndex;
            color.currentLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            auto &i = color.successfulImageCreateInfo;
            i.imageType = VK_IMAGE_TYPE_2D;
            i.format = VK_FORMAT_R8G8B8A8_UNORM;
            i.extent = {colorExtent_.width, colorExtent_.height, 1};
            i.mipLevels = i.arrayLayers = 1;
            i.samples = VK_SAMPLE_COUNT_1_BIT;
            i.tiling = VK_IMAGE_TILING_OPTIMAL;
            i.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            i.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if(frame().shared_.key.valid()) {
                color=frame().shared_.planes[0];
                color.currentLayout=VK_IMAGE_LAYOUT_GENERAL;
                color.ownerQueueFamily=record_.queueInfo.queueFamilyIndex;
            }
            status_.sharedCopyPath=sharedCopy_;
            status_.sharedColorDirect=frame().shared_.key.valid();
            status_.sharedAuxiliaryDirect=false;
            status_.sharedPlaneKey=frame().shared_.key.valid()?frame().shared_.key:frame().bundle_.shared.key;
            status_.sharedFallbackReason=sharedFallbackReason_;
            status_.depthCompute=frame().bundle_.ready&&frame().bundle_.depthCompute;
            status_.outputWidth = extent_.width;
            status_.outputHeight = extent_.height;
            status_.lowResOutputRequested = lowResRequested_;
            status_.lowResOutputActive = lowResActive_;
            status_.lowResOutputHeld = lowResHeld_;
            status_.xefgSourceExtent = extent_;
            status_.xefgOutputExtent = colorExtent_;
            status_.lowResOutputFallbackReason = lowResFallbackReason_;
            status_.presentMode = static_cast<std::uint32_t>(presentMode_);
            // Long output-entry intervals reset interpolation history. Observed
            // follow-on Present stalls motivate the optional next-frame reset;
            // the SDK does not promise that resetHistory clears pacing state.
#ifdef XESS_PRESENTATION_NATIVE_TEST
            const auto outputNow = testing::outputNow();
#else
            const auto outputNow = std::chrono::steady_clock::now();
#endif
            bool longFrame = false;
            const double thresholdMs = std::max(longFrameFloorMs, longFrameFactor * outputIntervalEmaMs_);
            if (resetTrace_.requested) resetTrace_.thresholdNs = static_cast<std::uint64_t>(thresholdMs * 1'000'000.0);
            if (previousOutputTimed_) {
                const double ms = std::chrono::duration<double, std::milli>(outputNow - previousOutputTime_).count();
                longFrame = ms > thresholdMs;
                if (resetTrace_.requested) {
                    resetTrace_.intervalKnown = true;
                    resetTrace_.outputIntervalNs = static_cast<std::uint64_t>(ms * 1'000'000.0);
                }
                outputIntervalEmaMs_ += 0.1 * (std::min(ms, 4.0 * outputIntervalEmaMs_) - outputIntervalEmaMs_);
            }
            resetTrace_.longFrame = resetTrace_.requested && longFrame;
            previousOutputTime_ = outputNow;
            previousOutputTimed_ = true;
            const auto intent = (frame().queuedOff_ || frame().queuedOn_) ? frame().queuedIntent_ : fsr_menu_control::snapshot();
            status_.controlEpoch = intent.controlEpoch;
            status_.menuRequestedEnabled = intent.requestedEnabled;
            status_.menuPaired = intent.paired;
            status_.menuGatesConsistent = intent.gatesConsistent;
            status_.menuNativeSuppressed = nativeSuppressedFor(frame().bundle_, intent);
            status_.menuInputObserved = intent.inputObserved;
            status_.menuGatesReady = intent.bothGatesReady;
            const auto currentIntent = fsr_menu_control::snapshot();
            status_.gameplayGateRequired = intent.gameplayGateRequired || currentIntent.gameplayGateRequired;
            status_.gameplayState = currentIntent.gameplayGateRequired ? currentIntent.gameplayState : intent.gameplayState;
            status_.gameplayReady = fsr_menu_control::gameplayAllowsInterpolation(intent) &&
                fsr_menu_control::gameplayAllowsInterpolation(currentIntent);
            status_.engineFrame = presentEngineFrame_;
            status_.inputMatched = frame().bundle_.ready;
            status_.queuedOff = frame().queuedOff_;
            status_.queuedOn = frame().queuedOn_;
            if (frame().bundle_.ready) {
                const auto depth = frame().bundle_.depth.successfulImageCreateInfo.extent;
                const auto motion = frame().bundle_.motion.successfulImageCreateInfo.extent;
                status_.depthExtent = {depth.width, depth.height};
                status_.motionExtent = {motion.width, motion.height};
                status_.renderSize = frame().bundle_.constants.renderSize;
                status_.upscaleSize = frame().bundle_.constants.upscaleSize;
                status_.outputRegion = frame().bundle_.constants.outputRegion;
            }
            const auto& region = status_.outputRegion;
            const auto outputRect = frame().queuedOn_ ? frame().queuedRect_ : outputRectFor(frame().bundle_);
            const auto taggedRect = lowResActive_ ? lowResRegion_ : outputRect;
            status_.xefgRegion = taggedRect;
            const bool regionInBounds = outputRect.extent.width != 0;
            auto onRefusal = frame().queuedOn_ ? nullptr : onRefusalFor(frame().bundle_, intent, outputRect);
            status_.characterMenu = intent.characterMenu || currentIntent.characterMenu;
            if (intent.requestedEnabled && !status_.gameplayReady)
                onRefusal = status_.characterMenu ? "character_menu" : "not_in_gameplay";
            if (frame().queuedOn_ && intent.localFiOffRequested && !status_.menuNativeSuppressed)
                onRefusal = "native_fsr_not_suppressed";
            // A late, unqueued bundle has no region mapped into the held low
            // canvas, so that frame stays OFF rather than resizing to full size.
            if (!onRefusal && lowResHeld_ && intent.requestedEnabled) onRefusal = "low_res_canvas_held";
            const bool enabled = intent.requestedEnabled && !onRefusal;
            status_.sharedAuxiliaryDirect=frame().shared_.key.valid()&&frame().queuedOn_&&enabled;
            const bool followupEligible = resetTrace_.requested && record_.fake && frame().queuedOn_ && enabled &&
                frame().bundle_.ready && !bootstrapId_ && presentTiming_ && presentTiming_.latencyEnabled;
            resetTrace_.eligible = followupEligible;
            if (!followupEligible) clearFollowup("not_admitted_single_chain_on");
            bool followupContinuous{};
            if (record_.gpuTimestamps) {
                GpuTimestampSample identity;
                identity.sourceEngineFrame = presentEngineFrame_;
                identity.sourceEngineFrameKnown = presentTiming_ || presentEngineFrame_ != 0;
                identity.sdkPresentId = presentTiming_.sdkId; identity.sdkPresentIdKnown = bool(presentTiming_);
                identity.sourceCopyTokenKnown = record_.fake && frame().pendingCopyTiming_.sequence != 0;
                if (identity.sourceCopyTokenKnown) identity.sourceCopyToken = frame().pendingCopyTiming_.sequence;
                bridge_->associateGpuTimestampFrame(identity);
            }
            if (frame().queuedOff_) {
                if(frame().shared_.key.valid())
                    presenter_->presentSharedQueuedOff(frame().shared_.key,queue_,frame().fence_,presentTiming_,frame().key_,frame().completion_);
                else if(frame().key_.valid())presenter_->presentQueuedOff(color,queue_,frame().fence_,presentTiming_,frame().key_,frame().completion_);
                else presenter_->presentQueuedOff(color, queue_, frame().fence_, presentTiming_);
                historyGap_ = true;
            } else if (frame().queuedOn_ && !enabled) {
                // Certificate revocation keeps the queued color path; auxiliary
                // inputs were never published and retain their existing rollback.
                if(frame().shared_.key.valid()) {
                    frame().queuedBundleEntered_=true;
                    presenter_->presentSharedQueuedOff(frame().shared_.key,queue_,frame().fence_,presentTiming_,frame().key_,frame().completion_);
                    frame().queuedBundlePublished_=true;
                } else if(frame().key_.valid())presenter_->presentQueuedOff(color,queue_,frame().fence_,presentTiming_,frame().key_,frame().completion_);
                else presenter_->presentQueuedOff(color, queue_, frame().fence_, presentTiming_);
                historyGap_ = true;
            } else if (!presentTiming_ && !bootstrapId_) {
                presenter_->presentUntimedOff(color, queue_, frame().fence_);
                if (frame().bundle_.ready) provider_->releaseAfterBridge(frame().bundle_.ticket);
                frame().bundle_ = {};
                historyGap_ = true;
            } else if (frame().bundle_.ready) {
                status_.inputCommands=frame().bundle_.commands;
                const auto &constants = frame().bundle_.constants;
                status_.producerToken = constants.producerToken;
                status_.recordingGeneration = constants.recordingGeneration;
                status_.viewportGeneration = constants.viewportGeneration;
                status_.submission = frame().bundle_.submission;
                status_.inputValidity = constants.validityMask;
                const std::array<std::int32_t, 4> effectiveRect = regionInBounds ? region.rect :
                    std::array<std::int32_t, 4>{0, 0, static_cast<std::int32_t>(extent_.width), static_cast<std::int32_t>(extent_.height)};
                std::uint32_t resetReasons{};
                if ((constants.validityMask & frame_input::Reset) && constants.reset) resetReasons |= HistoryResetInput;
                if (historyGap_) resetReasons |= HistoryResetGap;
                if (enabled != previousEnabled_) resetReasons |= HistoryResetEnabled;
                if (constants.producerToken != previousToken_ + 1) resetReasons |= HistoryResetProducerGap;
                if (constants.viewportGeneration != previousViewport_) resetReasons |= HistoryResetViewport;
                if (intent.controlEpoch != previousEpoch_) resetReasons |= HistoryResetControl;
                if (effectiveRect != previousOutputRect_) resetReasons |= HistoryResetRegion;
                if (longFrame) resetReasons |= HistoryResetLongFrame;
                const bool sameHistory = !(resetReasons & ~HistoryResetLongFrame);
                const bool sameOwner = presentTiming_.contextGeneration == previousFollowupContext_ &&
                    frame().pendingCopyTiming_.generation == previousFollowupCopyGeneration_;
                followupContinuous = sameHistory && sameOwner;
                if (followupPending_ && !sameHistory) clearFollowup("history_discontinuity");
                if (followupPending_ && !sameOwner) clearFollowup("owner_or_copy_generation_changed");
                const bool extra = followupEligible && resetTrace_.policyEnabled && followupPending_;
                if (extra) resetReasons |= HistoryResetFollowup;
                const bool reset = resetReasons != 0;
                status_.historyResetKnown = true;
                status_.historyResetReasons = resetReasons;
                if (resetTrace_.requested) {
                    resetTrace_.extraRequested = extra;
                    resetTrace_.resetRequested = reset && enabled && !bootstrapId_;
                    resetTrace_.resetReasons = resetReasons;
                }
                if (longFrame && enabled) ++longFrameResets_;
                NativeFrame nativeFrame{{color, frame().bundle_.depth, frame().bundle_.motion}, queue_, frame().fence_};
                nativeFrame.sharedSourceKey=frame().bundle_.shared.key;
                if (bootstrapId_)
                    presenter_->presentBootstrap(color, queue_, frame().fence_, bootstrapId_);
                else if (frame().queuedOn_) {
                    frame().queuedBundleEntered_ = true;
                    if(frame().shared_.key.valid())
                        presenter_->presentSharedBundleQueued(frame().shared_.key,queue_,frame().fence_,constants,reset,presentTiming_,taggedRect,frame().key_,frame().completion_);
                    else if(frame().key_.valid())presenter_->presentBundleQueued(nativeFrame,constants,reset,presentTiming_,taggedRect,frame().key_,frame().completion_);
                    else presenter_->presentBundleQueued(nativeFrame, constants, reset, presentTiming_, taggedRect);
                    frame().queuedBundlePublished_ = true;
                } else
                    presenter_->presentBundle(nativeFrame, constants, enabled, reset, presentTiming_, taggedRect);
                if (!frame().queuedOn_) provider_->releaseAfterBridge(frame().bundle_.ticket);
                previousToken_ = constants.producerToken;
                previousViewport_ = constants.viewportGeneration;
                previousOutputRect_ = effectiveRect;
                historyGap_ = !enabled;
                if (!frame().queuedOn_) frame().bundle_ = {};
            } else {
                if (bootstrapId_)
                    presenter_->presentBootstrap(color, queue_, frame().fence_, bootstrapId_);
                else
                    presenter_->present(color, queue_, frame().fence_, presentTiming_);
                historyGap_ = true;
            }
            previousEnabled_ = enabled;
            previousEpoch_ = intent.controlEpoch;
            if(frame().key_.valid()&&(!frame().completion_.producedValid||!frame().completion_.consumedValid||
                !frame().completion_.sdkTailValid||!frame().completion_.d3dWaitQueued||!frame().completion_.callerSubmitted||
                frame().completion_.poisoned||frame().completion_.key!=frame().key_||
                (frame().shared_.key.valid()&&(frame().completion_.sharedKey!=frame().shared_.key||
                    frame().completion_.sharedAuxiliary!=status_.sharedAuxiliaryDirect||!frame().completion_.postPresentSubmitted))))
                throw std::runtime_error("Queued frame has no valid post-Present completion receipt");
            const auto sdk = presenter_->lastPresentStatus();
            observeSdkResult(sdk);
            const bool successfulOn = followupEligible && frame().queuedBundlePublished_ && sdk.valid && sdk.queryResult == 0 && sdk.frameGenResult >= 0 &&
                sdk.requestedEnabled && sdk.isFrameGenEnabled && !sdk.occluded && sdk.framesPresented > 0;
            resetTrace_.outputSucceeded = successfulOn;
            if (followupEligible) {
                if (!successfulOn) clearFollowup(sdk.occluded ? "output_occluded" : "sdk_output_not_successful");
                else {
                    if (resetTrace_.extraRequested) { followupPending_ = false; resetTrace_.extraSucceeded = true; }
                    // Only a newly detected long interval arms the next frame; an extra reset cannot rearm itself.
                    if (resetTrace_.policyEnabled && longFrame && followupContinuous) {
                        followupPending_ = true;
                        followupTrigger_ = {presentEngineFrame_, presentTiming_.sdkId, frame().pendingCopyTiming_.sequence};
                        resetTrace_.armed = true;
                    }
                    previousFollowupContext_ = presentTiming_.contextGeneration;
                    previousFollowupCopyGeneration_ = frame().pendingCopyTiming_.generation;
                }
            }
            status_.bridgePublishCpu = sdk.bridgePublishCpu;
            status_.previousD3DWaitCpu = sdk.previousD3DWaitCpu;
            status_.dxgiPresentCpu = sdk.dxgiPresentCpu;
            status_.xellSleepCpu = sdk.xellSleepCpu;
            status_.d3dCallerCopyTagGpu = sdk.d3dCallerCopyTagGpu;
            status_.requestedEnabled = sdk.requestedEnabled;
            status_.sdkEnabled = sdk.isFrameGenEnabled;
            status_.interpolatedFrames = sdk.interpolatedFrames;
            status_.multiplierRequested=sdk.multiplierRequested;status_.multiplierMaximum=sdk.multiplierMaximum;
            status_.longFrameResets = longFrameResets_;
            status_.sleepFrameId = sdk.sleepFrameId;
            status_.sleepCalls = sdk.sleepCalls;
            status_.presentMarkerCalls = sdk.presentMarkerCalls;
            status_.consecutiveEnabledPresents = sdk.consecutiveEnabledPresents;
            status_.sleepResult = sdk.sleepResult;
            status_.presentStartResult = sdk.presentStartResult;
            status_.presentEndResult = sdk.presentEndResult;
            status_.partialXeLL = sdk.partialXeLLTiming;
            status_.timingPolicy = sdk.timingPolicy;
            status_.totalMarkerCalls = sdk.totalMarkerCalls;
            status_.completeFrameTiming = sdk.completeFrameTiming;
            status_.sdkPresentId = sdk.sdkPresentId;
            status_.untimedOff = sdk.untimedOff;
            status_.latencyTransitionAdmitted = sdk.latencyTransitionAdmitted;
            status_.onBlockedReason = onRefusal ? onRefusal : sdk.onBlockedReason;
            status_.outputOccluded = sdk.occluded;
            if (sdk.occluded) {
                historyGap_ = true;
                previousEnabled_ = false;
            }
            finishResetTrace();
            error_.clear();
            return VK_SUCCESS;
        } catch (const ColorPresenterUnavailable &e) {
            observeFailedSdkResult();
            clearFollowup("output_unavailable"); finishResetTrace();
            if(frame().key_.valid()) {
                frame().completion_.poisoned=true; unsafe_=sdkFailed_=true; shutdown_.failedQuarantined=true;
                if(frame().shared_.key.valid())bridge_->poisonSharedFrame(frame().shared_.key);
                if(frame().bundle_.shared.key.valid())bridge_->poisonSharedFrame(frame().bundle_.shared.key);
                error_=e.what(); return VK_ERROR_UNKNOWN;
            }
            if (frame().queuedBundleEntered_ && !frame().queuedBundlePublished_) {
                unsafe_ = sdkFailed_ = true;
                shutdown_.failedQuarantined = true;
                error_ = e.what();
                return VK_ERROR_UNKNOWN;
            }
            status_.untimedTimingUnavailable = !presentTiming_ && !bootstrapId_;
            error_ = e.what();
            historyGap_ = true;
            // Queued color fallback can refuse before any Bridge publication.
            // Its producer may still run; the original copy fence owns retirement.
            if (!frame().queuedOn_ || frame().queuedBundlePublished_) {
                if (frame().bundle_.ready) provider_->releaseAfterBridge(frame().bundle_.ticket);
                frame().bundle_ = {};
            }
            return VK_NOT_READY;
        } catch (const std::exception &e) {
            observeFailedSdkResult();
            clearFollowup("output_failed"); finishResetTrace();
            if(frame().key_.valid()) {
                frame().completion_.poisoned=true; unsafe_=sdkFailed_=true; shutdown_.failedQuarantined=true;
                if(frame().shared_.key.valid())bridge_->poisonSharedFrame(frame().shared_.key);
                if(frame().bundle_.shared.key.valid())bridge_->poisonSharedFrame(frame().bundle_.shared.key);
            }
            if (frame().queuedBundleEntered_ && !frame().queuedBundlePublished_) {
                unsafe_ = sdkFailed_ = true;
                shutdown_.failedQuarantined = true;
            }
            error_ = e.what();
            OutputDebugStringA(e.what());
            return VK_ERROR_UNKNOWN;
        }
    }
    VkResult drain() override {
        if(hasAsyncFrames()) {
            const auto result=retireAsyncFrames(true);
            if(result!=VK_SUCCESS) { shutdown_.failedQuarantined=true; return result; }
        }
        if (unsafe_ || sdkFailed_ || emergencyWaitHandedOff_) {
            shutdown_.failedQuarantined = true;
            return VK_ERROR_UNKNOWN;
        }
        if (frame().pending_) {
            if (frame().queuedOff_ || frame().queuedOn_) {
                const auto result = finishQueuedCopy();
                if (result != VK_SUCCESS) {
                    shutdown_.failedQuarantined = true;
                    return result;
                }
            }
        }
        if (frame().pending_) {
            const auto r = vk<PFN_vkWaitForFences>("vkWaitForFences")(record_.device, 1, &frame().fence_,
                                                                      VK_TRUE, 10'000'000'000ull);
            if (r != VK_SUCCESS) {
                shutdown_.failedQuarantined = true;
                return r;
            }
            frame().pending_ = false;
            readCompletedTimestamps();
            sourceCopyCompleted();
        }
        try {
            if (presenter_)
                presenter_->shutdown();
            active_ = false;
            shutdown_.sdkReleased = true;
            release();
            shutdown_.bridgeReleased = true;
            shutdown_.privateCleanupCompleted = true;
            error_ = shutdown_.nativeTailRetained ? "Private cleanup complete; native WSI tail retained" : "";
            return VK_SUCCESS;
        } catch (const std::exception &e) {
            OutputDebugStringA(e.what());
            unsafe_ = true;
            shutdown_.failedQuarantined = true;
            error_ = e.what();
            return VK_ERROR_UNKNOWN;
        }
    }
    void requestTimingStop() override {
        std::shared_ptr<ColorPresenter> owner;
        {
            std::lock_guard lock(presenterMutex_);
            timingStopped_ = true;
            sdkFailed_ = true;
            shutdown_.failedQuarantined = true;
            owner = presenter_;
        }
        if (owner)
            owner->requestStop();
        error_ = "Continuous presentation timing is unavailable; SDK path retired";
    }
    const char *error() const override {
        return error_.c_str();
    }
    FrameStatus frameStatus() const override {
        auto status = status_;
        status.longFrameReset = resetTrace_;
        status.copyFenceWaitCpu = copyFenceWaitCpu_;
        status.copyConsumedWaitCpu = copyConsumedWaitCpu_;
        status.inflightCapWaitCpu=inflightCapWaitCpu_;
        status.inflightFrames=static_cast<std::uint32_t>(std::count_if(frames_.begin(),frames_.end(),[](const auto& f){return f.slotOwned_;}));
        status.inflightHighWater=inflightHighWater_;
        status.inflightLimit=asyncCopy_ ? asyncFrameLimit_ : 0;
        status.asyncCopyPath=asyncCopy_;
        if(frame().bundle_.ready)status.inputCommands=frame().bundle_.commands;
        if(provider_){const auto inputs=provider_->stats();status.providerDepthGpu=inputs.depthGpu;status.providerAuxGpu=inputs.auxGpu;}
        status.sharedCopyPath=sharedCopy_;
        if(bridge_&&sharedCopy_)status.sharedPlanes=bridge_->sharedPlaneStats();
        status.previousCopy = previousCopyTiming_;
        status.immediateCopy = immediateCopyTiming_;
        status.vkPrivateCopyGpu = asyncCopy_&&latestPrivateTimestamp_.requested ? latestPrivateTimestamp_ : frame().timestampLatest_;
        if (bridge_) status.vkInteropCopyGpu = bridge_->gpuTimestampSample();
        return status;
    }
    FrameTimingTicket beginGameFrame(std::uint32_t id) override {
        const auto owner = borrowPresenter();
        return owner ? owner->beginGameFrame(id) : FrameTimingTicket{};
    }
    void simulationEnd(FrameTimingTicket ticket) override {
        const auto owner = borrowPresenter();
        if (owner)
            owner->simulationEnd(ticket);
    }
    void renderSubmitBegin(FrameTimingTicket ticket) override {
        const auto owner = borrowPresenter();
        if (owner)
            owner->renderSubmitBegin(ticket);
    }
    void renderSubmitEnd(FrameTimingTicket ticket) override {
        const auto owner = borrowPresenter();
        if (owner)
            owner->renderSubmitEnd(ticket);
    }
    void associatePresentTiming(std::uint32_t engineFrame, FrameTimingTicket ticket,
                                std::uint32_t bootstrapId) override {
        presentEngineFrame_ = engineFrame;
        presentTiming_ = ticket;
        bootstrapId_ = bootstrapId;
    }
    void abandonFrameTiming(FrameTimingTicket ticket) noexcept override {
        std::shared_ptr<ColorPresenter> owner;
        {
            std::lock_guard lock(presenterMutex_);
            owner = presenter_;
        }
        if (owner) owner->abandonFrameTiming(ticket);
    }
    std::uint64_t timingContextGeneration() const override {
        const auto owner = borrowPresenter();
        return owner ? owner->timingContextGeneration() : 0;
    }
    std::uint32_t nextSdkId() const override {
        const auto owner = borrowPresenter();
        return owner ? owner->nextSdkId() : 0;
    }
};
} // namespace
DeviceRecord::DeviceRecord(VkInstance i, VkPhysicalDevice p, VkDevice d,
                           PFN_vkGetInstanceProcAddr g, PFN_vkGetDeviceProcAddr gd,
                           std::uint32_t api, const VkDeviceCreateInfo &info, bool fake, QueueGate* gate,
                           wsi_acquire::QueueObserver queueObserver, void* queueObserverContext)
    : impl_(std::make_unique<Impl>()) {
    if (info.flags || info.queueCreateInfoCount != 1 || !info.pQueueCreateInfos ||
        info.pQueueCreateInfos[0].queueCount != 1 || info.pQueueCreateInfos[0].flags ||
        info.pQueueCreateInfos[0].pNext)
        throw std::invalid_argument("Presentation takeover requires exactly one unprotected device queue");
    auto &r = *impl_;
    r.queueObserver = queueObserver; r.queueObserverContext = queueObserverContext;
    if (fake && (!gate || info.pQueueCreateInfos[0].queueFamilyIndex != 0))
        throw std::invalid_argument("Fake swapchain requires device-owned gate and sole queue family zero");
    r.fake = fake;
    r.gpuTimestamps = gpuTimestampsRequested();
    r.queueGate = gate;
    r.instance = i;
    r.physical = p;
    r.device = d;
    r.gipa = g;
    r.gdpa = gd;
    r.app.apiVersion = api;
    r.instanceInfo.pApplicationInfo = &r.app;
    r.queueInfo = info.pQueueCreateInfos[0];
    r.queueInfo.pNext = nullptr;
    r.queueInfo.pQueuePriorities = &r.priority;
    const auto *node = reinterpret_cast<const VkBaseInStructure *>(info.pNext);
    for (unsigned depth = 0; node && depth < 256; ++depth, node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
            r.timeline.timelineSemaphore =
                reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures *>(node)
                    ->timelineSemaphore;
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            r.timeline.timelineSemaphore =
                reinterpret_cast<const VkPhysicalDeviceVulkan12Features *>(node)->timelineSemaphore;
        if (node->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO)
            throw std::invalid_argument("Device groups unsupported");
    }
    if (node || !r.timeline.timelineSemaphore)
        throw std::invalid_argument("Recorded device must enable timelineSemaphore");
    for (std::uint32_t n = 0; n < info.enabledExtensionCount; ++n)
        r.extensionStorage.emplace_back(info.ppEnabledExtensionNames[n]);
    for (const auto &e : r.extensionStorage)
        r.extensions.push_back(e.c_str());
    r.deviceInfo.pNext = &r.timeline;
    r.deviceInfo.queueCreateInfoCount = 1;
    r.deviceInfo.pQueueCreateInfos = &r.queueInfo;
    r.deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(r.extensions.size());
    r.deviceInfo.ppEnabledExtensionNames = r.extensions.data();
}
DeviceRecord::~DeviceRecord() = default;
std::uint32_t DeviceRecord::family() const {
    return impl_->queueInfo.queueFamilyIndex;
}
std::unique_ptr<Runtime> DeviceRecord::create(Surface &surface, VkQueue queue,
                                              continuous::Provider *provider) {
    auto &r = *impl_;
    Dispatch dispatch{r.vk<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR"),
                      r.vk<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR"),
                      r.vk<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR"),
                      r.vk<PFN_vkQueuePresentKHR>("vkQueuePresentKHR")};
    return std::make_unique<Runtime>(r.device, surface.handle, queue, dispatch,
                                     std::make_unique<NativeBackend>(r, surface, queue, provider),r.fake);
}
} // namespace xess_vk::presentation
