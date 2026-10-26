#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <PMDPSharedMemory.h>
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct Header {
    DWORD signature, version, stride, offset, capacity, count, position, status;
};
static_assert(sizeof(Header) == offsetof(PMDP_SHARED_MEMORY, arrFrame));
bool valid(const Header& h, std::size_t bytes) {
    return h.signature == 0x504d4450 && h.version == 0x00020000 &&
        h.stride >= sizeof(PMDP_FRAME_DATA) && h.stride <= 4096 &&
        h.capacity && h.capacity <= 8192 && !(h.capacity & (h.capacity - 1)) &&
        h.offset >= sizeof(Header) && h.offset <= bytes &&
        h.capacity <= (bytes - h.offset) / h.stride &&
        h.position == (h.count & (h.capacity - 1));
}
struct Snapshot {
    Header header{};
    std::vector<PMDP_FRAME_DATA> rows;
};
bool snapshot(const unsigned char* view, std::size_t bytes, Snapshot& out) {
    if (bytes < sizeof(Header)) return false;
    for (unsigned attempt = 0; attempt != 6; ++attempt) {
        Header before{}, middle{}, after{};
        std::memcpy(&before, view, sizeof(before));
        if (!valid(before, bytes)) return false;
        std::vector<PMDP_FRAME_DATA> first(before.capacity), second(before.capacity);
        for (DWORD n = 0; n < before.capacity; ++n)
            std::memcpy(&first[n], view + before.offset + n * before.stride, sizeof(first[n]));
        MemoryBarrier();
        std::memcpy(&middle, view, sizeof(middle));
        for (DWORD n = 0; n < before.capacity; ++n)
            std::memcpy(&second[n], view + before.offset + n * before.stride, sizeof(second[n]));
        MemoryBarrier();
        std::memcpy(&after, view, sizeof(after));
        if (!std::memcmp(&before, &middle, sizeof(before)) &&
            !std::memcmp(&before, &after, sizeof(before)) &&
            !std::memcmp(first.data(), second.data(), first.size() * sizeof(first[0]))) {
            out.header = before;
            out.rows = std::move(second);
            return true;
        }
        Sleep(1);
    }
    return false;
}
struct Cursor {
    bool initialized{};
    DWORD previous{};
    std::uint64_t epoch{1}, ordinal{}, lost{}, resets{};
    DWORD take(const Header& h, bool includeExisting) {
        // The producer overwrites position before publishing count/position.
        const auto safeCapacity = h.capacity - 1;
        const auto retained = std::min(h.count, safeCapacity);
        if (!initialized) {
            initialized = true; previous = h.count; ordinal = h.count;
            if (includeExisting && h.count >= h.capacity) ++lost;
            return includeExisting ? retained : 0;
        }
        const DWORD delta = h.count - previous;
        previous = h.count;
        if (delta > 0x7fffffff) {
            ++epoch; ++resets; ordinal = h.count;
            if (h.count >= h.capacity) ++lost;
            return retained;
        }
        ordinal += delta;
        if (delta > safeCapacity) lost += delta - safeCapacity;
        return std::min(delta, safeCapacity);
    }
};
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
unsigned number(const char* text) {
    unsigned value{};
    const auto end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, value);
    require(result.ec == std::errc{} && result.ptr == end, "invalid numeric argument");
    return value;
}
bool leaf(std::string_view value) {
    return !value.empty() && value.size() < 128 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == value.npos &&
        value != "." && value != "..";
}
bool stopped(const std::string& file) { return !file.empty() && GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES; }
void ready(const std::string& file, const char* state, const char* message) {
    if (file.empty()) return;
    const auto temporary = file + ".tmp";
    { std::ofstream out(temporary); out << "{\"state\":\"" << state << "\",\"message\":\"" << message << "\",\"readOnly\":true}\n"; }
    require(MoveFileExA(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0, "ready marker could not be written");
}
void selfTest() {
    auto memory = std::make_unique<PMDP_SHARED_MEMORY>();
    Header header{0x504d4450, 0x00020000, sizeof(PMDP_FRAME_DATA),
                  offsetof(PMDP_SHARED_MEMORY, arrFrame), 8, 12, 4, 0};
    std::memcpy(memory.get(), &header, sizeof(header));
    memory->arrFrame[3].data1.ProcessID = 99;
    Snapshot copy;
    require(snapshot(reinterpret_cast<const unsigned char*>(memory.get()), sizeof(*memory), copy) &&
            copy.rows[3].data1.ProcessID == 99, "stable official-layout snapshot");
    auto bad = header;
    bad.version = 0x00010000; require(!valid(bad, sizeof(*memory)), "version rejection");
    bad = header; bad.stride = 1; require(!valid(bad, sizeof(*memory)), "stride rejection");
    bad = header; bad.capacity = 8193; require(!valid(bad, sizeof(*memory)), "capacity rejection");
    bad = header; bad.offset = sizeof(*memory); require(!valid(bad, sizeof(*memory)), "bounds rejection");
    bad = header; bad.position = 3; require(!valid(bad, sizeof(*memory)), "unpublished position rejection");
    Cursor cursor;
    require(cursor.take(header, true) == 7 && cursor.lost == 1 && !cursor.take(header, true), "initial ring skips in-flight oldest slot and excludes duplicates");
    header.count = 15; header.position = 7;
    require(cursor.take(header, true) == 3 && cursor.ordinal == 15, "incremental frame sequence");
    header.count = 30; header.position = 6;
    require(cursor.take(header, true) == 7 && cursor.lost == 9, "overwritten unread frames and omitted in-flight slot reported");
    header.count = 38; header.position = 6;
    require(cursor.take(header, true) == 7 && cursor.lost == 10, "exact full-ring delta also excludes unpublished write position");
    header.count = 2; header.position = 2;
    require(cursor.take(header, true) == 2 && cursor.epoch == 2 && cursor.resets == 1, "provider reset starts new epoch");
    Cursor wrap; header.count = 0xfffffffe; header.position = 6; wrap.take(header, false);
    header.count = 2; header.position = 2;
    require(wrap.take(header, false) == 4 && wrap.epoch == 1 && wrap.ordinal == 0x100000002ull, "DWORD wrap keeps monotonic reader ordinal");
    std::cout << "{\"passed\":true,\"checks\":12,\"sharedMappingOpened\":false}\n";
}
struct Mapping {
    HANDLE handle{};
    const unsigned char* view{};
    ~Mapping() { if (view) UnmapViewOfFile(view); if (handle) CloseHandle(handle); }
};
void row(const PMDP_FRAME_DATA& f, std::uint64_t epoch, std::uint64_t ordinal,
         bool initial, LONGLONG readQpc) {
    std::cout << epoch << ',' << ordinal << ',' << initial << ',' << readQpc << ",\"";
    for (std::size_t n = 0; n < sizeof(f.data1.Application) && f.data1.Application[n]; ++n) {
        if (f.data1.Application[n] == '"') std::cout << '"';
        std::cout << f.data1.Application[n];
    }
    const auto& a = f.data1; const auto& b = f.data2;
    std::cout << "\"," << a.ProcessID << ",0x" << std::hex << a.SwapChainAddress << std::dec
        << ',' << a.Runtime << ',' << a.SyncInterval << ',' << a.PresentFlags << ',' << a.Dropped
        << ',' << a.TimeInSeconds << ',' << a.msInPresentAPI << ',' << a.msBetweenPresents
        << ',' << a.AllowsTearing << ',' << a.PresentMode << ',' << a.msUntilRenderComplete
        << ',' << a.msUntilDisplayed << ',' << a.msBetweenDisplayChange << ',' << a.msUntilRenderStart
        << ',' << a.qpcTime << ',' << a.msSinceInput << ',' << a.msGpuActive << ',' << a.msGpuVideoActive
        << ',' << b.CPUStart << ',' << b.Frametime << ',' << b.CPUBusy << ',' << b.CPUWait
        << ',' << b.GPULatency << ',' << b.GPUTime << ',' << b.GPUBusy << ',' << b.VideoBusy
        << ',' << b.GPUWait << ',' << b.DisplayLatency << ',' << b.DisplayedTime
        << ',' << b.AnimationError << ',' << b.ClickToPhotonLatency << '\n';
}
}
int main(int argc, char** argv) {
    std::string stopFile, readyFile;
    try {
        if (argc == 2 && !std::strcmp(argv[1], "--self-test")) { selfTest(); return 0; }
        require(argc >= 3, "Usage: pmdp-reader.exe PID seconds(1..86400) [--new-only] [--stop-file leaf] [--ready-file leaf]");
        const auto pid = number(argv[1]), seconds = number(argv[2]);
        require(pid && seconds && seconds <= 86400, "PID must be positive and seconds in 1..86400");
        bool includeExisting = true;
        for (int i = 3; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--new-only")) includeExisting = false;
            else if ((!std::strcmp(argv[i], "--stop-file") || !std::strcmp(argv[i], "--ready-file")) && i + 1 < argc) {
                const bool stop = !std::strcmp(argv[i], "--stop-file"); const std::string file = argv[++i];
                require(leaf(file), "marker must be a relative ASCII leaf name");
                (stop ? stopFile : readyFile) = file;
            } else require(false, "unknown or incomplete option");
        }
        if (stopped(stopFile)) { ready(readyFile, "stopped", "Stop requested before sampling"); std::cerr << "{\"stopMarkerObserved\":true,\"rows\":0}\n"; return 0; }
        Mapping mapping;
        mapping.handle = OpenFileMappingW(FILE_MAP_READ, FALSE, L"PMDPSharedMemory");
        require(mapping.handle != nullptr, "PMDPSharedMemory read-only open failed");
        mapping.view = static_cast<const unsigned char*>(MapViewOfFile(mapping.handle, FILE_MAP_READ, 0, 0, 0));
        require(mapping.view != nullptr, "read-only map failed");
        MEMORY_BASIC_INFORMATION region{};
        require(VirtualQuery(mapping.view, &region, sizeof(region)) == sizeof(region) &&
                region.State == MEM_COMMIT && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)), "invalid mapped region");
        LARGE_INTEGER frequency{}, started{}, now{};
        require(QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&started), "QPC unavailable");
        std::cout << std::setprecision(17)
            << "StreamEpoch,ProviderOrdinal,InitialSnapshot,ReadQPC,Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange,msUntilRenderStart,qpcTime,msSinceInput,msGpuActive,msGpuVideoActive,CPUStart,Frametime,CPUBusy,CPUWait,GPULatency,GPUTime,GPUBusy,VideoBusy,GPUWait,DisplayLatency,DisplayedTime,AnimationError,ClickToPhotonLatency\n";
        Cursor cursor;
        Snapshot capture;
        std::uint64_t rows{}, stable{}, missed{};
        require(snapshot(mapping.view, region.RegionSize, capture), "PMDP shared mapping is not initialized with the supported v2 layout");
        std::cout.flush(); ready(readyFile, "ready", "Existing PMDP read-only sampler started");
        do {
            if (stopped(stopFile)) break;
            const bool initial = !cursor.initialized;
            if (snapshot(mapping.view, region.RegionSize, capture)) {
                ++stable;
                const auto count = cursor.take(capture.header, includeExisting);
                QueryPerformanceCounter(&now);
                const auto begin = (capture.header.position - count) & (capture.header.capacity - 1);
                for (DWORD n = 0; n < count; ++n) {
                    const auto& frame = capture.rows[(begin + n) & (capture.header.capacity - 1)];
                    if (frame.data1.ProcessID == pid) {
                        row(frame, cursor.epoch, cursor.ordinal - count + n + 1, initial, now.QuadPart);
                        ++rows;
                    }
                }
            } else ++missed;
            std::cout.flush();
            Sleep(50);
            QueryPerformanceCounter(&now);
        } while (now.QuadPart - started.QuadPart < frequency.QuadPart * seconds);
        QueryPerformanceCounter(&now);
        std::cout.flush();
        std::cerr << "{\"readOnly\":true,\"mapping\":\"PMDPSharedMemory\",\"pid\":" << pid
            << ",\"qpcFrequency\":" << frequency.QuadPart << ",\"startQpc\":" << started.QuadPart
            << ",\"endQpc\":" << now.QuadPart << ",\"includeExisting\":" << (includeExisting ? "true" : "false")
            << ",\"stopMarkerObserved\":" << (stopped(stopFile) ? "true" : "false")
            << ",\"version\":" << capture.header.version << ",\"stride\":" << capture.header.stride
            << ",\"capacity\":" << capture.header.capacity << ",\"providerStatus\":" << capture.header.status
            << ",\"rows\":" << rows << ",\"stableSnapshots\":" << stable << ",\"missedSnapshots\":" << missed
            << ",\"streamResets\":" << cursor.resets << ",\"unreadFramesLost\":" << cursor.lost
            << ",\"atomicSnapshot\":false,\"newEtwSessions\":0,\"processHandlesOpened\":0}\n";
        return stable ? 0 : 1;
    } catch (const std::exception& e) {
        try { ready(readyFile, "unavailable", "PMDP mapping unavailable or invalid; see pmdp-run.json"); } catch (...) {}
        std::cerr << "PMDP_READONLY_ERROR: " << e.what() << "; win32=" << GetLastError() << '\n';
        return 2;
    }
}
