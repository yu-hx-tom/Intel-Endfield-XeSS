#pragma once

// Independent from the FI bridge; publish its own trampoline before enabling.
// Same ordinary Win64 ABI scope as fsr_detour_entry.hpp, not vectorcall/AVX.
extern "C" {
void upscale_observer_entry();
extern void* upscale_original_trampoline;
void upscale_observe_before(void* payload, void* graphContext) noexcept;
}
