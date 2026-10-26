#include "interop.hpp"
#include "fence_wait.hpp"
#include "native_source_fixture.hpp"
#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void check(VkResult value, const char* operation) {
    if (value != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(value));
}
std::string quote(const std::string& value) {
    std::string result = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { const char* hex = "0123456789abcdef"; result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}
using xess_vk::testing::SourceOwner;
PFN_vkGetInstanceProcAddr storageOriginalGet{};
PFN_vkGetPhysicalDeviceFormatProperties storageOriginalProperties{};
VKAPI_ATTR void VKAPI_CALL storageMaskedProperties(VkPhysicalDevice physical,VkFormat format,VkFormatProperties* properties) {
    storageOriginalProperties(physical,format,properties);
    if(format==VK_FORMAT_R32_SFLOAT&&properties)properties->optimalTilingFeatures&=~VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL storageMaskedGet(VkInstance instance,const char* name) {
    if(!std::strcmp(name,"vkGetPhysicalDeviceFormatProperties"))return reinterpret_cast<PFN_vkVoidFunction>(storageMaskedProperties);
    return storageOriginalGet(instance,name);
}

void colorLayout(SourceOwner& owner, VkImageLayout next) {
    owner.wait();
    check(owner.dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(owner.command, 0), "Reset color layout command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(owner.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(owner.command, &begin), "Begin color layout command");
    auto& color = owner.frame.planes[0];
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = color.currentLayout; barrier.newLayout = next;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = color.image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    owner.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(owner.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    check(owner.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(owner.command), "End color layout command");
    check(owner.dev<PFN_vkResetFences>("vkResetFences")(owner.device, 1, &owner.completion), "Reset color layout fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &owner.command;
    check(owner.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(owner.queue, 1, &submit, owner.completion), "Submit color layout command");
    color.currentLayout = next;
}

void checkQueuedNativePublication(SourceOwner& owner, const xess_vk::InteropConfig& config, bool fullBundle) {
    xess_vk::InteropBridge bridge(config, owner.context());
    owner.render(0); bridge.publishNativeFrame(owner.frame); bridge.verifyFrame(owner.expected(0));
    VkSemaphore gate{};
    bool gateReleased{};
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; create.pNext = &type;
    check(owner.dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(owner.device, &create, nullptr, &gate), "Create queued color gate");
    const auto releaseGate = [&] {
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore = gate; signal.value = 1;
        return owner.dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(owner.device, &signal);
    };
    try {
        const std::uint64_t value = 1;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = 1; timeline.pWaitSemaphoreValues = &value;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO}; wait.pNext = &timeline;
        wait.waitSemaphoreCount = 1; wait.pWaitSemaphores = &gate; wait.pWaitDstStageMask = &stage;
        check(owner.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(owner.queue, 1, &wait, VK_NULL_HANDLE), "Submit queued color gate");
        std::array<VkClearColorValue, 3> clear{};
        clear[0].float32[0] = clear[0].float32[2] = clear[0].float32[3] = 1.f;
        clear[1].float32[0] = xess_vk::testing::kDepth[1];
        clear[2].float32[0] = xess_vk::testing::kMotion[1]; clear[2].float32[1] = xess_vk::testing::kMotion[2];
        owner.renderClear(clear, [](void* opaque, VkCommandBuffer command) {
            auto& source = *static_cast<SourceOwner*>(opaque);
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = source.frame.planes[0].image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            source.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }, &owner);
        owner.frame.planes[0].currentLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (fullBundle) bridge.publishQueuedNativeFrame(owner.frame);
        else bridge.publishQueuedNativeColor(owner.frame.planes[0], owner.queue, owner.completion);
        if (bridge.frameValue() != 2 || owner.dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(owner.device, owner.completion) != VK_NOT_READY)
            throw std::runtime_error("Queued publication did not return while its real producer remained blocked");
        check(releaseGate(), "Release queued color gate");
        gateReleased = true;
        auto expected = owner.expected(fullBundle ? 1 : 0); expected[0] = owner.expected(5)[0];
        bridge.verifyFrame(expected);
        colorLayout(owner, VK_IMAGE_LAYOUT_GENERAL); owner.wait();
        owner.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(owner.device, gate, nullptr); gate = VK_NULL_HANDLE;
    } catch (...) {
        // Release the test-only gate; preserve the borrowed graph if draining fails.
        if (gate) try {
            if ((gateReleased || releaseGate() == VK_SUCCESS) && owner.drainQueue(config.timeoutMs))
                owner.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(owner.device, gate, nullptr);
        } catch (...) {}
        throw;
    }
}

void checkSlottedNativePublication(SourceOwner& owner, const xess_vk::InteropConfig& config) {
    using namespace xess_vk;
    owner.render(53); owner.wait();
    QueueGate queueGate;
    auto context = owner.context(); context.queueGate = &queueGate; context.gpuTimestamps = true;
    InteropBridge bridge(config, context);
    VkSemaphore gate{};
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; create.pNext = &type;
    check(owner.dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(owner.device, &create, nullptr, &gate), "Create slotted bridge gate");
    bool gateReleased{};
    const auto releaseGate = [&] {
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore = gate; signal.value = 1;
        return owner.dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(owner.device, &signal);
    };
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    const auto identify = [&](std::uint32_t frame) {
        GpuTimestampSample identity;
        identity.sourceEngineFrameKnown = identity.sdkPresentIdKnown = identity.sourceCopyTokenKnown = true;
        identity.sourceEngineFrame = frame; identity.sdkPresentId = frame + 100; identity.sourceCopyToken = frame + 200;
        bridge.associateGpuTimestampFrame(identity);
    };
    try {
        const std::uint64_t target = 1;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = 1; timeline.pWaitSemaphoreValues = &target;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
        wait.waitSemaphoreCount = 1; wait.pWaitSemaphores = &gate; wait.pWaitDstStageMask = &stage;
        {
            std::lock_guard lock(queueGate);
            check(owner.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(owner.queue, 1, &wait, VK_NULL_HANDLE), "Submit slotted bridge gate");
        }
        FrameCompletion first{{0,1}}, second{{1,2}};
        identify(71); bridge.publishQueuedNativeFrame(owner.frame, first.key, first);
        require(first.producedValid && first.producedValue == 1 && !first.d3dWaitQueued && !first.consumedValid,
                "Bridge publication must return its actual produced stage while the GPU is blocked");
        bool rejected{};
        try { bridge.publishQueuedNativeFrame(owner.frame, second.key, second); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected && detail::freshFrameCompletion(second) && first.producedValid && !first.consumedValid,
                "A missing consumed enqueue must reject the next publication without erasing the partial receipt");
        bridge.queueWaitForProduced(first);
        require(first.d3dWaitQueued && !first.consumedValid, "D3D produced Wait must be recorded before consumed Signal");
        bridge.queueSignalConsumed(first);
        identify(72); bridge.publishQueuedNativeFrame(owner.frame, second.key, second);
        bridge.queueWaitForProduced(second); bridge.queueSignalConsumed(second);
        require(second.producedValue == 2 && first.consumedValue == 1 && second.consumedValue == 2 &&
            bridge.consumedComplete(first.consumedValue) == FrameCompletionState::Pending &&
            bridge.consumedComplete(second.consumedValue) == FrameCompletionState::Pending,
            "Both K2 bridge command/query pairs must remain submitted without a per-frame CPU wait");
        FrameCompletion early{{0,3}};
        rejected = false;
        try { bridge.publishQueuedNativeFrame(owner.frame, early.key, early); }
        catch (const std::logic_error&) { rejected = true; }
        require(rejected && detail::freshFrameCompletion(early) &&
            bridge.consumedComplete(0) == FrameCompletionState::Invalid &&
            bridge.consumedComplete(UINT64_MAX) == FrameCompletionState::Invalid &&
            bridge.consumedComplete(3) == FrameCompletionState::Invalid,
            "Pending slot reuse and invalid timeline targets cannot become completion proofs");
        check(releaseGate(), "Release slotted bridge gate"); gateReleased = true;
        bridge.waitUntilConsumed();
        require(bridge.consumedComplete(first.consumedValue) == FrameCompletionState::Complete &&
            bridge.consumedComplete(second.consumedValue) == FrameCompletionState::Complete,
            "Retirement must use each recorded consumed target");
        auto sample = bridge.gpuTimestampSample();
        require(sample.available && sample.sourceEngineFrame == 72 && sample.sdkPresentId == 172 &&
            sample.sourceCopyToken == 272 && sample.sequence == 2,
            "Completed physical query pairs must preserve their original recording identity");
        identify(73); bridge.publishQueuedNativeFrame(owner.frame, early.key, early);
        bridge.queueWaitForProduced(early); bridge.queueSignalConsumed(early); bridge.waitUntilConsumed();
        sample = bridge.gpuTimestampSample();
        require(early.producedValue == 3 && early.consumedValue == 3 && sample.available && sample.sequence == 3 &&
            sample.sourceEngineFrame == 73 && sample.sdkPresentId == 173 && sample.sourceCopyToken == 273,
            "Wrapped slot reuse must retain its new query identity without borrowing the prior slot's sample");
        owner.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(owner.device, gate, nullptr); gate = VK_NULL_HANDLE;
    } catch (...) {
        if (gate) try {
            if ((gateReleased || releaseGate() == VK_SUCCESS) && owner.drainQueue(config.timeoutMs))
                owner.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(owner.device, gate, nullptr);
        } catch (...) {}
        throw;
    }
}

bool checkSharedPlaneRing(SourceOwner& owner, const xess_vk::InteropConfig& config, bool maskStorage) {
    using namespace xess_vk;
    const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    const auto hr = [](HRESULT result, const char* message) { if (FAILED(result)) throw std::runtime_error(message); };
    struct Resources {
        SourceOwner& owner;
        QueueGate gate;
        InteropBridge bridge;
        ID3D12CommandAllocator *allocator{}, *postAllocator{};
        ID3D12GraphicsCommandList *list{}, *postList{};
        ID3D12Fence *tail{}, *hold{};
        std::array<ID3D12Resource*,3> readbacks{};
        HANDLE event{};
        VkSemaphore vkHold{};
        std::uint64_t tailValue{};
        static BorrowedVulkanContext context(SourceOwner& owner, QueueGate& gate, bool maskStorage) {
            auto context = owner.context(); context.queueGate = &gate;
            if(maskStorage)context.getInstanceProcAddr=storageMaskedGet;
            return context;
        }
        Resources(SourceOwner& source, InteropConfig cfg, bool maskStorage) : owner(source), bridge(cfg, context(source,gate,maskStorage)) {}
        ~Resources() {
            for (auto* resource : readbacks) if (resource) resource->Release();
            if (list) list->Release(); if (allocator) allocator->Release();
            if (postList) postList->Release(); if (postAllocator) postAllocator->Release();
            if (tail) tail->Release(); if (hold) hold->Release();
            if (event) CloseHandle(event);
            if (vkHold) owner.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(owner.device,vkHold,nullptr);
        }
    };
    storageOriginalGet=owner.gipa;
    storageOriginalProperties=owner.inst<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
    auto resources = std::make_unique<Resources>(owner,config,maskStorage);
    auto& r = *resources;
    auto& bridge = r.bridge;
    bool depthStorage{};
    try {
        hr(bridge.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r.allocator)),"Create shared test allocator");
        hr(bridge.device()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r.allocator,nullptr,IID_PPV_ARGS(&r.list)),"Create shared test list");
        hr(r.list->Close(),"Close shared test initial list");
        hr(bridge.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r.postAllocator)),"Create shared test restore allocator");
        hr(bridge.device()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r.postAllocator,nullptr,IID_PPV_ARGS(&r.postList)),"Create shared test restore list");
        hr(r.postList->Close(),"Close shared test initial restore list");
        hr(bridge.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&r.tail)),"Create shared test tail");
        hr(bridge.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&r.hold)),"Create shared test hold");
        r.event = CreateEventW(nullptr,FALSE,FALSE,nullptr); require(r.event != nullptr,"Create shared test event");
        const auto input = config.extent(Format::DepthR32F);
        const auto changedWidth = input.width == 16384 ? input.width-1 : input.width+1;
        require(bridge.prepareSharedPlanes(input.width,input.height),"Initial shared ring preparation failed");
        depthStorage=bridge.sharedPlaneStats().depthStorage;
        require(!maskStorage||!depthStorage,"Unsupported STORAGE capability must select the existing transfer resources");
        std::array<SharedPlaneLease,3> leases;
        for (unsigned i=0;i<leases.size();++i) {
            require(bridge.tryReserveSharedFrame(input,true,leases[i])==SharedBorrowResult::Acquired && leases[i].key.index==i,
                    "N3 reservations must be independent of the two command slots");
            const auto depthDesc=bridge.texture(Format::DepthR32F,leases[i].key)->GetDesc();
            require(leases[i].depthStorage==depthStorage && bool(leases[i].depthStorageView)==depthStorage &&
                    bool(leases[i].planes[1].successfulImageCreateInfo.usage&VK_IMAGE_USAGE_STORAGE_BIT)==depthStorage &&
                    bool(depthDesc.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)==depthStorage,
                    "Storage lease, precreated view, actual Vulkan usage and D3D UAV resource flag must agree");
            require(!(leases[i].planes[0].successfulImageCreateInfo.usage&VK_IMAGE_USAGE_STORAGE_BIT) &&
                    !(leases[i].planes[2].successfulImageCreateInfo.usage&VK_IMAGE_USAGE_STORAGE_BIT),
                    "Storage capability must not change color or motion resource usage");
        }
        SharedPlaneLease missing;
        require(bridge.tryReserveSharedFrame(input,true,missing)==SharedBorrowResult::Unavailable && !missing.key.valid(),
                "A full shared ring must return an empty nonblocking fallback");
        const auto generation = leases[0].key.ringGeneration;
        require(!bridge.prepareSharedPlanes(changedWidth,input.height) && !bridge.sharedPlaneStats().ready &&
                bridge.sharedPlaneStats().ringGeneration==generation &&
                bridge.tryReserveSharedFrame(input,true,missing)==SharedBorrowResult::Unavailable,
                "Recorded references must pause and retain an extent-mismatched ring");
        require(bridge.cancelRecordedSharedFrame(leases[1].key) && !bridge.cancelRecordedSharedFrame(leases[1].key),
                "Only the exact unused recording may cancel a reservation once");
        require(bridge.prepareSharedPlanes(input.width,input.height),"Restoring the same extent must retain and resume its live ring");
        const auto write = [&](SharedPlaneLease& lease,unsigned mask,unsigned frame) {
            owner.wait();
            check(owner.dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(owner.command,0),"Reset shared writer");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(owner.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(owner.command,&begin),"Begin shared writer");
            std::array<VkClearColorValue,3> clear{};
            clear[0].float32[0]=(frame&1)?1.f:0.f; clear[0].float32[1]=(frame&2)?1.f:0.f;
            clear[0].float32[2]=(frame&4)?1.f:0.f; clear[0].float32[3]=1.f;
            clear[1].float32[0]=testing::kDepth[frame%4];
            clear[2].float32[0]=testing::kMotion[frame%4]; clear[2].float32[1]=testing::kMotion[(frame+1)%4];
            for (unsigned i=0;i<3;++i) if (mask&(1u<<i)) {
                auto& plane=lease.planes[i];
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.srcAccessMask=plane.ownerQueueFamily==owner.family?VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT:0;
                barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.oldLayout=plane.currentLayout;
                barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex=plane.ownerQueueFamily; barrier.dstQueueFamilyIndex=owner.family;
                barrier.image=plane.image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                owner.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(owner.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
                owner.dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(owner.command,plane.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear[i],1,&barrier.subresourceRange);
                barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
                barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
                barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
                owner.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(owner.command,VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
                plane.currentLayout=VK_IMAGE_LAYOUT_GENERAL; plane.ownerQueueFamily=owner.family;
            }
            check(owner.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(owner.command),"End shared writer");
            check(owner.dev<PFN_vkResetFences>("vkResetFences")(owner.device,1,&owner.completion),"Reset shared writer fence");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&owner.command;
            { std::lock_guard lock(r.gate); check(owner.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(owner.queue,1,&submit,owner.completion),"Submit shared writer"); }
            bridge.sharedWriteSubmitted(lease.key,mask);
        };
        // This is a component caller-tail witness, not an SDK Present or SDK-tag proof.
        const auto consume = [&](FrameCompletion& receipt,const FramePlanes& expected,bool blocked) {
            const bool direct=receipt.sharedKey.valid();
            const unsigned count=direct&&!receipt.sharedAuxiliary?1u:3u;
            if (!receipt.d3dWaitQueued) bridge.queueWaitForProduced(receipt);
            hr(r.allocator->Reset(),"Reset shared readback allocator"); hr(r.list->Reset(r.allocator,nullptr),"Reset shared readback list");
            std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,3> footprints{};
            for (unsigned i=0;i<count;++i) {
                if (r.readbacks[i]) { r.readbacks[i]->Release(); r.readbacks[i]=nullptr; }
                auto* texture=direct?bridge.texture(static_cast<Format>(i),receipt.sharedKey):bridge.texture(static_cast<Format>(i));
                const auto desc=texture->GetDesc(); UINT64 bytes{};
                bridge.device()->GetCopyableFootprints(&desc,0,1,0,&footprints[i],nullptr,nullptr,&bytes);
                D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=bytes;
                buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                hr(bridge.device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,IID_PPV_ARGS(&r.readbacks[i])),"Create shared readback");
                D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
                r.list->ResourceBarrier(1,&barrier);
                D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=texture; source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=r.readbacks[i];
                destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint=footprints[i];
                r.list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
                barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
                barrier.Transition.StateAfter=direct&&i?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_COMMON;
                r.list->ResourceBarrier(1,&barrier);
            }
            hr(r.list->Close(),"Close shared readback list"); ID3D12CommandList* lists[]{r.list};
            bridge.queue()->ExecuteCommandLists(1,lists); receipt.callerSubmitted=true;
            if (blocked) hr(bridge.queue()->Wait(r.hold,1),"Hold shared post-read tail");
            if (direct && receipt.sharedAuxiliary) {
                hr(r.postAllocator->Reset(),"Reset shared restore allocator"); hr(r.postList->Reset(r.postAllocator,nullptr),"Reset shared restore list");
                for (unsigned i=1;i<3;++i) {
                    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition={bridge.texture(static_cast<Format>(i),receipt.sharedKey),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON}; r.postList->ResourceBarrier(1,&barrier);
                }
                hr(r.postList->Close(),"Close shared restore list"); ID3D12CommandList* postLists[]{r.postList};
                bridge.queue()->ExecuteCommandLists(1,postLists);
                receipt.postPresentSubmitted=true;
            }
            if (direct) bridge.queueSignalSharedConsumed(receipt.sharedKey,receipt); else bridge.queueSignalConsumed(receipt);
            hr(bridge.queue()->Signal(r.tail,++r.tailValue),"Signal shared component tail");
            receipt.sdkTailValue=r.tailValue; receipt.sdkTailValid=true;
            if (blocked) {
                require(r.tail->GetCompletedValue()<r.tailValue &&
                    bridge.consumedComplete(receipt.consumedValue)==FrameCompletionState::Pending &&
                    !bridge.releaseSharedFrame(receipt.sharedKey,receipt),
                    "An enqueued shared post-read/consumed tail is not a completion proof while the real queue is held");
                hr(r.hold->Signal(1),"Release shared post-read hold");
            }
            detail::waitForD3D12Fence(r.tail,r.tailValue,r.event,config.timeoutMs,"Shared component tail");
            for (unsigned i=0;i<count;++i) {
                void* mapped{}; hr(r.readbacks[i]->Map(0,nullptr,&mapped),"Map shared readback");
                const auto extent=config.extent(static_cast<Format>(i)); const auto rowBytes=static_cast<std::size_t>(extent.width)*4;
                bool match=true;
                for (unsigned y=0;y<extent.height;++y)
                    match &= !std::memcmp(static_cast<const std::uint8_t*>(mapped)+footprints[i].Offset+y*footprints[i].Footprint.RowPitch,
                        expected[i].data()+y*rowBytes,rowBytes);
                D3D12_RANGE written{}; r.readbacks[i]->Unmap(0,&written); require(match,"Shared slot every-pixel readback mismatch");
            }
        };
        write(leases[0],7,17);
        FrameCompletion first{{0,1}}; first.sharedKey=leases[0].key; first.sharedAuxiliary=true;
        bridge.publishQueuedSharedFrame(leases[0].key,owner.queue,owner.completion,first.key,first);
        bridge.queueWaitForProduced(first);
        bool rejected{};
        try { bridge.queueSignalConsumed(first); } catch (const std::logic_error&) { rejected=true; }
        require(rejected && !first.consumedValid,"Direct shared inputs cannot use the legacy pre-Present consumed signal");
        consume(first,owner.expected(17),true);
        auto wrong=first; ++wrong.sharedKey.leaseGeneration;
        require(!bridge.releaseSharedFrame(leases[0].key,wrong),"A stale shared lease cannot retire the real publication");
        wrong=first; wrong.postPresentSubmitted=false;
        require(!bridge.releaseSharedFrame(leases[0].key,wrong) && bridge.releaseSharedFrame(leases[0].key,first) &&
            !bridge.releaseSharedFrame(leases[0].key,first),"Shared release requires its exact complete post-read receipt once");
        SharedPlaneLease wrapped,middle;
        require(bridge.tryReserveSharedFrame(input,true,wrapped)==SharedBorrowResult::Acquired && wrapped.key.index==0 &&
                wrapped.key.ringGeneration==first.sharedKey.ringGeneration && wrapped.key.leaseGeneration>first.sharedKey.leaseGeneration,
                "Shared slot wrap must advance its lease generation without borrowing an old frame identity");
        require(bridge.tryReserveSharedFrame(input,true,middle)==SharedBorrowResult::Acquired && middle.key.index==1,
                "The middle physical shared slot must remain independently writable");
        write(middle,7,19);
        FrameCompletion middleFrame{{1,2}}; middleFrame.sharedKey=middle.key; middleFrame.sharedAuxiliary=true;
        bridge.publishQueuedSharedFrame(middle.key,owner.queue,owner.completion,middleFrame.key,middleFrame);
        consume(middleFrame,owner.expected(19),false);
        require(bridge.releaseSharedFrame(middle.key,middleFrame),"Middle-slot pixels retire with their own exact identity");
        write(leases[2],7,22);
        FrameCompletion lastFrame{{0,3}}; lastFrame.sharedKey=leases[2].key; lastFrame.sharedAuxiliary=true;
        bridge.publishQueuedSharedFrame(leases[2].key,owner.queue,owner.completion,lastFrame.key,lastFrame);
        consume(lastFrame,owner.expected(22),false);
        require(bridge.releaseSharedFrame(leases[2].key,lastFrame),"Last-slot full-plane pixels retire with their own exact identity");
        write(wrapped,7,29);
        FrameCompletion off{{1,4}}; off.sharedKey=wrapped.key;
        bridge.publishQueuedSharedFrame(wrapped.key,owner.queue,owner.completion,off.key,off);
        consume(off,owner.expected(29),false);
        require(bridge.releaseSharedFrame(wrapped.key,off),"Wrapped auxiliary-capable shared OFF must release after its color Present tail");
        for (unsigned i=0;i<3;++i) require(bridge.tryReserveSharedFrame(input,true,leases[i])==SharedBorrowResult::Acquired,"Reborrow shared slots");
        require(leases[0].initializedMask==7 && leases[0].planes[0].ownerQueueFamily==VK_QUEUE_FAMILY_EXTERNAL &&
                leases[0].planes[1].ownerQueueFamily==owner.family && leases[0].planes[2].ownerQueueFamily==owner.family,
                "OFF releases color only and preserves the auxiliary Vulkan ownership for the next recording");
        for (auto& lease:leases) require(bridge.cancelRecordedSharedFrame(lease.key),"Cancel reborrowed recording");
        SharedPlaneLease colorOnly;
        require(bridge.tryReserveSharedFrame({},false,colorOnly)==SharedBorrowResult::Acquired &&
                !colorOnly.auxiliary && !colorOnly.depthStorage && !colorOnly.depthStorageView &&
                bridge.cancelRecordedSharedFrame(colorOnly.key),
                "Color-only reservations do not expose a storage depth view even in a capable full ring");
        owner.render(24);
        require(bridge.tryReserveSharedFrame(input,true,leases[0])==SharedBorrowResult::Acquired,"Reserve legacy shared source");
        write(leases[0],6,24);
        auto legacy=owner.frame; legacy.sharedSourceKey=leases[0].key;
        legacy.planes[1]=leases[0].planes[1]; legacy.planes[2]=leases[0].planes[2];
        FrameCompletion fallback{{0,5}};
        auto unkeyed=legacy; unkeyed.sharedSourceKey={}; rejected=false;
        try { bridge.publishQueuedNativeFrame(unkeyed,fallback.key,fallback); } catch (const std::logic_error&) { rejected=true; }
        require(rejected && detail::freshFrameCompletion(fallback),"Shared fallback images must carry their retained source lease");
        bridge.publishQueuedNativeFrame(legacy,fallback.key,fallback); consume(fallback,owner.expected(24),false);
        require(bridge.releaseSharedFrame(leases[0].key,fallback),"Legacy copies must keep the shared source pinned through their exact receipt");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,&type};
        check(owner.dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(owner.device,&create,nullptr,&r.vkHold),"Create shared abandon hold");
        owner.wait(); const std::uint64_t target=1; const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO}; timeline.waitSemaphoreValueCount=1; timeline.pWaitSemaphoreValues=&target;
        VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO,&timeline}; wait.waitSemaphoreCount=1; wait.pWaitSemaphores=&r.vkHold; wait.pWaitDstStageMask=&stage;
        { std::lock_guard lock(r.gate); check(owner.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(owner.queue,1,&wait,VK_NULL_HANDLE),"Submit shared abandon hold"); }
        require(bridge.tryReserveSharedFrame(input,true,leases[0])==SharedBorrowResult::Acquired,"Reserve shared abandoned submission");
        write(leases[0],6,29);
        require(!bridge.cancelRecordedSharedFrame(leases[0].key) && !bridge.abandonSubmittedSharedFrame(leases[0].key,owner.completion),
                "A submitted shared writer cannot cancel or abandon while its actual fence is pending");
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore=r.vkHold; signal.value=1;
        check(owner.dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(owner.device,&signal),"Release shared abandon hold"); owner.wait();
        require(bridge.abandonSubmittedSharedFrame(leases[0].key,owner.completion),"Completed unused same-queue shared writes may be abandoned");
        require(bridge.prepareSharedPlanes(changedWidth,input.height) && bridge.sharedPlaneStats().ringGeneration>generation &&
                bridge.sharedPlaneStats().leases==0,"Only a fully returned shared ring may rebuild its extent/generation");
        rejected=false; try { bridge.texture(Format::ColorRGBA8,first.sharedKey); } catch (const std::logic_error&) { rejected=true; }
        require(rejected && bridge.sharedPlaneStats().released==5 && bridge.sharedPlaneStats().abandoned==1 &&
            bridge.sharedPlaneStats().directFrames==4 && bridge.sharedPlaneStats().legacySourceFrames==1,
            "Old ring keys cannot access rebuilt resources and shared path accounting must match actual publications");
    } catch (...) {
        if (r.hold) r.hold->Signal(1);
        if (r.vkHold) { VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore=r.vkHold; signal.value=1;
            owner.dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(owner.device,&signal); }
        // Failed checks may follow real submissions; retain the complete component graph.
        resources.release(); throw;
    }
    return depthStorage;
}

