// Self-owned GPU clears and dispatch-position image copies. No production hook,
// game objects, source upload, shader image reads, conversion or XeFG.
#include "dispatch_snapshot.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace ds=xess_vk::dispatch_snapshot;
void check(VkResult r,const char* operation){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": VkResult "+std::to_string(r));}
void check(ds::Result r,const char* operation){if(!r.accepted())throw std::runtime_error(std::string(operation)+": "+ds::errorName(r.error));}
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
DWORD WINAPI watchdog(void* event){if(WaitForSingleObject(static_cast<HANDLE>(event),28000)==WAIT_TIMEOUT){
    const char message[]="Dispatch snapshot GPU fixture exceeded 28 seconds.\n";DWORD written{};
    WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,sizeof(message)-1,&written,nullptr);TerminateProcess(GetCurrentProcess(),124);}return 0;}
struct Watchdog {HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);HANDLE thread=event?CreateThread(nullptr,0,watchdog,event,0,nullptr):nullptr;
    Watchdog(){require(event&&thread,"fixture watchdog creation");}~Watchdog(){SetEvent(event);WaitForSingleObject(thread,INFINITE);CloseHandle(thread);CloseHandle(event);}};
PFN_vkCmdPipelineBarrier actualRecorderBarrier{};PFN_vkCmdCopyImage actualRecorderCopy{};std::uint64_t recorderCommandCalls{};
VKAPI_ATTR void VKAPI_CALL countedBarrier(VkCommandBuffer command,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
    std::uint32_t memoryCount,const VkMemoryBarrier* memory,std::uint32_t bufferCount,const VkBufferMemoryBarrier* buffers,std::uint32_t imageCount,const VkImageMemoryBarrier* images){
    ++recorderCommandCalls;actualRecorderBarrier(command,src,dst,flags,memoryCount,memory,bufferCount,buffers,imageCount,images);}
VKAPI_ATTR void VKAPI_CALL countedCopy(VkCommandBuffer command,VkImage source,VkImageLayout sourceLayout,VkImage target,VkImageLayout targetLayout,std::uint32_t count,const VkImageCopy* regions){
    ++recorderCommandCalls;actualRecorderCopy(command,source,sourceLayout,target,targetLayout,count,regions);}
