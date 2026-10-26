#include "rtss_display_stats.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace xess_vk::display_stats {
bool DisplayStats::begin(std::uint32_t pid, std::uint64_t generation, std::uint64_t frequency, unsigned seconds) {
    *this = {};
    if (!pid || !generation || !frequency || !seconds || seconds > 60 || frequency > UINT64_MAX / seconds) {
        state_ = State::SourceError;
        return false;
    }
    pid_ = pid; generation_ = generation; frequency_ = frequency;
    windowSeconds_ = seconds;
    windowTicks_ = frequency * seconds;
    return true;
}
void DisplayStats::sourceError() {
    resetSegment(); chains_ = {}; selected_ = 0;
    previous_ = {}; selectedXeFg_ = false;
    state_ = State::SourceError;
}
void DisplayStats::resetSegment() {
    intervals_.clear(); previous_ = {}; coverageStart_ = 0;
    state_ = State::Warming;
}
bool DisplayStats::live(std::uint64_t timestamp, std::uint64_t now) const {
    return timestamp && timestamp <= now && now - timestamp < windowTicks_;
}
bool DisplayStats::valid(const DisplaySample& sample, std::uint64_t now) const {
    return sample.pid == pid_ && sample.swapchain && sample.runtime == dxgiRuntime &&
        sample.displayTimeValid && sample.displayStartQpc && sample.displayEndQpc > sample.displayStartQpc &&
        live(sample.displayEndQpc, now) && std::isfinite(sample.displayedMs) && sample.displayedMs > 0;
}
void DisplayStats::update(std::span<const DisplaySample> samples, std::uint64_t now) {
    if (!frequency_ || state_ == State::SourceError || state_ == State::Capacity) return;
    if (now < lastNow_) { sourceError(); return; }
    if (samples.size() > maximumIntervals) { intervals_.clear(); state_ = State::Capacity; return; }
    lastNow_ = now;
    for (auto& chain : chains_) if (!live(chain.lastEnd, now)) chain = {};
    for (const auto& sample : samples) {
        if (sample.pid == pid_ && sample.runtime == dxgiRuntime && sample.displayTimeValid &&
            (!sample.swapchain || !sample.displayStartQpc || sample.displayEndQpc <= sample.displayStartQpc ||
             sample.displayEndQpc > now || !std::isfinite(sample.displayedMs) || sample.displayedMs <= 0)) {
            resetSegment(); return;
        }
        if (!valid(sample, now)) continue;
        const long double durationTicks = static_cast<long double>(sample.displayedMs) * frequency_ / 1000;
        if (std::fabs(durationTicks - (sample.displayEndQpc - sample.displayStartQpc)) > 2) {
            resetSegment(); return;
        }
        auto chain = std::find_if(chains_.begin(), chains_.end(), [&](const Chain& c) { return c.address == sample.swapchain; });
        if (chain == chains_.end())
            chain = std::find_if(chains_.begin(), chains_.end(), [](const Chain& c) { return !c.address; });
        if (chain == chains_.end()) { intervals_.clear(); state_ = State::Capacity; return; }
        chain->address = sample.swapchain;
        chain->lastEnd = std::max(chain->lastEnd, sample.displayEndQpc);
        if (sample.frameType == intelXeFgFrameType)
            chain->lastXeFgEnd = std::max(chain->lastXeFgEnd, sample.displayEndQpc);
    }
    std::uint64_t unique{}, uniqueXeFg{};
    unsigned count{}, xeFgCount{};
    for (const auto& chain : chains_) if (live(chain.lastEnd, now)) {
        ++count; unique = chain.address;
        if (live(chain.lastXeFgEnd, now)) { ++xeFgCount; uniqueXeFg = chain.address; }
    }
    // A confirmed XeFG output remains selected through OFF. A quiet output is
    // retained too: its next completed interval can contain a real long stall.
    const auto chosen = xeFgCount == 1 ? uniqueXeFg : xeFgCount > 1 ? 0 :
        selected_ && (!count || selectedXeFg_) ? selected_ : count == 1 ? unique : 0;
    if (!chosen) {
        state_ = count ? State::Ambiguous : State::NoData;
        if (xeFgCount > 1 || !selectedXeFg_) {
            intervals_.clear(); selected_ = coverageStart_ = 0;
            previous_ = {}; selectedXeFg_ = false;
        }
        return;
    }
    if (chosen != selected_) {
        resetSegment(); selected_ = chosen; selectedXeFg_ = false;
    }
    selectedXeFg_ |= xeFgCount == 1;
    while (!intervals_.empty() && !live(intervals_.front().end, now)) intervals_.pop_front();
    for (const auto& sample : samples) {
        if (!valid(sample, now) || sample.swapchain != chosen) continue;
        const Interval interval{sample.displayStartQpc, sample.displayEndQpc, sample.displayedMs};
        if (previous_.end) {
            if (interval.start == previous_.start && interval.end == previous_.end && interval.ms == previous_.ms) continue;
            const auto difference = interval.start > previous_.end ? interval.start - previous_.end : previous_.end - interval.start;
            // Two rounded QPC conversions can disagree by about two ticks.
            if (interval.end <= previous_.end) { resetSegment(); return; }
            if (difference > 2) resetSegment();
        }
        if (intervals_.size() == maximumIntervals) { intervals_.clear(); state_ = State::Capacity; return; }
        intervals_.push_back(interval);
        if (!coverageStart_) coverageStart_ = interval.start;
        previous_ = interval;
    }
    state_ = previous_.end && previous_.end - coverageStart_ >= windowTicks_ ? State::Ready : State::Warming;
}
Snapshot DisplayStats::snapshot(std::uint64_t now) const {
    Snapshot result;
    result.windowSeconds = windowSeconds_;
    result.state = state_; result.pid = pid_; result.captureGeneration = generation_; result.swapchain = selected_;
    if (!frequency_ || now < lastNow_ || !selected_) return result;
    if (state_ != State::Ready && state_ != State::Warming) return result;
    if (previous_.end) result.observedSeconds = static_cast<double>(previous_.end - coverageStart_) / frequency_;
    if (!previous_.end) return result;
    if (now - previous_.end > 2 * frequency_) { result.state = State::NoData; return result; }
    std::vector<double> sorted;
    long double total{};
    for (const auto& interval : intervals_) if (live(interval.end, now)) {
        sorted.push_back(interval.ms); total += interval.ms; result.latestMs = interval.ms;
    }
    if (sorted.empty()) { result.state = State::NoData; return result; }
    result.intervals = sorted.size();
    if (result.observedSeconds < windowSeconds_) { result.state = State::Warming; return result; }
    if (result.state != State::Ready) return result;
    std::sort(sorted.begin(), sorted.end(), std::greater<double>{});
    result.maximumMs = sorted.front();
    result.averageMs = static_cast<double>(total / sorted.size());
    result.fps = 1000.0 / result.averageMs;
    const auto integralLow = [&] {
        const auto threshold = total / 100;
        long double sum{};
        for (double ms : sorted) {
            // RTSS SDK FrametimeStats.cpp: the boundary crosses strictly >,
            // not >=; use the boundary frame, never average the slowest frames.
            if (sum + ms > threshold) return 1000.0 / ms;
            sum += ms;
        }
        return 1000.0 / sorted.back();
    };
    result.integralOnePercentLow = integralLow();
    return result;
}
std::string formatOsd(const Snapshot& snapshot) {
    const auto window = std::to_string(snapshot.windowSeconds) + "s";
    if (snapshot.state != State::Ready) {
        const auto warming = "warming " + window;
        const char* reason = snapshot.state == State::Warming ? warming.c_str() :
            snapshot.state == State::Ambiguous ? "ambiguous display chain" :
            snapshot.state == State::SourceError ? "source unavailable" :
            snapshot.state == State::Capacity ? "capacity exceeded" : "no displayed data";
        return "Display FT(last): N/A\nDisplay 1% Low(Integral," + window + "): N/A (" + reason + ")";
    }
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << "Display FT(last): " << snapshot.latestMs
         << " ms\nDisplay 1% Low(Integral," << window << "): " << snapshot.integralOnePercentLow << " FPS";
    return text.str();
}
} // namespace xess_vk::display_stats
