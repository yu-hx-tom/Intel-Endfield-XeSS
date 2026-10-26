#include "fsr_hook_transaction.hpp"

namespace xess_vk::fsr_hook_transaction {
const char* stageName(Stage stage) noexcept {
    switch (stage) {
    case Stage::ValidateShared: return "validate_shared";
    case Stage::ValidateTarget: return "validate_target";
    case Stage::PrepareShared: return "prepare_shared";
    case Stage::CreateTarget: return "create_target";
    case Stage::PublishTarget: return "publish_target";
    case Stage::ConfigureTarget: return "configure_target";
    case Stage::CheckBeforeEnable: return "check_before_enable";
    case Stage::EnableTarget: return "enable_target";
    case Stage::CheckAfterEnable: return "check_after_enable";
    case Stage::ActivateTarget: return "activate_target";
    case Stage::Complete: return "complete";
    }
    return "unknown";
}
const char* failureName(Failure failure) noexcept {
    switch (failure) {
    case Failure::None: return "none";
    case Failure::InvalidContract: return "invalid_contract";
    case Failure::Rejected: return "rejected";
    case Failure::Stopped: return "stopped";
    case Failure::Exception: return "exception";
    }
    return "unknown";
}
Result run(std::uint32_t mask, const Operations& supplied) noexcept {
    // Freeze callback addresses/user before allowing any user callback to run.
    const Operations operations = supplied;
    Result result; result.requestedMask = mask;
    const auto fail = [&](Failure failure) noexcept {
        result.failure = failure;
        if (operations.stopAll) operations.stopAll(operations.user);
        return false;
    };
    if ((mask != fiMask && mask != upscaleMask && mask != (fiMask | upscaleMask) && mask != continuousMask && mask != timedContinuousMask && mask != currentContinuousMask && mask != localFiOffMask && mask != srObservationMask && mask != localFiSrObservationMask && mask != srNativeObservationMask && mask != localFiSrNativeObservationMask) ||
        !operations.step || !operations.stopped || !operations.stopAll) {
        fail(Failure::InvalidContract); return result;
    }
    const auto step = [&](Stage stage, std::size_t index) noexcept {
        result.stage = stage; result.hookIndex = index;
        if (operations.stopped(operations.user)) return fail(Failure::Stopped);
        try {
            if (!operations.step(operations.user, stage, index)) return fail(Failure::Rejected);
        } catch (...) { return fail(Failure::Exception); }
        if (operations.stopped(operations.user)) return fail(Failure::Stopped);
        return true;
    };
    const auto targets = [&](Stage stage) noexcept {
        for (std::size_t index = 0; index < maximumTargetCount; ++index)
            if ((mask & (1u << index)) && !step(stage, index)) return false;
        return true;
    };
    const auto shared = sharedFor(mask);
    if (!step(Stage::ValidateShared, shared) || !targets(Stage::ValidateTarget) ||
        !step(Stage::PrepareShared, shared) || !targets(Stage::CreateTarget) ||
        !targets(Stage::PublishTarget) || !targets(Stage::ConfigureTarget) ||
        !targets(Stage::CheckBeforeEnable) || !targets(Stage::EnableTarget) ||
        !targets(Stage::CheckAfterEnable) || !targets(Stage::ActivateTarget)) return result;
    result.stage = Stage::Complete; result.hookIndex = shared; result.completed = true;
    return result;
}
} // namespace xess_vk::fsr_hook_transaction
