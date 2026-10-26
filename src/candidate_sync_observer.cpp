#include "candidate_sync_observer.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace xess_vk::candidate_sync {
namespace {
constexpr std::size_t poolLimit=128, queueLimit=32, batchLimit=64;
constexpr std::uint64_t candidateEventLimit=256, evidencePerRoleLimit=256;
struct ErrorGuard { DWORD saved=GetLastError(); ~ErrorGuard(){SetLastError(saved);} };
template<class T> std::uint64_t bits(T handle) { return reinterpret_cast<std::uint64_t>(handle); }
const char* roleName(Role role) {
    switch(role) { case Role::Motion:return "motion"; case Role::Depth:return "depth"; default:return "real_color"; }
}
const char* passName(Role role) { return role==Role::RealColor?"frame_inpainting":"reconstruct_previous_depth"; }
void hex(std::ostream& s,std::uint64_t v){s<<'"'<<"0x"<<std::hex<<v<<std::dec<<'"';}
void prefix(std::ostream& s,const char* event,std::uint64_t device,const char* role,const char* pass,std::uint64_t selection) {
    s<<"{\"event\":\""<<event<<"\",\"pid\":"<<GetCurrentProcessId()<<",\"uptime_ms\":"<<GetTickCount64()
     <<",\"thread_id\":"<<GetCurrentThreadId()<<",\"device\":";hex(s,device);
    s<<",\"role\":\""<<role<<"\",\"pass\":\""<<pass<<"\",\"selectionGeneration\":"<<selection;
}
void finish(std::ostream& s) {
    s<<",\"complete_hook_coverage\":false,\"gpu_complete\":false,\"last_writer_known\":false"
       ",\"resource_lifetime_pinned\":false,\"copy_permitted\":false,\"game_frame_inputs_verified\":false}";
}
void output(Sink sink,const std::ostringstream& s) { const auto text=s.str();if(sink&&text.size()<30000)sink(text.c_str()); }
}
struct Context {
    struct Selection { image_sync::ImageKey key;std::uint64_t generation{}, submissions{}; };
    struct RoleState { Selection current,previous;std::uint64_t details{},omitted{}; };
    struct Pool { std::uint64_t handle{};std::uint32_t family{VK_QUEUE_FAMILY_IGNORED}; };
    struct Queue { std::uint64_t handle{},inFlight{};std::uint32_t family{VK_QUEUE_FAMILY_IGNORED}; };
    std::uint64_t device{};Sink sink{};std::mutex mutex;
    image_sync::Tracker tracker;
    std::array<RoleState,3> roles{};std::array<Pool,poolLimit> pools{};std::array<Queue,queueLimit> queues{};
    std::atomic<std::uint64_t> failures{};
    std::uint64_t selectionSerial{},selectionChanges{},candidateDetails{},candidateOmitted{},submitToken{},queueOverlaps{};
    bool retired{};
    Context(VkDevice d,bool sync2,Sink s):device(bits(d)),sink(s),tracker([&]{image_sync::Config c;c.synchronization2Enabled=sync2;return c;}()){}
    Queue* queue(std::uint64_t handle) {for(auto& q:queues)if(q.handle==handle&&handle)return &q;return nullptr;}
    bool referenced(image_sync::ImageKey key) const {
        for(const auto& r:roles)if(r.current.key==key||r.previous.key==key)return true;return false;
    }
    image_sync::ImageKey selected(std::uint64_t image) const {
        for(const auto& r:roles)for(const auto* s:{&r.current,&r.previous})if(s->key.image==image&&image)return s->key;return {};
    }
    void retireUnreferenced(image_sync::ImageKey key){if(key.image&&!referenced(key))tracker.deselect(key);}
    void submittedDetails(std::uint64_t serial) {
        for(std::size_t index=0;index<roles.size();++index) {
            auto& role=roles[index];
            for(auto* selected:{&role.current,&role.previous}) {
                if(!selected->key.image)continue;
                auto e=tracker.snapshot(selected->key);
                if(!e.barrierSubmitted||e.submissionSerial!=serial)continue;
                const auto n=++selected->submissions;
                if(n>1&&n%600)continue;
                if(role.details>=evidencePerRoleLimit){++role.omitted;continue;}
                ++role.details;std::ostringstream s;const auto kind=static_cast<Role>(index);
                prefix(s,"image_sync_submit_evidence",device,roleName(kind),passName(kind),selected->generation);
                s<<",\"selection_is_current\":"<<(selected==&role.current?"true":"false")<<",\"image\":";hex(s,e.image.image);
                s<<",\"image_generation\":"<<e.image.generation<<",\"candidate_submission_count\":"<<n
                 <<",\"submission_serial\":"<<e.submissionSerial<<",\"command_generation\":"<<e.commandGeneration<<",\"command\":";hex(s,e.command);
                s<<",\"queue\":";hex(s,e.queue);s<<",\"queue_family\":"<<e.queueFamily<<",\"fence\":";hex(s,e.fence);
                const auto& b=e.lastBarrier;
                s<<",\"old_layout\":"<<b.oldLayout<<",\"new_layout\":"<<b.newLayout
                 <<",\"layout_fields_ignored\":"<<(e.lastBarrierLayoutFieldsIgnored?"true":"false")
                 <<",\"submitted_layout_known\":false,\"issues\":"<<e.issues<<",\"recording_issues\":"<<e.lastRecordingIssues
                 <<",\"selected_before_begin\":"<<((e.lastRecordingIssues&image_sync::bit(image_sync::Issue::NotSelectedAtBegin))?"false":"true")
                 <<",\"src_queue_family\":"<<b.srcQueueFamily<<",\"dst_queue_family\":"<<b.dstQueueFamily
                 <<",\"src_stages\":"<<b.srcStages<<",\"dst_stages\":"<<b.dstStages
                 <<",\"src_access\":"<<b.srcAccess<<",\"dst_access\":"<<b.dstAccess
                 <<",\"aspect_mask\":"<<b.range.aspectMask<<",\"base_mip\":"<<b.range.baseMipLevel
                 <<",\"level_count\":"<<b.range.levelCount<<",\"base_layer\":"<<b.range.baseArrayLayer<<",\"layer_count\":"<<b.range.layerCount;
                finish(s);output(sink,s);
            }
        }
    }
    void submitted(SubmitTicket ticket,std::span<const image_sync::Handle> buffers,VkFence fence,VkResult result,bool audited) {
        auto* q=queue(ticket.queue);
        if(!q||!ticket.token||!q->inFlight)tracker.untrackedDeviceOperation(image_sync::Issue::SubmitOrderAmbiguous);
        if(q&&q->inFlight)--q->inFlight;
        if(retired)return;
        tracker.submit(ticket.queue,q?q->family:VK_QUEUE_FAMILY_IGNORED,buffers,bits(fence),result,audited);
        if(result==VK_SUCCESS)submittedDetails(tracker.stats().successfulSubmissions);
    }
};
namespace {
template<class F> void guarded(Context* c,F&& fn) noexcept {
    if(!c)return;ErrorGuard error;try{std::lock_guard lock(c->mutex);fn();}catch(...){++c->failures;}
}
}
Context* create(VkDevice device,bool sync2,Sink sink) noexcept {
    ErrorGuard error;try {
        auto* c=new Context(device,sync2,sink);
        try {std::ostringstream s;prefix(s,"image_sync_start",c->device,"all","multiple",0);
            s<<",\"enabled_by_fsr_observer\":true,\"tracker_storage_bytes\":"<<image_sync::Tracker::allocatedStorageBytes()
             <<",\"helper_storage_bytes\":"<<sizeof(Context)<<",\"candidate_detail_limit\":256,\"submission_detail_limit_per_role\":256";
            finish(s);output(sink,s);}catch(...){++c->failures;}
        return c;
    }catch(...){return nullptr;}
}
void summary(Context* c) noexcept {guarded(c,[&]{
    const auto t=c->tracker.stats();std::ostringstream s;prefix(s,"image_sync_summary",c->device,"all","multiple",0);
    s<<",\"retired\":"<<(c->retired?"true":"false")<<",\"selection_changes\":"<<c->selectionChanges
     <<",\"candidate_details\":"<<c->candidateDetails<<",\"candidate_details_omitted\":"<<c->candidateOmitted
     <<",\"successful_submissions\":"<<t.successfulSubmissions<<",\"failed_submissions\":"<<t.failedSubmissions
     <<",\"recorded_barriers\":"<<t.recordedBarriers<<",\"ignored_barriers\":"<<t.ignoredBarriers
     <<",\"retired_operations_skipped\":"<<t.retiredOperationsSkipped<<",\"issues\":"<<t.issues
     <<",\"selected_images\":"<<t.selected<<",\"command_buffers\":"<<t.commands
     <<",\"overlapping_queue_submits\":"<<c->queueOverlaps<<",\"trace_failures\":"<<c->failures.load();
    for(std::size_t i=0;i<c->roles.size();++i)s<<",\""<<roleName(static_cast<Role>(i))<<"_submission_details\":"<<c->roles[i].details
        <<",\""<<roleName(static_cast<Role>(i))<<"_submission_details_omitted\":"<<c->roles[i].omitted;
    s<<",\"current_roles\":[";
    for(std::size_t i=0;i<c->roles.size();++i){if(i)s<<',';const auto& r=c->roles[i].current;const auto kind=static_cast<Role>(i);
        const auto e=c->tracker.snapshot(r.key);
        s<<"{\"role\":\""<<roleName(kind)<<"\",\"pass\":\""<<passName(kind)<<"\",\"selectionGeneration\":"<<r.generation<<",\"image\":";
        hex(s,r.key.image);s<<",\"image_generation\":"<<r.key.generation<<",\"candidate_submissions\":"<<r.submissions
         <<",\"latest_barrier_submission\":"<<e.submissionSerial<<",\"command_generation\":"<<e.commandGeneration
         <<",\"barrier_submitted\":"<<(e.barrierSubmitted?"true":"false")<<",\"queue\":";hex(s,e.queue);
        s<<",\"queue_family\":"<<e.queueFamily<<",\"latest_device_submission\":"<<e.latestDeviceSubmission
         <<",\"recording_issues\":"<<e.lastRecordingIssues
         <<",\"selected_before_begin\":"<<(e.barrierSubmitted&&!(e.lastRecordingIssues&image_sync::bit(image_sync::Issue::NotSelectedAtBegin))?"true":"false")
         <<",\"raw_old_layout\":"<<e.lastBarrier.oldLayout<<",\"raw_new_layout\":"<<e.lastBarrier.newLayout<<",\"issues\":"<<e.issues<<'}';}
    s<<']';
    finish(s);output(c->sink,s);
});}
void retire(Context* c) noexcept {guarded(c,[&]{c->retired=true;c->tracker.stop();});}
void destroy(Context* c) noexcept {if(!c)return;ErrorGuard error;retire(c);summary(c);delete c;}
SelectionNotice select(Context* c,Role role,const image_inventory::Record& record) noexcept {
    SelectionNotice notice;guarded(c,[&]{
        const auto index=static_cast<unsigned>(role);if(c->retired||index>=3)return;
        auto& r=c->roles[index];const image_sync::ImageKey key{record.image,record.generation};
        if(key==r.current.key)return;
        notice.device=c->device;notice.role=role;notice.record=record;
        notice.previousSelectionGeneration=r.current.generation;notice.selectionGeneration=++c->selectionSerial;
        ++c->selectionChanges;notice.emit=c->candidateDetails<candidateEventLimit;
        if(notice.emit)++c->candidateDetails;else ++c->candidateOmitted;
        const auto oldPrevious=r.previous.key,oldCurrent=r.current.key;
        r.previous=r.current;r.current={};
        if(r.previous.key==key)r.previous={};
        const auto& d=record.description;
        const auto expected=role==Role::Motion?VK_FORMAT_R16G16_SFLOAT:role==Role::Depth?VK_FORMAT_R32_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM;
        const bool compatible=key.image&&key.generation&&d.format==static_cast<std::uint32_t>(expected)&&d.width&&d.height&&d.depth==1&&
            d.mipLevels==1&&d.arrayLayers==1&&d.samples==VK_SAMPLE_COUNT_1_BIT&&d.tiling==VK_IMAGE_TILING_OPTIMAL&&
            d.sharingMode==VK_SHARING_MODE_EXCLUSIVE&&(d.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        // Remove only observation slots. No image destruction or Vulkan call.
        c->retireUnreferenced(oldPrevious);c->retireUnreferenced(oldCurrent);
        if(compatible&&c->tracker.select({key,VK_IMAGE_ASPECT_COLOR_BIT,d.mipLevels,d.arrayLayers,VK_SHARING_MODE_EXCLUSIVE})) {
            r.current={key,notice.selectionGeneration,0};notice.accepted=true;
        }
    });return notice;
}
void reportSelection(const SelectionNotice& n,Sink sink) noexcept {
    if(!n.emit)return;ErrorGuard error;try {
        std::ostringstream s;prefix(s,"image_sync_candidate",n.device,roleName(n.role),passName(n.role),n.selectionGeneration);
        s<<",\"accepted\":"<<(n.accepted?"true":"false")<<",\"previous_selection_generation\":"<<n.previousSelectionGeneration<<",\"image\":";
        hex(s,n.record.image);s<<",\"image_generation\":"<<n.record.generation<<",\"format\":"<<n.record.description.format
          <<",\"width\":"<<n.record.description.width<<",\"height\":"<<n.record.description.height;
        finish(s);output(sink,s);
    }catch(...){}
}
void destroyingImage(Context* c,std::uint64_t image) noexcept {guarded(c,[&]{
    const auto key=c->selected(image);if(!key.image)return;
    for(auto& r:c->roles)for(auto* s:{&r.current,&r.previous})if(s->key.image==image)*s={};
    c->tracker.destroying(key);
});}
void queue(Context* c,VkQueue queue,std::uint32_t family) noexcept {guarded(c,[&]{
    const auto handle=bits(queue);if(!handle)return;
    if(auto* q=c->queue(handle)){if(q->family!=family)c->tracker.untrackedDeviceOperation(image_sync::Issue::QueueFamilyMismatch);return;}
    for(auto& q:c->queues)if(!q.handle){q={handle,0,family};return;}
    c->tracker.untrackedDeviceOperation(image_sync::Issue::Capacity);
});}
void pool(Context* c,VkCommandPool pool,std::uint32_t family) noexcept {guarded(c,[&]{
    for(auto& p:c->pools)if(!p.handle){p={bits(pool),family};return;}
    c->tracker.untrackedDeviceOperation(image_sync::Issue::Capacity);
});}
void allocate(Context* c,const VkCommandBufferAllocateInfo* info,const VkCommandBuffer* buffers) noexcept {guarded(c,[&]{
    if(!info||!buffers)return;std::uint32_t family=VK_QUEUE_FAMILY_IGNORED;
    for(const auto& p:c->pools)if(p.handle==bits(info->commandPool)){family=p.family;break;}
    if(info->commandBufferCount>image_sync::Tracker::maxCommands){c->tracker.untrackedDeviceOperation(image_sync::Issue::Capacity);return;}
    for(std::uint32_t i=0;i<info->commandBufferCount;++i)c->tracker.allocate(bits(buffers[i]),bits(info->commandPool),family,info->level);
});}
void free(Context* c,std::uint32_t count,const VkCommandBuffer* buffers) noexcept {guarded(c,[&]{
    if(count>image_sync::Tracker::maxCommands){c->tracker.untrackedDeviceOperation(image_sync::Issue::Capacity);return;}
    if(buffers)for(std::uint32_t i=0;i<count;++i)c->tracker.free(bits(buffers[i]));
});}
void resetPool(Context* c,VkCommandPool pool,bool destroy) noexcept {guarded(c,[&]{
    c->tracker.resetPool(bits(pool),destroy);if(destroy)for(auto& p:c->pools)if(p.handle==bits(pool))p={};
});}
void begin(Context* c,VkCommandBuffer b,const VkCommandBufferBeginInfo* info,VkResult r) noexcept {guarded(c,[&]{
    c->tracker.begin(bits(b),info?info->flags:0,r);
    if(info&&info->pNext)c->tracker.opaque(bits(b),image_sync::Issue::UnsupportedExtension);
});}
void end(Context* c,VkCommandBuffer b,VkResult r) noexcept {guarded(c,[&]{c->tracker.end(bits(b),r);});}
void reset(Context* c,VkCommandBuffer b,VkResult r) noexcept {guarded(c,[&]{c->tracker.reset(bits(b),r);});}
void opaque(Context* c,VkCommandBuffer b,image_sync::Issue issue) noexcept {guarded(c,[&]{c->tracker.opaque(bits(b),issue);});}
void barriers(Context* c,VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
              std::uint32_t count,const VkImageMemoryBarrier* barriers) noexcept {guarded(c,[&]{
    if(c->retired)return;
    if(count>4096||(!barriers&&count)){c->tracker.opaque(bits(cb),image_sync::Issue::Capacity);return;}
    for(std::uint32_t i=0;i<count;++i){const auto& v=barriers[i];const auto key=c->selected(bits(v.image));if(!key.image)continue;
        image_sync::Barrier b;b.image=key;b.range=v.subresourceRange;b.oldLayout=v.oldLayout;b.newLayout=v.newLayout;
        b.srcQueueFamily=v.srcQueueFamilyIndex;b.dstQueueFamily=v.dstQueueFamilyIndex;b.srcStages=src;b.dstStages=dst;
        b.srcAccess=v.srcAccessMask;b.dstAccess=v.dstAccessMask;b.unsupportedExtension=v.pNext||flags;
        c->tracker.barrier(bits(cb),b);
    }
});}
void barriers2(Context* c,VkCommandBuffer cb,const VkDependencyInfo* info) noexcept {guarded(c,[&]{
    if(c->retired||!info)return;
    if(info->imageMemoryBarrierCount>4096||(!info->pImageMemoryBarriers&&info->imageMemoryBarrierCount)){
        c->tracker.opaque(bits(cb),image_sync::Issue::Capacity);return;}
    for(std::uint32_t i=0;i<info->imageMemoryBarrierCount;++i){const auto& v=info->pImageMemoryBarriers[i];const auto key=c->selected(bits(v.image));if(!key.image)continue;
        image_sync::Barrier b;b.image=key;b.range=v.subresourceRange;b.oldLayout=v.oldLayout;b.newLayout=v.newLayout;
        b.srcQueueFamily=v.srcQueueFamilyIndex;b.dstQueueFamily=v.dstQueueFamilyIndex;b.srcStages=v.srcStageMask;b.dstStages=v.dstStageMask;
        b.srcAccess=v.srcAccessMask;b.dstAccess=v.dstAccessMask;b.synchronization2Command=true;
        b.unsupportedExtension=v.pNext||info->pNext||info->dependencyFlags;c->tracker.barrier(bits(cb),b);
    }
});}
SubmitTicket beginSubmit(Context* c,VkQueue queue) noexcept {
    SubmitTicket ticket;guarded(c,[&]{ticket.queue=bits(queue);ticket.token=++c->submitToken;
        auto* q=c->queue(ticket.queue);
        if(!q){c->tracker.untrackedDeviceOperation(image_sync::Issue::QueueFamilyMismatch);return;}
        if(q->inFlight++){++c->queueOverlaps;c->tracker.untrackedDeviceOperation(image_sync::Issue::SubmitOrderAmbiguous);}
    });return ticket;
}
void submit(Context* c,SubmitTicket ticket,std::uint32_t count,const VkSubmitInfo* infos,VkFence fence,VkResult result) noexcept {guarded(c,[&]{
    std::array<image_sync::Handle,image_sync::Tracker::maxCommands> buffers{};std::size_t used=0;bool audited=true;
    if(count>batchLimit||(!infos&&count)){c->tracker.untrackedDeviceOperation(image_sync::Issue::Capacity);audited=false;}
    else for(std::uint32_t i=0;i<count;++i){const auto& info=infos[i];
        if(info.pNext)audited=false;
        if(info.commandBufferCount>buffers.size()-used||(!info.pCommandBuffers&&info.commandBufferCount)){
            c->tracker.untrackedDeviceOperation(image_sync::Issue::Capacity);audited=false;used=0;break;}
        for(std::uint32_t j=0;j<info.commandBufferCount;++j)buffers[used++]=bits(info.pCommandBuffers[j]);
    }
    c->submitted(ticket,{buffers.data(),used},fence,result,audited);
});}
void submit2(Context* c,SubmitTicket ticket,std::uint32_t count,const VkSubmitInfo2* infos,VkFence fence,VkResult result) noexcept {guarded(c,[&]{
    std::array<image_sync::Handle,image_sync::Tracker::maxCommands> buffers{};std::size_t used=0;bool audited=true;
    if(count>batchLimit||(!infos&&count)){c->tracker.untrackedDeviceOperation(image_sync::Issue::Capacity);audited=false;}
    else for(std::uint32_t i=0;i<count;++i){const auto& info=infos[i];if(info.pNext||info.flags)audited=false;
        if(info.commandBufferInfoCount>buffers.size()-used||(!info.pCommandBufferInfos&&info.commandBufferInfoCount)){
            c->tracker.untrackedDeviceOperation(image_sync::Issue::Capacity);audited=false;used=0;break;}
        for(std::uint32_t j=0;j<info.commandBufferInfoCount;++j){const auto& b=info.pCommandBufferInfos[j];
            if(b.pNext||(b.deviceMask!=0&&b.deviceMask!=1))audited=false;buffers[used++]=bits(b.commandBuffer);}
    }
    c->submitted(ticket,{buffers.data(),used},fence,result,audited);
});}
image_sync::Stats stats(Context* c) noexcept {image_sync::Stats out;guarded(c,[&]{out=c->tracker.stats();});return out;}
image_sync::Evidence evidence(Context* c,image_sync::ImageKey key) noexcept {image_sync::Evidence out;guarded(c,[&]{out=c->tracker.snapshot(key);});return out;}
} // namespace xess_vk::candidate_sync
