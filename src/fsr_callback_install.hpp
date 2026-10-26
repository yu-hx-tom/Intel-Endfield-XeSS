#pragma once
#include "fsr_callback_observer.hpp"
#include "upscale_callback_observer.hpp"
#include "game_frame_timing.hpp"
#include "continuous_registration.hpp"
#include "sr_observation.hpp"
#include <cstdint>

namespace xess_vk::fsr_callback_install {
using Sink = fsr_callback_observer::Sink;

// Exact current continuous flags only; excludes FI, raw capture and side hooks.
bool currentContinuousRequested() noexcept;
// Read-only pinned module/file/PE/site preflight before changing device arguments.
// No hook, forwarder publication, module load or late retry is performed.
bool currentContinuousProfileReady() noexcept;
// True only after the complete current-profile transaction; any stop revokes it.
bool currentContinuousReady(std::uint64_t device) noexcept;
// A late-initialized data slot, validated against the already pinned PE profile.
// This only returns its address; the natural present boundary owns one CAS.
std::uintptr_t currentContinuousPresentSlot(std::uint64_t device, std::uintptr_t moduleBase) noexcept;

// Call once for EVERY successful vkCreateDevice boundary, before returning the
// device to the application. Only the first boundary can attempt installation.
// XESS_FSR_UPSCALE_OBSERVER=1 alone admits only the current fixed read-only
// upscaler candidate profile with an inventory metadata consumer. It refuses
// continuous callbacks, suppression, bridge, capture and presentation requests.
// Current admission permits targets 1..12, plus 13/14 only for LocalFiOff and
// 15..18 only for the file-gated read-only SR observation profile.
// The retired FI callback remains refused.
// Both targets share one preflight/initialize transaction and first boundary.
// currentContinuousAdmission is supplied only after device preflight and all
// device-owned consumers succeed. False leaves menu/other hooks unactivated.
// No present-time or other late retries.
// This reduces exposure to concurrent execution; it does not establish that all
// game threads are quiescent, nor that MinHook suspended every other thread.
void installAtDeviceCreation(std::uint64_t device, Sink sink,
    upscale_callback_observer::SourceMetadataSink upscaleSource = nullptr,
                            upscale_callback_observer::InvocationSink invocation = nullptr,
                            game_timing::Sink timingSink = nullptr, void* timingUser = nullptr,
                            continuous_registration::FrameReader frameReader = nullptr,
                            game_timing::EnqueueSink enqueueSink = nullptr,
                            bool currentContinuousAdmission = false,
                            sr_observation::ResolveImages srImages = nullptr) noexcept;

// Attach the native-resolution control once IL2CPP is loaded; call outside
// command recording. Never retries a failed attachment.
bool ensureNativeResolution(std::uint64_t device) noexcept;

// Stop observation on destruction of the selected device. Any created MinHook
// trampoline and any installed forwarder remain alive for the whole process.
void onDeviceDestroyed(std::uint64_t device) noexcept;

// Read-only, point-in-time integrity report. Never reattaches or removes a hook.
void summary() noexcept;
}
