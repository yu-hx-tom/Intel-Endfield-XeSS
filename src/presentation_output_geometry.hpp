#pragma once
#include "upscale_frame_constants.hpp"
#include <cstdint>
#include <limits>
#include <vulkan/vulkan_core.h>

namespace xess_vk::presentation::detail {
struct LowResOutputGeometry {
    VkExtent2D canvas{};
    VkRect2D region{};
    const char* fallbackReason{};
    explicit operator bool() const noexcept { return !fallbackReason && canvas.width && canvas.height; }
};
// Scale the complete final WSI, including its existing borders. Exact integer
// mappings keep an asymmetric scene region in place without hidden rounding.
inline LowResOutputGeometry lowResOutputGeometry(VkExtent2D source,
    const frame_input::FrameConstantsSample& constants, VkExtent2D depth, VkExtent2D motion) noexcept {
    const auto refuse=[](const char* reason) { return LowResOutputGeometry{{},{},reason}; };
    constexpr auto maximum=std::uint32_t(std::numeric_limits<std::int32_t>::max());
    if(!source.width||!source.height||source.width>maximum||source.height>maximum)
        return refuse("low_res_source_extent_invalid");
    if(!(constants.validityMask&frame_input::Extents)||constants.renderSize[0]<=0||constants.renderSize[1]<=0||
       constants.upscaleSize[0]<=0||constants.upscaleSize[1]<=0)
        return refuse("low_res_constants_extent_invalid");
    const auto& r=constants.outputRegion;
    if(!r.observed||!r.valid||r.rect[0]<0||r.rect[1]<0||r.rect[2]<=0||r.rect[3]<=0||
       std::uint32_t(r.rect[0])>=source.width||std::uint32_t(r.rect[1])>=source.height||
       std::uint32_t(r.rect[2])>source.width-std::uint32_t(r.rect[0])||
       std::uint32_t(r.rect[3])>source.height-std::uint32_t(r.rect[1]))
        return refuse("low_res_output_region_invalid");
    const auto width=std::uint32_t(constants.upscaleSize[0]),height=std::uint32_t(constants.upscaleSize[1]);
    if(!depth.width||!depth.height||depth.width>width||depth.height>height||
       depth.width!=std::uint32_t(constants.renderSize[0])||depth.height!=std::uint32_t(constants.renderSize[1])||
       motion.width!=depth.width||motion.height!=depth.height)
        return refuse("low_res_input_extent_mismatch");
    if(width>=std::uint32_t(r.rect[2])||height>=std::uint32_t(r.rect[3]))
        return refuse("low_res_not_smaller");
    const auto denominator=std::uint64_t(r.rect[2]);
    if(std::uint64_t(width)*std::uint32_t(r.rect[3])!=std::uint64_t(height)*std::uint32_t(r.rect[2]))
        return refuse("low_res_aspect_mismatch");
    const std::uint64_t scaled[]{std::uint64_t(source.width)*width,std::uint64_t(source.height)*width,
        std::uint64_t(r.rect[0])*width,std::uint64_t(r.rect[1])*width};
    for(const auto value:scaled)if(value%denominator||value/denominator>maximum)
        return refuse("low_res_nonintegral_mapping");
    LowResOutputGeometry out;
    out.canvas={std::uint32_t(scaled[0]/denominator),std::uint32_t(scaled[1]/denominator)};
    out.region={{std::int32_t(scaled[2]/denominator),std::int32_t(scaled[3]/denominator)},{width,height}};
    if(!out.canvas.width||!out.canvas.height||out.region.extent.width>out.canvas.width-std::uint32_t(out.region.offset.x)||
       out.region.extent.height>out.canvas.height-std::uint32_t(out.region.offset.y))
        return refuse("low_res_canvas_invalid");
    return out;
}
} // namespace xess_vk::presentation::detail
