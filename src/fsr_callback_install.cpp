#include "fsr_callback_install.hpp"
#include "research_build.hpp"
#include "fsr_detour_entry.hpp"
#include "upscale_detour_entry.hpp"
#include "fsr_hook_transaction.hpp"
#include "fsr_pe_guard.hpp"
#include "fsr_trampoline_guard.hpp"
#include "fsr_menu_control.hpp"
#include "fsr_local_fi_off.hpp"
#include "diagnostic_flags.hpp"
#include "sr_native_resolution.hpp"
#include "continuous_registration.hpp"
#include "graph_registration_bridge.hpp"
#include "game_frame_timing.hpp"
#include "game_frame_cap.hpp"
#include "launcher_preferences.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace xess_vk::fsr_callback_install {
namespace {
constexpr char unityHash[] = "9aa9397a6c5ee4fa083ea379b81ff14019eeba26a76d15446318d5947fb3d2d0";
constexpr DWORD unityImageSize = 36315136;
constexpr std::uint64_t maximumFileBytes = 35269688;
// The retired FI input parser remains unverified; current upscale is separate.
constexpr bool inputLayoutVerified = false;
constexpr char inputLayoutRefusal[] = "refused_unity_1_6_input_layout_unverified";
struct HookSpec {
    DWORD thunkRva, bodyRva;
    std::array<unsigned char, 16> thunkBytes;
    std::array<unsigned char, 18> bodyBytes;
    void (*entry)();
    void** published;
    const char* event;
    const char* summaryEvent;
    bool ordinary{};
    std::uint8_t copiedLength{5};
};
void* interpolationOriginal{};
void* composeOriginal{};
void* renderParamsOriginal{};
// Indices 5..11 are game timing (timingSpecs); the render-parameter rewrite follows them.
constexpr std::size_t renderParamsIndex = 5 + game_timing::hookCount;
constexpr std::size_t targetCount = fsr_hook_transaction::maximumTargetCount;
static_assert(renderParamsIndex + 1 == fsr_hook_transaction::localFiFirstIndex);
std::array<HookSpec, targetCount> specs{{
    {0xeb2230, 0xeb3b50,
     {0xe9,0x1b,0x19,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},
     {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xec,0xd0,0,0,0},
#if XESS_RESEARCH_DIAGNOSTICS
     &fsr_observer_entry, &fsr_original_trampoline,
#else
     nullptr, nullptr,
#endif
     "fsr_observer_install", "fsr_observer_install_summary"},
    {0xeb22e0, 0xeb49d0,
     {0xe9,0xeb,0x26,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},
     {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xec,0x30,0x01,0,0},
     &upscale_observer_entry, &upscale_original_trampoline, "upscale_observer_install", "upscale_observer_install_summary"},
    {0x493070,0x493070,
     {0x4c,0x89,0x4c,0x24,0x20,0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55}, {},
     reinterpret_cast<void(*)()>(&fsr_menu_control::interpolationEntry), &interpolationOriginal,
     "fsr_interpolation_gate_install", "fsr_interpolation_gate_install_summary", true},
    {0x48f8d0,0x48f8d0,
     {0x48,0x89,0x54,0x24,0x10,0x55,0x53,0x56,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8b}, {},
     reinterpret_cast<void(*)()>(&fsr_menu_control::composeEntry), &composeOriginal,
     "fsr_compose_gate_install", "fsr_compose_gate_install_summary", true},
    {0xeb2f50,0xeb2f50,
     {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x41,0x08,0x48,0x8b}, {},
     reinterpret_cast<void(*)()>(&grb_upscale_prepare_entry), &continuous_registration_original,
     "upscale_camera_registration_install", "upscale_camera_registration_install_summary", true}
}};
std::atomic<Sink> output{};
std::atomic<std::uint64_t> boundaries{}, selectedDevice{}, traceFailures{};
std::atomic<bool> stopRequested{};
std::atomic<bool> currentInstalled{};
std::mutex mutex;
thread_local bool insideApi = false, insideSink = false;
struct ErrorGuard { DWORD value = GetLastError(); ~ErrorGuard() { SetLastError(value); } };
struct ApiGuard { ApiGuard() { insideApi = true; } ~ApiGuard() { insideApi = false; } };
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct ModuleReference { HMODULE value{}; ~ModuleReference() { if (value) FreeLibrary(value); } };
struct State {
    const char* state = "not_attempted";
    bool requested{}, attempted{}, pinned{}, created{}, enableCalled{}, enabled{}, samplingAllowed{};
    bool trampolineVerified{}, trampolineFlushed{}, postBytesVerified{}, protectionRestored{}, targetFlushed{};
    bool createCalled{}, forwarderPublished{}, configured{}, preEnableVerified{};
    MH_STATUS createStatus = MH_UNKNOWN, enableStatus = MH_UNKNOWN;
    DWORD originalProtection{}, trampolineFlushError{}, targetFlushError{};
    std::uintptr_t base{}, target{}, body{}, trampoline{}, relay{};
};
std::array<State, targetCount> states;
struct SharedState {
    bool initializeCalled{};
    MH_STATUS initializeStatus = MH_UNKNOWN;
    fsr_pe_guard::Diagnostic peDiagnostic;
    fsr_pe_guard::Headers diskHeaders, liveHeaders;
    fsr_hook_transaction::Result transaction;
} shared;

bool readBytes(std::uintptr_t address, void* result, std::size_t size) noexcept {
    if (!address || size > std::numeric_limits<std::uintptr_t>::max() - address) return false;
    SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), result, size, &read) && read == size;
}
template<class T> bool readObject(std::uintptr_t address, T& result) noexcept { return readBytes(address, &result, sizeof(result)); }
bool samePath(HMODULE module, const wchar_t* expected) {
    std::vector<wchar_t> path(32768);
    const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    return size && size < path.size() && CompareStringOrdinal(path.data(), static_cast<int>(size), expected, -1, TRUE) == CSTR_EQUAL;
}
std::wstring gameSiblingForPath(const std::wstring& executable,const wchar_t* name){
    const auto slash=executable.find_last_of(L'\\');
    if(slash==std::wstring::npos||slash<3||executable[1]!=L':'||executable[2]!=L'\\'||
       CompareStringOrdinal(executable.c_str()+slash+1,-1,L"Endfield.exe",-1,TRUE)!=CSTR_EQUAL)return {};
    return executable.substr(0,slash+1)+name;
}
std::wstring gameSibling(const wchar_t* name){
    std::vector<wchar_t> path(32768);const auto size=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    return size&&size<path.size()?gameSiblingForPath(std::wstring(path.data(),size),name):std::wstring{};
}
bool fileRead(HANDLE file, std::uint64_t fileSize, std::uint64_t offset, void* result, DWORD size) noexcept {
    if (offset > fileSize || size > fileSize - offset) return false;
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD read{};
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && ReadFile(file, result, size, &read, nullptr) && read == size;
}
bool hashMatches(HANDLE file, std::uint64_t fileSize,const char* expected=unityHash) {
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) return false;
    // The hash object's backing allocation must outlive BCryptDestroyHash.
    std::vector<unsigned char> object;
    std::array<unsigned char, 64 * 1024> buffer{};
    struct Crypto {
        BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
        ~Crypto() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } crypto;
    DWORD objectBytes{}, returned{};
    if (BCryptOpenAlgorithmProvider(&crypto.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(crypto.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned, 0) < 0 ||
        returned != sizeof(objectBytes) || !objectBytes || objectBytes > 1024 * 1024) return false;
    object.resize(objectBytes);
    if (BCryptCreateHash(crypto.algorithm, &crypto.hash, object.data(), objectBytes, nullptr, 0, 0) < 0) return false;
    std::uint64_t total{};
    while (total < fileSize) {
        const auto wanted = static_cast<DWORD>((fileSize - total < buffer.size()) ? fileSize - total : buffer.size());
        DWORD read{};
        if (!ReadFile(file, buffer.data(), wanted, &read, nullptr) || read != wanted ||
            BCryptHashData(crypto.hash, buffer.data(), read, 0) < 0) return false;
        total += read;
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(crypto.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < digest.size(); ++i)
        if (expected[i * 2] != hex[digest[i] >> 4] || expected[i * 2 + 1] != hex[digest[i] & 15]) return false;
    return true;
}
using Headers = fsr_pe_guard::Headers;
bool textRange(const Headers& headers, DWORD rva, std::size_t size, std::uint64_t& rawOffset) noexcept {
    unsigned matches{};
    for (const auto& s : headers.sections) {
        if (rva < s.VirtualAddress || rva - s.VirtualAddress > s.Misc.VirtualSize || size > s.Misc.VirtualSize - (rva - s.VirtualAddress)) continue;
        if (std::memcmp(s.Name, ".text\0\0\0", 8) || (s.Characteristics & (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE)) !=
            (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE) || (s.Characteristics & IMAGE_SCN_MEM_WRITE)) return false;
        const auto within = rva - s.VirtualAddress;
        if (within > s.SizeOfRawData || size > s.SizeOfRawData - within) return false;
        rawOffset = static_cast<std::uint64_t>(s.PointerToRawData) + within;
        ++matches;
    }
    return matches == 1;
}
bool readOnlyRange(const Headers& headers, DWORD rva, std::size_t size, std::uint64_t& rawOffset) noexcept {
    unsigned matches{};
    for (const auto& section : headers.sections) {
        if (rva < section.VirtualAddress || rva - section.VirtualAddress > section.Misc.VirtualSize ||
            size > section.Misc.VirtualSize - (rva - section.VirtualAddress)) continue;
        const auto within = rva - section.VirtualAddress;
        if (!(section.Characteristics & IMAGE_SCN_MEM_READ) || (section.Characteristics & IMAGE_SCN_MEM_WRITE) ||
            within > section.SizeOfRawData || size > section.SizeOfRawData - within) return false;
        rawOffset = static_cast<std::uint64_t>(section.PointerToRawData) + within;
        ++matches;
    }
    return matches == 1;
}
bool codeRange(std::uintptr_t address, std::size_t size, HMODULE allocation, DWORD* protection = nullptr) noexcept {
    if (!address || size > std::numeric_limits<std::uintptr_t>::max() - address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    if (address < start || address - start > info.RegionSize || size > info.RegionSize - (address - start)) return false;
    const auto mode = info.Protect & 0xff;
    if (mode != PAGE_EXECUTE_READ && mode != PAGE_EXECUTE_READWRITE && mode != PAGE_EXECUTE_WRITECOPY) return false;
    if (allocation && (info.AllocationBase != allocation || info.Type != MEM_IMAGE || mode != PAGE_EXECUTE_READ)) return false;
    if (protection) *protection = info.Protect;
    return true;
}
template<std::size_t N> bool bytesEqual(std::uintptr_t address, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual{};
    return readBytes(address, actual.data(), actual.size()) && actual == expected;
}
#include "sr_native_resolution_install.inc"
bool absoluteJumpTo(std::uintptr_t address, std::uintptr_t destination) noexcept {
    std::array<unsigned char,14> bytes{};
    return readBytes(address,bytes.data(),bytes.size())&&fsr_trampoline_guard::absoluteJumpMatches(bytes,destination);
}
bool trampolineMatches(std::size_t index) noexcept {
    const auto& state = states[index]; const auto& spec = specs[index];
    if (index >= fsr_hook_transaction::localFiFirstIndex && index < fsr_hook_transaction::localFiTargetCount && state.configured)
        return fsr_local_fi_off::siteReady(index - fsr_hook_transaction::localFiFirstIndex);
    if (spec.ordinary) {
        const auto length = spec.copiedLength;
        std::array<unsigned char,35> bytes{};
        if ((length != 5 && length != 7) || !state.trampoline ||
            state.relay != state.trampoline + length + 14 ||
            !codeRange(state.trampoline, length + 28, nullptr) ||
            !readBytes(state.trampoline,bytes.data(),length+28)) return false;
        return fsr_trampoline_guard::ordinaryMatches(std::span(bytes).first(length+28),state.trampoline,
            state.target,reinterpret_cast<std::uintptr_t>(spec.entry),spec.thunkBytes,length);
    }
    // Pinned MinHook v1.3.4 converts this exact external E9 into one absolute
    // jump, then appends one absolute relay. Reject another instruction shape.
    return state.trampoline && state.relay == state.trampoline + 14 && codeRange(state.trampoline, 28, nullptr) &&
        absoluteJumpTo(state.trampoline, state.body) && absoluteJumpTo(state.relay, reinterpret_cast<std::uintptr_t>(spec.entry));
}
bool targetMatches(std::size_t index) noexcept {
    const auto& state = states[index]; const auto& spec = specs[index];
    std::array<unsigned char, 16> bytes{};
    void* published{};
    if (!readObject(reinterpret_cast<std::uintptr_t>(spec.published), published) ||
        reinterpret_cast<std::uintptr_t>(published) != state.trampoline) return false;
    if (!readBytes(state.target, bytes.data(), bytes.size()) || bytes[0] != 0xe9 ||
        std::memcmp(bytes.data() + 5, spec.thunkBytes.data() + 5, bytes.size() - 5)) return false;
    std::int32_t displacement{}; std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
    // Both pointers are validated Windows user-space addresses, far below INT64_MAX.
    const auto destination = static_cast<std::int64_t>(state.target) + 5 + displacement;
    return destination > 0 && static_cast<std::uintptr_t>(destination) == state.relay && trampolineMatches(index) &&
        (spec.ordinary || bytesEqual(state.body, spec.bodyBytes));
}
bool originalProtectionRestored(std::size_t index) noexcept {
    const auto& state = states[index];
    DWORD current{};
    return codeRange(state.target, specs[index].thunkBytes.size(), reinterpret_cast<HMODULE>(state.base), &current) && current == state.originalProtection;
}
void emit(std::size_t index, bool summary = false) noexcept {
    const auto& state = states[index]; const auto& spec = specs[index];
    const auto* event = summary ? spec.summaryEvent : spec.event;
    const auto sink = output.load(std::memory_order_acquire);
    if (!state.requested || !sink || insideSink) return;
    insideSink = true;
    try {
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << "{\"event\":\"" << event << "\",\"pid\":" << GetCurrentProcessId() << ",\"uptime_ms\":" << GetTickCount64()
          << ",\"thread_id\":" << GetCurrentThreadId() << ",\"device\":\"0x" << std::hex << selectedDevice.load() << std::dec
          << "\",\"state\":\"" << state.state << "\",\"requested\":" << (state.requested ? "true" : "false")
          << ",\"device_creation_boundaries\":" << boundaries.load() << ",\"attempted\":" << (state.attempted ? "true" : "false")
          << ",\"modules_pinned\":" << (state.pinned ? "true" : "false") << ",\"hook_created\":" << (state.created ? "true" : "false")
          << ",\"enable_called\":" << (state.enableCalled ? "true" : "false") << ",\"enable_succeeded\":" << (state.enabled ? "true" : "false")
          << ",\"sampling_allowed\":" << (state.samplingAllowed && !stopRequested.load() ? "true" : "false")
          << ",\"stop_requested\":" << (stopRequested.load() ? "true" : "false")
          << ",\"forwarder_published\":" << (state.forwarderPublished ? "true" : "false")
          << ",\"helper_configured_disabled\":" << (state.configured ? "true" : "false")
          << ",\"pre_enable_recheck_passed\":" << (state.preEnableVerified ? "true" : "false")
          << ",\"trampoline_verified_before_enable\":" << (state.trampolineVerified ? "true" : "false")
          << ",\"trampoline_flush_succeeded\":" << (state.trampolineFlushed ? "true" : "false")
          << ",\"trampoline_flush_error\":" << state.trampolineFlushError
          << ",\"post_enable_bytes_verified\":" << (state.postBytesVerified ? "true" : "false")
          << ",\"post_enable_protection_restored\":" << (state.protectionRestored ? "true" : "false")
          << ",\"target_flush_succeeded\":" << (state.targetFlushed ? "true" : "false") << ",\"target_flush_error\":" << state.targetFlushError;
        const auto status = [&s](const char* name, bool called, MH_STATUS result) {
            s << ",\"" << name << "\":";
            if (!called) s << "null";
            else s << "{\"code\":" << static_cast<int>(result) << ",\"name\":\"" << MH_StatusToString(result) << "\"}";
        };
        s << ",\"pe_guard\":";
        fsr_pe_guard::writeJson(s, shared.peDiagnostic, shared.diskHeaders, shared.liveHeaders);
        status("mh_initialize", shared.initializeCalled, shared.initializeStatus);
        status("mh_create", state.createCalled, state.createStatus);
        status("mh_enable", state.enableCalled, state.enableStatus);
        s << ",\"target_rva\":" << spec.thunkRva << ",\"body_rva\":" << spec.bodyRva
          << ",\"hook_check\":\"point_in_time_read_only\",\"hook_bytes_still_active\":";
        if (state.enableCalled) s << (targetMatches(index) ? "true" : "false"); else s << "null";
        s << ",\"target_protection_still_restored\":";
        if (state.enableCalled) s << (originalProtectionRestored(index) ? "true" : "false"); else s << "null";
        s << ",\"transaction_requested_mask\":" << shared.transaction.requestedMask
          << ",\"transaction_stage\":\"" << fsr_hook_transaction::stageName(shared.transaction.stage)
          << "\",\"transaction_failure\":\"" << fsr_hook_transaction::failureName(shared.transaction.failure)
          << "\",\"transaction_hook_index\":" << shared.transaction.hookIndex
          << ",\"transaction_completed\":" << (shared.transaction.completed ? "true" : "false");
        s << ",\"trace_failures\":" << traceFailures.load()
          << ",\"installation_boundary\":\"first_successful_vkCreateDevice_only\",\"all_threads_quiescent_verified\":false"
          << ",\"hot_retry\":false,\"hook_lifetime\":\"process\",\"game_frame_inputs_verified\":false,\"xefg_integrated\":false}";
        sink(s.str().c_str());
    } catch (...) { ++traceFailures; }
    insideSink = false;
}
void emitAll(bool summary = false) noexcept {
    for (std::size_t index = 0; index < states.size(); ++index) emit(index, summary);
}
void requestStop() noexcept {
    fsr_local_fi_off::stop();
    currentInstalled.store(false,std::memory_order_release);
    stopRequested.store(true, std::memory_order_release);
    if constexpr(researchDiagnostics)fsr_callback_observer::stopSampling();
    upscale_callback_observer::stopSampling();
    fsr_menu_control::stop();
    continuous_registration::stop();
    sr_observation::stop();
    game_timing::stop();
}
void stop(const char* reason) noexcept {
    requestStop();
    for (auto& state : states) {
        state.samplingAllowed = false;
        if (state.requested) state.state = reason;
    }
}
bool originalTargetMatches(std::size_t index, bool requirePublished) noexcept {
    const auto& state = states[index]; const auto& spec = specs[index];
    void* published{};
    return bytesEqual(state.target, spec.thunkBytes) && (spec.ordinary || bytesEqual(state.body, spec.bodyBytes)) &&
        originalProtectionRestored(index) && trampolineMatches(index) &&
        readObject(reinterpret_cast<std::uintptr_t>(spec.published), published) &&
        (requirePublished ? reinterpret_cast<std::uintptr_t>(published) == state.trampoline : published == nullptr);
}
struct Attempt {
    ModuleReference module;
    File file;
    std::uint64_t fileSize{};
    std::uintptr_t base{};
    upscale_callback_observer::SourceMetadataSink upscaleSource{};
    upscale_callback_observer::InvocationSink invocation{};
    game_timing::Sink timingSink{};
    game_timing::EnqueueSink enqueueSink{};
    void* timingUser{};
    continuous_registration::FrameReader frameReader{};
    sr_observation::ResolveImages srImages{};
    const char* refusal{};
    bool pureObservation{}, currentContinuous{}, localFi{}, srObservation{},srNativeObservation{};
    fsr_pe_guard::Headers diskHeaders, liveHeaders;
    fsr_pe_guard::Diagnostic peDiagnostic;

    bool reject(const char* reason) noexcept { refusal = reason; return false; }
    bool validateLocalBody() {
        if (!localFi) return true;
        const auto guards = fsr_local_fi_off::bodyGuards();
        if (guards.size() != 8) return reject("refused_local_fi_body_guard_count");
        for (const auto& guard : guards) {
            std::uint64_t raw{};
            if (!guard.bytes || !guard.size || guard.size > 128 ||
                guard.rva > unityImageSize || guard.size > unityImageSize - guard.rva ||
                !readOnlyRange(liveHeaders, guard.rva, guard.size, raw))
                return reject("refused_local_fi_body_range");
            std::array<unsigned char, 128> disk{}, live{};
            MEMORY_BASIC_INFORMATION region{};
            const auto address = base + guard.rva;
            if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) ||
                region.State != MEM_COMMIT || region.Type != MEM_IMAGE || region.AllocationBase != module.value ||
                (region.Protect != PAGE_READONLY && region.Protect != PAGE_EXECUTE_READ) ||
                address < reinterpret_cast<std::uintptr_t>(region.BaseAddress) ||
                address - reinterpret_cast<std::uintptr_t>(region.BaseAddress) > region.RegionSize ||
                guard.size > region.RegionSize - (address - reinterpret_cast<std::uintptr_t>(region.BaseAddress)) ||
                !fileRead(file.value, fileSize, raw, disk.data(), static_cast<DWORD>(guard.size)) ||
                !readBytes(address, live.data(), guard.size) ||
                std::memcmp(disk.data(), guard.bytes, guard.size) || std::memcmp(live.data(), guard.bytes, guard.size))
                return reject("refused_local_fi_body_bytes");
        }
        return true;
    }
    bool validateShared() {
        const auto unityPath=gameSibling(L"UnityPlayer.dll");
        if (unityPath.empty()) return reject("refused_process_path");
        if (!inputLayoutVerified && !pureObservation && !currentContinuous) return reject(inputLayoutRefusal);
        if (!GetModuleHandleExW(0, L"UnityPlayer.dll", &module.value)) return reject("refused_module_not_ready_at_device_creation");
        if (!samePath(module.value, unityPath.c_str())) return reject("refused_module_path");
        // Deny writes and replacement/deletion through the entire transaction.
        // Never load a missing module. The strong reference covers pre-pin reads.
        file.value = CreateFileW(unityPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER length{};
        if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &length) || length.QuadPart <= 0 ||
            static_cast<std::uint64_t>(length.QuadPart) > maximumFileBytes) return reject("refused_file_lock_or_size");
        fileSize = static_cast<std::uint64_t>(length.QuadPart);
        if (!hashMatches(file.value, fileSize)) return reject("refused_unity_sha256");
        base = reinterpret_cast<std::uintptr_t>(module.value);
        if (base > UINTPTR_MAX - unityImageSize) return reject("refused_module_address_bounds");
        struct DiskContext { HANDLE file; std::uint64_t size; } diskContext{file.value, fileSize};
        struct LiveContext { std::uintptr_t base; } liveContext{base};
        const fsr_pe_guard::Reader diskReader{&diskContext, [](void* context, std::uint64_t offset, void* result, std::uint32_t size) noexcept {
            const auto& disk = *static_cast<DiskContext*>(context);
            if (offset > disk.size || size > disk.size - offset)
                return fsr_pe_guard::ReadResult{false, 0, 0, "file_bounds", false};
            LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
            if (!SetFilePointerEx(disk.file, position, nullptr, FILE_BEGIN))
                return fsr_pe_guard::ReadResult{false, GetLastError(), 0, "SetFilePointerEx", true};
            DWORD read{};
            const BOOL okay = ReadFile(disk.file, result, size, &read, nullptr);
            const DWORD error = okay ? ERROR_SUCCESS : GetLastError();
            return fsr_pe_guard::ReadResult{okay != FALSE, error, read, "ReadFile", true};
        }};
        const fsr_pe_guard::Reader liveReader{&liveContext, [](void* context, std::uint64_t offset, void* result, std::uint32_t size) noexcept {
            const auto& live = *static_cast<LiveContext*>(context);
            if (offset > unityImageSize || size > unityImageSize - offset)
                return fsr_pe_guard::ReadResult{false, 0, 0, "image_bounds", false};
            SIZE_T read{};
            const BOOL okay = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(live.base + offset), result, size, &read);
            const DWORD error = okay ? ERROR_SUCCESS : GetLastError();
            return fsr_pe_guard::ReadResult{okay != FALSE, error, read, "ReadProcessMemory", true};
        }};
        if (!fsr_pe_guard::validate(diskReader, liveReader, base, unityImageSize,
                                  diskHeaders, liveHeaders, peDiagnostic))
            return reject(peDiagnostic.state);
        if (specs[0].entry == specs[1].entry || specs[0].published == specs[1].published)
            return reject("refused_shared_forwarder_storage");
        return validateLocalBody();
    }
    bool validateTarget(std::size_t index, State& state) {
        const auto& spec = specs[index];
        std::uint64_t rawThunk{}, rawBody{};
        if (!textRange(liveHeaders, spec.thunkRva, spec.thunkBytes.size(), rawThunk) ||
            (!spec.ordinary && !textRange(liveHeaders, spec.bodyRva, spec.bodyBytes.size(), rawBody))) return reject("refused_text_section_bounds");
        std::array<unsigned char, 16> diskThunk{}; std::array<unsigned char, 18> diskBody{};
        if (!fileRead(file.value, fileSize, rawThunk, diskThunk.data(), static_cast<DWORD>(diskThunk.size())) || diskThunk != spec.thunkBytes ||
            (!spec.ordinary && (!fileRead(file.value, fileSize, rawBody, diskBody.data(), static_cast<DWORD>(diskBody.size())) || diskBody != spec.bodyBytes)))
            return reject("refused_file_target_bytes");
        state.base = base; state.target = base + spec.thunkRva; state.body = base + spec.bodyRva;
        if (!codeRange(state.target, spec.thunkBytes.size(), module.value, &state.originalProtection) ||
            (!spec.ordinary && !codeRange(state.body, spec.bodyBytes.size(), module.value))) return reject("refused_live_code_protection");
        if (!bytesEqual(state.target, spec.thunkBytes) || (!spec.ordinary && !bytesEqual(state.body, spec.bodyBytes))) return reject("refused_live_target_bytes");
        void* published{};
        if (!readObject(reinterpret_cast<std::uintptr_t>(spec.published), published) || published)
            return reject("refused_existing_forwarder");
        return true;
    }
    bool prepareShared() {
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&installAtDeviceCreation), &pinned) ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(module.value), &pinned)) return reject("refused_module_pin");
        for (auto& state : states) if (state.requested) state.pinned = true;
        shared.initializeCalled = true; shared.initializeStatus = MH_Initialize();
        // Exactly one private initialization. ALREADY_INITIALIZED is refused;
        // never assume ownership of an unrelated hook set.
        if (shared.initializeStatus != MH_OK) return reject("refused_mh_initialize");
        if(diagnostic_flags::srReplaceRequested()&&GetModuleHandleW(L"GameAssembly.dll")&&!nativeResolution::prepare())return reject("refused_native_resolution_control");
        if (localFi) {
            if (!fsr_local_fi_off::prepare(base)) return reject("refused_local_fi_preparation");
            for (std::size_t i = 0; i < 2; ++i)
                specs[fsr_hook_transaction::localFiFirstIndex + i].entry =
                    reinterpret_cast<void(*)()>(fsr_local_fi_off::entry(i));
        }
        return true;
    }
    bool createTarget(std::size_t index) {
        auto& state = states[index]; const auto& spec = specs[index];
        void* trampoline{};
        state.createCalled = true;
        state.createStatus = MH_CreateHook(reinterpret_cast<void*>(state.target), reinterpret_cast<void*>(spec.entry), &trampoline);
        if (state.createStatus != MH_OK) return reject("refused_mh_create");
        state.created = true; state.trampoline = reinterpret_cast<std::uintptr_t>(trampoline);
        const std::size_t bytes = spec.ordinary ? spec.copiedLength + 28 : 28;
        if (!state.trampoline || state.trampoline > UINTPTR_MAX - bytes) return reject("refused_trampoline_address");
        for (std::size_t other = 0; other < states.size(); ++other)
            if (other != index && states[other].created && states[other].trampoline == state.trampoline)
                return reject("refused_shared_trampoline");
        state.relay = state.trampoline + (spec.ordinary ? spec.copiedLength + 14 : 14);
        state.trampolineVerified = trampolineMatches(index);
        if (!state.trampolineVerified || !originalTargetMatches(index, false)) return reject("refused_created_hook_verification");
        state.trampolineFlushed = FlushInstructionCache(GetCurrentProcess(), trampoline, bytes) != FALSE;
        if (!state.trampolineFlushed) { state.trampolineFlushError = GetLastError(); return reject("refused_trampoline_flush"); }
        return true;
    }
    bool publishTarget(std::size_t index) {
        auto& state = states[index]; const auto& spec = specs[index];
        // Both creates/verifications finish before any cell is published. Once
        // published, the pointer/trampoline/pins remain intact for process life.
        if (InterlockedCompareExchangePointer(spec.published, reinterpret_cast<void*>(state.trampoline), nullptr) != nullptr)
            return reject("refused_forwarder_publication_race");
        state.forwarderPublished = true; return true;
    }
    bool configureTarget(std::size_t index) {
        if (index == 0) {
            // The legacy helper has a void configuration API; its sticky state
            // is checked by enableSampling only after ALL target postchecks.
            if constexpr(researchDiagnostics)fsr_callback_observer::configureVerifiedModule(base, selectedDevice.load(), output.load());
            else return reject("legacy_fi_not_in_product_build");
        } else if (index == 1 && !upscale_callback_observer::configureVerifiedModule(base, selectedDevice.load(), output.load(), upscaleSource,
                                                                      upscale_callback_observer::defaultLimits, invocation))
            return reject("refused_upscale_sampling_configuration");
        // Every PublishTarget precedes every ConfigureTarget, so the
        // render-parameter original (index 12) is already published here.
        else if (index == 2 && !fsr_menu_control::configureVerified(
                    reinterpret_cast<fsr_menu_control::GateFunction>(interpolationOriginal),
                    reinterpret_cast<fsr_menu_control::GateFunction>(composeOriginal),
                    reinterpret_cast<fsr_menu_control::RenderParamsFunction>(renderParamsOriginal), true,
                     continuous_registration::observeCompose, localFi, localFi ? fsr_local_fi_off::active : nullptr,
                     diagnostic_flags::srReplaceRequested() ? nativeResolution::readGameplay : nullptr))
            return reject("refused_native_fi_gate_configuration");
        else if (index == 4 && !continuous_registration::configure(base, selectedDevice.load(), frameReader, timingUser))
            return reject("refused_camera_registration_configuration");
        else if (index == 5 && !game_timing::configure({base, timingUser, timingSink, nullptr, enqueueSink}))
            return reject("refused_game_timing_configuration");
        else if (index == fsr_hook_transaction::srFirstIndex &&
                 !sr_observation::configure(base,selectedDevice.load(),output.load(),frameReader,timingUser,srImages,diagnostic_flags::srReplaceRequested(),srNativeObservation,diagnostic_flags::srNativeReplacementAvailable()))
            return reject("refused_sr_observation_configuration");
        else if (index >= fsr_hook_transaction::localFiFirstIndex && index < fsr_hook_transaction::localFiTargetCount &&
                 !fsr_local_fi_off::configureSite(index - fsr_hook_transaction::localFiFirstIndex,
                     reinterpret_cast<void*>(states[index].trampoline), reinterpret_cast<void*>(states[index].target + 5)))
            return reject("refused_local_fi_configuration");
        states[index].configured = true; return true;
    }
    bool checkBeforeEnable(std::size_t index) {
        states[index].preEnableVerified = originalTargetMatches(index, true);
        if (!states[index].preEnableVerified) return reject("refused_pre_enable_verification");
        if (index >= fsr_hook_transaction::localFiFirstIndex && index < fsr_hook_transaction::localFiTargetCount &&
            !fsr_local_fi_off::siteReady(index - fsr_hook_transaction::localFiFirstIndex))
            return reject("refused_local_fi_site_not_ready");
        return true;
    }
    bool enableTarget(std::size_t index) {
        auto& state = states[index];
        // Recheck immediately before this patch as well as the all-target pass.
        if (!originalTargetMatches(index, true)) return reject("refused_pre_enable_target_changed");
        state.enableCalled = true;
        state.enableStatus = MH_EnableHook(reinterpret_cast<void*>(state.target));
        state.enabled = state.enableStatus == MH_OK;
        if (!state.enabled) return reject("stopped_mh_enable_failure");
        return true;
    }
    bool checkAfterEnable(std::size_t index) {
        auto& state = states[index];
        state.postBytesVerified = targetMatches(index);
        state.protectionRestored = originalProtectionRestored(index);
        state.targetFlushed = FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(state.target), 5) != FALSE;
        if (!state.targetFlushed) state.targetFlushError = GetLastError();
        if (!state.postBytesVerified || !state.protectionRestored || !state.targetFlushed)
            return reject("stopped_post_enable_verification_failure");
        if (index >= fsr_hook_transaction::localFiFirstIndex && index < fsr_hook_transaction::localFiTargetCount &&
            !fsr_local_fi_off::siteReady(index - fsr_hook_transaction::localFiFirstIndex))
            return reject("stopped_local_fi_site_not_ready");
        return true;
    }
    bool activateTarget(std::size_t index) {
        bool enabled = true;
        if (index == 0) {if constexpr(researchDiagnostics)enabled=fsr_callback_observer::enableSampling();else enabled=false;}
        else if (index == 1) enabled = upscale_callback_observer::enableSampling();
        else if (index == 2) enabled = fsr_menu_control::activate();
        else if (index == 4) continuous_registration::activate();
        else if (index == 5) enabled = game_timing::activate();
        else if (index == (srNativeObservation?fsr_hook_transaction::maximumTargetCount:fsr_hook_transaction::srTargetCount)-1) enabled = sr_observation::activate();
        if (!enabled) return reject("stopped_sampling_activation_refused");
        states[index].samplingAllowed = true;
        return true;
    }
    bool step(fsr_hook_transaction::Stage stage, std::size_t index) {
        using Stage = fsr_hook_transaction::Stage;
        switch (stage) {
        case Stage::ValidateShared: return validateShared();
        case Stage::ValidateTarget: return validateTarget(index,states[index]);
        case Stage::PrepareShared: return prepareShared();
        case Stage::CreateTarget: return createTarget(index);
        case Stage::PublishTarget: return publishTarget(index);
        case Stage::ConfigureTarget: return configureTarget(index);
        case Stage::CheckBeforeEnable: return checkBeforeEnable(index);
        case Stage::EnableTarget: return enableTarget(index);
        case Stage::CheckAfterEnable: return checkAfterEnable(index);
        case Stage::ActivateTarget: return activateTarget(index);
        case Stage::Complete: break;
        }
        return reject("refused_transaction_stage");
    }
};
#include "game_frame_cap_install.inc"
void finishAttempt(Attempt& attempt,void(*installCap)(Attempt&)=frameCap::install){
    // Optional hooks still need the validated headers and locked file.
    if(shared.transaction.completed)installCap(attempt);
    shared.diskHeaders=std::move(attempt.diskHeaders);shared.liveHeaders=std::move(attempt.liveHeaders);
    shared.peDiagnostic=attempt.peDiagnostic;
}
void attempt(upscale_callback_observer::SourceMetadataSink upscaleSource,
             upscale_callback_observer::InvocationSink invocation,
             game_timing::Sink timingSink, void* timingUser, continuous_registration::FrameReader frameReader,
             game_timing::EnqueueSink enqueueSink,sr_observation::ResolveImages srImages) {
    Attempt attempt;
    attempt.pureObservation=states[1].requested && !states[0].requested && !states[4].requested;
    attempt.currentContinuous=states[4].requested && !states[0].requested;
    attempt.localFi = states[fsr_hook_transaction::localFiFirstIndex].requested;
    attempt.upscaleSource = upscaleSource;
    attempt.invocation = invocation;
    attempt.timingSink = timingSink;
    attempt.enqueueSink = enqueueSink;
    attempt.timingUser = timingUser;
    attempt.frameReader = frameReader;
    attempt.srImages=srImages;attempt.srObservation=states[fsr_hook_transaction::srFirstIndex].requested;
    attempt.srNativeObservation=states[fsr_hook_transaction::srNativeFirstIndex].requested;
    const auto srMask=attempt.srNativeObservation?(attempt.localFi?fsr_hook_transaction::localFiSrNativeObservationMask:fsr_hook_transaction::srNativeObservationMask):
        (attempt.localFi?fsr_hook_transaction::localFiSrObservationMask:fsr_hook_transaction::srObservationMask);
    const auto mask = attempt.srObservation ? srMask :
        attempt.localFi ? fsr_hook_transaction::localFiOffMask :
        attempt.currentContinuous ? fsr_hook_transaction::currentContinuousMask :
        states[4].requested ? fsr_hook_transaction::timedContinuousMask :
        (states[0].requested ? fsr_hook_transaction::fiMask : 0) | (states[1].requested ? fsr_hook_transaction::upscaleMask : 0);
    shared.transaction = fsr_hook_transaction::run(mask, {
        &attempt,
        [](void* user, fsr_hook_transaction::Stage stage, std::size_t index) { return static_cast<Attempt*>(user)->step(stage, index); },
        [](void*) noexcept { return stopRequested.load(std::memory_order_acquire); },
        [](void*) noexcept { requestStop(); }
    });
    if (shared.transaction.completed && attempt.localFi &&
        (stopRequested.load(std::memory_order_acquire) || !attempt.validateLocalBody() ||
         !fsr_local_fi_off::activate() || stopRequested.load(std::memory_order_acquire))) {
        requestStop();
        shared.transaction.completed = false;
        shared.transaction.failure = fsr_hook_transaction::Failure::Rejected;
        shared.transaction.stage = fsr_hook_transaction::Stage::Complete;
        shared.transaction.hookIndex = fsr_hook_transaction::localFiOffSharedIndex;
        if (!attempt.refusal) attempt.refusal = "stopped_local_fi_activation_refused";
    }
    if(shared.transaction.completed&&diagnostic_flags::srReplaceRequested()&&nativeResolution::created&&!nativeResolution::activate()){
        requestStop();shared.transaction.completed=false;attempt.refusal="refused_native_resolution_activation";
    }
    finishAttempt(attempt);
    if (shared.transaction.completed) {
        currentInstalled.store(attempt.currentContinuous,std::memory_order_release);
        for (auto& state : states) if (state.requested) state.state = "installed_at_first_device_creation";
        return;
    }
    const auto failure = shared.transaction.failure;
    const auto* reason = failure == fsr_hook_transaction::Failure::Exception ? "stopped_installer_exception" :
        failure == fsr_hook_transaction::Failure::Stopped ? "stopped_during_installation" :
        attempt.refusal ? attempt.refusal : "refused_transaction_contract";
    if (failure == fsr_hook_transaction::Failure::Exception) ++traceFailures;
    // The runner already stopped both helpers. Preserve the failing target's
    // reason, and distinguish an otherwise-prepared peer that cannot activate.
    for (std::size_t index = 0; index < states.size(); ++index) if (states[index].requested) {
        states[index].samplingAllowed = false;
        const auto sharedIndex = fsr_hook_transaction::sharedFor(mask);
        states[index].state = shared.transaction.hookIndex == sharedIndex ||
            shared.transaction.hookIndex == index ? reason : "stopped_peer_hook_transaction_failure";
    }
}
bool requested(const wchar_t* name) noexcept {
    wchar_t option[8]{};
    return GetEnvironmentVariableW(name, option, 8) == 1 && option[0] == L'1';
}
void timingSpecs() noexcept {
    const auto& sites=game_timing::hookSites();
    for (std::size_t i=0;i<sites.size();++i) {
        const auto& site=sites[i];
        specs[i+5]={static_cast<DWORD>(site.targetRva),static_cast<DWORD>(site.targetRva),site.expectedPrefix,{},
            reinterpret_cast<void(*)()>(const_cast<void*>(site.entry)),site.originalTrampoline,
            "game_frame_timing_install","game_frame_timing_install_summary",true,site.copiedInstructionLength};
    }
    // mov rax,rsp; mov [rax+20],rbx; push rdi; sub rsp,170; movaps...
    specs[renderParamsIndex]={fsr_menu_control::renderParamsRva,fsr_menu_control::renderParamsRva,
        {0x48,0x8b,0xc4,0x48,0x89,0x58,0x20,0x57,0x48,0x81,0xec,0x70,0x01,0x00,0x00,0x0f},{},
        reinterpret_cast<void(*)()>(&fsr_menu_control::renderParamsEntry),&renderParamsOriginal,
        "fsr_render_params_install","fsr_render_params_install_summary",true,7};
    constexpr std::array<std::array<unsigned char, 16>, 2> localPrefixes{{
        {0x41,0x80,0x7d,0x3b,0x00,0x75,0x14,0x48,0x8b,0x85,0xd8,0xb1,0x00,0x00,0xc6,0x45},
        {0x41,0x80,0x7d,0x3b,0x00,0x0f,0x85,0x4b,0x01,0x00,0x00,0x48,0x8d,0x95,0xf0,0x3a}}};
    for (std::size_t i = 0; i < localPrefixes.size(); ++i)
        specs[fsr_hook_transaction::localFiFirstIndex + i] = {
            fsr_local_fi_off::siteRvas[i], fsr_local_fi_off::siteRvas[i], localPrefixes[i], {},
            reinterpret_cast<void(*)()>(fsr_local_fi_off::entry(i)), fsr_local_fi_off::originalSlot(i),
            "fsr_local_fi_off_install", "fsr_local_fi_off_install_summary", true, 5};
    specs[fsr_hook_transaction::srFirstIndex]={0xEB23A0,0xEB5E10,
        {0xe9,0x6b,0x3a,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xec,0xb0,0x01,0,0},
        &sr_accumulate_observer_entry,&sr_accumulate_original_trampoline,"sr_accumulate_observer_install","sr_accumulate_observer_install_summary"};
    specs[fsr_hook_transaction::srFirstIndex+1]={0xEB2210,0xEB3880,
        {0xe9,0x6b,0x16,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xec,0x90,0,0,0},
        &sr_rcas_observer_entry,&sr_rcas_original_trampoline,"sr_rcas_observer_install","sr_rcas_observer_install_summary"};
    for(std::size_t i=0;i<2;++i)specs[fsr_hook_transaction::srFirstIndex+2+i]={i?0xEB2870u:0xEB36A0u,i?0xEB2870u:0xEB36A0u,
        {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x41,0x08,0x48,0x8b},{},
        reinterpret_cast<void(*)()>(i?&grb_sr_rcas_entry:&grb_sr_accumulate_entry),i?&sr_rcas_registration_original:&sr_accumulate_registration_original,
        "sr_pass_registration_install","sr_pass_registration_install_summary",true,5};
    struct NativeSpec {upscale_tail_probe::Pass pass;void(*entry)();void** original;
        std::uint64_t(*registration)(void*,std::uint32_t,const void*);void** registrationOriginal;
        std::array<unsigned char,16> thunk;std::array<unsigned char,18> body;bool longRegistration;};
    const std::array<NativeSpec,6> native{{
        {upscale_tail_probe::Pass::Reactive,&sr_reactive_observer_entry,&sr_reactive_original_trampoline,&grb_sr_reactive_entry,&sr_reactive_registration_original,
         {0xe9,0x1b,0x1e,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},
         {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xec,0xb0,0,0,0},true},
        {upscale_tail_probe::Pass::Transparency,&sr_transparency_observer_entry,&sr_transparency_original_trampoline,&grb_sr_transparency_entry,&sr_transparency_registration_original,
         {0xe9,0xdb,0x20,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},
         {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xec,0xd0,0,0,0},false},
        {upscale_tail_probe::Pass::Pyramid,&sr_pyramid_observer_entry,&sr_pyramid_original_trampoline,&grb_sr_pyramid_entry,&sr_pyramid_registration_original,
         {0xe9,0xeb,0x28,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},
         {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xec,0x90,1,0,0},true},
        {upscale_tail_probe::Pass::ShadingChange,&sr_shading_change_observer_entry,&sr_shading_change_original_trampoline,&grb_sr_shading_change_entry,&sr_shading_change_registration_original,
         {0xe9,0xeb,0x2d,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},
         {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x70,0x48,0x8b,0xda},true},
        {upscale_tail_probe::Pass::PrepareReactivity,&sr_reactivity_observer_entry,&sr_reactivity_original_trampoline,&grb_sr_reactivity_entry,&sr_reactivity_registration_original,
         {0xe9,0x1b,0x36,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},
         {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xec,0xb0,1,0,0},false},
        {upscale_tail_probe::Pass::LumaInstability,&sr_instability_observer_entry,&sr_instability_original_trampoline,&grb_sr_instability_entry,&sr_instability_registration_original,
         {0xe9,0x3b,0x38,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},
         {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xec,0x50,1,0,0},false}
    }};
    for(std::size_t i=0;i<native.size();++i){const auto& n=native[i];const auto profile=upscale_tail_probe::profile(n.pass);
        specs[fsr_hook_transaction::srNativeFirstIndex+2*i]={DWORD(profile.thunk),DWORD(profile.body),n.thunk,n.body,n.entry,n.original,
            "sr_native_job_observer_install","sr_native_job_observer_install_summary"};
        const std::array<unsigned char,16> registration=n.longRegistration?
            std::array<unsigned char,16>{0x48,0x89,0x5c,0x24,8,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48}:
            std::array<unsigned char,16>{0x48,0x89,0x5c,0x24,8,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x41,8,0x48,0x8b};
        specs[fsr_hook_transaction::srNativeFirstIndex+2*i+1]={DWORD(profile.registration),DWORD(profile.registration),registration,{},
            reinterpret_cast<void(*)()>(n.registration),n.registrationOriginal,"sr_native_job_registration_install","sr_native_job_registration_install_summary",true,5};
    }
}
} // namespace

