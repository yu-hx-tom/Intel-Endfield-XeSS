// Include the production layer so every call below crosses its real wrappers.
// The runtime and backend dispatch use own-process CPU stubs; no Vulkan loader,
// GPU, SDK initialization, or game process is created by this fixture. The
// capability-query case owns two small HWNDs only.
#include "vulkan_observer.cpp"
#include "presentation_runtime.hpp"
#include "presenter_frame_timing.hpp"
#include <algorithm>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <atomic>
#include <thread>

namespace presentation_layer_fixture {
namespace presentation = xess_vk::presentation;
unsigned checks{};
bool shutdownGateObserved{};
void require(bool value, const char* why) {
    ++checks;
    if (!value) throw std::runtime_error(why);
}
template<class T> T handle(std::uintptr_t value) { return reinterpret_cast<T>(value); }
int marker;
struct DispatchObject { void* key; } deviceObject{&marker}, queueObject{&marker}, instanceObject{&marker};
const auto device = reinterpret_cast<VkDevice>(&deviceObject);
const auto queue = reinterpret_cast<VkQueue>(&queueObject);
const auto instance = reinterpret_cast<VkInstance>(&instanceObject);
const auto surface = handle<VkSurfaceKHR>(0x101);

struct Trace {
    bool managed{}, fenceComplete{}, pending{};
    unsigned creates{}, imageCalls{}, presents{}, destroys{}, deviceDestroys{};
    unsigned surfaceDestroys{}, instanceDestroys{};
    unsigned prepares{}, copies{}, outputs{}, drains{}, backendDestroys{}, timingStops{};
    VkResult createResult{VK_SUCCESS}, imageResult{VK_SUCCESS};
    VkResult nativeResult{VK_SUCCESS}, individualResult{VK_SUCCESS};
    VkResult prepareResult{VK_SUCCESS}, copyResult{VK_SUCCESS}, outputResult{VK_SUCCESS}, drainResult{VK_SUCCESS};
    bool copySubmit{true}, copyUncertain{}, throwBeforeSubmit{}, throwAfterSubmit{}, untimed{};
    bool queuedOff{}, queuedOn{}, throwRetirement{};
    VkResult queuedFinishResult{VK_SUCCESS};
    unsigned queuedFinishes{}, healthyNativeWaits{};
    std::vector<std::uint64_t> retiredSemaphoreGenerations;
    bool retirementProof{true};
    bool nativeTailRetained{};
    bool nativeEverPresented{};
    presentation::ShutdownState shutdown;
    unsigned emergencyDestroys{}, admissionStops{}, tailSubmits{}, tailWaits{}, tailFenceDestroys{}, vendorDestroys{};
    unsigned captureHeld{}, discardedOutputs{};
    unsigned semaphoreCreates{},semaphoreDestroys{},appSubmits{},sparseSubmits{};
    VkResult appSubmitResult{VK_SUCCESS};
    std::uintptr_t nextSemaphore{0x501};
    const VkSemaphoreCreateInfo* semaphoreInfo{};
    const VkAllocationCallbacks* semaphoreAllocator{};
    std::map<VkSemaphore,bool> pendingWaits;
    VkSemaphore resignalDuringOutput{};
    VkResult tailWaitResult{VK_SUCCESS};
    presentation::CopyProgress copyProgress{presentation::CopyProgress::NotSubmitted};
    VkSemaphore completionSemaphore{}, nativeWait{};
    unsigned sdkId{};
    std::uint64_t timingGeneration{1};
    bool contextAppearsOnCopy{};
    xess_vk::FrameTimingTicket selectedTiming;
    std::uint32_t selectedBootstrap{};
    std::unique_ptr<xess_vk::detail::PresenterFrameTimings> timingTable;
    std::mutex timingTableMutex;
    unsigned timingReturns{};
    bool untimedUnavailable{};
    std::atomic<bool> blockOutput{}, outputEntered{}, releaseOutput{};
    std::atomic<bool> blockSnapshot{}, snapshotEntered{}, releaseSnapshot{};
    std::vector<std::uint64_t> generations;
    std::map<VkSemaphore, unsigned> privateConsumed;
    unsigned failImageCall{};
    std::uintptr_t nextChain{0x201};
    std::map<VkSwapchainKHR, std::array<VkImage, 2>> live;
    std::map<VkSemaphore, unsigned> consumed;
    std::vector<std::string> events;
    const VkSwapchainCreateInfoKHR* appCreate{};
    const VkPresentInfoKHR* appPresent{};
    const VkAllocationCallbacks* allocator{};
    VkSwapchainCreateInfoKHR nativeCreate{}, prepared{};
    VkFullScreenExclusiveEXT nativeExclusive{VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT};
    VkPresentInfoKHR nativePresent{};
    VkSwapchainKHR* swapchainOutput{};
    const std::uint32_t* imageCount{};
    VkImage* imageOutput{};
    VkImage copiedImage{};
    const VkPresentInfoKHR* copiedPresent{};
    VkSwapchainKHR destroyedChain{};
    const VkAllocationCallbacks* destroyedAllocator{};
};
Trace* trace{};
void unlocked() {
    require(TryAcquireSRWLockExclusive(&stateLock) != 0, "production stateLock released across backend/driver calls");
    ReleaseSRWLockExclusive(&stateLock);
}
void event(const char* name) { unlocked(); trace->events.emplace_back(name); }

VKAPI_ATTR VkResult VKAPI_CALL createDriver(VkDevice d, const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* out) {
    event("native-create"); ++trace->creates;
    require(d == device, "native create uses original device");
    trace->nativeCreate = *info; trace->swapchainOutput = out;
    if (info->pNext && info != trace->appCreate) {
        const auto* exclusive = static_cast<const VkSurfaceFullScreenExclusiveInfoEXT*>(info->pNext);
        require(exclusive->sType == VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
                "managed fullscreen metadata is a local explicit nonexclusive record");
        trace->nativeExclusive = exclusive->fullScreenExclusive;
    }
    if (info != trace->appCreate) {
        require(info != trace->appCreate && !allocator, "runtime owns a modified create record without application allocator");
        require(info->surface == surface && (info->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && info->clipped == VK_FALSE,
            "actual hidden WSI create enables transfer source and retains invisible pixels");
        require((info->oldSwapchain == trace->appCreate->oldSwapchain ||
                (!trace->appCreate->oldSwapchain && trace->live.count(info->oldSwapchain))) &&
                info->imageFormat == trace->appCreate->imageFormat,
            "native oldSwapchain and RGBA/BGRA format preserved");
    } else {
        require(info == trace->appCreate && allocator == trace->allocator, "off path forwards exact create/allocator pointers");
    }
    if (trace->createResult != VK_SUCCESS) return trace->createResult;
    *out = handle<VkSwapchainKHR>(trace->nextChain++);
    trace->live[*out] = {handle<VkImage>(bits(*out) * 16 + 1), handle<VkImage>(bits(*out) * 16 + 2)};
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL imagesDriver(VkDevice d, VkSwapchainKHR chain, std::uint32_t* count, VkImage* images) {
    event(images ? "native-image-data" : "native-image-count"); ++trace->imageCalls;
    require(d == device && trace->live.count(chain) == 1, "enumeration reads the actual native chain");
    trace->imageCount = count; trace->imageOutput = images;
    if (trace->failImageCall == trace->imageCalls) return trace->imageResult;
    const auto& source = trace->live.at(chain);
    if (!images) { *count = static_cast<std::uint32_t>(source.size()); return VK_SUCCESS; }
    const auto copied = std::min(*count, static_cast<std::uint32_t>(source.size()));
    std::copy_n(source.data(), copied, images); *count = copied;
    return copied == source.size() ? VK_SUCCESS : VK_INCOMPLETE;
}
VKAPI_ATTR VkResult VKAPI_CALL presentDriver(VkQueue q, const VkPresentInfoKHR* info) {
    event("native-present"); ++trace->presents;
    require(q == queue, "native present uses original queue"); trace->nativePresent = *info;
    if (info != trace->appPresent) {
        require(trace->copyProgress != presentation::CopyProgress::NotSubmitted,
            "modified native packet has an accepted private submission");
        if (trace->copyProgress == presentation::CopyProgress::Completed) {
            require(info->waitSemaphoreCount == 0 && !info->pWaitSemaphores && !trace->completionSemaphore,
                    "completed copy uses zero native waits and has destroyed its unused emergency semaphore");
        } else {
            require(info->waitSemaphoreCount == 1 && info->pWaitSemaphores &&
                info->pWaitSemaphores[0] == trace->completionSemaphore,
                "CPU copy timeout transfers exactly one actual emergency completion wait to native present");
            trace->nativeWait = info->pWaitSemaphores[0];
            if (trace->nativeResult != VK_ERROR_OUT_OF_HOST_MEMORY && trace->nativeResult != VK_ERROR_OUT_OF_DEVICE_MEMORY)
                ++trace->privateConsumed[trace->nativeWait];
        }
        require(info->pSwapchains == trace->appPresent->pSwapchains && info->pImageIndices == trace->appPresent->pImageIndices &&
            info->pResults == trace->appPresent->pResults, "native present retains chain/index/result arrays");
    } else {
        require(info == trace->appPresent, "off path forwards exact present pointer including pNext and original waits");
        for (std::uint32_t n = 0; n < info->waitSemaphoreCount; ++n) ++trace->consumed[info->pWaitSemaphores[n]];
    }
    if (info->pResults) for (std::uint32_t n = 0; n < info->swapchainCount; ++n) info->pResults[n] = trace->individualResult;
    return trace->nativeResult;
}
VKAPI_ATTR void VKAPI_CALL destroyDriver(VkDevice d, VkSwapchainKHR chain, const VkAllocationCallbacks* allocator) {
    event("native-destroy"); ++trace->destroys;
    require(d == device && trace->live.erase(chain) == 1, "each actual native chain destroyed once");
    trace->destroyedChain = chain; trace->destroyedAllocator = allocator;
}
VKAPI_ATTR void VKAPI_CALL destroyDeviceDriver(VkDevice d, const VkAllocationCallbacks*) {
    event("native-device-destroy"); ++trace->deviceDestroys;
    require(d == device && trace->live.empty() && !trace->pending, "native device destruction follows chain/work cleanup");
    require(!trace->managed || trace->backendDestroys == 1, "owned runtime backend released before original device destroy");
    SetLastError(0x76543210);
}
VKAPI_ATTR void VKAPI_CALL destroySurfaceDriver(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*) {
    event("native-surface-destroy"); ++trace->surfaceDestroys;
}
VKAPI_ATTR void VKAPI_CALL destroyInstanceDriver(VkInstance, const VkAllocationCallbacks*) {
    event("native-instance-destroy"); ++trace->instanceDestroys;
}
VKAPI_ATTR VkResult VKAPI_CALL shutdownFenceCreate(VkDevice, const VkFenceCreateInfo*,
                                                 const VkAllocationCallbacks*, VkFence* fence) {
    event("shutdown-fence-create"); *fence = handle<VkFence>(0xF001); return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL shutdownTailSubmit(VkQueue q, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    if(!fence) { event("app-submit");++trace->appSubmits;return trace->appSubmitResult; }
    event("shutdown-tail-submit"); ++trace->tailSubmits;
    if(devices&&devices->queueGate){
        bool acquired{};std::thread other([&]{acquired=devices->queueGate->try_lock();if(acquired)devices->queueGate->unlock();});other.join();
        require(!acquired,"shutdown tail holds the same device queue gate during its driver call");shutdownGateObserved=true;
    }
    require(q == queue && count == 1 && fence && !submits[0].commandBufferCount &&
            !submits[0].waitSemaphoreCount && !submits[0].signalSemaphoreCount,
            "device shutdown submits one real empty tail on the admitted single queue");
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL appSubmit2(VkQueue,std::uint32_t,const VkSubmitInfo2*,VkFence) {
    event("app-submit2");++trace->appSubmits;return trace->appSubmitResult;
}
VKAPI_ATTR VkResult VKAPI_CALL appBindSparse(VkQueue,std::uint32_t,const VkBindSparseInfo*,VkFence) {
    event("app-bind-sparse");++trace->sparseSubmits;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL createSemaphoreDriver(VkDevice,const VkSemaphoreCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkSemaphore* semaphore) {
    event("native-semaphore-create");++trace->semaphoreCreates;
    trace->semaphoreInfo=info;trace->semaphoreAllocator=allocator;
    *semaphore=handle<VkSemaphore>(trace->nextSemaphore++);SetLastError(0x12345678);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroySemaphoreDriver(VkDevice,VkSemaphore,const VkAllocationCallbacks*) {
    event("native-semaphore-destroy");++trace->semaphoreDestroys;
}
VKAPI_ATTR VkResult VKAPI_CALL shutdownFenceWait(VkDevice, std::uint32_t count, const VkFence*, VkBool32 all, std::uint64_t timeout) {
    event("shutdown-tail-wait"); ++trace->tailWaits;
    if(devices&&devices->queueGate){bool acquired{};std::thread other([&]{acquired=devices->queueGate->try_lock();if(acquired)devices->queueGate->unlock();});other.join();require(acquired,"shutdown fence wait holds no device queue gate");}
    require(count == 1 && all && timeout == 10'000'000'000ull, "private shutdown GPU proof has a bounded actual fence wait");
    return trace->pending ? VK_TIMEOUT : trace->tailWaitResult;
}
VKAPI_ATTR void VKAPI_CALL shutdownFenceDestroy(VkDevice, VkFence, const VkAllocationCallbacks*) {
    event("shutdown-fence-destroy"); ++trace->tailFenceDestroys;
}

class Backend final : public presentation::Backend {
public:
    ~Backend() override { ++trace->backendDestroys; trace->events.emplace_back("backend-destroy"); }
    VkResult prepare(const VkSwapchainCreateInfoKHR& info) override {
        event("prepare"); ++trace->prepares; trace->prepared = info;
        require(&info == trace->appCreate, "backend validates the original create record");
        return trace->prepareResult;
    }
    VkResult copy(const VkPresentInfoKHR& info, VkImage source, std::uint64_t generation) override {
        event("copy"); ++trace->copies; trace->copiedPresent = &info; trace->copiedImage = source;
        trace->copyProgress = presentation::CopyProgress::NotSubmitted;
        trace->completionSemaphore = VK_NULL_HANDLE;
        if (trace->throwBeforeSubmit) throw std::runtime_error("injected pre-submit allocation failure");
        if (!trace->copySubmit) return trace->copyResult;
        trace->generations.push_back(generation);
        require(&info == trace->appPresent, "copy alone receives original present waits");
        require(source == trace->live.at(info.pSwapchains[0])[info.pImageIndices[0]], "copy reads native acquired image selected by app index");
        for(std::uint32_t n=0;n<info.waitSemaphoreCount;++n) trace->pendingWaits[info.pWaitSemaphores[n]]=true;
        if(trace->copyUncertain) {
            trace->pending=true;
            trace->copyProgress=presentation::CopyProgress::SubmissionUncertain;
            return trace->copyResult;
        }
        for (std::uint32_t n = 0; n < info.waitSemaphoreCount; ++n)
            require(++trace->consumed[info.pWaitSemaphores[n]] == 1, "original binary semaphore consumed exactly once");
        trace->pending = true; trace->fenceComplete = false;
        trace->copyProgress = presentation::CopyProgress::SubmittedPending;
        trace->completionSemaphore = handle<VkSemaphore>(0xB000 + generation * 32 + info.pImageIndices[0]);
        if(trace->copyResult==VK_ERROR_DEVICE_LOST) {
            trace->copyProgress=presentation::CopyProgress::SubmissionUncertain;
            return trace->copyResult;
        }
        if (trace->throwAfterSubmit) throw std::runtime_error("injected accepted-submit failure");
        if ((trace->queuedOff || trace->queuedOn) && trace->copyResult==VK_SUCCESS) {
            trace->copyProgress=trace->queuedOn ? presentation::CopyProgress::QueuedOn : presentation::CopyProgress::QueuedOff;
            ++trace->captureHeld;
            return VK_SUCCESS;
        }
        if (trace->copyResult == VK_SUCCESS) {
            trace->events.emplace_back("copy-fence-complete"); trace->pending = false; trace->fenceComplete = true;
            trace->copyProgress = presentation::CopyProgress::Completed;
            trace->completionSemaphore = VK_NULL_HANDLE;
            ++trace->emergencyDestroys;
            ++trace->captureHeld;
            trace->pendingWaits.clear();
            if(trace->contextAppearsOnCopy)trace->timingGeneration=1;
        }
        return trace->copyResult;
    }
    presentation::CopyProgress copyProgress() const override { return trace->copyProgress; }
    VkSemaphore copyCompletionSemaphore() const override { return trace->completionSemaphore; }
    VkResult finishQueuedCopy() override {
        if((trace->copyProgress!=presentation::CopyProgress::QueuedOff &&
            trace->copyProgress!=presentation::CopyProgress::QueuedOn) || !trace->pending)return VK_SUCCESS;
        ++trace->queuedFinishes;
        if(trace->queuedFinishResult!=VK_SUCCESS)return trace->queuedFinishResult;
        trace->pending=false;trace->fenceComplete=true;trace->pendingWaits.clear();
        trace->copyProgress=presentation::CopyProgress::Completed;
        return VK_SUCCESS;
    }
    void retireChain(std::uint64_t generation) override {
        if(trace->throwRetirement)throw std::runtime_error("injected queued retirement failure");
        trace->retiredSemaphoreGenerations.push_back(generation);
    }
    bool retainsAppSemaphore(VkSemaphore semaphore) const override {return trace->pendingWaits.count(semaphore)!=0;}
    void nativePresentIssued(std::uint64_t,bool emergency) override {
        trace->nativeEverPresented=true;
        if(trace->copyProgress==presentation::CopyProgress::QueuedOff || trace->copyProgress==presentation::CopyProgress::QueuedOn) {
            require(!emergency,"healthy per-image completion waits are not emergency timeout handoffs");
            ++trace->healthyNativeWaits;
        }
    }
    std::uint64_t timingContextGeneration() const override {return trace->timingGeneration;}
    bool canDestroyRetiredChain(std::uint64_t) const override {
        return trace->retirementProof && !trace->pending;
    }
    VkResult output() override {
        event("output"); ++trace->outputs;
        if(trace->blockOutput) {
            trace->outputEntered=true;
            while(!trace->releaseOutput.load())std::this_thread::yield();
        }
        trace->untimedUnavailable=false;
        require(((trace->fenceComplete && !trace->pending) ||
                 trace->copyProgress==presentation::CopyProgress::QueuedOff ||
                 trace->copyProgress==presentation::CopyProgress::QueuedOn) && trace->presents > 0,
                "color output follows native present and either completed or explicitly queued private copy");
        if(trace->resignalDuringOutput) {
            VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.signalSemaphoreCount=1;
            info.pSignalSemaphores=&trace->resignalDuringOutput;
            require(vkQueueSubmit(queue,1,&info,VK_NULL_HANDLE)==VK_SUCCESS,
                    "CPU-ahead signal can be observed while prior SDK output is still active");
        }
        if(trace->timingTable && trace->outputResult==VK_SUCCESS) {
            std::lock_guard lock(trace->timingTableMutex);
            xess_vk::detail::PresenterFrameTimings::Handle timing;
            std::uint32_t id{};
            if(trace->untimed)id=trace->timingTable->admitUntimed();
            else if(trace->selectedTiming) {
                timing=trace->timingTable->admit(trace->selectedTiming);
                if(timing)id=timing.ticket.sdkId;
            } else id=trace->timingTable->admitBootstrap(trace->selectedBootstrap);
            --trace->captureHeld;
            if(!id) {trace->untimedUnavailable=trace->untimed;return VK_NOT_READY;}
            trace->timingTable->finish(timing,id,true);trace->sdkId=id;return VK_SUCCESS;
        }
        if (trace->outputResult == VK_SUCCESS || trace->outputResult == VK_NOT_READY) --trace->captureHeld;
        if (trace->outputResult == VK_SUCCESS) ++trace->sdkId;
        return trace->outputResult;
    }
    bool discardOutput() override {
        event("discard-output"); ++trace->discardedOutputs;
        require((trace->copyProgress == presentation::CopyProgress::Completed ||
                 trace->copyProgress == presentation::CopyProgress::QueuedOff ||
                 trace->copyProgress == presentation::CopyProgress::QueuedOn) && trace->captureHeld == 1,
                "unused output releases only a completed acquired capture bundle");
        --trace->captureHeld;
        return true;
    }
    void stopAdmission() override { event("stop-admission"); ++trace->admissionStops; }
    presentation::ShutdownState shutdownState() const override { return trace->shutdown; }
    VkResult drain() override {
        event("drain"); ++trace->drains;
        require(trace->tailWaits != 0, "SDK private cleanup occurs after Vulkan final-tail completion proof");
        if (trace->drainResult == VK_SUCCESS) {
            trace->pending = false;
            ++trace->vendorDestroys;
            trace->shutdown = {true, true, true, trace->nativeTailRetained || (trace->nativeEverPresented && !trace->retirementProof), false};
        } else trace->shutdown.failedQuarantined = true;
        return trace->drainResult;
    }
    void requestTimingStop() override {
        event("timing-stop");
        ++trace->timingStops;
    }
    void associatePresentTiming(std::uint32_t, xess_vk::FrameTimingTicket ticket, std::uint32_t bootstrap) override {
        trace->untimed = !ticket && !bootstrap;
        trace->selectedTiming=ticket;trace->selectedBootstrap=bootstrap;
    }
    presentation::FrameStatus frameStatus() const override {
        if(trace->blockSnapshot) {
            trace->snapshotEntered=true;
            while(!trace->releaseSnapshot.load())std::this_thread::yield();
        }
        presentation::FrameStatus status;
        status.untimedOff = trace->untimed;
        status.sdkPresentId = trace->sdkId;
        status.sdkStatusValid = true;
        status.untimedTimingUnavailable=trace->untimedUnavailable;
        status.queuedOff=trace->queuedOff;status.queuedOn=trace->queuedOn;
        return status;
    }
};

struct Fixture {
    Trace observation;
    DeviceState* state{};
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    VkAllocationCallbacks allocator{};
    explicit Fixture(bool managed = true) {
        trace = &observation; observation.managed = managed;
        state = static_cast<DeviceState*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DeviceState)));
        require(state != nullptr, "actual layer heap device state allocated");
        state->queueGate=new presentation::QueueGate;
        state->device = device; state->key = &marker; state->destroy = destroyDeviceDriver;
        state->createSwapchain = createDriver; state->destroySwapchain = destroyDriver;
        state->swapchainImages = imagesDriver; state->present = presentDriver;
        state->createFence = shutdownFenceCreate; state->submit = shutdownTailSubmit;
        state->waitFences = shutdownFenceWait; state->destroyFence = shutdownFenceDestroy;
        state->createSemaphore=createSemaphoreDriver;state->destroySemaphore=destroySemaphoreDriver;
        state->submit2=appSubmit2;state->submit2Khr=appSubmit2;state->bindSparse=appBindSparse;
        if (managed) state->presentation = new presentation::Runtime(device, surface, queue,
            {createDriver, destroyDriver, imagesDriver, presentDriver}, std::make_unique<Backend>());
        devices = state; inventorySessionRetired = false;
        info.surface = surface; info.minImageCount = 2; info.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
        info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; info.imageExtent = {640, 360}; info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR; info.clipped = VK_TRUE; allocator.pUserData = this;
    }
    ~Fixture() {
        if (!state || devices != state) return;
        observation.drainResult = VK_SUCCESS;
        while (!observation.live.empty()) {
            const auto size = observation.live.size();
            vkDestroySwapchainKHR(device, observation.live.begin()->first, nullptr);
            if (observation.live.size() == size) {
                devices = nullptr; state = nullptr; trace = nullptr;
                return; // Never retry or fabricate cleanup for a retained owner tree.
            }
        }
        vkDestroyDevice(device, nullptr); trace = nullptr;
    }
    VkResult create(VkSwapchainKHR& chain, const VkAllocationCallbacks* callbacks = nullptr) {
        observation.appCreate = &info; observation.allocator = callbacks;
        return vkCreateSwapchainKHR(device, &info, callbacks, &chain);
    }
    VkResult present(VkSwapchainKHR& chain, VkResult& individual, VkSemaphore semaphore = handle<VkSemaphore>(0x301),
                     const void* next = nullptr,PFN_vkQueuePresentKHR caller=vkQueuePresentKHR, std::uint32_t index = 1) {
        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; presentInfo.pNext = next;
        presentInfo.waitSemaphoreCount = 1; presentInfo.pWaitSemaphores = &semaphore;
        presentInfo.swapchainCount = 1; presentInfo.pSwapchains = &chain; presentInfo.pImageIndices = &index; presentInfo.pResults = &individual;
        observation.appPresent = &presentInfo;
        return caller(queue, &presentInfo);
    }
    VkSemaphore semaphore(const void* next=nullptr) {
        VkSemaphore semaphore{};VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};info.pNext=next;
        require(vkCreateSemaphore(device,&info,&allocator,&semaphore)==VK_SUCCESS && observation.semaphoreInfo==&info &&
                observation.semaphoreAllocator==&allocator && GetLastError()==0x12345678,
                "semaphore wrapper preserves exact driver arguments/result and last error");
        return semaphore;
    }
    void signal(VkSemaphore semaphore) {
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.signalSemaphoreCount=1;info.pSignalSemaphores=&semaphore;
        require(vkQueueSubmit(queue,1,&info,VK_NULL_HANDLE)==observation.appSubmitResult,
                "actual submit wrapper observes the original driver result");
    }
    void destroyDevice() { vkDestroyDevice(device, nullptr); if (devices != state) state = nullptr; }
};

void presentAdmissionCoversOutputAndSnapshot() {
    Fixture f;VkSwapchainKHR chain{};
    require(f.create(chain)==VK_SUCCESS,"present admission fixture creates its native chain");
    auto& observed=f.observation;observed.blockOutput=observed.blockSnapshot=true;
    VkResult result=VK_ERROR_UNKNOWN,individual=VK_ERROR_UNKNOWN;
    std::exception_ptr failure;
    std::thread presenting([&]{try {result=f.present(chain,individual);}catch(...){failure=std::current_exception();}});
    for(unsigned n=0;n<1000&&!observed.outputEntered.load();++n)Sleep(1);
    const bool outputEntered=observed.outputEntered.load();
    std::atomic<bool> associateEntered{},associated{};
    std::thread associating([&]{associateEntered=true;f.state->presentation->associatePresentTiming(88,{},999);associated=true;});
    for(unsigned n=0;n<1000&&!associateEntered.load();++n)Sleep(1);
    Sleep(10);
    const bool outputBlocked=!associated.load();
    bool outputGateFree=f.state->queueGate->try_lock();if(outputGateFree)f.state->queueGate->unlock();
    observed.releaseOutput=true;
    for(unsigned n=0;n<1000&&!observed.snapshotEntered.load();++n)Sleep(1);
    const bool snapshotEntered=observed.snapshotEntered.load();
    const bool snapshotBlocked=!associated.load();
    bool snapshotGateFree=f.state->queueGate->try_lock();if(snapshotGateFree)f.state->queueGate->unlock();
    observed.releaseSnapshot=true;presenting.join();associating.join();
    observed.blockOutput=observed.blockSnapshot=false;
    if(failure)std::rethrow_exception(failure);
    require(outputEntered&&snapshotEntered&&result==VK_SUCCESS&&individual==VK_SUCCESS,
            "the real present wrapper passes through SDK output and its observer snapshot");
    require(outputBlocked&&snapshotBlocked&&associated&&observed.selectedBootstrap==999,
            "another ticket association waits until both output and observer snapshot leave the present admission");
    require(outputGateFree&&snapshotGateFree,"neither the output wait nor the observer snapshot retains QueueGate");
}

void reservationMarker(void* user,xess_vk::FrameTimingTicket ticket,unsigned index) {
    auto& owner=*static_cast<Trace*>(user);std::lock_guard lock(owner.timingTableMutex);
    const auto handle=owner.timingTable->marker(ticket,index);
    require(bool(handle)&&owner.timingTable->completeMarker(handle,index,0),"owned timing marker completed");
}
void installBootstrapCoordinator(Fixture &fixture) {
    xess_vk::continuous_timing::Callbacks callbacks;
    callbacks.user=&fixture.observation;
    callbacks.contextGeneration = [](void* user) -> std::uint64_t { return static_cast<Trace*>(user)->timingGeneration; };
    callbacks.nextSdkId = [](void* user) -> std::uint32_t {
        auto& owner=*static_cast<Trace*>(user);std::lock_guard lock(owner.timingTableMutex);
        return owner.timingTable?owner.timingTable->nextId():1;
    };
    if(fixture.observation.timingTable) {
        callbacks.begin=[](void* user,std::uint32_t id) {
            auto& owner=*static_cast<Trace*>(user);std::lock_guard lock(owner.timingTableMutex);
            const auto handle=owner.timingTable->begin(id);
            require(bool(handle)&&owner.timingTable->completeBegin(handle,0,0),"owned timing begin completed");return handle.ticket;
        };
        callbacks.simulationEnd=[](void* user,xess_vk::FrameTimingTicket ticket){reservationMarker(user,ticket,1);};
        callbacks.renderBegin=[](void* user,xess_vk::FrameTimingTicket ticket){reservationMarker(user,ticket,2);};
        callbacks.renderEnd=[](void* user,xess_vk::FrameTimingTicket ticket){reservationMarker(user,ticket,3);};
        callbacks.abandon=[](void* user,xess_vk::FrameTimingTicket ticket) {
            auto& owner=*static_cast<Trace*>(user);std::lock_guard lock(owner.timingTableMutex);
            owner.timingTable->abandon(ticket);++owner.timingReturns;
        };
    }
    fixture.state->continuousTiming = new xess_vk::continuous_timing::Coordinator(callbacks);
    fixture.state->continuousQueue=queue;
    fixture.state->deviceIncarnation=77;
    fixture.state->packetIdentities=new packet::IdentityRegistry(77);
    for(const auto& chain:fixture.observation.live) fixture.state->packetIdentities->swapchainCreated(bits(chain.first),VK_SUCCESS);
    initializePacketLedger(fixture.state);
}
bool packetStage(Fixture &fixture, xess_vk::game_timing::Stage stage, std::uint32_t key) {
    xess_vk::game_timing::Event packet;
    packet.stage = stage;
    packet.engineFrame = key;
    packet.threadId = GetCurrentThreadId();
    packet.frameKeyValid = true;
    return fixture.state->continuousTiming->event(packet);
}
bool enqueuePacket(Fixture& fixture,VkSwapchainKHR chain,VkSemaphore semaphore,std::uint32_t sourceThread=GetCurrentThreadId()) {
    const std::uint32_t index=1;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount=1;present.pWaitSemaphores=&semaphore;
    present.swapchainCount=1;present.pSwapchains=&chain;present.pImageIndices=&index;
    xess_vk::game_timing::EnqueueEvent event;
    event.queue=bits(queue);event.presentInfo=reinterpret_cast<std::uintptr_t>(&present);
    event.threadId=sourceThread;event.queueValid=true;
    return continuousEnqueue(fixture.state,event);
}

// Own executable call stub gives the production wrapper a real immediate RA;
// no route test export or fabricated return-address argument enters production.
struct DirectCaller {
    unsigned char* code=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    DirectCaller() {
        require(code!=nullptr,"allocate owned direct caller");
        const std::array<unsigned char,21> bytes{0x48,0x83,0xec,0x28,0x48,0xb8,0,0,0,0,0,0,0,0,
            0xff,0xd0,0x48,0x83,0xc4,0x28,0xc3};
        std::memcpy(code,bytes.data(),bytes.size());
        const auto target=reinterpret_cast<std::uintptr_t>(&vkQueuePresentKHR);
        std::memcpy(code+6,&target,sizeof(target));DWORD old{};
        require(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old) && FlushInstructionCache(GetCurrentProcess(),code,bytes.size()),
                "owned caller becomes executable before use");
    }
    ~DirectCaller(){if(code)VirtualFree(code,0,MEM_RELEASE);}
    void admit(Fixture& fixture,std::uintptr_t rva)const {
        const auto address=reinterpret_cast<std::uintptr_t>(code+16);
        require(address>rva,"owned caller base subtraction does not wrap");
        fixture.state->continuousUnityBase=address-rva;
    }
    VkResult present(Fixture& fixture,VkSwapchainKHR& chain,VkResult& individual,VkSemaphore wait,const void* next=nullptr)const {
        return fixture.present(chain,individual,wait,next,reinterpret_cast<PFN_vkQueuePresentKHR>(code));
    }
};
void completeFuture(Fixture& fixture,std::uint32_t key,bool endRender=true) {
    using Stage=xess_vk::game_timing::Stage;
    require(packetStage(fixture,Stage::SimulationStart,key) && packetStage(fixture,Stage::SimulationEnd,key) &&
            packetStage(fixture,Stage::RenderSubmitStart,key) && (!endRender || packetStage(fixture,Stage::RenderSubmitEnd,key)),
            "owned exact frame reaches the requested real coordinator marker boundary");
}
void directPresentSources() {
    using Stage=xess_vk::game_timing::Stage;
    DirectCaller caller;
    {
        Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};VkResult individual{};
        t.timingTable=std::make_unique<xess_vk::detail::PresenterFrameTimings>(1,true);
        require(f.create(chain)==VK_SUCCESS,"direct source creates actual runtime");installBootstrapCoordinator(f);
        caller.admit(f,xess_vk::game_timing::directPresentReturnRvas[0]);
        auto wait=f.semaphore();f.signal(wait);
        require(packetStage(f,Stage::NativePresentStart,100) && caller.present(f,chain,individual,wait)==VK_SUCCESS &&
                packetStage(f,Stage::NativePresentEnd,100) && t.selectedBootstrap==1 && !t.untimed,
                "proven direct call publishes and claims its current bootstrap packet before any async source exists");
        for(const auto key:{101u,102u,103u})completeFuture(f,key);
        for(unsigned index=0;index<3;++index) {
            const auto key=101+index;const auto exact=f.state->continuousTiming->frame(key);
            caller.admit(f,xess_vk::game_timing::directPresentReturnRvas[index]);wait=f.semaphore();f.signal(wait);
            require(packetStage(f,Stage::NativePresentStart,key) && caller.present(f,chain,individual,wait)==VK_SUCCESS &&
                    packetStage(f,Stage::NativePresentEnd,key) && t.selectedTiming==exact && t.sdkId==index+2 &&
                    f.state->presentation->lastSdkOutputSucceeded(),
                    "each fixed direct RA selects its exact older packet while CPU keys remain ahead");
        }
        require(f.state->continuousDirectSeen==4 && f.state->continuousDirectAccepted==4 &&
                f.state->continuousDirectRefused==0 && f.state->packets->stats().claimed==4 &&
                f.state->packets->stats().finished==4 && !f.state->packets->stats().inFlight &&
                !t.timingTable->stats().occupied && !f.state->continuousTimingRetired,
                "direct packet pins return exactly once without changing async masks or SDK ownership");
        const auto wall = f.state->presentation->frameStatus();
        require(wall.runtimeCopyCpu.count==4 && wall.nativePresentCpu.count==4 && wall.outputCpu.count==4 &&
                wall.outputCpu.maxNs<=wall.outputCpu.totalNs,
                "actual wrapper reports each entered Runtime phase once without changing packet ownership");
        wait=f.semaphore();f.signal(wait);
        require(!packetStage(f,Stage::NativePresentStart,102) && caller.present(f,chain,individual,wait)==VK_SUCCESS &&
                t.untimed && f.state->continuousDirectRefused==1 && f.state->packets->stats().claimed==4,
                "already presented scope cannot republish an old ticket despite a recognized direct return");
        completeFuture(f,105);wait=f.semaphore();f.signal(wait);
        require(packetStage(f,Stage::NativePresentStart,104) && caller.present(f,chain,individual,wait)==VK_SUCCESS &&
                f.state->continuousTimingRetired && !f.state->continuousTiming->stats().startupGapDeferrals,
                "a later missing packet cannot reopen the startup gap window after complete timing succeeded");
    }
    for(const auto rva:{0x19EF19u,0xD38845u,0x19EF8Cu}) {
        Fixture f;VkSwapchainKHR chain{};VkResult individual{};
        require(f.create(chain)==VK_SUCCESS,"unsupported direct caller creates chain");installBootstrapCoordinator(f);
        caller.admit(f,rva);const auto wait=f.semaphore();f.signal(wait);
        require(packetStage(f,Stage::NativePresentStart,0) && caller.present(f,chain,individual,wait)==VK_SUCCESS &&
                f.observation.untimed && !f.state->continuousDirectSeen && !f.state->packets->stats().published &&
                f.state->continuousUnknownPresentCallers==1,
                "SL proxy, async consumer and nearby return cannot borrow a current native scope as direct source");
    }
    for(unsigned failure=0;failure<5;++failure) {
        Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};VkResult individual{};
        require(f.create(chain)==VK_SUCCESS,"direct refusal creates chain");installBootstrapCoordinator(f);
        caller.admit(f,xess_vk::game_timing::directPresentReturnRvas[0]);
        const auto wait=f.semaphore();f.signal(wait);
        if(failure==0) {
            xess_vk::game_timing::Event scope;scope.stage=Stage::NativePresentStart;scope.frameKeyValid=true;scope.threadId=7001;
            require(f.state->continuousTiming->event(scope) && enqueuePacket(f,chain,wait,7001),
                    "old async row exists but direct thread owns no source scope");
        } else {
            require(packetStage(f,Stage::NativePresentStart,0),"direct failure begins a genuine current scope");
            if(failure!=1)require(enqueuePacket(f,chain,wait),"failure fixture owns an existing matching row");
        }
        if(failure==1)f.state->continuousQueue=handle<VkQueue>(0x777);
        if(failure==3)t.timingGeneration=2;
        const auto* next=failure==4?reinterpret_cast<const void*>(1):nullptr;
        require(caller.present(f,chain,individual,wait,next)==VK_SUCCESS && f.state->continuousDirectSeen==1 &&
                f.state->continuousDirectRefused==1 && !f.state->continuousDirectAccepted &&
                !f.state->packets->stats().claimed && !f.state->packets->stats().occupied,
                "missing scope, queue mismatch, duplicate key, changed context and invalid packet cannot consume old async association");
        if(failure==2)require(f.state->packets->stats().duplicates==1,"same-key direct publication preserves ledger ambiguity rejection");
    }
    {
        Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};VkResult individual{};
        t.timingTable=std::make_unique<xess_vk::detail::PresenterFrameTimings>(1,true);
        require(f.create(chain)==VK_SUCCESS,"incomplete direct ticket creates chain");installBootstrapCoordinator(f);
        caller.admit(f,xess_vk::game_timing::directPresentReturnRvas[0]);
        require(packetStage(f,Stage::NativePresentStart,0) && packetStage(f,Stage::NativePresentEnd,0),"anchor exact SDK sequence");
        completeFuture(f,1,false);const auto wait=f.semaphore();f.signal(wait);
        require(packetStage(f,Stage::NativePresentStart,1) && caller.present(f,chain,individual,wait)==VK_SUCCESS &&
                f.state->continuousDirectRefused==1 && !f.state->continuousDirectAccepted &&
                f.state->packets->stats().published==1 && !f.state->packets->stats().claimed &&
                f.state->continuousTimingRetired && !t.timingTable->stats().occupied,
                "a direct source cannot use incomplete markers; existing OFF recovery returns its real reservation");
    }
    {
        // Real deferred initialization can miss the older CPU frame's start.
        // Direct-route recognition does not invent that frame's timing record.
        Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};VkResult individual{};
        t.timingGeneration=0;t.contextAppearsOnCopy=true;
        t.timingTable=std::make_unique<xess_vk::detail::PresenterFrameTimings>(1,true);
        require(f.create(chain)==VK_SUCCESS,"deferred startup creates chain");installBootstrapCoordinator(f);
        caller.admit(f,xess_vk::game_timing::directPresentReturnRvas[0]);
        completeFuture(f,6);completeFuture(f,7);completeFuture(f,8);
        auto wait=f.semaphore();f.signal(wait);
        require(packetStage(f,Stage::NativePresentStart,6) && caller.present(f,chain,individual,wait)==VK_SUCCESS &&
                t.sdkId==1 && f.state->presentation->lastSdkOutputSucceeded(),"contextless first direct packet still starts visible OFF");
        completeFuture(f,9);
        const auto exact=f.state->continuousTiming->frame(9);
        for(const auto key:{7u,8u}) {
            wait=f.semaphore();f.signal(wait);
            require(packetStage(f,Stage::NativePresentStart,key) && caller.present(f,chain,individual,wait)==VK_SUCCESS &&
                    !f.state->continuousTimingRetired && !f.state->presentation->lastSdkOutputSucceeded() &&
                    t.sdkId==1 && t.timingTable->stats().occupied==1 &&
                    packetStage(f,Stage::NativePresentEnd,key),
                    "bounded old direct startup gaps preserve the actual future reservation without inventing visible output or IDs");
        }
        wait=f.semaphore();f.signal(wait);
        require(packetStage(f,Stage::NativePresentStart,9) && caller.present(f,chain,individual,wait)==VK_SUCCESS &&
                f.state->presentation->lastSdkOutputSucceeded() && t.sdkId==2 && t.selectedTiming==exact &&
                !f.state->continuousTimingRetired && f.state->continuousTiming->stats().timedPresents==1 &&
                f.state->continuousTiming->stats().startupGapDeferrals==2 && !t.timingStops,
                "late-context startup reaches the first complete exact direct ticket after two older gaps");
    }
    for(const bool knownRoute:{true,false}) {
        Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};VkResult individual{};
        t.timingTable=std::make_unique<xess_vk::detail::PresenterFrameTimings>(1,true);
        require(f.create(chain)==VK_SUCCESS,"startup gap budget fixture creates chain");installBootstrapCoordinator(f);
        auto wait=f.semaphore();f.signal(wait);
        require(f.present(chain,individual,wait)==VK_SUCCESS && t.sdkId==1,"ordinary startup OFF establishes owner allocation");
        caller.admit(f,knownRoute?0x19EF8D:0x19EF19);
        completeFuture(f,100);
        const unsigned attempts=knownRoute?9:1;
        for(unsigned key=1;key<=attempts;++key) {
            wait=f.semaphore();f.signal(wait);
            require(packetStage(f,Stage::NativePresentStart,key) && caller.present(f,chain,individual,wait)==VK_SUCCESS &&
                    !f.state->presentation->lastSdkOutputSucceeded() && t.sdkId==1,
                    "startup pressure neither reports a visible SDK frame nor reuses the future ID");
            if(knownRoute && key<=8)
                require(!f.state->continuousTimingRetired && packetStage(f,Stage::NativePresentEnd,key),
                        "at most eight proven direct startup gaps keep their future reservation");
        }
        require(f.state->continuousTimingRetired && !t.timingTable->stats().occupied &&
                f.state->continuousTiming->stats().startupGapDeferrals==(knownRoute?8:0),
                "budget exhaustion retires normally; unsupported SL source receives no startup deferral");
        wait=f.semaphore();f.signal(wait);
        require(caller.present(f,chain,individual,wait)==VK_SUCCESS && f.state->presentation->lastSdkOutputSucceeded() &&
                t.sdkId==3 && !t.timingStops,"exhausted or unsupported startup still recovers ordinary visible OFF on the same owner");
    }
}
void timingRetirementAndCleanup() {
    using Stage = xess_vk::game_timing::Stage;
    Fixture f;
    auto &t = f.observation;
    VkSwapchainKHR chain{}, replacement{};
    VkResult individual{};
    require(f.create(chain) == VK_SUCCESS, "missing timing fixture creates native chain");
    installBootstrapCoordinator(f);
    const auto bootstrapWait=f.semaphore();f.signal(bootstrapWait);
    require(packetStage(f, Stage::NativePresentStart, 0) && enqueuePacket(f,chain,bootstrapWait) &&
            f.present(chain, individual,bootstrapWait) == VK_SUCCESS &&
            packetStage(f, Stage::NativePresentEnd, 0), "genuine bootstrap packet completes");
    for (unsigned n = 0; n < 10; ++n) {
        const auto wait=f.semaphore();f.signal(wait);
        require(f.present(chain, individual, wait) == VK_SUCCESS &&
                individual == VK_SUCCESS && t.outputs == n + 2 && t.timingStops == 0 &&
                !f.state->presentation->failed() && f.state->presentation->lastSdkOutputSucceeded(),
                "ten missing-scope packets retain native success and actual untimed OFF output without stopping timing");
    }
    require(f.state->continuousTiming->stats().untimedPresents == 10,
            "coordinator advances only using ten actual successful OFF IDs");
    f.info.oldSwapchain = chain;
    f.info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    require(f.create(replacement) == VK_SUCCESS && t.creates == 2 &&
            t.nativeCreate.oldSwapchain == chain && t.nativeCreate.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR,
            "missing timing does not block real FIFO to IMMEDIATE oldSwapchain replacement");
    const auto replacementWait=f.semaphore();f.signal(replacementWait);
    require(f.present(replacement, individual, replacementWait) == VK_SUCCESS &&
            t.generations.back() != t.generations.front(),
            "private semaphore ownership distinguishes chain generations");
}

