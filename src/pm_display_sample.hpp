#pragma once
#include "rtss_display_stats.hpp"

namespace xess_vk::display_stats {
enum class DisplayConversion { Displayed, NotDisplayed, Unavailable, Invalid };

// PresentMon v2.5.1 emits one row per displayIndex, including generated frames.
// DisplayLatency uses the same CalculateCPUStart as CPUStartQPC; DisplayedTime
// is nextScreenTime - screenTime. Neither value is a CPU present interval.
DisplayConversion convertPresentMonDisplay(
    DisplaySample& result, std::uint32_t pid, std::uint64_t swapchain,
    int runtime, int frameType, std::uint64_t cpuStartQpc,
    double displayLatencyMs, double displayedMs, std::uint64_t qpcFrequency) noexcept;
} // namespace xess_vk::display_stats
