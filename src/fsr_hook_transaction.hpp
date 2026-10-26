#pragma once
#include <cstddef>
#include <cstdint>

namespace xess_vk::fsr_hook_transaction {
inline constexpr std::size_t sharedIndex = 2;
inline constexpr std::uint32_t fiMask = 1, upscaleMask = 2;
inline constexpr std::uint32_t continuousMask = 31;
inline constexpr std::size_t continuousSharedIndex = 5;
// Targets 0..4 legacy/upscale/gates/camera, 5..11 game timing, 12 render parameters.
inline constexpr std::size_t timedTargetCount = 13;
inline constexpr std::uint32_t timedContinuousMask = (1u << timedTargetCount) - 1;
inline constexpr std::uint32_t currentContinuousMask = timedContinuousMask & ~fiMask;
inline constexpr std::size_t timedContinuousSharedIndex = timedTargetCount;
// LocalFiOff adds only the two renderer comparisons; legacy masks stay exact.
inline constexpr std::size_t localFiFirstIndex = timedTargetCount;
inline constexpr std::size_t localFiTargetCount = timedTargetCount + 2;
inline constexpr std::uint32_t localFiOffMask = currentContinuousMask |
    (1u << localFiFirstIndex) | (1u << (localFiFirstIndex + 1));
inline constexpr std::size_t localFiOffSharedIndex = localFiTargetCount;
// Read-only SR adds Accumulate/RCAS callbacks and their exact-payload registrations.
inline constexpr std::size_t srFirstIndex=localFiTargetCount,srTargetCount=srFirstIndex+4;
inline constexpr std::size_t srNativeFirstIndex=srTargetCount,maximumTargetCount=srNativeFirstIndex+12;
static_assert(maximumTargetCount<32);
inline constexpr std::uint32_t srTargetsMask=((1u<<4)-1)<<srFirstIndex;
inline constexpr std::uint32_t srObservationMask=currentContinuousMask|srTargetsMask;
inline constexpr std::uint32_t localFiSrObservationMask=localFiOffMask|srTargetsMask;
inline constexpr std::size_t srSharedIndex=srTargetCount,srNativeSharedIndex=maximumTargetCount;
inline constexpr std::uint32_t srNativeTargetsMask=((1u<<12)-1)<<srNativeFirstIndex;
inline constexpr std::uint32_t srNativeObservationMask=srObservationMask|srNativeTargetsMask;
inline constexpr std::uint32_t localFiSrNativeObservationMask=localFiSrObservationMask|srNativeTargetsMask;
inline constexpr std::size_t sharedFor(std::uint32_t mask) noexcept {
    return (mask==srNativeObservationMask||mask==localFiSrNativeObservationMask)?srNativeSharedIndex:
        (mask==srObservationMask||mask==localFiSrObservationMask)?srSharedIndex:
        mask == localFiOffMask ? localFiOffSharedIndex :
        (mask == timedContinuousMask || mask == currentContinuousMask) ? timedContinuousSharedIndex :
        mask == continuousMask ? continuousSharedIndex : sharedIndex;
}
enum class Stage {
    ValidateShared, ValidateTarget, PrepareShared, CreateTarget, PublishTarget,
    ConfigureTarget, CheckBeforeEnable, EnableTarget, CheckAfterEnable, ActivateTarget, Complete
};
enum class Failure { None, InvalidContract, Rejected, Stopped, Exception };
const char* stageName(Stage) noexcept;
const char* failureName(Failure) noexcept;
struct Operations {
    void* user{};
    // Shared stages use index 2 for the legacy pair, or index 5 for continuous
    // input. Targets: FI=0, upscale=1, FI gate=2, compose gate=3, camera=4.
    // Step may throw. All original forwarding storage must remain process-lived,
    // including on failure. The runner never requests rollback/removal/retry.
    bool (*step)(void*, Stage, std::size_t){};
    bool (*stopped)(void*) noexcept{};
    // Must be idempotent, sticky, nonthrowing, and stop every requested helper.
    // External lifecycle events must also stop helpers immediately themselves.
    void (*stopAll)(void*) noexcept{};
};
struct Result {
    bool completed{};
    Stage stage = Stage::ValidateShared;
    Failure failure = Failure::None;
    std::size_t hookIndex = sharedIndex;
    std::uint32_t requestedMask{};
};
// Runs only the exact legacy/current/LocalFiOff/SR masks, optionally with all six auxiliary SR jobs.
// LocalFiOff adds targets 13/14 and shared index 15. All preflight checks and disabled
// configurations precede every enable. All postchecks precede every activation.
// Stop is checked before/after each step. Activation is sequential, not atomic;
// an external stop still requires each helper's sticky gate/lifetime contract.
// No allocations, platform APIs, reads or patches are performed by this runner.
Result run(std::uint32_t requestedMask, const Operations&) noexcept;
} // namespace xess_vk::fsr_hook_transaction
