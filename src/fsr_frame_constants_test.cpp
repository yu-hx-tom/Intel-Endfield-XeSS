#include "fsr_frame_constants.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace reference {
// FrameInterpolationConstants below is copied from the official SDK v1.1.4,
// commit c6efa6bf7f2027b3ec94f28578bb5965eabb9e55, private header lines 40-78.
// Copyright (C) 2024 Advanced Micro Devices, Inc.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
typedef struct FrameInterpolationConstants {
    int32_t renderSize[2];
    int32_t displaySize[2];
    float displaySizeRcp[2];
    float cameraNear;
    float cameraFar;
    int32_t upscalerTargetSize[2];
    int Mode;
    int Reset;
    float deviceToViewDepth[4];
    float deltaTime;
    int HUDLessAttachedFactor;
    int32_t distortionFieldSize[2];
    float opticalFlowScale[2];
    int32_t opticalFlowBlockSize;
    uint32_t dispatchFlags;
    int32_t maxRenderSize[2];
    int opticalFlowHalfResMode;
    int numInstances;
    int32_t interpolationRectBase[2];
    int32_t interpolationRectSize[2];
    float debugBarColor[3];
    uint32_t backBufferTransferFunction;
    float minMaxLuminance[2];
    float fTanHalfFOV;
    float _pad1;
    float jitter[2];
    float motionVectorScale[2];
} FrameInterpolationConstants;
static_assert(sizeof(FrameInterpolationConstants) == 176);
static_assert(offsetof(FrameInterpolationConstants, cameraNear) == 0x18);
static_assert(offsetof(FrameInterpolationConstants, Reset) == 0x2C);
static_assert(offsetof(FrameInterpolationConstants, deltaTime) == 0x40);
static_assert(offsetof(FrameInterpolationConstants, fTanHalfFOV) == 0x98);
static_assert(offsetof(FrameInterpolationConstants, jitter) == 0xA0);
static_assert(offsetof(FrameInterpolationConstants, motionVectorScale) == 0xA8);
} // namespace reference

