#include "rtss_display_stats.hpp"
#include "pm_display_sample.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace ds = xess_vk::display_stats;
namespace {
unsigned checks{};
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
bool close(double a, double b) { return std::abs(a - b) < 0.000001; }
struct Fixture {
    ds::DisplayStats stats;
    std::uint64_t now{10000};
    Fixture() { require(stats.begin(123, 1, 10000000), "valid capture identity"); }
    ds::DisplaySample next(double ms, int type = 50, std::uint64_t chain = 10) {
        const auto start = now;
        now += static_cast<std::uint64_t>(std::llround(ms * 10000));
        return {123, chain, ds::dxgiRuntime, type, start, now, ms, true};
    }
    void add(double ms, int type = 50) { const auto s = next(ms, type); stats.update({&s, 1}, now); }
    void warm() { add(1); for (unsigned n = 0; n < 1000; ++n) add(10); }
};
void ordinaryAndIntegral() {
    Fixture f;
    require(f.stats.snapshot(f.now).state == ds::State::NoData, "no source gives N/A");
    f.add(1);
    require(f.stats.snapshot(f.now).state == ds::State::Warming, "first displayed frame requires observation warmup");
    for (unsigned n = 0; n < 1000; ++n) f.add(10);
    auto s = f.stats.snapshot(f.now);
    require(s.state == ds::State::Ready && s.intervals == 1000, "ten-second completed-interval rolling window");
    require(close(s.fps, 100) && close(s.latestMs, 10) && close(s.integralOnePercentLow, 100), "regular display cadence");
    const auto text = ds::formatOsd(s);
    require(text.find("FT(last)") != text.npos && text.find("1% Low(Integral,10s)") != text.npos &&
        text.find("0.1%") == text.npos, "two explicit OSD meanings without an extra metric");
    f.add(1169.6939);
    s = f.stats.snapshot(f.now);
    require(s.state == ds::State::Ready && close(s.maximumMs, 1169.6939) &&
        close(s.integralOnePercentLow, 1000.0 / 1169.6939), "real 1.17-second stall remains in Integral Low");
    Fixture boundary;
    boundary.add(1); boundary.add(100);
    for (unsigned n = 0; n < 100; ++n) boundary.add(99);
    s = boundary.stats.snapshot(boundary.now);
    require(s.state == ds::State::Ready && close(s.integralOnePercentLow, 1000.0 / 99),
        "exact one-percent time equality advances to the next boundary frame");
}
void droppedAndLongFreeze() {
    Fixture f; f.warm();
    ds::DisplaySample dropped{123, 10, 1, 0, 0, 0, std::numeric_limits<double>::quiet_NaN(), false};
    f.stats.update({&dropped, 1}, f.now + 2000000);
    require(f.stats.snapshot(f.now + 2000000).state == ds::State::Ready, "normal undisplayed present does not clear history");
    f.stats.update({}, f.now + 110000000);
    auto s = f.stats.snapshot(f.now + 110000000);
    require(s.state == ds::State::NoData && s.swapchain == 10 && ds::formatOsd(s).find("N/A") != std::string::npos,
        "expired display data gives N/A but retains the selected chain");
    const auto auxiliaryEnd = f.now + 120000000;
    const ds::DisplaySample auxiliary{123, 20, 1, 0, auxiliaryEnd - 100000, auxiliaryEnd, 10, true};
    f.stats.update({&auxiliary, 1}, auxiliaryEnd);
    s = f.stats.snapshot(auxiliaryEnd);
    require(s.state == ds::State::NoData && s.swapchain == 10,
        "a live auxiliary DXGI chain cannot replace a stalled confirmed XeFG output");
    f.add(20000);
    s = f.stats.snapshot(f.now);
    require(s.state == ds::State::Ready && s.intervals == 1 && close(s.maximumMs, 20000) &&
        close(s.integralOnePercentLow, 0.05), "completed long freeze survives the window untrimmed without another warmup");
}
void selectionAndRestart() {
    Fixture f; f.warm();
    for (unsigned n = 0; n < 1100; ++n) {
        const auto main = f.next(10, 0);
        const ds::DisplaySample auxiliary{123, 20, 1, 0, main.displayStartQpc, main.displayEndQpc, 10, true};
        const std::array batch{main, auxiliary}; f.stats.update(batch, f.now);
    }
    auto s = f.stats.snapshot(f.now);
    require(s.state == ds::State::Ready && s.swapchain == 10 && s.observedSeconds > 20,
        "confirmed XeFG output remains selected through OFF and a secondary DXGI chain");
    Fixture multiple; multiple.warm();
    const ds::DisplaySample second{123, 20, 1, 50, multiple.now - 100000, multiple.now, 10, true};
    multiple.stats.update({&second, 1}, multiple.now);
    require(multiple.stats.snapshot(multiple.now).state == ds::State::Ambiguous, "two active XeFG outputs cannot be guessed");
    multiple.now += 110000000;
    const auto replacement = multiple.next(10, 50, 20);
    multiple.stats.update({&replacement, 1}, multiple.now);
    s = multiple.stats.snapshot(multiple.now);
    require(s.state == ds::State::Warming && s.swapchain == 20, "new unique output starts fresh warmup");
    require(multiple.stats.begin(124, 2, 10000000), "new PID/capture can restart");
    s = multiple.stats.snapshot(multiple.now);
    require(s.state == ds::State::NoData && !s.intervals && s.pid == 124 && s.captureGeneration == 2,
        "capture restart carries no old Low or chain");
    Fixture off;
    const auto a = off.next(10, 0);
    const ds::DisplaySample b{123, 20, 1, 0, a.displayStartQpc, a.displayEndQpc, 10, true};
    const std::array batch{a, b}; off.stats.update(batch, off.now);
    require(off.stats.snapshot(off.now).state == ds::State::Ambiguous, "unclassified multiple DXGI outputs give N/A");
    Fixture uniqueOff; uniqueOff.add(1, 0);
    for (unsigned n = 0; n < 1000; ++n) uniqueOff.add(10, 0);
    require(uniqueOff.stats.snapshot(uniqueOff.now).state == ds::State::Ready, "one genuine DXGI output needs no generated-frame label");
}
void invalidSource() {
    Fixture f; f.warm();
    auto bad = f.next(10); bad.displayedMs = std::numeric_limits<double>::quiet_NaN();
    f.stats.update({&bad, 1}, f.now);
    require(f.stats.snapshot(f.now).state == ds::State::Warming, "claimed valid NaN invalidates the segment, never becomes zero FPS");
    Fixture order; order.warm();
    const auto first = order.next(10); const auto second = order.next(10);
    const std::array backwards{second, first}; order.stats.update(backwards, order.now);
    require(order.stats.snapshot(order.now).state == ds::State::Warming, "sorting must not hide original same-chain disorder");
    Fixture gap; gap.warm();
    auto missing = gap.next(10); missing.displayStartQpc += 3;
    gap.stats.update({&missing, 1}, gap.now);
    require(gap.stats.snapshot(gap.now).state == ds::State::Warming, "unexplained display gap restarts coverage instead of inventing an interval");
    Fixture rounding; rounding.warm();
    auto rounded = rounding.next(10); rounded.displayStartQpc += 2;
    rounding.stats.update({&rounded, 1}, rounding.now);
    require(rounding.stats.snapshot(rounding.now).state == ds::State::Ready, "two-tick reconstruction rounding remains valid");
    Fixture duplicate; duplicate.warm();
    const auto same = duplicate.next(10); duplicate.stats.update({&same, 1}, duplicate.now);
    duplicate.stats.update({&same, 1}, duplicate.now);
    require(duplicate.stats.snapshot(duplicate.now).state == ds::State::Ready, "identical repeated observation does not double count");
    auto conflict = same; conflict.displayedMs = 11;
    duplicate.stats.update({&conflict, 1}, duplicate.now);
    require(duplicate.stats.snapshot(duplicate.now).state == ds::State::Warming, "same endpoint with conflicting duration is refused");
    Fixture overflow;
    std::vector<ds::DisplaySample> tooMany(ds::maximumIntervals + 1);
    overflow.stats.update(tooMany, overflow.now);
    require(overflow.stats.snapshot(overflow.now).state == ds::State::Capacity, "bounded capacity refuses instead of silently shortening the window");
}
std::vector<std::string> csvFields(const std::string& line) {
    if (line.find('"') != line.npos) throw std::runtime_error("Reference replay accepts unquoted PresentMon CSV fields only");
    std::vector<std::string> fields;
    std::istringstream stream(line);
    std::string value;
    while (std::getline(stream, value, ',')) fields.push_back(value);
    return fields;
}
void replayCsv(const char* path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Reference CSV unavailable");
    std::string line; std::getline(input, line);
    const auto header = csvFields(line);
    const auto column = [&](const char* name) {
        const auto found = std::find(header.begin(), header.end(), name);
        if (found == header.end()) throw std::runtime_error("Missing reference CSV field");
        return static_cast<std::size_t>(found - header.begin());
    };
    const auto pid = column("ProcessID"), chain = column("SwapChainAddress"), runtime = column("PresentRuntime"),
        type = column("FrameType"), cpu = column("CPUStartQPCTime"), latency = column("DisplayLatency"), time = column("DisplayedTime");
    const auto number = [](const std::string& text) { return text == "NA" ? std::numeric_limits<double>::quiet_NaN() : std::stod(text); };
    std::vector<ds::DisplaySample> samples;
    while (std::getline(input, line)) {
        const auto fields = csvFields(line);
        if (fields.size() != header.size()) throw std::runtime_error("Malformed reference CSV row");
        ds::DisplaySample sample;
        // These historical CSVs use CPUStartQPCTime in milliseconds, not ticks.
        const auto cpuQpc = static_cast<std::uint64_t>(std::llround(number(fields[cpu]) * 10000));
        const auto result = ds::convertPresentMonDisplay(sample, static_cast<std::uint32_t>(std::stoul(fields[pid])),
            std::stoull(fields[chain], nullptr, 0), fields[runtime] == "DXGI" ? 1 : 0,
            fields[type] == "Intel XeSS-FG" ? 50 : 0, cpuQpc, number(fields[latency]), number(fields[time]), 10000000);
        if (result == ds::DisplayConversion::Invalid) throw std::runtime_error("Invalid reference display source");
        if (result == ds::DisplayConversion::Displayed) samples.push_back(sample);
    }
    require(!samples.empty(), "historical reference contains real displayed intervals");
    // CSV row order is CPU/event order; replay the actual reconstructed display chronology.
    std::stable_sort(samples.begin(), samples.end(), [](const auto& a, const auto& b) { return a.displayEndQpc < b.displayEndQpc; });
    ds::DisplayStats stats; require(stats.begin(samples.front().pid, 1, 10000000), "historical capture identity");
    for (const auto& sample : samples) stats.update({&sample, 1}, sample.displayEndQpc);
    const auto result = stats.snapshot(samples.back().displayEndQpc);
    require(result.state == ds::State::Ready, "historical selected output has ten seconds of real display coverage");
    std::cout << std::setprecision(12) << "{\"stage\":\"historical-display-replay\",\"ready\":true,\"pid\":" << result.pid
              << ",\"swapchain\":" << result.swapchain << ",\"displayed_rows\":" << samples.size()
              << ",\"last10_intervals\":" << result.intervals << ",\"fps10\":" << result.fps
              << ",\"integral1_low10\":" << result.integralOnePercentLow << ",\"max_ms10\":" << result.maximumMs
              << ",\"historical_only\":true,\"live_capture\":false,\"rtss_written\":false}\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--replay-csv") { replayCsv(argv[2]); return 0; }
        if (argc != 1) throw std::runtime_error("Usage: xess-display-stats-test [--replay-csv historical.csv]");
        ordinaryAndIntegral(); droppedAndLongFreeze(); selectionAndRestart(); invalidSource();
        std::cout << "{\"stage\":\"display-interval-stats\",\"passed\":true,\"checks\":" << checks
                  << ",\"live_capture\":false,\"rtss_written\":false}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
