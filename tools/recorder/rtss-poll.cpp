// Passive, exact-PID RTSS present-interval reader. No shared memory writes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "RTSSSharedMemory.h"
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using Entry = RTSS_SHARED_MEMORY::RTSS_SHARED_MEMORY_APP_ENTRY;
struct Header { DWORD signature, version, stride, offset, count; };
struct FrameTimes { DWORD pid, position; DWORD values[1024]; };
constexpr std::size_t requiredEntryBytes = offsetof(Entry, dwStatFrameTimeBufPos) + sizeof(DWORD);
void require(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
unsigned number(const char* text) {
    unsigned value{}; const auto end = text + std::strlen(text); const auto result = std::from_chars(text, end, value);
    require(result.ec == std::errc{} && result.ptr == end, "invalid numeric argument"); return value;
}
bool leaf(std::string_view value) {
    return !value.empty() && value.size() < 128 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == value.npos && value != "." && value != "..";
}
bool stopped(const std::string& file) { return !file.empty() && GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES; }
void ready(const std::string& file, const char* state, const char* message) {
    if (file.empty()) return;
    const auto temporary = file + ".tmp";
    { std::ofstream out(temporary); out << "{\"state\":\"" << state << "\",\"message\":\"" << message << "\",\"readOnly\":true}\n"; }
    require(MoveFileExA(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0, "ready marker could not be written");
}
bool valid(const Header& h, std::size_t bytes) {
    return h.signature == 0x52545353 && h.version >= 0x00020005 && h.version < 0x00030000 &&
        h.stride >= requiredEntryBytes && h.stride <= 65536 && h.count && h.count <= 4096 &&
        h.offset >= sizeof(Header) && h.offset <= bytes && h.count <= (bytes - h.offset) / h.stride;
}
const Entry* entry(const unsigned char* view, const Header& h, DWORD index) {
    return reinterpret_cast<const Entry*>(view + h.offset + std::size_t(index) * h.stride);
}
bool snapshot(const Entry* source, DWORD pid, FrameTimes& result) {
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        FrameTimes a{}, b{};
        a.pid = source->dwProcessID; a.position = source->dwStatFrameTimeBufPos;
        std::memcpy(a.values, source->dwStatFrameTimeBuf, sizeof(a.values)); MemoryBarrier();
        b.pid = source->dwProcessID; b.position = source->dwStatFrameTimeBufPos;
        std::memcpy(b.values, source->dwStatFrameTimeBuf, sizeof(b.values)); MemoryBarrier();
        if (a.pid == pid && b.pid == pid && a.position == b.position &&
            !std::memcmp(a.values, b.values, sizeof(a.values))) { result = b; result.position &= 1023; return true; }
    }
    return false;
}
struct Mapping {
    HANDLE handle{}; const unsigned char* view{};
    ~Mapping() { if (view) UnmapViewOfFile(view); if (handle) CloseHandle(handle); }
};
void selfTest() {
    Header h{0x52545353, 0x00020005, DWORD(requiredEntryBytes), 64, 2};
    const auto bytes = h.offset + std::size_t(h.count) * h.stride;
    require(valid(h, bytes), "official header bounds");
    auto bad = h; bad.signature = 0xdead; require(!valid(bad, bytes), "dead mapping rejection");
    bad = h; bad.stride = 4; require(!valid(bad, bytes), "entry stride rejection");
    bad = h; bad.offset = DWORD(bytes); require(!valid(bad, bytes), "array bounds rejection");
    bad = h; bad.count = 4097; require(!valid(bad, bytes), "array count rejection");
    auto source = new Entry{}; source->dwProcessID = 55; source->dwStatFrameTimeBufPos = 5; source->dwStatFrameTimeBuf[4] = 16667;
    FrameTimes copy; require(snapshot(source, 55, copy) && copy.values[4] == 16667, "stable exact-PID snapshot");
    require(!snapshot(source, 56, copy), "stale other-PID entry rejected"); delete source;
    require(leaf("stop.request") && !leaf("../stop.request") && !leaf("D:\\stop.request"), "relative ASCII leaves");
    std::cout << "{\"passed\":true,\"checks\":8,\"sharedMappingOpened\":false}\n";
}
}

