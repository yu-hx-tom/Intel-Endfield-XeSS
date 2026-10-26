#include "game_frame_timing.hpp"
#include <atomic>
#include <cstring>
#include <limits>
#include <windows.h>

#if !defined(__clang__) || !defined(_WIN64)
#error The timing bridges require the Clang Win64 native-SEH compiler contract.
#endif

extern "C" {
extern const unsigned char gft_sim_start_entry[], gft_sim_end_entry[];
extern const unsigned char gft_render_start_entry[], gft_render_end_entry[];
extern const unsigned char gft_present_start_entry[], gft_present_end_entry[];
extern const unsigned char gft_enqueue_entry[];
extern void* gft_sim_start_original;
extern void* gft_sim_end_original;
extern void* gft_render_start_original;
extern void* gft_render_end_original;
extern void* gft_present_start_original;
extern void* gft_present_end_original;
extern void* gft_enqueue_original;
}

namespace xess_vk::game_timing {
namespace {
Config config{};
std::atomic<bool> active{};
std::atomic<std::uint32_t> inFlight{};
std::atomic<std::uint64_t> emitted{}, invalidKeys{}, callbackFailures{}, ignoredHelperCallers{}, enqueueEntrances{};

const std::array<HookSite, hookCount> sites{{
    {Stage::SimulationStart, 0xC98E80, 0xC98E80, SourceAbi::NoArgumentsOpaqueReturn,
     Boundary::NormalReturn, FrameKeySource::GlobalC8AfterReturn, 5,
     {0x48,0x89,0x5C,0x24,0x20,0x57,0x48,0x83,0xEC,0x20,0xE8,0x51,0x4A,0x3D,0xFF,0x48},
     gft_sim_start_entry, &gft_sim_start_original},
    {Stage::SimulationEnd, 0x23D0F0, 0xC98FF0, SourceAbi::NoArgumentsBoolAl,
     Boundary::MatchedHelperEntry, FrameKeySource::CallerRbxDword, 7,
     {0x83,0x3D,0x05,0xB2,0xD0,0x01,0x00,0x75,0x03,0xB0,0x01,0xC3,0x32,0xC0,0xC3,0xCC},
     gft_sim_end_entry, &gft_sim_end_original},
    {Stage::RenderSubmitStart, 0xC988D0, 0xC988D0, SourceAbi::RcxDwordPointerOpaqueReturn,
     Boundary::NormalReturn, FrameKeySource::EntryRcxDword, 5,
     {0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0xE8,0xCE,0xA1},
     gft_render_start_entry, &gft_render_start_original},
    {Stage::RenderSubmitEnd, 0xC98940, 0xC98940, SourceAbi::RcxDwordPointerOpaqueReturn,
     Boundary::NormalReturn, FrameKeySource::EntryRcxDword, 5,
     {0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0xE8,0x5E,0xA1},
     gft_render_end_entry, &gft_render_end_original},
    {Stage::NativePresentStart, 0xD46DB0, 0xD46DB0, SourceAbi::RcxDwordPointerOpaqueReturn,
     Boundary::NormalReturn, FrameKeySource::EntryRcxDword, 5,
     {0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0xE8,0xEE,0xBC},
     gft_present_start_entry, &gft_present_start_original},
    {Stage::NativePresentEnd, 0xD46D40, 0xD46D40, SourceAbi::RcxDwordPointerOpaqueReturn,
     Boundary::NormalReturn, FrameKeySource::EntryRcxDword, 5,
     {0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0xE8,0x5E,0xBD},
     gft_present_end_entry, &gft_present_end_original},
    {Stage::NativePresentEnqueue, 0x13D106C, 0x13D106C, SourceAbi::EnqueuePresentOpaqueFive,
     Boundary::EntryBeforeOriginal, FrameKeySource::ExistingNativePresentScope, 7,
     {0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18,0x4C},
     gft_enqueue_entry, &gft_enqueue_original}
}};

// The assembly owns one POD snapshot per invocation, with no TLS stack or map.
struct PostSnapshot {
    std::uintptr_t keyAddress{};
    std::uint32_t engineFrame{}, threadId{};
    bool valid{}, sampled{};
    std::uint8_t padding[6]{};
};
static_assert(sizeof(PostSnapshot) == 24);

bool readCpp(std::uintptr_t address, void* output, std::size_t bytes) noexcept {
    try {
        if (config.reader) return config.reader(config.context, address, output, bytes);
        std::memcpy(output, reinterpret_cast<const void*>(address), bytes);
        return true;
    } catch (...) { return false; }
}
bool sinkCpp(const Event& event) noexcept {
    try { return config.sink(config.context, event); }
    catch (...) { return false; }
}
bool enqueueCpp(const EnqueueEvent& event) noexcept {
    try { return config.enqueueSink && config.enqueueSink(config.context, event); }
    catch (...) { return false; }
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
bool readSafe(std::uintptr_t address, void* output, std::size_t bytes) noexcept {
    if (!address || !output || !bytes || bytes > 8 ||
        address > std::numeric_limits<std::uintptr_t>::max() - bytes) return false;
    __try { return readCpp(address, output, bytes); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool sinkSafe(const Event& event) noexcept {
    __try { return sinkCpp(event); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool enqueueSafe(const EnqueueEvent& event) noexcept {
    __try { return enqueueCpp(event); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#pragma clang diagnostic pop

void publish(Event event) noexcept {
    if (!active.load(std::memory_order_acquire)) return;
    if (!event.frameKeyValid) invalidKeys.fetch_add(1, std::memory_order_relaxed);
    emitted.fetch_add(1, std::memory_order_relaxed);
    if (!sinkSafe(event)) callbackFailures.fetch_add(1, std::memory_order_relaxed);
}
}

const std::array<HookSite, hookCount>& hookSites() noexcept { return sites; }
bool configure(const Config& next) noexcept {
    if (active.load(std::memory_order_acquire) || inFlight.load(std::memory_order_acquire) ||
        !next.moduleBase || !next.sink ||
        next.moduleBase > std::numeric_limits<std::uintptr_t>::max() - 0x1D17F30) return false;
    config = next;
    return true;
}
bool activate() noexcept {
    if (!config.moduleBase || !config.sink) return false;
    for (const auto& site : sites) if (!*site.originalTrampoline) return false;
    active.store(true, std::memory_order_release);
    return true;
}
void stop() noexcept { active.store(false, std::memory_order_release); }
std::uint32_t callbacksInFlight() noexcept { return inFlight.load(std::memory_order_acquire); }
Counters counters() noexcept {
    return {emitted.load(std::memory_order_relaxed), invalidKeys.load(std::memory_order_relaxed),
            callbackFailures.load(std::memory_order_relaxed), ignoredHelperCallers.load(std::memory_order_relaxed),
            enqueueEntrances.load(std::memory_order_relaxed)};
}
CpuFrameKey readCpuFrameKey() noexcept {
    struct ReadOperation {
        DWORD error{GetLastError()};
        ReadOperation() { inFlight.fetch_add(1, std::memory_order_acq_rel); }
        ~ReadOperation() {
            inFlight.fetch_sub(1, std::memory_order_release);
            SetLastError(error);
        }
    } operation;
    CpuFrameKey result;
    if (!active.load(std::memory_order_acquire))
        return result;
    std::uintptr_t firstOwner{}, secondOwner{};
    std::uint64_t firstKey{}, secondKey{};
    const auto ownerAddress = config.moduleBase + 0x1EE5E48;
    if (!readSafe(ownerAddress, &firstOwner, sizeof(firstOwner)) || !firstOwner ||
        firstOwner > std::numeric_limits<std::uintptr_t>::max() - 0xD0 ||
        !readSafe(firstOwner + 0xC8, &firstKey, sizeof(firstKey)) ||
        !readSafe(ownerAddress, &secondOwner, sizeof(secondOwner)) || secondOwner != firstOwner ||
        !readSafe(secondOwner + 0xC8, &secondKey, sizeof(secondKey)) || firstKey != secondKey)
        return result;
    result.owner = firstOwner;
    result.engineFrame = static_cast<std::uint32_t>(firstKey);
    result.valid = true;
    return result;
}
}

// Every facade restores LastError before returning to assembly. No exception
// from an observer crosses into game code; exceptions from originals propagate.
extern "C" void gft_before(std::uint32_t index, std::uintptr_t rawRcx,
                            xess_vk::game_timing::PostSnapshot* snapshot) noexcept {
    using namespace xess_vk::game_timing;
    const DWORD error = GetLastError();
    inFlight.fetch_add(1, std::memory_order_acq_rel);
    *snapshot = {};
    snapshot->sampled = active.load(std::memory_order_acquire);
    if (snapshot->sampled) {
        snapshot->threadId = GetCurrentThreadId();
        if (index != 0) {
            snapshot->keyAddress = rawRcx;
            snapshot->valid = readSafe(rawRcx, &snapshot->engineFrame, sizeof(snapshot->engineFrame));
        }
    }
    inFlight.fetch_sub(1, std::memory_order_release);
    SetLastError(error);
}

extern "C" void gft_after(std::uint32_t index,
                           xess_vk::game_timing::PostSnapshot* snapshot) noexcept {
    using namespace xess_vk::game_timing;
    const DWORD error = GetLastError();
    inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (index < hookCount && snapshot->sampled && active.load(std::memory_order_acquire)) {
        const auto& site = sites[index];
        if (index == 0) {
            std::uintptr_t owner{};
            snapshot->valid = readSafe(config.moduleBase + 0x1EE5E48, &owner, sizeof(owner));
            if (snapshot->valid && owner && owner <= std::numeric_limits<std::uintptr_t>::max() - 0xD0) {
                snapshot->keyAddress = owner + 0xC8;
                std::uint64_t key{};
                snapshot->valid = readSafe(snapshot->keyAddress, &key, sizeof(key));
                snapshot->engineFrame = static_cast<std::uint32_t>(key);
            } else snapshot->valid = false;
        }
        publish({site.stage, site.boundary, site.keySource, snapshot->engineFrame,
                 snapshot->threadId, site.sourceRva, snapshot->keyAddress, snapshot->valid});
    }
    inFlight.fetch_sub(1, std::memory_order_release);
    SetLastError(error);
}

extern "C" void gft_sim_end_before(std::uintptr_t returnAddress, std::uint64_t callerRbx) noexcept {
    using namespace xess_vk::game_timing;
    const DWORD error = GetLastError();
    inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (active.load(std::memory_order_acquire)) {
        if (returnAddress == config.moduleBase + 0xC9906F) {
            publish({Stage::SimulationEnd, Boundary::MatchedHelperEntry, FrameKeySource::CallerRbxDword,
                     static_cast<std::uint32_t>(callerRbx), GetCurrentThreadId(), 0xC98FF0, 0, true});
        } else ignoredHelperCallers.fetch_add(1, std::memory_order_relaxed);
    }
    inFlight.fetch_sub(1, std::memory_order_release);
    SetLastError(error);
}

extern "C" void gft_enqueue_before(std::uintptr_t state, std::uintptr_t presentInfo) noexcept {
    using namespace xess_vk::game_timing;
    const DWORD error = GetLastError();
    inFlight.fetch_add(1, std::memory_order_acq_rel);
    enqueueEntrances.fetch_add(1, std::memory_order_relaxed);
    if (active.load(std::memory_order_acquire) && config.enqueueSink) {
        EnqueueEvent event{state, 0, presentInfo, GetCurrentThreadId(), false};
        if (state && state <= std::numeric_limits<std::uintptr_t>::max() - 0x158 &&
            readSafe(state + 0x150, &event.queue, sizeof(event.queue))) {
            event.queueValid = event.queue != 0 || readSafe(state + 0x8, &event.queue, sizeof(event.queue));
            event.queueValid = event.queueValid && event.queue != 0;
        }
        if (!event.queueValid) invalidKeys.fetch_add(1, std::memory_order_relaxed);
        emitted.fetch_add(1, std::memory_order_relaxed);
        if (!enqueueSafe(event)) callbackFailures.fetch_add(1, std::memory_order_relaxed);
    }
    inFlight.fetch_sub(1, std::memory_order_release);
    SetLastError(error);
}
