#include "descriptor_image_observer.hpp"
#include "cpu_wall_time.hpp"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <limits>
#include <mutex>
#include <type_traits>

namespace xess_vk::descriptor_image_observer {
namespace {
namespace dm=descriptor_image_metadata;
struct ErrorGuard { DWORD saved=GetLastError();~ErrorGuard(){SetLastError(saved);} };
template<class T> std::uint64_t bits(T value) noexcept {
    if constexpr(std::is_pointer_v<T>)return reinterpret_cast<std::uint64_t>(value);
    else return static_cast<std::uint64_t>(value);
}
void increment(std::uint64_t& value) noexcept { if(value!=UINT64_MAX)++value; }
void increment(std::atomic<std::uint64_t>& value) noexcept {
    auto old=value.load(std::memory_order_relaxed);
    while(old!=UINT64_MAX&&!value.compare_exchange_weak(old,old+1,std::memory_order_relaxed)){}
}
unsigned long long n(std::uint64_t value) noexcept {return static_cast<unsigned long long>(value);}
const char* boolean(bool value) noexcept {return value?"true":"false";}
const char* sourceName(std::size_t source) noexcept {
    constexpr const char* names[]={"fi_motion","fi_depth","fi_real_color","raw_motion","raw_depth","raw_color",
        "dilated_motion","dilated_depth","reconstructed_depth","farthest_depth","luma"};
    return source<sourceCount?names[source]:"unknown";
}
const char* propertyName(std::size_t source) noexcept {
    constexpr const char* names[]={"r_dilated_motion_vectors","r_dilated_depth","r_present_backbuffer",
        "r_input_motion_vectors","r_input_depth","r_input_color_jittered","rw_dilated_motion_vectors",
        "rw_dilated_depth","rw_reconstructed_previous_nearest_depth","rw_farthest_depth","rw_current_luma"};
    return source<sourceCount?names[source]:"unknown";
}
struct Json {
    char text[8192]{};std::size_t used{};bool valid{true};
    template<class... T> void append(const char* format,T... args) noexcept {
        if(!valid)return;
        int count;
        if constexpr(sizeof...(args)==0)count=std::snprintf(text+used,sizeof(text)-used,"%s",format);
        else count=std::snprintf(text+used,sizeof(text)-used,format,args...);
        if(count<0||std::size_t(count)>=sizeof(text)-used){valid=false;return;}used+=std::size_t(count);
    }
};
void falseClaims(Json& out) noexcept {
    out.append(",\"complete_resource_set\":false,\"snapshot_atomic\":false,\"same_frame_verified\":false,"
        "\"source_semantics_verified\":false,\"undilated_verified\":false,\"shader_semantics_verified\":false,"
        "\"layout_verified\":false,\"submitted\":false,\"gpu_complete\":false,\"resource_lifetime_pinned\":false,"
        "\"copy_permitted\":false,\"pixels_captured\":false,\"xefg_integrated\":false");
}
}
struct Context {
    std::uint64_t device{};Sink sink{};std::mutex mutex;dm::Tracker tracker;Stats totals;
    std::atomic<std::uint64_t> failures{},groupsEmitted{},bindingsEmitted{};
    std::atomic<std::uint64_t> imageRevision{2};
    std::atomic<bool> referenceMasksHealthy{true};
    Context(VkDevice value,Sink output,dm::Config config):device(bits(value)),sink(output),tracker(config){totals.referenceQueryTiming=config.measureReferenceConsumption;}
};
namespace {
template<class F> void guarded(Context* c,F&& function) noexcept {
    ErrorGuard preserve;if(!c)return;
    try {std::lock_guard lock(c->mutex);if(!c->totals.retired){
        const auto revision=c->imageRevision.load(std::memory_order_relaxed);
        c->imageRevision.store(revision&&revision<UINT64_MAX-1?revision+1:0,std::memory_order_release);
        struct Publish {Context* c;std::uint64_t old;~Publish(){c->imageRevision.store(old&&old<UINT64_MAX-1?old+2:0,std::memory_order_release);}} publish{c,revision};
        function();}}
    catch(...){c->imageRevision.store(0,std::memory_order_release);c->referenceMasksHealthy=false;increment(c->failures);}
}
Stats snapshot(Context* c) noexcept {
    auto value=c->totals;value.metadata=c->tracker.stats();
    value.failures=c->failures.load(std::memory_order_relaxed);
    value.emittedGroups=c->groupsEmitted.load(std::memory_order_relaxed);
    value.emittedBindings=c->bindingsEmitted.load(std::memory_order_relaxed);
    return value;
}
bool emit(Context* c,const Json& out) noexcept {
    if(!out.valid){increment(c->failures);return false;}
    if(!c->sink)return false;
    try{c->sink(out.text);return true;}catch(...){increment(c->failures);return false;}
}
void prefix(Json& out,Context* c,const char* event) noexcept {
    out.append("{\"event\":\"%s\",\"pid\":%lu,\"uptime_ms\":%llu,\"thread_id\":%lu,\"device\":\"0x%llx\"",
        event,GetCurrentProcessId(),n(GetTickCount64()),GetCurrentThreadId(),n(c->device));
}
std::uint32_t sourceMask(const dm::ImageBinding& binding,const Stats& state) noexcept {
    std::uint32_t mask=0;
    for(std::size_t i=0;i<sourceCount;++i)if(binding.image.handle&&binding.image.generation&&binding.image==state.sources[i].image)mask|=1u<<i;
    return mask;
}
void dispatchIdentity(Json& out,const DispatchEvidence& value,std::uint64_t call) noexcept {
    out.append(",\"dispatch_call\":%llu,\"command\":\"0x%llx\",\"command_generation\":%llu,\"record_ordinal\":%llu,"
        "\"pipeline\":\"0x%llx\",\"pipeline_generation\":%llu,\"pipeline_layout\":\"0x%llx\",\"pipeline_layout_generation\":%llu",
        n(call),n(value.command.handle),n(value.command.generation),n(value.recordOrdinal),n(value.pipeline.handle),
        n(value.pipeline.generation),n(value.pipelineLayout.handle),n(value.pipelineLayout.generation));
}
void recordingIdentity(Json& out,const CommandRecordingFacts& f) noexcept {
    out.append(",\"command_generation_scope\":\"descriptor_tracker\",\"record_ordinal_scope\":\"descriptor_dispatch_only\","
        "\"recording_facts\":{\"device\":\"0x%llx\",\"context_incarnation\":%llu,\"command\":\"0x%llx\","
        "\"rp_command_generation\":%llu,\"rp_command_generation_scope\":\"render_pass_observer_recording\","
        "\"common_record_order\":%llu,\"common_record_order_scope\":\"hooked_control_barrier_dispatch_events\","
        "\"pool\":\"0x%llx\",\"pool_generation\":%llu,\"pool_flags\":%u,\"queue_family\":%u,\"level\":%d,\"begin_usage_flags\":%u,"
        "\"allocation_known\":%s,\"pool_known\":%s,\"begin_flags_known\":%s,\"state_known\":%s,\"recording\":%s,\"executable\":%s,"
        "\"end_succeeded\":%s,\"scope_known\":%s,\"in_render_pass\":%s,\"in_dynamic_rendering\":%s,\"supported\":%s,\"reasons\":%llu,"
        "\"successful_submission_count\":%llu,\"last_submit_queue\":\"0x%llx\",\"last_submit_serial\":%llu,\"last_submit_queue_known\":%s,"
        "\"complete_hook_coverage\":false,\"gpu_complete\":false,\"layout_verified\":false,\"copy_permitted\":false}",
        n(f.device),n(f.contextIncarnation),n(f.command),n(f.rpRecordingGeneration),n(f.commonRecordOrder),
        n(f.pool),n(f.poolGeneration),unsigned(f.poolFlags),f.queueFamily,int(f.level),unsigned(f.beginUsageFlags),
        boolean(f.allocationKnown),boolean(f.poolKnown),boolean(f.beginFlagsKnown),boolean(f.stateKnown),boolean(f.recording),boolean(f.executable),
        boolean(f.endSucceeded),boolean(f.scopeKnown),boolean(f.inRenderPass),boolean(f.inDynamicRendering),boolean(f.supported()),n(f.reasons),
        n(f.successfulSubmissionCount),n(f.lastSubmitQueue),n(f.lastSubmitSerial),boolean(f.lastSubmitQueueKnown));
}
struct SourceLayouts {
    std::array<RecordedSourceLayout,dm::maxImageBindings> values{};
    std::array<bool,dm::maxImageBindings> requested{};
    std::array<resource_memory_observer::Observation,dm::maxImageBindings> memory{};
    std::array<std::uint32_t,dm::maxImageBindings> sourceMasks{};
    std::uint32_t requestedCount{},queriedCount{},resolvedCount{},supportedCount{};std::uint64_t issues{};
    std::uint32_t memoryQueries{},memoryBounds{},memoryAliasFree{};std::uint64_t memoryLocalIssues{},memoryGlobalIssues{};
};
void sourceMemory(Json& out,const resource_memory_observer::Observation& o,bool requested) noexcept {
    const auto& f=o.binding;
    out.append(",\"resource_memory\":{\"status\":\"%s\",\"queried\":%s,\"memory\":\"0x%llx\",\"memory_generation\":%llu,"
        "\"offset\":%llu,\"size\":%llu,\"end\":%llu,\"bounds_known\":%s,\"alias_free_observed\":%s,\"granularity\":%llu,"
        "\"padded_alias_range_known\":%s,\"padded_begin\":%llu,\"padded_end\":%llu,\"overlap_count\":%zu,"
        "\"issues\":%llu,\"local_issues\":%llu,\"global_issues\":%llu,\"resource_lifetime_pinned\":false,\"gpu_complete\":false,\"copy_permitted\":false}",
        !requested?"not_requested":!o.queried?"query_unavailable":f.issues?"refused":"observed",boolean(o.queried),n(f.memory.handle),n(f.memory.generation),n(f.offset),n(f.size),n(f.end),
        boolean(f.bindingBoundsKnown),boolean(f.observedAliasFree),n(f.bufferImageGranularity),boolean(f.paddedAliasRangeKnown),n(f.paddedAliasBegin),n(f.paddedAliasEnd),f.overlapCount,n(f.issues),n(o.localIssues),n(o.globalIssues));
}
void layoutEvent(Json& out,const char* name,const RecordedLayoutEvent& e) noexcept {
    out.append(",\"%s\":{\"order\":%llu,\"sequence\":%u,\"kind\":%u,\"old_layout\":%u,\"new_layout\":%u,"
        "\"explicit_scopes_known\":%s,\"src_stage\":%llu,\"dst_stage\":%llu,\"src_access\":%llu,\"dst_access\":%llu}",
        name,n(e.order),e.sequence,unsigned(e.kind),unsigned(e.oldLayout),unsigned(e.newLayout),boolean(e.explicitScopesKnown),n(e.srcStage),n(e.dstStage),n(e.srcAccess),n(e.dstAccess));
}
void sourceLayout(Json& out,const RecordedSourceLayout& r,bool requested) noexcept {
    const auto& range=r.resolvedLayoutRange;
    out.append(",\"recorded_source_layout\":{\"status\":\"%s\",\"queried\":%s,\"device\":\"0x%llx\",\"context_incarnation\":%llu,"
        "\"command\":\"0x%llx\",\"rp_command_generation\":%llu,\"prefix_order\":%llu,\"image\":\"0x%llx\",\"image_generation\":%llu,"
        "\"copied_aspect\":%u,\"resolved_layout_range\":[%u,%u,%u,%u,%u],\"recorded_layout\":%u,\"declared_layout\":%u,"
        "\"recorded_layout_resolved\":%s,\"recorded_prefix_supported\":%s,\"issues\":%llu,"
        "\"observed_explicit_images\":%llu,\"retained_explicit_images\":%llu,\"dropped_explicit_images\":%llu,"
        "\"explicit_image_high_water\":%llu,\"explicit_image_capacity\":%llu,\"explicit_call_refusals\":%llu,"
        "\"examined_passes\":%llu,\"direct_passes\":%llu,\"no_direct_passes\":%llu,\"matched_events\":%llu,\"ignored_equal_sync2_events\":%llu",
        !requested?"not_requested":!r.queried?"query_unavailable":r.recordedLayoutResolved?"resolved":"refused",boolean(r.queried),
        n(r.device),n(r.contextIncarnation),n(r.command),n(r.rpRecordingGeneration),n(r.prefixOrder),n(r.image.handle),n(r.image.generation),r.copiedAspect,
        range.aspectMask,range.baseMipLevel,range.levelCount,range.baseArrayLayer,range.layerCount,unsigned(r.recordedLayout),unsigned(r.declaredLayout),
        boolean(r.recordedLayoutResolved),boolean(r.recordedPrefixSupported),n(r.issues),n(r.observedExplicitImages),n(r.retainedExplicitImages),n(r.droppedExplicitImages),
        n(r.explicitImageHighWater),n(r.explicitImageCapacity),n(r.explicitCallRefusals),
        n(r.examinedPasses),n(r.directPasses),n(r.noDirectPasses),n(r.matchedEvents),n(r.ignoredEqualSync2Events));
    layoutEvent(out,"anchor",r.anchor);layoutEvent(out,"last_event",r.lastEvent);
    out.append(",\"layout_verified\":false,\"gpu_complete\":false,\"owner_verified\":false,\"resource_lifetime_pinned\":false,\"copy_permitted\":false}");
}
void emitDetails(Context* c,const DispatchEvidence& value,const Stats& state,const char* reason,const CommandRecordingFacts& facts,const SourceLayouts& layouts) noexcept {
    Json out;prefix(out,c,"descriptor_pre_dispatch");dispatchIdentity(out,value,state.dispatchCalls);
    recordingIdentity(out,facts);
    out.append(",\"resource_memory\":{\"queried_count\":%u,\"bounds_known_count\":%u,\"alias_free_observed_count\":%u,\"local_issues\":%llu,\"global_issues\":%llu,\"copy_permitted\":false}",
        layouts.memoryQueries,layouts.memoryBounds,layouts.memoryAliasFree,n(layouts.memoryLocalIssues),n(layouts.memoryGlobalIssues));
    out.append(",\"recorded_source_layout\":{\"status\":\"%s\",\"requested_count\":%u,\"queried_count\":%u,\"resolved_count\":%u,\"supported_count\":%u,\"issues\":%llu,"
        "\"layout_verified\":false,\"gpu_complete\":false,\"owner_verified\":false,\"resource_lifetime_pinned\":false,\"copy_permitted\":false}",
        layouts.requestedCount?"queried_bindings":"not_requested",layouts.requestedCount,layouts.queriedCount,layouts.resolvedCount,layouts.supportedCount,n(layouts.issues));
    out.append(",\"sample_reason\":\"%s\",\"metadata_accepted\":%s,\"issues\":%llu,\"binding_count\":%zu,"
        "\"ignored_non_image_bindings\":%zu,\"groups\":[%u,%u,%u],\"matched_dispatch_call\":%llu",
        reason,boolean(value.metadataAccepted),n(value.issues),value.count,
        value.ignoredNonImageBindingCount,value.groups[0],value.groups[1],value.groups[2],n(state.matchedDispatches));
    falseClaims(out);out.append("}");if(emit(c,out))increment(c->groupsEmitted);
    // Malformed/partial evidence can never produce accepted binding detail.
    if(!value.metadataAccepted||value.issues||value.count>value.bindings.size())return;
    for(std::size_t j=0;j<value.count;++j){
        const auto& binding=value.bindings[j];const auto mask=sourceMask(binding,state);
        Json line;prefix(line,c,"descriptor_image_binding");dispatchIdentity(line,value,state.dispatchCalls);
        recordingIdentity(line,facts);
        sourceLayout(line,layouts.values[j],layouts.requested[j]);
        sourceMemory(line,layouts.memory[j],layouts.requested[j]);
        line.append(",\"binding_index\":%zu,\"set_index\":%u,\"binding\":%u,\"array_element\":%u,\"descriptor_type\":%u,"
            "\"set\":\"0x%llx\",\"set_generation\":%llu,\"set_layout\":\"0x%llx\",\"set_layout_generation\":%llu,"
            "\"pool\":\"0x%llx\",\"pool_generation\":%llu,\"descriptor_version\":%llu,"
            "\"view\":\"0x%llx\",\"view_generation\":%llu,\"image\":\"0x%llx\",\"image_generation\":%llu,"
            "\"view_type\":%u,\"view_format\":%u,\"swizzle\":[%u,%u,%u,%u],\"declared_layout\":%u,"
            "\"aspect_mask\":%u,\"base_mip\":%u,\"level_count\":%u,\"base_layer\":%u,\"layer_count\":%u,"
            "\"format\":%u,\"width\":%u,\"height\":%u,\"depth\":%u,\"mip_levels\":%u,\"array_layers\":%u,"
            "\"samples\":%u,\"usage\":%u,\"flags\":%u,\"tiling\":%u,\"sharing_mode\":%u,\"initial_layout\":%u,"
            "\"source_match_mask\":%u,\"same_image_as_source_sample\":%s,\"source_samples\":[",
            j,binding.setIndex,binding.binding,binding.arrayElement,unsigned(binding.descriptorType),n(binding.set.handle),n(binding.set.generation),
            n(binding.setLayout.handle),n(binding.setLayout.generation),n(binding.pool.handle),n(binding.pool.generation),n(binding.descriptorVersion),
            n(binding.view.handle),n(binding.view.generation),n(binding.image.handle),n(binding.image.generation),unsigned(binding.viewType),unsigned(binding.viewFormat),
            unsigned(binding.components.r),unsigned(binding.components.g),unsigned(binding.components.b),unsigned(binding.components.a),unsigned(binding.declaredLayout),
            binding.range.aspectMask,binding.range.baseMipLevel,binding.range.levelCount,binding.range.baseArrayLayer,binding.range.layerCount,
            binding.imageDescription.format,binding.imageDescription.width,binding.imageDescription.height,binding.imageDescription.depth,
            binding.imageDescription.mipLevels,binding.imageDescription.arrayLayers,binding.imageDescription.samples,binding.imageDescription.usage,
            binding.imageDescription.flags,binding.imageDescription.tiling,binding.imageDescription.sharingMode,binding.imageDescription.initialLayout,
            mask,boolean(mask!=0));
        bool comma=false;
        for(std::size_t i=0;i<sourceCount;++i)if(mask&(1u<<i)){
            line.append("%s{\"source_key\":\"%s\",\"source_property\":\"%s\",\"source_counter\":%llu,\"same_image_as_source_sample\":true}",
                comma?",":"",sourceName(i),propertyName(i),n(state.sources[i].counter));comma=true;
        }
        line.append("]");falseClaims(line);line.append("}");if(emit(c,line))increment(c->bindingsEmitted);
    }
}
void emitSummary(Context* c,const Stats& state,const char* reason) noexcept {
    Json out;prefix(out,c,"descriptor_image_summary");const auto& m=state.metadata;
    out.append(",\"storage_bytes\":%zu,\"storage_mode\":\"paged_payload\"",m.storageBytes);
    out.append(",\"reason\":\"%s\",\"retired\":%s,\"dispatch_calls\":%llu,\"dispatches\":%llu,\"accepted_dispatches\":%llu,\"refused_dispatches\":%llu,"
        "\"matched_dispatches\":%llu,\"ordinary_dispatches\":%llu,\"matched_details\":%llu,\"ordinary_details\":%llu,"
        "\"emitted_groups\":%llu,\"emitted_bindings\":%llu,\"omitted_groups\":%llu,\"periodic_summaries\":%llu,\"omitted_periodic_summaries\":%llu,"
        "\"periodic_matched_details\":%llu,\"omitted_periodic_matched_details\":%llu,\"health_summaries\":%llu,\"omitted_health_summaries\":%llu,\"final_summaries\":%llu,"
        "\"explicit_summaries\":%llu,\"omitted_explicit_summaries\":%llu,\"source_observations\":%llu,\"source_changes\":%llu,"
        "\"source_clears\":%llu,\"invalid_sources\":%llu,\"validation_calls\":%llu,\"validation_refusals\":%llu,\"failures\":%llu,"
        "\"registrations\":%llu,\"retirements\":%llu,\"mutations\":%llu,\"capacity_refusals\":%llu,\"failed_driver_calls\":%llu,"
        "\"observed_issues\":%llu,\"fatal_issues\":%llu,\"referenced_sets_high_water\":%zu,\"referenced_sets_capacity\":%zu,"
        "\"reference_overflow_refusals\":%llu,\"restore_range_disturbances\":%llu,\"ordinary_update_calls\":%llu,\"core_template_update_calls\":%llu,\"khr_template_update_calls\":%llu,"
        "\"template_update_refusals\":%llu,\"template_read_refusals\":%llu,\"object_usage\":[",
        reason,boolean(state.retired),n(state.dispatchCalls),n(m.dispatches),n(m.acceptedDispatches),n(m.refusedDispatches),n(state.matchedDispatches),n(state.ordinaryDispatches),
        n(state.matchedDetails),n(state.ordinaryDetails),n(state.emittedGroups),n(state.emittedBindings),n(state.omittedGroups),
        n(state.periodicSummaries),n(state.omittedPeriodicSummaries),n(state.periodicMatchedDetails),n(state.omittedPeriodicMatchedDetails),
        n(state.healthSummaries),n(state.omittedHealthSummaries),n(state.finalSummaries),n(state.explicitSummaries),n(state.omittedExplicitSummaries),n(state.sourceObservations),
        n(state.sourceChanges),n(state.sourceClears),n(state.invalidSources),n(state.validationCalls),n(state.validationRefusals),n(state.failures),
        n(m.registrations),n(m.retirements),n(m.mutations),n(m.capacityRefusals),n(m.failedDriverCalls),n(m.observedIssues),n(m.fatalIssues),
        m.referencedSetsHighWater,m.referencedSetsCapacity,n(m.referenceOverflowRefusals),n(m.restoreRangeDisturbances),n(m.ordinaryUpdateCalls),n(m.coreTemplateUpdateCalls),n(m.khrTemplateUpdateCalls),
        n(m.templateUpdateRefusals),n(m.templateReadRefusals));
    for(std::size_t i=0;i<m.objects.size();++i){const auto& object=m.objects[i];
        out.append("%s{\"kind\":\"%s\",\"live\":%zu,\"high_water\":%zu,\"capacity\":%zu,\"capacity_refusals\":%llu}",
            i?",":"",dm::objectName(static_cast<dm::ObjectKind>(i)),object.live,object.highWater,object.capacity,n(object.capacityRefusals));}
    out.append("],\"template_refused_issue_records\":[");
    bool comma=false;
    for(std::size_t i=0;i<m.templateRefusedIssueRecords.size();++i)if(m.templateRefusedIssueRecords[i]){
        out.append("%s{\"issue_bit\":%llu,\"count\":%llu}",comma?",":"",n(std::uint64_t(1)<<i),n(m.templateRefusedIssueRecords[i]));comma=true;}
    out.append("],\"refused_issue_records\":[");comma=false;
    for(std::size_t i=0;i<state.refusedIssueRecords.size();++i)if(state.refusedIssueRecords[i]){
        out.append("%s{\"issue_bit\":%llu,\"count\":%llu}",comma?",":"",n(std::uint64_t(1)<<i),n(state.refusedIssueRecords[i]));comma=true;}
    out.append("],\"source_samples\":[");
    for(std::size_t i=0;i<sourceCount;++i)out.append("%s{\"source_key\":\"%s\",\"image\":\"0x%llx\",\"image_generation\":%llu,\"source_counter\":%llu}",
        i?",":"",sourceName(i),n(state.sources[i].image.handle),n(state.sources[i].image.generation),n(state.sources[i].counter));
    out.append("]");
    out.append(",\"driver_set_allocation_calls\":%llu,\"driver_sets_allocated\":%llu,"
               "\"driver_set_free_calls\":%llu,\"driver_sets_freed\":%llu,"
               "\"driver_pool_reset_calls\":%llu,\"driver_pool_destroy_calls\":%llu,"
               "\"driver_live_sets\":%llu,\"driver_set_high_water\":%llu,"
               "\"declared_pool_max_sets\":%llu,\"driver_set_ledger_complete\":%s",
               n(m.driverSetAllocationCalls), n(m.driverSetsAllocated),
               n(m.driverSetFreeCalls), n(m.driverSetsFreed),
               n(m.driverPoolResetCalls), n(m.driverPoolDestroyCalls),
               n(m.driverLiveSets), n(m.driverSetHighWater),
               n(m.declaredPoolMaxSets), boolean(m.driverSetLedgerComplete));
    out.append(",\"continuous_dispatch_selection\":%s,\"filtered_dispatches\":%llu",boolean(m.continuousDispatchSelection),n(m.filteredDispatches));
    out.append(",\"compact_ordinary_dispatch\":%s,\"compact_pipeline_classifications\":%llu,\"compact_dispatches\":%llu,\"compact_unknown_dispatches\":%llu,\"compact_image_references\":%llu",
        boolean(m.compactOrdinaryDispatch),n(m.compactPipelineClassifications),n(m.compactDispatches),n(m.compactUnknownDispatches),n(m.compactImageReferences));
    out.append(",\"template_read_calls\":%llu,\"template_read_bytes\":%llu,\"template_read_elements\":%llu",n(m.templateReadCalls),n(m.templateReadBytes),n(m.templateReadElements));
    out.append(",\"reference_mask_builds\":%llu,\"reference_mask_images\":%llu",n(m.referenceMaskBuilds),n(m.referenceMaskImages));
    out.append(",\"reference_mask_policy\":\"on_query\"");
    out.append(",\"reference_query_timing\":%s",boolean(state.referenceQueryTiming));
    for(unsigned i=0;i<state.referenceQueries.size();++i){const auto& query=state.referenceQueries[i];
        out.append(",\"reference_query_%s\":{\"count\":%llu,\"sets\":%llu,\"contended\":%llu,\"total_ns\":%llu,\"wait_ns\":%llu}",
            i?"draw":"bind",n(query.calls),n(query.sets),n(query.contended),n(query.totalNs),n(query.waitNs));}
    out.append(",\"reference_consumption_measured\":%s,\"reference_mask_measured_builds\":%llu,\"reference_mask_retired_builds\":%llu,\"reference_mask_consumed_builds\":%llu,\"reference_mask_unconsumed_builds\":%llu,\"reference_mask_pending_builds\":%llu",
        boolean(m.referenceConsumptionMeasured),n(m.referenceMaskMeasuredBuilds),n(m.referenceMaskRetiredBuilds),n(m.referenceMaskConsumedBuilds),n(m.referenceMaskUnconsumedBuilds),n(m.referenceMaskPendingBuilds));
    out.append(",\"reference_lifetime_scans\":%llu,\"reference_lifetime_candidates\":%llu,\"reference_lifetime_invalidations\":%llu",
        n(m.referenceLifetimeScans),n(m.referenceLifetimeCandidates),n(m.referenceLifetimeInvalidations));
    out.append(",\"reference_edges\":%llu,\"reference_edge_overflows\":%llu",n(m.referenceEdges),n(m.referenceEdgeOverflows));
    out.append(",\"template_span_read_calls\":%llu,\"template_span_read_successes\":%llu,\"template_span_read_fallbacks\":%llu",n(m.templateSpanReadCalls),n(m.templateSpanReadSuccesses),n(m.templateSpanReadFallbacks));
    out.append(",\"template_span_read_oversize\":%llu,\"template_span_max_bytes\":%llu",n(m.templateSpanReadOversize),n(m.templateSpanMaxBytes));
    falseClaims(out);out.append("}");emit(c,out);
}
}
Context* create(VkDevice device,Sink sink,dm::Config config) noexcept {
    ErrorGuard preserve;if(!bits(device))return nullptr;
    try{return new Context(device,sink,config);}catch(...){return nullptr;}
}
void destroy(Context* c) noexcept {ErrorGuard preserve;delete c;}
void retire(Context* c) noexcept {guarded(c,[&]{c->tracker.stop();c->totals.retired=true;c->totals.sources={};});}
Stats stats(Context* c) noexcept {ErrorGuard preserve;if(!c)return {};try{std::lock_guard lock(c->mutex);return snapshot(c);}catch(...){increment(c->failures);return {};}}
void summary(Context* c) noexcept {
    ErrorGuard preserve;if(!c)return;Stats state;bool final=false;
    try{std::lock_guard lock(c->mutex);
        if(c->totals.retired&&!c->totals.finalSummaries){increment(c->totals.finalSummaries);final=true;}
        else {if(c->totals.explicitSummaries>=explicitSummaryLimit){increment(c->totals.omittedExplicitSummaries);return;}
            increment(c->totals.explicitSummaries);}
        state=snapshot(c);}catch(...){increment(c->failures);return;}
    emitSummary(c,state,final?"retired_final":"explicit");
}
void observeSource(Context* c,Source source,Key key,std::uint64_t counter) noexcept {guarded(c,[&]{
    increment(c->totals.sourceObservations);const auto i=static_cast<std::size_t>(source);
    if(i>=sourceCount){increment(c->totals.invalidSources);return;}
    if((key.handle==0)!=(key.generation==0)){increment(c->totals.invalidSources);key={};counter=0;}
    if(!key.handle)counter=0;
    auto& slot=c->totals.sources[i];if(slot.image!=key){increment(c->totals.sourceChanges);if(!key.handle)increment(c->totals.sourceClears);}
    slot={key,counter};});}
void imageObserved(Context* c,const image_inventory::Record& value) noexcept {guarded(c,[&]{c->tracker.imageObserved(value);});}
void imageRetiring(Context* c,Key key) noexcept {guarded(c,[&]{c->tracker.imageRetiring(key);for(auto& source:c->totals.sources)if(source.image==key&&key.handle){source={};increment(c->totals.sourceClears);increment(c->totals.sourceChanges);}});}
void viewCreated(Context* c,VkImageView h,const VkImageViewCreateInfo* p,VkResult r) noexcept {guarded(c,[&]{c->tracker.viewCreated(h,p,r);});}
void viewDestroying(Context* c,VkImageView h) noexcept {guarded(c,[&]{c->tracker.viewDestroying(h);});}
descriptor_image_metadata::Result setLayoutCreated(Context* c,VkDescriptorSetLayout h,const VkDescriptorSetLayoutCreateInfo* p,VkResult r) noexcept {
    descriptor_image_metadata::Result result{{},1};guarded(c,[&]{result=c->tracker.setLayoutCreated(h,p,r);});return result;}
void setLayoutDestroying(Context* c,VkDescriptorSetLayout h) noexcept {guarded(c,[&]{c->tracker.setLayoutDestroying(h);});}
descriptor_image_metadata::Result pipelineLayoutCreated(Context* c,VkPipelineLayout h,const VkPipelineLayoutCreateInfo* p,VkResult r) noexcept {
    descriptor_image_metadata::Result result{{},1};guarded(c,[&]{result=c->tracker.pipelineLayoutCreated(h,p,r);});return result;}
void pipelineLayoutDestroying(Context* c,VkPipelineLayout h) noexcept {guarded(c,[&]{c->tracker.pipelineLayoutDestroying(h);});}
void poolCreated(Context* c,VkDescriptorPool h,const VkDescriptorPoolCreateInfo* p,VkResult r) noexcept {guarded(c,[&]{c->tracker.poolCreated(h,p,r);});}
void poolReset(Context* c,VkDescriptorPool h,VkResult r) noexcept {guarded(c,[&]{c->tracker.poolReset(h,r);});}
void poolDestroying(Context* c,VkDescriptorPool h) noexcept {guarded(c,[&]{c->tracker.poolDestroying(h);});}
void setsAllocated(Context* c,const VkDescriptorSetAllocateInfo* p,const VkDescriptorSet* h,VkResult r) noexcept {
    guarded(c,[&]{c->tracker.setsAllocated(p,h,r);});
}
void setsFreed(Context* c,VkDescriptorPool p,std::uint32_t count,const VkDescriptorSet* h,VkResult r) noexcept {guarded(c,[&]{c->tracker.setsFreed(p,count,h,r);});}
void updateDescriptors(Context* c,std::uint32_t nw,const VkWriteDescriptorSet* w,std::uint32_t nc,const VkCopyDescriptorSet* p) noexcept {guarded(c,[&]{c->tracker.updateDescriptors(nw,w,nc,p);});}
void templateCreated(Context* c,VkDescriptorUpdateTemplate h,const VkDescriptorUpdateTemplateCreateInfo* p,VkResult r) noexcept {guarded(c,[&]{c->tracker.templateCreated(h,p,r);});}
void templateDestroying(Context* c,VkDescriptorUpdateTemplate h) noexcept {guarded(c,[&]{c->tracker.templateDestroying(h);});}
void updateWithTemplate(Context* c,VkDescriptorSet h,VkDescriptorUpdateTemplate t,const void* p,TemplateUpdateKind k,TemplateReader r) noexcept {
    guarded(c,[&]{c->tracker.updateWithTemplate(h,t,p,k,r);});
}
void setUnsupported(Context* c,VkDescriptorSet h,Issue issue) noexcept {guarded(c,[&]{c->tracker.setUnsupported(h,issue);});}
void commandPoolCreated(Context* c,VkCommandPool h,const VkCommandPoolCreateInfo* p,VkResult r) noexcept {guarded(c,[&]{c->tracker.commandPoolCreated(h,p,r);});}
void commandPoolReset(Context* c,VkCommandPool h,VkResult r) noexcept {guarded(c,[&]{c->tracker.commandPoolReset(h,r);});}
void commandPoolDestroying(Context* c,VkCommandPool h) noexcept {guarded(c,[&]{c->tracker.commandPoolDestroying(h);});}
void allocate(Context* c,const VkCommandBufferAllocateInfo* p,const VkCommandBuffer* h,VkResult r) noexcept {guarded(c,[&]{c->tracker.commandsAllocated(p,h,r);});}
void free(Context* c,std::uint32_t count,const VkCommandBuffer* h) noexcept {guarded(c,[&]{c->tracker.commandsFreed(count,h);});}
void begin(Context* c,VkCommandBuffer h,const VkCommandBufferBeginInfo* p,VkResult r) noexcept {guarded(c,[&]{c->tracker.begin(h,p,r);});}
void end(Context* c,VkCommandBuffer h,VkResult r) noexcept {guarded(c,[&]{c->tracker.end(h,r);});}
void reset(Context* c,VkCommandBuffer h,VkResult r) noexcept {guarded(c,[&]{c->tracker.reset(h,r);});}
descriptor_image_metadata::Result pipelineCreated(Context* c,VkPipeline h,const VkComputePipelineCreateInfo* p,VkResult r) noexcept {
    descriptor_image_metadata::Result result{{},descriptor_image_metadata::bit(Issue::UnknownObject)};
    guarded(c,[&]{result=c->tracker.pipelineCreated(h,p,r);});
    return result;
}
void pipelineDestroying(Context* c,VkPipeline h) noexcept {guarded(c,[&]{c->tracker.pipelineDestroying(h);});}
bool allowCompactPipeline(Context* c,Key key) noexcept {
    bool allowed=false;guarded(c,[&]{allowed=c->tracker.allowCompactPipeline(key);});return allowed;
}
// The tracker records compute bindings only; other bind points skip the lock.
void bindPipeline(Context* c,VkCommandBuffer h,VkPipelineBindPoint b,VkPipeline p) noexcept {if(b!=VK_PIPELINE_BIND_POINT_COMPUTE)return;guarded(c,[&]{c->tracker.bindPipeline(h,b,p);});}
void bindSets(Context* c,VkCommandBuffer h,VkPipelineBindPoint b,VkPipelineLayout l,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* p,std::uint32_t offsets,const std::uint32_t* values) noexcept {if(b!=VK_PIPELINE_BIND_POINT_COMPUTE)return;guarded(c,[&]{c->tracker.bindSets(h,b,l,first,count,p,offsets,values);});}
void commandUnsupported(Context* c,VkCommandBuffer h,Issue issue) noexcept {guarded(c,[&]{c->tracker.commandUnsupported(h,issue);});}
DispatchEvidence preDispatch(Context* c,VkCommandBuffer h,std::uint32_t x,std::uint32_t y,std::uint32_t z,DispatchKind kind,const CommandRecordingFacts* recording,RecordedSourceLayoutQuery query,resource_memory_observer::SourceQuery memoryQuery,DispatchSourceFacts* output,SourceQueryDemand demand) noexcept {
    if(output)output->observed=false;
    ErrorGuard preserve;DispatchEvidence value;value.issues=dm::bit(Issue::UnknownObject);if(!c)return value;
    // Copy once, before any sink can mutate a separate observer. Every line in
    // this group uses this exact owned copy; descriptor acceptance is unchanged.
    CommandRecordingFacts facts;if(recording)facts=*recording;
    if(recording&&(facts.device!=c->device||facts.command!=bits(h)||!facts.contextIncarnation)){
        facts.reasons|=bit(CommandRecordingReason::IdentityMismatch);
        facts.stateKnown=facts.recording=facts.executable=facts.scopeKnown=false;
    }
    Stats state;std::array<SourceSample,sourceCount> querySources{};SourceLayouts layouts;
    bool details=false,periodic=false,health=false,matched=false;const char* reason="ordinary_coverage";
    try{
        std::lock_guard lock(c->mutex);if(c->totals.retired){value.issues=dm::bit(Issue::Stopped);return value;}
        increment(c->totals.dispatchCalls);value=c->tracker.preDispatch(h,x,y,z,kind,demand.diagnosticAllBindings);
        if(value.filteredOut)return value;
        if(!value.metadataAccepted)for(std::size_t i=0;i<c->totals.refusedIssueRecords.size();++i)
            if(value.issues&(std::uint64_t(1)<<i))increment(c->totals.refusedIssueRecords[i]);
        if(value.metadataAccepted&&!value.issues&&value.count<=value.bindings.size())
            for(std::size_t i=0;i<value.count;++i)if(sourceMask(value.bindings[i],c->totals)){matched=true;break;}
        if(matched){increment(c->totals.matchedDispatches);
            if(c->totals.matchedDetails<matchedDetailLimit){increment(c->totals.matchedDetails);details=true;reason="source_image_match";}
            if(c->totals.matchedDispatches%periodicInterval==0){
                if(c->totals.periodicSummaries<periodicSummaryLimit){increment(c->totals.periodicSummaries);periodic=true;}
                else increment(c->totals.omittedPeriodicSummaries);
                if(c->totals.periodicMatchedDetails<periodicMatchedDetailLimit){increment(c->totals.periodicMatchedDetails);details=true;reason="periodic_source_image_match";}
                else increment(c->totals.omittedPeriodicMatchedDetails);}
        }else {increment(c->totals.ordinaryDispatches);if(c->totals.ordinaryDetails<ordinaryDetailLimit){increment(c->totals.ordinaryDetails);details=true;}}
        const auto dispatches=c->totals.dispatchCalls;
        if(dispatches&&dispatches%healthInterval==0){
            if(c->totals.healthSummaries<healthSummaryLimit){increment(c->totals.healthSummaries);health=true;}
            else increment(c->totals.omittedHealthSummaries);}
        if(!details)increment(c->totals.omittedGroups);
        if(details||periodic||health)state=snapshot(c);
        if(matched)querySources=c->totals.sources;
    }catch(...){increment(c->failures);value={};value.issues=dm::bit(Issue::Stopped);return value;}
    // Callback runs after releasing the descriptor mutex and before the original
    // dispatch. It never holds two observers' locks at once. Matching sources and
    // results are owned snapshots even when this dispatch emits no detail.
    std::uint32_t rawRoles=0;
    if(matched&&value.metadataAccepted&&!value.issues&&value.count<=value.bindings.size())for(std::size_t j=0;j<value.count;++j){
        const auto& binding=value.bindings[j];bool requested=false;
        for(std::size_t k=0;k<querySources.size();++k){const auto& source=querySources[k];if(binding.image==source.image&&source.image.handle&&source.image.generation){requested=true;layouts.sourceMasks[j]|=1u<<k;}}
        if(requested&&binding.setIndex==0){
            if(binding.binding==5&&(layouts.sourceMasks[j]&(1u<<unsigned(Source::RawDepth))))rawRoles|=1;
            if(binding.binding==6&&(layouts.sourceMasks[j]&(1u<<unsigned(Source::RawColor))))rawRoles|=2;
            if(binding.binding==7&&(layouts.sourceMasks[j]&(1u<<unsigned(Source::RawMotion))))rawRoles|=4;
        }
    }
    const bool probeQuery=demand.rawPatternProbeLive&&rawRoles==7&&(!demand.requestProbeQuery||demand.requestProbeQuery(demand.queryUser));
    const bool runQueries=demand.everyMatchedBinding||details||probeQuery;
    if(runQueries&&matched&&value.metadataAccepted&&!value.issues&&value.count<=value.bindings.size())for(std::size_t j=0;j<value.count;++j){
        const auto& binding=value.bindings[j];if(!layouts.sourceMasks[j])continue;
        if(!demand.everyMatchedBinding&&!details&&!(binding.setIndex==0&&binding.binding==6&&(layouts.sourceMasks[j]&(1u<<unsigned(Source::RawColor)))))continue;
        layouts.requested[j]=true;++layouts.requestedCount;
        auto& result=layouts.values[j];
        if(query.resolve&&recording)result=query.resolve(query.user,facts,{binding.image.handle,binding.image.generation},binding.range,binding.declaredLayout);
        if(memoryQuery.query)layouts.memory[j]=memoryQuery.query(memoryQuery.user,{binding.image.handle,binding.image.generation});
        const auto& memory=layouts.memory[j];if(memory.queried)++layouts.memoryQueries;
        if(memory.binding.bindingBoundsKnown)++layouts.memoryBounds;if(memory.binding.observedAliasFree)++layouts.memoryAliasFree;
        layouts.memoryLocalIssues|=memory.localIssues;layouts.memoryGlobalIssues|=memory.globalIssues;
        if(result.queried)++layouts.queriedCount;
        if(result.recordedLayoutResolved)++layouts.resolvedCount;
        if(result.recordedPrefixSupported)++layouts.supportedCount;
        layouts.issues|=result.issues;
    }
    if(output&&(probeQuery||demand.everyMatchedBinding)){output->observed=true;output->evidence=value;output->recording=facts;output->sources=querySources;output->sourceMasks=layouts.sourceMasks;output->layouts=layouts.values;output->memory=layouts.memory;}
    if(details)emitDetails(c,value,state,reason,facts,layouts);
    if(periodic)emitSummary(c,state,"matched_dispatch_interval");
    if(health)emitSummary(c,state,"all_dispatch_health_interval");
    return value;
}
dm::Result validateEvidence(Context* c,const DispatchEvidence& evidence) noexcept {
    ErrorGuard preserve;dm::Result result{evidence.command,dm::bit(Issue::UnknownObject)};if(!c)return result;
    try{std::lock_guard lock(c->mutex);increment(c->totals.validationCalls);
        result=c->totals.retired?dm::Result{evidence.command,dm::bit(Issue::Stopped)}:c->tracker.validateEvidence(evidence);
        if(result.issues)increment(c->totals.validationRefusals);
    }catch(...){increment(c->failures);result.issues=dm::bit(Issue::Stopped);}return result;
}
dm::SetImageSnapshot diagnosticSetImages(Context* c,VkDescriptorSet set) noexcept {
    ErrorGuard preserve; dm::SetImageSnapshot out; if(!c)return out;
    try { std::lock_guard lock(c->mutex); if(!c->totals.retired)out=c->tracker.diagnosticSetImages(set); }
    catch(...) { increment(c->failures); }
    return out;
}
dm::SetImageReferences imageReferences(Context* c,VkDescriptorSet set) noexcept {
    ErrorGuard preserve;dm::SetImageReferences out;if(!c)return out;
    try {std::lock_guard lock(c->mutex);if(!c->totals.retired)out=c->tracker.imageReferences(set);}
    catch(...) {increment(c->failures);}
    return out;
}
dm::Key setIdentity(Context* c,VkDescriptorSet set) noexcept {
    ErrorGuard preserve;dm::Key out;if(!c)return out;
    try {std::lock_guard lock(c->mutex);if(!c->totals.retired)out=c->tracker.setIdentity(set);}
    catch(...) {increment(c->failures);}
    return out;
}
std::uint64_t imageReferenceRevision(Context* c) noexcept {
    if(!c)return 0;const auto revision=c->imageRevision.load(std::memory_order_acquire);return revision&1?0:revision;
}
std::uint64_t referenceMaskEpoch(Context* c) noexcept {
    return c&&c->referenceMasksHealthy.load(std::memory_order_acquire)?c->tracker.referenceEpoch():0;
}
std::uint64_t watchReferenceImages(Context* c,std::span<const Key> images) noexcept {
    ErrorGuard preserve;if(!c)return 0;
    try{std::lock_guard lock(c->mutex);return c->totals.retired||!c->referenceMasksHealthy?0:c->tracker.watchReferenceImages(images);}
    catch(...){c->referenceMasksHealthy=false;increment(c->failures);return 0;}
}
bool referenceBindings(Context* c,std::span<const VkDescriptorSet> sets,std::span<dm::ReferenceBinding> out,ReferenceQuerySource source) noexcept {
    ErrorGuard preserve;for(auto& value:out)value={};if(!c||sets.size()!=out.size())return false;
    try{const bool measured=c->totals.referenceQueryTiming;
        const auto started=measured?cpuWallNowNs():0;
        std::unique_lock lock(c->mutex,std::defer_lock);bool contended=false;
        if(measured){if(!lock.try_lock()){contended=true;lock.lock();}}else lock.lock();
        const auto acquired=measured?cpuWallNowNs():0;
        const bool accepted=!c->totals.retired&&c->referenceMasksHealthy;
        if(accepted)for(std::size_t n=0;n<sets.size();++n)if(sets[n])out[n]=c->tracker.referenceBinding(sets[n]);
        if(measured){auto& query=c->totals.referenceQueries[source==ReferenceQuerySource::Draw?1:0];
            ++query.calls;query.sets+=sets.size();query.contended+=contended;query.waitNs+=acquired-started;query.totalNs+=cpuWallNowNs()-started;}
        return accepted;
    }catch(...){c->referenceMasksHealthy=false;increment(c->failures);return false;}
}
std::uint64_t imageReferences(Context* c,std::span<const dm::Key> sets,std::span<dm::SetImageReferences> out) noexcept {
    ErrorGuard preserve;for(auto& row:out)row={};if(!c||sets.size()!=out.size())return 0;
    try{std::lock_guard lock(c->mutex);if(c->totals.retired)return 0;
        for(std::size_t n=0;n<sets.size();++n)if(sets[n].handle)out[n]=c->tracker.imageReferences(reinterpret_cast<VkDescriptorSet>(sets[n].handle));
        return imageReferenceRevision(c);
    }catch(...){increment(c->failures);return 0;}
}
std::uint64_t setIdentities(Context* c,std::span<const VkDescriptorSet> sets,std::span<dm::Key> out) noexcept {
    ErrorGuard preserve;for(auto& row:out)row={};if(!c||sets.size()!=out.size())return 0;
    try{std::lock_guard lock(c->mutex);if(c->totals.retired)return 0;
        for(std::size_t n=0;n<sets.size();++n)out[n]=c->tracker.setIdentity(sets[n]);
        return imageReferenceRevision(c);
    }catch(...){increment(c->failures);return 0;}
}
dm::Result validateImageBinding(Context* c,const dm::ImageBinding& image) noexcept {
    ErrorGuard preserve;if(!c)return {image.image,dm::bit(Issue::UnknownObject)};
    try{std::lock_guard lock(c->mutex);return c->totals.retired?dm::Result{image.image,dm::bit(Issue::Stopped)}:c->tracker.validateImageBinding(image);}
    catch(...){return {image.image,dm::bit(Issue::Stopped)};}
}
} // namespace xess_vk::descriptor_image_observer
