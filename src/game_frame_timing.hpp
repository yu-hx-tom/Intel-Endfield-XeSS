#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Fixed UnityPlayer build only. This module observes CPU boundaries; it neither
// installs hooks nor owns XeLL calls, SDK tickets, or frame-key associations.
namespace xess_vk::game_timing {

inline constexpr const char* pinnedUnitySha256 =
    "9aa9397a6c5ee4fa083ea379b81ff14019eeba26a76d15446318d5947fb3d2d0";
inline constexpr std::size_t hookCount = 7;
// Ordinary HG-worker calls only. SL proxy and queued-consumer returns are not
// direct sources. The caller supplies the module base from admitted installation.
inline constexpr std::array<std::uintptr_t, 3> directPresentReturnRvas{0x19EF8D, 0x19EFAA, 0x19EFC7};
inline constexpr std::uintptr_t presentFunctionSlotRva = 0x1EA24C0;
inline constexpr std::uintptr_t queuedPresentReturnRva = 0xD38845;
inline bool directPresentCaller(std::uintptr_t admittedModuleBase, std::uintptr_t returnAddress) noexcept {
    if (!admittedModuleBase || returnAddress < admittedModuleBase)
        return false;
    const auto rva = returnAddress - admittedModuleBase;
    for (const auto expected : directPresentReturnRvas) {
        if (rva == expected)
            return true;
    }
    return false;
}

enum class Stage : std::uint8_t {
    SimulationStart, SimulationEnd, RenderSubmitStart, RenderSubmitEnd,
    NativePresentStart, NativePresentEnd, NativePresentEnqueue
};
enum class Boundary : std::uint8_t { NormalReturn, MatchedHelperEntry, EntryBeforeOriginal };
enum class FrameKeySource : std::uint8_t {
    GlobalC8AfterReturn, EntryRcxDword, CallerRbxDword, ExistingNativePresentScope
};
enum class SourceAbi : std::uint8_t {
    // These fixed bodies consume no stack arguments. Their return convention is
    // opaque: the assembly forwards GPR/XMM arguments and preserves RAX/XMM0.
    NoArgumentsOpaqueReturn, RcxDwordPointerOpaqueReturn,
    // 23D0F0 is a leaf with no arguments, returning bool in AL. Its tail bridge
    // preserves the original stack, return address, all registers and RFLAGS.
    NoArgumentsBoolAl,
    // RCX=backend, RDX=VkPresentInfoKHR*, R8=opaque context, XMM3=double,
    // stack argument five=double. Entry observes then tail-forwards unchanged.
    EnqueuePresentOpaqueFive
};

struct Event {
    Stage stage{};
    Boundary boundary{};
    FrameKeySource keySource{};
    std::uint32_t engineFrame{};
    std::uint32_t threadId{};
    std::uintptr_t sourceRva{};     // The original outer callback, not helper RVA.
    std::uintptr_t keyAddress{};    // Zero for the copied caller-RBX key.
    bool frameKeyValid{};
};

// Failures are contained by the module, including C++ and native SEH failures.
// Callbacks must not retain pointers into an invocation. Present stages provide
// source associations only: they are never permission to emit SDK PRESENT.
using Sink = bool (*)(void*, const Event&);
using Reader = bool (*)(void*, std::uintptr_t, void*, std::size_t);
struct EnqueueEvent {
    std::uintptr_t state{}, queue{}, presentInfo{};
    std::uint32_t threadId{};
    bool queueValid{};
};
using EnqueueSink = bool (*)(void*, const EnqueueEvent&);
struct Config {
    std::uintptr_t moduleBase{};
    void* context{};
    Sink sink{};
    Reader reader{};               // Null selects bounded native-SEH memcpy.
    EnqueueSink enqueueSink{};     // Optional pre-entry packet carrier; no key guess.
};

struct HookSite {
    Stage stage{};
    std::uintptr_t targetRva{};
    std::uintptr_t sourceRva{};
    SourceAbi abi{};
    Boundary boundary{};
    FrameKeySource keySource{};
    std::uint8_t copiedInstructionLength{};
    std::array<std::uint8_t, 16> expectedPrefix{};
    const void* entry{};           // Opaque assembly address; no invented C++ ABI.
    void** originalTrampoline{};   // Main publishes once before group enable.
};

// Fixed targets: C98E80, 23D0F0, C988D0, C98940, D46DB0, D46D40, 13D106C.
// SIM_END observes 23D0F0 only for caller returnAddress == base+C9906F,
// copying the outer callback's saved RBX frame key before the helper executes.
const std::array<HookSite, hookCount>& hookSites() noexcept;
bool configure(const Config&) noexcept;
bool activate() noexcept;
void stop() noexcept;
// stop prevents new events; caller keeps Config/context/trampolines alive until
// hooks are disabled and all source calls are drained. No lock spans originals.
std::uint32_t callbacksInFlight() noexcept;

struct Counters {
    std::uint64_t emitted{}, invalidKeys{}, callbackFailures{}, ignoredHelperCallers{};
    // Counts the installed enqueue entry even after stop; no parameter reads.
    std::uint64_t enqueueEntrances{};
};
Counters counters() noexcept;
struct CpuFrameKey {
    std::uintptr_t owner{};
    std::uint32_t engineFrame{};
    bool valid{};
};
// Fixed engine field, read twice with stable owner and counter. This does not
// create a timing record or authorize cross-thread/global frame substitution.
CpuFrameKey readCpuFrameKey() noexcept;

} // namespace xess_vk::game_timing
