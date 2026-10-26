// Host-signaled COMPUTE wait -> recorder ALL_COMMANDS -> TRANSFER on one queue.
// Source A is already GPU-complete before consumer submission. This does not
// test inter-queue synchronization or unfinished GPU-producer visibility.
#define main dispatch_snapshot_base_host_fixture_main
#include "dispatch_snapshot_gpu_test.cpp"
#undef main
#include <type_traits>

namespace {
template<class T>unsigned long long hostBits(T value){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<unsigned long long>(value);else return static_cast<unsigned long long>(value);}
struct HostTrace {unsigned kind{};VkPipelineStageFlags src{},dst{};std::array<VkImageMemoryBarrier,2> images{};VkImageCopy copy{};
    VkImage source{},target{};VkImageLayout sourceLayout{},targetLayout{};};
std::array<HostTrace,12> hostTrace{};std::size_t hostTraceCount{};
VKAPI_ATTR void VKAPI_CALL hostBarrier(VkCommandBuffer command,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
    std::uint32_t memoryCount,const VkMemoryBarrier* memory,std::uint32_t bufferCount,const VkBufferMemoryBarrier* buffers,std::uint32_t imageCount,const VkImageMemoryBarrier* images){
    require(hostTraceCount<hostTrace.size()&&imageCount==2,"host recorder barrier trace bounds");auto& row=hostTrace[hostTraceCount++];row.kind=0;row.src=src;row.dst=dst;std::memcpy(row.images.data(),images,sizeof(row.images));
    ++recorderCommandCalls;actualRecorderBarrier(command,src,dst,flags,memoryCount,memory,bufferCount,buffers,imageCount,images);}
VKAPI_ATTR void VKAPI_CALL hostCopy(VkCommandBuffer command,VkImage source,VkImageLayout sourceLayout,VkImage target,VkImageLayout targetLayout,std::uint32_t count,const VkImageCopy* regions){
    require(hostTraceCount<hostTrace.size()&&count==1,"host recorder copy trace bounds");auto& row=hostTrace[hostTraceCount++];row.kind=1;row.copy=*regions;row.source=source;row.target=target;row.sourceLayout=sourceLayout;row.targetLayout=targetLayout;
    ++recorderCommandCalls;actualRecorderCopy(command,source,sourceLayout,target,targetLayout,count,regions);}
// Admit only the requested host-wait recipe. Other recipes are not analyzed.
bool hostRecipe(VkPipelineStageFlags wait,VkPipelineStageFlags firstSrc,VkPipelineStageFlags firstDst){
    return wait==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&(firstSrc&VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)&&firstDst==VK_PIPELINE_STAGE_TRANSFER_BIT;
}
struct HostFixture : Fixture {
    VkFence initializationFence{};VkEvent marker{};bool initializationSubmitted{},initializationCompleted{},consumerSubmitted{},submissionUncertain{},traceVerified{};
    bool markerPendingVerified{},markerCompleteVerified{};unsigned markerSamples{};std::uint64_t hostBefore{},hostAfter{};VkResult initializationStatus{VK_NOT_READY};
    void initializeHost(){initialize();for(auto& source:sources)source.currentLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        std::uint32_t familyCount{};inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&familyCount,nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&familyCount,families.data());
        require(family<families.size()&&families[family].queueCount&&(families[family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT),"actual initialization/consumer family supports graphics and compute");
        // The original helper preallocates a recorder. Replace its unused/free
        // owner with the identical bounded target recipe plus counted callbacks.
        check(ds::destroy(recorder),"replace unrecorded recorder for host trace");recorder=nullptr;
        ds::Config config;config.device=device;config.queueFamily=family;config.queueFlags=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;config.memory=memory;config.planeCount=4;
        for(std::size_t n=0;n<sources.size();++n)config.planes[n]={sources[n].successfulCreateInfo.format,{sources[n].successfulCreateInfo.extent.width,sources[n].successfulCreateInfo.extent.height}};
        config.functions={dev<PFN_vkCreateImage>("vkCreateImage"),dev<PFN_vkDestroyImage>("vkDestroyImage"),dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements"),
            dev<PFN_vkAllocateMemory>("vkAllocateMemory"),dev<PFN_vkFreeMemory>("vkFreeMemory"),dev<PFN_vkBindImageMemory>("vkBindImageMemory"),hostBarrier,hostCopy,
            dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer"),dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")};
        ds::Result why;recorder=ds::create(config,&why);require(recorder!=nullptr,"create counted host recorder");check(why,"preallocate host recorder targets");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(dev<PFN_vkCreateFence>("vkCreateFence")(device,&fi,nullptr,&initializationFence),"create independent initialization fence");
        VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};check(dev<PFN_vkCreateEvent>("vkCreateEvent")(device,&ei,nullptr,&marker),"create host-wait transfer marker");
        std::printf("{\"event\":\"host_wait_runtime\",\"vendorId\":%u,\"deviceId\":%u,\"queueFamily\":%u,\"availableQueueCount\":%u,\"queueFlags\":%u,\"enabledQueueCount\":1,\"queue\":\"0x%llx\",\"hostGate\":\"0x%llx\",\"sourceLayout\":5,\"separateDepthStencilLayoutsEnabled\":false,\"observerLoaded\":false}\n",properties.vendorID,properties.deviceID,family,families[family].queueCount,families[family].queueFlags,hostBits(queue),hostBits(gate));}
    void initializeSourcePixels(){VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands[1],&begin),"begin independent source initialization");
        std::swap(commands[0],commands[1]);clearSources(true);std::swap(commands[0],commands[1]);
        check(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands[1]),"end GPU clearA and actual common layout5");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&commands[1];
        const auto result=dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&submit,initializationFence);
        if(result!=VK_SUCCESS)submissionUncertain=true;check(result,"initialization submit failed; consumer forbidden");initializationSubmitted=true;++submissions;
        check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&initializationFence,VK_TRUE,5'000'000'000),"finite independent initialization completion");
        initializationStatus=dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,initializationFence);require(initializationStatus==VK_SUCCESS,"initialization fence must actually query SUCCESS");
        initialized=initializationCompleted=true;
        std::printf("{\"event\":\"source_initialization_completed\",\"actualFenceStatus\":%d,\"waitSemaphoreCount\":0,\"sourceLayout\":5,\"gpuClearGeneratedA\":true,\"sourceCpuUploaded\":false,\"consumerSubmitted\":false}\n",int(initializationStatus));}
    void verifyHostTrace(){require(hostTraceCount==12&&recorderCommandCalls==12,"exact twelve-command four-plane recorder trace");
        for(std::size_t n=0;n<4;++n){const auto& before=hostTrace[n*3];const auto& copy=hostTrace[n*3+1];const auto& after=hostTrace[n*3+2];
            require(before.kind==0&&copy.kind==1&&after.kind==0&&hostRecipe(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,before.src,before.dst),"actual host recipe uses ALL_COMMANDS source and TRANSFER destination");
            require(before.images[0].oldLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&before.images[0].newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&before.images[0].srcAccessMask==(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT)&&
                before.images[0].dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT&&before.images[1].newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&before.images[1].dstAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT,"source/target transfer layout and access are exact");
            require(copy.source==sources[n].image&&copy.target==captures[0].targets[n].image&&copy.sourceLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&copy.targetLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&
                copy.copy.srcSubresource.aspectMask==aspect(sources[n].successfulCreateInfo.format)&&after.images[0].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,"actual copy identities, layouts and source restoration");
            require(before.images[0].subresourceRange.aspectMask==barrierAspect(sources[n].successfulCreateInfo.format)&&after.images[0].subresourceRange.aspectMask==barrierAspect(sources[n].successfulCreateInfo.format)&&
                after.images[0].srcAccessMask==VK_ACCESS_TRANSFER_READ_BIT&&after.images[0].dstAccessMask==sources[n].resumedAccess&&after.images[1].newLayout==captures[0].targets[n].finalLayout&&
                after.images[1].srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT&&after.images[1].dstAccessMask==VK_ACCESS_SHADER_READ_BIT,"D32S8 common-aspect restoration and owned readable layout/access are exact");
            std::printf("{\"event\":\"host_recorder_chain_trace\",\"plane\":%zu,\"format\":%u,\"srcStages\":%u,\"dstStages\":%u,\"sourceAccess\":%u,\"destinationAccess\":%u,\"targetDestinationAccess\":%u,\"copySourceLayout\":%u,\"copyTargetLayout\":%u,\"sourceOldLayout\":5,\"sourceRestoredLayout\":5,\"targetFinalLayout\":%u,\"barrierAspects\":%u,\"copyAspect\":%u,\"order\":[\"barrier\",\"copy\",\"restore\"]}\n",n,unsigned(sources[n].successfulCreateInfo.format),before.src,before.dst,before.images[0].srcAccessMask,before.images[0].dstAccessMask,
                before.images[1].dstAccessMask,unsigned(copy.sourceLayout),unsigned(copy.targetLayout),unsigned(after.images[1].newLayout),before.images[0].subresourceRange.aspectMask,copy.copy.srcSubresource.aspectMask);}
        traceVerified=true;}
    void recordHostConsumer(){require(initializationCompleted&&initializationStatus==VK_SUCCESS,"unfinished initialization cannot admit consumer recording");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands[0],&begin),"begin host-wait consumer");
        recordingGeneration=1;captureCount=1;captures[0]=ds::record(recorder,{commands[0],1,family},sources.size(),sources.data());check(captures[0].result,"record inline host-wait capture");++captureRecords;verifyHostTrace();
        dev<PFN_vkCmdSetEvent>("vkCmdSetEvent")(commands[0],marker,VK_PIPELINE_STAGE_TRANSFER_BIT);
        dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);dev<PFN_vkCmdDispatch>("vkCmdDispatch")(commands[0],1,1,1);clearSources(false);
        for(std::size_t n=0;n<4;++n){copyReadback(sources[n].image,n,0);const auto& own=captures[0].targets[n];
            transition(imageBarrier(own.image,own.format,own.finalLayout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            copyReadback(own.image,n,1);transition(imageBarrier(own.image,own.format,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,own.finalLayout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            transition(imageBarrier(sources[n].image,sources[n].successfulCreateInfo.format,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,sources[n].currentLayout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);}
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=readback;host.size=VK_WHOLE_SIZE;
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands[0],VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);check(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands[0]),"end fenced host-wait capture plus readback");}
    void runHost(){initializeHost();initializeSourcePixels();recordHostConsumer();require(initializationCompleted,"unfinished GPU producer cannot reach consumer submit");
        const std::uint64_t value=1;VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};timeline.waitSemaphoreValueCount=1;timeline.pWaitSemaphoreValues=&value;
        const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&timeline;submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&gate;submit.pWaitDstStageMask=&waitStage;submit.commandBufferCount=1;submit.pCommandBuffers=&commands[0];
        gateClosed=mightPending=true;const auto result=dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&submit,fence);consumerSubmitted=result==VK_SUCCESS;if(result!=VK_SUCCESS)submissionUncertain=true;
        const auto association=ds::trackSubmission(recorder,captures[0].ticket,{commands[0],1,family},fence,result);if(!association.accepted())submissionUncertain=true;
        check(association,"associate actual host consumer fence");check(result,"host consumer submission failed");++submissions;
        initializationStatus=dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,initializationFence);const auto pending=dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,fence);
        const auto timed=dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,5'000'000);check(dev<PFN_vkGetSemaphoreCounterValue>("vkGetSemaphoreCounterValue")(device,gate,&hostBefore),"query H while host consumer pending");
        require(initializationStatus==VK_SUCCESS&&pending==VK_NOT_READY&&timed==VK_TIMEOUT&&hostBefore==0,"only consumer is pending; GPU source initialization already completed");
        for(unsigned i=0;i<8;++i){require(dev<PFN_vkGetEventStatus>("vkGetEventStatus")(device,marker)==VK_EVENT_RESET,"post-copy TRANSFER marker must remain RESET before host signal");++markerSamples;Sleep(1);}
        markerPendingVerified=true;require(!mapped,"no mapping or host reads of pending readback");
        std::printf("{\"event\":\"host_wait_pending\",\"initializationFenceStatus\":%d,\"consumerFenceStatus\":%d,\"finiteWaitResult\":%d,\"hostGateValue\":%llu,\"consumerWaitStages\":%u,\"markerStatus\":%d,\"markerSamples\":%u,\"pendingHostRead\":false,\"gpuProducerPending\":false}\n",int(initializationStatus),int(pending),int(timed),static_cast<unsigned long long>(hostBefore),waitStage,int(VK_EVENT_RESET),markerSamples);
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};signal.semaphore=gate;signal.value=1;check(dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(device,&signal),"host signal H=1");gateClosed=false;
        check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,5'000'000'000),"bounded actual host consumer fence");mightPending=false;
        const auto completed=dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,fence);const auto event=dev<PFN_vkGetEventStatus>("vkGetEventStatus")(device,marker);require(completed==VK_SUCCESS&&event==VK_EVENT_SET,"actual completed fence and post-copy marker SET");markerCompleteVerified=true;
        check(dev<PFN_vkGetSemaphoreCounterValue>("vkGetSemaphoreCounterValue")(device,gate,&hostAfter),"query completed H");require(hostAfter==1,"actual H reaches one");check(ds::pollCapture(recorder,captures[0].ticket),"query actual host capture completion");verify();
        std::printf("{\"event\":\"host_wait_completed\",\"initializationFenceStatus\":%d,\"consumerFenceStatus\":%d,\"markerStatus\":%d,\"hostGateValue\":%llu}\n",int(initializationStatus),int(completed),int(event),static_cast<unsigned long long>(hostAfter));}
    bool cleanupHost()noexcept{try{if(device&&gdpa){if(gateClosed){VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};signal.semaphore=gate;signal.value=1;
                if(dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(device,&signal)!=VK_SUCCESS)return false;gateClosed=false;}
            if(submissionUncertain)return false;
            if(initializationSubmitted&&dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&initializationFence,VK_TRUE,2'500'000'000)!=VK_SUCCESS)return false;
            if(consumerSubmitted&&dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,2'500'000'000)!=VK_SUCCESS)return false;
            mightPending=false;if(marker){dev<PFN_vkDestroyEvent>("vkDestroyEvent")(device,marker,nullptr);marker={};}if(initializationFence){dev<PFN_vkDestroyFence>("vkDestroyFence")(device,initializationFence,nullptr);initializationFence={};}}
        return cleanup();}catch(...){return false;}}
};
void hostFalseClaims(){std::cout<<",\"interQueueSynchronizationTested\":false,\"gpuProducerPendingWhenConsumerSubmitted\":false,\"gameTested\":false,\"productionLinked\":false,\"gamePixelsCaptured\":false,\"xefgIntegrated\":false,\"gameSameFrameVerified\":false,\"motionSemanticsVerified\":false,\"depthConverted\":false";}
}
int main(int argc,char** argv){Watchdog bounded;bool negative=false;for(int i=1;i<argc;++i){if(std::strcmp(argv[i],"--reject-transfer-only-recipe")==0)negative=true;else return 4;}
    if(negative){const auto admitted=hostRecipe(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        std::cout<<"{\"stage\":\"dispatch-snapshot-host-wait\",\"passed\":false,\"contractRejected\":"<<(admitted?"false":"true")<<",\"hostWaitStageChainTested\":false,\"sourceInitializationCompletedBeforeConsumer\":false,\"commandsIssued\":0,\"recorderCommandsIssued\":0,\"submissions\":0,\"cleanupSucceeded\":true,\"illegalGpuWorkSubmitted\":false,\"selfOwnedPixelsCaptured\":false";hostFalseClaims();std::cout<<"}\n";return admitted?1:3;}
    auto f=std::make_unique<HostFixture>();try{require(hostRecipe(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT),"positive host recipe rejected before initialization");f->runHost();const auto clean=f->cleanupHost();require(clean,"host-wait owner cleanup incomplete");
        std::cout<<"{\"stage\":\"dispatch-snapshot-host-wait\",\"passed\":true,\"hostWaitStageChainTested\":true,\"sourceInitializationCompletedBeforeConsumer\":"<<(f->initializationCompleted?"true":"false")<<",\"selfOwnedPixelsCaptured\":true,\"sourceGpuGenerated\":true,\"sourceCpuUploaded\":false,\"enabledQueueCount\":1,\"markerPendingVerified\":"<<(f->markerPendingVerified?"true":"false")
            <<",\"markerCompleteVerified\":"<<(f->markerCompleteVerified?"true":"false")<<",\"commandTraceVerified\":"<<(f->traceVerified?"true":"false")<<",\"recorderCommandsIssued\":"<<recorderCommandCalls<<",\"submissions\":"<<f->submissions<<",\"captures\":1,\"formats\":[37,83,130,122],\"sourceLayout\":5,\"verifiedPixels\":"<<f->verifiedPixels<<",\"cleanupSucceeded\":true";hostFalseClaims();std::cout<<"}\n";return 0;
    }catch(const std::exception& e){const auto clean=f->cleanupHost();if(!clean)f.release();std::cerr<<e.what()<<'\n';std::cout<<"{\"stage\":\"dispatch-snapshot-host-wait\",\"passed\":false,\"hostWaitStageChainTested\":false,\"sourceInitializationCompletedBeforeConsumer\":false,\"selfOwnedPixelsCaptured\":false,\"cleanupSucceeded\":"<<(clean?"true":"false")<<",\"pendingOwnerRetained\":"<<(clean?"false":"true");hostFalseClaims();std::cout<<"}\n";return 1;}}
