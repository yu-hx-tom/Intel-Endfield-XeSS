#include "borrowed_capture_completion.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <windows.h>

namespace {std::atomic<bool> measured{};std::atomic<unsigned> allocations{};}
void* operator new(std::size_t n){if(measured)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}

namespace {
namespace bc=xess_vk::borrowed_capture_completion;
unsigned checks{},cases{};
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void ok(bc::Result result){require(result.accepted(),"completion result accepted");}
void error(bc::Result result,bc::Reason reason){require((result.reasons&bc::bit(reason))!=0,"specific refusal reported");}
template<class T>T h(std::uintptr_t value){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(value);else return static_cast<T>(value);}
struct Fixture;
Fixture* active{};
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence);
VKAPI_ATTR VkResult VKAPI_CALL fenceStatus(VkDevice,VkFence);
VKAPI_ATTR VkResult VKAPI_CALL wrapperSubmit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence);
struct Fixture {
    bc::Context context{};
    bc::PreparedOwner owner{};
    bc::RecordingAdmission admission{};
    bc::QueueWindow window{};
    std::array<VkCommandBuffer,2> commands{};
    std::array<bc::RecordingKey,2> membership{};
    VkSemaphore wait=h<VkSemaphore>(8),signal=h<VkSemaphore>(9);
    VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    std::uint64_t waitValue=7,signalValue=8;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    VkSubmitInfo app{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkFence appFence=h<VkFence>(44);
    VkResult appResult=VK_SUCCESS,ownResult=VK_SUCCESS,privateFenceResult=VK_NOT_READY;
    bc::Result lastCompletionResult{};
    unsigned apps{},owns{},fenceQueries{},stagingWrites{},imageValue{},stagingValue{};
    bool retireDuringOriginal{},retireDuringPrivate{},timelineEnabled{};
    explicit Fixture(VkCommandBufferUsageFlags flags=0){
        active=this;owner.device=h<VkDevice>(1);owner.queue=h<VkQueue>(2);owner.queueFamily=3;
        owner.image=h<VkImage>(10);owner.staging=h<VkBuffer>(11);owner.readbackCommand=h<VkCommandBuffer>(12);
        owner.completionFence=h<VkFence>(13);owner.originalQueueSubmit=submit;owner.originalFenceStatus=fenceStatus;
        owner.singlePhysicalDeviceKnown=owner.ordinaryUnprotectedQueueKnown=owner.uniqueExternallySynchronizedQueueKnown=true;
        owner.imageAndStagingOwned=owner.readbackExecutableKnown=owner.readbackSameFamilyKnown=true;
        owner.readbackWritesStagingOnce=owner.readbackRestoresOwnedFinalLayout=true;
        owner.transferToHostBarrierKnown=owner.hostCoherenceResponsibilityKnown=true;
        owner.inlineTargetStartsUndefined=owner.inlineTargetRestoresFinalLayout=owner.inlineSourceRestoresOriginalLayout=true;
        admission.key={owner.device,30,h<VkCommandBuffer>(40),50};admission.poolGeneration=60;
        admission.queueFamily=3;admission.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;admission.actualBeginFlags=flags;
        admission.allocationKnown=admission.poolKnown=admission.beginFlagsKnown=admission.currentlyRecordingKnown=true;
        admission.outsideRenderingKnown=admission.ordinaryUnprotectedPoolKnown=admission.controlSupported=true;
        window={owner.queue,owner.queueFamily,true,true};commands={admission.key.command,h<VkCommandBuffer>(41)};
        membership={admission.key,bc::RecordingKey{owner.device,30,commands[1],51}};
        timeline.waitSemaphoreValueCount=1;timeline.pWaitSemaphoreValues=&waitValue;
        timeline.signalSemaphoreValueCount=1;timeline.pSignalSemaphoreValues=&signalValue;
        app.waitSemaphoreCount=1;app.pWaitSemaphores=&wait;app.pWaitDstStageMask=&waitStage;
        app.commandBufferCount=2;app.pCommandBuffers=commands.data();app.signalSemaphoreCount=1;app.pSignalSemaphores=&signal;
    }
    void initialize(){active=this;ok(context.initialize(owner));}
    void record(){ok(context.armBeforeSourceCommands(admission));ok(context.sourceCommandsRecorded());}
    bc::SubmitObservation observation(){return {window,membership,appFence,true};}
    bc::Result invoke(){
        active=this;const auto beforeApps=apps,beforeOwns=owns;
        const auto original=wrapperSubmit(window.queue,1,&app,appFence);
        require(apps==beforeApps+1,"wrapper forwarded original once");
        require(original==appResult,"literal original VkResult retained even if private append fails");
        require(GetLastError()==0x12345678,"private submit preserves original LastError");
        require(owns-beforeOwns<=1,"at most one private append");return lastCompletionResult;
    }
    void complete(){privateFenceResult=VK_SUCCESS;ok(context.pollOwnCompletion());}
};
VKAPI_ATTR VkResult VKAPI_CALL wrapperSubmit(VkQueue queue,std::uint32_t count,const VkSubmitInfo* info,VkFence fence){
    auto& f=*active;const auto ticket=f.context.preSubmit(f.observation());
    const auto original=submit(queue,count,info,fence);
    f.lastCompletionResult=f.context.afterOriginalResult(ticket,original,f.window);
    return original;
}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue,std::uint32_t count,const VkSubmitInfo* info,VkFence fence){
    auto& f=*active;
    if(fence==f.owner.completionFence){
        ++f.owns;require(f.context.status().privateCallInProgress,"private submit marked in flight before driver");
        require(queue==f.owner.queue&&count==1&&info&&info->sType==VK_STRUCTURE_TYPE_SUBMIT_INFO,"private original queue submit exact shape");
        require(!info->pNext&&!info->waitSemaphoreCount&&!info->signalSemaphoreCount,"private append does not borrow waits/signals");
        if(f.owner.mode==bc::Mode::Readback){
            require(info->commandBufferCount==1&&info->pCommandBuffers&&info->pCommandBuffers[0]==f.owner.readbackCommand,"one exact private readback CB");
            if(f.ownResult==VK_SUCCESS){++f.stagingWrites;f.stagingValue=f.imageValue;}
        }else require(info->commandBufferCount==0,"marker mode submits no readback command");
        if(f.retireDuringPrivate){f.context.observeRecordingRetired(f.admission.key);require(f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"private in-flight retirement never cancels");}
        SetLastError(0xabcdef01);return f.ownResult;
    }
    ++f.apps;require(count==1&&info==&f.app&&fence==f.appFence,"original arrays/fence pointers unchanged");
    require(info->pCommandBuffers==f.commands.data()&&info->commandBufferCount==2&&info->pCommandBuffers[0]==f.admission.key.command,"original CB array unchanged");
    require(info->pWaitSemaphores==&f.wait&&info->pWaitDstStageMask==&f.waitStage&&info->pSignalSemaphores==&f.signal,"original semaphore/stage arrays unchanged");
    require(info->pNext==(f.timelineEnabled?&f.timeline:nullptr),"ordinary timeline pNext forwarded unchanged");
    if(f.appResult==VK_SUCCESS)f.imageValue=100+f.apps;
    if(f.retireDuringOriginal){
        require(f.context.status().originalCallInProgress,"matching call in-flight before original");
        f.context.observeRecordingRetired(f.admission.key);
        require(f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"reset during original is not never-submitted cancellation");
    }
    SetLastError(0x12345678);return f.appResult;
}
VKAPI_ATTR VkResult VKAPI_CALL fenceStatus(VkDevice device,VkFence fence){
    auto& f=*active;++f.fenceQueries;require(device==f.owner.device&&fence==f.owner.completionFence,"only actual private completion fence queried");
    SetLastError(0x98765432);return f.privateFenceResult;
}
void lifecycle(){++cases;Fixture f;f.initialize();f.record();ok(f.invoke());
    require(f.owns==1&&f.stagingWrites==1&&f.stagingValue==101,"first successful original appends one staging write");
    error(f.context.pollOwnCompletion(),bc::Reason::Pending);require(!f.context.status().readbackReady,"NOT_READY never reports readback ready");
    f.privateFenceResult=VK_TIMEOUT;error(f.context.pollOwnCompletion(),bc::Reason::Pending);
    f.context.observeRecordingRetired(f.admission.key);require(f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"retirement is not completion");
    f.complete();require(f.context.status().readbackReady,"actual private SUCCESS permits first readback");
    require(f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"host consumption is separate");
    ok(f.context.readbackConsumed());require(f.context.releaseDisposition()==bc::ReleaseDisposition::ReleaseCompleted,"retired completed consumed nonreplay can release");
    ok(f.context.markOwnerReleased());require(f.context.status().ownerReleased&&!f.context.status().readbackReady,"released owner never supplies new readback");
}
void replay(){++cases;Fixture f;f.initialize();f.record();ok(f.invoke());f.complete();ok(f.context.readbackConsumed());
    ok(f.invoke());require(f.apps==2&&f.owns==1&&f.stagingWrites==1&&f.imageValue==102&&f.stagingValue==101,"flags0 replay overwrites target but first staging remains stable");
    require(f.context.status().replays==1,"serial replay observed");f.context.observeQueueIdle(f.owner.queue,VK_SUCCESS);
    ok(f.invoke());f.context.observeRecordingRetired(f.admission.key);
    require(f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"older idle cannot cover later replay");
    f.context.observeQueueIdle(h<VkQueue>(99),VK_SUCCESS);require(f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"unrelated queue idle cannot release");
    f.context.observeQueueIdle(f.owner.queue,VK_SUCCESS);require(f.context.releaseDisposition()==bc::ReleaseDisposition::ReleaseCompleted,"actual latest queue idle plus retire/ownfence allows replay owner release");
    ok(f.context.markOwnerReleased());
}
void failures(){++cases;
    {Fixture f;f.initialize();f.record();f.appResult=VK_ERROR_OUT_OF_HOST_MEMORY;error(f.invoke(),bc::Reason::OriginalFailed);
     require(f.owns==0&&!f.context.status().readbackReady,"failed original appends no private submit");f.context.observeRecordingRetired(f.admission.key);
     f.context.observeDeviceIdle(f.owner.device,VK_SUCCESS);require(f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"failed original retains uncertain owner even after idle");}
    {Fixture f;f.initialize();f.record();f.ownResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;error(f.invoke(),bc::Reason::PrivateSubmitFailed);
     require(f.owns==1&&!f.context.status().readbackReady,"failed append retains owner");f.context.observeRecordingRetired(f.admission.key);f.context.observeQueueIdle(f.owner.queue,VK_SUCCESS);
     require(f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"append failure never becomes captured");}
    for(auto lostAt:{0,1,2}){Fixture f;f.initialize();f.record();if(lostAt==0)f.appResult=VK_ERROR_DEVICE_LOST;if(lostAt==1)f.ownResult=VK_ERROR_DEVICE_LOST;
     const auto result=f.invoke();if(lostAt<2)error(result,bc::Reason::DeviceLost);else{ok(result);f.privateFenceResult=VK_ERROR_DEVICE_LOST;error(f.context.pollOwnCompletion(),bc::Reason::DeviceLost);}
     require(!f.context.status().readbackReady,"device loss never grants completion");f.context.observeDeviceTerminated(f.owner.device);
     require(f.context.releaseDisposition()==bc::ReleaseDisposition::DropAfterDeviceTerminated,"terminated device drop is separate from completion");ok(f.context.markOwnerReleased());}
    {Fixture f;f.initialize();f.record();ok(f.invoke());f.privateFenceResult=VK_ERROR_UNKNOWN;error(f.context.pollOwnCompletion(),bc::Reason::FenceFailed);require(!f.context.status().readbackReady,"unknown fence error retains");}
    {Fixture f;f.initialize();f.record();ok(f.invoke());f.complete();ok(f.context.readbackConsumed());
     require(f.context.status().readbackReady,"first private readback was ready before global loss");
     f.window.queue=h<VkQueue>(99);f.admission.key.command=h<VkCommandBuffer>(90);
     f.commands[0]=f.admission.key.command;f.membership[0]=f.admission.key;
     f.appResult=VK_ERROR_DEVICE_LOST;error(f.invoke(),bc::Reason::DeviceLost);
     require(f.context.status().uncertain&&!f.context.status().readbackReady,"irrelevant other queue DEVICE_LOST invalidates whole device context");
     require(f.owns==1&&f.context.status().successfulOriginalSubmissions==1,"unrelated failing submission creates no capture association or private retry");}
}
void admission(){++cases;
    for(unsigned variant=0;variant<10;++variant){Fixture f;f.initialize();auto a=f.admission;
        if(variant==0)a.actualBeginFlags=VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        if(variant==1)a.actualBeginFlags=VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        if(variant==2)a.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;if(variant==3)a.beginFlagsKnown=false;
        if(variant==4)a.queueFamily=8;if(variant==5)a.ordinaryUnprotectedPoolKnown=false;
        if(variant==6)a.controlSupported=false;if(variant==7)a.currentlyRecordingKnown=false;
        if(variant==8)a.outsideRenderingKnown=false;if(variant==9)a.poolKnown=false;
        error(f.context.armBeforeSourceCommands(a),bc::Reason::UnsupportedRecording);
        require(!f.context.status().armed&&!f.context.status().sourceCommandsMayExist&&f.apps==0&&f.owns==0,"unsupported refused before source commands");}
    for(unsigned variant=0;variant<4;++variant){Fixture f;auto owner=f.owner;
        if(variant==0)owner.inlineTargetStartsUndefined=false;if(variant==1)owner.transferToHostBarrierKnown=false;
        if(variant==2)owner.readbackWritesStagingOnce=false;if(variant==3)owner.uniqueExternallySynchronizedQueueKnown=false;
        error(f.context.initialize(owner),bc::Reason::UnsupportedOwnerRecipe);require(!f.context.status().initialized&&f.fenceQueries==0,"invalid recipe rejected before driver calls");}
    {Fixture f;f.privateFenceResult=VK_SUCCESS;error(f.context.initialize(f.owner),bc::Reason::FenceNotUnsignaled);require(!f.context.status().initialized,"already-signaled private fence is not completion provenance");}
}
void association(){++cases;
    for(unsigned variant=0;variant<6;++variant){Fixture f;f.initialize();f.record();auto observation=f.observation();
        if(variant==0)f.membership[1]=f.membership[0];if(variant==1)observation.structureKnown=false;
        if(variant==2)observation.window.queueKnown=false;if(variant==3)observation.window.queue=h<VkQueue>(99);
        if(variant==4)observation.window.externallySynchronizedWindowKnown=false;if(variant==5)observation.applicationFence=f.owner.completionFence;
        const auto ticket=f.context.preSubmit(observation);require(ticket.relevant&&!ticket.associationKnown,"ambiguous associated submission rejected");
        const auto original=submit(f.window.queue,1,&f.app,f.appFence);require(!f.context.afterOriginalResult(ticket,original,observation.window).accepted(),"ambiguous association appends nothing");
        require(f.apps==1&&f.owns==0&&f.context.status().sourceCommandsMayExist,"late association refusal never falsely claims zero inline source commands");}
    {Fixture f;f.initialize();f.record();std::array<bc::RecordingKey,65> members{};members.fill(f.admission.key);auto o=f.observation();o.completeMembership=members;
     const auto ticket=f.context.preSubmit(o);require(ticket.reasons&bc::bit(bc::Reason::MembershipCapacity),"full membership bounded");
     const auto original=submit(f.window.queue,1,&f.app,f.appFence);require(!f.context.afterOriginalResult(ticket,original,f.window).accepted()&&f.owns==0,"capacity refusal retains original forwarding without append");}
    {Fixture f;f.initialize();f.record();f.timelineEnabled=true;f.app.pNext=&f.timeline;ok(f.invoke());require(f.owns==1,"ordinary timeline normalization is admissible");}
}
void retirementAndAba(){++cases;
    {Fixture f;f.initialize();f.record();f.context.observeRecordingRetired(f.admission.key);
     require(f.context.releaseDisposition()==bc::ReleaseDisposition::DiscardRetiredNeverSubmitted,"actual retired never-submitted lease can discard metadata without resetting app CB");ok(f.context.markOwnerReleased());require(f.apps==0&&f.owns==0,"cancel emits no Vulkan calls");}
    {Fixture f;f.initialize();f.record();f.retireDuringOriginal=true;ok(f.invoke());require(f.context.status().recordingRetired&&f.owns==1,"frozen key survives retirement during original result publication");
     require(f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"pending original-success owner retained");f.complete();ok(f.context.readbackConsumed());
     require(f.context.releaseDisposition()==bc::ReleaseDisposition::ReleaseCompleted,"exact original plus own completion resolves retired first capture");}
    {Fixture f;f.initialize();f.record();f.retireDuringPrivate=true;ok(f.invoke());require(f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"retire during append cannot discard in-flight own submit");}
    {Fixture f;f.initialize();f.record();auto admission=f.admission;admission.key.rpRecordingGeneration++;
     f.context.observeRecordingRetired(admission.key);require(!f.context.status().recordingRetired,"new generation retirement never retires old key");
     f.membership[0]=admission.key;const auto ticket=f.context.preSubmit(f.observation());require(!ticket.relevant,"same handle new generation never inherits old lease");
     ok(f.context.afterOriginalResult(ticket,VK_SUCCESS,f.window));require(f.owns==0&&f.context.status().successfulOriginalSubmissions==0,"unrelated generation cannot publish original-success history");}
    {Fixture first;first.initialize();first.record();const auto old=first.context.preSubmit(first.observation());
     Fixture second;second.admission.key.rpRecordingGeneration=100;second.membership[0]=second.admission.key;second.initialize();second.record();
     const auto current=second.context.preSubmit(second.observation());require(current.contextSerial!=old.contextSerial,"context ABA uses fresh serial");
     error(second.context.afterOriginalResult(old,VK_SUCCESS,second.window),bc::Reason::StaleTicket);require(second.owns==0,"old context ticket cannot append through new context");}
    {Fixture f;f.initialize();ok(f.context.armBeforeSourceCommands(f.admission));error(f.context.sourceRecordingFailed(false),bc::Reason::SourceRecordingFailed);
     require(!f.context.status().sourceCommandsMayExist,"actual zero-command record refusal preserved");require(f.context.releaseDisposition()!=bc::ReleaseDisposition::Retain,"unreferenced owner can discard");}
    {Fixture f;f.initialize();ok(f.context.armBeforeSourceCommands(f.admission));error(f.context.sourceRecordingFailed(true),bc::Reason::SourceRecordingFailed);
     f.context.observeRecordingRetired(f.admission.key);require(f.context.status().sourceCommandsMayExist&&f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"partial source recording stays uncertain");}
    {Fixture f;f.initialize();f.record();error(f.context.sourceRecordingFailed(false),bc::Reason::WrongState);
     require(f.context.status().armed&&f.context.status().sourceCommandsMayExist&&f.context.releaseDisposition()==bc::ReleaseDisposition::Retain,"committed inline commands cannot be converted into unrecorded cancellation");}
}
void oneTimeAndMarker(){++cases;
    {Fixture f(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);f.initialize();f.record();ok(f.invoke());f.complete();ok(f.context.readbackConsumed());
     error(f.invoke(),bc::Reason::OneTimeReplay);require(f.owns==1&&f.stagingWrites==1&&!f.context.status().readbackReady,"ONE_TIME repeat is association-refused and retained without new staging write");
     require(f.context.status().reasons&bc::bit(bc::Reason::OneTimeReplay),"ONE_TIME reason retained specifically");}
    {Fixture f;f.owner.mode=bc::Mode::FenceMarkerOnly;f.owner.staging=VK_NULL_HANDLE;f.owner.readbackCommand=VK_NULL_HANDLE;f.initialize();f.record();ok(f.invoke());f.complete();
     require(!f.context.status().readbackReady&&f.stagingWrites==0,"fence-only marker never supplies production readback success");error(f.context.readbackConsumed(),bc::Reason::WrongState);
     f.context.observeRecordingRetired(f.admission.key);require(f.context.releaseDisposition()==bc::ReleaseDisposition::ReleaseCompleted,"completed retired marker lease can release without pixel claim");}
}
void hotPath(){++cases;Fixture f;f.initialize();f.record();ok(f.invoke());f.complete();ok(f.context.readbackConsumed());
    allocations=0;measured=true;for(unsigned i=0;i<20000;++i){ok(f.invoke());ok(f.context.pollOwnCompletion());require(f.stagingValue==101&&f.stagingWrites==1,"many serial replays preserve first snapshot");}
    f.context.observeRecordingRetired(f.admission.key);f.context.observeDeviceIdle(f.owner.device,VK_SUCCESS);ok(f.context.markOwnerReleased());measured=false;
    require(allocations==0,"completion/submit/replay/poll/retire hot path allocates zero heap");require(f.owns==1&&f.context.stats().replaySubmissions==20000,"bounded lease never becomes a generalized readback queue");
}
}
int main(){try{lifecycle();replay();failures();admission();association();retirementAndAba();oneTimeAndMarker();hotPath();
    std::cout<<"{\"stage\":\"borrowed-capture-completion-cpu\",\"passed\":true,\"cases\":"<<cases<<",\"checks\":"<<checks
      <<",\"hotPathAllocations\":"<<allocations<<",\"storageBytes\":"<<sizeof(bc::Context)
      <<",\"realVulkanCalls\":0,\"gpuTouched\":false,\"gameTouched\":false,\"appObjectsResetOrFreed\":false,\"gamePixelsCaptured\":false,\"xefgIntegrated\":false}\n";return 0;
}catch(const std::exception& e){measured=false;std::cerr<<e.what()<<'\n';return 1;}}
