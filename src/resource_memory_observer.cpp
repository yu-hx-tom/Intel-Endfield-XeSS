#include "resource_memory_observer.hpp"
#include <windows.h>
#include <cstdio>
#include <atomic>
#include <mutex>

namespace xess_vk::resource_memory_observer {
namespace {
namespace rm=resource_memory_facts;
struct ErrorGuard {DWORD saved=GetLastError();~ErrorGuard(){SetLastError(saved);}};
}
struct Context {
    VkDevice device{};Sink sink{};std::mutex mutex;rm::Tracker tracker;bool retired{};std::atomic<std::uint64_t> failures{};std::uint64_t summaries{};
    Context(VkDevice d,const VkPhysicalDeviceMemoryProperties& p,rm::Config c,Sink s):device(d),sink(s),tracker(d,p,c){}
};
namespace {
template<class F>void guarded(Context* c,F&& f) noexcept {ErrorGuard error;if(!c)return;try{std::lock_guard lock(c->mutex);if(!c->retired)f();}catch(...){++c->failures;}}
Observation callback(void* c,RecordedImageKey image) noexcept {return query(static_cast<Context*>(c),image);}
}
rm::DeviceScope deviceScope(const VkDeviceCreateInfo* info) noexcept {
    ErrorGuard error;if(!info||info->sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO)return rm::DeviceScope::Unverified;
    const auto* node=static_cast<const VkBaseInStructure*>(info->pNext);std::size_t count=0;
    while(node&&count++<64){if(node->sType==VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO)return rm::DeviceScope::DeviceGroup;node=node->pNext;}
    return node?rm::DeviceScope::Unverified:rm::DeviceScope::SinglePhysicalDevice;
}
bool imageRequirementsQuerySupported(const VkImageCreateInfo* info) noexcept {
    ErrorGuard error;if(!info||info->sType!=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO)return false;
    constexpr VkImageCreateFlags ordinary=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT|VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT
        |VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT|VK_IMAGE_CREATE_EXTENDED_USAGE_BIT|VK_IMAGE_CREATE_ALIAS_BIT;
    if(info->flags&~ordinary)return false;
    if(!info->pNext)return true;const auto* node=static_cast<const VkBaseInStructure*>(info->pNext);
    if(node->sType!=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO||node->pNext)return false;
    const auto* formats=static_cast<const VkImageFormatListCreateInfo*>(info->pNext);
    return formats->viewFormatCount<=64&&(!formats->viewFormatCount||formats->pViewFormats);
}
bool bufferRequirementsQuerySupported(const VkBufferCreateInfo* info) noexcept {
    ErrorGuard error;return info&&info->sType==VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO&&!info->pNext&&!info->flags;
}
Context* create(VkDevice device,const VkPhysicalDeviceMemoryProperties& properties,rm::Config config,Sink sink) noexcept {
    ErrorGuard error;try{return new Context(device,properties,config,sink);}catch(...){return nullptr;}
}
void destroy(Context* c) noexcept {ErrorGuard error;delete c;}
void retire(Context* c) noexcept {guarded(c,[&]{c->tracker.stop();c->retired=true;});}
void coverageLost(Context* c) noexcept {guarded(c,[&]{c->tracker.coverageLost();});}
rm::Stats stats(Context* c) noexcept {ErrorGuard error;rm::Stats out;if(!c)return out;try{std::lock_guard lock(c->mutex);return c->tracker.stats();}catch(...){++c->failures;return out;}}
void imageCreated(Context* c,const image_inventory::Record& record,const VkImageCreateInfo* info,VkResult driver,const VkMemoryRequirements* requirements) noexcept {guarded(c,[&]{
    const auto result=c->tracker.imageCreated(record,info,driver);
    if(driver==VK_SUCCESS&&requirements&&result.key.handle)c->tracker.requirementsObserved({rm::ResourceKind::Image,result.key},*requirements);
});}
void bufferCreated(Context* c,VkBuffer buffer,const VkBufferCreateInfo* info,VkResult driver,const VkMemoryRequirements* requirements) noexcept {guarded(c,[&]{
    const auto result=c->tracker.bufferCreated(buffer,info,driver);
    if(driver==VK_SUCCESS&&requirements&&result.key.handle)c->tracker.requirementsObserved({rm::ResourceKind::Buffer,result.key},*requirements);
});}
void imageDestroying(Context* c,VkImage image) noexcept {guarded(c,[&]{c->tracker.imageRetiring(c->tracker.imageKey(image).key);});}
void bufferDestroying(Context* c,VkBuffer buffer) noexcept {guarded(c,[&]{c->tracker.bufferRetiring(c->tracker.bufferKey(buffer).key);});}
void memoryAllocated(Context* c,VkDeviceMemory memory,const VkMemoryAllocateInfo* info,VkResult driver) noexcept {guarded(c,[&]{c->tracker.memoryAllocated(memory,info,driver);});}
void memoryFreeing(Context* c,VkDeviceMemory memory) noexcept {guarded(c,[&]{c->tracker.memoryRetiring(c->tracker.memoryKey(memory).key);});}
rm::BindTicket beforeImageBind(Context* c,rm::BindApi api,std::uint32_t count,const VkBindImageMemoryInfo* infos) noexcept {rm::BindTicket out;guarded(c,[&]{out=c->tracker.beforeImageBind(api,count,infos);});return out;}
rm::BindTicket beforeBufferBind(Context* c,rm::BindApi api,std::uint32_t count,const VkBindBufferMemoryInfo* infos) noexcept {rm::BindTicket out;guarded(c,[&]{out=c->tracker.beforeBufferBind(api,count,infos);});return out;}
void afterBind(Context* c,rm::BindTicket ticket,VkResult driver) noexcept {guarded(c,[&]{c->tracker.afterBind(ticket,driver);});}
Observation query(Context* c,RecordedImageKey image) noexcept {
    ErrorGuard error;Observation out;out.queried=true;out.binding.resource={rm::ResourceKind::Image,{image.handle,image.generation}};
    if(!c){out.globalIssues=out.binding.issues=rm::bit(rm::Issue::CoverageIncomplete);return out;}
    try{std::lock_guard lock(c->mutex);out.binding=c->tracker.query(out.binding.resource);out.globalIssues=c->tracker.stats().globalIssues;
        if(c->retired||c->failures)out.globalIssues|=rm::bit(rm::Issue::CoverageIncomplete);
        out.binding.issues|=out.globalIssues;out.localIssues=out.binding.issues&~out.globalIssues;
        if(out.binding.issues)out.binding.observedAliasFree=false;return out;
    }catch(...){++c->failures;out.globalIssues=out.binding.issues=rm::bit(rm::Issue::CoverageIncomplete);return out;}
}
SourceQuery sourceQuery(Context* c) noexcept {ErrorGuard error;return {callback,c};}
void summary(Context* c) noexcept {
    ErrorGuard error;if(!c)return;rm::Stats s;std::uint64_t failures{};bool emit=false;
    try{std::lock_guard lock(c->mutex);if(c->summaries<16){++c->summaries;s=c->tracker.stats();failures=c->failures;emit=true;}}catch(...){++c->failures;return;}
    if(!emit||!c->sink)return;char line[2048];
    const auto n=[](std::uint64_t v){return static_cast<unsigned long long>(v);};
    const int size=std::snprintf(line,sizeof(line),"{\"event\":\"resource_memory_summary\",\"device\":\"0x%llx\",\"storage_bytes\":%zu,\"storage_mode\":\"paged_payload\",\"images\":[%zu,%zu,%zu,%llu],\"buffers\":[%zu,%zu,%zu,%llu],\"memories\":[%zu,%zu,%zu,%llu],"
        "\"registrations\":%llu,\"retirements\":%llu,\"requirements_observed\":%llu,\"bind_attempts\":%llu,\"successful_binds\":%llu,\"failed_binds\":%llu,\"uncertain_batches\":%llu,\"global_issues\":%llu,\"failures\":%llu,"
        "\"bind_calls\":[%llu,%llu,%llu,%llu,%llu,%llu],\"layout_verified\":false,\"gpu_complete\":false,\"resource_lifetime_pinned\":false,\"copy_permitted\":false}",
        n(reinterpret_cast<std::uint64_t>(c->device)),s.storageBytes,s.images.live,s.images.highWater,s.images.capacity,n(s.images.capacityRefusals),s.buffers.live,s.buffers.highWater,s.buffers.capacity,n(s.buffers.capacityRefusals),s.memories.live,s.memories.highWater,s.memories.capacity,n(s.memories.capacityRefusals),
        n(s.registrations),n(s.retirements),n(s.requirementsObserved),n(s.bindAttempts),n(s.successfulBinds),n(s.failedBinds),n(s.uncertainBatches),n(s.globalIssues),n(failures),n(s.bindCalls[0]),n(s.bindCalls[1]),n(s.bindCalls[2]),n(s.bindCalls[3]),n(s.bindCalls[4]),n(s.bindCalls[5]));
    if(size>0&&std::size_t(size)<sizeof(line))try{c->sink(line);}catch(...){++c->failures;}
}
} // namespace xess_vk::resource_memory_observer
