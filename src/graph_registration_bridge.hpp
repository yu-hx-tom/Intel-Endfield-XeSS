#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Offline, ordinary Win64, three integer arguments and integer RAX return only.
// No installation, target discovery, memory reader, or graph association engine.
namespace xess_vk::registration_bridge {

using Original = std::uint64_t (*)(void*, std::uint32_t, const void*);
inline constexpr std::uint32_t maximumNesting = 32;

struct EntrySnapshot {
    std::uintptr_t rsp{};
    std::uintptr_t rbp{};
    std::uintptr_t returnAddress{};
    std::uint64_t rcx{};
    std::uint64_t rdx{};
    std::uint64_t r8{};
    std::uint32_t threadId{};
    std::uint32_t lastError{};
};
static_assert(sizeof(EntrySnapshot) == 56);
static_assert(offsetof(EntrySnapshot, rcx) == 24);
static_assert(offsetof(EntrySnapshot, threadId) == 48);

// All token bytes belong to this invocation. Addresses do not establish identity.
struct Token { std::uint64_t owner{}, scope{}; };
struct BeforeResult {
    Token token{};
    bool published{};
    bool associationAvailable{};
};
enum class Exit : std::uint8_t { Normal, Abnormal };
enum class Unavailable : std::uint8_t {
    None, Stopped, Capacity, NoObserver, ObserverDeclined, BeforeFailure, CloseFailure
};
struct CloseEvent {
    EntrySnapshot entry{};
    Token token{};
    Exit exit{};
    std::uint64_t copiedPayload{}; // Meaningful only for Normal.
    std::uint32_t lastError{};
    bool associationUnavailable{};
};

// The invocation facades are noexcept and contain callback C++ and SEH failures.
// Callbacks can instead report failure with false. A throwing noexcept callback
// terminates inside the callback itself and cannot be rescued by any caller.
// before must set published last, after fully writing its owned token. If it
// later fails, that published token still receives one close invocation.
using Before = bool (*)(void*, const EntrySnapshot&, BeforeResult&);
using Close = bool (*)(void*, const CloseEvent&);

struct Control {
    std::atomic<bool> sampling{true};
    std::atomic<std::uint64_t> calls{};
    std::atomic<std::uint64_t> unavailable{};
    std::atomic<std::uint64_t> stopped{};
    std::atomic<std::uint64_t> capacity{};
    std::atomic<std::uint64_t> beforeFailures{};
    std::atomic<std::uint64_t> closeFailures{};
    std::atomic<std::uint64_t> normalCloses{};
    std::atomic<std::uint64_t> abnormalCloses{};
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

// Define each extern const Route once before publishing an entry. Its original,
// callbacks, context and control must remain live through all in-flight calls.
// No shared mutable original pointer and no lock crosses Original.
struct Route {
    const Original original;
    const Before before;
    const Close close;
    void* const context;
    Control* const control;
};
}

extern "C" {
extern const xess_vk::registration_bridge::Route grb_fi_prepare_route;
extern const xess_vk::registration_bridge::Route grb_fi_copy_route;
extern const xess_vk::registration_bridge::Route grb_upscale_prepare_route;
std::uint64_t grb_fi_prepare_entry(void*, std::uint32_t, const void*);
std::uint64_t grb_fi_copy_entry(void*, std::uint32_t, const void*);
std::uint64_t grb_upscale_prepare_entry(void*, std::uint32_t, const void*);
}
