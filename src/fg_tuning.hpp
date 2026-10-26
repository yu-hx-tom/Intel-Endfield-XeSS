#pragma once
#include <array>
#include <cstdint>

namespace xess_vk::fg_tuning {
// Index zero means no SDK setter call, including when the numeric default is 0.7.
inline constexpr std::array<float,7> thresholds{0.7f,0.3f,0.5f,0.6f,0.7f,0.8f,0.9f};
enum class Debug : unsigned { Off, Tag, FailedBlack, OnlyGenerated };
struct Selection {unsigned threshold{},debug{},invalid{};};
struct Applied {
    Selection requested;
    bool available{};
    bool thresholdAttempted{},thresholdApplied{},debugAttempted{},debugApplied{};
    std::int32_t thresholdResult{},debugResult{};
};
template<class Threshold,class DebugSetter>
Applied apply(Selection value,Threshold threshold,DebugSetter debug) {
    if(value.threshold>=thresholds.size()){value.threshold=0;value.invalid|=1;}
    if(value.debug>3){value.debug=0;value.invalid|=2;}
    Applied out;out.requested=value;out.available=true;
    if(value.threshold){out.thresholdAttempted=true;out.thresholdResult=threshold(thresholds[value.threshold]);out.thresholdApplied=out.thresholdResult==0;}
    if(value.debug){out.debugAttempted=true;out.debugResult=debug(value.debug);out.debugApplied=out.debugResult==0;}
    return out;
}
struct Totals {
    std::uint64_t queries{},requested{},enabled{},multiOutput{},singleOutput{},zeroOutput{},queryUnavailable{};
    std::uint64_t resultWarnings{},resultErrors{},historyResets{};
    void observe(bool requestedOn,int query,bool sdkOn,unsigned frames,int result) noexcept {
        ++queries;if(!requestedOn)return;++requested;
        if(query!=0){++queryUnavailable;return;}
        resultWarnings+=result>0;resultErrors+=result<0;
        if(!sdkOn)return;++enabled;
        if(frames>1)++multiOutput;
        else if(frames==1)++singleOutput;
        else ++zeroOutput;
    }
};
}
