#include "fsr_detour_entry.hpp"
#include <windows.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

struct RawSnapshot {
    uint64_t gp[7];
    uint8_t xmm[6][16];
    uint64_t flags, entryRsp, stack[2], returnAddress;
};
static_assert(offsetof(RawSnapshot, xmm) == 56 && offsetof(RawSnapshot, flags) == 152);
static_assert(offsetof(RawSnapshot, returnAddress) == 184);
struct UnwindPoint { uintptr_t pc; uint32_t phase, padding; };
extern "C" {
uint32_t fsr_test_mode = 0;
RawSnapshot fsr_test_seen{};
uint64_t fsr_test_expected_rsp = 0;
uint8_t fsr_test_xmm[6][16]{};
uint64_t fsr_test_thunk(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
double fsr_test_float_thunk(double, double, double, double, double, double);
void fsr_test_original();
uint64_t fsr_test_invoke_registers();
void fsr_test_raw_return();
void fsr_test_clobber();
extern const UnwindPoint fsr_observer_unwind_points[], fsr_observer_unwind_points_end[];
}
namespace {
constexpr DWORD incomingError = 0x12345678, outgoingError = 0x87654321;
constexpr uint64_t sampleLimit = 64;
std::atomic<uint64_t> helperCalls{}, originalCalls{}, diagnosticSamples{}, diagnosticFailures{},
    reentrantSkips{}, originalErrorMismatches{}, threadFailures{};
std::atomic<bool> sampling{true}, throwDiagnostic{false}, recurseDiagnostic{false};
thread_local bool insideDiagnostic = false, recursiveOriginal = false;
uint64_t checks = 0;
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void requireMh(MH_STATUS result, const char* step) {
    ++checks;
    if (result != MH_OK) throw std::runtime_error(std::string(step) + ": " + MH_StatusToString(result));
}
uint64_t integerValue(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    return (a * 0x100000001b3ull) ^ (b + 7 * c) ^ (d * 19) ^ (e << 11) ^ (f >> 3);
}
double floatingValue(double a, double b, double c, double d, double e, double f) {
    return a + 2 * b + 4 * c + 8 * d + 16 * e + 32 * f;
}
bool fixedThunkMatches() {
    const auto* p = reinterpret_cast<const uint8_t*>(&fsr_test_thunk);
    if (p[0] != 0xe9) return false;
    int32_t displacement{};
    std::memcpy(&displacement, p + 1, sizeof(displacement));
    return reinterpret_cast<uintptr_t>(p + 5) + displacement == reinterpret_cast<uintptr_t>(&fsr_test_original);
}
size_t testUnwind() {
    // Every actual bridge instruction boundary is exported by the self-test
    // assembly build; runtime objects contain neither this table nor fixtures.
    // The supplied contexts model the exact stack state at each boundary.
    alignas(16) std::array<uint8_t, 4096> stack{};
    const uintptr_t entryRsp = reinterpret_cast<uintptr_t>(stack.data() + 2056);
    constexpr uint64_t callerRbp = 0x1122334455667788ull;
    const uintptr_t callerPc = reinterpret_cast<uintptr_t>(&testUnwind);
    std::memcpy(reinterpret_cast<void*>(entryRsp), &callerPc, 8);
    std::memcpy(reinterpret_cast<void*>(entryRsp - 8), &callerRbp, 8);
    const uint64_t savedFlags = 0x247;
    std::memcpy(reinterpret_cast<void*>(entryRsp - 16), &savedFlags, 8);
    std::memcpy(reinterpret_cast<void*>(entryRsp - 224), &savedFlags, 8);
    size_t tested = 0;
    for (auto* point = fsr_observer_unwind_points; point != fsr_observer_unwind_points_end; ++point) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        context.Rip = point->pc;
        context.Rbp = point->phase >= 4 && point->phase <= 6 ? entryRsp - 216 : callerRbp;
        switch (point->phase) {
        case 0: case 7: context.Rsp = entryRsp; break;
        case 1: case 6: context.Rsp = entryRsp - 8; break;
        case 2: context.Rsp = entryRsp - 16; break;
        case 3: case 4: context.Rsp = entryRsp - 216; break;
        case 5: context.Rsp = entryRsp - 224; break;
        default: throw std::runtime_error("unknown unwind-point phase");
        }
        DWORD64 imageBase{};
        auto* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        require(function != nullptr, "missing bridge RUNTIME_FUNCTION");
        PVOID handlerData{}; DWORD64 establisher{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context,
                         &handlerData, &establisher, nullptr);
        if (context.Rip != callerPc || context.Rsp != entryRsp + 8 || context.Rbp != callerRbp) {
            throw std::runtime_error("RtlVirtualUnwind failed at bridge point " + std::to_string(tested) +
                                     ", phase " + std::to_string(point->phase));
        }
        ++checks; ++tested;
    }
    require(tested >= 30, "instruction-boundary unwind coverage too small");
    return tested;
}
void testRegisters() {
    fsr_test_mode = 0;
    for (size_t r = 0; r < 6; ++r) for (size_t b = 0; b < 16; ++b)
        fsr_test_xmm[r][b] = static_cast<uint8_t>(17 + 29 * r + b);
    SetLastError(incomingError);
    const uint64_t result = fsr_test_invoke_registers();
    const DWORD error = GetLastError();
    require(result == 0xabcdef0123456789ull, "raw RAX return changed");
    require(error == incomingError, "helper changed LastError before raw original");
    constexpr uint64_t expectedGp[]{0x0123456789abcdefull, 0x1023456789abcdefull,
        0x2023456789abcdefull, 0x3023456789abcdefull, 0x4023456789abcdefull,
        0x5023456789abcdefull, 0x6023456789abcdefull};
    for (size_t r = 0; r < 7; ++r)
        require(fsr_test_seen.gp[r] == expectedGp[r], "volatile GPR changed");
    require(std::memcmp(fsr_test_seen.xmm, fsr_test_xmm, sizeof(fsr_test_xmm)) == 0, "XMM0..5 changed");
    constexpr uint64_t arithmeticMask = 0x8d5; // OF/SF/ZF/AF/PF/CF; DF remains zero.
    require((fsr_test_seen.flags & arithmeticMask) == (0x247 & arithmeticMask), "arithmetic RFLAGS changed");
    require((fsr_test_seen.flags & 0x400) == 0, "DF changed");
    require(fsr_test_seen.entryRsp == fsr_test_expected_rsp, "original RSP changed");
    require(fsr_test_seen.returnAddress == reinterpret_cast<uintptr_t>(&fsr_test_raw_return), "original return address changed");
    require(fsr_test_seen.stack[0] == 0x5555666677778888ull && fsr_test_seen.stack[1] == 0x9999aaaabbbbccccull,
            "fifth/sixth stack argument changed");
}
void callInteger(uint64_t a) {
    constexpr uint64_t b = 0xabcde123, c = 0xffeeabcd, d = 0x1020304050607080ull,
        e = 0x554433221100ffaaull, f = 0xfedcba9876543210ull;
    SetLastError(incomingError);
    const auto result = fsr_test_thunk(a, b, c, d, e, f);
    const DWORD error = GetLastError();
    require(result == integerValue(a, b, c, d, e, f), "integer result or arguments changed");
    require(error == outgoingError, "original outgoing LastError changed");
}
}

