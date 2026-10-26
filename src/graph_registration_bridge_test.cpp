#include "graph_registration_bridge.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace rb = xess_vk::registration_bridge;
extern "C" {
std::uint32_t grb_fixture_enabled{};
std::uintptr_t grb_fixture_expected_rsp{}, grb_fixture_returned_rbp{};
std::uintptr_t grb_fixture_seen_rsp{}, grb_fixture_seen_return{};
std::uint64_t grb_fixture_seen_rcx{}, grb_fixture_seen_rdx{}, grb_fixture_seen_r8{};
std::uint64_t grb_fixture_invoke(rb::Original, void*, std::uint64_t, const void*);
void grb_fixture_original();
void grb_fixture_return();
std::uint64_t grb_dispatch(const rb::EntrySnapshot*, const rb::Route*);
std::uintptr_t grb_test_original_call_address() noexcept;
struct UnwindPoint { std::uintptr_t pc; std::uint32_t phase, padding; };
extern const UnwindPoint grb_unwind_points[], grb_unwind_points_end[];
}

namespace {
constexpr DWORD incomingError = 0x10203040, outgoingError = 0x50607080;
constexpr DWORD observerError = 0xdeadbeef, abnormalError = 0x61728394;
constexpr DWORD sehCode = 0xe0531234, observerSehCode = 0xe0535678;
enum class Action {
    Normal, CppThrow, SehUnwind, SehContinue, Nested, Capacity, Stop,
    BeforeCpp, BeforeCppPublished, BeforeSehPublished, CloseCpp,
    BeforeReentry, CloseReentry, NativeWalk
};
enum class CloseFailure { None, Cpp, Seh };
struct Input {
    Action action{Action::Normal};
    std::uint32_t depth{};
    CloseFailure failClose{CloseFailure::None};
};
struct Marker { unsigned route; std::uint32_t pass; };
struct ObserverMarker {};
struct State { unsigned route; rb::Control control; };
State states[3]{{0, {}}, {1, {}}, {2, {}}};
std::atomic<std::uint64_t> sequence{}, originalCalls{}, failures{}, destructorCalls{};
std::atomic<std::uint64_t> beforeCalls{}, closeCalls{}, unavailableCloses{};
std::uint64_t checks{}, cases{};
std::uint64_t backendWalks{};
thread_local bool inReentry{};
struct Frame {
    rb::Token token{};
    rb::EntrySnapshot entry{};
    std::uint64_t result{};
    bool originalSeen{};
};
thread_local std::array<Frame, rb::maximumNesting> frames{};
thread_local std::uint32_t frameCount{};
rb::EntrySnapshot rawEntry{};

void require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void observe(bool value) noexcept {
    if (!value) failures.fetch_add(1, std::memory_order_relaxed);
}
rb::Original entry(unsigned route) {
    return route == 0 ? grb_fi_prepare_entry :
        route == 1 ? grb_fi_copy_entry : grb_upscale_prepare_entry;
}
std::uint64_t value(unsigned route, std::uint32_t pass, const void* source) {
    return (0xaabbccdd00000000ull + route * 0x11111111ull) ^
        (std::uint64_t(pass) * 0x100000001b3ull) ^ reinterpret_cast<std::uintptr_t>(source);
}
std::uint64_t invoke(unsigned route, Input& input, std::uint32_t pass) {
    SetLastError(incomingError);
    return entry(route)(&input, pass, &input);
}
bool before(void* context, const rb::EntrySnapshot& snapshot, rb::BeforeResult& result) {
    auto& state = *static_cast<State*>(context);
    const auto& input = *reinterpret_cast<const Input*>(snapshot.rcx);
    beforeCalls.fetch_add(1, std::memory_order_relaxed);
    observe(GetLastError() == incomingError && snapshot.lastError == incomingError);
    observe(snapshot.threadId == GetCurrentThreadId());
    observe(snapshot.r8 == snapshot.rcx && (snapshot.rsp & 15) == 8);
    if (grb_fixture_enabled) rawEntry = snapshot;
    SetLastError(observerError);
    if (input.action == Action::BeforeCpp) throw ObserverMarker{};
    observe(frameCount < frames.size());
    if (frameCount >= frames.size()) return false;
    result.token = {std::uint64_t(snapshot.threadId) << 32 | state.route,
        sequence.fetch_add(1, std::memory_order_relaxed) + 1};
    frames[frameCount++] = Frame{result.token, snapshot, 0, false};
    result.associationAvailable = true;
    result.published = true;
    if (input.action == Action::BeforeCppPublished) throw ObserverMarker{};
    if (input.action == Action::BeforeSehPublished)
        RaiseException(observerSehCode, 0, 0, nullptr);
    if (input.action == Action::BeforeReentry && !inReentry) {
        inReentry = true;
        Input child{};
        const auto actual = invoke((state.route + 1) % 3, child, 42);
        observe(actual == value((state.route + 1) % 3, 42, &child));
        inReentry = false;
    }
    SetLastError(observerError);
    return true;
}
bool close(void* context, const rb::CloseEvent& event) {
    auto& state = *static_cast<State*>(context);
    const auto& input = *reinterpret_cast<const Input*>(event.entry.rcx);
    closeCalls.fetch_add(1, std::memory_order_relaxed);
    observe(GetLastError() == event.lastError && frameCount != 0);
    if (frameCount == 0) return false;
    const auto frame = frames[--frameCount];
    observe(frame.token.owner == event.token.owner && frame.token.scope == event.token.scope);
    observe(std::memcmp(&frame.entry, &event.entry, sizeof(event.entry)) == 0);
    observe(frame.originalSeen);
    if (event.exit == rb::Exit::Normal) {
        observe(event.copiedPayload == frame.result && event.lastError == outgoingError);
    } else {
        observe(event.copiedPayload == 0 && event.lastError == abnormalError);
    }
    if (event.associationUnavailable) unavailableCloses.fetch_add(1, std::memory_order_relaxed);
    SetLastError(observerError);
    if (input.action == Action::CloseCpp || input.failClose == CloseFailure::Cpp)
        throw ObserverMarker{};
    if (input.failClose == CloseFailure::Seh) RaiseException(observerSehCode, 0, 0, nullptr);
    if (input.action == Action::CloseReentry && !inReentry) {
        inReentry = true;
        Input child{};
        const auto actual = invoke((state.route + 1) % 3, child, 24);
        observe(actual == value((state.route + 1) % 3, 24, &child));
        inReentry = false;
    }
    SetLastError(observerError);
    return true;
}
struct Guard {
    ~Guard() { destructorCalls.fetch_add(1, std::memory_order_relaxed); }
};
__attribute__((noinline)) void walkNativeFrames(unsigned route, const rb::EntrySnapshot& snapshot) {
    CONTEXT context{};
    RtlCaptureContext(&context);
    DWORD64 dispatchBase{}, originalBase{}, entryBase{};
    auto* dispatchFunction = RtlLookupFunctionEntry(reinterpret_cast<std::uintptr_t>(&grb_dispatch),
        &dispatchBase, nullptr);
    auto* originalFunction = RtlLookupFunctionEntry(grb_test_original_call_address(), &originalBase, nullptr);
    auto* entryFunction = RtlLookupFunctionEntry(reinterpret_cast<std::uintptr_t>(entry(route)),
        &entryBase, nullptr);
    observe(dispatchFunction != nullptr && originalFunction != nullptr && entryFunction != nullptr);
    if (!dispatchFunction || !originalFunction || !entryFunction) return;
    const auto* info = reinterpret_cast<const std::uint8_t*>(originalBase + originalFunction->UnwindData);
    observe((info[0] >> 3 & 3) == 3 && (info[3] & 15) == 5);
    bool seenDispatch{}, seenOriginal{}, seenEntry{};
    for (unsigned frame = 0; frame != 16; ++frame) {
        DWORD64 imageBase{}, establisher{};
        PVOID handlerData{};
        auto* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!function) {
            context.Rip = *reinterpret_cast<const std::uintptr_t*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        seenDispatch |= imageBase == dispatchBase && function == dispatchFunction;
        seenOriginal |= imageBase == originalBase && function == originalFunction;
        const bool isEntry = imageBase == entryBase && function == entryFunction;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context,
            &handlerData, &establisher, nullptr);
        if (isEntry) {
            observe(context.Rip == snapshot.returnAddress && context.Rsp == snapshot.rsp + 8 &&
                context.Rbp == snapshot.rbp);
            seenEntry = true;
            break;
        }
    }
    observe(seenDispatch && seenOriginal && seenEntry);
    ++backendWalks;
}
__attribute__((noinline)) std::uint64_t original(unsigned route, void* graph,
    std::uint32_t pass, const void* source) {
    observe(GetLastError() == incomingError && graph == source);
    originalCalls.fetch_add(1, std::memory_order_relaxed);
    auto& input = *static_cast<Input*>(graph);
    Frame* owned = frameCount != 0 && frames[frameCount - 1].entry.rcx ==
        reinterpret_cast<std::uintptr_t>(graph) ? &frames[frameCount - 1] : nullptr;
    if (owned) owned->originalSeen = true;
    if (input.action == Action::NativeWalk && owned) walkNativeFrames(route, owned->entry);
    if (input.action == Action::CppThrow) {
        Guard guard;
        SetLastError(abnormalError);
        throw Marker{route, pass};
    }
    if (input.action == Action::SehUnwind || input.action == Action::SehContinue) {
        SetLastError(abnormalError);
        const ULONG_PTR arguments[]{route, pass};
        RaiseException(sehCode, 0, 2, arguments);
    }
    auto result = value(route, pass, source);
    if ((input.action == Action::Nested || input.action == Action::Capacity) && input.depth != 0) {
        Input child{input.action, input.depth - 1};
        result ^= invoke((route + 1) % 3, child, pass + 1);
    }
    if (input.action == Action::Stop) states[route].control.sampling.store(false);
    if (owned) owned->result = result;
    SetLastError(outgoingError);
    return result;
}

