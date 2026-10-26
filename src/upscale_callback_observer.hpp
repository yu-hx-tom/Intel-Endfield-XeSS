#pragma once
#include "upscale_input_probe.hpp"
#include "cpu_wall_time.hpp"
#include <cstdint>

namespace xess_vk::upscale_callback_observer {
using Sink = void (*)(const char*);
// Independent source metadata only. callbackCall is an entry-call counter, never
// a frame ID or an association with the FI observer. The consumer may perform
// same-device inventory matching; it must not select shared FI Motion/Depth
// roles, retain Result, invoke game functions, or submit GPU work. Both sinks
// must support concurrent calls. Exceptions are contained by this module.
using SourceMetadataSink = void (*)(std::uint64_t device, std::uint64_t callbackCall,
                                   const upscale_probe::Result&, bool emitDetail);
// Production consumer runs on every admitted invocation, including rejection
// so a failed read cannot leave an old source attached to a new invocation.
// Arguments are live only for this callback; copy scalar/byte facts immediately.
using InvocationSink = void (*)(std::uint64_t device, std::uint64_t callbackCall,
                               std::uintptr_t payload, std::uintptr_t graphContext,
                               const upscale_probe::Result&, bool emitDetail);
inline constexpr std::uint64_t detailLimit = 32, refreshInterval = 120;
inline constexpr upscale_probe::Limits defaultLimits{8192, 256 * 1024, 128};

// Installer contract: verify the pinned module identity, exact thunk/body and
// lifetime before configuring. This module neither verifies nor installs a hook.
// Publication is one-shot and immutable. Limits may only lower the defaults.
// A non-null InvocationSink is supplied only by the admitted current continuous
// transaction. It receives every result, including rejection, before diagnostics.
// Pure observation leaves it null and never reads constants or submits work.
// Successful configuration stays disabled until enableSampling succeeds after
// the installer's postchecks. Null module/device/log sink or invalid limits fail.
bool configureVerifiedModule(std::uintptr_t moduleBase, std::uint64_t device, Sink,
                             SourceMetadataSink = nullptr,
                             upscale_probe::Limits = defaultLimits,
                             InvocationSink = nullptr) noexcept;
bool enableSampling() noexcept;
// Sticky: no later configure/enable can restart. Stops admission and checks the
// gate before each RPM read and delivery. Does not wait for a read or sink already
// entered; consumers still need their own device-generation/lifetime validation.
void stopSampling() noexcept;
// Explicit, finite-field summary. Caller owns its scheduling/frequency. Also
// available after stop; counters are independently sampled, not an atomic tuple.
void summary() noexcept;
// Cumulative admitted observe work, including RPM/consumers, never the original game function.
CpuWallTime observeCpuTime() noexcept;

#ifdef XESS_UPSCALE_CALLBACK_TEST
// Test-only reset requires every callback/configurer/summary thread to be joined.
void resetForTest() noexcept;
// Test-only deterministic scheduling point, after parsing and before delivery.
// Set only while callbacks are quiescent. Null removes it; it may throw.
void setAfterProbeForTest(void (*)()) noexcept;
#endif
} // namespace xess_vk::upscale_callback_observer

// Future independent assembly entry (or self-owned CPU fixture) calls this with
// RCX=payload, RDX=graphContext. Preserves LastError and never calls the original.
extern "C" void upscale_observe_before(void* payload, void* graphContext) noexcept;