namespace {
using namespace xess_vk::fsr_constants;
using PublicFi = reference::FrameInterpolationConstants;
std::size_t assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
Request official(const unsigned char* bytes, std::size_t size) {
    return {FiLayout::AmdSdk114CbFi176, requiredAmdSdkVersion, bytes, size};
}
Request game(const unsigned char* bytes, std::size_t size) {
    return {FiLayout::UnityPlayerBee7be52CbFi224, requiredUnitySha256, bytes, size};
}
template<class Container>
void putWord(Container& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<unsigned char>(value >> (8 * i));
}
bool zeroFields(const FiFieldCandidates& f) {
    return f.renderSize == std::array<std::int32_t, 2>{} &&
        f.displaySize == std::array<std::int32_t, 2>{} &&
        f.displaySizeRcp == std::array<float, 2>{} && f.cameraNear == 0 && f.cameraFar == 0 &&
        f.upscalerTargetSize == std::array<std::int32_t, 2>{} && f.resetRaw == 0 &&
        f.deviceToViewDepth == std::array<float, 4>{} && f.deltaTime == 0 &&
        f.opticalFlowScale == std::array<float, 2>{} && f.tanHalfFovHorizontal == 0 &&
        f.jitter == std::array<float, 2>{} && f.motionVectorScale == std::array<float, 2>{};
}

std::array<unsigned char, officialFiBytes> publicFixture() {
    static_assert(std::endian::native == std::endian::little, "fixture serializes the published native C ABI");
    PublicFi cb{};
    cb.renderSize[0] = 1280; cb.renderSize[1] = 720;
    cb.displaySize[0] = 1920; cb.displaySize[1] = 1080;
    cb.displaySizeRcp[0] = 1.f / 1920; cb.displaySizeRcp[1] = 1.f / 1080;
    cb.cameraNear = 0.1f; cb.cameraFar = 1000.f;
    cb.upscalerTargetSize[0] = 1680; cb.upscalerTargetSize[1] = 945;
    cb.Mode = 9; cb.Reset = 1;
    cb.deviceToViewDepth[0] = -0.25f; cb.deviceToViewDepth[1] = 0.5f;
    cb.deviceToViewDepth[2] = 1.5f; cb.deviceToViewDepth[3] = -2.f;
    cb.deltaTime = 16.75f;
    cb.opticalFlowScale[0] = 1.f / 1920; cb.opticalFlowScale[1] = 1.f / 1080;
    cb.fTanHalfFOV = 1.25f;
    cb._pad1 = std::bit_cast<float>(0x7FC00123u); // Uninterpreted padding.
    cb.jitter[0] = -0.75f; cb.jitter[1] = 0.625f; // Must not enforce [-.5,.5].
    cb.motionVectorScale[0] = -2.f; cb.motionVectorScale[1] = 0.f;
    std::array<unsigned char, officialFiBytes> bytes{};
    std::memcpy(bytes.data(), &cb, sizeof(cb));
    return bytes;
}

void publicCases() {
    auto bytes = publicFixture();
    auto result = decodeFi(official(bytes.data(), bytes.size()));
    require(result.status == Status::DecodedUnverified, "public ABI fixture decodes");
    require(result.rawRetained && result.rawPrefix == bytes && result.unknownTailSize == 0,
            "full public bytes and padding retained");
    const auto& f = result.fields;
    require(f.renderSize == std::array<std::int32_t, 2>{1280, 720}, "public render extent");
    require(f.displaySize == std::array<std::int32_t, 2>{1920, 1080}, "public display extent");
    require(f.displaySizeRcp == std::array<float, 2>{1.f / 1920, 1.f / 1080}, "public reciprocal extent");
    require(f.upscalerTargetSize == std::array<std::int32_t, 2>{1680, 945}, "target is distinct from display");
    require(f.cameraNear == .1f && f.cameraFar == 1000.f && f.resetRaw == 1, "public near far reset");
    require(f.deviceToViewDepth == std::array<float, 4>{-.25f, .5f, 1.5f, -2.f}, "four coefficients retained");
    require(f.deltaTime == 16.75f && f.tanHalfFovHorizontal == 1.25f, "time and tangent unchanged");
    require(f.opticalFlowScale == std::array<float, 2>{1.f / 1920, 1.f / 1080}, "optical scale distinct");
    require(f.jitter == std::array<float, 2>{-.75f, .625f}, "custom jitter range accepted without clamping");
    require(f.motionVectorScale == std::array<float, 2>{-2.f, 0.f}, "MV scale unchanged without semantic validation");
    std::vector<unsigned char> unaligned(bytes.size() + 1);
    std::copy(bytes.begin(), bytes.end(), unaligned.begin() + 1);
    require(decodeFi(official(unaligned.data() + 1, bytes.size())).rawPrefix == bytes, "unaligned CPU bytes");

    putWord(bytes, offsetof(PublicFi, cameraNear), std::bit_cast<std::uint32_t>(10000.f));
    putWord(bytes, offsetof(PublicFi, cameraFar), std::bit_cast<std::uint32_t>(.1f));
    putWord(bytes, offsetof(PublicFi, Reset), 255);
    result = decodeFi(official(bytes.data(), bytes.size()));
    require(result.status == Status::DecodedUnverified && result.fields.cameraNear == 10000.f &&
            result.fields.cameraFar == .1f && result.fields.resetRaw == 255, "no near/far sort or reset Boolean guess");
    putWord(bytes, offsetof(PublicFi, cameraNear), 0x7F7FFFFFu);
    putWord(bytes, offsetof(PublicFi, jitter), 0x80000000u);
    putWord(bytes, offsetof(PublicFi, jitter) + 4, 1);
    result = decodeFi(official(bytes.data(), bytes.size()));
    require(result.status == Status::DecodedUnverified && result.fields.cameraNear == std::numeric_limits<float>::max(),
            "finite FLT_MAX depth endpoint accepted");
    require(std::bit_cast<std::uint32_t>(result.fields.jitter[0]) == 0x80000000u &&
            std::bit_cast<std::uint32_t>(result.fields.jitter[1]) == 1, "negative zero and subnormal bits preserved");
}

void versionAndSizeCases() {
    const auto* unreadable = reinterpret_cast<const unsigned char*>(std::uintptr_t{1});
    for (const auto layout : {FiLayout::Unknown, static_cast<FiLayout>(999)}) {
        require(decodeFi({layout, requiredAmdSdkVersion, unreadable, officialFiBytes}).status == Status::UnknownLayout,
                "unknown layout rejected before dereference");
    }
    for (const auto version : {std::string_view{}, std::string_view{"v1.1.3"}, std::string_view{"v1.1.4 "}}) {
        auto request = official(unreadable, officialFiBytes); request.sourceVersion = version;
        require(decodeFi(request).status == Status::VersionMismatch, "wrong official version rejected before read");
    }
    for (const auto& version : {std::string{}, std::string(requiredUnitySha256.substr(0, 8)),
                              std::string(requiredUnitySha256) + " "}) {
        auto request = game(unreadable, unityFiBytes); request.sourceVersion = version;
        require(decodeFi(request).status == Status::VersionMismatch, "complete game identity required");
    }
    auto changed = std::string(requiredUnitySha256);
    for (std::size_t i = 0; i < changed.size(); ++i) {
        const char old = changed[i]; changed[i] = old == '0' ? '1' : '0';
        auto request = game(unreadable, unityFiBytes); request.sourceVersion = changed;
        require(decodeFi(request).status == Status::VersionMismatch, "each changed hash character rejected");
        changed[i] = old;
    }
    for (std::size_t size = 0; size <= unityFiBytes + 1; ++size) {
        for (const bool isGame : {false, true}) {
            const std::size_t expected = isGame ? unityFiBytes : officialFiBytes;
            if (size == expected) continue;
            const auto result = decodeFi(isGame ? game(unreadable, size) : official(unreadable, size));
            require(result.status == Status::SizeMismatch && !result.rawRetained && zeroFields(result.fields),
                    "all truncations/extra bytes rejected before read");
            require(std::all_of(result.rawPrefix.begin(), result.rawPrefix.end(), [](auto b) { return b == 0; }) &&
                    result.unknownTailSize == 0, "failed preflight publishes no bytes");
        }
    }
    require(decodeFi(official(nullptr, officialFiBytes)).status == Status::NullInput, "null public input");
    require(decodeFi(game(nullptr, unityFiBytes)).status == Status::NullInput, "null game input");
}

void nonFiniteAndOpaqueCases() {
    // Offsets independently derive from the published full C structure above.
    constexpr std::array<std::size_t, 16> offsets{
        offsetof(PublicFi, displaySizeRcp), offsetof(PublicFi, displaySizeRcp) + 4,
        offsetof(PublicFi, cameraNear), offsetof(PublicFi, cameraFar),
        offsetof(PublicFi, deviceToViewDepth), offsetof(PublicFi, deviceToViewDepth) + 4,
        offsetof(PublicFi, deviceToViewDepth) + 8, offsetof(PublicFi, deviceToViewDepth) + 12,
        offsetof(PublicFi, deltaTime), offsetof(PublicFi, opticalFlowScale), offsetof(PublicFi, opticalFlowScale) + 4,
        offsetof(PublicFi, fTanHalfFOV), offsetof(PublicFi, jitter), offsetof(PublicFi, jitter) + 4,
        offsetof(PublicFi, motionVectorScale), offsetof(PublicFi, motionVectorScale) + 4};
    for (const auto offset : offsets) for (const auto bits : {0x7F800000u, 0xFF800000u, 0x7FC01234u, 0x7F800001u}) {
        auto bytes = publicFixture(); putWord(bytes, offset, bits);
        const auto result = decodeFi(official(bytes.data(), bytes.size()));
        require(result.status == Status::NonFiniteCandidate && result.firstNonFiniteByteOffset == offset,
                "all named float components reject infinity and quiet/signaling NaN");
        require(result.rawRetained && result.rawPrefix == bytes && zeroFields(result.fields),
                "nonfinite record retains raw evidence without publishing partial typed fields");
    }
    std::array<unsigned char, unityFiBytes> bytes{};
    const auto prefix = publicFixture(); std::copy(prefix.begin(), prefix.end(), bytes.begin());
    for (std::size_t i = officialFiBytes; i < bytes.size(); ++i) bytes[i] = static_cast<unsigned char>(i * 37 + 11);
    for (std::size_t i = officialFiBytes; i < bytes.size(); i += 4) putWord(bytes, i, 0x7FC01234u);
    auto upper = std::string(requiredUnitySha256);
    for (auto& ch : upper) if (ch >= 'a' && ch <= 'f') ch = static_cast<char>(ch - 'a' + 'A');
    auto request = game(bytes.data(), bytes.size()); request.sourceVersion = upper;
    auto result = decodeFi(request);
    require(result.status == Status::DecodedUnverified && result.rawPrefix == prefix,
            "game version accepts full uppercase SHA and same prefix");
    require(result.unknownTailSize == unityUnknownTailBytes &&
            std::equal(result.unknownTail.begin(), result.unknownTail.end(), bytes.begin() + officialFiBytes),
            "all 48 opaque tail bytes preserved without float interpretation");
    putWord(bytes, offsetof(PublicFi, cameraFar), 0x7F800000u);
    result = decodeFi(request);
    require(result.status == Status::NonFiniteCandidate && result.unknownTailSize == unityUnknownTailBytes &&
            std::equal(result.unknownTail.begin(), result.unknownTail.end(), bytes.begin() + officialFiBytes),
            "nonfinite game field does not discard opaque tail");
}

void savedGameCase() {
    // Exact raw_u32_bits from frozen game observer.jsonl line 7465, SHA-256
    // 0ed940be93a0b7eb4a8c095f8d43e6a6d87b9a86435d6ac754437bb56594ef21.
    // Decoding this diagnostic record cannot upgrade its false verification flags.
    constexpr std::array<std::uint32_t, 56> words{
        1128,634,1920,1080,973637769,980597206,1036831949,1176256512,
        1920,1080,0,0,3072837146,1036832083,1065121952,1057875764,
        1024797209,1,0,0,973637769,980597206,8,0,
        1920,1080,0,0,0,0,1920,1080,1065353216,0,0,0,0,0,1065108914,1,
        1040187392,1049508068,1065353216,1065353216,
        1056964608,1056964608,0,0,1065923642,1065353216,1065353216,0,0,0,0,1065353216};
    std::array<unsigned char, unityFiBytes> bytes{};
    for (std::size_t i = 0; i < words.size(); ++i) putWord(bytes, i * 4, words[i]);
    const auto result = decodeFi(game(bytes.data(), bytes.size()));
    require(result.status == Status::DecodedUnverified, "saved complete game record decodes");
    const auto& f = result.fields;
    require(f.renderSize == std::array<std::int32_t, 2>{1128, 634} &&
            f.displaySize == std::array<std::int32_t, 2>{1920, 1080}, "saved game extents");
    require(f.cameraNear == .1f && f.cameraFar == 10000.f && f.resetRaw == 0, "saved game depth/reset candidates");
    require(f.deltaTime == 0.036417100578546524f && f.tanHalfFovHorizontal == .9854384660720825f,
            "saved time preserved without factor-1000 or tangent-to-angle conversion");
    require(f.jitter == std::array<float, 2>{.125f, .2777777910232544f} &&
            f.motionVectorScale == std::array<float, 2>{1.f, 1.f}, "saved jitter and MV candidates");
    require(result.unknownTailSize == 48 && result.unknownTail[3] == 0x3F,
            "saved game extra block stays opaque");
}

std::size_t savedRecords(const char* path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "saved records file opens");
    std::array<unsigned char, unityFiBytes> bytes{};
    std::size_t records = 0;
    while (input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        const auto result = decodeFi(game(bytes.data(), bytes.size()));
        require(result.status == Status::DecodedUnverified && result.rawRetained && result.unknownTailSize == 48,
                "saved binary record decodes with opaque tail");
        ++records;
    }
    require(input.eof() && input.gcount() == 0 && records > 0, "saved records are complete and nonempty");
    return records;
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc <= 2, "usage: fsr_frame_constants_test [saved-224B-records.bin]");
        publicCases(); versionAndSizeCases(); nonFiniteAndOpaqueCases(); savedGameCase();
        const std::size_t records = argc == 2 ? savedRecords(argv[1]) : 0;
        std::cout << "{\"passed\":true,\"assertions\":" << assertions
                  << ",\"extra_saved_records\":" << records
                  << ",\"shader_semantics_verified\":false,\"xefg_constants_ready\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
