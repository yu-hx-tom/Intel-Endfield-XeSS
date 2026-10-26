#pragma once
#include "frame_timing.hpp"
#include "fg_tuning.hpp"
#include "gpu_timestamp_sample.hpp"
#include "interop.hpp"
#include "upscale_frame_constants.hpp"
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
#include <windows.h>
namespace xess_vk::continuous {
class Provider;
}
namespace xess_vk::presentation {
using QueueGate = xess_vk::QueueGate;
#ifdef XESS_PRESENTATION_NATIVE_TEST
namespace testing {
struct AcquireProbe {
    const char* reason{};
    VkImage image{};
    std::uint64_t generation{}, token{}, submitted{}, lastCopy{}, lastAcquire{}, entryNs{}, returnNs{};
    VkResult result{VK_ERROR_UNKNOWN};
    bool resultKnown{}, async{}, unsafe{}, stopped{}, poolFound{}, imageFound{}, signalAttempted{};
};
void acquireProbe(const AcquireProbe&) noexcept;
}
#endif
struct CopyFenceTiming {
    std::uint64_t sequence{}, generation{}, image{}, fence{}, queue{};
    std::uint32_t engineFrame{};
    wsi_acquire::QueueCall submit;
    std::uint64_t waitEntryNs{}, waitReturnNs{};
    std::int32_t waitResult{};
    GpuTimestampSample privateCopyGpu;
    bool privateCopyQueryMatch{};
};
enum HistoryResetReason : std::uint32_t {
    HistoryResetInput=1u, HistoryResetGap=2u, HistoryResetEnabled=4u, HistoryResetProducerGap=8u,
    HistoryResetViewport=16u, HistoryResetControl=32u, HistoryResetRegion=64u,
    HistoryResetLongFrame=128u, HistoryResetFollowup=256u
};
// Bounded diagnostic snapshots: a trigger/clear plus the following four outputs.
struct LongFrameResetTrace {
    struct Trigger {
        std::uint32_t engineFrame{}, sdkPresentId{};
        std::uint64_t copyToken{};
        friend bool operator==(const Trigger&, const Trigger&) = default;
    };
    bool requested{}, policyEnabled{}, outputEntryKnown{}, intervalKnown{}, emit{}, longFrame{}, eligible{},
        extraRequested{}, extraSucceeded{}, resetRequested{}, outputSucceeded{}, armed{},
        pendingBefore{}, pendingAfter{};
    std::uint32_t engineFrame{}, sdkPresentId{}, resetReasons{};
    std::uint64_t traceSequence{}, copyToken{}, copyGeneration{}, timingContext{}, outputEntryQpcTicks{}, qpcFrequency{},
        outputIntervalNs{}, thresholdNs{};
    Trigger triggerBefore, triggerAfter;
    const char* clearReason{};
};
struct FrameStatus {
    // Cumulative per-owner CPU wall counters; nested detail is not additive.
    CpuWallTime runtimeCopyCpu, nativePresentCpu, outputCpu;
    CpuWallTime bridgePublishCpu, previousD3DWaitCpu, dxgiPresentCpu, xellSleepCpu;
    // Inside runtimeCopyCpu: waits before the single private copy can be reused.
    CpuWallTime copyFenceWaitCpu, copyConsumedWaitCpu;
    CpuWallTime inflightCapWaitCpu;
    std::uint32_t inflightFrames{}, inflightHighWater{}, inflightLimit{};
    bool asyncCopyPath{};
    bool sharedCopyPath{},sharedColorDirect{},sharedAuxiliaryDirect{},depthCompute{};
    SharedPlaneKey sharedPlaneKey;
    SharedPlaneStats sharedPlanes;
    const char* sharedFallbackReason{};
    CopyFenceTiming previousCopy, immediateCopy;
    GpuTimestampSample vkPrivateCopyGpu, vkInteropCopyGpu, d3dCallerCopyTagGpu;
    GpuTimestampSample providerDepthGpu,providerAuxGpu;
    measurement::Commands inputCommands{};
    std::uint32_t outputWidth{}, outputHeight{};
    bool lowResOutputRequested{}, lowResOutputActive{};
    // OFF/refused frame scaled into the last admitted low canvas (no resize).
    bool lowResOutputHeld{};
    VkExtent2D xefgSourceExtent{}, xefgOutputExtent{};
    VkRect2D xefgRegion{};
    const char* lowResOutputFallbackReason{};
    VkExtent2D depthExtent{}, motionExtent{};
    std::array<std::int32_t, 2> renderSize{}, upscaleSize{};
    frame_input::OutputRegionSample outputRegion;
    std::uint32_t presentMode{UINT32_MAX};
    std::uint64_t producerToken{}, recordingGeneration{}, viewportGeneration{}, submission{},
        controlEpoch{};
    std::uint32_t inputValidity{}, framesPresented{}, interpolatedFrames{};
    std::uint32_t multiplierRequested{},multiplierMaximum{};
    fg_tuning::Applied fgTuning;
    fg_tuning::Totals fgOutcomes;
    bool historyResetKnown{};
    std::uint32_t historyResetReasons{};
    // ON frames whose history was reset because the real frame came after a stall.
    std::uint64_t longFrameResets{};
    LongFrameResetTrace longFrameReset;
    std::int32_t queryResult{}, frameGenResult{};
    bool inputMatched{}, requestedEnabled{}, sdkEnabled{}, sdkStatusValid{}, sdkResultKnown{};
    std::uint64_t sleepFrameId{}, sleepCalls{}, presentMarkerCalls{}, consecutiveEnabledPresents{};
    std::int32_t sleepResult{}, presentStartResult{}, presentEndResult{};
    bool partialXeLL{};
    const char *timingPolicy{};
    std::uint32_t engineFrame{}, totalMarkerCalls{};
    bool completeFrameTiming{}, untimedOff{}, latencyTransitionAdmitted{};
    const char *onBlockedReason{};
    std::uint32_t sdkPresentId{};
    bool untimedTimingUnavailable{};
    bool menuRequestedEnabled{}, menuPaired{}, menuGatesConsistent{};
    bool menuNativeSuppressed{}, menuInputObserved{}, menuGatesReady{};
    bool gameplayGateRequired{}, gameplayReady{}, characterMenu{};
    std::int32_t gameplayState{-1};
    bool outputOccluded{}, queuedOff{}, queuedOn{};
};
struct Dispatch {
    PFN_vkCreateSwapchainKHR create{};
    PFN_vkDestroySwapchainKHR destroy{};
    PFN_vkGetSwapchainImagesKHR images{};
    PFN_vkQueuePresentKHR present{};
};
enum class CopyProgress { NotSubmitted, SubmittedPending, Completed, SubmissionUncertain, QueuedOff, QueuedOn };
struct ShutdownState {
    bool privateCleanupCompleted{}, sdkReleased{}, bridgeReleased{};
    bool nativeTailRetained{}, failedQuarantined{};
};
class Backend {
  public:
    virtual ~Backend() = default;
    virtual VkResult prepare(const VkSwapchainCreateInfoKHR &) = 0;
    virtual VkResult createFakeImages(const VkSwapchainCreateInfoKHR&, std::uint64_t, std::vector<VkImage>&) { return VK_ERROR_FEATURE_NOT_PRESENT; }
    virtual VkResult destroyFakeImages(std::uint64_t) { return VK_ERROR_FEATURE_NOT_PRESENT; }
    virtual VkResult signalAcquire(VkSemaphore, VkFence, wsi_acquire::Call* = nullptr) { return VK_ERROR_FEATURE_NOT_PRESENT; }
    virtual bool asyncAcquireEnabled() const { return false; }
    virtual VkResult signalAcquireAfterCopy(VkImage, std::uint64_t, std::uint64_t, VkSemaphore, VkFence, wsi_acquire::Call* = nullptr) { return VK_ERROR_FEATURE_NOT_PRESENT; }
    virtual VkResult windowStatus(VkExtent2D) const { return VK_SUCCESS; }
    virtual std::uint64_t copyCompletionToken() const { return 0; }
    virtual VkResult pollCopyCompletion(std::uint64_t) { return VK_ERROR_FEATURE_NOT_PRESENT; }
    virtual VkResult consumePresentWaits(const VkPresentInfoKHR&) { return VK_ERROR_FEATURE_NOT_PRESENT; }
    virtual VkResult consumePresentWaits(const VkPresentInfoKHR& info, VkImage, std::uint64_t) { return consumePresentWaits(info); }
    // Once submitted, original binary waits belong to this submission even if
    // the CPU fence wait times out. Native present must then use its completion
    // semaphore, never replay the application's waits.
    virtual VkResult copy(const VkPresentInfoKHR &, VkImage, std::uint64_t chainGeneration) = 0;
    virtual CopyProgress copyProgress() const = 0;
    virtual VkSemaphore copyCompletionSemaphore() const = 0;
    // Retire only the previous healthy queued copy before its single slot is reused.
    virtual VkResult finishQueuedCopy() { return VK_SUCCESS; }
    virtual void retireChain(std::uint64_t) {}
    virtual bool retainsAppSemaphore(VkSemaphore) const { return false; }
    virtual void nativePresentIssued(std::uint64_t, bool) {}
    virtual void stopAdmission() {}
    virtual ShutdownState shutdownState() const = 0;
    // Copy fences and queue-tail fences are not WSI retirement proof. Native
    // backend defaults to retention; CPU fixtures can model a real proof.
    virtual bool canDestroyRetiredChain(std::uint64_t) const { return false; }
    virtual VkResult output() = 0;
    virtual bool discardOutput() { return true; }
    virtual VkResult drain() = 0;
    // Stop SDK admission without waiting for or cancelling existing calls.
    virtual void requestTimingStop() {}
    virtual const char *error() const {
        return "";
    }
    virtual FrameStatus frameStatus() const {
        return {};
    }
    virtual FrameTimingTicket beginGameFrame(std::uint32_t = 0) {
        return {};
    }
    virtual void simulationEnd(FrameTimingTicket) {}
    virtual void renderSubmitBegin(FrameTimingTicket) {}
    virtual void renderSubmitEnd(FrameTimingTicket) {}
    virtual void abandonFrameTiming(FrameTimingTicket) noexcept {}
    virtual void associatePresentTiming(std::uint32_t, FrameTimingTicket, std::uint32_t) {}
    virtual std::uint64_t timingContextGeneration() const {
        return 0;
    }
    virtual std::uint32_t nextSdkId() const {
        return 0;
    }
};
class Runtime {
  public:
    // Covers ticket association, all output submissions and the resulting snapshot.
    // This mutex never protects Vulkan queue access or replaces QueueGate.
    std::unique_lock<std::recursive_mutex> admitPresent() const { return std::unique_lock(presentAdmission_); }
    Runtime(VkDevice, VkSurfaceKHR, VkQueue, Dispatch, Backend &, bool fake = false);
    Runtime(VkDevice, VkSurfaceKHR, VkQueue, Dispatch, std::unique_ptr<Backend>, bool fake = false);
    ~Runtime();
    VkResult create(const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *,
                    VkSwapchainKHR *);
    VkResult images(VkSwapchainKHR, std::uint32_t *, VkImage *);
    bool fakeEnabled() const noexcept { return fake_; }
    bool ownsFakeChain(VkSwapchainKHR) const;
    bool isFakeImage(VkImage) const;
    VkResult acquire(VkSwapchainKHR, std::uint64_t timeout, VkSemaphore, VkFence, std::uint32_t*, wsi_acquire::Call* = nullptr);
    VkResult acquire2(const VkAcquireNextImageInfoKHR*, std::uint32_t*, wsi_acquire::Call* = nullptr);
    VkResult status(VkSwapchainKHR);
    VkResult present(VkQueue, const VkPresentInfoKHR *);
    void holdOutput(bool hold) noexcept {holdOutput_=hold;}
    // Missing timing selects an actual untimed OFF output for this packet.
    // It does not stop timing admission or retire the runtime.
    VkResult presentUntimedOff(VkQueue, const VkPresentInfoKHR *);
    VkResult destroy(VkSwapchainKHR, const VkAllocationCallbacks *);
    VkResult stop();
    bool failed() const;
    bool lastPrivateCopyCompleted() const {
        return lastPrivateCopyCompleted_;
    }
    bool lastNativePresentCalled() const {
        return lastNativePresentCalled_;
    }
    bool lastSdkOutputSucceeded() const { return lastSdkOutputSucceeded_; }
    bool lastSdkOutputAttempted() const { return lastSdkOutputAttempted_; }
    bool lastUntimedTimingUnavailable() const { return lastUntimedTimingUnavailable_; }
    ShutdownState shutdownState() const { return shutdown_; }
    VkQueue queue() const { return queue_; }
    void stopAdmission() { if (backend_) backend_->stopAdmission(); }
    void quarantineShutdown() { fail(VK_ERROR_UNKNOWN, "shutdown_gpu_completion_unproven"); }
    CopyProgress lastCopyProgress() const { return lastCopyProgress_; }
    bool retainsAppSemaphore(VkSemaphore semaphore) const {
        return backend_ && backend_->retainsAppSemaphore(semaphore);
    }
    const char *stage() const {
        return stage_;
    }
    const char *error() const {
        return backend_ ? backend_->error() : (shutdown_.nativeTailRetained ? "Private cleanup complete; native WSI tail retained" : "");
    }
    std::uint64_t frames() const {
        return frames_;
    }
    std::size_t retainedChains() const;
    std::uint64_t retiredChainsDestroyed() const { return retiredChainsDestroyed_; }
    FrameStatus frameStatus() const {
        const auto admission = admitPresent();
        const auto backendStatus = backend_ && !shutdown_.privateCleanupCompleted ? backend_->frameStatus() : FrameStatus{};
        auto status = lastSdkOutputAttempted_ ? backendStatus : FrameStatus{};
        if(!lastSdkOutputSucceeded_)status.sdkStatusValid=false;
        if(!status.sdkResultKnown&&!lastSdkOutputSucceeded_)status.queryResult=status.frameGenResult=0;
        status.previousCopy = backendStatus.previousCopy;
        status.immediateCopy = backendStatus.immediateCopy;
        status.vkPrivateCopyGpu = backendStatus.vkPrivateCopyGpu;
        status.vkInteropCopyGpu = backendStatus.vkInteropCopyGpu;
        status.d3dCallerCopyTagGpu = backendStatus.d3dCallerCopyTagGpu;
        status.providerDepthGpu=backendStatus.providerDepthGpu;
        status.providerAuxGpu=backendStatus.providerAuxGpu;
        status.longFrameReset = backendStatus.longFrameReset;
        status.inflightCapWaitCpu = backendStatus.inflightCapWaitCpu;
        status.inflightFrames = backendStatus.inflightFrames;
        status.inflightHighWater = backendStatus.inflightHighWater;
        status.inflightLimit = backendStatus.inflightLimit;
        status.asyncCopyPath = backendStatus.asyncCopyPath;
        status.sharedCopyPath = backendStatus.sharedCopyPath;
        status.sharedColorDirect = backendStatus.sharedColorDirect;
        status.sharedAuxiliaryDirect = backendStatus.sharedAuxiliaryDirect;
        status.sharedPlaneKey = backendStatus.sharedPlaneKey;
        status.sharedPlanes = backendStatus.sharedPlanes;
        status.sharedFallbackReason = backendStatus.sharedFallbackReason;
        status.depthCompute = backendStatus.depthCompute;
        status.runtimeCopyCpu = copyCpu_;
        status.nativePresentCpu = nativePresentCpu_;
        status.outputCpu = outputCpu_;
        return status;
    }
    FrameTimingTicket beginGameFrame(std::uint32_t id = 0) {
        return backend_ ? backend_->beginGameFrame(id) : FrameTimingTicket{};
    }
    void simulationEnd(FrameTimingTicket ticket) {
        if (backend_) backend_->simulationEnd(ticket);
    }
    void renderSubmitBegin(FrameTimingTicket ticket) {
        if (backend_) backend_->renderSubmitBegin(ticket);
    }
    void renderSubmitEnd(FrameTimingTicket ticket) {
        if (backend_) backend_->renderSubmitEnd(ticket);
    }
    void abandonFrameTiming(FrameTimingTicket ticket) noexcept {
        if (backend_) backend_->abandonFrameTiming(ticket);
    }
    void associatePresentTiming(std::uint32_t engineFrame, FrameTimingTicket ticket,
                                std::uint32_t bootstrapId) {
        const auto admission = admitPresent();
        if (backend_) backend_->associatePresentTiming(engineFrame, ticket, bootstrapId);
    }
    std::uint64_t timingContextGeneration() const {
        return backend_ ? backend_->timingContextGeneration() : 0;
    }
    std::uint32_t nextSdkId() const {
        return backend_ ? backend_->nextSdkId() : 0;
    }

