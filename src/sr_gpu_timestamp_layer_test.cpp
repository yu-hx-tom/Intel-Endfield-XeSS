// Read-only reuse of the existing production-wrapper CPU fixture.
#define main descriptor_layer_original_main
#include "descriptor_image_layer_test.cpp"
#undef main

namespace sr_timestamp_layer_fixture {
using namespace descriptor_layer_fixture;
namespace ro=xess_vk::render_pass_observer;
std::vector<unsigned> events;
unsigned queryCreates{},queryDestroys{},queryResets{},queryWrites{},queryReads{};
bool queryComplete{},querySubmitted{},queryDestroyThrows{},nativeDeviceDestroyed{};
std::uint64_t execution{};
const VkSubmitInfo* submittedInfos{};const VkSubmitInfo2* submittedInfos2{};VkFence submittedFence{};
std::vector<std::string> observationLines;
const char* queryInvariantFailure{};
void queryRequire(bool condition,const char* text){if(!condition&&!queryInvariantFailure)queryInvariantFailure=text;require(condition,text);}
Dispatch queueDispatch{&marker};const auto queue=reinterpret_cast<VkQueue>(&queueDispatch);
void observed(const char* text){SetLastError(0xAABBCCDD);observationLines.emplace_back(text);}
bool observedText(const char* text){return std::any_of(observationLines.begin(),observationLines.end(),[&](const auto& s){return s.find(text)!=std::string::npos;});}
void queryCall(){queryRequire(!nativeDeviceDestroyed,"no query API after the actual native device destruction");
    queryRequire(TryAcquireSRWLockExclusive(&stateLock)!=0,"metadata stateLock must not span query driver calls");ReleaseSRWLockExclusive(&stateLock);SetLastError(0xABCDEF01);}
VKAPI_ATTR VkResult VKAPI_CALL createQuery(VkDevice d,const VkQueryPoolCreateInfo* info,const VkAllocationCallbacks* a,VkQueryPool* out){
    queryCall();++queryCreates;queryRequire(d==device&&!a&&info->queryType==VK_QUERY_TYPE_TIMESTAMP&&info->queryCount==16,"wrapper creates bounded private timestamp pool");
    *out=handle<VkQueryPool>(0x8000+queryCreates);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyQuery(VkDevice,VkQueryPool,const VkAllocationCallbacks*){
    queryCall();queryRequire(!querySubmitted||queryComplete,"wrapper never destroys pending query references");
    if(queryDestroyThrows)throw std::runtime_error("injected cleanup failure");++queryDestroys;events.push_back(6);
}
VKAPI_ATTR void VKAPI_CALL resetQuery(VkCommandBuffer c,VkQueryPool,std::uint32_t first,std::uint32_t count){
    queryCall();queryRequire(c==command&&first%2==0&&count==2,"same original CB, owned query pair reset");++queryResets;events.push_back(1);
}
VKAPI_ATTR void VKAPI_CALL writeQuery(VkCommandBuffer c,VkPipelineStageFlagBits stage,VkQueryPool,std::uint32_t index){
    queryCall();queryRequire(c==command&&stage==(index%2?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),"same-CB timestamp bracket stages");
    ++queryWrites;events.push_back(index%2?4:2);
}
VKAPI_ATTR VkResult VKAPI_CALL readQuery(VkDevice,VkQueryPool,std::uint32_t first,std::uint32_t count,std::size_t bytes,void* data,VkDeviceSize stride,VkQueryResultFlags flags){
    queryCall();queryRequire(queryComplete&&querySubmitted,"production wrapper reads only successful completed original work");
    queryRequire(count==2&&bytes==32&&stride==16&&flags==(VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT),"production read has availability and no WAIT");
    ++queryReads;events.push_back(5);auto* out=static_cast<std::uint64_t*>(data);out[0]=execution*1000+first*10;out[1]=1;out[2]=out[0]+20;out[3]=1;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL direct(VkCommandBuffer c,std::uint32_t x,std::uint32_t y,std::uint32_t z){events.push_back(3);dispatch(c,x,y,z);}
VKAPI_ATTR void VKAPI_CALL indirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize offset){events.push_back(3);dispatchIndirect(c,b,offset);}
template<Tag tag> VKAPI_ATTR void VKAPI_CALL base(VkCommandBuffer c,std::uint32_t bx,std::uint32_t by,std::uint32_t bz,std::uint32_t x,std::uint32_t y,std::uint32_t z){
    events.push_back(3);dispatchBase<tag>(c,bx,by,bz,x,y,z);
}
VKAPI_ATTR VkResult VKAPI_CALL submitLegacy(VkQueue q,std::uint32_t n,const VkSubmitInfo* infos,VkFence fence){
    hit(GatedSubmit);require(q==queue&&n==1,"original legacy submission forwarded unchanged");submittedInfos=infos;submittedFence=fence;events.push_back(7);return chosen;
}
template<Tag tag> VKAPI_ATTR VkResult VKAPI_CALL submitModern(VkQueue q,std::uint32_t n,const VkSubmitInfo2* infos,VkFence fence){
    hit(tag);require(q==queue&&n==1,"original Submit2 forwarded unchanged");submittedInfos2=infos;submittedFence=fence;events.push_back(7);return chosen;
}
VKAPI_ATTR VkResult VKAPI_CALL idle(VkDevice d){hit(GatedDeviceIdle);require(d==device,"original idle device unchanged");
    if(chosen==VK_SUCCESS)queryComplete=true;return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL sparse(VkQueue q,std::uint32_t count,const VkBindSparseInfo* infos,VkFence fence){
    hit(GatedSparse);require(q==queue&&count==1&&infos&&fence==handle<VkFence>(0x7300),"original sparse parameters forwarded unchanged");return chosen;}
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice d,const VkAllocationCallbacks* a){
    hit(DestroyShader);require(d==device&&!a,"native device destroy forwarded once despite observation cleanup result");nativeDeviceDestroyed=true;
}
struct Fixture:descriptor_layer_fixture::Fixture {
    VkCommandPool pool{};xess_vk::sr_observation::DispatchTicket match;
    explicit Fixture(bool enabled=true):descriptor_layer_fixture::Fixture(false){
        queryCreates=queryDestroys=queryResets=queryWrites=queryReads=0;queryComplete=querySubmitted=queryDestroyThrows=nativeDeviceDestroyed=false;
        execution=0;events.clear();observationLines.clear();srGpuTestSink=observed;srTailTestSink=observed;srTailWindowOverride=enabled?1:0;trackingCpuTimingOverride=0;
        state.srObservationRequested=enabled;state.srGpuRequested=enabled;state.srGpuPeriod=2;state.srGpuFamilyCount=state.srGpuFamilyReported=2;
        state.srGpuFamilies[1].queueFlags=VK_QUEUE_COMPUTE_BIT;state.srGpuFamilies[1].timestampValidBits=36;
        state.srGpuFunctions={createQuery,destroyQuery,resetQuery,writeQuery,readQuery};
        state.cmdDispatch=direct;state.cmdDispatchIndirect=indirect;state.cmdDispatchBase=base<DispatchBase>;state.cmdDispatchBaseKhr=base<DispatchBaseKhr>;
        state.submit=submitLegacy;state.submit2=submitModern<GatedSubmit2>;state.submit2Khr=submitModern<GatedSubmit2Khr>;state.deviceWaitIdle=idle;state.destroy=destroyDevice;state.bindSparse=sparse;
        state.renderPass=ro::create(device,sink);require(state.renderPass!=nullptr,"actual RPO command lifecycle owner created");ro::queue(state.renderPass,queue,1);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pi.queueFamilyIndex=1;returned=0x7100;
        beginCheck();require(vkCreateCommandPool(device,&pi,nullptr,&pool)==VK_SUCCESS,"production pool creation");once(CreateCP);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;VkCommandBuffer allocated{};
        beginCheck();require(vkAllocateCommandBuffers(device,&ai,&allocated)==VK_SUCCESS&&allocated==command,"production command allocation");once(AllocateCB);
        begin();
    }
    void begin(){VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};beginCheck();require(vkBeginCommandBuffer(command,&bi)==VK_SUCCESS,"production recording begins");once(BeginCB);
        const auto f=ro::recordingFacts(state.renderPass,command);match={};match.count=1;match.callbacks[0]=17;match.command=f.command;match.recording=f.rpRecordingGeneration;
        match.window=1;match.ordinal=18;match.pipeline=19;match.pipelineGeneration=20;match.containsAccumulate=true;match.frame={21,{22,23,false},0,true};
        match.groups={3,4,5};match.renderExtent={1128,634};match.outputExtent={1920,1080};match.extentsKnown=true;srTailDispatchOverride=&match;}
    void end(){beginCheck();require(vkEndCommandBuffer(command)==VK_SUCCESS,"production recording ends");once(EndCB);}
    void execute(){queryComplete=querySubmitted=true;++execution;}
    void submit(unsigned api=0,VkResult result=VK_SUCCESS){
        chosen=result;const auto fence=handle<VkFence>(0x7200);beginCheck();
        if(!api){VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&command;
            require(vkQueueSubmit(queue,1,&info,fence)==result&&submittedInfos==&info&&submittedFence==fence,"legacy exact submit arrays/fence/result");once(GatedSubmit);}
        else {VkCommandBufferSubmitInfo entry{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};entry.commandBuffer=command;entry.deviceMask=1;
            VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=1;info.pCommandBufferInfos=&entry;
            require((api==1?vkQueueSubmit2(queue,1,&info,fence):vkQueueSubmit2KHR(queue,1,&info,fence))==result&&submittedInfos2==&info&&submittedFence==fence,"Submit2 exact arrays/fence/result");once(api==1?GatedSubmit2:GatedSubmit2Khr);}
        if(result==VK_SUCCESS){querySubmitted=true;queryComplete=false;}chosen=VK_SUCCESS;
    }
    void destroyThroughWrapper(){
        auto* owned=static_cast<DeviceState*>(HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(DeviceState)));
        require(owned!=nullptr,"CPU fixture heap owner matches production device teardown");*owned=state;devices=owned;
        state.srGpu=nullptr;state.srTailCommands=nullptr;state.renderPass=nullptr;
        beginCheck();vkDestroyDevice(device,nullptr);once(DestroyShader);require(devices==nullptr,"production wrapper removed and freed its heap device owner");
    }
    ~Fixture(){srTailDispatchOverride=nullptr;srTailClose(&state,"test_close");srGpuDeviceTerminated(&state);ro::destroy(state.renderPass);state.renderPass=nullptr;
        srTailWindowOverride=-1;srGpuTestSink=srTailTestSink=nullptr;}
};
void run(){
    {Fixture f(false);constexpr const char* added[]{"vkCmdPushConstants","vkCmdPushConstants2","vkCmdPushConstants2KHR","vkCmdSetViewport","vkCmdSetScissor",
        "vkCmdSetViewportWithCount","vkCmdSetScissorWithCount","vkCmdSetViewportWithCountEXT","vkCmdSetScissorWithCountEXT","vkCmdBindVertexBuffers",
        "vkCmdBindVertexBuffers2","vkCmdBindVertexBuffers2EXT","vkCmdBindIndexBuffer","vkCmdBindIndexBuffer2","vkCmdBindIndexBuffer2KHR",
        "vkCmdDrawIndirectCount","vkCmdDrawIndirectCountKHR","vkCmdDrawIndexedIndirectCount","vkCmdDrawIndexedIndirectCountKHR","vkCmdSetLineWidth",
        "vkCmdSetDepthBias","vkCmdSetDepthBounds","vkCmdSetStencilCompareMask","vkCmdSetStencilWriteMask","vkCmdSetStencilReference","vkCmdSetEvent",
        "vkCmdResetEvent","vkCmdResetEvent2","vkCmdResetEvent2KHR","vkCmdFillBuffer","vkCmdSetBlendConstants","vkCmdCopyBuffer","vkCmdCopyBufferToImage","vkCmdCopyImageToBuffer"};
        static_assert(std::size(added)==34);
        const auto downstream=reinterpret_cast<PFN_vkVoidFunction>(unknown);
        for(const auto* name:added){require(!deviceHook(name)&&!instanceHook(name),"SrObservation OFF omits only-observation wrappers from both hook tables");
            require(vkGetDeviceProcAddr(device,name)==downstream&&vkGetInstanceProcAddr(instance,name)==downstream,"OFF returns exact downstream, no daily viewport/pushconstant wrapper cost");}
        srTailWindowOverride=1;
        for(const auto* name:added){require(deviceHook(name)&&instanceHook(name),"mock dedicated SR enable exposes each additional observation wrapper");
            require(vkGetDeviceProcAddr(device,name)==deviceHook(name)&&vkGetInstanceProcAddr(instance,name)==instanceHook(name),"enabled tables return matching production wrapper");}
        srTailWindowOverride=0;
        for(const auto* name:{"vkCmdBindDescriptorSets","vkCmdDispatch","vkCmdPipelineBarrier"})
            require(deviceHook(name)&&instanceHook(name),"existing descriptor/T3 command hooks remain present in ordinary mode");
    }
    {Fixture f(false);AcquireSRWLockExclusive(&f.state.srGpuLock);beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);ReleaseSRWLockExclusive(&f.state.srGpuLock);
        f.end();AcquireSRWLockExclusive(&f.state.srGpuLock);f.submit();ReleaseSRWLockExclusive(&f.state.srGpuLock);
        require(!f.state.srGpu&&!queryCreates&&!queryWrites,"daily OFF dispatch/submit take no timestamp lock and emit no GPU query commands");}
    for(unsigned api=0;api<4;++api){Fixture f;events.clear();beginCheck();
        if(api==0)vkCmdDispatch(command,3,4,5);else if(api==1)vkCmdDispatchIndirect(command,handle<VkBuffer>(0x1234),64);
        else if(api==2)vkCmdDispatchBase(command,7,8,9,3,4,5);else vkCmdDispatchBaseKHR(command,7,8,9,3,4,5);
        once(api==0?DispatchDirect:api==1?DispatchIndirect:api==2?DispatchBase:DispatchBaseKhr);
        require(events==std::vector<unsigned>{1,2,3,4}&&queryCreates==1,"actual dispatch wrapper brackets exactly one unchanged original dispatch");
        if(api==1)require(last.numbers[0]==0x1234&&last.numbers[1]==64,"indirect buffer and offset unchanged");
        if(api>=2)require(last.numbers==std::array<std::uint64_t,8>{7,8,9,3,4,5,0,0},"base coordinates and groups unchanged");
        f.end();beginCheck();require(vkResetCommandBuffer(command,0)==VK_SUCCESS,"unsubmitted actual reset");once(ResetCB);
        require(queryDestroys==1&&!queryReads,"never-submitted recording discarded after real reset without an uninitialized read");
    }
    for(unsigned api=0;api<3;++api){Fixture f;beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);f.end();f.submit(api);
        require(!queryReads&&!f.state.srGpuIdleComplete,"success submit alone cannot poll fresh query");f.execute();events.clear();f.submit(api);
        require(events==std::vector<unsigned>{5,7}&&queryReads==1,"actual replay wrapper reads old execution before native resubmission, never after");
        require(observedText("\"render_width\":1128")&&observedText("\"output_width\":1920")&&observedText("\"groups\":[3,4,5]")&&observedText("\"ticket_generation\":23"),"production Sample JSON keeps actual metadata");
        srTailWindowOverride=0;srTailPoll(&f.state);require(!queryDestroys&&f.state.srGpu&&observedText("\"retained_at_stop\":1"),"real window close retains pending replay query references");
        f.execute();beginCheck();require(vkDeviceWaitIdle(device)==VK_SUCCESS,"original idle completion observed");once(GatedDeviceIdle);
        require(queryReads==2&&!queryDestroys&&f.state.srGpuIdleComplete&&observedText("native_device_idle_success"),"actual idle reads without destroying still-replayable pool");
        beginCheck();require(vkResetCommandPool(device,f.pool,0)==VK_SUCCESS,"original pool reset");once(ResetCP);
        require(queryDestroys==1&&f.state.srGpu->drained(),"actual successful pool reset retires query resource once");
    }
    {Fixture f;beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);f.end();f.submit(0,VK_ERROR_OUT_OF_DEVICE_MEMORY);
        require(!queryReads,"failed original submit cannot read queries");beginCheck();vkFreeCommandBuffers(device,f.pool,1,&command);once(FreeCB);
        require(queryDestroys==1&&!queryReads,"actual free retires original exact generation after failed submit");}
    {Fixture f;f.match.extentsKnown=false;beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);
        require(!queryCreates&&f.state.srGpu->stats().skipped[std::size_t(sgt::Skip::Identity)]==1,"wrapper refuses unknown actual extents without guessing");}
    {Fixture f;f.state.srGpuFamilyCount=0;beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);
        require(!queryCreates&&!f.state.srGpu&&observedText("queue_family_metadata_or_owner_unavailable"),"missing/over-limit physical family metadata refuses query insertion explicitly");}
    {Fixture f;f.state.srGpuFamilyCount=0;f.state.srGpuFamilyReported=65;beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);
        require(!queryCreates&&!f.state.srGpu&&observedText("queue_family_properties_capacity")&&observedText("\"reported_family_count\":65"),"over-limit family enumeration is explicit, never truncated into invented support");}
    {Fixture f;f.match.containsRcas=true;beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);require(!queryCreates&&!f.state.srGpu,"ambiguous pass ticket never enables query recording");}
    {Fixture f;f.match.containsAccumulate=false;f.match.containsRcas=true;beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);
        f.end();f.submit();f.execute();beginCheck();vkDestroyCommandPool(device,f.pool,nullptr);once(DestroyCP);
        require(queryReads==1&&queryDestroys==1&&observedText("\"pass\":\"rcas\""),"RCAS gets a real separate pass sample after native pool destruction");}
    {Fixture f;beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);f.end();f.submit();
        f.destroyThroughWrapper();require(!queryReads&&!queryDestroys&&observedText("device_destroy_no_completed_idle_samples_may_be_unread")&&nativeDeviceDestroyed,
            "without native idle actual destroy forwards once, records unread samples, and only drops host metadata afterward");}
    {Fixture f;beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);f.end();f.submit();f.execute();beginCheck();require(vkDeviceWaitIdle(device)==VK_SUCCESS,"real idle precedes final cleanup");once(GatedDeviceIdle);
        queryDestroyThrows=true;f.destroyThroughWrapper();require(!queryDestroys&&nativeDeviceDestroyed&&!f.state.srGpu,
            "native destroy wrapper still runs once and preserves LastError after failed observation cleanup");}
    {Fixture f;beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);f.end();f.submit();f.execute();beginCheck();require(vkDeviceWaitIdle(device)==VK_SUCCESS,"native idle before sparse");once(GatedDeviceIdle);
        const auto oldWrites=queryWrites,oldReads=queryReads;VkBindSparseInfo info{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};beginCheck();
        require(vkQueueBindSparse(queue,1,&info,handle<VkFence>(0x7300))==VK_SUCCESS,"original sparse result");once(GatedSparse);
        require(!f.state.srGpuIdleComplete&&queryReads==oldReads&&queryWrites==oldWrites,"sparse only invalidates idle evidence, no timestamp or invented CB membership");
        querySubmitted=false;beginCheck();require(vkResetCommandBuffer(command,0)==VK_SUCCESS,"actual reset after sparse");once(ResetCB);}
}
}
int main(){using namespace sr_timestamp_layer_fixture;SetEnvironmentVariableW(L"XESS_OBSERVER_LOG",nullptr);
    SetEnvironmentVariableW(L"XESS_RENDER_TAIL_DIAGNOSTIC",nullptr);
    try{run();if(queryInvariantFailure)throw std::runtime_error(queryInvariantFailure);
    std::cout<<"SR timestamp production-wrapper CPU: "<<descriptor_layer_fixture::checks<<" checks passed, no GPU/game/loader\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
