#include "dispatch_snapshot.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace {std::atomic<bool> measured{};std::atomic<std::size_t> allocations{};}
void* operator new(std::size_t n){if(measured)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
namespace ds=xess_vk::dispatch_snapshot;
unsigned checks{},cases{},retainedOwners{};
void require(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
void ok(ds::Result r){require(r.accepted(),"snapshot result refused");}
void error(ds::Result r,ds::Error e){require(r.error==e,"wrong snapshot refusal");}
template<class T>T h(std::uint64_t n){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(n);else return static_cast<T>(n);}
struct Trace {unsigned commands{},creates{},destroys{},allocates{},frees{},resets{};std::uint64_t imageSerial{1000},memorySerial{5000};
    VkResult resetResult{VK_SUCCESS},captureStatus{VK_SUCCESS},consumerStatus{VK_SUCCESS},createResult{VK_SUCCESS};
    std::array<std::array<VkImageMemoryBarrier,2>,8> barriers{};std::array<VkImageCopy,4> copies{};unsigned barrierCount{},copyCount{};
    VkCommandBuffer lastReset{};} trace;
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice,const VkImageCreateInfo* i,const VkAllocationCallbacks*,VkImage* image){
    require(i->usage==(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT),"targets have independent copy/read usage");
    ++trace.creates;if(trace.createResult!=VK_SUCCESS)return trace.createResult;*image=h<VkImage>(++trace.imageSerial);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice,VkImage,const VkAllocationCallbacks*){++trace.destroys;}
VKAPI_ATTR void VKAPI_CALL requirements(VkDevice,VkImage,VkMemoryRequirements* r){*r={4096,256,1};}
VKAPI_ATTR VkResult VKAPI_CALL allocateMemory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory* memory){++trace.allocates;*memory=h<VkDeviceMemory>(++trace.memorySerial);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*){++trace.frees;}
VKAPI_ATTR VkResult VKAPI_CALL bindMemory(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize){return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL pipelineBarrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,std::uint32_t,const VkMemoryBarrier*,std::uint32_t,const VkBufferMemoryBarrier*,std::uint32_t n,const VkImageMemoryBarrier* b){
    ++trace.commands;if(trace.barrierCount<trace.barriers.size()){require(n==2,"source and target barriers paired");std::memcpy(trace.barriers[trace.barrierCount++].data(),b,2*sizeof(*b));}}
VKAPI_ATTR void VKAPI_CALL copyImage(VkCommandBuffer,VkImage,VkImageLayout,VkImage,VkImageLayout,std::uint32_t n,const VkImageCopy* region){
    ++trace.commands;if(trace.copyCount<trace.copies.size()){require(n==1,"one exact image region");trace.copies[trace.copyCount++]=*region;}}