void checkTailQueueDrain(SourceOwner& owner) {
    // Real GPU control: hold work after an already-completed producer fence.
    // A producer-only teardown would incorrectly conclude this queue is idle.
    owner.render(0); owner.wait();
    VkSemaphore gate = VK_NULL_HANDLE;
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; create.pNext = &type;
    check(owner.dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(owner.device, &create, nullptr, &gate), "vkCreateSemaphore(drain gate)");
    bool gateReleased = false;
    const auto releaseGate = [&] {
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO}; signal.semaphore = gate; signal.value = 1;
        return owner.dev<PFN_vkSignalSemaphore>("vkSignalSemaphore")(owner.device, &signal);
    };
    try {
        const uint64_t waitValue = 1;
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.waitSemaphoreValueCount = 1; timeline.pWaitSemaphoreValues = &waitValue;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.pNext = &timeline;
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &gate; submit.pWaitDstStageMask = &stage;
        check(owner.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(owner.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(blocked drain control)");
        if (owner.drainQueue(20)) throw std::runtime_error("Tail drain incorrectly completed through an unsignaled timeline wait");
        check(owner.dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(owner.device, owner.completion), "Producer fence must already be complete");
        if (!owner.drainFence || owner.dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(owner.device, owner.drainFence) != VK_NOT_READY)
            throw std::runtime_error("Timed-out tail fence was not retained pending GPU completion");
        check(releaseGate(), "vkSignalSemaphore(release drain control)");
        gateReleased = true;
        if (!owner.drainQueue(3000) || owner.drainFence) throw std::runtime_error("Tail drain did not complete after releasing timeline wait");
        owner.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(owner.device, gate, nullptr);
        gate = VK_NULL_HANDLE;
    } catch (...) {
        // Do not free the semaphore while its wait may still be pending.
        if (gate) {
            owner.retainForPossibleBorrowedChildren = true;
            try {
                if ((gateReleased || releaseGate() == VK_SUCCESS) && owner.drainQueue(3000)) {
                    owner.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(owner.device, gate, nullptr);
                    owner.retainForPossibleBorrowedChildren = false;
                }
            } catch (...) { /* retain the whole owner until process exit */ }
        }
        throw;
    }
}

template<class F> void expectInvalid(F&& operation, const char* name, uint32_t& count) {
    try { operation(); } catch (const std::invalid_argument&) { ++count; return; }
    throw std::runtime_error(std::string("Expected contract rejection: ") + name);
}
void checkLateFenceNotification(ID3D12Device* device) {
    ID3D12Fence* previous = nullptr;
    ID3D12Fence* target = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread signalThread;
    HRESULT firstSignal = E_FAIL, targetSignal = E_FAIL;
    auto cleanup = [&] {
        if (signalThread.joinable()) signalThread.join();
        if (previous) previous->Release();
        if (target) target->Release();
        if (event) CloseHandle(event);
    };
    try {
        if (!event || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&previous))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&target))))
            throw std::runtime_error("Unable to create real D3D12 fence notification regression test");
        bool firstTimedOut = false;
        try { xess_vk::detail::waitForD3D12Fence(previous, 1, event, 20, "Expected initial fence timeout"); }
        catch (const xess_vk::detail::FenceWaitTimeout&) { firstTimedOut = true; }
        if (!firstTimedOut) throw std::runtime_error("Unsignaled test fence incorrectly completed");
        // The first timed-out wait still has an event notification registered.
        // Wake it during the second wait, then complete the real target later.
        signalThread = std::thread([&] {
            Sleep(15); firstSignal = previous->Signal(1);
            Sleep(60); targetSignal = target->Signal(1);
        });
        xess_vk::detail::waitForD3D12Fence(target, 1, event, 1000, "Actual target after stale notification");
        const bool actualTargetCompleted = target->GetCompletedValue() >= 1;
        signalThread.join();
        if (!actualTargetCompleted || FAILED(firstSignal) || FAILED(targetSignal))
            throw std::runtime_error("Late notification from another fence incorrectly satisfied the target wait");
        // Deterministic stale-notification control: the target never reaches 2.
        // A scheduling delay cannot accidentally turn an early return into success.
        if (FAILED(previous->SetEventOnCompletion(2, event)) || FAILED(previous->Signal(2)))
            throw std::runtime_error("Unable to prime stale-only fence notification");
        bool staleOnlyTimedOut = false;
        try { xess_vk::detail::waitForD3D12Fence(target, 2, event, 20, "Stale-only notification must time out"); }
        catch (const xess_vk::detail::FenceWaitTimeout&) { staleOnlyTimedOut = true; }
        if (!staleOnlyTimedOut || target->GetCompletedValue() != 1)
            throw std::runtime_error("Stale event was accepted without target fence completion");
        cleanup();
    } catch (...) { cleanup(); throw; }
}
} // namespace

