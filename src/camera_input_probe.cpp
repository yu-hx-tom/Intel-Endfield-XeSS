#include "camera_input_probe.hpp"

#include <limits>

namespace xess_vk::camera_probe {
namespace {
constexpr Address maxAddress = std::numeric_limits<Address>::max();
bool validRange(Address start, std::size_t bytes) noexcept {
    // Require a representable exclusive end; no canonical-address assumption.
    return start && bytes && bytes <= maxAddress - start;
}
std::uint32_t u32(const unsigned char* p) noexcept {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
        (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
template<std::size_t N>
bool finite(const std::array<unsigned char, N>& bytes, Address offset, Result& result) noexcept {
    static_assert(N % 4 == 0);
    for (std::size_t i = 0; i < N; i += 4) {
        // Exponent all ones means either infinity or NaN. Preserve finite bits,
        // including signed zero/subnormals, without touching the floating env.
        if ((u32(bytes.data() + i) & 0x7F800000u) == 0x7F800000u) {
            result.nonFiniteOffsetAvailable = true;
            result.firstNonFiniteByteOffset = offset + i;
            return false;
        }
    }
    return true;
}
template<std::size_t N>
void publish(const std::array<unsigned char, N>& source, std::array<std::uint32_t, N / 4>& destination) noexcept {
    for (std::size_t i = 0; i < destination.size(); ++i) destination[i] = u32(source.data() + 4 * i);
}
bool read(Reader reader, const Limits& limits, Address address, void* destination,
          std::size_t size, Phase phase, Result& result) noexcept {
    result.phase = phase;
    const auto remaining = limits.maxBytes - result.bytesRequested;
    result.bytesRequested += size;
    ++result.readCalls;
    try {
        const auto copied = reader.read(reader.user, address, destination, size, remaining);
        if (copied > size) result.status = Status::CallbackContractViolation;
        else {
            result.bytesCopied += copied;
            if (copied == size) return true;
            result.status = copied ? Status::PartialRead : Status::Unreadable;
        }
    } catch (...) { result.status = Status::CallbackException; }
    result.failedReadAddress = address;
    result.failedReadBytes = size;
    return false;
}
} // namespace

Result readCamera(const Request& supplied, Reader reader) noexcept {
    const Request request = supplied; // Callbacks cannot change this probe's plan.
    Result result;
    result.camera = request.camera;
    result.clipToPrevRequested = request.captureClipToPrev;
    if (request.layout != InputLayout::UnityPlayerBee7be52CameraRaw &&
        request.layout != InputLayout::UnityPlayer9aa9397aCameraRaw) {
        result.status = Status::UnknownInputLayout; return result;
    }
    if (request.layout == InputLayout::UnityPlayer9aa9397aCameraRaw && request.captureClipToPrev) {
        result.status = Status::UnverifiedClipToPrevLayout; return result;
    }
    if (!request.limits.maxBytes || request.limits.maxBytes > hardMaxBytes ||
        !request.limits.maxReadCalls || request.limits.maxReadCalls > hardMaxReadCalls) {
        result.status = Status::InvalidLimits; return result;
    }
    if (!reader.read) { result.status = Status::MissingReader; return result; }
    if (!request.camera) { result.status = Status::InvalidAddress; return result; }
    if (!validRange(request.camera, prefixBytes) || (request.captureClipToPrev &&
        (clipToPrevSourceOffset > maxAddress - request.camera ||
         !validRange(request.camera + clipToPrevSourceOffset, clipToPrevBytes)))) {
        result.status = Status::AddressOverflow; return result;
    }
    const std::size_t plannedBytes = 2 * prefixBytes + (request.captureClipToPrev ? 2 * clipToPrevBytes : 0);
    const std::size_t plannedCalls = request.captureClipToPrev ? 4 : 2;
    if (request.limits.maxBytes < plannedBytes) { result.status = Status::ByteBudgetExceeded; return result; }
    if (request.limits.maxReadCalls < plannedCalls) { result.status = Status::ReadCallBudgetExceeded; return result; }

    std::array<unsigned char, prefixBytes> prefixBefore{}, prefixAfter{};
    std::array<unsigned char, clipToPrevBytes> temporalBefore{}, temporalAfter{};
    if (!read(reader, request.limits, request.camera, prefixBefore.data(), prefixBefore.size(), Phase::PrefixBefore, result))
        return result;
    if (request.captureClipToPrev) {
        const auto address = request.camera + clipToPrevSourceOffset;
        if (!read(reader, request.limits, address, temporalBefore.data(), temporalBefore.size(), Phase::ClipToPrevBefore, result) ||
            !read(reader, request.limits, address, temporalAfter.data(), temporalAfter.size(), Phase::ClipToPrevAfter, result))
            return result;
    }
    if (!read(reader, request.limits, request.camera, prefixAfter.data(), prefixAfter.size(), Phase::PrefixAfter, result))
        return result;
    result.phase = Phase::Validate;
    result.prefixReadingsEqual = prefixBefore == prefixAfter;
    result.clipToPrevReadingsEqual = request.captureClipToPrev && temporalBefore == temporalAfter;
    if (!result.prefixReadingsEqual) { result.status = Status::PrefixChanged; return result; }
    if (request.captureClipToPrev && !result.clipToPrevReadingsEqual) { result.status = Status::ClipToPrevChanged; return result; }
    result.requestedReadingsEqual = true;
    if (!finite(prefixBefore, 0, result)) { result.status = Status::NonFinitePrefix; return result; }
    if (request.captureClipToPrev && !finite(temporalBefore, clipToPrevSourceOffset, result)) {
        result.status = Status::NonFiniteClipToPrev; return result;
    }
    result.allRetainedFloatCandidatesFinite = true;
    publish(prefixBefore, result.snapshot.prefixFloatBits);
    if (request.captureClipToPrev) {
        publish(temporalBefore, result.snapshot.clipToPrevFloatBits);
        result.snapshot.clipToPrevCaptured = true;
    }
    result.snapshot.captured = true;
    result.status = Status::SnapshotReadUnverified;
    result.phase = Phase::Complete;
    return result;
}

const char* statusName(Status status) noexcept {
    switch (status) {
#define CAMERA_STATUS(value) case Status::value: return #value
    CAMERA_STATUS(NotAttempted); CAMERA_STATUS(SnapshotReadUnverified); CAMERA_STATUS(UnknownInputLayout);
    CAMERA_STATUS(InvalidLimits); CAMERA_STATUS(MissingReader); CAMERA_STATUS(InvalidAddress); CAMERA_STATUS(AddressOverflow);
    CAMERA_STATUS(ByteBudgetExceeded); CAMERA_STATUS(ReadCallBudgetExceeded); CAMERA_STATUS(Unreadable);
    CAMERA_STATUS(PartialRead); CAMERA_STATUS(CallbackException); CAMERA_STATUS(CallbackContractViolation);
    CAMERA_STATUS(PrefixChanged); CAMERA_STATUS(ClipToPrevChanged); CAMERA_STATUS(NonFinitePrefix); CAMERA_STATUS(NonFiniteClipToPrev);
    CAMERA_STATUS(UnverifiedClipToPrevLayout);
#undef CAMERA_STATUS
    }
    return "UnknownStatus";
}
const char* phaseName(Phase phase) noexcept {
    switch (phase) {
#define CAMERA_PHASE(value) case Phase::value: return #value
    CAMERA_PHASE(Preflight); CAMERA_PHASE(PrefixBefore); CAMERA_PHASE(ClipToPrevBefore); CAMERA_PHASE(ClipToPrevAfter);
    CAMERA_PHASE(PrefixAfter); CAMERA_PHASE(Validate); CAMERA_PHASE(Complete);
#undef CAMERA_PHASE
    }
    return "UnknownPhase";
}
} // namespace xess_vk::camera_probe
