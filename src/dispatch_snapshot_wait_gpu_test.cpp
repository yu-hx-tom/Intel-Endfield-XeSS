// A specific two-queue semaphore -> ALL_COMMANDS barrier -> transfer chain.
// Reuse the isolated fixture's owner, pixel oracle and bounded cleanup helpers.
// No production hooks or game objects. The negative never submits bad GPU work.
#define main dispatch_snapshot_base_gpu_fixture_main
#include "dispatch_snapshot_gpu_test.cpp"
#undef main
#include <type_traits>

namespace {
struct Unsupported : std::runtime_error {using std::runtime_error::runtime_error;};
template<class T>unsigned long long handleBits(T value){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<unsigned long long>(value);else return static_cast<unsigned long long>(value);}
struct CommandTrace {unsigned kind{};VkPipelineStageFlags src{},dst{};std::array<VkImageMemoryBarrier,2> images{};VkImageCopy copy{};VkImage source{},target{};VkImageLayout sourceLayout{},targetLayout{};};
std::array<CommandTrace,16> waitTrace{};std::size_t waitTraceCount{};
VKAPI_ATTR void VKAPI_CALL waitBarrier(VkCommandBuffer command,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
    std::uint32_t memoryCount,const VkMemoryBarrier* memory,std::uint32_t bufferCount,const VkBufferMemoryBarrier* buffers,std::uint32_t imageCount,const VkImageMemoryBarrier* images){
    require(waitTraceCount<waitTrace.size()&&imageCount==2,"recorder barrier trace bounds");auto& row=waitTrace[waitTraceCount++];row.kind=0;row.src=src;row.dst=dst;std::memcpy(row.images.data(),images,sizeof(row.images));
    ++recorderCommandCalls;actualRecorderBarrier(command,src,dst,flags,memoryCount,memory,bufferCount,buffers,imageCount,images);}
VKAPI_ATTR void VKAPI_CALL waitCopy(VkCommandBuffer command,VkImage source,VkImageLayout sourceLayout,VkImage target,VkImageLayout targetLayout,std::uint32_t count,const VkImageCopy* regions){
    require(waitTraceCount<waitTrace.size()&&count==1,"recorder copy trace bounds");auto& row=waitTrace[waitTraceCount++];row.kind=1;row.copy=*regions;row.source=source;row.target=target;row.sourceLayout=sourceLayout;row.targetLayout=targetLayout;
    ++recorderCommandCalls;actualRecorderCopy(command,source,sourceLayout,target,targetLayout,count,regions);}
// This oracle admits one recipe only. It is not a general synchronization
// validator and the rejected transfer-only plan is never recorded/submitted.
bool recipe(VkPipelineStageFlags wait,VkPipelineStageFlags firstSrc,VkPipelineStageFlags firstDst){
    return wait==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&(firstSrc&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)&&firstDst==VK_PIPELINE_STAGE_TRANSFER_BIT;
}
struct WaitFixture : Fixture {
    VkQueue producerQueue{};VkFence producerFence{};VkSemaphore produced{};VkEvent marker{};
    bool producerAttempted{},producerSubmitted{},consumerSubmitted{},submissionUncertain{},markerPendingVerified{},markerCompleteVerified{},traceVerified{};
    unsigned markerSamples{},producerQueueCount{};std::uint64_t hostGateBefore{},signalBefore{},hostGateAfter{},signalAfter{};
    void initializeWait(){
        require(!GetModuleHandleW(L"xess-vulkan-observer.dll"),"raw wait fixture cannot load observer");
        loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);require(loader!=nullptr,"system Vulkan loader unavailable");
        gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));require(gipa!=nullptr,"vkGetInstanceProcAddr unavailable");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Isolated compute-wait snapshot chain";app.apiVersion=VK_API_VERSION_1_2;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;check(inst<PFN_vkCreateInstance>("vkCreateInstance")(&ci,nullptr,&instance),"create wait instance");
        std::uint32_t count{};check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance,&count,nullptr),"enumerate wait device count");std::vector<VkPhysicalDevice> candidates(count);
        check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance,&count,candidates.data()),"enumerate wait devices");
        for(auto candidate:candidates){VkPhysicalDeviceProperties p{};inst<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate,&p);
            if(p.apiVersion>=VK_API_VERSION_1_2&&(!physical||p.vendorID==0x8086)){physical=candidate;properties=p;if(p.vendorID==0x8086)break;}}
        if(!physical)throw Unsupported("Vulkan 1.2 device unavailable");
        std::uint32_t familiesCount{};inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&familiesCount,nullptr);std::vector<VkQueueFamilyProperties> families(familiesCount);
        inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&familiesCount,families.data());
        auto selected=std::find_if(families.begin(),families.end(),[](const auto& f){return f.queueCount>=2&&(f.queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT);});
        if(selected==families.end())throw Unsupported("Selected device has no same graphics/compute family with two queues; no single-queue fallback");
        family=static_cast<std::uint32_t>(selected-families.begin());producerQueueCount=selected->queueCount;
        VkPhysicalDeviceTimelineSemaphoreFeatures supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&supported;
        inst<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical,&features);if(!supported.timelineSemaphore)throw Unsupported("timeline semaphores unavailable");
        const float priorities[]{1,1};VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=2;qi.pQueuePriorities=priorities;
        VkPhysicalDeviceTimelineSemaphoreFeatures enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};enabled.timelineSemaphore=VK_TRUE;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.pNext=&enabled;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;check(inst<PFN_vkCreateDevice>("vkCreateDevice")(physical,&di,nullptr,&device),"create actual two-queue device");
        gdpa=inst<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device,family,0,&producerQueue);dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device,family,1,&queue);
        require(producerQueue&&queue&&producerQueue!=queue,"actual producer/consumer queues must be distinct");inst<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical,&memory);
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=family;check(dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device,&cp,nullptr,&pool),"create same-family command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=2;check(dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device,&ca,commands.data()),"allocate two primary commands");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(dev<PFN_vkCreateFence>("vkCreateFence")(device,&fi,nullptr,&fence),"create consumer fence");check(dev<PFN_vkCreateFence>("vkCreateFence")(device,&fi,nullptr,&producerFence),"create producer fence");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};si.pNext=&type;
        check(dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device,&si,nullptr,&gate),"create H host timeline gate at zero");check(dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device,&si,nullptr,&produced),"create S producer timeline at zero");
        VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};check(dev<PFN_vkCreateEvent>("vkCreateEvent")(device,&ei,nullptr,&marker),"create GPU transfer marker");
        ds::Config config;config.device=device;config.queueFamily=family;config.queueFlags=selected->queueFlags;config.memory=memory;config.planeCount=4;
        config.planes={ds::PlaneSpec{VK_FORMAT_R8G8B8A8_UNORM,{63,37}},ds::PlaneSpec{VK_FORMAT_R16G16_SFLOAT,{31,19}},ds::PlaneSpec{VK_FORMAT_D32_SFLOAT_S8_UINT,{31,19}},ds::PlaneSpec{VK_FORMAT_B10G11R11_UFLOAT_PACK32,{63,37}}};
        actualRecorderBarrier=dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");actualRecorderCopy=dev<PFN_vkCmdCopyImage>("vkCmdCopyImage");
        config.functions={dev<PFN_vkCreateImage>("vkCreateImage"),dev<PFN_vkDestroyImage>("vkDestroyImage"),dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements"),
            dev<PFN_vkAllocateMemory>("vkAllocateMemory"),dev<PFN_vkFreeMemory>("vkFreeMemory"),dev<PFN_vkBindImageMemory>("vkBindImageMemory"),waitBarrier,waitCopy,
            dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer"),dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")};
        for(std::size_t n=0;n<sources.size();++n){auto& s=sources[n];const auto& p=config.planes[n];VkFormatProperties fp{};inst<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical,p.format,&fp);
            constexpr VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if((fp.optimalTilingFeatures&needed)!=needed)throw Unsupported("owned format transfer/sample features unavailable");
            s.device=device;s.ownerQueueFamily=family;s.currentLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;s.range={aspect(p.format),0,1,0,1};
            s.precedingStages=s.resumedStages=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;s.precedingAccess=s.resumedAccess=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            auto& i=s.successfulCreateInfo;i.imageType=VK_IMAGE_TYPE_2D;i.format=p.format;i.extent={p.extent.width,p.extent.height,1};i.mipLevels=i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;
            i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;i.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            check(dev<PFN_vkCreateImage>("vkCreateImage")(device,&i,nullptr,&s.image),"create self-owned wait source");VkMemoryRequirements mr{};dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device,s.image,&mr);
            VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=memoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);check(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&ma,nullptr,&sourceMemory[n]),"allocate source memory");
            check(dev<PFN_vkBindImageMemory>("vkBindImageMemory")(device,s.image,sourceMemory[n],0),"bind source memory");const VkDeviceSize bytes=static_cast<VkDeviceSize>(p.extent.width)*p.extent.height*4;for(auto& offset:offsets[n]){offset=readbackBytes;readbackBytes+=bytes;}}
        ds::Result why;recorder=ds::create(config,&why);require(recorder!=nullptr,"create wait recorder");check(why,"preallocate wait recorder targets");
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=readbackBytes;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;check(dev<PFN_vkCreateBuffer>("vkCreateBuffer")(device,&bi,nullptr,&readback),"create wait readback buffer");
        VkMemoryRequirements br{};dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device,readback,&br);std::uint32_t index=UINT32_MAX;
        for(std::uint32_t n=0;n<memory.memoryTypeCount;++n)if((br.memoryTypeBits&(1u<<n))&&(memory.memoryTypes[n].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){index=n;coherent=true;break;}
        if(index==UINT32_MAX)index=memoryType(br.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);VkMemoryAllocateInfo ba{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ba.allocationSize=br.size;ba.memoryTypeIndex=index;
        check(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&ba,nullptr,&readbackMemory),"allocate wait readback memory");check(dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device,readback,readbackMemory,0),"bind wait readback memory");
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};check(dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device,&pl,nullptr,&pipelineLayout),"create empty wait no-op layout");
        constexpr std::uint32_t code[]={0x07230203,0x00010000,0,5,0,(2u<<16)|17,1,(3u<<16)|14,0,1,(5u<<16)|15,5,3,0x6e69616d,0,(6u<<16)|16,3,17,1,1,1,
            (2u<<16)|19,1,(3u<<16)|33,2,1,(5u<<16)|54,1,3,0,2,(2u<<16)|248,4,(1u<<16)|253,(1u<<16)|56};
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=sizeof(code);sm.pCode=code;check(dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(device,&sm,nullptr,&shader),"create wait no-op shader");
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pi.layout=pipelineLayout;pi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pi.stage.module=shader;pi.stage.pName="main";
        check(dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline),"create wait no-op pipeline");
        require(!GetModuleHandleW(L"xess-vulkan-observer.dll"),"wait fixture unexpectedly loaded production observer");
        std::printf("{\"event\":\"wait_runtime\",\"vendorId\":%u,\"deviceId\":%u,\"queueFamily\":%u,\"availableQueueCount\":%u,\"enabledQueueCount\":2,\"producerQueue\":\"0x%llx\",\"consumerQueue\":\"0x%llx\",\"hostGate\":\"0x%llx\",\"producerSemaphore\":\"0x%llx\",\"sourceLayout\":5,\"separateDepthStencilLayoutsEnabled\":false,\"observerLoaded\":false}\n",properties.vendorID,properties.deviceID,family,producerQueueCount,handleBits(producerQueue),handleBits(queue),handleBits(gate),handleBits(produced));
    }
    void recordProducer(){VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands[1],&begin),"begin GPU producer");
        // The shared clear helper records to commands[0]; temporarily select the
        // producer CB, then restore the consumer identity before recording it.
        std::swap(commands[0],commands[1]);clearSources(true);std::swap(commands[0],commands[1]);
        check(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands[1]),"end GPU producer clearA plus layout5");}
    void verifyTrace(){require(waitTraceCount==12,"four-plane recorder emits exact barrier/copy/barrier trace");
        for(std::size_t n=0;n<4;++n){const auto& before=waitTrace[n*3];const auto& copy=waitTrace[n*3+1];const auto& after=waitTrace[n*3+2];
            require(before.kind==0&&copy.kind==1&&after.kind==0&&recipe(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,before.src,before.dst),"actual recorder trace must chain compute wait through ALL_COMMANDS to TRANSFER");
            require(before.images[0].oldLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&before.images[0].newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&before.images[0].dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT&&
                before.images[1].newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&before.images[1].dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT,"actual source/target transfer access and layout trace");
            require(before.images[0].srcAccessMask==(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT)&&copy.sourceLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&copy.targetLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&
                after.images[0].srcAccessMask==VK_ACCESS_TRANSFER_READ_BIT&&after.images[0].dstAccessMask==sources[n].resumedAccess&&after.images[1].srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT&&after.images[1].dstAccessMask==VK_ACCESS_SHADER_READ_BIT,
                "actual source/target availability, copy layouts and restored readable access trace");
            require(copy.source==sources[n].image&&copy.target==captures[0].targets[n].image&&copy.copy.srcSubresource.aspectMask==aspect(sources[n].successfulCreateInfo.format)&&after.images[0].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,"copy identities/aspect and exact source restoration");
            require(before.images[0].subresourceRange.aspectMask==barrierAspect(sources[n].successfulCreateInfo.format)&&after.images[0].subresourceRange.aspectMask==barrierAspect(sources[n].successfulCreateInfo.format),"depth shared layout transition covers depth and stencil");
            std::printf("{\"event\":\"recorder_chain_trace\",\"plane\":%zu,\"format\":%u,\"srcStages\":%u,\"dstStages\":%u,\"sourceAccess\":%u,\"destinationAccess\":%u,\"targetDestinationAccess\":%u,\"copySourceLayout\":%u,\"copyTargetLayout\":%u,\"sourceOldLayout\":5,\"sourceRestoredLayout\":5,\"targetFinalLayout\":%u,\"barrierAspects\":%u,\"copyAspect\":%u,\"order\":[\"barrier\",\"copy\",\"restore\"]}\n",n,unsigned(sources[n].successfulCreateInfo.format),before.src,before.dst,before.images[0].srcAccessMask,before.images[0].dstAccessMask,before.images[1].dstAccessMask,unsigned(copy.sourceLayout),unsigned(copy.targetLayout),unsigned(after.images[1].newLayout),before.images[0].subresourceRange.aspectMask,copy.copy.srcSubresource.aspectMask);}
        traceVerified=true;}
    void recordConsumer(){VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands[0],&begin),"begin compute-wait consumer");
        recordingGeneration=1;captureCount=1;captures[0]=ds::record(recorder,{commands[0],1,family},sources.size(),sources.data());check(captures[0].result,"record inline snapshot after semaphore wait");++captureRecords;verifyTrace();
        dev<PFN_vkCmdSetEvent>("vkCmdSetEvent")(commands[0],marker,VK_PIPELINE_STAGE_TRANSFER_BIT);
        dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);dev<PFN_vkCmdDispatch>("vkCmdDispatch")(commands[0],1,1,1);clearSources(false);
        for(std::size_t n=0;n<4;++n){copyReadback(sources[n].image,n,0);const auto& own=captures[0].targets[n];
            transition(imageBarrier(own.image,own.format,own.finalLayout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyReadback(own.image,n,1);transition(imageBarrier(own.image,own.format,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,own.finalLayout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            transition(imageBarrier(sources[n].image,sources[n].successfulCreateInfo.format,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,sources[n].currentLayout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);}
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=readback;host.size=VK_WHOLE_SIZE;
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands[0],VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);check(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands[0]),"end fenced capture/readback consumer");}
    void runWait(){initializeWait();recordProducer();recordConsumer();const std::uint64_t value=1;
        VkTimelineSemaphoreSubmitInfo pt{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};pt.waitSemaphoreValueCount=pt.signalSemaphoreValueCount=1;pt.pWaitSemaphoreValues=pt.pSignalSemaphoreValues=&value;
        const VkPipelineStageFlags producerWait=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;VkSubmitInfo ps{VK_STRUCTURE_TYPE_SUBMIT_INFO};ps.pNext=&pt;ps.waitSemaphoreCount=ps.signalSemaphoreCount=1;ps.pWaitSemaphores=&gate;ps.pSignalSemaphores=&produced;ps.pWaitDstStageMask=&producerWait;ps.commandBufferCount=1;ps.pCommandBuffers=&commands[1];
        producerAttempted=true;gateClosed=true;const auto producerResult=dev<PFN_vkQueueSubmit>("vkQueueSubmit")(producerQueue,1,&ps,producerFence);
        if(producerResult!=VK_SUCCESS){submissionUncertain=true;check(producerResult,"producer submit failed; consumer not submitted");}producerSubmitted=true;++submissions;
        VkTimelineSemaphoreSubmitInfo ct{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};ct.waitSemaphoreValueCount=1;ct.pWaitSemaphoreValues=&value;
        const VkPipelineStageFlags consumerWait=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;VkSubmitInfo cs{VK_STRUCTURE_TYPE_SUBMIT_INFO};cs.pNext=&ct;cs.waitSemaphoreCount=1;cs.pWaitSemaphores=&produced;cs.pWaitDstStageMask=&consumerWait;cs.commandBufferCount=1;cs.pCommandBuffers=&commands[0];
        mightPending=true;const auto consumerResult=dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&cs,fence);consumerSubmitted=consumerResult==VK_SUCCESS;
        if(consumerResult!=VK_SUCCESS)submissionUncertain=true;
        const auto associated=ds::trackSubmission(recorder,captures[0].ticket,{commands[0],1,family},fence,consumerResult);
        if(!associated.accepted())submissionUncertain=true;
        check(associated,"associate actual consumer capture fence");check(consumerResult,"consumer submit failed");++submissions;
        check(dev<PFN_vkGetSemaphoreCounterValue>("vkGetSemaphoreCounterValue")(device,gate,&hostGateBefore),"read actual H before host signal");check(dev<PFN_vkGetSemaphoreCounterValue>("vkGetSemaphoreCounterValue")(device,produced,&signalBefore),"read actual S before producer runs");
        require(!hostGateBefore&&!signalBefore,"actual H and S start at zero");const auto pendingFence=dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,fence);
        const auto pendingProducer=dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,producerFence);const auto timed=dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,5'000'000);
        require(pendingFence==VK_NOT_READY&&pendingProducer==VK_NOT_READY&&timed==VK_TIMEOUT,"both actual submissions must remain pending behind H");
        for(unsigned i=0;i<8;++i){const auto event=dev<PFN_vkGetEventStatus>("vkGetEventStatus")(device,marker);require(event==VK_EVENT_RESET,"post-copy TRANSFER marker must remain RESET while producer is gated");++markerSamples;Sleep(1);}
        markerPendingVerified=true;require(!mapped,"pending readback is never mapped or host-read");
        std::printf("{\"event\":\"compute_wait_pending\",\"hostGateValue\":%llu,\"producerSignalValue\":%llu,\"producerFenceStatus\":%d,\"consumerFenceStatus\":%d,\"finiteWaitResult\":%d,\"markerStatus\":%d,\"markerSamples\":%u,\"consumerWaitStages\":%u,\"producerWaitStages\":%u,\"pendingHostRead\":false}\n",static_cast<unsigned long long>(hostGateBefore),static_cast<unsigned long long>(signalBefore),int(pendingProducer),int(pendingFence),int(timed),int(VK_EVENT_RESET),markerSamples,consumerWait,producerWait);
        VkSemaphoreSignalInfo hs{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};hs.semaphore=gate;hs.value=1;check(dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(device,&hs),"release H host gate");gateClosed=false;
        check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,5'000'000'000),"bounded actual consumer completion");check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&producerFence,VK_TRUE,5'000'000'000),"bounded actual producer completion");mightPending=false;
        const auto completeMarker=dev<PFN_vkGetEventStatus>("vkGetEventStatus")(device,marker);require(completeMarker==VK_EVENT_SET,"post-copy transfer marker must SET after actual completion");markerCompleteVerified=true;
        const auto completeProducer=dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,producerFence);const auto completeConsumer=dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,fence);
        require(completeProducer==VK_SUCCESS&&completeConsumer==VK_SUCCESS,"query actual completed producer and consumer fences");
        check(dev<PFN_vkGetSemaphoreCounterValue>("vkGetSemaphoreCounterValue")(device,gate,&hostGateAfter),"read completed H");check(dev<PFN_vkGetSemaphoreCounterValue>("vkGetSemaphoreCounterValue")(device,produced,&signalAfter),"read completed S");require(hostGateAfter==1&&signalAfter==1,"actual H and S complete at one");
        check(ds::pollCapture(recorder,captures[0].ticket),"query actual capture completion");verify();
        std::printf("{\"event\":\"compute_wait_completed\",\"hostGateValue\":%llu,\"producerSignalValue\":%llu,\"markerStatus\":%d,\"producerFenceStatus\":%d,\"consumerFenceStatus\":%d}\n",static_cast<unsigned long long>(hostGateAfter),static_cast<unsigned long long>(signalAfter),int(completeMarker),int(completeProducer),int(completeConsumer));}
    bool cleanupWait()noexcept{try{if(device&&gdpa){if(gateClosed){VkSemaphoreSignalInfo hs{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};hs.semaphore=gate;hs.value=1;if(dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(device,&hs)!=VK_SUCCESS)return false;gateClosed=false;}
            if(submissionUncertain)return false;
            if(producerSubmitted&&dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&producerFence,VK_TRUE,2'500'000'000)!=VK_SUCCESS)return false;
            if(consumerSubmitted&&dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,2'500'000'000)!=VK_SUCCESS)return false;
            mightPending=false;if(marker){dev<PFN_vkDestroyEvent>("vkDestroyEvent")(device,marker,nullptr);marker={};}if(produced){dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device,produced,nullptr);produced={};}
            if(producerFence){dev<PFN_vkDestroyFence>("vkDestroyFence")(device,producerFence,nullptr);producerFence={};}}
        return cleanup();}catch(...){return false;}}
};
void waitFalseClaims(){std::cout<<",\"gameTested\":false,\"productionLinked\":false,\"gamePixelsCaptured\":false,\"xefgIntegrated\":false,\"gameSameFrameVerified\":false,\"motionSemanticsVerified\":false,\"depthConverted\":false";}
}
int main(int argc,char** argv){Watchdog bounded;bool negative=false;for(int i=1;i<argc;++i){if(std::strcmp(argv[i],"--reject-transfer-only-recipe")==0)negative=true;else return 4;}
    if(negative){const auto admitted=recipe(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        std::cout<<"{\"stage\":\"dispatch-snapshot-wait\",\"passed\":false,\"contractRejected\":"<<(admitted?"false":"true")<<",\"recorderCommandsIssued\":0,\"commandsIssued\":0,\"submissions\":0,\"cleanupSucceeded\":true,\"illegalGpuWorkSubmitted\":false,\"selfOwnedPixelsCaptured\":false";waitFalseClaims();std::cout<<"}\n";return admitted?1:3;}
    auto f=std::make_unique<WaitFixture>();try{require(recipe(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT),"positive recipe rejected before command emission");f->runWait();const auto clean=f->cleanupWait();require(clean,"wait owner cleanup incomplete");
        std::cout<<"{\"stage\":\"dispatch-snapshot-wait\",\"passed\":true,\"unsupported\":false,\"selfOwnedPixelsCaptured\":true,\"sourceGpuGenerated\":true,\"sourceCpuUploaded\":false,\"distinctSameFamilyQueues\":true,\"markerPendingVerified\":"<<(f->markerPendingVerified?"true":"false")<<",\"markerCompleteVerified\":"<<(f->markerCompleteVerified?"true":"false")<<",\"commandTraceVerified\":"<<(f->traceVerified?"true":"false")
            <<",\"submissions\":"<<f->submissions<<",\"captures\":1,\"formats\":[37,83,130,122],\"verifiedPixels\":"<<f->verifiedPixels<<",\"sourceLayout\":5,\"cleanupSucceeded\":true";waitFalseClaims();std::cout<<"}\n";return 0;
    }catch(const Unsupported& e){const auto clean=f->cleanupWait();if(!clean)f.release();std::cerr<<e.what()<<'\n';std::cout<<"{\"stage\":\"dispatch-snapshot-wait\",\"passed\":false,\"unsupported\":true,\"selfOwnedPixelsCaptured\":false,\"cleanupSucceeded\":"<<(clean?"true":"false");waitFalseClaims();std::cout<<"}\n";return 2;
    }catch(const std::exception& e){const auto clean=f->cleanupWait();if(!clean)f.release();std::cerr<<e.what()<<'\n';std::cout<<"{\"stage\":\"dispatch-snapshot-wait\",\"passed\":false,\"unsupported\":false,\"selfOwnedPixelsCaptured\":false,\"cleanupSucceeded\":"<<(clean?"true":"false")<<",\"pendingOwnerRetained\":"<<(clean?"false":"true");waitFalseClaims();std::cout<<"}\n";return 1;}}
