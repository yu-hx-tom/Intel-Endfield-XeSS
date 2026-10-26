#include "dispatch_snapshot.hpp"
#include <algorithm>
#include <new>

namespace xess_vk::dispatch_snapshot {
namespace {
bool depth(VkFormat f) noexcept{return f==VK_FORMAT_D32_SFLOAT_S8_UINT;}
bool supported(VkFormat f) noexcept{return f==VK_FORMAT_R8G8B8A8_UNORM||f==VK_FORMAT_R16G16_SFLOAT||depth(f)||f==VK_FORMAT_B10G11R11_UFLOAT_PACK32;}
VkImageAspectFlags copiedAspect(VkFormat f) noexcept{return depth(f)?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;}
VkImageAspectFlags barrierAspects(VkFormat f) noexcept{return depth(f)?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT;}
VkImageLayout finalLayout(VkFormat f) noexcept{return depth(f)?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;}
void increment(std::uint64_t& v) noexcept{if(v!=UINT64_MAX)++v;}
Result failure(Error e,VkResult r=VK_SUCCESS) noexcept{return {e,r};}
Result driverResult(VkResult r) noexcept{return r==VK_SUCCESS?Result{}:failure(r==VK_ERROR_DEVICE_LOST?Error::DeviceLost:Error::DriverFailed,r);}
bool scope(VkPipelineStageFlags stages,VkAccessFlags access) noexcept {
    constexpr VkPipelineStageFlags allowed=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr VkAccessFlags known=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|
        VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if(!stages||!access||(stages&~allowed)||(access&~known))return false;
    if(stages&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)return true;
    if(access&(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT))return false;
    if((access&(VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT))&&!(stages&VK_PIPELINE_STAGE_TRANSFER_BIT))return false;
    if((access&(VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT))&&!(stages&(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)))return false;
    if((access&(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT))&&!(stages&(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)))return false;
    return !(access&(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT))||(stages&VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
}
struct Owned {VkImage image{};VkDeviceMemory memory{};VkImageLayout executedLayout{VK_IMAGE_LAYOUT_UNDEFINED};};
struct Slot {State state{State::Free};std::uint64_t generation{};CommandRecording recording{};VkFence captureFence{},consumerFence{};std::array<Owned,maxPlanes> owned{};};
VkImageMemoryBarrier barrier(VkImage image,VkFormat format,VkImageLayout oldLayout,VkImageLayout newLayout,VkAccessFlags src,VkAccessFlags dst) noexcept {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=src;b.dstAccessMask=dst;b.oldLayout=oldLayout;b.newLayout=newLayout;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={barrierAspects(format),0,1,0,1};return b;
}
}
struct Context {Config config{};std::array<Slot,slotCount> slots{};std::uint64_t serial{};Stats totals{};bool deviceLost{};};
namespace {
Slot* find(Context* c,Ticket t) noexcept{return c&&t.slot<slotCount&&t.generation&&c->slots[t.slot].generation==t.generation&&c->slots[t.slot].state!=State::Free?&c->slots[t.slot]:nullptr;}
void teardown(Context* c) noexcept {
    const auto& f=c->config.functions;for(auto& s:c->slots)for(auto& v:s.owned){
        if(v.image)f.destroyImage(c->config.device,v.image,nullptr);if(v.memory)f.freeMemory(c->config.device,v.memory,nullptr);v={};}
}
Result sourceIssue(const Context& c,const Source& s,std::size_t i) noexcept {
    const auto& d=s.successfulCreateInfo;const auto& p=c.config.planes[i];
    if(s.device!=c.config.device||!s.image||d.sType!=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO)return failure(Error::InvalidInput);
    if(d.pNext||d.flags||d.imageType!=VK_IMAGE_TYPE_2D||d.format!=p.format||!supported(d.format)||d.extent.width!=p.extent.width||d.extent.height!=p.extent.height||
        d.extent.depth!=1||d.mipLevels!=1||d.arrayLayers!=1||d.samples!=VK_SAMPLE_COUNT_1_BIT||d.tiling!=VK_IMAGE_TILING_OPTIMAL||
        d.sharingMode!=VK_SHARING_MODE_EXCLUSIVE||d.queueFamilyIndexCount||d.pQueueFamilyIndices||d.initialLayout!=VK_IMAGE_LAYOUT_UNDEFINED||!(d.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT))return failure(Error::UnsupportedImage);
    if(s.ownerQueueFamily!=c.config.queueFamily)return failure(Error::WrongQueue);
    if(s.range.aspectMask!=copiedAspect(d.format)||s.range.baseMipLevel||s.range.levelCount!=1||s.range.baseArrayLayer||s.range.layerCount!=1)return failure(Error::UnsupportedImage);
    if(s.currentLayout!=VK_IMAGE_LAYOUT_GENERAL&&s.currentLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&s.currentLayout!=finalLayout(d.format))return failure(Error::UnsupportedLayout);
    if(s.currentLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&!(d.usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)))return failure(Error::UnsupportedImage);
    if(s.currentLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL&&!(d.usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))return failure(Error::UnsupportedImage);
    if(!scope(s.precedingStages,s.precedingAccess)||!scope(s.resumedStages,s.resumedAccess))return failure(Error::UnsupportedScope);
    for(const auto& slot:c.slots)for(std::size_t j=0;j<c.config.planeCount;++j)if(slot.owned[j].image==s.image)return failure(Error::Alias);
    return {};
}
void clearRecording(Context& c,CommandRecording recording) noexcept {
    for(auto& s:c.slots)if(s.state==State::Recorded&&s.recording.command==recording.command&&s.recording.generation==recording.generation){
        s.state=State::Free;s.recording={};s.captureFence=s.consumerFence=VK_NULL_HANDLE;increment(c.totals.cancellations);}
}
}
const char* errorName(Error e) noexcept {
    constexpr const char* names[]{"none","invalid_input","unsupported_image","unsupported_layout","unsupported_scope","wrong_queue","alias","capacity",
        "missing_function","driver_failed","stale_ticket","wrong_state","pending","device_lost","generation_exhausted"};
    const auto i=static_cast<std::size_t>(e);return i<std::size(names)?names[i]:"unknown";
}
Context* create(const Config& config,Result* why) noexcept {
    auto fail=[&](Result r)->Context*{if(why)*why=r;return nullptr;};const auto& f=config.functions;
    if(!config.device||!config.planeCount||config.planeCount>maxPlanes||config.queueFamily==VK_QUEUE_FAMILY_IGNORED||
        (config.queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))!=(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)||
        !config.memory.memoryTypeCount||config.memory.memoryTypeCount>VK_MAX_MEMORY_TYPES)return fail(failure(Error::InvalidInput));
    if(!f.createImage||!f.destroyImage||!f.imageMemoryRequirements||!f.allocateMemory||!f.freeMemory||!f.bindImageMemory||!f.resetCommandBuffer||!f.fenceStatus)return fail(failure(Error::MissingFunction));
    for(std::size_t i=0;i<config.planeCount;++i)if(!supported(config.planes[i].format)||!config.planes[i].extent.width||!config.planes[i].extent.height)return fail(failure(Error::UnsupportedImage));
    auto* c=new(std::nothrow) Context;if(!c)return fail(failure(Error::Capacity));c->config=config;
    for(auto& slot:c->slots)for(std::size_t i=0;i<config.planeCount;++i){auto& own=slot.owned[i];const auto& p=config.planes[i];
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=p.format;image.extent={p.extent.width,p.extent.height,1};
        image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
        image.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VkImage created{};auto r=f.createImage(config.device,&image,nullptr,&created);
        if(r==VK_SUCCESS&&!created)r=VK_ERROR_INITIALIZATION_FAILED;
        if(r!=VK_SUCCESS){teardown(c);delete c;return fail(driverResult(r));}
        own.image=created;
        VkMemoryRequirements requirements{};f.imageMemoryRequirements(config.device,own.image,&requirements);std::uint32_t memoryType=UINT32_MAX;
        for(std::uint32_t n=0;n<config.memory.memoryTypeCount;++n)if((requirements.memoryTypeBits&(1u<<n))&&(config.memory.memoryTypes[n].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){memoryType=n;break;}
        if(!requirements.size||memoryType==UINT32_MAX){teardown(c);delete c;return fail(failure(Error::UnsupportedImage));}
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memoryType;
        VkDeviceMemory allocated{};r=f.allocateMemory(config.device,&allocation,nullptr,&allocated);if(r==VK_SUCCESS&&!allocated)r=VK_ERROR_INITIALIZATION_FAILED;
        if(r==VK_SUCCESS)own.memory=allocated;
        if(r==VK_SUCCESS)r=f.bindImageMemory(config.device,own.image,own.memory,0);
        if(r!=VK_SUCCESS){teardown(c);delete c;return fail(driverResult(r));}
    }
    if(why)*why={};return c;
}
Result destroy(Context* c) noexcept {
    if(!c)return {};for(const auto& s:c->slots)if(s.state!=State::Free)return failure(Error::Pending);
    teardown(c);delete c;return {};
}
Capture record(Context* c,CommandRecording command,std::size_t count,const Source* sources) noexcept {
    Capture out;auto refuse=[&](Error e,VkResult r=VK_SUCCESS){out.result=failure(e,r);if(c){increment(c->totals.refusals);if(e==Error::Capacity)increment(c->totals.capacityRefusals);}return out;};
    if(count>maxPlanes)return refuse(Error::Capacity);
    if(!c||!command.command||!command.generation||!sources||count!=c->config.planeCount)return refuse(Error::InvalidInput);
    if(c->deviceLost)return refuse(Error::DeviceLost,VK_ERROR_DEVICE_LOST);
    if(command.queueFamily!=c->config.queueFamily)return refuse(Error::WrongQueue);
    const auto& f=c->config.functions;if(!f.pipelineBarrier||!f.copyImage)return refuse(Error::MissingFunction);
    std::size_t available=slotCount;
    for(std::size_t i=0;i<slotCount;++i){const auto& slot=c->slots[i];if(slot.state==State::Free&&available==slotCount)available=i;
        if(slot.state!=State::Free&&slot.recording.command==command.command&&(slot.state!=State::Recorded||slot.recording.generation!=command.generation))return refuse(Error::WrongState);}
    if(available==slotCount)return refuse(Error::Capacity);
    if(c->serial==UINT64_MAX)return refuse(Error::GenerationExhausted);
    for(std::size_t i=0;i<count;++i){const auto result=sourceIssue(*c,sources[i],i);if(!result.accepted())return refuse(result.error,result.driver);
        for(std::size_t j=0;j<i;++j)if(sources[i].image==sources[j].image)return refuse(Error::Alias);
        if(!c->slots[available].owned[i].image||!c->slots[available].owned[i].memory)return refuse(Error::InvalidInput);}
    auto& slot=c->slots[available];slot.state=State::Recorded;slot.generation=++c->serial;slot.recording=command;slot.captureFence=slot.consumerFence=VK_NULL_HANDLE;
    out.ticket={static_cast<std::uint32_t>(available),slot.generation};out.count=count;
    for(std::size_t i=0;i<count;++i){const auto& s=sources[i];const auto& p=c->config.planes[i];auto& own=slot.owned[i];
        std::array<VkImageMemoryBarrier,2> before{
            barrier(s.image,p.format,s.currentLayout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.precedingAccess,VK_ACCESS_TRANSFER_READ_BIT),
            barrier(own.image,p.format,own.executedLayout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                own.executedLayout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT)};
        f.pipelineBarrier(command.command,s.precedingStages|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,before.data());
        VkImageCopy region{};region.srcSubresource=region.dstSubresource={copiedAspect(p.format),0,0,1};region.extent={p.extent.width,p.extent.height,1};
        f.copyImage(command.command,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,own.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        std::array<VkImageMemoryBarrier,2> after{
            barrier(s.image,p.format,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.currentLayout,VK_ACCESS_TRANSFER_READ_BIT,s.resumedAccess),
            barrier(own.image,p.format,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,finalLayout(p.format),VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT)};
        f.pipelineBarrier(command.command,VK_PIPELINE_STAGE_TRANSFER_BIT,s.resumedStages|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,2,after.data());
        out.targets[i]={own.image,p.format,p.extent,copiedAspect(p.format),finalLayout(p.format)};increment(c->totals.copies);
    }
    increment(c->totals.records);return out;
}
Result cancelUnsubmitted(Context* c,Ticket ticket) noexcept {
    auto* s=find(c,ticket);if(!s)return failure(Error::StaleTicket);if(s->state!=State::Recorded)return failure(Error::WrongState);
    for(const auto& other:c->slots)if(other.state!=State::Free&&other.recording.command==s->recording.command&&other.state!=State::Recorded)return failure(Error::WrongState);
    const auto result=c->config.functions.resetCommandBuffer(s->recording.command,0);if(result!=VK_SUCCESS)return driverResult(result);
    clearRecording(*c,s->recording);return {};
}
Result trackSubmission(Context* c,Ticket ticket,CommandRecording recording,VkFence fence,VkResult result) noexcept {
    auto* s=find(c,ticket);if(!s)return failure(Error::StaleTicket);if(s->state!=State::Recorded)return failure(Error::WrongState);
    if(!fence||recording.command!=s->recording.command||recording.generation!=s->recording.generation||recording.queueFamily!=c->config.queueFamily)return failure(Error::InvalidInput);
    for(auto& slot:c->slots)if(slot.state==State::Recorded&&slot.recording.command==recording.command&&slot.recording.generation==recording.generation){
        slot.state=result==VK_SUCCESS?State::Submitted:State::Uncertain;slot.captureFence=fence;}
    if(result==VK_ERROR_DEVICE_LOST)c->deviceLost=true;increment(c->totals.submissions);return driverResult(result);
}
Result pollCapture(Context* c,Ticket ticket) noexcept {
    auto* s=find(c,ticket);if(!s)return failure(Error::StaleTicket);
    if(s->state==State::Captured)return {};if(s->state==State::Uncertain)return failure(c->deviceLost?Error::DeviceLost:Error::WrongState);
    if(s->state!=State::Submitted)return failure(Error::WrongState);
    const auto result=c->config.functions.fenceStatus(c->config.device,s->captureFence);
    if(result==VK_NOT_READY||result==VK_TIMEOUT)return failure(Error::Pending,result);
    if(result!=VK_SUCCESS){if(result==VK_ERROR_DEVICE_LOST){c->deviceLost=true;s->state=State::Uncertain;}return driverResult(result);}
    s->state=State::Captured;for(std::size_t i=0;i<c->config.planeCount;++i)s->owned[i].executedLayout=finalLayout(c->config.planes[i].format);
    increment(c->totals.completions);return {};
}
Result releaseConsumed(Context* c,Ticket ticket,VkFence fence) noexcept {
    auto* s=find(c,ticket);if(!s)return failure(Error::StaleTicket);if(s->state!=State::Captured)return failure(Error::WrongState);if(!fence)return failure(Error::InvalidInput);
    if(s->consumerFence&&s->consumerFence!=fence)return failure(Error::InvalidInput);s->consumerFence=fence;
    const auto result=c->config.functions.fenceStatus(c->config.device,fence);if(result==VK_NOT_READY||result==VK_TIMEOUT)return failure(Error::Pending,result);
    if(result!=VK_SUCCESS){if(result==VK_ERROR_DEVICE_LOST){c->deviceLost=true;s->state=State::Uncertain;}return driverResult(result);}
    s->state=State::Free;s->recording={};s->captureFence=s->consumerFence=VK_NULL_HANDLE;increment(c->totals.consumed);return {};
}
Stats stats(const Context* c) noexcept {if(!c)return {};auto s=c->totals;for(std::size_t i=0;i<slotCount;++i)s.states[i]=c->slots[i].state;return s;}
} // namespace xess_vk::dispatch_snapshot
