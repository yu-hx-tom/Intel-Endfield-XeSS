#pragma once
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace xess_vk::sr_interop {
struct Signal {VkSemaphore semaphore{};std::uint64_t value{};VkPipelineStageFlags2 stage=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;};
struct Command {VkCommandBuffer buffer{};std::uint32_t mask{};};
struct Batch {std::vector<Signal> waits,signals;std::vector<Command> commands;};
struct Handoff {VkCommandBuffer prefix{},tail{};VkSemaphore semaphore{};std::uint64_t ready{},done{},retired{};};
struct Segment : Batch {std::size_t runAfter=SIZE_MAX;};
inline void require(bool condition,const char* reason){if(!condition)throw std::invalid_argument(reason);}
inline std::vector<Batch> normalize(std::span<const VkSubmitInfo> source){
    std::vector<Batch> batches;batches.reserve(source.size());
    for(const auto& s:source){
        require(s.sType==VK_STRUCTURE_TYPE_SUBMIT_INFO,"Invalid submit structure");
        require((!s.waitSemaphoreCount||(s.pWaitSemaphores&&s.pWaitDstStageMask))&&(!s.signalSemaphoreCount||s.pSignalSemaphores)&&(!s.commandBufferCount||s.pCommandBuffers),"Missing submit arrays");
        const VkTimelineSemaphoreSubmitInfo* timeline=nullptr;
        for(auto* p=static_cast<const VkBaseInStructure*>(s.pNext);p;p=p->pNext){
            if(p->sType==VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO){require(!timeline,"Duplicate submit timeline");timeline=reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(p);}
            else if(p->sType==VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO){const auto& g=*reinterpret_cast<const VkDeviceGroupSubmitInfo*>(p);
                require((!g.waitSemaphoreCount||g.pWaitSemaphoreDeviceIndices)&&(!g.signalSemaphoreCount||g.pSignalSemaphoreDeviceIndices)&&(!g.commandBufferCount||g.pCommandBufferDeviceMasks),"Missing device group arrays");
                for(unsigned n=0;n<g.waitSemaphoreCount;++n)require(g.pWaitSemaphoreDeviceIndices[n]==0,"Multi-device wait unsupported");
                for(unsigned n=0;n<g.signalSemaphoreCount;++n)require(g.pSignalSemaphoreDeviceIndices[n]==0,"Multi-device signal unsupported");
                for(unsigned n=0;n<g.commandBufferCount;++n)require(g.pCommandBufferDeviceMasks[n]==1,"Multi-device command unsupported");}
            else if(p->sType==VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO)require(!reinterpret_cast<const VkProtectedSubmitInfo*>(p)->protectedSubmit,"Protected interop unsupported");
            else throw std::invalid_argument("Unsupported submit extension for SR interop");
        }
        if(timeline)require((!timeline->waitSemaphoreValueCount||timeline->waitSemaphoreValueCount==s.waitSemaphoreCount)&&
            (!timeline->signalSemaphoreValueCount||timeline->signalSemaphoreValueCount==s.signalSemaphoreCount)&&
            (!timeline->waitSemaphoreValueCount||timeline->pWaitSemaphoreValues)&&(!timeline->signalSemaphoreValueCount||timeline->pSignalSemaphoreValues),"Invalid submit timeline arrays");
        Batch b;
        for(unsigned n=0;n<s.waitSemaphoreCount;++n)b.waits.push_back({s.pWaitSemaphores[n],timeline&&timeline->waitSemaphoreValueCount?timeline->pWaitSemaphoreValues[n]:0,s.pWaitDstStageMask[n]});
        for(unsigned n=0;n<s.signalSemaphoreCount;++n)b.signals.push_back({s.pSignalSemaphores[n],timeline&&timeline->signalSemaphoreValueCount?timeline->pSignalSemaphoreValues[n]:0});
        for(unsigned n=0;n<s.commandBufferCount;++n)b.commands.push_back({s.pCommandBuffers[n],1});
        batches.push_back(std::move(b));
    }
    return batches;
}
inline std::vector<Batch> normalize(std::span<const VkSubmitInfo2> source){
    std::vector<Batch> batches;batches.reserve(source.size());
    for(const auto& s:source){
        require(s.sType==VK_STRUCTURE_TYPE_SUBMIT_INFO_2&&!s.pNext&&!s.flags,"Unsupported Submit2 extension or flags");
        require((!s.waitSemaphoreInfoCount||s.pWaitSemaphoreInfos)&&(!s.signalSemaphoreInfoCount||s.pSignalSemaphoreInfos)&&(!s.commandBufferInfoCount||s.pCommandBufferInfos),"Missing Submit2 arrays");
        Batch b;
        for(unsigned n=0;n<s.waitSemaphoreInfoCount;++n){const auto& w=s.pWaitSemaphoreInfos[n];require(!w.pNext&&!w.deviceIndex,"Multi-device wait unsupported");b.waits.push_back({w.semaphore,w.value,w.stageMask});}
        for(unsigned n=0;n<s.signalSemaphoreInfoCount;++n){const auto& w=s.pSignalSemaphoreInfos[n];require(!w.pNext&&!w.deviceIndex,"Multi-device signal unsupported");b.signals.push_back({w.semaphore,w.value,w.stageMask});}
        for(unsigned n=0;n<s.commandBufferInfoCount;++n){const auto& c=s.pCommandBufferInfos[n];require(!c.pNext&&c.deviceMask<=1,"Multi-device command unsupported");b.commands.push_back({c.commandBuffer,c.deviceMask});}
        batches.push_back(std::move(b));
    }
    return batches;
}
// Build the complete transaction before either API receives work. App waits
// remain on the first segment, app signals/fence follow every continuation.
inline std::vector<Segment> plan(std::span<const Batch> batches,std::span<const Handoff> handoffs){
    std::vector<Segment> result;std::vector<bool> seen(handoffs.size());
    for(const auto& batch:batches){
        Segment current;current.waits=batch.waits;std::vector<Signal> retirement;
        for(auto command:batch.commands){
            current.commands.push_back(command);
            const auto h=std::find_if(handoffs.begin(),handoffs.end(),[&](const Handoff& h){return h.prefix==command.buffer;});
            if(h==handoffs.end())continue;
            const auto index=std::size_t(h-handoffs.begin());require(!seen[index],"SR recording submitted more than once");seen[index]=true;
            require(h->prefix&&h->tail&&h->prefix!=h->tail&&h->semaphore&&h->ready&&h->done>h->ready&&h->retired>h->done,"Invalid SR handoff");
            current.signals.push_back({h->semaphore,h->ready});current.runAfter=index;result.push_back(std::move(current));
            current={};current.waits.push_back({h->semaphore,h->done});current.commands.push_back({h->tail,command.mask});
            retirement.push_back({h->semaphore,h->retired});
        }
        current.signals=batch.signals;current.signals.insert(current.signals.end(),retirement.begin(),retirement.end());result.push_back(std::move(current));
    }
    return result;
}
}