void unavailableUntimedRetiresTimingOnce() {
    using Stage=xess_vk::game_timing::Stage;
    Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};VkResult individual{};
    t.timingTable=std::make_unique<xess_vk::detail::PresenterFrameTimings>(1,true);
    require(f.create(chain)==VK_SUCCESS,"untimed recovery fixture creates the real runtime chain");
    installBootstrapCoordinator(f);
    const auto present=[&] {
        const auto wait=f.semaphore();f.signal(wait);
        const auto before=t.presents;
        require(f.present(chain,individual,wait)==VK_SUCCESS && individual==VK_SUCCESS,
                "each recovery packet preserves native present success");
        require(t.presents==before+1 && t.nativePresent.waitSemaphoreCount==0 && t.consumed[wait]==1,
                "each recovery packet consumes app waits in copy and presents hidden WSI once with zero waits");
        require(!t.captureHeld,"each attempted SDK output returns its private capture");
    };
    present();present();
    require(t.sdkId==2 && !f.state->continuousTimingRetired && !t.timingStops,
            "startup without source packets advances real untimed IDs without retiring healthy timing");
    std::uint32_t futureHigh{};
    packet::Claim held;
    for(const auto key:{10u,11u}) {
        require(packetStage(f,Stage::SimulationStart,key) && packetStage(f,Stage::SimulationEnd,key) &&
                packetStage(f,Stage::RenderSubmitStart,key) && packetStage(f,Stage::RenderSubmitEnd,key),
                "CPU-ahead future frame reserves a complete real timing ticket");
        futureHigh=f.state->continuousTiming->frame(key).sdkId;
        const auto wait=f.semaphore();f.signal(wait);
        require(packetStage(f,Stage::NativePresentStart,key) && enqueuePacket(f,chain,wait) &&
                packetStage(f,Stage::NativePresentEnd,key),"future source packet owns an exact timing pin");
        if(key==10) {
            const std::array<std::uint64_t,1> waits{bits(wait)};
            held=f.state->packets->claim(f.state->packetIdentities->makeKey(bits(queue),bits(chain),1,waits),1);
            require(held.valid,"actual ledger claim pins the pending consumer until it returns");
        }
    }
    require(t.timingTable->stats().occupied==2,"two future reservations reproduce the startup OFF block");
    present();
    require(f.state->presentation->lastUntimedTimingUnavailable() && f.state->continuousTimingRetired==1 &&
            !f.state->presentation->failed() && t.sdkId==2 && t.timingReturns==1 &&
            t.timingTable->stats().occupied==1 && f.state->packets->stats().inFlight==1 &&
            !t.timingStops && !t.backendDestroys && !t.admissionStops,
            "only actual untimed admission failure retires owner timing, returning unclaimed pins while preserving claimed SDK ownership");
    present();
    require(t.sdkId==2 && t.timingReturns==1 && t.timingTable->stats().occupied==1 &&
            !packetStage(f,Stage::SimulationStart,12) && !f.state->presentation->lastSdkOutputSucceeded(),
            "repeated blocked OFF cannot recycle the claim, reenable timing, or count as a visible success");
    f.state->packets->finish(held.lease);
    require(t.timingReturns==2 && !t.timingTable->stats().occupied &&
            !f.state->packets->stats().inFlight && !f.state->continuousTiming->stats().occupied,
            "actual consumer return releases the last cancelled reservation exactly once");
    for(unsigned n=0;n<3;++n) {
        present();
        require(f.state->presentation->lastSdkOutputSucceeded() && t.sdkId==futureHigh+1+n &&
                f.state->continuousTimingRetired==1 && t.timingReturns==2 && !t.timingStops &&
                !t.backendDestroys && !t.admissionStops && !*f.state->presentation->error(),
                "same SDK owner resumes monotonic ordinary OFF after real pins return; timing stays retired");
    }
    require(f.state->continuousTiming->stats().timedPresents==0,
            "ordinary OFF recovery is never counted as successful continuous timing");
    vkDestroySwapchainKHR(device,chain,nullptr);f.destroyDevice();
    require(t.vendorDestroys==1 && t.backendDestroys==1 && t.deviceDestroys==1,
            "sticky OFF owner still follows normal SDK/bridge/device cleanup");
    {
        Fixture temporary;VkSwapchainKHR native{};VkResult result{};
        require(temporary.create(native)==VK_SUCCESS,"unrelated temporary output fixture creates chain");
        installBootstrapCoordinator(temporary);temporary.observation.outputResult=VK_NOT_READY;
        const auto wait=temporary.semaphore();temporary.signal(wait);
        require(temporary.present(native,result,wait)==VK_SUCCESS &&
                !temporary.state->continuousTimingRetired &&
                !temporary.state->presentation->lastUntimedTimingUnavailable(),
                "generic VK_NOT_READY cannot retire timing without the specific ColorPresenter admission failure");
    }
}