// Native SEH wrappers intentionally contain no objects needing C++ destruction.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
LONG filter(EXCEPTION_POINTERS* exception, unsigned route, std::uint32_t pass, bool resume) {
    const auto& record = *exception->ExceptionRecord;
    observe(record.ExceptionCode == sehCode && record.NumberParameters == 2);
    observe(record.ExceptionInformation[0] == route && record.ExceptionInformation[1] == pass);
    return resume ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_EXECUTE_HANDLER;
}
bool sehOutside(unsigned route, Input& input, std::uint32_t pass, bool resume,
    std::uint64_t* result) {
    __try { *result = invoke(route, input, pass); }
    __except(filter(GetExceptionInformation(), route, pass, resume)) { return true; }
    return false;
}
#pragma clang diagnostic pop

std::size_t unwindAssembly() {
    alignas(16) std::array<std::uint8_t, 4096> stack{};
    const auto rsp = reinterpret_cast<std::uintptr_t>(stack.data() + 2056);
    const auto pc = reinterpret_cast<std::uintptr_t>(&unwindAssembly);
    constexpr std::uint64_t rbp = 0x2132435465768798;
    std::memcpy(reinterpret_cast<void*>(rsp), &pc, 8);
    std::memcpy(reinterpret_cast<void*>(rsp - 8), &rbp, 8);
    std::size_t tested{};
    for (auto* point = grb_unwind_points; point != grb_unwind_points_end; ++point) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        context.Rip = point->pc;
        context.Rbp = point->phase >= 3 && point->phase <= 4 ? rsp - 104 : rbp;
        switch (point->phase) {
        case 0: case 5: context.Rsp = rsp; break;
        case 1: case 4: context.Rsp = rsp - 8; break;
        case 2: case 3: context.Rsp = rsp - 104; break;
        default: throw std::runtime_error("bad ASM unwind phase");
        }
        DWORD64 imageBase{}, establisher{};
        PVOID handlerData{};
        auto* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        require(function != nullptr, "ASM RUNTIME_FUNCTION absent");
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context,
            &handlerData, &establisher, nullptr);
        require(context.Rip == pc && context.Rsp == rsp + 8 && context.Rbp == rbp,
            "ASM instruction-state RtlVirtualUnwind mismatch");
        ++tested;
    }
    return tested;
}
void normalCase(Action action, unsigned route = 0) {
    Input input{action};
    const auto count = originalCalls.load();
    const auto result = invoke(route, input, 0xfedcba98);
    const DWORD error = GetLastError();
    require(result == value(route, 0xfedcba98, &input) && error == outgoingError,
        "normal RAX/LastError changed");
    require(originalCalls.load() == count + 1 && frameCount == 0,
        "normal original count/token lifetime mismatch");
    ++cases;
}
}

