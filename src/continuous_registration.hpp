#pragma once
#include "camera_input_probe.hpp"
#include "graph_registration_bridge.hpp"
#include "frame_timing.hpp"
#include "upscale_frame_constants.hpp"
namespace xess_vk::continuous_registration {
struct CameraSample {
    camera_probe::Snapshot camera;
    std::uint64_t registration{};
    bool matched{};
    FrameTimingAssociation frame;
    frame_input::OutputRegionSample outputRegion;
};
using FrameReader = FrameTimingAssociation(*)(void*, std::uint32_t thread);
bool configure(std::uintptr_t verifiedModule, std::uint64_t device, FrameReader = nullptr, void* = nullptr) noexcept;
void activate() noexcept;
void stop() noexcept;
// True only for this first Compose's unique graph/frame/ticket and valid ROI.
bool observeCompose(void* originalInput, void* graph, std::uint32_t thread,
                    bool localFiOffCandidate = false) noexcept;
CameraSample consume(std::uintptr_t actualCopiedPayload) noexcept;
} // namespace xess_vk::continuous_registration
extern "C" void *continuous_registration_original;