int main(int argc, char** argv) {
    uint32_t completed = 0;
    try {
        uint32_t frames = 24;
        bool corruptExpected = false;
        bool maskDepthStorage = false;
        xess_vk::InteropConfig cfg; cfg.timeoutMs = 3000;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--negative-mismatch") { corruptExpected = true; continue; }
            if (option == "--mask-depth-storage") { maskDepthStorage = true; continue; }
            if (option == "--help") {
                std::puts("Usage: xess-vulkan-native-test [--frames 24] [--width 127] [--height 73] [--input-width 0] [--input-height 0] [--timeout-ms 3000] [--negative-mismatch] [--mask-depth-storage]"); return 0;
            }
            if (++i >= argc) throw std::invalid_argument("Missing value after " + option);
            const std::string argument = argv[i];
            if (argument.empty() || argument.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("Expected unsigned integer");
            const auto parsed = std::stoull(argument);
            if (parsed > UINT32_MAX) throw std::invalid_argument("Numeric argument too large");
            if (option == "--frames") frames = static_cast<uint32_t>(parsed);
            else if (option == "--width") cfg.width = static_cast<uint32_t>(parsed);
            else if (option == "--height") cfg.height = static_cast<uint32_t>(parsed);
            else if (option == "--input-width") cfg.inputWidth = static_cast<uint32_t>(parsed);
            else if (option == "--input-height") cfg.inputHeight = static_cast<uint32_t>(parsed);
            else if (option == "--timeout-ms") cfg.timeoutMs = static_cast<uint32_t>(parsed);
            else throw std::invalid_argument("Unknown option: " + option);
        }
        if (!frames || frames > 1000000) throw std::invalid_argument("--frames must be 1..1000000");
        cfg.validate();
        SourceOwner owner; owner.initialize(cfg);
        checkTailQueueDrain(owner);
        owner.retainForPossibleBorrowedChildren = true;
        const auto context = owner.context();
        uint32_t rejected = 0;
        auto invalidDeviceInfo = owner.deviceInfo;
        invalidDeviceInfo.enabledExtensionCount = 1;
        auto badContext = context; badContext.successfulDeviceCreateInfo = &invalidDeviceInfo;
        expectInvalid([&] { xess_vk::InteropBridge invalid(cfg, badContext); }, "missing enabled extension", rejected);
        auto disabledTimeline = owner.timeline; disabledTimeline.timelineSemaphore = VK_FALSE;
        invalidDeviceInfo = owner.deviceInfo; invalidDeviceInfo.pNext = &disabledTimeline;
        expectInvalid([&] { xess_vk::InteropBridge invalid(cfg, badContext); }, "timeline not enabled", rejected);
        checkQueuedNativePublication(owner, cfg, false);
        checkQueuedNativePublication(owner, cfg, true);
        checkSlottedNativePublication(owner, cfg);
        const bool sharedDepthStorage=checkSharedPlaneRing(owner, cfg, maskDepthStorage);
        xess_vk::TransferStats stats;
        {
            xess_vk::InteropBridge bridge(cfg, context);
            checkLateFenceNotification(bridge.device());
            owner.render(0);
            auto bad = owner.frame; bad.planes[0].device = VK_NULL_HANDLE;
            expectInvalid([&] { bridge.publishNativeFrame(bad); }, "wrong source device", rejected);
            bad = owner.frame; bad.planes[0].successfulImageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            expectInvalid([&] { bridge.publishNativeFrame(bad); }, "missing TRANSFER_SRC", rejected);
            bad = owner.frame; bad.planes[0].currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            expectInvalid([&] { bridge.publishNativeFrame(bad); }, "undefined layout", rejected);
            bad = owner.frame; bad.planes[0].currentLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
            expectInvalid([&] { bridge.publishNativeFrame(bad); }, "preinitialized layout", rejected);
            bad = owner.frame; bad.planes[0].ownerQueueFamily = VK_QUEUE_FAMILY_EXTERNAL;
            expectInvalid([&] { bridge.publishNativeFrame(bad); }, "external source ownership", rejected);
            bad = owner.frame; bad.producerCompleteFence = VK_NULL_HANDLE;
            expectInvalid([&] { bridge.publishNativeFrame(bad); }, "unknown producer completion", rejected);
            bad = owner.frame; bad.planes[1].successfulImageCreateInfo.mipLevels = 2;
            expectInvalid([&] { bridge.publishNativeFrame(bad); }, "multiple mip levels", rejected);
            bad = owner.frame; bad.planes[2].successfulImageCreateInfo.samples = VK_SAMPLE_COUNT_4_BIT;
            expectInvalid([&] { bridge.publishNativeFrame(bad); }, "multisampled source", rejected);
            bad = owner.frame; ++bad.planes[0].successfulImageCreateInfo.extent.width;
            expectInvalid([&] { bridge.publishNativeFrame(bad); }, "extent mismatch", rejected);
            bad = owner.frame; ++bad.planes[1].successfulImageCreateInfo.extent.width;
            expectInvalid([&] { bridge.publishNativeFrame(bad); }, "depth extent mismatch", rejected);
            bad = owner.frame; ++bad.planes[2].successfulImageCreateInfo.extent.height;
            expectInvalid([&] { bridge.publishNativeFrame(bad); }, "motion extent mismatch", rejected);
            for (uint32_t index = 0; index < frames; ++index) {
                owner.render(index);
                bridge.publishNativeFrame(owner.frame);
                // publish returned after restoring source layouts. Overwrite all
                // sources on the GPU before D3D12 consumes the preserved copy.
                owner.render(index + 997);
                auto expected = owner.expected(index);
                if (corruptExpected && index == 0) expected[0][0] ^= 0xff;
                bridge.verifyFrame(expected); ++completed;
            }
            stats = bridge.stats();
            if (stats.cpuUploadFrames != 0 || stats.nativeImageFrames != frames || stats.framesVerified != frames)
                throw std::runtime_error("Unexpected CPU-upload path or missing native frames");
            const uint64_t bytesPerFrame = cfg.byteSize(xess_vk::Format::ColorRGBA8) + cfg.byteSize(xess_vk::Format::DepthR32F) + cfg.byteSize(xess_vk::Format::MotionRG16F);
            if (stats.bytesVerified != bytesPerFrame * frames) throw std::runtime_error("Per-plane readback byte total mismatch");
            bridge.waitUntilConsumed();
        }
        owner.retainForPossibleBorrowedChildren = false;
        // A real GPU submission after bridge destruction proves caller-owned
        // device, command pool, source images, queue and fence are still usable.
        owner.render(1023); owner.wait();
        VkSemaphore extra = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(owner.dev<PFN_vkCreateSemaphore>("vkCreateSemaphore")(owner.device, &info, nullptr, &extra), "vkCreateSemaphore(after bridge destruction)");
        owner.dev<PFN_vkDestroySemaphore>("vkDestroySemaphore")(owner.device, extra, nullptr);
        const auto input = cfg.extent(xess_vk::Format::DepthR32F);
        std::printf("{\"stage\":\"borrowed-Vulkan-native-image-copy\",\"passed\":true,\"framesVerified\":%llu,\"bytesVerified\":%llu,"
                    "\"width\":%u,\"height\":%u,\"inputWidth\":%u,\"inputHeight\":%u,"
                    "\"nativeImageFrames\":%llu,\"cpuUploadFrames\":%llu,\"negativeValidationChecks\":%u,\"sourceGeneratedByGpuClear\":true,"
                    "\"sourcesReusedBeforeD3D12Readback\":true,\"borrowedDeviceAliveAfterBridgeDestruction\":true,\"everyPixelCompared\":true,"
                    "\"lateFenceNotificationRegressionPassed\":true,\"queueTailDrainRegressionPassed\":true,"
                    "\"queuedColorProducerPendingPassed\":true,\"queuedColorReadbackPassed\":true,"
                    "\"queuedBundleProducerPendingPassed\":true,\"queuedBundleThreePlaneReadbackPassed\":true,"
                    "\"slottedK2PendingPassed\":true,\"slottedPartialReceiptPassed\":true,\"slottedQueryIdentityPassed\":true,"
                    "\"sharedN3EveryPixelPassed\":true,\"sharedPhysicalIndicesPixelVerified\":[0,1,2],\"sharedWrappedLeasePassed\":true,"
                    "\"sharedNonblockingFallbackPassed\":true,\"sharedPausedResizePassed\":true,"
                    "\"sharedLegacyLeasePassed\":true,\"sharedAuxToOffPassed\":true,\"sharedPendingConsumedPassed\":true,"
                    "\"sharedAbandonFencePassed\":true,\"sharedDepthStorage\":%s,\"sharedDepthStorageCapabilityMasked\":%s,"
                    "\"sharedStorageResourceContractPassed\":true,\"sharedSdkExecuted\":false,\"generatedFramesVerified\":false}\n",
                    static_cast<unsigned long long>(stats.framesVerified), static_cast<unsigned long long>(stats.bytesVerified),
                    cfg.width, cfg.height, input.width, input.height,
                    static_cast<unsigned long long>(stats.nativeImageFrames), static_cast<unsigned long long>(stats.cpuUploadFrames), rejected,
                    sharedDepthStorage?"true":"false",maskDepthStorage?"true":"false");
        return 0;
    } catch (const std::exception& error) {
        std::cout << "{\"stage\":\"borrowed-Vulkan-native-image-copy\",\"passed\":false,\"framesVerified\":" << completed
                  << ",\"error\":" << quote(error.what()) << ",\"generatedFramesVerified\":false}\n"; return 1;
    }
}
