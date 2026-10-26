#pragma once
#ifndef XESS_RELEASE_MINIMAL
#error "XESS_RELEASE_MINIMAL must be supplied by the CMake build-mode target"
#endif
#if XESS_RELEASE_MINIMAL != 0 && XESS_RELEASE_MINIMAL != 1
#error "XESS_RELEASE_MINIMAL must be 0 or 1"
#endif
inline constexpr bool releaseMinimal = XESS_RELEASE_MINIMAL != 0;
