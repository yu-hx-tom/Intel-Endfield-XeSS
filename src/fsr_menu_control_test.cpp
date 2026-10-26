#include "fsr_menu_control.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace menu_fixture {
namespace menu = xess_vk::fsr_menu_control;
unsigned checks{};
void require(bool value, const char* why) {
    ++checks;
    if (!value) throw std::runtime_error(why);
}
std::array<std::atomic<unsigned>, 2> originalCalls{};
std::atomic<unsigned> paramsCalls{};
std::atomic<bool> contractCorrect{true};

struct GateExpected {
    void* owner{};
    void* output{};
    unsigned char* input{};
    void* graph{};
    void* returned{};
    std::uint32_t passIndex{};
    bool shadow{};
    bool clearShadowSwitch{true};
};
thread_local GateExpected gateExpected;
void* gateOriginal(std::size_t gate, void* owner, void* output, void* input, void* graph, std::uint32_t passIndex) {
    ++originalCalls[gate];
    const auto& e = gateExpected;
    bool okay = owner == e.owner && output == e.output && graph == e.graph && passIndex == e.passIndex &&
        GetLastError() == 0x12345678;
    if (e.shadow) {
        // A local OFF shadow: never the caller's memory, +0 cleared, and Compose
        // gets the game's own FG-OFF input {0,0} (+1 set with +0 clear would
        // queue an extra per-frame FI device call that real OFF frames never make).
        const auto* bytes = static_cast<const unsigned char*>(input);
        okay = okay && input && input != e.input && bytes[0] == 0 &&
            (gate == 0 || bytes[1] == (e.clearShadowSwitch ? 0 : e.input[1]));
    } else {
        okay = okay && input == e.input;
    }
    if (!okay) contractCorrect = false;
    SetLastError(0x76543210);
    return e.returned;
}
void* interpolation(void* a, void* b, void* c, void* d, std::uint32_t e) { return gateOriginal(0, a, b, c, d, e); }
void* compose(void* a, void* b, void* c, void* d, std::uint32_t e) { return gateOriginal(1, a, b, c, d, e); }

// Owned stand-in for the Unity params object; the fake builder writes the
// algo like 239D4D does, then returns the params pointer in RAX.
struct Params {
    std::array<unsigned char, 0xA00> bytes{};
    std::uint32_t algo() const { std::uint32_t v{}; std::memcpy(&v, bytes.data() + menu::frameInterpolationAlgoOffset, 4); return v; }
    void setAlgo(std::uint32_t v) { std::memcpy(bytes.data() + menu::frameInterpolationAlgoOffset, &v, 4); }
};
struct ParamsExpected {
    Params* params{};
    void* camera{};
    float first{}, second{};
    std::uint32_t algo{};
    bool returnNull{}, stopInside{};
};
thread_local ParamsExpected paramsExpected;
void* paramsOriginal(void* params, void* camera, float first, float second) {
    ++paramsCalls;
    const auto& e = paramsExpected;
    if (params != e.params || camera != e.camera || first != e.first || second != e.second ||
        GetLastError() != 0x12345678) contractCorrect = false;
    if (e.params) e.params->setAlgo(e.algo);
    if (e.stopInside) menu::stop();
    SetLastError(0x76543210);
    return e.returnNull ? nullptr : params;
}

