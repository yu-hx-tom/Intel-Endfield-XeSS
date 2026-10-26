#include "fsr_frame_constants.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace xess_vk::fsr_constants {
namespace {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(std::int32_t) == 4);

bool equalSha256(std::string_view actual, std::string_view expected) noexcept {
    if (actual.size() != expected.size()) return false;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const char ch = actual[i];
        const char lower = ch >= 'A' && ch <= 'F' ? static_cast<char>(ch - 'A' + 'a') : ch;
        if (lower != expected[i]) return false;
    }
    return true;
}

std::uint32_t word(const unsigned char* bytes, std::size_t offset) noexcept {
    return std::uint32_t{bytes[offset]} | (std::uint32_t{bytes[offset + 1]} << 8) |
           (std::uint32_t{bytes[offset + 2]} << 16) | (std::uint32_t{bytes[offset + 3]} << 24);
}
float scalar(const unsigned char* bytes, std::size_t offset) noexcept {
    return std::bit_cast<float>(word(bytes, offset));
}
template<std::size_t Count>
std::array<float, Count> floats(const unsigned char* bytes, std::size_t offset) noexcept {
    std::array<float, Count> values{};
    for (std::size_t i = 0; i < Count; ++i) values[i] = scalar(bytes, offset + i * 4);
    return values;
}
std::array<std::int32_t, 2> extent(const unsigned char* bytes, std::size_t offset) noexcept {
    return {std::bit_cast<std::int32_t>(word(bytes, offset)),
            std::bit_cast<std::int32_t>(word(bytes, offset + 4))};
}
} // namespace

const char* statusName(Status status) noexcept {
    switch (status) {
    case Status::NotAttempted: return "NotAttempted";
    case Status::DecodedUnverified: return "DecodedUnverified";
    case Status::UnknownLayout: return "UnknownLayout";
    case Status::VersionMismatch: return "VersionMismatch";
    case Status::SizeMismatch: return "SizeMismatch";
    case Status::NullInput: return "NullInput";
    case Status::NonFiniteCandidate: return "NonFiniteCandidate";
    }
    return "UnknownStatus";
}

Result decodeFi(const Request& request) noexcept {
    Result result;
    result.layout = request.layout;
    std::size_t expectedBytes = 0;
    bool versionMatches = false;
    switch (request.layout) {
    case FiLayout::AmdSdk114CbFi176:
        expectedBytes = officialFiBytes;
        versionMatches = request.sourceVersion == requiredAmdSdkVersion;
        break;
    case FiLayout::UnityPlayerBee7be52CbFi224:
        expectedBytes = unityFiBytes;
        versionMatches = equalSha256(request.sourceVersion, requiredUnitySha256);
        break;
    default:
        result.status = Status::UnknownLayout;
        return result;
    }
    if (!versionMatches) { result.status = Status::VersionMismatch; return result; }
    if (request.byteSize != expectedBytes) { result.status = Status::SizeMismatch; return result; }
    if (!request.bytes) { result.status = Status::NullInput; return result; }

    // Copy first, then decode only the retained copy. The caller owns the stable
    // input requirement; this memcpy does not acquire a live-memory lifetime.
    std::copy_n(request.bytes, officialFiBytes, result.rawPrefix.begin());
    result.unknownTailSize = expectedBytes - officialFiBytes;
    if (result.unknownTailSize)
        std::copy_n(request.bytes + officialFiBytes, result.unknownTailSize, result.unknownTail.begin());
    result.rawRetained = true;
    const auto* bytes = result.rawPrefix.data();

    // Check IEEE754 exponent bits before materializing any float. Opaque fields
    // and tail bytes are not float-validated; NaN-like bits there remain opaque.
    constexpr std::array<std::size_t, 16> floatOffsets{
        0x10, 0x14, 0x18, 0x1C, 0x30, 0x34, 0x38, 0x3C,
        0x40, 0x50, 0x54, 0x98, 0xA0, 0xA4, 0xA8, 0xAC};
    for (const auto offset : floatOffsets) {
        if ((word(bytes, offset) & 0x7F800000u) == 0x7F800000u) {
            result.status = Status::NonFiniteCandidate;
            result.firstNonFiniteByteOffset = offset;
            return result;
        }
    }
    auto& fields = result.fields;
    fields.renderSize = extent(bytes, 0x00);
    fields.displaySize = extent(bytes, 0x08);
    fields.displaySizeRcp = floats<2>(bytes, 0x10);
    fields.cameraNear = scalar(bytes, 0x18);
    fields.cameraFar = scalar(bytes, 0x1C);
    fields.upscalerTargetSize = extent(bytes, 0x20);
    fields.resetRaw = word(bytes, 0x2C);
    fields.deviceToViewDepth = floats<4>(bytes, 0x30);
    fields.deltaTime = scalar(bytes, 0x40);
    fields.opticalFlowScale = floats<2>(bytes, 0x50);
    fields.tanHalfFovHorizontal = scalar(bytes, 0x98);
    fields.jitter = floats<2>(bytes, 0xA0);
    fields.motionVectorScale = floats<2>(bytes, 0xA8);
    result.status = Status::DecodedUnverified;
    return result;
}
} // namespace xess_vk::fsr_constants
