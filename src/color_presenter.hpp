#pragma once
#include "interop.hpp"
#include "upscale_frame_constants.hpp"
#include "frame_timing.hpp"
#include "presenter_frame_timing.hpp"
#include "gpu_timestamp_sample.hpp"
#include "fg_tuning.hpp"
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <windows.h>
namespace xess_vk {
struct ColorPresentStatus {
    fg_tuning::Applied fgTuning;
    fg_tuning::Totals fgOutcomes;
    CpuWallTime bridgePublishCpu, previousD3DWaitCpu, dxgiPresentCpu, xellSleepCpu;
    GpuTimestampSample d3dCallerCopyTagGpu;
    std::uint32_t sdkPresentId{};
    bool untimedOff{};
    bool latencyTransitionAdmitted{};
    const char* onBlockedReason{};
    std::uint32_t presentId{}, framesPresented{};
    // Generated frames per application frame applied at proxy initialization.
    std::uint32_t interpolatedFrames{};
    std::uint32_t multiplierRequested{},multiplierMaximum{};
    std::int32_t frameGenResult{}, queryResult{};
    bool requestedEnabled{}, isFrameGenEnabled{}, valid{}, occluded{};
    bool queryAttempted{}; // This presentation obtained an SDK query return, including failure.
    std::uint32_t sleepFrameId{}, sleepCalls{}, presentMarkerCalls{}, consecutiveEnabledPresents{};
    std::int32_t sleepResult{}, presentStartResult{}, presentEndResult{};
    bool partialXeLLTiming{};
    std::uint32_t totalMarkerCalls{};
    std::int32_t simulationStartResult{}, simulationEndResult{}, renderStartResult{}, renderEndResult{};
    bool completeFrameTiming{};
    const char* timingPolicy{"borrowed-off-no-xell-timing"};
};
class ColorPresenterRetained : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
class ColorPresenterUnavailable : public std::logic_error {
  public:
    using std::logic_error::logic_error;
};
// Borrows the application's HWND. Never pumps messages or destroys that window.
// Native inputs and constants come from the same completed producer. The color
// entry remains OFF; continuous bundles can enable frame generation explicitly.
class ColorPresenter {
  public:
    // initiallyQuiescent admits LL1 only for a fresh unpublished owner, after
    // the sole Vulkan queue completed and before any bridge D3D12 submission.
    // Diagnostic fixtures may request the maximum; the game passes its startup selection.
    static constexpr std::uint32_t useMaxSupportedInterpolatedFrames = 0xFFFFFFFFu;
    ColorPresenter(InteropBridge &, const std::filesystem::path &sdkBin, HWND,
                   bool vsync = true, bool continuous = false, bool initiallyQuiescent = false,
                   std::uint32_t interpolatedFrames = 1, bool gpuTimestamps = false);
    ~ColorPresenter();
    void present(const NativeImageSource &, VkQueue, VkFence);
    FrameTimingTicket beginGameFrame(std::uint32_t mappedSdkId = 0);
    void simulationEnd(FrameTimingTicket);
    void renderSubmitBegin(FrameTimingTicket);
    void renderSubmitEnd(FrameTimingTicket);
    void present(const NativeImageSource &, VkQueue, VkFence, FrameTimingTicket, std::uint32_t bootstrapSdkId = 0);
    // Same queued-source lifetime as InteropBridge::publishQueuedNativeColor.
    // Continuous owners require their real ticket; standalone OFF keeps its IDs.
    void presentQueuedOff(const NativeImageSource &, VkQueue, VkFence, FrameTimingTicket = {});
    void presentQueuedOff(const NativeImageSource &, VkQueue, VkFence, FrameTimingTicket,
                          FrameSlotKey, FrameCompletion&);
    void presentSharedQueuedOff(SharedPlaneKey, VkQueue, VkFence, FrameTimingTicket,
                                FrameSlotKey, FrameCompletion&);
    void presentBootstrap(const NativeImageSource &, VkQueue, VkFence, std::uint32_t mappedSdkId);
    // Uses the same owner ID sequence, FG disabled, and no invented markers.
    std::uint32_t presentUntimedOff(const NativeImageSource &, VkQueue, VkFence);
    void abandonFrameTiming(FrameTimingTicket) noexcept;
    detail::PresenterTimingStats timingStats() const;
    void setVsync(bool);
    // Keep the SDK context and timing owner while resizing their output.
    void resizeOutput(std::uint32_t width, std::uint32_t height);
    void presentBundle(const NativeFrame &, const frame_input::FrameConstantsSample &,
                       bool enabled, bool resetHistory);
    void presentBundle(const NativeFrame &, const frame_input::FrameConstantsSample &,
                       bool enabled, bool resetHistory, FrameTimingTicket);
    // Explicit rectangle is the caller's same-frame scene region in the final
    // output, not the raw reconstruction size. Empty retains the exact-size path.
    void presentBundle(const NativeFrame &, const frame_input::FrameConstantsSample &,
                       bool enabled, bool resetHistory, FrameTimingTicket, VkRect2D outputInterpolationRect);
    // ON only, with a real latency-enabled continuous ticket and the retained
    // three-plane source/lease lifetime of publishQueuedNativeFrame. Exceptions
    // after entering Bridge publication do not prove that nothing was submitted.
    void presentBundleQueued(const NativeFrame &, const frame_input::FrameConstantsSample &,
                             bool resetHistory, FrameTimingTicket, VkRect2D outputInterpolationRect);
    void presentBundleQueued(const NativeFrame &, const frame_input::FrameConstantsSample &,
                             bool resetHistory, FrameTimingTicket, VkRect2D, FrameSlotKey, FrameCompletion&);
    void presentSharedBundleQueued(SharedPlaneKey, VkQueue, VkFence,
                                   const frame_input::FrameConstantsSample&, bool resetHistory,
                                   FrameTimingTicket, VkRect2D, FrameSlotKey, FrameCompletion&);
    // Receipt-specific polling never waits or substitutes the latest frame.
    FrameCompletionState frameComplete(const FrameCompletion&) const noexcept;
    void waitFrame(const FrameCompletion&);
    // Quarantine: refuse new calls, retain pending calls and every SDK owner.
    // Normal shutdown uses shutdown() directly, without entering quarantine.
    void requestStop();
    // Close admission normally, preserving the healthy shutdown contract.
    void stopAdmission();
    void shutdown();
    std::uint64_t frames() const;
    std::uint64_t timingContextGeneration() const;
    std::uint32_t nextSdkId() const;
    ColorPresentStatus lastPresentStatus() const;

  private:
    void presentShared(SharedPlaneKey, VkQueue, VkFence, const frame_input::FrameConstantsSample*,
                       bool resetHistory, FrameTimingTicket, VkRect2D, FrameSlotKey, FrameCompletion&);
    void presentBundleImpl(const NativeFrame &, const frame_input::FrameConstantsSample &,
                           bool enabled, bool resetHistory, FrameTimingTicket, VkRect2D, bool queued,
                           FrameCompletion* = nullptr);
    void presentColor(const NativeImageSource &, VkQueue, VkFence, FrameTimingTicket,
                      std::uint32_t bootstrapSdkId, bool queued, FrameCompletion* = nullptr);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xess_vk
