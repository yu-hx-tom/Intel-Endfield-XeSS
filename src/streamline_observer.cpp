#include "streamline_observer.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace xess_vk::streamline_observer {
namespace {
constexpr wchar_t gamePath[] = L"D:\\games\\Hypergryph Launcher\\games\\Arknights Endfield\\Endfield.exe";
constexpr wchar_t unityPath[] = L"D:\\games\\Hypergryph Launcher\\games\\Arknights Endfield\\UnityPlayer.dll";
constexpr wchar_t slPath[] = L"D:\\games\\Hypergryph Launcher\\games\\Arknights Endfield\\sl.interposer.dll";
constexpr char unityHash[] = "bee7be52370adddd67ba61e4937ca51b7f272656841d187e95e505496da798d1";
constexpr DWORD unityImageSize = 34119680;
constexpr std::array<DWORD, 3> slotRvas{0x1cf5b28, 0x1cf5b40, 0x1cf5b68};
constexpr std::array<const char*, 3> exportNames{"slSetTagForFrame", "slSetConstants", "slGetNewFrameToken"};
constexpr uint64_t tagSamples = 128, tokenSamples = 32, constantsSamples = 32;
std::atomic<Sink> output{};
std::atomic<PFun_slSetTagForFrame*> originalTags{};
std::atomic<PFun_slSetConstants*> originalConstants{};
std::atomic<PFun_slGetNewFrameToken*> originalToken{};
std::atomic<uint64_t> tagsCalls{}, constantsCalls{}, tokenCalls{}, tagsTotal{}, emitted{}, traceFailures{};
std::mutex installMutex;
InstallStatus status = InstallStatus::NotReady;
std::atomic<bool> checkedIdentity{};
bool identityValid = false, checkedHash = false, hashValid = false;
std::string lastState;
#ifndef XESS_SL_OBSERVER_TEST
HMODULE slModule{};
DWORD slImageSize{};
#endif
std::atomic<unsigned> installedMask{};
std::array<std::atomic<void**>, 3> observedSlots{};
thread_local bool inSink = false;

bool readBytes(const void* address, void* result, size_t size) noexcept {
    SIZE_T read = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), address, result, size, &read) && read == size;
}
template<class T> bool readObject(const T* address, T& result) noexcept { return readBytes(address, &result, sizeof(T)); }
std::string pointer(const void* p) { std::ostringstream s; s << "\"0x" << std::hex << reinterpret_cast<uintptr_t>(p) << '"'; return s.str(); }
void emit(const std::string& line) noexcept {
    const auto sink = output.load(std::memory_order_acquire);
    if (!sink || inSink) return;
    inSink = true;
    try {
        // Append correlation metadata uniformly, including install and summary.
        std::string record = line;
        if (record.empty() || record.back() != '}') { ++traceFailures; inSink = false; return; }
        record.pop_back();
        record += ",\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"uptime_ms\":" + std::to_string(GetTickCount64())
                + ",\"thread_id\":" + std::to_string(GetCurrentThreadId()) + '}';
        sink(record.c_str()); ++emitted;
    } catch (...) { ++traceFailures; }
    inSink = false;
}
void state(const char* reason) {
    if (lastState == reason) return;
    lastState = reason;
    emit(std::string("{\"event\":\"sl_observer_install\",\"state\":\"") + reason + "\",\"installedMask\":" + std::to_string(installedMask.load()) + "}");
}
bool executable(const void* p) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (!p || !VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD mode = info.Protect & 0xff;
    return mode == PAGE_EXECUTE || mode == PAGE_EXECUTE_READ || mode == PAGE_EXECUTE_READWRITE || mode == PAGE_EXECUTE_WRITECOPY;
}
bool samePath(HMODULE module, const wchar_t* expected) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    return length && length < path.size() && CompareStringOrdinal(path.data(), static_cast<int>(length), expected, -1, TRUE) == CSTR_EQUAL;
}
bool imageHeaders(HMODULE module, IMAGE_NT_HEADERS64& nt, std::vector<IMAGE_SECTION_HEADER>& sections) {
    IMAGE_DOS_HEADER dos{};
    if (!readObject(reinterpret_cast<const IMAGE_DOS_HEADER*>(module), dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 || dos.e_lfanew > 1024 * 1024) return false;
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    if (!readObject(reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) || !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96 ||
        nt.OptionalHeader.SizeOfImage < nt.OptionalHeader.SizeOfHeaders) return false;
    const size_t offset = static_cast<size_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64);
    const size_t bytes = nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (offset > nt.OptionalHeader.SizeOfHeaders || bytes > nt.OptionalHeader.SizeOfHeaders - offset) return false;
    sections.resize(nt.FileHeader.NumberOfSections);
    return readBytes(base + offset, sections.data(), bytes);
}
bool hashMatches() {
    struct File { HANDLE value; ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } file{
        CreateFileW(unityPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.value, &length) || length.QuadPart <= 0 || length.QuadPart > 64 * 1024 * 1024) return false;
    // BCryptDestroyHash must run before the caller-owned hash object buffer dies.
    std::vector<unsigned char> object, data(64 * 1024);
    struct Crypto {
        BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
        ~Crypto() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } crypto;
    auto& algorithm = crypto.algorithm; auto& hash = crypto.hash;
    bool okay = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    DWORD objectBytes = 0, resultBytes = 0;
    if (okay) okay = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &resultBytes, 0) >= 0 && objectBytes <= 1024 * 1024;
    object.resize(objectBytes);
    std::array<unsigned char, 32> digest{};
    if (okay) okay = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) >= 0;
    while (okay) {
        DWORD bytes = 0;
        if (!ReadFile(file.value, data.data(), static_cast<DWORD>(data.size()), &bytes, nullptr)) { okay = false; break; }
        if (!bytes) break;
        okay = BCryptHashData(hash, data.data(), bytes, 0) >= 0;
    }
    if (okay) okay = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (!okay) return false;
    constexpr char hex[] = "0123456789abcdef";
    std::string value; value.reserve(64);
    for (const auto byte : digest) { value += hex[byte >> 4]; value += hex[byte & 15]; }
    return value == unityHash;
}
bool validBase(const sl::BaseStructure& value, const sl::StructType& type) noexcept { return value.structType == type && value.structVersion >= 1 && value.structVersion <= 2; }
bool frameNumber(const sl::FrameToken& frame, uint32_t& value) noexcept {
    // The official class has one virtual conversion, no virtual destructor, and
    // the MS x64 vptr precedes its non-polymorphic 32-byte BaseStructure.
    void** vtable{}; void* function{}; sl::BaseStructure base(sl::FrameToken::s_structType, 1);
    if (!readBytes(&frame, &vtable, sizeof(vtable)) || !readBytes(vtable, &function, sizeof(function)) || !executable(function) ||
        !readBytes(reinterpret_cast<const unsigned char*>(&frame) + sizeof(void*), &base, sizeof(base)) || !validBase(base, sl::FrameToken::s_structType)) return false;
#ifndef XESS_SL_OBSERVER_TEST
    const auto at = reinterpret_cast<uintptr_t>(function), start = reinterpret_cast<uintptr_t>(slModule);
    if (!start || at < start || at - start >= slImageSize) return false;
#endif
    try { value = static_cast<uint32_t>(frame); return true; } catch (...) { return false; }
}
void frameFields(std::ostringstream& s, const sl::FrameToken& frame, const sl::ViewportHandle& viewport) {
    s << ",\"token\":" << pointer(&frame) << ",\"frame\":";
    uint32_t number = 0;
    if (frameNumber(frame, number)) s << number; else s << "null";
    sl::ViewportHandle copy;
    s << ",\"viewport\":";
    if (readObject(&viewport, copy) && validBase(copy, sl::ViewportHandle::s_structType)) s << static_cast<uint32_t>(copy); else s << "null";
}
void floating(std::ostringstream& s, float value) { if (std::isfinite(value)) s << value; else s << "null"; }
void matrix(std::ostringstream& s, const sl::float4x4& value) {
    std::array<float, 16> elements{}; std::memcpy(elements.data(), &value, sizeof(elements));
    s << '['; for (size_t i = 0; i < elements.size(); ++i) { if (i) s << ','; floating(s, elements[i]); } s << ']';
}
bool sampleTags(uint64_t call) noexcept {
    // 32 initial calls plus 96 spaced samples retain scene-entry coverage while
    // bounding process-lifetime logging to 128 calls even under concurrency.
    return call <= 32 || (call % 1200 == 0 && call / 1200 <= tagSamples - 32);
}
bool sampleConstants(uint64_t call) noexcept { return call <= 2 || (call % 600 == 0 && call / 600 <= constantsSamples - 2); }
sl::Result traceTags(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t count, sl::CommandBuffer* commands) {
    const DWORD incomingError = GetLastError();
    const auto original = originalTags.load(std::memory_order_acquire);
    const auto call = ++tagsCalls; tagsTotal += count;
    std::string before;
    try {
        if (sampleTags(call)) {
            std::ostringstream s; s.imbue(std::locale::classic());
            s << "{\"event\":\"sl_tags\",\"call\":" << call; frameFields(s, frame, viewport);
            s << ",\"commandBuffer\":" << pointer(commands) << ",\"tagCount\":" << count << ",\"tags\":[";
            for (uint32_t i = 0; i < count && i < 16; ++i) {
                if (i) s << ',';
                sl::ResourceTag tag;
                if (!readObject(tags ? tags + i : nullptr, tag) || !validBase(tag, sl::ResourceTag::s_structType)) { s << "{\"readable\":false}"; continue; }
                s << "{\"type\":" << tag.type << ",\"lifecycle\":" << static_cast<unsigned>(tag.lifecycle)
                  << ",\"extent\":[" << tag.extent.left << ',' << tag.extent.top << ',' << tag.extent.width << ',' << tag.extent.height << ']';
                sl::Resource resource;
                if (readObject(tag.resource, resource) && validBase(resource, sl::Resource::s_structType)) {
                    s << ",\"native\":" << pointer(resource.native) << ",\"view\":" << pointer(resource.view)
                      << ",\"resourceType\":" << static_cast<unsigned>(resource.type) << ",\"state\":" << resource.state
                      << ",\"format\":" << resource.nativeFormat << ",\"width\":" << resource.width << ",\"height\":" << resource.height
                      << ",\"mips\":" << resource.mipLevels << ",\"layers\":" << resource.arrayLayers << ",\"usage\":" << resource.usage << ",\"flags\":" << resource.flags;
                } else s << ",\"resourceReadable\":false";
                s << '}';
            }
            s << "],\"sampleTruncated\":" << (count > 16 ? "true" : "false"); before = s.str();
        }
    } catch (...) { ++traceFailures; }
    // Every installed wrapper has a published original before its slot is changed.
    SetLastError(incomingError);
    const auto result = original(frame, viewport, tags, count, commands);
    const DWORD outgoingError = GetLastError();
    if (!before.empty()) { try { emit(before + ",\"result\":" + std::to_string(static_cast<int>(result)) + '}'); } catch (...) { ++traceFailures; } }
    SetLastError(outgoingError);
    return result;
}
sl::Result traceConstants(const sl::Constants& constants, const sl::FrameToken& frame, const sl::ViewportHandle& viewport) {
    const DWORD incomingError = GetLastError();
    const auto original = originalConstants.load(std::memory_order_acquire);
    const auto call = ++constantsCalls;
    std::string before;
    try {
        if (sampleConstants(call)) {
            std::ostringstream s; s.imbue(std::locale::classic()); s << std::setprecision(9);
            s << "{\"event\":\"sl_constants\",\"call\":" << call; frameFields(s, frame, viewport);
            sl::Constants copy;
            // Version 1 ends before the v2 depth-separation field.
            const auto v1Size = static_cast<size_t>(reinterpret_cast<const unsigned char*>(&copy.minRelativeLinearDepthObjectSeparation) - reinterpret_cast<const unsigned char*>(&copy));
            if (readBytes(&constants, &copy, v1Size) && validBase(copy, sl::Constants::s_structType)) {
                s << ",\"version\":" << copy.structVersion << ",\"jitter\":["; floating(s, copy.jitterOffset.x); s << ','; floating(s, copy.jitterOffset.y);
                s << "],\"motionScale\":["; floating(s, copy.mvecScale.x); s << ','; floating(s, copy.mvecScale.y);
                s << "],\"depthInverted\":" << static_cast<unsigned>(copy.depthInverted) << ",\"cameraMotionIncluded\":" << static_cast<unsigned>(copy.cameraMotionIncluded)
                  << ",\"motionVectorsJittered\":" << static_cast<unsigned>(copy.motionVectorsJittered) << ",\"reset\":" << static_cast<unsigned>(copy.reset);
                s << ",\"near\":"; floating(s, copy.cameraNear); s << ",\"far\":"; floating(s, copy.cameraFar); s << ",\"fov\":"; floating(s, copy.cameraFOV);
                s << ",\"cameraViewToClip\":"; matrix(s, copy.cameraViewToClip); s << ",\"clipToCameraView\":"; matrix(s, copy.clipToCameraView);
                s << ",\"clipToPrevClip\":"; matrix(s, copy.clipToPrevClip); s << ",\"prevClipToClip\":"; matrix(s, copy.prevClipToClip);
            } else s << ",\"readable\":false";
            before = s.str();
        }
    } catch (...) { ++traceFailures; }
    SetLastError(incomingError);
    const auto result = original(constants, frame, viewport);
    const DWORD outgoingError = GetLastError();
    if (!before.empty()) { try { emit(before + ",\"result\":" + std::to_string(static_cast<int>(result)) + '}'); } catch (...) { ++traceFailures; } }
    SetLastError(outgoingError);
    return result;
}
sl::Result traceToken(sl::FrameToken*& token, const uint32_t* frameIndex) {
    const auto original = originalToken.load(std::memory_order_acquire);
    const auto call = ++tokenCalls;
    const auto result = original(token, frameIndex);
    const DWORD outgoingError = GetLastError();
    try {
        if (call <= tokenSamples) {
            // A failing original may leave the caller's output slot untouched.
            // Do not evaluate a potentially uninitialized output pointer then.
            sl::FrameToken* observed{};
            if (result == sl::Result::eOk) readObject(&token, observed);
            std::ostringstream s; s << "{\"event\":\"sl_frame_token\",\"call\":" << call << ",\"result\":" << static_cast<int>(result)
                << ",\"token\":" << pointer(observed) << ",\"frame\":";
            uint32_t number = 0;
            if (observed && frameNumber(*observed, number)) s << number; else s << "null";
            s << ",\"requestedFrame\":"; if (readObject(frameIndex, number)) s << number; else s << "null";
            s << '}'; emit(s.str());
        }
    } catch (...) { ++traceFailures; }
    SetLastError(outgoingError);
    return result;
}
bool writableSlot(HMODULE module, DWORD imageSize, DWORD rva, const std::vector<IMAGE_SECTION_HEADER>& sections, void**& slot) {
    if (rva % alignof(void*) || rva > imageSize || sizeof(void*) > imageSize - rva) return false;
    bool inData = false;
    for (const auto& section : sections) {
        if (std::memcmp(section.Name, ".data\0\0\0", 8) || !(section.Characteristics & IMAGE_SCN_MEM_WRITE) || (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (rva >= section.VirtualAddress && rva - section.VirtualAddress <= section.Misc.VirtualSize && sizeof(void*) <= section.Misc.VirtualSize - (rva - section.VirtualAddress)) inData = true;
    }
    if (!inData) return false;
    slot = reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(module) + rva);
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(slot, &info, sizeof(info)) || info.AllocationBase != module || info.State != MEM_COMMIT || info.Type != MEM_IMAGE ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || reinterpret_cast<uintptr_t>(slot) - reinterpret_cast<uintptr_t>(info.BaseAddress) + sizeof(void*) > info.RegionSize) return false;
    const auto protection = info.Protect & 0xff;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY;
}
}

