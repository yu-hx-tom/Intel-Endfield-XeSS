#include "game_frame_timing.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <windows.h>

namespace gt = xess_vk::game_timing;
extern "C" {
bool gft_test_invoke_sim_end(std::uint64_t);
extern const unsigned char gft_test_sim_end_return[];
extern const unsigned char gft_test_sim_end_original[];
extern std::uint64_t gft_test_helper_calls, gft_test_helper_rbx, gft_test_helper_return;
void gft_test_clobber();
std::uint64_t gft_test_invoke_enqueue(std::uintptr_t, std::uintptr_t, std::uintptr_t, double, double);
extern const unsigned char gft_test_enqueue_original[];
extern std::uint64_t gft_test_enqueue_flags, gft_test_enqueue_args[5];
extern std::uint64_t gft_test_enqueue_original_calls;
}
namespace {
std::array<std::byte, 0xD0> owner{};
std::array<gt::Event, 32> events{};
std::size_t eventCount{};
std::uint64_t originalCalls{};
std::uintptr_t moduleBase{};
bool failReads{}, throwSink{}, faultSink{};
unsigned ownerReads{}, keyReads{}, readerCalls{};
enum class KeyReadMode { Stable, ChangedOwner, ChangedCounter, OverflowOwner };
KeyReadMode keyReadMode{};
constexpr std::uint64_t opaqueResult = 0xABCDEF1234567890ull;

void require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}
void setSimKey(std::uint64_t value) { std::memcpy(owner.data() + 0xC8, &value, sizeof(value)); }
bool reader(void*, std::uintptr_t address, void* output, std::size_t bytes) {
    ++readerCalls;
    SetLastError(0x3333);
    if (failReads) return false;
    if (address == moduleBase + 0x1EE5E48 && bytes == sizeof(std::uintptr_t)) {
        ++ownerReads;
        auto pointer = reinterpret_cast<std::uintptr_t>(owner.data());
        if (keyReadMode == KeyReadMode::ChangedOwner && ownerReads == 2)
            pointer += 8;
        if (keyReadMode == KeyReadMode::OverflowOwner)
            pointer = UINTPTR_MAX - 4;
        std::memcpy(output, &pointer, bytes);
    } else {
        std::memcpy(output, reinterpret_cast<const void*>(address), bytes);
        if (address == reinterpret_cast<std::uintptr_t>(owner.data()) + 0xC8 && bytes == 8) {
            ++keyReads;
            if (keyReadMode == KeyReadMode::ChangedCounter && keyReads == 2) {
                std::uint64_t value{};
                std::memcpy(&value, output, 8);
                ++value;
                std::memcpy(output, &value, 8);
            }
        }
    }
    return true;
}
bool sink(void*, const gt::Event& event) {
    events.at(eventCount++) = event;
    SetLastError(0x4444);
    gft_test_clobber();
    if (throwSink) throw std::runtime_error("observer exception");
    if (faultSink) RaiseException(0xE0420011, 0, 0, nullptr);
    return true;
}
std::uint64_t simOriginal() {
    ++originalCalls;
    setSimKey(0x112233440000002Au);
    SetLastError(0x7777);
    return opaqueResult;
}
std::uint64_t pointerOriginal(std::uint32_t* key) {
    ++originalCalls;
    if (key && key != reinterpret_cast<std::uint32_t*>(1)) *key = 999;
    SetLastError(0x7777);
    return opaqueResult;
}
double floatOriginal(std::uint32_t* key) {
    ++originalCalls;
    if (key) *key = 999;
    SetLastError(0x7777);
    return 123.375;
}
std::uint64_t throwingOriginal(std::uint32_t*) {
    ++originalCalls;
    throw std::runtime_error("original exception");
}
void bindOriginals() {
    const auto& sites = gt::hookSites();
    *sites[0].originalTrampoline = reinterpret_cast<void*>(&simOriginal);
    *sites[1].originalTrampoline = const_cast<unsigned char*>(gft_test_sim_end_original);
    for (std::size_t i = 2; i < sites.size(); ++i)
        *sites[i].originalTrampoline = reinterpret_cast<void*>(&pointerOriginal);
}
using Sim = std::uint64_t (*)();
using Pointer = std::uint64_t (*)(std::uint32_t*);
using Float = double (*)(std::uint32_t*);
std::array<gt::EnqueueEvent, 8> enqueueEvents{};
std::size_t enqueueCount{};
bool enqueueThrows{};
bool enqueueSink(void*, const gt::EnqueueEvent& event) {
    enqueueEvents.at(enqueueCount++) = event;
    SetLastError(0xDEAD);
    gft_test_clobber();
    return true;
}
std::uint64_t throwingEnqueue(std::uintptr_t, std::uintptr_t, std::uintptr_t, double, double) {
    ++originalCalls;
    if (enqueueThrows) throw std::runtime_error("enqueue original exception");
    return opaqueResult;
}
void enqueueAbi() {
    gt::stop();
    const auto entrances = gt::counters().enqueueEntrances;
    require(gt::configure({moduleBase, nullptr, sink, reader, enqueueSink}) && gt::activate(), "configure enqueue pre-entry");
    const auto& site = gt::hookSites()[6];
    require(site.targetRva == 0x13D106C && site.copiedInstructionLength == 7 &&
            site.boundary == gt::Boundary::EntryBeforeOriginal && site.abi == gt::SourceAbi::EnqueuePresentOpaqueFive,
            "enqueue site prefix/full ABI contract mismatch");
    *site.originalTrampoline = const_cast<unsigned char*>(gft_test_enqueue_original);
    std::array<std::byte, 0x160> backend{};
    const std::uintptr_t primary = 0x1122334455667788ull, alternate = 0x8877665544332211ull;
    std::memcpy(backend.data() + 0x8, &primary, 8);
    std::memcpy(backend.data() + 0x150, &alternate, 8);
    const std::uint64_t inFlightCount = 19;
    std::memcpy(backend.data() + 0x148, &inFlightCount, 8);
    const auto state = reinterpret_cast<std::uintptr_t>(backend.data());
    constexpr std::uintptr_t packet = 0x1234567890ABCDEFull, context = 0xFEEDBEEFAABBCCDDull;
    const auto invoke = [&] {
        const auto calls = gft_test_enqueue_original_calls;
        SetLastError(0x5151);
        require(gft_test_invoke_enqueue(state, packet, context, 32.375, 256.625) == opaqueResult,
                "enqueue pre-observer changed original RAX return");
        double scheduled{}, extra{};
        std::memcpy(&scheduled, &gft_test_enqueue_args[3], 8);
        std::memcpy(&extra, &gft_test_enqueue_args[4], 8);
        require(gft_test_enqueue_args[0] == state && gft_test_enqueue_args[1] == packet &&
                gft_test_enqueue_args[2] == context && scheduled == 32.375 && extra == 256.625 &&
                (gft_test_enqueue_flags & 1) && GetLastError() == 0x5151,
                "enqueue changed RCX/RDX/R8/XMM3/stack fifth/flags/LastError");
        require(gft_test_enqueue_original_calls == calls + 1, "enqueue original was not called exactly once");
    };
    invoke();
    require(gt::counters().enqueueEntrances == entrances + 1 && enqueueCount == 1 && enqueueEvents[0].state == state && enqueueEvents[0].queue == alternate &&
            enqueueEvents[0].presentInfo == packet && enqueueEvents[0].queueValid,
            "enqueue did not use the actual consumer's alternate queue selector");
    const std::uintptr_t zero{};
    std::memcpy(backend.data() + 0x150, &zero, 8);
    invoke();
    require(enqueueCount == 2 && enqueueEvents[1].queue == primary, "enqueue base queue fallback changed");
    failReads = true;
    invoke();
    require(!enqueueEvents[2].queueValid && !enqueueEvents[2].queue, "failed enqueue reader retained a previous queue");
    failReads = false;
    *site.originalTrampoline = reinterpret_cast<void*>(&throwingEnqueue);
    enqueueThrows = true;
    bool propagated{};
    using Enqueue = std::uint64_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, double, double);
    try { reinterpret_cast<Enqueue>(const_cast<void*>(site.entry))(state, packet, context, 1.0, 2.0); }
    catch (const std::runtime_error& error) { propagated = std::strcmp(error.what(), "enqueue original exception") == 0; }
    require(propagated && !gt::callbacksInFlight(), "enqueue tail changed original native unwind");
    enqueueThrows = false;
    gt::stop();
    const auto count = enqueueCount;
    const auto reads = readerCalls;
    const auto before = gt::counters();
    require(reinterpret_cast<Enqueue>(const_cast<void*>(site.entry))(1, 1, context, 1.0, 2.0) == opaqueResult &&
            enqueueCount == count && readerCalls == reads && gt::counters().emitted == before.emitted &&
            gt::counters().enqueueEntrances == before.enqueueEntrances + 1,
            "stopped enqueue counts entry and forwards without any parameter reads or callback");
}
}

