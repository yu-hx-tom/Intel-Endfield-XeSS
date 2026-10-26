#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <string>

namespace xess_vk::display_stats {
inline constexpr unsigned windowSeconds = 10, updateIntervalMs = 200;
inline constexpr std::size_t maximumChains = 32, maximumIntervals = 65536;
inline constexpr int dxgiRuntime = 1, intelXeFgFrameType = 50;

// The collector must supply actual display timestamps/duration in its QPC domain.
// CPU start differences, SDK frame counts and unshown presents are not substitutes.
struct DisplaySample {
    std::uint32_t pid{};
    std::uint64_t swapchain{};
    int runtime{}, frameType{};
    std::uint64_t displayStartQpc{}, displayEndQpc{};
    double displayedMs{};
    bool displayTimeValid{};
};
enum class State { NoData, Ambiguous, Warming, Ready, SourceError, Capacity };
struct Snapshot {
    State state{State::NoData};
    std::uint32_t pid{};
    std::uint64_t captureGeneration{}, swapchain{};
    std::size_t intervals{};
    unsigned windowSeconds{display_stats::windowSeconds};
    double observedSeconds{}, fps{}, latestMs{}, averageMs{}, maximumMs{};
    double integralOnePercentLow{};
};

// Single collector thread. update() may be called every 200 ms, including empty
// batches; no per-frame I/O, global hooks or RTSS access is performed here.
class DisplayStats {
public:
    bool begin(std::uint32_t pid, std::uint64_t captureGeneration, std::uint64_t qpcFrequency, unsigned seconds = windowSeconds);
    void sourceError();
    void update(std::span<const DisplaySample>, std::uint64_t nowQpc);
    Snapshot snapshot(std::uint64_t nowQpc) const;
private:
    struct Chain { std::uint64_t address{}, lastEnd{}, lastXeFgEnd{}; };
    struct Interval { std::uint64_t start{}, end{}; double ms{}; };
    std::uint32_t pid_{};
    std::uint64_t generation_{}, frequency_{}, windowTicks_{}, selected_{}, coverageStart_{}, lastNow_{};
    unsigned windowSeconds_{windowSeconds};
    State state_{State::NoData};
    std::array<Chain, maximumChains> chains_{};
    std::deque<Interval> intervals_;
    Interval previous_{};
    bool selectedXeFg_{};
    void resetSegment();
    bool valid(const DisplaySample&, std::uint64_t now) const;
    bool live(std::uint64_t timestamp, std::uint64_t now) const;
};
std::string formatOsd(const Snapshot&);
} // namespace xess_vk::display_stats