struct Fixture {
    HMODULE loader{};PFN_vkGetInstanceProcAddr gipa{};PFN_vkGetDeviceProcAddr gdpa{};
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};std::uint32_t family{};
    VkPhysicalDeviceMemoryProperties memory{};VkPhysicalDeviceProperties properties{};
    VkCommandPool pool{};std::array<VkCommandBuffer,2> commands{};VkFence fence{};VkSemaphore gate{};
    VkPipelineLayout pipelineLayout{};VkPipeline pipeline{};VkShaderModule shader{};
    std::array<ds::Source,4> sources{};std::array<VkDeviceMemory,4> sourceMemory{};
    ds::Context* recorder{};std::array<ds::Capture,2> captures{};std::size_t captureCount{};
    VkBuffer readback{};VkDeviceMemory readbackMemory{};void* mapped{};bool coherent{};
    std::array<std::array<VkDeviceSize,3>,4> offsets{};VkDeviceSize readbackBytes{};
    bool initialized{},mightPending{},gateClosed{},cleanupDone{},cleanupSucceeded{},pendingVerified{},reuseRefused{},generationAdvanced{};
    unsigned submissions{},captureRecords{};std::uint64_t verifiedPixels{};std::uint64_t recordingGeneration{};
    bool injectMismatch{};
    ~Fixture(){(void)cleanup();}
    template<class T>T inst(const char* name)const{auto p=gipa(instance,name);if(!p)throw std::runtime_error(std::string("Missing instance function ")+name);return reinterpret_cast<T>(p);}
    template<class T>T dev(const char* name)const{auto p=gdpa(device,name);if(!p)throw std::runtime_error(std::string("Missing device function ")+name);return reinterpret_cast<T>(p);}
    static VkImageAspectFlags aspect(VkFormat format){return format==VK_FORMAT_D32_SFLOAT_S8_UINT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;}
    static VkImageAspectFlags barrierAspect(VkFormat format){return format==VK_FORMAT_D32_SFLOAT_S8_UINT?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT;}
    std::uint32_t memoryType(std::uint32_t mask,VkMemoryPropertyFlags wanted){
        for(std::uint32_t i=0;i<memory.memoryTypeCount;++i){if((mask&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&wanted)==wanted)return i;}
        throw std::runtime_error("fixture memory type unavailable");}
    void initialize(){
        require(!GetModuleHandleW(L"xess-vulkan-observer.dll"),"raw fixture cannot run with production observer loaded");
        loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);require(loader!=nullptr,"system Vulkan loader unavailable");
        gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));require(gipa!=nullptr,"vkGetInstanceProcAddr unavailable");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Isolated dispatch snapshot pixels";app.apiVersion=VK_API_VERSION_1_2;
        VkInstanceCreateInfo createInstance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};createInstance.pApplicationInfo=&app;check(inst<PFN_vkCreateInstance>("vkCreateInstance")(&createInstance,nullptr,&instance),"create instance");
        std::uint32_t deviceCount{};check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance,&deviceCount,nullptr),"enumerate device count");std::vector<VkPhysicalDevice> candidates(deviceCount);
        check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance,&deviceCount,candidates.data()),"enumerate devices");
        // Prefer the same Intel device as the project's owned native fixtures.
        for(auto candidate:candidates){VkPhysicalDeviceProperties p{};inst<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate,&p);
            if(p.apiVersion>=VK_API_VERSION_1_2&&(!physical||p.vendorID==0x8086)){physical=candidate;properties=p;if(p.vendorID==0x8086)break;}}
        require(physical!=VK_NULL_HANDLE,"Vulkan 1.2 device unavailable");
        VkPhysicalDeviceTimelineSemaphoreFeatures available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&available;
        inst<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical,&features);require(available.timelineSemaphore==VK_TRUE,"timeline semaphores unavailable");
        std::uint32_t familyCount{};inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&familyCount,nullptr);std::vector<VkQueueFamilyProperties> families(familyCount);
        inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&familyCount,families.data());
        auto chosen=std::find_if(families.begin(),families.end(),[](const auto& f){return f.queueCount&&(f.queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT);});
        require(chosen!=families.end(),"same graphics/compute queue family unavailable");family=static_cast<std::uint32_t>(chosen-families.begin());
        const float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
        VkPhysicalDeviceTimelineSemaphoreFeatures enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};enabled.timelineSemaphore=VK_TRUE;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=&enabled;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;
        check(inst<PFN_vkCreateDevice>("vkCreateDevice")(physical,&dc,nullptr,&device),"create timeline device");gdpa=inst<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device,family,0,&queue);inst<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical,&memory);
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=family;
        check(dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device,&cp,nullptr,&pool),"create reset-capable command pool");VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=2;
        check(dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device,&ca,commands.data()),"allocate primary commands");VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(dev<PFN_vkCreateFence>("vkCreateFence")(device,&fi,nullptr,&fence),"create capture fence");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;type.initialValue=0;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};si.pNext=&type;check(dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device,&si,nullptr,&gate),"create initially unsignaled timeline gate");
        ds::Config config;config.device=device;config.queueFamily=family;config.queueFlags=chosen->queueFlags;config.memory=memory;config.planeCount=4;
        config.planes={ds::PlaneSpec{VK_FORMAT_R8G8B8A8_UNORM,{63,37}},ds::PlaneSpec{VK_FORMAT_R16G16_SFLOAT,{31,19}},
            ds::PlaneSpec{VK_FORMAT_D32_SFLOAT_S8_UINT,{31,19}},ds::PlaneSpec{VK_FORMAT_B10G11R11_UFLOAT_PACK32,{63,37}}};
        config.functions={dev<PFN_vkCreateImage>("vkCreateImage"),dev<PFN_vkDestroyImage>("vkDestroyImage"),dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements"),
            dev<PFN_vkAllocateMemory>("vkAllocateMemory"),dev<PFN_vkFreeMemory>("vkFreeMemory"),dev<PFN_vkBindImageMemory>("vkBindImageMemory"),
            dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier"),dev<PFN_vkCmdCopyImage>("vkCmdCopyImage"),dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer"),dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")};
        actualRecorderBarrier=config.functions.pipelineBarrier;actualRecorderCopy=config.functions.copyImage;
        config.functions.pipelineBarrier=countedBarrier;config.functions.copyImage=countedCopy;
        for(std::size_t n=0;n<sources.size();++n){auto& s=sources[n];const auto& p=config.planes[n];VkFormatProperties fp{};inst<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical,p.format,&fp);
            constexpr VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            require((fp.optimalTilingFeatures&needed)==needed,"required owned format transfer/sample features unavailable");
            s.device=device;s.ownerQueueFamily=family;s.currentLayout=n==2?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            s.range={aspect(p.format),0,1,0,1};s.precedingStages=s.resumedStages=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;s.precedingAccess=s.resumedAccess=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            auto& i=s.successfulCreateInfo;i.imageType=VK_IMAGE_TYPE_2D;i.format=p.format;i.extent={p.extent.width,p.extent.height,1};i.mipLevels=i.arrayLayers=1;
            i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;i.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            check(dev<PFN_vkCreateImage>("vkCreateImage")(device,&i,nullptr,&s.image),"create GPU-only source image");VkMemoryRequirements mr{};dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device,s.image,&mr);
            VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=memoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&ma,nullptr,&sourceMemory[n]),"allocate source device-local memory");check(dev<PFN_vkBindImageMemory>("vkBindImageMemory")(device,s.image,sourceMemory[n],0),"bind source image");
            const VkDeviceSize bytes=static_cast<VkDeviceSize>(p.extent.width)*p.extent.height*4;for(auto& offset:offsets[n]){offset=readbackBytes;readbackBytes+=bytes;}}
        ds::Result why;recorder=ds::create(config,&why);require(recorder!=nullptr,"create isolated recorder");check(why,"recorder image allocation");
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=readbackBytes;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(dev<PFN_vkCreateBuffer>("vkCreateBuffer")(device,&bi,nullptr,&readback),"create host readback buffer");VkMemoryRequirements br{};dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device,readback,&br);
        std::uint32_t index=UINT32_MAX;for(std::uint32_t n=0;n<memory.memoryTypeCount;++n)if((br.memoryTypeBits&(1u<<n))&&(memory.memoryTypes[n].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){index=n;coherent=true;break;}
        if(index==UINT32_MAX)index=memoryType(br.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);VkMemoryAllocateInfo ba{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ba.allocationSize=br.size;ba.memoryTypeIndex=index;
        check(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&ba,nullptr,&readbackMemory),"allocate readback memory");check(dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device,readback,readbackMemory,0),"bind readback");
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};check(dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device,&pl,nullptr,&pipelineLayout),"create empty no-op layout");
        constexpr std::uint32_t code[]={0x07230203,0x00010000,0,5,0,(2u<<16)|17,1,(3u<<16)|14,0,1,(5u<<16)|15,5,3,0x6e69616d,0,
            (6u<<16)|16,3,17,1,1,1,(2u<<16)|19,1,(3u<<16)|33,2,1,(5u<<16)|54,1,3,0,2,(2u<<16)|248,4,(1u<<16)|253,(1u<<16)|56};
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=sizeof(code);sm.pCode=code;check(dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(device,&sm,nullptr,&shader),"create no-resource compute shader");
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pi.layout=pipelineLayout;pi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pi.stage.module=shader;pi.stage.pName="main";
        check(dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline),"create no-op pipeline");
        const bool observerLoaded=GetModuleHandleW(L"xess-vulkan-observer.dll")!=nullptr;
        require(!observerLoaded,"raw fixture unexpectedly loaded the production observer");
        std::printf("{\"event\":\"snapshot_runtime\",\"vendorId\":%u,\"deviceId\":%u,\"queueFamily\":%u,\"observerLoaded\":%s,\"separateDepthStencilLayoutsEnabled\":false}\n",properties.vendorID,properties.deviceID,family,observerLoaded?"true":"false");
    }
    VkImageMemoryBarrier imageBarrier(VkImage image,VkFormat format,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=from;b.newLayout=to;
        b.srcAccessMask=src;b.dstAccessMask=dst;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={barrierAspect(format),0,1,0,1};return b;}
    void transition(const VkImageMemoryBarrier& barrier,VkPipelineStageFlags before,VkPipelineStageFlags after){dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands[0],before,after,0,0,nullptr,0,nullptr,1,&barrier);}
    static std::uint32_t expected(std::size_t plane,bool a){constexpr std::uint32_t values[4][2]{{0xffff00ffu,0xff00ff00u},{0x34003a00u,0x3a003400u},{0x3f400000u,0x3e800000u},{0x780003c0u,0x001e0000u}};return values[plane][a?1:0];}
    void clearSources(bool a){for(std::size_t n=0;n<sources.size();++n){const auto& s=sources[n];const auto format=s.successfulCreateInfo.format;
        auto b=imageBarrier(s.image,format,a?(initialized?s.currentLayout:VK_IMAGE_LAYOUT_UNDEFINED):s.currentLayout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            a&&!initialized?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        transition(b,a&&!initialized?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        if(n==2){VkClearDepthStencilValue clear{a?0.25f:0.75f,a?17u:91u};dev<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(commands[0],s.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&b.subresourceRange);}
        else {VkClearColorValue clear{};if(n==1){clear.float32[0]=a?0.25f:0.75f;clear.float32[1]=a?0.75f:0.25f;}
            else {clear.float32[0]=a?0.0f:1.0f;clear.float32[1]=a?1.0f:0.0f;clear.float32[2]=a?0.0f:1.0f;clear.float32[3]=1.0f;}
            dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands[0],s.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&b.subresourceRange);}
        b=imageBarrier(s.image,format,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,a?s.currentLayout:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,a?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_TRANSFER_READ_BIT);
        transition(b,VK_PIPELINE_STAGE_TRANSFER_BIT,a?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT);}}
    void copyReadback(VkImage image,std::size_t plane,std::size_t group){VkBufferImageCopy region{};region.bufferOffset=offsets[plane][group];region.imageSubresource={aspect(sources[plane].successfulCreateInfo.format),0,0,1};region.imageExtent=sources[plane].successfulCreateInfo.extent;
        dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands[0],image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&region);}
    void recordBatch(bool twoCaptures){check(dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands[0],0),"reset completed command");++recordingGeneration;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands[0],&begin),"begin snapshot batch");
        clearSources(true);captureCount=twoCaptures?2:1;const ds::CommandRecording recording{commands[0],recordingGeneration,family};
        if(twoCaptures){auto invalid=sources;invalid.back().successfulCreateInfo.usage&=~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;const auto before=recorderCommandCalls;
            const auto rejected=ds::record(recorder,recording,invalid.size(),invalid.data());require(rejected.result.error==ds::Error::UnsupportedImage&&recorderCommandCalls==before,"four-plane preflight failure must issue zero actual Vulkan commands");
            std::printf("{\"event\":\"all_plane_preflight_refusal\",\"planeCount\":4,\"badPlane\":3,\"commandsIssued\":%llu}\n",static_cast<unsigned long long>(recorderCommandCalls-before));}
        for(std::size_t i=0;i<captureCount;++i){captures[i]=ds::record(recorder,recording,sources.size(),sources.data());check(captures[i].result,"capture before dispatch");++captureRecords;
            std::printf("{\"event\":\"capture_recorded\",\"slot\":%u,\"ticketGeneration\":%llu,\"commandRecordingGeneration\":%llu,\"planeCount\":4,\"shaderReadsImages\":false}\n",captures[i].ticket.slot,
                static_cast<unsigned long long>(captures[i].ticket.generation),static_cast<unsigned long long>(recordingGeneration));}
        // Capture location is immediately before a real dispatch command. The
        // no-op shader intentionally does not read the four source images.
        dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);dev<PFN_vkCmdDispatch>("vkCmdDispatch")(commands[0],1,1,1);
        clearSources(false);
        for(std::size_t n=0;n<sources.size();++n){copyReadback(sources[n].image,n,0);for(std::size_t i=0;i<captureCount;++i){const auto& own=captures[i].targets[n];
            transition(imageBarrier(own.image,own.format,own.finalLayout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyReadback(own.image,n,i+1);
            transition(imageBarrier(own.image,own.format,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,own.finalLayout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);}
            transition(imageBarrier(sources[n].image,sources[n].successfulCreateInfo.format,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,sources[n].currentLayout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);}
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=readback;host.size=VK_WHOLE_SIZE;
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands[0],VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        check(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands[0]),"end snapshot batch");}
    void submit(bool gated){check(dev<PFN_vkResetFences>("vkResetFences")(device,1,&fence),"reset released capture fence");VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};submitInfo.commandBufferCount=1;submitInfo.pCommandBuffers=&commands[0];
        std::uint64_t value=1;VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        if(gated){timeline.waitSemaphoreValueCount=1;timeline.pWaitSemaphoreValues=&value;submitInfo.pNext=&timeline;submitInfo.waitSemaphoreCount=1;submitInfo.pWaitSemaphores=&gate;submitInfo.pWaitDstStageMask=&waitStage;gateClosed=true;}
        mightPending=true;const auto result=dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&submitInfo,fence);
        check(ds::trackSubmission(recorder,captures[0].ticket,{commands[0],recordingGeneration,family},fence,result),"associate actual capture submit/fence");check(result,"submit captured GPU batch");++submissions;
        if(gated){const auto status=dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,fence);const auto waited=dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,5'000'000);
            require(status==VK_NOT_READY&&waited==VK_TIMEOUT,"unsignaled timeline gate must keep actual fence pending");require(ds::pollCapture(recorder,captures[0].ticket).error==ds::Error::Pending,"recorder cannot complete while gate is closed");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands[1],&begin),"begin capacity-check command");
            const auto before=recorderCommandCalls;const auto blocked=ds::record(recorder,{commands[1],1,family},sources.size(),sources.data());require(blocked.result.error==ds::Error::Capacity&&before==recorderCommandCalls,"both submitted slots must refuse reuse without any Vulkan command");reuseRefused=true;
            std::printf("{\"event\":\"timeline_pending\",\"actualFenceStatus\":%d,\"finiteWaitResult\":%d,\"reuseRefusal\":\"%s\",\"refusedCommandsIssued\":%llu}\n",int(status),int(waited),ds::errorName(blocked.result.error),static_cast<unsigned long long>(recorderCommandCalls-before));
            require(ds::cancelUnsubmitted(recorder,captures[1].ticket).error==ds::Error::WrongState&&ds::destroy(recorder).error==ds::Error::Pending,"pending slot cannot cancel or destroy");pendingVerified=true;
            VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};signal.semaphore=gate;signal.value=1;check(dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(device,&signal),"release actual timeline gate");gateClosed=false;}
        check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,5'000'000'000),"bounded capture plus readback fence");mightPending=false;initialized=true;
        for(std::size_t i=0;i<captureCount;++i)check(ds::pollCapture(recorder,captures[i].ticket),"actual capture completion query");}
    void verify(){check(dev<PFN_vkMapMemory>("vkMapMemory")(device,readbackMemory,0,VK_WHOLE_SIZE,0,&mapped),"map completed GPU readback");
        if(!coherent){VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=readbackMemory;range.size=VK_WHOLE_SIZE;check(dev<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device,1,&range),"invalidate completed noncoherent readback");}
        for(std::size_t plane=0;plane<sources.size();++plane){const auto& image=sources[plane].successfulCreateInfo;for(std::size_t group=0;group<=captureCount;++group){
            const auto* data=static_cast<const unsigned char*>(mapped)+offsets[plane][group];const auto wanted=expected(plane,group!=0);
            for(std::uint32_t y=0;y<image.extent.height;++y)for(std::uint32_t x=0;x<image.extent.width;++x){std::uint32_t actual{};std::memcpy(&actual,data+(static_cast<std::size_t>(y)*image.extent.width+x)*4,4);
                const bool inject=injectMismatch&&plane==0&&group==1&&x==image.extent.width/2&&y==image.extent.height/2;const auto compared=inject?wanted^1u:wanted;
                if(actual!=compared){std::printf("{\"event\":\"pixel_mismatch\",\"plane\":%zu,\"format\":%u,\"group\":%zu,\"x\":%u,\"y\":%u,\"expectedBits\":%u,\"actualBits\":%u,\"expectedMismatchInjected\":%s}\n",plane,unsigned(image.format),group,x,y,compared,actual,inject?"true":"false");throw std::runtime_error("GPU pixel mismatch at reported plane/group/x/y");}
                ++verifiedPixels;}}
            std::printf("{\"event\":\"plane_pixels_verified\",\"format\":%u,\"width\":%u,\"height\":%u,\"capturedCopies\":%zu,\"verifiedPixelCount\":%llu,\"capturedABits\":%u,\"sourceBBits\":%u,\"depthOnly\":%s}\n",unsigned(image.format),image.extent.width,image.extent.height,captureCount,
                static_cast<unsigned long long>(image.extent.width)*image.extent.height*(captureCount+1),expected(plane,true),expected(plane,false),plane==2?"true":"false");}
        dev<PFN_vkUnmapMemory>("vkUnmapMemory")(device,readbackMemory);mapped=nullptr;
        for(std::size_t i=0;i<captureCount;++i)check(ds::releaseConsumed(recorder,captures[i].ticket,fence),"release actual completed capture plus readback consumer");}
    void run(){initialize();recordBatch(true);const auto old=captures[0].ticket;submit(true);verify();recordBatch(false);
        generationAdvanced=captures[0].ticket.slot==old.slot&&captures[0].ticket.generation>old.generation;require(generationAdvanced,"completed consumed slot must reuse with newer generation");
        require(ds::pollCapture(recorder,old).error==ds::Error::StaleTicket,"old generation must not validate newer capture");submit(false);verify();}
    bool cleanup()noexcept {if(cleanupDone)return cleanupSucceeded;try{
        if(device&&gdpa){if(gateClosed){VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};signal.semaphore=gate;signal.value=1;
                if(dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(device,&signal)!=VK_SUCCESS)return false;gateClosed=false;}
            if(mightPending){if(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,2'500'000'000)!=VK_SUCCESS)return false;mightPending=false;}
            if(mapped){dev<PFN_vkUnmapMemory>("vkUnmapMemory")(device,readbackMemory);mapped=nullptr;}
            if(recorder){for(const auto& capture:captures)if(capture.result.accepted()&&capture.ticket.generation){auto r=ds::pollCapture(recorder,capture.ticket);
                    if(r.accepted())(void)ds::releaseConsumed(recorder,capture.ticket,fence);else if(r.error==ds::Error::WrongState)(void)ds::cancelUnsubmitted(recorder,capture.ticket);}
                if(!ds::destroy(recorder).accepted())return false;recorder=nullptr;}
            if(pipeline)dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device,pipeline,nullptr);if(shader)dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device,shader,nullptr);
            if(pipelineLayout)dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device,pipelineLayout,nullptr);
            if(readback)dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device,readback,nullptr);if(readbackMemory)dev<PFN_vkFreeMemory>("vkFreeMemory")(device,readbackMemory,nullptr);
            for(std::size_t i=0;i<sources.size();++i){if(sources[i].image)dev<PFN_vkDestroyImage>("vkDestroyImage")(device,sources[i].image,nullptr);if(sourceMemory[i])dev<PFN_vkFreeMemory>("vkFreeMemory")(device,sourceMemory[i],nullptr);}
            if(pool)dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device,pool,nullptr);if(fence)dev<PFN_vkDestroyFence>("vkDestroyFence")(device,fence,nullptr);
            if(gate)dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device,gate,nullptr);dev<PFN_vkDestroyDevice>("vkDestroyDevice")(device,nullptr);device=VK_NULL_HANDLE;}
        if(instance){inst<PFN_vkDestroyInstance>("vkDestroyInstance")(instance,nullptr);instance=VK_NULL_HANDLE;}if(loader){FreeLibrary(loader);loader=nullptr;}
        cleanupDone=cleanupSucceeded=true;return true;
    }catch(...){return false;}}
};
void claims(){std::cout<<",\"gamePixelsCaptured\":false,\"xefgIntegrated\":false,\"motionSemanticsVerified\":false,\"gameSameFrameVerified\":false,\"depthConverted\":false";}
}
int main(int argc,char** argv){Watchdog watch;Fixture f;for(int i=1;i<argc;++i){if(std::strcmp(argv[i],"--inject-pixel-mismatch")==0)f.injectMismatch=true;else {std::cerr<<"unknown fixture option\n";return 2;}}
    try{f.run();const auto clean=f.cleanup();require(clean,"bounded cleanup did not complete");std::cout<<"{\"stage\":\"dispatch-snapshot-gpu\",\"passed\":true,\"selfOwnedPixelsCaptured\":true,\"sourceGpuGenerated\":true,\"sourceCpuUploaded\":false,\"pendingTimelineGateVerified\":"<<(f.pendingVerified?"true":"false")
        <<",\"pendingReuseRefused\":"<<(f.reuseRefused?"true":"false")<<",\"ticketGenerationAdvanced\":"<<(f.generationAdvanced?"true":"false")<<",\"submissions\":"<<f.submissions<<",\"captures\":"<<f.captureRecords<<",\"formats\":[37,83,130,122],\"verifiedPixels\":"<<f.verifiedPixels<<",\"cleanupSucceeded\":true";claims();std::cout<<"}\n";return 0;
    }catch(const std::exception& error){const auto clean=f.cleanup();std::cerr<<error.what()<<'\n';std::cout<<"{\"stage\":\"dispatch-snapshot-gpu\",\"passed\":false,\"selfOwnedPixelsCaptured\":false,\"sourceCpuUploaded\":false,\"cleanupSucceeded\":"<<(clean?"true":"false")<<",\"pendingOwnerRetained\":"<<(clean?"false":"true");claims();std::cout<<"}\n";return 1;}}