bool currentContinuousRequested() noexcept {
    ErrorGuard error;
    for(const auto* name:{L"XESS_FSR_UPSCALE_OBSERVER",L"XESS_CONTINUOUS_INPUTS",L"XESS_FSR_NATIVE_OFF",
                          L"XESS_BRIDGE_DEVICE",L"XESS_PRESENTATION_OFF"})
        if(!requested(name))return false;
    for(const auto* name:{L"XESS_FSR_OBSERVER",L"XESS_RAWCOLOR_CAPTURE",L"XESS_COMPUTE_OBSERVER",L"XESS_SL_OBSERVER"})
        if(requested(name))return false;
    wchar_t sdk[32768]{};const auto length=GetEnvironmentVariableW(L"XESS_SDK_BIN",sdk,32768);
    return length>=3 && length<32768 && sdk[1]==L':' && (sdk[2]==L'\\'||sdk[2]==L'/');
}
bool currentContinuousProfileReady() noexcept {
    ErrorGuard error;
    if(insideApi || !currentContinuousRequested())return false;
    ApiGuard api;
    try {
        std::lock_guard lock(mutex);
        if(boundaries.load() || stopRequested.load())return false;
        timingSpecs();Attempt attempt;attempt.currentContinuous=true;
        attempt.localFi = diagnostic_flags::localFiOff();
        attempt.srNativeObservation=(diagnostic_flags::srFullGraphObservation()||diagnostic_flags::srNativeReplacementAvailable());
        attempt.srObservation=(diagnostic_flags::srObservation()||diagnostic_flags::srReplaceRequested()||attempt.srNativeObservation);
        if(!attempt.validateShared())return false;
        const auto srMask=attempt.srNativeObservation?(attempt.localFi?fsr_hook_transaction::localFiSrNativeObservationMask:fsr_hook_transaction::srNativeObservationMask):
            (attempt.localFi?fsr_hook_transaction::localFiSrObservationMask:fsr_hook_transaction::srObservationMask);
        const auto mask=attempt.srObservation?srMask:
            attempt.localFi?fsr_hook_transaction::localFiOffMask:fsr_hook_transaction::currentContinuousMask;
        for(std::size_t i=1;i<states.size();++i) if(mask&(1u<<i)) {
            State local;
            if(!attempt.validateTarget(i,local))return false;
        }
        return true;
    } catch(...) {return false;}
}
bool currentContinuousReady(std::uint64_t device) noexcept {
    ErrorGuard error;
    return device && selectedDevice.load()==device && currentInstalled.load(std::memory_order_acquire) && !stopRequested.load();
}
std::uintptr_t currentContinuousPresentSlot(std::uint64_t device, std::uintptr_t base) noexcept {
    ErrorGuard error;
    if (insideApi || !currentContinuousReady(device)) return 0;
    ApiGuard api;
    try {
        std::lock_guard lock(mutex);
        if (!currentContinuousReady(device) || !states[1].pinned || states[1].base != base) return 0;
        constexpr auto rva = game_timing::presentFunctionSlotRva;
        if (rva > unityImageSize - sizeof(void*) || base > UINTPTR_MAX - rva) return 0;
        for (const auto& section : shared.liveHeaders.sections) {
            const auto start = static_cast<std::uint64_t>(section.VirtualAddress);
            const auto end = start + section.Misc.VirtualSize;
            if (rva >= start && rva + sizeof(void*) <= end &&
                (section.Characteristics & (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE)) ==
                    (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE) &&
                !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE))
                return base + rva;
        }
    } catch (...) {}
    return 0;
}

