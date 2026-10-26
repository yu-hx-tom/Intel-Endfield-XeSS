#include "continuous_provider.hpp"
#include "depth_snapshot_spirv.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <windows.h>

namespace xess_vk::continuous {
namespace {
template <class T> T handle(std::uint64_t n) {
    return reinterpret_cast<T>(n);
}
void check(VkResult r) {
    if (r != VK_SUCCESS)
        throw std::runtime_error("Continuous input resource creation failed");
}
VkImageMemoryBarrier imageBarrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout before,
                                  VkImageLayout after, VkAccessFlags source,
                                  VkAccessFlags destination) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    b.oldLayout = before;
    b.newLayout = after;
    b.srcAccessMask = source;
    b.dstAccessMask = destination;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    return b;
}
bool sourceValid(const descriptor_image_metadata::ImageBinding &b, VkFormat format) {
    const auto &d = b.imageDescription;
    const auto aspect = format == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                               : VK_IMAGE_ASPECT_COLOR_BIT;
    return b.image.handle && b.image.generation && b.viewType == VK_IMAGE_VIEW_TYPE_2D &&
           b.viewFormat == format && d.format == static_cast<unsigned>(format) && d.width &&
           d.height && d.depth == 1 && d.mipLevels == 1 && d.arrayLayers == 1 &&
           d.samples == VK_SAMPLE_COUNT_1_BIT && d.tiling == VK_IMAGE_TILING_OPTIMAL &&
           d.sharingMode == VK_SHARING_MODE_EXCLUSIVE &&
           (d.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && b.range.aspectMask == aspect &&
           b.range.baseMipLevel == 0 && b.range.levelCount == 1 && b.range.baseArrayLayer == 0 &&
           b.range.layerCount == 1 &&
           (b.declaredLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
            b.declaredLayout == VK_IMAGE_LAYOUT_GENERAL ||
            (format == VK_FORMAT_D32_SFLOAT_S8_UINT &&
             b.declaredLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL));
}
} // namespace
struct Provider::Impl {
    struct Image {
        VkImage image{};
        VkDeviceMemory allocation{};
    };
    struct Slot {
        State state{State::Free};
        std::uint64_t generation{}, invocation{}, submission{}, submitSerial{}, viewport{}, pool{};
        RecordingKey recording;
        std::array<VkSemaphore, maximumSignals> signals{};
        std::size_t signalCount{};
        Image depth, motion;
        VkBuffer intermediate{};
        VkDeviceMemory intermediateAllocation{};
        VkExtent2D extent{};
        bool initialized{}, queuedLease{},depthCompute{};
        VkDescriptorSet depthSet{};
        frame_input::FrameConstantsSample constants;
        FrameTimingTicket timing;
        std::uint32_t engineFrame{};
        SharedPlaneLease shared;
        measurement::Commands commands{};
        VkQueryPool queries{};
        bool queryRecorded{},queryFailed{};
        GpuTimestampSample querySample;
    };
    Config config;
    mutable std::mutex mutex;
    std::array<SourcePair, invocationCapacity> invocations{};
    std::array<bool, invocationCapacity> ambiguousInvocations{};
    std::array<Slot, slotCount+sharedPlaneSlotCount> slots{};
    SharedInputAccess sharedInputs;
    bool sharedBorrowing{};
    VkDescriptorSetLayout depthSetLayout{};
    VkPipelineLayout depthPipelineLayout{};
    VkPipeline depthPipeline{};
    VkDescriptorPool depthPool{};
    bool computeAllowed{},computeReady{},computeFailed{};
    Stats totals;
    std::uint64_t serial{}, viewportSerial{}, successfulSubmitSerial{};
    VkExtent2D lastExtent{};
    PFN_vkCreateQueryPool createQueries{};
    PFN_vkDestroyQueryPool destroyQueries{};
    PFN_vkCmdResetQueryPool resetQueries{};
    PFN_vkCmdWriteTimestamp writeTimestamp{};
    PFN_vkGetQueryPoolResults readQueries{};
    std::uint64_t queryContext{},querySerial{},qpcFrequency{},lastSubmissionNs{};
    FrameTimingTicket lastSubmissionTicket;
    void prepareQueries(Slot& s) {
        if(!createQueries||s.queries||s.queryFailed)return;
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=4;
        VkQueryPool created{};
        if(createQueries(config.device,&info,nullptr,&created)!=VK_SUCCESS||!created){s.queries={};s.queryFailed=true;++totals.timestampFailures;}
        else s.queries=created;
    }
    void completeQueries(Slot& s) {
        if(!s.queryRecorded)return;s.queryRecorded=false;
        std::array<std::uint64_t,8> values{};
        const auto result=readQueries(config.device,s.queries,0,4,sizeof(values),values.data(),2*sizeof(std::uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if(result!=VK_SUCCESS||!values[1]||!values[3]||!values[5]||!values[7]){++totals.timestampUnavailable;return;}
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        auto sample=s.querySample;sample.available=true;sample.queryReadAttempted=true;sample.queryResultKnown=true;sample.queryResult=result;
        sample.queryAvailabilityKnown=true;sample.beginAvailability=sample.endAvailability=1;sample.fenceObservedQpcTicks=now.QuadPart;
        const bool wrapBounded=sample.validBits==64||sample.submitQpcTicks;
        sample.beginTicks=values[0];sample.endTicks=values[2];if(wrapBounded)timestampDuration(sample);totals.depthGpu=sample;
        if(sample.durationKnown)measurement::observe(s.depthCompute?measurement::Metric::ProviderDepthGpu:measurement::Metric::ProviderDepthTransferGpu,
            static_cast<std::uint64_t>(sample.durationNs),sample.sdkPresentId,sample.contextGeneration);
        sample.beginTicks=values[4];sample.endTicks=values[6];if(wrapBounded)timestampDuration(sample);totals.auxGpu=sample;
        if(sample.durationKnown)measurement::observe(measurement::Metric::ProviderAuxGpu,static_cast<std::uint64_t>(sample.durationNs),sample.sdkPresentId,sample.contextGeneration);
        if(!sample.durationKnown)++totals.timestampUnavailable;else ++totals.timestampCompleted;
    }
    Slot* ticketSlot(Ticket ticket) {
        if(ticket.slot >= (ticket.shared?sharedPlaneSlotCount:slotCount))return nullptr;
        return &slots[ticket.slot+(ticket.shared?slotCount:0)];
    }
    bool freeShared(Slot& slot,VkFence completed = VK_NULL_HANDLE,const FrameCompletion* receipt = nullptr) {
        if(!slot.shared.key.valid())return true;
        bool released{};
        if(receipt)released=sharedInputs.release(sharedInputs.context,slot.shared.key,*receipt);
        else if(slot.state==State::Recorded)released=sharedInputs.cancel(sharedInputs.context,slot.shared.key);
        else if(slot.state==State::Submitted)released=sharedInputs.abandon(sharedInputs.context,slot.shared.key,completed);
        if(released)slot.shared={};
        else ++totals.sharedFailures;
        return released;
    }
    void retireOlder(std::uint64_t cutoff, FrameTimingAssociation frame,VkFence completed) {
        if (!frame.valid || !frame.ticket) return;
        for (auto& slot : slots)
            if (slot.state == State::Submitted && slot.submitSerial <= cutoff &&
                slot.timing.contextGeneration == frame.ticket.contextGeneration &&
                slot.timing.sdkId < frame.ticket.sdkId) {
                if(!freeShared(slot,completed))continue;
                completeQueries(slot);
                slot.state = State::Free;
                slot.initialized = true;
                slot.signalCount = 0;
                ++totals.cancelled;
            }
    }
    Bundle bundle(Slot& slot) {
        Bundle result;
        const auto index=static_cast<std::uint32_t>(&slot-slots.data());
        result.ticket = {index>=slotCount?index-static_cast<std::uint32_t>(slotCount):index,slot.generation,index>=slotCount};
        if(slot.shared.key.valid()) {
            result.shared=slot.shared;
            result.depth=slot.shared.planes[1];result.motion=slot.shared.planes[2];
            for(auto* image:{&result.depth,&result.motion}) {
                image->currentLayout=VK_IMAGE_LAYOUT_GENERAL;image->ownerQueueFamily=config.queueFamily;
            }
        } else {
            result.depth = source(slot.depth, VK_FORMAT_R32_SFLOAT, slot.extent);
            result.motion = source(slot.motion, VK_FORMAT_R16G16_SFLOAT, slot.extent);
        }
        result.constants = slot.constants;
        result.timing = slot.timing;
        result.engineFrame = slot.engineFrame;
        result.invocation = slot.invocation;
        result.submission = slot.submission;
        result.ready = true;
        result.depthCompute=slot.depthCompute;
        result.commands=slot.commands;
        return result;
    }
    template <class T> T fn(const char *name) const {
        const auto p = config.functions(config.device, name);
        if (!p)
            throw std::runtime_error(name);
        return reinterpret_cast<T>(p);
    }
    explicit Impl(Config c) : config(c) {
        if (!c.device || !c.queue || !c.functions || !c.memory.memoryTypeCount)
            throw std::invalid_argument("Continuous provider requires a borrowed device/queue");
        (void)fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        (void)fn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
        (void)fn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
        (void)fn<PFN_vkCmdCopyImage>("vkCmdCopyImage");
        if(c.gpuTimestamps&&c.timestampPeriodNs>0&&c.timestampValidBits&&c.timestampValidBits<=64)try {
            createQueries=fn<PFN_vkCreateQueryPool>("vkCreateQueryPool");destroyQueries=fn<PFN_vkDestroyQueryPool>("vkDestroyQueryPool");
            resetQueries=fn<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool");writeTimestamp=fn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp");
            readQueries=fn<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults");
            LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);qpcFrequency=frequency.QuadPart;
            static std::atomic<std::uint64_t> contexts{};queryContext=++contexts;
        }catch(...){createQueries=nullptr;++totals.timestampFailures;}
    }
    std::uint32_t memoryType(std::uint32_t bits) const {
        for (std::uint32_t i = 0; i < config.memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) &&
                (config.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                return i;
        throw std::runtime_error("No device-local continuous input memory");
    }
    VkDeviceMemory allocate(VkMemoryRequirements r) {
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = r.size;
        a.memoryTypeIndex = memoryType(r.memoryTypeBits);
        VkDeviceMemory memory{};
        check(fn<PFN_vkAllocateMemory>("vkAllocateMemory")(config.device, &a, nullptr, &memory));
        return memory;
    }
    void destroy(Slot &s) {
        if(s.queries){destroyQueries(config.device,s.queries,nullptr);s.queries={};s.queryRecorded=false;}
        for (auto *image : {&s.depth, &s.motion}) {
            if (image->image)
                fn<PFN_vkDestroyImage>("vkDestroyImage")(config.device, image->image, nullptr);
            if (image->allocation)
                fn<PFN_vkFreeMemory>("vkFreeMemory")(config.device, image->allocation, nullptr);
            *image = {};
        }
        if (s.intermediate)
            fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(config.device, s.intermediate, nullptr);
        if (s.intermediateAllocation)
            fn<PFN_vkFreeMemory>("vkFreeMemory")(config.device, s.intermediateAllocation, nullptr);
        s.intermediate = VK_NULL_HANDLE;
        s.intermediateAllocation = VK_NULL_HANDLE;
        s.initialized = false;
    }
    void createImage(Image &target, VkFormat format, VkExtent2D extent) {
        VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        i.imageType = VK_IMAGE_TYPE_2D;
        i.format = format;
        i.extent = {extent.width, extent.height, 1};
        i.mipLevels = i.arrayLayers = 1;
        i.samples = VK_SAMPLE_COUNT_1_BIT;
        i.tiling = VK_IMAGE_TILING_OPTIMAL;
        i.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT;
        i.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(fn<PFN_vkCreateImage>("vkCreateImage")(config.device, &i, nullptr, &target.image));
        VkMemoryRequirements r{};
        fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(config.device,
                                                                             target.image, &r);
        target.allocation = allocate(r);
        check(fn<PFN_vkBindImageMemory>("vkBindImageMemory")(config.device, target.image,
                                                             target.allocation, 0));
    }
    void prepare(Slot &s, VkExtent2D extent,bool sharedCarrier = false) {
        if ((sharedCarrier || s.shared.key.valid() || (s.depth.image && s.motion.image)) && s.intermediate && s.extent.width == extent.width &&
            s.extent.height == extent.height)
            return;
        destroy(s);
        s.extent = extent;
        if(!sharedCarrier&&!s.shared.key.valid()) {
            createImage(s.depth, VK_FORMAT_R32_SFLOAT, extent);
            createImage(s.motion, VK_FORMAT_R16G16_SFLOAT, extent);
        }
        VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        b.size = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
        b.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        b.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(
            fn<PFN_vkCreateBuffer>("vkCreateBuffer")(config.device, &b, nullptr, &s.intermediate));
        VkMemoryRequirements r{};
        fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(config.device,
                                                                               s.intermediate, &r);
        s.intermediateAllocation = allocate(r);
        check(fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(config.device, s.intermediate,
                                                               s.intermediateAllocation, 0));
        prepareQueries(s);
    }
    void destroyCompute() {
        if(depthPool)fn<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(config.device,depthPool,nullptr);
        if(depthPipeline)fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(config.device,depthPipeline,nullptr);
        if(depthPipelineLayout)fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(config.device,depthPipelineLayout,nullptr);
        if(depthSetLayout)fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(config.device,depthSetLayout,nullptr);
        depthPool={};depthPipeline={};depthPipelineLayout={};depthSetLayout={};computeReady=false;
        for(auto& slot:slots)slot.depthSet={};
    }
    void prepareCompute() {
        if(computeReady||computeFailed)return;
        VkShaderModule shader{};
        try {
            for(const auto* name:{"vkCmdBindPipeline","vkCmdBindDescriptorSets","vkCmdDispatch","vkUpdateDescriptorSets",
                "vkDestroyShaderModule","vkDestroyDescriptorPool","vkDestroyPipeline","vkDestroyPipelineLayout","vkDestroyDescriptorSetLayout"})
                (void)fn<PFN_vkVoidFunction>(name);
            const std::array<VkDescriptorSetLayoutBinding,2> bindings{{
                {0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
                {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
            VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layout.bindingCount=bindings.size();layout.pBindings=bindings.data();
            check(fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(config.device,&layout,nullptr,&depthSetLayout));
            VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pipelineLayout.setLayoutCount=1;pipelineLayout.pSetLayouts=&depthSetLayout;
            check(fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(config.device,&pipelineLayout,nullptr,&depthPipelineLayout));
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module.codeSize=sizeof(depthSnapshotSpirv);module.pCode=depthSnapshotSpirv;
            check(fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(config.device,&module,nullptr,&shader));
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline.layout=depthPipelineLayout;
            pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
            pipeline.stage.module=shader;pipeline.stage.pName="main";
            check(fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(config.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&depthPipeline));
            const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,sharedPlaneSlotCount},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,sharedPlaneSlotCount}}};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=sharedPlaneSlotCount;
            pool.poolSizeCount=sizes.size();pool.pPoolSizes=sizes.data();
            check(fn<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(config.device,&pool,nullptr,&depthPool));
            std::array<VkDescriptorSetLayout,sharedPlaneSlotCount> layouts;layouts.fill(depthSetLayout);
            std::array<VkDescriptorSet,sharedPlaneSlotCount> descriptors{};
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=depthPool;
            allocate.descriptorSetCount=layouts.size();allocate.pSetLayouts=layouts.data();
            check(fn<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(config.device,&allocate,descriptors.data()));
            for(unsigned i=0;i<descriptors.size();++i)slots[slotCount+i].depthSet=descriptors[i];
            computeReady=true;
        } catch(...) {
            destroyCompute();computeFailed=true;++totals.depthComputeFailures;
        }
        if(shader)fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(config.device,shader,nullptr);
    }
    bool computeFor(Slot& slot,const descriptor_image_metadata::ImageBinding& depth,
                    const descriptor_image_metadata::DispatchEvidence& evidence) {
        if(!computeAllowed||!computeReady||!slot.depthSet||!slot.shared.depthStorage||!slot.shared.depthStorageView||
           !evidence.computeRestoreKnown||!evidence.pipeline.handle||!evidence.pipeline.generation||
           !evidence.pipelineLayout.handle||!evidence.pipelineLayout.generation||!evidence.computeRestoreSetCount||
           evidence.computeRestoreSetCount>evidence.computeRestoreSets.size()||!depth.view.handle||!depth.view.generation||
           !(depth.imageDescription.usage&VK_IMAGE_USAGE_SAMPLED_BIT)||slot.extent.width>65535u*8||slot.extent.height>65535u*8||
           depth.components.r!=VK_COMPONENT_SWIZZLE_IDENTITY||depth.components.g!=VK_COMPONENT_SWIZZLE_IDENTITY||
           depth.components.b!=VK_COMPONENT_SWIZZLE_IDENTITY||depth.components.a!=VK_COMPONENT_SWIZZLE_IDENTITY)return false;
        for(unsigned i=0;i<evidence.computeRestoreSetCount;++i)
            if(!evidence.computeRestoreSets[i].handle||!evidence.computeRestoreSets[i].generation)return false;
        const std::array<VkDescriptorImageInfo,2> images{{{VK_NULL_HANDLE,handle<VkImageView>(depth.view.handle),depth.declaredLayout},
            {VK_NULL_HANDLE,slot.shared.depthStorageView,VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet,2> writes{};
        for(unsigned i=0;i<2;++i){auto& write=writes[i];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=slot.depthSet;
            write.dstBinding=i;write.descriptorCount=1;write.descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=&images[i];}
        fn<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(config.device,writes.size(),writes.data(),0,nullptr);
        return true;
    }
    void copy(Slot &s, VkCommandBuffer command,
              const descriptor_image_metadata::ImageBinding &depth,
              const descriptor_image_metadata::ImageBinding &motion,
              const descriptor_image_metadata::DispatchEvidence& evidence) {
        const bool compute=s.depthCompute;
        s.commands={};prepareQueries(s);
        if(s.queries){resetQueries(command,s.queries,0,4);writeTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,s.queries,2);}
        const auto sourceAspects = config.separateDepthStencilLayouts
                                       ? VK_IMAGE_ASPECT_DEPTH_BIT
                                       : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        const auto previous =
            s.initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        const bool shared=s.shared.key.valid();
        const auto depthTarget=shared?s.shared.planes[1].image:s.depth.image;
        const auto motionTarget=shared?s.shared.planes[2].image:s.motion.image;
        std::array before{imageBarrier(handle<VkImage>(depth.image.handle), sourceAspects,
                                       depth.declaredLayout, compute?depth.declaredLayout:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_ACCESS_MEMORY_WRITE_BIT, compute?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_TRANSFER_READ_BIT),
                          imageBarrier(handle<VkImage>(motion.image.handle),
                                       VK_IMAGE_ASPECT_COLOR_BIT, motion.declaredLayout,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
                          imageBarrier(depthTarget, VK_IMAGE_ASPECT_COLOR_BIT, shared?s.shared.planes[1].currentLayout:previous,
                                       compute?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_ACCESS_TRANSFER_READ_BIT, compute?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT),
                          imageBarrier(motionTarget, VK_IMAGE_ASPECT_COLOR_BIT, shared?s.shared.planes[2].currentLayout:previous,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)};
        if(shared)for(unsigned i=1;i<=2;++i) {
            if(s.shared.planes[i].ownerQueueFamily==VK_QUEUE_FAMILY_EXTERNAL) {
                before[i+1].srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
                before[i+1].dstQueueFamilyIndex=config.queueFamily;
                before[i+1].srcAccessMask=0;
            }
        }
        VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        buffer.buffer = s.intermediate;
        buffer.size = VK_WHOLE_SIZE;
        buffer.srcQueueFamilyIndex = buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buffer.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        buffer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        auto barrier = fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        if(compute) {
            const std::array depthBefore{before[0],before[2]};
            barrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,depthBefore.size(),depthBefore.data());
        } else barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                nullptr, 1, &buffer, 4, before.data());
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        c.imageExtent = {s.extent.width, s.extent.height, 1};
        if(s.queries)writeTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,s.queries,0);
        if(compute) {
            auto bindPipeline=fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");auto bindSets=fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
            auto dispatch=fn<PFN_vkCmdDispatch>("vkCmdDispatch");
            std::array<VkDescriptorSet,descriptor_image_metadata::maxBoundSets> restore{};
            for(unsigned i=0;i<evidence.computeRestoreSetCount;++i)restore[i]=handle<VkDescriptorSet>(evidence.computeRestoreSets[i].handle);
            const auto restoreGame=[&] {
                bindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,handle<VkPipeline>(evidence.pipeline.handle));
                bindSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,handle<VkPipelineLayout>(evidence.pipelineLayout.handle),0,evidence.computeRestoreSetCount,restore.data(),0,nullptr);
            };
            try {
                bindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,depthPipeline);
                bindSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,depthPipelineLayout,0,1,&s.depthSet,0,nullptr);
                dispatch(command,(s.extent.width+7)/8,(s.extent.height+7)/8,1);
                ++s.commands[static_cast<unsigned>(measurement::Command::DepthDispatch)];
                if(s.queries)writeTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.queries,1);
            } catch(...) {restoreGame();throw;}
            restoreGame();
            const std::array motionBefore{before[1],before[3]};
            barrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,motionBefore.size(),motionBefore.data());
        } else {
        fn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
            command, handle<VkImage>(depth.image.handle), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            s.intermediate, 1, &c);
        ++s.commands[static_cast<unsigned>(measurement::Command::ImageToBuffer)];
        buffer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        buffer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                nullptr, 1, &buffer, 0, nullptr);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        fn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(
            command, s.intermediate, depthTarget, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        ++s.commands[static_cast<unsigned>(measurement::Command::BufferToImage)];
        if(s.queries)writeTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.queries,1);
        }
        VkImageCopy m{};
        m.srcSubresource = m.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        m.extent = c.imageExtent;
        fn<PFN_vkCmdCopyImage>("vkCmdCopyImage")(
            command, handle<VkImage>(motion.image.handle), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            motionTarget, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &m);
        ++s.commands[static_cast<unsigned>(measurement::Command::ImageCopy)];
        std::array after{
            imageBarrier(handle<VkImage>(depth.image.handle), sourceAspects,
                         compute?depth.declaredLayout:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth.declaredLayout,
                         compute?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT),
            imageBarrier(handle<VkImage>(motion.image.handle), VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, motion.declaredLayout,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT),
            imageBarrier(depthTarget, VK_IMAGE_ASPECT_COLOR_BIT,
                         compute?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, shared?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         compute?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            imageBarrier(motionTarget, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, shared?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT)};
        barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT|(compute?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                nullptr, 0, nullptr, 4, after.data());
        if(s.queries){
            writeTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.queries,3);s.queryRecorded=true;
            auto& sample=s.querySample;sample={};sample.requested=sample.queryStateKnown=sample.diagnosticActive=true;
            sample.sequence=++querySerial;sample.contextGeneration=queryContext;sample.generation=s.generation;
            sample.sourceEngineFrame=s.engineFrame;sample.sourceEngineFrameKnown=true;
            sample.sdkPresentId=s.timing.sdkId;sample.sdkPresentIdKnown=bool(s.timing);
            sample.validBits=config.timestampValidBits;sample.timestampPeriodNs=config.timestampPeriodNs;
        }
    }
    NativeImageSource source(const Image &image, VkFormat format, VkExtent2D extent) const {
        NativeImageSource out;
        out.device = config.device;
        out.image = image.image;
        out.ownerQueueFamily = config.queueFamily;
        out.currentLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        auto &i = out.successfulImageCreateInfo;
        i.imageType = VK_IMAGE_TYPE_2D;
        i.format = format;
        i.extent = {extent.width, extent.height, 1};
        i.mipLevels = i.arrayLayers = 1;
        i.samples = VK_SAMPLE_COUNT_1_BIT;
        i.tiling = VK_IMAGE_TILING_OPTIMAL;
        i.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT;
        i.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        return out;
    }
};
bool requested() noexcept {
    wchar_t v[8]{};
    return GetEnvironmentVariableW(L"XESS_CONTINUOUS_INPUTS", v, 8) == 1 && v[0] == L'1';
}
Provider::Provider(Config c) : impl_(std::make_unique<Impl>(c)) {}
Provider::~Provider() {
    if (std::any_of(impl_->slots.begin(), impl_->slots.end(),
                    [](const auto &s) { return s.state != State::Free; })) {
        impl_.release();
        return;
    }
    for (auto &s : impl_->slots)
        impl_->destroy(s);
    impl_->destroyCompute();
}
bool Provider::setSharedInputs(SharedInputAccess access) {
    std::lock_guard lock(impl_->mutex);
    const bool live=std::any_of(impl_->slots.begin(),impl_->slots.end(),[](const auto& s){return s.shared.key.valid();});
    if(!access.context) {
        impl_->sharedBorrowing=false;
        if(live)return false;
        impl_->sharedInputs={};return true;
    }
    if(!access.reserve||!access.submitted||!access.cancel||!access.abandon||!access.release||!access.poison||
       (live&&access.context!=impl_->sharedInputs.context))return false;
    impl_->sharedInputs=access;impl_->sharedBorrowing=true;return true;
}
bool Provider::prepareSharedInputs(VkExtent2D extent,bool depthStorage) {
    std::lock_guard lock(impl_->mutex);
    if(!impl_->sharedBorrowing||!extent.width||!extent.height)return false;
    impl_->computeAllowed=impl_->config.depthCompute&&depthStorage;
    if(impl_->computeAllowed)impl_->prepareCompute();
    bool ready=true;
    for(std::size_t i=slotCount;i<impl_->slots.size();++i) {
        auto& slot=impl_->slots[i];if(slot.state!=State::Free)continue;
        try {impl_->prepare(slot,extent,true);}
        catch(...) {
            try {impl_->destroy(slot);}catch(...) {slot.state=State::Uncertain;}
            ++impl_->totals.sharedFailures;ready=false;
        }
    }
    return ready;
}
void Provider::invocation(SourcePair pair) {
    std::lock_guard lock(impl_->mutex);
    ++impl_->totals.invocations;
    if (!pair.frameValid || !pair.timing) {
        // An unkeyed failure cannot identify which pending CPU source is stale.
        // Do not disturb GPU slots already recorded, submitted or borrowed.
        impl_->invocations = {};
        impl_->ambiguousInvocations = {};
    }
    if (!pair.valid) {
        ++impl_->totals.rejectedInvocations;
        if (pair.frameValid && pair.timing) {
            for (std::size_t i = 0; i < invocationCapacity; ++i) {
                const auto &candidate = impl_->invocations[i];
                if (candidate.valid && candidate.engineFrame == pair.engineFrame &&
                    candidate.timing == pair.timing)
                    impl_->ambiguousInvocations[i] = true;
            }
        }
        return;
    }
    if (!pair.frameValid || !pair.timing || !pair.invocation) {
        ++impl_->totals.missingInvocationFrame;
        return;
    }
    const auto available = std::find_if(impl_->invocations.begin(), impl_->invocations.end(),
                                        [](const auto &v) { return !v.valid; });
    if (available == impl_->invocations.end()) {
        ++impl_->totals.invocationCapacityRefusals;
        // A refused duplicate must not turn the remaining older record into a
        // falsely unique match. Keep its resources intact, but refuse that pair.
        for (std::size_t i = 0; i < invocationCapacity; ++i) {
            const auto &candidate = impl_->invocations[i];
            if (candidate.engineFrame == pair.engineFrame && candidate.timing == pair.timing &&
                candidate.depth == pair.depth && candidate.motion == pair.motion)
                impl_->ambiguousInvocations[i] = true;
        }
        return;
    }
    *available = pair;
    impl_->ambiguousInvocations[available - impl_->invocations.begin()] = false;
}
bool Provider::record(std::uint32_t thread, const CommandRecordingFacts &f,
                      const descriptor_image_metadata::DispatchEvidence &e,
                      FrameTimingAssociation recordingFrame, bool captureInputs) {
    (void)thread; // The callback's thread is not the command recorder's identity.
    std::lock_guard lock(impl_->mutex);
    if (!e.metadataAccepted || e.issues || e.count > e.bindings.size() ||
        e.command.handle != f.command || !e.command.generation ||
        !f.recording || !f.stateKnown || !f.scopeKnown || f.inRenderPass || f.inDynamicRendering ||
        f.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY || f.queueFamily != impl_->config.queueFamily ||
        !f.rpRecordingGeneration ||
        !(f.beginUsageFlags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) ||
        (f.beginUsageFlags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)) {
        ++impl_->totals.invalidRecording;
        return false;
    }
    const descriptor_image_metadata::ImageBinding *depth = nullptr, *motion = nullptr;
    bool duplicate = false;
    for (std::size_t i = 0; i < e.count; ++i) {
        const auto &b = e.bindings[i];
        if (b.setIndex == 0 && b.binding == 5) {
            duplicate |= depth != nullptr || b.arrayElement != 0;
            depth = &b;
        }
        if (b.setIndex == 0 && b.binding == 7) {
            duplicate |= motion != nullptr || b.arrayElement != 0;
            motion = &b;
        }
    }
    if (duplicate || !depth || !motion || !sourceValid(*depth, VK_FORMAT_D32_SFLOAT_S8_UINT) ||
        !sourceValid(*motion, VK_FORMAT_R16G16_SFLOAT) ||
        depth->imageDescription.width != motion->imageDescription.width ||
        depth->imageDescription.height != motion->imageDescription.height) {
        ++impl_->totals.invalidSources;
        return false;
    }
    if (!recordingFrame.valid || !recordingFrame.ticket) {
        ++impl_->totals.missingRecordingFrame;
        return false;
    }
    SourcePair *found = nullptr;
    for (std::size_t i = 0; i < invocationCapacity; ++i) {
        auto &candidate = impl_->invocations[i];
        if (!candidate.valid || !candidate.frameValid ||
            candidate.engineFrame != recordingFrame.engineFrame ||
            candidate.timing != recordingFrame.ticket || candidate.depth != depth->image ||
            candidate.motion != motion->image) {
            continue;
        }
        if (found || impl_->ambiguousInvocations[i]) {
            ++impl_->totals.ambiguousDispatches;
            return false;
        }
        found = &candidate;
    }
    if (!found) {
        ++impl_->totals.unmatchedDispatches;
        return false;
    }
    const auto invocation = *found;
    impl_->ambiguousInvocations[found - impl_->invocations.data()] = false;
    *found = {};
    ++impl_->totals.matchedDispatches;
    // Consume this exact source even when OFF needs no auxiliary GPU copies.
    if (!captureInputs) {
        ++impl_->totals.skippedOffCaptures;
        return false;
    }
    auto available = std::find_if(impl_->slots.begin(), impl_->slots.begin()+slotCount,
                                  [](const auto &s) { return s.state == State::Free; });
    SharedPlaneLease shared;
    if(impl_->sharedBorrowing) {
        const VkExtent2D extent{depth->imageDescription.width,depth->imageDescription.height};
        const auto result=impl_->sharedInputs.reserve(impl_->sharedInputs.context,extent,shared);
        if(result==SharedBorrowResult::Acquired&&(!shared.key.valid()||!shared.auxiliary)) {
            if(shared.key.valid())impl_->sharedInputs.poison(impl_->sharedInputs.context,shared.key);
            ++impl_->totals.sharedFailures;return false;
        }
        if(result==SharedBorrowResult::Acquired&&shared.key.valid()&&shared.auxiliary) {
            available=impl_->slots.begin()+slotCount+shared.key.index;
            if(available->state!=State::Free) {
                impl_->sharedInputs.poison(impl_->sharedInputs.context,shared.key);
                ++impl_->totals.sharedFailures;return false;
            }
            bool valid=true;
            for(unsigned i=1;i<=2;++i) {
                const auto& target=shared.planes[i];const auto& info=target.successfulImageCreateInfo;
                valid=valid&&target.image&&target.device==impl_->config.device&&
                    (target.ownerQueueFamily==impl_->config.queueFamily||target.ownerQueueFamily==VK_QUEUE_FAMILY_EXTERNAL)&&
                    (target.currentLayout==VK_IMAGE_LAYOUT_GENERAL||target.currentLayout==VK_IMAGE_LAYOUT_UNDEFINED)&&
                    info.format==(i==1?VK_FORMAT_R32_SFLOAT:VK_FORMAT_R16G16_SFLOAT)&&
                    info.extent.width==extent.width&&info.extent.height==extent.height&&info.extent.depth==1&&
                    (info.usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT)&&target.image!=handle<VkImage>(depth->image.handle)&&
                    target.image!=handle<VkImage>(motion->image.handle);
            }
            if(!valid||shared.planes[1].image==shared.planes[2].image) {
                available->shared=shared;available->state=State::Recorded;
                if(impl_->freeShared(*available))available->state=State::Free;
                else {available->state=State::Uncertain;impl_->sharedInputs.poison(impl_->sharedInputs.context,shared.key);}
                ++impl_->totals.sharedFailures;return false;
            }
            if(!available->intermediate||available->extent.width!=extent.width||available->extent.height!=extent.height) {
                available->shared=shared;available->state=State::Recorded;
                if(!impl_->freeShared(*available)) {
                    available->state=State::Uncertain;impl_->sharedInputs.poison(impl_->sharedInputs.context,shared.key);return false;
                }
                available->state=State::Free;shared={};++impl_->totals.sharedFallbacks;
                available=std::find_if(impl_->slots.begin(),impl_->slots.begin()+slotCount,
                    [](const auto& s){return s.state==State::Free;});
            }
        } else {
            ++impl_->totals.sharedFallbacks;
            if(result==SharedBorrowResult::DeviceLost||result==SharedBorrowResult::Invalid)++impl_->totals.sharedFailures;
            if(result==SharedBorrowResult::DeviceLost)return false;
            shared={};
        }
    }
    if (!shared.key.valid()&&available == impl_->slots.begin()+slotCount) {
        ++impl_->totals.saturated;
        return false;
    }
    bool recordingStarted{};
    try {
        auto &s = *available;
        s.shared=shared;
        const VkExtent2D extent{depth->imageDescription.width, depth->imageDescription.height};
        impl_->prepare(s, extent);
        s.recording = {f.command, f.rpRecordingGeneration};
        s.pool = f.pool;
        s.generation = ++impl_->serial;
        s.invocation = invocation.invocation;
        s.constants = invocation.constants;
        s.timing = invocation.timing;
        s.engineFrame = invocation.engineFrame;
        s.constants.commandBuffer = f.command;
        s.constants.recordingGeneration = f.rpRecordingGeneration;
        s.constants.deviceIncarnation = f.contextIncarnation;
        if (impl_->lastExtent.width != extent.width || impl_->lastExtent.height != extent.height) {
            impl_->lastExtent = extent;
            ++impl_->viewportSerial;
        }
        s.viewport = impl_->viewportSerial;
        s.constants.viewportGeneration = s.viewport;
        s.depthCompute=impl_->config.depthCompute&&shared.key.valid()&&impl_->computeFor(s,*depth,e);
        recordingStarted=true;
        impl_->copy(s, handle<VkCommandBuffer>(f.command), *depth, *motion,e);
        s.state = State::Recorded;
        ++impl_->totals.captured;
        if(shared.key.valid())++impl_->totals.sharedCaptures;
        if(s.depthCompute)++impl_->totals.depthComputeCaptures;
        else {
            ++impl_->totals.depthTransferCaptures;
            if(impl_->config.depthCompute&&shared.depthStorage)++impl_->totals.depthComputeFallbacks;
        }
        return true;
    } catch (...) {
        if(shared.key.valid()) {
            available->state=recordingStarted?State::Uncertain:State::Recorded;
            if(!recordingStarted&&impl_->freeShared(*available))available->state=State::Free;
            else impl_->sharedInputs.poison(impl_->sharedInputs.context,shared.key);
            ++impl_->totals.sharedFailures;
        }
        ++impl_->totals.copyFailures;
        return false;
    }
}
void Provider::retireFrame(FrameTimingAssociation frame) {
    if (!frame.valid || !frame.ticket)
        return;
    std::lock_guard lock(impl_->mutex);
    for (std::size_t i = 0; i < invocationCapacity; ++i) {
        auto &candidate = impl_->invocations[i];
        if (candidate.valid && candidate.frameValid &&
            candidate.timing.contextGeneration == frame.ticket.contextGeneration &&
            candidate.engineFrame <= frame.engineFrame) {
            candidate = {};
            impl_->ambiguousInvocations[i] = false;
        }
    }
}
bool Provider::containsRecording(RecordingKey key) const {
    std::lock_guard lock(impl_->mutex);
    return std::any_of(impl_->slots.begin(), impl_->slots.end(), [&](const auto &s) {
        return s.state == State::Recorded && s.recording == key;
    });
}
void Provider::submitted(RecordingKey key, std::span<const VkSemaphore> signals, VkQueue queue,
                         std::uint64_t submission, VkResult result,std::uint64_t entryNs,std::uint64_t entryQpcTicks) {
    std::lock_guard lock(impl_->mutex);
    for (auto &s : impl_->slots)
        if (s.state == State::Recorded && s.recording == key) {
            if(result==VK_SUCCESS){
                auto depth=s.commands;
                depth[static_cast<unsigned>(measurement::Command::ImageCopy)]=0;
                measurement::addCommands(measurement::CopyRole::Depth,depth);
                measurement::Commands motion{};
                motion[static_cast<unsigned>(measurement::Command::ImageCopy)]=s.commands[static_cast<unsigned>(measurement::Command::ImageCopy)];
                measurement::addCommands(measurement::CopyRole::Motion,motion);
            }
            if (result != VK_SUCCESS || queue != impl_->config.queue ||
                signals.size() > maximumSignals || impl_->successfulSubmitSerial == UINT64_MAX) {
                if(result!=VK_SUCCESS)++impl_->totals.copyFailures;
                s.state = State::Uncertain;
                if(s.shared.key.valid())impl_->sharedInputs.poison(impl_->sharedInputs.context,s.shared.key);
                continue;
            }
            if(s.shared.key.valid())try {
                impl_->sharedInputs.submitted(impl_->sharedInputs.context,s.shared.key,6);
            } catch(...) {
                s.state=State::Uncertain;impl_->sharedInputs.poison(impl_->sharedInputs.context,s.shared.key);
                ++impl_->totals.sharedFailures;continue;
            }
            s.signalCount = signals.size();
            std::copy(signals.begin(), signals.end(), s.signals.begin());
            s.submission = submission;
            s.submitSerial = ++impl_->successfulSubmitSerial;
            s.state = State::Submitted;
            s.querySample.submitQpcTicks=entryQpcTicks;s.querySample.qpcFrequency=entryQpcTicks?impl_->qpcFrequency:0;
            if(entryNs&&s.timing!=impl_->lastSubmissionTicket){
                if(impl_->lastSubmissionNs&&entryNs>=impl_->lastSubmissionNs&&s.timing.contextGeneration==impl_->lastSubmissionTicket.contextGeneration&&
                    s.timing.sdkId==impl_->lastSubmissionTicket.sdkId+1)
                    measurement::observe(measurement::Metric::SourceSubmitInterval,entryNs-impl_->lastSubmissionNs,s.timing.sdkId,s.timing.contextGeneration);
                else ++impl_->totals.submissionIntervalResets;
                impl_->lastSubmissionNs=entryNs;impl_->lastSubmissionTicket=s.timing;
            }
            ++impl_->totals.submissions;
        }
}
std::uint64_t Provider::submittedCutoff(VkQueue queue) const {
    std::lock_guard lock(impl_->mutex);
    return queue == impl_->config.queue ? impl_->successfulSubmitSerial : 0;
}
Bundle Provider::acquireForPresent(VkQueue queue, std::span<const VkSemaphore> waits,
                                   VkFence fence, std::uint64_t cutoff, FrameTimingAssociation frame) {
    std::lock_guard lock(impl_->mutex);
    Bundle result;
    if (queue != impl_->config.queue || !fence || cutoff > impl_->successfulSubmitSerial ||
        impl_->fn<PFN_vkGetFenceStatus>("vkGetFenceStatus")(impl_->config.device, fence) !=
            VK_SUCCESS)
        return result;
    Impl::Slot *selected = nullptr;
    std::array<Impl::Slot*, slotCount> matches{};
    std::size_t matchCount{};
    const bool exactFrame = frame.valid && bool(frame.ticket);
    for (auto &s : impl_->slots)
        if (s.state == State::Submitted && !s.shared.key.valid() && s.submitSerial <= cutoff) {
            bool matched = exactFrame && s.timing == frame.ticket && s.engineFrame == frame.engineFrame;
            if (!exactFrame) for (const auto wait : waits)
                matched |= std::find(s.signals.begin(), s.signals.begin() + s.signalCount, wait) !=
                           s.signals.begin() + s.signalCount;
            if (!matched)
                continue;
            matches[matchCount++] = &s;
            selected = &s;
        }
    // GPU completion alone never makes a future frame obsolete.
    if (exactFrame) impl_->retireOlder(cutoff, frame,fence);
    if (matchCount > 1) {
        ++impl_->totals.ambiguousPresent;
        // This same-queue copy fence covers these pre-cutoff writes. No bridge
        // read was admitted, including when present waits forwarded the signals.
        for (std::size_t i = 0; i < matchCount; ++i) {
            auto& discarded = *matches[i];
            discarded.state = State::Free;
            discarded.initialized = true;
            discarded.signalCount = 0;
            ++impl_->totals.cancelled;
        }
        return result;
    }
    if (!selected) {
        ++impl_->totals.missingPresent;
        return result;
    }
    auto &s = *selected;
    s.state = State::Consuming;
    s.initialized = true;
    s.queuedLease = false;
    ++impl_->totals.presentMatches;
    return impl_->bundle(s);
}
Bundle Provider::acquireQueuedForPresent(VkQueue queue, std::uint64_t cutoff, FrameTimingAssociation frame) {
    std::lock_guard lock(impl_->mutex);
    if (queue != impl_->config.queue || !frame.valid || !frame.ticket || !frame.ticket.latencyEnabled ||
        cutoff > impl_->successfulSubmitSerial)
        return {};
    Impl::Slot* selected{};
    for (auto& slot : impl_->slots)
        if (slot.state == State::Submitted && slot.submitSerial <= cutoff &&
            slot.timing == frame.ticket && slot.engineFrame == frame.engineFrame) {
            if (selected) return {}; // No completion proof: even ambiguous slots remain owned.
            selected = &slot;
        }
    if (!selected) return {};
    selected->state = State::Consuming;
    selected->queuedLease = true;
    ++impl_->totals.presentMatches;
    return impl_->bundle(*selected);
}
void Provider::rollbackQueuedAcquire(Ticket ticket) {
    std::lock_guard lock(impl_->mutex);
    auto* found=impl_->ticketSlot(ticket);if(!found)return;
    auto& slot = *found;
    if (slot.generation == ticket.generation && slot.state == State::Consuming && slot.queuedLease) {
        slot.state = State::Submitted;
        slot.queuedLease = false;
    }
}
void Provider::retireCompletedForPresent(VkQueue queue, VkFence fence, std::uint64_t cutoff, FrameTimingAssociation frame) {
    std::lock_guard lock(impl_->mutex);
    if (queue == impl_->config.queue && fence && cutoff <= impl_->successfulSubmitSerial &&
        impl_->fn<PFN_vkGetFenceStatus>("vkGetFenceStatus")(impl_->config.device, fence) == VK_SUCCESS)
        impl_->retireOlder(cutoff, frame,fence);
}
bool Provider::releaseAfterBridge(Ticket ticket,const FrameCompletion* receipt) {
    std::lock_guard lock(impl_->mutex);
    auto* found=impl_->ticketSlot(ticket);if(!found)return false;
    auto &s = *found;
    if (s.generation != ticket.generation || s.state != State::Consuming)
        return false;
    if(s.shared.key.valid()&&(!receipt||!impl_->freeShared(s,VK_NULL_HANDLE,receipt)))return false;
    impl_->completeQueries(s);
    s.state = State::Free;
    s.initialized = true;
    s.queuedLease = false;
    s.signalCount = 0;
    ++impl_->totals.consumed;
    return true;
}
void Provider::retireUnsubmitted(RecordingKey key) {
    std::lock_guard lock(impl_->mutex);
    for (auto &s : impl_->slots)
        if (s.state == State::Recorded && s.recording == key) {
            if(!impl_->freeShared(s))continue;
            s.state = State::Free;
            ++impl_->totals.cancelled;
        }
}
void Provider::retirePool(std::uint64_t pool) {
    std::lock_guard lock(impl_->mutex);
    for (auto &slot : impl_->slots) {
        if (slot.state == State::Recorded && slot.pool == pool) {
            if(!impl_->freeShared(slot))continue;
            slot.state = State::Free;
            ++impl_->totals.cancelled;
        }
    }
}
VkResult Provider::stop(VkFence fence) {
    std::lock_guard lock(impl_->mutex);
    if (!fence)
        return VK_ERROR_UNKNOWN;
    const auto status =
        impl_->fn<PFN_vkGetFenceStatus>("vkGetFenceStatus")(impl_->config.device, fence);
    if (status != VK_SUCCESS)
        return status;
    impl_->sharedBorrowing=false;
    for (auto &s : impl_->slots) {
        if(s.shared.key.valid()) {
            if(s.state==State::Consuming)continue;
            if(!impl_->freeShared(s,fence))return VK_ERROR_UNKNOWN;
        }
        s.state = State::Free;
    }
    return VK_SUCCESS;
}
Stats Provider::stats() const {
    std::lock_guard lock(impl_->mutex);
    auto out = impl_->totals;
    for (std::size_t i = 0; i < slotCount; ++i)
        out.states[i] = impl_->slots[i].state;
    for(std::size_t i=0;i<sharedPlaneSlotCount;++i)out.sharedStates[i]=impl_->slots[slotCount+i].state;
    return out;
}
} // namespace xess_vk::continuous
