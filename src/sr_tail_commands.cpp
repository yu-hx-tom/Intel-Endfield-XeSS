#include "sr_tail_commands.hpp"
#include <algorithm>
#include <cstring>

namespace xess_vk::sr_tail_commands {
namespace {
template<class T> std::uint64_t bits(T value) noexcept {return reinterpret_cast<std::uintptr_t>(value);}
template<class T,std::size_t N> std::uint32_t prefix(Command& out,std::uint32_t count,const T* data,const std::array<T,N>&) noexcept {
    if(count&&!data){out.missingData=true;out.truncated=true;return 0;}
    if(count>N)out.truncated=true;
    return static_cast<std::uint32_t>(std::min<std::size_t>(count,N));
}
std::uint32_t bounded(Command& out,std::uint32_t count,const void* data,std::size_t capacity) noexcept {
    if(count&&!data){out.missingData=true;out.truncated=true;return 0;}
    if(count>capacity)out.truncated=true;
    return static_cast<std::uint32_t>(std::min<std::size_t>(count,capacity));
}
bool valid(const Key& value) noexcept {
    return value.device&&value.incarnation&&value.command&&value.recording&&value.pool&&value.poolGeneration;
}
Attachment attachment(Command& out,const VkRenderingAttachmentInfo& value) noexcept {
    out.extensionsOmitted|=value.pNext!=nullptr;
    return {bits(value.imageView),bits(value.resolveImageView),value.imageLayout,value.resolveImageLayout,
        value.resolveMode,value.loadOp,value.storeOp,value.clearValue};
}
}
const char* name(Kind value) noexcept {
    switch(value){
#define TAIL_KIND(x) case Kind::x:return #x
        TAIL_KIND(Other);TAIL_KIND(Barrier);TAIL_KIND(BindPipeline);TAIL_KIND(BindDescriptorSets);TAIL_KIND(PushConstants);
        TAIL_KIND(BeginRenderPass);TAIL_KIND(BeginRendering);TAIL_KIND(Dispatch);TAIL_KIND(DispatchIndirect);TAIL_KIND(DispatchBase);
        TAIL_KIND(Draw);TAIL_KIND(DrawIndexed);TAIL_KIND(DrawIndirect);TAIL_KIND(DrawIndexedIndirect);
        TAIL_KIND(DrawIndirectCount);TAIL_KIND(DrawIndexedIndirectCount);
#undef TAIL_KIND
    }return "Unknown";
}
const char* name(Pass value) noexcept {return value==Pass::Accumulate?"accumulate":value==Pass::Rcas?"rcas":"unknown";}
Key key(const CommandRecordingFacts& value) noexcept {
    return {value.device,value.contextIncarnation,value.command,value.rpRecordingGeneration,value.pool,value.poolGeneration};
}
Command simple(Kind kind,std::span<const std::uint64_t> arguments,Api api) noexcept {
    Command out;out.kind=kind;out.api=api;
    out.argumentCount=static_cast<std::uint32_t>(std::min<std::size_t>(arguments.size(),UINT32_MAX));
    out.storedArguments=static_cast<std::uint32_t>(std::min(arguments.size(),out.arguments.size()));
    out.truncated=arguments.size()>out.arguments.size();
    std::copy_n(arguments.begin(),out.storedArguments,out.arguments.begin());return out;
}
Command other(std::string_view api,std::span<const std::uint64_t> arguments) noexcept {
    auto out=simple(Kind::Other,arguments);
    const auto length=std::min(api.size(),out.otherName.size()-1);
    std::copy_n(api.begin(),length,out.otherName.begin());out.truncated|=length<api.size();return out;
}
Command barrier(VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
    std::uint32_t memoryCount,const VkMemoryBarrier* memory,std::uint32_t bufferCount,const VkBufferMemoryBarrier* buffers,
    std::uint32_t imageCount,const VkImageMemoryBarrier* images) noexcept {
    Command out;out.kind=Kind::Barrier;auto& b=out.barriers;b.srcStage=src;b.dstStage=dst;b.dependencyFlags=flags;
    b.memoryCount=memoryCount;b.bufferCount=bufferCount;b.imageCount=imageCount;
    b.storedMemory=bounded(out,memoryCount,memory,b.memory.size());b.storedBuffers=bounded(out,bufferCount,buffers,b.buffers.size());
    b.storedImages=bounded(out,imageCount,images,b.images.size());
    for(unsigned i=0;i<b.storedMemory;++i){const auto& value=memory[i];out.extensionsOmitted|=value.pNext!=nullptr;
        b.memory[i]={src,dst,value.srcAccessMask,value.dstAccessMask};}
    for(unsigned i=0;i<b.storedBuffers;++i){const auto& value=buffers[i];out.extensionsOmitted|=value.pNext!=nullptr;
        b.buffers[i]={{src,dst,value.srcAccessMask,value.dstAccessMask},bits(value.buffer),value.offset,value.size,value.srcQueueFamilyIndex,value.dstQueueFamilyIndex};}
    for(unsigned i=0;i<b.storedImages;++i){const auto& value=images[i];out.extensionsOmitted|=value.pNext!=nullptr;
        b.images[i]={{src,dst,value.srcAccessMask,value.dstAccessMask},bits(value.image),value.oldLayout,value.newLayout,value.srcQueueFamilyIndex,value.dstQueueFamilyIndex,value.subresourceRange};}
    return out;
}
Command barrier2(const VkDependencyInfo* info,Api api) noexcept {
    Command out;out.kind=Kind::Barrier;out.api=api;if(!info){out.missingData=true;return out;}
    out.extensionsOmitted=info->pNext!=nullptr;auto& b=out.barriers;b.dependencyFlags=info->dependencyFlags;
    b.memoryCount=info->memoryBarrierCount;b.bufferCount=info->bufferMemoryBarrierCount;b.imageCount=info->imageMemoryBarrierCount;
    b.storedMemory=bounded(out,b.memoryCount,info->pMemoryBarriers,b.memory.size());
    b.storedBuffers=bounded(out,b.bufferCount,info->pBufferMemoryBarriers,b.buffers.size());
    b.storedImages=bounded(out,b.imageCount,info->pImageMemoryBarriers,b.images.size());
    for(unsigned i=0;i<b.storedMemory;++i){const auto& value=info->pMemoryBarriers[i];out.extensionsOmitted|=value.pNext!=nullptr;
        b.memory[i]={value.srcStageMask,value.dstStageMask,value.srcAccessMask,value.dstAccessMask};}
    for(unsigned i=0;i<b.storedBuffers;++i){const auto& value=info->pBufferMemoryBarriers[i];out.extensionsOmitted|=value.pNext!=nullptr;
        b.buffers[i]={{value.srcStageMask,value.dstStageMask,value.srcAccessMask,value.dstAccessMask},bits(value.buffer),value.offset,value.size,value.srcQueueFamilyIndex,value.dstQueueFamilyIndex};}
    for(unsigned i=0;i<b.storedImages;++i){const auto& value=info->pImageMemoryBarriers[i];out.extensionsOmitted|=value.pNext!=nullptr;
        b.images[i]={{value.srcStageMask,value.dstStageMask,value.srcAccessMask,value.dstAccessMask},bits(value.image),value.oldLayout,value.newLayout,value.srcQueueFamilyIndex,value.dstQueueFamilyIndex,value.subresourceRange};}
    return out;
}
Command bindDescriptorSets(VkPipelineBindPoint point,VkPipelineLayout layout,std::uint32_t first,std::uint32_t count,
    const VkDescriptorSet* sets,std::uint32_t offsetCount,const std::uint32_t* offsets) noexcept {
    Command out;out.kind=Kind::BindDescriptorSets;auto& d=out.descriptors;d.bindPoint=point;d.layout=bits(layout);d.firstSet=first;
    d.setCount=count;d.dynamicOffsetCount=offsetCount;d.storedSets=bounded(out,count,sets,d.sets.size());
    d.storedOffsets=prefix(out,offsetCount,offsets,d.offsets);
    for(unsigned i=0;i<d.storedSets;++i)d.sets[i]=bits(sets[i]);
    if(d.storedOffsets)std::copy_n(offsets,d.storedOffsets,d.offsets.begin());return out;
}
Command bindDescriptorSets2(const VkBindDescriptorSetsInfo* info,Api api) noexcept {
    if(!info){auto out=simple(Kind::BindDescriptorSets,{},api);out.missingData=true;return out;}
    auto out=bindDescriptorSets(VK_PIPELINE_BIND_POINT_MAX_ENUM,info->layout,info->firstSet,info->descriptorSetCount,
        info->pDescriptorSets,info->dynamicOffsetCount,info->pDynamicOffsets);
    out.api=api;out.descriptors.stages=info->stageFlags;out.extensionsOmitted=info->pNext!=nullptr;return out;
}
Command pushConstants(VkPipelineLayout layout,VkShaderStageFlags stages,std::uint32_t offset,std::uint32_t size,const void* data) noexcept {
    Command out;out.kind=Kind::PushConstants;auto& p=out.push;p.layout=bits(layout);p.stages=stages;p.offset=offset;p.size=size;
    p.storedBytes=bounded(out,size,data,p.bytes.size());if(p.storedBytes)std::memcpy(p.bytes.data(),data,p.storedBytes);return out;
}
Command pushConstants2(const VkPushConstantsInfo* info,Api api) noexcept {
    if(!info){auto out=simple(Kind::PushConstants,{},api);out.missingData=true;return out;}
    auto out=pushConstants(info->layout,info->stageFlags,info->offset,info->size,info->pValues);
    out.api=api;out.extensionsOmitted=info->pNext!=nullptr;return out;
}
Command beginRenderPass(const VkRenderPassBeginInfo* info,VkSubpassContents contents,Api api) noexcept {
    auto out=simple(Kind::BeginRenderPass,{},api);if(!info){out.missingData=true;return out;}
    auto& r=out.renderPass;r.renderPass=bits(info->renderPass);r.framebuffer=bits(info->framebuffer);r.area=info->renderArea;r.contents=contents;
    r.clearCount=info->clearValueCount;r.storedClears=prefix(out,r.clearCount,info->pClearValues,r.clears);
    if(r.storedClears)std::copy_n(info->pClearValues,r.storedClears,r.clears.begin());out.extensionsOmitted=info->pNext!=nullptr;return out;
}
Command beginRenderPass2(const VkRenderPassBeginInfo* info,const VkSubpassBeginInfo* subpass,Api api) noexcept {
    auto out=beginRenderPass(info,subpass?subpass->contents:VK_SUBPASS_CONTENTS_MAX_ENUM,api);
    if(!subpass)out.missingData=true;else out.extensionsOmitted|=subpass->pNext!=nullptr;return out;
}
Command beginRendering(const VkRenderingInfo* info,Api api) noexcept {
    auto out=simple(Kind::BeginRendering,{},api);if(!info){out.missingData=true;return out;}
    auto& r=out.rendering;r.flags=info->flags;r.area=info->renderArea;r.layerCount=info->layerCount;r.viewMask=info->viewMask;
    r.colorCount=info->colorAttachmentCount;r.storedColors=bounded(out,r.colorCount,info->pColorAttachments,r.colors.size());
    out.extensionsOmitted=info->pNext!=nullptr;
    for(unsigned i=0;i<r.storedColors;++i)r.colors[i]=attachment(out,info->pColorAttachments[i]);
    if(info->pDepthAttachment){r.hasDepth=true;r.depth=attachment(out,*info->pDepthAttachment);}
    if(info->pStencilAttachment){r.hasStencil=true;r.stencil=attachment(out,*info->pStencilAttachment);}return out;
}
bool Observer::tail(const Tail& value,Origin origin) noexcept {
    if(origin==Origin::Layer){++stats_.ignoredLayer;return false;}
    if(!window_||!valid(value.recording)||!value.dispatchOrdinal||(value.pass!=Pass::Accumulate&&value.pass!=Pass::Rcas)){
        ++stats_.invalidInputs;return false;
    }
    Slot* available{};
    for(auto& slot:slots_){
        if(!slot.live){if(!available)available=&slot;continue;}
        const auto& existing=slot.trace.tail.recording;const auto& incoming=value.recording;
        if(existing!=incoming){
            if(existing.device==incoming.device&&existing.incarnation==incoming.incarnation&&existing.command==incoming.command&&existing.recording==incoming.recording){
                ++stats_.invalidInputs;return false;
            }
            continue;
        }
        const auto& old=slot.trace.tail;
        if(value.dispatchOrdinal==old.dispatchOrdinal&&value.pass==old.pass){++stats_.duplicates;return true;}
        if(value.dispatchOrdinal<=old.dispatchOrdinal){++stats_.invalidInputs;return false;}
        const auto replacements=slot.trace.replacements+1;
        slot.trace={};slot.trace.window=window_;slot.trace.replacements=replacements;slot.trace.tail=value;
        ++stats_.tails;++stats_.replacements;return true;
    }
    if(!available){++stats_.capacityRefusals;return false;}
    available->live=true;available->trace={};available->trace.window=window_;available->trace.tail=value;++stats_.tails;return true;
}
bool Observer::wantsCommand(const Key& value) const noexcept {
    for(const auto& slot:slots_)if(slot.live&&slot.trace.tail.recording==value)return slot.trace.count<commandCapacity;
    return false;
}
void Observer::command(const Key& value,const Command& command,Origin origin) noexcept {
    if(origin==Origin::Layer){++stats_.ignoredLayer;return;}
    for(auto& slot:slots_)if(slot.live&&slot.trace.tail.recording==value){
        ++stats_.commands;++slot.trace.commandsSeen;
        if(slot.trace.count<commandCapacity)slot.trace.commands[slot.trace.count++]=command;else ++stats_.omittedAfterEight;
        return;
    }
}
bool Observer::finish(const Key& value,Trace& out) noexcept {
    for(auto& slot:slots_)if(slot.live&&slot.trace.tail.recording==value){
        out=slot.trace;out.end=End::RecordingEnded;slot.live=false;++stats_.finished;return true;
    }
    return false;
}
void Observer::retire(const Key& value) noexcept {
    for(auto& slot:slots_)if(slot.live&&slot.trace.tail.recording==value){slot.live=false;++stats_.retired;}
}
void Observer::retirePool(std::uint64_t pool,std::uint64_t generation) noexcept {
    for(auto& slot:slots_)if(slot.live&&slot.trace.tail.recording.pool==pool&&(!generation||slot.trace.tail.recording.poolGeneration==generation)){
        slot.live=false;++stats_.retired;
    }
}
bool Observer::drain(Trace& out) noexcept {
    for(auto& slot:slots_)if(slot.live){out=slot.trace;out.end=End::WindowClosed;slot.live=false;++stats_.finished;return true;}
    return false;
}
}
