#include "fsr_menu_control.hpp"
#include "diagnostic_flags.hpp"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <limits>

namespace xess_vk::fsr_menu_control {
namespace {
SRWLOCK lock = SRWLOCK_INIT;
std::array<std::atomic<GateFunction>, 2> originals{};
std::atomic<RenderParamsFunction> renderParamsOriginal{};
bool configured{}, active{}, stopped{}, suppress{};
bool modeObserved{}, modeEnabled{};
std::uint64_t epoch{};
// Latest observations. Order within one frame: params construction, then the
// renderer's Interpolation gate, then its Compose gate, then presentation.
std::uint32_t lastAlgo{};
bool algoObserved{}, algoFsr3{}, forcedLast{};
bool switchObserved{}, switchEnabled{}, backstopLast{};
ControlIntent latest;
Stats counters;
ComposeSink composeSink{};
bool localFiMode{}, localParamsProven{}, localComposeProven{}, localUnexpectedNative{};
LocalFiOffActive localActive{};
GameplayStateReader gameplayReader{};
std::int32_t gameplayState{gameplay_state::unknown};
// GetTickCount64 of the latest character-menu pass; 0 = never. Written by the
// render thread without the menu lock, read only when publishing.
std::atomic<std::uint64_t> characterMenuTick{};
bool localReady() noexcept { return localFiMode && localActive && localActive(); }
#ifdef XESS_FSR_MENU_CONTROL_TEST
bool legacyKeepNativeAlgoForTest{};
#endif
struct SharedLock {
    SharedLock() { AcquireSRWLockShared(&lock); }
    ~SharedLock() { ReleaseSRWLockShared(&lock); }
};
struct ExclusiveLock {
    ExclusiveLock() { AcquireSRWLockExclusive(&lock); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock); }
};
void refreshReadiness(ControlIntent& intent) noexcept {
    intent.bothGatesReady = configured && active && !stopped;
    intent.localFiOffRequested = localFiMode;
    intent.localFiOffActive = localReady();
    if (!intent.bothGatesReady || (localFiMode && (!localReady() || !localComposeProven)))
        intent.nativeSuppressed = false;
}
// Caller holds the exclusive lock.
void publish() noexcept {
    ControlIntent intent;
    intent.rawAlgo = lastAlgo;
    intent.rawEnable = switchEnabled ? 1 : 0;
    const auto menuTick = characterMenuTick.load(std::memory_order_relaxed);
    intent.characterMenu = menuTick && GetTickCount64() - menuTick < characterMenuHoldMs;
    intent.gameplayGateRequired = gameplayReader != nullptr || intent.characterMenu;
    intent.gameplayState = gameplayState;
    intent.gameplayReady = gameplayReader && gameplayState == gameplay_state::active && !intent.characterMenu;
    intent.inputObserved = algoObserved && switchObserved;
    intent.requestedEnabled = intent.inputObserved && algoFsr3 && switchEnabled;
    intent.nativeSuppressed = intent.requestedEnabled && (localFiMode
        ? localComposeProven && localReady() : forcedLast || backstopLast);
    intent.paired = intent.gatesConsistent = intent.inputObserved;
    const bool effectiveEnabled = intent.requestedEnabled && gameplayAllowsInterpolation(intent);
    if (intent.inputObserved && (!modeObserved || modeEnabled != effectiveEnabled)) {
        ++epoch;
        modeObserved = true;
        modeEnabled = effectiveEnabled;
    }
    intent.controlEpoch = epoch;
    refreshReadiness(intent);
    latest = intent;
}
bool algoAddress(void* params, std::uintptr_t& address) noexcept {
    address = reinterpret_cast<std::uintptr_t>(params);
    if (!address || address > std::numeric_limits<std::uintptr_t>::max() - frameInterpolationAlgoOffset - sizeof(std::uint32_t))
        return false;
    address += frameInterpolationAlgoOffset;
    return true;
}
bool readAlgo(void* params, std::uint32_t& algo) noexcept {
    std::uintptr_t address{};
    SIZE_T copied{};
    return algoAddress(params, address) &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &algo, sizeof(algo), &copied) &&
        copied == sizeof(algo);
}
bool writeAlgo(void* params, std::uint32_t algo) noexcept {
    std::uintptr_t address{};
    SIZE_T copied{};
    return algoAddress(params, address) &&
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &algo, sizeof(algo), &copied) &&
        copied == sizeof(algo);
}
void* invoke(Gate gate, void* owner, void* output, void* input, void* graph,
             std::uint32_t passIndex) {
    const auto index = static_cast<std::size_t>(gate);
    const auto original = originals[index].load(std::memory_order_acquire);
    // The installation contract makes an entry without its original unreachable.
    if (!original) std::abort();
    const auto entryError = GetLastError();
    bool observe{}, suppressNow{}, localBefore{};
    GameplayStateReader readGameplay{};
    ComposeSink sink{};
    {
        SharedLock guard;
        observe = active && !stopped;
        suppressNow = observe && suppress;
        localBefore = localReady();
        if (observe && gate == Gate::Compose) sink = composeSink;
        if (observe && gate == Gate::Compose) readGameplay = gameplayReader;
    }
    const auto observedGameplay = readGameplay ? readGameplay() : gameplay_state::unknown;
    std::array<unsigned char, 2> prefix{};
    const std::size_t bytes = gate == Gate::Compose ? prefix.size() : 1;
    SIZE_T copied{};
    const bool read = observe && input &&
        ReadProcessMemory(GetCurrentProcess(), input, prefix.data(), bytes, &copied) && copied == bytes;
    bool localCandidate{};
    if (localBefore && gate == Gate::Compose && read && prefix[0] == 0 && prefix[1] == 1) {
        SharedLock guard;
        localCandidate = active && !stopped && suppress && localReady() &&
            localParamsProven && algoObserved && algoFsr3 && !localUnexpectedNative;
    }
    // The registration reader and sink must never run under the menu lock.
    const bool composeMatched = sink && sink(input, graph, GetCurrentThreadId(), localCandidate);
    bool useShadow{}, clearSwitch{};
    if (observe) {
        ExclusiveLock guard;
        ++counters.calls[index];
        if (active && !stopped) {
            if (localFiMode) {
                localComposeProven = false;
                if (read && prefix[0]) {
                    localUnexpectedNative = true;
                    ++counters.localUnexpectedNativeEnabled;
                }
            }
            // A set native enable means params were built before the switch was
            // seen. Keep native FI from running on that frame with the pinned
            // OFF branches, which read only +0 (FI) or +0/+1 (Compose).
            if (read && prefix[0]) {
                ++counters.enabledGateCalls;
                useShadow = suppressNow;
                if (useShadow) ++counters.backstopShadows;
            }
            if (gate == Gate::Interpolation) {
                backstopLast = useShadow;
                if (read && prefix[0]) switchObserved = switchEnabled = true;
            } else {
                if (readGameplay) gameplayState = observedGameplay;
                switchObserved = read;
                switchEnabled = read && prefix[1] != 0;
                backstopLast = backstopLast || useShadow;
                // While native FSR3 FI is suppressed, Compose must see the game's
                // own FG-OFF input {0,0}. With +0 clear and +1 set it still queues
                // a per-frame FI device call (48F95D: device vtable+0xEE0 with
                // callback 0xEB21B0 -> vtable+0xE40) that a real OFF frame never
                // makes. LocalFiOff requires this call's proof before clearing +1.
                if (localFiMode) {
                    localComposeProven = localCandidate && composeMatched && localReady();
                    clearSwitch = localComposeProven;
                    if (read && prefix[1]) {
                        if (localComposeProven) ++counters.localComposeMatched;
                        else ++counters.localComposeRefused;
                    }
                } else {
                    clearSwitch = suppressNow && read && prefix[1] != 0 && (forcedLast || useShadow);
                }
                if (clearSwitch) {
                    useShadow = true;
                    ++counters.composeOffShadows;
                }
                publish();
            }
        }
    }
    prefix[0] = 0;
    if (clearSwitch) prefix[1] = 0;
    SetLastError(entryError);
    return original(owner, output, useShadow ? prefix.data() : input, graph, passIndex);
}
}