extern "C" uint64_t fsr_test_int_original(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    const DWORD error = GetLastError();
    ++originalCalls;
    if (error != incomingError) ++originalErrorMismatches;
    auto value = integerValue(a, b, c, d, e, f);
    if (recursiveOriginal && a > 0) {
        SetLastError(incomingError);
        value += fsr_test_thunk(a - 1, b, c, d, e, f);
    }
    SetLastError(outgoingError);
    return value;
}
extern "C" double fsr_test_float_original(double a, double b, double c, double d, double e, double f) {
    const DWORD error = GetLastError();
    ++originalCalls;
    if (error != incomingError) ++originalErrorMismatches;
    const auto value = floatingValue(a, b, c, d, e, f);
    SetLastError(outgoingError);
    return value;
}
extern "C" void fsr_observe_before(void*, void*) noexcept {
    const DWORD saved = GetLastError();
    ++helperCalls;
    if (insideDiagnostic) {
        ++reentrantSkips;
    } else if (sampling.load()) {
        insideDiagnostic = true;
        try {
            auto samples = diagnosticSamples.load();
            while (samples < sampleLimit && !diagnosticSamples.compare_exchange_weak(samples, samples + 1)) {}
            if (throwDiagnostic.load()) throw std::runtime_error("synthetic diagnostic failure");
            if (recurseDiagnostic.load()) {
                SetLastError(incomingError);
                (void)fsr_test_thunk(0, 1, 2, 3, 4, 5);
            }
            SetLastError(0xdeadbeef);
        } catch (...) { ++diagnosticFailures; }
        insideDiagnostic = false;
    }
    SetLastError(saved);
    // Deliberately destroy every volatile GPR and XMM register. The bridge,
    // rather than an accidentally gentle C++ helper, must restore target input.
    fsr_test_clobber();
}