void retainedCompletionOwners() {
    Fixture f;
    auto &t = f.observation;
    VkSwapchainKHR chain{};
    VkResult individual{};
    require(f.create(chain) == VK_SUCCESS, "retained fixture creates native chain");
    t.copyResult = VK_TIMEOUT;
    require(f.present(chain, individual) == VK_SUCCESS && t.pending && t.presents == 1 &&
            t.outputs == 0 && t.nativeWait == t.completionSemaphore,
            "CPU timeout queues exactly one native present on real completion semaphore");
    InstanceState parent{};
    parent.instance = instance; parent.key = &marker;
    parent.destroySurface = destroySurfaceDriver; parent.destroy = destroyInstanceDriver;
    f.state->parent = &parent; instances = &parent;
    t.drainResult = VK_NOT_READY;
    vkDestroySwapchainKHR(device, chain, nullptr);
    require(t.destroys == 0 && t.backendDestroys == 0 && parent.presentationQuarantined &&
            t.pending && t.live.count(chain), "unproven completion retains native chain and full owner tree");
    const auto drains = t.drains;
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    require(t.drains == drains && !t.deviceDestroys && !t.surfaceDestroys && !t.instanceDestroys &&
            !t.backendDestroys && !t.tailSubmits && !t.vendorDestroys,
            "copy timeout retains all dependencies without new tail submissions or vendor/parent destruction");
    f.state->parent = nullptr;
    instances = nullptr;
    devices = nullptr;
    f.state = nullptr;
    trace = nullptr; // CPU-only mock handles are intentionally retained until process exit.
}

