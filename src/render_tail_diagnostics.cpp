#include "render_tail_diagnostics.hpp"
#include <algorithm>
#include <mutex>
#include <sstream>

namespace xess_vk::render_tail {
namespace {
bool sameFrame(const FrameTimingAssociation& a, const FrameTimingAssociation& b) {
    return a.valid && b.valid && a.ticket && a.engineFrame == b.engineFrame && a.ticket == b.ticket;
}
void handle(std::ostream& out, std::uint64_t v) { out << "\"0x" << std::hex << v << std::dec << '"'; }
const char* kind(Kind value) {
    switch (value) {
    case Kind::Pass: return "render_pass";
    case Kind::Dispatch: return "compute_dispatch";
    case Kind::Copy: return "copy_image";
    case Kind::Blit: return "blit_image";
    default: return "execute_secondary";
    }
}
}
struct Store::Impl {
    struct Command {
        Key key{};
        std::array<Event, eventLimit> events{};
        std::size_t next{}, count{};
        std::uint64_t observed{}, dropped{}, activePass{}, lastOrdinal{}, queue{}, submitToken{}, targetOrdinal{},pool{};
        VkResult submitResult{VK_NOT_READY};
        GraphicsState graphics;
        Key target{}, source{};
        wsi_image_inventory::OwnedImage owner;
        bool hasTarget{}, ended{};
    };
    struct Frozen {
        std::uint64_t serial{}, queue{}, token{};
        std::array<Command, otherCommandLimit + 1> commands{};
        std::array<bool, otherCommandLimit + 1> sameSubmit{};
        std::size_t count{}, relatedDrops{};
        Source source;
        bool frameKnown{}, emitting{};
    };
    std::uint64_t device{}, ordinal{}, serial{};
    Sink sink{};
    mutable std::mutex mutex;
    Stats stats;
    std::array<Command, commandLimit> commands{};
    std::array<wsi_image_inventory::OwnedImage, wsi_image_inventory::maximumRemoved> wsi{};
    std::array<Source, 8> sources{};
    std::size_t sourceCursor{};
    std::array<FrameTimingAssociation, 2> seen{};
    std::array<Frozen, 2> pending{};
    Impl(std::uint64_t d, Sink s) : device(d), sink(s) {}
    bool active() const { return stats.armed && !stats.stopped; }
    static void clear(Command& c, Key key = {}) {
        c.key=key;c.next=c.count=0;c.observed=c.dropped=c.activePass=c.lastOrdinal=c.queue=c.submitToken=c.targetOrdinal=0;
        c.submitResult=VK_NOT_READY;c.graphics={};c.target=c.source={};c.owner={};c.hasTarget=c.ended=false;c.pool=0;
        for(auto& e:c.events)e.ordinal=0;
    }
    Command* find(Key key) {
        for (auto& c : commands) if (c.key == key && key.handle && key.generation) return &c;
        return nullptr;
    }
    Command* command(const CommandRecordingFacts& f) {
        if (!active() || f.device != device || !f.contextIncarnation || !f.command || !f.rpRecordingGeneration) return nullptr;
        const Key key{f.command, f.rpRecordingGeneration};
        if (auto* c = find(key)) return c;
        Command* empty = nullptr;
        for (auto& c : commands) {
            if (c.key.handle == f.command) { clear(c,key);c.pool=f.pool;return &c; }
            if (!c.key.handle && !empty) empty = &c;
        }
        if (!empty) {
            for(auto& c:commands)if(!c.hasTarget&&(c.ended||c.submitToken)&&(!empty||c.lastOrdinal<empty->lastOrdinal))empty=&c;
            if(!empty)for(auto& c:commands)if(!c.hasTarget&&(!empty||c.lastOrdinal<empty->lastOrdinal))empty=&c;
            if(empty){clear(*empty,key);empty->pool=f.pool;++stats.evictedCommands;return empty;}
            ++stats.refused;return nullptr;
        }
        empty->key = key;empty->pool=f.pool;return empty;
    }
    const wsi_image_inventory::OwnedImage* owner(Key key) const {
        for (const auto& i : wsi) if (i.record.image == key.handle && i.record.generation == key.generation && key.handle) return &i;
        return nullptr;
    }
    Event* currentPass(Command& c) {
        for (auto& event : c.events) if (event.ordinal && event.ordinal == c.activePass && event.kind == Kind::Pass) return &event;
        return nullptr;
    }
    void append(Command& c, Event event) {
        if (ordinal == UINT64_MAX) { stats.stopped = true; return; }
        event.ordinal = ++ordinal; c.lastOrdinal = ordinal;
        ++c.observed; ++stats.observed;
        if (c.count == eventLimit) { ++c.dropped; ++stats.overwritten; } else ++c.count;
        c.events[c.next] = event;
        if (event.kind == Kind::Pass) c.activePass = event.ordinal;
        if ((event.kind == Kind::Copy || event.kind == Kind::Blit) && event.regionCount) {
            Key source{}, destination{};
            for (std::size_t i = 0; i < event.imageCount; ++i) {
                const auto& image = event.images[i];
                if (image.role & TransferSource) source = {image.record.image, image.record.generation};
                if (image.role & TransferDestination) destination = {image.record.image, image.record.generation};
            }
            if (const auto* owned = owner(destination)) {
                c.hasTarget = true; c.target = destination; c.source = source; c.owner = *owned;c.targetOrdinal=event.ordinal;
            }
        }
        c.next = (c.next + 1) % eventLimit;
    }
    static bool writes(const Command& c, Key image) {
        if (!image.handle || !image.generation) return false;
        for (const auto& e : c.events) if (e.ordinal)
            for (std::size_t n = 0; n < e.imageCount; ++n)
                if (e.images[n].record.image == image.handle && e.images[n].record.generation == image.generation &&
                    (e.images[n].role & (Attachment | Storage | TransferDestination))) return true;
        return false;
    }
    void emit(const std::string& line) {
        bool deliver = false;
        {
            std::lock_guard lock(mutex);
            if (line.size() + 1 <= byteLimit - std::min<std::size_t>(byteLimit, stats.emittedBytes)) {
                stats.emittedBytes += line.size() + 1; deliver = true;
            } else { ++stats.omittedLines; stats.stopped = true; }
        }
        if (deliver && sink) sink(line.c_str());
    }
    void output(const Frozen& snapshot, VkResult driver) {
        std::ostringstream header;
        header << "{\"event\":\"render_tail_sample\",\"device\":"; handle(header, device);
        header << ",\"snapshot\":" << snapshot.serial << ",\"queue\":"; handle(header, snapshot.queue);
        header << ",\"submit_token\":" << snapshot.token << ",\"driver_result\":" << driver
            << ",\"frame_association_unknown\":" << (snapshot.frameKnown ? "false" : "true")
            << ",\"engine_frame\":" << snapshot.source.frame.engineFrame << ",\"sdk_id\":" << snapshot.source.frame.ticket.sdkId
            << ",\"context_generation\":" << snapshot.source.frame.ticket.contextGeneration
            << ",\"prepare_producer\":" << snapshot.source.producer << ",\"registration\":" << snapshot.source.registration
            << ",\"graph_context\":"; handle(header, snapshot.source.graphContext);
        header << ",\"prepare_accepted\":" << (snapshot.source.accepted ? "true" : "false")
            << ",\"prepare_depth\":";handle(header,snapshot.source.depth.handle);
        header << ",\"prepare_depth_generation\":" << snapshot.source.depth.generation << ",\"prepare_motion\":";handle(header,snapshot.source.motion.handle);
        header << ",\"prepare_motion_generation\":" << snapshot.source.motion.generation
            << ",\"render_extent\":[" << snapshot.source.renderWidth << ',' << snapshot.source.renderHeight << ']'
            << ",\"upscale_extent\":[" << snapshot.source.upscaleWidth << ',' << snapshot.source.upscaleHeight << ']'
            << ",\"related_command_count\":" << snapshot.count - 1 << ",\"related_command_drops\":" << snapshot.relatedDrops
            << ",\"graph_inputs\":[";
        for (std::size_t n = 0; n < snapshot.source.graphHandles.size(); ++n) {
            if (n) header << ',';
            header << "{\"handle\":"; handle(header, snapshot.source.graphHandles[n]);
            header << ",\"key\":" << snapshot.source.graphKeys[n] << ",\"texture_id\":" << snapshot.source.textureIds[n] << '}';
        }
        header << "],\"cross_cb_inputs_unknown\":true,\"gpu_complete\":false,\"copy_permitted\":false}"; emit(header.str());
        for (std::size_t index = 0; index < snapshot.count; ++index) {
            const auto& c = snapshot.commands[index];
            std::ostringstream command;
            command << "{\"event\":\"render_tail_command\",\"snapshot\":" << snapshot.serial << ",\"scope\":\""
                << (index == 0 ? "target_wsi_write" : snapshot.sameSubmit[index] ? "same_submit_writer_candidate" : "recent_other_cb_not_dependency_proof")
                << "\",\"command\":"; handle(command, c.key.handle);
            command << ",\"rp_recording_generation\":" << c.key.generation << ",\"observed_events\":" << c.observed
                << ",\"retained_events\":" << c.count << ",\"overwritten_events\":" << c.dropped << ",\"queue\":";
            handle(command, snapshot.sameSubmit[index] ? snapshot.queue : c.queue);
            command << ",\"submit_token\":" << (snapshot.sameSubmit[index] ? snapshot.token : c.submitToken)
                << ",\"driver_result\":" << (snapshot.sameSubmit[index] ? driver : c.submitResult)
                << ",\"target_image\":"; handle(command, c.target.handle);
            command << ",\"target_generation\":" << c.target.generation << ",\"source_image\":"; handle(command, c.source.handle);
            command << ",\"source_generation\":" << c.source.generation << ",\"swapchain\":"; handle(command, c.owner.owner.chain);
            for(const auto& anchor:c.events)if(anchor.ordinal&&anchor.ordinal==c.targetOrdinal)command<<",\"anchor_kind\":\""<<kind(anchor.kind)<<'"';
            command << ",\"swapchain_generation\":" << c.owner.owner.generation << ",\"swapchain_index\":" << c.owner.index
                << ",\"complete_hook_coverage\":false,\"last_writer_known\":false,\"resource_lifetime_pinned\":false}"; emit(command.str());
            for (std::size_t e = 0; e < c.count; ++e) {
                const auto& event = c.events[(c.next + eventLimit - c.count + e) % eventLimit];
                std::ostringstream row;
                row << "{\"event\":\"render_tail_event\",\"snapshot\":" << snapshot.serial << ",\"command\":"; handle(row, c.key.handle);
                row << ",\"rp_recording_generation\":" << c.key.generation << ",\"ordinal\":" << event.ordinal
                    << ",\"common_record_order\":" << event.recording.commonRecordOrder << ",\"kind\":\"" << kind(event.kind)
                    << "\",\"engine_frame\":" << event.frame.engineFrame << ",\"sdk_id\":" << event.frame.ticket.sdkId
                    << ",\"context_generation\":" << event.frame.ticket.contextGeneration << ",\"same_frame_correlation\":"
                    << (sameFrame(event.frame, snapshot.source.frame) ? "true" : "false") << ",\"pipeline\":"; handle(row, event.pipeline);
                row << ",\"pipeline_generation_known\":false,\"render_pass\":"; handle(row, event.renderPass.handle);
                row << ",\"render_pass_generation\":" << event.renderPass.generation << ",\"framebuffer\":"; handle(row, event.framebuffer.handle);
                row << ",\"framebuffer_generation\":" << event.framebuffer.generation << ",\"render_area\":["
                    << event.renderArea.offset.x << ',' << event.renderArea.offset.y << ',' << event.renderArea.extent.width << ',' << event.renderArea.extent.height
                    << "],\"draw_count\":" << event.drawCount << ",\"pipeline_changes\":" << event.pipelineChanges << ",\"image_count\":" << event.imageCount
                    << ",\"image_drops\":" << event.imageDrops << ",\"issues\":" << event.issues << ",\"source_layout\":" << event.sourceLayout
                    << ",\"destination_layout\":" << event.destinationLayout << ",\"blit_filter\":" << event.filter << ",\"regions\":[";
                for (std::size_t r = 0; r < event.regionCount; ++r) {
                    if (r) row << ','; const auto& region = event.regions[r];
                    row << "{\"src_subresource\":[" << region.source.aspectMask << ',' << region.source.mipLevel << ',' << region.source.baseArrayLayer << ',' << region.source.layerCount
                        << "],\"dst_subresource\":[" << region.destination.aspectMask << ',' << region.destination.mipLevel << ',' << region.destination.baseArrayLayer << ',' << region.destination.layerCount
                        << "],\"src_offsets\":[";
                    for (unsigned n = 0; n < 2; ++n) { if (n) row << ','; row << '[' << region.sourceOffsets[n].x << ',' << region.sourceOffsets[n].y << ',' << region.sourceOffsets[n].z << ']'; }
                    row << "],\"dst_offsets\":[";
                    for (unsigned n = 0; n < 2; ++n) { if (n) row << ','; row << '[' << region.destinationOffsets[n].x << ',' << region.destinationOffsets[n].y << ',' << region.destinationOffsets[n].z << ']'; }
                    row << "],\"extent\":[" << region.extent.width << ',' << region.extent.height << ',' << region.extent.depth << "]}";
                }
                row << "],\"region_drops\":" << event.regionDrops << ",\"secondary\":[";
                for (std::size_t n = 0; n < event.secondaryCount; ++n) {
                    if (n) row << ','; row << "{\"command\":"; handle(row, event.secondary[n].handle); row << ",\"rp_recording_generation\":" << event.secondary[n].generation << '}';
                }
                row << "],\"secondary_drops\":" << event.secondaryDrops
                    << ",\"pass_name\":\"unknown\",\"shader_semantics_verified\":false,\"graphics_sampling_complete\":false}"; emit(row.str());
                for (std::size_t n = 0; n < event.imageCount; ++n) {
                    const auto& i = event.images[n]; const auto& d = i.record.description;
                    std::ostringstream image;
                    image << "{\"event\":\"render_tail_image\",\"snapshot\":" << snapshot.serial << ",\"ordinal\":" << event.ordinal << ",\"image\":"; handle(image, i.record.image);
                    image << ",\"image_generation\":" << i.record.generation << ",\"view\":"; handle(image, i.view.handle);
                    image << ",\"view_generation\":" << i.view.generation << ",\"set\":"; handle(image, i.set.handle);
                    image << ",\"set_generation\":" << i.set.generation << ",\"descriptor_version\":" << i.descriptorVersion
                        << ",\"declared_role_mask\":" << i.role << ",\"set_index\":" << i.setIndex << ",\"binding\":" << i.binding
                        << ",\"array_element\":" << i.arrayElement << ",\"descriptor_type\":" << i.descriptorType
                        << ",\"format\":" << d.format << ",\"extent\":[" << d.width << ',' << d.height << ',' << d.depth
                        << "],\"mips\":" << d.mipLevels << ",\"layers\":" << d.arrayLayers << ",\"usage\":" << d.usage
                        << ",\"layout\":" << i.layout << ",\"range\":[" << i.range.aspectMask << ',' << i.range.baseMipLevel << ',' << i.range.levelCount << ',' << i.range.baseArrayLayer << ',' << i.range.layerCount
                        << "],\"issues\":" << i.issues << ",\"memory_queried\":" << (i.memory.queried ? "true" : "false") << ",\"memory\":"; handle(image, i.memory.binding.memory.handle);
                    image << ",\"memory_generation\":" << i.memory.binding.memory.generation << ",\"memory_offset\":" << i.memory.binding.offset
                        << ",\"memory_size\":" << i.memory.binding.size << ",\"memory_overlap_count\":" << i.memory.binding.overlapCount
                        << ",\"memory_bounds_known\":" << (i.memory.binding.bindingBoundsKnown ? "true" : "false")
                        << ",\"observed_alias_free\":" << (i.memory.binding.observedAliasFree ? "true" : "false")
                        << ",\"memory_local_issues\":" << i.memory.localIssues << ",\"memory_global_issues\":" << i.memory.globalIssues
                        << ",\"recorded_layout_query\":" << (i.layoutQueried?"true":"false") << ",\"recorded_layout_resolved\":" << (i.layoutResolved?"true":"false")
                        << ",\"recorded_prefix_supported\":" << (i.layoutPrefixSupported?"true":"false") << ",\"recorded_layout\":" << i.recordedLayout
                        << ",\"layout_prefix_order\":" << i.layoutPrefix << ",\"layout_issues\":" << i.layoutIssues
                        << ",\"last_layout_order\":" << i.layoutLastOrder << ",\"barrier_old_layout\":" << i.barrierOldLayout << ",\"barrier_new_layout\":" << i.barrierNewLayout
                        << ",\"src_stage\":" << i.srcStage << ",\"dst_stage\":" << i.dstStage << ",\"src_access\":" << i.srcAccess << ",\"dst_access\":" << i.dstAccess
                        << ",\"actual_shader_usage_known\":false,\"layout_verified\":false,\"gpu_complete\":false,\"copy_permitted\":false}"; emit(image.str());
                }
            }
        }
    }
};
Store::Store(std::uint64_t device, Sink sink) : p_(std::make_unique<Impl>(device, sink)) {}
Store::~Store() = default;
bool Store::active() const noexcept { try { std::lock_guard l(p_->mutex); return p_->active(); } catch (...) { return false; } }
void Store::arm() noexcept { try { std::lock_guard l(p_->mutex); if (!p_->stats.stopped) p_->stats.armed = true; } catch (...) {} }
void Store::source(const Source& source) noexcept { try { std::lock_guard l(p_->mutex); if (p_->active() && source.accepted) { p_->sources[p_->sourceCursor++ % p_->sources.size()] = source; ++p_->stats.sourceObservations; } } catch (...) {} }
void Store::wsiAdded(const wsi_image_inventory::OwnedImage& image) noexcept { try {
    std::lock_guard l(p_->mutex);
    for (auto& i : p_->wsi) if (!i.record.image || i.record.image == image.record.image) { i = image; return; }
    ++p_->stats.refused;
} catch (...) {} }
void Store::wsiRemoved(Key key) noexcept { try { std::lock_guard l(p_->mutex); for (auto& i : p_->wsi) if (i.record.image == key.handle && i.record.generation == key.generation) i = {}; } catch (...) {} }
void Store::begin(const CommandRecordingFacts& f, VkResult result) noexcept { if (result != VK_SUCCESS) return; try { std::lock_guard l(p_->mutex); if (auto* c = p_->command(f)){Impl::clear(*c,{f.command,f.rpRecordingGeneration});c->pool=f.pool;} } catch (...) {} }
void Store::end(const CommandRecordingFacts& f,VkResult result) noexcept {try{std::lock_guard l(p_->mutex);if(auto* c=p_->find({f.command,f.rpRecordingGeneration}))c->ended=result==VK_SUCCESS;}catch(...){}}
void Store::reset(std::uint64_t command) noexcept { try { std::lock_guard l(p_->mutex); for (auto& c : p_->commands) if (c.key.handle == command) Impl::clear(c); } catch (...) {} }
void Store::resetPool(std::uint64_t pool) noexcept {try{std::lock_guard l(p_->mutex);for(auto& c:p_->commands)if(c.pool==pool)Impl::clear(c);}catch(...){}}
void Store::record(Event event) noexcept { try { std::lock_guard l(p_->mutex); if (auto* c = p_->command(event.recording)) p_->append(*c, event); } catch (...) {} }
void Store::endPass(const CommandRecordingFacts& f) noexcept { try { std::lock_guard l(p_->mutex); if (auto* c = p_->command(f)) c->activePass = 0; } catch (...) {} }
void Store::bindPipeline(const CommandRecordingFacts& f, std::uint64_t pipeline) noexcept { try { std::lock_guard l(p_->mutex); if (auto* c = p_->command(f)) { c->graphics.pipeline = pipeline; c->graphics.known = true; } } catch (...) {} }
void Store::bindSets(const CommandRecordingFacts& f, std::uint64_t layout, std::uint32_t first, std::span<const std::uint64_t> sets) noexcept { try {
    std::lock_guard l(p_->mutex); if (auto* c = p_->command(f)) {
        if (first > c->graphics.sets.size() || sets.size() > c->graphics.sets.size() - first) { c->graphics.sets = {}; c->graphics.known = false; return; }
        if (c->graphics.layout != layout) c->graphics.sets = {};
        c->graphics.layout = layout; std::copy(sets.begin(), sets.end(), c->graphics.sets.begin() + first);
    }
} catch (...) {} }
GraphicsState Store::graphics(const CommandRecordingFacts& f) const noexcept { try { std::lock_guard l(p_->mutex); if (const auto* c = p_->find({f.command, f.rpRecordingGeneration})) return c->graphics; } catch (...) {} return {}; }
void Store::draw(const CommandRecordingFacts& f, std::span<const Image> images) noexcept { try {
    std::lock_guard l(p_->mutex); if (auto* c = p_->command(f)) if (auto* pass = p_->currentPass(*c)) {
        ++pass->drawCount;
        if (pass->pipeline != c->graphics.pipeline) { ++pass->pipelineChanges; pass->pipeline = c->graphics.pipeline; }
        for (const auto& image : images) {
            bool found = false;
            for (std::size_t n = 0; n < pass->imageCount; ++n) { const auto& old = pass->images[n]; if (old.record.image == image.record.image && old.record.generation == image.record.generation && old.view == image.view && old.set == image.set && old.descriptorVersion == image.descriptorVersion && old.binding == image.binding && old.arrayElement == image.arrayElement && old.role == image.role) { found = true; break; } }
            if (found) continue;
            if (pass->imageCount < imageLimit) pass->images[pass->imageCount++] = image; else ++pass->imageDrops;
        }
        for(std::size_t n=0;n<pass->imageCount;++n)if(pass->images[n].role&Attachment) {
            const Key key{pass->images[n].record.image,pass->images[n].record.generation};
            if(const auto* owned=p_->owner(key)) {
                c->hasTarget=true;c->target=key;c->owner=*owned;c->targetOrdinal=pass->ordinal;c->source={};
                for(std::size_t j=0;j<pass->imageCount;++j)if(pass->images[j].role&Sampled) {
                    const Key source{pass->images[j].record.image,pass->images[j].record.generation};
                    if(c->source.handle&&c->source!=source){c->source={};break;}c->source=source;
                }
            }
        }
    }
} catch (...) {} }
Ticket Store::beforeSubmit(std::uint64_t queue, std::uint64_t token, std::span<const Key> keys) noexcept { try {
    std::lock_guard l(p_->mutex); if (!p_->active() || p_->stats.emittedSamples >= 2) return {};
    Impl::Frozen* frozen = nullptr; for (auto& f : p_->pending) if (!f.serial) { frozen = &f; break; }
    if (!frozen || p_->serial == UINT64_MAX) { ++p_->stats.refused; return {}; }
    Impl::Command* target = nullptr;
    for (auto key : keys) if (auto* c = p_->find(key); c && c->hasTarget && p_->owner(c->target)) { target = c; break; }
    if (!target) return {};
    const Event* copy = nullptr; for (const auto& e : target->events) if (e.ordinal&&e.ordinal==target->targetOrdinal) copy=&e;
    Source source{}; if (p_->sourceCursor) source = p_->sources[(p_->sourceCursor - 1) % p_->sources.size()];
    if (copy) for (const auto& candidate : p_->sources) if (sameFrame(copy->frame, candidate.frame)) { source = candidate; break; }
    const bool known = copy && source.accepted && sameFrame(copy->frame, source.frame);
    if (known) for (const auto& seen : p_->seen) if (sameFrame(seen, source.frame)) return {};
    if (known) for (const auto& pending : p_->pending) if (pending.serial&&pending.frameKnown&&sameFrame(pending.source.frame,source.frame)) return {};
    frozen->count=0;frozen->relatedDrops=0;frozen->emitting=false;frozen->sameSubmit={};
    frozen->serial = ++p_->serial; frozen->queue = queue; frozen->token = token;
    frozen->source = source; frozen->frameKnown = known; frozen->commands[0] = *target; frozen->sameSubmit[0] = true; frozen->count = 1;
    std::array<Key,imageLimit> sourceImages{};std::size_t sourceCount=0;
    if(copy&&copy->kind==Kind::Pass) {
        for(std::size_t n=0;n<copy->imageCount;++n)if(copy->images[n].role&Sampled)
            sourceImages[sourceCount++]={copy->images[n].record.image,copy->images[n].record.generation};
    } else if(target->source.handle)sourceImages[sourceCount++]=target->source;
    for (unsigned phase = 0; phase < 2; ++phase) for (const auto& c : p_->commands) {
        bool writesSource=false;for(std::size_t n=0;n<sourceCount;++n)writesSource|=Impl::writes(c,sourceImages[n]);
        if (c.key == target->key || !writesSource) continue;
        const bool same = std::find(keys.begin(), keys.end(), c.key) != keys.end();
        if (same != (phase == 0)) continue;
        if (frozen->count == frozen->commands.size()) { ++frozen->relatedDrops; continue; }
        frozen->commands[frozen->count] = c; frozen->sameSubmit[frozen->count++] = same;
    }
    ++p_->stats.submittedTargets; return {frozen->serial};
} catch (...) { return {}; } }
void Store::submitted(std::uint64_t queue,std::uint64_t token,std::span<const Key> keys,VkResult result) noexcept {
    try{std::lock_guard l(p_->mutex);for(auto key:keys)if(auto* c=p_->find(key)){c->queue=queue;c->submitToken=token;c->submitResult=result;}}catch(...){}
}
void Store::afterSubmit(Ticket ticket, VkResult result) noexcept { try {
    Impl::Frozen* frozen = nullptr;
    {
        std::lock_guard l(p_->mutex);
        for (auto& f : p_->pending) if (ticket.serial && f.serial == ticket.serial && !f.emitting) { frozen = &f; break; }
        if (!frozen) return;
        if (result != VK_SUCCESS) { ++p_->stats.failedTargets; frozen->serial = 0; return; }
        if (p_->stats.emittedSamples >= 2) { frozen->serial = 0; return; }
        if(frozen->frameKnown)for(const auto& seen:p_->seen)if(sameFrame(seen,frozen->source.frame)){frozen->serial=0;return;}
        frozen->emitting = true;
        if (frozen->frameKnown) p_->seen[p_->stats.emittedSamples] = frozen->source.frame;
        ++p_->stats.emittedSamples;
        if (auto* c = p_->find(frozen->commands[0].key)) { c->hasTarget = false; c->queue = frozen->queue; c->submitToken = frozen->token; c->submitResult = result; }
        if (p_->stats.emittedSamples == 2) p_->stats.stopped = true;
    }
    p_->output(*frozen, result);
    { std::lock_guard l(p_->mutex); frozen->serial = 0; frozen->emitting = false; }
} catch (...) {} }
void Store::present() noexcept { try { std::lock_guard l(p_->mutex); if (p_->active() && ++p_->stats.presentAttempts >= 120) p_->stats.stopped = true; } catch (...) {} }
Stats Store::stats() const noexcept { try { std::lock_guard l(p_->mutex); return p_->stats; } catch (...) { return {}; } }
std::size_t Store::storageBytes() noexcept { return sizeof(Impl); }
} // namespace xess_vk::render_tail
