#pragma once
#include <cstdint>
#include <cmath>

namespace xess_vk {
// CPU-observed query data. Calibration is optional; absence never invents a QPC/GPU mapping.
struct GpuTimestampCalibration {
    bool available{}, maxDeviationKnown{};
    std::uint64_t gpuTicks{}, qpcTicks{}, qpcFrequency{}, captureQpcTicks{}, maxDeviationNs{};
};
struct GpuTimestampSample {
    bool requested{}, available{}, durationKnown{};
    bool queryStateKnown{}, diagnosticActive{}, queryReadAttempted{}, queryResultKnown{}, queryAvailabilityKnown{};
    std::int32_t queryResult{};
    std::uint64_t beginAvailability{}, endAvailability{};
    std::uint64_t sequence{}, contextGeneration{}, sourceCopyToken{}, generation{};
    bool sourceCopyTokenKnown{}, sourceEngineFrameKnown{}, sdkPresentIdKnown{};
    std::uint32_t sourceEngineFrame{}, sdkPresentId{}, validBits{};
    std::uint64_t beginTicks{}, endTicks{}, frequencyHz{}, fenceObservedQpcTicks{};
    std::uint64_t submitQpcTicks{}, qpcFrequency{};
    double timestampPeriodNs{}, durationNs{};
    GpuTimestampCalibration calibration;
};
inline bool timestampDuration(GpuTimestampSample& sample) noexcept {
    if (!sample.available || !sample.validBits || sample.validBits > 64 || !(sample.timestampPeriodNs > 0) ||
        !std::isfinite(sample.timestampPeriodNs))
        return sample.durationKnown = false;
    if (sample.validBits < 64 && sample.qpcFrequency && sample.fenceObservedQpcTicks >= sample.submitQpcTicks &&
        static_cast<double>(sample.fenceObservedQpcTicks - sample.submitQpcTicks) * 1e9 / sample.qpcFrequency >=
            std::ldexp(sample.timestampPeriodNs, static_cast<int>(sample.validBits)))
        return sample.durationKnown = false; // More than one counter wrap cannot be reconstructed.
    const auto mask = sample.validBits == 64 ? UINT64_MAX : (std::uint64_t{1} << sample.validBits) - 1;
    sample.durationNs = static_cast<double>((sample.endTicks - sample.beginTicks) & mask) * sample.timestampPeriodNs;
    return sample.durationKnown = std::isfinite(sample.durationNs);
}
inline bool timestampQpcUs(const GpuTimestampSample& sample, std::uint64_t ticks, double& qpcUs) noexcept {
    const auto& c = sample.calibration;
    if (!sample.available || !sample.durationKnown || !c.available || !c.qpcFrequency || !sample.validBits || sample.validBits > 64 ||
        !(sample.timestampPeriodNs > 0) || !std::isfinite(sample.timestampPeriodNs)) return false;
    const auto wrap = std::ldexp(1.L, static_cast<int>(sample.validBits));
    const auto ageTicks = sample.fenceObservedQpcTicks >= c.captureQpcTicks ?
        sample.fenceObservedQpcTicks - c.captureQpcTicks : c.captureQpcTicks - sample.fenceObservedQpcTicks;
    if (static_cast<long double>(ageTicks) * 1e9L / c.qpcFrequency >= wrap * sample.timestampPeriodNs / 2) return false;
    if (sample.qpcFrequency && sample.fenceObservedQpcTicks >= sample.submitQpcTicks &&
        static_cast<long double>(sample.fenceObservedQpcTicks - sample.submitQpcTicks) * 1e9L / sample.qpcFrequency >=
            wrap * sample.timestampPeriodNs / 2) return false;
    const auto mask = sample.validBits == 64 ? UINT64_MAX : (std::uint64_t{1} << sample.validBits) - 1;
    const auto rawDelta = (ticks - c.gpuTicks) & mask;
    const auto delta = rawDelta > mask / 2 ? -static_cast<long double>(mask - rawDelta + 1) : static_cast<long double>(rawDelta);
    qpcUs = static_cast<double>(static_cast<long double>(c.qpcTicks) * 1e6L / c.qpcFrequency + delta * sample.timestampPeriodNs / 1000);
    return std::isfinite(qpcUs) && qpcUs >= 0;
}
} // namespace xess_vk
