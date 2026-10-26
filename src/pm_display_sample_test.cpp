#undef NDEBUG
#include "pm_display_sample.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

int main() {
    using namespace xess_vk::display_stats;
    DisplaySample a{}, generated{}, next{};
    constexpr std::uint64_t frequency = 10000000, cpuStart = 100000000;
    assert(convertPresentMonDisplay(a, 42, 99, dxgiRuntime, 2, cpuStart, 20, 8.3333, frequency) == DisplayConversion::Displayed);
    assert(convertPresentMonDisplay(generated, 42, 99, dxgiRuntime, 50, cpuStart, 28.3333, 8.3334, frequency) == DisplayConversion::Displayed);
    assert(a.displayStartQpc == 100200000 && a.displayEndQpc == generated.displayStartQpc);
    assert(generated.displayEndQpc == 100366667 && generated.frameType == 50);
    // A shared CPU origin does not collapse the generated frame's display start.
    assert(generated.displayStartQpc > a.displayStartQpc);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, cpuStart, 36.6667, 48.9, frequency) == DisplayConversion::Displayed);
    assert(next.displayEndQpc - next.displayStartQpc == 489000);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, cpuStart, nan, nan, frequency) == DisplayConversion::NotDisplayed);
    assert(!next.displayTimeValid && !next.displayStartQpc);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, cpuStart, 0, 0, frequency) == DisplayConversion::NotDisplayed);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, cpuStart, -1, 8, frequency) == DisplayConversion::Invalid);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, cpuStart, 1, nan, frequency) == DisplayConversion::NotDisplayed);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, 0, nan, 8, frequency) == DisplayConversion::Unavailable);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, cpuStart, nan, 8, frequency) == DisplayConversion::Unavailable);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, cpuStart, INFINITY, 8, frequency) == DisplayConversion::Invalid);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, cpuStart, 1, 8, 0) == DisplayConversion::Invalid);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, UINT64_MAX - 1, 1, 8, frequency) == DisplayConversion::Invalid);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, UINT64_MAX - 10001, 1, 8, frequency) == DisplayConversion::Invalid);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, cpuStart, 1, 1e300, frequency) == DisplayConversion::Invalid);
    assert(convertPresentMonDisplay(next, 42, 99, 1, 2, cpuStart, 0, 0.000001, frequency) == DisplayConversion::Invalid);
    std::cout << "PresentMon display conversion: generated shared-origin, long hold, dropped, invalid and overflow checks passed.\n";
}
