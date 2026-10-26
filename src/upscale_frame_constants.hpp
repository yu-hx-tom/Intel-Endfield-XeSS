#pragma once
#include "fsr_input_probe.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace xess_vk::frame_input {
inline constexpr std::size_t upscaleConstantsBytes = 148;
inline constexpr std::uint64_t upscaleSliceOffset = 0x128;
enum Validity : std::uint32_t {
    Extents = 1u << 0, Jitter = 1u << 1, MotionScale = 1u << 2,
    DeltaSeconds = 1u << 3, DepthParameters = 1u << 4, HorizontalFov = 1u << 5,
    Reset = 1u << 6, CameraPlanes = 1u << 7, View = 1u << 8,
    Projection = 1u << 9, FrameIndex = 1u << 10
};
struct OutputRegionSample {
    std::array<std::int32_t, 4> rect{}; // Destination x, y, width, height in the final backbuffer.
    bool observed{}, valid{};
    // Same Compose/graph/frame/ticket proof, never a global latest-frame flag.
    bool localFiOffVerified{};
};
struct FrameConstantsSample {
    // Assigned by the actual recording/registration owner. Decoder never
    // derives these from float frameIndex, wall clock or a global latest sample.
    std::uint64_t deviceIncarnation{}, commandBuffer{}, producerToken{};
    std::uint64_t recordingGeneration{}, viewportGeneration{};
    // Only the same-frame Compose/registration association supplies this;
    // the 148-byte constants decoder never derives an output mapping.
    OutputRegionSample outputRegion;
    std::uint32_t reset{};
    float deltaSeconds{};
    std::array<float, 2> jitter{}, motionVectorScale{};
    std::array<float, 2> previousJitter{}, motionVectorJitterCancellation{};
    float cameraNear{}, cameraFar{}, tanHalfFovHorizontal{};
    std::array<float, 16> view{}, projection{};
    std::uint32_t validityMask{};
    std::array<std::int32_t, 2> renderSize{}, upscaleSize{};
    std::array<float, 4> deviceToViewDepth{};
    float frameIndex{};
};
enum class ConstantsStatus {
    NotAttempted, Decoded, SizeMismatch, NullInput, NonFinite,
    InvalidAddress, MissingReader, ReadFailed, DescriptorChanged
};
struct ConstantsResult {
    ConstantsStatus status{ConstantsStatus::NotAttempted};
    FrameConstantsSample sample;
    std::array<unsigned char, upscaleConstantsBytes> raw{};
    bool rawCaptured{}, descriptorUnchanged{};
    std::uint32_t bufferId{}, byteOffset{}, byteSize{};
    std::uint64_t cpuPointer{};
    std::size_t readCalls{}, bytesRequested{};
};
// Current producer 1833B08 allocates/copies exactly 0x94 (148) bytes via 073020, matching the
// distinct public cbFSR3Upscaler layout. Validity marks decoded finite candidates,
// not shader conventions or readiness for XeFG. The pinned finite inverted-depth
// branch recovers plane candidates; +7C floatIndex=0 records the owner's actual
// temporal history clear. This float is never a frame identity. Matrices still
// require their actual camera source.
ConstantsResult decodeUpscaleConstants(const unsigned char*, std::size_t,
                                      const FrameConstantsSample& provenance = {}) noexcept;
// Fixed three bounded CPU reads: +128 slice, its 148-byte copy, slice recheck.
// Actual Prepare identity and graph/CB association remain the caller's job.
ConstantsResult readPrepareConstants(std::uint64_t payload, fsr_probe::Reader,
                                     const FrameConstantsSample& provenance = {}) noexcept;
}
