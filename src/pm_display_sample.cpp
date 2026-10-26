#include "pm_display_sample.hpp"
#include <cmath>
#include <limits>

namespace xess_vk::display_stats {
namespace {
bool ticks(double ms, std::uint64_t frequency, std::uint64_t& result) noexcept {
    const long double rounded = std::floor(static_cast<long double>(ms) * frequency / 1000.0L + 0.5L);
    if (!std::isfinite(rounded) || rounded < 0 ||
        rounded >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) return false;
    result = static_cast<std::uint64_t>(rounded);
    return true;
}
}
DisplayConversion convertPresentMonDisplay(
    DisplaySample& result, std::uint32_t pid, std::uint64_t swapchain,
    int runtime, int frameType, std::uint64_t cpuStartQpc,
    double displayLatencyMs, double displayedMs, std::uint64_t frequency) noexcept {
    result = {pid, swapchain, runtime, frameType, 0, 0, displayedMs, false};
    if (!pid || !swapchain || !frequency || std::isinf(displayLatencyMs) ||
        std::isinf(displayedMs) || displayLatencyMs < 0 || displayedMs < 0)
        return DisplayConversion::Invalid;
    // FrameEventQuery encodes dropped/zero-duration display metrics as NaN.
    if (std::isnan(displayedMs) || displayedMs == 0) return DisplayConversion::NotDisplayed;
    // The first completed display can lack a previous CPU origin. Keep it raw,
    // then accept later locatable displays without poisoning the whole session.
    if (!cpuStartQpc || std::isnan(displayLatencyMs)) return DisplayConversion::Unavailable;
    std::uint64_t delay = 0, duration = 0;
    if (!ticks(displayLatencyMs, frequency, delay) || !ticks(displayedMs, frequency, duration) || !duration ||
        delay > std::numeric_limits<std::uint64_t>::max() - cpuStartQpc)
        return DisplayConversion::Invalid;
    const auto start = cpuStartQpc + delay;
    if (duration > std::numeric_limits<std::uint64_t>::max() - start) return DisplayConversion::Invalid;
    result.displayStartQpc = start;
    result.displayEndQpc = start + duration;
    result.displayTimeValid = true;
    return DisplayConversion::Displayed;
}
} // namespace xess_vk::display_stats