void installAtDeviceCreation(std::uint64_t device, Sink sink, upscale_callback_observer::SourceMetadataSink upscaleSource,
                            upscale_callback_observer::InvocationSink invocation,
                            game_timing::Sink timingSink, void* timingUser,
                            continuous_registration::FrameReader frameReader,
                            game_timing::EnqueueSink enqueueSink, bool currentContinuousAdmission,sr_observation::ResolveImages srImages) noexcept {
    ErrorGuard error;
    if (insideApi) return;
    ApiGuard api;
    const auto boundary = boundaries.fetch_add(1) + 1;
    if (boundary > 1) requestStop(); // No mutex wait before revoking BOTH gates.
    try {
        std::lock_guard lock(mutex);
        if (boundary > 1) { stop("stopped_additional_device_creation"); emitAll(); return; }
        selectedDevice.store(device); output.store(sink, std::memory_order_release);
        if(launcher_preferences::startupFrameCap().requested){game_frame_cap::requested=true;game_frame_cap::state="awaiting_verified_unity_hooks";}
        states[0].requested = requested(L"XESS_FSR_OBSERVER");
        states[1].requested = requested(L"XESS_FSR_UPSCALE_OBSERVER");
        const bool continuous = requested(L"XESS_CONTINUOUS_INPUTS");
        for (std::size_t i = 2; i < fsr_hook_transaction::timedTargetCount; ++i) states[i].requested = continuous;
        const bool localFi = continuous && currentContinuousRequested() && diagnostic_flags::localFiOff();
        for (std::size_t i = fsr_hook_transaction::localFiFirstIndex; i < fsr_hook_transaction::localFiTargetCount; ++i) states[i].requested = localFi;
        const bool fullSr=(diagnostic_flags::srFullGraphObservation()||diagnostic_flags::srNativeReplacementAvailable());
        const bool sr=continuous&&currentContinuousRequested()&&(diagnostic_flags::srObservation()||diagnostic_flags::srReplaceRequested()||fullSr);
        for(std::size_t i=fsr_hook_transaction::srFirstIndex;i<fsr_hook_transaction::srTargetCount;++i)states[i].requested=sr;
        for(std::size_t i=fsr_hook_transaction::srNativeFirstIndex;i<states.size();++i)states[i].requested=sr&&fullSr;
        if (continuous) timingSpecs();
        for (auto& state : states) if (!state.requested) state.state = "disabled";
        if (states[0].requested && continuous && !inputLayoutVerified) stop(inputLayoutRefusal);
        else if (continuous && (!currentContinuousRequested() || !invocation || !timingSink || !timingUser || !frameReader || !enqueueSink))
            stop("refused_current_continuous_contract");
        else if (continuous && !currentContinuousAdmission) stop("refused_current_continuous_device_admission");
        else if(sr&&!srImages)stop("refused_sr_observation_image_resolver");
        else if (!continuous && states[1].requested && !states[0].requested &&
            (invocation || timingSink || frameReader || enqueueSink || requested(L"XESS_FSR_NATIVE_OFF") ||
             requested(L"XESS_BRIDGE_DEVICE") || requested(L"XESS_PRESENTATION_OFF") || requested(L"XESS_RAWCOLOR_CAPTURE")))
            stop("refused_upscale_observation_conflict");
        else if (!states[0].requested && !states[1].requested) {} // Preserve the silent default-off path.
        else if (!device || !sink) stop("refused_missing_device_or_sink");
        else if (states[1].requested && !upscaleSource) stop("refused_missing_upscale_source_sink");
        else {
            for (auto& state : states) if (state.requested) state.attempted = true;
            attempt(upscaleSource, invocation, timingSink, timingUser, frameReader, enqueueSink,srImages);
        }
        emitAll();
    } catch (...) {
        ++traceFailures; requestStop();
        // Retain every published pointer, created trampoline and module pin.
        try { std::lock_guard lock(mutex); stop("stopped_installer_exception"); emitAll(); }
        catch (...) { ++traceFailures; }
    }
}
bool ensureNativeResolution(std::uint64_t device) noexcept {
    if(sr_native_resolution::control.installed.load(std::memory_order_acquire))return true;
    if(!diagnostic_flags::srReplaceRequested()||!currentContinuousReady(device))return false;
    static std::atomic<bool> failed{false};
    try{std::lock_guard lock(mutex);
        if(sr_native_resolution::control.installed.load())return true;
        if(failed||!GetModuleHandleW(L"GameAssembly.dll"))return false;
        // Some Unity startup orders load IL2CPP after graphics creation. One
        // deferred attachment at a recording boundary; no worker or retry loop.
        failed=(!nativeResolution::created&&!nativeResolution::prepare())||!nativeResolution::activate();
        if(failed){if(auto sink=output.load())sink("{\"event\":\"sr_native_resolution_hook_refused\"}");}
        return !failed;
    }catch(...){failed=true;return false;}
}
void onDeviceDestroyed(std::uint64_t device) noexcept {
    ErrorGuard error;
    if (!device || device != selectedDevice.load() || insideApi) return;
    ApiGuard api;
    requestStop();
    try { std::lock_guard lock(mutex); stop("stopped_selected_device_destroyed"); emitAll(); }
    catch (...) { ++traceFailures; }
}
void summary() noexcept {
    ErrorGuard error;
    if (insideApi || !boundaries.load()) return;
    ApiGuard api;
    try { std::lock_guard lock(mutex); emitAll(true); sr_observation::summary(); }
    catch (...) { ++traceFailures; }
}
} // namespace xess_vk::fsr_callback_install
