// Actual NativeBackend/Runtime, with inert Vulkan and presenter/bridge doubles.
// Own hidden HWNDs only; no Vulkan loader, D3D, SDK or game is loaded.
#include <chrono>
#include <cstdint>
namespace xess_vk::presentation::testing {
bool followupEnabled{}, controlledClock{}, asyncEnabled{},sharedEnabled{},syncSharedEnabled{};
std::uint32_t asyncFrameLimit{2};
std::chrono::steady_clock::time_point clockTime;
std::chrono::steady_clock::time_point outputNow() noexcept {
    return controlledClock ? clockTime : std::chrono::steady_clock::now();
}
bool longFrameFollowupReset() noexcept { return followupEnabled; }
bool asyncCopyPath() noexcept { return asyncEnabled; }
std::uint32_t asyncCopyFrames() noexcept { return asyncFrameLimit; }
bool sharedCopyPath() noexcept { return sharedEnabled; }
bool sharedCopySync() noexcept { return syncSharedEnabled; }
}
#define XESS_PRESENTATION_NATIVE_TEST 1
#include "presentation_native.cpp"
#include <cstdio>
#include <thread>
#include <atomic>
namespace xess_vk::presentation::testing {
std::vector<AcquireProbe> acquireProbes;
bool acquireProbeCaptureFailed{}, acquireProbeGateFree{true};
}
static_assert(xess_vk::presentation::gameOnAdmitted, "Fixture exercises the reviewed ON candidate's real formula");

