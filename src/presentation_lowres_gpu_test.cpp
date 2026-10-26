// Self-owned WSI/Provider/NativeBackend/bridge/SDK experiment. Typed menu
// intent and reset policy are local; the opt-in gate touches only this fixture.
#include "continuous_provider.hpp"
#include "fsr_menu_control.hpp"
#include "native_source_fixture.hpp"
#include "presentation_runtime.hpp"
#include "depth_restore_probe_spirv.hpp"
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <map>
#include <memory>

namespace xess_vk::presentation::testing {
bool followupEnabled{};
bool asyncEnabled{},sharedEnabled{},syncSharedEnabled{};
std::uint32_t asyncFrameLimit{2};
std::uint32_t acquireSequence{};
std::uint64_t outputEntryQpc{};
std::chrono::steady_clock::time_point outputNow() noexcept {
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now); outputEntryQpc = now.QuadPart;
    return std::chrono::steady_clock::now();
}
bool longFrameFollowupReset() noexcept { return followupEnabled; }
bool asyncCopyPath() noexcept { return asyncEnabled; }
std::uint32_t asyncCopyFrames() noexcept { return asyncFrameLimit; }
bool sharedCopyPath() noexcept { return sharedEnabled; }
bool sharedCopySync() noexcept { return syncSharedEnabled; }
void acquireProbe(const AcquireProbe& value) noexcept {
    std::fprintf(stdout,"{\"event\":\"native_pending_acquire_probe\",\"sequence\":%u,\"reason\":\"%s\",\"image\":%llu,\"generation\":%llu,\"token\":%llu,\"submitted\":%llu,\"lastCopy\":%llu,\"lastAcquire\":%llu,\"entryNs\":%llu,\"returnNs\":%llu,\"resultKnown\":%s,\"result\":%d,\"async\":%s,\"unsafe\":%s,\"stopped\":%s,\"poolFound\":%s,\"imageFound\":%s,\"signalAttempted\":%s}\n",
        acquireSequence,value.reason,static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(value.image)),
        static_cast<unsigned long long>(value.generation),static_cast<unsigned long long>(value.token),static_cast<unsigned long long>(value.submitted),
        static_cast<unsigned long long>(value.lastCopy),static_cast<unsigned long long>(value.lastAcquire),static_cast<unsigned long long>(value.entryNs),
        static_cast<unsigned long long>(value.returnNs),value.resultKnown?"true":"false",value.result,value.async?"true":"false",value.unsafe?"true":"false",
        value.stopped?"true":"false",value.poolFound?"true":"false",value.imageFound?"true":"false",value.signalAttempted?"true":"false");
}
}

