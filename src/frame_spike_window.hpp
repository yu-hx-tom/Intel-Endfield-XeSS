#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace xess_vk {
// Serialized by the admitted present queue, like the existing per-frame totals.
// Store owned JSON, never a FrameStatus pointer or borrowed diagnostic string.
struct FrameSpikeWindow {
    static constexpr std::int64_t periodNs=60'000'000'000;
    static constexpr std::size_t detailBytes=32768;
    struct Detail {std::int64_t intervalNs{};std::size_t bytes{};std::array<char,detailBytes> text{};};
    std::array<Detail,3> worst{};
    std::int64_t startedNs{},maximumNs{};
    std::uint64_t intervals{},spikes{},droppedDetails{};
    bool due(std::int64_t now)const noexcept{return startedNs&&now>=startedNs&&now-startedNs>=periodNs;}
    void observe(std::int64_t now,std::int64_t interval) noexcept {
        if(!startedNs)startedNs=now;
        if(interval<=0)return;
        ++intervals;if(interval>maximumNs)maximumNs=interval;
    }
    int consider(std::int64_t interval) noexcept {
        ++spikes;
        std::size_t selected=0;
        for(std::size_t n=0;n<worst.size();++n){if(!worst[n].bytes)return int(n);if(worst[n].intervalNs<worst[selected].intervalNs)selected=n;}
        return interval>worst[selected].intervalNs?int(selected):-1;
    }
    void retain(int slot,std::int64_t interval,const char* text,std::size_t bytes) noexcept {
        if(slot<0||std::size_t(slot)>=worst.size()||!text||!bytes||bytes>=detailBytes){++droppedDetails;return;}
        auto& d=worst[slot];d.intervalNs=interval;d.bytes=bytes;std::memcpy(d.text.data(),text,bytes);
    }
    void reset(std::int64_t now) noexcept {
        for(auto& d:worst){d.intervalNs=0;d.bytes=0;}
        startedNs=now;maximumNs=0;intervals=spikes=droppedDetails=0;
    }
};
}
