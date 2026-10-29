#include "fsr4_bridge.hpp"
#include "native_source_fixture.hpp"
#include <cmath>
#include <iostream>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <future>

using namespace xess_vk;
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
PFN_vkGetDeviceProcAddr originalGdpa{};
PFN_vkQueueSubmit originalSubmit{};PFN_vkQueueSubmit2 originalSubmit2{};
std::atomic<bool> pauseNext{},oomNext{};std::mutex pauseMutex;std::condition_variable pauseCondition;
bool entered{},released{};
VkResult interceptSubmit(){
    if(oomNext.exchange(false))return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(pauseNext.exchange(false)){
        std::unique_lock lock(pauseMutex);entered=true;pauseCondition.notify_all();
        if(!pauseCondition.wait_for(lock,std::chrono::seconds(5),[]{return released;}))return VK_ERROR_UNKNOWN;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL pausedSubmit(VkQueue q,unsigned n,const VkSubmitInfo* info,VkFence fence){
    const auto r=interceptSubmit();return r==VK_SUCCESS?originalSubmit(q,n,info,fence):r;
}
VKAPI_ATTR VkResult VKAPI_CALL pausedSubmit2(VkQueue q,unsigned n,const VkSubmitInfo2* info,VkFence fence){
    const auto r=interceptSubmit();return r==VK_SUCCESS?originalSubmit2(q,n,info,fence):r;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL testGdpa(VkDevice d,const char* name){
    if(!std::strcmp(name,"vkQueueSubmit"))return reinterpret_cast<PFN_vkVoidFunction>(pausedSubmit);
    if(!std::strcmp(name,"vkQueueSubmit2")||!std::strcmp(name,"vkQueueSubmit2KHR"))return reinterpret_cast<PFN_vkVoidFunction>(pausedSubmit2);
    return originalGdpa(d,name);
}
template<class Submit>VkResult concurrentSubmit(fsr4::Bridge& bridge,sr_overlay::RecordingKey key,VkCommandBuffer tail,Submit submit){
    {std::lock_guard lock(pauseMutex);entered=released=false;}pauseNext=true;
    auto result=std::async(std::launch::async,submit);
    bool arrived{};{std::unique_lock lock(pauseMutex);arrived=pauseCondition.wait_for(lock,std::chrono::seconds(3),[]{return entered;});}
    auto lookup=std::async(std::launch::async,[&]{return bridge.command(reinterpret_cast<VkCommandBuffer>(std::uintptr_t(key.command)))==tail&&bridge.contains(key);});
    const bool ready=lookup.wait_for(std::chrono::milliseconds(300))==std::future_status::ready;
    {std::lock_guard lock(pauseMutex);released=true;}pauseCondition.notify_all();
    const auto r=result.get();const auto correct=lookup.get();
    require(arrived&&ready&&correct,"Driver submission held the bridge metadata/command lock");
    return r;
}
unsigned memoryType(const testing::SourceOwner& owner,unsigned bits,VkMemoryPropertyFlags wanted){
    for(unsigned n=0;n<owner.memory.memoryTypeCount;++n)if((bits&(1u<<n))&&(owner.memory.memoryTypes[n].propertyFlags&wanted)==wanted)return n;
    throw std::runtime_error("Test memory type unavailable");
}
struct Image {
    testing::SourceOwner& owner;VkImage image{};VkImageView view{};VkDeviceMemory memory{};
    Image(testing::SourceOwner& o,VkExtent2D extent,VkFormat format):owner(o){
        const bool depth=format==VK_FORMAT_D32_SFLOAT_S8_UINT;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;info.format=format;info.extent={extent.width,extent.height,1};
        info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        if(depth)info.usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        testing::check(owner.dev<PFN_vkCreateImage>("vkCreateImage")(owner.device,&info,nullptr,&image),"Create SR test image");
        VkMemoryRequirements requirements{};owner.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(owner.device,image,&requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=requirements.size;allocate.memoryTypeIndex=memoryType(owner,requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        testing::check(owner.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(owner.device,&allocate,nullptr,&memory),"Allocate SR test image");
        testing::check(owner.dev<PFN_vkBindImageMemory>("vkBindImageMemory")(owner.device,image,memory,0),"Bind SR test image");
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=format;v.subresourceRange={depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        testing::check(owner.dev<PFN_vkCreateImageView>("vkCreateImageView")(owner.device,&v,nullptr,&view),"Create SR test view");
    }
    ~Image(){if(owner.retainForPossibleBorrowedChildren)return;owner.dev<PFN_vkDestroyImageView>("vkDestroyImageView")(owner.device,view,nullptr);owner.dev<PFN_vkDestroyImage>("vkDestroyImage")(owner.device,image,nullptr);owner.dev<PFN_vkFreeMemory>("vkFreeMemory")(owner.device,memory,nullptr);}
};
float half(std::uint16_t value){const auto exponent=(value>>10)&31,mantissa=value&1023;
    const auto magnitude=exponent==31?(mantissa?NAN:INFINITY):exponent?std::ldexp(1.f+float(mantissa)/1024,int(exponent)-15):std::ldexp(float(mantissa),-24);return value&0x8000?-magnitude:magnitude;}
void commandBenchmark(fsr4::Bridge& bridge,VkCommandBuffer original,VkCommandBuffer expected){
    constexpr unsigned count=1'000'000;
    for(bool matched:{true,false}){
        const auto command=matched?original:reinterpret_cast<VkCommandBuffer>(std::uintptr_t(original)+0x100000);
        std::uintptr_t checksum{};ULONG64 before{},after{};QueryThreadCycleTime(GetCurrentThread(),&before);
        const auto start=std::chrono::steady_clock::now();
        for(unsigned n=0;n<count;++n)checksum+=std::uintptr_t(bridge.command(command));
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();QueryThreadCycleTime(GetCurrentThread(),&after);
        require(bridge.command(command)==(matched?expected:command),"Benchmark command mapping changed");
        std::cout<<"{\"benchmark\":\"bridge_command\",\"matched\":"<<(matched?"true":"false")<<",\"calls\":"<<count
            <<",\"ns_per_call\":"<<double(ns)/count<<",\"thread_cycles_per_call\":"<<double(after-before)/count<<",\"checksum\":"<<checksum<<"}\n";
    }
}
}
int wmain(int argc,wchar_t** argv){SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    try{
        require(argc==2||(argc==3&&std::wstring_view(argv[2])==L"1440"),"Usage: xess-fsr4-bridge-gpu-test <absolute-payload-directory> [1440]");
        const bool high=argc==3;const VkExtent2D input=high?VkExtent2D{1504,846}:VkExtent2D{1128,634},output=high?VkExtent2D{2560,1440}:VkExtent2D{1920,1080};
        testing::SourceOwner source;source.initialize({input.width,input.height,30000},false,true);source.retainForPossibleBorrowedChildren=true;
        Image color(source,input,VK_FORMAT_B10G11R11_UFLOAT_PACK32),depth(source,input,VK_FORMAT_D32_SFLOAT_S8_UINT),motion(source,input,VK_FORMAT_R16G16_SFLOAT),result(source,output,VK_FORMAT_R16G16B16A16_SFLOAT);
        VkBuffer readback{};VkDeviceMemory readbackMemory{};VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=output.width*output.height*8;buffer.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        testing::check(source.dev<PFN_vkCreateBuffer>("vkCreateBuffer")(source.device,&buffer,nullptr,&readback),"Create test readback");
        VkMemoryRequirements requirements{};source.dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(source.device,readback,&requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=requirements.size;allocate.memoryTypeIndex=memoryType(source,requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        testing::check(source.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(source.device,&allocate,nullptr,&readbackMemory),"Allocate test readback");
        testing::check(source.dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(source.device,readback,readbackMemory,0),"Bind test readback");
        {
            originalGdpa=source.gdpa;originalSubmit=source.dev<PFN_vkQueueSubmit>("vkQueueSubmit");originalSubmit2=source.dev<PFN_vkQueueSubmit2>("vkQueueSubmit2");
            fsr4::Bridge bridge({source.instance,source.physical,source.device,source.family,source.gipa,testGdpa},std::filesystem::path(argv[1]));
            sr_overlay::Configuration configuration;configuration.input=input;configuration.output=output;configuration.domain=sr_overlay::ColorDomain::SceneLinearHdr;
            configuration.invertedDepth=configuration.jitteredMotion=true;configuration.velocityScaleX=input.width;configuration.velocityScaleY=input.height;bridge.configure(configuration);
            for(unsigned frame=1;frame<=3;++frame){
                testing::check(source.dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(source.command,0),"Reset source command");
                testing::check(source.dev<PFN_vkResetFences>("vkResetFences")(source.device,1,&source.completion),"Reset source fence");
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                testing::check(source.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(source.command,&begin),"Begin source command");
                const std::array<Image*,4> images{&color,&depth,&motion,&result};std::array<VkImageMemoryBarrier,4> initialize{};
                for(unsigned n=0;n<4;++n){auto& b=initialize[n];b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.oldLayout=frame==1?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
                    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=images[n]->image;b.subresourceRange={n==1?VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT):VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;}
                source.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(source.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,initialize.size(),initialize.data());
                std::array<VkClearColorValue,4> clear{};clear[0].float32[0]=0.25f;clear[0].float32[1]=0.5f;clear[0].float32[2]=0.75f;clear[0].float32[3]=1;clear[1].float32[0]=0.5f;
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                for(unsigned n=0;n<4;++n)if(n!=1)source.dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(source.command,images[n]->image,VK_IMAGE_LAYOUT_GENERAL,&clear[n],1,&range);
                const VkClearDepthStencilValue depthClear{0.5f,0};const VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
                source.dev<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(source.command,depth.image,VK_IMAGE_LAYOUT_GENERAL,&depthClear,1,&depthRange);
                sr_overlay::Frame f;f.configuration=configuration;f.owner={reinterpret_cast<std::uintptr_t>(source.device),1,reinterpret_cast<std::uintptr_t>(source.command),frame,1,frame};
                f.recording.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;f.recording.pool=reinterpret_cast<std::uintptr_t>(source.pool);
                f.recording.beginFlagsKnown=true;f.recording.beginUsageFlags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                const std::array<VkFormat,4> formats{VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_D32_SFLOAT_S8_UINT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT};
                const std::array<sr_overlay::Image*,4> fields{&f.color,&f.depth,&f.motion,&f.nativeOutput};
                for(unsigned n=0;n<4;++n){auto& image=*fields[n];image.sdk.image=images[n]->image;image.sdk.imageView=images[n]->view;image.sdk.format=formats[n];image.currentLayout=VK_IMAGE_LAYOUT_GENERAL;
                    image.barrierRange=n==1?depthRange:range;image.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;}
                if(frame==1){auto reusable=f;reusable.recording.beginUsageFlags=0;bool refused=false;
                    try{bridge.record(reusable,true);}catch(const std::invalid_argument&){refused=true;}
                    require(refused&&bridge.command(source.command)==source.command,"Reusable CB was split or mutated");}
                bridge.record(f,frame==1);const auto tail=bridge.command(source.command);require(tail!=source.command,"Continuation was not created");
                VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};toRead.oldLayout=VK_IMAGE_LAYOUT_GENERAL;toRead.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;toRead.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;toRead.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
                toRead.srcQueueFamilyIndex=toRead.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;toRead.image=result.image;toRead.subresourceRange=range;
                source.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(tail,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&toRead);
                VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={output.width,output.height,1};source.dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(tail,result.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
                std::swap(toRead.oldLayout,toRead.newLayout);std::swap(toRead.srcAccessMask,toRead.dstAccessMask);
                source.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(tail,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&toRead);
                testing::check(source.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(tail),"End continuation");
                const sr_overlay::RecordingKey recordedKey{f.owner.device,f.owner.incarnation,f.owner.command,f.owner.recording};
                if(frame==1)testing::check(concurrentSubmit(bridge,recordedKey,tail,[&]{return bridge.submit(source.queue,0,nullptr,VK_NULL_HANDLE);}),"Unrelated submit while a handoff is pending");
                if(frame==2){VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};command.commandBuffer=source.command;command.deviceMask=1;
                    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};submit.commandBufferInfoCount=1;submit.pCommandBufferInfos=&command;
                    oomNext=true;require(bridge.submit2(source.queue,1,&submit,source.completion)==VK_ERROR_OUT_OF_HOST_MEMORY,"Injected OOM was not propagated");
                    require(bridge.command(source.command)==tail&&!bridge.completed(recordedKey),"Failed submission lost its recording");
                    testing::check(concurrentSubmit(bridge,recordedKey,tail,[&]{return bridge.submit2(source.queue,1,&submit,source.completion);}),"Submit2 SR handoff");}
                else{VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&source.command;
                    testing::check(concurrentSubmit(bridge,recordedKey,tail,[&]{return bridge.submit(source.queue,1,&submit,source.completion);}),"Submit SR handoff");}
                testing::check(source.dev<PFN_vkWaitForFences>("vkWaitForFences")(source.device,1,&source.completion,VK_TRUE,30'000'000'000ull),"Wait SR continuation");
                const sr_overlay::RecordingKey completedKey{f.owner.device,f.owner.incarnation,f.owner.command,f.owner.recording};
                require(bridge.completed(completedKey),"Exact tail retirement was not observable");
                auto staleKey=completedKey;++staleKey.recording;
                require(!bridge.completed(staleKey),"Another CB generation borrowed the completion proof");
                require(bridge.command(source.command)==tail,"Completion polling retired a still-owned CB mapping");
                VkSubmitInfo duplicate{VK_STRUCTURE_TYPE_SUBMIT_INFO};duplicate.commandBufferCount=1;duplicate.pCommandBuffers=&source.command;
                require(bridge.submit(source.queue,1,&duplicate,VK_NULL_HANDLE)==VK_ERROR_UNKNOWN,"Duplicate one-time handoff was forwarded");
                void* mapped{};testing::check(source.dev<PFN_vkMapMemory>("vkMapMemory")(source.device,readbackMemory,0,VK_WHOLE_SIZE,0,&mapped),"Map SR result");
                unsigned finite{},positive{},samples{};for(unsigned y=20;y<output.height;y+=41)for(unsigned x=20;x<output.width;x+=43){std::array<std::uint16_t,4> pixel{};
                    std::memcpy(pixel.data(),static_cast<std::uint8_t*>(mapped)+(y*output.width+x)*8,8);for(unsigned n=0;n<3;++n){const float v=half(pixel[n]);++samples;finite+=std::isfinite(v);positive+=v>0.01f;}}
                source.dev<PFN_vkUnmapMemory>("vkUnmapMemory")(source.device,readbackMemory);require(samples&&finite==samples&&positive==samples,"Roundtrip output is not valid HDR color");
                if(frame==1){commandBenchmark(bridge,source.command,tail);
                    bridge.deactivate();require(bridge.command(source.command)==tail&&bridge.completed(completedKey),"Backend stop destroyed a published continuation");
                    bool inactiveRefused=false;try{bridge.record(f,true);}catch(const std::logic_error&){inactiveRefused=true;}
                    require(inactiveRefused,"Stopped backend accepted another frame before reinitialization");
                    auto invalid=configuration;invalid.input.width=0;bool failed=false;
                    try{bridge.configure(invalid);}catch(const std::exception&){failed=true;}
                    require(failed&&bridge.command(source.command)==tail&&bridge.contains(completedKey),"Failed context initialization invalidated the published bridge");
                    auto resized=configuration;resized.input={input.width/2,input.height/2};resized.output={output.width/2,output.height/2};
                    bridge.configure(resized);
                    require(bridge.command(source.command)==tail&&bridge.completed(completedKey),"Resize lost a completed but still-owned continuation");
                    bridge.configure(configuration);}
                bridge.retire({f.owner.device,f.owner.incarnation,f.owner.command,f.owner.recording});require(bridge.command(source.command)==source.command,"Retired continuation still mapped");bridge.deviceIdle();
                std::cout<<"{\"frame\":"<<frame<<",\"submit2\":"<<(frame==2?"true":"false")<<",\"finite\":"<<finite<<",\"samples\":"<<samples<<"}\n";
            }
            require(bridge.submittedFrames()==3,"Missing SR submissions");
            testing::check(source.dev<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(source.device),"Final source idle");bridge.deviceIdle();
        }
        source.retainForPossibleBorrowedChildren=false;
        source.dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(source.device,readback,nullptr);source.dev<PFN_vkFreeMemory>("vkFreeMemory")(source.device,readbackMemory,nullptr);
        std::cout<<"{\"passed\":true,\"stage\":\"FSR4 XMX Vulkan-D3D12-Vulkan\",\"frames\":3,\"width\":"<<output.width<<",\"height\":"<<output.height
            <<",\"concurrency_checks\":4,\"oom_retry\":true,\"stable_stop_reconfigure\":true,\"validation_layer_loaded\":"<<(GetModuleHandleW(L"VkLayer_khronos_validation.dll")?"true":"false")<<"}\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FSR4 bridge test failed: "<<e.what()<<'\n';return 1;}
}