VKAPI_ATTR VkResult VKAPI_CALL resetCommand(VkCommandBuffer command,VkCommandBufferResetFlags){++trace.resets;trace.lastReset=command;return trace.resetResult;}
VKAPI_ATTR VkResult VKAPI_CALL fenceStatus(VkDevice,VkFence fence){return fence==h<VkFence>(1)?trace.captureStatus:trace.consumerStatus;}
struct Fixture {
    ds::Config config{};std::array<ds::Source,4> sources{};ds::Context* context{};
    ds::CommandRecording command{h<VkCommandBuffer>(30),1,3};
    explicit Fixture(bool missingCopy=false){trace={};config.device=h<VkDevice>(1);config.queueFamily=3;config.queueFlags=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;
        config.memory.memoryTypeCount=1;config.memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        config.functions={createImage,destroyImage,requirements,allocateMemory,freeMemory,bindMemory,pipelineBarrier,missingCopy?nullptr:copyImage,resetCommand,fenceStatus};
        config.planeCount=4;config.planes={ds::PlaneSpec{VK_FORMAT_R8G8B8A8_UNORM,{63,37}},ds::PlaneSpec{VK_FORMAT_R16G16_SFLOAT,{31,19}},
            ds::PlaneSpec{VK_FORMAT_D32_SFLOAT_S8_UINT,{31,19}},ds::PlaneSpec{VK_FORMAT_B10G11R11_UFLOAT_PACK32,{31,19}}};
        for(std::size_t n=0;n<sources.size();++n){auto& s=sources[n];const auto& p=config.planes[n];s.device=config.device;s.image=h<VkImage>(100+n);
            auto& i=s.successfulCreateInfo;i.imageType=VK_IMAGE_TYPE_2D;i.format=p.format;i.extent={p.extent.width,p.extent.height,1};i.mipLevels=i.arrayLayers=1;
            i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;i.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            s.currentLayout=n==2?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;s.ownerQueueFamily=3;
            s.range={n==2?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};s.precedingStages=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            s.precedingAccess=VK_ACCESS_MEMORY_WRITE_BIT;s.resumedStages=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;s.resumedAccess=VK_ACCESS_SHADER_READ_BIT;}
        ds::Result why;context=ds::create(config,&why);require(context&&why.accepted(),"fixed target creation");}
    ~Fixture(){if(!ds::destroy(context).accepted())++retainedOwners;}
    ds::Capture record(){return ds::record(context,command,sources.size(),sources.data());}
};
void transitions(){++cases;Fixture f;const auto capture=f.record();ok(capture.result);require(capture.count==4&&trace.commands==12,"one source/target pair and copy per independently sized plane");
    require(trace.creates==8&&trace.allocates==8,"two preallocated slots with independent image memory");
    for(std::size_t i=0;i<4;++i){const auto& before=trace.barriers[i*2];const auto& after=trace.barriers[i*2+1];
        require(before[0].oldLayout==f.sources[i].currentLayout&&before[0].newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&before[1].oldLayout==VK_IMAGE_LAYOUT_UNDEFINED,"source layout known and fresh target initially undefined");
        require(after[0].newLayout==f.sources[i].currentLayout&&after[0].srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&after[0].dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED,"source exact original layout and ownership restored");
        require(after[0].dstAccessMask==f.sources[i].resumedAccess&&capture.targets[i].extent.width==f.config.planes[i].extent.width,"resumed access and per-plane extent preserved");
        require(trace.copies[i].extent.width==f.config.planes[i].extent.width&&trace.copies[i].extent.height==f.config.planes[i].extent.height,"copy spans each independent full extent");}
    require(trace.barriers[4][0].subresourceRange.aspectMask==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT)&&trace.copies[2].srcSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT,"D32S8 barriers cover both aspects while copy copies depth only");
    error(ds::destroy(f.context),ds::Error::Pending);ok(ds::cancelUnsubmitted(f.context,capture.ticket));require(trace.lastReset==f.command.command,"cancellation executes actual reset of exact never-submitted CB");
    error(ds::pollCapture(f.context,capture.ticket),ds::Error::StaleTicket);
}
void preflight(){++cases;
    for(unsigned mode=0;mode<16;++mode){Fixture f;auto& s=f.sources[mode==15?2:3];auto& i=s.successfulCreateInfo;ds::Error expected=ds::Error::UnsupportedImage;VkBaseInStructure extension{};
        if(mode==0)i.usage&=~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;if(mode==1){s.currentLayout=VK_IMAGE_LAYOUT_UNDEFINED;expected=ds::Error::UnsupportedLayout;}
        if(mode==2)i.format=VK_FORMAT_R32_SFLOAT;if(mode==3)s.range.baseMipLevel=1;if(mode==4){s.ownerQueueFamily=9;expected=ds::Error::WrongQueue;}
        if(mode==5){s.image=f.sources[0].image;expected=ds::Error::Alias;}if(mode==6)i.pNext=&extension;if(mode==7)i.flags=VK_IMAGE_CREATE_ALIAS_BIT;
        if(mode==8)i.samples=VK_SAMPLE_COUNT_4_BIT;if(mode==9)i.mipLevels=2;if(mode==10)i.extent.height=18;
        if(mode==11){s.precedingStages=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;s.precedingAccess=VK_ACCESS_TRANSFER_WRITE_BIT;expected=ds::Error::UnsupportedScope;}
        if(mode==12){s.resumedStages=0;expected=ds::Error::UnsupportedScope;}if(mode==13){f.command.queueFamily=4;expected=ds::Error::WrongQueue;}
        if(mode==14||mode==15)i.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const auto before=trace.commands;error(f.record().result,expected);require(trace.commands==before&&ds::stats(f.context).records==0,"last-plane failure emits zero partial commands and reserves no slot");}
    Fixture missing(true);error(missing.record().result,ds::Error::MissingFunction);require(trace.commands==0,"missing copy entrypoint issues no barrier");
    auto invalid=missing.config;invalid.functions.destroyImage=nullptr;ds::Result why;require(!ds::create(invalid,&why)&&why.error==ds::Error::MissingFunction,"incomplete resource lifecycle functions rejected at create");
}
void boundedAndCancel(){++cases;Fixture f;const auto first=f.record();ok(first.result);f.command.command=h<VkCommandBuffer>(31);const auto second=f.record();ok(second.result);
    f.command.command=h<VkCommandBuffer>(32);const auto count=trace.commands;error(f.record().result,ds::Error::Capacity);require(trace.commands==count,"full slot refusal issues zero commands");
    trace.resetResult=VK_ERROR_OUT_OF_HOST_MEMORY;error(ds::cancelUnsubmitted(f.context,first.ticket),ds::Error::DriverFailed);error(ds::destroy(f.context),ds::Error::Pending);
    trace.resetResult=VK_SUCCESS;ok(ds::cancelUnsubmitted(f.context,first.ticket));const auto third=f.record();ok(third.result);
    require(third.ticket.slot==first.ticket.slot&&third.ticket.generation>first.ticket.generation,"reuse advances ticket generation");
    error(ds::cancelUnsubmitted(f.context,first.ticket),ds::Error::StaleTicket);ok(ds::cancelUnsubmitted(f.context,second.ticket));ok(ds::cancelUnsubmitted(f.context,third.ticket));
}
void submittedAndConsumed(){++cases;Fixture f;auto captured=f.record();ok(captured.result);trace.captureStatus=VK_NOT_READY;
    ok(ds::trackSubmission(f.context,captured.ticket,f.command,h<VkFence>(1),VK_SUCCESS));error(ds::cancelUnsubmitted(f.context,captured.ticket),ds::Error::WrongState);
    error(ds::pollCapture(f.context,captured.ticket),ds::Error::Pending);trace.captureStatus=VK_TIMEOUT;error(ds::pollCapture(f.context,captured.ticket),ds::Error::Pending);
    error(ds::destroy(f.context),ds::Error::Pending);trace.captureStatus=VK_SUCCESS;ok(ds::pollCapture(f.context,captured.ticket));
    error(ds::destroy(f.context),ds::Error::Pending);trace.consumerStatus=VK_NOT_READY;error(ds::releaseConsumed(f.context,captured.ticket,h<VkFence>(2)),ds::Error::Pending);
    error(ds::releaseConsumed(f.context,captured.ticket,h<VkFence>(3)),ds::Error::InvalidInput);trace.consumerStatus=VK_SUCCESS;ok(ds::releaseConsumed(f.context,captured.ticket,h<VkFence>(2)));
    ++f.command.generation;const auto next=f.record();ok(next.result);require(next.ticket.generation>captured.ticket.generation,"actual completed capture and consumer allow newer ticket");
    error(ds::pollCapture(f.context,captured.ticket),ds::Error::StaleTicket);ok(ds::cancelUnsubmitted(f.context,next.ticket));
}
void sameRecordingAndAlias(){++cases;Fixture f;const auto first=f.record();const auto second=f.record();ok(first.result);ok(second.result);
    ok(ds::cancelUnsubmitted(f.context,first.ticket));require(ds::stats(f.context).cancellations==2,"actual CB reset cancels every ticket in exact recording");
    error(ds::cancelUnsubmitted(f.context,second.ticket),ds::Error::StaleTicket);f.sources[0].image=first.targets[1].image;
    const auto commands=trace.commands;error(f.record().result,ds::Error::Alias);require(commands==trace.commands,"source cannot alias any owned target plane or slot");
    f.sources[0].image=h<VkImage>(100);++f.command.generation;const auto a=f.record();const auto b=f.record();ok(a.result);ok(b.result);
    trace.captureStatus=VK_NOT_READY;ok(ds::trackSubmission(f.context,a.ticket,f.command,h<VkFence>(1),VK_SUCCESS));
    error(ds::cancelUnsubmitted(f.context,b.ticket),ds::Error::WrongState);error(ds::pollCapture(f.context,b.ticket),ds::Error::Pending);
    trace.captureStatus=VK_SUCCESS;ok(ds::pollCapture(f.context,a.ticket));ok(ds::pollCapture(f.context,b.ticket));ok(ds::releaseConsumed(f.context,a.ticket,h<VkFence>(1)));ok(ds::releaseConsumed(f.context,b.ticket,h<VkFence>(1)));
}
void uncertainRetained(){++cases;
    for(unsigned mode=0;mode<2;++mode){Fixture f;const auto capture=f.record();ok(capture.result);
        if(!mode)error(ds::trackSubmission(f.context,capture.ticket,f.command,h<VkFence>(1),VK_ERROR_OUT_OF_HOST_MEMORY),ds::Error::DriverFailed);
        else {ok(ds::trackSubmission(f.context,capture.ticket,f.command,h<VkFence>(1),VK_SUCCESS));trace.captureStatus=VK_ERROR_DEVICE_LOST;error(ds::pollCapture(f.context,capture.ticket),ds::Error::DeviceLost);}
        trace.captureStatus=VK_SUCCESS;require(!ds::pollCapture(f.context,capture.ticket).accepted(),"uncertain submission/device loss cannot become completion by later unrelated success");
        error(ds::cancelUnsubmitted(f.context,capture.ticket),ds::Error::WrongState);error(ds::destroy(f.context),ds::Error::Pending);require(trace.destroys==0&&trace.frees==0,"uncertain resources and owner retained to process exit");}
}
void depthCommonShaderReadLayout(){++cases;Fixture f;f.sources[2].currentLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const auto capture=f.record();ok(capture.result);constexpr auto both=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    require(trace.barriers[4][0].oldLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&trace.barriers[4][0].subresourceRange.aspectMask==both,
        "D32S8 common layout5 enters transfer with both aspects");
    require(trace.barriers[5][0].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&trace.barriers[5][0].subresourceRange.aspectMask==both&&
        trace.copies[2].srcSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT,"D32S8 restores exact common layout5 while copying only depth");
    ok(ds::cancelUnsubmitted(f.context,capture.ticket));
    f.sources[2].successfulCreateInfo.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const auto before=trace.commands;error(f.record().result,ds::Error::UnsupportedImage);require(trace.commands==before,"layout5 without sampled/input usage refuses all commands");
}
void hotPath(){++cases;Fixture f;allocations=0;measured=true;
    for(unsigned i=0;i<10000;++i){++f.command.generation;const auto capture=f.record();ok(capture.result);ok(ds::trackSubmission(f.context,capture.ticket,f.command,h<VkFence>(1),VK_SUCCESS));
        ok(ds::pollCapture(f.context,capture.ticket));ok(ds::releaseConsumed(f.context,capture.ticket,h<VkFence>(1)));}
    measured=false;require(!allocations,"record/fence polling/consumer release allocate no hot-path heap");require(ds::stats(f.context).records==10000,"bounded recorder remains reusable");
}
}
int main(){try{transitions();preflight();boundedAndCancel();submittedAndConsumed();sameRecordingAndAlias();uncertainRetained();depthCommonShaderReadLayout();hotPath();
    std::cout<<"{\"stage\":\"dispatch-snapshot-cpu\",\"passed\":true,\"cases\":"<<cases<<",\"checks\":"<<checks<<",\"hotPathAllocations\":"<<allocations
        <<",\"retainedUncertainOwners\":"<<retainedOwners<<",\"vulkanLoaded\":false,\"gpuTouched\":false,\"gamePixelsCaptured\":false,\"xefgIntegrated\":false}\n";return 0;
}catch(const std::exception& e){measured=false;std::cerr<<e.what()<<'\n';return 1;}}
