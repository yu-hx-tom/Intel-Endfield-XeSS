#include "rtss_osd_adapter.hpp"
#include <windows.h>
#include <RTSSSharedMemory.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xess_vk::display_stats {
namespace detail {
OsdResult writeOsdMemory(void* memory, std::size_t size, std::string_view owner,
                         std::string_view text, bool clear) noexcept {
    using Memory = RTSS_SHARED_MEMORY;
    using Entry = Memory::RTSS_SHARED_MEMORY_OSD_ENTRY;
    if (owner.empty() || owner.size() >= sizeof(Entry::szOSDOwner) || owner.find('\0') != owner.npos ||
        text.size() >= sizeof(Entry::szOSDEx) || text.find('\0') != text.npos)
        return OsdResult::InvalidText;
    constexpr auto headerSize = offsetof(Memory, dwBusy) + sizeof(LONG);
    if (!memory || size < headerSize || reinterpret_cast<std::uintptr_t>(memory) % alignof(LONG))
        return OsdResult::InvalidLayout;
    auto& shared = *static_cast<Memory*>(memory);
    const auto stride = static_cast<std::size_t>(shared.dwOSDEntrySize);
    const auto offset = static_cast<std::size_t>(shared.dwOSDArrOffset);
    const auto count = shared.dwOSDArrSize;
    if (shared.dwSignature != 0x52545353 || shared.dwVersion < 0x0002000e || shared.dwVersion >> 16 != 2 ||
        stride < offsetof(Entry, szOSDEx) + sizeof(Entry::szOSDEx) || count < 2 || count > 256 ||
        offset < headerSize || offset > size || count > (size - offset) / stride)
        return OsdResult::InvalidLayout;
    if (InterlockedOr(&shared.dwBusy, 1) & 1) return OsdResult::Busy;
    struct Unlock { LONG& busy; ~Unlock() { InterlockedAnd(&busy, ~1L); } } unlock{shared.dwBusy};
    Entry* vacant{};
    Entry* owned{};
    for (DWORD index = 1; index < count; ++index) {
        auto* entry = reinterpret_cast<Entry*>(static_cast<unsigned char*>(memory) + offset + index * stride);
        const auto end = static_cast<const char*>(std::memchr(entry->szOSDOwner, 0, sizeof(entry->szOSDOwner)));
        if (!end) continue;
        const std::string_view existing(entry->szOSDOwner, end - entry->szOSDOwner);
        if (existing == owner) { owned = entry; break; }
        if (existing.empty() && !vacant) vacant = entry;
    }
    if (clear) {
        if (!owned) return OsdResult::NoSlot;
        owned->szOSD[0] = owned->szOSDEx[0] = owned->szOSDOwner[0] = 0;
    } else {
        if (!owned) owned = vacant;
        if (!owned) return OsdResult::NoSlot;
        std::memcpy(owned->szOSDOwner, owner.data(), owner.size()); owned->szOSDOwner[owner.size()] = 0;
        std::memcpy(owned->szOSDEx, text.data(), text.size()); owned->szOSDEx[text.size()] = 0;
        const auto shortSize = std::min(text.size(), sizeof(owned->szOSD) - 1);
        std::memcpy(owned->szOSD, text.data(), shortSize); owned->szOSD[shortSize] = 0;
    }
    InterlockedIncrement(reinterpret_cast<LONG*>(&shared.dwOSDFrame));
    return OsdResult::Updated;
}
} // namespace detail
OsdWriter::OsdWriter() {
    LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc);
    char owner[96]{};
    std::snprintf(owner, sizeof(owner), "EndfieldDisplayLow.%lu.%llx", GetCurrentProcessId(),
                  static_cast<unsigned long long>(qpc.QuadPart));
    owner_ = owner;
}
OsdWriter::~OsdWriter() { if (published_) clear(); }
OsdResult OsdWriter::write(std::string_view text, bool clearSlot) {
    const auto mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, L"RTSSSharedMemoryV2");
    if (!mapping) return OsdResult::Unavailable;
    auto* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    MEMORY_BASIC_INFORMATION info{};
    auto result = OsdResult::Unavailable;
    if (view && VirtualQuery(view, &info, sizeof(info)) == sizeof(info))
        result = detail::writeOsdMemory(view, info.RegionSize, owner_, text, clearSlot);
    if (view) UnmapViewOfFile(view);
    CloseHandle(mapping);
    return result;
}
OsdResult OsdWriter::publish(std::string_view text, std::uint32_t targetPid) {
    DWORD foreground{};
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    if (!targetPid || foreground != targetPid) return clear();
    const auto result = write(text, false);
    if (result == OsdResult::Updated) published_ = true;
    return result;
}
OsdResult OsdWriter::clear() {
    if (!published_) return OsdResult::NoSlot;
    const auto result = write({}, true);
    if (result == OsdResult::Updated || result == OsdResult::NoSlot) published_ = false;
    return result;
}
} // namespace xess_vk::display_stats
