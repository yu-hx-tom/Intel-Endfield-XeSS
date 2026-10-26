#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace xess_vk::display_stats {
enum class OsdResult { Updated, Busy, Unavailable, InvalidLayout, InvalidText, NoSlot };
namespace detail {
// Uses the installed RTSS SDK layout; also callable with an owned memory image.
OsdResult writeOsdMemory(void*, std::size_t, std::string_view owner, std::string_view text, bool clear) noexcept;
}
class OsdWriter {
public:
    OsdWriter(); // Does not open or modify RTSS shared memory.
    ~OsdWriter();
    OsdWriter(const OsdWriter&) = delete;
    OsdWriter& operator=(const OsdWriter&) = delete;
    // Only the target foreground process may receive our text; otherwise clear it.
    OsdResult publish(std::string_view, std::uint32_t targetPid);
    OsdResult clear();
private:
    std::string owner_;
    bool published_{};
    OsdResult write(std::string_view, bool clear);
};
} // namespace xess_vk::display_stats
