#include "graph_registration_bridge.hpp"
#include <windows.h>

#if !defined(__clang__) || !defined(_WIN64)
#error This bridge requires the tested Clang Win64 SEH compiler contract.
#endif

namespace xess_vk::registration_bridge {
namespace {
// Build with -fno-emulated-tls: native static PE TLS, no first-call allocation.
thread_local std::uint32_t nesting{};

struct Invocation {
    const Route& route;
    EntrySnapshot entry;
    BeforeResult before{};
    Unavailable unavailable{Unavailable::None};
    bool counted{};
    bool consumed{};
    bool holdsDepth{};
    std::uint64_t result{};
    DWORD outgoingError{};
};

void markUnavailable(Invocation& call, Unavailable reason) noexcept {
    call.unavailable = reason;
    if (!call.counted) {
        call.counted = true;
        call.route.control->unavailable.fetch_add(1, std::memory_order_relaxed);
    }
}

bool beforeCpp(Invocation& call) noexcept {
    try { return call.route.before(call.route.context, call.entry, call.before); }
    catch (...) { return false; }
}
bool closeCpp(Invocation& call, const CloseEvent& event) noexcept {
    try { return call.route.close(call.route.context, event); }
    catch (...) { return false; }
}

// These tiny SEH facades have POD locals and no C++ object requiring destruction.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
bool beforeSafe(Invocation& call) noexcept {
    __try { return beforeCpp(call); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool closeSafe(Invocation& call, const CloseEvent& event) noexcept {
    __try { return closeCpp(call, event); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#pragma clang diagnostic pop

void begin(Invocation& call) noexcept {
    auto& control = *call.route.control;
    control.calls.fetch_add(1, std::memory_order_relaxed);
    if (!control.sampling.load(std::memory_order_acquire)) {
        control.stopped.fetch_add(1, std::memory_order_relaxed);
        markUnavailable(call, Unavailable::Stopped);
    } else if (nesting == maximumNesting) {
        control.capacity.fetch_add(1, std::memory_order_relaxed);
        markUnavailable(call, Unavailable::Capacity);
    } else {
        ++nesting;
        call.holdsDepth = true;
        if (call.route.before == nullptr || call.route.close == nullptr) {
            markUnavailable(call, Unavailable::NoObserver);
        } else if (!beforeSafe(call)) {
            control.beforeFailures.fetch_add(1, std::memory_order_relaxed);
            markUnavailable(call, Unavailable::BeforeFailure);
        } else if (!call.before.published || !call.before.associationAvailable) {
            markUnavailable(call, Unavailable::ObserverDeclined);
        }
    }
    SetLastError(call.entry.lastError);
}

void finish(Invocation& call, Exit exit) noexcept {
    const DWORD savedError = GetLastError();
    if (call.before.published && !call.consumed) {
        // Consume before entering the observer, so observer reentry cannot use it.
        call.consumed = true;
        const CloseEvent event{call.entry, call.before.token, exit,
            exit == Exit::Normal ? call.result : 0, savedError,
            call.unavailable != Unavailable::None};
        auto& control = *call.route.control;
        (exit == Exit::Normal ? control.normalCloses : control.abnormalCloses)
            .fetch_add(1, std::memory_order_relaxed);
        if (!closeSafe(call, event)) {
            control.closeFailures.fetch_add(1, std::memory_order_relaxed);
            markUnavailable(call, Unavailable::CloseFailure);
        }
    }
    if (call.holdsDepth) {
        --nesting;
        call.holdsDepth = false;
    }
    SetLastError(savedError);
}

// This is a real native SEH termination scope. Dispatch/search or continued
// exceptions do not execute this handler: only unwinding out of this frame does.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
__attribute__((noinline)) void callOriginal(Invocation& call) {
    __try {
        call.result = call.route.original(reinterpret_cast<void*>(call.entry.rcx),
            static_cast<std::uint32_t>(call.entry.rdx),
            reinterpret_cast<const void*>(call.entry.r8));
        call.outgoingError = GetLastError();
    } __finally {
        if (AbnormalTermination()) finish(call, Exit::Abnormal);
    }
}
#pragma clang diagnostic pop
}
}

// Potentially throwing: the original's C++ exception must retain its identity.
extern "C" std::uint64_t grb_dispatch(
    const xess_vk::registration_bridge::EntrySnapshot* machine,
    const xess_vk::registration_bridge::Route* route) {
    const DWORD incomingError = GetLastError();
    const DWORD threadId = GetCurrentThreadId();
    using namespace xess_vk::registration_bridge;
    Invocation call{*route, *machine};
    call.entry.threadId = threadId;
    call.entry.lastError = incomingError;
    begin(call);
    callOriginal(call);
    SetLastError(call.outgoingError);
    finish(call, Exit::Normal);
    return call.result;
}

#ifdef GRAPH_REGISTRATION_BRIDGE_SELF_TEST
extern "C" std::uintptr_t grb_test_original_call_address() noexcept {
    return reinterpret_cast<std::uintptr_t>(&xess_vk::registration_bridge::callOriginal);
}
#endif
