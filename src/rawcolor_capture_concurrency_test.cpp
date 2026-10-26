// Event-controlled calls through the actual production wrappers. No Vulkan
// loader, GPU, game, installed layer, or scheduler timing assumptions.
#define main rawcolor_serial_fixture_main
#include "rawcolor_capture_layer_test.cpp"
#undef main
#include <atomic>
#include <thread>

namespace rawcolor_concurrency_fixture {
namespace layer=recording_layer_fixture;
namespace driver=rawcolor_cpu_fixture;
HANDLE entered{},releaseDriver{},retired{};
std::atomic<unsigned> appCalls{},resetCalls{};
const void* expectedInfos{};VkFence expectedFence{};std::uint32_t expectedCount{};
std::atomic<bool> exactArgs{true};
VkResult appResult=VK_SUCCESS;
VKAPI_ATTR VkResult VKAPI_CALL blockedSubmit(VkQueue q,std::uint32_t n,const VkSubmitInfo* i,VkFence fence){
    ++appCalls;if(q!=layer::queue||n!=expectedCount||i!=expectedInfos||fence!=expectedFence)exactArgs=false;
    SetEvent(entered);if(WaitForSingleObject(releaseDriver,5000)!=WAIT_OBJECT_0)exactArgs=false;
    SetLastError(0x87654321);return appResult;
}
VKAPI_ATTR VkResult VKAPI_CALL unrelatedReset(VkCommandBuffer,VkCommandBufferResetFlags){++resetCalls;SetLastError(0x23456789);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL blockedSubmit2(VkQueue q,std::uint32_t n,const VkSubmitInfo2* i,VkFence fence){
    ++appCalls;if(q!=layer::queue||n!=expectedCount||i!=expectedInfos||fence!=expectedFence)exactArgs=false;
    SetEvent(entered);if(WaitForSingleObject(releaseDriver,5000)!=WAIT_OBJECT_0)exactArgs=false;SetLastError(0x87654321);return appResult;
}
void events(){entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);releaseDriver=CreateEventW(nullptr,TRUE,FALSE,nullptr);retired=CreateEventW(nullptr,TRUE,FALSE,nullptr);rawcolor_layer_fixture::need(entered&&releaseDriver&&retired,"events created");appCalls=resetCalls=0;exactArgs=true;}
void closeEvents(){CloseHandle(entered);CloseHandle(releaseDriver);CloseHandle(retired);}
void check(bool value,const char* why){rawcolor_layer_fixture::need(value,why);}
void overlap(bool expectOld){
    layer::Fixture f;driver::reset();driver::device=layer::device;driver::queue=layer::queue;driver::borrowedCommand=layer::command;
    f.state.rawQueue=layer::queue;f.state.rawUniqueQueue=true;f.state.rawProbe=raw::create(driver::config());f.state.submit=blockedSubmit;f.state.resetCommandBuffer=unrelatedReset;
    wchar_t directory[1024]{},marker[1024]{};check(GetTempPathW(1024,directory)&&GetTempFileNameW(directory,L"xrc",0,marker),"own process marker path");DeleteFileW(marker);std::wmemcpy(f.state.rawArmPath,marker,std::wcslen(marker)+1);
    events();
    layer::DispatchObject unrelated{&layer::marker};const auto other=reinterpret_cast<VkCommandBuffer>(&unrelated);
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};expectedInfos=&info;expectedFence=layer::handle<VkFence>(999);expectedCount=0;
    VkResult result=VK_NOT_READY;DWORD error{};std::thread submitter([&]{result=vkQueueSubmit(layer::queue,0,&info,expectedFence);error=GetLastError();});
    check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0,"original QueueSubmit entered event");
    std::thread recorder([&]{vkResetCommandBuffer(other,0);SetEvent(retired);});
    const bool finished=WaitForSingleObject(retired,2000)==WAIT_OBJECT_0;
    SetEvent(releaseDriver);recorder.join();submitter.join();check(finished,"unrelated recording completes while original driver waits");
    check(result==appResult&&error==0x87654321&&appCalls==1&&resetCalls==1&&exactArgs,"original args result fence LastError exact once");
    constexpr char literal[]="END_FIELD_RAWCOLOR_ONCE_V1";const auto file=CreateFileW(marker,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);DWORD bytes{};
    check(file!=INVALID_HANDLE_VALUE&&WriteFile(file,literal,sizeof(literal)-1,&bytes,nullptr)&&bytes==sizeof(literal)-1,"exact marker written after contention");CloseHandle(file);
    f.dispatch();const bool missed=InterlockedCompareExchange(&f.state.rawObservationMissed,0,0)!=0;
    check(expectOld?missed&&!f.state.rawArmed:!missed&&f.state.rawArmed,expectOld?"old wrapper lock contention permanently bypasses valid marker":"new wrapper ordinary contention leaves valid marker armable");
    check(raw::status(f.state.rawProbe).sourceQueryAttempts==0&&!driver::leases,"nonmatching pre-arm recording consumes no query or lease");
    raw::destroy(f.state.rawProbe);f.state.rawProbe=nullptr;DeleteFileW(marker);closeEvents();
    std::cout<<"{\"stage\":\"rawcolor-wrapper-concurrency\",\"oldDefectExpected\":"<<(expectOld?"true":"false")<<",\"driverWaitRecorderCompleted\":"<<(finished?"true":"false")<<",\"rawObservationMissed\":"<<(missed?"true":"false")<<",\"markerArmed\":"<<(f.state.rawArmed?"true":"false")<<",\"passed\":true,\"gpuTouched\":false,\"gameTouched\":false}\n";
}
void beforeCapture(layer::Fixture& f,VkCommandBuffer,VkQueue){
    f.state.rawArmed=false;wchar_t directory[1024]{},marker[1024]{};check(GetTempPathW(1024,directory)&&GetTempFileNameW(directory,L"xrc",0,marker),"capture own marker path");DeleteFileW(marker);std::wmemcpy(f.state.rawArmPath,marker,std::wcslen(marker)+1);
    const auto original=f.state.submit;const auto reset=f.state.resetCommandBuffer;f.state.submit=blockedSubmit;f.state.resetCommandBuffer=unrelatedReset;events();expectedCount=0;VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};expectedInfos=&info;expectedFence=layer::handle<VkFence>(999);appResult=VK_SUCCESS;
    VkResult result=VK_NOT_READY;DWORD error{};std::thread submitter([&]{result=vkQueueSubmit(layer::queue,0,&info,expectedFence);error=GetLastError();});
    check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0,"capture original waits");layer::DispatchObject object{&layer::marker};const auto other=reinterpret_cast<VkCommandBuffer>(&object);
    std::thread recorder([&]{vkResetCommandBuffer(other,0);SetEvent(retired);});const bool finished=WaitForSingleObject(retired,2000)==WAIT_OBJECT_0;SetEvent(releaseDriver);recorder.join();submitter.join();
    check(finished&&result==VK_SUCCESS&&error==0x87654321&&exactArgs&&appCalls==1,"capture recorder progresses inside original wait");check(!f.state.rawObservationMissed&&!f.state.rawArmed&&!driver::leases&&!raw::status(f.state.rawProbe).sourceQueryAttempts,"pre-arm unrelated lifecycle consumes no demand");
    constexpr char literal[]="END_FIELD_RAWCOLOR_ONCE_V1";const auto file=CreateFileW(marker,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);DWORD bytes{};check(file!=INVALID_HANDLE_VALUE&&WriteFile(file,literal,sizeof(literal)-1,&bytes,nullptr)&&bytes==sizeof(literal)-1,"eligible actual dispatch exact marker");CloseHandle(file);
    f.state.submit=original;f.state.resetCommandBuffer=reset;closeEvents();
    // Delete after the first accepted dispatch has latched it, in captureReady.
}
void capturedOverlap(layer::Fixture& f,VkCommandBuffer command,VkQueue q){
    check(f.state.rawArmed&&driver::leases==1&&driver::privateSubmits==1&&raw::status(f.state.rawProbe).sourceInserted,"first eligible actual wrapper armed and source/private1");DeleteFileW(f.state.rawArmPath);
    const auto submit=f.state.submit;const auto submit2=f.state.submit2;const auto submit2khr=f.state.submit2Khr;const auto reset=f.state.resetCommandBuffer;
    f.state.submit=blockedSubmit;f.state.submit2=f.state.submit2Khr=blockedSubmit2;f.state.resetCommandBuffer=unrelatedReset;
    for(unsigned variant=0;variant<3;++variant){events();VkSubmitInfo one{VK_STRUCTURE_TYPE_SUBMIT_INFO};one.commandBufferCount=1;one.pCommandBuffers=&command;
        VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=command;VkSubmitInfo2 two{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};two.commandBufferInfoCount=1;two.pCommandBufferInfos=&cb;
        expectedInfos=variant?static_cast<const void*>(&two):static_cast<const void*>(&one);expectedCount=1;expectedFence=layer::handle<VkFence>(999);VkResult result=VK_NOT_READY;DWORD error{};
        std::thread submitter([&]{result=variant==0?vkQueueSubmit(q,1,&one,expectedFence):variant==1?vkQueueSubmit2(q,1,&two,expectedFence):vkQueueSubmit2KHR(q,1,&two,expectedFence);error=GetLastError();});
        check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0,"captured original submit waits");layer::DispatchObject object{&layer::marker};const auto other=reinterpret_cast<VkCommandBuffer>(&object);
        std::thread recorder([&]{vkResetCommandBuffer(other,0);SetEvent(retired);});const bool finished=WaitForSingleObject(retired,2000)==WAIT_OBJECT_0;SetEvent(releaseDriver);recorder.join();submitter.join();
        const auto status=raw::status(f.state.rawProbe);check(finished&&appCalls==1&&resetCalls==1&&exactArgs&&result==VK_SUCCESS&&error==0x87654321,"core1/core2/KHR original forwarding and unrelated concurrency");
        check(!f.state.rawObservationMissed&&!status.completion.uncertain&&!status.completion.recordingRetired&&!status.completion.originalCallInProgress&&driver::privateSubmits==1&&driver::inlineCopies==1,"inserted lease survives unrelated retirement and flags0 replay keeps first staging");closeEvents();
    }
    f.state.submit=submit;f.state.submit2=submit2;f.state.submit2Khr=submit2khr;f.state.resetCommandBuffer=reset;
}
void prime(layer::Fixture& f){
    driver::reset();driver::device=layer::device;driver::queue=layer::queue;driver::borrowedCommand=layer::command;driver::expectedSource=driver::handle<VkImage>(200);
    f.beginInfo.flags=0;f.start();
    f.state.rawQueue=layer::queue;f.state.rawUniqueQueue=true;f.state.rawProbe=raw::create(driver::config());
    auto facts=driver::facts();const auto key=rawKey(&f.state,layer::command);auto& recording=facts.recording;recording.device=bits(key.device);recording.contextIncarnation=key.contextIncarnation;recording.command=bits(key.command);recording.rpRecordingGeneration=key.rpRecordingGeneration;
    facts.evidence.command.handle=recording.command;recording.beginUsageFlags=0;auto& layout=facts.layouts[1];layout.device=recording.device;layout.contextIncarnation=recording.contextIncarnation;layout.command=recording.command;layout.rpRecordingGeneration=recording.rpRecordingGeneration;
    {RawOperation operation(&f.state,true);const auto result=raw::preDispatch(f.state.rawProbe,facts);if(!result.commandsInserted)std::cerr<<"prime reasons="<<result.reasons<<" key="<<key.contextIncarnation<<":"<<key.rpRecordingGeneration<<"\n";check(result.commandsInserted,"fresh one lease insertion fixture");}
}
VKAPI_ATTR VkResult VKAPI_CALL literalSubmit(VkQueue q,std::uint32_t n,const VkSubmitInfo* i,VkFence fence){++appCalls;if(q!=layer::queue||n!=expectedCount||i!=expectedInfos||fence!=expectedFence)exactArgs=false;SetLastError(0x87654321);return appResult;}
void unsafeCases(){
    for(unsigned scenario=0;scenario<4;++scenario){layer::Fixture f;prime(f);f.state.submit=literalSubmit;VkCommandBuffer command=layer::command;VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&command;VkDeviceGroupSubmitInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO};
        if(scenario==0)info.pNext=&group;else if(scenario==1)info.commandBufferCount=65;else if(scenario==2)info.sType=VK_STRUCTURE_TYPE_MAX_ENUM;
        expectedCount=1;expectedInfos=&info;expectedFence=layer::handle<VkFence>(999);appCalls=0;exactArgs=true;appResult=scenario==3?VK_ERROR_DEVICE_LOST:VK_SUCCESS;
        const auto result=vkQueueSubmit(layer::queue,1,&info,expectedFence);const auto error=GetLastError();const auto status=raw::status(f.state.rawProbe);
        check(appCalls==1&&exactArgs&&result==appResult&&error==0x87654321,"malformed/unknown/deviceLost exact unchanged original");
        check(status.completion.uncertain&&!status.completion.readbackReady&&status.completion.release==borrowed::ReleaseDisposition::Retain&&!driver::privateSubmits&&raw::pixels(f.state.rawProbe).empty()&&!raw::destroy(f.state.rawProbe),"true unknown association and DeviceLost retain pending lease without pixels");
        driver::waitResult=VK_SUCCESS;check(raw::beforeDeviceDestroy(f.state.rawProbe,2'000'000'000ull).accepted()&&raw::destroy(f.state.rawProbe),"only explicit own drain proves safe uncertain teardown");f.state.rawProbe=nullptr;
    }
    appResult=VK_SUCCESS;
}
HANDLE queueOriginal{};std::atomic<unsigned> getQueueCalls{};
VKAPI_ATTR void VKAPI_CALL getQueue(VkDevice d,std::uint32_t family,std::uint32_t index,VkQueue* out){++getQueueCalls;if(d!=layer::device||family||index)exactArgs=false;*out=layer::queue;SetEvent(queueOriginal);SetLastError(0x98765432);}
void firstQueueContext(){
    layer::Fixture f;f.state.rawEnabled=f.state.rawUniqueQueue=true;f.state.getQueue=getQueue;events();queueOriginal=CreateEventW(nullptr,TRUE,FALSE,nullptr);getQueueCalls=0;
    std::thread holder([&]{RawOperation operation(&f.state,true);SetEvent(entered);WaitForSingleObject(releaseDriver,5000);});check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0,"first queue context competing lock held");
    VkQueue q{};DWORD error{};std::thread getter([&]{vkGetDeviceQueue(layer::device,0,0,&q);error=GetLastError();SetEvent(retired);});check(WaitForSingleObject(queueOriginal,5000)==WAIT_OBJECT_0,"single original getQueue observed during contention");
    SetEvent(releaseDriver);getter.join();holder.join();check(q==layer::queue&&getQueueCalls==1&&error==0x98765432&&exactArgs&&f.state.rawProbe&&f.state.rawQueue==q&&!f.state.rawObservationMissed,"single GetQueue creates Context after serialization without permanent missed queue");
    raw::destroy(f.state.rawProbe);f.state.rawProbe=nullptr;CloseHandle(queueOriginal);closeEvents();
}
void leaseDuringOriginal(){
    for(bool deviceLost:{false,true}){layer::Fixture f;f.state.submit=blockedSubmit;events();expectedCount=0;VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};expectedInfos=&info;expectedFence=layer::handle<VkFence>(999);appResult=deviceLost?VK_ERROR_DEVICE_LOST:VK_SUCCESS;
        VkResult result=VK_NOT_READY;DWORD error{};std::thread submitter([&]{result=vkQueueSubmit(layer::queue,0,&info,expectedFence);error=GetLastError();});check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0,"unrelated original before fresh capture");
        prime(f);SetEvent(releaseDriver);submitter.join();closeEvents();const auto status=raw::status(f.state.rawProbe);check(result==appResult&&error==0x87654321&&appCalls==1&&exactArgs,"new lease outside original args result error exact once");
        check(!driver::privateSubmits&&!status.fencePolls&&!status.completion.readbackReady,"older unrelated original never privately appends or polls new lease");
        if(deviceLost)check(status.observationUnsafe&&status.completion.uncertain&&status.completion.release==borrowed::ReleaseDisposition::Retain,"DeviceLost with pre-lease ticket retains new lease explicitly unsafe");
        else check(!status.reasons&&!status.completion.uncertain,"old unrelated successful call does not invent completion refusal");
        check(raw::beforeDeviceDestroy(f.state.rawProbe,2'000'000'000ull).accepted()&&raw::destroy(f.state.rawProbe),"new lease retained until explicit own drain");f.state.rawProbe=nullptr;
    }appResult=VK_SUCCESS;
}
void busyAndRelated(){
    layer::Fixture f;prime(f);events();std::thread holder([&]{RawOperation operation(&f.state,true);SetEvent(entered);WaitForSingleObject(releaseDriver,5000);});
    check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0,"context lock holder entered");const auto before=layer::di::stats(f.state.descriptors).dispatchCalls;f.dispatch();
    check(layer::di::stats(f.state.descriptors).dispatchCalls==before+1&&!f.state.rawObservationMissed&&f.state.rawBusySkips==1&&!raw::status(f.state.rawProbe).sourceQueryAttempts,"busy dispatch preserves single descriptor observation without raw demand");SetEvent(releaseDriver);holder.join();closeEvents();
    f.state.submit=blockedSubmit;f.state.resetCommandBuffer=unrelatedReset;VkCommandBuffer command=layer::command;VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&command;
    events();expectedInfos=&info;expectedCount=1;expectedFence=layer::handle<VkFence>(999);std::thread submitter([&]{vkQueueSubmit(layer::queue,1,&info,expectedFence);});check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0,"related submit in progress");
    std::thread recorder([&]{vkResetCommandBuffer(command,0);SetEvent(retired);});const bool finished=WaitForSingleObject(retired,2000)==WAIT_OBJECT_0;SetEvent(releaseDriver);recorder.join();submitter.join();
    check(finished&&raw::status(f.state.rawProbe).completion.recordingRetired&&!f.state.rawObservationMissed&&driver::privateSubmits==1,"related retirement event delivered across original wait, first append once");closeEvents();
    // Deliberate same-thread reentry cannot deadlock or be misreported as ordinary
    // contention. The actual reset wrapper loses a lifecycle event at this gate.
    {RawOperation operation(&f.state,true);vkResetCommandBuffer(command,0);}const auto status=raw::status(f.state.rawProbe);
    check(status.observationUnsafe&&status.completion.uncertain&&!status.completion.readbackReady&&status.completion.release==borrowed::ReleaseDisposition::Retain&&raw::pixels(f.state.rawProbe).empty()&&!raw::destroy(f.state.rawProbe),"same thread actual lifecycle reentry explicitly unsafe and retained");
    check(raw::beforeDeviceDestroy(f.state.rawProbe,2'000'000'000ull).accepted()&&raw::destroy(f.state.rawProbe),"unsafe private owner freed only after real own drain fixture");f.state.rawProbe=nullptr;
}
VKAPI_ATTR VkResult VKAPI_CALL privateSubmitReentry(VkQueue q,std::uint32_t count,const VkSubmitInfo* info,VkFence fence){
    const auto result=driver::submit(q,count,info,fence);if(info->commandBufferCount)vkResetCommandBuffer(layer::command,0);return result;
}
void privateRecipeReentry(){
    layer::Fixture f;prime(f);auto* previous=f.state.rawProbe;check(raw::beforeDeviceDestroy(previous,2'000'000'000ull).accepted()&&raw::destroy(previous),"replace preparer fixture after own drain");f.state.rawProbe=nullptr;
    driver::reset();auto config=driver::config();config.functions.submit=privateSubmitReentry;f.state.rawProbe=raw::create(config);f.state.resetCommandBuffer=unrelatedReset;
    auto facts=driver::facts();const auto key=rawKey(&f.state,layer::command);facts.recording.device=bits(key.device);facts.recording.contextIncarnation=key.contextIncarnation;facts.recording.command=bits(key.command);facts.recording.rpRecordingGeneration=key.rpRecordingGeneration;facts.evidence.command.handle=facts.recording.command;
    facts.layouts[1].device=facts.recording.device;facts.layouts[1].contextIncarnation=facts.recording.contextIncarnation;facts.layouts[1].command=facts.recording.command;facts.layouts[1].rpRecordingGeneration=facts.recording.rpRecordingGeneration;
    {RawOperation operation(&f.state,true);check(raw::preDispatch(f.state.rawProbe,facts).commandsInserted,"private callback reentry one lease inserted");}
    f.state.submit=literalSubmit;VkCommandBuffer command=layer::command;VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&command;expectedInfos=&info;expectedCount=1;expectedFence=layer::handle<VkFence>(999);appCalls=0;exactArgs=true;appResult=VK_SUCCESS;driver::fenceResult=VK_SUCCESS;
    const auto result=vkQueueSubmit(layer::queue,1,&info,expectedFence);const auto error=GetLastError();const auto status=raw::status(f.state.rawProbe);
    check(result==VK_SUCCESS&&error==0x87654321&&appCalls==1&&exactArgs&&driver::privateSubmits==1,"unexpected private PFN reentry preserves original app exact once");
    check(status.observationUnsafe&&status.completion.uncertain&&!status.completion.readbackReady&&!status.pixelsWritten&&raw::pixels(f.state.rawProbe).empty()&&!raw::destroy(f.state.rawProbe),"private PFN same thread reentry stops pixels before immediate-success fence poll");
    check(raw::beforeDeviceDestroy(f.state.rawProbe,2'000'000'000ull).accepted()&&raw::destroy(f.state.rawProbe),"reentered private recipe cleanup only after own drain");f.state.rawProbe=nullptr;
}
void deviceLostAfterReady(){
    layer::Fixture f;prime(f);f.state.submit=literalSubmit;VkCommandBuffer command=layer::command;VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&command;
    expectedInfos=&info;expectedCount=1;expectedFence=layer::handle<VkFence>(999);appCalls=0;exactArgs=true;appResult=VK_SUCCESS;driver::fenceResult=VK_SUCCESS;
    check(vkQueueSubmit(layer::queue,1,&info,expectedFence)==VK_SUCCESS&&raw::pixels(f.state.rawProbe).size()==16&&driver::privateSubmits==1,"initial actual-wrapper completion pixels ready");
    appResult=VK_ERROR_DEVICE_LOST;const auto result=vkQueueSubmit(layer::queue,1,&info,expectedFence);const auto error=GetLastError();const auto status=raw::status(f.state.rawProbe);
    check(result==VK_ERROR_DEVICE_LOST&&error==0x87654321&&appCalls==2&&exactArgs&&status.completion.uncertain&&!status.completion.readbackReady&&raw::pixels(f.state.rawProbe).empty()&&!raw::destroy(f.state.rawProbe),"later DeviceLost revokes pixel exposure and retains physical owner");
    check(raw::beforeDeviceDestroy(f.state.rawProbe,2'000'000'000ull).accepted()&&raw::destroy(f.state.rawProbe),"later DeviceLost explicit safe own drain cleanup");f.state.rawProbe=nullptr;appResult=VK_SUCCESS;
}
}
int main(int argc,char**){try{SetEnvironmentVariableW(L"XESS_OBSERVER_LOG",nullptr);InitOnceExecuteOnce(&logOnce,initializeLog,nullptr,nullptr);rawcolor_concurrency_fixture::overlap(argc>1);if(argc>1)return 0;
    rawcolor_layer_fixture::actualDispatchAndSubmit(false,rawcolor_concurrency_fixture::beforeCapture,rawcolor_concurrency_fixture::capturedOverlap);inventorySessionRetired=false;
    rawcolor_concurrency_fixture::unsafeCases();rawcolor_concurrency_fixture::busyAndRelated();rawcolor_concurrency_fixture::firstQueueContext();rawcolor_concurrency_fixture::leaseDuringOriginal();rawcolor_concurrency_fixture::privateRecipeReentry();rawcolor_concurrency_fixture::deviceLostAfterReady();rawcolor_layer_fixture::armGate();rawcolor_cpu_fixture::budget();
    std::cout<<"{\"stage\":\"rawcolor-concurrency-affected-cpu\",\"passed\":true,\"checks\":"<<rawcolor_layer_fixture::checks+recording_layer_fixture::checks+rawcolor_cpu_fixture::checks<<",\"gpuTouched\":false,\"gameTouched\":false}\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
