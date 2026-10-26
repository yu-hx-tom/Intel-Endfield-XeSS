#pragma once

// This bridge has no knowledge of the target function's return type. It restores
// the caller's volatile Win64 register state and tail-jumps to the trampoline.
// Only the normal Win64 ABI is supported: not vectorcall or AVX upper halves.
extern "C" {
void fsr_observer_entry();
extern void* fsr_original_trampoline; // Set once, before publishing/enabling hook.
// The embedding module must contain failures and restore the incoming LastError.
void fsr_observe_before(void* payload, void* graphContext) noexcept;
}
