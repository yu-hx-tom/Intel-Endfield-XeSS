#pragma once
// Standalone research fixtures retain their original behavior. The product DLL
// explicitly defines this to 0; a separate diagnostic build may opt back in.
#ifndef XESS_RESEARCH_DIAGNOSTICS
#define XESS_RESEARCH_DIAGNOSTICS 1
#endif
inline constexpr bool researchDiagnostics = XESS_RESEARCH_DIAGNOSTICS != 0;