namespace fixture {
using xess_vk::testing::check;
namespace cp = xess_vk::continuous;
namespace dm = xess_vk::descriptor_image_metadata;
constexpr VkExtent2D sourceExtent{1280, 800}, lowExtent{640, 400};
constexpr VkRect2D sourceRegion{{0, 40}, {1280, 720}}, lowRegion{{0, 20}, {640, 360}};
constexpr std::uint64_t timeout = 10'000'000'000;
constexpr VkDeviceSize colorOracleBytes=sourceExtent.width*sourceExtent.height*4;
constexpr VkDeviceSize auxiliaryOracleBytes=320*180*4, oracleStride=colorOracleBytes+2*auxiliaryOracleBytes;
xess_vk::fsr_menu_control::ControlIntent menu;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> std::uint64_t bits(T value) { return reinterpret_cast<std::uint64_t>(value); }
bool accepted(VkResult value) { return value == VK_SUCCESS || value == VK_SUBOPTIMAL_KHR; }
bool same(VkExtent2D a, VkExtent2D b) { return a.width == b.width && a.height == b.height; }
bool same(VkRect2D a, VkRect2D b) {
    return a.offset.x == b.offset.x && a.offset.y == b.offset.y && same(a.extent, b.extent);
}
constexpr bool cadenceHasHeadroom(unsigned gateMs,std::uint64_t intervalMax,std::uint64_t thresholdMax) {
    return gateMs==35 ? intervalMax<35'000'000 : thresholdMax<static_cast<std::uint64_t>(gateMs)*800'000;
}
static_assert(cadenceHasHeadroom(35,18'000'000,40'000'000));
static_assert(!cadenceHasHeadroom(35,35'000'000,40'000'000));
static_assert(cadenceHasHeadroom(60,18'000'000,47'000'000));
static_assert(!cadenceHasHeadroom(60,18'000'000,48'000'000));
static_assert(cadenceHasHeadroom(100,18'000'000,79'000'000));
static_assert(!cadenceHasHeadroom(100,18'000'000,80'000'000));
struct Watchdog {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = event ? CreateThread(nullptr, 0, [](void* value) -> DWORD {
        if (WaitForSingleObject(static_cast<HANDLE>(value), 120000) == WAIT_TIMEOUT)
            TerminateProcess(GetCurrentProcess(), 124);
        return 0;
    }, event, 0, nullptr) : nullptr;
    Watchdog() { require(event && thread, "watchdog creation failed"); }
    ~Watchdog() { SetEvent(event); WaitForSingleObject(thread, INFINITE); CloseHandle(thread); CloseHandle(event); }
};

// Only the production borrower receives these forwarding resolvers. Fixture
// rendering/readback use the real resolver, so they cannot manufacture evidence.
PFN_vkGetInstanceProcAddr realGipa{};
PFN_vkGetDeviceProcAddr realGdpa{};
PFN_vkQueuePresentKHR realPresent{};
PFN_vkQueueSubmit realQueueSubmit{};
PFN_vkWaitForFences realWaitForFences{};
PFN_vkCreateImage realCreateImage{};
PFN_vkCmdBlitImage realBlit{};
PFN_vkCmdCopyImage realCopy{};
PFN_vkCmdCopyBufferToImage realBufferCopy{};
PFN_vkCmdPipelineBarrier realPipelineBarrier{};
PFN_vkCmdCopyImageToBuffer realImageToBuffer{};
PFN_vkCmdDispatch realDispatch{};
PFN_vkCreateImageView realCreateImageView{};
PFN_vkUpdateDescriptorSets realUpdateDescriptors{};
PFN_vkCmdWriteTimestamp realWriteTimestamp{};
PFN_vkGetPhysicalDeviceFormatProperties realProperties{};
struct ValidationCapture {
    std::atomic<unsigned> errors{},warnings{},messages{};
    bool loaded{},cleanupFailed{};
} validation;
bool validationPassed() noexcept {return validation.loaded&&!validation.cleanupFailed&&!validation.errors.load();}
VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* data,void*) noexcept {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++validation.errors;
    else if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)++validation.warnings;
    if(validation.messages.fetch_add(1)<64)std::fprintf(stderr,"VVL severity=%u id=%s: %s\n",severity,
        data&&data->pMessageIdName?data->pMessageIdName:"unknown",data&&data->pMessage?data->pMessage:"no message");
    return VK_FALSE;
}
struct Trace {
    bool fake{};
    bool asyncCopy{};
    bool sharedCopy{};
    bool syncShared{};
    bool depthCompute{};
    bool maskedDepthStorage{};
    bool validationRequested{};
    bool asyncCapacity{};
    bool gpuTimestamps{};
    bool gatedCopy{};
    bool forceK1Reset{};
    unsigned gateDelayMs{100};
    VkCommandBuffer privateCopyCommand{};
    unsigned driverCreates{},driverImages{},driverAcquires{},driverPresents{},driverDestroys{},semaphoreAcquires{};
    std::vector<VkImage> wsi;
    std::map<VkImage, VkImageCreateInfo> created;
    std::map<VkImageView,VkImage> viewImages;
    VkImage source{}, stable{};
    std::array<VkImage,2> auxiliarySources{};
    VkCommandBuffer producerCommand{};
    VkBuffer oracle{};
    std::array<VkImage,2> auxiliaryTargets{};
    unsigned oracleMask{};
    VkImage computeDepthTarget{};
    unsigned depthDispatches{},depthImageToBuffer{},depthBufferToImage{};
    VkQueryPool depthQueries{};
    std::uint32_t depthTimestampBits{};
    float depthTimestampPeriod{};
    unsigned depthQueryStage{};
    std::uint64_t depthQueryRecordQpc{};
    VkFence privateFence{};
    VkImageBlit blit{};
    VkExtent3D copied{};
    VkFilter filter{};
    unsigned blits{}, copies{};
    bool maskedLinear{}, valid{true};
    bool native(VkImage image) const { return std::find(wsi.begin(), wsi.end(), image) != wsi.end(); }
    void frame() { source = stable = VK_NULL_HANDLE; auxiliarySources={};auxiliaryTargets={};oracleMask=0;producerCommand={};computeDepthTarget={};
        depthDispatches=depthImageToBuffer=depthBufferToImage=depthQueryStage=0;privateCopyCommand = VK_NULL_HANDLE; blits = copies = 0; blit = {}; copied = {}; valid = true; }
} trace;
void depthTimestamp(VkCommandBuffer command,VkPipelineStageFlagBits stage,bool begin) {
    if(!trace.depthQueries)return;
    require(trace.depthQueryStage==(begin?0u:1u),"depth timestamp must identify one exact shader or two-transfer capture");
    if(begin) {LARGE_INTEGER now{};QueryPerformanceCounter(&now);trace.depthQueryRecordQpc=now.QuadPart;}
    realWriteTimestamp(command,stage,trace.depthQueries,begin?0:1);++trace.depthQueryStage;
}
// Test-only reads occur before production releases imported images to EXTERNAL.
// Restore TRANSFER_DST so the production barriers and ownership stay unchanged.
void snapshotWrittenPlane(VkCommandBuffer command,VkImage image,VkExtent3D extent,unsigned plane,bool storage = false) {
    if(!trace.sharedCopy)return;
    require(trace.oracle&&plane<3&&!(trace.oracleMask&(1u<<plane)),"one exact destination snapshot per production plane write");
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=image;
    barrier.oldLayout=storage?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask=storage?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    realPipelineBarrier(command,storage?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=extent;
    copy.bufferOffset=plane?colorOracleBytes+(plane-1)*auxiliaryOracleBytes:0;
    realImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,trace.oracle,1,&copy);
    std::swap(barrier.oldLayout,barrier.newLayout);std::swap(barrier.srcAccessMask,barrier.dstAccessMask);
    realPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,storage?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    trace.oracleMask|=1u<<plane;
}
std::array<std::uint8_t,4> expectedColor(std::uint32_t sequence) {
    if(trace.asyncCopy||trace.sharedCopy)return {static_cast<std::uint8_t>(sequence),static_cast<std::uint8_t>(sequence>>8),0xA5,255};
    return {static_cast<std::uint8_t>((sequence&1)?255:0),static_cast<std::uint8_t>((sequence&1)?0:255),0,255};
}
float expectedDepth(std::uint32_t sequence) {
    return trace.asyncCopy||trace.sharedCopy ? 0.125f+static_cast<float>(sequence)/1024 : (0.1f*1000/5-0.1f)/(1000-0.1f);
}
std::array<float,2> expectedMotion(std::uint32_t sequence) {
    return {static_cast<float>(sequence%31+1)/32,-static_cast<float>(sequence%29+1)/32};
}
std::uint16_t exactHalf(float value) {
    std::uint32_t bits{};std::memcpy(&bits,&value,sizeof(bits));
    return static_cast<std::uint16_t>((bits>>16&0x8000)|(((bits>>23&255)-112)<<10)|(bits>>13&1023));
}
// The gate is after the producer submission, so its fence remains reusable.
// Release it before any teardown or queue drain, including exception unwinding.
struct CopyGate;
CopyGate* copyGate{};
struct CopyGate {
    VkDevice device{}; VkCommandBuffer command{}; VkEvent event{}; VkFence fence{};
    VkFence privateFence{};
    HANDLE submitted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr), thread{};
    std::atomic<bool> released{};
    std::atomic<std::uint64_t> releaseQpc{};
    VkResult releaseResult{VK_NOT_READY};
    bool inserted{}, returnedBeforeRelease{}, returnObserved{}, completed{};
    std::uint64_t presentReturnQpc{};
    std::uint64_t nextWaitEntryQpc{},nextWaitReturnQpc{};
    unsigned delayMs{};
    CopyGate(VkDevice d, VkCommandPool pool, unsigned delay) : device(d), delayMs(delay) {
        require(submitted && cancel, "gate host events unavailable");
        VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        a.commandPool=pool; a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; a.commandBufferCount=1;
        check(reinterpret_cast<PFN_vkAllocateCommandBuffers>(realGdpa(d,"vkAllocateCommandBuffers"))(d,&a,&command),"allocate gate command");
        VkEventCreateInfo e{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        check(reinterpret_cast<PFN_vkCreateEvent>(realGdpa(d,"vkCreateEvent"))(d,&e,nullptr,&event),"create gate event");
        VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(reinterpret_cast<PFN_vkCreateFence>(realGdpa(d,"vkCreateFence"))(d,&f,nullptr,&fence),"create gate fence");
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(reinterpret_cast<PFN_vkBeginCommandBuffer>(realGdpa(d,"vkBeginCommandBuffer"))(command,&b),"begin gate command");
        reinterpret_cast<PFN_vkCmdWaitEvents>(realGdpa(d,"vkCmdWaitEvents"))(command,1,&event,
            VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,nullptr,0,nullptr,0,nullptr);
        check(reinterpret_cast<PFN_vkEndCommandBuffer>(realGdpa(d,"vkEndCommandBuffer"))(command),"end gate command");
        thread=CreateThread(nullptr,0,[](void* p)->DWORD {
            auto& gate=*static_cast<CopyGate*>(p);
            if(WaitForSingleObject(gate.submitted,1000)==WAIT_OBJECT_0) WaitForSingleObject(gate.cancel,gate.delayMs);
            gate.release(); return 0;
        },this,0,nullptr);
        require(thread!=nullptr,"gate releaser unavailable");
    }
    void release() noexcept {
        if(!event||released.exchange(true)) return;
        releaseResult=reinterpret_cast<PFN_vkSetEvent>(realGdpa(device,"vkSetEvent"))(device,event);
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now); releaseQpc=now.QuadPart;
    }
    void finish() noexcept {
        SetEvent(cancel); release();
        if(thread) { WaitForSingleObject(thread,INFINITE); CloseHandle(thread); thread=nullptr; }
        if(inserted&&!completed) completed=reinterpret_cast<PFN_vkWaitForFences>(realGdpa(device,"vkWaitForFences"))(device,1,&fence,VK_TRUE,timeout)==VK_SUCCESS;
    }
    ~CopyGate() {
        finish();
        if(copyGate==this) copyGate=nullptr;
        if(!inserted||completed) {
            if(fence) reinterpret_cast<PFN_vkDestroyFence>(realGdpa(device,"vkDestroyFence"))(device,fence,nullptr);
            if(event) reinterpret_cast<PFN_vkDestroyEvent>(realGdpa(device,"vkDestroyEvent"))(device,event,nullptr);
        }
        if(submitted) CloseHandle(submitted); if(cancel) CloseHandle(cancel);
    }
};
VKAPI_ATTR VkResult VKAPI_CALL queueSubmit(VkQueue queue,std::uint32_t count,const VkSubmitInfo* submits,VkFence fence) {
    for(std::uint32_t i=0;i<count;++i)for(std::uint32_t j=0;j<submits[i].commandBufferCount;++j)
        if(trace.privateCopyCommand&&submits[i].pCommandBuffers[j]==trace.privateCopyCommand)trace.privateFence=fence;
    if(copyGate&&!copyGate->inserted&&trace.privateCopyCommand) {
        bool matched{};
        for(std::uint32_t i=0;i<count;++i) for(std::uint32_t j=0;j<submits[i].commandBufferCount;++j)
            matched|=submits[i].pCommandBuffers[j]==trace.privateCopyCommand;
        if(matched) {
            VkSubmitInfo gate{VK_STRUCTURE_TYPE_SUBMIT_INFO}; gate.commandBufferCount=1; gate.pCommandBuffers=&copyGate->command;
            const auto result=realQueueSubmit(queue,1,&gate,copyGate->fence);
            if(result!=VK_SUCCESS) return result;
            copyGate->inserted=true; SetEvent(copyGate->submitted);
            copyGate->privateFence=fence;
        }
    }
    return realQueueSubmit(queue,count,submits,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL waitForFences(VkDevice device,std::uint32_t count,const VkFence* fences,VkBool32 all,std::uint64_t timeoutNs) {
    const bool watch=copyGate&&copyGate->returnObserved&&!copyGate->nextWaitEntryQpc&&count==1&&fences&&*fences==copyGate->privateFence;
    LARGE_INTEGER now{};
    if(watch) { QueryPerformanceCounter(&now); copyGate->nextWaitEntryQpc=now.QuadPart; }
    const auto result=realWaitForFences(device,count,fences,all,timeoutNs);
    if(watch) { QueryPerformanceCounter(&now); copyGate->nextWaitReturnQpc=now.QuadPart; }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL createChain(VkDevice d, const VkSwapchainCreateInfoKHR *info,
                                           const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    ++trace.driverCreates;
    return reinterpret_cast<PFN_vkCreateSwapchainKHR>(realGdpa(d, "vkCreateSwapchainKHR"))(d, info,
                                                                                           a, out);
}
VKAPI_ATTR void VKAPI_CALL destroyChain(VkDevice d, VkSwapchainKHR chain,
                                        const VkAllocationCallbacks *a) {
    ++trace.driverDestroys;
    reinterpret_cast<PFN_vkDestroySwapchainKHR>(realGdpa(d, "vkDestroySwapchainKHR"))(d, chain, a);
}
VKAPI_ATTR VkResult VKAPI_CALL chainImages(VkDevice d, VkSwapchainKHR chain, std::uint32_t *count,
                                           VkImage *images) {
    ++trace.driverImages;
    return reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(realGdpa(d, "vkGetSwapchainImagesKHR"))(
        d, chain, count, images);
}
VKAPI_ATTR VkResult VKAPI_CALL nativePresent(VkQueue queue, const VkPresentInfoKHR *info) {
    ++trace.driverPresents;
    return realPresent(queue, info);
}
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice device, const VkImageCreateInfo *info,
                                           const VkAllocationCallbacks *allocation,
                                           VkImage *image) {
    const auto result = realCreateImage(device, info, allocation, image);
    if (result == VK_SUCCESS && info && image) trace.created[*image] = *info;
    return result;
}
VKAPI_ATTR void VKAPI_CALL blitImage(VkCommandBuffer command, VkImage source, VkImageLayout sourceLayout,
    VkImage destination, VkImageLayout destinationLayout, std::uint32_t count, const VkImageBlit* regions, VkFilter filter) {
    if (trace.native(source)) {
        trace.privateCopyCommand=command;
        ++trace.blits; trace.source = source; trace.stable = destination; trace.filter = filter;
        trace.valid &= count == 1 && regions && sourceLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            destinationLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if (count == 1 && regions) trace.blit = regions[0];
    }
    realBlit(command, source, sourceLayout, destination, destinationLayout, count, regions, filter);
    if(trace.sharedCopy&&trace.native(source)&&count==1&&regions)
        snapshotWrittenPlane(command,destination,{static_cast<std::uint32_t>(regions[0].dstOffsets[1].x),static_cast<std::uint32_t>(regions[0].dstOffsets[1].y),1},0);
}
VKAPI_ATTR void VKAPI_CALL copyImage(VkCommandBuffer command, VkImage source, VkImageLayout sourceLayout,
    VkImage destination, VkImageLayout destinationLayout, std::uint32_t count, const VkImageCopy* regions) {
    if (trace.native(source)) {
        trace.privateCopyCommand=command;
        ++trace.copies; trace.source = source; trace.stable = destination;
        trace.valid &= count == 1 && regions && sourceLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            destinationLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if (count == 1 && regions) trace.copied = regions[0].extent;
    } else if(const auto target=trace.created.find(destination);target!=trace.created.end()) {
        if(target->second.format==VK_FORMAT_R32_SFLOAT)trace.auxiliarySources[0]=source;
        if(target->second.format==VK_FORMAT_R16G16_SFLOAT)trace.auxiliarySources[1]=source;
    }
    realCopy(command, source, sourceLayout, destination, destinationLayout, count, regions);
    if(trace.sharedCopy&&count==1&&regions) {
        if(trace.native(source))snapshotWrittenPlane(command,destination,regions[0].extent,0);
        else if(command==trace.producerCommand) {
            trace.auxiliaryTargets[1]=destination;snapshotWrittenPlane(command,destination,regions[0].extent,2);
        }
    }
}
VKAPI_ATTR void VKAPI_CALL copyBufferToImage(VkCommandBuffer command,VkBuffer source,VkImage destination,VkImageLayout layout,
        std::uint32_t count,const VkBufferImageCopy* regions) {
    realBufferCopy(command,source,destination,layout,count,regions);
    if(trace.sharedCopy&&command==trace.producerCommand&&count==1&&regions) {
        depthTimestamp(command,VK_PIPELINE_STAGE_TRANSFER_BIT,false);
        ++trace.depthBufferToImage;
        trace.auxiliaryTargets[0]=destination;snapshotWrittenPlane(command,destination,regions[0].imageExtent,1);
    }
}
VKAPI_ATTR void VKAPI_CALL copyImageToBuffer(VkCommandBuffer command,VkImage source,VkImageLayout layout,VkBuffer target,
        std::uint32_t count,const VkBufferImageCopy* regions) {
    if(trace.sharedCopy&&command==trace.producerCommand) {depthTimestamp(command,VK_PIPELINE_STAGE_TRANSFER_BIT,true);++trace.depthImageToBuffer;}
    realImageToBuffer(command,source,layout,target,count,regions);
}
VKAPI_ATTR VkResult VKAPI_CALL createImageView(VkDevice device,const VkImageViewCreateInfo* info,const VkAllocationCallbacks* allocator,VkImageView* out) {
    const auto result=realCreateImageView(device,info,allocator,out);
    if(result==VK_SUCCESS&&info&&out)trace.viewImages[*out]=info->image;return result;
}
VKAPI_ATTR void VKAPI_CALL updateDescriptors(VkDevice device,std::uint32_t count,const VkWriteDescriptorSet* writes,
        std::uint32_t copies,const VkCopyDescriptorSet* copy) {
    realUpdateDescriptors(device,count,writes,copies,copy);
    if(trace.sharedCopy)for(unsigned i=0;i<count;++i)if(writes[i].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE&&
            writes[i].dstBinding==1&&writes[i].descriptorCount==1&&writes[i].pImageInfo) {
        const auto image=trace.viewImages.find(writes[i].pImageInfo->imageView);
        require(image!=trace.viewImages.end(),"depth shader output must use the actual Bridge-created storage view");trace.computeDepthTarget=image->second;
    }
}
VKAPI_ATTR void VKAPI_CALL depthDispatch(VkCommandBuffer command,std::uint32_t x,std::uint32_t y,std::uint32_t z) {
    if(trace.sharedCopy&&command==trace.producerCommand)depthTimestamp(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,true);
    realDispatch(command,x,y,z);
    if(trace.sharedCopy&&command==trace.producerCommand) {
        depthTimestamp(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,false);
        require(trace.computeDepthTarget&&x==40&&y==23&&z==1,"depth shader dispatch bounds differ from actual 320x180 input");
        ++trace.depthDispatches;trace.auxiliaryTargets[0]=trace.computeDepthTarget;
        snapshotWrittenPlane(command,trace.computeDepthTarget,{320,180,1},1,true);
    }
}
VKAPI_ATTR void VKAPI_CALL formatProperties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties* properties) {
    realProperties(physical, format, properties);
    // Capability-negative case only: preserve all real driver answers except
    // this bit, and let production select its own full-resolution fallback.
    if (trace.maskedLinear && properties)
        properties->optimalTilingFeatures &= ~VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if(trace.maskedDepthStorage&&format==VK_FORMAT_R32_SFLOAT&&properties)
        properties->optimalTilingFeatures&=~VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceResolver(VkDevice device, const char* name) {
    if(!std::strcmp(name,"vkQueueSubmit"))return reinterpret_cast<PFN_vkVoidFunction>(queueSubmit);
    if(!std::strcmp(name,"vkWaitForFences"))return reinterpret_cast<PFN_vkVoidFunction>(waitForFences);
    if(!std::strcmp(name,"vkCreateSwapchainKHR"))return reinterpret_cast<PFN_vkVoidFunction>(createChain);
    if(!std::strcmp(name,"vkDestroySwapchainKHR"))return reinterpret_cast<PFN_vkVoidFunction>(destroyChain);
    if(!std::strcmp(name,"vkGetSwapchainImagesKHR"))return reinterpret_cast<PFN_vkVoidFunction>(chainImages);
    if(!std::strcmp(name,"vkQueuePresentKHR"))return reinterpret_cast<PFN_vkVoidFunction>(nativePresent);
    if (!std::strcmp(name, "vkCreateImage")) return reinterpret_cast<PFN_vkVoidFunction>(createImage);
    if (!std::strcmp(name, "vkCmdBlitImage")) return reinterpret_cast<PFN_vkVoidFunction>(blitImage);
    if (!std::strcmp(name, "vkCmdCopyImage")) return reinterpret_cast<PFN_vkVoidFunction>(copyImage);
    if (!std::strcmp(name, "vkCmdCopyBufferToImage")) return reinterpret_cast<PFN_vkVoidFunction>(copyBufferToImage);
    if (!std::strcmp(name, "vkCmdCopyImageToBuffer")) return reinterpret_cast<PFN_vkVoidFunction>(copyImageToBuffer);
    if (!std::strcmp(name, "vkCreateImageView")) return reinterpret_cast<PFN_vkVoidFunction>(createImageView);
    if (!std::strcmp(name, "vkUpdateDescriptorSets")) return reinterpret_cast<PFN_vkVoidFunction>(updateDescriptors);
    if (!std::strcmp(name, "vkCmdDispatch")) return reinterpret_cast<PFN_vkVoidFunction>(depthDispatch);
    return realGdpa(device, name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceResolver(VkInstance instance, const char* name) {
    if (!std::strcmp(name, "vkGetPhysicalDeviceFormatProperties")) return reinterpret_cast<PFN_vkVoidFunction>(formatProperties);
    return realGipa(instance, name);
}

// SourceOwner supplies finite waits, GPU clears and safe parent retention. This
// initializer additionally enables real Win32 WSI, as in the lifecycle fixture.
struct Owner : xess_vk::testing::SourceOwner {
    xess_vk::QueueGate queueGate;
    HWND window{};
    std::unique_ptr<xess_vk::presentation::Surface> surface;
    std::unique_ptr<xess_vk::presentation::DeviceRecord> record;
    std::unique_ptr<cp::Provider> provider;
    std::unique_ptr<xess_vk::presentation::Runtime> runtime;
    VkSwapchainKHR chain{};
    VkSwapchainCreateInfoKHR chainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    VkExtent2D currentSourceExtent{sourceExtent};
    VkRect2D currentSourceRegion{sourceRegion};
    unsigned physicalResizeChecks{};
    VkFence acquireFence{}, readFence{};
    VkSemaphore acquireSemaphore{};
    VkSemaphore ready{};
    VkCommandBuffer readCommand{};
    VkBuffer readback{};
    VkDeviceMemory hostMemory{};
    bool coherent{}, successfulStop{};
    std::vector<bool> presented;
    std::uint32_t imageIndex{}, sequence{};
    unsigned lowFrames{}, heldFrames{}, enabledFrames{}, doublePresentOnSamples{}, fullFrames{}, pixelFrames{}, missingFrames{};
    std::array<unsigned,3> phaseDoublePresentSamples{};
    unsigned canvasTransitions{};
    VkImage previousStable{};
    VkExtent2D previousCanvas{};
    std::uint64_t pixelCount{};
    unsigned vkGpuSamples{}, interopGpuSamples{}, d3dGpuSamples{};
    std::map<std::uint64_t, std::uint64_t> vkGpuSequences, interopGpuSequences, d3dGpuSequences;
    std::map<std::uint64_t,std::pair<std::uint32_t,std::uint32_t>> copyIdentities;
    xess_vk::FrameTimingTicket currentTicket;
    bool captureCurrent{};
    bool inputReset{};
    bool fullLogicalOutput{};
    bool nextImageAcquired{};
    std::uint32_t nextImageIndex{};
    struct Producer {VkCommandBuffer command{};VkFence fence{};VkSemaphore ready{};};
    std::array<Producer,3> asyncProducers;
    struct CanvasRead {
        VkCommandBuffer command{};VkFence fence{};VkDeviceSize offset{};
        VkExtent2D extent{};VkRect2D region{};std::uint32_t sequence{};bool auxiliary{};
    };
    std::array<CanvasRead,3> asyncReads;
    std::vector<std::uint32_t> preparedImages;
    int deferredRead{-1};
    unsigned asyncCapacityCases{},asyncPendingAcquireCases{},auxiliaryPixelFrames{};
    unsigned sharedColorFrames{},sharedAuxiliaryFrames{},sharedSourceFrames{},sharedWraps{};
    unsigned sharedStableRecoveries{};
    std::array<VkImageView,2> gameViews{};
    std::array<VkDescriptorSetLayout,2> gameSetLayouts{};
    std::array<VkDescriptorSet,2> gameSets{};
    VkDescriptorPool gameDescriptorPool{};
    VkPipelineLayout gamePipelineLayout{};
    VkPipeline gamePipeline{};
    VkBuffer gameProbe{};
    VkDeviceMemory gameProbeMemory{};
    bool gameProbeCoherent{};
    unsigned depthComputeFrames{},depthTransferFrames{},computeRestoreChecks{};
    unsigned depthGpuSamples{};
    VkDebugUtilsMessengerEXT validationMessenger{};
    std::map<std::pair<std::uint64_t,std::uint32_t>,std::uint64_t> sharedLeaseGenerations;
    xess_vk::presentation::FrameStatus lastFrameStatus;
    void checkGpuTimestamp(const xess_vk::GpuTimestampSample& sample, unsigned scope, std::uint32_t currentPresentId) {
        require(sample.requested == trace.gpuTimestamps, "GPU timestamp opt-in differs from requested mode");
        if (!sample.available) return;
        require(sample.sequence && sample.contextGeneration && sample.generation && sample.durationKnown &&
            std::isfinite(sample.durationNs) && sample.durationNs >= 0 && sample.validBits &&
            sample.fenceObservedQpcTicks >= sample.submitQpcTicks,
            "Completed GPU timestamp has invalid identity, duration, or host completion order");
        if (scope == 2) {
            require(sample.sdkPresentIdKnown && sample.sdkPresentId && sample.sdkPresentId <= currentPresentId &&
                sample.validBits == 64 && sample.frequencyHz && !sample.calibration.maxDeviationKnown,
                "D3D caller query lost its original Present ID or invented calibration deviation");
        }
        if (scope == 1)
            require(sample.sourceEngineFrameKnown && sample.sourceEngineFrame <= sequence &&
                (!sample.sdkPresentIdKnown || sample.sdkPresentId <= currentPresentId) && sample.sourceCopyTokenKnown == trace.fake &&
                !sample.calibration.available, "Interop query must retain source frame/token without inventing QPC calibration");
        if(trace.asyncCopy&&scope<2) {
            const auto expected=copyIdentities.find(sample.sourceCopyToken);
            require(sample.sourceCopyTokenKnown&&sample.sourceEngineFrameKnown&&expected!=copyIdentities.end()&&
                expected->second.first==sample.sourceEngineFrame&&(!sample.sdkPresentIdKnown||expected->second.second==sample.sdkPresentId),
                "completed per-slot query borrowed a different submission's token, engine frame or SDK ID");
        }
        auto& sequences = scope == 2 ? d3dGpuSequences : scope == 1 ? interopGpuSequences : vkGpuSequences;
        auto& previous = sequences[sample.contextGeneration];
        require(sample.sequence >= previous, "GPU timestamp sequence went backwards within one context");
        if (sample.sequence != previous) {
            ++(scope == 2 ? d3dGpuSamples : scope == 1 ? interopGpuSamples : vkGpuSamples);
            previous = sample.sequence;
        }
    }
    const char* instanceExtensions[3]{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const char* validationLayer="VK_LAYER_KHRONOS_validation";
    const char* deviceExtensions[3]{VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    ~Owner() {
        if (trace.fake && successfulStop) {
            runtime.reset();
            record.reset();
            provider.reset();
            if(gamePipeline)dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device,gamePipeline,nullptr);
            if(gamePipelineLayout)dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device,gamePipelineLayout,nullptr);
            if(gameDescriptorPool)dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(device,gameDescriptorPool,nullptr);
            for(const auto layout:gameSetLayouts)if(layout)dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(device,layout,nullptr);
            for(const auto view:gameViews)if(view)dev<PFN_vkDestroyImageView>("vkDestroyImageView")(device,view,nullptr);
            if(gameProbe)dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device,gameProbe,nullptr);
            if(gameProbeMemory)dev<PFN_vkFreeMemory>("vkFreeMemory")(device,gameProbeMemory,nullptr);
            if (acquireSemaphore)
                dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device, acquireSemaphore,
                                                                  nullptr);
            if (ready)
                dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device, ready, nullptr);
            for(const auto& producer:asyncProducers) {
                if(producer.ready&&producer.ready!=ready)dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device,producer.ready,nullptr);
                if(producer.fence&&producer.fence!=completion)dev<PFN_vkDestroyFence>("vkDestroyFence")(device,producer.fence,nullptr);
            }
            for(const auto& read:asyncReads)if(read.fence)dev<PFN_vkDestroyFence>("vkDestroyFence")(device,read.fence,nullptr);
            if (acquireFence)
                dev<PFN_vkDestroyFence>("vkDestroyFence")(device, acquireFence, nullptr);
            if (readFence)
                dev<PFN_vkDestroyFence>("vkDestroyFence")(device, readFence, nullptr);
            if (readback)
                dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, readback, nullptr);
            if (hostMemory)
                dev<PFN_vkFreeMemory>("vkFreeMemory")(device, hostMemory, nullptr);
            if(trace.depthQueries)dev<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(device,trace.depthQueries,nullptr);
            if (surface && surface->handle)
                inst<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(instance, surface->handle,
                                                                     nullptr);
            surface.reset();
            if (window)
                DestroyWindow(window);
            if(trace.validationRequested) {
                // Keep the messenger through device-child destruction too.
                if(!drainQueue(cfg.timeoutMs)) {validation.cleanupFailed=true;return;}
                if(pool)dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device,pool,nullptr);
                if(completion)dev<PFN_vkDestroyFence>("vkDestroyFence")(device,completion,nullptr);
                for(unsigned n=0;n<frame.planes.size();++n) {
                    if(frame.planes[n].image)dev<PFN_vkDestroyImage>("vkDestroyImage")(device,frame.planes[n].image,nullptr);
                    if(allocation[n])dev<PFN_vkFreeMemory>("vkFreeMemory")(device,allocation[n],nullptr);
                }
                dev<PFN_vkDestroyDevice>("vkDestroyDevice")(device,nullptr);device={};
                inst<PFN_vkDestroyDebugUtilsMessengerEXT>("vkDestroyDebugUtilsMessengerEXT")(instance,validationMessenger,nullptr);validationMessenger={};
            }
            retainForPossibleBorrowedChildren = false;
            return;
        }
        // Runtime may retain native WSI references even after private cleanup.
        // Keep the complete self-owned native tail until this test process exits.
        runtime.release();
        record.release();
        surface.release();
        if (!successfulStop)
            provider.release();
    }
    std::uint32_t memoryType(std::uint32_t mask, VkMemoryPropertyFlags flags) const {
        for (std::uint32_t n = 0; n < memory.memoryTypeCount; ++n)
            if ((mask & (1u << n)) && (memory.memoryTypes[n].propertyFlags & flags) == flags) return n;
        throw std::runtime_error("required memory type unavailable");
    }
    void prepareGameProbe() {
        if(!trace.sharedCopy)return;
        for(unsigned n=0;n<gameViews.size();++n) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=frame.planes[n+1].image;
            view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=frame.planes[n+1].successfulImageCreateInfo.format;
            view.subresourceRange={n?VK_IMAGE_ASPECT_COLOR_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
            check(dev<PFN_vkCreateImageView>("vkCreateImageView")(device,&view,nullptr,&gameViews[n]),"create sampled game input view");
        }
        const std::array<VkDescriptorSetLayoutBinding,2> images{{
            {5,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {7,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        const VkDescriptorSetLayoutBinding output{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=images.size();layout.pBindings=images.data();
        check(dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(device,&layout,nullptr,&gameSetLayouts[0]),"create original game image layout");
        layout.bindingCount=1;layout.pBindings=&output;
        check(dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(device,&layout,nullptr,&gameSetLayouts[1]),"create higher opaque game layout");
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipelineLayout.setLayoutCount=gameSetLayouts.size();pipelineLayout.pSetLayouts=gameSetLayouts.data();
        check(dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device,&pipelineLayout,nullptr,&gamePipelineLayout),"create original game pipeline layout");
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=sizeof(xess_vk::testing::depthRestoreProbeSpirv);module.pCode=xess_vk::testing::depthRestoreProbeSpirv;
        VkShaderModule shader{};check(dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(device,&module,nullptr,&shader),"create original game probe shader");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline.layout=gamePipelineLayout;
        pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline.stage.module=shader;pipeline.stage.pName="main";
        const auto created=dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(device,VK_NULL_HANDLE,1,&pipeline,nullptr,&gamePipeline);
        dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device,shader,nullptr);check(created,"create original game compute pipeline");
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=16;buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(dev<PFN_vkCreateBuffer>("vkCreateBuffer")(device,&buffer,nullptr,&gameProbe),"create game dispatch result");
        VkMemoryRequirements req{};dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device,gameProbe,&req);
        VkMemoryAllocateInfo memoryInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};memoryInfo.allocationSize=req.size;
        try {memoryInfo.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);gameProbeCoherent=true;}
        catch(...) {memoryInfo.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);}
        check(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&memoryInfo,nullptr,&gameProbeMemory),"allocate game dispatch result");
        check(dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device,gameProbe,gameProbeMemory,0),"bind game dispatch result");
        const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.maxSets=2;poolInfo.poolSizeCount=sizes.size();poolInfo.pPoolSizes=sizes.data();
        check(dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(device,&poolInfo,nullptr,&gameDescriptorPool),"create game probe descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=gameDescriptorPool;allocate.descriptorSetCount=gameSetLayouts.size();allocate.pSetLayouts=gameSetLayouts.data();
        check(dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(device,&allocate,gameSets.data()),"allocate original game descriptor sets");
        const std::array<VkDescriptorImageInfo,2> descriptors{{{VK_NULL_HANDLE,gameViews[0],VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,gameViews[1],VK_IMAGE_LAYOUT_GENERAL}}};
        VkDescriptorBufferInfo result{gameProbe,0,16};std::array<VkWriteDescriptorSet,3> writes{};
        for(unsigned n=0;n<2;++n) {auto& write=writes[n];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=gameSets[0];write.dstBinding=n?7:5;
            write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=&descriptors[n];}
        auto& write=writes[2];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=gameSets[1];write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&result;
        dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(device,writes.size(),writes.data(),0,nullptr);
        if(trace.gpuTimestamps) {
            require(trace.depthTimestampBits&&trace.depthTimestampBits<=64&&std::isfinite(trace.depthTimestampPeriod)&&trace.depthTimestampPeriod>0,
                "selected queue cannot provide a real depth GPU timestamp comparison");
            VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};queries.queryType=VK_QUERY_TYPE_TIMESTAMP;queries.queryCount=2;
            check(dev<PFN_vkCreateQueryPool>("vkCreateQueryPool")(device,&queries,nullptr,&trace.depthQueries),"create depth-only timestamp pair");
        }
    }
    void initialize() {
        cfg = {sourceExtent.width, sourceExtent.height, 10000, 320, 180, false};
        depthStencilSource = true;
        retainForPossibleBorrowedChildren = true;
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW;
        wc.lpszClassName = L"XeSSOwnedNativeLowResolution";
        require(RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "register owned window");
        RECT rect{0, 0, LONG(sourceExtent.width), LONG(sourceExtent.height)};
        require(AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE), "adjust owned window");
        window = CreateWindowExW(0, wc.lpszClassName, L"Self-owned NativeBackend low-resolution GPU fixture",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right-rect.left, rect.bottom-rect.top,
            nullptr, nullptr, wc.hInstance, nullptr);
        require(window != nullptr, "create owned window"); ShowWindow(window, SW_SHOW); ShowWindow(window, SW_SHOW);
        surface = std::make_unique<xess_vk::presentation::Surface>(window);
        loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(loader != nullptr, "system Vulkan loader unavailable");
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        require(gipa != nullptr, "vkGetInstanceProcAddr unavailable");
        application.apiVersion = VK_API_VERSION_1_2; application.pApplicationName = "Owned NativeBackend low-resolution fixture";
        instanceInfo.pApplicationInfo = &application; instanceInfo.enabledExtensionCount = 2;
        instanceInfo.ppEnabledExtensionNames = instanceExtensions;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        if(trace.validationRequested) {
            std::uint32_t layerCount{};check(inst<PFN_vkEnumerateInstanceLayerProperties>("vkEnumerateInstanceLayerProperties")(&layerCount,nullptr),"enumerate actual instance layers");
            std::vector<VkLayerProperties> layers(layerCount);check(inst<PFN_vkEnumerateInstanceLayerProperties>("vkEnumerateInstanceLayerProperties")(&layerCount,layers.data()),"read actual instance layers");
            require(std::any_of(layers.begin(),layers.end(),[&](const auto& layer){return !std::strcmp(layer.layerName,validationLayer);}),"requested Khronos validation layer is unavailable");
            instanceInfo.enabledLayerCount=1;instanceInfo.ppEnabledLayerNames=&validationLayer;instanceInfo.enabledExtensionCount=3;
            debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback=validationMessage;instanceInfo.pNext=&debug;
        }
        check(inst<PFN_vkCreateInstance>("vkCreateInstance")(&instanceInfo, nullptr, &instance), "create owned Vulkan instance");
        instanceInfo.pNext=nullptr;
        if(trace.validationRequested) {
            check(inst<PFN_vkCreateDebugUtilsMessengerEXT>("vkCreateDebugUtilsMessengerEXT")(instance,&debug,nullptr,&validationMessenger),"install real validation debug-utils messenger");
            std::array<wchar_t,32768> directory{},actual{};
            const auto length=GetEnvironmentVariableW(L"VK_LAYER_PATH",directory.data(),directory.size());
            require(length&&length<directory.size(),"validation requires one explicit fixture-only VK_LAYER_PATH");
            const auto expected=std::filesystem::path(directory.data())/L"VkLayer_khronos_validation.dll";
            const auto module=GetModuleHandleW(expected.c_str());require(module!=nullptr,"requested validation DLL was not actually loaded in this fixture");
            const auto copied=GetModuleFileNameW(module,actual.data(),actual.size());
            require(copied&&copied<actual.size()&&std::filesystem::equivalent(expected,std::filesystem::path(actual.data())),"loaded validation DLL differs from the requested local artifact");
            validation.loaded=true;
            std::cout<<"{\"event\":\"validation_layer_loaded\",\"explicitInstanceLayer\":true,\"debugUtilsMessengerInstalled\":true,\"modulePath\":"
                <<std::quoted(std::filesystem::path(actual.data()).generic_string())<<"}\n";
        }
        VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        si.hinstance = wc.hInstance; si.hwnd = surface->hidden();
        check(inst<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR")(instance, &si, nullptr, &surface->handle), "create hidden WSI surface");
        std::uint32_t count{};
        check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance, &count, nullptr), "physical device count");
        std::vector<VkPhysicalDevice> devices(count);
        check(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance, &count, devices.data()), "physical devices");
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties p{}; inst<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate, &p);
            if (p.vendorID != 0x8086) continue;
            inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(candidate, &count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(count);
            inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(candidate, &count, queues.data());
            for (std::uint32_t n=0; n<count; ++n) {
                VkBool32 supports{};
                check(inst<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR")(
                    candidate, n, surface->handle, &supports), "owned surface support");
                if (supports && queues[n].queueCount && (queues[n].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    physical=candidate;family=n;trace.depthTimestampBits=queues[n].timestampValidBits;trace.depthTimestampPeriod=p.limits.timestampPeriod;break;
                }
            }
            if (physical) break;
        }
        require(physical != VK_NULL_HANDLE, "Intel graphics/present queue unavailable");
        require(!trace.fake||family==0,"fake swapchain fixture requires sole queue family zero");
        queueInfo.queueFamilyIndex=family; queueInfo.queueCount=1; queueInfo.pQueuePriorities=&priority;
        timeline.timelineSemaphore=VK_TRUE; deviceInfo.pNext=&timeline;
        deviceInfo.queueCreateInfoCount=1; deviceInfo.pQueueCreateInfos=&queueInfo;
        deviceInfo.enabledExtensionCount=3; deviceInfo.ppEnabledExtensionNames=deviceExtensions;
        check(inst<PFN_vkCreateDevice>("vkCreateDevice")(physical, &deviceInfo, nullptr, &device), "create owned Vulkan device");
        gdpa=inst<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, family, 0, &queue);
        inst<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex=family;
        pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &pi, nullptr, &pool), "create owned command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
        check(dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &ai, &command), "allocate producer command");
        check(dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &ai, &readCommand), "allocate oracle command");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        check(dev<PFN_vkCreateFence>("vkCreateFence")(device, &fi, nullptr, &completion), "producer fence");
        fi.flags=0;
        check(dev<PFN_vkCreateFence>("vkCreateFence")(device, &fi, nullptr, &acquireFence), "acquire fence");
        if(trace.fake){VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};check(dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device,&si,nullptr,&acquireSemaphore),"owned fake acquire semaphore");}
        check(dev<PFN_vkCreateFence>("vkCreateFence")(device, &fi, nullptr, &readFence), "oracle fence");
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device, &sem, nullptr, &ready), "producer signal");
        if(trace.asyncCopy) {
            asyncProducers[0]={command,completion,ready};
            for(unsigned n=1;n<asyncProducers.size();++n) {
                auto& producer=asyncProducers[n];fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
                check(dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device,&ai,&producer.command),"allocate async producer command");
                check(dev<PFN_vkCreateFence>("vkCreateFence")(device,&fi,nullptr,&producer.fence),"create async producer fence");
                check(dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device,&sem,nullptr,&producer.ready),"create async producer signal");
            }
            fi.flags=0;
            for(unsigned n=0;n<asyncReads.size();++n) {
                auto& read=asyncReads[n];read.offset=(n+1)*oracleStride;
                check(dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device,&ai,&read.command),"allocate deferred canvas oracle");
                check(dev<PFN_vkCreateFence>("vkCreateFence")(device,&fi,nullptr,&read.fence),"create deferred canvas oracle fence");
            }
        }
        frame.producerQueue=queue; frame.producerCompleteFence=completion;
        for (unsigned n=0;n<3;++n) {
            auto& image=frame.planes[n]; auto& info=image.successfulImageCreateInfo; const auto size=cfg.extent(static_cast<xess_vk::Format>(n));
            image.device=device; image.ownerQueueFamily=family; image.currentLayout=VK_IMAGE_LAYOUT_GENERAL;
            info.imageType=VK_IMAGE_TYPE_2D; info.format=n==1?VK_FORMAT_D32_SFLOAT_S8_UINT:xess_vk::testing::kFormats[n]; info.extent={size.width,size.height,1};
            info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL;
            info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
            if(n==1)info.usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            check(dev<PFN_vkCreateImage>("vkCreateImage")(device,&info,nullptr,&image.image),"create fixture source image");
            VkMemoryRequirements req{}; dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device,image.image,&req);
            VkMemoryAllocateInfo mem{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mem.allocationSize=req.size;
            mem.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&mem,nullptr,&allocation[n]),"allocate fixture source");
            check(dev<PFN_vkBindImageMemory>("vkBindImageMemory")(device,image.image,allocation[n],0),"bind fixture source");
        }
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=oracleStride*(trace.asyncCopy?4:1);
        bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(dev<PFN_vkCreateBuffer>("vkCreateBuffer")(device,&bi,nullptr,&readback),"create canvas oracle");
        VkMemoryRequirements req{}; dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device,readback,&req);
        VkMemoryAllocateInfo mem{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mem.allocationSize=req.size;
        try { mem.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); coherent=true; }
        catch (...) { mem.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT); }
        check(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&mem,nullptr,&hostMemory),"allocate canvas oracle");
        check(dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device,readback,hostMemory,0),"bind canvas oracle");
        realGipa=gipa; realGdpa=gdpa; realCreateImage=dev<PFN_vkCreateImage>("vkCreateImage");
        realPresent=dev<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
        realQueueSubmit=dev<PFN_vkQueueSubmit>("vkQueueSubmit");
        realWaitForFences=dev<PFN_vkWaitForFences>("vkWaitForFences");
        realBlit=dev<PFN_vkCmdBlitImage>("vkCmdBlitImage"); realCopy=dev<PFN_vkCmdCopyImage>("vkCmdCopyImage");
        realBufferCopy=dev<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
        realPipelineBarrier=dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");realImageToBuffer=dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
        realDispatch=dev<PFN_vkCmdDispatch>("vkCmdDispatch");realCreateImageView=dev<PFN_vkCreateImageView>("vkCreateImageView");
        realUpdateDescriptors=dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
        realWriteTimestamp=dev<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp");
        realProperties=inst<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
        prepareGameProbe();
        cp::Config config{device,queue,family,memory,trace.sharedCopy?deviceResolver:gdpa};
        config.depthCompute=trace.depthCompute;
        if(trace.gpuTimestamps){VkPhysicalDeviceProperties props{};inst<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical,&props);
            std::uint32_t count{};const auto families=inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");families(physical,&count,nullptr);
            std::vector<VkQueueFamilyProperties> properties(count);families(physical,&count,properties.data());
            config.gpuTimestamps=true;config.timestampPeriodNs=props.limits.timestampPeriod;config.timestampValidBits=properties.at(family).timestampValidBits;}
        provider=std::make_unique<cp::Provider>(config);
        trace.oracle=readback;
        record=std::make_unique<xess_vk::presentation::DeviceRecord>(instance,physical,device,instanceResolver,deviceResolver,VK_API_VERSION_1_2,deviceInfo,trace.fake,trace.fake?&queueGate:nullptr);
        runtime=record->create(*surface,queue,provider.get());
        VkSurfaceCapabilitiesKHR caps{};
        check(inst<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(physical,surface->handle,&caps),"WSI capabilities");
        require(same(caps.currentExtent,sourceExtent),"hidden source extent mismatch");
        check(inst<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR")(physical,surface->handle,&count,nullptr),"WSI format count");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(inst<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR")(physical,surface->handle,&count,formats.data()),"WSI formats");
        require(std::any_of(formats.begin(),formats.end(),[](auto f){return f.format==VK_FORMAT_R8G8B8A8_UNORM&&f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;}),"RGBA8 SDR WSI unavailable");
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; ci.surface=surface->handle;
        ci.minImageCount=std::max(2u,caps.minImageCount); if(caps.maxImageCount)ci.minImageCount=std::min(ci.minImageCount,caps.maxImageCount);
        ci.imageFormat=VK_FORMAT_R8G8B8A8_UNORM;ci.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        ci.imageExtent=sourceExtent;ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=caps.currentTransform;
        ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=VK_FALSE;
        check(runtime->create(&ci,nullptr,&chain),"production Runtime create");
        require(!runtime->failed(),runtime->error());
        check(runtime->images(chain,&count,nullptr),"WSI image count");trace.wsi.resize(count);
        check(runtime->images(chain,&count,trace.wsi.data()),"WSI images");presented.resize(count);
        chainInfo=ci;
        require(!validation.errors,"validation reported an error during real fixture initialization");
    }
    cp::SourcePair pair(VkCommandBuffer command) const {
        namespace fi=xess_vk::frame_input;
        cp::SourcePair p; p.depth={bits(frame.planes[1].image),2}; p.motion={bits(frame.planes[2].image),3};
        p.invocation=sequence;p.thread=GetCurrentThreadId();p.valid=p.frameValid=true;p.engineFrame=sequence;p.timing=currentTicket;
        auto& c=p.constants;c.producerToken=sequence;c.commandBuffer=bits(command);c.recordingGeneration=sequence;
        c.viewportGeneration=1;c.deviceIncarnation=1;c.renderSize={320,180};c.upscaleSize=fullLogicalOutput?
            std::array<std::int32_t,2>{static_cast<std::int32_t>(currentSourceRegion.extent.width),static_cast<std::int32_t>(currentSourceRegion.extent.height)}:
            std::array<std::int32_t,2>{640,360};
        c.outputRegion={{0,currentSourceRegion.offset.y,static_cast<std::int32_t>(currentSourceExtent.width),static_cast<std::int32_t>(currentSourceRegion.extent.height)},true,true};c.motionVectorScale={1,1};c.jitter={0,0};
        c.cameraNear=0.1f;c.cameraFar=1000;c.deltaSeconds=1.0f/60;c.frameIndex=static_cast<float>(sequence);
        c.reset=inputReset;
        c.validityMask=fi::Extents|fi::Jitter|fi::MotionScale|fi::View|fi::Projection|fi::CameraPlanes|fi::Reset|fi::DeltaSeconds;
        c.view={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        const float ys=1.0f/std::tan(3.14159265358979323846f/6),xs=ys*9.0f/16;
        const float a=c.cameraNear/(c.cameraFar-c.cameraNear),b=c.cameraNear*c.cameraFar/(c.cameraFar-c.cameraNear);
        c.projection={xs,0,0,0,0,-ys,0,0,0,0,a,-1,0,0,b,0};
        return p;
    }
    void recordProducer(VkCommandBuffer cb) {
        trace.producerCommand=cb;
        const auto color=frame.planes[0].image,wsi=trace.wsi[imageIndex];
        std::array<VkImageMemoryBarrier,2> barriers{};
        for(auto& b:barriers){b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
        barriers[0].image=color;barriers[0].oldLayout=VK_IMAGE_LAYOUT_GENERAL;barriers[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barriers[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].image=wsi;barriers[1].oldLayout=presented[imageIndex]?(trace.fake?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_PRESENT_SRC_KHR):VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barriers[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        auto barrier=dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,barriers.data());
        const VkClearColorValue black{{0,0,0,1}};
        dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cb,wsi,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&barriers[1].subresourceRange);
        VkImageMemoryBarrier writes=barriers[1];writes.oldLayout=writes.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        writes.srcAccessMask=writes.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&writes);
        VkImageBlit region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        region.srcOffsets[1]={1280,800,1};region.dstOffsets[0]={0,currentSourceRegion.offset.y,0};
        region.dstOffsets[1]={static_cast<std::int32_t>(currentSourceExtent.width),currentSourceRegion.offset.y+static_cast<std::int32_t>(currentSourceRegion.extent.height),1};
        dev<PFN_vkCmdBlitImage>("vkCmdBlitImage")(cb,color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,wsi,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region,VK_FILTER_NEAREST);
        barriers[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barriers[0].newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barriers[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barriers[1].newLayout=trace.fake?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barriers[1].dstAccessMask=0;
        barrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,barriers.data());
        if(!captureCurrent)return;
        if(trace.depthQueries)dev<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(cb,trace.depthQueries,0,2);
        const auto p=pair(cb);provider->invocation(p);
        dm::DispatchEvidence e;e.metadataAccepted=true;e.count=2;e.command={bits(cb),sequence};
        if(trace.sharedCopy) {
            e.pipeline={bits(gamePipeline),1};e.pipelineLayout={bits(gamePipelineLayout),1};
            e.computeRestoreKnown=true;e.computeRestoreSetCount=gameSets.size();
            for(unsigned n=0;n<gameSets.size();++n)e.computeRestoreSets[n]={bits(gameSets[n]),1};
            dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb,VK_PIPELINE_BIND_POINT_COMPUTE,gamePipeline);
            dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb,VK_PIPELINE_BIND_POINT_COMPUTE,gamePipelineLayout,0,gameSets.size(),gameSets.data(),0,nullptr);
        }
        for(unsigned n=0;n<2;++n){
            const auto& image=frame.planes[n+1];const auto& info=image.successfulImageCreateInfo;auto& b=e.bindings[n];
            b.setIndex=0;b.binding=n?7:5;b.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;b.image=n?p.motion:p.depth;
            b.viewType=VK_IMAGE_VIEW_TYPE_2D;b.viewFormat=info.format;
            if(trace.sharedCopy)b.view={bits(gameViews[n]),1};
            b.range={n?VK_IMAGE_ASPECT_COLOR_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};b.declaredLayout=VK_IMAGE_LAYOUT_GENERAL;
            auto& d=b.imageDescription;d.format=info.format;d.width=info.extent.width;d.height=info.extent.height;d.depth=1;
            d.mipLevels=d.arrayLayers=1;d.samples=info.samples;d.usage=info.usage;d.tiling=info.tiling;d.sharingMode=info.sharingMode;
        }
        xess_vk::CommandRecordingFacts facts;facts.command=bits(cb);facts.device=bits(device);facts.pool=bits(pool);
        facts.contextIncarnation=1;facts.rpRecordingGeneration=sequence;facts.queueFamily=family;facts.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        facts.beginUsageFlags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;facts.recording=facts.stateKnown=facts.scopeKnown=true;
        require(provider->record(p.thread,facts,e,{sequence,currentTicket,0,true}),"production Provider refused same-ticket inputs");
        if(trace.sharedCopy) {
            VkBufferMemoryBarrier output{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};output.buffer=gameProbe;output.size=16;
            output.srcQueueFamilyIndex=output.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            output.srcAccessMask=output.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
            barrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&output,0,nullptr);
            // No rebind: this dispatch needs the original pipeline and both sets.
            dev<PFN_vkCmdDispatch>("vkCmdDispatch")(cb,1,1,1);
        }
    }
    void submitCanvasRead(CanvasRead& read,VkExtent2D extent,VkRect2D region,bool auxiliary) {
        read.extent=extent;read.region=region;read.sequence=sequence;read.auxiliary=auxiliary;
        require(trace.stable!=VK_NULL_HANDLE,"production color destination not observed");
        check(dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(read.command,0),"reset oracle command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(read.command,&begin),"begin oracle command");
        if(trace.sharedCopy) {
            require((trace.oracleMask&(auxiliary?7u:1u))==(auxiliary?7u:1u),"real destination write oracle missed this frame's color/depth/motion");
        } else {
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};image.image=trace.stable;
        image.oldLayout=image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;image.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
        image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(read.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
        VkBufferImageCopy copy{};copy.bufferOffset=read.offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};
        dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(read.command,trace.stable,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
        if(auxiliary)for(unsigned n=0;n<2;++n) {
            image.image=trace.auxiliarySources[n];require(image.image!=VK_NULL_HANDLE,"actual bridge did not record its Provider snapshot source");
            dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(read.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
            copy.bufferOffset=read.offset+colorOracleBytes+n*auxiliaryOracleBytes;copy.imageExtent={320,180,1};
            dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(read.command,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
        }
        }
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=readback;host.offset=read.offset;host.size=oracleStride;
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(read.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        if(trace.sharedCopy&&captureCurrent) {
            host.buffer=gameProbe;host.offset=0;host.size=16;host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
            dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(read.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        }
        check(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(read.command),"end oracle command");
        check(dev<PFN_vkResetFences>("vkResetFences")(device,1,&read.fence),"reset oracle fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&read.command;
        std::lock_guard lock(queueGate);
        check(dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&submit,read.fence),"submit real stable readback");
    }
    void verifyCanvasRead(const CanvasRead& read) {
        check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&read.fence,VK_TRUE,timeout),"complete real stable readback");
        void* mapped{};check(dev<PFN_vkMapMemory>("vkMapMemory")(device,hostMemory,0,VK_WHOLE_SIZE,0,&mapped),"map real stable pixels");
        if(!coherent){VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=hostMemory;range.size=VK_WHOLE_SIZE;
            check(dev<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device,1,&range),"invalidate real stable pixels");}
        const auto* pixels=static_cast<const std::uint8_t*>(mapped)+read.offset;bool valid=true;
        const auto extent=read.extent;const auto region=read.region;const auto color=expectedColor(read.sequence);
        for(std::uint32_t y=0;y<extent.height;++y)for(std::uint32_t x=0;x<extent.width;++x){
            const auto at=(static_cast<std::size_t>(y)*extent.width+x)*4;
            const bool active=x>=static_cast<unsigned>(region.offset.x)&&y>=static_cast<unsigned>(region.offset.y)&&
                x<static_cast<unsigned>(region.offset.x)+region.extent.width&&y<static_cast<unsigned>(region.offset.y)+region.extent.height;
            valid&=pixels[at]==(active?color[0]:0)&&pixels[at+1]==(active?color[1]:0)&&
                pixels[at+2]==(active?color[2]:0)&&pixels[at+3]==255;
        }
        if(read.auxiliary) {
            const auto depth=expectedDepth(read.sequence);const auto motion=expectedMotion(read.sequence);
            const std::array<std::uint16_t,2> half{exactHalf(motion[0]),exactHalf(motion[1])};
            for(std::size_t at=0;at<auxiliaryOracleBytes;at+=4)
                valid&=!std::memcmp(pixels+colorOracleBytes+at,&depth,4)&&
                    !std::memcmp(pixels+colorOracleBytes+auxiliaryOracleBytes+at,half.data(),4);
            ++auxiliaryPixelFrames;
        }
        dev<PFN_vkUnmapMemory>("vkUnmapMemory")(device,hostMemory);
        require(valid,"frame-coded color/depth/motion pixels differ from this submitted frame identity");
        ++pixelFrames;pixelCount+=static_cast<std::uint64_t>(extent.width)*extent.height;
        if(trace.sharedCopy&&read.auxiliary) {
            void* result{};check(dev<PFN_vkMapMemory>("vkMapMemory")(device,gameProbeMemory,0,VK_WHOLE_SIZE,0,&result),"map following game dispatch result");
            if(!gameProbeCoherent) {VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=gameProbeMemory;range.size=VK_WHOLE_SIZE;
                check(dev<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device,1,&range),"invalidate following game dispatch result");}
            std::array<std::uint32_t,4> words{};std::memcpy(words.data(),result,16);
            dev<PFN_vkUnmapMemory>("vkUnmapMemory")(device,gameProbeMemory);
            const auto depth=expectedDepth(read.sequence);std::uint32_t depthBits{};std::memcpy(&depthBits,&depth,4);
            const auto motion=expectedMotion(read.sequence);
            require(words==std::array<std::uint32_t,4>{depthBits,static_cast<std::uint32_t>(exactHalf(motion[0]))|(static_cast<std::uint32_t>(exactHalf(motion[1]))<<16),0xc0def00d,320|(180u<<16)},
                "following game dispatch lost its pipeline, full descriptor range, source layout, or current frame pixels");
            ++computeRestoreChecks;
            if(trace.depthQueries) {
                require(trace.depthQueryStage==2,"readback fence did not cover one complete depth query pair");
                std::array<std::uint64_t,4> values{};
                const auto result=dev<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(device,trace.depthQueries,0,2,sizeof(values),values.data(),2*sizeof(std::uint64_t),
                    VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                require(result==VK_SUCCESS&&values[1]&&values[3],"completed depth query pair is unavailable after its real readback fence");
                xess_vk::GpuTimestampSample sample;sample.available=true;sample.validBits=trace.depthTimestampBits;sample.timestampPeriodNs=trace.depthTimestampPeriod;
                sample.beginTicks=values[0];sample.endTicks=values[2];sample.submitQpcTicks=trace.depthQueryRecordQpc;
                LARGE_INTEGER now{},frequency{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
                sample.fenceObservedQpcTicks=now.QuadPart;sample.qpcFrequency=frequency.QuadPart;
                require(xess_vk::timestampDuration(sample),"depth query wrap or period cannot establish an actual GPU duration");++depthGpuSamples;
                std::cout<<"{\"event\":\"native_depth_gpu\",\"engineFrame\":"<<read.sequence<<",\"producerToken\":"<<lastFrameStatus.producerToken
                    <<",\"sdkPresentId\":"<<lastFrameStatus.sdkPresentId<<",\"compute\":"<<(lastFrameStatus.depthCompute?"true":"false")
                    <<",\"scope\":\""<<(lastFrameStatus.depthCompute?"depth_dispatch":"depth_two_transfers")<<"\",\"beginTicks\":\""<<values[0]<<"\",\"endTicks\":\""<<values[2]
                    <<"\",\"beginAvailability\":"<<values[1]<<",\"endAvailability\":"<<values[3]<<",\"queryResult\":"<<result
                    <<",\"validBits\":"<<sample.validBits<<",\"timestampPeriodNs\":"<<sample.timestampPeriodNs<<",\"durationNs\":"<<sample.durationNs
                    <<",\"observedAfterRealFence\":true,\"destinationReadbackExcluded\":true,\"followingGameDispatchExcluded\":true}\n";
            }
        }
    }
    void readCanvas(VkExtent2D extent,VkRect2D region,bool auxiliary=false) {
        if(deferredRead>=0)submitCanvasRead(asyncReads[deferredRead],extent,region,auxiliary);
        else {CanvasRead read{readCommand,readFence};submitCanvasRead(read,extent,region,auxiliary);verifyCanvasRead(read);}
    }
    void runFrame(const char* phase,bool enabled,bool supplyBundle,bool expectLow,bool expectHeld=false,bool verifyPixels=true) {
        inputReset=trace.forceK1Reset&&!std::strcmp(phase,"gated_retire_K1");
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        require(IsWindowVisible(window)&&!IsIconic(window),"fixture window unavailable");
        Sleep(16);++sequence;trace.frame();
        if(trace.asyncCopy) {
            const auto& producer=asyncProducers[(sequence-1)%asyncProducers.size()];
            command=producer.command;completion=producer.fence;ready=producer.ready;
        }
        if(menu.requestedEnabled!=enabled)++menu.controlEpoch;
        menu.rawAlgo=xess_vk::fsr_menu_control::fsr3FrameInterpolation;menu.rawEnable=enabled?1:0;menu.requestedEnabled=enabled;
        menu.bothGatesReady=menu.nativeSuppressed=menu.inputObserved=menu.paired=menu.gatesConsistent=true;
        currentTicket=runtime->beginGameFrame();
        const auto bootstrap=currentTicket?0u:1u;
        require(currentTicket||sequence==1,"production timing ticket missing after bootstrap");
        if(expectLow)require(currentTicket.latencyEnabled,"low-resolution ON ticket has no actual latency admission");
        captureCurrent=supplyBundle&&bool(currentTicket);
        runtime->simulationEnd(currentTicket);runtime->renderSubmitBegin(currentTicket);
        if(!preparedImages.empty()) {
            imageIndex=preparedImages.front();preparedImages.erase(preparedImages.begin());
        } else if(nextImageAcquired) {
            imageIndex=nextImageIndex; nextImageAcquired=false;
        } else {
        check(dev<PFN_vkResetFences>("vkResetFences")(device, 1, &acquireFence),
              "reset acquire fence");
        VkResult acquired;
        xess_vk::wsi_acquire::Call acquireCall;
        xess_vk::presentation::testing::acquireSequence=sequence;
        RECT clientBefore{};const bool clientBeforeKnown=GetClientRect(window,&clientBefore)!=0;
        if (trace.fake) {
            const bool semaphoreOnly = (sequence % 2) == 0;
            acquired =
                runtime->acquire(chain, timeout, semaphoreOnly ? acquireSemaphore : VK_NULL_HANDLE,
                                 semaphoreOnly ? VK_NULL_HANDLE : acquireFence, &imageIndex,&acquireCall);
            if (semaphoreOnly && accepted(acquired)) {
                ++trace.semaphoreAcquires;
                const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                wait.waitSemaphoreCount = 1;
                wait.pWaitSemaphores = &acquireSemaphore;
                wait.pWaitDstStageMask = &stage;
                VkResult submitted;
                {
                    std::lock_guard lock(queueGate);
                    submitted =
                        dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &wait, acquireFence);
                }
                check(submitted, "real queue consumes fake acquire semaphore");
            }
        } else {
            ++trace.driverAcquires;
            acquired = dev<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR")(
                device, chain, timeout, VK_NULL_HANDLE, acquireFence, &imageIndex);
        }
        if(!accepted(acquired)) {
            RECT clientAfter{};const bool clientAfterKnown=GetClientRect(window,&clientAfter)!=0;
            const auto runtimeStatus=trace.fake?runtime->status(chain):VK_ERROR_FEATURE_NOT_PRESENT;
            std::cout<<"{\"event\":\"native_acquire_failure\",\"phase\":\""<<phase<<"\",\"sequence\":"<<sequence
                <<",\"result\":"<<static_cast<std::int32_t>(acquired)<<",\"fake\":"<<(trace.fake?"true":"false")
                <<",\"runtimeStatus\":"<<static_cast<std::int32_t>(runtimeStatus)<<",\"runtimeFailed\":"<<(runtime->failed()?"true":"false")
                <<",\"runtimeStage\":\""<<runtime->stage()<<"\",\"poolOwned\":"<<(runtime->ownsFakeChain(chain)?"true":"false")
                <<",\"generation\":"<<acquireCall.generation<<",\"signalSubmitObserved\":"<<(acquireCall.signal.entryNs?"true":"false")
                <<",\"signalSubmitResult\":"<<acquireCall.signal.result<<",\"physicalResizeChecks\":"<<physicalResizeChecks
                <<",\"expectedExtent\":["<<currentSourceExtent.width<<','<<currentSourceExtent.height<<"]"
                <<",\"clientBeforeKnown\":"<<(clientBeforeKnown?"true":"false")<<",\"clientBefore\":["<<clientBefore.right-clientBefore.left<<','<<clientBefore.bottom-clientBefore.top<<"]"
                <<",\"clientAfterKnown\":"<<(clientAfterKnown?"true":"false")<<",\"clientAfter\":["<<clientAfter.right-clientAfter.left<<','<<clientAfter.bottom-clientAfter.top<<"]"
                <<",\"visible\":"<<(IsWindowVisible(window)?"true":"false")<<",\"iconic\":"<<(IsIconic(window)?"true":"false")<<"}\n";
            throw std::runtime_error(std::string(trace.fake?"fake":"real WSI")+" acquire failed result="+std::to_string(acquired)+
                " runtime_status="+std::to_string(runtimeStatus));
        }
        check(
            dev<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &acquireFence, VK_TRUE, timeout),
            "real acquire fence completion");
        }
        if(copyGate&&!copyGate->inserted&&!trace.asyncCopy) {
            // Acquire itself queues a signal; reserve K+1 before inserting the
            // gate so this fixture can isolate Runtime's next-copy retirement.
            check(dev<PFN_vkResetFences>("vkResetFences")(device,1,&acquireFence),"reset next preacquire fence");
            require(accepted(runtime->acquire(chain,timeout,VK_NULL_HANDLE,acquireFence,&nextImageIndex)),"preacquire next fake image");
            check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&acquireFence,VK_TRUE,timeout),"complete next preacquire signal");
            nextImageAcquired=true;
        }
        std::array<VkClearColorValue, 3> values{};
        const auto color=expectedColor(sequence);
        for(unsigned n=0;n<4;++n)values[0].float32[n]=color[n]/255.0f;
        values[1].float32[0]=expectedDepth(sequence);
        if(trace.asyncCopy||trace.sharedCopy) {const auto motion=expectedMotion(sequence);values[2].float32[0]=motion[0];values[2].float32[1]=motion[1];}
        renderClear(
            values,
            [](void *user, VkCommandBuffer cb) { static_cast<Owner *>(user)->recordProducer(cb); },
            this, ready);
        if (captureCurrent)
            provider->submitted({bits(command), sequence}, std::span<const VkSemaphore>(&ready, 1),
                                queue, sequence, VK_SUCCESS);
        runtime->renderSubmitEnd(currentTicket);runtime->associatePresentTiming(sequence,currentTicket,bootstrap);
        VkResult perChain=VK_NOT_READY;VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount=1;present.pWaitSemaphores=&ready;present.swapchainCount=1;present.pSwapchains=&chain;
        present.pImageIndices=&imageIndex;present.pResults=&perChain;
        const auto result=runtime->present(queue,&present);
        if(copyGate&&copyGate->inserted&&!copyGate->returnObserved) {
            LARGE_INTEGER now{}; QueryPerformanceCounter(&now); copyGate->presentReturnQpc=now.QuadPart;
            copyGate->returnedBeforeRelease=!copyGate->released.load(); copyGate->returnObserved=true;
        }
        require(accepted(result)&&accepted(perChain)&&!runtime->failed(),runtime->error());presented[imageIndex]=true;
        require(runtime->lastSdkOutputSucceeded(),"actual SDK output failed");
        const auto status=runtime->frameStatus();
        lastFrameStatus=status;
        if(trace.asyncCopy&&trace.gpuTimestamps) {
            require(status.longFrameReset.copyToken!=0,"GPU identity diagnostic omitted this submitted copy token");
            copyIdentities[status.longFrameReset.copyToken]={sequence,currentTicket.sdkId};
        }
        require(status.asyncCopyPath==trace.asyncCopy,"fixture async opt-in did not reach the real Native backend");
        require(status.sharedCopyPath==trace.sharedCopy,"fixture shared opt-in did not reach the real Native backend");
        if(trace.sharedCopy&&captureCurrent) {
            if(status.depthCompute) {
                require(trace.depthDispatches==1&&!trace.depthImageToBuffer&&!trace.depthBufferToImage,
                    "Native compute evidence did not replace both actual depth transfers");++depthComputeFrames;
            } else {
                require(!trace.depthDispatches&&trace.depthImageToBuffer==1&&trace.depthBufferToImage==1,
                    "depth fallback did not retain the actual image-buffer-image snapshot");++depthTransferFrames;
            }
        }
        if(status.sharedColorDirect)++sharedColorFrames;
        if(status.sharedAuxiliaryDirect)++sharedAuxiliaryFrames;
        if(status.sharedPlaneKey.valid()) {
            auto& previous=sharedLeaseGenerations[{status.sharedPlaneKey.ringGeneration,status.sharedPlaneKey.index}];
            if(previous) {require(status.sharedPlaneKey.leaseGeneration>previous,"shared slot wrap reused an old lease generation");++sharedWraps;}
            previous=status.sharedPlaneKey.leaseGeneration;
            if(!status.sharedColorDirect)++sharedSourceFrames;
        }
        require(status.inflightLimit==(trace.asyncCopy?xess_vk::presentation::testing::asyncFrameLimit:0u)&&
                status.inflightFrames<=status.inflightLimit,"fixture effective capacity differs from its selected route");
        if(trace.syncShared)require(!status.inflightFrames&&!status.inflightHighWater&&!status.inflightCapWaitCpu.count,
            "synchronous shared must never enter async admission or capacity waiting");
        if(trace.asyncCopy||trace.sharedCopy) {
            require(status.engineFrame==sequence&&status.sdkPresentId==(currentTicket?currentTicket.sdkId:bootstrap),"async output changed the submitted engine frame or SDK ticket");
            if(enabled&&supplyBundle)require(status.inputMatched&&status.producerToken==sequence&&status.recordingGeneration==sequence&&status.submission==sequence,
                "async color output and Provider constants belong to different submissions");
        }
        checkGpuTimestamp(status.vkPrivateCopyGpu, 0, status.sdkPresentId);
        checkGpuTimestamp(status.vkInteropCopyGpu, 1, status.sdkPresentId);
        checkGpuTimestamp(status.d3dCallerCopyTagGpu, 2, status.sdkPresentId);
        const bool lowCanvas=expectLow||expectHeld;
        const auto target=lowCanvas?lowExtent:currentSourceExtent;const auto region=lowCanvas?lowRegion:currentSourceRegion;
        require(status.lowResOutputRequested&&status.lowResOutputActive==expectLow&&status.lowResOutputHeld==expectHeld,
            "actual low-resolution state mismatch");
        require(same(status.xefgSourceExtent,currentSourceExtent)&&same(status.xefgOutputExtent,target),"actual source/private extent mismatch");
        require(status.outputWidth==currentSourceExtent.width&&status.outputHeight==currentSourceExtent.height,"physical output extent differs from the current fake generation");
        require(trace.valid&&trace.native(trace.source)&&trace.stable!=VK_NULL_HANDLE,"production native copy trace invalid");
        const auto created=trace.created.find(trace.stable);require(created!=trace.created.end(),"stable creation not observed");
        require(created->second.extent.width==target.width&&created->second.extent.height==target.height&&created->second.extent.depth==1,
            "real stable image creation extent differs from output");
        if(previousStable){
            if(same(previousCanvas,target)) {if(!trace.sharedCopy)require(previousStable==trace.stable,"unchanged canvas recreated stable image");}
            else{require(previousStable!=trace.stable,"changed canvas reused the old stable image");++canvasTransitions;}
        }
        previousStable=trace.stable;previousCanvas=target;
        if(lowCanvas){
            const auto& b=trace.blit;
            require(trace.blits==1&&trace.copies==0&&trace.filter==VK_FILTER_LINEAR&&
                b.srcOffsets[0].x==0&&b.srcOffsets[0].y==0&&b.srcOffsets[0].z==0&&
                b.srcOffsets[1].x==static_cast<int>(currentSourceExtent.width)&&b.srcOffsets[1].y==static_cast<int>(currentSourceExtent.height)&&b.srcOffsets[1].z==1&&
                b.dstOffsets[0].x==0&&b.dstOffsets[0].y==0&&b.dstOffsets[0].z==0&&
                b.dstOffsets[1].x==640&&b.dstOffsets[1].y==400&&b.dstOffsets[1].z==1,
                "production low path did not issue one full-domain LINEAR blit");
            if(expectLow){
                require(same(status.xefgRegion,lowRegion)&&status.sdkEnabled&&status.inputMatched&&status.queuedOn&&
                    !status.lowResOutputFallbackReason,"low-resolution SDK ON admission failed");++lowFrames;
            }else ++heldFrames;
        }else{
            require(trace.blits==0&&trace.copies==1&&trace.copied.width==currentSourceExtent.width&&trace.copied.height==currentSourceExtent.height,
                "full-resolution fallback changed the original RGBA8 copy");++fullFrames;
        }
        require(status.sdkStatusValid&&status.framesPresented>0&&!status.outputOccluded,"actual SDK status invalid, empty or output occluded");
        if(enabled&&supplyBundle){require(status.sdkEnabled,"valid ON frame did not enable SDK");++enabledFrames;}
        else require(!status.sdkEnabled,"OFF/missing-input frame enabled SDK");
        if(enabled&&!supplyBundle){
            require(status.lowResOutputFallbackReason&&!std::strcmp(status.lowResOutputFallbackReason,"frame_inputs_missing"),
                "missing bundle did not report its actual fallback reason");++missingFrames;
        }
        if(trace.maskedLinear&&enabled&&supplyBundle&&status.queuedOn)
            require(status.lowResOutputFallbackReason&&!std::strcmp(status.lowResOutputFallbackReason,"low_res_linear_blit_unsupported"),
                "masked LINEAR support did not reach the capability fallback");
        if(status.sdkStatusValid&&status.sdkEnabled&&enabled&&supplyBundle&&status.framesPresented>=2)++doublePresentOnSamples;
        if(verifyPixels) readCanvas(target,region,(trace.asyncCopy||trace.sharedCopy)&&enabled&&supplyBundle);
        std::cout<<"{\"event\":\"native_lowres_frame\",\"phase\":\""<<phase<<"\",\"sequence\":"<<sequence
            <<",\"source\":"<<bits(trace.source)<<",\"stable\":"<<bits(trace.stable)
            <<",\"sourceExtent\":["<<currentSourceExtent.width<<','<<currentSourceExtent.height<<"],\"stableExtent\":["<<target.width<<','<<target.height<<"]"
            <<",\"pixelActiveRegion\":["<<region.offset.x<<','<<region.offset.y<<','<<region.extent.width<<','<<region.extent.height<<"]"
            <<",\"xefgRegion\":["<<status.xefgRegion.offset.x<<','<<status.xefgRegion.offset.y<<','<<status.xefgRegion.extent.width<<','<<status.xefgRegion.extent.height<<"]"
            <<",\"linearBlits\":"<<trace.blits<<",\"fullCopies\":"<<trace.copies
            <<",\"blitSourceOffsets\":[["<<trace.blit.srcOffsets[0].x<<','<<trace.blit.srcOffsets[0].y<<','<<trace.blit.srcOffsets[0].z
            <<"],["<<trace.blit.srcOffsets[1].x<<','<<trace.blit.srcOffsets[1].y<<','<<trace.blit.srcOffsets[1].z<<"]]"
            <<",\"blitDestinationOffsets\":[["<<trace.blit.dstOffsets[0].x<<','<<trace.blit.dstOffsets[0].y<<','<<trace.blit.dstOffsets[0].z
            <<"],["<<trace.blit.dstOffsets[1].x<<','<<trace.blit.dstOffsets[1].y<<','<<trace.blit.dstOffsets[1].z<<"]]"
            <<",\"vkFilter\":"<<static_cast<unsigned>(trace.filter)<<",\"sdkRequestedEnabled\":"<<(status.requestedEnabled?"true":"false")
            <<",\"sdkEnabled\":"<<(status.sdkEnabled?"true":"false")<<",\"sdkFramesPresented\":"<<status.framesPresented
            <<",\"sdkFrameGenResult\":"<<status.frameGenResult<<",\"sdkQueryResult\":"<<status.queryResult
            <<",\"sdkConfiguredInterpolatedFrames\":"<<status.interpolatedFrames
            <<",\"asyncCopyPath\":"<<(status.asyncCopyPath?"true":"false")<<",\"inflightFrames\":"<<status.inflightFrames
            <<",\"sharedCopyPath\":"<<(status.sharedCopyPath?"true":"false")<<",\"sharedColorDirect\":"<<(status.sharedColorDirect?"true":"false")
            <<",\"sharedAuxiliaryDirect\":"<<(status.sharedAuxiliaryDirect?"true":"false")
            <<",\"depthCompute\":"<<(status.depthCompute?"true":"false")<<",\"depthStorageCapable\":"<<(status.sharedPlanes.depthStorage?"true":"false")
            <<",\"depthDispatches\":"<<trace.depthDispatches<<",\"depthImageToBuffer\":"<<trace.depthImageToBuffer<<",\"depthBufferToImage\":"<<trace.depthBufferToImage
            <<",\"followingGameDispatchVerified\":"<<(trace.sharedCopy&&captureCurrent&&verifyPixels&&deferredRead<0?"true":"false")
            <<",\"sharedRingGeneration\":"<<status.sharedPlaneKey.ringGeneration<<",\"sharedLeaseGeneration\":"<<status.sharedPlaneKey.leaseGeneration
            <<",\"sharedSlotIndex\":"<<status.sharedPlaneKey.index<<",\"sharedLeases\":"<<status.sharedPlanes.leases
            <<",\"sharedDirectFrames\":"<<status.sharedPlanes.directFrames<<",\"sharedLegacySourceFrames\":"<<status.sharedPlanes.legacySourceFrames
            <<",\"sharedReleased\":"<<status.sharedPlanes.released<<",\"sharedPoisoned\":"<<status.sharedPlanes.poisoned
            <<",\"sharedOracleWrites\":"<<trace.oracleMask<<",\"sharedAuxTargets\":["<<bits(trace.auxiliaryTargets[0])<<','<<bits(trace.auxiliaryTargets[1])<<']'
            <<",\"inflightLimit\":"<<status.inflightLimit
            <<",\"inflightCapWaits\":"<<status.inflightCapWaitCpu.count<<",\"producerToken\":"<<status.producerToken
            <<",\"fullCanvasPixelsVerified\":"<<(verifyPixels&&deferredRead<0?"true":"false")
            <<",\"pixelReadbackQueued\":"<<(verifyPixels&&deferredRead>=0?"true":"false")<<",\"fallback\":\""<<(status.lowResOutputFallbackReason?status.lowResOutputFallbackReason:"")<<'"';
        const auto& reset=status.longFrameReset;
        std::cout<<",\"historyReset\":{\"diagnostic\":"<<(reset.requested?"true":"false")
            <<",\"policyN\":"<<(reset.policyEnabled?1:0)<<",\"outputEntryQpc\":"<<reset.outputEntryQpcTicks
            <<",\"qpcFrequency\":"<<reset.qpcFrequency<<",\"intervalNs\":"<<reset.outputIntervalNs<<",\"thresholdNs\":"<<reset.thresholdNs
            <<",\"longFrame\":"<<(reset.longFrame?"true":"false")<<",\"historyResetRequested\":"<<(reset.resetRequested?"true":"false")
            <<",\"reasons\":"<<reset.resetReasons<<",\"extraRequested\":"<<(reset.extraRequested?"true":"false")
            <<",\"extraSucceeded\":"<<(reset.extraSucceeded?"true":"false")<<",\"armed\":"<<(reset.armed?"true":"false")
            <<",\"pendingBefore\":"<<(reset.pendingBefore?"true":"false")<<",\"pendingAfter\":"<<(reset.pendingAfter?"true":"false")
            <<",\"copyToken\":"<<reset.copyToken<<",\"copyGeneration\":"<<reset.copyGeneration
            <<",\"engineFrame\":"<<reset.engineFrame<<",\"sdkPresentId\":"<<reset.sdkPresentId
            <<",\"triggerEngineFrame\":"<<reset.triggerBefore.engineFrame<<",\"triggerSdkPresentId\":"<<reset.triggerBefore.sdkPresentId
            <<",\"triggerCopyToken\":"<<reset.triggerBefore.copyToken<<'}';
        std::cout<<",\"cpuTotalNs\":{\"runtimeCopy\":"<<status.runtimeCopyCpu.totalNs<<",\"output\":"<<status.outputCpu.totalNs
            <<",\"copyFenceWait\":"<<status.copyFenceWaitCpu.totalNs<<",\"copyConsumedWait\":"<<status.copyConsumedWaitCpu.totalNs
            <<",\"previousD3DWait\":"<<status.previousD3DWaitCpu.totalNs<<",\"bridgePublish\":"<<status.bridgePublishCpu.totalNs
            <<",\"dxgiPresent\":"<<status.dxgiPresentCpu.totalNs<<",\"xellSleep\":"<<status.xellSleepCpu.totalNs<<'}';
        const auto gpuSample = [](const char* scope, const xess_vk::GpuTimestampSample& sample) {
            std::cout << ",\"" << scope << "\":{\"requested\":" << (sample.requested ? "true" : "false")
                << ",\"available\":" << (sample.available ? "true" : "false") << ",\"sequence\":" << sample.sequence
                << ",\"contextGeneration\":" << sample.contextGeneration << ",\"generation\":" << sample.generation
                <<",\"sourceEngineFrameKnown\":"<<(sample.sourceEngineFrameKnown?"true":"false")<<",\"sourceEngineFrame\":"<<sample.sourceEngineFrame
                <<",\"sourceCopyTokenKnown\":"<<(sample.sourceCopyTokenKnown?"true":"false")<<",\"sourceCopyToken\":"<<sample.sourceCopyToken
                <<",\"queryStateKnown\":"<<(sample.queryStateKnown?"true":"false")<<",\"diagnosticActive\":"<<(sample.diagnosticActive?"true":"false")
                <<",\"queryReadAttempted\":"<<(sample.queryReadAttempted?"true":"false")<<",\"queryResultKnown\":"<<(sample.queryResultKnown?"true":"false")
                <<",\"queryResult\":"<<sample.queryResult<<",\"queryAvailabilityKnown\":"<<(sample.queryAvailabilityKnown?"true":"false")
                <<",\"beginAvailability\":"<<sample.beginAvailability<<",\"endAvailability\":"<<sample.endAvailability
                << ",\"sdkPresentIdKnown\":" << (sample.sdkPresentIdKnown ? "true" : "false")
                << ",\"sdkPresentId\":" << sample.sdkPresentId << ",\"beginTicks\":" << sample.beginTicks
                << ",\"endTicks\":" << sample.endTicks << ",\"validBits\":" << sample.validBits
                << ",\"frequencyHz\":" << sample.frequencyHz << ",\"timestampPeriodNs\":" << sample.timestampPeriodNs
                << ",\"durationKnown\":" << (sample.durationKnown ? "true" : "false") << ",\"durationNs\":" << sample.durationNs
                << ",\"submitQpcTicks\":" << sample.submitQpcTicks << ",\"fenceObservedQpcTicks\":" << sample.fenceObservedQpcTicks
                << ",\"qpcFrequency\":" << sample.qpcFrequency
                << ",\"calibrationAvailable\":" << (sample.calibration.available ? "true" : "false")
                << ",\"calibrationGpuTicks\":" << sample.calibration.gpuTicks << ",\"calibrationQpcTicks\":" << sample.calibration.qpcTicks
                << ",\"calibrationQpcFrequency\":" << sample.calibration.qpcFrequency
                << ",\"calibrationCaptureQpcTicks\":" << sample.calibration.captureQpcTicks
                << ",\"calibrationMaxDeviationKnown\":" << (sample.calibration.maxDeviationKnown ? "true" : "false") << '}';
        };
        gpuSample("private_copy_command_buffer", status.vkPrivateCopyGpu);
        gpuSample("interop_copy_command_buffer", status.vkInteropCopyGpu);
        gpuSample("caller_copy_tag_command_list", status.d3dCallerCopyTagGpu);
        std::cout << "}\n";
        require(!validation.errors,"validation reported an error during the real owned frame");
    }
    void runOnPhase(const char* phase,unsigned frames,unsigned index) {
        const auto before=doublePresentOnSamples;
        for(unsigned n=0;n<frames;++n)runFrame(phase,true,true,!trace.maskedLinear);
        phaseDoublePresentSamples[index]=doublePresentOnSamples-before;
        require(phaseDoublePresentSamples[index]>0,"continuous ON phase had no SDK status sample with framesPresented >= 2");
    }
    void sharedStableRecovery(const char* phase) {
        unsigned sdkDouble{},sharedDouble{},consecutive{},maximumConsecutive{};
        for(unsigned n=0;n<4;++n) {
            runFrame(phase,true,true,!trace.maskedLinear);
            const auto& status=lastFrameStatus;
            if(status.sdkStatusValid&&status.sdkEnabled&&status.queryResult==0&&status.frameGenResult==0&&
               status.framesPresented>=2&&!status.longFrameReset.resetRequested) {
                ++sdkDouble;if(status.sharedAuxiliaryDirect) {++sharedDouble;++consecutive;}
                else consecutive=0;
            } else consecutive=0;
            maximumConsecutive=std::max(maximumConsecutive,consecutive);
        }
        std::cout<<"{\"event\":\"shared_stable_recovery\",\"phase\":\""<<phase<<"\",\"fixedFrames\":4,\"sdkDoubleSamples\":"<<sdkDouble
            <<",\"sharedSdkDoubleSamples\":"<<sharedDouble<<",\"maximumConsecutiveSharedDoubleSamples\":"<<maximumConsecutive
            <<",\"noThresholdOrPacingChanges\":true}\n";
        require(maximumConsecutive>=2,"fixed recovery segment did not restore consecutive actual SDK double output on the direct shared path");
        ++sharedStableRecoveries;
    }
    void resizeFakeSource(VkExtent2D extent) {
        require(trace.fake&&(trace.asyncCopy?runtime->frameStatus().inflightFrames>0:
            trace.syncShared&&runtime->frameStatus().sharedPlanes.leases>0),"resize must start with an actual owned frame or shared reservation");
        RECT rect{0,0,static_cast<LONG>(extent.width),static_cast<LONG>(extent.height)};
        require(AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE)&&SetWindowPos(window,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,
            SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE),"resize only the self-owned application window");
        const auto previous=chain;chainInfo.oldSwapchain=previous;chainInfo.imageExtent=extent;
        check(runtime->create(&chainInfo,nullptr,&chain),"create resized fake image generation");
        require(chain!=previous&&runtime->ownsFakeChain(previous)&&runtime->ownsFakeChain(chain),"resize lost a live image generation");
        std::uint32_t count{};check(runtime->images(chain,&count,nullptr),"resized fake image count");trace.wsi.resize(count);
        check(runtime->images(chain,&count,trace.wsi.data()),"resized fake images");presented.assign(count,false);preparedImages.clear();
        currentSourceExtent=extent;currentSourceRegion={{0,static_cast<std::int32_t>(extent.height/20)},{extent.width,extent.height*9/10}};
        check(runtime->destroy(previous,nullptr),"retire resized fake source pool");++physicalResizeChecks;
    }
    void asyncCapacity() {
        for(unsigned mode=0;mode<3;++mode) {
            while(preparedImages.size()<3) {
                std::uint32_t index{};
                check(dev<PFN_vkResetFences>("vkResetFences")(device,1,&acquireFence),"reset preacquire signal fence");
                check(runtime->acquire(chain,timeout,VK_NULL_HANDLE,acquireFence,&index),"preacquire capacity image");
                check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&acquireFence,VK_TRUE,timeout),"complete preacquire signal");
                require(std::find(preparedImages.begin(),preparedImages.end(),index)==preparedImages.end(),"capacity images must be distinct");
                preparedImages.push_back(index);
            }
            CopyGate gate(device,pool,2000);copyGate=&gate;
            deferredRead=0;runFrame("async_pending_K",true,true,true);
            const auto first=lastFrameStatus;const auto firstImage=imageIndex;
            require(gate.inserted&&!gate.released&&dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,gate.privateFence)==VK_NOT_READY,
                "async capacity precondition not reproduced: SDK/Present returned after the real private-copy gate released");
            check(dev<PFN_vkResetFences>("vkResetFences")(device,1,&acquireFence),"reset pending acquire fence");
            const bool semaphore=mode!=1,fence=mode!=0;std::uint32_t acquired{};
            xess_vk::presentation::testing::acquireSequence=sequence;
            check(runtime->acquire(chain,0,semaphore?acquireSemaphore:VK_NULL_HANDLE,fence?acquireFence:VK_NULL_HANDLE,&acquired),"GPU acquire of exact pending image");
            require(acquired==firstImage&&!gate.released&&dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,gate.privateFence)==VK_NOT_READY,
                "pending-image acquire waited on the CPU or returned a different submitted image");
            if(fence)require(dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,acquireFence)==VK_NOT_READY,
                "acquire fence signalled before the exact queued private read");
            if(semaphore) {
                const auto consumedFence=fence?readFence:acquireFence;
                if(fence)check(dev<PFN_vkResetFences>("vkResetFences")(device,1,&readFence),"reset combined acquire consumer fence");
                const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkSubmitInfo consume{VK_STRUCTURE_TYPE_SUBMIT_INFO};consume.waitSemaphoreCount=1;consume.pWaitSemaphores=&acquireSemaphore;consume.pWaitDstStageMask=&stage;
                {std::lock_guard lock(queueGate);check(dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&consume,consumedFence),"queue real acquire semaphore consumer");}
                require(dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,consumedFence)==VK_NOT_READY,"acquire semaphore consumer bypassed the pending private read");
            }
            preparedImages.push_back(acquired);
            deferredRead=1;runFrame("async_pending_K1",true,true,true);
            const auto second=lastFrameStatus;
            require(!gate.released&&first.inflightFrames==1&&second.inflightFrames==2&&second.inflightHighWater==2&&
                first.inflightCapWaitCpu.count==second.inflightCapWaitCpu.count&&
                first.copyFenceWaitCpu.count==second.copyFenceWaitCpu.count&&first.copyConsumedWaitCpu.count==second.copyConsumedWaitCpu.count&&
                dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,trace.privateFence)==VK_NOT_READY,
                "two genuinely pending frame slots did not return without a per-frame host wait");
            deferredRead=2;runFrame("async_capacity_K2",true,true,true);
            const auto third=lastFrameStatus;
            require(gate.released&&third.inflightCapWaitCpu.count==second.inflightCapWaitCpu.count+1&&
                third.inflightCapWaitCpu.totalNs>second.inflightCapWaitCpu.totalNs&&third.inflightHighWater==2,
                "third frame did not count the bounded matching post-Present capacity wait");
            gate.finish();require(gate.completed&&gate.releaseResult==VK_SUCCESS,"real capacity gate did not complete cleanly");
            check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&acquireFence,VK_TRUE,timeout),"pending acquire fence completes after source read");
            if(semaphore&&fence)check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&readFence,VK_TRUE,timeout),"combined acquire semaphore completes after source read");
            for(const auto& read:asyncReads)verifyCanvasRead(read);
            copyGate=nullptr;deferredRead=-1;++asyncCapacityCases;++asyncPendingAcquireCases;
            std::cout<<"{\"event\":\"async_capacity_result\",\"signalMode\":\""<<(mode==0?"semaphore":mode==1?"fence":"both")
                <<"\",\"gateMs\":2000,\"pendingFrames\":2,\"capacityWaits\":1,\"imageIndex\":"<<acquired
                <<",\"firstEngineFrame\":"<<first.engineFrame<<",\"thirdEngineFrame\":"<<third.engineFrame
                <<",\"frameIdentityVerified\":true,\"deferredPixelFrames\":3,\"colorDepthMotionPixelsVerified\":true,\"gameTouched\":false}\n";
        }
    }
    void run() {
        // Before the first admitted low frame there is no canvas to hold; after
        // it, menu OFF and missing-input frames keep the low canvas unresized.
        const bool hold=!trace.maskedLinear;
        for(unsigned n=0;n<4;++n)runFrame("warmup_off",false,false,false);
        runOnPhase("low_on",12,0);
        unsigned expectedCaptured=trace.asyncCapacity?21:trace.gatedCopy?19:24;
        if(trace.asyncCapacity) {
            asyncCapacity();
            require(enabledFrames==21&&pixelFrames==sequence&&auxiliaryPixelFrames==21&&asyncCapacityCases==3&&asyncPendingAcquireCases==3,
                "async capacity/pending-acquire/per-frame pixel coverage incomplete");
        } else if(trace.gatedCopy) {
            std::array<xess_vk::presentation::LongFrameResetTrace,6> recent;
            unsigned cadenceFrames{};
            bool cadenceStable{};
            std::uint64_t intervalMin{},intervalMax{},thresholdMin{},thresholdMax{};
            // The early pixel oracle raises the cadence EMA. Let real output
            // intervals settle without readback before injecting a late copy.
            for(;cadenceFrames<80;) {
                runFrame("gated_cadence_warmup",true,true,true,false,false);
                recent[cadenceFrames%recent.size()]=lastFrameStatus.longFrameReset;
                ++cadenceFrames;
                if(cadenceFrames<40) continue;
                intervalMin=thresholdMin=UINT64_MAX; intervalMax=thresholdMax=0;
                bool clean=true;
                for(const auto& d:recent) {
                    intervalMin=std::min(intervalMin,d.outputIntervalNs); intervalMax=std::max(intervalMax,d.outputIntervalNs);
                    thresholdMin=std::min(thresholdMin,d.thresholdNs); thresholdMax=std::max(thresholdMax,d.thresholdNs);
                    clean&=d.intervalKnown&&!d.longFrame&&!d.extraRequested&&!d.pendingAfter&&d.outputIntervalNs<d.thresholdNs;
                }
                cadenceStable=clean&&cadenceHasHeadroom(trace.gateDelayMs,intervalMax,thresholdMax)&&
                    thresholdMax-thresholdMin<=std::max<std::uint64_t>(4'000'000,thresholdMax/10);
                if(cadenceStable) break;
            }
            expectedCaptured+=cadenceFrames;
            std::cout<<"{\"event\":\"gated_cadence\",\"frames\":"<<cadenceFrames<<",\"recentFrames\":"<<recent.size()
                <<",\"stable\":"<<(cadenceStable?"true":"false")<<",\"intervalMinNs\":"<<intervalMin<<",\"intervalMaxNs\":"<<intervalMax
                <<",\"thresholdMinNs\":"<<thresholdMin<<",\"thresholdMaxNs\":"<<thresholdMax<<",\"readbacks\":0}\n";
            CopyGate gate(device,pool,trace.gateDelayMs); copyGate=&gate;
            runFrame("gated_submit_K",true,true,true,false,false);
            const auto token=lastFrameStatus.longFrameReset.copyToken;
            const auto noFollowup=[](const auto& d) { return !d.policyEnabled&&!d.pendingBefore&&!d.pendingAfter&&!d.extraRequested&&!d.extraSucceeded; };
            bool criticalFollowupInactive=noFollowup(lastFrameStatus.longFrameReset);
            runFrame("gated_retire_K1",true,true,true,false,false);
            const auto retired=lastFrameStatus.previousCopy;
            const auto k1=lastFrameStatus.longFrameReset;
            const bool k1LongFrame=k1.longFrame;
            const auto k1SdkFrames=lastFrameStatus.framesPresented;
            criticalFollowupInactive&=noFollowup(k1);
            unsigned extraSucceeded=lastFrameStatus.longFrameReset.extraSucceeded?1:0;
            bool k2ExtraRequested{},k2ExtraSucceeded{};
            for(unsigned n=0;n<5;++n) {
                constexpr const char* phases[]{"gated_successor_K2","gated_successor_K3","gated_successor_K4","gated_successor_K5","gated_successor_K6"};
                runFrame(trace.gateDelayMs==35?phases[n]:"gated_successor",true,true,true,false,false);
                criticalFollowupInactive&=noFollowup(lastFrameStatus.longFrameReset);
                if(n==0) { k2ExtraRequested=lastFrameStatus.longFrameReset.extraRequested; k2ExtraSucceeded=lastFrameStatus.longFrameReset.extraSucceeded; }
                extraSucceeded+=lastFrameStatus.longFrameReset.extraSucceeded?1:0;
            }
            gate.finish();
            require(gate.inserted&&gate.completed&&gate.releaseResult==VK_SUCCESS,"self-owned gate did not complete normally");
            readCanvas(lowExtent,lowRegion);
            LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
            const auto release=gate.releaseQpc.load();
            const bool reproduced=gate.returnedBeforeRelease&&gate.nextWaitEntryQpc&&gate.nextWaitEntryQpc<release&&
                gate.nextWaitReturnQpc>=release&&retired.sequence==token;
            const bool k1BelowThreshold=k1.intervalKnown&&k1.outputIntervalNs<k1.thresholdNs;
            const bool subthresholdQualified=trace.gateDelayMs==35&&cadenceStable&&reproduced&&k1BelowThreshold&&!k1LongFrame&&
                criticalFollowupInactive&&k1.resetRequested==trace.forceK1Reset&&
                k1.resetReasons==(trace.forceK1Reset?xess_vk::presentation::HistoryResetInput:0u);
            std::cout<<"{\"event\":\"gated_copy_result\",\"policyN\":"<<(xess_vk::presentation::testing::followupEnabled?1:0)
                <<",\"gateDelayMs\":"<<gate.delayMs<<",\"preAcquireNext\":true,\"criticalReadbacks\":0"
                <<",\"forceK1Reset\":"<<(trace.forceK1Reset?"true":"false")<<",\"criticalFollowupInactive\":"<<(criticalFollowupInactive?"true":"false")
                <<",\"cadenceWarmupFrames\":"<<cadenceFrames<<",\"cadenceStable\":"<<(cadenceStable?"true":"false")
                <<",\"preGateIntervalMinNs\":"<<intervalMin<<",\"preGateIntervalMaxNs\":"<<intervalMax
                <<",\"preGateThresholdMinNs\":"<<thresholdMin<<",\"preGateThresholdMaxNs\":"<<thresholdMax
                <<",\"kReturnedBeforeRelease\":"<<(gate.returnedBeforeRelease?"true":"false")<<",\"kPresentReturnQpc\":"<<gate.presentReturnQpc
                <<",\"gateReleaseQpc\":"<<release<<",\"k1WaitEntryQpc\":"<<gate.nextWaitEntryQpc<<",\"k1WaitReturnQpc\":"<<gate.nextWaitReturnQpc
                <<",\"qpcFrequency\":"<<frequency.QuadPart<<",\"kCopyToken\":"<<token<<",\"retiredCopyToken\":"<<retired.sequence
                <<",\"previousFenceWaitNs\":"<<(retired.waitReturnNs-retired.waitEntryNs)
                <<",\"privateQueryMatch\":"<<(retired.privateCopyQueryMatch?"true":"false")
                <<",\"privateGpuAvailable\":"<<(retired.privateCopyGpu.available?"true":"false")
                <<",\"privateGpuDurationNs\":"<<retired.privateCopyGpu.durationNs
                <<",\"reproducedPreviousFenceWait\":"<<(reproduced?"true":"false")<<",\"extraSucceeded\":"<<extraSucceeded
                <<",\"k1LongFrame\":"<<(k1LongFrame?"true":"false")<<",\"targetLongFrameQualified\":"<<(cadenceStable&&reproduced&&k1LongFrame?"true":"false")
                <<",\"k1IntervalNs\":"<<k1.outputIntervalNs<<",\"k1ThresholdNs\":"<<k1.thresholdNs<<",\"k1BelowThreshold\":"<<(k1BelowThreshold?"true":"false")
                <<",\"k1ResetReasons\":"<<k1.resetReasons<<",\"k1ResetRequested\":"<<(k1.resetRequested?"true":"false")<<",\"k1SdkFramesPresented\":"<<k1SdkFrames
                <<",\"subthresholdResetQualified\":"<<(subthresholdQualified?"true":"false")
                <<",\"k2ExtraRequested\":"<<(k2ExtraRequested?"true":"false")<<",\"k2ExtraSucceeded\":"<<(k2ExtraSucceeded?"true":"false")
                <<",\"gameTouched\":false}\n";
            copyGate=nullptr;
            require(enabledFrames==expectedCaptured&&lowFrames==expectedCaptured&&pixelFrames==17&&missingFrames==0,"gated sample coverage incomplete");
        } else {
        for(unsigned n=0;n<3;++n)runFrame("menu_off",false,false,false,hold);
        runOnPhase("on_recovery",6,1);
        runFrame("missing_bundle",true,false,false,hold);
        runOnPhase("bundle_recovery",6,2);
        require(enabledFrames==24&&missingFrames==1,"ON/fallback sequence coverage incomplete");
        require((trace.maskedLinear?lowFrames==0:lowFrames==24)&&heldFrames==(hold?4u:0u)&&pixelFrames==sequence,
            "low-resolution/held/pixel coverage incomplete");
        }
        require(canvasTransitions==(trace.maskedLinear?0u:1u),"held canvas still resized on OFF/missing-input frames");
        if(trace.sharedCopy&&!trace.maskedLinear) {
            fullLogicalOutput=true;runFrame("shared_canvas_legacy_source",true,true,false);++expectedCaptured;
            require(!lastFrameStatus.sharedColorDirect&&lastFrameStatus.sharedPlaneKey.valid()&&sharedSourceFrames,
                "real canvas mismatch did not pin the old shared auxiliary source to a legacy publication");
            fullLogicalOutput=false;runFrame("shared_canvas_recovery",true,true,true);++expectedCaptured;
            sharedStableRecovery("shared_canvas_stable");expectedCaptured+=4;
        }
        if((trace.asyncCopy||trace.syncShared)&&!trace.asyncCapacity) {
            resizeFakeSource({960,600});runFrame("resize_transition",true,true,false);runFrame("resize_recovery",true,true,!trace.maskedLinear);
            if(trace.sharedCopy){sharedStableRecovery("resize_stable");expectedCaptured+=4;}
            resizeFakeSource(sourceExtent);runFrame("restore_transition",true,true,false);runFrame("restore_recovery",true,true,!trace.maskedLinear);
            if(trace.sharedCopy){sharedStableRecovery("restore_stable");expectedCaptured+=4;}
            expectedCaptured+=4;
            require(physicalResizeChecks==2&&pixelFrames==sequence&&auxiliaryPixelFrames==expectedCaptured,
                "async resize/recovery did not verify all color and Provider auxiliary pixels");
        }
        if(trace.sharedCopy)require(sharedColorFrames>4&&sharedAuxiliaryFrames>4&&sharedWraps>4&&
                !runtime->frameStatus().sharedPlanes.poisoned,"real shared direct ON/OFF and exact lease-generation wrap coverage incomplete");
        if(trace.sharedCopy)require(computeRestoreChecks==expectedCaptured&&depthComputeFrames+depthTransferFrames==expectedCaptured&&
                (trace.maskedDepthStorage||!trace.depthCompute?depthComputeFrames==0:depthComputeFrames>4),
                "depth compute/capability fallback or following game-dispatch restore oracle coverage incomplete");
        if(trace.sharedCopy&&trace.gpuTimestamps)require(depthGpuSamples==expectedCaptured,"actual depth GPU query coverage differs from this fixture's captures");
        if (trace.gpuTimestamps)
            require(vkGpuSamples > 1 && interopGpuSamples > 1 && d3dGpuSamples > 1, "Opt-in fixture requires repeated private/Interop/D3D GPU samples");
        const auto countFailure=[&](const char* stage,const cp::Stats& stats) {
            std::cout<<"{\"event\":\"provider_count_mismatch\",\"stage\":\""<<stage<<"\",\"expectedCaptured\":"<<expectedCaptured
                <<",\"captured\":"<<stats.captured<<",\"consumed\":"<<stats.consumed<<",\"cancelled\":"<<stats.cancelled
                <<",\"presentMatches\":"<<stats.presentMatches<<",\"sharedCaptured\":"<<stats.sharedCaptures
                <<",\"sharedFallbacks\":"<<stats.sharedFallbacks<<",\"sharedFailures\":"<<stats.sharedFailures<<",\"privateStates\":[";
            for(unsigned i=0;i<stats.states.size();++i){if(i)std::cout<<',';std::cout<<static_cast<unsigned>(stats.states[i]);}
            std::cout<<"],\"sharedStates\":[";
            for(unsigned i=0;i<stats.sharedStates.size();++i){if(i)std::cout<<',';std::cout<<static_cast<unsigned>(stats.sharedStates[i]);}
            std::cout<<"],\"lastSequence\":"<<sequence<<",\"lastProducerToken\":"<<lastFrameStatus.producerToken
                <<",\"lastSharedLease\":"<<lastFrameStatus.sharedPlaneKey.leaseGeneration<<"}\n";
        };
        if(trace.sharedCopy) {
            const auto before=provider->stats();
            const auto owned=[](const auto& states) {return std::count(states.begin(),states.end(),cp::State::Consuming);};
            const auto unowned=[](const auto& states) {return std::any_of(states.begin(),states.end(),[](auto state){return state!=cp::State::Free&&state!=cp::State::Consuming;});};
            const bool accounted=before.captured==expectedCaptured&&before.consumed+owned(before.states)+owned(before.sharedStates)==before.captured&&
                !unowned(before.states)&&!unowned(before.sharedStates);
            if(!accounted)countFailure("before_native_drain",before);
            require(accounted,"shared fixture has an unpresented Provider capture before Native shutdown");
        }
        // Every remaining shared input is already pinned by Native. Let it
        // record matching consumption before Provider::stop clears private tickets.
        const auto stopped=runtime->stop();
        const auto shutdown=runtime->shutdownState();
        if(trace.fake)require(stopped==VK_SUCCESS&&!shutdown.nativeTailRetained&&runtime->retainedChains()==0,"fake stop must really destroy its image pools without native-tail retention");
        require(stopped==VK_SUCCESS||(stopped==VK_NOT_READY&&shutdown.nativeTailRetained),"production Runtime stop failed");
        require(shutdown.privateCleanupCompleted&&shutdown.sdkReleased&&shutdown.bridgeReleased&&!shutdown.failedQuarantined,
            "production private SDK/bridge cleanup failed");
        check(dev<PFN_vkResetFences>("vkResetFences")(device,1,&readFence),"reset final queue drain fence");
        VkSubmitInfo tail{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        check(dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&tail,readFence),"submit final owned queue drain");
        check(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&readFence,VK_TRUE,timeout),"complete final owned queue drain");
        check(provider->stop(readFence),"Provider stop after actual queue drain fence");
        const auto stats=provider->stats();
        if(stats.captured!=expectedCaptured||stats.consumed!=expectedCaptured)countFailure("after_native_and_provider_stop",stats);
        require(stats.captured==expectedCaptured&&stats.consumed==expectedCaptured,"actual Provider capture/bridge consumption count mismatch");
        if(trace.gpuTimestamps)require(stats.timestampCompleted==expectedCaptured&&!stats.timestampFailures&&!stats.timestampUnavailable,
            "production Provider depth/aux query coverage or completed availability mismatch");
        if(trace.sharedCopy)require(stats.depthComputeCaptures==depthComputeFrames&&stats.depthTransferCaptures==depthTransferFrames,
            "per-frame depth path evidence differs from actual Provider compute/transfer captures");
        auto copySummary=std::make_unique<xess_vk::measurement::SummarySnapshot>();
        require(xess_vk::measurement::active&&xess_vk::measurement::active->takeSummary(xess_vk::cpuWallNowNs(),*copySummary,true),
            "actual submitted-command measurement summary missing");
        const auto copies=[&](xess_vk::measurement::CopyRole role) {
            std::uint64_t sum{};const auto& commands=copySummary->copyCommands[static_cast<unsigned>(role)];
            for(unsigned n=0;n<commands.size();++n)if(n!=static_cast<unsigned>(xess_vk::measurement::Command::DepthDispatch))sum+=commands[n];
            return sum;
        };
        const auto colorCopies=copies(xess_vk::measurement::CopyRole::Color);
        const auto depthCopies=copies(xess_vk::measurement::CopyRole::Depth);
        const auto motionCopies=copies(xess_vk::measurement::CopyRole::Motion);
        require(colorCopies==3ull*sequence-sharedColorFrames&&
            depthCopies==3ull*stats.captured-sharedAuxiliaryFrames-2ull*depthComputeFrames&&
            motionCopies==2ull*stats.captured-sharedAuxiliaryFrames,
            "actual successful submitted copies do not match the removed shared color/depth/motion bridge copies");
        std::cout<<"{\"event\":\"measurement_submitted_copies\",\"frames\":"<<sequence<<",\"providerCaptured\":"<<stats.captured
            <<",\"color\":"<<colorCopies<<",\"depth\":"<<depthCopies<<",\"motion\":"<<motionCopies
            <<",\"total\":"<<colorCopies+depthCopies+motionCopies<<",\"sharedColorFrames\":"<<sharedColorFrames
            <<",\"sharedAuxiliaryFrames\":"<<sharedAuxiliaryFrames<<",\"depthDispatches\":"<<depthComputeFrames
            <<",\"steadyDirectOnCopiesPerFrame\":"<<(trace.sharedCopy?(depthComputeFrames?3:5):8)
            <<",\"sdkInternalCopies\":null,\"testOracleCopiesExcluded\":true}\n";
        successfulStop=true;
        if(trace.fake)require(!trace.driverCreates&&!trace.driverImages&&!trace.driverAcquires&&!trace.driverPresents&&!trace.driverDestroys&&trace.semaphoreAcquires>0,"fake GPU path called a driver WSI entry or missed real semaphore acquisition");
        std::cout<<"{\"testPassed\":true,\"frames\":"<<sequence<<",\"lowFrames\":"<<lowFrames<<",\"heldFrames\":"<<heldFrames<<",\"fullFrames\":"<<fullFrames
            <<",\"sdkEnabledFrames\":"<<enabledFrames<<",\"sdkDoublePresentOnSamples\":"<<doublePresentOnSamples
            <<",\"phaseDoublePresentSamples\":{\"low_on\":"<<phaseDoublePresentSamples[0]
            <<",\"on_recovery\":"<<phaseDoublePresentSamples[1]<<",\"bundle_recovery\":"<<phaseDoublePresentSamples[2]<<'}'
            <<",\"sdkEvidenceScope\":\"per-present SDK status samples; not cumulative scanout totals\""
            <<",\"missingBundleFallbacks\":"<<missingFrames<<",\"pixelFrames\":"<<pixelFrames<<",\"verifiedPixels\":"<<pixelCount
            <<",\"canvasTransitions\":"<<canvasTransitions<<",\"providerCaptured\":"<<stats.captured<<",\"providerConsumed\":"<<stats.consumed
            <<",\"linearCapabilityMasked\":"<<(trace.maskedLinear?"true":"false")
            <<",\"depthStorageCapabilityMasked\":"<<(trace.maskedDepthStorage?"true":"false")
            <<",\"depthComputeFrames\":"<<depthComputeFrames<<",\"depthTransferFrames\":"<<depthTransferFrames<<",\"computeRestoreChecks\":"<<computeRestoreChecks
            <<",\"depthGpuSamples\":"<<depthGpuSamples<<",\"validationRequested\":"<<(trace.validationRequested?"true":"false")
            <<",\"validationLayerLoaded\":"<<(validation.loaded?"true":"false")<<",\"validationErrorsBeforeParentCleanup\":"<<validation.errors.load()
            <<",\"depthComputeFailures\":"<<stats.depthComputeFailures<<",\"depthComputeFallbacks\":"<<stats.depthComputeFallbacks
            <<",\"productionRuntime\":true,\"productionNativeBackend\":true,\"productionProvider\":true,\"actualSdk\":true"
            <<",\"privateCleanupCompleted\":true,\"nativeWsiTailRetained\":"<<(shutdown.nativeTailRetained?"true":"false")
            <<",\"fakeSwapchain\":"<<(trace.fake?"true":"false")<<",\"driverSwapchainCreates\":"<<trace.driverCreates<<",\"driverSwapchainImageQueries\":"<<trace.driverImages
            <<",\"providerTimestampCompleted\":"<<stats.timestampCompleted<<",\"providerTimestampFailures\":"<<stats.timestampFailures<<",\"providerTimestampUnavailable\":"<<stats.timestampUnavailable
            <<",\"asyncCopyPath\":"<<(trace.asyncCopy?"true":"false")<<",\"asyncCapacityCases\":"<<asyncCapacityCases
            <<",\"sharedCopyPath\":"<<(trace.sharedCopy?"true":"false")<<",\"sharedColorFrames\":"<<sharedColorFrames
            <<",\"sharedCopySync\":"<<(trace.syncShared?"true":"false")
            <<",\"sharedAuxiliaryFrames\":"<<sharedAuxiliaryFrames<<",\"sharedLegacySourceFrames\":"<<sharedSourceFrames<<",\"sharedWraps\":"<<sharedWraps
            <<",\"sharedStableRecoveries\":"<<sharedStableRecoveries
            <<",\"gpuScopesIncludeDestinationReadbackOracle\":"<<(trace.sharedCopy?"true":"false")
            <<",\"inflightLimit\":"<<(trace.asyncCopy?xess_vk::presentation::testing::asyncFrameLimit:0u)
            <<",\"inflightHighWater\":"<<lastFrameStatus.inflightHighWater<<",\"inflightCapWaits\":"<<lastFrameStatus.inflightCapWaitCpu.count
            <<",\"asyncPendingAcquireCases\":"<<asyncPendingAcquireCases<<",\"auxiliaryPixelFrames\":"<<auxiliaryPixelFrames
            <<",\"physicalResizeChecks\":"<<physicalResizeChecks
            <<",\"gpuTimestampsRequested\":"<<(trace.gpuTimestamps?"true":"false")<<",\"vkGpuSamples\":"<<vkGpuSamples<<",\"interopGpuSamples\":"<<interopGpuSamples<<",\"d3dGpuSamples\":"<<d3dGpuSamples
            <<",\"driverAcquires\":"<<trace.driverAcquires<<",\"driverPresents\":"<<trace.driverPresents<<",\"driverSwapchainDestroys\":"<<trace.driverDestroys<<",\"semaphoreOnlyAcquires\":"<<trace.semaphoreAcquires
            <<",\"nativeParentsRetainedUntilProcessExit\":"<<(trace.fake?"false":"true")<<",\"gameTouched\":false}\n";
    }
};
} // namespace fixture
namespace xess_vk::fsr_menu_control { ControlIntent snapshot() noexcept { return fixture::menu; } }
int main(int argc,char** argv) {
    try {
        std::filesystem::path sdk;
        bool selfTest{};
        for(int n=1;n<argc;++n){const std::string arg=argv[n];
            if(arg=="--sdk-bin"&&n+1<argc)sdk=std::filesystem::absolute(argv[++n]);
            else if(arg=="--self-test")selfTest=true;
            else if(arg=="--mask-linear")fixture::trace.maskedLinear=true;
            else if(arg=="--mask-depth-storage")fixture::trace.maskedDepthStorage=true;
            else if(arg=="--validation")fixture::trace.validationRequested=true;
            else if(arg=="--fake-swapchain")fixture::trace.fake=true;
            else if(arg=="--async-copy-path")fixture::trace.asyncCopy=xess_vk::presentation::testing::asyncEnabled=true;
            else if(arg=="--shared-copy-path")fixture::trace.sharedCopy=xess_vk::presentation::testing::sharedEnabled=true;
            else if(arg=="--shared-copy-sync") {
                fixture::trace.syncShared=xess_vk::presentation::testing::syncSharedEnabled=true;
                fixture::trace.sharedCopy=xess_vk::presentation::testing::sharedEnabled=true;
            }
            else if(arg=="--depth-compute")fixture::trace.depthCompute=true;
            else if(arg=="--async-copy-frames=1")xess_vk::presentation::testing::asyncFrameLimit=1;
            else if(arg=="--async-copy-frames=2")xess_vk::presentation::testing::asyncFrameLimit=2;
            else if(arg=="--async-capacity")fixture::trace.asyncCapacity=true;
            else if(arg=="--gpu-timestamps")fixture::trace.gpuTimestamps=true;
            else if(arg=="--followup-reset=0")xess_vk::presentation::testing::followupEnabled=false;
            else if(arg=="--followup-reset=1")xess_vk::presentation::testing::followupEnabled=true;
            else if(arg=="--gated-copy")fixture::trace.gatedCopy=true;
            else if(arg=="--force-k1-reset")fixture::trace.forceK1Reset=true;
            else if(arg=="--gate-ms"&&n+1<argc) {
                const std::string delay=argv[++n];
                fixture::require(delay=="35"||delay=="60"||delay=="100"||delay=="200","gate delay must be 35, 60, 100 or 200 ms");
                fixture::trace.gateDelayMs=static_cast<unsigned>(std::stoul(delay));
            }
            else throw std::invalid_argument("Usage: --self-test | --sdk-bin PATH [--validation] [--mask-linear|--mask-depth-storage] [--fake-swapchain] [--gpu-timestamps] [--async-copy-frames=1|2] [--shared-copy-sync | --async-copy-path [--shared-copy-path|--async-capacity]] [--gated-copy --gate-ms 35|60|100|200 --followup-reset=0|1 [--force-k1-reset]]");
        }
        if(selfTest) {
            fixture::require(!fixture::validationPassed(),"request alone never proves a validation layer was loaded");
            fixture::validation.loaded=true;fixture::require(fixture::validationPassed(),"a loaded error-free layer can pass");
            VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
            data.pMessageIdName="fixture_cpu";data.pMessage="intentional CPU-only callback test";
            fixture::validationMessage(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,0,&data,nullptr);
            fixture::require(fixture::validation.warnings==1&&fixture::validationPassed(),"warnings are counted independently from errors");
            fixture::validationMessage(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,0,&data,nullptr);
            fixture::require(fixture::validation.errors==1&&!fixture::validationPassed(),"one actual debug-utils error refuses validation success");
            fixture::validation.errors=0;fixture::validation.cleanupFailed=true;
            fixture::require(!fixture::validationPassed(),"an incomplete parent cleanup refuses validation success");
            std::cout<<"{\"passed\":true,\"validationCallbackAndFailureGate\":true,\"vulkanLoaded\":false,\"gpuTouched\":false,\"gameTouched\":false}\n";
            return 0;
        }
        fixture::require(!fixture::trace.asyncCopy||fixture::trace.fake,"async-copy-path requires fake swapchain");
        fixture::require(!fixture::trace.syncShared||(fixture::trace.fake&&!fixture::trace.asyncCopy&&!fixture::trace.gatedCopy&&
            !xess_vk::presentation::testing::followupEnabled),"shared-copy-sync requires fake swapchain and excludes async admission and P1 gates");
        fixture::require(!fixture::trace.sharedCopy||((fixture::trace.asyncCopy||fixture::trace.syncShared)&&!fixture::trace.asyncCapacity),
            "shared-copy-path requires async-copy-path or shared-copy-sync; the separate SDK backlog-cap experiment keeps its original oracle");
        fixture::require(!fixture::trace.depthCompute||fixture::trace.sharedCopy,"depth-compute requires shared-copy-path");
        fixture::require(!fixture::trace.maskedDepthStorage||(fixture::trace.sharedCopy&&!fixture::trace.maskedLinear),
            "depth storage capability-negative fixture requires only shared-copy-path");
        fixture::require(!fixture::trace.validationRequested||fixture::trace.fake,"validation checks require complete fake-chain parent cleanup");
        fixture::require(!fixture::trace.asyncCapacity||(fixture::trace.asyncCopy&&fixture::trace.gpuTimestamps&&!fixture::trace.maskedLinear&&
                xess_vk::presentation::testing::asyncFrameLimit==2),
            "async-capacity requires async-copy-path, GPU timestamps, real LINEAR support and a two-frame limit");
        fixture::require(!fixture::trace.asyncCopy||(!fixture::trace.gatedCopy&&!xess_vk::presentation::testing::followupEnabled),
            "P2 async fixture keeps the separate P1 gate/reset experiment disabled");
        fixture::require(!fixture::trace.gatedCopy||(fixture::trace.fake&&fixture::trace.gpuTimestamps&&!fixture::trace.maskedLinear),
            "gated experiment requires fake swapchain, GPU timestamps and real LINEAR support");
        fixture::require(!fixture::trace.gatedCopy||fixture::trace.gateDelayMs!=35||!xess_vk::presentation::testing::followupEnabled,
            "35 ms reset experiment requires FollowupReset=0");
        fixture::require(!fixture::trace.forceK1Reset||(fixture::trace.gatedCopy&&fixture::trace.gateDelayMs==35),
            "forced K1 input reset requires the gated 35 ms experiment");
        fixture::require(!sdk.empty()&&std::filesystem::exists(sdk/L"libxess_fg.dll"),"real XeFG SDK directory required");
        fixture::require(!GetModuleHandleW(L"xess-vulkan-observer.dll"),"fixture must not load the game observer");
        fixture::require(SetEnvironmentVariableW(L"XESS_SDK_BIN",sdk.c_str())&&SetEnvironmentVariableW(L"XESS_XEFG_LOW_RES_OUTPUT",L"1"),"fixture environment failed");
        fixture::require(SetEnvironmentVariableW(L"XESS_GPU_TIMESTAMPS",fixture::trace.gpuTimestamps?L"1":nullptr),"fixture GPU timestamp environment failed");
        SetProcessDPIAware();
        auto measurementWindow=std::make_unique<xess_vk::measurement::Window>(xess_vk::cpuWallNowNs());
        xess_vk::measurement::Scope measurementScope(measurementWindow.get());
        fixture::Watchdog watchdog;
        {fixture::Owner owner;owner.initialize();owner.run();}
        if(fixture::trace.gpuTimestamps){auto snapshot=std::make_unique<xess_vk::measurement::Snapshot>();
            fixture::require(measurementWindow->take(xess_vk::cpuWallNowNs(),*snapshot,true),"actual GPU measurement snapshot missing");
            const auto count=[&](xess_vk::measurement::Metric metric){const auto& value=snapshot->metrics[static_cast<unsigned>(metric)];fixture::require(!value.dropped,"actual GPU statistics overflow");return value.count;};
            fixture::require(count(xess_vk::measurement::Metric::NativeGpu)>0&&count(xess_vk::measurement::Metric::CallerGpu)>0&&
                count(xess_vk::measurement::Metric::ProviderDepthGpu)+count(xess_vk::measurement::Metric::ProviderDepthTransferGpu)>0,
                "cross-component timestamp completions did not reach the scoped collector");
            std::cout<<"{\"event\":\"measurement_gpu_scopes\",\"native\":"<<count(xess_vk::measurement::Metric::NativeGpu)
                <<",\"caller\":"<<count(xess_vk::measurement::Metric::CallerGpu)<<",\"providerAux\":"<<count(xess_vk::measurement::Metric::ProviderAuxGpu)<<"}\n";}
        if(fixture::trace.validationRequested) {
            std::cout<<"{\"event\":\"validation_cleanup\",\"layerActuallyLoaded\":"<<(fixture::validation.loaded?"true":"false")
                <<",\"errors\":"<<fixture::validation.errors.load()<<",\"warnings\":"<<fixture::validation.warnings.load()
                <<",\"parentCleanupCompleted\":"<<(fixture::validation.cleanupFailed?"false":"true")<<"}\n";
            fixture::require(fixture::validationPassed(),
                "real validation errors or incomplete parent cleanup fail this fixture");
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<"Native low-resolution GPU fixture: "<<e.what()<<'\n';return 1;}
}
