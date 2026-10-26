#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace xess_vk::fsr_constants {
inline constexpr std::size_t officialFiBytes = 176;
inline constexpr std::size_t unityFiBytes = 224;
inline constexpr std::size_t unityUnknownTailBytes = unityFiBytes - officialFiBytes;
inline constexpr std::size_t noInvalidOffset = std::numeric_limits<std::size_t>::max();
inline constexpr std::string_view requiredAmdSdkVersion = "v1.1.4";
inline constexpr std::string_view requiredAmdSdkCommit = "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55";
inline constexpr std::string_view requiredUnitySha256 =
    "bee7be52370adddd67ba61e4937ca51b7f272656841d187e95e505496da798d1";

// These are cbFI layouts, not cbFSR3Upscaler or public dispatch-description ABI.
// Layout and sourceVersion are caller declarations; bytes cannot prove identity.
enum class FiLayout { Unknown, AmdSdk114CbFi176, UnityPlayerBee7be52CbFi224 };
enum class Status {
    NotAttempted, DecodedUnverified, UnknownLayout, VersionMismatch,
    SizeMismatch, NullInput, NonFiniteCandidate
};
const char* statusName(Status) noexcept;

struct Request {
    FiLayout layout = FiLayout::Unknown;
    // SDK layout: "v1.1.4". Game layout: complete pinned UnityPlayer SHA-256.
    // Game SHA comparison accepts upper/lower case, without trimming or prefixes.
    std::string_view sourceVersion;
    // Already copied, stable, caller-owned CPU bytes. No live reader is used.
    const unsigned char* bytes = nullptr;
    std::size_t byteSize = 0;
};

// Only fields independently supported by the saved game builder and the fixed
// public cbFI prefix are interpreted. Names remain static semantic candidates
// for the game. Finite values do not establish validity for any GPU/API consumer.
struct FiFieldCandidates {
    std::array<std::int32_t, 2> renderSize{};          // +00
    std::array<std::int32_t, 2> displaySize{};         // +08
    std::array<float, 2> displaySizeRcp{};             // +10
    float cameraNear = 0, cameraFar = 0;              // +18/+1C
    std::array<std::int32_t, 2> upscalerTargetSize{};  // +20
    std::uint32_t resetRaw = 0;                       // +2C; not a frame ID
    std::array<float, 4> deviceToViewDepth{};          // +30; not a matrix
    float deltaTime = 0;                             // +40; no unit conversion
    std::array<float, 2> opticalFlowScale{};           // +50
    float tanHalfFovHorizontal = 0;                  // +98; not a FOV angle
    std::array<float, 2> jitter{};                    // +A0
    std::array<float, 2> motionVectorScale{};          // +A8
};

struct Result {
    Status status = Status::NotAttempted;
    FiLayout layout = FiLayout::Unknown;
    FiFieldCandidates fields;
    // Every prefix byte is retained, including uninterpreted gaps/padding.
    std::array<unsigned char, officialFiBytes> rawPrefix{};
    std::array<unsigned char, unityUnknownTailBytes> unknownTail{};
    std::size_t unknownTailSize = 0;
    bool rawRetained = false;
    std::size_t firstNonFiniteByteOffset = noInvalidOffset;
};

// Exact known version and size are mandatory. No layout autodetection, frame
// association, clamping, Halton/range assumption, rescaling, or matrix synthesis.
// A complete non-finite record retains raw bytes for diagnosis but publishes no
// typed fields. Earlier failures publish neither raw bytes nor typed fields.
// Success proves decoding only, not snapshot atomicity, source/shader identity,
// same-frame association, depth/MV/jitter conventions, or XeFG constant readiness.
Result decodeFi(const Request&) noexcept;
} // namespace xess_vk::fsr_constants
