#include "frame_camera_input.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace frame_fixture {
namespace input = xess_vk::frame_input;
unsigned checks{};
void require(bool value, const char* why) {
    ++checks;
    if (!value) throw std::runtime_error(why);
}
// Published AMD v1.1.4 CPU cbFSR3Upscaler structure; not the cbFI structure.
struct PublishedUpscaleConstants {
    std::int32_t renderSize[2], previousFrameRenderSize[2];
    std::int32_t upscaleSize[2], previousFrameUpscaleSize[2];
    std::int32_t maxRenderSize[2], maxUpscaleSize[2];
    float deviceToViewDepth[4];
    float jitterOffset[2], previousFrameJitterOffset[2];
    float motionVectorScale[2], downscaleFactor[2];
    float motionVectorJitterCancellation[2], tanHalfFOV, jitterPhaseCount;
    float deltaTime, deltaPreExposure, viewSpaceToMetersFactor, frameIndex;
    float velocityFactor, reactivenessScale, shadingChangeScale;
    float accumulationAddedPerFrame, minDisocclusionAccumulation;
};
static_assert(sizeof(PublishedUpscaleConstants) == 148);
PublishedUpscaleConstants constants() {
    PublishedUpscaleConstants value{};
    value.renderSize[0] = 1128; value.renderSize[1] = 634;
    value.upscaleSize[0] = 1920; value.upscaleSize[1] = 1080;
    value.deviceToViewDepth[0] = -0.0000100001f;
    value.deviceToViewDepth[1] = 0.100001f;
    value.deviceToViewDepth[2] = 0.9854f; value.deviceToViewDepth[3] = 0.5538f;
    value.jitterOffset[0] = 0.125f; value.jitterOffset[1] = 1.25f;
    value.previousFrameJitterOffset[0] = 0.625f; value.previousFrameJitterOffset[1] = -0.375f;
    value.motionVectorScale[0] = 1.0f; value.motionVectorScale[1] = -2.0f;
    value.motionVectorJitterCancellation[0] = 0.0001f; value.motionVectorJitterCancellation[1] = 0.0002f;
    value.tanHalfFOV = 0.9854f; value.deltaTime = 0.0364f; value.frameIndex = 17.0f;
    return value;
}
struct ReaderFixture {
    std::array<unsigned char, 0x140> payload{};
    PublishedUpscaleConstants record = constants();
    unsigned calls{}, failCall{};
    bool changed{};
    std::array<std::size_t, 3> budgets{};
    ReaderFixture() {
        struct Slice { std::uint32_t id, offset, size, opaque; const void* pointer; };
        static_assert(sizeof(Slice) == 24);
        const Slice slice{77, 256, sizeof(record), 0x87654321, &record};
        std::memcpy(payload.data() + 0x128, &slice, sizeof(slice));
    }
    static std::size_t read(void* user, std::uint64_t address, void* destination, std::size_t size, std::size_t budget) {
        auto& fixture = *static_cast<ReaderFixture*>(user);
        ++fixture.calls;
        if (fixture.calls <= fixture.budgets.size()) fixture.budgets[fixture.calls - 1] = budget;
        if (fixture.calls == fixture.failCall) return 0;
        const bool slice = address == reinterpret_cast<std::uintptr_t>(fixture.payload.data() + 0x128) && size == 24;
        const bool record = address == reinterpret_cast<std::uintptr_t>(&fixture.record) && size == sizeof(fixture.record);
        if ((!slice && !record) || size > budget) return 0;
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        if (fixture.changed && fixture.calls == 3) static_cast<unsigned char*>(destination)[0] ^= 1;
        return size;
    }
    auto capture(const input::FrameConstantsSample& provenance) {
        return input::readPrepareConstants(reinterpret_cast<std::uintptr_t>(payload.data()), {read, this}, provenance);
    }
};
void actualConstantsDecoderAndReader() {
    input::FrameConstantsSample identity;
    identity.deviceIncarnation = 4; identity.commandBuffer = 0x888; identity.producerToken = 71;
    identity.recordingGeneration = 6; identity.viewportGeneration = 3;
    identity.validityMask = input::View | input::Projection | input::Reset;
    identity.view[0] = 1; identity.projection[0] = 1; identity.reset = 1;
    ReaderFixture fixture;
    const auto result = fixture.capture(identity);
    const auto& sample = result.sample;
    require(result.status == input::ConstantsStatus::Decoded && result.rawCaptured && result.descriptorUnchanged && result.readCalls == 3 &&
        result.bytesRequested == 196 && fixture.budgets == std::array<std::size_t, 3>{196, 172, 24},
        "production reader copies one actual 24-byte slice and its 148-byte CPU constants, then rechecks within fixed budget");
    require(sample.deviceIncarnation == 4 && sample.commandBuffer == 0x888 && sample.producerToken == 71 &&
        sample.recordingGeneration == 6 && sample.viewportGeneration == 3 && result.bufferId == 77 && result.byteOffset == 256,
        "actual producer/recording provenance preserved without deriving a frame token from float frameIndex");
    require(sample.renderSize == std::array<std::int32_t, 2>{1128, 634} && sample.upscaleSize == std::array<std::int32_t, 2>{1920, 1080} &&
        sample.jitter == std::array<float, 2>{0.125f, 1.25f} && sample.motionVectorScale == std::array<float, 2>{1.0f, -2.0f} &&
        sample.previousJitter == std::array<float, 2>{0.625f, -0.375f} && sample.motionVectorJitterCancellation == std::array<float, 2>{0.0001f, 0.0002f} &&
        sample.deltaSeconds == fixture.record.deltaTime && sample.frameIndex == 17.0f && sample.tanHalfFovHorizontal == fixture.record.tanHalfFOV,
        "published upscaler fields decode at their own layout without clamping jitter, rescaling MV or converting time");
    require((sample.validityMask & input::CameraPlanes) && std::abs(sample.cameraNear - 0.1f) < 0.00001f && std::abs(sample.cameraFar - 10000) < 0.01f,
        "finite inverted depth helper coefficients recover actual min/max plane candidates algebraically");
    require(!(sample.validityMask & (input::View | input::Projection)) && (sample.validityMask & input::Reset) && sample.view[0] == 0 &&
        sample.projection[0] == 0 && !sample.reset && !input::xefgJitterInRange(sample),
        "upscaler decode replaces stale reset with the actual counter and cannot inherit a stale camera; SDK jitter admission remains separate");
    std::array<unsigned char, 224> fi{};
    require(input::decodeUpscaleConstants(fi.data(), fi.size()).status == input::ConstantsStatus::SizeMismatch,
        "cbFI 224-byte records cannot be mistaken for the 148-byte upscaler record");
    fixture.record.deltaTime = std::numeric_limits<float>::quiet_NaN();
    const auto invalid = input::decodeUpscaleConstants(reinterpret_cast<const unsigned char*>(&fixture.record), sizeof(fixture.record), identity);
    require(invalid.status == input::ConstantsStatus::NonFinite && invalid.rawCaptured && !invalid.sample.validityMask && invalid.sample.producerToken == 71,
        "nonfinite CPU constants preserve diagnostic bytes and identity without publishing typed fields");
    ReaderFixture changed; changed.changed = true;
    require(changed.capture(identity).status == input::ConstantsStatus::DescriptorChanged,
        "changed slice descriptor cannot publish a constants sample");
    ReaderFixture unreadable; unreadable.failCall = 2;
    const auto failure = unreadable.capture(identity);
    require(failure.status == input::ConstantsStatus::ReadFailed && failure.readCalls == 2 && !failure.rawCaptured && !failure.sample.validityMask,
        "failed CPU read does not reuse any previous sample");
}
void actualHistoryReset() {
    auto record = constants();
    input::FrameConstantsSample provenance;
    provenance.producerToken = 9001; provenance.validityMask = input::Reset; provenance.reset = 1;
    for (const float index : {0.0f, 1.0f, 17.0f, -1.0f}) {
        record.frameIndex = index;
        const auto decoded = input::decodeUpscaleConstants(reinterpret_cast<const unsigned char*>(&record), sizeof(record), provenance);
        require(decoded.status == input::ConstantsStatus::Decoded && decoded.sample.producerToken == 9001 &&
            bool(decoded.sample.validityMask & input::Reset) == (index >= 0) &&
            decoded.sample.reset == std::uint32_t(index == 0),
            "same-invocation float zero resets once; subsequent counters clear reset, negative counters cannot inherit reset validity or act as frame identity");
    }
    record.frameIndex = std::numeric_limits<float>::quiet_NaN();
    const auto invalid = input::decodeUpscaleConstants(reinterpret_cast<const unsigned char*>(&record), sizeof(record), provenance);
    require(invalid.status == input::ConstantsStatus::NonFinite && !invalid.sample.validityMask && !invalid.sample.reset,
        "nonfinite frameIndex publishes no reset or typed constants");
}
void bits(std::uint32_t& output, float value) { std::memcpy(&output, &value, sizeof(value)); }
void cameraGroup(xess_vk::camera_probe::Snapshot& camera, unsigned offset, const std::array<float, 16>& columnMajor) {
    for (unsigned index = 0; index < 16; ++index) bits(camera.prefixFloatBits[offset / 4 + index], columnMajor[index]);
}
bool close(float a, float b) { return std::abs(a - b) < 0.00001f; }
void actualCameraConversion() {
    xess_vk::camera_probe::Snapshot camera; camera.captured = true;
    // Nonidentity rotation with translation: inverse maps the actual camera
    // position to the origin and its right/up/Z columns to unit camera axes.
    const std::array<float, 16> transform{0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 5, 2, -3, 1};
    cameraGroup(camera, 0, transform);
    const std::array<float, 16> projection{2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0.25f, -1, 0, 0, 0.5f, 0};
    cameraGroup(camera, 0xC0, projection);
    input::FrameConstantsSample sample; sample.producerToken = 73; sample.validityMask = input::Reset; sample.reset = 1;
    auto convention = input::pinnedUnityProjectionConvention();
    const auto converted = input::convertFrameCamera(camera, sample, convention);
    require(converted.status == input::CameraStatus::Converted && (converted.sample.validityMask & (input::View | input::Projection)) ==
        (input::View | input::Projection) && converted.sample.producerToken == 73 &&
        (converted.sample.validityMask & input::Reset) && converted.sample.reset == 1,
        "production converter preserves actual reset while creating matrices from this snapshot and explicit convention");
    const auto& view = converted.sample.view;
    require(view[0] == 0 && view[2] == 1 && view[3] == 0 && view[5] == 1 && view[8] == -1 && view[12] == -3 && view[13] == -2 && view[14] == -5 && view[15] == 1,
        "actual nonidentity camera basis/position inverse maps to world-to-view rather than an identity fallback");
    require(converted.sample.projection[0] == 2 && converted.sample.projection[5] == -3 && converted.sample.projection[11] == -1 &&
        converted.sample.projection[14] == 0.5f, "SDK projection preserves SL row-vector storage and flips exact Y indices while retaining depth and W");
    sample.validityMask = input::Extents | input::Jitter;
    sample.renderSize = {100, 50}; sample.jitter = {0.25f, -0.5f};
    auto jittered = projection;
    // Add signed pixel jitter as a full clip translation of the W row.
    for (unsigned row = 0; row < 4; ++row) {
        jittered[row * 4] += 0.005f * projection[row * 4 + 3];
        jittered[row * 4 + 1] += 0.02f * projection[row * 4 + 3];
    }
    cameraGroup(camera, 0xC0, jittered);
    convention.jitter = input::ProjectionJitter::CurrentPixelJitter;
    convention.pixelJitterSignX = 1; convention.pixelJitterSignY = -1;
    const auto removed = input::convertFrameCamera(camera, sample, convention);
    require(removed.status == input::CameraStatus::Converted && close(removed.sample.projection[8], 0) && close(removed.sample.projection[9], 0) &&
        removed.sample.projection[11] == -1 && input::xefgJitterInRange(sample),
        "calibrated pixel-to-NDC jitter is removed using complete perspective W column without clamping SDK offsets");
    input::ProjectionConvention unknown;
    const auto missing = input::convertFrameCamera(camera, sample, unknown);
    require(missing.status == input::CameraStatus::ProjectionConventionMissing && !(missing.sample.validityMask & (input::View | input::Projection)),
        "unknown jitter/Y/depth/source conventions do not masquerade as ready matrices");
    auto singular = camera;
    for (unsigned index = 0; index < 12; ++index) bits(singular.prefixFloatBits[index], 0);
    require(input::convertFrameCamera(singular, sample, convention).status == input::CameraStatus::SingularBasis,
        "degenerate camera basis is rejected instead of inventing a view transform");
}
}
int main() {
    using namespace frame_fixture;
    try {
        actualConstantsDecoderAndReader(); actualHistoryReset(); actualCameraConversion();
        std::cout << "{\"stage\":\"production-upscale-constants-camera\",\"passed\":true,\"checks\":" << checks
            << ",\"gpuTouched\":false,\"gameTouched\":false,\"gameFunctionsCalled\":false}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
