#pragma once
#include "descriptor_image_metadata.hpp"
#include "frame_timing.hpp"
#include "resource_memory_observer.hpp"
#include "sr_tail_commands.hpp"
#include <array>
#include <span>

namespace xess_vk::sr_consumers {
using Key=descriptor_image_metadata::Key;
namespace stc=sr_tail_commands;
inline constexpr std::size_t outputLimit=64,hitLimit=32,frameLimit=8,commandLimit=frameLimit*4096;
inline constexpr std::size_t consumerShaderLimit=4;
enum class Kind {Compute,Graphics,Attachment,Transfer};
struct Image {Key key;resource_memory_observer::Observation memory;std::uint32_t setIndex{},binding{};VkDescriptorType descriptorType{VK_DESCRIPTOR_TYPE_MAX_ENUM};};
struct Write {stc::Key recording;FrameTimingAssociation frame;std::uint64_t sequence{},order{},site{};};
struct Output {
    Image image;stc::Key recording;FrameTimingAssociation frame;
    std::uint64_t sequence{},writes{},internalReferences{},externalReferences{},aliasReferences{},crossRecordingReferences{};
    std::uint64_t nativeCrossReferences{};
    Write latest;
    std::uint32_t producers{};
};
struct Hit {Key image,reference,pipeline;stc::Key recording;std::uint64_t sequence{},recordOrder{};std::uint32_t setIndex{},binding{};Kind kind{};bool alias{},crossRecording{},knownNativeConsumer{};Write producer;VkDescriptorType descriptorType{VK_DESCRIPTOR_TYPE_MAX_ENUM};};
struct ProducerFrame {FrameTimingAssociation identity;stc::Key recording;bool ended{},multipleRecordings{};};
struct Graphics {stc::Key recording;std::array<std::uint64_t,descriptor_image_metadata::maxBoundSets> sets{};std::uint64_t layout{},pipeline{};bool beginObserved{},bindingsKnown{};};
struct OpaqueCommand {std::array<char,64> name{};stc::Key firstRecording;std::uint64_t firstOrder{},count{};};
struct Stats {
    std::uint64_t commands{},references{},unknownCommands{},unknownMemory{},invalidIdentities{},outputDrops{},hitDrops{},commandDrops{},graphicsDrops{},frameDrops{},endedRecordings{};
    std::uint64_t graphicsDrawsKnown{},graphicsDrawsUnknown{};
    std::uint64_t opaqueNameDrops{};
    std::uint64_t preTargetReferenceSites{},stateCommands{},incompleteStateCommands{};
    std::uint64_t shaderRequests{},shaderRequestDrops{};
};
// Potential references, not shader reads. Observe only; unknown coverage never
// grants replacement. Samples are bounded, totals survive sample exhaustion.
class Audit {
    std::array<Output,outputLimit> outputs_{};std::size_t count_{};
    std::array<Hit,hitLimit> hits_{};std::size_t hitCount_{};
    std::array<ProducerFrame,frameLimit> frames_{};std::size_t frameCount_{};
    Write boundary_;bool closed_{};
    std::array<Key,consumerShaderLimit> shaders_{};std::size_t shaderCount_{};
    std::array<Graphics,16> graphics_{};
    std::array<OpaqueCommand,16> opaque_{};std::size_t opaqueCount_{};
    std::uint64_t sequence_{};Stats stats_;
    static bool memoryKnown(const Image& i) noexcept {
        const auto& m=i.memory;const auto& b=m.binding;
        constexpr auto overlap=resource_memory_facts::bit(resource_memory_facts::Issue::Overlap);
        return m.queried&&!(m.localIssues&~overlap)&&!m.globalIssues&&!(b.issues&~overlap)&&b.bindingBoundsKnown&&
            b.resource.kind==resource_memory_facts::ResourceKind::Image&&b.resource.key.handle==i.key.handle&&b.resource.key.generation==i.key.generation&&
            b.memory.handle&&b.memory.generation&&b.end>b.offset&&b.size==b.end-b.offset&&b.paddedAliasRangeKnown&&
            b.paddedAliasBegin<=b.offset&&b.paddedAliasEnd>=b.end;
    }
    static bool aliases(const Image& a,const Image& b) noexcept {
        if(!memoryKnown(a)||!memoryKnown(b))return false;
        const auto& x=a.memory.binding;const auto& y=b.memory.binding;
        if(x.memory!=y.memory)return false;
        const auto begin=x.linearResource==y.linearResource?x.offset:x.paddedAliasBegin;
        const auto end=x.linearResource==y.linearResource?x.end:x.paddedAliasEnd;
        const auto otherBegin=x.linearResource==y.linearResource?y.offset:y.paddedAliasBegin;
        const auto otherEnd=x.linearResource==y.linearResource?y.end:y.paddedAliasEnd;
        return begin<otherEnd&&otherBegin<end;
    }
public:
    bool producerFrame(const stc::Key& recording,const FrameTimingAssociation& frame,std::uint64_t order=0) noexcept {
        if(closed_)return false;
        if(!recording.device||!recording.incarnation||!recording.command||!recording.recording||!frame.valid||!frame.ticket){++stats_.invalidIdentities;return false;}
        for(auto& f:std::span(frames_).first(frameCount_))if(f.identity.engineFrame==frame.engineFrame&&f.identity.ticket==frame.ticket){
            if(f.recording!=recording&&!f.multipleRecordings){f.multipleRecordings=true;unknown();}return true;
        }
        // Keep eight producer frames and the prefix before the next producer.
        // This is a finite CPU observation boundary, never future/GPU proof.
        if(frameCount_==frameLimit){++stats_.frameDrops;boundary_={recording,frame,sequence_,order,0};closed_=true;return false;}
        frames_[frameCount_++]={frame,recording};return true;
    }
    void begin(const stc::Key& recording) noexcept {
        if(closed_)return;
        for(auto& g:graphics_)if(g.recording.device==recording.device&&g.recording.incarnation==recording.incarnation&&g.recording.command==recording.command)g={};
        for(auto& g:graphics_)if(!g.recording.command){g.recording=recording;g.beginObserved=g.bindingsKnown=true;return;}
        ++stats_.graphicsDrops;
    }
    bool command(const stc::Key& key,bool complete=true,bool canProduce=true) noexcept {
        if(closed_)return false;
        ++sequence_;
        if(!count_&&!canProduce){++stats_.preTargetReferenceSites;return false;}
        if(stats_.commands==commandLimit){++stats_.commandDrops;return false;}++stats_.commands;
        if(!key.device||!key.incarnation||!key.command||!key.recording){++stats_.invalidIdentities;return false;}
        if(!complete)++stats_.unknownCommands;return true;
    }
    bool state(const stc::Key& key,bool complete=true) noexcept {
        if(closed_)return false;
        ++sequence_;++stats_.stateCommands;stats_.incompleteStateCommands+=!complete;
        if(!key.device||!key.incarnation||!key.command||!key.recording){++stats_.invalidIdentities;return false;}
        return true;
    }
    void unknown() noexcept {++stats_.unknownCommands;}
    void noteOpaque(const stc::Key& recording,std::span<const char> name,std::uint64_t order) noexcept {
        std::array<char,64> copied{};
        for(std::size_t n=0;n<name.size()&&n<copied.size()-1&&name[n];++n){const auto c=name[n];
            copied[n]=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'?c:'_';}
        for(auto& entry:std::span(opaque_).first(opaqueCount_))if(entry.name==copied){++entry.count;return;}
        if(opaqueCount_==opaque_.size()){++stats_.opaqueNameDrops;return;}
        opaque_[opaqueCount_++]={copied,recording,order,1};
    }
    bool use(const stc::Key& recording,Kind kind,std::span<const Image> images,bool internal=false,Key pipeline={},std::uint64_t recordOrder=0) noexcept {
        if(closed_)return false;bool matched=false;
        for(const auto& reference:images){++stats_.references;
            if(!reference.key.handle||!reference.key.generation){++stats_.invalidIdentities;continue;}
            if(!memoryKnown(reference))++stats_.unknownMemory;
            for(auto& output:std::span(outputs_).first(count_)){
                const bool exact=output.image.key==reference.key;
                const bool alias=!exact&&aliases(output.image,reference);
                if(!exact&&!alias)continue;
                matched=true;const bool cross=output.latest.recording!=recording;
                if(internal&&!cross&&!alias){++output.internalReferences;continue;}
                ++output.externalReferences;output.aliasReferences+=alias;output.crossRecordingReferences+=cross;
                output.nativeCrossReferences+=internal&&cross;
                if(hitCount_==hitLimit){++stats_.hitDrops;continue;}
                hits_[hitCount_++]={output.image.key,reference.key,pipeline,recording,sequence_,recordOrder,reference.setIndex,reference.binding,kind,alias,cross,internal,output.latest,reference.descriptorType};
            }
        }return matched;
    }
    bool requestShader(Key pipeline) noexcept {
        if(closed_||!pipeline.handle||!pipeline.generation)return false;
        for(const auto& p:std::span(shaders_).first(shaderCount_))if(p==pipeline)return false;
        if(shaderCount_==consumerShaderLimit){++stats_.shaderRequestDrops;return false;}
        shaders_[shaderCount_++]=pipeline;++stats_.shaderRequests;return true;
    }
    void produce(const stc::Key& recording,const FrameTimingAssociation& frame,Image image,std::size_t profile,std::uint64_t order=0) noexcept {
        if(!recording.device||!recording.incarnation||!recording.command||!recording.recording||
           !frame.valid||!frame.ticket||!image.key.handle||!image.key.generation||profile>=32){++stats_.invalidIdentities;return;}
        if(!producerFrame(recording,frame,order))return;
        for(auto& previous:std::span(outputs_).first(count_))if(previous.image.key==image.key){
            previous.producers|=1u<<profile;++previous.writes;
            const auto& w=previous.latest;
            const bool same=w.recording==recording&&w.sequence==sequence_&&w.order==order&&w.frame.engineFrame==frame.engineFrame&&w.frame.ticket==frame.ticket;
            previous.latest={recording,frame,sequence_,order,w.site+!same};previous.image.memory=image.memory;return;
        }
        if(count_==outputLimit){++stats_.outputDrops;return;}
        if(!memoryKnown(image))++stats_.unknownMemory;
        auto& out=outputs_[count_++];out.image=image;out.recording=recording;out.frame=frame;
        out.sequence=sequence_;out.writes=1;out.producers=1u<<profile;out.latest={recording,frame,sequence_,order,1};
    }
    void bindGraphics(const stc::Key& recording,const stc::DescriptorSets& sets,bool complete) noexcept {
        if(sets.bindPoint!=VK_PIPELINE_BIND_POINT_GRAPHICS)return;
        auto* target=graphics(recording);
        if(!target)for(auto& g:graphics_)if(!g.recording.command){g.recording=recording;target=&g;break;}
        if(!target){++stats_.graphicsDrops;return;}
        // Enumerate all known bound sets, including Vulkan-disturbed sets.
        // This superset needs no graphics shader/layout reflection.
        if(!complete||sets.firstSet>target->sets.size()||sets.storedSets>target->sets.size()-sets.firstSet){target->bindingsKnown=false;unknown();return;}
        target->layout=sets.layout;
        for(std::size_t n=0;n<sets.storedSets;++n)target->sets[sets.firstSet+n]=sets.sets[n];
    }
    Graphics* graphics(const stc::Key& recording) noexcept {for(auto& g:graphics_)if(g.recording==recording)return &g;return nullptr;}
    void graphicsPipeline(const stc::Key& recording,std::uint64_t pipeline) noexcept {
        auto* target=graphics(recording);
        if(!target)for(auto& g:graphics_)if(!g.recording.command){g.recording=recording;target=&g;break;}
        if(target)target->pipeline=pipeline;else ++stats_.graphicsDrops;
    }
    void graphicsDraw(const stc::Key& recording) noexcept {
        const auto* g=graphics(recording);
        if(g&&g->beginObserved&&g->bindingsKnown)++stats_.graphicsDrawsKnown;
        else {++stats_.graphicsDrawsUnknown;unknown();}
    }
    void opaqueGraphics(const stc::Key& recording) noexcept {if(auto* g=graphics(recording))g->bindingsKnown=false;unknown();}
    void end(const stc::Key& recording,bool success) noexcept {
        bool found=false;for(auto& f:std::span(frames_).first(frameCount_))if(f.recording==recording&&!f.ended){found=true;f.ended=success;}
        if(found){if(success)++stats_.endedRecordings;else unknown();}
        for(auto& g:graphics_)if(g.recording==recording)g={};
    }
    std::span<const Output> outputs() const noexcept {return std::span(outputs_).first(count_);}
    std::span<const Hit> hits() const noexcept {return std::span(hits_).first(hitCount_);}
    std::span<const OpaqueCommand> opaqueCommands() const noexcept {return std::span(opaque_).first(opaqueCount_);}
    bool closed() const noexcept {return closed_;}
    const Write& boundary() const noexcept {return boundary_;}
    std::span<const ProducerFrame> producerFrames() const noexcept {return std::span(frames_).first(frameCount_);}
    Stats stats() const noexcept {return stats_;}
};
}