InstallStatus tryInstall(Sink sink) noexcept {
    try {
        std::lock_guard lock(installMutex);
        if (sink) output.store(sink, std::memory_order_release);
        if (status == InstallStatus::Installed || status == InstallStatus::Refused || status == InstallStatus::Disabled) return status;
        if (!checkedIdentity) {
            wchar_t opt[8]{};
            if (GetEnvironmentVariableW(L"XESS_SL_OBSERVER", opt, 8) != 1 || opt[0] != L'1') { status = InstallStatus::Disabled; return status; }
            checkedIdentity = true; identityValid = samePath(nullptr, gamePath);
        }
        if (!identityValid) { status = InstallStatus::Refused; state("refused_process_path"); return status; }
        struct ModuleReferences {
            HMODULE unity{}, streamline{};
            ~ModuleReferences() { if (streamline) FreeLibrary(streamline); if (unity) FreeLibrary(unity); }
        } modules;
        // Strong references cover every read, validation, and CAS. This lookup
        // does not load DLLs and does not assume the engine prevents unloading.
        if (!GetModuleHandleExW(0, L"UnityPlayer.dll", &modules.unity) || !GetModuleHandleExW(0, L"sl.interposer.dll", &modules.streamline)) {
            state("not_ready_modules"); return InstallStatus::NotReady;
        }
        const auto unity = modules.unity, streamline = modules.streamline;
        if (!samePath(unity, unityPath) || !samePath(streamline, slPath)) { status = InstallStatus::Refused; state("refused_module_path"); return status; }
        if (!checkedHash) { hashValid = hashMatches(); checkedHash = true; }
        if (!hashValid) { status = InstallStatus::Refused; state("refused_unity_sha256"); return status; }
        IMAGE_NT_HEADERS64 nt{}, slNt{}; std::vector<IMAGE_SECTION_HEADER> sections, slSections;
        if (!imageHeaders(unity, nt, sections) || nt.OptionalHeader.SizeOfImage != unityImageSize || !imageHeaders(streamline, slNt, slSections)) {
            status = InstallStatus::Refused; state("refused_pe_headers"); return status;
        }
        std::array<void**, 3> slots{}; std::array<void*, 3> originals{};
        for (size_t i = 0; i < slots.size(); ++i) {
            if (!writableSlot(unity, unityImageSize, slotRvas[i], sections, slots[i])) { status = InstallStatus::Refused; state("refused_slot_protection_or_bounds"); return status; }
            originals[i] = reinterpret_cast<void*>(GetProcAddress(streamline, exportNames[i]));
            const auto at = reinterpret_cast<uintptr_t>(originals[i]), base = reinterpret_cast<uintptr_t>(streamline);
            if (!originals[i] || at < base || at - base >= slNt.OptionalHeader.SizeOfImage || !executable(originals[i])) { status = InstallStatus::Refused; state("refused_export_address"); return status; }
            void* current{};
            if (!readObject(slots[i], current)) { status = InstallStatus::Refused; state("refused_slot_read"); return status; }
            if (!current) { state("not_ready_unity_function_slots"); return InstallStatus::NotReady; }
            if (current != originals[i]) { status = InstallStatus::Refused; state("refused_slot_export_mismatch"); return status; }
        }
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&tryInstall), &pinned)) {
            status = InstallStatus::Refused; state("refused_observer_pin"); return status;
        }
        // A slot is Unity-owned data and a cached original points into Streamline.
        // All three modules must outlive every wrapper, including a partial CAS.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(unity), &pinned) ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(streamline), &pinned)) {
            status = InstallStatus::Refused; state("refused_original_module_pin"); return status;
        }
