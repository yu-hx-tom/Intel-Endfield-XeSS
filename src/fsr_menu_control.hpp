#pragma once
#include <array>
#include <cstdint>
#include "gameplay_state.hpp"

namespace xess_vk::fsr_menu_control {
inline constexpr std::uint32_t interpolationRva = 0x493070;
inline constexpr std::uint32_t composeRva = 0x48F8D0;
// Per-frame render-parameter construction. 239D4D copies the camera's
// FrameInterpolationAlgo (+0x530) into params+0x998; the renderer 1DF3C0
// receives these params and branches on ==1 at 1EFAD3, 1F0644 and 1F081B.
// The algo stays FSR3 while the menu toggles; the menu switch is the settings
// byte [[view+0x20]+0x84]. Native FI runs only when that byte is set, view+3B
// is clear and the algo is FSR3 (1EFAB8..1EFAD3). Compose input+1 is a copy of
// the same settings byte (1F2158..1F216A, same [rbp-30] view object).
inline constexpr std::uint32_t renderParamsRva = 0x239C50;
inline constexpr std::uint32_t frameInterpolationAlgoOffset = 0x998;
// FrameInterpolationAlgo values compared in this build: 1 FSR3, 2, 4, 8.
inline constexpr std::uint32_t fsr3FrameInterpolation = 1;
inline constexpr std::uint32_t noFrameInterpolation = 0;
inline constexpr wchar_t suppressionOption[] = L"XESS_FSR_NATIVE_OFF";

// Verified Win64 ABI: RCX, RDX, R8, R9 and entry RSP+28. Both return the
// original output-pair pointer in RAX. These are function bodies, not E9 thunks.
using GateFunction = void* (*)(void* owner, void* outputPair, void* input,
                               void* graph, std::uint32_t passIndex);
// RCX params, RDX camera, XMM2/XMM3 floats; returns params in RAX. Callers at
// EC9ED0, ECA33B and ECA374 pass no stack arguments.
using RenderParamsFunction = void* (*)(void* params, void* camera, float first, float second);
using ComposeSink = bool (*)(void* originalInput, void* graph, std::uint32_t thread,
                            bool localFiOffCandidate) noexcept;
using LocalFiOffActive = bool (*)() noexcept;
using GameplayStateReader = std::int32_t (*)() noexcept;
enum class Gate : std::uint8_t { Interpolation, Compose };
struct ControlIntent {
    // Raw FrameInterpolationAlgo read from the latest constructed params.
    std::uint32_t rawAlgo{};
    // The menu switch: latest Compose input+1 (settings +0x84).
    std::uint8_t rawEnable{};
    bool requestedEnabled{};
    std::uint64_t controlEpoch{};
    bool bothGatesReady{};
    // LocalFiOff requires this Compose's exact registration, original {0,1}
    // prefix and committed decisions. Legacy mode uses params/backstop shadows.
    bool nativeSuppressed{};
    bool inputObserved{};
    // Kept for the presentation admission contract: true whenever both the
    // algo and the menu switch were observed.
    bool paired{}, gatesConsistent{};
    bool localFiOffRequested{}, localFiOffActive{};
    bool gameplayGateRequired{}, gameplayReady{};
    std::int32_t gameplayState{gameplay_state::unknown};
    // The character menu keeps the gameplay state active; its own GPU passes
    // were recorded within characterMenuHoldMs, so it counts as not gameplay.
    bool characterMenu{};
};
inline constexpr std::uint64_t characterMenuHoldMs = 250;
inline bool gameplayAllowsInterpolation(const ControlIntent& intent) noexcept {
    return !intent.gameplayGateRequired || intent.gameplayReady;
}
inline bool auxiliaryCaptureRequired(const ControlIntent& intent) noexcept {
    return gameplayAllowsInterpolation(intent) && (!intent.inputObserved || intent.requestedEnabled);
}
struct Stats {
    std::array<std::uint64_t, 2> calls{};
    std::uint64_t renderParamsCalls{}, forcedOff{}, readFailures{}, writeFailures{};
    // Gate calls whose native enable was set (the rewrite had not seen the
    // switch yet) and how many of them received the OFF shadow.
    std::uint64_t enabledGateCalls{}, backstopShadows{};
    // Compose calls that received the FG-OFF input {0,0} while native FI was
    // suppressed (the switch byte cleared in the local shadow only).
    std::uint64_t composeOffShadows{};
    std::uint64_t localAlgoPreserved{}, localComposeMatched{}, localComposeRefused{}, localUnexpectedNativeEnabled{};
    bool localFiOffActive{};
};

bool suppressionRequested() noexcept;
// Installer publishes all process-lived originals after the existing pinned
// Unity/MinHook checks and before enabling any hook. No hooks are installed
// here. Configuration is single-use, even if later activation fails.
// The local certificate reader is process-lived/nonblocking and reads the same
// sticky active state as both local detours; installation publishes it last.
bool configureVerified(GateFunction interpolation, GateFunction compose, RenderParamsFunction renderParams,
                       bool suppressNative, ComposeSink = nullptr, bool localFiOff = false,
                       LocalFiOffActive = nullptr, GameplayStateReader = nullptr) noexcept;
// Call only after every target postcheck. Stop is sticky for process lifetime;
// it never removes a forwarder or releases an original trampoline.
bool activate() noexcept;
void stop() noexcept;
ControlIntent snapshot() noexcept;
Stats stats() noexcept;
// Render-thread signal from the SR shader identity of the character menu's
// passes. Lock-free; the next Compose publish turns it into the FG gate.
void characterMenuObserved() noexcept;
void characterMenuObservedAt(std::uint64_t tickMs) noexcept;

// Late native enables retain the protective +0 shadow. LocalFiOff clears the
// Compose switch only after this call's exact registration/certificate proof.
void* interpolationEntry(void*, void*, void*, void*, std::uint32_t);
void* composeEntry(void*, void*, void*, void*, std::uint32_t);
void* renderParamsEntry(void*, void*, float, float);

#ifdef XESS_FSR_MENU_CONTROL_TEST
// Own-process fixture only, after all fixture threads have joined.
void resetForTest() noexcept;
void keepNativeAlgoForTest(bool) noexcept;
#endif
}