unsigned sinkCalls{};
void* sinkInput{};
void* sinkGraph{};
bool sinkMatched{true}, sinkStops{}, sinkCandidate{};
std::atomic<bool> localCertificate{};
bool localReady() noexcept { return localCertificate.load(); }
bool composeSink(void* input, void* graph, std::uint32_t thread, bool candidate) noexcept {
    ++sinkCalls;
    sinkCandidate = candidate;
    if (input != sinkInput || graph != sinkGraph || thread != GetCurrentThreadId() ||
        !menu::snapshot().bothGatesReady) contractCorrect = false;
    SetLastError(0xDEADBEEF);
    if (sinkStops) menu::stop();
    return sinkMatched;
}
void reset(bool suppress, bool activate = true, menu::ComposeSink sink = nullptr, bool localMode = false,
           menu::GameplayStateReader gameplay = nullptr) {
    menu::resetForTest();
    originalCalls[0] = originalCalls[1] = 0;
    paramsCalls = 0;
    contractCorrect = true;
    sinkMatched = true; sinkStops = sinkCandidate = false; localCertificate.store(false);
    require(!menu::configureVerified(interpolation, compose, nullptr, suppress, sink),
            "configuration requires the render-parameter original");
    if (localMode) {
        require(!menu::configureVerified(interpolation, compose, paramsOriginal, suppress, sink, true),
                "local decisions require a process-lived certificate reader");
        require(!menu::configureVerified(interpolation, compose, paramsOriginal, suppress, nullptr, true, localReady),
                "local decisions require an exact same-call Compose consumer");
    }
    require(menu::configureVerified(interpolation, compose, paramsOriginal, suppress, sink,
                                   localMode, localMode ? localReady : nullptr, gameplay),
            "publish three actual typed originals");
    if (activate) require(menu::activate(), "activate only the configured set");
}
void gate(menu::Gate which, unsigned char* input, bool shadow, void* owner = nullptr,
          std::uint32_t passIndex = 7, bool clearShadowSwitch = true) {
    std::array<std::uint64_t, 2> output{};
    int graph{}, returned{}, localOwner{};
    gateExpected = {owner ? owner : &localOwner, output.data(), input, &graph, &returned,
                    passIndex, shadow, clearShadowSwitch};
    sinkInput = input; sinkGraph = &graph;
    SetLastError(0x12345678);
    auto entry = which == menu::Gate::Interpolation ? menu::interpolationEntry : menu::composeEntry;
    const auto result = entry(gateExpected.owner, output.data(), input, &graph, passIndex);
    if (result != &returned || GetLastError() != 0x76543210) contractCorrect = false;
}
// Runs the params entry exactly as the three Unity callers do and returns the algo they would store.
std::uint32_t build(Params* params, std::uint32_t algo, bool returnNull = false, bool stopInside = false) {
    int camera{};
    paramsExpected = {params, &camera, 1.25f, -3.5f, algo, returnNull, stopInside};
    SetLastError(0x12345678);
    const auto result = menu::renderParamsEntry(params, &camera, 1.25f, -3.5f);
    if (result != (returnNull ? nullptr : params) || GetLastError() != 0x76543210) contractCorrect = false;
    return params ? params->algo() : 0;
}
// One rendered frame as the renderer orders it. The native enable mirrors
// 1EFAB8..1EFAD3 (switch && algo==1, view+3B assumed clear).
struct Frame { std::uint32_t storedAlgo{}; menu::ControlIntent intent; };
Frame frame(bool menuSwitch, std::uint32_t algo, bool expectFiShadow) {
    Params params;
    std::fill(params.bytes.begin(), params.bytes.end(), static_cast<unsigned char>(0x5A));
    const auto stored = build(&params, algo);
    const unsigned char nativeEnable = menuSwitch && stored == menu::fsr3FrameInterpolation;
    std::array<unsigned char, 1> fi{nativeEnable};
    std::array<unsigned char, 0x50> composeInput{nativeEnable, static_cast<unsigned char>(menuSwitch)};
    // Suppressed frames (backstop, or params rewritten FSR3 -> OFF) shadow Compose too.
    const bool composeShadow = expectFiShadow ||
        (menuSwitch && algo == menu::fsr3FrameInterpolation && stored == menu::noFrameInterpolation);
    gate(menu::Gate::Interpolation, fi.data(), expectFiShadow);
    gate(menu::Gate::Compose, composeInput.data(), composeShadow);
    require(fi[0] == nativeEnable && composeInput[0] == nativeEnable && composeInput[1] == menuSwitch,
            "the caller's gate inputs are never written");
    return {stored, menu::snapshot()};
}

