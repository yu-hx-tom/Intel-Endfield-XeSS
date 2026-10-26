#include "rtss_osd_adapter.hpp"
#include <windows.h>
#include <RTSSSharedMemory.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace ds = xess_vk::display_stats;
namespace {
unsigned checks{};
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
}
int main() {
    try {
        using Memory = RTSS_SHARED_MEMORY;
        using Entry = Memory::RTSS_SHARED_MEMORY_OSD_ENTRY;
        constexpr std::size_t offset = 128, count = 3, size = offset + count * sizeof(Entry);
        std::vector<std::uint64_t> storage((size + 7) / 8);
        auto* bytes = reinterpret_cast<unsigned char*>(storage.data());
        auto& header = *reinterpret_cast<Memory*>(bytes);
        header.dwSignature = 0x52545353; header.dwVersion = 0x00020014;
        header.dwOSDArrOffset = offset; header.dwOSDArrSize = count; header.dwOSDEntrySize = sizeof(Entry);
        auto entry = [&](unsigned index) -> Entry& { return *reinterpret_cast<Entry*>(bytes + offset + index * sizeof(Entry)); };
        std::strcpy(entry(0).szOSDOwner, "MSIAfterburner"); std::strcpy(entry(0).szOSDEx, "GPU CPU VRAM RAM");
        std::strcpy(entry(1).szOSDOwner, "OtherClient"); std::strcpy(entry(1).szOSDEx, "Existing metrics");
        const std::vector<unsigned char> existing(bytes + offset, bytes + offset + 2 * sizeof(Entry));
        header.dwBusy = 2;
        auto result = ds::detail::writeOsdMemory(bytes, size, "OwnedDisplayStats", "Display FT(last): N/A", false);
        require(result == ds::OsdResult::Updated && header.dwBusy == 2 && header.dwOSDFrame == 1,
            "own update releases only busy bit zero");
        require(std::strcmp(entry(2).szOSDOwner, "OwnedDisplayStats") == 0 &&
            std::strcmp(entry(2).szOSDEx, "Display FT(last): N/A") == 0, "third-party text uses an independent owner slot");
        require(std::memcmp(existing.data(), bytes + offset, existing.size()) == 0,
            "primary AB and other clients are byte-for-byte unchanged");
        header.dwBusy = 3;
        result = ds::detail::writeOsdMemory(bytes, size, "OwnedDisplayStats", "Changed", false);
        require(result == ds::OsdResult::Busy && header.dwBusy == 3 && header.dwOSDFrame == 1 &&
            std::strcmp(entry(2).szOSDEx, "Display FT(last): N/A") == 0, "renderer lock causes no blocking or writes");
        header.dwBusy = 2;
        require(ds::detail::writeOsdMemory(bytes, size, "Unrelated", {}, true) == ds::OsdResult::NoSlot,
            "unowned clear never claims or erases another slot");
        require(ds::detail::writeOsdMemory(bytes, size, "Unrelated", "Another", false) == ds::OsdResult::NoSlot,
            "full OSD table does not evict another owner");
        require(ds::detail::writeOsdMemory(bytes, size, "OwnedDisplayStats", {}, true) == ds::OsdResult::Updated &&
            !entry(2).szOSDOwner[0] && !entry(2).szOSDEx[0] && header.dwBusy == 2,
            "normal cleanup releases only this owner and its text");
        require(std::memcmp(existing.data(), bytes + offset, existing.size()) == 0, "cleanup preserves every existing hardware item");
        header.dwOSDArrOffset = static_cast<DWORD>(size);
        require(ds::detail::writeOsdMemory(bytes, size, "OwnedDisplayStats", "x", false) == ds::OsdResult::InvalidLayout,
            "unbounded shared-memory offset is rejected before a write");
        header.dwOSDArrOffset = offset; header.dwVersion = 0x0002000d;
        require(ds::detail::writeOsdMemory(bytes, size, "OwnedDisplayStats", "x", false) == ds::OsdResult::InvalidLayout,
            "unsupported old locking protocol is not silently used");
        std::cout << "{\"stage\":\"rtss-owned-osd-memory\",\"passed\":true,\"checks\":" << checks
                  << ",\"real_rtss_opened\":false,\"real_rtss_written\":false}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
