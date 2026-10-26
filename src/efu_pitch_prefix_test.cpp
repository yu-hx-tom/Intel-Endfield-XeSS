#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using Target = unsigned (*)(const unsigned*);
static Target original = nullptr;
static bool featureEnabled = true;
static unsigned hookCalls = 0;

static void require(bool condition, const char* step) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (Win32 %lu)\n", step, GetLastError());
        std::exit(1);
    }
}

static void mh(MH_STATUS status, const char* step) {
    if (status != MH_OK) {
        std::fprintf(stderr, "FAIL: %s: %s\n", step, MH_StatusToString(status));
        std::exit(1);
    }
}

static unsigned replacement(const unsigned* input) {
    ++hookCalls;
    const unsigned value = original(input);
    return featureEnabled ? value + 1000 : value;
}

int main() {
    require(GetModuleHandleW(L"GameAssembly.dll") == nullptr, "game DLL absent");
    require(GetModuleHandleW(L"OneChannelLC.dll") == nullptr, "channel DLL absent");
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const std::size_t page = systemInfo.dwPageSize;
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    require(memory != nullptr, "allocate owned fixture");

    const unsigned char pitchPrefix[32] = {
        0x40,0x53,0x48,0x81,0xec,0x90,0x00,0x00,
        0x00,0x48,0x8b,0x05,0x70,0x98,0x10,0x0c,
        0x48,0x8b,0xd9,0x48,0x8b,0x48,0x40,0x48,
        0x85,0xc9,0x0f,0x85,0x56,0xf2,0xcf,0x01
    };
    std::memcpy(memory, pitchPrefix, sizeof(pitchPrefix));
    // Both PC-relative operands point only into this owned allocation.
    const std::int32_t ownedRipOffset = static_cast<std::int32_t>(page - 16);
    const std::int32_t ownedBranchOffset = 64 - 32;
    std::memcpy(memory + 12, &ownedRipOffset, sizeof(ownedRipOffset));
    std::memcpy(memory + 28, &ownedBranchOffset, sizeof(ownedBranchOffset));
    const unsigned char fallthrough[] = {
        0x8b,0x03,0x48,0x81,0xc4,0x90,0x00,0x00,0x00,0x5b,0xc3
    };
    const unsigned char branch[] = {
        0x8b,0x03,0x01,0xc8,0x48,0x81,0xc4,0x90,0x00,0x00,0x00,0x5b,0xc3
    };
    std::memcpy(memory + 32, fallthrough, sizeof(fallthrough));
    std::memcpy(memory + 64, branch, sizeof(branch));
    const std::uintptr_t ownedData = reinterpret_cast<std::uintptr_t>(memory + page + 128);
    std::memcpy(memory + page, &ownedData, sizeof(ownedData));
    auto* branchValue = reinterpret_cast<std::uintptr_t*>(memory + page + 128 + 64);
    *branchValue = 7;
    DWORD oldProtection = 0;
    require(VirtualProtect(memory, page, PAGE_EXECUTE_READ, &oldProtection) != FALSE,
        "make only fixture code executable");
    require(FlushInstructionCache(GetCurrentProcess(), memory, page) != FALSE,
        "flush fixture code");
    const auto target = reinterpret_cast<Target>(memory);
    const unsigned input = 35;
    require(target(&input) == 42, "baseline owned RIP data and taken branch");
    *branchValue = 0;
    require(target(&input) == 35, "baseline owned fallthrough");
    *branchValue = 7;

    mh(MH_Initialize(), "initialize pinned MinHook");
    mh(MH_CreateHook(memory, reinterpret_cast<LPVOID>(&replacement),
        reinterpret_cast<LPVOID*>(&original)), "create exact-prefix hook");
    require(original(&input) == 42, "trampoline preserves original computation");
    const auto* trampoline = reinterpret_cast<const unsigned char*>(original);
    require(std::memcmp(trampoline, pitchPrefix, 9) == 0, "trampoline preserves first nine bytes");
    require(trampoline[9] == 0xff && trampoline[10] == 0x25, "trampoline absolute return jump");
    std::uintptr_t returnAddress = 0;
    std::memcpy(&returnAddress, trampoline + 15, sizeof(returnAddress));
    require(returnAddress == reinterpret_cast<std::uintptr_t>(memory + 9),
        "MinHook returns before original RIP-relative instruction");

    mh(MH_EnableHook(memory), "enable hook");
    require(target(&input) == 1042 && hookCalls == 1, "enabled hook calls original trampoline");
    featureEnabled = false;
    require(target(&input) == 42 && hookCalls == 2, "feature off preserves original result");
    featureEnabled = true;
    require(target(&input) == 1042 && hookCalls == 3, "feature on reapplies only requested delta");
    require(original(&input) == 42, "active hook leaves trampoline result unchanged");
    mh(MH_DisableHook(memory), "disable hook");
    require(target(&input) == 42 && hookCalls == 3, "disabled hook restores original behavior");
    mh(MH_EnableHook(memory), "reenable hook");
    require(target(&input) == 1042 && hookCalls == 4, "reenabled hook works");
    mh(MH_RemoveHook(memory), "remove hook");
    require(target(&input) == 42 && hookCalls == 4, "removed hook restores original behavior");
    mh(MH_Uninitialize(), "uninitialize MinHook");
    require(GetModuleHandleW(L"GameAssembly.dll") == nullptr, "game DLL remains absent");
    require(GetModuleHandleW(L"OneChannelLC.dll") == nullptr, "channel DLL remains absent");
    require(VirtualFree(memory, 0, MEM_RELEASE) != FALSE, "release owned fixture");

    std::puts("{\"passed\":true,\"minhookCommit\":\"c3fcafdc10146beb5919319d0683e44e3c30d537\",\"gameDllLoaded\":false,\"ownedPcRelativeOperands\":true,\"originalTakenBranchResult\":42,\"originalFallthroughResult\":35,\"trampolineResult\":42,\"hookedResult\":1042,\"featureOffResult\":42,\"disabledResult\":42,\"reenabledResult\":1042,\"removedResult\":42,\"hookCalls\":4,\"trampolineCopiedPrefixBytes\":9,\"originalRipRelativeInstructionRetained\":true}");
    return 0;
}