void menuSequence() {
    reset(true, true, composeSink);
    sinkCalls = 0;
    // OFF: the game is left alone (algo stays FSR3, both gates see native OFF).
    auto f = frame(false, 1, false);
    std::uint64_t epoch = f.intent.controlEpoch;
    require(f.storedAlgo == 1 && f.intent.inputObserved && !f.intent.requestedEnabled && f.intent.rawAlgo == 1 &&
            !f.intent.rawEnable && menu::stats().forcedOff == 0, "switch OFF never rewrites params");
    // The frame the switch turns on: params were built before it was seen, so
    // both late gates take the OFF shadow and native FI still does not run.
    f = frame(true, 1, true);
    require(f.storedAlgo == 1 && f.intent.requestedEnabled && f.intent.nativeSuppressed && f.intent.rawEnable &&
            f.intent.controlEpoch > epoch && menu::stats().backstopShadows == 2,
            "first ON frame is suppressed by the backstop shadow and starts a new epoch");
    epoch = f.intent.controlEpoch;
    // Following ON frames: params say no interpolation, Interpolation forwards the
    // caller's input and Compose gets the game's own FG-OFF input {0,0}.
    for (int i = 0; i < 3; ++i) {
        f = frame(true, 1, false);
        require(f.storedAlgo == 0 && f.intent.requestedEnabled && f.intent.nativeSuppressed &&
                f.intent.controlEpoch == epoch, "ON frames build the complete native OFF graph");
    }
    require(menu::stats().forcedOff == 3 && menu::stats().backstopShadows == 2 && menu::stats().enabledGateCalls == 2,
            "rewrite replaces the backstop after one frame");
    require(menu::stats().composeOffShadows == 4,
            "every suppressed ON frame (backstop and rewritten) hands Compose the FG-OFF input");
    // The frame the switch turns off still has stale-ON params (rewritten, harmless).
    f = frame(false, 1, false);
    require(f.storedAlgo == 0 && !f.intent.requestedEnabled && f.intent.controlEpoch > epoch,
            "switch OFF ends the request in a new epoch");
    epoch = f.intent.controlEpoch;
    f = frame(false, 1, false);
    require(f.storedAlgo == 1 && !f.intent.requestedEnabled && f.intent.controlEpoch == epoch && menu::stats().forcedOff == 4,
            "afterwards the game is left exactly as it is");
    require(sinkCalls == 7 && contractCorrect && originalCalls[0] == 7 && originalCalls[1] == 7,
            "every gate calls its original once and Compose always feeds the region sink");
    // Other interpolation algorithms are never requested or rewritten.
    for (const std::uint32_t algo : {2u, 4u, 8u}) {
        frame(true, algo, false);
        f = frame(true, algo, false);
        require(f.storedAlgo == algo && !f.intent.requestedEnabled && !f.intent.nativeSuppressed,
                "non-FSR3 algorithms stay untouched");
    }
    require(contractCorrect, "algo checks preserved the gate ABI");
}

void failuresAndStop() {
    reset(true);
    frame(true, 1, true);
    Params params;
    require(build(&params, 1) == 0, "switch already observed ON rewrites the next params");
    build(nullptr, 1, true);
    require(!menu::snapshot().inputObserved && !menu::snapshot().requestedEnabled && menu::stats().readFailures == 1,
            "an unreadable params result revokes the request instead of keeping the previous frame");
    gate(menu::Gate::Compose, nullptr, false);
    require(!menu::snapshot().inputObserved && contractCorrect, "an unreadable Compose input revokes the switch");
    Params stopped;
    require(build(&stopped, 1, false, true) == 1 && !menu::snapshot().bothGatesReady && !menu::activate() && contractCorrect,
            "stop inside the original wins; no rewrite after stop and stop stays sticky");
    std::array<unsigned char, 2> enabled{1, 1};
    gate(menu::Gate::Interpolation, enabled.data(), false);
    gate(menu::Gate::Compose, enabled.data(), false);
    require(build(&stopped, 1) == 1 && contractCorrect, "stopped forwarders keep original behavior and ABI");

    reset(true, false);
    Params early;
    std::array<unsigned char, 2> on{1, 1};
    require(build(&early, 1) == 1, "not-yet-active params entry leaves the algo");
    gate(menu::Gate::Interpolation, on.data(), false);
    require(contractCorrect && !menu::snapshot().bothGatesReady && menu::stats().calls[0] == 0,
            "not-yet-active gates forward without observation or shadow");

    reset(false);
    frame(true, 1, false);
    const auto observed = frame(true, 1, false);
    require(observed.storedAlgo == 1 && observed.intent.requestedEnabled && !observed.intent.nativeSuppressed &&
            menu::stats().backstopShadows == 0 && contractCorrect,
            "without the explicit native-off option intent is observed and native FI is left running");
}

