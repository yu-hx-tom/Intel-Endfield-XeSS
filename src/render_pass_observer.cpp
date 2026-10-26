#include "render_pass_observer.hpp"
#include "render_pass_diagnostics.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <type_traits>

namespace xess_vk::render_pass_observer {
namespace {
namespace rp=render_pass_metadata;
namespace rd=render_pass_diagnostics;
using Reason=CommandRecordingReason;
constexpr std::size_t imageLimit=1024, commandLimit=128, poolLimit=128, queueLimit=32, batchLimit=64;
static_assert(commandLimit<=255,"command hints store slot+1 in one byte");
std::atomic<std::uint64_t> nextContextIncarnation{};
std::uint64_t newIncarnation() noexcept {
    auto old=nextContextIncarnation.load(std::memory_order_relaxed);
    while(old!=UINT64_MAX){if(nextContextIncarnation.compare_exchange_weak(old,old+1,std::memory_order_relaxed))return old+1;}
    return 0; // Exhaustion must not reuse an incarnation.
}
constexpr std::uint64_t operationLimitPerRole=256, submissionSummaryLimit=128;
constexpr std::uint64_t projectionOperationLimitPerRole=256,projectionSummaryLimit=128;
constexpr VkPipelineStageFlags2 supportedSubmitStages=VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT|VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
    |VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT
    |VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT|VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
    |VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
    |VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT|VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT
    |VK_PIPELINE_STAGE_2_HOST_BIT|VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT|VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
    |VK_PIPELINE_STAGE_2_COPY_BIT|VK_PIPELINE_STAGE_2_RESOLVE_BIT|VK_PIPELINE_STAGE_2_BLIT_BIT|VK_PIPELINE_STAGE_2_CLEAR_BIT
    |VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT|VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT|VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
constexpr VkImageUsageFlags attachmentUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
struct ErrorGuard { DWORD saved=GetLastError(); ~ErrorGuard(){SetLastError(saved);} };
template<class T> std::uint64_t bits(T handle) noexcept {
    if constexpr(std::is_pointer_v<T>) return reinterpret_cast<std::uint64_t>(handle);
    else return static_cast<std::uint64_t>(handle);
}
const char* roleName(unsigned role) noexcept { return role==0 ? "motion" : role==1 ? "depth" : "real_color"; }
const char* sourceName(rp::SourceKind source) noexcept {
    switch(source) {
    case rp::SourceKind::InitialLayoutAssertion:return "initial_layout_assertion";
    case rp::SourceKind::SubpassReferenceTransition:return "subpass_reference_transition";
    case rp::SourceKind::UnusedInitialToFinalTransition:return "unused_initial_to_final_transition";
    default:return "final_layout_transition";
    }
}
}
struct Context {
    struct Image { rp::Key key; bool swapchain{}; };
    struct Command { std::uint64_t handle{},pool{},order{},allocationIssues{},allocationReasons{},lastFinishedSubmitToken{};rp::Key key;VkCommandBufferLevel level{VK_COMMAND_BUFFER_LEVEL_MAX_ENUM};CommandRecordingFacts facts; };
    struct Pool {std::uint64_t handle{},generation{},reasons{};VkCommandPoolCreateFlags flags{};std::uint32_t family{VK_QUEUE_FAMILY_IGNORED};};
    struct Queue { std::uint64_t handle{},activeToken{};std::uint32_t family{};std::uint32_t inFlight{};bool familyKnown{true}; };
    struct Selection { rp::Key current,previous;std::uint64_t emitted{}; };
    struct ProjectionSelection { rp::ProjectionSelection binding;std::uint64_t emitted{}; };
    struct SwapchainObservation { rp::ImageDescription description;std::uint64_t issues{};bool emitted{}; };
    std::uint64_t device{},contextIncarnation{},commandGeneration{},poolGeneration{},submitToken{},recordingFatalReasons{};
    Sink sink{};const InventoryReader inventoryReader;std::mutex mutex;rp::Metadata metadata;
    std::array<Image,imageLimit> images{};
    std::array<Command,commandLimit> commands{};
    std::array<Pool,poolLimit> pools{};
    std::array<Queue,queueLimit> queues{};
    std::array<Selection,3> selections{};
    std::array<ProjectionSelection,rp::projectionSlots> projectionSelections{};
    std::uint64_t projectionSelectionGeneration{},projectionFatalIssues{};
    std::array<SwapchainObservation,8> swapchainObservations{};std::size_t swapchainObservationCount{};
    std::array<rp::ViewSnapshot,256> ignoredViews{};std::size_t ignoredViewCursor{};
    Stats totals;
    rd::Store diagnostics;
    std::atomic<std::uint64_t> failures{};
    bool retired{};
    std::array<std::atomic<std::uint8_t>,256> commandHints{}; // slot+1 per handle hash; only a candidate, always revalidated
    std::atomic<std::uint64_t> factsQueries{},factsLockContended{},commandLookupScans{}; // written under mutex, read lock-free
    Context(VkDevice d,Sink s,InventoryReader reader):device(bits(d)),contextIncarnation(newIncarnation()),sink(s),inventoryReader(reader){if(!contextIncarnation)recordingFatalReasons|=bit(Reason::Capacity);}
    static std::size_t hintIndex(std::uint64_t h) noexcept {return ((h>>4)^(h>>12)^(h>>20)^(h>>32))&255;}
    static void bump(std::atomic<std::uint64_t>& value) noexcept {value.store(value.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);} // under mutex
    Image* image(std::uint64_t h) noexcept { if(h)for(auto& i:images)if(i.key.handle==h)return &i;return nullptr; }
    // Allocation rejects a live duplicate handle, so the hinted slot and the
    // scan can only name the same command. Callers hold `mutex`.
    Command* command(std::uint64_t h) noexcept {
        if(!h)return nullptr;
        auto& hint=commandHints[hintIndex(h)];
        if(const auto slot=hint.load(std::memory_order_relaxed);slot&&commands[slot-1].handle==h)return &commands[slot-1];
        bump(commandLookupScans);
        for(std::size_t n=0;n<commands.size();++n)if(commands[n].handle==h){hint.store(std::uint8_t(n+1),std::memory_order_relaxed);return &commands[n];}
        return nullptr;
    }
    Queue* queue(std::uint64_t h) noexcept { if(h)for(auto& q:queues)if(q.handle==h)return &q;return nullptr; }
    Pool* pool(std::uint64_t h) noexcept {if(h)for(auto& p:pools)if(p.handle==h)return &p;return nullptr;}
    CommandRecordingFacts facts(Command* command,std::uint64_t handle) const noexcept {
        CommandRecordingFacts out;if(command)out=command->facts;
        out.device=device;out.contextIncarnation=contextIncarnation;out.command=handle;
        if(command){out.rpRecordingGeneration=command->key.generation;out.commonRecordOrder=command->order;}
        out.reasons|=recordingFatalReasons;
        if(retired)out.reasons|=bit(Reason::Stopped);
        if(failures)out.reasons|=bit(Reason::DriverStateUnknown);
        if(retired||failures||recordingFatalReasons){out.stateKnown=false;out.recording=out.executable=out.scopeKnown=false;}
        return out;
    }
    void uncertain(Command& command,Reason reason) noexcept {
        auto& f=command.facts;f.reasons|=bit(reason)|bit(Reason::DriverStateUnknown);
        f.stateKnown=f.recording=f.executable=f.scopeKnown=false;
    }
    void resetFacts(Command& command) noexcept {
        auto& f=command.facts;f.rpRecordingGeneration=0;f.commonRecordOrder=0;
        f.beginUsageFlags=0;f.beginFlagsKnown=false;f.stateKnown=true;f.recording=f.executable=f.endSucceeded=false;
        f.scopeKnown=false;f.inRenderPass=f.inDynamicRendering=false;f.successfulSubmissionCount=0;
        f.lastSubmitQueue=f.lastSubmitSerial=0;f.lastSubmitQueueKnown=false;f.reasons=command.allocationReasons|bit(Reason::NotRecording);
        command.key={};command.order=command.lastFinishedSubmitToken=0;
    }
    void scopeBegin(Command& command,bool dynamic,bool valid) noexcept {
        auto& f=command.facts;
        if(!f.recording||!f.stateKnown||!f.scopeKnown||f.inRenderPass||f.inDynamicRendering||!valid){
            f.scopeKnown=false;f.reasons|=bit(Reason::ScopeUnknown);return;
        }
        f.inRenderPass=!dynamic;f.inDynamicRendering=dynamic;
    }
    void scopeEnd(Command& command,bool dynamic,bool valid=true) noexcept {
        auto& f=command.facts;
        if(!f.recording||!f.stateKnown||!f.scopeKnown||!(dynamic?f.inDynamicRendering:f.inRenderPass)||!valid){
            f.scopeKnown=false;f.reasons|=bit(Reason::ScopeUnknown);return;
        }
        f.inRenderPass=f.inDynamicRendering=false;
    }
    void result(rp::Result r,bool object) noexcept {
        if(r.issues){totals.observedIssues|=r.issues;if(object)++totals.objectRejections;else ++totals.recordingRejections;}
    }
    void fatal(rp::Issue issue) noexcept { totals.fatalIssues|=rp::bit(issue); }
    rp::Result registerImage(const rp::ImageDescription& description,bool swapchain) noexcept {
        if(image(description.key.handle)){fatal(rp::Issue::Lifecycle);return {description.key,rp::bit(rp::Issue::Lifecycle)};}
        Image* entry=nullptr;for(auto& item:images)if(!item.key.handle){entry=&item;break;}
        if(!entry){fatal(rp::Issue::Capacity);return {description.key,rp::bit(rp::Issue::Capacity)};}
        entry->key=description.key;entry->swapchain=swapchain;
        return metadata.observeImage(description,VK_SUCCESS);
    }
    void projectSelection(unsigned slot,const image_inventory::Record* record) noexcept {
        auto& current=projectionSelections[slot].binding;rp::ProjectionSelection desired;
        if(record) {
            const auto snapshot=metadata.imageSnapshot(record->image);
            const rp::Key key{record->image,record->generation};
            if(!snapshot.known || snapshot.description.key!=key || snapshot.issues) {
                rd::increment(totals.projection.selectionRejections);
                totals.projection.issues |= !snapshot.known ? rp::bit(rp::Issue::UnknownObject) :
                    snapshot.description.key!=key ? rp::bit(rp::Issue::StaleGeneration) : snapshot.issues;
            } else {
                // An accepted immutable R0 image is audited 2D/one mip/layer.
                // Role::Depth may be R32F COLOR; never infer aspect from role.
                const auto format=snapshot.description.format;
                const bool depth=format==VK_FORMAT_D16_UNORM||format==VK_FORMAT_X8_D24_UNORM_PACK32||format==VK_FORMAT_D32_SFLOAT;
                desired.enabled=true;desired.image=key;
                desired.range={depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            }
        }
        const auto& a=current.range;const auto& b=desired.range;
        if(current.enabled==desired.enabled && current.image==desired.image && a.aspectMask==b.aspectMask && a.baseMipLevel==b.baseMipLevel
            && a.levelCount==b.levelCount && a.baseArrayLayer==b.baseArrayLayer && a.layerCount==b.layerCount)return;
        if(projectionSelectionGeneration==UINT64_MAX){projectionFatalIssues|=rp::bit(rp::Issue::Capacity);totals.projection.issues|=projectionFatalIssues;return;}
        desired.selectionSerial=++projectionSelectionGeneration;
        // Source/pass tokens intentionally stay zero. No callback/frame identity
        // is available through this adapter's existing select interface.
        const auto result=metadata.setProjectionSelection(slot,desired);
        if(result.issues){rd::increment(totals.projection.selectionRejections);totals.projection.issues|=result.issues;projectionFatalIssues|=result.issues;return;}
        current=desired;rd::increment(totals.projection.selectionChanges);
    }
    void rememberIgnoredView(const VkImageViewCreateInfo& info,VkImageView view) noexcept {
        auto& facts=ignoredViews[ignoredViewCursor++%ignoredViews.size()];if(facts.key.handle)rd::increment(totals.diagnosticIgnoredViewEvictions);
        facts={};facts.key={bits(view),0};facts.image={bits(info.image),0};facts.hasPNext=info.pNext!=nullptr;
        facts.type=info.viewType;facts.format=info.format;facts.flags=info.flags;facts.components=info.components;facts.rawRange=info.subresourceRange;
        rd::Sample sample;sample.site=rd::Site::IgnoredView;sample.ignoredViewCreationKnown=true;sample.view=facts;
        sample.issues=rp::bit(rp::Issue::UnknownObject);diagnostics.observe(sample);
    }
    void diagnose(rd::Site site,rp::Result result,std::uint64_t handle,VkResult driver=VK_SUCCESS,
                  const rp::CommandSnapshot& incoming={},const VkRenderPassBeginInfo* beginInfo=nullptr,
                  VkSubpassContents contents=VK_SUBPASS_CONTENTS_INLINE,std::uint64_t order=0) noexcept {
        if(!result.issues)return;
        rd::Sample sample;sample.site=site;sample.issues=result.issues;sample.driver=driver;
        sample.command=incoming;sample.incomingIssues=incoming.issues;sample.recordOrder=order;
        if(site==rd::Site::Image)sample.image=metadata.imageSnapshot(handle);
        else if(site==rd::Site::View){sample.view=metadata.viewSnapshot(handle);sample.image=metadata.imageSnapshot(sample.view.image.handle);}
        else if(site==rd::Site::RenderPass||site==rd::Site::RenderPass2)sample.pass=metadata.renderPassSnapshot(handle);
        else if(site==rd::Site::Framebuffer){
            sample.framebuffer=metadata.framebufferSnapshot(handle);sample.pass=metadata.renderPassSnapshot(sample.framebuffer.renderPass.handle);
            const auto count=std::min<std::uint32_t>(sample.framebuffer.attachmentCount,rp::maxAttachments);
            if(!(sample.framebuffer.flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))for(std::uint32_t i=0;i<count;++i){
                sample.attachment=i;const auto viewHandle=sample.framebuffer.views[i].handle;sample.view=metadata.viewSnapshot(viewHandle);
                sample.ignoredViewCreationKnown=false;
                if(!sample.view.known)for(const auto& ignored:ignoredViews)if(ignored.key.handle&&ignored.key.handle==viewHandle){sample.view=ignored;sample.ignoredViewCreationKnown=true;break;}
                sample.image=metadata.imageSnapshot(sample.view.image.handle);diagnostics.observe(sample);
            }
            if(count&&!(sample.framebuffer.flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))return;
        }else{
            if(!sample.command.known)sample.command.key=result.key;
            sample.pass=metadata.renderPassSnapshot(incoming.activeRenderPass.handle);
            sample.framebuffer=metadata.framebufferSnapshot(incoming.activeFramebuffer.handle);
            if(beginInfo){sample.beginHasPNext=beginInfo->pNext!=nullptr;sample.contents=contents;sample.renderArea=beginInfo->renderArea;
                sample.clearValueCount=beginInfo->clearValueCount;sample.pass=metadata.renderPassSnapshot(bits(beginInfo->renderPass));
                sample.framebuffer=metadata.framebufferSnapshot(bits(beginInfo->framebuffer));}
        }
        diagnostics.observe(sample);
    }
    template<class F> void recorded(rd::Site site,Command* command,F&& operation,
                                   const VkRenderPassBeginInfo* info=nullptr,VkSubpassContents contents=VK_SUBPASS_CONTENTS_INLINE) noexcept {
        const auto incoming=metadata.commandSnapshot(command->key);const auto status=operation();result(status,false);
        diagnose(site,status,command->handle,VK_SUCCESS,incoming,info,contents,command->order);
    }
    Command* recording(VkCommandBuffer b) noexcept {
        auto* c=command(bits(b));
        if(!c||!c->key.generation){++totals.recordingRejections;totals.observedIssues|=rp::bit(rp::Issue::UnknownObject);return nullptr;}
        if(c->order==UINT64_MAX){c->facts.reasons|=bit(Reason::Capacity);c->facts.scopeKnown=false;result(metadata.unsupported(c->key,rp::Issue::Capacity),false);return nullptr;}
        ++c->order;metadata.recordingPoint(c->key,c->order);return c;
    }
};
namespace {
template<class F> void guarded(Context* c,F&& fn) noexcept {
    ErrorGuard error;
    if(!c)return;
    try {std::lock_guard lock(c->mutex);if(!c->retired)fn();}
    catch(...){++c->failures;}
}
void emit(Context* c,const char* line) noexcept {
    if(!c||!c->sink)return;
    try{c->sink(line);}catch(...){++c->failures;}
}
struct DiagnosticJson {
    char text[8192]{};std::size_t used{};bool valid{true};
    template<class... T> void append(const char* format,T... args) noexcept {
        if(!valid)return;
        int size;
        if constexpr(sizeof...(T)==0)size=std::snprintf(text+used,sizeof(text)-used,"%s",format);
        else size=std::snprintf(text+used,sizeof(text)-used,format,args...);
        if(size<0||std::size_t(size)>=sizeof(text)-used){valid=false;return;}used+=std::size_t(size);
    }
};
unsigned long long number(std::uint64_t value) noexcept{return static_cast<unsigned long long>(value);}
void emitDiagnostic(Context* c,std::size_t id,const rd::Sample& s,std::uint64_t occurrences) noexcept {
    DiagnosticJson out;const auto& image=s.image.description;const auto& view=s.view;const auto& pass=s.pass;const auto& fb=s.framebuffer;
    out.append("{\"event\":\"rp_rejection_sample\",\"pid\":%lu,\"device\":\"0x%llx\",\"sample_id\":%zu,\"site\":\"%s\","
        "\"occurrences_at_emit\":%llu,\"issues\":%llu,\"incoming_command_issues\":%llu,\"added_issue_bits\":%llu,\"driver_result\":%d,\"role_mask\":%u,"
        "\"command\":\"0x%llx\",\"rp_command_generation\":%llu,\"record_order\":%llu,\"incoming_command_known\":%s,\"incoming_in_pass\":%s,\"active_rp_established\":%s,"
        "\"active_rp\":\"0x%llx\",\"active_rp_generation\":%llu,\"attachment_index\":%u,\"ignored_view_creation_known\":%s,",
        GetCurrentProcessId(),number(c->device),id,rd::siteName(s.site),number(occurrences),number(s.issues),number(s.incomingIssues),number(s.issues&~s.incomingIssues),int(s.driver),s.roleMask,
        number(s.command.key.handle),number(s.command.key.generation),number(s.recordOrder),s.command.known?"true":"false",s.command.inPass?"true":"false",s.command.activeRenderPass.handle?"true":"false",
        number(s.command.activeRenderPass.handle),number(s.command.activeRenderPass.generation),s.attachment,s.ignoredViewCreationKnown?"true":"false");
    out.append("\"image\":{\"known\":%s,\"handle\":\"0x%llx\",\"generation\":%llu,\"issues\":%llu,\"type\":%d,\"format\":%d,\"width\":%u,\"height\":%u,\"depth\":%u,"
        "\"mips\":%u,\"layers\":%u,\"samples\":%u,\"usage\":%u,\"flags\":%u,\"tiling\":%d,\"sharing\":%d,\"has_pnext\":%s},",
        s.image.known?"true":"false",number(image.key.handle),number(image.key.generation),number(s.image.issues),int(image.type),int(image.format),image.extent.width,image.extent.height,image.extent.depth,
        image.mipLevels,image.arrayLayers,unsigned(image.samples),image.usage,image.flags,int(image.tiling),int(image.sharing),image.unsupportedPNext?"true":"false");
    out.append("\"view\":{\"known\":%s,\"handle\":\"0x%llx\",\"generation\":%llu,\"image\":\"0x%llx\",\"image_generation\":%llu,\"issues\":%llu,"
        "\"type\":%d,\"format\":%d,\"flags\":%u,\"has_pnext\":%s,\"swizzle\":[%d,%d,%d,%d],\"range\":[%u,%u,%u,%u,%u]},",
        view.known?"true":"false",number(view.key.handle),number(view.key.generation),number(view.image.handle),number(view.image.generation),number(view.issues),int(view.type),int(view.format),view.flags,view.hasPNext?"true":"false",
        int(view.components.r),int(view.components.g),int(view.components.b),int(view.components.a),view.rawRange.aspectMask,view.rawRange.baseMipLevel,view.rawRange.levelCount,view.rawRange.baseArrayLayer,view.rawRange.layerCount);
    out.append("\"framebuffer\":{\"known\":%s,\"handle\":\"0x%llx\",\"generation\":%llu,\"issues\":%llu,\"creation_rp\":\"0x%llx\",\"creation_rp_generation\":%llu,"
        "\"flags\":%u,\"width\":%u,\"height\":%u,\"layers\":%u,\"attachment_count\":%u,\"has_pnext\":%s,\"exact_rp_identity_matches\":%s,\"views\":[",
        fb.known?"true":"false",number(fb.key.handle),number(fb.key.generation),number(fb.issues),number(fb.renderPass.handle),number(fb.renderPass.generation),fb.flags,fb.width,fb.height,fb.layers,fb.attachmentCount,fb.hasPNext?"true":"false",
        (fb.known&&pass.known&&fb.renderPass==pass.key)?"true":"false");
    for(std::size_t i=0;i<std::min<std::size_t>(fb.attachmentCount,rp::maxAttachments);++i)out.append("%s{\"handle\":\"0x%llx\",\"generation\":%llu}",i?",":"",number(fb.views[i].handle),number(fb.views[i].generation));
    out.append("]},\"render_pass\":{\"known\":%s,\"handle\":\"0x%llx\",\"generation\":%llu,\"issues\":%llu,\"flags\":%u,\"has_pnext\":%s,\"attachment_count\":%u,\"subpass_count\":%u,\"dependency_count\":%u,\"attachments\":[",
        pass.known?"true":"false",number(pass.key.handle),number(pass.key.generation),number(pass.issues),pass.flags,pass.hasPNext?"true":"false",pass.attachmentCount,pass.subpassCount,pass.dependencyCount);
    for(std::size_t i=0;i<pass.copiedAttachmentCount;++i){const auto& a=pass.attachments[i];out.append("%s{\"index\":%zu,\"format\":%d,\"samples\":%u,\"flags\":%u,\"load\":%d,\"store\":%d,\"stencil_load\":%d,\"stencil_store\":%d,"
        "\"initial\":%d,\"reference\":%d,\"final\":%d,\"aspect\":%u,\"role\":%u,\"reference_count\":%u,\"has_pnext\":%s}",i?",":"",i,int(a.format),unsigned(a.samples),a.flags,int(a.loadOp),int(a.storeOp),int(a.stencilLoadOp),int(a.stencilStoreOp),
        int(a.initial),int(a.reference),int(a.final),a.aspects,a.role,a.referenceCount,a.hasPNext?"true":"false");}
    out.append("]},\"begin\":{\"has_pnext\":%s,\"contents\":%d,\"render_area\":[%d,%d,%u,%u],\"clear_count\":%u},\"diagnostic_only\":true,\"copy_permitted\":false}",
        s.beginHasPNext?"true":"false",int(s.contents),s.renderArea.offset.x,s.renderArea.offset.y,s.renderArea.extent.width,s.renderArea.extent.height,s.clearValueCount);
    if(out.valid)emit(c,out.text);else ++c->failures;
}
void flushDiagnosticSamples(Context* c) noexcept {
    try{std::lock_guard lock(c->mutex);if(c->diagnostics.totals.emittedSamples==c->diagnostics.totals.storedSamples)return;}catch(...){++c->failures;return;}
    for(std::size_t i=0;i<rd::sampleLimit;++i){rd::Sample sample;std::uint64_t occurrences{};bool ready=false;
        try{std::lock_guard lock(c->mutex);ready=c->diagnostics.take(i,sample,occurrences);}catch(...){++c->failures;return;}
        if(ready)emitDiagnostic(c,i,sample,occurrences);
    }
}
void emitDiagnosticCounts(Context* c) noexcept {
    rd::Totals totals;std::array<std::uint64_t,rd::sampleLimit> frequencies{};
    try{std::lock_guard lock(c->mutex);totals=c->diagnostics.totals;for(std::size_t i=0;i<frequencies.size();++i)frequencies[i]=c->diagnostics.buckets[i].occurrences;}catch(...){++c->failures;return;}
    DiagnosticJson out;out.append("{\"event\":\"rp_rejection_counts\",\"pid\":%lu,\"device\":\"0x%llx\",\"observations\":%llu,\"stored_samples\":%llu,\"omitted_observations\":%llu,\"emitted_samples\":%llu,\"sites\":[",
        GetCurrentProcessId(),number(c->device),number(totals.observations),number(totals.storedSamples),number(totals.omittedSamples),number(totals.emittedSamples));
    for(std::size_t i=0;i<rd::siteCount;++i)out.append("%s{\"site\":\"%s\",\"observations\":%llu,\"omitted\":%llu}",i?",":"",rd::siteName(static_cast<rd::Site>(i)),number(totals.bySite[i]),number(totals.omittedBySite[i]));
    out.append("],\"sample_occurrences\":[");bool first=true;
    for(std::size_t i=0;i<frequencies.size();++i)if(frequencies[i]){out.append("%s{\"sample_id\":%zu,\"count\":%llu}",first?"":",",i,number(frequencies[i]));first=false;}
    out.append("],\"copy_permitted\":false}");if(out.valid)emit(c,out.text);else ++c->failures;
}
void emitSubmission(Context* c,const SubmitTicket& ticket,VkFence fence,VkResult driver,rp::Key command,
                    const rp::Batch& batch,bool permit) noexcept {
    if(!permit)return;
    char line[1600];
    const int n=std::snprintf(line,sizeof(line),
        "{\"event\":\"rp_metadata_submission\",\"pid\":%lu,\"device\":\"0x%llx\",\"queue\":\"0x%llx\",\"fence\":\"0x%llx\","
        "\"rp_submit_token\":%llu,\"command\":\"0x%llx\",\"rp_command_generation\":%llu,\"driver_result\":%d,"
        "\"has_captured_recording\":%s,\"rp_recording_metadata_accepted\":%s,\"operation_count\":%zu,\"rp_issues\":%llu,"
        "\"complete_hook_coverage\":false,\"submitted_layout_known\":false,\"gpu_complete\":false,\"copy_permitted\":false}",
        GetCurrentProcessId(),static_cast<unsigned long long>(c->device),static_cast<unsigned long long>(ticket.queue),
        static_cast<unsigned long long>(bits(fence)),static_cast<unsigned long long>(ticket.token),static_cast<unsigned long long>(command.handle),
        static_cast<unsigned long long>(command.generation),int(driver),command.handle?"true":"false",batch.metadataAccepted?"true":"false",batch.count,
        static_cast<unsigned long long>(batch.issues));
    if(n>0&&std::size_t(n)<sizeof(line))emit(c,line);else ++c->failures;
}
void emitOperation(Context* c,const SubmitTicket& ticket,const rp::LayoutOperation& op,unsigned role) noexcept {
    char line[1800];
    const int n=std::snprintf(line,sizeof(line),
        "{\"event\":\"rp_layout_operation\",\"pid\":%lu,\"device\":\"0x%llx\",\"queue\":\"0x%llx\",\"rp_submit_token\":%llu,"
        "\"role\":\"%s\",\"image\":\"0x%llx\",\"image_generation\":%llu,\"image_generation_scope\":\"image_inventory\","
        "\"view\":\"0x%llx\",\"view_generation\":%llu,\"render_pass\":\"0x%llx\",\"render_pass_generation\":%llu,"
        "\"framebuffer\":\"0x%llx\",\"framebuffer_generation\":%llu,\"command\":\"0x%llx\",\"rp_command_generation\":%llu,"
        "\"record_order\":%llu,\"sequence_within_call\":%u,\"source_kind\":\"%s\",\"old_layout\":%d,\"new_layout\":%d,"
        "\"aspect_mask\":%u,\"base_mip\":%u,\"mip_count\":%u,\"base_layer\":%u,\"layer_count\":%u,"
        "\"complete_hook_coverage\":false,\"submitted_layout_known\":false,\"gpu_complete\":false,\"last_writer_known\":false,"
        "\"resource_lifetime_pinned\":false,\"copy_permitted\":false}",GetCurrentProcessId(),
        static_cast<unsigned long long>(c->device),static_cast<unsigned long long>(ticket.queue),static_cast<unsigned long long>(ticket.token),
        roleName(role),static_cast<unsigned long long>(op.image.handle),static_cast<unsigned long long>(op.image.generation),
        static_cast<unsigned long long>(op.view.handle),static_cast<unsigned long long>(op.view.generation),
        static_cast<unsigned long long>(op.renderPass.handle),static_cast<unsigned long long>(op.renderPass.generation),
        static_cast<unsigned long long>(op.framebuffer.handle),static_cast<unsigned long long>(op.framebuffer.generation),
        static_cast<unsigned long long>(op.command.handle),static_cast<unsigned long long>(op.command.generation),
        static_cast<unsigned long long>(op.recordOrder),op.sequenceWithinCall,sourceName(op.sourceKind),int(op.oldLayout),int(op.newLayout),
        op.range.aspectMask,op.range.baseMipLevel,op.range.levelCount,op.range.baseArrayLayer,op.range.layerCount);
    if(n>0&&std::size_t(n)<sizeof(line))emit(c,line);else ++c->failures;
}
const char* projectionName(rp::ProjectionClassification value) noexcept {
    switch(value){
    case rp::ProjectionClassification::DirectAttachmentLayouts:return "direct_attachment_layouts";
    case rp::ProjectionClassification::NoDirectAttachmentReference:return "no_direct_attachment_reference";
    case rp::ProjectionClassification::Refused:return "refused";
    default:return "disabled";
    }
}
void projectionEvidence(DiagnosticJson& out,const rp::ProjectionBatch& batch,unsigned role) noexcept {
    out.append("\"role\":\"%s\",\"selection_serial\":%llu,\"selection_serial_scope\":\"rp_adapter_device_binding\","
        "\"source_token\":%llu,\"pass_token\":%llu,\"source_association_known\":false,\"candidate_semantics_verified\":false,"
        "\"memory_alias_unverified\":true,\"complete_hook_coverage\":false,\"submitted_layout_known\":false,"
        "\"gpu_complete\":false,\"last_writer_known\":false,\"resource_lifetime_pinned\":false,\"copy_permitted\":false",
        roleName(role),number(batch.selection.selectionSerial),number(batch.selection.sourceToken),number(batch.selection.passToken));
}
void emitProjectionSubmission(Context* c,const SubmitTicket& ticket,const rp::ProjectionTicket& captured,
    VkResult driver,const rp::ProjectionBatch& batch,unsigned role) noexcept {
    DiagnosticJson out;
    out.append("{\"event\":\"rp_projection_submission\",\"pid\":%lu,\"device\":\"0x%llx\",\"queue\":\"0x%llx\","
        "\"rp_submit_token\":%llu,\"projection_ticket\":%llu,\"command\":\"0x%llx\",\"rp_command_generation\":%llu,"
        "\"driver_result\":%d,\"classification\":\"%s\",\"metadata_accepted\":%s,\"issues\":%llu,\"operation_count\":%zu,"
        "\"examined_passes\":%zu,\"direct_passes\":%zu,\"no_direct_passes\":%zu,\"image\":\"0x%llx\",\"image_generation\":%llu,",
        GetCurrentProcessId(),number(c->device),number(ticket.queue),number(ticket.token),number(captured.serial),number(captured.command.handle),
        number(captured.command.generation),int(driver),projectionName(batch.classification),batch.metadataAccepted?"true":"false",number(batch.issues),
        batch.count,batch.examinedPasses,batch.directPasses,batch.noDirectPasses,number(batch.selection.image.handle),number(batch.selection.image.generation));
    out.append("\"journal_observed_passes\":%llu,\"journal_retained_passes\":%llu,\"journal_dropped_passes\":%llu,\"journal_pass_capacity\":%zu,",
        number(batch.observedPasses),number(batch.retainedPasses),number(batch.droppedPasses),rp::maxProjectionPasses);
    projectionEvidence(out,batch,role);out.append("}");if(out.valid)emit(c,out.text);else ++c->failures;
}
void emitProjectionOperation(Context* c,const SubmitTicket& ticket,const rp::ProjectionBatch& batch,const rp::LayoutOperation& op,unsigned role) noexcept {
    DiagnosticJson out;
    out.append("{\"event\":\"rp_projection_layout_operation\",\"pid\":%lu,\"device\":\"0x%llx\",\"queue\":\"0x%llx\","
        "\"rp_submit_token\":%llu,\"image\":\"0x%llx\",\"image_generation\":%llu,\"image_generation_scope\":\"image_inventory\","
        "\"view\":\"0x%llx\",\"view_generation\":%llu,\"render_pass\":\"0x%llx\",\"render_pass_generation\":%llu,"
        "\"framebuffer\":\"0x%llx\",\"framebuffer_generation\":%llu,\"command\":\"0x%llx\",\"rp_command_generation\":%llu,"
        "\"record_order\":%llu,\"sequence_within_call\":%u,\"source_kind\":\"%s\",\"old_layout\":%d,\"new_layout\":%d,"
        "\"aspect_mask\":%u,\"base_mip\":%u,\"mip_count\":%u,\"base_layer\":%u,\"layer_count\":%u,",
        GetCurrentProcessId(),number(c->device),number(ticket.queue),number(ticket.token),number(op.image.handle),number(op.image.generation),
        number(op.view.handle),number(op.view.generation),number(op.renderPass.handle),number(op.renderPass.generation),number(op.framebuffer.handle),
        number(op.framebuffer.generation),number(op.command.handle),number(op.command.generation),number(op.recordOrder),op.sequenceWithinCall,
        sourceName(op.sourceKind),int(op.oldLayout),int(op.newLayout),op.range.aspectMask,op.range.baseMipLevel,op.range.levelCount,op.range.baseArrayLayer,op.range.layerCount);
    projectionEvidence(out,batch,role);out.append("}");if(out.valid)emit(c,out.text);else ++c->failures;
}
void emitProjectionSummary(Context* c,const ProjectionStats& s) noexcept {
    DiagnosticJson out;
    out.append("{\"event\":\"rp_projection_summary\",\"pid\":%lu,\"device\":\"0x%llx\","
        "\"selection_changes\":%llu,\"selection_rejections\":%llu,\"captured_commands\":%llu,\"completed_commands\":%llu,"
        "\"accepted_slots\":%llu,\"refused_slots\":%llu,\"disabled_slots\":%llu,\"no_direct_slots\":%llu,\"layout_operations\":%llu,"
        "\"emitted_operations\":%llu,\"omitted_operations\":%llu,\"emitted_summaries\":%llu,\"omitted_summaries\":%llu,\"issues\":%llu,"
        "\"swapchain_images_registered\":%llu,\"swapchain_image_rejections\":%llu,\"swapchain_image_duplicates\":%llu,"
        "\"swapchain_samples_emitted\":%llu,\"swapchain_samples_omitted\":%llu,"
        "\"swapchain_images_retired\":%llu,\"swapchain_retirements_ignored\":%llu,"
        "\"journal_observed_passes\":%llu,\"journal_retained_passes\":%llu,\"journal_dropped_passes\":%llu,\"journal_max_observed_passes\":%llu,\"journal_pass_capacity\":%zu,"
        "\"source_association_known\":false,\"candidate_semantics_verified\":false,\"memory_alias_unverified\":true,"
        "\"complete_hook_coverage\":false,\"gpu_complete\":false,\"resource_lifetime_pinned\":false,\"copy_permitted\":false}",
        GetCurrentProcessId(),number(c->device),number(s.selectionChanges),number(s.selectionRejections),number(s.capturedCommands),number(s.completedCommands),
        number(s.acceptedSlots),number(s.refusedSlots),number(s.disabledSlots),number(s.noDirectSlots),number(s.layoutOperations),
        number(s.emittedOperations),number(s.omittedOperations),number(s.emittedSummaries),number(s.omittedSummaries),number(s.issues),
        number(s.swapchainImagesRegistered),number(s.swapchainImageRejections),number(s.swapchainImageDuplicates),number(s.swapchainSamplesEmitted),number(s.swapchainSamplesOmitted),
        number(s.swapchainImagesRetired),number(s.swapchainRetirementsIgnored),
        number(s.journalObservedPasses),number(s.journalRetainedPasses),number(s.journalDroppedPasses),number(s.journalMaxObservedPasses),rp::maxProjectionPasses);
    if(out.valid)emit(c,out.text);else ++c->failures;
}
void addCounter(std::uint64_t& value,std::size_t amount) noexcept { value=amount>UINT64_MAX-value ? UINT64_MAX : value+amount; }
bool sameImageDescription(const rp::ImageDescription& a,const rp::ImageDescription& b) noexcept {
    return a.key==b.key&&a.type==b.type&&a.format==b.format&&a.extent.width==b.extent.width&&a.extent.height==b.extent.height
        &&a.extent.depth==b.extent.depth&&a.mipLevels==b.mipLevels&&a.arrayLayers==b.arrayLayers&&a.samples==b.samples
        &&a.usage==b.usage&&a.flags==b.flags&&a.tiling==b.tiling&&a.sharing==b.sharing&&a.unsupportedPNext==b.unsupportedPNext;
}
void flushSwapchainObservations(Context* c) noexcept {
    for(std::size_t n=0;n<c->swapchainObservations.size();++n) {
        Context::SwapchainObservation sample;bool ready=false;
        try{std::lock_guard lock(c->mutex);if(n<c->swapchainObservationCount&&!c->swapchainObservations[n].emitted){
            sample=c->swapchainObservations[n];c->swapchainObservations[n].emitted=true;rd::increment(c->totals.projection.swapchainSamplesEmitted);ready=true;}
        }catch(...){++c->failures;return;}
        if(!ready)continue;
        const auto& image=sample.description;DiagnosticJson out;
        out.append("{\"event\":\"rp_swapchain_image_observation\",\"pid\":%lu,\"device\":\"0x%llx\","
            "\"identity_source\":\"audited_swapchain_enumeration\",\"image\":\"0x%llx\",\"image_generation\":%llu,"
            "\"image_generation_scope\":\"image_inventory\",\"format\":%d,\"width\":%u,\"height\":%u,\"mip_levels\":%u,\"array_layers\":%u,"
            "\"usage\":%u,\"flags\":%u,\"unsupported_pnext\":%s,\"issues\":%llu,"
            "\"resource_lifetime_pinned\":false,\"gpu_complete\":false,\"copy_permitted\":false}",
            GetCurrentProcessId(),number(c->device),number(image.key.handle),number(image.key.generation),int(image.format),image.extent.width,image.extent.height,
            image.mipLevels,image.arrayLayers,image.usage,image.flags,image.unsupportedPNext?"true":"false",number(sample.issues));
        if(out.valid)emit(c,out.text);else ++c->failures;
    }
}
SubmitTicket start(Context* c,VkQueue queue) noexcept {
    SubmitTicket t;t.queue=bits(queue);t.device=c->device;t.contextIncarnation=c->contextIncarnation;
    if(c->submitToken==UINT64_MAX){t.issues|=rp::bit(rp::Issue::Capacity);c->fatal(rp::Issue::Capacity);return t;}
    t.token=++c->submitToken;
    auto* q=c->queue(t.queue);
    if(!q){t.issues|=rp::bit(rp::Issue::UnknownObject);return t;}
    t.queueKnown=q->familyKnown;t.queueFamily=q->family;
    if(q->inFlight){++c->totals.queueOverlaps;c->fatal(rp::Issue::Lifecycle);t.issues|=rp::bit(rp::Issue::Lifecycle);}
    else q->activeToken=t.token;
    ++q->inFlight;return t;
}
void capture(Context* c,SubmitTicket& t,VkCommandBuffer buffer) noexcept {
    auto* command=c->command(bits(buffer));
    if(!command||!command->key.generation){t.issues|=rp::bit(rp::Issue::UnknownObject);return;}
    if(t.commandCount>=t.commands.size()){t.issues|=rp::bit(rp::Issue::Capacity);return;}
    const auto index=t.commandCount++;t.commands[index]=command->key;
    t.recordingFacts[index]=c->facts(command,command->handle);
    t.projections[index]=c->metadata.captureProjectionSubmission(command->key);
    rd::increment(c->totals.projection.capturedCommands);
}
void inspectSemaphores(SubmitTicket& t,std::size_t& inspected,std::uint32_t count,const VkSemaphoreSubmitInfo* infos) noexcept {
    if(count>maxSubmittedSemaphores-inspected||(!infos&&count)){t.issues|=rp::bit(rp::Issue::Capacity);return;}
    inspected+=count;
    for(std::uint32_t i=0;i<count;++i){const auto& info=infos[i];
        if(info.sType!=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO||info.pNext||info.deviceIndex
            ||(info.stageMask&~supportedSubmitStages))t.issues|=rp::bit(rp::Issue::UnsupportedExtension);
        if(!bits(info.semaphore))t.issues|=rp::bit(rp::Issue::InvalidInput);
        // The actual semaphore type, payload and completion are not inferred.
    }
}
}
Context* create(VkDevice device,Sink sink,InventoryReader reader) noexcept {ErrorGuard e;try{return new Context(device,sink,reader);}catch(...){return nullptr;}}
DiagnosticPassSnapshot diagnosticPassSnapshot(Context* c,VkCommandBuffer buffer,const VkRenderPassBeginInfo* info) noexcept {
    ErrorGuard e; DiagnosticPassSnapshot out; if(!c)return out;
    try {
        std::lock_guard lock(c->mutex);
        const auto* command=c->command(bits(buffer)); if(!command)return out;
        out.recording=c->facts(c->command(bits(buffer)),bits(buffer));
        out.command=c->metadata.commandSnapshot(command->key);
        if(!info||info->sType!=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO)return out;
        out.pass=c->metadata.renderPassSnapshot(bits(info->renderPass));
        out.framebuffer=c->metadata.framebufferSnapshot(bits(info->framebuffer));
        out.count=std::min<std::size_t>(out.framebuffer.attachmentCount,out.views.size());
        for(std::size_t n=0;n<out.count;++n) {
            out.views[n]=c->metadata.viewSnapshot(out.framebuffer.views[n].handle);
            out.images[n]=c->metadata.imageSnapshot(out.views[n].image.handle);
        }
    } catch (...) { ++c->failures; }
    return out;
}
rp::ViewSnapshot diagnosticViewSnapshot(Context* c,VkImageView view) noexcept {
    ErrorGuard e;rp::ViewSnapshot out;if(!c)return out;
    try{std::lock_guard lock(c->mutex);out=c->metadata.viewSnapshot(bits(view));
        if(!out.known)for(const auto& candidate:c->ignoredViews)if(candidate.key.handle==bits(view)){out=candidate;break;}
    }catch(...){++c->failures;}return out;
}
void destroy(Context* c) noexcept {ErrorGuard e;delete c;}
void retire(Context* c) noexcept {guarded(c,[&]{c->retired=true;c->metadata.stop();c->fatal(rp::Issue::Stopped);});}
Stats stats(Context* c) noexcept {
    ErrorGuard e;Stats result;if(!c)return result;
    try{std::lock_guard lock(c->mutex);result=c->totals;result.metadata=c->metadata.stats();result.failures=c->failures;
        result.diagnosticObservations=c->diagnostics.totals.observations;result.diagnosticSamples=c->diagnostics.totals.storedSamples;
        result.diagnosticOmitted=c->diagnostics.totals.omittedSamples;result.diagnosticEmitted=c->diagnostics.totals.emittedSamples;
        result.fatalIssues|=result.metadata.globalIssues;if(result.failures)result.fatalIssues|=rp::bit(rp::Issue::Lifecycle);
    }catch(...){++c->failures;}return result;
}
void summary(Context* c) noexcept {
    ErrorGuard e;if(!c)return;flushDiagnosticSamples(c);flushSwapchainObservations(c);const auto s=stats(c);char line[2100];
    const int n=std::snprintf(line,sizeof(line),
        "{\"event\":\"rp_metadata_summary\",\"pid\":%lu,\"device\":\"0x%llx\",\"storage_bytes\":%zu,\"storage_mode\":\"paged_payload\",\"registrations\":%llu,"
        "\"object_rejections\":%llu,\"recording_rejections\":%llu,\"ignored_non_attachment_images\":%llu,\"ignored_untracked_views\":%llu,"
        "\"successful_driver_submissions\":%llu,\"failed_driver_submissions\":%llu,\"accepted_recordings\":%llu,\"refused_recordings\":%llu,"
        "\"accepted_layout_operations\":%llu,"
        "\"emitted_operations\":%llu,\"omitted_operations\":%llu,\"emitted_submission_summaries\":%llu,\"omitted_submission_summaries\":%llu,"
        "\"observed_issue_history\":%llu,\"fatal_issues\":%llu,\"failures\":%llu,\"queue_overlaps\":%llu,"
        "\"diagnostic_observations\":%llu,\"diagnostic_samples\":%llu,\"diagnostic_omitted\":%llu,\"diagnostic_emitted\":%llu,\"diagnostic_ignored_view_evictions\":%llu,"
        "\"complete_hook_coverage\":false,\"gpu_complete\":false,\"copy_permitted\":false}",GetCurrentProcessId(),
        static_cast<unsigned long long>(c->device),s.metadata.storageBytes,static_cast<unsigned long long>(s.metadata.registrations),
        static_cast<unsigned long long>(s.objectRejections),static_cast<unsigned long long>(s.recordingRejections),
        static_cast<unsigned long long>(s.ignoredNonAttachmentImages),static_cast<unsigned long long>(s.ignoredUntrackedViews),
        static_cast<unsigned long long>(s.successfulDriverSubmissions),static_cast<unsigned long long>(s.failedDriverSubmissions),
        static_cast<unsigned long long>(s.acceptedRecordings),static_cast<unsigned long long>(s.refusedRecordings),
        static_cast<unsigned long long>(s.acceptedLayoutOperations),
        static_cast<unsigned long long>(s.emittedOperations),static_cast<unsigned long long>(s.omittedOperations),
        static_cast<unsigned long long>(s.emittedSummaries),static_cast<unsigned long long>(s.omittedSummaries),
        static_cast<unsigned long long>(s.observedIssues),static_cast<unsigned long long>(s.fatalIssues),
        static_cast<unsigned long long>(s.failures),static_cast<unsigned long long>(s.queueOverlaps),number(s.diagnosticObservations),number(s.diagnosticSamples),number(s.diagnosticOmitted),number(s.diagnosticEmitted),number(s.diagnosticIgnoredViewEvictions));
    if(n>0&&std::size_t(n)<sizeof(line))emit(c,line);else ++c->failures;
    emitDiagnosticCounts(c);
    emitProjectionSummary(c,s.projection);
}
void imageCreated(Context* c,const VkImageCreateInfo* info,VkImage image,VkResult driver,const image_inventory::Record* record) noexcept {guarded(c,[&]{
    if(driver!=VK_SUCCESS){if(driver==VK_ERROR_DEVICE_LOST)c->fatal(rp::Issue::DeviceLost);return;}
    if(!info||info->sType!=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO||!record||record->image!=bits(image)||!record->generation){
        c->result({{},rp::bit(rp::Issue::InvalidInput)},true);return;}
    const auto& inventory=record->description;
    if(inventory.format!=std::uint32_t(info->format)||inventory.width!=info->extent.width||inventory.height!=info->extent.height
        ||inventory.depth!=info->extent.depth||inventory.mipLevels!=info->mipLevels||inventory.arrayLayers!=info->arrayLayers
        ||inventory.samples!=std::uint32_t(info->samples)||inventory.usage!=info->usage||inventory.flags!=info->flags
        ||inventory.tiling!=std::uint32_t(info->tiling)||inventory.sharingMode!=std::uint32_t(info->sharingMode)
        ||inventory.initialLayout!=std::uint32_t(info->initialLayout)){
        c->result({{},rp::bit(rp::Issue::InvalidInput)},true);return;}
    if(!(info->usage&attachmentUsage)){++c->totals.ignoredNonAttachmentImages;return;}
    if(c->image(bits(image))){c->fatal(rp::Issue::Lifecycle);return;}
    Context::Image* entry=nullptr;for(auto& i:c->images)if(!i.key.handle){entry=&i;break;}
    if(!entry){c->fatal(rp::Issue::Capacity);return;}
    rp::ImageDescription d;d.key={record->image,record->generation};d.type=info->imageType;d.format=info->format;d.extent=info->extent;
    d.mipLevels=info->mipLevels;d.arrayLayers=info->arrayLayers;d.samples=info->samples;d.usage=info->usage;d.flags=info->flags;
    d.tiling=info->tiling;d.sharing=info->sharingMode;d.unsupportedPNext=info->pNext!=nullptr;
    entry->key=d.key;const auto result=c->metadata.observeImage(d,driver);c->result(result,true);c->diagnose(rd::Site::Image,result,bits(image),driver);
});}
void swapchainImageObserved(Context* c,const rp::ImageDescription& description) noexcept {guarded(c,[&]{
    auto& totals=c->totals.projection;rp::Result result;result.key=description.key;
    if(!description.key.handle||!description.key.generation)result.issues=rp::bit(rp::Issue::InvalidInput);
    else if(auto* existing=c->image(description.key.handle)) {
        const auto snapshot=c->metadata.imageSnapshot(description.key.handle);
        if(existing->swapchain&&existing->key==description.key&&snapshot.known&&sameImageDescription(snapshot.description,description)) {
            rd::increment(totals.swapchainImageDuplicates);return;
        }
        result.issues=rp::bit(rp::Issue::Lifecycle);c->fatal(rp::Issue::Lifecycle);
    } else if(description.unsupportedPNext || description.flags || !(description.usage&attachmentUsage)) {
        // The provider must establish these facts before registering WSI identity.
        result.issues=description.unsupportedPNext ? rp::bit(rp::Issue::UnsupportedExtension) : rp::bit(rp::Issue::UnsupportedImage);
    } else {
        const auto before=c->metadata.stats().registrations;result=c->registerImage(description,true);
        if(c->metadata.stats().registrations!=before)rd::increment(totals.swapchainImagesRegistered);
    }
    if(result.issues){rd::increment(totals.swapchainImageRejections);totals.issues|=result.issues;}
    if(c->swapchainObservationCount<c->swapchainObservations.size())c->swapchainObservations[c->swapchainObservationCount++]={description,result.issues,false};
    else rd::increment(totals.swapchainSamplesOmitted);
});}
void imageDestroying(Context* c,VkImage image) noexcept {guarded(c,[&]{
    if(auto* i=c->image(bits(image))){
        for(unsigned slot=0;slot<c->projectionSelections.size();++slot)if(c->projectionSelections[slot].binding.image==i->key)c->projectSelection(slot,nullptr);
        c->metadata.destroyImage(i->key);for(auto& s:c->selections){if(s.current==i->key)s.current={};if(s.previous==i->key)s.previous={};}*i={};}
});}
void swapchainImageRetiring(Context* c,rp::Key key) noexcept {guarded(c,[&]{
    auto* image=c->image(key.handle);
    if(!image||!image->swapchain||image->key!=key){rd::increment(c->totals.projection.swapchainRetirementsIgnored);return;}
    for(unsigned slot=0;slot<c->projectionSelections.size();++slot)if(c->projectionSelections[slot].binding.image==key)c->projectSelection(slot,nullptr);
    c->metadata.destroyImage(key);
    for(auto& selection:c->selections){if(selection.current==key)selection.current={};if(selection.previous==key)selection.previous={};}
    *image={};rd::increment(c->totals.projection.swapchainImagesRetired);
});}
void select(Context* c,Role role,const image_inventory::Record& record) noexcept {guarded(c,[&]{
    const auto index=static_cast<unsigned>(role);if(index>=c->selections.size())return;
    const rp::Key key{record.image,record.generation};auto& s=c->selections[index];
    if(s.current!=key){s.previous=s.current;s.current=key;
        rd::Sample sample;sample.site=rd::Site::SelectedImage;sample.roleMask=1u<<index;sample.image=c->metadata.imageSnapshot(key.handle);
        sample.issues=sample.image.known?sample.image.issues:rp::bit(rp::Issue::UnknownObject);c->diagnostics.observe(sample);}
    c->projectSelection(index,&record);
});}
void viewCreated(Context* c,const VkImageViewCreateInfo* info,VkImageView view,VkResult driver) noexcept {guarded(c,[&]{
    if(driver==VK_SUCCESS&&info&&!c->image(bits(info->image))){++c->totals.ignoredUntrackedViews;c->rememberIgnoredView(*info,view);return;}
    const auto result=c->metadata.createView(bits(view),info,driver);c->result(result,true);c->diagnose(rd::Site::View,result,bits(view),driver);
});}
void viewDestroying(Context* c,VkImageView view) noexcept {guarded(c,[&]{c->metadata.destroyView(bits(view));for(auto& ignored:c->ignoredViews)if(ignored.key.handle==bits(view))ignored={};});}
void renderPassCreated(Context* c,const VkRenderPassCreateInfo* info,VkRenderPass pass,VkResult driver) noexcept {guarded(c,[&]{const auto result=c->metadata.createRenderPass(bits(pass),info,driver);c->result(result,true);c->diagnose(rd::Site::RenderPass,result,bits(pass),driver);});}
void renderPass2Created(Context* c,const VkRenderPassCreateInfo2* info,VkRenderPass pass,VkResult driver) noexcept {guarded(c,[&]{const auto result=c->metadata.createRenderPass2(bits(pass),info,driver);c->result(result,true);c->diagnose(rd::Site::RenderPass2,result,bits(pass),driver);});}
void renderPassDestroying(Context* c,VkRenderPass pass) noexcept {guarded(c,[&]{c->metadata.destroyRenderPass(bits(pass));});}
void framebufferCreated(Context* c,const VkFramebufferCreateInfo* info,VkFramebuffer fb,VkResult driver) noexcept {guarded(c,[&]{const auto result=c->metadata.createFramebuffer(bits(fb),info,driver);c->result(result,true);c->diagnose(rd::Site::Framebuffer,result,bits(fb),driver);});}
void framebufferDestroying(Context* c,VkFramebuffer fb) noexcept {guarded(c,[&]{c->metadata.destroyFramebuffer(bits(fb));});}

void queue(Context* c,VkQueue queue,std::uint32_t family) noexcept {guarded(c,[&]{
    if(!bits(queue)){c->fatal(rp::Issue::InvalidInput);return;}
    if(auto* q=c->queue(bits(queue))){if(q->family!=family){q->familyKnown=false;c->fatal(rp::Issue::Lifecycle);}return;}
    for(auto& q:c->queues)if(!q.handle){q.handle=bits(queue);q.family=family;return;}c->fatal(rp::Issue::Capacity);
});}
void poolCreated(Context* c,VkCommandPool pool,const VkCommandPoolCreateInfo* info,VkResult driver) noexcept {guarded(c,[&]{
    if(driver!=VK_SUCCESS){if(driver==VK_ERROR_DEVICE_LOST)c->recordingFatalReasons|=bit(Reason::DeviceLost);return;}
    if(!bits(pool)){c->recordingFatalReasons|=bit(Reason::InvalidInput);return;}
    if(auto* existing=c->pool(bits(pool))){existing->reasons|=bit(Reason::Lifecycle);
        for(auto& command:c->commands)if(command.handle&&command.facts.poolGeneration==existing->generation&&command.pool==existing->handle){
            command.allocationReasons|=bit(Reason::Lifecycle);command.facts.poolKnown=false;c->uncertain(command,Reason::Lifecycle);
        }
        return;}
    Context::Pool* slot=nullptr;for(auto& p:c->pools)if(!p.handle){slot=&p;break;}
    if(!slot||c->poolGeneration==UINT64_MAX){c->recordingFatalReasons|=bit(Reason::Capacity);return;}
    slot->handle=bits(pool);slot->generation=++c->poolGeneration;
    if(!info||info->sType!=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO){slot->reasons=bit(Reason::InvalidInput);return;}
    slot->flags=info->flags;slot->family=info->queueFamilyIndex;
    if(info->pNext)slot->reasons|=bit(Reason::PoolPNext);
    if(info->flags&~(VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT))slot->reasons|=bit(Reason::UnsupportedFlags);
});}
void allocate(Context* c,const VkCommandBufferAllocateInfo* info,const VkCommandBuffer* buffers,VkResult driver) noexcept {guarded(c,[&]{
    if(driver!=VK_SUCCESS){if(driver==VK_ERROR_DEVICE_LOST){c->fatal(rp::Issue::DeviceLost);c->recordingFatalReasons|=bit(Reason::DeviceLost);}return;}
    if(!info||info->sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO||!buffers||info->commandBufferCount>commandLimit){c->recordingFatalReasons|=bit(Reason::Capacity);c->fatal(rp::Issue::Capacity);return;}
    for(std::uint32_t i=0;i<info->commandBufferCount;++i){
        if(auto* existing=c->command(bits(buffers[i]))){c->uncertain(*existing,Reason::Lifecycle);c->fatal(rp::Issue::Lifecycle);continue;}
        if(!bits(buffers[i])){c->recordingFatalReasons|=bit(Reason::InvalidInput);c->fatal(rp::Issue::Lifecycle);continue;}
        Context::Command* slot=nullptr;for(auto& item:c->commands)if(!item.handle){slot=&item;break;}
        if(!slot){c->recordingFatalReasons|=bit(Reason::Capacity);c->fatal(rp::Issue::Capacity);return;}
        slot->handle=bits(buffers[i]);slot->pool=bits(info->commandPool);slot->level=info->level;
        auto& f=slot->facts;f.device=c->device;f.contextIncarnation=c->contextIncarnation;f.command=slot->handle;
        f.allocationKnown=true;f.pool=slot->pool;f.level=slot->level;
        if(auto* pool=c->pool(slot->pool)){
            f.poolGeneration=pool->generation;f.poolFlags=pool->flags;f.queueFamily=pool->family;
            f.poolKnown=(pool->reasons&bit(Reason::InvalidInput))==0;slot->allocationReasons|=pool->reasons;
        }else slot->allocationReasons|=bit(Reason::UnknownPool);
        if(info->pNext){slot->allocationIssues=rp::bit(rp::Issue::UnsupportedExtension);slot->allocationReasons|=bit(Reason::AllocationPNext);}
        if(info->level==VK_COMMAND_BUFFER_LEVEL_SECONDARY)slot->allocationReasons|=bit(Reason::Secondary);
        else if(info->level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY)slot->allocationReasons|=bit(Reason::InvalidInput);
        c->resetFacts(*slot);
    }
});}
void free(Context* c,std::uint32_t count,const VkCommandBuffer* buffers) noexcept {guarded(c,[&]{
    if(count>commandLimit||(!buffers&&count)){c->recordingFatalReasons|=bit(Reason::Capacity);c->fatal(rp::Issue::Capacity);return;}
    for(std::uint32_t i=0;i<count;++i)if(auto* command=c->command(bits(buffers[i]))){c->metadata.freeCommand(command->handle);*command={};}
});}
void resetPool(Context* c,VkCommandPool pool,bool destroy,VkResult driver) noexcept {guarded(c,[&]{
    if(!destroy&&driver!=VK_SUCCESS){
        for(auto& command:c->commands)if(command.handle&&command.pool==bits(pool))c->uncertain(command,Reason::DriverStateUnknown);
        if(driver==VK_ERROR_DEVICE_LOST){c->fatal(rp::Issue::DeviceLost);c->recordingFatalReasons|=bit(Reason::DeviceLost);}return;}
    for(auto& command:c->commands)if(command.handle&&command.pool==bits(pool)){
        if(destroy){c->metadata.freeCommand(command.handle);command={};}
        else{c->metadata.resetCommand(command.handle,VK_SUCCESS);c->resetFacts(command);}
    }
    if(destroy)if(auto* p=c->pool(bits(pool)))*p={};
});}
void begin(Context* c,VkCommandBuffer buffer,const VkCommandBufferBeginInfo* info,VkResult driver) noexcept {guarded(c,[&]{
    auto* command=c->command(bits(buffer));if(!command){c->result({{},rp::bit(rp::Issue::UnknownObject)},false);return;}
    if(driver==VK_SUCCESS){if(c->commandGeneration==UINT64_MAX){c->recordingFatalReasons|=bit(Reason::Capacity);c->fatal(rp::Issue::Capacity);return;}
        c->resetFacts(*command);command->key={command->handle,++c->commandGeneration};
        auto& f=command->facts;f.rpRecordingGeneration=command->key.generation;f.reasons=command->allocationReasons;
        f.stateKnown=true;f.recording=true;f.executable=false;
        f.beginFlagsKnown=info&&info->sType==VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if(f.beginFlagsKnown){
            f.beginUsageFlags=info->flags;if(info->pNext)f.reasons|=bit(Reason::BeginPNext);
            if(!(info->flags&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))f.reasons|=bit(Reason::NotOneTime);
            if(info->flags&VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)f.reasons|=bit(Reason::SimultaneousUse);
            if(info->flags&~(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT|VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT))f.reasons|=bit(Reason::UnsupportedFlags);
        }else f.reasons|=bit(Reason::InvalidInput);
        f.scopeKnown=f.beginFlagsKnown&&f.level==VK_COMMAND_BUFFER_LEVEL_PRIMARY&&!(f.beginUsageFlags&VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT);
        if(!f.scopeKnown)f.reasons|=bit(Reason::ScopeUnknown);
    }else{c->uncertain(*command,Reason::DriverStateUnknown);if(driver==VK_ERROR_DEVICE_LOST)c->recordingFatalReasons|=bit(Reason::DeviceLost);}
    const auto incoming=c->metadata.commandSnapshot(command->key);
    auto result=c->metadata.beginCommand(command->key,command->level,info,driver);c->result(result,false);
    if(command->allocationIssues){result=c->metadata.unsupported(command->key,rp::Issue::UnsupportedExtension);c->result(result,false);}
    c->diagnose(rd::Site::BeginCommand,result,command->handle,driver,incoming);
});}
void end(Context* c,VkCommandBuffer buffer,VkResult driver) noexcept {guarded(c,[&]{
    auto* command=c->command(bits(buffer));if(!command){c->result({{},rp::bit(rp::Issue::UnknownObject)},false);return;}
    const auto incoming=c->metadata.commandSnapshot(command->key);const auto result=c->metadata.endCommand(command->key,driver);c->result(result,false);
    auto& f=command->facts;
    if(driver==VK_SUCCESS){
        const bool observedRecording=f.stateKnown&&f.recording&&command->key.generation;
        f.recording=false;f.endSucceeded=true;f.executable=observedRecording;f.stateKnown=observedRecording;
        if(!observedRecording)f.reasons|=bit(Reason::Lifecycle);
        if(!f.scopeKnown||f.inRenderPass||f.inDynamicRendering){f.scopeKnown=false;f.reasons|=bit(Reason::ScopeUnknown);}
    }else{c->uncertain(*command,Reason::DriverStateUnknown);if(driver==VK_ERROR_DEVICE_LOST)c->recordingFatalReasons|=bit(Reason::DeviceLost);}
    c->diagnose(rd::Site::EndCommand,result,command->handle,driver,incoming,nullptr,VK_SUBPASS_CONTENTS_INLINE,command->order);
});}
void reset(Context* c,VkCommandBuffer buffer,VkResult driver) noexcept {guarded(c,[&]{
    c->metadata.resetCommand(bits(buffer),driver);if(auto* command=c->command(bits(buffer))){
        if(driver==VK_SUCCESS)c->resetFacts(*command);else c->uncertain(*command,Reason::DriverStateUnknown);
    }
    if(driver==VK_ERROR_DEVICE_LOST)c->recordingFatalReasons|=bit(Reason::DeviceLost);
});}
void explicitBarrier(Context* c,VkCommandBuffer buffer) noexcept {guarded(c,[&]{
    if(auto* command=c->recording(buffer))c->metadata.unknownExplicitBarrier(command->key,command->order);
});}
void explicitImageBarriers(Context* c,VkCommandBuffer buffer,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
    std::uint32_t count,const VkImageMemoryBarrier* images) noexcept {
    ErrorGuard error;if(!c)return;std::array<image_inventory::Snapshot,rp::maxExplicitImageEvents> identities{};
    const bool lookup=c->inventoryReader.lookup!=nullptr;
    if(lookup&&images&&count<=identities.size())for(std::uint32_t n=0;n<count;++n)identities[n]=c->inventoryReader.lookup(c->inventoryReader.user,bits(images[n].image));
    guarded(c,[&]{if(auto* command=c->recording(buffer))c->metadata.explicitImageBarriers(command->key,command->order,src,dst,flags,count,images,lookup?identities.data():nullptr);});
}
void explicitImageBarriers2(Context* c,VkCommandBuffer buffer,const VkDependencyInfo* info) noexcept {
    ErrorGuard error;if(!c)return;std::array<image_inventory::Snapshot,rp::maxExplicitImageEvents> identities{};
    const bool lookup=c->inventoryReader.lookup!=nullptr;
    if(lookup&&info&&info->pImageMemoryBarriers&&info->imageMemoryBarrierCount<=identities.size())for(std::uint32_t n=0;n<info->imageMemoryBarrierCount;++n)
        identities[n]=c->inventoryReader.lookup(c->inventoryReader.user,bits(info->pImageMemoryBarriers[n].image));
    guarded(c,[&]{if(auto* command=c->recording(buffer))c->metadata.explicitImageBarriers2(command->key,command->order,info,lookup?identities.data():nullptr);});
}
CommandRecordingFacts recordingFacts(Context* c,VkCommandBuffer buffer) noexcept {
    ErrorGuard error;CommandRecordingFacts out;out.command=bits(buffer);if(!c)return out;
    try{std::unique_lock lock(c->mutex,std::try_to_lock);
        if(!lock.owns_lock()){c->factsLockContended.fetch_add(1,std::memory_order_relaxed);lock.lock();}
        Context::bump(c->factsQueries);return c->facts(c->command(bits(buffer)),bits(buffer));}
    catch(...){++c->failures;out.reasons|=bit(Reason::DriverStateUnknown);return out;}
}
RecordingFactsCounters recordingFactsCounters(Context* c) noexcept {
    if(!c)return {};constexpr auto r=std::memory_order_relaxed;
    return {c->factsQueries.load(r),c->factsLockContended.load(r),c->commandLookupScans.load(r)};
}
CommandRecordingFacts dispatchPoint(Context* c,VkCommandBuffer buffer) noexcept {
    ErrorGuard error;CommandRecordingFacts out;out.command=bits(buffer);if(!c)return out;
    try{std::lock_guard lock(c->mutex);
        if(!c->retired)(void)c->recording(buffer);
        out=c->facts(c->command(bits(buffer)),bits(buffer));
        if(!out.stateKnown||!out.recording)out.reasons|=bit(Reason::NotRecording);
        return out;
    }catch(...){++c->failures;out.reasons|=bit(Reason::DriverStateUnknown);return out;}
}
RecordedSourceLayout resolveSourceAt(Context* c,const CommandRecordingFacts& facts,RecordedImageKey image,const VkImageSubresourceRange& range,VkImageLayout declared) noexcept {
    ErrorGuard error;RecordedSourceLayout out;out.queried=true;out.device=facts.device;out.contextIncarnation=facts.contextIncarnation;
    out.command=facts.command;out.rpRecordingGeneration=facts.rpRecordingGeneration;out.prefixOrder=facts.commonRecordOrder;
    out.image=image;out.copiedAspect=range.aspectMask;out.declaredLayout=declared;out.issues=0;
    if(!c){out.issues=bit(RecordedLayoutIssue::QueryMissing);return out;}
    try{std::unique_lock lock(c->mutex);
        const auto validate=[&]() -> Context::Command* {
            auto* command=c->command(facts.command);
            if(facts.device!=c->device||facts.contextIncarnation!=c->contextIncarnation||!command||!command->key.generation||command->key.generation!=facts.rpRecordingGeneration){out.issues=bit(RecordedLayoutIssue::IdentityMismatch);return nullptr;}
            if(!facts.commonRecordOrder||facts.commonRecordOrder!=command->order){out.issues=bit(RecordedLayoutIssue::PrefixMismatch);return nullptr;}
            const auto live=c->facts(command,facts.command);
            const auto outside=[](const CommandRecordingFacts& f){
                // Reusable primary recording flags do not weaken a layout-only
                // CPU prefix query. NotOneTime remains a literal recording fact.
                const auto unsupported=f.reasons&~bit(CommandRecordingReason::NotOneTime);
                return !unsupported&&f.beginFlagsKnown&&f.stateKnown&&f.recording&&f.level==VK_COMMAND_BUFFER_LEVEL_PRIMARY&&f.scopeKnown&&!f.inRenderPass&&!f.inDynamicRendering;
            };
            if(!outside(facts)||!outside(live)){out.issues=bit(RecordedLayoutIssue::RecordingUnsupported);return nullptr;}
            return command;
        };
        auto* command=validate();if(!command)return out;
        rp::RecordedPrefixLiveness live;std::array<RecordedImageKey,rp::maxExplicitImageEvents> keys{};
        if(c->inventoryReader.lookup){
            if(!c->metadata.prefixImageKeys(command->key,facts.commonRecordOrder,keys,live.count)){out.issues=bit(RecordedLayoutIssue::PrefixMismatch);return out;}
            lock.unlock();
            live.source=c->inventoryReader.lookup(c->inventoryReader.user,image.handle);
            for(std::size_t n=0;n<live.count;++n)live.images[n]=c->inventoryReader.lookup(c->inventoryReader.user,keys[n].handle);
            lock.lock();command=validate();if(!command)return out;
        }
        out=c->metadata.resolveSourceAt(command->key,facts.commonRecordOrder,image,range,declared,c->inventoryReader.lookup?&live:nullptr);
        out.device=c->device;out.contextIncarnation=c->contextIncarnation;return out;
    }catch(...){++c->failures;out.issues|=bit(RecordedLayoutIssue::RecordingUnsupported);return out;}
}
namespace {
RecordedSourceLayout resolveCallback(void* user,const CommandRecordingFacts& facts,RecordedImageKey image,const VkImageSubresourceRange& range,VkImageLayout declared) noexcept {
    return resolveSourceAt(static_cast<Context*>(user),facts,image,range,declared);
}
}
RecordedSourceLayoutQuery sourceLayoutQuery(Context* c) noexcept {ErrorGuard error;return {resolveCallback,c};}
void imageTouched(Context* c,VkCommandBuffer buffer,VkImage image,RecordedImageTouch kind) noexcept {
    guarded(c,[&]{if(auto* command=c->recording(buffer))c->metadata.imageTouched(command->key,command->order,bits(image),kind);});
}
RecordedImageInterval imageInterval(Context* c,const CommandRecordingFacts& read,const CommandRecordingFacts& use,RecordedImageKey image) noexcept {
    ErrorGuard error;RecordedImageInterval out;
    if(!c||c->retired||read.device!=c->device||read.contextIncarnation!=c->contextIncarnation||
       read.device!=use.device||read.contextIncarnation!=use.contextIncarnation||read.command!=use.command||
       read.rpRecordingGeneration!=use.rpRecordingGeneration)return out;
    try{std::lock_guard lock(c->mutex);const auto* command=c->command(use.command);
        if(!command||command->key.generation!=use.rpRecordingGeneration)return out;
        return c->metadata.imageInterval(command->key,read.commonRecordOrder,use.commonRecordOrder,{image.handle,image.generation});
    }catch(...){++c->failures;return out;}
}
RecordedImageIntervalQuery imageIntervalQuery(Context* c) noexcept {
    return {[](void* user,const CommandRecordingFacts& read,const CommandRecordingFacts& use,RecordedImageKey image) noexcept {
        return imageInterval(static_cast<Context*>(user),read,use,image);
    },c};
}
void unsupported(Context* c,VkCommandBuffer buffer,rp::Issue reason) noexcept {guarded(c,[&]{
    if(auto* command=c->recording(buffer)){
        command->facts.reasons|=bit(reason==rp::Issue::Secondary?Reason::SecondaryExecution:Reason::Events);
        c->recorded(rd::Site::Unsupported,command,[&]{return c->metadata.unsupported(command->key,reason);});
    }
});}
void beginRenderPass(Context* c,VkCommandBuffer b,const VkRenderPassBeginInfo* info,VkSubpassContents contents) noexcept {guarded(c,[&]{
    if(auto* command=c->recording(b)){
        c->scopeBegin(*command,false,info&&info->sType==VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        if(info&&info->pNext)command->facts.reasons|=bit(Reason::UnknownControl);
        if(contents!=VK_SUBPASS_CONTENTS_INLINE)command->facts.reasons|=bit(Reason::SecondaryExecution);
        c->recorded(rd::Site::BeginRenderPass,command,[&]{return c->metadata.beginRenderPass(command->key,command->order,info,contents);},info,contents);
    }
});}
void beginRenderPass2(Context* c,VkCommandBuffer b,const VkRenderPassBeginInfo* info,const VkSubpassBeginInfo* sub) noexcept {guarded(c,[&]{
    if(auto* command=c->recording(b)){
        c->scopeBegin(*command,false,info&&info->sType==VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO&&sub&&sub->sType==VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO);
        if(info&&info->pNext)command->facts.reasons|=bit(Reason::UnknownControl);
        if(sub&&(sub->pNext||sub->contents!=VK_SUBPASS_CONTENTS_INLINE))command->facts.reasons|=bit(Reason::UnknownControl);
        c->recorded(rd::Site::BeginRenderPass2,command,[&]{return c->metadata.beginRenderPass2(command->key,command->order,info,sub);},info,sub?sub->contents:VK_SUBPASS_CONTENTS_INLINE);
    }
});}
void endRenderPass(Context* c,VkCommandBuffer b) noexcept {guarded(c,[&]{
    if(auto* command=c->recording(b)){c->scopeEnd(*command,false);c->recorded(rd::Site::EndRenderPass,command,[&]{return c->metadata.endRenderPass(command->key,command->order);});}
});}
void endRenderPass2(Context* c,VkCommandBuffer b,const VkSubpassEndInfo* sub) noexcept {guarded(c,[&]{
    if(auto* command=c->recording(b)){
        c->scopeEnd(*command,false,sub&&sub->sType==VK_STRUCTURE_TYPE_SUBPASS_END_INFO);
        if(sub&&sub->pNext)command->facts.reasons|=bit(Reason::UnknownControl);
        c->recorded(rd::Site::EndRenderPass2,command,[&]{return c->metadata.endRenderPass2(command->key,command->order,sub);});
    }
});}
void nextSubpass(Context* c,VkCommandBuffer b) noexcept {guarded(c,[&]{
    if(auto* command=c->recording(b)){command->facts.reasons|=bit(Reason::NextSubpass);c->recorded(rd::Site::NextSubpass,command,[&]{return c->metadata.nextSubpass(command->key,command->order);});}
});}
void dynamicRendering(Context* c,VkCommandBuffer b) noexcept {guarded(c,[&]{
    if(auto* command=c->recording(b)){
        command->facts.scopeKnown=false;command->facts.reasons|=bit(Reason::DynamicRendering)|bit(Reason::ScopeUnknown);
        c->recorded(rd::Site::DynamicRendering,command,[&]{return c->metadata.dynamicRendering(command->key,command->order);});
    }
});}
void beginDynamicRendering(Context* c,VkCommandBuffer b,const VkRenderingInfo* info) noexcept {guarded(c,[&]{
    if(auto* command=c->recording(b)){
        c->scopeBegin(*command,true,info&&info->sType==VK_STRUCTURE_TYPE_RENDERING_INFO);
        command->facts.reasons|=bit(Reason::DynamicRendering);
        if(info&&(info->pNext||info->flags))command->facts.reasons|=bit(Reason::UnknownControl);
        c->recorded(rd::Site::DynamicRendering,command,[&]{return c->metadata.dynamicRendering(command->key,command->order);});
    }
});}
void endDynamicRendering(Context* c,VkCommandBuffer b) noexcept {guarded(c,[&]{
    if(auto* command=c->recording(b)){
        c->scopeEnd(*command,true);command->facts.reasons|=bit(Reason::DynamicRendering);
        c->recorded(rd::Site::DynamicRendering,command,[&]{return c->metadata.dynamicRendering(command->key,command->order);});
    }
});}
SubmitTicket beginSubmit(Context* c,VkQueue queue,std::uint32_t count,const VkSubmitInfo* infos) noexcept {
    SubmitTicket t;guarded(c,[&]{t=start(c,queue);
        if(count>batchLimit||(!infos&&count)){t.issues|=rp::bit(rp::Issue::Capacity);return;}
        for(std::uint32_t i=0;i<count;++i){const auto& info=infos[i];
            if(info.sType!=VK_STRUCTURE_TYPE_SUBMIT_INFO||info.pNext)t.issues|=rp::bit(rp::Issue::UnsupportedExtension);
            if(info.commandBufferCount>maxSubmittedCommands-t.commandCount||(!info.pCommandBuffers&&info.commandBufferCount)){
                t.issues|=rp::bit(rp::Issue::Capacity);return;}
            for(std::uint32_t j=0;j<info.commandBufferCount;++j)capture(c,t,info.pCommandBuffers[j]);
        }
    });return t;
}
SubmitTicket beginSubmit2(Context* c,VkQueue queue,std::uint32_t count,const VkSubmitInfo2* infos) noexcept {
    SubmitTicket t;guarded(c,[&]{t=start(c,queue);
        if(count>batchLimit||(!infos&&count)){t.issues|=rp::bit(rp::Issue::Capacity);return;}
        std::size_t inspectedSemaphores=0;
        for(std::uint32_t i=0;i<count;++i){const auto& info=infos[i];
            if(info.sType!=VK_STRUCTURE_TYPE_SUBMIT_INFO_2||info.pNext||info.flags)t.issues|=rp::bit(rp::Issue::UnsupportedExtension);
            inspectSemaphores(t,inspectedSemaphores,info.waitSemaphoreInfoCount,info.pWaitSemaphoreInfos);
            inspectSemaphores(t,inspectedSemaphores,info.signalSemaphoreInfoCount,info.pSignalSemaphoreInfos);
            if(info.commandBufferInfoCount>maxSubmittedCommands-t.commandCount||(!info.pCommandBufferInfos&&info.commandBufferInfoCount)){
                t.issues|=rp::bit(rp::Issue::Capacity);return;}
            for(std::uint32_t j=0;j<info.commandBufferInfoCount;++j){const auto& entry=info.pCommandBufferInfos[j];
                if(entry.sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO||entry.pNext||(entry.deviceMask!=0&&entry.deviceMask!=1))
                    t.issues|=rp::bit(rp::Issue::UnsupportedExtension);
                capture(c,t,entry.commandBuffer);
            }
        }
    });return t;
}
void finishSubmit(Context* c,const SubmitTicket& ticket,VkFence fence,VkResult driver) noexcept {
    ErrorGuard error;if(!c)return;
    std::uint64_t transactionIssues=ticket.issues;bool canProcess=false;
    guarded(c,[&]{
        if(driver==VK_SUCCESS)++c->totals.successfulDriverSubmissions;else ++c->totals.failedDriverSubmissions;
        auto* q=c->queue(ticket.queue);
        const bool sameContext=ticket.device==c->device&&ticket.contextIncarnation==c->contextIncarnation;
        const bool ticketActive=sameContext&&q&&ticket.token&&q->inFlight&&q->activeToken==ticket.token;
        if(!ticketActive){transactionIssues|=rp::bit(rp::Issue::Lifecycle);c->fatal(rp::Issue::Lifecycle);}
        else{--q->inFlight;q->activeToken=0;}
        if(ticket.commandCount>maxSubmittedCommands)transactionIssues|=rp::bit(rp::Issue::Capacity);
        const auto factsCount=std::min<std::size_t>(ticket.commandCount,maxSubmittedCommands);
        for(std::size_t i=0;i<factsCount;++i){
            auto* command=c->command(ticket.commands[i].handle);
            if(!sameContext||!command||command->key!=ticket.commands[i]||!command->key.generation){
                rd::increment(c->totals.recordingFactsStaleSubmissions);continue;
            }
            auto& f=command->facts;const auto& frozen=ticket.recordingFacts[i];
            if(frozen.device!=c->device||frozen.contextIncarnation!=c->contextIncarnation
                ||frozen.command!=command->handle||frozen.rpRecordingGeneration!=command->key.generation){
                c->uncertain(*command,Reason::IdentityMismatch);rd::increment(c->totals.recordingFactsUnknownSubmissions);continue;
            }
            bool duplicate=false;for(std::size_t j=0;j<factsCount;++j)if(j!=i&&ticket.commands[j]==ticket.commands[i])duplicate=true;
            if(duplicate)f.reasons|=bit(Reason::DuplicateSubmission);
            if(command->lastFinishedSubmitToken==ticket.token)continue; // duplicate occurrence or replayed finish
            if(!ticketActive){
                c->uncertain(*command,Reason::UnknownQueue);rd::increment(c->totals.recordingFactsUnknownSubmissions);continue;
            }
            command->lastFinishedSubmitToken=ticket.token;
            if(!ticket.queueKnown)f.reasons|=bit(Reason::UnknownQueue);
            else if(!f.poolKnown)f.reasons|=bit(Reason::UnknownPool);
            else if(ticket.queueFamily!=f.queueFamily)f.reasons|=bit(Reason::QueueFamilyMismatch);
            if(ticket.issues)f.reasons|=bit(Reason::SubmissionStructureUnknown);
            if(!frozen.stateKnown||!frozen.executable)f.reasons|=bit(Reason::NotExecutable);
            if(driver==VK_SUCCESS){
                if(f.successfulSubmissionCount==UINT64_MAX)f.reasons|=bit(Reason::Capacity);
                else ++f.successfulSubmissionCount;
                f.lastSubmitQueue=ticket.queue;f.lastSubmitSerial=ticket.token;f.lastSubmitQueueKnown=ticket.queueKnown;
                if(f.successfulSubmissionCount>1)f.reasons|=bit(Reason::DuplicateSubmission);
                if(f.beginFlagsKnown&&(f.beginUsageFlags&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))f.reasons|=bit(Reason::OneTimeAlreadySubmitted);
                f.reasons|=bit(Reason::CompletionUnknown);f.stateKnown=f.recording=f.executable=false;
                // CPU control scope is historical. No pending/completion state is inferred.
            }else{
                c->uncertain(*command,Reason::DriverStateUnknown);
                if(driver==VK_ERROR_DEVICE_LOST)f.reasons|=bit(Reason::DeviceLost);
            }
        }
        if(driver==VK_ERROR_DEVICE_LOST){c->fatal(rp::Issue::DeviceLost);c->recordingFatalReasons|=bit(Reason::DeviceLost);}
        transactionIssues|=c->totals.fatalIssues|c->metadata.stats().globalIssues;
        if(c->failures)transactionIssues|=rp::bit(rp::Issue::Lifecycle);
        canProcess=true;
    });
    if(!canProcess)return;
    flushDiagnosticSamples(c);
    flushSwapchainObservations(c);
    const auto count=std::min<std::size_t>(ticket.commandCount,maxSubmittedCommands);
    for(std::size_t index=0;index<std::max<std::size_t>(count,1);++index){
        rp::Batch batch;std::array<std::uint8_t,rp::maxOperations> emitRoles{};bool emitSummary=false;
        rp::ProjectionSubmission projection;
        std::array<bool,rp::projectionSlots> projectionSummaries{};
        std::array<std::size_t,rp::projectionSlots> projectionEmissions{};
        const auto command=count ? ticket.commands[index] : rp::Key{};
        guarded(c,[&]{
            if(count)batch=c->metadata.submitted(command,driver);
            else{batch.driverSubmissionSucceeded=driver==VK_SUCCESS;if(driver!=VK_SUCCESS)batch.issues=rp::bit(rp::Issue::DriverFailed);}
            batch.issues|=transactionIssues|c->totals.fatalIssues|c->metadata.stats().globalIssues;
            if(c->failures)batch.issues|=rp::bit(rp::Issue::Lifecycle);
            if(batch.issues){batch.metadataAccepted=false;batch.count=0;}
            if(count){
                if(batch.metadataAccepted){++c->totals.acceptedRecordings;c->totals.acceptedLayoutOperations+=batch.count;}
                else ++c->totals.refusedRecordings;
            }
            c->totals.observedIssues|=batch.issues;
            if(c->totals.emittedSummaries<submissionSummaryLimit){++c->totals.emittedSummaries;emitSummary=true;}else ++c->totals.omittedSummaries;
            for(std::size_t i=0;i<batch.count;++i)for(unsigned role=0;role<c->selections.size();++role){
                auto& selection=c->selections[role];const auto key=batch.operations[i].image;
                if(key!=selection.current&&key!=selection.previous)continue;
                if(selection.emitted<operationLimitPerRole){++selection.emitted;++c->totals.emittedOperations;emitRoles[i]|=std::uint8_t(1u<<role);}
                else ++c->totals.omittedOperations;
            }
            if(count) {
                // Always consume captured tickets with the ORIGINAL driver result,
                // including a rejected enclosing Submit transaction. Do not merge
                // legacy batch.issues into this independent projection path.
                projection=c->metadata.projectSubmitted(ticket.projections[index],driver);
                auto& totals=c->totals.projection;rd::increment(totals.completedCommands);
                addCounter(totals.journalObservedPasses,projection.observedPasses);
                addCounter(totals.journalRetainedPasses,projection.retainedPasses);
                addCounter(totals.journalDroppedPasses,projection.droppedPasses);
                totals.journalMaxObservedPasses=std::max(totals.journalMaxObservedPasses,projection.observedPasses);
                projection.issues |= transactionIssues|c->totals.fatalIssues|c->metadata.stats().globalIssues|c->projectionFatalIssues;
                if(ticket.projections[index].command!=command)projection.issues|=rp::bit(rp::Issue::StaleGeneration);
                if(c->failures)projection.issues|=rp::bit(rp::Issue::Lifecycle);
                totals.issues |= projection.issues;
                for(unsigned role=0;role<rp::projectionSlots;++role) {
                    auto& slot=projection.slots[role];
                    if(slot.classification==rp::ProjectionClassification::Disabled){rd::increment(totals.disabledSlots);continue;}
                    slot.issues |= projection.issues;
                    if(slot.issues){slot.metadataAccepted=false;slot.count=0;slot.classification=rp::ProjectionClassification::Refused;}
                    totals.issues |= slot.issues;
                    if(slot.metadataAccepted) {
                        rd::increment(totals.acceptedSlots);addCounter(totals.layoutOperations,slot.count);
                        if(slot.classification==rp::ProjectionClassification::NoDirectAttachmentReference)rd::increment(totals.noDirectSlots);
                    } else rd::increment(totals.refusedSlots);
                    if(totals.emittedSummaries<projectionSummaryLimit){rd::increment(totals.emittedSummaries);projectionSummaries[role]=true;}
                    else rd::increment(totals.omittedSummaries);
                    auto& emitted=c->projectionSelections[role].emitted;
                    const auto available=projectionOperationLimitPerRole-emitted;
                    projectionEmissions[role]=std::min<std::size_t>(slot.count,static_cast<std::size_t>(available));
                    addCounter(emitted,projectionEmissions[role]);addCounter(totals.emittedOperations,projectionEmissions[role]);
                    addCounter(totals.omittedOperations,slot.count-projectionEmissions[role]);
                }
            }
        });
        // Copied snapshots only; no metadata or outer stateLock held during I/O.
        emitSubmission(c,ticket,fence,driver,command,batch,emitSummary);
        for(std::size_t i=0;i<batch.count;++i)for(unsigned role=0;role<3;++role)if(emitRoles[i]&(1u<<role))emitOperation(c,ticket,batch.operations[i],role);
        if(count)for(unsigned role=0;role<rp::projectionSlots;++role) {
            const auto& slot=projection.slots[role];
            if(projectionSummaries[role])emitProjectionSubmission(c,ticket,ticket.projections[index],driver,slot,role);
            for(std::size_t i=0;i<projectionEmissions[role];++i)emitProjectionOperation(c,ticket,slot,slot.operations[i],role);
        }
    }
}
} // namespace xess_vk::render_pass_observer
