#pragma once
#include "gpu_timestamp_sample.hpp"
#include "measurement_window.hpp"

namespace xess_vk::detail {
// One query pair belongs to the caller list and may be reused only after its existing completion proof.
class CallerGpuTimestamps {
    bool enabled_{}, recording_{}, staged_{}, pending_{};
    std::uint64_t sequence_{}, generation_{1};
    GpuTimestampSample latest_, recordingSample_;
public:
    template<class Resources> void initialize(bool requested, Resources& resources) noexcept {
        latest_.requested = requested;
        enabled_ = requested && resources.initialize();
    }
    template<class Resources> void begin(Resources& resources, std::uint32_t presentId,
                                        std::uint64_t sequence = 0, std::uint64_t generation = 0,std::uint64_t context = 0) noexcept {
        if (!enabled_ || pending_) return;
        if (sequence && sequence <= sequence_) { enabled_ = false; return; }
        if (sequence_ == UINT64_MAX) { enabled_ = false; return; }
        recordingSample_ = {};
        recordingSample_.requested = true;
        sequence_ = sequence ? sequence : sequence_ + 1;
        recordingSample_.sequence = sequence_;
        recordingSample_.generation = generation ? generation : generation_;
        recordingSample_.contextGeneration=context;
        recordingSample_.sdkPresentIdKnown = true;
        recordingSample_.sdkPresentId = presentId;
        recordingSample_.validBits = 64;
        recordingSample_.frequencyHz = resources.frequencyHz;
        recordingSample_.timestampPeriodNs = 1e9 / static_cast<double>(resources.frequencyHz);
        recordingSample_.qpcFrequency = resources.qpcFrequency;
        recordingSample_.calibration = resources.calibrate();
        resources.begin();
        recording_ = true;
        staged_ = false;
    }
    template<class Resources> void end(Resources& resources) noexcept {
        if (!recording_) return;
        resources.endAndResolve();
        recording_ = false;
        staged_ = true;
    }
    template<class Resources> void submitted(Resources& resources) noexcept {
        if (!staged_) return;
        recordingSample_.submitQpcTicks = resources.qpcNow();
        staged_ = false;
        pending_ = true;
    }
    // The caller invokes this only after an already-required caller/full-tail fence wait succeeds.
    template<class Resources> void completed(Resources& resources) noexcept {
        if (!pending_) return;
        pending_ = false;
        recordingSample_.fenceObservedQpcTicks = resources.qpcNow();
        recordingSample_.available = resources.read(recordingSample_.beginTicks, recordingSample_.endTicks);
        timestampDuration(recordingSample_);
        if(recordingSample_.durationKnown)measurement::observe(measurement::Metric::CallerGpu,
            static_cast<std::uint64_t>(recordingSample_.durationNs),recordingSample_.sdkPresentId,recordingSample_.contextGeneration);
        latest_ = recordingSample_;
        if (!latest_.available) enabled_ = false;
    }
    void rebuilt() noexcept {
        if (pending_ || generation_ == UINT64_MAX) { enabled_ = false; return; }
        ++generation_; recording_ = staged_ = false;
    }
    const GpuTimestampSample& latest() const noexcept { return latest_; }
};
} // namespace xess_vk::detail