void concurrentParams() {
    reset(true);
    frame(true, 1, true);
    std::vector<std::thread> threads;
    std::array<std::atomic<unsigned>, 4> wrong{};
    for (unsigned thread = 0; thread < 4; ++thread) threads.emplace_back([&, thread] {
        for (unsigned invocation = 0; invocation < 64; ++invocation) {
            Params params;
            const std::uint32_t algo = (thread + invocation) % 2;
            if (build(&params, algo) != 0) ++wrong[thread];
        }
    });
    for (auto& thread : threads) thread.join();
    const auto stats = menu::stats();
    require(contractCorrect && stats.renderParamsCalls == 257 && stats.forcedOff == 128 && !stats.writeFailures &&
            std::all_of(wrong.begin(), wrong.end(), [](const auto& v) { return v.load() == 0; }),
            "concurrent constructions each rewrite only their own params");
}

void exactOptIn() {
    SetLastError(0x12345678);
    SetEnvironmentVariableW(menu::suppressionOption, nullptr);
    require(!menu::suppressionRequested(), "native suppression defaults off");
    SetEnvironmentVariableW(menu::suppressionOption, L"01");
    require(!menu::suppressionRequested(), "native suppression requires literal opt-in");
    SetEnvironmentVariableW(menu::suppressionOption, L"1");
    SetLastError(0x12345678);
    require(menu::suppressionRequested() && GetLastError() == 0x12345678, "literal opt-in lookup preserves caller LastError");
    SetEnvironmentVariableW(menu::suppressionOption, nullptr);
}
void localDecisionSequence() {
    reset(true, true, composeSink, true);
    menu::keepNativeAlgoForTest(true);
    Params params;
    std::array<unsigned char, 2> on{0, 1}, off{0, 0}, nativeOn{1, 1};
    build(&params, 1);
    gate(menu::Gate::Compose, on.data(), false);
    require(!menu::snapshot().nativeSuppressed && build(&params, 1) == 0,
            "LocalFiOff without a certificate ignores residual KeepNativeAlgo and retains legacy suppression");
    localCertificate.store(true);
    gate(menu::Gate::Compose, on.data(), false);
    require(!menu::snapshot().nativeSuppressed,
            "certificate becoming active cannot authenticate params already rewritten in this frame");
    require(build(&params, 1) == 1 && !menu::snapshot().nativeSuppressed,
            "committed local decisions preserve algo but need this Compose's graph proof");
    gate(menu::Gate::Interpolation, off.data(), false);
    gate(menu::Gate::Compose, on.data(), true);
    require(menu::snapshot().nativeSuppressed && menu::snapshot().rawEnable == 1 && sinkCandidate &&
            menu::stats().localComposeMatched == 1,
            "current exact graph and original zero-native/ON-switch prefix admit local suppression");
    gate(menu::Gate::Interpolation, off.data(), false);
    require(!menu::snapshot().nativeSuppressed,
            "a new gate cannot leave the previous Compose's published proof live");
    sinkMatched = false;
    gate(menu::Gate::Compose, on.data(), false);
    require(!menu::snapshot().nativeSuppressed,
            "another graph/ticket cannot borrow the previous successful Compose proof");
    sinkMatched = true;
    require(build(&params, 1) == 1, "a new certified frame still preserves FSR3");
    gate(menu::Gate::Interpolation, nativeOn.data(), true);
    gate(menu::Gate::Compose, on.data(), false);
    require(!menu::snapshot().nativeSuppressed && !sinkCandidate && menu::stats().localUnexpectedNativeEnabled == 1,
            "an unexpected native FI enable at the earlier gate invalidates this frame despite late backstop");
    build(&params, 1);
    gate(menu::Gate::Compose, nativeOn.data(), true, nullptr, 7, false);
    require(!menu::snapshot().nativeSuppressed && !sinkCandidate && menu::stats().localUnexpectedNativeEnabled == 2,
            "native-enable one is only backstopped, never cleared/admitted as the new zero-native prefix");
    build(&params, 1);
    gate(menu::Gate::Compose, off.data(), false);
    require(!menu::snapshot().requestedEnabled && !menu::snapshot().nativeSuppressed && params.algo() == 1,
            "menu OFF forwards its original prefix and keeps the original algo");
    build(&params, 2);
    gate(menu::Gate::Compose, on.data(), false);
    require(!menu::snapshot().requestedEnabled && !menu::snapshot().nativeSuppressed && params.algo() == 2,
            "another original algorithm is never claimed as local FSR3 suppression");
    build(&params, 1);
    gate(menu::Gate::Compose, on.data(), true);
    localCertificate.store(false);
    require(!menu::snapshot().nativeSuppressed, "certificate revocation invalidates an already published intent");
    gate(menu::Gate::Compose, on.data(), false);
    localCertificate.store(true);
    build(&params, 1);
    sinkStops = true;
    gate(menu::Gate::Compose, on.data(), false);
    require(!menu::snapshot().bothGatesReady && !menu::snapshot().nativeSuppressed && contractCorrect,
            "stop during this sink call defeats its match and preserves original forwarding/LastError");

    reset(true, true, composeSink);
    menu::keepNativeAlgoForTest(true);
    frame(true, 1, true);
    require(build(&params, 1) == 1, "legacy KeepNativeAlgo retains its diagnostic meaning without LocalFiOff");
}
std::int32_t testGameplayState=xess_vk::gameplay_state::unknown;
std::int32_t gameplayReader() noexcept {
    // The reader is called outside the menu lock and cannot alter LastError.
    (void)menu::snapshot();SetLastError(0xABCDEF01);return testGameplayState;
}
void gameplayStartup() {
    using namespace xess_vk::gameplay_state;
    reset(true,true,nullptr,false,gameplayReader);
    require(menu::snapshot().gameplayGateRequired&&!menu::auxiliaryCaptureRequired(menu::snapshot()),
            "unobserved startup cannot capture auxiliary FG inputs");
    testGameplayState=unknown;
    auto f=frame(true,1,true);auto epoch=f.intent.controlEpoch;
    require(f.intent.requestedEnabled&&!menu::gameplayAllowsInterpolation(f.intent)&&!menu::auxiliaryCaptureRequired(f.intent),
            "persisted menu ON is distinct from startup eligibility");
    for(auto state:{0,10,20,30,39}){
        testGameplayState=state;f=frame(true,1,false);
        require(f.intent.gameplayState==state&&!menu::gameplayAllowsInterpolation(f.intent)&&f.intent.controlEpoch==epoch,
                "login and loading states stay single-output without generating epochs per sample");
    }
    testGameplayState=active;f=frame(true,1,false);
    require(f.intent.gameplayReady&&menu::gameplayAllowsInterpolation(f.intent)&&menu::auxiliaryCaptureRequired(f.intent)&&f.intent.controlEpoch>epoch,
            "gameplay enables the existing menu request and resets FG history once");
    epoch=f.intent.controlEpoch;f=frame(true,1,false);
    require(f.intent.controlEpoch==epoch,"steady gameplay preserves the interpolation epoch");
    testGameplayState=10;f=frame(true,1,false);
    require(!f.intent.gameplayReady&&f.intent.controlEpoch>epoch,"return to login revokes FG");
    testGameplayState=active;f=frame(false,1,false);
    require(!f.intent.requestedEnabled&&!menu::auxiliaryCaptureRequired(f.intent),"gameplay does not override menu OFF");
    require(contractCorrect,"gameplay reads preserve ABI and LastError");

    // Owned address-space stand-in exercises the actual pointer traversal.
    std::array<unsigned char,0x100> type{};
    std::array<unsigned char,0x60> instance{};
    std::array<unsigned char,0x30> stateObject{};
    auto typePointer=reinterpret_cast<std::uintptr_t>(type.data());
    auto instancePointer=reinterpret_cast<std::uintptr_t>(instance.data());
    auto statePointer=reinterpret_cast<std::uintptr_t>(stateObject.data());
    auto fieldsPointer=reinterpret_cast<std::uintptr_t>(&instancePointer);
    std::uint32_t initialized=1;std::int32_t value=active;
    std::memcpy(type.data()+0xe0,&initialized,4);std::memcpy(type.data()+0xb8,&fieldsPointer,8);
    std::memcpy(instance.data()+0x48,&statePointer,8);std::memcpy(stateObject.data()+0x20,&value,4);
    const auto read=[](std::uintptr_t address,auto& result){SIZE_T copied{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),&result,sizeof(result),&copied)&&copied==sizeof(result);};
    const auto slot=reinterpret_cast<std::uintptr_t>(&typePointer);
    require(xess_vk::gameplay_state::read(slot,read)==active,"native game-state chain matches the pinned gameplay property");
    initialized=0;std::memcpy(type.data()+0xe0,&initialized,4);
    require(xess_vk::gameplay_state::read(slot,read)==unknown,"uninitialized types are not initialized by our layer");
    initialized=1;std::memcpy(type.data()+0xe0,&initialized,4);statePointer=0;std::memcpy(instance.data()+0x48,&statePointer,8);
    require(xess_vk::gameplay_state::read(slot,read)==unknown,"missing state object cannot grant interpolation");
    require(xess_vk::gameplay_state::read(UINTPTR_MAX,read)==unknown,"unreadable and overflowing pointers stay OFF");
}
}
namespace menu_fixture {
void characterMenuGate() {
    using namespace xess_vk::gameplay_state;
    reset(true,true,nullptr,false,gameplayReader);
    testGameplayState=active;auto f=frame(true,1,false);
    require(f.intent.gameplayReady&&!f.intent.characterMenu,"gameplay before the character menu allows FG");
    auto epoch=f.intent.controlEpoch;
    menu::characterMenuObserved();f=frame(true,1,false);
    require(f.intent.characterMenu&&f.intent.gameplayState==active&&!f.intent.gameplayReady&&!menu::gameplayAllowsInterpolation(f.intent)&&
            !menu::auxiliaryCaptureRequired(f.intent)&&f.intent.controlEpoch>epoch,
            "the character menu keeps FG off although its gameplay state stays active");
    epoch=f.intent.controlEpoch;f=frame(true,1,false);
    require(f.intent.characterMenu&&f.intent.controlEpoch==epoch,"a steady menu does not generate epochs");
    menu::characterMenuObservedAt(GetTickCount64()-menu::characterMenuHoldMs-1);f=frame(true,1,false);
    require(!f.intent.characterMenu&&f.intent.gameplayReady&&f.intent.controlEpoch>epoch,"FG returns once the menu's passes stop");
    reset(true,true);menu::characterMenuObserved();f=frame(true,1,false);
    require(f.intent.gameplayGateRequired&&!menu::gameplayAllowsInterpolation(f.intent),"the menu gate applies without a gameplay reader");
}
}
int main() {
    using namespace menu_fixture;
    try {
        exactOptIn(); menuSequence(); failuresAndStop(); concurrentParams(); localDecisionSequence(); gameplayStartup(); characterMenuGate();
        std::cout << "{\"stage\":\"production-fsr-menu-adapters\",\"passed\":true,\"checks\":" << checks
            << ",\"gpuTouched\":false,\"gameTouched\":false,\"hooksInstalled\":false}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
