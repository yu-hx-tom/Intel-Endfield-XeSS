#pragma once
#include "../third_party/streamline_headers/sl_core_api.h"

namespace xess_vk::streamline_observer {
using Sink = void (*)(const char* jsonLine);
enum class InstallStatus { Disabled, NotReady, Installed, Refused };

// Call outside loader lock and outside the sink's mutex. Never loads a module.
// Explicit opt-in, exact executable path and a known UnityPlayer hash are required.
// Installed wrappers live until process exit; observer, Unity and Streamline
// modules are pinned before any slot changes so code/data cannot disappear.
InstallStatus tryInstall(Sink sink) noexcept;
void flushSummary() noexcept;

#ifdef XESS_SL_OBSERVER_TEST
namespace testing {
struct Functions {
    PFun_slSetTagForFrame* tags{};
    PFun_slSetConstants* constants{};
    PFun_slGetNewFrameToken* token{};
};
// No module lookup, slot patch, or game access. Only available in the test build.
Functions reset(Sink sink, Functions originals) noexcept;
// Points summary checks at caller-owned test cells; never writes those cells.
void observeSlots(void** tags, void** constants, void** token, unsigned mask) noexcept;
}
#endif
}