int main() {
    try {
        const size_t unwindPoints = testUnwind();
        require(fixedThunkMatches(), "fixture is not the expected fixed E9 thunk");
        std::array<uint8_t, 16> originalBytes{};
        std::memcpy(originalBytes.data(), reinterpret_cast<const void*>(&fsr_test_thunk), originalBytes.size());
        requireMh(MH_Initialize(), "MH_Initialize");
        require(MH_CreateHook(nullptr, reinterpret_cast<void*>(&fsr_observer_entry), nullptr) == MH_ERROR_NOT_EXECUTABLE,
                "invalid target was not refused");
        requireMh(MH_CreateHook(reinterpret_cast<void*>(&fsr_test_thunk), reinterpret_cast<void*>(&fsr_observer_entry),
                                &fsr_original_trampoline), "MH_CreateHook");
        require(fsr_original_trampoline != nullptr, "missing original trampoline");
        require(std::memcmp(originalBytes.data(), reinterpret_cast<const void*>(&fsr_test_thunk), originalBytes.size()) == 0,
                "CreateHook modified target before enable");
        requireMh(MH_EnableHook(reinterpret_cast<void*>(&fsr_test_thunk)), "MH_EnableHook");
        require(!fixedThunkMatches(), "EnableHook did not change target jump destination");
        // No disable/remove/uninitialize: installed code and trampoline live until
        // process exit, exactly as the proposed integration lifetime contract.
        testRegisters();
        fsr_test_mode = 1;
        auto before = originalCalls.load();
        for (uint64_t i = 0; i < 1000; ++i) callInteger(100 + i);
        require(originalCalls.load() - before == 1000, "integer original not called exactly once");
        throwDiagnostic = true;
        before = originalCalls.load();
        callInteger(345);
        throwDiagnostic = false;
        require(diagnosticFailures == 1 && originalCalls.load() - before == 1, "diagnostic exception blocked forwarding");
        recurseDiagnostic = true;
        before = originalCalls.load();
        callInteger(456);
        recurseDiagnostic = false;
        require(reentrantSkips == 1 && originalCalls.load() - before == 2, "diagnostic reentrancy did not forward nested call");
        recursiveOriginal = true;
        before = originalCalls.load();
        SetLastError(incomingError);
        const auto recursive = fsr_test_thunk(4, 11, 22, 33, 44, 55);
        const auto recursiveError = GetLastError();
        recursiveOriginal = false;
        uint64_t expected{};
        for (uint64_t a = 0; a <= 4; ++a) expected += integerValue(a, 11, 22, 33, 44, 55);
        require(recursive == expected && originalCalls.load() - before == 5, "original recursion changed");
        require(recursiveError == outgoingError, "recursive outgoing LastError changed");
        fsr_test_mode = 2;
        before = originalCalls.load();
        for (int i = 0; i < 1000; ++i) {
            const double a = i + 0.25, b = -0.125, c = 1024.5, d = -64.25, e = 0.0625, f = 32.5;
            SetLastError(incomingError);
            const auto value = fsr_test_float_thunk(a, b, c, d, e, f);
            const DWORD error = GetLastError();
            require(value == floatingValue(a, b, c, d, e, f), "XMM return or floating/stack args changed");
            require(error == outgoingError, "floating outgoing LastError changed");
        }
        require(originalCalls.load() - before == 1000, "floating original not called exactly once");
        fsr_test_mode = 1;
        before = originalCalls.load();
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < 8; ++t) workers.emplace_back([t] {
            for (uint64_t i = 0; i < 10000; ++i) {
                const uint64_t a = 100 + i, b = t + 1, c = 3, d = 4, e = 0xaabbccddeeff0011ull, f = 6;
                SetLastError(incomingError);
                const auto value = fsr_test_thunk(a, b, c, d, e, f);
                const DWORD error = GetLastError();
                if (value != integerValue(a, b, c, d, e, f) || error != outgoingError) ++threadFailures;
            }
        });
        for (auto& worker : workers) worker.join();
        require(threadFailures == 0 && originalCalls.load() - before == 80000, "multithread forwarding changed");
        require(diagnosticSamples <= sampleLimit, "diagnostic sample budget exceeded");
        sampling = false;
        const auto samplesBefore = diagnosticSamples.load();
        before = originalCalls.load();
        for (uint64_t i = 0; i < 100; ++i) callInteger(i + 1000);
        require(diagnosticSamples == samplesBefore && originalCalls.load() - before == 100,
                "sampling-off did not keep original forwarding");
        require(originalErrorMismatches == 0, "incoming LastError did not reach original");
        require(helperCalls == originalCalls + 1, "hook entry/original call count mismatch (one raw assembly call expected)");
        std::cout << "{\"passed\":true,\"checks\":" << checks << ",\"unwindInstructionPoints\":" << unwindPoints
                  << ",\"originalCalls\":" << originalCalls << ",\"helperCalls\":" << helperCalls
                  << ",\"diagnosticSamples\":" << diagnosticSamples << ",\"diagnosticFailuresContained\":" << diagnosticFailures
                  << ",\"realMinHook\":true,\"fixedE9Thunk\":true,\"gpRegistersCompared\":7,\"xmmRegistersCompared\":6"
                  << ",\"samplingOffForwardingVerified\":true,\"trampolineLifetime\":\"process\",\"gamePatched\":false"
                  << ",\"gameAbiVerified\":false,\"allGameThreadsSuspendedVerified\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fsr-detour-test: " << error.what() << '\n';
        return 1;
    }
}