int main() {
    try {
        constexpr std::uintptr_t routeBase=0x180000000;
        for(const auto rva:gt::directPresentReturnRvas)
            require(gt::directPresentCaller(routeBase,routeBase+rva),"fixed ordinary direct return is recognized");
        for(const auto rva:{0x19EF19u,0xD38845u,0x19EF8Cu,0x19EF8Eu,0u})
            require(!gt::directPresentCaller(routeBase,routeBase+rva),"proxy/async/nearby returns cannot become direct sources");
        require(!gt::directPresentCaller(0,routeBase+0x19EF8D) &&
                !gt::directPresentCaller(UINTPTR_MAX,routeBase+0x19EF8D),"unknown base and wrapped addresses refuse direct identity");
        moduleBase = reinterpret_cast<std::uintptr_t>(gft_test_sim_end_return) - 0xC9906F;
        require(gt::configure({moduleBase, nullptr, sink, reader}), "configure");
        require(!gt::activate(), "no publication with missing original");
        bindOriginals();
        require(gt::activate(), "activate complete originals");
        require(!gt::configure({moduleBase, nullptr, sink, reader}), "reject active reconfigure");
        const auto& sites = gt::hookSites();
        for (const auto& site : sites) {
            DWORD64 base{};
            require(RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(site.entry), &base, nullptr) != nullptr,
                    "entry has native unwind metadata");
        }

        setSimKey(41);
        SetLastError(0x1111);
        require(reinterpret_cast<Sim>(const_cast<void*>(sites[0].entry))() == opaqueResult,
                "opaque integer return preserved");
        require(originalCalls == 1 && events[0].engineFrame == 42 && events[0].frameKeyValid &&
                events[0].keySource == gt::FrameKeySource::GlobalC8AfterReturn,
                "simulation key is read after original advances C8");
        require(GetLastError() == 0x7777, "post observer preserves original LastError");

        for (std::size_t i = 2; i < 6; ++i) {
            std::uint32_t key = static_cast<std::uint32_t>(50 + i);
            const auto count = eventCount;
            require(reinterpret_cast<Pointer>(const_cast<void*>(sites[i].entry))(&key) == opaqueResult,
                    "pointer callback integer return");
            require(key == 999 && events[count].engineFrame == 50 + i && events[count].frameKeyValid &&
                    events[count].threadId == GetCurrentThreadId() && events[count].stage == sites[i].stage,
                    "pointer key copied at entry before original mutates it");
            require(GetLastError() == 0x7777, "pointer callback LastError");
        }
        require(originalCalls == 5, "all outer originals called once");

        std::uint32_t key = 123;
        *sites[2].originalTrampoline = reinterpret_cast<void*>(&floatOriginal);
        require(reinterpret_cast<Float>(const_cast<void*>(sites[2].entry))(&key) == 123.375,
                "opaque XMM0 double return preserved");
        *sites[2].originalTrampoline = reinterpret_cast<void*>(&pointerOriginal);

        const auto beforeHelper = eventCount;
        SetLastError(0x5151);
        require(gft_test_invoke_sim_end(0x998877660000004Du), "helper AL return preserved");
        require(eventCount == beforeHelper + 1, "matched helper emits once");
        require(events[beforeHelper].stage == gt::Stage::SimulationEnd &&
                events[beforeHelper].engineFrame == 77 &&
                events[beforeHelper].keySource == gt::FrameKeySource::CallerRbxDword &&
                events[beforeHelper].boundary == gt::Boundary::MatchedHelperEntry &&
                gft_test_helper_rbx == 0x998877660000004Du &&
                gft_test_helper_return == reinterpret_cast<std::uintptr_t>(gft_test_sim_end_return),
                "matched helper retains original RBX and return address");
        require(GetLastError() == 0x5151 && gft_test_helper_calls == 1, "tail helper LastError and once");
        require(reinterpret_cast<bool (*)()>(const_cast<void*>(sites[1].entry))(), "unrelated helper forwards");
        require(eventCount == beforeHelper + 1 && gft_test_helper_calls == 2 &&
                gt::counters().ignoredHelperCallers == 1, "unrelated caller emits no SIM_END");

        failReads = true;
        require(!gt::readCpuFrameKey().valid, "CPU field read refusal is not a valid frame");
        key = 6;
        const auto failedRead = eventCount;
        reinterpret_cast<Pointer>(const_cast<void*>(sites[2].entry))(&key);
        require(!events[failedRead].frameKeyValid && key == 999, "read refusal still calls original");
        failReads = false;
        setSimKey(0);
        ownerReads = keyReads = 0;
        SetLastError(0x5151);
        const auto cpuZero = gt::readCpuFrameKey();
        require(cpuZero.valid && cpuZero.engineFrame == 0 && cpuZero.owner == reinterpret_cast<std::uintptr_t>(owner.data()) &&
                ownerReads == 2 && keyReads == 2 && GetLastError() == 0x5151,
                "CPU key zero, stable double read and LastError preservation");
        for (const auto mode : {KeyReadMode::ChangedOwner, KeyReadMode::ChangedCounter, KeyReadMode::OverflowOwner}) {
            ownerReads = keyReads = 0;
            keyReadMode = mode;
            require(!gt::readCpuFrameKey().valid, "inconsistent or overflowing CPU owner/key was accepted");
        }
        keyReadMode = KeyReadMode::Stable;
        gt::stop();
        require(!gt::readCpuFrameKey().valid, "stopped CPU field reader refused");
        require(gt::configure({moduleBase, nullptr, sink, nullptr}) && gt::activate(), "native reader");
        const auto nativeFault = eventCount;
        reinterpret_cast<Pointer>(const_cast<void*>(sites[2].entry))(reinterpret_cast<std::uint32_t*>(1));
        require(!events[nativeFault].frameKeyValid && GetLastError() == 0x7777,
                "native-SEH invalid pointer is contained");

        throwSink = true;
        key = 7;
        require(reinterpret_cast<Pointer>(const_cast<void*>(sites[2].entry))(&key) == opaqueResult,
                "C++ sink failure contained");
        throwSink = false;
        faultSink = true;
        key = 8;
        require(reinterpret_cast<Pointer>(const_cast<void*>(sites[2].entry))(&key) == opaqueResult,
                "SEH sink failure contained");
        faultSink = false;
        require(gt::counters().callbackFailures == 2, "callback failure accounting");

        *sites[2].originalTrampoline = reinterpret_cast<void*>(&throwingOriginal);
        const auto beforeThrow = eventCount;
        bool originalPropagated{};
        try { reinterpret_cast<Pointer>(const_cast<void*>(sites[2].entry))(&key); }
        catch (const std::runtime_error& e) { originalPropagated = std::strcmp(e.what(), "original exception") == 0; }
        require(originalPropagated && eventCount == beforeThrow && !gt::callbacksInFlight(),
                "original unwind propagates without a false normal event");
        *sites[2].originalTrampoline = reinterpret_cast<void*>(&pointerOriginal);
        gt::stop();
        const auto beforeStop = eventCount;
        key = 9;
        require(reinterpret_cast<Pointer>(const_cast<void*>(sites[2].entry))(&key) == opaqueResult &&
                eventCount == beforeStop && key == 999, "stopped bridge forwards without event");
        enqueueAbi();
        const auto counters = gt::counters();
        std::printf("PASS game_frame_timing CPU: originalCalls=%llu events=%zu invalidKeys=%llu callbackFailures=%llu helperCalls=%llu; no game/GPU/SDK/hooks\n",
                    static_cast<unsigned long long>(originalCalls), eventCount,
                    static_cast<unsigned long long>(counters.invalidKeys),
                    static_cast<unsigned long long>(counters.callbackFailures),
                    static_cast<unsigned long long>(gft_test_helper_calls));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL game_frame_timing: %s\n", e.what());
        return 1;
    }
}
