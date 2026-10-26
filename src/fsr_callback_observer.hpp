#pragma once
#include <cstdint>
namespace xess_vk::fsr_probe { struct Result; }
namespace xess_vk::fsr_callback_observer {
using Sink = void (*)(const char*);
using ImageMetadataSink = void (*)(std::uint64_t device, std::uint64_t matchedCall, const fsr_probe::Result&, bool emitDetail);
// Optional metadata-only callback; configured before the first device hook.
// Called for valid bounded diagnostic samples and every 120 matched callbacks
// thereafter. emitDetail=false refreshes metadata without adding detail logs.
// Per-probe byte/read limits still apply; this is not per-frame GPU input capture.
// Must not retain Result, invoke
// game functions, or submit GPU work. Result.pass is part of the association key:
// matchedCall is independent for each pass and is never a frame identifier.
// Exceptions are contained by the observer.
void setImageMetadataSink(ImageMetadataSink) noexcept;
// Internal integration contract: the installer must validate the exact module,
// map and code first. Does not install hooks, find modules, or change game memory.
// Publish once, before enabling the detour. Sampling stays disabled until all
// post-install checks pass. Sink must support concurrent callback threads.
void configureVerifiedModule(std::uintptr_t moduleBase, std::uint64_t device, Sink) noexcept;
bool enableSampling() noexcept;
// Sticky for process lifetime: later configure/enable calls cannot undo a stop.
void stopSampling() noexcept;
void summary() noexcept;
#ifdef XESS_FSR_CALLBACK_TEST
void resetForTest(std::uintptr_t moduleBase, std::uint64_t device, Sink, bool activate = true) noexcept;
// Zero restores GetTickCount64. Nonzero provides a controlled gap clock only;
// JSON timestamps still report the actual process clock.
void setClockForTest(std::uint64_t now) noexcept;
#endif
}
// Called only by the tested assembler entry (or the self-owned test fixture).
// It performs bounded reads and preserves LastError; it never calls the original.
extern "C" void fsr_observe_before(void* payload, void* context) noexcept;
