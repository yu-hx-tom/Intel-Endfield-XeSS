// CPU-only metadata and byte-layout fixtures. No Vulkan/D3D12 device creation,
// GPU submissions or game access. Synthetic handles here prove no provenance.
#include "interop.hpp"
#include "native_source_fixture.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace xess_vk;
unsigned positive = 0, negative = 0;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++positive;
}
template<class F> void rejects(F&& operation, const char* message) {
    try { operation(); } catch (const std::invalid_argument&) { ++negative; return; }
    throw std::runtime_error(message);
}
template<class T> T inert(uintptr_t number) { return reinterpret_cast<T>(number); }
NativeFrame metadata(const InteropConfig& config) {
    NativeFrame frame;
    frame.producerQueue = inert<VkQueue>(0x2000);
    frame.producerCompleteFence = inert<VkFence>(0x3000);
    for (unsigned i = 0; i < frame.planes.size(); ++i) {
        auto& source = frame.planes[i]; auto& info = source.successfulImageCreateInfo;
        const auto extent = config.extent(static_cast<Format>(i));
        source.device = inert<VkDevice>(0x1000); source.image = inert<VkImage>(0x4000 + i);
        source.ownerQueueFamily = 0; source.currentLayout = VK_IMAGE_LAYOUT_GENERAL;
        info.imageType = VK_IMAGE_TYPE_2D; info.format = testing::kFormats[i]; info.extent = {extent.width, extent.height, 1};
        info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    return frame;
}
void validate(const InteropConfig& config, const NativeFrame& frame) {
    detail::validateNativeFrameMetadata(config, frame, inert<VkDevice>(0x1000), inert<VkQueue>(0x2000), 0);
}
}
int main() {
    try {
        static_assert(std::is_aggregate_v<InteropConfig>);
        const InteropConfig legacy{127, 73, 3000};
        require(!legacy.inputWidth && !legacy.inputHeight, "Legacy aggregate did not preserve equal-resolution defaults");
        for (unsigned i = 0; i < 3; ++i) require(legacy.byteSize(static_cast<Format>(i)) == 127u * 73u * 4u, "Legacy plane byte count changed");
        const InteropConfig mixed{1920, 1080, 3000, 1128, 634};
        const auto mixedFrame = metadata(mixed);
        validate(mixed, mixedFrame); ++positive;
        validate(legacy, metadata(legacy)); ++positive;
        require(mixed.extent(Format::ColorRGBA8).width == 1920 && mixed.extent(Format::ColorRGBA8).height == 1080, "Color extent was changed to input extent");
        require(mixed.byteSize(Format::ColorRGBA8) == 8294400, "Color byte count incorrect");
        require(mixed.byteSize(Format::DepthR32F) == 2860608, "Depth byte count incorrect");
        require(mixed.byteSize(Format::MotionRG16F) == 2860608, "Motion byte count incorrect");
        const auto generated = InteropBridge::makePattern(mixed, 37);
        require(generated[0].size() == 8294400 && generated[1].size() == 2860608 && generated[2].size() == 2860608, "Mixed pattern allocated equal plane sizes");
        float lastDepth = 0;
        std::memcpy(&lastDepth, generated[1].data() + generated[1].size() - 4, 4);
        const float expectedDepth = float((37u * 131u + 1127u + 633u * 1128u) % 8192u) / 8192.0f;
        require(lastDepth == expectedDepth, "Depth pattern used output row width");
        const auto next = InteropBridge::makePattern(mixed, 38);
        for (unsigned i = 0; i < 3; ++i) require(next[i] != generated[i], "Plane pattern lost frame identity");
        testing::SourceOwner expectedOnly; expectedOnly.cfg = mixed;
        const auto gpuClearExpected = expectedOnly.expected(3);
        require(gpuClearExpected[0].size() == 8294400 && gpuClearExpected[1].size() == 2860608 && gpuClearExpected[2].size() == 2860608, "Native clear fixture has wrong plane sizes");
        require(!std::memcmp(gpuClearExpected[2].data(), gpuClearExpected[2].data() + gpuClearExpected[2].size() - 4, 4), "Native clear expected row layout corrupted");
        const InteropConfig explicitEqual{127, 73, 3000, 127, 73};
        require(InteropBridge::makePattern(legacy, 11) == InteropBridge::makePattern(explicitEqual, 11), "Explicit equal extent differs from legacy path");
        for (const auto bad : {InteropConfig{1920,1080,3000,1128,0}, InteropConfig{1920,1080,3000,0,634},
                               InteropConfig{1920,1080,3000,16385,634}, InteropConfig{1920,1080,3000,1128,16385},
                               InteropConfig{0,1080,3000,1128,634}, InteropConfig{1920,16385,3000,1128,634},
                               InteropConfig{1920,1080,0,1128,634}, InteropConfig{1920,1080,60001,1128,634}})
            rejects([&] { bad.validate(); }, "Invalid mixed config accepted");
        rejects([&] { mixed.extent(static_cast<Format>(3)); }, "Invalid plane enum accepted");
        rejects([&] { validate(legacy, mixedFrame); }, "Mixed frame accepted by equal-resolution contract");
        for (unsigned i = 0; i < 3; ++i) {
            auto bad = mixedFrame; ++bad.planes[i].successfulImageCreateInfo.extent.width;
            rejects([&] { validate(mixed, bad); }, "Per-plane wrong width accepted");
            bad = mixedFrame; ++bad.planes[i].successfulImageCreateInfo.extent.height;
            rejects([&] { validate(mixed, bad); }, "Per-plane wrong height accepted");
            bad = mixedFrame; bad.planes[i].successfulImageCreateInfo.format = VK_FORMAT_R8_UNORM;
            rejects([&] { validate(mixed, bad); }, "Wrong plane format accepted");
            bad = mixedFrame; bad.planes[i].ownerQueueFamily = VK_QUEUE_FAMILY_EXTERNAL;
            rejects([&] { validate(mixed, bad); }, "External source owner accepted");
        }
        auto bad = mixedFrame; bad.planes[1].successfulImageCreateInfo.extent = {1920,1080,1};
        rejects([&] { validate(mixed, bad); }, "Output-sized depth silently accepted or rescaled");
        bad = mixedFrame; bad.planes[2].successfulImageCreateInfo.extent = {1920,1080,1};
        rejects([&] { validate(mixed, bad); }, "Output-sized motion silently accepted or rescaled");
        bad = mixedFrame; bad.producerCompleteFence = VK_NULL_HANDLE;
        rejects([&] { validate(mixed, bad); }, "Missing actual producer fence accepted");
        bad = mixedFrame; bad.producerQueue = inert<VkQueue>(0x9999);
        rejects([&] { validate(mixed, bad); }, "Wrong producer queue accepted");
        bad = mixedFrame; bad.planes[1].device = inert<VkDevice>(0x9999);
        rejects([&] { validate(mixed, bad); }, "Wrong device accepted");
        bad = mixedFrame; bad.planes[2].image = bad.planes[1].image;
        rejects([&] { validate(mixed, bad); }, "Aliased source planes accepted");
        bad = mixedFrame; bad.planes[1].currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        rejects([&] { validate(mixed, bad); }, "Undefined source contents accepted");
        std::cout << "{\"stage\":\"mixed-resolution-interop-metadata\",\"passed\":true,\"positiveChecks\":" << positive
                  << ",\"negativeChecks\":" << negative << ",\"colorExtent\":[1920,1080],\"inputExtent\":[1128,634],"
                     "\"gpuExecuted\":false,\"handlesSynthetic\":true,\"producerFenceProvenanceVerified\":false,"
                     "\"motionVectorSemanticsVerified\":false,\"gameInputReady\":false}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Mixed-resolution metadata fixture failed: " << e.what() << '\n';
        return 1;
    }
}
