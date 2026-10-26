#include "fsr_local_fi_off.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

extern "C" {
extern const std::uint8_t fsr_local_fi_off_template[], fsr_local_fi_off_template_end[];
extern const std::uint8_t fsr_local_fi_off_phase_disp[], fsr_local_fi_off_one_disp[];
extern const std::uint8_t fsr_local_fi_off_active_disp0[], fsr_local_fi_off_active_disp1[];
extern const std::uint8_t fsr_local_fi_off_inactive_disp0[], fsr_local_fi_off_inactive_disp1[];
extern const std::uint8_t fsr_local_fi_off_unwind[];
}
namespace xess_vk::fsr_local_fi_off {
namespace {
constexpr std::size_t codeSize = 40, unwindSize = 56, trampolineSize = 33;
constexpr std::size_t siteStride = 64, metadataOffset = 128, pageSize = 4096;
SRWLOCK setupLock = SRWLOCK_INIT;
struct SetupLock {
    SetupLock() noexcept { AcquireSRWLockExclusive(&setupLock); }
    ~SetupLock() { ReleaseSRWLockExclusive(&setupLock); }
};
alignas(4) volatile LONG initialPhase{}; // 0 configuring, 1 active, 2 sticky stop
std::atomic<volatile LONG*> phase{&initialPhase};
void* originals[2]{};
std::uint8_t* island{};
RUNTIME_FUNCTION islandFunctions[2]{};
struct Site {
    RUNTIME_FUNCTION function{};
    std::uintptr_t base{};
    void* trampoline{};
    void* continuation{};
    DWORD protection{};
    bool registered{};
};
// Published code, phase and function tables remain valid for process life.
Site sites[2];
LONG currentPhase() noexcept { return InterlockedCompareExchange(phase.load(std::memory_order_acquire), 0, 0); }
std::size_t offset(const std::uint8_t* label) noexcept {
    return reinterpret_cast<std::uintptr_t>(label) - reinterpret_cast<std::uintptr_t>(fsr_local_fi_off_template);
}
bool read(std::uintptr_t address, void* out, std::size_t size) noexcept {
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, size, &copied) && copied == size;
}
bool codeProtection(void* address, std::size_t size, DWORD& protection) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    if (start < base || start - base > memory.RegionSize || size > memory.RegionSize - (start - base)) return false;
    const auto access = memory.Protect & 0xff;
    if (access != PAGE_EXECUTE_READ && access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY) return false;
    protection = memory.Protect;
    return true;
}
bool relative(std::uint8_t* bytes, std::uintptr_t code, std::size_t displacement,
              std::uintptr_t target, std::size_t trailing = 0) noexcept {
    const auto next = code + displacement + 4 + trailing;
    const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next);
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    const auto value = static_cast<std::int32_t>(delta);
    std::memcpy(bytes + displacement, &value, sizeof(value));
    return true;
}
bool pair(std::uint8_t* bytes, std::uintptr_t code, std::size_t at, std::uintptr_t target) noexcept {
    bytes[at] = 0x0f; bytes[at + 1] = 0x84;
    bytes[at + 6] = 0x0f; bytes[at + 7] = 0x85;
    bytes[at + 12] = 0x90; bytes[at + 13] = 0x90;
    return relative(bytes, code, at + 2, target) && relative(bytes, code, at + 8, target);
}
bool absoluteJump(const std::uint8_t* code, std::uintptr_t target) noexcept {
    std::uintptr_t actual{};
    std::memcpy(&actual, code + 6, 8);
    return code[0] == 0xff && code[1] == 0x25 && !code[2] && !code[3] && !code[4] && !code[5] && actual == target;
}
bool originalShape(std::size_t index, void* trampoline, void* continuation) noexcept {
    DWORD protection{};
    if (!codeProtection(trampoline, trampolineSize, protection)) return false;
    std::array<std::uint8_t, trampolineSize> bytes{};
    return read(reinterpret_cast<std::uintptr_t>(trampoline), bytes.data(), bytes.size()) &&
        std::memcmp(bytes.data(), cmpBytes.data(), cmpBytes.size()) == 0 &&
        absoluteJump(bytes.data() + 5, reinterpret_cast<std::uintptr_t>(continuation)) &&
        absoluteJump(bytes.data() + 19, reinterpret_cast<std::uintptr_t>(entry(index)));
}
bool expectedCode(std::size_t index, std::uintptr_t trampoline, std::uintptr_t continuation,
                  std::array<std::uint8_t, codeSize>& bytes) noexcept {
    const auto start = reinterpret_cast<std::uintptr_t>(entry(index));
    std::memcpy(bytes.data(), fsr_local_fi_off_template, codeSize);
    return relative(bytes.data(), start, offset(fsr_local_fi_off_phase_disp), reinterpret_cast<std::uintptr_t>(phase.load()), 1) &&
        relative(bytes.data(), start, offset(fsr_local_fi_off_one_disp), start + codeSize, 1) &&
        relative(bytes.data(), start, offset(fsr_local_fi_off_active_disp0), continuation) &&
        relative(bytes.data(), start, offset(fsr_local_fi_off_active_disp1), continuation) &&
        relative(bytes.data(), start, offset(fsr_local_fi_off_inactive_disp0), trampoline) &&
        relative(bytes.data(), start, offset(fsr_local_fi_off_inactive_disp1), trampoline);
}
bool expectedTrampoline(std::size_t index, std::uintptr_t trampoline, std::uintptr_t continuation,
                        std::array<std::uint8_t, trampolineSize>& bytes) noexcept {
    std::memcpy(bytes.data(), cmpBytes.data(), cmpBytes.size());
    return pair(bytes.data(), trampoline, 5, continuation) && pair(bytes.data(), trampoline, 19,
        reinterpret_cast<std::uintptr_t>(entry(index)));
}
bool writeCode(void* destination, const void* source, std::size_t size, DWORD expectedProtection) noexcept {
    DWORD actual{}, previous{};
    if (!codeProtection(destination, size, actual) || actual != expectedProtection ||
        !VirtualProtect(destination, size, PAGE_READWRITE, &previous)) return false;
    std::memcpy(destination, source, size);
    DWORD ignored{};
    const bool restored = VirtualProtect(destination, size, previous, &ignored) != FALSE;
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), destination, size) != FALSE;
    return restored && flushed;
}
bool islandReady(std::size_t index) noexcept {
    if (!island) return false;
    DWORD protection{};
    if (!codeProtection(island, pageSize, protection) || protection != PAGE_EXECUTE_READ) return false;
    DWORD64 base{};
    const auto* fn = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(entry(index)), &base, nullptr);
    return fn == &islandFunctions[index] && base == reinterpret_cast<DWORD64>(island) &&
        std::memcmp(island + metadataOffset, fsr_local_fi_off_unwind, unwindSize) == 0;
}
}
std::span<const BodyGuard> bodyGuards() noexcept {
    static constexpr std::uint8_t g0[]{0xc0,0xf3,0x1d,0x00,0x2f,0x1a,0x1e,0x00,0xdc,0xca,0xd0,0x01};
    static constexpr std::uint8_t g1[]{0x2f,0x1a,0x1e,0x00,0x6b,0x1a,0x1e,0x00,0xfc,0xca,0xd0,0x01};
    static constexpr std::uint8_t g2[]{0xf3,0x21,0x1e,0x00,0x4b,0x1e,0x1f,0x00,0x40,0xcb,0xd0,0x01};
    static constexpr std::uint8_t g3[]{0x21,0x00,0x00,0x00,0x2f,0x1a,0x1e,0x00,0x6b,0x1a,0x1e,0x00,0xfc,0xca,0xd0,0x01};
    static constexpr std::uint8_t g4[]{0x21,0x3c,0x0c,0x00,0x3c,0xc8,0x2e,0x0b,0x33,0xb8,0x2f,0x0b,0x2a,0xa8,0x30,0x0b,0x21,0x98,0x31,0x0b,0x18,0x88,0x32,0x0b,0x08,0x68,0x34,0x0b,0xc0,0xf3,0x1d,0x00,0x2f,0x1a,0x1e,0x00,0xdc,0xca,0xd0,0x01};
    static constexpr std::uint8_t g5[]{0x01,0x3f,0x0d,0x00,0x3f,0x78,0x33,0x0b,0x34,0x34,0x74,0x16,0x34,0x01,0x6a,0x16,0x1f,0xf0,0x1d,0xe0,0x1b,0xd0,0x19,0xc0,0x17,0x70,0x16,0x60,0x15,0x50,0x00,0x00};
    static constexpr std::uint8_t g6[]{0x48,0x89,0x5c,0x24,0x18,0x4c,0x89,0x4c,0x24,0x20,0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x80,0x4e,0xff,0xff,0xb8,0x50,0xb3,0x00,0x00,0xe8,0x7f,0x55,0xa1,0x00,0x48,0x2b,0xe0,0x48,0x8b,0xf1,0x0f,0x29,0xbc,0x24,0x30,0xb3,0x00,0x00};
    static constexpr std::uint8_t g7[]{0x0f,0x29,0xb4,0x24,0x40,0xb3,0x00,0x00,0x48,0x8d,0x8d,0x40,0x48,0x00,0x00,0x44,0x0f,0x29,0x84,0x24,0x20,0xb3,0x00,0x00,0x44,0x0f,0x29,0x8c,0x24,0x10,0xb3,0x00,0x00,0x44,0x0f,0x29,0x94,0x24,0x00,0xb3,0x00,0x00,0x44,0x0f,0x29,0x9c,0x24,0xf0,0xb2,0x00,0x00,0x44,0x0f,0x29,0xa4,0x24,0xe0,0xb2,0x00,0x00};
    static constexpr BodyGuard guards[]{
        {0x213f3d0, g0, sizeof(g0)},
        {0x213f3dc, g1, sizeof(g1)},
        {0x213f3f4, g2, sizeof(g2)},
        {0x1d0cb40, g3, sizeof(g3)},
        {0x1d0cafc, g4, sizeof(g4)},
        {0x1d0cadc, g5, sizeof(g5)},
        {0x1df3c0, g6, sizeof(g6)},
        {0x1e1a2f, g7, sizeof(g7)}
    };
    return guards;
}
bool prepare(std::uintptr_t moduleBase) noexcept {
    SetupLock lock;
    if (island || currentPhase() != 0 || moduleBase > UINTPTR_MAX - siteRvas.back() ||
        offset(fsr_local_fi_off_template_end) != codeSize) return false;
    using Allocate = void* (WINAPI*)(HANDLE, void*, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    const auto allocate = reinterpret_cast<Allocate>(GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "VirtualAlloc2"));
    if (!allocate) return false;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto anchor = moduleBase + siteRvas.front();
    const auto maximum = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    if (anchor > maximum) return false;
    constexpr std::uintptr_t reach = 0x3fff0000;
    const auto mask = std::uintptr_t(system.dwAllocationGranularity - 1);
    const auto low = anchor > reach ? anchor - reach : std::uintptr_t(0x10000);
    const auto high = std::min(maximum, anchor + std::min(reach, maximum - anchor));
    MEM_ADDRESS_REQUIREMENTS addresses{};
    addresses.LowestStartingAddress = reinterpret_cast<void*>((low + mask) & ~mask);
    addresses.HighestEndingAddress = reinterpret_cast<void*>((high & ~mask) - 1);
    addresses.Alignment = system.dwAllocationGranularity;
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &addresses;
    auto* allocation = static_cast<std::uint8_t*>(allocate(GetCurrentProcess(), nullptr, pageSize * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1));
    if (!allocation) return false;
    for (std::size_t index = 0; index < 2; ++index) {
        std::memcpy(allocation + index * siteStride, fsr_local_fi_off_template, codeSize + 1);
        islandFunctions[index] = {static_cast<DWORD>(index * siteStride),
            static_cast<DWORD>(index * siteStride + codeSize), metadataOffset};
    }
    std::memcpy(allocation + metadataOffset, fsr_local_fi_off_unwind, unwindSize);
    DWORD previous{};
    if (!VirtualProtect(allocation, pageSize, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), allocation, pageSize) ||
        !RtlAddFunctionTable(islandFunctions, 2, reinterpret_cast<DWORD64>(allocation))) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return false;
    }
    phase.store(reinterpret_cast<volatile LONG*>(allocation + pageSize), std::memory_order_release);
    island = allocation;
    return true;
}
void* entry(std::size_t index) noexcept { return island && index < 2 ? island + index * siteStride : nullptr; }
void** originalSlot(std::size_t index) noexcept { return index < 2 ? &originals[index] : nullptr; }
bool configureSite(std::size_t index, void* trampoline, void* continuation) noexcept {
    SetupLock lock;
    if (index >= 2 || !island || currentPhase() != 0 || sites[index].registered ||
        *originalSlot(index) != trampoline || !continuation || !originalShape(index, trampoline, continuation) || !islandReady(index)) return false;
    std::array<std::uint8_t, codeSize> code{};
    std::array<std::uint8_t, trampolineSize> forwarded{};
    const auto address = reinterpret_cast<std::uintptr_t>(trampoline);
    const auto resume = reinterpret_cast<std::uintptr_t>(continuation);
    if (!expectedCode(index, address, resume, code) || !expectedTrampoline(index, address, resume, forwarded)) return false;
    DWORD protection{};
    if (!codeProtection(trampoline, trampolineSize, protection) ||
        !writeCode(entry(index), code.data(), code.size(), PAGE_EXECUTE_READ) ||
        !writeCode(trampoline, forwarded.data(), forwarded.size(), protection)) return false;
    auto& site = sites[index];
    const auto metadata = reinterpret_cast<std::uintptr_t>(island + metadataOffset);
    site.base = std::min(address, metadata);
    if (address - site.base > UINT32_MAX - trampolineSize || metadata - site.base > UINT32_MAX) return false;
    site.function = {static_cast<DWORD>(address - site.base), static_cast<DWORD>(address - site.base + trampolineSize),
        static_cast<DWORD>(metadata - site.base)};
    if (!RtlAddFunctionTable(&site.function, 1, site.base)) return false;
    site.trampoline = trampoline;
    site.continuation = continuation;
    site.protection = protection;
    site.registered = true;
    return siteReady(index);
}
bool siteReady(std::size_t index) noexcept {
    if (index >= 2 || !sites[index].registered || !islandReady(index)) return false;
    const auto& site = sites[index];
    DWORD protection{};
    if (*originalSlot(index) != site.trampoline || !codeProtection(site.trampoline, trampolineSize, protection) ||
        protection != site.protection) return false;
    std::array<std::uint8_t, codeSize> expected{}, actual{};
    std::array<std::uint8_t, trampolineSize> expectedForwarded{}, actualForwarded{};
    const auto address = reinterpret_cast<std::uintptr_t>(site.trampoline);
    if (!expectedCode(index, address, reinterpret_cast<std::uintptr_t>(site.continuation), expected) ||
        !expectedTrampoline(index, address, reinterpret_cast<std::uintptr_t>(site.continuation), expectedForwarded) ||
        !read(reinterpret_cast<std::uintptr_t>(entry(index)), actual.data(), actual.size()) || actual != expected ||
        !read(address, actualForwarded.data(), actualForwarded.size()) || actualForwarded != expectedForwarded ||
        island[index * siteStride + codeSize] != 1) return false;
    DWORD64 base{};
    const auto* fn = RtlLookupFunctionEntry(address, &base, nullptr);
    return fn == &site.function && base == site.base;
}
bool activate() noexcept {
    SetupLock lock;
    if (!siteReady(0) || !siteReady(1)) return false;
    return InterlockedCompareExchange(phase.load(), 1, 0) == 0;
}
void stop() noexcept {
    SetupLock lock;
    InterlockedExchange(phase.load(), 2);
}
bool active() noexcept { return currentPhase() == 1; }
}
