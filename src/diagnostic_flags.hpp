#pragma once
#include "build_flavor.hpp"
#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>

// Process-start copy policy and diagnostic switches for in-game bisection.
// A switch is on when either
//   - the file C:\ProgramData\EndfieldXeSSDiag\<Name> exists (any user may
//     create it; works whatever token the game process runs with), or
//   - HKCU\Software\EndfieldXeSSObserver\Diagnostics\<Name> is DWORD 1.
// Read once per process on first use. advapi32 is loaded dynamically so the
// test executables that compile the layer need no extra link dependency.
namespace xess_vk::diagnostic_flags {
inline bool runtimeDiagnostics() noexcept {
    static const bool enabled=[] {
        const auto error=GetLastError();wchar_t value[2]{};
        const auto n=GetEnvironmentVariableW(L"XESS_DIAGNOSTIC_MODE",value,2);
        const bool result=releaseMinimal?(n==1&&value[0]==L'1'):!(n==1&&value[0]==L'0');
        SetLastError(error);return result;
    }();return enabled;
}
struct Reading {
    bool on{}, file{};
    LONG registry{ERROR_FILE_NOT_FOUND}; // RegGetValueW status; logged for diagnosis
};
inline Reading read(const wchar_t* name) noexcept {
    if constexpr(releaseMinimal)return {};
    const DWORD error = GetLastError();
    Reading out;
    try {
        const std::wstring path = std::wstring(L"C:\\ProgramData\\EndfieldXeSSDiag\\") + name;
        out.file = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    } catch (...) {
    }
    if (HMODULE advapi = LoadLibraryW(L"advapi32.dll")) {
        using RegGetValue = LONG(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
        if (auto get = reinterpret_cast<RegGetValue>(GetProcAddress(advapi, "RegGetValueW"))) {
            DWORD value = 0, size = sizeof(value);
            out.registry = get(HKEY_CURRENT_USER, L"Software\\EndfieldXeSSObserver\\Diagnostics", name,
                               RRF_RT_REG_DWORD, nullptr, &value, &size);
            if (out.registry == ERROR_SUCCESS && value != 1) out.registry = -1; // present but not 1
        } else {
            out.registry = -2;
        }
        FreeLibrary(advapi);
    } else {
        out.registry = -3;
    }
    out.on = out.file || out.registry == ERROR_SUCCESS;
    SetLastError(error);
    return out;
}
// Leave the game's FrameInterpolationAlgo at FSR3; native FI is then held off
// only by the late-gate OFF shadows.
inline const Reading& keepNativeAlgoReading() noexcept {
    static const Reading value = read(L"KeepNativeAlgo");
    return value;
}
inline bool keepNativeAlgo() noexcept { return keepNativeAlgoReading().on; }
// Never record the depth/motion snapshot copies into game command buffers.
inline const Reading& skipInputSnapshotReading() noexcept {
    static const Reading value = read(L"SkipInputSnapshot");
    return value;
}
inline bool skipInputSnapshot() noexcept { return skipInputSnapshotReading().on; }
// Current-profile only: replace the two native FI decisions, preserving algo.
// Single-chain launches enable this by default; LocalFiOff remains an opt-in for legacy launches.
inline const Reading& localFiOffReading() noexcept {
    static const Reading value = read(L"LocalFiOff");
    return value;
}
inline bool localFiOff() noexcept {
    static const bool enabled = [] {
        const auto error = GetLastError();
        wchar_t route[2]{};
        const bool singleChain = GetEnvironmentVariableW(L"XESS_FAKE_SWAPCHAIN", route, 2) == 1 && route[0] == L'1';
        // The file is the explicit escape hatch, including when LocalFiOff is also present.
        const bool disabled = read(L"DisableLocalFiOff").file;
        const bool requested = !disabled && (singleChain || localFiOffReading().on);
        SetLastError(error);
        return requested;
    }();
    return enabled;
}
// Isolated experiment: one extra reset on a continuous admitted single-chain ON frame.
// File-only and default OFF; the launcher never creates this marker.
inline bool longFrameFollowupReset() noexcept {
    static const bool enabled = read(L"LongFrameFollowupReset").file;
    return enabled;
}
inline bool srReplaceRequested() noexcept {
    static const bool enabled=[] {
        const DWORD error=GetLastError();wchar_t value[2]{};
        const bool requested=GetEnvironmentVariableW(L"XESS_SR_REPLACE",value,2)==1&&value[0]==L'1';
        SetLastError(error);return requested;
    }();
    return enabled;
}
inline bool srInvertJitteredMotion() noexcept {
    static const bool enabled=read(L"SrInvertJitteredMotion").file;
    return enabled;
}
// T5.3 A/B, file or exact env=1, default OFF and frozen per process.
// Never drops descriptor mutations.
inline bool fsrPipelineTracking() noexcept {
    if constexpr(releaseMinimal)return true;
    static const bool enabled=[] {
        const auto error=GetLastError();wchar_t value[2]{};
        const bool requested=GetEnvironmentVariableW(L"XESS_FSR_PIPELINE_TRACKING",value,2)==1&&value[0]==L'1';
        const bool enabled=requested||read(L"FsrPipelineTracking").file;SetLastError(error);return enabled;
    }();return enabled;
}
// SR opt-in is always core replacement. The retired file must never select
// a hidden FSR+XeSS overlay route.
inline bool srNativeReplacementAvailable() noexcept {return srReplaceRequested();}
// Explicit diagnostic opt-in. Missing evidence never terminates a user's game by default.
inline bool srFatalOnUnproven() noexcept {
    static const bool enabled=read(L"SrFatalOnUnproven").file;
    return enabled;
}
// Read-only SR metadata window; never enables Execute or changes game dispatches.
inline bool srObservation() noexcept {
    static const bool enabled = read(L"SrObservation").file;
    return enabled;
}
// Read-only, bounded metadata for the remaining native FSR jobs. Never suppression.
inline bool srFullGraphObservation() noexcept {
    static const bool enabled=read(L"SrFullGraphObservation").file;
    return enabled;
}
// Single-chain defaults to T3.3; legacy launches retain the file opt-in.
inline bool readDepthComputePath() noexcept {
    const DWORD error=GetLastError();
    wchar_t route[2]{};
    const bool singleChain=GetEnvironmentVariableW(L"XESS_FAKE_SWAPCHAIN",route,2)==1&&route[0]==L'1';
    if constexpr(releaseMinimal){SetLastError(error);return singleChain;}
    const bool enabled=GetFileAttributesW(L"C:\\ProgramData\\EndfieldXeSSDiag\\DepthComputePath")!=INVALID_FILE_ATTRIBUTES;
    const bool disabled=GetFileAttributesW(L"C:\\ProgramData\\EndfieldXeSSDiag\\DisableDepthComputePath")!=INVALID_FILE_ATTRIBUTES;
    SetLastError(error);
    return (singleChain||enabled)&&!disabled;
}
inline bool depthComputePath() noexcept {
    const DWORD error=GetLastError();
    static const bool enabled=readDepthComputePath();
    SetLastError(error);
    return enabled;
}
struct AsyncCopyFramesReading {
    std::uint32_t frames{2};
    bool valid{};
};
inline AsyncCopyFramesReading parseAsyncCopyFramesReading(std::string_view text) noexcept {
    if (text.size() >= 3 && text.substr(0,3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    if (end == begin && (text[begin] == '1' || text[begin] == '2'))
        return {static_cast<std::uint32_t>(text[begin] - '0'), true};
    return {};
}
inline std::uint32_t parseAsyncCopyFrames(std::string_view text) noexcept {
    return parseAsyncCopyFramesReading(text).frames;
}
inline AsyncCopyFramesReading readAsyncCopyFramesReading(const wchar_t* path) noexcept {
    const DWORD error = GetLastError();
    AsyncCopyFramesReading reading;
    const auto file = CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                 nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        char bytes[17]{};
        DWORD count{};
        // Read once, with one excess byte to reject a truncated configuration.
        if (ReadFile(file,bytes,sizeof(bytes),&count,nullptr) && count < sizeof(bytes))
            reading = parseAsyncCopyFramesReading({bytes,count});
        CloseHandle(file);
    }
    SetLastError(error);
    return reading;
}
inline std::uint32_t readAsyncCopyFramesFile(const wchar_t* path) noexcept {
    return readAsyncCopyFramesReading(path).frames;
}
enum class CopyPathSource { Default, File };
inline constexpr const char* copyPathSourceName(CopyPathSource source) noexcept {
    return source == CopyPathSource::File ? "file" : "default";
}
struct CopyPathInputs {
    bool singleChain{}, asyncFile{}, sharedFile{}, syncFile{}, disableSharedFile{};
    std::uint32_t frames{2};
    bool framesFile{}; // Only a successfully read, valid 1 or 2 has file provenance.
    bool sharedSyncFile{};
};
struct CopyPathSelection {
    bool async{}, shared{};
    std::uint32_t inflightLimit{};
    CopyPathSource routeSource{}, inflightSource{}, sharedSource{};
};
inline constexpr CopyPathSelection selectCopyPath(CopyPathInputs input) noexcept {
    if (!input.singleChain) return {}; // The real-WSI legacy route keeps its existing synchronous path.
    const bool framesFile = input.framesFile && (input.frames == 1 || input.frames == 2);
    const bool async = !input.syncFile && !input.sharedSyncFile && (input.asyncFile || framesFile);
    const bool shared = !input.syncFile && !input.disableSharedFile;
    return {async, shared, async ? (framesFile ? input.frames : 2u) : 0u,
        input.syncFile || input.sharedSyncFile || input.asyncFile || framesFile ? CopyPathSource::File : CopyPathSource::Default,
        input.syncFile || input.sharedSyncFile || (async && framesFile) ? CopyPathSource::File : CopyPathSource::Default,
        input.syncFile || input.sharedSyncFile || input.disableSharedFile || (async && input.sharedFile) ? CopyPathSource::File : CopyPathSource::Default};
}
inline CopyPathInputs readCopyPathInputs() noexcept {
    const DWORD error = GetLastError();
    CopyPathInputs input;
    wchar_t route[2]{};
    input.singleChain = GetEnvironmentVariableW(L"XESS_FAKE_SWAPCHAIN", route, 2) == 1 && route[0] == L'1';
    // Distribution always uses the accepted synchronous/shared route. Old
    // developer markers must not silently turn a user's release into K1/K2.
    if constexpr(releaseMinimal){SetLastError(error);return input;}
    input.asyncFile = GetFileAttributesW(L"C:\\ProgramData\\EndfieldXeSSDiag\\AsyncCopyPath") != INVALID_FILE_ATTRIBUTES;
    input.sharedFile = GetFileAttributesW(L"C:\\ProgramData\\EndfieldXeSSDiag\\SharedCopyPath") != INVALID_FILE_ATTRIBUTES;
    input.syncFile = GetFileAttributesW(L"C:\\ProgramData\\EndfieldXeSSDiag\\SyncCopyPath") != INVALID_FILE_ATTRIBUTES;
    input.disableSharedFile = GetFileAttributesW(L"C:\\ProgramData\\EndfieldXeSSDiag\\DisableSharedCopyPath") != INVALID_FILE_ATTRIBUTES;
    input.sharedSyncFile = GetFileAttributesW(L"C:\\ProgramData\\EndfieldXeSSDiag\\SharedCopySync") != INVALID_FILE_ATTRIBUTES;
    const auto frames = readAsyncCopyFramesReading(L"C:\\ProgramData\\EndfieldXeSSDiag\\AsyncCopyFrames");
    input.frames = frames.frames;
    input.framesFile = frames.valid;
    SetLastError(error);
    return input;
}
inline const CopyPathInputs& copyPathReading() noexcept {
    const DWORD error = GetLastError();
    static const auto input = readCopyPathInputs();
    SetLastError(error);
    return input;
}
inline const CopyPathSelection& copyPathSelection() noexcept {
    const DWORD error = GetLastError();
    static const auto selection = selectCopyPath(copyPathReading());
    SetLastError(error);
    return selection;
}
// Fake single-chain defaults to synchronous/shared. AsyncCopyFrames=1/2 (or
// AsyncCopyPath) explicitly selects K1/K2; SyncCopyPath still wins.
// Old opt-in files remain valid;
// NativeBackend also gates both routes on its actual fake-swapchain record.
inline bool asyncCopyPath() noexcept { return copyPathSelection().async; }
inline bool sharedCopyPath() noexcept { return copyPathSelection().shared; }
// A numeric limit alone never enables the legacy route.
inline std::uint32_t asyncCopyFrames() noexcept {
    return copyPathReading().frames;
}
} // namespace xess_vk::diagnostic_flags