int main(int argc, char** argv) {
    std::string stopFile, readyFile; unsigned pid{};
    try {
        if (argc == 2 && !std::strcmp(argv[1], "--self-test")) { selfTest(); return 0; }
        require(argc >= 4, "Usage: rtss-poll.exe PID seconds(1..86400) output.csv [--stop-file leaf] [--ready-file leaf]");
        pid = number(argv[1]); const auto seconds = number(argv[2]); const std::string outputFile = argv[3];
        require(pid && seconds && seconds <= 86400 && leaf(outputFile), "positive PID, bounded duration and output leaf required");
        for (int i = 4; i < argc; ++i) {
            require((!std::strcmp(argv[i], "--stop-file") || !std::strcmp(argv[i], "--ready-file")) && i + 1 < argc, "unknown or incomplete option");
            const bool stop = !std::strcmp(argv[i], "--stop-file"); const std::string file = argv[++i];
            require(leaf(file), "marker must be a relative ASCII leaf name"); (stop ? stopFile : readyFile) = file;
        }
        if (stopped(stopFile)) { ready(readyFile, "stopped", "Stop requested before sampling"); std::cout << "{\"stopMarkerObserved\":true,\"rows\":0}\n"; return 0; }
        Mapping mapping; mapping.handle = OpenFileMappingA(FILE_MAP_READ, FALSE, "RTSSSharedMemoryV2");
        require(mapping.handle != nullptr, "RTSSSharedMemoryV2 read-only open failed");
        mapping.view = static_cast<const unsigned char*>(MapViewOfFile(mapping.handle, FILE_MAP_READ, 0, 0, 0));
        require(mapping.view != nullptr, "RTSS read-only map failed");
        MEMORY_BASIC_INFORMATION region{};
        require(VirtualQuery(mapping.view, &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && region.RegionSize >= sizeof(Header), "invalid mapped region");
        Header header{}; std::memcpy(&header, mapping.view, sizeof(header)); require(valid(header, region.RegionSize), "RTSS header or entry bounds are invalid");
        DWORD index = header.count; unsigned matches = 0;
        for (DWORD i = 0; i < header.count; ++i) if (entry(mapping.view, header, i)->dwProcessID == pid) { index = i; ++matches; }
        require(matches == 1, "exact target PID has no unique RTSS entry");
        FrameTimes current{}; require(snapshot(entry(mapping.view, header, index), pid, current), "RTSS target snapshot is unstable");
        std::ofstream out(outputFile); require(out.good(), "could not create RTSS output");
        LARGE_INTEGER frequency{}, start{}, now{}, previousPoll{}; require(QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&start), "QPC unavailable");
        previousPoll = now = start; DWORD last = current.position; std::uint64_t rows{}, riskPolls{}, unstablePolls{}; bool layoutChanged = false;
        out << std::setprecision(17) << "qpc_us,frame_time_us,targetPid,readQpcTicks,qpcFrequency\n"; out.flush();
        ready(readyFile, "ready", "Exact-PID RTSS read-only sampler started");
        while (!stopped(stopFile) && now.QuadPart - start.QuadPart < frequency.QuadPart * seconds) {
            Sleep(100); if (stopped(stopFile)) break;
            Header live{}; std::memcpy(&live, mapping.view, sizeof(live));
            if (!valid(live, region.RegionSize) || std::memcmp(&live, &header, sizeof(header)) || entry(mapping.view, header, index)->dwProcessID != pid) { layoutChanged = true; break; }
            QueryPerformanceCounter(&now);
            if (!snapshot(entry(mapping.view, header, index), pid, current)) { ++unstablePolls; continue; }
            const DWORD fresh = (current.position + 1024 - last) % 1024;
            // shortcut: a modulo ring cannot reveal full wraps; risk counts are heuristics, never a lossless claim.
            if (fresh >= 900 || now.QuadPart - previousPoll.QuadPart > frequency.QuadPart) ++riskPolls;
            for (DWORD k = 0; k < fresh; ++k) {
                out << now.QuadPart * 1000000.0 / frequency.QuadPart << ',' << current.values[(last + k) % 1024] << ',' << pid << ',' << now.QuadPart << ',' << frequency.QuadPart << '\n'; ++rows;
            }
            out.flush(); last = current.position; previousPoll = now;
        }
        QueryPerformanceCounter(&now); out.close();
        std::cout << "{\"readOnly\":true,\"targetPid\":" << pid << ",\"rows\":" << rows << ",\"startQpcTicks\":" << start.QuadPart << ",\"endQpcTicks\":" << now.QuadPart
            << ",\"qpcFrequency\":" << frequency.QuadPart << ",\"ringOverrunRiskPolls\":" << riskPolls << ",\"unstablePolls\":" << unstablePolls << ",\"lossless\":false,\"targetEntryOrLayoutChanged\":" << (layoutChanged ? "true" : "false")
            << ",\"stopMarkerObserved\":" << (stopped(stopFile) ? "true" : "false") << "}\n";
        return layoutChanged ? 3 : 0;
    } catch (const std::exception& error) {
        const DWORD code = GetLastError();
        try { ready(readyFile, "unavailable", "RTSS mapping or exact-PID entry unavailable; see rtss.stderr.txt"); } catch (...) {}
        std::cerr << "RTSS_READONLY_ERROR: " << error.what() << "; win32=" << code << '\n';
        std::cout << "{\"readOnly\":true,\"state\":\"unavailable\",\"targetPid\":" << pid << ",\"win32\":" << code << "}\n"; return 2;
    }
}