  private:
    mutable std::recursive_mutex presentAdmission_;
    struct Chain {
        std::vector<VkImage> images;
        bool retired{}, nativeRetired{}, managed{};
        bool nativePresentCalled{}, privateSubmitted{};
        std::uint64_t generation{};
        std::uint64_t replacementFrom{}, replacementThrough{};
        VkImage presentAnchor{};
        bool destroyRequested{}, presentCompleted{};
    };
    VkDevice device_{};
    VkSurfaceKHR surface_{};
    VkQueue queue_{};
    Dispatch dispatch_{};
    std::unique_ptr<Backend> owned_;
    Backend *backend_{};
    std::map<VkSwapchainKHR, Chain> chains_;
    VkSwapchainKHR current_{};
    bool failed_{}, shutdownBlocked_{}, cleanupProven_{true};
    const bool fake_{};
    struct FakeState;
    std::unique_ptr<FakeState> fakeState_;
    bool lastPrivateCopyCompleted_{}, lastNativePresentCalled_{}, lastSdkOutputSucceeded_{};
    bool lastSdkOutputAttempted_{};
    bool lastUntimedTimingUnavailable_{};
    CopyProgress lastCopyProgress_{CopyProgress::NotSubmitted};
    ShutdownState shutdown_;
    const char *stage_ = "ready";
    bool holdOutput_{};
    std::uint64_t frames_{}, chainGeneration_{}, retiredChainsDestroyed_{};
    CpuWallTime copyCpu_, nativePresentCpu_, outputCpu_;
    struct QueuedProof {
        std::uint64_t generation{}, retirementEpoch{};
        VkImage image{};
    } queuedProof_;
    std::uint64_t retirementEpoch_{};
    VkResult fail(VkResult, const char *);
    VkResult presentPacket(VkQueue, const VkPresentInfoKHR *);
    VkResult forward(VkQueue, const VkPresentInfoKHR *, const char *);
    void completeReplacementPresent(Chain &, VkImage);
    void destroyCompletedRequests();
    VkResult createFake(const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
    VkResult presentFake(VkQueue, const VkPresentInfoKHR*);
    VkResult destroyFake(VkSwapchainKHR, const VkAllocationCallbacks*);
};
bool enabled() noexcept;
bool gpuTimestampsRequested() noexcept;
// Owns only a private hidden HWND and its dedicated message thread.
class Surface {
  public:
    explicit Surface(HWND original);
    ~Surface();
    HWND original() const;
    HWND hidden() const;
    bool resize(VkExtent2D);
    VkSurfaceKHR handle{};

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Copies the successful creation records needed by the bridge before the
// original vkCreateDevice arguments cease to be valid.
class DeviceRecord {
  public:
    struct Impl;
    DeviceRecord(VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr,
                 PFN_vkGetDeviceProcAddr, std::uint32_t apiVersion, const VkDeviceCreateInfo &,
                 bool fake = false, QueueGate* queueGate = nullptr,
                 wsi_acquire::QueueObserver queueObserver = nullptr, void* queueObserverContext = nullptr);
    ~DeviceRecord();
    std::unique_ptr<Runtime> create(Surface &, VkQueue, continuous::Provider * = nullptr);
    std::uint32_t family() const;

  private:
    std::unique_ptr<Impl> impl_;
};
} // namespace xess_vk::presentation