bool suppressionRequested() noexcept {
    const auto error = GetLastError();
    wchar_t value[8]{};
    const bool requested = GetEnvironmentVariableW(suppressionOption, value, 8) == 1 && value[0] == L'1';
    SetLastError(error);
    return requested;
}
bool configureVerified(GateFunction interpolation, GateFunction compose, RenderParamsFunction renderParams,
                       bool suppressNative, ComposeSink sink, bool localFiOff, LocalFiOffActive ready,
                       GameplayStateReader readGameplay) noexcept {
    ExclusiveLock guard;
    if (configured || stopped || !interpolation || !compose || !renderParams ||
        (localFiOff && (!suppressNative || !sink || !ready))) return false;
    originals[0].store(interpolation, std::memory_order_release);
    originals[1].store(compose, std::memory_order_release);
    renderParamsOriginal.store(renderParams, std::memory_order_release);
    suppress = suppressNative;
    composeSink = sink;
    localFiMode = localFiOff;
    localActive = ready;
    gameplayReader = readGameplay;
    latest.gameplayGateRequired = readGameplay != nullptr;
    configured = true;
    return true;
}
bool activate() noexcept {
    ExclusiveLock guard;
    if (!configured || active || stopped) return false;
    active = true;
    latest.controlEpoch = ++epoch;
    refreshReadiness(latest);
    return true;
}
void stop() noexcept {
    ExclusiveLock guard;
    if (!stopped) latest.controlEpoch = ++epoch;
    stopped = true;
    active = false;
    localParamsProven = false;
    localComposeProven = false;
    refreshReadiness(latest);
}
ControlIntent snapshot() noexcept {
    SharedLock guard;
    auto result = latest;
    refreshReadiness(result);
    return result;
}
void characterMenuObservedAt(std::uint64_t tickMs) noexcept {
    characterMenuTick.store(tickMs ? tickMs : 1, std::memory_order_relaxed);
}
void characterMenuObserved() noexcept { characterMenuObservedAt(GetTickCount64()); }
Stats stats() noexcept {
    SharedLock guard;
    auto result = counters;
    result.localFiOffActive = localReady();
    return result;
}
void* interpolationEntry(void* owner, void* output, void* input, void* graph, std::uint32_t passIndex) {
    return invoke(Gate::Interpolation, owner, output, input, graph, passIndex);
}
void* composeEntry(void* owner, void* output, void* input, void* graph, std::uint32_t passIndex) {
    return invoke(Gate::Compose, owner, output, input, graph, passIndex);
}
void* renderParamsEntry(void* params, void* camera, float first, float second) {
    const auto original = renderParamsOriginal.load(std::memory_order_acquire);
    if (!original) std::abort();
    auto* result = original(params, camera, first, second);
    const auto error = GetLastError();
    bool observe{}, suppressNow{}, switchOn{}, localMode{}, localCommitted{};
    {
        SharedLock guard;
        observe = active && !stopped;
        suppressNow = observe && suppress;
        switchOn = switchObserved && switchEnabled;
        localMode = localFiMode;
        localCommitted = localReady();
    }
    if (observe) {
        std::uint32_t algo{};
        const bool read = result && readAlgo(result, algo);
        // Only while the menu switch is on would native FI run; with the switch
        // off the game is left exactly as it is. The caller has not stored or
        // consumed these params yet, so the whole frame sees one value.
#ifdef XESS_FSR_MENU_CONTROL_TEST
        const bool legacyKeepNativeAlgo = legacyKeepNativeAlgoForTest;
#else
        const bool legacyKeepNativeAlgo = !localMode && diagnostic_flags::keepNativeAlgo();
#endif
        const bool keepNativeAlgo = localMode ? localCommitted : legacyKeepNativeAlgo;
        const bool attempted = read && suppressNow && switchOn && algo == fsr3FrameInterpolation && !keepNativeAlgo;
        const bool forced = attempted && writeAlgo(result, noFrameInterpolation);
        ExclusiveLock guard;
        ++counters.renderParamsCalls;
        if (!read) ++counters.readFailures;
        if (attempted && !forced) ++counters.writeFailures;
        if (forced) ++counters.forcedOff;
        if (active && !stopped) {
            if (localMode) {
                localComposeProven = false;
                localUnexpectedNative = false;
                localParamsProven = read && algo == fsr3FrameInterpolation && localCommitted && localReady();
                if (localParamsProven)
                    ++counters.localAlgoPreserved;
            }
            lastAlgo = read ? algo : 0;
            algoObserved = read;
            algoFsr3 = read && algo == fsr3FrameInterpolation;
            forcedLast = forced;
            publish();
        }
    }
    SetLastError(error);
    return result;
}
#ifdef XESS_FSR_MENU_CONTROL_TEST
void resetForTest() noexcept {
    ExclusiveLock guard;
    originals[0].store(nullptr);
    originals[1].store(nullptr);
    renderParamsOriginal.store(nullptr);
    configured = active = stopped = suppress = false;
    modeObserved = modeEnabled = false;
    epoch = 0;
    lastAlgo = 0;
    algoObserved = algoFsr3 = forcedLast = false;
    switchObserved = switchEnabled = backstopLast = false;
    latest = {};
    counters = {};
    composeSink = nullptr;
    localFiMode = localParamsProven = localComposeProven = localUnexpectedNative = false;
    localActive = nullptr;
    gameplayReader = nullptr;gameplayState = gameplay_state::unknown;
    characterMenuTick.store(0);
    legacyKeepNativeAlgoForTest = false;
}
void keepNativeAlgoForTest(bool value) noexcept { legacyKeepNativeAlgoForTest = value; }
#endif
}
