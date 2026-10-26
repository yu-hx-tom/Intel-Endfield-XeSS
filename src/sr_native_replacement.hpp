#pragma once
#include "descriptor_image_metadata.hpp"
#include <array>
#include <span>
#include <vector>

namespace xess_vk::sr_replacement {
using Key=descriptor_image_metadata::Key;
// The seven exact shader profiles, in the observed pinned-build order.
inline constexpr unsigned firstCore=2,lastCore=8,coreCount=7;
inline bool fullColorRange(const VkImageSubresourceRange& r,unsigned mips,unsigned layers) noexcept {
    return mips&&layers&&r.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&!r.baseMipLevel&&!r.baseArrayLayer&&
        (r.levelCount==mips||r.levelCount==VK_REMAINING_MIP_LEVELS)&&
        (r.layerCount==layers||r.layerCount==VK_REMAINING_ARRAY_LAYERS);
}
enum class Fault {None,Consumer,Capacity,Sequence,IncompleteFrame,Terminal};
struct Counts {
    std::uint64_t replacementFrames{},guards{},discards{},capacity{};
    std::array<std::uint64_t,coreCount> skipped{},forwarded{};
};
// Kept writes are valid content, not missing content. A full-image discard
// kills either content version; an ordinary native write cannot prove full coverage.
class Transaction {
    struct Content {Key image;bool needed{},omitted{};unsigned producer{};};
    // Sized by live resource identities, not by a diagnostic sample budget.
    std::vector<Content> contents_;
    std::uint64_t frame_{},command_{},recording_{};
    unsigned next_{firstCore};
    bool open_{},replace_{},stopped_{};
    Fault fault_{Fault::None};Counts counts_;
    std::uint64_t contentsRevision_{1};
    void changed()noexcept {if(contentsRevision_)contentsRevision_=contentsRevision_==UINT64_MAX?0:contentsRevision_+1;}
public:
    std::uint64_t contentsRevision()const noexcept{return contentsRevision_;}
    bool referenceMasks(std::span<Key> images,std::size_t& count,std::uint64_t& missing)const noexcept {
        count=0;missing=0;
        for(const auto& content:contents_)if(content.image.handle){
            if(count>=images.size()||count>=64)return false;
            images[count]=content.image;if(content.needed&&content.omitted)missing|=UINT64_C(1)<<count;++count;
        }
        return true;
    }
    bool replacement()const noexcept{return open_&&replace_;}
    bool previousReplaced()const noexcept{return replace_;}
    bool open()const noexcept{return open_;}
    bool owns(std::uint64_t command,std::uint64_t recording)const noexcept{return command_==command&&recording_==recording;}
    bool stopped()const noexcept{return stopped_;}
    Fault fault()const noexcept{return fault_;}
    bool fatal()const noexcept{return stopped_&&replace_;}
    const Counts& counts()const noexcept{return counts_;}
    void retired(std::uint64_t command,std::uint64_t recording) noexcept {
        if(owns(command,recording)){open_=false;command_=recording_=0;}
    }
    // Only after a stopped run has shown the engine's own frames and its
    // consumers went quiet: forgetting omitted content never grants FSR work.
    unsigned abandonOmitted() noexcept {
        if(!stopped_)return 0;
        unsigned n{};for(auto& c:contents_)if(c.needed&&c.omitted){c.needed=false;++n;}
        if(n)changed();
        return n;
    }
    bool recover() noexcept {
        if(open_||missingContents())return false;
        contents_.clear();changed();frame_=command_=recording_=0;next_=firstCore;replace_=stopped_=false;fault_=Fault::None;return true;
    }
    void stop(Fault f) noexcept {if(stopped_)return;fault_=f;stopped_=true;++counts_.guards;}
    void begin(std::uint64_t frame,std::uint64_t command,std::uint64_t recording) noexcept {
        if(open_&&replace_){stop(Fault::IncompleteFrame);return;}
        frame_=frame;command_=command;recording_=recording;next_=firstCore;open_=true;
        // SR opt-in has exactly one core policy, including initialisation and
        // resource changes. SDK readiness never re-enables native FSR work.
        replace_=true;
        if(!frame||!command||!recording)stop(Fault::Sequence);
        ++counts_.replacementFrames;
    }
    bool core(unsigned profile,std::uint64_t frame,std::uint64_t command,std::uint64_t recording) noexcept {
        if(profile<firstCore||profile>lastCore)return false;
        if(!open_||profile!=next_||frame!=frame_||command!=command_||recording!=recording_){
            stop(Fault::Sequence);
            return true; // Never forward a recognised core after a failed transaction.
        }
        ++next_;
        ++counts_.skipped[profile-firstCore];
        return true;
    }
    void complete(bool xessSucceeded) noexcept {
        if(!open_||next_!=lastCore+1)stop(Fault::Sequence);
        if(replace_&&!xessSucceeded)stop(Fault::Terminal);
        open_=false;
    }
    bool use(Key image,bool discarded=false) noexcept {
        // Report the first fault only; never turn a refusal into native FSR permission.
        if((stopped_&&!discarded)||!image.handle||!image.generation)return false;
        for(auto& c:contents_)if(c.needed&&c.image==image){
            if(discarded){c.needed=false;changed();++counts_.discards;return false;}
            if(!c.omitted)return false;
            stop(Fault::Consumer);return true;
        }
        return false;
    }
    void produce(Key image,bool omitted=false,unsigned producer=0) noexcept {
        if(!image.handle||!image.generation){stop(Fault::Capacity);return;}
        changed();
        for(auto& c:contents_)if(c.image==image){
            if(!c.needed||!c.omitted||omitted)c.producer=producer;
            c.omitted=(c.needed&&c.omitted)||omitted;c.needed=true;return;
        }
        for(auto& c:contents_)if(!c.needed||c.image.handle==image.handle){c={image,true,omitted,producer};return;}
        try { contents_.push_back({image,true,omitted,producer}); }
        catch (...) { ++counts_.capacity;stop(Fault::Capacity); }
    }
    bool hasContents()const noexcept {for(const auto& c:contents_)if(c.needed)return true;return false;}
    unsigned missingContents()const noexcept {unsigned n{};for(const auto& c:contents_)n+=c.needed&&c.omitted;return n;}
    bool omitted(Key image)const noexcept {for(const auto& c:contents_)if(c.needed&&c.image==image)return c.omitted;return false;}
    unsigned producer(Key image)const noexcept {for(const auto& c:contents_)if(c.needed&&c.image==image)return c.producer;return 0;}
    void retire(Key image) noexcept {for(auto& c:contents_)if(c.image==image){c={};changed();}}
    bool watches(Key image)const noexcept {for(const auto& c:contents_)if(c.needed&&c.image==image)return true;return false;}
};
}
