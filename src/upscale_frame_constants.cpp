#include "upscale_frame_constants.hpp"
#include <cmath>
#include <cstring>
#include <limits>

namespace xess_vk::frame_input {
namespace {
std::uint32_t u32(const unsigned char* bytes) noexcept {
    std::uint32_t value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}
std::uint64_t u64(const unsigned char* bytes) noexcept {
    std::uint64_t value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}
float f32(const unsigned char* bytes) noexcept {
    float value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}
FrameConstantsSample identity(const FrameConstantsSample& provenance) noexcept {
    FrameConstantsSample result;
    result.deviceIncarnation = provenance.deviceIncarnation;
    result.commandBuffer = provenance.commandBuffer;
    result.producerToken = provenance.producerToken;
    result.recordingGeneration = provenance.recordingGeneration;
    result.viewportGeneration = provenance.viewportGeneration;
    return result;
}
bool range(std::uint64_t address, std::size_t bytes) noexcept {
    return address && bytes <= std::numeric_limits<std::uint64_t>::max() - address;
}
}

ConstantsResult decodeUpscaleConstants(const unsigned char* bytes, std::size_t size,
                                      const FrameConstantsSample& provenance) noexcept {
    ConstantsResult result;
    result.sample = identity(provenance);
    if (size != upscaleConstantsBytes) {
        result.status = ConstantsStatus::SizeMismatch;
        return result;
    }
    if (!bytes) {
        result.status = ConstantsStatus::NullInput;
        return result;
    }
    std::memcpy(result.raw.data(), bytes, result.raw.size());
    result.rawCaptured = true;
    // The first 12 DWORDs are int32 extent pairs; all remaining fields in the
    // published 37-DWORD record are floats. Retain custom finite jitter/scale.
    for (std::size_t offset = 0x30; offset < upscaleConstantsBytes; offset += 4) {
        if (!std::isfinite(f32(bytes + offset))) {
            result.status = ConstantsStatus::NonFinite;
            return result;
        }
    }
    auto& sample = result.sample;
    for (std::size_t index = 0; index < 2; ++index) {
        std::memcpy(&sample.renderSize[index], bytes + index * 4, 4);
        std::memcpy(&sample.upscaleSize[index], bytes + 0x10 + index * 4, 4);
        sample.jitter[index] = f32(bytes + 0x40 + index * 4);
        sample.previousJitter[index] = f32(bytes + 0x48 + index * 4);
        sample.motionVectorScale[index] = f32(bytes + 0x50 + index * 4);
        sample.motionVectorJitterCancellation[index] = f32(bytes + 0x60 + index * 4);
    }
    for (std::size_t index = 0; index < 4; ++index)
        sample.deviceToViewDepth[index] = f32(bytes + 0x30 + index * 4);
    sample.tanHalfFovHorizontal = f32(bytes + 0x68);
    sample.deltaSeconds = f32(bytes + 0x70);
    sample.frameIndex = f32(bytes + 0x7C);
    sample.validityMask = Jitter | MotionScale | DeltaSeconds | DepthParameters | HorizontalFov | FrameIndex;
    // 1833CDF..1833D1E: owner+8C history clear resets +88, copied into record+7C.
    if (sample.frameIndex >= 0) {
        sample.reset = sample.frameIndex == 0;
        sample.validityMask |= Reset;
    }
    if (sample.renderSize[0] > 0 && sample.renderSize[1] > 0 && sample.upscaleSize[0] > 0 && sample.upscaleSize[1] > 0)
        sample.validityMask |= Extents;
    // Current renderer supplies inverted=1/infinite=0 to helper 18339AC.
    // Its A=-Q, B=Q*fMin support this finite reversed-depth reconstruction.
    // Other coefficient shapes publish no plane candidates.
    const double depthA = sample.deviceToViewDepth[0];
    const double depthB = sample.deviceToViewDepth[1];
    if (depthA < 0 && depthB > 0) {
        const float nearPlane = static_cast<float>(depthB / (1 - depthA));
        const float farPlane = static_cast<float>(-depthB / depthA);
        if (std::isfinite(nearPlane) && std::isfinite(farPlane) && nearPlane > 0 && farPlane > nearPlane) {
            sample.cameraNear = nearPlane;
            sample.cameraFar = farPlane;
            sample.validityMask |= CameraPlanes;
        }
    }
    result.status = ConstantsStatus::Decoded;
    return result;
}

ConstantsResult readPrepareConstants(std::uint64_t payload, fsr_probe::Reader reader,
                                     const FrameConstantsSample& provenance) noexcept {
    ConstantsResult result;
    result.sample = identity(provenance);
    if (!reader.read) {
        result.status = ConstantsStatus::MissingReader;
        return result;
    }
    if (!range(payload, upscaleSliceOffset + 24)) {
        result.status = ConstantsStatus::InvalidAddress;
        return result;
    }
    constexpr std::size_t budget = 24 + upscaleConstantsBytes + 24;
    auto read = [&](std::uint64_t address, void* destination, std::size_t bytes) noexcept {
        const auto remaining = budget - result.bytesRequested;
        ++result.readCalls;
        result.bytesRequested += bytes;
        try {
            return reader.read(reader.user, address, destination, bytes, remaining) == bytes;
        } catch (...) {
            return false;
        }
    };
    std::array<unsigned char, 24> descriptor{}, after{};
    if (!read(payload + upscaleSliceOffset, descriptor.data(), descriptor.size())) {
        result.status = ConstantsStatus::ReadFailed;
        return result;
    }
    result.bufferId = u32(descriptor.data());
    result.byteOffset = u32(descriptor.data() + 4);
    result.byteSize = u32(descriptor.data() + 8);
    result.cpuPointer = u64(descriptor.data() + 16);
    if (result.byteSize != upscaleConstantsBytes) {
        result.status = ConstantsStatus::SizeMismatch;
        return result;
    }
    if (!range(result.cpuPointer, upscaleConstantsBytes)) {
        result.status = ConstantsStatus::InvalidAddress;
        return result;
    }
    std::array<unsigned char, upscaleConstantsBytes> bytes{};
    if (!read(result.cpuPointer, bytes.data(), bytes.size()) ||
        !read(payload + upscaleSliceOffset, after.data(), after.size())) {
        result.status = ConstantsStatus::ReadFailed;
        return result;
    }
    if (descriptor != after) {
        result.status = ConstantsStatus::DescriptorChanged;
        return result;
    }
    const auto decoded = decodeUpscaleConstants(bytes.data(), bytes.size(), provenance);
    result.status = decoded.status;
    result.sample = decoded.sample;
    result.raw = decoded.raw;
    result.rawCaptured = decoded.rawCaptured;
    result.descriptorUnchanged = true;
    return result;
}
}
