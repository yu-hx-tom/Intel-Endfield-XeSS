#pragma once
#include "frame_timing.hpp"
#include "upscale_tail_probe.hpp"
#include "sr_native_shader_catalog.hpp"
#include <array>
#include <cstdint>

namespace xess_vk::sr_dispatch_shadow {
inline constexpr std::size_t frameLimit=8,dispatchLimit=24;
inline constexpr std::uint32_t prepareBit=1u<<unsigned(upscale_tail_probe::Pass::Count);
constexpr std::uint32_t passBit(upscale_tail_probe::Pass p) noexcept {
    return p>upscale_tail_probe::Pass::Unknown&&p<upscale_tail_probe::Pass::Count?1u<<unsigned(p):0;
}
enum class Api { Unknown,Direct,Indirect,Base,BaseKhr };
enum BlitCheck : std::uint32_t { SourceUsage=1,DestinationUsage=2,SourceFormat=4,DestinationFormat=8,LinearFilter=16,SingleSample=32 };
inline std::uint32_t blitStaticChecks(const image_inventory::Description& source,const image_inventory::Description& destination,
        VkFormatFeatureFlags sourceFeatures,VkFormatFeatureFlags destinationFeatures) noexcept {
    return ((source.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)?SourceUsage:0u)|
        ((destination.usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT)?DestinationUsage:0u)|
        ((sourceFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)?SourceFormat:0u)|
        ((destinationFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT)?DestinationFormat:0u)|
        ((sourceFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)?LinearFilter:0u)|
        ((source.samples==VK_SAMPLE_COUNT_1_BIT&&destination.samples==VK_SAMPLE_COUNT_1_BIT)?SingleSample:0u);
}
struct Dispatch {
    std::uint64_t command{},recording{},incarnation{},order{},pipeline{},pipelineGeneration{};
    std::uint32_t exactMask{},candidateMask{},registeredMask{};
    std::uint32_t blitChecks{};
    VkFormatFeatureFlags sourceFormatFeatures{},destinationFormatFeatures{};
    sr_native_shaders::Policy policy{sr_native_shaders::Policy::KeepUnknown};
    Api api{};
    bool rawPairAvailable{},stage1SourceAvailable{},fingerprintKnown{},blitFormatsQueried{};
};
struct Frame {
    FrameTimingAssociation identity;
    std::array<Dispatch,dispatchLimit> dispatches{};
    std::size_t count{};
    std::uint64_t dropped{};
    bool multipleRecordings{},nonIncreasingOrder{},conflictingDuplicate{};
};
struct Stats {std::uint64_t forwarded{},invalid{},duplicates{},frameLimitDrops{},dispatchLimitDrops{};};
// Bounded recording-order evidence only. No SDK readiness, consumer coverage,
// frame completion, shader semantics or replacement permission is inferred.
class Audit {
    std::array<Frame,frameLimit> frames_{};
    std::size_t count_{};
    Stats stats_;
public:
    void forwarded(const FrameTimingAssociation& identity,const Dispatch& d) noexcept {
        if(!identity.valid||!identity.ticket||!d.command||!d.recording||!d.incarnation||!d.order||
           !d.pipeline||!d.pipelineGeneration||!(d.exactMask|d.candidateMask)) {++stats_.invalid;return;}
        Frame* selected=nullptr;
        for(std::size_t n=0;n<count_;++n)if(frames_[n].identity.engineFrame==identity.engineFrame&&frames_[n].identity.ticket==identity.ticket){selected=&frames_[n];break;}
        if(!selected){if(count_==frameLimit){++stats_.frameLimitDrops;return;}selected=&frames_[count_++];selected->identity=identity;}
        for(std::size_t n=0;n<selected->count;++n){const auto& previous=selected->dispatches[n];
            if(previous.command!=d.command||previous.recording!=d.recording||previous.incarnation!=d.incarnation)continue;
            if(previous.order==d.order){++stats_.duplicates;
                selected->conflictingDuplicate|=previous.pipeline!=d.pipeline||previous.pipelineGeneration!=d.pipelineGeneration||
                    previous.exactMask!=d.exactMask||previous.candidateMask!=d.candidateMask||previous.api!=d.api;return;}
            if(previous.order>d.order)selected->nonIncreasingOrder=true;
        }
        if(selected->count){const auto& first=selected->dispatches[0];
            selected->multipleRecordings|=first.command!=d.command||first.recording!=d.recording||first.incarnation!=d.incarnation;}
        ++stats_.forwarded;
        if(selected->count==dispatchLimit){++selected->dropped;++stats_.dispatchLimitDrops;return;}
        selected->dispatches[selected->count++]=d;
    }
    std::size_t count() const noexcept{return count_;}
    const Frame& frame(std::size_t n) const noexcept{return frames_[n];}
    Stats stats() const noexcept{return stats_;}
};
}