extern "C" std::uint64_t grb_test_original0(void* graph, std::uint32_t pass, const void* source) {
    return original(0, graph, pass, source);
}
extern "C" std::uint64_t grb_test_original1(void* graph, std::uint32_t pass, const void* source) {
    return original(1, graph, pass, source);
}
extern "C" std::uint64_t grb_test_original2(void* graph, std::uint32_t pass, const void* source) {
    return original(2, graph, pass, source);
}
extern "C" {
const rb::Route grb_fi_prepare_route{reinterpret_cast<rb::Original>(&grb_fixture_original),
    before, close, &states[0], &states[0].control};
const rb::Route grb_fi_copy_route{grb_test_original1, before, close, &states[1], &states[1].control};
const rb::Route grb_upscale_prepare_route{grb_test_original2, before, close, &states[2], &states[2].control};
}

int main() {
    try {
        const auto unwindPoints = unwindAssembly();
        require(unwindPoints == 57, "unexpected per-entry ASM instruction coverage");
        ++cases;
        Input raw{};
        grb_fixture_enabled = 1;
        SetLastError(incomingError);
        const auto rawResult = grb_fixture_invoke(grb_fi_prepare_entry, &raw,
            0x99887766fedcba98ull, &raw);
        const DWORD rawError = GetLastError();
        grb_fixture_enabled = 0;
        require(rawResult == value(0, 0xfedcba98, &raw) && rawError == outgoingError,
            "raw fixture result or LastError changed");
        require(rawEntry.rsp == grb_fixture_expected_rsp && rawEntry.rbp == 0x123456789abcdef0ull,
            "original RSP/RBP snapshot mismatch");
        require(rawEntry.returnAddress == reinterpret_cast<std::uintptr_t>(&grb_fixture_return),
            "caller continuation snapshot mismatch");
        require(rawEntry.rdx == 0x99887766fedcba98ull &&
            grb_fixture_seen_rdx == 0xfedcba98 &&
            grb_fixture_seen_rcx == rawEntry.rcx && grb_fixture_seen_r8 == rawEntry.r8,
            "raw args snapshot or typed EDX contract mismatch");
        require(grb_fixture_seen_return != rawEntry.returnAddress &&
            (grb_fixture_seen_rsp & 15) == 8 && grb_fixture_returned_rbp == rawEntry.rbp,
            "real target continuation/alignment/nonvolatile RBP mismatch");
        ++cases;
        for (unsigned route = 0; route != 3; ++route) normalCase(Action::Normal, route);
        for (unsigned route = 0; route != 3; ++route) normalCase(Action::NativeWalk, route);
        require(backendWalks == 3, "three route live backend RtlVirtualUnwind walks absent");

        Input cpp{Action::CppThrow};
        const auto beforeAbnormal = states[1].control.abnormalCloses.load();
        const auto beforeDtor = destructorCalls.load();
        bool caught{};
        try { (void)invoke(1, cpp, 73); }
        catch (const Marker& marker) { caught = marker.route == 1 && marker.pass == 73; }
        require(caught && destructorCalls.load() == beforeDtor + 1 && frameCount == 0,
            "C++ exception identity/destruction/token cleanup failed");
        require(states[1].control.abnormalCloses.load() == beforeAbnormal + 1,
            "C++ original did not issue exactly one abnormal close");
        ++cases;

        // A second failure inside the close observer must not replace the
        // original's already-unwinding C++ exception or native SEH record.
        const auto closeFailureStart = states[1].control.closeFailures.load();
        for (CloseFailure failure : {CloseFailure::Cpp, CloseFailure::Seh}) {
            Input throwing{Action::CppThrow, 0, failure};
            const auto ab = states[1].control.abnormalCloses.load();
            caught = false;
            try { (void)invoke(1, throwing, 81); }
            catch (const Marker& marker) { caught = marker.route == 1 && marker.pass == 81; }
            require(caught && frameCount == 0 && states[1].control.abnormalCloses.load() == ab + 1,
                "observer failure replaced original C++ exception");
            ++cases;
            Input throwingSeh{Action::SehUnwind, 0, failure};
            std::uint64_t result{};
            require(sehOutside(1, throwingSeh, 82, false, &result) && frameCount == 0,
                "observer failure replaced original SEH exception");
            ++cases;
        }
        require(states[1].control.closeFailures.load() == closeFailureStart + 4,
            "unwinding close observer failures not contained");

        for (bool resume : {false, true}) {
            Input seh{resume ? Action::SehContinue : Action::SehUnwind};
            auto& control = states[2].control;
            const auto ab = control.abnormalCloses.load(), no = control.normalCloses.load();
            std::uint64_t result{};
            const bool handled = sehOutside(2, seh, 91, resume, &result);
            require(handled == !resume && frameCount == 0, "SEH propagation/token cleanup failed");
            require(control.abnormalCloses.load() == ab + (resume ? 0 : 1) &&
                control.normalCloses.load() == no + (resume ? 1 : 0),
                "continued/unwound SEH wrong exit classification");
            if (resume) require(result == value(2, 91, &seh) && GetLastError() == outgoingError,
                "continued SEH normal result changed");
            ++cases;
        }

        Input nested{Action::Nested, 5};
        const auto nestedBefore = originalCalls.load(), nestedClose = closeCalls.load();
        (void)invoke(0, nested, 1);
        require(originalCalls.load() == nestedBefore + 6 && closeCalls.load() == nestedClose + 6 &&
            frameCount == 0, "nested independent routes leaked/crossed tokens");
        ++cases;
        for (Action action : {Action::BeforeReentry, Action::CloseReentry}) {
            Input input{action};
            const auto count = originalCalls.load();
            (void)invoke(0, input, 1);
            require(originalCalls.load() == count + 2 && frameCount == 0,
                "observer reentry lifetime mismatch");
            ++cases;
        }

        Input capacity{Action::Capacity, rb::maximumNesting + 4};
        const auto capacityBefore = originalCalls.load(), capacityClose = closeCalls.load();
        std::uint64_t capacityCount{};
        for (auto& state : states) capacityCount += state.control.capacity.load();
        (void)invoke(0, capacity, 1);
        std::uint64_t capacityAfter{};
        for (auto& state : states) capacityAfter += state.control.capacity.load();
        require(originalCalls.load() == capacityBefore + rb::maximumNesting + 5 &&
            closeCalls.load() == capacityClose + rb::maximumNesting &&
            capacityAfter == capacityCount + 5 && frameCount == 0,
            "capacity refused call borrowed parent token or stopped original");
        ++cases;

        const auto failuresBefore = states[0].control.beforeFailures.load();
        const auto unavailableBefore = unavailableCloses.load();
        for (Action action : {Action::BeforeCpp, Action::BeforeCppPublished, Action::BeforeSehPublished})
            normalCase(action);
        require(states[0].control.beforeFailures.load() == failuresBefore + 3 &&
            unavailableCloses.load() == unavailableBefore + 2,
            "observer C++/SEH failure or already-published token not contained");
        const auto closeFailures = states[0].control.closeFailures.load();
        normalCase(Action::CloseCpp);
        require(states[0].control.closeFailures.load() == closeFailures + 1,
            "close observer throw not contained");
        const auto stopClose = closeCalls.load(), stopBefore = beforeCalls.load();
        normalCase(Action::Stop);
        normalCase(Action::Normal);
        require(closeCalls.load() == stopClose + 1 && beforeCalls.load() == stopBefore + 1 &&
            states[0].control.stopped.load() == 1,
            "stop during original lost inflight close or admitted new association");
        states[0].control.sampling.store(true);

        const auto concurrentOriginal = originalCalls.load(), concurrentClose = closeCalls.load();
        std::array<std::thread, 6> workers;
        for (unsigned i = 0; i != workers.size(); ++i) workers[i] = std::thread([i] {
            for (std::uint32_t pass = 0; pass != 300; ++pass) {
                Input input{};
                const auto result = invoke(i % 3, input, pass);
                observe(result == value(i % 3, pass, &input) && GetLastError() == outgoingError);
            }
            observe(frameCount == 0);
        });
        for (auto& worker : workers) worker.join();
        require(originalCalls.load() == concurrentOriginal + 1800 &&
            closeCalls.load() == concurrentClose + 1800, "concurrent original/close counts mismatch");
        ++cases;
        require(failures.load() == 0 && frameCount == 0, "native observer invariant failed");
        std::cout << "{\"passed\":true,\"cases\":" << cases << ",\"assertions\":" << checks
            << ",\"asmUnwindInstructionStates\":" << unwindPoints
            << ",\"liveBackendUnwindWalks\":" << backendWalks
            << ",\"originalCalls\":" << originalCalls.load()
            << ",\"nativeCppException\":true,\"nativeSehUnwind\":true,\"sehContinueExecution\":true"
            << ",\"observerCppAndSehContained\":true,\"observerFailureDuringOriginalUnwind\":true"
            << ",\"immutableRoutes\":3,\"concurrentCalls\":1800"
            << ",\"nativeStaticTls\":true,\"gamePatched\":false,\"gpuWork\":false"
            << ",\"sameFrameVerified\":false,\"xeFGConstantsReady\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "graph-registration-bridge-test: " << error.what() << '\n';
        return 1;
    }
}
