// A separate Win64 entry/trampoline for the eight-binding observation path.
// Reuse the single audited instruction sequence rather than maintaining a copy.
// This file only defines code/data; it never chooses or patches a target.
#define fsr_observer_entry upscale_observer_entry
#define fsr_observer_entry_end upscale_observer_entry_end
#define fsr_original_trampoline upscale_original_trampoline
#define fsr_observe_before upscale_observe_before
#include "fsr_detour_entry.S"