namespace fixture {
using namespace xess_vk;
unsigned checks{}, constructions{}, outputs{}, nativePresents{}, lastNativeWaits{}, submits{}, vsyncChanges{};
unsigned nativeCreates{},nativeImages{},nativeDestroys{},fakeEmptySubmits{};
presentation::QueueGate* fakeGate{};
bool fakeLayoutsValid{true};
bool delayFakeSubmit{}, queueCallbackReleased{true};
bool throwFakeSubmit{};
unsigned queueCallbacks{};
wsi_acquire::QueueCall lastQueueCall;
unsigned timestampCreates{},timestampDestroys{},timestampResets{},timestampWrites{},timestampReads{},timestampProperties{},calibrations{};
unsigned timestampFailure{};bool presenterTimestamps{};
std::uint64_t timestampReadDelayNs{};
void queueObserved(void* context,const char*,std::uint64_t,const wsi_acquire::QueueCall& call) noexcept {
    ++queueCallbacks;lastQueueCall=call;
    try {
        bool released{};auto* gate=static_cast<QueueGate*>(context);
        std::thread other([&]{released=gate->try_lock();if(released)gate->unlock();});other.join();
        queueCallbackReleased=queueCallbackReleased&&released;
    } catch (...) { queueCallbackReleased=false; }
}
unsigned fenceWaits{}, queuedOutputs{}, semaphoreCreates{}, semaphoreDestroys{};
unsigned queuedOnOutputs{}, bridgeWaits{};
unsigned colorCopies{}, colorBlits{};
VkImageBlit lastBlit{};
VkFilter lastBlitFilter{};
VkImage lastColorSource{};
VkExtent3D lastPublishedColor{};
bool linearFilterSupported{true}, failStableCreate{};
VkFormatFeatureFlags missingFormatFeatures{};
std::vector<VkExtent3D> stableExtents;
VkSemaphore lastNativeSemaphore{};
bool enableMenuAtPresent{}, disableMenuAtPresent{}, revokeLocalAtPresent{}, revokeLocalAtCopySubmit{};
bool leaveGameplayAtCopySubmit{};
bool throwSemaphore{}, throwQueuedPublication{}, throwQueuedColorUnavailable{}, failBridgeWait{};
VkResult nativeResult=VK_SUCCESS;
unsigned resizes{};
VkExtent2D surfaceExtent{64,32};
VkResult submitResult = VK_SUCCESS, waitResult = VK_SUCCESS, privateSubmitResult=VK_SUCCESS;
bool copyCompleted{}, failInitialization{}, requestedLl1{};
bool failResize{}, occluded{};
std::int32_t frameGenResult{},queryResult{};
bool lastBundleEnabled{}, lastHistoryReset{}, lastQueuedHistoryReset{};
VkRect2D lastOutputRect{};
bool changeMenuDuringOutput{};
unsigned bundleCalls{};
frame_input::FrameConstantsSample lastConstants;
fsr_menu_control::ControlIntent menuIntent;
presentation::Runtime* runtime{};
std::map<VkFence,bool> completedFences;
std::map<VkCommandBuffer,VkFence> commandFences;
std::map<std::uint64_t,VkFence> asyncNativeFences;
std::map<std::uint64_t,FrameSlotKey> asyncReceiptKeys;
std::vector<FrameSlotKey> asyncKeys;
std::uint64_t asyncSerial{},asyncConsumed{},asyncTail{};
unsigned asyncWaits{};
std::vector<char> syncWaitOrder;
bool failAsyncWait{},asyncDeviceLost{};
SharedPlaneLease lastSharedLease;
VkImage lastColorTarget{};
unsigned sharedColorOutputs{},sharedBundleOutputs{},sharedSourceOutputs{};
bool throwSharedPublication{};
void require(bool value,const char* message);
void completeAsync(std::uint64_t value,bool tail=true) {
    asyncConsumed=std::max(asyncConsumed,value);
    if(tail)asyncTail=std::max(asyncTail,value);
    for(const auto& [serial,fence]:asyncNativeFences)if(serial<=value)completedFences[fence]=true;
}
void beginAsyncReceipt(FrameSlotKey key,VkFence fence,FrameCompletion& receipt) {
    require(key.valid(),"Native chooses a valid immutable frame slot");
    receipt={key};receipt.producedValue=++asyncSerial;receipt.producedValid=true;
    receipt.d3dWaitQueued=receipt.callerSubmitted=true;
    asyncNativeFences[asyncSerial]=fence;asyncReceiptKeys[asyncSerial]=key;asyncKeys.push_back(key);
}
void finishAsyncReceipt(FrameCompletion& receipt) {
    receipt.consumedValue=receipt.sdkTailValue=receipt.producedValue;
    receipt.consumedValid=receipt.sdkTailValid=true;
}
std::atomic<std::uintptr_t> nextHandle{0x100};
template<class T> T h(std::uintptr_t n) { return reinterpret_cast<T>(n); }
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void reset() {
    presentation::testing::followupEnabled=presentation::testing::controlledClock=presentation::testing::asyncEnabled=presentation::testing::sharedEnabled=presentation::testing::syncSharedEnabled=false;
    presentation::testing::asyncFrameLimit=2;
    constructions=outputs=nativePresents=lastNativeWaits=submits=vsyncChanges=0;
    nativeCreates=nativeImages=nativeDestroys=fakeEmptySubmits=0;fakeLayoutsValid=true;fakeGate=nullptr;
    delayFakeSubmit=false;queueCallbackReleased=true;queueCallbacks=0;lastQueueCall={};
    throwFakeSubmit=false;presentation::testing::acquireProbes.clear();presentation::testing::acquireProbeCaptureFailed=false;presentation::testing::acquireProbeGateFree=true;
    timestampCreates=timestampDestroys=timestampResets=timestampWrites=timestampReads=timestampProperties=calibrations=0;
    timestampFailure=0;presenterTimestamps=false;timestampReadDelayNs=0;
    fenceWaits=queuedOutputs=semaphoreCreates=semaphoreDestroys=0;lastNativeSemaphore={};enableMenuAtPresent=false;
    queuedOnOutputs=bridgeWaits=0;disableMenuAtPresent=revokeLocalAtPresent=revokeLocalAtCopySubmit=false;
    leaveGameplayAtCopySubmit=false;
    colorCopies=colorBlits=0;lastBlit={};lastBlitFilter={};lastColorSource={};lastPublishedColor={};
    linearFilterSupported=true;missingFormatFeatures=0;failStableCreate=false;stableExtents.clear();
    throwSemaphore=throwQueuedPublication=throwQueuedColorUnavailable=failBridgeWait=false;nativeResult=VK_SUCCESS;
    resizes=0;surfaceExtent={64,32};failResize=occluded=false;frameGenResult=queryResult=0;
    copyCompleted=failInitialization=requestedLl1=false;
    lastBundleEnabled=lastHistoryReset=lastQueuedHistoryReset=changeMenuDuringOutput=false;bundleCalls=0;lastConstants={};lastOutputRect={};
    menuIntent={};
    completedFences.clear();commandFences.clear();asyncNativeFences.clear();asyncReceiptKeys.clear();asyncKeys.clear();
    asyncSerial=asyncConsumed=asyncTail=0;asyncWaits=0;failAsyncWait=asyncDeviceLost=false;
    syncWaitOrder.clear();
    lastSharedLease={};lastColorTarget={};sharedColorOutputs=sharedBundleOutputs=sharedSourceOutputs=0;throwSharedPublication=false;
    submitResult=waitResult=privateSubmitResult=VK_SUCCESS; runtime=nullptr;
}
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice,const VkImageCreateInfo* info,const VkAllocationCallbacks*,VkImage* out) {
    if(info->format==VK_FORMAT_R8G8B8A8_UNORM&&info->usage==(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        if(failStableCreate)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        stableExtents.push_back(info->extent);
    }
    *out=h<VkImage>(++nextHandle);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice,VkImage,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL createBuffer(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer* out) { *out=h<VkBuffer>(++nextHandle); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL destroyBuffer(VkDevice,VkBuffer,const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL bufferRequirements(VkDevice,VkBuffer,VkMemoryRequirements* out) { *out={4096,256,1}; }
VKAPI_ATTR void VKAPI_CALL imageRequirements(VkDevice,VkImage,VkMemoryRequirements* out) { *out={4096,256,1}; }
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory* out) { *out=h<VkDeviceMemory>(++nextHandle); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL bind(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL bindBuffer(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice,const VkCommandPoolCreateInfo*,const VkAllocationCallbacks*,VkCommandPool* out) { *out=h<VkCommandPool>(++nextHandle); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice,VkCommandPool,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL commands(VkDevice,const VkCommandBufferAllocateInfo*,VkCommandBuffer* out) { *out=h<VkCommandBuffer>(++nextHandle); return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL createFence(VkDevice,const VkFenceCreateInfo* info,const VkAllocationCallbacks*,VkFence* out) { *out=h<VkFence>(++nextHandle);completedFences[*out]=(info->flags&VK_FENCE_CREATE_SIGNALED_BIT)!=0; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL destroyFence(VkDevice,VkFence,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL createSemaphore(VkDevice,const VkSemaphoreCreateInfo*,const VkAllocationCallbacks*,VkSemaphore* out) {
    if(throwSemaphore)throw std::runtime_error("injected pre-submit semaphore creation exception");
    ++semaphoreCreates;*out=h<VkSemaphore>(++nextHandle); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroySemaphore(VkDevice,VkSemaphore,const VkAllocationCallbacks*) { ++semaphoreDestroys; }
VKAPI_ATTR VkResult VKAPI_CALL resetFences(VkDevice,std::uint32_t count,const VkFence* fences) {
    copyCompleted=false;
    for(unsigned i=0;i<count;++i) {
        if((presentation::testing::asyncEnabled||presentation::testing::syncSharedEnabled)&&std::any_of(commandFences.begin(),commandFences.end(),[&](const auto& item){return item.second==fences[i];}))
            require(completedFences[fences[i]],"a pending native command cannot reset even if another slot is free");
        std::erase_if(commandFences,[&](const auto& item){return item.second==fences[i];});
        completedFences[fences[i]]=false;
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL resetCommand(VkCommandBuffer,VkCommandBufferResetFlags) {return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer,const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL end(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,std::uint32_t,const VkMemoryBarrier*,std::uint32_t,const VkBufferMemoryBarrier*,std::uint32_t count,const VkImageMemoryBarrier* barriers) {
    if(runtime&&runtime->fakeEnabled())for(std::uint32_t i=0;i<count;++i)if(runtime->isFakeImage(barriers[i].image))fakeLayoutsValid&=
        (barriers[i].oldLayout==VK_IMAGE_LAYOUT_GENERAL&&barriers[i].newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)||
        (barriers[i].oldLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&barriers[i].newLayout==VK_IMAGE_LAYOUT_GENERAL);
}
VKAPI_ATTR void VKAPI_CALL copy(VkCommandBuffer,VkImage source,VkImageLayout,VkImage target,VkImageLayout,std::uint32_t,const VkImageCopy*) {if(source==h<VkImage>(0xF00)||(runtime&&runtime->isFakeImage(source))){++colorCopies;lastColorSource=source;lastColorTarget=target;}}
VKAPI_ATTR void VKAPI_CALL blit(VkCommandBuffer,VkImage source,VkImageLayout,VkImage target,VkImageLayout,std::uint32_t count,const VkImageBlit* regions,VkFilter filter) {
    if(source==h<VkImage>(0xF00)||(runtime&&runtime->isFakeImage(source))) {
        require(count==1,"private blit records one complete source/destination mapping");
        ++colorBlits;lastColorSource=source;lastColorTarget=target;lastBlit=regions[0];lastBlitFilter=filter;
    }
}
VKAPI_ATTR void VKAPI_CALL copyToBuffer(VkCommandBuffer,VkImage,VkImageLayout,VkBuffer,std::uint32_t,const VkBufferImageCopy*) {}
VKAPI_ATTR void VKAPI_CALL copyToImage(VkCommandBuffer,VkBuffer,VkImage,VkImageLayout,std::uint32_t,const VkBufferImageCopy*) {}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue,std::uint32_t count,const VkSubmitInfo* info,VkFence fence) {
    if(info->commandBufferCount&&leaveGameplayAtCopySubmit){menuIntent.gameplayReady=false;menuIntent.gameplayState=10;}
    if(throwFakeSubmit)throw std::runtime_error("injected Native queue submit exception");
    const auto result=submitResult==VK_SUCCESS&&info->commandBufferCount ? privateSubmitResult : submitResult;
    if(presentation::testing::asyncEnabled&&result==VK_SUCCESS&&fence) {
        completedFences[fence]=false;
        for(unsigned i=0;i<count;++i)for(unsigned j=0;j<info[i].commandBufferCount;++j)
            commandFences[info[i].pCommandBuffers[j]]=fence;
    }
    if(fakeGate){
        bool available{};std::thread other([&]{available=fakeGate->try_lock();if(available)fakeGate->unlock();});other.join();require(!available,"actual NativeBackend queue calls use device gate");
        ++submits;require(count==1,"fake NativeBackend uses one real submit record");
        for(unsigned i=0;runtime&&i<info->waitSemaphoreCount;++i)
            require(runtime->retainsAppSemaphore(info->pWaitSemaphores[i]),"app wait is retained before its queue submit can return or fail");
        if(info->commandBufferCount)require(info->commandBufferCount==1&&info->waitSemaphoreCount==1&&!info->signalSemaphoreCount,"fake copy consumes app wait without native-present binary signal");
        else {++fakeEmptySubmits;require((info->signalSemaphoreCount==1&&!info->waitSemaphoreCount)||(!info->signalSemaphoreCount&&fence),"fake acquire signal or queue-tail empty submission");}
        if(info->commandBufferCount&&revokeLocalAtCopySubmit)menuIntent.localFiOffActive=false;
        if(delayFakeSubmit){delayFakeSubmit=false;Sleep(30);}
        return result;
    }
    ++submits; require(count==1 && info->commandBufferCount==1 && info->waitSemaphoreCount==1,
                       "private copy owns exactly the original application wait");
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice,std::uint32_t count,const VkFence* fences,VkBool32,std::uint64_t) {
    if(presentation::testing::syncSharedEnabled)syncWaitOrder.push_back('v');
    ++fenceWaits;copyCompleted=waitResult==VK_SUCCESS;
    if(copyCompleted)for(unsigned i=0;i<count;++i)completedFences[fences[i]]=true;
    return waitResult;
}
VKAPI_ATTR VkResult VKAPI_CALL fenceStatus(VkDevice,VkFence fence) {
    if(presentation::testing::asyncEnabled) {
        const auto found=completedFences.find(fence);
        if(found!=completedFences.end()) {if(found->second)copyCompleted=true;return found->second?VK_SUCCESS:VK_NOT_READY;}
        return asyncTail==asyncSerial ? VK_SUCCESS : VK_NOT_READY;
    }
    return copyCompleted ? VK_SUCCESS : VK_NOT_READY;
}
VKAPI_ATTR VkResult VKAPI_CALL createChain(VkDevice,const VkSwapchainCreateInfoKHR*,const VkAllocationCallbacks*,VkSwapchainKHR* out) { ++nativeCreates;*out=h<VkSwapchainKHR>(++nextHandle); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL destroyChain(VkDevice,VkSwapchainKHR,const VkAllocationCallbacks*) {++nativeDestroys;}
VKAPI_ATTR VkResult VKAPI_CALL images(VkDevice,VkSwapchainKHR,std::uint32_t* count,VkImage* out) {++nativeImages;if (out) out[0]=h<VkImage>(0xF00); *count=1; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue,const VkPresentInfoKHR* info) {
    ++nativePresents; lastNativeWaits=info->waitSemaphoreCount;
    lastNativeSemaphore=info->waitSemaphoreCount?info->pWaitSemaphores[0]:VK_NULL_HANDLE;
    if(enableMenuAtPresent)menuIntent.requestedEnabled=true;
    if(disableMenuAtPresent)menuIntent.requestedEnabled=false;
    if(revokeLocalAtPresent)menuIntent.localFiOffActive=false;
    if(info->pResults) info->pResults[0]=nativeResult; return nativeResult;
}
VKAPI_ATTR VkResult VKAPI_CALL createTimestampPool(VkDevice,const VkQueryPoolCreateInfo* info,const VkAllocationCallbacks*,VkQueryPool* out){
    ++timestampCreates;require(info->queryType==VK_QUERY_TYPE_TIMESTAMP&&info->queryCount==2,"exactly two private timestamps");
    if(timestampFailure==4)return VK_ERROR_OUT_OF_DEVICE_MEMORY;*out=h<VkQueryPool>(presentation::testing::asyncEnabled?++nextHandle:0xAB01);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyTimestampPool(VkDevice,VkQueryPool,const VkAllocationCallbacks*){++timestampDestroys;}
VKAPI_ATTR void VKAPI_CALL resetTimestampPool(VkCommandBuffer,VkQueryPool,std::uint32_t first,std::uint32_t count){++timestampResets;require(first==0&&count==2,"query reset remains inside existing private command");}
VKAPI_ATTR void VKAPI_CALL writeTimestamp(VkCommandBuffer,VkPipelineStageFlagBits stage,VkQueryPool,std::uint32_t index){
    ++timestampWrites;require((index==0&&stage==VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)||(index==1&&stage==VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT),"private copy timestamp stages bracket existing work");
}
VKAPI_ATTR VkResult VKAPI_CALL readTimestampPool(VkDevice,VkQueryPool,std::uint32_t first,std::uint32_t count,std::size_t bytes,void* data,VkDeviceSize stride,VkQueryResultFlags flags){
    ++timestampReads;require(copyCompleted,"query read only after existing copy fence success");
    require(first==0&&count==2&&bytes==32&&stride==16&&flags==(VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT),"timestamp read never adds WAIT or partial query results");
    if(timestampFailure==5)return VK_NOT_READY;if(timestampFailure==6)throw std::runtime_error("diagnostic read exception");
    if(timestampFailure==9)return VK_ERROR_DEVICE_LOST;
    if(timestampFailure==10){const auto begin=cpuWallNowNs();Sleep(30);timestampReadDelayNs+=cpuWallNowNs()-begin;}
    auto* values=static_cast<std::uint64_t*>(data);values[0]=100;values[1]=timestampFailure==7?0:1;values[2]=140;values[3]=1;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL calibratedTimestamps(VkDevice,std::uint32_t count,const VkCalibratedTimestampInfoKHR* info,std::uint64_t* values,std::uint64_t* deviation){
    ++calibrations;require(count==2&&info[0].timeDomain==VK_TIME_DOMAIN_DEVICE_KHR&&info[1].timeDomain==VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR,"calibration uses supported device/QPC domains only");
    LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);values[0]=140;values[1]=qpc.QuadPart;*deviation=42;return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceFunctions(VkDevice,const char* name) {
#define FN(n,f) if(!std::strcmp(name,n)) return reinterpret_cast<PFN_vkVoidFunction>(&f)
    FN("vkCreateImage",createImage); FN("vkDestroyImage",destroyImage); FN("vkGetImageMemoryRequirements",imageRequirements);
    FN("vkCreateBuffer",createBuffer); FN("vkDestroyBuffer",destroyBuffer); FN("vkGetBufferMemoryRequirements",bufferRequirements); FN("vkBindBufferMemory",bindBuffer);
    FN("vkAllocateMemory",allocate); FN("vkFreeMemory",freeMemory); FN("vkBindImageMemory",bind);
    FN("vkCreateCommandPool",createPool); FN("vkDestroyCommandPool",destroyPool); FN("vkAllocateCommandBuffers",commands);
    FN("vkCreateFence",createFence); FN("vkDestroyFence",destroyFence); FN("vkCreateSemaphore",createSemaphore); FN("vkDestroySemaphore",destroySemaphore);
    FN("vkResetFences",resetFences); FN("vkResetCommandBuffer",resetCommand); FN("vkBeginCommandBuffer",begin); FN("vkEndCommandBuffer",end);
    FN("vkCmdPipelineBarrier",barrier); FN("vkCmdCopyImage",copy); FN("vkCmdBlitImage",blit);
    FN("vkCmdCopyImageToBuffer",copyToBuffer); FN("vkCmdCopyBufferToImage",copyToImage);
    FN("vkQueueSubmit",submit); FN("vkWaitForFences",wait); FN("vkGetFenceStatus",fenceStatus);
    FN("vkCreateSwapchainKHR",createChain); FN("vkDestroySwapchainKHR",destroyChain); FN("vkGetSwapchainImagesKHR",images); FN("vkQueuePresentKHR",present);
    FN("vkCreateQueryPool",createTimestampPool);FN("vkDestroyQueryPool",destroyTimestampPool);
    FN("vkCmdResetQueryPool",resetTimestampPool);FN("vkCmdWriteTimestamp",writeTimestamp);
    if(timestampFailure!=1){FN("vkGetQueryPoolResults",readTimestampPool);}
    if(timestampFailure!=8){FN("vkGetCalibratedTimestampsEXT",calibratedTimestamps);FN("vkGetCalibratedTimestampsKHR",calibratedTimestamps);}
#undef FN
    return nullptr;
}
VKAPI_ATTR void VKAPI_CALL memoryProperties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties* out) { *out={}; out->memoryTypeCount=1; out->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; }
VKAPI_ATTR void VKAPI_CALL formatProperties(VkPhysicalDevice,VkFormat,VkFormatProperties* out) { *out={}; out->optimalTilingFeatures=(VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|(linearFilterSupported?VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT:0))&~missingFormatFeatures; }
VKAPI_ATTR VkResult VKAPI_CALL capabilities(VkPhysicalDevice,VkSurfaceKHR,VkSurfaceCapabilitiesKHR* out) { *out={}; out->currentExtent=surfaceExtent; out->supportedUsageFlags=VK_IMAGE_USAGE_TRANSFER_SRC_BIT; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL support(VkPhysicalDevice,std::uint32_t,VkSurfaceKHR,VkBool32* out) { *out=VK_TRUE; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL timestampDeviceProperties(VkPhysicalDevice,VkPhysicalDeviceProperties* out){++timestampProperties;*out={};out->limits.timestampPeriod=timestampFailure==3?0.f:2.f;}
VKAPI_ATTR void VKAPI_CALL timestampQueueProperties(VkPhysicalDevice,std::uint32_t* count,VkQueueFamilyProperties* out){if(!out){*count=1;return;}*out={};out->queueFlags=VK_QUEUE_GRAPHICS_BIT;out->timestampValidBits=timestampFailure==2?0:36;*count=1;}
VKAPI_ATTR VkResult VKAPI_CALL timestampDomains(VkPhysicalDevice,std::uint32_t* count,VkTimeDomainKHR* domains){if(!domains){*count=2;return VK_SUCCESS;}domains[0]=VK_TIME_DOMAIN_DEVICE_KHR;domains[1]=VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;*count=2;return VK_SUCCESS;}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceFunctions(VkInstance,const char* name) {
    if(!std::strcmp(name,"vkGetPhysicalDeviceMemoryProperties")) return reinterpret_cast<PFN_vkVoidFunction>(&memoryProperties);
    if(!std::strcmp(name,"vkGetPhysicalDeviceFormatProperties")) return reinterpret_cast<PFN_vkVoidFunction>(&formatProperties);
    if(!std::strcmp(name,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return reinterpret_cast<PFN_vkVoidFunction>(&capabilities);
    if(!std::strcmp(name,"vkGetPhysicalDeviceSurfaceSupportKHR")) return reinterpret_cast<PFN_vkVoidFunction>(&support);
    if(!std::strcmp(name,"vkGetPhysicalDeviceProperties"))return reinterpret_cast<PFN_vkVoidFunction>(&timestampDeviceProperties);
    if(!std::strcmp(name,"vkGetPhysicalDeviceQueueFamilyProperties"))return reinterpret_cast<PFN_vkVoidFunction>(&timestampQueueProperties);
    if(!std::strcmp(name,"vkGetPhysicalDeviceCalibrateableTimeDomainsEXT")||!std::strcmp(name,"vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"))return reinterpret_cast<PFN_vkVoidFunction>(&timestampDomains);
    return nullptr;
}
}

namespace xess_vk {
struct InteropBridge::Impl {
    InteropConfig config,sharedConfig;
    GpuTimestampSample timestamp;
    SharedPlaneStats sharedStats;
    struct Slot { SharedPlaneLease lease;unsigned state{},writes{};FrameCompletion publication; };
    std::array<Slot,sharedPlaneSlotCount> slots;
    VkDevice device{};std::uint32_t family{};std::uint64_t generation{},serial{};
    Slot* find(SharedPlaneKey key) {
        return key.valid()&&slots[key.index].lease.key==key?&slots[key.index]:nullptr;
    }
};
InteropBridge::InteropBridge(InteropConfig c,const BorrowedVulkanContext& borrowed) : impl_(std::make_unique<Impl>(Impl{c,{}})) {
    impl_->device=borrowed.device;impl_->family=borrowed.queueFamilyIndex;
    impl_->timestamp.requested=borrowed.gpuTimestamps;
    if(borrowed.queueGate)fixture::require(borrowed.queueObserver==fixture::queueObserved&&borrowed.queueObserverContext==borrowed.queueGate,
        "borrowed bridge retains only the device-owned queue callback and context");
}
InteropBridge::~InteropBridge()=default;
const InteropConfig& InteropBridge::config() const { return impl_->config; }
void InteropBridge::associateGpuTimestampFrame(const GpuTimestampSample& identity) noexcept {
    const auto requested=impl_->timestamp.requested;impl_->timestamp=identity;impl_->timestamp.requested=requested;
}
GpuTimestampSample InteropBridge::gpuTimestampSample() const noexcept { return impl_->timestamp; }
void InteropBridge::waitUntilConsumed() {
    ++fixture::bridgeWaits;
    if(presentation::testing::syncSharedEnabled)fixture::syncWaitOrder.push_back('c');
    if(fixture::failBridgeWait)throw std::runtime_error("injected previous Bridge completion failure");
    fixture::require(fixture::copyCompleted,"queued source release follows the prior real copy completion");
}
FrameCompletionState InteropBridge::consumedComplete(std::uint64_t value) const noexcept {
    if(!value)return FrameCompletionState::Invalid;
    if(fixture::asyncDeviceLost)return FrameCompletionState::DeviceLost;
    return !presentation::testing::asyncEnabled||fixture::asyncConsumed>=value ? FrameCompletionState::Complete : FrameCompletionState::Pending;
}
void InteropBridge::resizeOutput(std::uint32_t width,std::uint32_t height) {
    impl_->config.width=width;impl_->config.height=height;
}
bool InteropBridge::prepareSharedPlanes(std::uint32_t width,std::uint32_t height) {
    const bool same=impl_->generation&&impl_->sharedConfig.width==impl_->config.width&&impl_->sharedConfig.height==impl_->config.height&&
        impl_->sharedConfig.inputWidth==width&&impl_->sharedConfig.inputHeight==height;
    if(same){impl_->sharedStats.ready=true;return true;}
    impl_->sharedStats.ready=false;
    if(std::any_of(impl_->slots.begin(),impl_->slots.end(),[](const auto& s){return s.state!=0;}))return false;
    impl_->sharedConfig=impl_->config;impl_->sharedConfig.inputWidth=width;impl_->sharedConfig.inputHeight=height;
    impl_->sharedConfig.colorOnly=!width&&!height;++impl_->generation;
    for(auto& slot:impl_->slots) {
        slot={};slot.lease.auxiliary=!impl_->sharedConfig.colorOnly;
        for(unsigned p=0;p<(impl_->sharedConfig.colorOnly?1u:3u);++p) {
            auto& image=slot.lease.planes[p];image.device=impl_->device;image.image=fixture::h<VkImage>(++fixture::nextHandle);
            image.ownerQueueFamily=impl_->family;image.currentLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            auto& info=image.successfulImageCreateInfo;info.imageType=VK_IMAGE_TYPE_2D;
            info.format=p==0?VK_FORMAT_R8G8B8A8_UNORM:p==1?VK_FORMAT_R32_SFLOAT:VK_FORMAT_R16G16_SFLOAT;
            info.extent=p?VkExtent3D{width,height,1}:VkExtent3D{impl_->config.width,impl_->config.height,1};
            info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
            info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        }
    }
    impl_->sharedStats.available=impl_->sharedStats.ready=true;impl_->sharedStats.ringGeneration=impl_->generation;
    impl_->sharedStats.resourceBytes=3ull*(impl_->config.width*impl_->config.height*4ull+width*height*8ull);return true;
}
SharedBorrowResult InteropBridge::tryReserveSharedFrame(VkExtent2D extent,bool auxiliary,SharedPlaneLease& lease) noexcept {
    lease={};
    if(!impl_->sharedStats.ready||(auxiliary&&(impl_->sharedConfig.colorOnly||extent.width!=impl_->sharedConfig.inputWidth||extent.height!=impl_->sharedConfig.inputHeight))) {
        ++impl_->sharedStats.extentFallbacks;return SharedBorrowResult::Unavailable;
    }
    for(unsigned i=0;i<impl_->slots.size();++i)if(!impl_->slots[i].state) {
        auto& slot=impl_->slots[i];slot.state=1;slot.writes=0;slot.publication={};
        slot.lease.key={i,impl_->generation,++impl_->serial};slot.lease.auxiliary=auxiliary;lease=slot.lease;
        ++impl_->sharedStats.reservations;return SharedBorrowResult::Acquired;
    }
    ++impl_->sharedStats.noFreeFallbacks;return SharedBorrowResult::Unavailable;
}
void InteropBridge::sharedWriteSubmitted(SharedPlaneKey key,std::uint32_t mask) {
    auto* slot=impl_->find(key);fixture::require(slot&&(slot->state==1||slot->state==2),"actual successful shared write names a reserved source");
    slot->state=2;slot->writes|=mask;
    for(unsigned p=0;p<3;++p)if(mask&(1u<<p)) {
        slot->lease.planes[p].currentLayout=VK_IMAGE_LAYOUT_GENERAL;slot->lease.planes[p].ownerQueueFamily=impl_->family;
        slot->lease.initializedMask|=1u<<p;
    }
}
bool InteropBridge::cancelRecordedSharedFrame(SharedPlaneKey key) noexcept {
    auto* slot=impl_->find(key);if(!slot||slot->state!=1||slot->writes)return false;
    slot->state=0;++impl_->sharedStats.cancelled;return true;
}
bool InteropBridge::abandonSubmittedSharedFrame(SharedPlaneKey key,VkFence fence) noexcept {
    auto* slot=impl_->find(key);if(!slot||slot->state!=2||!fence||fixture::fenceStatus(impl_->device,fence)!=VK_SUCCESS)return false;
    slot->state=0;++impl_->sharedStats.abandoned;return true;
}
bool InteropBridge::releaseSharedFrame(SharedPlaneKey key,const FrameCompletion& receipt) noexcept {
    auto* slot=impl_->find(key);
    if(!slot||slot->state!=3||slot->publication.key!=receipt.key||slot->publication.producedValue!=receipt.producedValue||
       slot->publication.sharedKey!=receipt.sharedKey||!receipt.consumedValid||!receipt.sdkTailValid||receipt.poisoned||
       fixture::asyncConsumed<receipt.consumedValue||fixture::asyncTail<receipt.sdkTailValue||
       (receipt.sharedKey.valid()&&!receipt.postPresentSubmitted))return false;
    slot->state=0;++impl_->sharedStats.released;return true;
}
void InteropBridge::poisonSharedFrame(SharedPlaneKey key) noexcept {
    if(auto* slot=impl_->find(key)){slot->state=4;++impl_->sharedStats.poisoned;}
}
SharedPlaneStats InteropBridge::sharedPlaneStats() const noexcept {
    auto stats=impl_->sharedStats;
    stats.leases=static_cast<std::uint32_t>(std::count_if(impl_->slots.begin(),impl_->slots.end(),[](const auto& s){return s.state!=0;}));return stats;
}
void InteropBridge::publishQueuedSharedFrame(SharedPlaneKey key,VkQueue,VkFence,FrameSlotKey frame,FrameCompletion& receipt) {
    auto* slot=impl_->find(key);fixture::require(slot&&slot->state==2&&slot->writes&1&&receipt.sharedKey==key&&frame==receipt.key,
        "shared publication uses its actual successful color write and exact frame identity");
    fixture::require(!receipt.sharedAuxiliary||(slot->lease.auxiliary&&slot->writes==7),"shared ON requires same-lease submitted depth and motion");
    fixture::require(fixture::lastColorTarget==slot->lease.planes[0].image,"Native copied game color directly into this shared slot");
    slot->state=3;slot->publication=receipt;fixture::lastSharedLease=slot->lease;++impl_->sharedStats.directFrames;
    if(fixture::throwSharedPublication)throw std::runtime_error("injected partial direct shared publication");
}
void InteropBridge::publishQueuedNativeFrame(const NativeFrame& frame,FrameSlotKey key,FrameCompletion& receipt) {
    if(!frame.sharedSourceKey.valid())return;
    auto* slot=impl_->find(frame.sharedSourceKey);
    fixture::require(slot&&slot->state==2&&(slot->writes&6)==6&&receipt.key==key&&!receipt.sharedKey.valid()&&
        frame.planes[1].image==slot->lease.planes[1].image&&frame.planes[2].image==slot->lease.planes[2].image,
        "legacy fallback pins the exact shared source reservation to its real frame receipt");
    slot->state=3;slot->publication=receipt;++impl_->sharedStats.legacySourceFrames;++fixture::sharedSourceOutputs;
}
InteropConfig InteropBridge::config(SharedPlaneKey key) const {
    fixture::require(impl_->find(key)!=nullptr,"shared configuration retains exact ring and lease generation");return impl_->sharedConfig;
}
struct ColorPresenter::Impl {
    detail::PresenterFrameTimings timings{42,true};
    ColorPresentStatus status;
    InteropBridge* bridge{};
};
ColorPresenter::ColorPresenter(InteropBridge& bridge,const std::filesystem::path&,HWND,bool,bool continuous,bool quiescent,
                               std::uint32_t interpolatedFrames,bool gpuTimestamps)
    : impl_(std::make_unique<Impl>()) {
    impl_->bridge=&bridge;
    ++fixture::constructions; fixture::requestedLl1=continuous&&quiescent;fixture::presenterTimestamps=gpuTimestamps;
    fixture::require(interpolatedFrames==(continuous?launcher_preferences::startupMultiplier()-1:1u),
                     "game frame generation capacity follows the fixed startup selection; the OFF route keeps one");
    fixture::require(fixture::copyCompleted, "fresh SDK initialization follows actual private copy completion");
    fixture::require(!fixture::runtime->retainsAppSemaphore(fixture::h<VkSemaphore>(0x10000+fixture::submits-1)),
                     "completed copy releases original wait ownership before SDK construction can throw");
    fixture::require(fixture::runtime && !fixture::runtime->timingContextGeneration() && !fixture::runtime->beginGameFrame(),
                     "callback cannot borrow a partially initialized unpublished owner");
    if(fixture::failInitialization) throw std::runtime_error("injected SDK initialization failure");
}
ColorPresenter::~ColorPresenter()=default;
FrameTimingTicket ColorPresenter::beginGameFrame(std::uint32_t id) {
    const auto handle=impl_->timings.begin(id);
    return impl_->timings.completeBegin(handle,0,0) ? handle.ticket : FrameTimingTicket{};
}
void ColorPresenter::simulationEnd(FrameTimingTicket t) { const auto h=impl_->timings.marker(t,1); impl_->timings.completeMarker(h,1,0); }
void ColorPresenter::renderSubmitBegin(FrameTimingTicket t) { const auto h=impl_->timings.marker(t,2); impl_->timings.completeMarker(h,2,0); }
void ColorPresenter::renderSubmitEnd(FrameTimingTicket t) { const auto h=impl_->timings.marker(t,3); impl_->timings.completeMarker(h,3,0); }
void ColorPresenter::abandonFrameTiming(FrameTimingTicket t) noexcept { impl_->timings.abandon(t); }
void ColorPresenter::present(const NativeImageSource& color,VkQueue,VkFence,FrameTimingTicket t,std::uint32_t) {
    fixture::lastPublishedColor=color.successfulImageCreateInfo.extent;
    fixture::require(fixture::lastPublishedColor.width==impl_->bridge->config().width&&fixture::lastPublishedColor.height==impl_->bridge->config().height,
                     "native stable color exactly matches the existing shared Bridge canvas");
    const auto h=impl_->timings.admit(t); fixture::require(bool(h),"only a complete exact timing ticket presents");
    impl_->timings.finish(h,t.sdkId,true); impl_->status={}; impl_->status.sdkPresentId=t.sdkId;
    impl_->status.occluded=fixture::occluded;impl_->status.valid=!fixture::occluded;
    impl_->status.framesPresented=fixture::occluded?0:1; ++fixture::outputs;
    impl_->status.queryAttempted=!fixture::occluded;
    if(!fixture::occluded) {
        impl_->status.queryResult=fixture::queryResult;
        if(fixture::queryResult<0)measurement::increment(measurement::Counter::SdkErrors);
        if(fixture::queryResult>0)measurement::increment(measurement::Counter::SdkWarnings);
        if(fixture::queryResult) {impl_->status.valid=false;impl_->status.framesPresented=0;throw std::runtime_error("injected current SDK status query failure");}
    }
}
void ColorPresenter::presentQueuedOff(const NativeImageSource& color,VkQueue queue,VkFence fence,FrameTimingTicket t) {
    fixture::require(!fixture::copyCompleted,"queued output does not wait the current native copy fence");
    if(fixture::throwQueuedColorUnavailable)throw ColorPresenterUnavailable("owned fixture unavailable before Bridge publication");
    if(t)present(color,queue,fence,t,0);
    else presentUntimedOff(color,queue,fence);
    ++fixture::queuedOutputs;
}
void ColorPresenter::presentBootstrap(const NativeImageSource&,VkQueue,VkFence,std::uint32_t id) { impl_->timings.admitBootstrap(id); impl_->timings.finish({},id,true); impl_->status={};impl_->status.sdkPresentId=id;impl_->status.queryAttempted=true;++fixture::outputs; }
std::uint32_t ColorPresenter::presentUntimedOff(const NativeImageSource&,VkQueue,VkFence) {
    const auto id=impl_->timings.admitUntimed(); fixture::require(id!=0,"initial owner can output untimed OFF");
    impl_->timings.finish({},id,true); impl_->status={}; impl_->status.untimedOff=true; impl_->status.sdkPresentId=id;
    impl_->status.queryAttempted=!fixture::occluded;
    impl_->status.occluded=fixture::occluded;impl_->status.valid=!fixture::occluded; ++fixture::outputs;
    if(!fixture::occluded) {
        impl_->status.queryResult=fixture::queryResult;
        if(fixture::queryResult<0)measurement::increment(measurement::Counter::SdkErrors);
        if(fixture::queryResult>0)measurement::increment(measurement::Counter::SdkWarnings);
        if(fixture::queryResult) {impl_->status.valid=false;throw std::runtime_error("injected untimed SDK status query failure");}
    }
    return id;
}
void ColorPresenter::presentBundle(const NativeFrame& f,const frame_input::FrameConstantsSample& constants,
                                 bool enabled,bool resetHistory,FrameTimingTicket t) {
    presentBundle(f,constants,enabled,resetHistory,t,{});
}
void ColorPresenter::presentBundle(const NativeFrame& f,const frame_input::FrameConstantsSample& constants,
                                 bool enabled,bool resetHistory,FrameTimingTicket t,VkRect2D rect) {
    fixture::require(f.planes[1].successfulImageCreateInfo.format==VK_FORMAT_R32_SFLOAT &&
                     f.planes[2].successfulImageCreateInfo.format==VK_FORMAT_R16G16_SFLOAT,
                     "actual provider supplies converted depth and motion to Native output");
    present(f.planes[0],f.producerQueue,f.producerCompleteFence,t,0);
    ++fixture::bundleCalls;
    fixture::lastBundleEnabled=enabled;fixture::lastHistoryReset=resetHistory;fixture::lastConstants=constants;
    fixture::lastOutputRect=rect;
    impl_->status.requestedEnabled=impl_->status.isFrameGenEnabled=enabled;
    impl_->status.framesPresented=fixture::occluded?0:(enabled?2:1);
    impl_->status.frameGenResult=fixture::frameGenResult;
    if(fixture::changeMenuDuringOutput) {
        fixture::menuIntent.paired=fixture::menuIntent.gatesConsistent=fixture::menuIntent.nativeSuppressed=false;
        ++fixture::menuIntent.controlEpoch;
    }
}
void ColorPresenter::presentBundleQueued(const NativeFrame& frame,const frame_input::FrameConstantsSample& constants,
                                        bool reset,FrameTimingTicket timing,VkRect2D rect) {
    fixture::lastQueuedHistoryReset=reset;
    fixture::require(!fixture::copyCompleted && timing && timing.latencyEnabled,
                     "queued ON enters with pending source copy and a real ON-capable ticket");
    if(fixture::throwQueuedPublication)throw std::runtime_error("injected unknown Bridge submission result with unchanged prior value");
    presentBundle(frame,constants,true,reset,timing,rect);
    ++fixture::queuedOnOutputs;
}
void ColorPresenter::presentBundleQueued(const NativeFrame& frame,const frame_input::FrameConstantsSample& constants,
                                        bool reset,FrameTimingTicket timing,VkRect2D rect,FrameSlotKey key,FrameCompletion& receipt) {
    fixture::beginAsyncReceipt(key,frame.producerCompleteFence,receipt);
    try { impl_->bridge->publishQueuedNativeFrame(frame,key,receipt);presentBundleQueued(frame,constants,reset,timing,rect);fixture::finishAsyncReceipt(receipt); }
    catch(...) {receipt.poisoned=true;throw;}
}
void ColorPresenter::presentSharedQueuedOff(SharedPlaneKey shared,VkQueue queue,VkFence fence,FrameTimingTicket timing,
                                           FrameSlotKey key,FrameCompletion& receipt) {
    fixture::beginAsyncReceipt(key,fence,receipt);receipt.sharedKey=shared;
    try {
        impl_->bridge->publishQueuedSharedFrame(shared,queue,fence,key,receipt);
        presentQueuedOff(fixture::lastSharedLease.planes[0],queue,fence,timing);
        fixture::finishAsyncReceipt(receipt);receipt.postPresentSubmitted=true;++fixture::sharedColorOutputs;
    } catch(...) {receipt.poisoned=true;throw;}
}
void ColorPresenter::presentSharedBundleQueued(SharedPlaneKey shared,VkQueue queue,VkFence fence,
        const frame_input::FrameConstantsSample& constants,bool reset,FrameTimingTicket timing,VkRect2D rect,
        FrameSlotKey key,FrameCompletion& receipt) {
    fixture::beginAsyncReceipt(key,fence,receipt);receipt.sharedKey=shared;receipt.sharedAuxiliary=true;
    try {
        impl_->bridge->publishQueuedSharedFrame(shared,queue,fence,key,receipt);
        NativeFrame frame{fixture::lastSharedLease.planes,queue,fence};
        presentBundleQueued(frame,constants,reset,timing,rect);
        fixture::finishAsyncReceipt(receipt);receipt.postPresentSubmitted=true;++fixture::sharedBundleOutputs;
    } catch(...) {receipt.poisoned=true;throw;}
}
void ColorPresenter::presentQueuedOff(const NativeImageSource& source,VkQueue queue,VkFence fence,
                                     FrameTimingTicket timing,FrameSlotKey key,FrameCompletion& receipt) {
    fixture::beginAsyncReceipt(key,fence,receipt);
    try {presentQueuedOff(source,queue,fence,timing);fixture::finishAsyncReceipt(receipt);}
    catch(...) {receipt.poisoned=true;throw;}
}
FrameCompletionState ColorPresenter::frameComplete(const FrameCompletion& receipt) const noexcept {
    if(fixture::asyncDeviceLost)return FrameCompletionState::DeviceLost;
    const auto found=fixture::asyncReceiptKeys.find(receipt.sdkTailValue);
    if(!receipt.key.valid()||!receipt.sdkTailValid||!receipt.producedValid||!receipt.consumedValid||receipt.poisoned||
        !receipt.d3dWaitQueued||!receipt.callerSubmitted||found==fixture::asyncReceiptKeys.end()||found->second!=receipt.key)
        return FrameCompletionState::Invalid;
    return fixture::asyncTail>=receipt.sdkTailValue ? FrameCompletionState::Complete : FrameCompletionState::Pending;
}
void ColorPresenter::waitFrame(const FrameCompletion& receipt) {
    fixture::require(frameComplete(receipt)==FrameCompletionState::Pending,"frame wait names one actual pending post-Present tail");
    if(presentation::testing::syncSharedEnabled)fixture::syncWaitOrder.push_back('t');
    ++fixture::asyncWaits;
    if(fixture::fakeGate) {
        bool free{};std::thread other([&]{free=fixture::fakeGate->try_lock();if(free)fixture::fakeGate->unlock();});other.join();
        fixture::require(free,"capacity wait never owns QueueGate");
    }
    if(fixture::failAsyncWait)throw std::runtime_error("injected finite frame-tail timeout");
    fixture::completeAsync(receipt.sdkTailValue);
}
void ColorPresenter::setVsync(bool) { ++fixture::vsyncChanges; }
void ColorPresenter::resizeOutput(std::uint32_t width,std::uint32_t height) {
    ++fixture::resizes;
    if(fixture::failResize)throw std::runtime_error("injected proxy ResizeBuffers failure");
    impl_->bridge->resizeOutput(width,height);
}
void ColorPresenter::stopAdmission() {}
void ColorPresenter::requestStop() {}
void ColorPresenter::shutdown() {}
std::uint64_t ColorPresenter::timingContextGeneration() const { return 42; }
std::uint32_t ColorPresenter::nextSdkId() const { return impl_->timings.nextId(); }
ColorPresentStatus ColorPresenter::lastPresentStatus() const {
    auto status=impl_->status; status.latencyTransitionAdmitted=impl_->timings.latencyCapability(); return status;
}
namespace fsr_menu_control { ControlIntent snapshot() noexcept { return fixture::menuIntent; } }
}

namespace fixture {
struct Owner {
    presentation::QueueGate gate;
    bool fake{};
    HWND window{};
    std::unique_ptr<presentation::Surface> surface;
    std::unique_ptr<presentation::DeviceRecord> record;
    std::unique_ptr<continuous::Provider> provider;
    std::unique_ptr<presentation::Runtime> ownedRuntime;
    VkSwapchainKHR chain{};
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    explicit Owner(bool withProvider=true,const wchar_t* lowRes=nullptr,VkExtent2D initialExtent={64,32},bool useFake=false,bool gpuTimestamps=false,unsigned gpuFailure=0,bool calibrated=false,bool asyncCopy=false,std::uint32_t asyncFrames=2,bool sharedCopy=false,bool calibrationKhr=false,bool syncShared=false):fake(useFake) {
        reset(); SetEnvironmentVariableW(L"XESS_SDK_BIN",L"D:\\fixture-sdk-not-loaded");
        presentation::testing::asyncEnabled=asyncCopy;
        presentation::testing::asyncFrameLimit=asyncFrames;
        presentation::testing::sharedEnabled=sharedCopy;
        presentation::testing::syncSharedEnabled=syncShared;
        SetEnvironmentVariableW(L"XESS_GPU_TIMESTAMPS",gpuTimestamps?L"1":nullptr);timestampFailure=gpuFailure;
        if(fake)fakeGate=&gate;
        SetEnvironmentVariableW(L"XESS_XEFG_LOW_RES_OUTPUT",lowRes);surfaceExtent=initialExtent;
        window=CreateWindowExW(0,L"STATIC",L"hidden native init fixture",WS_POPUP,0,0,int(initialExtent.width),int(initialExtent.height),nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window!=nullptr,"own hidden HWND created");
        surface=std::make_unique<presentation::Surface>(window); surface->handle=h<VkSurfaceKHR>(4);
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queueInfo.queueCount=1;
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES}; timeline.timelineSemaphore=VK_TRUE;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&timeline}; deviceInfo.queueCreateInfoCount=1; deviceInfo.pQueueCreateInfos=&queueInfo;
        const char* calibrationExtension=calibrationKhr?"VK_KHR_calibrated_timestamps":"VK_EXT_calibrated_timestamps";
        if(calibrated){deviceInfo.enabledExtensionCount=1;deviceInfo.ppEnabledExtensionNames=&calibrationExtension;}
        auto invalid=deviceInfo; invalid.queueCreateInfoCount=2;
        bool refused{};
        try { presentation::DeviceRecord bad(h<VkInstance>(1),h<VkPhysicalDevice>(2),h<VkDevice>(3),instanceFunctions,deviceFunctions,VK_API_VERSION_1_2,invalid); }
        catch(const std::invalid_argument&) { refused=true; }
        require(refused,"fresh LL1 contract refuses more than one device queue create record");
        queueInfo.queueCount=2; refused=false;
        try { presentation::DeviceRecord bad(h<VkInstance>(1),h<VkPhysicalDevice>(2),h<VkDevice>(3),instanceFunctions,deviceFunctions,VK_API_VERSION_1_2,deviceInfo); }
        catch(const std::invalid_argument&) { refused=true; }
        require(refused,"fresh LL1 contract refuses more than one queue in its device queue record"); queueInfo.queueCount=1;
        record=std::make_unique<presentation::DeviceRecord>(h<VkInstance>(1),h<VkPhysicalDevice>(2),h<VkDevice>(3),instanceFunctions,deviceFunctions,VK_API_VERSION_1_2,deviceInfo,fake,fake?&gate:nullptr,
            fake?queueObserved:nullptr,fake?&gate:nullptr);
        continuous::Config c; c.device=h<VkDevice>(3); c.queue=h<VkQueue>(5); c.functions=deviceFunctions; c.memory.memoryTypeCount=1;
        c.memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if(withProvider) provider=std::make_unique<continuous::Provider>(c);
        ownedRuntime=record->create(*surface,h<VkQueue>(5),provider.get()); runtime=ownedRuntime.get();
        info.surface=surface->handle; info.imageFormat=VK_FORMAT_R8G8B8A8_UNORM; info.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.imageExtent=initialExtent; info.imageArrayLayers=1; info.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE; info.presentMode=VK_PRESENT_MODE_FIFO_KHR;
        if(fake){info.minImageCount=3;info.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;}
        require(runtime->create(&info,nullptr,&chain)==VK_SUCCESS && !constructions,"swapchain prepare allocates bridge without starting the SDK");
    }
    ~Owner() {
        if(provider&&presentation::testing::sharedEnabled) {
            completedFences[h<VkFence>(0xF001)]=true;
            provider->stop(h<VkFence>(0xF001));
        }
        ownedRuntime->stop();ownedRuntime.reset();
        if(provider && provider->stats().captured)provider->stop(h<VkFence>(0xF001));
        provider.reset();record.reset();surface.reset();DestroyWindow(window);runtime=nullptr;fakeGate=nullptr;
    }
    void frame(bool untimed=true, VkResult expected=VK_SUCCESS) {
        const auto semaphore=h<VkSemaphore>(0x10000+submits);std::uint32_t index=0;
        if(fake)require(runtime->acquire(chain,UINT64_MAX,semaphore,VK_NULL_HANDLE,&index)==VK_SUCCESS,"actual NativeBackend fake acquire signals app semaphore");
        VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; p.waitSemaphoreCount=1; p.pWaitSemaphores=&semaphore;
        p.swapchainCount=1; p.pSwapchains=&chain; p.pImageIndices=&index;
        const auto result=untimed ? runtime->presentUntimedOff(h<VkQueue>(5),&p) : runtime->present(h<VkQueue>(5),&p);
        if(result!=expected)std::fprintf(stderr,"present got %d expected %d async=%d serial=%llu tail=%llu waited=%u\n",result,expected,presentation::testing::asyncEnabled,asyncSerial,asyncTail,asyncWaits);
        require(result==expected,
                "actual Runtime preserves the native result");
    }
    void capture(FrameTimingTicket ticket,std::uint32_t engine,const frame_input::FrameConstantsSample& constants,
                 bool captureInputs=true,VkExtent2D input={32,16}) {
        continuous::SourcePair source;
        source.invocation=constants.producerToken;source.engineFrame=engine;source.timing=ticket;
        source.depth={0xA01,11};source.motion={0xA02,12};source.constants=constants;
        source.frameValid=source.valid=true;provider->invocation(source);
        CommandRecordingFacts f;
        f.device=3;f.contextIncarnation=7;f.command=0xB00+source.invocation;f.rpRecordingGeneration=source.invocation;
        f.pool=0xC00;f.poolGeneration=1;f.queueFamily=0;f.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        f.beginUsageFlags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;f.reasons=0;
        f.allocationKnown=f.poolKnown=f.beginFlagsKnown=f.stateKnown=f.recording=f.scopeKnown=true;
        descriptor_image_metadata::DispatchEvidence evidence;
        evidence.metadataAccepted=true;evidence.count=2;evidence.command={f.command,source.invocation+1000};
        for(unsigned i=0;i<2;++i) {
            auto& b=evidence.bindings[i];const auto format=i?VK_FORMAT_R16G16_SFLOAT:VK_FORMAT_D32_SFLOAT_S8_UINT;
            b.setIndex=0;b.binding=i?7:5;b.image=i?source.motion:source.depth;
            b.viewType=VK_IMAGE_VIEW_TYPE_2D;b.viewFormat=format;
            b.range={i?VK_IMAGE_ASPECT_COLOR_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
            b.declaredLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            auto& d=b.imageDescription;d.format=format;d.width=input.width;d.height=input.height;d.depth=d.mipLevels=d.arrayLayers=1;
            d.samples=VK_SAMPLE_COUNT_1_BIT;d.tiling=VK_IMAGE_TILING_OPTIMAL;
            d.sharingMode=VK_SHARING_MODE_EXCLUSIVE;d.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        const bool captured=provider->record(GetCurrentThreadId(),f,evidence,{engine,ticket,0,true},captureInputs);
        if(captured!=captureInputs) {
            const auto stats=provider->stats();
            std::fprintf(stderr,"capture token=%llu engine=%u shared=%d invalid=%llu copies=%llu sharedFailures=%llu saturated=%llu\n",
                static_cast<unsigned long long>(source.invocation),engine,presentation::testing::sharedEnabled,
                static_cast<unsigned long long>(stats.invalidSources),static_cast<unsigned long long>(stats.copyFailures),
                static_cast<unsigned long long>(stats.sharedFailures),static_cast<unsigned long long>(stats.saturated));
        }
        require(captured==captureInputs,
                "production Provider consumes exact source evidence and records auxiliary copies only when requested");
        if(captured)provider->submitted({f.command,f.rpRecordingGeneration},{},h<VkQueue>(5),source.invocation,VK_SUCCESS);
    }
};

FrameTimingTicket completeTiming() {
    const auto ticket=runtime->beginGameFrame();
    require(bool(ticket),"real timing table reserves the next exact SDK ID");
    runtime->simulationEnd(ticket);runtime->renderSubmitBegin(ticket);runtime->renderSubmitEnd(ticket);
    return ticket;
}
frame_input::FrameConstantsSample constants(std::uint64_t token) {
    frame_input::FrameConstantsSample sample;
    sample.producerToken=token;sample.renderSize={32,16};sample.upscaleSize={64,32};
    sample.motionVectorScale={1.0f/32,1.0f/16};sample.jitter={0.125f,-0.25f};
    sample.cameraNear=0.1f;sample.cameraFar=100;
    for(unsigned index=0;index<4;++index)sample.view[index*5]=sample.projection[index*5]=1;
    sample.validityMask=frame_input::Extents|frame_input::Jitter|frame_input::MotionScale|
        frame_input::View|frame_input::Projection|frame_input::CameraPlanes|frame_input::Reset;
    return sample;
}
void enableMenu() {
    menuIntent={};menuIntent.controlEpoch=1;
    menuIntent.requestedEnabled=menuIntent.bothGatesReady=menuIntent.nativeSuppressed=menuIntent.inputObserved=true;
    menuIntent.paired=menuIntent.gatesConsistent=true;
}
void offAuxiliaryCaptureTransitions() {
    Owner owner;owner.frame();
    const auto warm=completeTiming();runtime->associatePresentTiming(1,warm,0);owner.frame(false);
    menuIntent={};menuIntent.inputObserved=true; // Explicit OFF needs no ON gate pairing.
    const auto skipped=completeTiming();
    owner.capture(skipped,2,constants(100),fsr_menu_control::auxiliaryCaptureRequired(menuIntent));
    require(owner.provider->stats().skippedOffCaptures==1 && !owner.provider->stats().captured,
            "the production menu policy skips observed OFF even before dual-gate pairing");
    enableMenu();runtime->associatePresentTiming(2,skipped,0);owner.frame(false);
    auto status=runtime->frameStatus();
    require(!status.inputMatched && !status.sdkEnabled && status.framesPresented==1 &&
            status.sdkPresentId==skipped.sdkId && runtime->lastSdkOutputSucceeded() &&
            status.onBlockedReason && !std::strcmp(status.onBlockedReason,"frame_inputs_missing"),
            "OFF recording followed by ON present uses exact timed ordinary output rather than old auxiliary inputs");
    const auto resumed=completeTiming();
    owner.capture(resumed,3,constants(101),fsr_menu_control::auxiliaryCaptureRequired(menuIntent));
    runtime->associatePresentTiming(3,resumed,0);owner.frame(false);
    require(runtime->frameStatus().sdkEnabled && runtime->frameStatus().queuedOn && lastHistoryReset &&
            !owner.provider->stats().consumed,
            "the next exact ON bundle resumes with a history reset and holds its queued GPU slot");
    const auto captured=completeTiming();owner.capture(captured,4,constants(102));
    menuIntent.requestedEnabled=false;runtime->associatePresentTiming(4,captured,0);owner.frame(false);
    require(!runtime->frameStatus().sdkEnabled && owner.provider->stats().consumed==1 &&
            runtime->lastCopyProgress()==presentation::CopyProgress::QueuedOff,
            "ON recording followed by queued OFF leaves submitted auxiliary ownership for real synchronous retirement");
    menuIntent.inputObserved=false;
    const auto unknown=completeTiming();
    owner.capture(unknown,5,constants(103),fsr_menu_control::auxiliaryCaptureRequired(menuIntent));
    runtime->associatePresentTiming(5,unknown,0);owner.frame(false);
    require(owner.provider->stats().captured==3 && owner.provider->stats().skippedOffCaptures==1 &&
            owner.provider->stats().consumed==2 && runtime->lastSdkOutputSucceeded(),
            "unknown menu state retains capture and normal slot release under the same production policy");
}
void queuedOffOwnership() {
    {
        Owner owner(false);owner.frame();
        require(fenceWaits==1 && !queuedOutputs,"fresh owner still requires completed-copy initialization");
        owner.frame();const auto queuedWait=lastNativeSemaphore;
        require(runtime->lastCopyProgress()==presentation::CopyProgress::QueuedOff && !runtime->failed() &&
                !runtime->lastPrivateCopyCompleted() && runtime->lastSdkOutputSucceeded() &&
                fenceWaits==1 && queuedOutputs==1 && lastNativeWaits==1 && queuedWait,
                "initialized pure OFF queues once without waiting the current copy and presents with private completion sync");
        const auto app=h<VkSemaphore>(0x10000+submits-1);
        require(runtime->retainsAppSemaphore(app),"pending healthy queued copy protects its original application wait");
        copyCompleted=true;
        require(!runtime->retainsAppSemaphore(app),"real fence completion permits legal application semaphore destruction before the next present");
        owner.frame();
        require(fenceWaits==2 && queuedOutputs==2 && lastNativeSemaphore==queuedWait && semaphoreCreates==2,
                "single slot waits its prior copy before reuse and reuses only this generation/image completion semaphore");
        owner.info.oldSwapchain=owner.chain;owner.info.presentMode=VK_PRESENT_MODE_IMMEDIATE_KHR;
        require(runtime->create(&owner.info,nullptr,&owner.chain)==VK_SUCCESS && fenceWaits==2,
                "recreation only stages a mode transition without submitting or waiting on the borrowed queue");
        owner.frame();
        require(fenceWaits==4 && !lastNativeWaits && runtime->lastCopyProgress()==presentation::CopyProgress::Completed && vsyncChanges==1,
                "mode transition drains the queued copy then uses the original synchronized path");
    }
    {
        Owner owner;owner.frame();const auto warm=completeTiming();runtime->associatePresentTiming(1,warm,0);owner.frame(false);
        menuIntent.inputObserved=true;
        const auto off=completeTiming();runtime->associatePresentTiming(2,off,0);enableMenuAtPresent=true;owner.frame(false);
        require(menuIntent.requestedEnabled && !runtime->frameStatus().menuRequestedEnabled &&
                !runtime->frameStatus().sdkEnabled && queuedOutputs==1,
                "copy locks explicit OFF for the whole packet even when the menu changes before output");
        enableMenuAtPresent=false;
        const auto next=completeTiming();runtime->associatePresentTiming(3,next,0);owner.frame(false);
        require(runtime->lastCopyProgress()==presentation::CopyProgress::Completed && queuedOutputs==1,
                "a following non-OFF packet resumes the synchronized path after retiring the queued copy");
    }
    {
        Owner owner(false);owner.frame();owner.frame();const auto before=submits;waitResult=VK_TIMEOUT;owner.frame();
        require(submits==before && runtime->failed() && lastNativeWaits==1,
                "previous queued timeout never submits or consumes this new packet's original waits");
        waitResult=VK_SUCCESS;
    }
}
void queuedOnOwnership() {
    const auto prime=[](Owner& owner) {
        owner.frame();const auto warm=completeTiming();
        runtime->associatePresentTiming(1,warm,0);owner.frame(false);enableMenu();
    };
    const auto capture=[](Owner& owner,std::uint32_t engine,std::uint64_t token) {
        const auto ticket=completeTiming();owner.capture(ticket,engine,constants(token));
        runtime->associatePresentTiming(engine,ticket,0);
    };
    {
        Owner owner;prime(owner);capture(owner,2,100);
        const auto waited=fenceWaits;disableMenuAtPresent=true;owner.frame(false);disableMenuAtPresent=false;
        auto status=runtime->frameStatus();const auto first=owner.provider->stats();
        require(status.queuedOn && status.sdkEnabled && status.menuRequestedEnabled && !menuIntent.requestedEnabled &&
                status.framesPresented==2 && queuedOnOutputs==1 && fenceWaits==waited && lastNativeWaits==1 &&
                !runtime->lastPrivateCopyCompleted() && !first.consumed &&
                std::count(first.states.begin(),first.states.end(),continuous::State::Consuming)==1,
                "complete ON is queued without a current fence wait, latches its packet intent, and retains its exact input lease");
        enableMenu();capture(owner,3,101);owner.frame(false);
        require(fenceWaits==waited+1 && bridgeWaits==1 && owner.provider->stats().consumed==1 && queuedOnOutputs==2 &&
                runtime->frameStatus().runtimeCopyCpu.count==4 && runtime->frameStatus().queuedOn,
                "next copy accounts previous-batch backpressure and releases exactly one lease after Bridge consumed");
        menuIntent.requestedEnabled=false;
        const auto off=completeTiming();runtime->associatePresentTiming(4,off,0);owner.frame(false);
        require(bridgeWaits==2 && owner.provider->stats().consumed==2 && runtime->frameStatus().queuedOff &&
                !runtime->frameStatus().sdkEnabled && runtime->frameStatus().framesPresented==1,
                "ON-to-OFF completes the queued source lease before returning to ordinary queued color");
        require(runtime->stop()==VK_NOT_READY && runtime->shutdownState().privateCleanupCompleted &&
                !runtime->shutdownState().failedQuarantined && owner.provider->stats().consumed==2,
                "normal stop completes the final batch without double-releasing queued inputs");
    }
    {
        Owner owner;prime(owner);capture(owner,2,100);nativeResult=VK_ERROR_OUT_OF_DATE_KHR;
        owner.frame(false,VK_ERROR_OUT_OF_DATE_KHR);const auto state=owner.provider->stats();
        require(!state.consumed && std::count(state.states.begin(),state.states.end(),continuous::State::Submitted)==1 &&
                !queuedOnOutputs && !bridgeWaits,
                "native OUT_OF_DATE before Bridge rolls the exact lease back to Submitted without freeing pending writes");
        nativeResult=VK_SUCCESS;
    }
    {
        Owner owner;prime(owner);capture(owner,2,100);const auto submitted=submits;
        throwSemaphore=true;owner.frame(false);throwSemaphore=false;const auto state=owner.provider->stats();
        require(runtime->failed() && submits==submitted && !state.consumed &&
                std::count(state.states.begin(),state.states.end(),continuous::State::Submitted)==1 && lastNativeWaits==1,
                "pre-submit exception unwinds a borrowed lease to Submitted and forwards original waits unchanged");
    }
    {
        Owner owner;prime(owner);capture(owner,2,100);throwQueuedPublication=true;owner.frame(false);
        const auto state=owner.provider->stats();
        require(runtime->failed() && !runtime->lastSdkOutputSucceeded() && !state.consumed &&
                std::count(state.states.begin(),state.states.end(),continuous::State::Consuming)==1 && !bridgeWaits,
                "unknown queued Bridge submission retains its lease even when the old Bridge value could appear complete");
        require(runtime->stop()<0 && !bridgeWaits && !owner.provider->stats().consumed,
                "failed queued publication never uses an old consumed value as a new source-completion proof");
    }
    {
        Owner owner;prime(owner);capture(owner,2,100);owner.frame(false);
        failBridgeWait=true;const auto next=completeTiming();runtime->associatePresentTiming(3,next,0);
        const auto submitted=submits;owner.frame(false);const auto state=owner.provider->stats();
        require(runtime->failed() && submits==submitted && !state.consumed && lastNativeWaits==1 &&
                std::count(state.states.begin(),state.states.end(),continuous::State::Consuming)==1,
                "previous Bridge completion failure retains its lease and never consumes the new packet's original waits");
        failBridgeWait=false;
    }
}
void localFiOffFrameProof() {
    const auto prime=[](Owner& owner) {
        owner.frame();const auto warm=completeTiming();
        runtime->associatePresentTiming(1,warm,0);owner.frame(false);enableMenu();
        menuIntent.localFiOffRequested=menuIntent.localFiOffActive=true;
    };
    const auto proven=[](std::uint64_t token) {
        auto sample=constants(token);
        sample.outputRegion={{0,0,64,32},true,true,true};
        return sample;
    };
    for(bool fake:{false,true}) {
    {
        Owner owner(true,nullptr,{64,32},fake);prime(owner);const auto ticket=completeTiming();
        owner.capture(ticket,2,proven(100));runtime->associatePresentTiming(2,ticket,0);
        menuIntent.nativeSuppressed=false; // N+1 params/gates have cleared the latest global proof.
        owner.frame(false);const auto status=runtime->frameStatus();
        require(status.queuedOn && status.sdkEnabled && status.framesPresented==2 && status.menuNativeSuppressed &&
                status.outputRegion.localFiOffVerified && lastConstants.outputRegion.localFiOffVerified && lastHistoryReset,
                "N's immutable bundle proof admits N even after N+1 clears the global menu proof");
        const auto onState=owner.provider->stats();
        require(!copyCompleted && std::count(onState.states.begin(),onState.states.end(),continuous::State::Consuming)==1,
                "proven ON retains its auxiliary producer until the existing completion path");
        menuIntent.requestedEnabled=false;
        const auto off=completeTiming();owner.capture(off,3,proven(101),false);
        runtime->associatePresentTiming(3,off,0);owner.frame(false);
        const auto state=owner.provider->stats();
        require(!runtime->frameStatus().sdkEnabled && runtime->frameStatus().framesPresented==1 &&
                runtime->frameStatus().queuedOff && state.consumed==1 && state.skippedOffCaptures==1 &&
                std::count(state.states.begin(),state.states.end(),continuous::State::Consuming)==0,
                "LocalFiOff OFF to proven ON to OFF keeps single-frame output and retires the prior auxiliary lease");
        if(fake) require(runtime->stop()==VK_SUCCESS && runtime->shutdownState().privateCleanupCompleted &&
                !runtime->shutdownState().failedQuarantined && runtime->retainedChains()==0 && fakeLayoutsValid &&
                !nativeCreates && !nativeImages && !nativePresents && !nativeDestroys,
                "fake LocalFiOff transition cleans private resources and pools without forwarding any driver WSI call");
    }
    {
        Owner owner(true,nullptr,{64,32},fake);prime(owner);const auto ticket=completeTiming();auto other=ticket;++other.contextGeneration;
        owner.capture(other,2,proven(100));runtime->associatePresentTiming(2,ticket,0);owner.frame(false);
        require(!runtime->frameStatus().sdkEnabled && !runtime->frameStatus().inputMatched &&
                !runtime->frameStatus().menuNativeSuppressed,
                "another complete ticket cannot lend its saved suppression proof to this present");
    }
    {
        Owner owner(true,nullptr,{64,32},fake);prime(owner);const auto ticket=completeTiming();auto sample=proven(100);
        sample.outputRegion.localFiOffVerified=false;
        owner.capture(ticket,2,sample);runtime->associatePresentTiming(2,ticket,0);owner.frame(false);
        require(menuIntent.nativeSuppressed && !runtime->frameStatus().sdkEnabled &&
                !runtime->frameStatus().menuNativeSuppressed,
                "the latest global true cannot authenticate a bundle without its own Compose proof");
    }
    {
        Owner owner(true,nullptr,{64,32},fake);prime(owner);const auto ticket=completeTiming();
        owner.capture(ticket,2,proven(100));runtime->associatePresentTiming(2,ticket,0);
        const auto waited=fenceWaits;revokeLocalAtPresent=!fake;revokeLocalAtCopySubmit=fake;
        owner.frame(false);revokeLocalAtPresent=revokeLocalAtCopySubmit=false;
        require(runtime->frameStatus().queuedOn && !runtime->frameStatus().sdkEnabled &&
                !runtime->frameStatus().menuNativeSuppressed && queuedOnOutputs==0 && queuedOutputs==1 &&
                fenceWaits==waited,
                "revocation between admitted copy and output uses queued color OFF without an extra GPU wait");
        const auto next=completeTiming();runtime->associatePresentTiming(3,next,0);owner.frame(false);
        const auto state=owner.provider->stats();
        require(!runtime->frameStatus().sdkEnabled && !bridgeWaits &&
                std::count(state.states.begin(),state.states.end(),continuous::State::Consuming)==0,
                "unpublished auxiliary lease follows its existing rollback/completed-fence retirement after revocation");
    }
    {
        Owner owner(true,nullptr,{64,32},fake);prime(owner);const auto ticket=completeTiming();
        owner.capture(ticket,2,proven(100));runtime->associatePresentTiming(2,ticket,0);
        revokeLocalAtPresent=!fake;revokeLocalAtCopySubmit=fake;throwQueuedColorUnavailable=true;owner.frame(false);
        revokeLocalAtPresent=revokeLocalAtCopySubmit=throwQueuedColorUnavailable=false;
        auto state=owner.provider->stats();
        require(!copyCompleted && !runtime->lastSdkOutputSucceeded() && !runtime->failed() &&
                std::count(state.states.begin(),state.states.end(),continuous::State::Consuming)==1,
                "temporary color admission refusal retains the still-pending auxiliary lease instead of freeing its producer");
        const auto waited=fenceWaits;
        require(runtime->stop()==(fake?VK_SUCCESS:VK_NOT_READY) && fenceWaits==waited+(fake?2:1) && !runtime->shutdownState().failedQuarantined,
                "shutdown supplies the original completion fence after temporary color refusal");
        state=owner.provider->stats();
        require(std::count(state.states.begin(),state.states.end(),continuous::State::Consuming)==0 && !bridgeWaits,
                "only that completed fence rolls back/retires the unpublished source, without a fabricated Bridge consumption");
    }
    }
}
void sdkResultFailureFacts() {
    for(const bool fake:{false,true}) {
        {
            auto measured=std::make_unique<measurement::Window>(cpuWallNowNs());measurement::Scope measurementScope(measured.get());
            Owner owner(true,nullptr,{64,32},fake);queryResult=-77;
            owner.frame(true,fake?VK_ERROR_DEVICE_LOST:VK_SUCCESS);queryResult=0;
            const auto status=runtime->frameStatus();
            require(runtime->lastSdkOutputAttempted()&&!runtime->lastSdkOutputSucceeded()&&status.sdkResultKnown&&
                    !status.sdkStatusValid&&status.queryResult==-77&&status.sdkPresentId==1,
                    "a failed initial untimed SDK query keeps its actual query ID rather than an old last-presented ID");
            measurement::SummarySnapshot summary;
            require(measured->takeSummary(cpuWallNowNs(),summary,true)&&summary.counters[static_cast<unsigned>(measurement::Counter::SdkErrors)]==1&&
                    !summary.counters[static_cast<unsigned>(measurement::Counter::SdkWarnings)],"untimed negative query is counted exactly once in the legacy summary window");
        }
        struct Case {
            Owner owner;
            std::uint32_t engine{2};std::uint64_t token{900};
            explicit Case(bool fake):owner(true,nullptr,{64,32},fake) {
                owner.frame();const auto warm=completeTiming();runtime->associatePresentTiming(1,warm,0);owner.frame(false);enableMenu();
            }
            presentation::FrameStatus step(VkResult expected=VK_SUCCESS) {
                const auto ticket=completeTiming();owner.capture(ticket,engine,constants(token++));
                runtime->associatePresentTiming(engine++,ticket,0);owner.frame(false,expected);return runtime->frameStatus();
            }
        };
        {
            auto measured=std::make_unique<measurement::Window>(cpuWallNowNs());measurement::Scope measurementScope(measured.get());
            Case c(fake);queryResult=-123;
            const auto status=c.step(fake?VK_ERROR_DEVICE_LOST:VK_SUCCESS);queryResult=0;
            require(runtime->lastSdkOutputAttempted()&&!runtime->lastSdkOutputSucceeded()&&status.sdkResultKnown&&
                    !status.sdkStatusValid&&status.queryResult==-123&&status.sdkPresentId&&status.inputMatched&&
                    status.producerToken==900&&status.engineFrame==2,
                    "current negative SDK query survives the Native exception and Runtime failure with its exact input identity");
            measurement::SummarySnapshot summary;
            require(measured->takeSummary(cpuWallNowNs(),summary,true)&&summary.counters[static_cast<unsigned>(measurement::Counter::SdkErrors)]==1&&
                    !summary.counters[static_cast<unsigned>(measurement::Counter::SdkWarnings)],"timed negative query is counted exactly once in the legacy summary window");
        }
        {
            Case c(fake);frameGenResult=-9;const auto failed=c.step();frameGenResult=0;
            require(failed.sdkResultKnown&&failed.sdkStatusValid&&failed.queryResult==0&&failed.frameGenResult==-9,
                    "a returned negative frame-generation result is known independently of output success");
            const auto before=outputs;throwQueuedPublication=true;
            const auto noQuery=c.step(fake?VK_ERROR_DEVICE_LOST:VK_SUCCESS);throwQueuedPublication=false;
            require(runtime->lastSdkOutputAttempted()&&!runtime->lastSdkOutputSucceeded()&&!noQuery.sdkResultKnown&&
                    !noQuery.sdkStatusValid&&!noQuery.queryResult&&!noQuery.frameGenResult&&outputs==before&&
                    noQuery.inputMatched&&noQuery.producerToken==901&&noQuery.engineFrame==3,
                    "an exception before this SDK query cannot reuse the previous negative result even if ColorPresenter still returns it");
        }
    }
}
void actualOnFormula() {
    Owner owner;owner.frame();
    const auto warm=completeTiming();runtime->associatePresentTiming(1,warm,0);owner.frame(false);
    std::uint32_t engine=2;std::uint64_t token=100;
    const auto output=[&](frame_input::FrameConstantsSample sample,bool capture=true,int sourceOffset=0,bool otherContext=false) {
        const auto ticket=completeTiming();
        require(ticket.latencyEnabled,"complete prior OFF admits subsequent LL1 tickets");
        auto sourceTicket=ticket;
        if(otherContext)++sourceTicket.contextGeneration;
        if(capture)owner.capture(sourceTicket,engine+sourceOffset,sample);
        runtime->associatePresentTiming(engine++,ticket,0);owner.frame(false);
        require(runtime->lastSdkOutputSucceeded(),"candidate ON and guarded OFF retain actual Runtime output success");
        return runtime->frameStatus();
    };
    enableMenu();auto status=output(constants(token++));
    require(lastBundleEnabled && status.sdkEnabled && status.requestedEnabled && status.framesPresented==2 &&
            !status.onBlockedReason && lastHistoryReset && status.menuPaired && status.menuGatesConsistent &&
            status.menuNativeSuppressed,"complete exact input and verified dual shadows reach SDK enabled=true through actual Native output");
    require(status.depthExtent.width==32 && status.depthExtent.height==16 &&
            status.motionExtent.width==32 && status.motionExtent.height==16 &&
            status.renderSize==std::array<std::int32_t,2>{32,16} &&
            status.upscaleSize==std::array<std::int32_t,2>{64,32},
            "healthy Native diagnostics retain this exact bundle's physical and constant dimensions");
    status=output(constants(token++));
    require(lastBundleEnabled && !lastHistoryReset,"consecutive ON preserves temporal history");
    require(status.historyResetKnown && status.historyResetReasons==0 && !status.longFrameReset.requested,
            "actual history preservation remains known without the GPU reset diagnostic");
    auto sample=constants(token++);sample.reset=1;status=output(sample);
    require(lastHistoryReset && lastConstants.reset==1,"same-source real history reset reaches Native output");
    require(status.historyResetKnown && (status.historyResetReasons&presentation::HistoryResetInput) && !status.longFrameReset.requested,
            "actual input reset reasons remain known without the GPU reset diagnostic");
    output(constants(token++));require(!lastHistoryReset,"next source clears a one-frame reset");
    menuIntent.requestedEnabled=false;status=output(constants(token++));
    require(!status.sdkEnabled && status.framesPresented==1 && status.queuedOff,
            "menu OFF queues ordinary presentation without borrowing unused auxiliary inputs");
    enableMenu();++menuIntent.controlEpoch;output(constants(token++));
    require(lastBundleEnabled && lastHistoryReset,"OFF-to-ON transition resets history");
    ++menuIntent.controlEpoch;output(constants(token++));
    require(lastHistoryReset,"menu epoch transition resets ON history");
    ++token;output(constants(token++));require(lastHistoryReset,"missing producer token resets history");
    struct Refusal {
        const char* reason;
        void (*change)(fsr_menu_control::ControlIntent&,frame_input::FrameConstantsSample&);
    };
    const Refusal refused[]{
        {"menu_gates_not_ready",[](auto& m,auto&){m.bothGatesReady=false;}},
        {"menu_input_not_observed",[](auto& m,auto&){m.inputObserved=false;}},
        {"menu_gates_unpaired",[](auto& m,auto&){m.paired=false;}},
        {"menu_gates_inconsistent",[](auto& m,auto&){m.gatesConsistent=false;}},
        {"native_fsr_not_suppressed",[](auto& m,auto&){m.nativeSuppressed=false;}},
        {"frame_constants_incomplete",[](auto&,auto& s){s.validityMask&=~frame_input::Reset;}},
        {"jitter_out_of_sdk_range",[](auto&,auto& s){s.jitter[0]=0.51f;}},
        {"motion_scale_zero",[](auto&,auto& s){s.motionVectorScale[1]=0;}},
        {"frame_extent_mismatch",[](auto&,auto& s){s.renderSize[0]=31;}},
        {"frame_extent_mismatch",[](auto&,auto& s){s.upscaleSize[1]=31;}}
    };
    for(const auto& refusal:refused) {
        enableMenu();sample=constants(token++);refusal.change(menuIntent,sample);
        status=output(sample);
        require(!lastBundleEnabled && !status.sdkEnabled && status.framesPresented==1 && status.onBlockedReason &&
                !std::strcmp(status.onBlockedReason,refusal.reason),"each real Native ON guard fails closed with its specific current reason");
        require(status.menuPaired==menuIntent.paired && status.menuGatesConsistent==menuIntent.gatesConsistent &&
                status.menuNativeSuppressed==menuIntent.nativeSuppressed && status.menuInputObserved==menuIntent.inputObserved &&
                status.menuGatesReady==menuIntent.bothGatesReady,"rejected ON status describes the same intent used by the Native decision");
        require(status.depthExtent.width==32 && status.depthExtent.height==16 &&
                status.motionExtent.width==32 && status.motionExtent.height==16 &&
                status.renderSize==sample.renderSize && status.upscaleSize==sample.upscaleSize,
                "rejected ON retains actual same-bundle extents including render or upscale mismatch values");
    }
    enableMenu();status=output(constants(token++),false);
    require(!status.sdkEnabled && !std::strcmp(status.onBlockedReason,"frame_inputs_missing"),"missing actual capture stays OFF despite valid menu/timing");
    require(!status.depthExtent.width && !status.depthExtent.height && !status.motionExtent.width &&
            !status.motionExtent.height && status.renderSize==std::array<std::int32_t,2>{} &&
            status.upscaleSize==std::array<std::int32_t,2>{},
            "missing bundle clears every physical and constant dimension instead of publishing previous-frame data");
    status=output(constants(token++),true,1);
    require(!status.inputMatched && !status.sdkEnabled && !std::strcmp(status.onBlockedReason,"frame_inputs_missing"),
            "another engine frame cannot supply this ticket's ON inputs");
    status=output(constants(token++),true,0,true);
    require(!status.inputMatched && !status.sdkEnabled && !std::strcmp(status.onBlockedReason,"frame_inputs_missing"),
            "another SDK context cannot supply matching-frame ON inputs");
    require(owner.provider->stop(h<VkFence>(0xF001))==VK_SUCCESS,"proved mock queue tail retires the unmatched source");
    enableMenu();changeMenuDuringOutput=true;status=output(constants(token++));changeMenuDuringOutput=false;
    require(status.menuPaired && status.menuGatesConsistent && status.menuNativeSuppressed && status.controlEpoch==1 &&
            !menuIntent.paired && menuIntent.controlEpoch==2,
            "FrameStatus retains the intent actually used by Native even when another builder changes the global menu during output");
}
void gameplayStageAdmission() {
    for(bool shared:{false,true}){
        Owner owner(true,nullptr,{64,32},shared,false,0,false,false,2,shared,false,shared);
        owner.frame();const auto warm=completeTiming();runtime->associatePresentTiming(1,warm,0);owner.frame(false);
        enableMenu();menuIntent.gameplayGateRequired=true;
        std::uint32_t engine=2;std::uint64_t token=100;
        const auto stage=[&](std::int32_t value){
            menuIntent.gameplayState=value;menuIntent.gameplayReady=value==gameplay_state::active;++menuIntent.controlEpoch;
            const auto ticket=completeTiming();
            owner.capture(ticket,engine,constants(token++),fsr_menu_control::auxiliaryCaptureRequired(menuIntent));
            runtime->associatePresentTiming(engine++,ticket,0);owner.frame(false);return runtime->frameStatus();
        };
        for(auto value:{gameplay_state::unknown,0,10,20,30}){
            const auto status=stage(value);
            require(status.menuRequestedEnabled&&!status.sdkEnabled&&status.framesPresented==1&&status.gameplayGateRequired&&
                !status.gameplayReady&&status.onBlockedReason&&std::string_view(status.onBlockedReason)=="not_in_gameplay",
                "login/loading keeps the saved ON request but submits SDK enabled=false and one real frame");
        }
        auto status=stage(gameplay_state::active);
        require(status.sdkEnabled&&status.framesPresented==2&&status.gameplayReady&&status.historyResetKnown&&
                (status.historyResetReasons&presentation::HistoryResetGap),
                "the first gameplay frame resumes real interpolation with fresh history");
        status=stage(10);
        require(!status.sdkEnabled&&status.framesPresented==1&&!runtime->failed(),"returning to login stops FG without failing the rendering path");
        status=stage(gameplay_state::active);
        require(status.sdkEnabled&&status.framesPresented==2,"loading completion can resume FG repeatedly");
        leaveGameplayAtCopySubmit=true;status=stage(gameplay_state::active);leaveGameplayAtCopySubmit=false;
        require(!status.sdkEnabled&&status.framesPresented==1&&!status.gameplayReady&&status.gameplayState==10&&
                status.onBlockedReason&&std::string_view(status.onBlockedReason)=="not_in_gameplay"&&!runtime->failed(),
                "a gameplay exit after ON copy admission revokes interpolation at output and keeps the queued color frame");
        menuIntent.requestedEnabled=false;status=stage(gameplay_state::active);
        require(!status.sdkEnabled&&status.framesPresented==1,"gameplay cannot override an explicit menu OFF request");
    }
}
void mappedOutputRegion() {
    Owner owner;owner.frame();
    const auto warm=completeTiming();runtime->associatePresentTiming(1,warm,0);owner.frame(false);
    surfaceExtent={2880,1800};
    SetWindowPos(owner.window,nullptr,0,0,2880,1800,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    owner.info.oldSwapchain=owner.chain;owner.info.imageExtent=surfaceExtent;
    require(runtime->create(&owner.info,nullptr,&owner.chain)==VK_SUCCESS,"stage actual final letterboxed backbuffer extent");
    enableMenu();
    std::uint32_t engine=2;std::uint64_t token=100;
    auto sample=constants(token);
    sample.renderSize={1128,634};sample.upscaleSize={1920,1080};
    sample.motionVectorScale={1.0f/1128,1.0f/634};
    const auto output=[&](frame_input::FrameConstantsSample value,bool capture=true) {
        value.producerToken=token++;
        const auto ticket=completeTiming();
        if(capture)owner.capture(ticket,engine,value,true,{1128,634});
        runtime->associatePresentTiming(engine++,ticket,0);owner.frame(false);
        return runtime->frameStatus();
    };
    auto status=output(sample);
    require(!status.sdkEnabled && !std::strcmp(status.onBlockedReason,"frame_extent_mismatch"),
            "different raw reconstruction and final output still reject ON without a proven mapping");
    sample.outputRegion={{0,90,2880,1620},true,true};
    status=output(sample);
    require(lastBundleEnabled && status.sdkEnabled && status.framesPresented==2 && lastHistoryReset &&
            lastOutputRect.offset.x==0 && lastOutputRect.offset.y==90 &&
            lastOutputRect.extent.width==2880 && lastOutputRect.extent.height==1620 &&
            lastConstants.renderSize==std::array<std::int32_t,2>{1128,634} &&
            lastConstants.upscaleSize==std::array<std::int32_t,2>{1920,1080} &&
            lastConstants.motionVectorScale==sample.motionVectorScale && status.outputRegion.rect==sample.outputRegion.rect,
            "same-bundle final pixel region permits exact low-resolution inputs without rewriting raw constants or normalized motion");
    output(sample);require(!lastHistoryReset,"stable mapped region preserves ON history");
    sample.outputRegion.rect={0,80,2880,1640};
    output(sample);require(lastHistoryReset,"a real same-owner output region change resets history");
    const auto valid=sample;
    for(const auto region : {
        frame_input::OutputRegionSample{{0,90,2880,1620},true,false},
        frame_input::OutputRegionSample{{-1,90,2880,1620},true,true},
        frame_input::OutputRegionSample{{0,90,2880,1800},true,true},
        frame_input::OutputRegionSample{{0,0,INT32_MAX,INT32_MAX},true,true},
        frame_input::OutputRegionSample{{0,0,0,0},true,true}}) {
        sample.outputRegion=region;status=output(sample);
        require(!status.sdkEnabled && !std::strcmp(status.onBlockedReason,"frame_output_region_invalid"),
                "observed-invalid, negative, out-of-bounds, overflow-sized and empty regions never become a full-output default");
    }
    sample=valid;sample.renderSize[0]=1127;status=output(sample);
    require(!status.sdkEnabled && !std::strcmp(status.onBlockedReason,"frame_extent_mismatch"),
            "a valid output region does not relax physical input versus reconstruction size");
    status=output(valid,false);
    require(!status.sdkEnabled && !status.outputRegion.observed && !status.outputRegion.valid &&
            status.outputRegion.rect==std::array<std::int32_t,4>{},
            "missing exact bundle clears region provenance instead of inheriting the previous mapping");
    output(valid);require(lastBundleEnabled && lastHistoryReset,"next exact region and bundle recover ON after the missing frame");
    menuIntent.requestedEnabled=false;status=output(valid);
    require(status.queuedOff && !status.sdkEnabled && status.framesPresented==1,
            "mapped ON-to-OFF keeps the existing queued color-only path");
}
void resizeAndWindowRecovery() {
    Owner owner;
    owner.frame();
    const auto warm=completeTiming();
    runtime->associatePresentTiming(1,warm,0);
    owner.frame(false);
    enableMenu();
    std::uint32_t engine=2;
    std::uint64_t token=100;
    const auto output=[&](VkExtent2D outputSize,FrameTimingTicket ticket={}) {
        if(!ticket)ticket=completeTiming();
        auto sample=constants(token++);
        sample.upscaleSize={static_cast<std::int32_t>(outputSize.width),static_cast<std::int32_t>(outputSize.height)};
        owner.capture(ticket,engine,sample);
        runtime->associatePresentTiming(engine++,ticket,0);
        owner.frame(false);
        return runtime->frameStatus();
    };
    output({64,32});
    require(lastBundleEnabled,"pre-resize exact inputs reach ON");
    const auto context=runtime->timingContextGeneration();
    const auto future=completeTiming();
    const auto submissionsBefore=submits;
    surfaceExtent={96,48};
    SetWindowPos(owner.window,nullptr,0,0,96,48,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    owner.info.oldSwapchain=owner.chain;
    owner.info.imageExtent=surfaceExtent;
    require(runtime->create(&owner.info,nullptr,&owner.chain)==VK_SUCCESS && !resizes && submits==submissionsBefore,
            "create only validates and stages the new extent without touching either queue or resizing the SDK");
    auto status=output(surfaceExtent,future);
    require(resizes==1 && constructions==1 && runtime->timingContextGeneration()==context &&
            status.sdkPresentId==future.sdkId && lastBundleEnabled && lastHistoryReset,
            "new-chain first present resizes once and keeps the already reserved ticket, context and LL1 owner");
    output(surfaceExtent);
    require(lastBundleEnabled && !lastHistoryReset,"next exact new-size input preserves recovered ON history");
    status=output({64,32});
    require(!status.sdkEnabled && status.onBlockedReason && !std::strcmp(status.onBlockedReason,"frame_extent_mismatch"),
            "old-size constants are returned normally as OFF instead of reaching the SDK with incompatible resources");
    output(surfaceExtent);
    require(lastBundleEnabled && lastHistoryReset,"new exact inputs resume ON with reset after old-size OFF");
    SetWindowPos(owner.window,nullptr,0,0,128,64,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    status=output(surfaceExtent);
    require(!status.sdkEnabled && !std::strcmp(status.onBlockedReason,"window_extent_transition") && resizes==1 &&
            runtime->timingContextGeneration()==context,"client change before Vulkan rebuild stretches the positive old output and keeps timing alive");
    SetWindowPos(owner.window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    status=output(surfaceExtent);
    require(!status.sdkEnabled && runtime->lastSdkOutputSucceeded() && !lastNativeWaits && resizes==1,
            "zero client extent still completes real copy/waits/ID without resizing resources to zero");
    SetWindowPos(owner.window,nullptr,0,0,96,48,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    occluded=true;
    status=output(surfaceExtent);
    require(status.outputOccluded && !status.sdkStatusValid && !status.framesPresented && runtime->lastSdkOutputSucceeded(),
            "completed but occluded SDK present consumes its real ID without claiming visible output");
    const auto occludedId=status.sdkPresentId;
    occluded=false;
    status=output(surfaceExtent);
    require(lastBundleEnabled && lastHistoryReset && status.sdkPresentId>occludedId && !status.outputOccluded &&
            runtime->timingContextGeneration()==context && constructions==1,
            "occlusion restores ON on a strictly newer ID with history reset and no owner reconstruction");
    require(runtime->stop()==VK_NOT_READY && runtime->shutdownState().privateCleanupCompleted &&
            !runtime->shutdownState().failedQuarantined,
            "resize/restore stop finishes the last queued input and retains only the real final WSI tail");
    require(owner.provider->stats().captured==owner.provider->stats().consumed,
            "every old/new-size and temporarily invisible bundle returns its real Provider slot");
}
void resizeFailureOwnership() {
    Owner owner;
    owner.frame();
    surfaceExtent={96,48};
    owner.info.oldSwapchain=owner.chain;
    owner.info.imageExtent=surfaceExtent;
    require(runtime->create(&owner.info,nullptr,&owner.chain)==VK_SUCCESS && !resizes,
            "failing resize is still not attempted during create");
    failResize=true;
    owner.frame();
    require(resizes==1 && constructions==1 && submits==1 && lastNativeWaits==1 && nativePresents==2,
            "resize failure occurs before any new copy submission and forwards original waits once");
}
void lowResOutputPrototype() {
    const auto prime=[](Owner& owner) {
        owner.frame();const auto warm=completeTiming();runtime->associatePresentTiming(1,warm,0);owner.frame(false);enableMenu();
    };
    const auto mapped=[](std::uint64_t token) {auto s=constants(token);s.outputRegion={{0,6,96,48},true,true};return s;};
    const auto capture=[](Owner& owner,std::uint32_t engine,frame_input::FrameConstantsSample sample) {
        const auto ticket=completeTiming();owner.capture(ticket,engine,sample);runtime->associatePresentTiming(engine,ticket,0);
    };
    for(const auto flag:{static_cast<const wchar_t*>(nullptr),L"0",L"true",L"01",L"11"}) {
        Owner owner(true,flag,{96,60});prime(owner);capture(owner,2,mapped(100));const auto waited=fenceWaits;owner.frame(false);
        const auto status=runtime->frameStatus();
        require(!status.lowResOutputRequested&&!status.lowResOutputActive&&status.sdkEnabled&&colorCopies==3&&!colorBlits&&!resizes&&fenceWaits==waited,
                "only exact environment 1 enables the prototype; default path retains its copy and wait counts");
    }
    {
        Owner owner(true,L"1",{96,60});prime(owner);capture(owner,2,mapped(100));const auto waited=fenceWaits;owner.frame(false);
        auto status=runtime->frameStatus();
        require(status.lowResOutputRequested&&status.lowResOutputActive&&status.sdkEnabled&&!status.lowResOutputFallbackReason&&
                status.outputWidth==96&&status.outputHeight==60&&status.outputRegion.rect==std::array<std::int32_t,4>{0,6,96,48}&&
                status.xefgSourceExtent.width==96&&status.xefgSourceExtent.height==60&&status.xefgOutputExtent.width==64&&status.xefgOutputExtent.height==40&&
                status.xefgRegion.offset.y==4&&status.xefgRegion.extent.width==64&&status.xefgRegion.extent.height==32,
                "low-res status separates physical WSI and original region from the low canvas and mapped SDK region");
        require(colorCopies==2&&colorBlits==1&&lastColorSource==h<VkImage>(0xF00)&&lastBlitFilter==VK_FILTER_LINEAR&&
                !lastBlit.srcOffsets[0].x&&!lastBlit.srcOffsets[0].y&&!lastBlit.dstOffsets[0].x&&!lastBlit.dstOffsets[0].y&&
                lastBlit.srcOffsets[1].x==96&&lastBlit.srcOffsets[1].y==60&&lastBlit.dstOffsets[1].x==64&&lastBlit.dstOffsets[1].y==40&&
                stableExtents.back().width==64&&stableExtents.back().height==40&&lastPublishedColor.width==64&&lastPublishedColor.height==40&&
                lastOutputRect.offset.y==4&&lastOutputRect.extent.height==32&&resizes==1&&fenceWaits==waited,
                "one direct complete-WSI linear blit initializes a low stable image and shares only that canvas without a high-size copy");
        capture(owner,3,mapped(101));owner.frame(false);
        require(runtime->frameStatus().lowResOutputActive&&colorBlits==2&&colorCopies==2&&resizes==1&&!lastHistoryReset&&owner.provider->stats().consumed==1,
                "same low extent reuses its owner and stable image while the previous real consumer releases exactly one lease");
        const auto missing=completeTiming();runtime->associatePresentTiming(4,missing,0);owner.frame(false);
        status=runtime->frameStatus();require(!status.lowResOutputActive&&status.lowResOutputHeld&&!status.sdkEnabled&&
                status.xefgOutputExtent.width==64&&status.xefgOutputExtent.height==40&&status.lowResOutputFallbackReason&&
                resizes==1&&colorCopies==2&&colorBlits==3&&lastBlitFilter==VK_FILTER_LINEAR&&
                lastBlit.dstOffsets[1].x==64&&lastBlit.dstOffsets[1].y==40,
                "missing same-frame bundle stays OFF in the held low canvas instead of resizing the proxy swapchain");
        capture(owner,5,mapped(102));owner.frame(false);status=runtime->frameStatus();
        require(status.lowResOutputActive&&!status.lowResOutputHeld&&resizes==1&&lastHistoryReset,"exact later bundle re-enters low output with a history reset and no resize");
        menuIntent.requestedEnabled=false;const auto off=completeTiming();runtime->associatePresentTiming(6,off,0);owner.frame(false);
        status=runtime->frameStatus();require(!status.lowResOutputActive&&status.lowResOutputHeld&&!status.sdkEnabled&&status.queuedOff&&resizes==1&&
                status.xefgOutputExtent.width==64&&status.xefgOutputExtent.height==40,"ON-to-OFF retires low inputs and keeps the low canvas (game menus)");
    }
    {
        Owner owner(true,L"1",{96,60});prime(owner);capture(owner,2,mapped(100));owner.frame(false);
        surfaceExtent={120,75};SetWindowPos(owner.window,nullptr,0,0,120,75,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        owner.info.oldSwapchain=owner.chain;owner.info.imageExtent=surfaceExtent;
        require(runtime->create(&owner.info,nullptr,&owner.chain)==VK_SUCCESS&&resizes==1,"physical WSI recreation only stages its separate output transition");
        auto sample=mapped(101);sample.outputRegion.rect={0,15,120,60};capture(owner,3,sample);owner.frame(false);
        auto status=runtime->frameStatus();require(!status.lowResOutputActive&&status.sdkEnabled&&status.xefgSourceExtent.width==120&&
                status.xefgOutputExtent.width==120&&status.xefgOutputExtent.height==75&&resizes==2,"physical transition frame uses the completed original full-resolution path");
        sample.producerToken=102;capture(owner,4,sample);owner.frame(false);status=runtime->frameStatus();
        require(status.lowResOutputActive&&status.xefgOutputExtent.width==64&&status.xefgOutputExtent.height==40&&
                status.xefgRegion.offset.y==8&&resizes==3&&lastHistoryReset,"new physical WSI maps its non-centered scene into a fresh low canvas");
        sample.producerToken=103;sample.upscaleSize={32,16};capture(owner,5,sample);owner.frame(false);status=runtime->frameStatus();
        require(status.lowResOutputActive&&status.xefgOutputExtent.width==32&&status.xefgOutputExtent.height==20&&
                status.xefgRegion.offset.y==4&&resizes==4&&lastHistoryReset,"same physical WSI can stage a different same-frame logical output size");
        sample.producerToken=104;capture(owner,6,sample);owner.frame(false);require(resizes==4&&!lastHistoryReset,"unchanged smaller logical target reuses the new canvas");
        sample.producerToken=105;sample.upscaleSize={120,60};capture(owner,7,sample);owner.frame(false);status=runtime->frameStatus();
        require(!status.lowResOutputActive&&status.sdkEnabled&&!std::strcmp(status.lowResOutputFallbackReason,"low_res_not_smaller")&&
                status.xefgOutputExtent.width==120&&resizes==5,"already-native logical output uses the original copy without a no-benefit blit");
        const auto gap=completeTiming();runtime->associatePresentTiming(8,gap,0);owner.frame(false);status=runtime->frameStatus();
        require(!status.lowResOutputActive&&!status.lowResOutputHeld&&status.xefgOutputExtent.width==120&&resizes==5,
                "an ON frame that rejects low output drops the held canvas, so later OFF frames stay full resolution");
    }
    for(unsigned variant=0;variant<9;++variant) {
        Owner owner(true,L"1",{96,60});prime(owner);auto sample=mapped(100);
        if(variant==4)sample.outputRegion.rect[1]=7;
        if(variant==5||variant==7||variant==8) {
            linearFilterSupported=variant!=5;missingFormatFeatures=variant==7?VK_FORMAT_FEATURE_BLIT_SRC_BIT:variant==8?VK_FORMAT_FEATURE_BLIT_DST_BIT:0;
            owner.info.oldSwapchain=owner.chain;require(runtime->create(&owner.info,nullptr,&owner.chain)==VK_SUCCESS,"feature check preserves the supported original copy fallback");
        }
        if(variant==6)sample.upscaleSize={16,8};
        capture(owner,2,sample);const auto submitted=submits;
        failStableCreate=variant==0;throwSemaphore=variant==1;
        if(variant==2)submitResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;
        throwQueuedPublication=variant==3;owner.frame(false);
        failStableCreate=throwSemaphore=throwQueuedPublication=false;submitResult=VK_SUCCESS;
        const auto stats=owner.provider->stats();
        if(variant<=2)require(runtime->failed()&&!stats.consumed&&lastNativeWaits==1&&
                std::count(stats.states.begin(),stats.states.end(),continuous::State::Submitted)==1&&submits==submitted+(variant==2),
                "pre-submit allocation/exception or OOM returns the lease to Submitted and preserves application wait ownership");
        else if(variant==3)require(runtime->failed()&&!stats.consumed&&std::count(stats.states.begin(),stats.states.end(),continuous::State::Consuming)==1,
                "unknown queued publication retains the exact low-mode lease without an old completion proof");
        else require(!runtime->frameStatus().lowResOutputActive&&runtime->frameStatus().sdkEnabled&&runtime->frameStatus().lowResOutputFallbackReason&&
                !colorBlits&&colorCopies==3,"nonintegral/input/linear-feature failures use the original full-resolution ON path");
    }
    SetEnvironmentVariableW(L"XESS_XEFG_LOW_RES_OUTPUT",nullptr);
}
}
namespace fixture {
void gpuTimestamps() {
    for(const auto* value:{static_cast<const wchar_t*>(nullptr),L"0",L"01",L"11",L"1 ",L"1"}){
        SetEnvironmentVariableW(L"XESS_GPU_TIMESTAMPS",value);
        require(presentation::gpuTimestampsRequested()==(value&&std::wcscmp(value,L"1")==0),"GPU timestamps need exact independent request");
    }
    {
        Owner owner(false);owner.frame();
        require(!timestampProperties&&!timestampCreates&&!timestampWrites&&!timestampReads&&!presenterTimestamps,
                "default OFF performs no timestamp capability, query or calibration calls");
    }
    for(bool fake:{false,true}){
        Owner owner(false,nullptr,{64,32},fake,true,0,!fake);
        SetEnvironmentVariableW(L"XESS_GPU_TIMESTAMPS",nullptr); // The device admission is frozen.
        runtime->associatePresentTiming(71,{},0);owner.frame(false);
        auto sample=runtime->frameStatus().vkPrivateCopyGpu;
        require(sample.requested&&sample.available&&sample.durationKnown&&sample.durationNs==80&&sample.validBits==36&&
                    sample.sourceEngineFrame==71&&sample.sourceEngineFrameKnown&&!sample.sdkPresentIdKnown&&presenterTimestamps,
                "both native/fake freeze timestamp opt-in and the original copy's identity");
        require(sample.sourceCopyTokenKnown==fake&&((fake&&sample.sourceCopyToken==1)||(!fake&&!sample.sourceCopyToken)),
                "diagnostic serial does not expand fake-only source completion tokens into native mode");
        const auto interop=runtime->frameStatus().vkInteropCopyGpu;
        require(interop.requested&&!interop.available&&interop.sourceEngineFrameKnown&&interop.sourceEngineFrame==71&&
                    !interop.sdkPresentIdKnown&&interop.sourceCopyTokenKnown==fake&&(!fake||interop.sourceCopyToken==1),
                "Interop inherits frozen opt-in and recorded token/frame identity independently of query availability");
        require(sample.calibration.available==!fake&&sample.calibration.maxDeviationKnown==!fake&&
                    (fake||sample.calibration.maxDeviationNs==42),"calibration requires an actually enabled supported extension");
        const auto context=sample.contextGeneration;
        runtime->associatePresentTiming(72,{},0);owner.frame(false);
        require(timestampReads==1,"queued copy does not add a current-frame GPU wait or early query read");
        const auto reads=timestampReads;runtime->retainsAppSemaphore(h<VkSemaphore>(0x10000+submits-1));
        require(timestampReads==reads,"acquire/completion polling never reads or resets timestamp queries");
        runtime->associatePresentTiming(73,{},0);owner.frame(false);sample=runtime->frameStatus().vkPrivateCopyGpu;
        require(sample.sequence==2&&sample.contextGeneration==context&&sample.sourceEngineFrame==72&&timestampReads==2&&timestampWrites==6,
                "delayed query read retains the previous source frame instead of the current frame");
        const auto stopped=runtime->stop();require((stopped==VK_SUCCESS||(!fake&&stopped==VK_NOT_READY))&&timestampReads==3&&timestampDestroys==1,
                "existing shutdown copy proof reads the final query and releases its pool without another wait");
    }
    for(bool khr:{false,true}){
        Owner owner(false,nullptr,{64,32},true,true,0,true,true,2,false,khr);
        for(unsigned frame=80;frame<84;++frame){runtime->associatePresentTiming(frame,{},0);owner.frame(false);}
        const auto sample=runtime->frameStatus().vkPrivateCopyGpu;
        double aligned{};
        require(sample.available&&sample.calibration.available&&sample.calibration.maxDeviationKnown&&
            sample.calibration.maxDeviationNs==42&&timestampQpcUs(sample,sample.beginTicks,aligned),
            "EXT/KHR calibration reaches asynchronous fake frame slots with a bounded device-to-QPC mapping");
    }
    for(unsigned failure=1;failure<=8;++failure){
        Owner owner(false,nullptr,{64,32},true,true,failure,failure==8);
        owner.frame();owner.frame();owner.frame();const auto sample=runtime->frameStatus().vkPrivateCopyGpu;
        require(!runtime->failed()&&!runtime->shutdownState().failedQuarantined&&runtime->lastSdkOutputSucceeded(),
                "timestamp capability/create/read/exception failure cannot fail the game path");
        if(failure<=7)require(sample.requested&&!sample.available,"failed timestamp diagnostic remains explicitly unavailable");
        else require(sample.available&&!sample.calibration.available,"missing optional calibration keeps raw duration sampling");
        if(failure>=5&&failure<=7)require(timestampReads==1&&timestampWrites==2,"query read failure stops only subsequent diagnostic commands");
    }
    SetEnvironmentVariableW(L"XESS_GPU_TIMESTAMPS",nullptr);
}
void asyncFrameConfiguration() {
    struct ScratchFile {
        wchar_t path[MAX_PATH]{};
        ScratchFile() {
            wchar_t directory[MAX_PATH]{};
            const auto count=GetTempPathW(MAX_PATH,directory);
            require(count&&count<MAX_PATH&&GetTempFileNameW(directory,L"xcp",0,path),"create an owned temporary numeric configuration");
        }
        ~ScratchFile() { DeleteFileW(path); }
        void write(std::string_view text) {
            const auto file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            require(file!=INVALID_HANDLE_VALUE,"open owned temporary configuration");
            DWORD written{};const auto result=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr);
            CloseHandle(file);require(result&&written==text.size(),"write owned temporary configuration");
        }
    } scratch;
    struct ConfigCase { std::string_view text; std::uint32_t expected; };
    const ConfigCase cases[]={{"",2},{" \t\r\n",2},{"1",1},{"2",2},{" 1\r\n",1},{"1               ",1},{"\xEF\xBB\xBF" "1\r\n",1},
        {"0",2},{"3",2},{"-1",2},{"01",2},{"1x",2},{"1 2",2},{std::string_view{"1\0",2},2},{"9999999999999999",2}};
    for(const auto& value:cases) {
        require(diagnostic_flags::parseAsyncCopyFrames(value.text)==value.expected,"numeric parser accepts only one or two and defaults to two");
        scratch.write(value.text);SetLastError(0x17261018);
        require(diagnostic_flags::readAsyncCopyFramesFile(scratch.path)==value.expected&&GetLastError()==0x17261018,
                "bounded file-only configuration preserves the caller error and defaults conservatively");
    }
    scratch.write('1'+std::string(16,' '));
    require(diagnostic_flags::readAsyncCopyFramesFile(scratch.path)==2,"one excess byte rejects oversized numeric configuration instead of accepting its prefix");
    scratch.write("1");
    const auto locked=CreateFileW(scratch.path,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(locked!=INVALID_HANDLE_VALUE,"hold owned configuration unreadable through exclusive sharing");
    const auto unreadable=diagnostic_flags::readAsyncCopyFramesFile(scratch.path);
    CloseHandle(locked);require(unreadable==2,"unreadable configuration defaults to two");
    require(DeleteFileW(scratch.path),"remove owned temporary configuration");
    require(diagnostic_flags::readAsyncCopyFramesFile(scratch.path)==2,"missing numeric configuration defaults to two");
    {
        Owner owner(false,nullptr,{64,32},true,false,0,false,false,1);owner.frame();
        const auto status=runtime->frameStatus();
        require(!status.asyncCopyPath&&!status.inflightLimit&&!status.inflightFrames,
                "numeric limit alone never enables the async route");
    }
    {
        Owner owner(false,nullptr,{64,32},false,false,0,false,true,1);
        require(!runtime->frameStatus().asyncCopyPath&&!runtime->frameStatus().inflightLimit,
                "async configuration remains inapplicable to a real WSI route");
    }
}
void asyncFrameContexts() {
    struct Case {
        Owner owner;
        std::uint32_t engine{2};
        std::uint64_t token{100};
        explicit Case(bool gpu=false,std::uint32_t limit=2):owner(true,nullptr,{64,32},true,gpu,0,false,true,limit) {
            owner.frame();
            const auto warm=completeTiming();runtime->associatePresentTiming(1,warm,0);owner.frame(false);
            require(asyncSerial==1,"first timed OFF uses an owned frame slot");completeAsync(asyncSerial);enableMenu();
        }
        presentation::FrameStatus step(bool inputs=true,bool on=true,VkResult expected=VK_SUCCESS) {
            menuIntent.requestedEnabled=on;
            const auto ticket=completeTiming();
            if(inputs)owner.capture(ticket,engine,constants(token++));
            runtime->associatePresentTiming(engine++,ticket,0);owner.frame(false,expected);
            return runtime->frameStatus();
        }
    };
    {
        Case c(false,1);const auto waited=fenceWaits;const auto first=c.step();
        require(first.asyncCopyPath&&first.inflightLimit==1&&first.inflightFrames==1&&first.inflightHighWater==1&&
                !first.inflightCapWaitCpu.count&&!asyncWaits&&fenceWaits==waited,
                "K1 admits its first pending frame without an early host wait");
        completeAsync(2,false);presentation::testing::asyncFrameLimit=2;
        const auto second=c.step();
        require(second.inflightLimit==1&&second.inflightFrames==1&&second.inflightHighWater==1&&
                asyncWaits==1&&second.inflightCapWaitCpu.count==1&&second.inflightCapWaitCpu.totalNs&&fenceWaits==waited,
                "K1 keeps its frozen limit and waits for the matching SDK tail despite completed private and consumed work");
        require(c.owner.provider->stats().consumed==1&&asyncKeys.size()==3&&
                asyncKeys[1].index==0&&asyncKeys[2].index==0&&asyncKeys[2].generation>asyncKeys[1].generation,
                "K1 reuses only slot zero with a new generation after its exact lease retires");
        completeAsync(3);const auto third=c.step();
        require(third.inflightFrames==1&&third.inflightCapWaitCpu.count==1&&asyncWaits==1,
                "K1 completed matching tail admits the next frame without another capacity wait");
    }
    {
        Case c(false,1);c.step();completedFences[asyncNativeFences[2]]=true;failAsyncWait=true;
        c.step(true,true,VK_ERROR_DEVICE_LOST);
        require(runtime->failed()&&runtime->shutdownState().failedQuarantined&&c.owner.provider->stats().consumed==0,
                "K1 finite tail-wait failure retains the pending slot and Provider lease");
        failAsyncWait=false;
    }
    {
        Case c;const auto waited=fenceWaits;const auto first=c.step();const auto second=c.step();
        require(first.asyncCopyPath&&second.inflightLimit==2&&second.inflightFrames==2&&second.inflightHighWater==2&&
                !second.inflightCapWaitCpu.count&&!asyncWaits&&fenceWaits==waited&&!bridgeWaits,
                "two pending complete frames use distinct native slots without a host GPU wait");
        require(c.owner.provider->stats().consumed==0,"pending bridge reads retain both exact Provider leases");
        completeAsync(2,false);const auto third=c.step();
        require(asyncWaits==1&&third.inflightCapWaitCpu.count==1&&third.inflightCapWaitCpu.totalNs&&fenceWaits==waited,
                "third frame waits only for the old post-Present tail even when its native and consumed points completed");
        require(c.owner.provider->stats().consumed==1&&asyncKeys.size()==4&&asyncKeys[1].index==asyncKeys[3].index&&
                asyncKeys[1].generation<asyncKeys[3].generation,"slot wrap keeps a new generation and releases only its consumed lease");
    }
    {
        Case c;c.step();c.step();completedFences[asyncNativeFences[2]]=true;failAsyncWait=true;
        c.step(true,true,VK_ERROR_DEVICE_LOST);
        require(runtime->failed()&&runtime->shutdownState().failedQuarantined&&c.owner.provider->stats().consumed==0,
                "a private fence alone cannot free Provider inputs when the finite SDK-tail wait fails");
        failAsyncWait=false;
    }
    {
        Case c;c.step();const auto status=c.step(false,true);
        require(!status.sdkEnabled&&status.queuedOff&&status.inflightFrames==2&&!status.inflightCapWaitCpu.count&&
                !std::strcmp(status.onBlockedReason,"frame_inputs_missing"),
                "missing inputs use queued ordinary color without draining a healthy previous frame");
    }
    {
        Case c;c.step();privateSubmitResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;
        const auto failed=c.step(true,true,VK_ERROR_OUT_OF_DEVICE_MEMORY);
        require(!runtime->failed()&&failed.inflightFrames==1&&!asyncWaits&&asyncSerial==2,
                "known unsubmitted private OOM cancels only the new slot reservation and returns its input lease");
        privateSubmitResult=VK_SUCCESS;const auto retried=c.step();
        require(retried.inflightFrames==2&&!asyncWaits&&asyncSerial==3,
                "retry reuses the canceled free slot without waiting for the other pending frame");
    }
    {
        Case c;throwQueuedPublication=true;c.step(true,true,VK_ERROR_DEVICE_LOST);throwQueuedPublication=false;
        require(runtime->failed()&&runtime->shutdownState().failedQuarantined&&runtime->frameStatus().inflightFrames==1&&
                c.owner.provider->stats().consumed==0,"partial publication retains the native slot and its lease after failure");
    }
    {
        Case c(true);const auto first=c.step();c.step();completeAsync(2,false);const auto next=c.step();
        require(timestampCreates==2&&next.previousCopy.privateCopyQueryMatch&&
                next.previousCopy.privateCopyGpu.sourceEngineFrameKnown&&next.previousCopy.privateCopyGpu.sourceEngineFrame==first.engineFrame&&
                next.previousCopy.privateCopyGpu.sourceCopyToken==next.previousCopy.sequence,
                "per-slot query retirement preserves its original frame and copy token through wrap");
    }
    {
        Case c;c.step();c.step();c.owner.info.oldSwapchain=c.owner.chain;c.owner.info.imageExtent={128,64};
        surfaceExtent={128,64};SetWindowPos(c.owner.window,nullptr,0,0,128,64,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        require(runtime->create(&c.owner.info,nullptr,&c.owner.chain)==VK_SUCCESS,"stage an in-flight fake resize");
        const auto status=c.step();
        require(status.outputWidth==128&&status.outputHeight==64&&asyncWaits&&!status.inflightCapWaitCpu.count&&
                c.owner.provider->stats().consumed>=2,"resize drains complete frame tails and retained leases outside capacity waits");
    }
}
void sharedFrameContexts() {
    {
        Owner owner(true,nullptr,{64,32},true,false,0,false,false,2,true);owner.frame();
        require(!runtime->frameStatus().sharedCopyPath&&!runtime->frameStatus().sharedPlanes.available,
                "SharedCopyPath alone never enables async or allocates a shared ring");
    }
    struct Case {
        Owner owner;
        std::uint32_t engine{1};std::uint64_t token{300};
        std::uint64_t initialConsumed{},initialReleased{},initialSharedCaptures{};
        explicit Case(const wchar_t* lowRes=nullptr,VkExtent2D extent={64,32}):
            owner(true,lowRes,extent,true,false,0,false,true,2,true) {
            owner.frame();enableMenu();
        }
        presentation::FrameStatus step(bool inputs=true,bool on=true,bool alternateCanvas=false,VkResult expected=VK_SUCCESS) {
            menuIntent.requestedEnabled=on;const auto ticket=completeTiming();auto sample=constants(token++);
            if(owner.info.imageExtent.width==96) {
                sample.outputRegion={{0,6,96,48},true,true};
                if(alternateCanvas)sample.upscaleSize={32,16};
            }
            if(inputs)owner.capture(ticket,engine,sample);
            runtime->associatePresentTiming(engine++,ticket,0);owner.frame(false,expected);return runtime->frameStatus();
        }
        void prime() {
            step(false,false);completeAsync(asyncSerial);
            step();completeAsync(asyncSerial);step();completeAsync(asyncSerial);
            initialConsumed=owner.provider->stats().captured;
            initialSharedCaptures=owner.provider->stats().sharedCaptures;
            const auto stats=runtime->frameStatus().sharedPlanes;initialReleased=stats.released+stats.leases;
            sharedColorOutputs=sharedBundleOutputs=sharedSourceOutputs=0;
        }
    };
    {
        Case c;c.prime();const auto first=c.step();const auto firstSerial=asyncSerial;
        const auto second=c.step();
        require(first.sharedCopyPath&&first.sharedColorDirect&&first.sharedAuxiliaryDirect&&first.sharedPlaneKey.valid()&&
                second.sharedPlanes.leases==2&&sharedBundleOutputs==2&&!bridgeWaits&&!asyncWaits,
                "actual Native shared ON directly targets two exact reservations without a previous-consumer host wait");
        require(c.owner.provider->stats().sharedCaptures==c.initialSharedCaptures+2&&c.owner.provider->stats().consumed==c.initialConsumed,
                "shared Provider inputs stay owned while SDK tail and consumed are pending");
        completeAsync(firstSerial,false);
        require(c.owner.provider->stats().consumed==c.initialConsumed,"private completion plus consumed alone does not release UNTIL_NEXT_PRESENT input");
        const auto next=c.step();
        require(asyncWaits==1&&next.inflightCapWaitCpu.count==1&&next.sharedPlanes.released==c.initialReleased+1&&
                next.sharedPlanes.leases==2&&c.owner.provider->stats().consumed==c.initialConsumed+1,
                "capacity wraps only after the exact post-Present tail and releases that shared lease once");
    }
    {
        Case c;c.prime();const auto captured=c.owner.provider->stats().captured;
        const auto status=c.step(false,false);
        require(status.sharedColorDirect&&!status.sharedAuxiliaryDirect&&sharedColorOutputs==1&&
                c.owner.provider->stats().captured==captured&&status.sharedPlaneKey.valid(),
                "explicit OFF uses a color-only shared reservation without inventing an auxiliary producer");
    }
    {
        Case c;c.prime();revokeLocalAtCopySubmit=true;menuIntent.localFiOffRequested=menuIntent.localFiOffActive=true;
        const auto ticket=completeTiming();auto sample=constants(999);sample.outputRegion={{0,0,64,32},true,true,true};
        c.owner.capture(ticket,c.engine,sample);runtime->associatePresentTiming(c.engine,ticket,0);c.owner.frame(false);
        const auto status=runtime->frameStatus();
        require(status.sharedColorDirect&&!status.sharedAuxiliaryDirect&&!status.sdkEnabled&&sharedColorOutputs==1&&
                c.owner.provider->stats().sharedCaptures==c.initialSharedCaptures+1&&status.sharedPlanes.leases==1,
                "a revoked ON certificate can present shared color OFF while retaining the auxiliary reservation to the OFF tail");
        completeAsync(asyncSerial);const auto retired=c.step(false,false);
        require(c.owner.provider->stats().consumed==c.initialConsumed+1&&retired.sharedPlanes.released==c.initialReleased+1,
                "shared ON-to-OFF whole reservation retires on its real OFF receipt");
    }
    {
        Case c(L"1",{96,60});c.prime();const auto status=c.step(true,true,true);
        require(status.sharedCopyPath&&!status.sharedColorDirect&&!status.sharedAuxiliaryDirect&&status.sharedPlaneKey.valid()&&
                status.sharedFallbackReason&&!std::strcmp(status.sharedFallbackReason,"shared_color_extent_mismatch")&&
                status.sharedPlanes.legacySourceFrames==1&&status.sharedPlanes.leases==1&&sharedSourceOutputs==1,
                "same-frame canvas change falls back to the private color and pins the actual shared auxiliary source receipt");
        completeAsync(asyncSerial,false);const auto held=c.step(false,false);
        require(held.sharedPlanes.leases==1&&held.sharedPlanes.released==c.initialReleased,"legacy source cannot recycle shared auxiliaries before its SDK tail");
        completeAsync(asyncSerial);const auto retired=c.step(false,false);
        require(retired.sharedPlanes.released==c.initialReleased+1&&retired.sharedColorDirect&&
                retired.sharedPlaneKey.ringGeneration!=status.sharedPlaneKey.ringGeneration,
                "matching legacy tail retires the exact source lease before rebuilding and borrowing the new canvas ring");
    }
    {
        Case c;c.prime();throwSharedPublication=true;c.step(true,true,false,VK_ERROR_DEVICE_LOST);throwSharedPublication=false;
        require(runtime->failed()&&runtime->shutdownState().failedQuarantined&&runtime->frameStatus().sharedPlanes.poisoned&&
                runtime->frameStatus().sharedPlanes.leases==1&&c.owner.provider->stats().consumed==c.initialConsumed,
                "partial shared publication retains Native and shared Provider owners rather than falling back after a read may have queued");
    }
    {
        Case c;c.prime();privateSubmitResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;
        c.step(false,false,false,VK_ERROR_OUT_OF_DEVICE_MEMORY);privateSubmitResult=VK_SUCCESS;
        require(!runtime->failed()&&runtime->frameStatus().sharedPlanes.cancelled==1&&runtime->frameStatus().sharedPlanes.leases==0,
                "known unsubmitted color-only OOM cancels its shared reservation without changing old pending work");
    }
    for(const bool providerFirst:{true,false}) {
        Case c;c.prime();c.owner.provider->setSharedInputs({});
        const auto last=c.step();
        require(last.inputMatched&&!last.sharedPlaneKey.valid()&&!last.sharedColorDirect,
                "shutdown-order regression ends with an actual queued private Provider fallback");
        const auto before=c.owner.provider->stats();completedFences[h<VkFence>(0xF001)]=true;
        if(providerFirst)require(c.owner.provider->stop(h<VkFence>(0xF001))==VK_SUCCESS,"modeled queue tail precedes old fixture Provider stop");
        require(runtime->stop()==VK_SUCCESS,"actual Native completes the pending private fallback and SDK tail");
        if(!providerFirst)require(c.owner.provider->stop(h<VkFence>(0xF001))==VK_SUCCESS,"fixture Provider stop follows actual Native drain");
        const auto after=c.owner.provider->stats();
        require(after.captured==before.captured&&after.consumed+(providerFirst?1u:0u)==after.captured,
                "Provider-first clears the last private ticket before consumption bookkeeping; Native-first records its real bridge completion");
        std::printf("{\"event\":\"shared_fixture_shutdown_order_cpu\",\"providerFirst\":%s,\"beforeCaptured\":%llu,\"beforeConsumed\":%llu,\"afterCaptured\":%llu,\"afterConsumed\":%llu}\n",
            providerFirst?"true":"false",static_cast<unsigned long long>(before.captured),static_cast<unsigned long long>(before.consumed),
            static_cast<unsigned long long>(after.captured),static_cast<unsigned long long>(after.consumed));
    }
    std::puts("PASS actual Native shared path: direct ON/OFF, exact full-tail capacity, revoked ON-to-OFF, canvas legacy source pin, partial retention and unsubmitted OOM cancellation; CPU mock only");
}
void synchronousSharedFrames() {
    struct Case {
        Owner owner;
        std::uint32_t engine{1};std::uint64_t token{500};
        explicit Case(const wchar_t* lowRes=nullptr,VkExtent2D extent={64,32}):
            owner(true,lowRes,extent,true,false,0,false,false,2,true,false,true) {
            owner.frame();enableMenu();
        }
        presentation::FrameStatus step(bool inputs=true,bool on=true,bool alternateCanvas=false,VkResult expected=VK_SUCCESS) {
            menuIntent.requestedEnabled=on;const auto ticket=completeTiming();auto sample=constants(token++);
            if(owner.info.imageExtent.width==96) {
                sample.outputRegion={{0,6,96,48},true,true};
                if(alternateCanvas)sample.upscaleSize={32,16};
            }
            if(inputs)owner.capture(ticket,engine,sample);
            runtime->associatePresentTiming(engine++,ticket,0);owner.frame(false,expected);return runtime->frameStatus();
        }
        void prime() {step(false,false);step();step();}
    };
    {
        Case c;c.prime();const auto before=runtime->frameStatus();
        const auto consumed=c.owner.provider->stats().consumed;
        const auto waits=fenceWaits,consumers=bridgeWaits,tails=asyncWaits;
        completeAsync(asyncSerial,false);syncWaitOrder.clear();
        const auto status=c.step();
        require(!status.asyncCopyPath&&status.sharedCopyPath&&!status.inflightLimit&&!status.inflightFrames&&
                !status.inflightHighWater&&!status.inflightCapWaitCpu.count&&status.sharedColorDirect&&status.sharedAuxiliaryDirect,
                "synchronous shared writes actual color and auxiliary planes without any async admission, ledger ownership or capacity wait");
        require(fenceWaits==waits+1&&bridgeWaits==consumers+1&&asyncWaits==tails+1&&
                syncWaitOrder==std::vector<char>{'v','c','t'},
                "synchronous shared preserves the original prior-copy fence then consumer wait and additionally verifies the exact SDK tail");
        require(c.owner.provider->stats().consumed==consumed+1&&status.sharedPlanes.released==before.sharedPlanes.released+1&&
                status.sharedPlanes.leases==1&&asyncKeys.back().index==0&&asyncKeys[asyncKeys.size()-2].index==0&&
                asyncKeys.back().generation>asyncKeys[asyncKeys.size()-2].generation,
                "only a completed real receipt releases the previous reservation while the synchronous command identity stays in slot zero");
        const auto off=c.step(false,false);
        require(off.sharedColorDirect&&!off.sharedAuxiliaryDirect&&off.sharedPlanes.leases==1&&!off.inflightCapWaitCpu.count,
                "synchronous OFF directly shares color without inventing auxiliary inputs or async work");
        const auto captured=c.owner.provider->stats().captured;
        require(runtime->stop()==VK_SUCCESS&&c.owner.provider->stats().consumed==captured,
                "normal stop waits and releases the last synchronous shared frame and inputs");
    }
    {
        Case c;c.prime();const auto consumed=c.owner.provider->stats().consumed;
        revokeLocalAtCopySubmit=true;menuIntent.localFiOffRequested=menuIntent.localFiOffActive=true;
        const auto ticket=completeTiming();auto sample=constants(999);sample.outputRegion={{0,0,64,32},true,true,true};
        c.owner.capture(ticket,c.engine,sample);runtime->associatePresentTiming(c.engine,ticket,0);c.owner.frame(false);
        const auto revoked=runtime->frameStatus();
        require(revoked.sharedColorDirect&&!revoked.sharedAuxiliaryDirect&&!revoked.sdkEnabled&&revoked.sharedPlanes.leases==1,
                "revoked synchronous ON publishes only shared OFF color and retains its whole auxiliary reservation");
        revokeLocalAtCopySubmit=false;c.step(false,false);
        require(c.owner.provider->stats().consumed==consumed+2,
                "synchronous revoked ON-to-OFF inputs release only after the actual OFF receipt completes");
    }
    {
        Case c(L"1",{96,60});c.prime();const auto consumed=c.owner.provider->stats().consumed;
        const auto fallback=c.step(true,true,true);
        require(!fallback.sharedColorDirect&&!fallback.sharedAuxiliaryDirect&&fallback.sharedPlaneKey.valid()&&
                fallback.sharedPlanes.legacySourceFrames==1&&sharedSourceOutputs==1&&!fallback.inflightFrames,
                "same-frame synchronous canvas transition keeps private color and pins the exact shared auxiliary source");
        c.step(false,false);
        require(c.owner.provider->stats().consumed==consumed+2,
                "synchronous private-color fallback also retires the shared input source using its real receipt");
        const auto beforeWaits=asyncWaits;
        SetWindowPos(c.owner.window,nullptr,0,0,128,80,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        surfaceExtent={128,80};c.owner.info.imageExtent=surfaceExtent;c.owner.info.oldSwapchain=c.owner.chain;
        require(runtime->create(&c.owner.info,nullptr,&c.owner.chain)==VK_SUCCESS,"synchronous shared physical resize stages normally");
        c.step(false,false);
        require(runtime->frameStatus().outputWidth==128&&asyncWaits>beforeWaits&&!runtime->frameStatus().inflightCapWaitCpu.count,
                "physical resize drains the previous synchronous shared consumer before rebuilding its resources");
    }
    for(const bool failTail:{false,true}) {
        Case c;c.prime();const auto consumed=c.owner.provider->stats().consumed;
        const auto copies=colorCopies;failBridgeWait=!failTail;failAsyncWait=failTail;
        c.step(false,false,false,VK_ERROR_DEVICE_LOST);
        require(runtime->failed()&&runtime->shutdownState().failedQuarantined&&colorCopies==copies&&
                c.owner.provider->stats().consumed==consumed&&runtime->frameStatus().sharedPlanes.leases==1,
                "failed synchronous consumer or SDK-tail wait blocks reuse and retains the concrete shared owners");
        failBridgeWait=failAsyncWait=false;
    }
    {
        Case c;c.prime();throwSharedPublication=true;c.step(true,true,false,VK_ERROR_DEVICE_LOST);throwSharedPublication=false;
        require(runtime->failed()&&runtime->shutdownState().failedQuarantined&&runtime->frameStatus().sharedPlanes.poisoned&&
                runtime->frameStatus().sharedPlanes.leases==1,
                "partial synchronous shared publication poisons and retains its lease instead of replaying work");
    }
    {
        Case c;c.prime();privateSubmitResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;
        c.step(false,false,false,VK_ERROR_OUT_OF_DEVICE_MEMORY);privateSubmitResult=VK_SUCCESS;
        require(!runtime->failed()&&runtime->frameStatus().sharedPlanes.cancelled==1&&runtime->frameStatus().sharedPlanes.leases==0,
                "known unsubmitted synchronous shared color OOM cancels only its own reservation");
    }
    std::puts("PASS actual Native synchronous shared: original copy/consumer wait boundary, exact SDK tail, direct ON/OFF, fallback, resize, stop and failure retention; CPU mock only");
}
void longFrameFollowupReset() {
    struct Case {
        Owner owner;
        std::uint32_t engine{2};
        std::uint64_t token{100};
        VkExtent2D input{32,16};
        frame_input::OutputRegionSample region;
        bool inputReset{};
        explicit Case(bool diagnostic=true,bool fake=true,bool gpu=false):owner(true,nullptr,{64,32},fake,gpu) {
            presentation::testing::followupEnabled=diagnostic;
            presentation::testing::controlledClock=true;
            presentation::testing::clockTime=std::chrono::steady_clock::time_point(std::chrono::seconds(10));
            owner.frame();
            presentation::testing::clockTime+=std::chrono::milliseconds(16);
            const auto warm=completeTiming();runtime->associatePresentTiming(1,warm,0);owner.frame(false);
            enableMenu();step();step();
            require(lastBundleEnabled&&!lastHistoryReset,"followup fixture begins with stable admitted ON history");
        }
        presentation::FrameStatus step(unsigned ms=16,bool inputs=true,bool on=true,VkResult expected=VK_SUCCESS) {
            presentation::testing::clockTime+=std::chrono::milliseconds(ms);
            menuIntent.requestedEnabled=on;
            const auto ticket=completeTiming();
            auto sample=constants(token++);sample.renderSize={static_cast<int>(input.width),static_cast<int>(input.height)};
            sample.outputRegion=region;sample.reset=inputReset;
            if(inputs)owner.capture(ticket,engine,sample,true,input);
            runtime->associatePresentTiming(engine++,ticket,0);owner.frame(false,expected);
            return runtime->frameStatus();
        }
        presentation::FrameStatus arm() {
            const auto status=step(100);
            const auto& d=status.longFrameReset;
            require(d.longFrame&&d.eligible&&d.armed&&d.pendingAfter&&!d.extraRequested&&lastHistoryReset,
                    "successful admitted ON long frame arms exactly one successor without toggling SDK OFF");
            require(d.intervalKnown&&d.outputIntervalNs==100'000'000&&d.thresholdNs>=40'000'000&&
                    d.outputEntryKnown&&d.outputEntryQpcTicks&&d.qpcFrequency&&
                    d.triggerAfter==presentation::LongFrameResetTrace::Trigger{d.engineFrame,d.sdkPresentId,d.copyToken},
                    "trigger records actual output cadence, original threshold and this frame's immutable identity");
            return status;
        }
    };
    {
        Case c(false);c.step(100);require(lastHistoryReset,"diagnostic OFF preserves the existing long-frame reset");
        const auto status=c.step();require(!lastHistoryReset&&!status.longFrameReset.requested,
                "diagnostic OFF adds no successor reset");
    }
    {
        Case c(false,true,true);const auto d=c.step(100).longFrameReset;
        require(d.requested&&!d.policyEnabled&&d.longFrame&&d.emit&&!d.armed&&!d.pendingAfter&&d.resetRequested,
                "GPU diagnostics observe the unchanged N0 long-frame reset without enabling successor policy");
        require(!c.step().longFrameReset.extraRequested&&!lastHistoryReset,"N0 diagnostic observation never adds a reset");
    }
    {
        Case c;c.arm();const auto next=c.step();const auto& d=next.longFrameReset;
        require(lastQueuedHistoryReset&&d.extraRequested&&d.extraSucceeded&&d.resetRequested&&!d.pendingAfter&&!d.armed&&
                d.copyToken>d.triggerBefore.copyToken&&next.sdkEnabled&&next.queuedOn,
                "next continuous ON consumes one extra reset despite its naturally changing copy token");
        require(d.resetReasons==presentation::HistoryResetFollowup,"successor reason is distinct from the triggering long frame");
        require(!c.step().longFrameReset.extraRequested&&!lastHistoryReset,"extra reset cannot rearm itself");
        c.step();c.step();require(c.step().longFrameReset.emit,"four successor outputs retain bounded diagnostic evidence");
        require(!c.step().longFrameReset.emit,"diagnostic tail stops after four outputs without another trigger");
    }
    {
        Case c;c.arm();const auto repeat=c.step(100).longFrameReset;
        require(repeat.extraSucceeded&&repeat.armed&&repeat.pendingAfter,"an independent new long interval re-arms one, never accumulates a count");
        require(c.step().longFrameReset.extraSucceeded&&!c.step().longFrameReset.extraRequested,
                "repeated long frames stop extending reset immediately after their one successor");
    }
    for(bool off:{false,true}) {
        Case c;c.arm();const auto gap=c.step(16,false,!off).longFrameReset;
        require(!gap.extraRequested&&!gap.pendingAfter&&gap.clearReason,"OFF and missing inputs both clear the pending successor");
        require(!c.step().longFrameReset.extraRequested,"recovery never borrows a pre-gap trigger");
    }
    for(unsigned cut=0;cut<5;++cut) {
        Case c;c.arm();
        if(cut==0)++menuIntent.controlEpoch;
        if(cut==1)c.input={16,8};
        if(cut==2)c.region={{4,2,56,28},true,true};
        if(cut==3)++c.token;
        if(cut==4)c.inputReset=true;
        const auto d=c.step(100).longFrameReset;
        require(d.longFrame&&d.clearReason&&!d.extraRequested&&!d.armed&&!d.pendingAfter,
                "a coincident long interval cannot rearm across epoch, viewport, region, producer gap or scene reset");
        c.inputReset=false;
        require(!c.step().longFrameReset.extraRequested,"the next continuous frame cannot inherit a discontinuity's long interval");
    }
    {
        Case c;c.step(16,false,false);const auto d=c.step(100).longFrameReset;
        require(d.longFrame&&!d.armed&&!d.pendingAfter&&!d.extraRequested&&lastHistoryReset,
                "a long OFF-to-ON recovery keeps its original history reset without arming a successor");
        require(!c.step().longFrameReset.extraRequested,"the frame after long ON recovery has no extra reset");
    }
    for(bool modeChange:{false,true}) {
        Case c;c.arm();c.owner.info.oldSwapchain=c.owner.chain;
        if(modeChange)c.owner.info.presentMode=VK_PRESENT_MODE_IMMEDIATE_KHR;
        require(runtime->create(&c.owner.info,nullptr,&c.owner.chain)==VK_SUCCESS,"new fake generation keeps the existing owner");
        const auto d=c.step().longFrameReset;
        require(d.clearReason&&!d.extraRequested&&!d.pendingAfter,"copy-generation and presentation-mode transitions clear pending reset");
    }
    {
        Case c;c.arm();occluded=true;const auto hidden=c.step().longFrameReset;occluded=false;
        require(hidden.extraRequested&&lastQueuedHistoryReset&&!hidden.extraSucceeded&&!hidden.pendingAfter&&hidden.clearReason,
                "occlusion distinguishes a requested reset from successful visible output and clears its latch");
        require(!c.step().longFrameReset.extraRequested,"restored output cannot inherit the occluded trigger");
    }
    for(std::int32_t result:{-1,1}) {
        Case c;c.arm();frameGenResult=result;const auto d=c.step().longFrameReset;frameGenResult=0;
        require(d.extraRequested&&d.extraSucceeded==(result>=0)&&!d.pendingAfter,
                "negative SDK generation results refuse successful consumption while nonnegative reset warnings remain valid");
        require(!c.step().longFrameReset.extraRequested,"SDK status recovery does not reuse an old reset trigger");
    }
    {
        Case c;c.arm();throwQueuedPublication=true;
        const auto failed=c.step(16,true,true,VK_ERROR_DEVICE_LOST).longFrameReset;throwQueuedPublication=false;
        require(failed.extraRequested&&lastQueuedHistoryReset&&!failed.extraSucceeded&&!failed.pendingAfter&&failed.clearReason,
                "failed publication exposes the actual reset argument, clears pending state and never counts successful application");
    }
    {
        Case c;c.arm();std::uint32_t index{};const auto sem=h<VkSemaphore>(0x9abc);
        require(runtime->acquire(c.owner.chain,UINT64_MAX,sem,VK_NULL_HANDLE,&index)==VK_SUCCESS,"reserve own fake image before minimizing");
        require(SetWindowPos(c.owner.window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=0,"minimize only fixture client extent");
        const auto ticket=completeTiming();runtime->associatePresentTiming(c.engine++,ticket,0);
        VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};p.waitSemaphoreCount=1;p.pWaitSemaphores=&sem;
        p.swapchainCount=1;p.pSwapchains=&c.owner.chain;p.pImageIndices=&index;
        require(runtime->present(h<VkQueue>(5),&p)==VK_ERROR_OUT_OF_DATE_KHR,"zero-sized fake window consumes waits without SDK output");
        const auto d=runtime->frameStatus().longFrameReset;
        require(d.clearReason&&!d.pendingAfter&&!d.outputEntryKnown&&!d.extraRequested,
                "window-only wait consumption clears pending state without inventing an output-entry timestamp");
    }
    {
        Case c(true,false);const auto d=c.step(100).longFrameReset;
        require(d.longFrame&&!d.eligible&&!d.armed&&!d.pendingAfter,"legacy dual-chain route is outside the isolated experiment");
    }
}
void gpuTimestampCopyAssociation() {
    {
        Owner owner(false,nullptr,{64,32},true,true);
        runtime->associatePresentTiming(71,{},0);owner.frame(false);
        auto status=runtime->frameStatus();const auto& copy=status.immediateCopy;const auto& gpu=copy.privateCopyGpu;
        require(copy.privateCopyQueryMatch&&gpu.sourceCopyTokenKnown&&gpu.sourceCopyToken==copy.sequence&&
                    gpu.generation==copy.generation&&gpu.available&&gpu.durationKnown&&gpu.queryStateKnown&&
                    gpu.diagnosticActive&&gpu.queryReadAttempted&&gpu.queryResultKnown&&gpu.queryResult==VK_SUCCESS&&
                    gpu.queryAvailabilityKnown&&gpu.beginAvailability==1&&gpu.endAvailability==1,
                "immediate fence snapshot owns the exact completed copy query and raw read status");
        double aligned{};
        require(!timestampQpcUs(gpu,gpu.beginTicks,aligned),"uncalibrated private query never invents a QPC correlation");
        runtime->associatePresentTiming(72,{},0);owner.frame(false);
        runtime->associatePresentTiming(73,{},0);owner.frame(false);status=runtime->frameStatus();
        require(status.previousCopy.privateCopyQueryMatch&&status.previousCopy.sequence==2&&
                    status.previousCopy.privateCopyGpu.sourceCopyToken==2&&status.previousCopy.privateCopyGpu.sourceEngineFrame==72&&
                    status.previousCopy.privateCopyGpu.generation==status.previousCopy.generation&&timestampReads==2,
                "queued fence snapshot carries the previous source query without another query read");
    }
    {
        Owner owner(false,nullptr,{64,32},true,true);owner.frame(false);owner.frame(false);
        timestampFailure=10;owner.frame(false);const auto status=runtime->frameStatus();
        require(timestampReadDelayNs>=25'000'000&&status.copyFenceWaitCpu.count==1&&
                    status.copyFenceWaitCpu.totalNs<timestampReadDelayNs/2&&status.previousCopy.privateCopyQueryMatch,
                "optional query read latency remains outside the CPU fence-wait counter");
    }
    for(unsigned failure:{5u,6u,7u,9u}) {
        Owner owner(false,nullptr,{64,32},true,true);owner.frame(false);owner.frame(false);
        require(runtime->frameStatus().vkPrivateCopyGpu.available,"query failure fixture starts from a successful older sample");
        timestampFailure=failure;owner.frame(false);
        auto status=runtime->frameStatus();const auto& copy=status.previousCopy;const auto& gpu=copy.privateCopyGpu;
        require(copy.privateCopyQueryMatch&&copy.sequence==2&&gpu.sourceCopyToken==2&&gpu.generation==copy.generation&&
                    gpu.sequence==2&&gpu.queryReadAttempted&&!gpu.available&&!gpu.durationKnown&&!gpu.diagnosticActive&&
                    status.vkPrivateCopyGpu.sequence==2&&!runtime->failed(),
                "query failure retains this copy identity and never lends the earlier successful duration");
        require(gpu.queryResultKnown==(failure!=6)&&gpu.queryAvailabilityKnown==(failure==5||failure==7),
                "query exceptions and negative results do not fabricate result or availability values");
        if(failure==5)require(gpu.queryResult==VK_NOT_READY&&!gpu.beginAvailability&&!gpu.endAvailability,"NOT_READY availability is reported without a duration");
        if(failure==7)require(gpu.queryResult==VK_SUCCESS&&!gpu.beginAvailability&&gpu.endAvailability==1,"success result alone cannot hide an unavailable endpoint");
        if(failure==9)require(gpu.queryResult==VK_ERROR_DEVICE_LOST,"diagnostic device error preserves its exact VkResult");
        owner.frame(false);status=runtime->frameStatus();
        require(!status.previousCopy.privateCopyQueryMatch&&!status.previousCopy.privateCopyGpu.available&&
                    !status.previousCopy.privateCopyGpu.sourceCopyTokenKnown&&!status.previousCopy.privateCopyGpu.queryReadAttempted&&
                    !status.previousCopy.privateCopyGpu.diagnosticActive&&timestampReads==2,
                "later copy after diagnostic shutdown cannot borrow any previous query");
    }
    {
        Owner owner(false,nullptr,{64,32},true,true);owner.frame(false);owner.frame(false);
        waitResult=VK_TIMEOUT;owner.frame(false,VK_ERROR_DEVICE_LOST);const auto status=runtime->frameStatus();
        require(status.previousCopy.waitResult==VK_TIMEOUT&&!status.previousCopy.privateCopyQueryMatch&&
                    status.previousCopy.privateCopyGpu.requested&&status.previousCopy.privateCopyGpu.diagnosticActive&&
                    !status.previousCopy.privateCopyGpu.available&&!status.previousCopy.privateCopyGpu.queryReadAttempted&&timestampReads==1,
                "fence timeout never reads a pending query or borrows the previous successful sample");
        waitResult=VK_SUCCESS;
    }
}
void fakeSlowTiming() {
    {
        Owner owner(false,nullptr,{64,32},true);
        wsi_acquire::Call call;std::uint32_t index{};delayFakeSubmit=true;
        require(runtime->acquire(owner.chain,0,h<VkSemaphore>(0xABCDE),VK_NULL_HANDLE,&index,&call)==VK_SUCCESS,
                "timed fake acquire preserves the actual signal submit result");
        require(queueCallbacks&&queueCallbackReleased&&lastQueueCall.slow()&&call.signal.returnNs>=call.signal.driverEntryNs&&
                    call.signal.driverEntryNs>=call.signal.entryNs&&call.signal.result==VK_SUCCESS,
                "actual Native signal submit has ordered QPC and callback runs after gate release");
        runtime->associatePresentTiming(71,{},0);owner.frame(false);
        auto status=runtime->frameStatus();
        require(status.immediateCopy.sequence==1&&status.immediateCopy.engineFrame==71&&status.immediateCopy.generation&&
                    status.immediateCopy.image&&status.immediateCopy.fence&&status.immediateCopy.queue==5&&
                    status.immediateCopy.submit.returnNs<=status.immediateCopy.waitEntryNs&&status.immediateCopy.waitResult==VK_SUCCESS,
                "fresh copy wait retains source token/frame/queue and submit-to-observed-fence QPC");
        runtime->associatePresentTiming(72,{},0);owner.frame(false);
        const auto waited=fenceWaits;
        runtime->associatePresentTiming(73,{},0);owner.frame(false);status=runtime->frameStatus();
        require(fenceWaits==waited+1&&status.previousCopy.sequence==2&&status.previousCopy.engineFrame==72&&
                    status.previousCopy.submit.returnNs<=status.previousCopy.waitEntryNs&&
                    status.previousCopy.waitReturnNs>=status.previousCopy.waitEntryNs,
                "queued drain identifies the previous copy without adding a fence wait");
    }
    {
        Owner owner(false,nullptr,{64,32},true,true);
        const auto sem=h<VkSemaphore>(0xBCDEF),sem2=h<VkSemaphore>(0xBCDF0);std::uint32_t first{},second{};
        require(runtime->acquire(owner.chain,0,sem,VK_NULL_HANDLE,&first)==VK_SUCCESS&&
                    runtime->acquire(owner.chain,0,sem2,VK_NULL_HANDLE,&second)==VK_SUCCESS,
                "wait-only timing fixture reserves two fake images");
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};info.waitSemaphoreCount=1;info.pWaitSemaphores=&sem;
        info.swapchainCount=1;info.pSwapchains=&owner.chain;info.pImageIndices=&first;
        require(SetWindowPos(owner.window,nullptr,0,0,65,32,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=0,
                "resize only fixture-owned HWND");
        require(runtime->present(h<VkQueue>(5),&info)==VK_ERROR_OUT_OF_DATE_KHR&&!fenceWaits,
                "window error still queues wait-only consumption without a new CPU wait");
        SetWindowPos(owner.window,nullptr,0,0,64,32,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        info.pImageIndices=&second;info.pWaitSemaphores=&sem2;
        require(runtime->present(h<VkQueue>(5),&info)==VK_SUCCESS,"restore fixture and drain wait-only copy");
        const auto status=runtime->frameStatus();
        require(fenceWaits==2&&status.previousCopy.sequence==1&&status.previousCopy.waitResult==VK_SUCCESS&&
                    status.previousCopy.waitReturnNs>=status.previousCopy.waitEntryNs&&status.immediateCopy.sequence==2,
                "wait-only drain timing survives the same present's fresh synchronous copy wait");
        require(timestampResets==1&&timestampWrites==2&&timestampReads==1&&status.vkPrivateCopyGpu.sequence==1,
                "wait-only submit never fabricates a GPU copy sample or query read");
    }
}
void fakePoolConcurrency() {
    reset();
    SetEnvironmentVariableW(L"XESS_SDK_BIN", L"D:\\fixture-sdk-not-loaded");
    const auto window =
        CreateWindowExW(0, L"STATIC", L"fake pool concurrency fixture", WS_POPUP, 0, 0, 64, 32,
                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(window != nullptr, "fake pool concurrency owns its HWND");
    {
        presentation::Surface surface(window);
        presentation::QueueGate gate;
        presentation::DeviceRecord::Impl record;
        record.fake = true;
        record.queueGate = &gate;
        record.device = h<VkDevice>(3);
        record.physical = h<VkPhysicalDevice>(2);
        record.gipa = instanceFunctions;
        record.gdpa = deviceFunctions;
        fakeGate = &gate;
        presentation::NativeBackend backend(record, surface, h<VkQueue>(5), nullptr);
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.minImageCount = 3;
        info.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
        info.imageExtent = {64, 32};
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        std::vector<VkImage> first, second;
        require(backend.createFakeImages(info, 1, first) == VK_SUCCESS,
                "native fake concurrency creates first generation");
        std::atomic<bool> start{};
        VkResult created = VK_ERROR_UNKNOWN, destroyed = VK_ERROR_UNKNOWN;
        std::thread creating([&] {
            while (!start.load())
                std::this_thread::yield();
            created = backend.createFakeImages(info, 2, second);
        });
        std::thread destroying([&] {
            while (!start.load())
                std::this_thread::yield();
            destroyed = backend.destroyFakeImages(1);
        });
        start = true;
        creating.join();
        destroying.join();
        require(created == VK_SUCCESS && destroyed == VK_SUCCESS && second.size() == 3,
                "native fake map supports new generation creation alongside old generation "
                "destruction");
        require(backend.destroyFakeImages(2) == VK_SUCCESS,
                "native fake concurrency releases remaining generation");
        fakeGate = nullptr;
    }
    DestroyWindow(window);
}
void waitOnlyFailureOwnership() {
    for(const bool asyncCopy:{false,true})for(const auto failed:{VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_OUT_OF_DEVICE_MEMORY,VK_ERROR_DEVICE_LOST,VK_ERROR_UNKNOWN}) {
        Owner owner(false,nullptr,{64,32},true,false,0,false,asyncCopy);
        const auto sem=h<VkSemaphore>(0xCB001);std::uint32_t image{};
        require(runtime->acquire(owner.chain,0,sem,VK_NULL_HANDLE,&image)==VK_SUCCESS,"wait-only failure owns an acquired fake image");
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};info.waitSemaphoreCount=1;info.pWaitSemaphores=&sem;
        info.swapchainCount=1;info.pSwapchains=&owner.chain;info.pImageIndices=&image;
        require(SetWindowPos(owner.window,nullptr,0,0,65,32,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=0,"wait-only failure changes only fixture HWND");
        submitResult=failed;
        const bool oom=failed==VK_ERROR_OUT_OF_HOST_MEMORY||failed==VK_ERROR_OUT_OF_DEVICE_MEMORY;
        require(runtime->present(h<VkQueue>(5),&info)==(oom?failed:VK_ERROR_DEVICE_LOST),"wait-only failure preserves OOM or terminates uncertain submission");
        require(runtime->retainsAppSemaphore(sem)==!oom&&runtime->failed()==!oom&&runtime->lastCopyProgress()==
            (oom?presentation::CopyProgress::NotSubmitted:presentation::CopyProgress::SubmissionUncertain),
            "only known unsubmitted wait-only OOM releases app waits; unknown submission keeps their references");
        submitResult=VK_SUCCESS;
        if(oom)require(runtime->present(h<VkQueue>(5),&info)==VK_ERROR_OUT_OF_DATE_KHR&&runtime->retainsAppSemaphore(sem),
            "known wait-only OOM leaves the image and original wait available for one valid retry");
    }
}
void pendingAcquireFailureProbe() {
    struct Case {
        HWND window{};presentation::QueueGate gate;
        std::unique_ptr<presentation::Surface> surface;
        presentation::DeviceRecord::Impl record;
        std::unique_ptr<presentation::NativeBackend> backend;
        std::unique_ptr<presentation::Runtime> route;
        VkSwapchainKHR chain{};std::array<VkImage,3> images{};
        explicit Case(bool async=true) {
            reset();presentation::testing::asyncEnabled=async;SetEnvironmentVariableW(L"XESS_SDK_BIN",L"D:\\fixture-sdk-not-loaded");
            window=CreateWindowExW(0,L"STATIC",L"pending acquire probe CPU",WS_POPUP,0,0,64,32,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            require(window!=nullptr,"probe owns its inert window");surface=std::make_unique<presentation::Surface>(window);surface->handle=h<VkSurfaceKHR>(4);
            record.fake=true;record.queueGate=&gate;record.device=h<VkDevice>(3);record.physical=h<VkPhysicalDevice>(2);
            record.gipa=instanceFunctions;record.gdpa=deviceFunctions;record.queueObserver=queueObserved;record.queueObserverContext=&gate;fakeGate=&gate;
            backend=std::make_unique<presentation::NativeBackend>(record,*surface,h<VkQueue>(5),nullptr);
            route=std::make_unique<presentation::Runtime>(record.device,surface->handle,h<VkQueue>(5),presentation::Dispatch{},*backend,true);runtime=route.get();
            VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};info.surface=surface->handle;info.minImageCount=3;
            info.imageFormat=VK_FORMAT_R8G8B8A8_UNORM;info.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;info.imageExtent={64,32};
            info.imageArrayLayers=1;info.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;info.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;info.presentMode=VK_PRESENT_MODE_FIFO_KHR;
            require(route->create(&info,nullptr,&chain)==VK_SUCCESS,"probe creates an actual Native fake pool");
            std::uint32_t count=3;require(route->images(chain,&count,images.data())==VK_SUCCESS&&count==3,"probe reads exact image identities");
            for(unsigned n=0;n<3;++n) {std::uint32_t image=99;require(route->acquire(chain,0,h<VkSemaphore>(0xDD000+n),VK_NULL_HANDLE,&image)==VK_SUCCESS&&image==n,"probe reserves every fake image");}
            const auto wait=h<VkSemaphore>(0xDD100);std::uint32_t image{};VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present.waitSemaphoreCount=1;present.pWaitSemaphores=&wait;present.swapchainCount=1;present.pSwapchains=&chain;present.pImageIndices=&image;
            require(route->presentUntimedOff(h<VkQueue>(5),&present)==VK_SUCCESS&&backend->copyCompletionToken()==1,"probe binds one successful private source token");
        }
        ~Case() {
            submitResult=VK_SUCCESS;throwFakeSubmit=false;
            if(route->failed()||backend->shutdownState().failedQuarantined)backend.release();
            else route->stop();
            route.reset();backend.reset();runtime=nullptr;fakeGate=nullptr;surface.reset();DestroyWindow(window);
        }
        VkResult acquire() {std::uint32_t image=99;return route->acquire(chain,0,h<VkSemaphore>(0xDD200),VK_NULL_HANDLE,&image);}
        const presentation::testing::AcquireProbe& last(const char* reason) {
            require(!presentation::testing::acquireProbes.empty(),"failed pending acquire emits one test-only snapshot");
            const auto& probe=presentation::testing::acquireProbes.back();
            require(!std::strcmp(probe.reason,reason)&&probe.entryNs&&probe.returnNs>=probe.entryNs&&probe.submitted==1,
                "probe names the exact rejection branch with an ordered snapshot of submitted tokens");
            require(!presentation::testing::acquireProbeCaptureFailed&&presentation::testing::acquireProbeGateFree,"probe callback runs after QueueGate is released");
            return probe;
        }
    };
    {
        Case c;const auto reject=[&](VkImage image,std::uint64_t generation,std::uint64_t token,const char* reason) {
            const auto count=presentation::testing::acquireProbes.size();
            require(c.backend->signalAcquireAfterCopy(image,generation,token,h<VkSemaphore>(0xDD201),VK_NULL_HANDLE)==VK_ERROR_UNKNOWN,
                "invalid pending identity preserves the Native rejection result");
            const auto& probe=c.last(reason);
            require(presentation::testing::acquireProbes.size()==count+1&&probe.image==image&&probe.generation==generation&&probe.token==token&&
                probe.resultKnown&&probe.result==VK_ERROR_UNKNOWN&&!probe.signalAttempted&&!probe.unsafe&&!probe.stopped,
                "rejected identity is recorded without queue submission or state mutation");
        };
        reject(VK_NULL_HANDLE,1,1,"null_image");reject(c.images[0],0,1,"zero_generation");reject(c.images[0],1,0,"zero_token");
        reject(c.images[0],99,1,"pool_missing");reject(h<VkImage>(0xDEAD),1,1,"image_missing");
        reject(c.images[0],1,2,"token_not_submitted");reject(c.images[1],1,1,"token_mismatch");
        const auto count=presentation::testing::acquireProbes.size();require(c.acquire()==VK_SUCCESS&&presentation::testing::acquireProbes.size()==count,
            "healthy pending acquisition remains silent and succeeds after rejected identities");
        reject(c.images[0],1,1,"already_acquired");const auto& duplicate=c.last("already_acquired");
        require(duplicate.lastCopy==1&&duplicate.lastAcquire==1&&duplicate.poolFound&&duplicate.imageFound,"duplicate rejection retains the exact successfully acquired source token");
        c.backend->stopAdmission();require(c.backend->signalAcquireAfterCopy(c.images[0],1,1,h<VkSemaphore>(0xDD201),VK_NULL_HANDLE)==VK_ERROR_UNKNOWN,"stopped backend rejects pending acquire");
        require(c.last("timing_stopped").stopped,"stopped rejection records timing admission state");
    }
    for(const auto failure:{VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_OUT_OF_DEVICE_MEMORY,VK_ERROR_DEVICE_LOST,VK_ERROR_UNKNOWN}) {
        Case c;submitResult=failure;
        require(c.acquire()==failure,"probe preserves the actual pending signal submit result");
        const bool oom=failure==VK_ERROR_OUT_OF_HOST_MEMORY||failure==VK_ERROR_OUT_OF_DEVICE_MEMORY;
        const auto& probe=c.last(oom?"signal_submit_oom":"signal_submit_failed");
        require(probe.signalAttempted&&probe.resultKnown&&probe.result==failure&&probe.lastCopy==1&&!probe.lastAcquire&&probe.poolFound&&probe.imageFound,
            "submit failure records an unadvanced once-only token and exact known result");
        require(c.route->failed()==!oom,"known OOM retains retry while unknown submission quarantines Runtime");
        if(oom) {submitResult=VK_SUCCESS;const auto count=presentation::testing::acquireProbes.size();require(c.acquire()==VK_SUCCESS&&presentation::testing::acquireProbes.size()==count,"known unsubmitted OOM retry remains silent and consumes the exact token once");}
        else require(c.route->stop()==VK_ERROR_DEVICE_LOST&&c.route->shutdownState().failedQuarantined,"unknown signal result keeps the complete fake pool quarantined");
    }
    {
        Case c;throwFakeSubmit=true;require(c.acquire()==VK_ERROR_DEVICE_LOST&&c.route->failed(),"thrown signal submission retains Runtime exception quarantine");
        const auto& probe=c.last("signal_submit_exception");require(probe.signalAttempted&&!probe.resultKnown&&!probe.lastAcquire&&probe.lastCopy==1,
            "submission exception never fabricates a VkResult or advances the once-only token");
    }
    {
        Case c;submitResult=VK_ERROR_UNKNOWN;VkPresentInfoKHR wait{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        require(c.backend->consumePresentWaits(wait,c.images[0],1)==VK_ERROR_UNKNOWN,"probe can observe an existing unsafe backend without resetting its state");
        require(c.backend->signalAcquireAfterCopy(c.images[0],1,1,h<VkSemaphore>(0xDD201),VK_NULL_HANDLE)==VK_ERROR_UNKNOWN&&c.last("unsafe_backend").unsafe,
            "unsafe rejection records the original backend quarantine state");
    }
    {
        Case c(false);require(c.backend->signalAcquireAfterCopy(c.images[0],1,1,h<VkSemaphore>(0xDD201),VK_NULL_HANDLE)==VK_ERROR_UNKNOWN&&
            !c.last("async_disabled").async,"sync-only backend keeps its original rejection and identifies the disabled async path");
    }
}
} // namespace fixture
namespace xess_vk::presentation::testing {
void acquireProbe(const AcquireProbe& value) noexcept {
    try {
        if(fixture::fakeGate) {
            bool free{};std::thread other([&]{free=fixture::fakeGate->try_lock();if(free)fixture::fakeGate->unlock();});other.join();
            acquireProbeGateFree&=free;
        }
        acquireProbes.push_back(value);
    } catch(...) {acquireProbeCaptureFailed=true;}
}
}
int main() {
    using namespace fixture;
    try {
        queuedOffOwnership();
        queuedOnOwnership();
        offAuxiliaryCaptureTransitions();
        localFiOffFrameProof();
        actualOnFormula();
        gameplayStageAdmission();
        sdkResultFailureFacts();
        mappedOutputRegion();
        resizeAndWindowRecovery();
        resizeFailureOwnership();
        lowResOutputPrototype();
        gpuTimestamps();
        gpuTimestampCopyAssociation();
        asyncFrameConfiguration();
        asyncFrameContexts();
        sharedFrameContexts();
        synchronousSharedFrames();
        longFrameFollowupReset();
        fakeSlowTiming();
        fakePoolConcurrency();
        waitOnlyFailureOwnership();
        pendingAcquireFailureProbe();
        {Owner o(false,nullptr,{64,32},true);o.frame();o.frame();
            require(runtime->fakeEnabled()&&!nativeCreates&&!nativeImages&&!nativePresents&&!nativeDestroys&&colorCopies==2&&fakeLayoutsValid&&fakeEmptySubmits==2,
                "actual NativeBackend fake path uses GENERAL and no driver WSI calls or native binary completion signal");
            require(runtime->stop()==VK_SUCCESS&&!runtime->shutdownState().nativeTailRetained&&runtime->retainedChains()==0,
                "actual NativeBackend fake pool and private resources really clean without native tail");}
        { Owner o;
          o.info.oldSwapchain=o.chain;
          require(runtime->create(&o.info,nullptr,&o.chain)==VK_SUCCESS && !constructions,
                  "same-size recreate before first present reuses prepared resources without requiring an owner");
          o.frame(); require(constructions==1 && requestedLl1 && outputs==1 && nativePresents==1 && !lastNativeWaits,
              "first completed-copy frame initializes LL1 exactly once and remains untimed OFF");
          auto first=runtime->beginGameFrame(); require(first && !first.latencyEnabled,"first timing ticket still warms up OFF");
          menuIntent.requestedEnabled=menuIntent.bothGatesReady=menuIntent.nativeSuppressed=menuIntent.inputObserved=true;
          menuIntent.paired=menuIntent.gatesConsistent=true;
          runtime->simulationEnd(first); runtime->renderSubmitBegin(first); runtime->renderSubmitEnd(first);
          runtime->associatePresentTiming(10,first,0); o.frame(false);
          const auto status=runtime->frameStatus();
          require(status.runtimeCopyCpu.count==2 && status.nativePresentCpu.count==2 &&
              status.outputCpu.count==2 && status.outputWidth==64 && status.outputHeight==32 &&
              status.presentMode==VK_PRESENT_MODE_FIFO_KHR &&
              status.runtimeCopyCpu.maxNs<=status.runtimeCopyCpu.totalNs,
              "actual Native/Runtime status carries cumulative real phase calls and actual output extent");
          require(fsr_menu_control::snapshot().requestedEnabled && !status.requestedEnabled && !status.sdkEnabled &&
              status.framesPresented==1 && status.latencyTransitionAdmitted && status.onBlockedReason &&
              !std::strcmp(status.onBlockedReason,"timing_warmup_off"),
              "menu ON remains OFF during real timing warmup without hiding actual LL1 capability");
          const auto next=runtime->beginGameFrame(); require(next.latencyEnabled && constructions==1,"only a successful complete timed present admits later ON-capable tickets");
          runtime->abandonFrameTiming(next);
          o.info.oldSwapchain=o.chain; o.info.presentMode=VK_PRESENT_MODE_IMMEDIATE_KHR;
          require(runtime->create(&o.info,nullptr,&o.chain)==VK_SUCCESS && constructions==1 && !vsyncChanges,
                  "same-size mode rebuild stages its existing owner without an SDK call during create");
          o.frame();require(vsyncChanges==1 && constructions==1,"present applies the mode change without resetting LL1 or the owner"); }
        { Owner o; waitResult=VK_TIMEOUT; o.frame(); require(!constructions && !outputs && lastNativeWaits==1,"pending copy does not initialize SDK and uses its actual emergency wait");
          const auto status=runtime->frameStatus();
          require(status.runtimeCopyCpu.count==1 && status.nativePresentCpu.count==1 && !status.outputCpu.count,
              "pending copy counts actual copy/native calls without inventing an unentered output duration"); }
        { Owner o; waitResult=VK_ERROR_DEVICE_LOST; o.frame(true,VK_ERROR_DEVICE_LOST); require(!constructions && !nativePresents,"device-lost copy fence never initializes SDK or claims native wait ownership"); }
        { Owner o; submitResult=VK_ERROR_OUT_OF_HOST_MEMORY; o.frame(); require(!constructions && !outputs && lastNativeWaits==1,"failed copy submit does not initialize SDK or consume application waits privately"); }
        { Owner o(false); o.frame(); require(constructions==1 && !requestedLl1 && outputs==1 && !lastNativeWaits,
                "provider-absent OFF still initializes after copy and leaves LL capability disabled"); }
        { Owner o; failInitialization=true; o.frame(); require(constructions==1 && !outputs && nativePresents==1 && !lastNativeWaits && runtime->lastPrivateCopyCompleted(),
              "initialization failure preserves completed-copy ownership and returns the image with zero native waits");
          require(!runtime->beginGameFrame(),"failed unpublished initialization cannot grant tickets"); o.frame();
          require(constructions==1 && nativePresents==2 && lastNativeWaits==1 && submits==1,"failed initialization is not retried and next original packet is forwarded once"); }
        std::printf("{\"passed\":true,\"checks\":%u,\"actualNativeBackend\":true,\"vulkanCallsMocked\":true,\"sdkLoaded\":false,\"gameStarted\":false}\n",checks);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL native initialization: %s\n", e.what());
        return 1;
    }
}