#ifndef XESS_SL_OBSERVER_TEST
        slModule = streamline; slImageSize = slNt.OptionalHeader.SizeOfImage;
#endif
        originalTags.store(reinterpret_cast<PFun_slSetTagForFrame*>(originals[0]), std::memory_order_release);
        originalConstants.store(reinterpret_cast<PFun_slSetConstants*>(originals[1]), std::memory_order_release);
        originalToken.store(reinterpret_cast<PFun_slGetNewFrameToken*>(originals[2]), std::memory_order_release);
        const std::array<void*, 3> wrappers{reinterpret_cast<void*>(&traceTags), reinterpret_cast<void*>(&traceConstants), reinterpret_cast<void*>(&traceToken)};
        // Publish addresses only after the three owning modules are pinned.
        // Summaries observe these cells without ever reinstalling a wrapper.
        for (size_t i = 0; i < slots.size(); ++i) observedSlots[i].store(slots[i], std::memory_order_release);
        for (size_t i = 0; i < slots.size(); ++i) {
            if (InterlockedCompareExchangePointer(slots[i], wrappers[i], originals[i]) != originals[i]) {
                // Earlier wrappers remain valid and pinned; no unguarded rollback.
                status = InstallStatus::Refused; state("refused_slot_compare_exchange_race"); return status;
            }
            installedMask.fetch_or(1u << i);
        }
        status = InstallStatus::Installed; state("installed_three_data_slots"); return status;
    } catch (...) {
        ++traceFailures;
        // Retry is safe only before any slot changed. A partial install remains pinned.
        if (installedMask.load()) return InstallStatus::Refused;
        return InstallStatus::NotReady;
    }
}
void flushSummary() noexcept {
    if (!output.load() || !checkedIdentity) return;
    try {
        const auto installed = installedMask.load(std::memory_order_acquire);
        const std::array<void*, 3> wrappers{reinterpret_cast<void*>(&traceTags), reinterpret_cast<void*>(&traceConstants), reinterpret_cast<void*>(&traceToken)};
        const std::array<void*, 3> originals{reinterpret_cast<void*>(originalTags.load()), reinterpret_cast<void*>(originalConstants.load()), reinterpret_cast<void*>(originalToken.load())};
        unsigned active = 0;
        std::ostringstream slotStates; slotStates << '[';
        for (size_t i = 0; i < observedSlots.size(); ++i) {
            if (i) slotStates << ',';
            const auto slot = observedSlots[i].load(std::memory_order_acquire);
            void* current{};
            const char* slotState = "not_installed";
            bool readable = false;
            if (installed & (1u << i)) {
                if (!slot) slotState = "address_missing";
                else if (!(readable = readObject(slot, current))) slotState = "unreadable";
                else if (current == wrappers[i]) { active |= 1u << i; slotState = "active"; }
                else if (current == originals[i]) slotState = "restored_original";
                else if (!current) slotState = "null";
                else slotState = "replaced";
            }
            slotStates << "{\"api\":\"" << exportNames[i] << "\",\"rva\":" << slotRvas[i] << ",\"state\":\"" << slotState
                       << "\",\"slot\":" << pointer(slot) << ",\"observed\":";
            if (readable) slotStates << pointer(current); else slotStates << "null";
            slotStates << '}';
        }
        slotStates << ']';
        std::ostringstream s; s << "{\"event\":\"sl_observer_summary\",\"installedMask\":" << installed
            << ",\"hookSlotsStillActive\":" << active << ",\"hookSlotCheck\":\"point_in_time_read_only\",\"hookSlotStates\":" << slotStates.str()
            << ",\"tagCalls\":" << tagsCalls.load() << ",\"tagsTotal\":" << tagsTotal.load() << ",\"constantsCalls\":" << constantsCalls.load()
            << ",\"tokenCalls\":" << tokenCalls.load() << ",\"emitted\":" << emitted.load() << ",\"traceFailures\":" << traceFailures.load()
            << ",\"gameFrameInputsVerified\":false,\"pixelsRead\":false}"; emit(s.str());
    } catch (...) { ++traceFailures; }
}

#ifdef XESS_SL_OBSERVER_TEST
namespace testing {
Functions reset(Sink sink, Functions originals) noexcept {
    output = sink; originalTags = originals.tags; originalConstants = originals.constants; originalToken = originals.token;
    tagsCalls = 0; constantsCalls = 0; tokenCalls = 0; tagsTotal = 0; emitted = 0; traceFailures = 0; installedMask = 0; checkedIdentity = true;
    for (auto& slot : observedSlots) slot = nullptr;
    return {&traceTags, &traceConstants, &traceToken};
}
void observeSlots(void** tags, void** constants, void** token, unsigned mask) noexcept {
    observedSlots[0] = tags; observedSlots[1] = constants; observedSlots[2] = token;
    installedMask.store(mask & 7u, std::memory_order_release);
}
}
#endif
}