void retainOldWithoutClosingNew() {
    Fixture f;
    auto &t = f.observation;
    VkSwapchainKHR old{}, current{};
    VkResult individual{};
    require(f.create(old) == VK_SUCCESS && f.present(old, individual) == VK_SUCCESS,
            "old-chain retention fixture first outputs a real mock packet");
    f.info.oldSwapchain = old;
    f.info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    require(f.create(current) == VK_SUCCESS, "mode replacement creates its real new chain");
    t.retirementProof = false;
    const auto drains = t.drains;
    vkDestroySwapchainKHR(device, old, nullptr);
    require(t.destroys == 0 && t.drains == drains && t.timingStops == 0 && t.backendDestroys == 0,
            "old chain with unproven WSI waits is retained without shared SDK drain or stop");
    require(f.present(current, individual, handle<VkSemaphore>(0x430)) == VK_SUCCESS &&
            t.outputs == 2 && !f.state->presentation->failed(),
            "create-new destroy-old present-new keeps the active shared backend running");
    // Mock retirement proof is restored only for local fixture cleanup.
    t.retirementProof = true;
}
void destroyCurrentThenCreateWithoutOld() {
    Fixture f;auto& t=f.observation;VkSwapchainKHR old{},fresh{};VkResult individual{};
    require(f.create(old)==VK_SUCCESS && f.present(old,individual)==VK_SUCCESS,"current-retirement fixture has visible output");
    t.retirementProof=false;
    vkDestroySwapchainKHR(device,old,nullptr);
    f.info.oldSwapchain=VK_NULL_HANDLE;
    require(f.create(fresh)==VK_SUCCESS && t.nativeCreate.oldSwapchain==old && !f.info.oldSwapchain && t.prepares==2,
            "NULL-old application create privately replaces its unique not-yet-native-retired destroyed predecessor");
    require(f.present(fresh,individual,handle<VkSemaphore>(0x432))==VK_SUCCESS && t.outputs==2 && !t.drains && !t.timingStops,
            "NULL-old replacement reuses active shared presenter without shutdown");
    require(f.present(fresh,individual,handle<VkSemaphore>(0x433))==VK_SUCCESS && !t.live.count(old) && t.destroys == 1,
            "the actual native replacement lineage retires an implicitly inherited old chain after anchor reuse");
    t.retirementProof=true;
}
void implicitReplacementFailureDoesNotReuseOld() {
    Fixture f; auto& t = f.observation; VkSwapchainKHR old{}, fresh{}; VkResult individual{};
    t.retirementProof = false;
    require(f.create(old) == VK_SUCCESS && f.present(old, individual) == VK_SUCCESS, "implicit failure fixture has used current chain");
    vkDestroySwapchainKHR(device, old, nullptr);
    t.createResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    require(f.create(fresh) == VK_ERROR_OUT_OF_DEVICE_MEMORY && t.nativeCreate.oldSwapchain == old,
            "failed implicit native create still consumes the old chain retirement transition");
    t.createResult = VK_SUCCESS;
    require(f.create(fresh) == VK_SUCCESS && !t.nativeCreate.oldSwapchain && !f.info.oldSwapchain,
            "retry never supplies the same already-native-retired handle as implicit oldSwapchain");
    t.retirementProof = true;
}
void replacementPresentRetirement() {
    InstanceState parent{};
    Fixture f; f.state->parent = &parent; auto& t = f.observation; VkSwapchainKHR current{}; VkResult individual{};
    t.retirementProof = false;
    require(f.create(current) == VK_SUCCESS && f.present(current, individual) == VK_SUCCESS,
            "replacement retirement starts from a genuinely presented native chain");
    for (unsigned cycle = 0; cycle != 12; ++cycle) {
        const auto old = current;
        f.info.oldSwapchain = old;
        f.info.imageExtent = {640 + cycle * 2, 360 + cycle * 2};
        require(f.create(current) == VK_SUCCESS && t.prepares == cycle + 2,
                "more than eight real extent replacements remain managed without a history-size cutoff");
        vkDestroySwapchainKHR(device, old, nullptr);
        require(t.destroys == 0 && !t.timingStops && !t.drains,
                "destroy request retains the old native tail without shutting down timing or the shared SDK");
        require(!parent.presentationQuarantined, "healthy delayed retirement never poisons the parent instance");
        require(f.present(current, individual, handle<VkSemaphore>(0xD000 + cycle)) == VK_SUCCESS && !t.destroys,
                "first successful replacement present records an anchor but does not prove completion");
    }
    require(t.live.size() == 13, "all requested but unproven native generations remain owned");
    require(f.present(current, individual, handle<VkSemaphore>(0xD100), nullptr, vkQueuePresentKHR, 0) == VK_SUCCESS && !t.destroys,
            "completion of a different acquired image is not the anchor proof");
    require(f.present(current, individual, handle<VkSemaphore>(0xD101)) == VK_SUCCESS && t.destroys == 12 && t.live.size() == 1,
            "same anchor reacquired and rendered before a completed copy retires every explicitly replaced generation");
    require(!f.state->presentation->failed() && !t.timingStops && !t.drains,
            "healthy delayed destroy does not poison the live runtime");
    require(f.state->presentation->retiredChainsDestroyed() == 12 && f.state->presentation->retainedChains() == 1,
            "retirement counters describe actual destroyed generations and the retained current chain");
    t.retirementProof = true;
}
void queuedOffCompletionAndRetirement() {
    for (const bool queuedOn : {false,true}) {
        Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};VkResult individual{};
        t.queuedOff=!queuedOn;t.queuedOn=queuedOn;
        require(f.create(chain)==VK_SUCCESS && f.present(chain,individual,handle<VkSemaphore>(0xEE01))==VK_SUCCESS &&
                t.pending && !f.state->presentation->failed() && f.state->presentation->lastSdkOutputSucceeded() &&
                !f.state->presentation->lastPrivateCopyCompleted() && t.healthyNativeWaits==1 &&
                t.consumed[handle<VkSemaphore>(0xEE01)]==1,
                "healthy queued OFF/ON consumes original waits once and outputs through private native synchronization without a CPU completion claim");
        require(f.present(chain,individual,handle<VkSemaphore>(0xEE02))==VK_SUCCESS && t.queuedFinishes==1 &&
                t.outputs==2 && f.state->presentation->frameStatus().runtimeCopyCpu.count==2,
                "the next present completes the previous single slot within its normal copy timing scope");
        const auto copied=t.copies;t.queuedFinishResult=VK_TIMEOUT;
        require(f.present(chain,individual,handle<VkSemaphore>(0xEE03))==VK_SUCCESS && t.copies==copied &&
                t.consumed[handle<VkSemaphore>(0xEE03)]==1 && t.nativePresent.waitSemaphoreCount==1 &&
                f.state->presentation->failed(),
                "previous queued timeout forwards this unsubmitted new packet without replaying the old original wait");
        t.pending=false;t.queuedFinishResult=VK_SUCCESS;
    }
    for (const bool queuedOn : {false,true}) for(const bool lateOld:{false,true}) {
        Fixture f;auto& t=f.observation;VkSwapchainKHR old{},current{};VkResult individual{};
        t.retirementProof=false;
        require(f.create(old)==VK_SUCCESS && f.present(old,individual)==VK_SUCCESS,
                "queued retirement starts with a real native old-chain tail");
        f.info.oldSwapchain=old;t.queuedOff=!queuedOn;t.queuedOn=queuedOn;
        require(f.create(current)==VK_SUCCESS && f.present(current,individual,handle<VkSemaphore>(0xEF01))==VK_SUCCESS,
                "new-chain queued present does not invent completed-copy retirement proof");
        if(lateOld)require(f.present(old,individual,handle<VkSemaphore>(0xEF02))==VK_SUCCESS,
                          "late old present is forwarded and invalidates saved queued proof");
        vkDestroySwapchainKHR(device,old,nullptr);
        require(!t.destroys && f.present(current,individual,handle<VkSemaphore>(0xEF03))==VK_SUCCESS && !t.destroys,
                "completion of the first queued image only establishes a new accepted-packet anchor");
        require(f.present(current,individual,handle<VkSemaphore>(0xEF04))==VK_SUCCESS && t.destroys==(lateOld?0u:1u),
                "only a subsequent completed same-image copy proves retirement; a pre-late-old proof stays revoked");
        if(lateOld)require(f.present(current,individual,handle<VkSemaphore>(0xEF05))==VK_SUCCESS && t.destroys==1,
                          "a fresh same-image queued sequence eventually retires the real late old tail");
        require(t.retiredSemaphoreGenerations.size()==1 && t.retiredSemaphoreGenerations[0]==1,
                "per-image completion synchronization is retired only for the generation with WSI proof");
        vkDestroySwapchainKHR(device,current,nullptr);
        require(t.destroys==1 && t.live.count(current),"the final queued WSI tail is retained despite private fence completion");
        t.pending=false;t.retirementProof=true;
    }
    {
        Fixture f;auto& t=f.observation;VkSwapchainKHR old{},current{};VkResult individual{};
        t.retirementProof=false;
        require(f.create(old)==VK_SUCCESS && f.present(old,individual)==VK_SUCCESS,"retirement exception fixture has an old native tail");
        f.info.oldSwapchain=old;t.queuedOff=true;
        require(f.create(current)==VK_SUCCESS && f.present(current,individual,handle<VkSemaphore>(0xEF11))==VK_SUCCESS &&
                f.present(current,individual,handle<VkSemaphore>(0xEF12))==VK_SUCCESS,"retirement exception fixture has two queued same-image packets");
        vkDestroySwapchainKHR(device,old,nullptr);t.throwRetirement=true;const auto copied=t.copies;
        require(f.present(current,individual,handle<VkSemaphore>(0xEF13))==VK_SUCCESS && t.copies==copied &&
                t.consumed[handle<VkSemaphore>(0xEF13)]==1 && t.nativePresent.waitSemaphoreCount==1 &&
                f.state->presentation->failed(),
                "proof callback exception before copy cannot reuse previous Completed status or drop current original waits");
        t.throwRetirement=false;t.pending=false;t.retirementProof=true;
    }
}
void replacementProofRequiresCompletedCopy() {
    Fixture f; auto& t = f.observation; VkSwapchainKHR old{}, current{}; VkResult individual{};
    t.retirementProof = false;
    require(f.create(old) == VK_SUCCESS && f.present(old, individual) == VK_SUCCESS, "pending proof fixture has old native output");
    f.info.oldSwapchain = old;
    require(f.create(current) == VK_SUCCESS && f.present(current, individual, handle<VkSemaphore>(0xD210)) == VK_SUCCESS,
            "pending proof fixture has a successful replacement anchor");
    vkDestroySwapchainKHR(device, old, nullptr);
    t.copyResult = VK_TIMEOUT;
    require(f.present(current, individual, handle<VkSemaphore>(0xD211)) == VK_SUCCESS && !t.destroys &&
            f.state->presentation->lastCopyProgress() == presentation::CopyProgress::SubmittedPending,
            "same-image pending copy and synchronized native success still cannot prove old presentation completion");
    // Only the CPU driver model is completed for fixture cleanup.
    t.pending = false; t.retirementProof = true;
}
void replacementProofRequiresAcceptedPresent() {
    Fixture f; auto& t = f.observation; VkSwapchainKHR old{}, current{}; VkResult individual{};
    t.retirementProof = false;
    require(f.create(old) == VK_SUCCESS && f.present(old, individual) == VK_SUCCESS, "failure proof fixture has an old output");
    f.info.oldSwapchain = old;
    require(f.create(current) == VK_SUCCESS && f.present(current, individual, handle<VkSemaphore>(0xD200)) == VK_SUCCESS,
            "failure proof fixture records replacement anchor");
    vkDestroySwapchainKHR(device, old, nullptr);
    t.nativeResult = VK_ERROR_OUT_OF_DATE_KHR;
    require(f.present(current, individual, handle<VkSemaphore>(0xD201)) == VK_ERROR_OUT_OF_DATE_KHR && !t.destroys,
            "rejected native present cannot retire old generations even after completed copy");
    t.nativeResult = VK_SUCCESS; t.retirementProof = true;
}
void delayedDestroyAndRapidReplacement() {
    Fixture f; auto& t = f.observation; VkSwapchainKHR old{}, unused{}, current{}; VkResult individual{};
    t.retirementProof = false;
    require(f.create(old) == VK_SUCCESS && f.present(old, individual) == VK_SUCCESS, "rapid fixture has one native output");
    f.info.oldSwapchain = old;
    require(f.create(unused) == VK_SUCCESS, "first replacement is deliberately never presented");
    f.info.oldSwapchain = unused;
    require(f.create(current) == VK_SUCCESS, "second replacement inherits explicit earlier lineage");
    vkDestroySwapchainKHR(device, unused, nullptr);
    require(t.destroys == 1, "unused intermediate chain can be destroyed without WSI completion proof");
    require(f.present(current, individual, handle<VkSemaphore>(0xD300)) == VK_SUCCESS &&
            f.present(current, individual, handle<VkSemaphore>(0xD301)) == VK_SUCCESS && t.destroys == 1,
            "proved retirement does not destroy an older chain before its application destroy request");
    vkDestroySwapchainKHR(device, current, nullptr);
    require(t.destroys == 1, "destroying current first preserves its unproven final native tail");
    vkDestroySwapchainKHR(device, old, nullptr);
    require(t.destroys == 2 && f.state->presentation->retiredChainsDestroyed() == 2,
            "late destroy consumes the recorded real proof, including rapid unpresented replacement lineage");
    t.retirementProof = true;
}
void lateOldPresentInvalidatesProof() {
    for (bool alreadyProved : {false, true}) {
        Fixture f; auto& t = f.observation; VkSwapchainKHR old{}, current{}; VkResult individual{};
        t.retirementProof = false;
        require(f.create(old) == VK_SUCCESS && f.present(old, individual) == VK_SUCCESS,
                "late-old fixture has a used old chain");
        f.info.oldSwapchain = old;
        require(f.create(current) == VK_SUCCESS && f.present(current, individual, handle<VkSemaphore>(0xD400)) == VK_SUCCESS,
                "late-old fixture records first replacement anchor");
        if (alreadyProved)
            require(f.present(current, individual, handle<VkSemaphore>(0xD401)) == VK_SUCCESS,
                    "old completion proof may exist before its delayed destroy request");
        const auto copies = t.copies;
        require(f.present(old, individual, handle<VkSemaphore>(0xD402)) == VK_SUCCESS && t.copies == copies,
                "an image acquired before retirement may still be presented through original native waits");
        vkDestroySwapchainKHR(device, old, nullptr);
        require(!t.destroys, "late old native present invalidates even an already formed retirement proof");
        require(f.present(current, individual, handle<VkSemaphore>(0xD403)) == VK_SUCCESS && !t.destroys,
                "first replacement present after late old work establishes a fresh anchor only");
        require(f.present(current, individual, handle<VkSemaphore>(0xD404)) == VK_SUCCESS && t.destroys == 1,
                "fresh same-image completion finally retires the late old native work");
        t.retirementProof = true;
    }
}
void fullscreenMetadataContract() {
    for (auto mode : {VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT, VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT,
                      VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT}) {
        Fixture f; VkSwapchainKHR chain{};
        VkSurfaceFullScreenExclusiveWin32InfoEXT monitor{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
        monitor.hmonitor = handle<HMONITOR>(0x100);
        VkSurfaceFullScreenExclusiveInfoEXT exclusive{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
        exclusive.fullScreenExclusive = mode;
        if (mode == VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT) { monitor.pNext = &exclusive; f.info.pNext = &monitor; }
        else { exclusive.pNext = &monitor; f.info.pNext = &exclusive; }
        require(f.create(chain) == VK_SUCCESS && f.observation.prepares == 1 &&
                f.observation.nativeExclusive == VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT,
                "three known non-application-controlled fullscreen modes use a nonexclusive hidden-chain shadow");
        require(exclusive.fullScreenExclusive == mode && monitor.hmonitor == handle<HMONITOR>(0x100) &&
                (mode == VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT ? monitor.pNext == &exclusive : exclusive.pNext == &monitor),
                "normalization never modifies caller fullscreen records or their ordering");
    }
    for (unsigned kind = 0; kind != 3; ++kind) {
        Fixture f; VkSwapchainKHR chain{};
        VkSurfaceFullScreenExclusiveInfoEXT exclusive{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
        VkSurfaceFullScreenExclusiveInfoEXT duplicate{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
        VkBaseInStructure unknown{VK_STRUCTURE_TYPE_MAX_ENUM, nullptr};
        exclusive.fullScreenExclusive = kind == 0 ? VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT : VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT;
        if (kind == 1) exclusive.pNext = &duplicate;
        f.info.pNext = kind == 2 ? static_cast<void*>(&unknown) : static_cast<void*>(&exclusive);
        require(f.create(chain) == VK_ERROR_FEATURE_NOT_PRESENT && !chain && !f.observation.creates && !f.observation.prepares,
                "exclusive ownership, duplicate and unknown metadata reject before a hidden passthrough chain can be created");
    }
}
presentation::Surface* capabilitySurface{};
VkPhysicalDevice capabilityPhysical{};
const VkPhysicalDeviceSurfaceInfo2KHR* capabilityInfo{};
VkSurfaceCapabilities2KHR* capabilityOutput{};
unsigned capabilityCalls{};
VKAPI_ATTR VkResult VKAPI_CALL capabilityDriver(VkPhysicalDevice physical, VkSurfaceKHR queried,
                                               VkSurfaceCapabilitiesKHR* out) {
    require(physical == capabilityPhysical, "capability query preserves physical device");
    ++capabilityCalls;
    RECT client{}; require(GetClientRect(capabilitySurface->hidden(), &client) != 0, "owned hidden HWND remains valid");
    out->currentExtent = {static_cast<std::uint32_t>(client.right), static_cast<std::uint32_t>(client.bottom)};
    out->minImageCount = queried == surface ? 2 : 3;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL capabilityDriver2(VkPhysicalDevice physical,
    const VkPhysicalDeviceSurfaceInfo2KHR* info, VkSurfaceCapabilities2KHR* out) {
    require(info == capabilityInfo && out == capabilityOutput,
            "capabilities2 preserves original structures and extension chains");
    return capabilityDriver(physical, info->surface, &out->surfaceCapabilities);
}
void capabilityWindowSynchronization() {
    const auto original = CreateWindowExW(0, L"STATIC", L"Owned capability CPU fixture", WS_OVERLAPPEDWINDOW,
                                         0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(original != nullptr, "capability fixture creates its own non-game HWND");
    {
        presentation::Surface mapped(original); mapped.handle = surface; capabilitySurface = &mapped;
        InstanceState parent{}; parent.instance = instance; parent.key = &marker;
        parent.surfaceCapabilities = capabilityDriver; parent.surfaceCapabilities2 = capabilityDriver2;
        parent.presentationSurface = &mapped; instances = &parent;
        capabilityPhysical = reinterpret_cast<VkPhysicalDevice>(&instanceObject);
        RECT client{}; require(GetClientRect(original, &client), "original fixture extent readable");
        VkSurfaceCapabilitiesKHR out{};
        require(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(capabilityPhysical, surface, &out) == VK_SUCCESS &&
                out.currentExtent.width == static_cast<std::uint32_t>(client.right) &&
                out.currentExtent.height == static_cast<std::uint32_t>(client.bottom),
                "mapped KHR query synchronizes positive original client extent before actual downstream query");
        const auto first = out.currentExtent;
        require(SetWindowPos(original, nullptr, 0, 0, 420, 260, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
                "CPU fixture changes only its own window size");
        VkPhysicalDeviceSurfaceInfo2KHR info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
        info.surface = handle<VkSurfaceKHR>(0x999);
        VkSurfaceCapabilities2KHR second{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
        capabilityInfo = &info; capabilityOutput = &second;
        require(vkGetPhysicalDeviceSurfaceCapabilities2KHR(capabilityPhysical, &info, &second) == VK_SUCCESS &&
                second.surfaceCapabilities.currentExtent.width == first.width && second.surfaceCapabilities.minImageCount == 3,
                "unmapped surface query forwards without changing the mapped hidden window");
        info.surface = surface;
        require(vkGetPhysicalDeviceSurfaceCapabilities2KHR(capabilityPhysical, &info, &second) == VK_SUCCESS &&
                GetClientRect(original, &client) && second.surfaceCapabilities.currentExtent.width == static_cast<std::uint32_t>(client.right),
                "mapped capabilities2 query synchronizes the new positive extent");
        const auto positive = second.surfaceCapabilities.currentExtent;
        ShowWindow(original, SW_MINIMIZE);
        require(IsIconic(original) && vkGetPhysicalDeviceSurfaceCapabilitiesKHR(capabilityPhysical, surface, &out) == VK_SUCCESS &&
                out.currentExtent.width == positive.width && out.currentExtent.height == positive.height,
                "minimized original retains last positive hidden extent instead of resizing zero");
        ShowWindow(original, SW_RESTORE); ShowWindow(original, SW_HIDE);
        require(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(capabilityPhysical, surface, &out) == VK_SUCCESS &&
                out.currentExtent.width == positive.width, "restored window resumes ordinary extent query");
        require(instanceHook("vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) &&
                instanceHook("vkGetPhysicalDeviceSurfaceCapabilities2KHR") == reinterpret_cast<PFN_vkVoidFunction>(vkGetPhysicalDeviceSurfaceCapabilities2KHR),
                "both capability functions are exposed through the actual instance hook table");
        instances = nullptr; capabilitySurface = nullptr;
    }
    require(DestroyWindow(original), "CPU fixture releases its owned HWND");
}
void uncertainSubmissionAndSemaphoreRetention() {
    for(const auto result:{VK_ERROR_DEVICE_LOST,VK_ERROR_UNKNOWN}) {
        Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};VkResult individual=VK_SUCCESS;
        require(f.create(chain)==VK_SUCCESS,"uncertain-submit fixture creates a real mock chain");
        const auto wait=f.semaphore();t.copyUncertain=true;t.copyResult=result;
        require(f.present(chain,individual,wait)==result && individual==result && !t.presents && !t.outputs &&
                f.state->presentation->lastCopyProgress()==presentation::CopyProgress::SubmissionUncertain &&
                f.state->presentation->shutdownState().failedQuarantined,
                "unprovable private submit error returns literal error without replaying original waits or inventing emergency signal");
        vkDestroySemaphore(device,wait,&f.allocator);
        f.destroyDevice();
        require(!t.semaphoreDestroys && !t.tailSubmits && !t.vendorDestroys && !t.deviceDestroys,
                "uncertain submission retains known original wait and full owners without further GPU or vendor work");
        devices=nullptr;f.state=nullptr;trace=nullptr;
    }
    {
        Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};VkResult individual=VK_SUCCESS;
        require(f.create(chain)==VK_SUCCESS,"lost-copy-fence fixture creates chain");
        const auto wait=f.semaphore();t.copyResult=VK_ERROR_DEVICE_LOST;
        require(f.present(chain,individual,wait)==VK_ERROR_DEVICE_LOST && individual==VK_ERROR_DEVICE_LOST && !t.presents,
                "accepted copy followed by DEVICE_LOST fence wait retains dependencies without a new emergency native submission");
        vkDestroySemaphore(device,wait,nullptr);f.destroyDevice();
        require(!t.semaphoreDestroys && !t.tailSubmits && !t.vendorDestroys,
                "lost fence completion keeps known waits and failed SDK tree alive");
        devices=nullptr;f.state=nullptr;trace=nullptr;
    }
    for(bool pending:{false,true}) {
        Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};VkResult individual{};
        require(f.create(chain)==VK_SUCCESS,"wait-lifetime fixture creates chain");
        const auto wait=f.semaphore();if(pending)t.copyResult=VK_TIMEOUT;
        require(f.present(chain,individual,wait)==VK_SUCCESS,"wait-lifetime fixture conserves native packet");
        vkDestroySemaphore(device,wait,&f.allocator);
        require(t.semaphoreDestroys==(pending?0u:1u),
                "only an unresolved private wait intercepts actual application semaphore destruction");
        if(pending){devices=nullptr;f.state=nullptr;trace=nullptr;}
    }
}
void observedSubmitCyclesAndTypes() {
    Fixture f;VkSwapchainKHR chain{};require(f.create(chain)==VK_SUCCESS,"identity-submit fixture creates chain");
    installBootstrapCoordinator(f);
    const auto binary=f.semaphore();f.signal(binary);
    const std::array<std::uint64_t,1> waits{bits(binary)};
    auto key=[&] {return f.state->packetIdentities->makeKey(bits(queue),bits(chain),1,waits);};
    require(key().valid,"successful ordinary binary signal grants exactly its observed cycle");
    VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO};wait.waitSemaphoreCount=1;wait.pWaitSemaphores=&binary;
    const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;wait.pWaitDstStageMask=&stage;
    require(vkQueueSubmit(queue,1,&wait,VK_NULL_HANDLE)==VK_SUCCESS && !key().valid,
            "ordinary application submit wait retires a signal before any later present can reuse it");
    f.signal(binary);require(key().valid,"new successful signal restores a distinct binary cycle");
    f.observation.appSubmitResult=VK_ERROR_OUT_OF_HOST_MEMORY;
    require(vkQueueSubmit(queue,1,&wait,VK_NULL_HANDLE)==VK_ERROR_OUT_OF_HOST_MEMORY && !key().valid,
            "failed submit never resurrects a previously observed cycle");
    f.observation.appSubmitResult=VK_SUCCESS;
    VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};signal.signalSemaphoreCount=1;signal.pSignalSemaphores=&binary;
    std::array<VkSubmitInfo,2> batch{signal,wait};
    require(vkQueueSubmit(queue,2,batch.data(),VK_NULL_HANDLE)==VK_SUCCESS && !key().valid,
            "multi-batch signal followed by wait is conservatively unavailable for packet matching");
    batch={wait,signal};
    require(vkQueueSubmit(queue,2,batch.data(),VK_NULL_HANDLE)==VK_SUCCESS && !key().valid,
            "same-call wait/signal overlap preserves driver semantics without claiming batch-order proof");
    const auto other=f.semaphore();VkSemaphoreSubmitInfo wait2{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait2.semaphore=binary;wait2.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal2=wait2;signal2.semaphore=other;
    VkSubmitInfo2 submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};submit2.waitSemaphoreInfoCount=1;submit2.pWaitSemaphoreInfos=&wait2;
    submit2.signalSemaphoreInfoCount=1;submit2.pSignalSemaphoreInfos=&signal2;
    require(vkQueueSubmit2(queue,1,&submit2,VK_NULL_HANDLE)==VK_SUCCESS,
            "Submit2 forwards the real wait-to-distinct-signal call");
    const std::array<std::uint64_t,1> otherWait{bits(other)};
    require(f.state->packetIdentities->makeKey(bits(queue),bits(chain),1,otherWait).valid,
            "Submit2 publishes a distinct ordinary binary signal after driver success");
    submit2.pSignalSemaphoreInfos=&wait2;
    require(vkQueueSubmit2KHR(queue,1,&submit2,VK_NULL_HANDLE)==VK_SUCCESS && !key().valid,
            "Submit2KHR same-handle wait/signal overlap is unavailable too");
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
    const auto timeline=f.semaphore(&type);
    const std::array<std::uint64_t,2> timelineValues{1,2};
    std::array<VkTimelineSemaphoreSubmitInfo,2> timelineInfo{};
    std::array<VkSubmitInfo,2> timelineBatches{};
    for(unsigned i=0;i<2;++i) {
        timelineInfo[i].sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo[i].signalSemaphoreValueCount=1;timelineInfo[i].pSignalSemaphoreValues=&timelineValues[i];
        timelineBatches[i].sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;timelineBatches[i].pNext=&timelineInfo[i];
        timelineBatches[i].signalSemaphoreCount=1;timelineBatches[i].pSignalSemaphores=&timeline;
    }
    require(vkQueueSubmit(queue,2,timelineBatches.data(),VK_NULL_HANDLE)==VK_SUCCESS && f.state->packetIdentities->stats().reliable,
            "unrelated repeated timeline handles across batches do not poison binary provenance");
    const std::array<std::uint64_t,1> timelineWait{bits(timeline)};
    require(!f.state->packetIdentities->makeKey(bits(queue),bits(chain),1,timelineWait).valid,
            "timeline semaphore is ineligible for binary present packet identity");
    VkExportSemaphoreCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    const auto exported=f.semaphore(&external);f.signal(exported);
    const std::array<std::uint64_t,1> externalWait{bits(exported)};
    require(!f.state->packetIdentities->makeKey(bits(queue),bits(chain),1,externalWait).valid,
            "external export pNext does not receive an unproven ordinary binary identity");
    f.signal(binary);require(key().valid,"timeline and exported objects leave ordinary binary tracking usable");
    require(vkQueueBindSparse(queue,0,nullptr,VK_NULL_HANDLE)==VK_SUCCESS && !key().valid,
            "unsupported sparse semaphore effects retire identity proof while preserving original driver call");
    vkDestroySemaphore(device,binary,nullptr);vkDestroySemaphore(device,other,nullptr);
    vkDestroySemaphore(device,timeline,nullptr);vkDestroySemaphore(device,exported,nullptr);
}
void exactSourceConsumerAndResignal() {
    Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};VkResult individual{};
    require(f.create(chain)==VK_SUCCESS,"exact async packet fixture creates chain");installBootstrapCoordinator(f);
    constexpr std::uint32_t producerThread=7001;
    xess_vk::game_timing::Event boundary;
    boundary.stage=xess_vk::game_timing::Stage::NativePresentStart;boundary.frameKeyValid=true;boundary.threadId=producerThread;
    require(f.state->continuousTiming->event(boundary),"source owns a real bootstrap scope on its producer thread");
    const auto wait=f.semaphore();f.signal(wait);
    require(enqueuePacket(f,chain,wait,producerThread),"source entry publishes a copied exact native packet identity");
    require(!f.state->continuousTiming->scopeSnapshot(GetCurrentThreadId()).inPresent,
            "consumer thread has no producer timing scope");
    t.resignalDuringOutput=wait;
    require(f.present(chain,individual,wait)==VK_SUCCESS && !t.untimed && t.sdkId==1 &&
            f.state->packets->stats().claimed==1 && f.state->packets->stats().inFlight==0,
            "unscoped consumer resolves only the exact pinned source packet and returns its lease after output");
    const std::array<std::uint64_t,1> handles{bits(wait)};
    require(f.state->packetIdentities->makeKey(bits(queue),bits(chain),1,handles).valid,
            "entry retirement never erases a newer signal observed during prior SDK output");
    t.resignalDuringOutput=VK_NULL_HANDLE;
    boundary.stage=xess_vk::game_timing::Stage::NativePresentEnd;f.state->continuousTiming->event(boundary);
    xess_vk::game_timing::EnqueueEvent unreadable;
    unreadable.queue=bits(queue);unreadable.queueValid=true;unreadable.presentInfo=1;unreadable.threadId=producerThread;
    require(!continuousEnqueue(f.state,unreadable) && !f.state->packets->stats().inFlight,
            "unreadable source packet invalidates unclaimed candidates without inventing association");
    vkDestroySemaphore(device,wait,nullptr);
    {
        // The real current-thread scope still cannot replace a missing source
        // packet fingerprint. CPU bootstrap OFF remains recoverable.
        Fixture absent;VkSwapchainKHR native{};VkResult perChain{};
        require(absent.create(native)==VK_SUCCESS,"missing carrier fixture creates chain");installBootstrapCoordinator(absent);
        const auto missing=absent.semaphore();absent.signal(missing);
        require(packetStage(absent,xess_vk::game_timing::Stage::NativePresentStart,0) &&
                absent.present(native,perChain,missing)==VK_SUCCESS && absent.observation.untimed &&
                absent.state->packets->stats().claimed==0,
                "consumer never substitutes its current-thread scope for absent exact source evidence");
        vkDestroySemaphore(device,missing,nullptr);
    }
    // Nested CPU fixtures use the same dispatcher globals. Restore this one
    // only for its independently proved cleanup, never for a real GPU object.
    trace=&t;devices=f.state;
}
void callbackAndLeaseShutdownGates() {
    {
        Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};
        require(f.create(chain)==VK_SUCCESS,"reader-gate fixture creates chain");installBootstrapCoordinator(f);
        std::atomic<bool> ready{},release{};
        std::thread reader([&] {
            AcquireSRWLockShared(&stateLock);ready=true;
            while(!release.load())std::this_thread::yield();
            ReleaseSRWLockShared(&stateLock);
        });
        while(!ready.load())std::this_thread::yield();
        f.destroyDevice();
        const auto late=continuousRegistrationFrame(f.state,GetCurrentThreadId());
        const bool held=t.tailSubmits==0 && t.vendorDestroys==0 && t.backendDestroys==0 && !late.valid &&
            InterlockedCompareExchange(&f.state->continuousClosing,0,0)==1;
        release=true;reader.join();
        require(held,"live shared reader makes device teardown retain callback owners before GPU/SDK cleanup; closing rejects late registration");
        auto* deadState=f.state;
        vkDestroySwapchainKHR(device,chain,nullptr);f.destroyDevice();
        require(!continuousRegistrationFrame(deadState,GetCurrentThreadId()).valid,
                "registration callback checks live membership before dereferencing a detached CPU owner");
    }
    {
        Fixture f;auto& t=f.observation;VkSwapchainKHR chain{};
        require(f.create(chain)==VK_SUCCESS,"claim-gate fixture creates chain");installBootstrapCoordinator(f);
        const auto wait=f.semaphore();f.signal(wait);
        require(packetStage(f,xess_vk::game_timing::Stage::NativePresentStart,0) && enqueuePacket(f,chain,wait),
                "claim-gate source has a real pinned packet");
        const std::array<std::uint64_t,1> handles{bits(wait)};
        const auto key=f.state->packetIdentities->makeKey(bits(queue),bits(chain),1,handles);
        const auto lease=f.state->packets->claim(key,1);
        require(lease.valid && f.state->packets->stats().inFlight==1,"actual ledger exposes an unresolved consumer lease");
        f.destroyDevice();
        require(!t.tailSubmits && !t.vendorDestroys && !t.backendDestroys && f.state->continuousTiming &&
                f.state->packets->stats().inFlight==1,
                "claimed packet shutdown retains callback owners without SDK cleanup or fabricated lease completion");
        f.state->packets->finish(lease.lease);
        require(!f.state->packets->stats().inFlight,"actual consumer completion returns the retained pin");
        vkDestroySemaphore(device,wait,nullptr);vkDestroySwapchainKHR(device,chain,nullptr);f.destroyDevice();
    }
}
void normalPrivateCleanupRetainsNativeTail() {
    Fixture f; auto& t = f.observation;
    VkSwapchainKHR chain{}, fresh{}; VkResult individual{};
    require(f.create(chain) == VK_SUCCESS && f.present(chain, individual) == VK_SUCCESS,
            "normal cleanup starts with a completed copy and real SDK output");
    t.retirementProof = false;
    vkDestroySwapchainKHR(device, chain, nullptr);
    require(t.drains == 0 && t.destroys == 0 && !t.timingStops,
            "current chain destruction retains WSI without prematurely draining the shared SDK");
    f.destroyDevice();
    const auto cleanup = f.state->presentation->shutdownState();
    require(cleanup.privateCleanupCompleted && cleanup.sdkReleased && cleanup.bridgeReleased &&
            cleanup.nativeTailRetained && !cleanup.failedQuarantined && t.tailSubmits == 1 && t.tailWaits == 1 &&
            t.vendorDestroys == 1 && t.tailFenceDestroys == 1 && !t.backendDestroys && !t.deviceDestroys,
            "device teardown releases proven private resources and preserves only native tail plus stable CPU shell");
    require(!f.state->presentation->frameStatus().sdkStatusValid,
            "completed private cleanup exposes no stale SDK frame status");
    f.destroyDevice();
    require(t.tailSubmits==1 && t.tailWaits==1 && t.vendorDestroys==1 && !t.deviceDestroys,
            "repeated device destroy preserves cached private cleanup without another queue tail or vendor call");
    vkDestroySwapchainKHR(device, chain, nullptr);
    require(!t.destroys && t.vendorDestroys == 1,
            "repeated old-chain destroy cannot confuse private completion with WSI retirement");
    f.info.oldSwapchain = VK_NULL_HANDLE;
    require(f.create(fresh, &f.allocator) == VK_SUCCESS && t.prepares == 1 && t.nativeCreate.imageUsage == f.info.imageUsage,
            "closed backend forwards a fresh create without restarting private SDK resources");
    require(f.present(fresh, individual, handle<VkSemaphore>(0x431)) == VK_SUCCESS && t.copies == 1 && t.outputs == 1,
            "closed backend forwards a fresh packet without accessing released private copy resources");
    vkDestroySwapchainKHR(device, fresh, &f.allocator);
    require(t.destroys == 1 && t.vendorDestroys == 1,
            "fresh untracked native chain destruction remains exact passthrough after private cleanup");
    devices = nullptr; f.state = nullptr; trace = nullptr;
}

void exactWaitAndLifecycle() {
    Fixture f; auto& t = f.observation; VkSwapchainKHR first{}, second{}; VkResult individual = VK_ERROR_UNKNOWN;
    require(f.create(first) == VK_SUCCESS && t.creates == 1 && t.prepares == 1 && t.imageCalls == 2 && t.swapchainOutput == &first,
        "actual create runs one native create and two private image enumeration calls");
    require(f.info.imageUsage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT && f.info.clipped == VK_TRUE,
        "managed create leaves original app structure intact");
    std::uint32_t count = 1; VkImage image{}; const auto beforeImages = t.imageCalls;
    require(vkGetSwapchainImagesKHR(device, first, &count, &image) == VK_INCOMPLETE && count == 1 &&
        image == t.live.at(first)[0] && t.imageCalls == beforeImages + 1 && t.imageCount == &count && t.imageOutput == &image,
        "actual app enumeration preserves one native call, pointers, data, count and INCOMPLETE");
    require(f.present(first, individual) == VK_SUCCESS && individual == VK_SUCCESS && t.copies == 1 && t.presents == 1 && t.outputs == 1,
        "actual wrapper executes one copy, zero-wait native present and OFF output");
    require(t.events[t.events.size()-3] == "copy-fence-complete" && t.events[t.events.size()-2] == "native-present" && t.events.back() == "output",
        "native-present/output follow actual mock copy completion in order");
    f.info.oldSwapchain = first; f.info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM; f.info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    require(f.create(second) == VK_SUCCESS && second != first && t.creates == 2 && t.prepares == 2 && t.nativeCreate.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR &&
        f.present(second, individual, handle<VkSemaphore>(0x303)) == VK_SUCCESS && t.copies == 2 && t.outputs == 2,
        "actual oldSwapchain replacement routes BGRA and preserves native IMMEDIATE through successful presentation");
    const auto drains = t.drains;
    vkDestroySwapchainKHR(device, first, &f.allocator);
    require(t.destroys == 1 && t.drains == drains && t.destroyedChain == first && t.destroyedAllocator == &f.allocator,
        "independently proved retired old chain forwards exact destroy without closing the live shared backend");
    require(f.present(second, individual, handle<VkSemaphore>(0x304)) == VK_SUCCESS && t.outputs == 3 &&
        t.timingStops == 0, "new chain output survives destroy-old without shared backend stop");
    vkDestroySwapchainKHR(device, second, nullptr);
    require(t.destroys == 2 && t.drains == drains && t.live.empty(), "proved native chain destruction never drains the active SDK backend");
    f.destroyDevice();
    require(t.backendDestroys == 1 && t.deviceDestroys == 1 && devices == nullptr && GetLastError() == 0x76543210,
        "production device wrapper stops/deletes actual owned runtime before exact original destroy");
}

void failureWithoutWaitFallback() {
    for (unsigned fault = 0; fault < 5; ++fault) {
        Fixture f; auto &t = f.observation;
        VkSwapchainKHR chain{}, replacement{};
        VkResult individual = VK_SUBOPTIMAL_KHR;
        require(f.create(chain) == VK_SUCCESS, "failure matrix creates actual native chain");
        if (fault == 0) { t.copySubmit = false; t.copyResult = VK_ERROR_OUT_OF_HOST_MEMORY; }
        if (fault == 1) t.copyResult = VK_TIMEOUT;
        if (fault == 2) t.outputResult = VK_ERROR_UNKNOWN;
        if (fault == 3) t.throwBeforeSubmit = true;
        if (fault == 4) t.throwAfterSubmit = true;
        require(f.present(chain, individual) == VK_SUCCESS && individual == VK_SUCCESS &&
                t.presents == 1 && t.consumed[handle<VkSemaphore>(0x301)] == 1 &&
                f.state->presentation->failed(), "every injected internal fault preserves one native result and consumes app wait once");
        if (fault == 0 || fault == 3)
            require(t.privateConsumed.empty() && t.outputs == 0,
                    "pre-submit failure forwards exact original packet without any private-wait replay");
        else if (fault == 1 || fault == 4)
            require(t.privateConsumed[t.completionSemaphore] == 1 && t.nativeWait == t.completionSemaphore,
                    "accepted copy submission has one native wait on private completion, including thrown post-submit failure");
        else require(t.privateConsumed.empty() && t.emergencyDestroys == 1,
                     "completed-copy output failure keeps zero native waits without replaying the unused emergency signal");
        require(t.outputs == (fault == 2 ? 1u : 0u), "native-called output failure never replays present");
        require(f.state->presentation->lastSdkOutputAttempted()==(fault==2),"SDK attempt distinguishes a current failed output from stale prior status");
        require(!f.state->presentation->frameStatus().sdkStatusValid,
                "failed output does not expose previous SDK status as current success");
        require(f.present(chain, individual, handle<VkSemaphore>(0x302)) == VK_SUCCESS &&
                t.presents == 2 && t.copies == 1 && t.consumed[handle<VkSemaphore>(0x302)] == 1,
                "fresh packet after failure forwards original waits exactly once");
        require(!f.state->presentation->lastSdkOutputAttempted(),"passthrough frame clears the previous SDK attempt fact");
        f.info.oldSwapchain = chain; f.info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        require(f.create(replacement, &f.allocator) == VK_SUCCESS && t.creates == 2 &&
                t.nativeCreate.oldSwapchain == chain && t.nativeCreate.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR,
                "failed private runtime still performs original oldSwapchain replacement with allocator intact");
    }
    {
        Fixture f; auto &t = f.observation; VkSwapchainKHR chain{}; VkResult individual{};
        require(f.create(chain) == VK_SUCCESS, "temporary OFF unavailability creates chain");
        t.outputResult = VK_NOT_READY;
        require(f.present(chain, individual) == VK_SUCCESS && !f.state->presentation->failed() &&
                t.presents == 1 && !f.state->presentation->lastSdkOutputSucceeded()&&f.state->presentation->lastSdkOutputAttempted(),
                "no-side-effect OFF admission refusal preserves native packet and recoverable SDK state");
        t.outputResult = VK_SUCCESS;
        require(f.present(chain, individual, handle<VkSemaphore>(0x305)) == VK_SUCCESS &&
                t.copies == 2 && t.outputs == 2 && f.state->presentation->lastSdkOutputSucceeded(),
                "next frame recovers actual output after temporary OFF refusal");
    }
    {
        Fixture f; auto &t = f.observation; VkSwapchainKHR chain{}; VkResult individual{};
        require(f.create(chain) == VK_SUCCESS, "unsupported contract creates chain");
        VkBaseInStructure next{VK_STRUCTURE_TYPE_MAX_ENUM, nullptr};
        require(f.present(chain, individual, handle<VkSemaphore>(0x306), &next) == VK_SUCCESS &&
                t.copies == 0 && t.nativePresent.pNext == &next && t.nativePresent.waitSemaphoreCount == 1 &&
                !f.state->presentation->failed(), "unsupported private pNext contract forwards exact legal native packet");
        require(f.present(chain, individual, handle<VkSemaphore>(0x307)) == VK_SUCCESS && t.copies == 1,
                "a later supported packet can still use private output");
    }
}

void nativeResultsAndReplacement() {
    Fixture f; auto& t = f.observation; VkSwapchainKHR old{}, current{}; VkResult individual = VK_SUCCESS;
    require(f.create(old) == VK_SUCCESS, "native-result fixture creates actual chain");
    t.nativeResult = VK_SUBOPTIMAL_KHR;
    require(f.present(old, individual, handle<VkSemaphore>(0x311)) == VK_SUBOPTIMAL_KHR && individual == VK_SUCCESS && t.outputs == 1,
        "native aggregate SUBOPTIMAL and per-chain SUCCESS remain distinct after output");
    t.nativeResult = VK_SUCCESS; t.individualResult = VK_ERROR_OUT_OF_DATE_KHR;
    require(f.present(old, individual, handle<VkSemaphore>(0x312)) == VK_SUCCESS && individual == VK_ERROR_OUT_OF_DATE_KHR &&
        t.outputs == 1 && !f.state->presentation->failed() && t.discardedOutputs == 1 && !t.captureHeld,
        "per-chain OUT_OF_DATE retains results and returns unused completed capture bundle without SDK entry");
    f.info.oldSwapchain = old;
    require(f.create(current) == VK_SUCCESS && t.nativeCreate.oldSwapchain == old, "native OUT_OF_DATE recovery uses genuine oldSwapchain create");
    vkDestroySwapchainKHR(device, old, nullptr);
    t.nativeResult = VK_ERROR_OUT_OF_DATE_KHR; t.individualResult = VK_SUCCESS;
    require(f.present(current, individual, handle<VkSemaphore>(0x313)) == VK_ERROR_OUT_OF_DATE_KHR && individual == VK_SUCCESS && t.outputs == 1,
        "aggregate OUT_OF_DATE retained independently of successful per-chain result");
    require(t.discardedOutputs == 2 && !t.captureHeld, "aggregate OUT_OF_DATE returns its unused capture reservation too");
    t.nativeResult = VK_SUCCESS;
    require(f.present(current, individual, handle<VkSemaphore>(0x315)) == VK_SUCCESS && t.outputs == 2 && !t.captureHeld,
            "next legal packet recovers actual SDK output after OUT_OF_DATE bundle release");
    t.nativeResult = VK_ERROR_DEVICE_LOST; t.individualResult = VK_ERROR_OUT_OF_DATE_KHR;
    require(f.present(current, individual, handle<VkSemaphore>(0x314)) == VK_ERROR_DEVICE_LOST && individual == VK_ERROR_OUT_OF_DATE_KHR &&
        t.outputs == 2 && f.state->presentation->failed(), "per-chain OUT_OF_DATE cannot mask independently fatal native aggregate; both literal results preserved");
}

void creationFailureCleanup() {
    for (unsigned failedEnumeration : {1u, 2u}) {
        Fixture f; auto &t = f.observation; VkSwapchainKHR chain{};
        VkResult individual{};
        t.failImageCall = failedEnumeration; t.imageResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        require(f.create(chain) == VK_SUCCESS && chain && t.creates == 1 &&
                t.imageCalls == failedEnumeration && t.destroys == 0 && t.live.count(chain) &&
                f.state->presentation->failed(),
                "native-success chain remains application-owned when private enumeration fails");
        require(f.present(chain, individual) == VK_SUCCESS && t.copies == 0 && t.presents == 1,
                "enumeration-failed chain presents original packet through driver");
    }
    {
        Fixture f; auto &t = f.observation; VkSwapchainKHR chain{};
        VkBaseInStructure unsupported{VK_STRUCTURE_TYPE_MAX_ENUM, nullptr}; f.info.pNext = &unsupported;
        f.info.imageFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        require(f.create(chain, &f.allocator) == VK_ERROR_FEATURE_NOT_PRESENT && t.prepares == 0 && t.imageCalls == 0 && !t.creates,
                "unknown metadata on the mapped hidden surface explicitly rejects instead of creating invisible passthrough output");
    }
    {
        Fixture f; auto &t = f.observation; VkSwapchainKHR old{}, replacement{};
        require(f.create(old) == VK_SUCCESS, "prepare failure starts with a real old chain");
        f.info.oldSwapchain = old;
        t.prepareResult = VK_ERROR_UNKNOWN;
        require(f.create(replacement, nullptr) == VK_SUCCESS && replacement &&
                t.creates == 2 && t.nativeCreate.oldSwapchain == old && t.nativeCreate.imageUsage == f.info.imageUsage,
                "private prepare failure still calls original create and retires real oldSwapchain");
    }
    {
        Fixture f; auto &t = f.observation; VkSwapchainKHR old{}, replacement{};
        require(f.create(old) == VK_SUCCESS, "native create failure starts with old chain");
        f.info.oldSwapchain = old; t.createResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        require(f.create(replacement) == VK_ERROR_OUT_OF_DEVICE_MEMORY && t.creates == 2 &&
                t.nativeCreate.oldSwapchain == old, "native create allocation failure remains literal without retry");
    }
}

void offPathExactForwarding() {
    Fixture f(false); auto& t = f.observation; VkSwapchainKHR chain{}; VkResult individual = VK_SUCCESS;
    VkBaseInStructure next{VK_STRUCTURE_TYPE_MAX_ENUM, nullptr}; f.info.pNext = &next;
    f.info.imageFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32; f.info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    require(f.create(chain, &f.allocator) == VK_SUCCESS && t.creates == 1 && t.prepares == 0 && t.imageCalls == 0 && t.swapchainOutput == &chain &&
        t.nativeCreate.pNext == &next && t.nativeCreate.imageUsage == f.info.imageUsage && t.nativeCreate.clipped == f.info.clipped,
        "opt-out create leaves original pNext/format/mode/usage/clipped and no private enumeration");
    std::uint32_t count{};
    require(vkGetSwapchainImagesKHR(device, chain, &count, nullptr) == VK_SUCCESS && count == 2 && t.imageCalls == 1 && t.imageCount == &count,
        "opt-out image-count wrapper calls original driver exactly once");
    t.nativeResult = VK_SUBOPTIMAL_KHR; t.individualResult = VK_ERROR_OUT_OF_DATE_KHR;
    require(f.present(chain, individual, handle<VkSemaphore>(0x321), &next) == VK_SUBOPTIMAL_KHR && individual == VK_ERROR_OUT_OF_DATE_KHR &&
        t.presents == 1 && t.copies == 0 && t.outputs == 0 && t.nativePresent.waitSemaphoreCount == 1 && t.nativePresent.pNext == &next &&
        t.consumed[handle<VkSemaphore>(0x321)] == 1, "opt-out present preserves original waits/pNext and aggregate/pResults in one original call");
    vkDestroySwapchainKHR(device, chain, &f.allocator);
    require(t.destroys == 1 && t.drains == 0 && t.destroyedAllocator == &f.allocator, "opt-out destroy passes original allocator once");
    f.destroyDevice();
    require(t.deviceDestroys == 1 && !t.backendDestroys, "opt-out device destroy has no runtime lifecycle");
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL diagnosticFunctions(VkDevice, const char*) {
    return reinterpret_cast<PFN_vkVoidFunction>(+[] {});
}
void diagnosticWindowBounds() {
    Fixture f;
    xess_vk::continuous::Config config;
    config.device = device;
    config.queue = queue;
    config.functions = diagnosticFunctions;
    config.memory.memoryTypeCount = 1;
    config.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    xess_vk::continuous::Provider provider(config);
    f.state->continuous = &provider;
    try {
        xess_vk::CommandRecordingFacts facts;
        facts.command = 0x8080;
        facts.rpRecordingGeneration = 1;
        facts.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        facts.queueFamily = 0;
        facts.beginUsageFlags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        facts.recording = facts.stateKnown = facts.scopeKnown = true;
        xess_vk::descriptor_image_metadata::DispatchEvidence evidence;
        evidence.command = {facts.command, 4};
        evidence.metadataAccepted = true;
        evidence.count = 2;
        for (std::size_t index = 0; index < evidence.count; ++index) {
            auto& binding = evidence.bindings[index];
            binding.binding = index == 0 ? 5 : 7;
            binding.image = {0x9000 + index, index + 1};
            binding.viewType = VK_IMAGE_VIEW_TYPE_2D;
            binding.viewFormat = index == 0 ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_R16G16_SFLOAT;
            binding.range = {index == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            binding.declaredLayout = VK_IMAGE_LAYOUT_GENERAL;
            auto& description = binding.imageDescription;
            description.format = binding.viewFormat;
            description.width = description.height = 128;
            description.depth = description.mipLevels = description.arrayLayers = 1;
            description.samples = VK_SAMPLE_COUNT_1_BIT;
            description.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        require(!continuousWindowActive(f.state), "diagnostic window does not consume startup presents");
        continuousRecord(f.state, facts, evidence);
        require(continuousWindowActive(f.state) && provider.stats().missingRecordingFrame == 1 &&
                provider.stats().captured == 0, "eligible missing-frame dispatch arms trace without granting GPU copy");
        const auto remaining = f.state->continuousWindowBudget;
        bool allAccepted = true;
        for (LONG index = 0; index < remaining; ++index)
            allAccepted &= continuousWindowLog(f.state);
        require(allAccepted && !continuousWindowLog(f.state) && !continuousWindowActive(f.state),
                "fixed record budget closes a stalled presentation window");
        require(!continuousWindowLog(f.state), "closed budget cannot reopen");
        // Start the separately checked present bound with the actual wrapper.
        f.state->continuousWindowState = 1;
        f.state->continuousWindowBudget = 512;
        f.state->continuousWindowPresents = 0;
        VkSwapchainKHR chain{};
        VkResult individual{};
        require(f.create(chain) == VK_SUCCESS, "diagnostic present test creates real runtime mock chain");
        for (std::uintptr_t index = 0; index < 16; ++index)
            require(f.present(chain, individual, handle<VkSemaphore>(0xA000 + index)) == VK_SUCCESS,
                    "diagnostic window preserves native mock present and app wait");
        require(!continuousWindowActive(f.state) && f.state->continuousWindowPresents == 16 &&
                f.observation.presents == 16 && f.observation.outputs == 16,
                "actual present wrapper closes at sixteen without stopping output");
        f.state->continuous = nullptr;
    } catch (...) {
        f.state->continuous = nullptr;
        throw;
    }
}
}

namespace fake_pool_fixture {
using namespace presentation_layer_fixture;
namespace p = xess_vk::presentation;
unsigned forbiddenCalls{};
VKAPI_ATTR VkResult VKAPI_CALL forbiddenCreate(VkDevice, const VkSwapchainCreateInfoKHR *,
                                               const VkAllocationCallbacks *, VkSwapchainKHR *) {
    ++forbiddenCalls;
    return VK_ERROR_UNKNOWN;
}
VKAPI_ATTR void VKAPI_CALL forbiddenDestroy(VkDevice, VkSwapchainKHR,
                                            const VkAllocationCallbacks *) {
    ++forbiddenCalls;
}
VKAPI_ATTR VkResult VKAPI_CALL forbiddenImages(VkDevice, VkSwapchainKHR, std::uint32_t *,
                                               VkImage *) {
    ++forbiddenCalls;
    return VK_ERROR_UNKNOWN;
}
VKAPI_ATTR VkResult VKAPI_CALL forbiddenPresent(VkQueue, const VkPresentInfoKHR *) {
    ++forbiddenCalls;
    return VK_ERROR_UNKNOWN;
}
struct Backend final : p::Backend {
    p::QueueGate gate;
    std::atomic<std::uint64_t> completed{};
    std::atomic<unsigned> polls{}, signals{};
    std::atomic<bool> signalEntered{},releaseSignal{};
    bool asyncAcquire{},blockSignal{},invalidCopyBinding{};
    struct CopyBinding {VkImage image{};std::uint64_t generation{};bool acquired{};};
    std::map<std::uint64_t,CopyBinding> copyBindings;
    std::vector<std::uint64_t> acquiredTokens;
    std::uint64_t token{};
    unsigned finishes{}, copies{}, waitOnly{}, releases{}, destroys{}, waitsConsumed{};
    bool bundleHeld{}, throwSignal{}, throwStatus{};
    VkResult createResult{VK_SUCCESS}, signalResult{VK_SUCCESS}, destroyResult{VK_SUCCESS},
        outputResult{VK_SUCCESS}, windowResult{VK_SUCCESS};
    p::CopyProgress progress{p::CopyProgress::NotSubmitted};
    p::ShutdownState shutdown;
    VkResult prepare(const VkSwapchainCreateInfoKHR &) override {
        return VK_SUCCESS;
    }
    VkResult createFakeImages(const VkSwapchainCreateInfoKHR &info, std::uint64_t gen,
                              std::vector<VkImage> &out) override {
        if (createResult != VK_SUCCESS)
            return createResult;
        for (unsigned i = 0; i < std::max(3u, info.minImageCount); ++i)
            out.push_back(handle<VkImage>(0x10000 + gen * 32 + i));
        return VK_SUCCESS;
    }
    VkResult destroyFakeImages(std::uint64_t) override {
        ++destroys;
        return destroyResult;
    }
    VkResult signalAcquire(VkSemaphore semaphore, VkFence fence, xess_vk::wsi_acquire::Call* = nullptr) override {
        if (throwSignal)
            throw std::runtime_error("injected acquire signal uncertainty");
        bool available{};
        std::thread other([&] {
            available = gate.try_lock();
            if (available)
                gate.unlock();
        });
        other.join();
        require(available, "Acquire enters Backend without holding queue gate");
        std::lock_guard lock(gate);
        require(semaphore || fence, "at least one acquire signal object");
        ++signals;
        return signalResult;
    }
    bool asyncAcquireEnabled() const override {return asyncAcquire;}
    VkResult signalAcquireAfterCopy(VkImage image,std::uint64_t generation,std::uint64_t copyToken,
                                    VkSemaphore semaphore,VkFence fence,xess_vk::wsi_acquire::Call* timing) override {
        const auto found=copyBindings.find(copyToken);
        require(asyncAcquire&&image&&generation&&copyToken&&found!=copyBindings.end()&&
                found->second.image==image&&found->second.generation==generation&&!found->second.acquired,
                "early acquire names one exact submitted image generation and unused copy token");
        signalEntered=true;
        if(blockSignal)while(!releaseSignal.load())std::this_thread::yield();
        if(invalidCopyBinding)return VK_ERROR_UNKNOWN;
        const auto result=signalAcquire(semaphore,fence,timing);
        if(result==VK_SUCCESS) {found->second.acquired=true;acquiredTokens.push_back(copyToken);}
        return result;
    }
    VkResult windowStatus(VkExtent2D) const override {
        if (throwStatus)
            throw std::runtime_error("injected window query failure");
        return windowResult;
    }
    VkResult copy(const VkPresentInfoKHR &info, VkImage image, std::uint64_t generation) override {
        ++copies;
        waitsConsumed += info.waitSemaphoreCount;
        progress = p::CopyProgress::QueuedOn;
        ++token;
        copyBindings[token]={image,generation};
        bundleHeld = true;
        return VK_SUCCESS;
    }
    VkResult consumePresentWaits(const VkPresentInfoKHR &info) override {
        ++waitOnly;
        waitsConsumed += info.waitSemaphoreCount;
        progress = p::CopyProgress::SubmittedPending;
        ++token;
        return VK_SUCCESS;
    }
    VkResult consumePresentWaits(const VkPresentInfoKHR &info,VkImage image,std::uint64_t generation) override {
        const auto result=consumePresentWaits(info);copyBindings[token]={image,generation};return result;
    }
    VkResult finishQueuedCopy() override {
        ++finishes;
        if(asyncAcquire)return VK_SUCCESS;
        completed = token;
        if (bundleHeld) {
            ++releases;
            bundleHeld = false;
        }
        return VK_SUCCESS;
    }
    std::uint64_t copyCompletionToken() const override {
        return token;
    }
    VkResult pollCopyCompletion(std::uint64_t value) override {
        ++polls;
        return completed.load() >= value ? VK_SUCCESS : VK_NOT_READY;
    }
    p::CopyProgress copyProgress() const override {
        return progress;
    }
    VkSemaphore copyCompletionSemaphore() const override {
        return VK_NULL_HANDLE;
    }
    p::ShutdownState shutdownState() const override {
        return shutdown;
    }
    VkResult output() override {
        return outputResult;
    }
    VkResult drain() override {
        shutdown = {true, true, true, false, false};
        return VK_SUCCESS;
    }
};
struct Owner {
    Backend backend;
    p::Runtime runtime{
        device,  surface,
        queue,   {forbiddenCreate, forbiddenDestroy, forbiddenImages, forbiddenPresent},
        backend, true};
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    VkSwapchainKHR chain{};
    Owner() {
        info.surface = surface;
        info.minImageCount = 3;
        info.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
        info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.imageExtent = {640, 360};
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        require(runtime.create(&info, nullptr, &chain) == VK_SUCCESS,
                "fake Runtime creates its own image pool");
    }
    std::uint32_t acquire(VkSemaphore semaphore = handle<VkSemaphore>(0x910),
                          VkFence fence = VK_NULL_HANDLE) {
        std::uint32_t index = 99;
        require(runtime.acquire(chain, 0, semaphore, fence, &index) == VK_SUCCESS,
                "fake image acquisition");
        return index;
    }
    VkResult present(VkSwapchainKHR target, std::uint32_t index, VkResult *individual = nullptr) {
        const auto semaphore = handle<VkSemaphore>(0x920);
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &semaphore;
        info.swapchainCount = 1;
        info.pSwapchains = &target;
        info.pImageIndices = &index;
        info.pResults = individual;
        return runtime.present(queue, &info);
    }
};
void run() {
    forbiddenCalls = 0;
    {
        Owner owner;owner.runtime.holdOutput(true);const auto index=owner.acquire();VkResult individual=VK_ERROR_UNKNOWN;
        require(owner.present(owner.chain,index,&individual)==VK_SUCCESS&&individual==VK_SUCCESS&&
            owner.backend.waitOnly==1&&owner.backend.waitsConsumed==1&&!owner.backend.copies&&
            !owner.runtime.lastSdkOutputAttempted()&&std::string_view(owner.runtime.stage())=="sr_output_held_waits_consumed",
            "held SR output consumes original present waits through a real backend submission without publishing an invalid frame");
        owner.runtime.holdOutput(false);const auto next=owner.acquire();
        require(owner.present(owner.chain,next)==VK_SUCCESS&&owner.backend.copies==1&&owner.runtime.lastSdkOutputSucceeded(),
            "after recovery the ordinary acquire/copy/present path resumes with no fabricated completion");
        owner.runtime.destroy(owner.chain,nullptr);owner.runtime.stop();
    }
    {
        Owner owner;
        auto &r = owner.runtime;
        auto &b = owner.backend;
        std::uint32_t count{};
        require(r.images(owner.chain, &count, nullptr) == VK_SUCCESS && count == 3,
                "fake count-only enumeration");
        std::array<VkImage, 2> partial{};
        count = 2;
        require(r.images(owner.chain, &count, partial.data()) == VK_INCOMPLETE && count == 2 &&
                    r.isFakeImage(partial[0]) && r.ownsFakeChain(owner.chain),
                "fake incomplete enumeration and image identity");
        const auto first = owner.acquire();
        owner.acquire(VK_NULL_HANDLE, handle<VkFence>(0x930));
        owner.acquire(handle<VkSemaphore>(0x931), handle<VkFence>(0x932));
        std::uint32_t next = 77;
        xess_vk::wsi_acquire::Call noWait, timedWait;
        require(r.acquire(owner.chain, 0, handle<VkSemaphore>(0x933), VK_NULL_HANDLE, &next, &noWait) ==
                        VK_NOT_READY &&
                    next == 77,
                "timeout zero does not overwrite output");
        require(r.acquire(owner.chain, 100'000, handle<VkSemaphore>(0x933), VK_NULL_HANDLE,
                          &next, &timedWait) == VK_TIMEOUT,
                "finite pool timeout");
        require(noWait.generation&&noWait.noSlotPollCpu.count==1&&!noWait.noSlotWaitCpu.count&&!noWait.signal.entryNs,
                "timeout zero records no-slot polling without waiting or signalling");
        require(timedWait.noSlotPollCpu.count&&timedWait.noSlotWaitCpu.count&&timedWait.noSlotWaitCpu.totalNs&&!timedWait.signal.entryNs,
                "finite no-slot wait records elapsed CPU time without changing timeout or signal behavior");
        require(owner.present(owner.chain, first) == VK_SUCCESS && b.bundleHeld,
                "queued fake present consumes app wait and keeps input lease");
        const auto before = b.finishes;
        std::atomic<bool> acquired{};
        VkResult result = VK_ERROR_UNKNOWN;
        std::thread waiting([&] {
            result = r.acquire(owner.chain, UINT64_MAX, handle<VkSemaphore>(0x934), VK_NULL_HANDLE,
                               &next);
            acquired = true;
        });
        while (!b.polls.load())
            std::this_thread::yield();
        b.completed = b.token;
        for (unsigned n = 0; n < 1000 && !acquired.load(); ++n)
            Sleep(1);
        if (!acquired.load())
            r.stop();
        waiting.join();
        require(result == VK_SUCCESS && next == first && b.finishes == before && b.bundleHeld &&
                    b.releases == 0,
                "MAX acquire advances on source completion without next present or bundle release");
        require(r.destroy(owner.chain, nullptr) == VK_SUCCESS && !r.ownsFakeChain(owner.chain) &&
                    b.destroys == 1,
                "acquired but unpresented slots do not retain forever after real cleanup proof");
        require(r.stop() == VK_SUCCESS && !r.shutdownState().nativeTailRetained,
                "fake shutdown has no native tail");
    }
    {
        Owner owner;
        const auto first = owner.acquire();
        const auto old = owner.chain;
        owner.info.oldSwapchain = old;
        owner.backend.createResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VkSwapchainKHR replacement{};
        require(owner.runtime.create(&owner.info, nullptr, &replacement) ==
                        VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                    owner.runtime.status(old) == VK_ERROR_OUT_OF_DATE_KHR,
                "failed replacement still retires old acquisitions");
        require(owner.present(old, first) == VK_SUCCESS,
                "already acquired retired image may still present");
        owner.backend.createResult = VK_SUCCESS;
        owner.info.oldSwapchain = VK_NULL_HANDLE;
        require(owner.runtime.create(&owner.info, nullptr, &replacement) == VK_SUCCESS,
                "new chain after failed replacement");
        require(owner.runtime.destroy(old, nullptr) == VK_SUCCESS &&
                    owner.runtime.destroy(replacement, nullptr) == VK_SUCCESS &&
                    owner.runtime.stop() == VK_SUCCESS,
                "retired/current fake pools clean independently");
    }
    {
        Owner owner;
        const auto index = owner.acquire();
        const auto old = owner.chain;
        owner.info.oldSwapchain = old;
        owner.info.imageExtent = {800, 450};
        VkSwapchainKHR replacement{};
        require(owner.runtime.create(&owner.info, nullptr, &replacement) == VK_SUCCESS,
                "fake replacement stages a different source extent");
        VkResult individual = VK_SUCCESS;
        require(owner.present(old, index, &individual) == VK_ERROR_OUT_OF_DATE_KHR &&
                    individual == VK_ERROR_OUT_OF_DATE_KHR && owner.backend.waitOnly == 1 &&
                    owner.backend.waitsConsumed == 1 && !owner.backend.copies,
                "incompatible acquired old source consumes waits without using new source geometry");
        owner.runtime.destroy(old, nullptr);
        owner.runtime.destroy(replacement, nullptr);
        owner.runtime.stop();
    }
    {
        Owner owner;
        const auto index = owner.acquire();
        owner.backend.windowResult = VK_ERROR_SURFACE_LOST_KHR;
        VkResult individual = VK_SUCCESS;
        require(
            owner.present(owner.chain, index, &individual) == VK_ERROR_SURFACE_LOST_KHR &&
                individual == VK_ERROR_SURFACE_LOST_KHR && owner.backend.waitOnly == 1 &&
                owner.backend.waitsConsumed == 1 && !owner.backend.copies,
            "surface loss consumes valid present waits exactly once without copy/native present");
        owner.backend.windowResult = VK_SUCCESS;
        owner.runtime.destroy(owner.chain, nullptr);
        owner.runtime.stop();
    }
    {
        Owner owner;
        owner.backend.signalResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        std::uint32_t index = 91;
        require(owner.runtime.acquire(owner.chain, 0, handle<VkSemaphore>(0x940), VK_NULL_HANDLE,
                                      &index) == VK_ERROR_OUT_OF_HOST_MEMORY &&
                    index == 91 && !owner.runtime.failed(),
                "known unsubmitted acquire OOM rolls slot back");
        owner.backend.signalResult = VK_SUCCESS;
        require(owner.acquire() == 0, "OOM rollback retains first free image");
        owner.backend.destroyResult = VK_TIMEOUT;
        require(owner.runtime.destroy(owner.chain, nullptr) == VK_TIMEOUT &&
                    owner.runtime.ownsFakeChain(owner.chain),
                "unproven cleanup retains pool");
        owner.backend.destroyResult = VK_SUCCESS;
        require(owner.runtime.destroy(owner.chain, nullptr) == VK_SUCCESS,
                "later cleanup can reuse retained proof");
        owner.runtime.stop();
    }
    {
        Owner owner;
        owner.backend.throwSignal = true;
        std::uint32_t index = 71;
        require(owner.runtime.acquire(owner.chain, 0, handle<VkSemaphore>(0x950), VK_NULL_HANDLE,
                                      &index) == VK_ERROR_DEVICE_LOST &&
                    owner.runtime.failed() && index == 71,
                "throwing signal quarantines reserved slot without C ABI exception");
        VkResult individual = VK_SUCCESS;
        require(owner.present(owner.chain, 0, &individual) == VK_ERROR_DEVICE_LOST &&
                    individual == VK_ERROR_DEVICE_LOST,
                "terminal present updates per-chain result consistently");
        require(owner.runtime.stop() == VK_ERROR_DEVICE_LOST && !owner.backend.destroys,
                "uncertain signal keeps source resources quarantined");
    }
    {
        Owner owner;
        owner.backend.throwStatus = true;
        require(owner.runtime.status(owner.chain) == VK_ERROR_DEVICE_LOST && owner.runtime.failed(),
                "throwing window status cannot cross API boundary");
    }
    {
        Owner owner;
        owner.info.oldSwapchain = owner.chain;
        VkSurfaceFullScreenExclusiveInfoEXT exclusive{
            VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
        exclusive.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
        owner.info.pNext = &exclusive;
        VkSwapchainKHR replacement{};
        require(owner.runtime.create(&owner.info, nullptr, &replacement) ==
                        VK_ERROR_FEATURE_NOT_PRESENT &&
                    !replacement,
                "fake chain rejects unimplemented application-controlled fullscreen ownership");
        owner.runtime.destroy(owner.chain, nullptr);
        owner.runtime.stop();
    }
    const auto fillPending=[](Owner& owner) {
        owner.backend.asyncAcquire=true;
        for(unsigned i=0;i<3;++i)require(owner.present(owner.chain,owner.acquire())==VK_SUCCESS,
            "async source copy remains pending after present returns");
        require(!owner.backend.completed&&!owner.backend.polls,"available images need no copy completion poll");
    };
    {
        Owner owner;fillPending(owner);
        const auto before=owner.backend.signals.load();
        require(owner.acquire()==0&&owner.acquire(VK_NULL_HANDLE,handle<VkFence>(0x960))==1&&
                owner.acquire(handle<VkSemaphore>(0x961),handle<VkFence>(0x962))==2,
                "pending images return through GPU semaphore-only fence-only and combined acquire signals");
        std::uint32_t untouched=72;
        require(owner.runtime.acquire(owner.chain,0,handle<VkSemaphore>(0x963),VK_NULL_HANDLE,&untouched)==VK_NOT_READY&&untouched==72&&
                owner.backend.acquiredTokens==std::vector<std::uint64_t>({1,2,3})&&
                owner.backend.signals==before+3&&!owner.backend.completed,
                "each pending copy token is acquired once without a CPU completion wait");
        owner.runtime.stop();
    }
    {
        Owner owner;fillPending(owner);owner.backend.signalResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;
        std::uint32_t index=73;
        require(owner.runtime.acquire(owner.chain,0,handle<VkSemaphore>(0x964),VK_NULL_HANDLE,&index)==VK_ERROR_OUT_OF_DEVICE_MEMORY&&
                index==73&&!owner.runtime.failed()&&owner.backend.acquiredTokens.empty(),
                "unsubmitted pending signal OOM restores CopyPending and its unused token");
        owner.backend.signalResult=VK_SUCCESS;
        require(owner.acquire()==0&&owner.backend.acquiredTokens==std::vector<std::uint64_t>({1}),
                "OOM retry reuses the same exact pending image token");
        owner.runtime.stop();
    }
    {
        Owner owner;fillPending(owner);owner.backend.invalidCopyBinding=true;
        std::uint32_t index=74;
        require(owner.runtime.acquire(owner.chain,0,handle<VkSemaphore>(0x965),VK_NULL_HANDLE,&index)==VK_ERROR_UNKNOWN&&
                index==74&&owner.runtime.failed()&&owner.backend.acquiredTokens.empty(),
                "a rejected image generation or copy token quarantines the reservation without successful acquisition");
    }
    {
        Owner owner;owner.backend.asyncAcquire=true;
        const auto first=owner.acquire();owner.acquire();owner.acquire();owner.backend.windowResult=VK_ERROR_OUT_OF_DATE_KHR;
        require(owner.present(owner.chain,first)==VK_ERROR_OUT_OF_DATE_KHR&&owner.backend.waitOnly==1,
                "wait-only unavailable output still submits one app-wait consumer");
        owner.backend.windowResult=VK_SUCCESS;
        require(owner.acquire()==first&&owner.backend.acquiredTokens==std::vector<std::uint64_t>({1}),
                "wait-only submission binds image and generation before early GPU acquisition");
        owner.runtime.stop();
    }
    {
        Owner owner;fillPending(owner);owner.backend.blockSignal=true;
        std::uint32_t index=75;VkResult acquired=VK_ERROR_UNKNOWN;
        std::thread worker([&]{acquired=owner.runtime.acquire(owner.chain,0,handle<VkSemaphore>(0x966),VK_NULL_HANDLE,&index);});
        for(unsigned n=0;n<1000&&!owner.backend.signalEntered.load();++n)Sleep(1);
        const bool entered=owner.backend.signalEntered.load();
        const auto destroyed=entered?owner.runtime.destroy(owner.chain,nullptr):VK_ERROR_UNKNOWN;
        owner.backend.releaseSignal=true;worker.join();
        require(entered&&destroyed==VK_NOT_READY&&owner.backend.destroys==0&&acquired==VK_SUCCESS&&index==0&&
                owner.runtime.ownsFakeChain(owner.chain),"destroy cannot free a pool while its reserved GPU acquire signal is in progress");
        require(owner.runtime.destroy(owner.chain,nullptr)==VK_SUCCESS&&owner.backend.destroys==1,
                "pool destruction retries normally after signal submission returns");
        owner.runtime.stop();
    }
    require(!forbiddenCalls,
            "fake Runtime never invokes driver swapchain create images present or destroy");
}
} // namespace fake_pool_fixture

int main() {
    using namespace presentation_layer_fixture;
    try {
        SetEnvironmentVariableW(L"XESS_OBSERVER_LOG", nullptr);
        SetEnvironmentVariableW(L"XESS_PRESENTATION_OFF", nullptr);
        InitOnceExecuteOnce(&logOnce, initializeLog, nullptr, nullptr);
        exactWaitAndLifecycle(); failureWithoutWaitFallback(); nativeResultsAndReplacement(); creationFailureCleanup(); offPathExactForwarding();
        timingRetirementAndCleanup(); directPresentSources(); unavailableUntimedRetiresTimingOnce(); retainedCompletionOwners(); retainOldWithoutClosingNew();
        normalPrivateCleanupRetainsNativeTail();
        destroyCurrentThenCreateWithoutOld();uncertainSubmissionAndSemaphoreRetention();
        implicitReplacementFailureDoesNotReuseOld();
        replacementPresentRetirement();replacementProofRequiresAcceptedPresent();
        queuedOffCompletionAndRetirement();
        replacementProofRequiresCompletedCopy();
        delayedDestroyAndRapidReplacement();fullscreenMetadataContract();
        lateOldPresentInvalidatesProof();
        capabilityWindowSynchronization();
        observedSubmitCyclesAndTypes();exactSourceConsumerAndResignal();callbackAndLeaseShutdownGates();
        diagnosticWindowBounds();
        presentAdmissionCoversOutputAndSnapshot();
        fake_pool_fixture::run();
        require(shutdownGateObserved,"real shutdown tail fixture exercised device queue gate");
        std::cout << "{\"stage\":\"presentation-runtime-actual-wrappers\",\"passed\":true,\"checks\":" << checks
            << ",\"realVulkanCalls\":0,\"gpuTouched\":false,\"gameTouched\":false}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
