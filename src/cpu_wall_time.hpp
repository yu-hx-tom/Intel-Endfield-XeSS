#pragma once
#include <chrono>
#include <cstdint>
#include "measurement_window.hpp"
#include "diagnostic_flags.hpp"

namespace xess_vk {
// Cumulative CPU wall time, including blocked time. Not GPU execution time.
// The caller provides serialization; XeLL updates use the owner's timing mutex.
struct CpuWallTime {
    std::uint64_t count{}, totalNs{}, maxNs{};
    void add(std::uint64_t ns) noexcept {
        ++count;
        totalNs += ns;
        if (ns > maxNs) maxNs = ns;
    }
};
using CpuWallClock = std::chrono::steady_clock;
inline std::uint64_t cpuWallNowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(CpuWallClock::now().time_since_epoch()).count());
}
inline std::uint64_t cpuWallElapsedNs(CpuWallClock::time_point start) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(CpuWallClock::now() - start).count());
}
class CpuWallScope {
    CpuWallTime& target_;
    CpuWallClock::time_point start_{};
    bool enabled_{diagnostic_flags::runtimeDiagnostics()};
    measurement::Metric metric_{measurement::Metric::Count};
public:
    explicit CpuWallScope(CpuWallTime& target,measurement::Metric metric=measurement::Metric::Count) noexcept : target_(target),metric_(metric) {if(enabled_)start_=CpuWallClock::now();}
    ~CpuWallScope() {
        if(!enabled_)return;
        const auto ns=cpuWallElapsedNs(start_);target_.add(ns);
        if(metric_!=measurement::Metric::Count)measurement::observe(metric_,ns);
    }
    CpuWallScope(const CpuWallScope&) = delete;
    CpuWallScope& operator=(const CpuWallScope&) = delete;
};
} // namespace xess_vk
