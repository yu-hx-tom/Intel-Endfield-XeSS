#include "sr_observation.hpp"
#include "upscale_frame_constants.hpp"
#include <windows.h>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <locale>
#include <mutex>
#include <sstream>

extern "C" {void* sr_accumulate_registration_original{};void* sr_rcas_registration_original{};
#define SR_NATIVE_PASS(name, pass) void* sr_##name##_registration_original{};
#include "sr_native_passes.inc"
#undef SR_NATIVE_PASS
}
namespace xess_vk::sr_observation {
namespace {
namespace p=upscale_tail_probe;
namespace rb=registration_bridge;
struct ErrorGuard {DWORD value=GetLastError();~ErrorGuard(){SetLastError(value);}};
struct Registration {
    std::uint64_t serial{},window{};std::uintptr_t source{},copy{},graph{};std::uint32_t thread{},node{};p::Pass pass{};
    std::array<unsigned char,p::maxPayloadBytes> bytes{};FrameTimingAssociation frame;
    bool constructing{},published{};
};
struct Candidate {
    std::uint64_t callback{},registration{},hits{},window{};std::uint32_t callbackThread{},registrationThread{};
    p::Pass pass{};FrameTimingAssociation frame;
    std::array<descriptor_image_metadata::Key,p::maxBindings> images{};
    std::array<p::Property,p::maxBindings> properties{};
    std::array<std::uint64_t,p::maxBindings> propertyAddresses{};
    std::array<std::uint32_t,3> groups{};std::size_t count{};std::uint32_t optionalMask{},omittedMask{};bool live{};
    VkExtent2D renderExtent{},outputExtent{};bool extentsKnown{};frame_input::FrameConstantsSample constants;
};
enum class DispatchGate : std::size_t {
    FrameUnknown,RecordingInvalid,DescriptorFiltered,DescriptorRejected,BindingsInvalid,
    NoCandidate,CandidateFrameMismatch,GroupsMismatch,ResourceMismatch,Matched,Limit,Count
};
constexpr std::array<const char*,std::size_t(DispatchGate::Count)> dispatchGateNames{
    "frame_unknown","recording_invalid","descriptor_filtered","descriptor_rejected","bindings_invalid",
    "no_candidate","candidate_frame_mismatch","groups_mismatch","resource_mismatch","matched","dispatch_limit"};
struct DispatchDiagnostics {
    std::array<std::uint64_t,std::size_t(DispatchGate::Count)> gates{};
    std::array<std::uint64_t,64> descriptorIssues{};
    struct Signature {p::Pass pass{};std::uint32_t missing{},mismatched{},unmapped{};std::uint64_t evaluations{},qualified{};};
    std::array<Signature,resourceSignatureHistogramLimit> signatures{};
    std::uint64_t candidateEvaluations{},qualifiedEvaluations{},overflowEvaluations{},overflowQualified{};
};
struct SignatureProperty {std::uint64_t rva;const char* name;};
// Names from the pinned Unity property constructors, not inferred shader use.
constexpr std::array<SignatureProperty,13> signatureProperties{{
    {0x1EDE540,"r_input_color_jittered"},{0x1EDE544,"r_current_luma"},{0x1EDE54C,"r_dilated_motion_vectors"},
    {0x1EDE550,"r_input_exposure"},{0x1EDE574,"r_dilated_reactive_masks"},{0x1EDE580,"r_farthest_depth_mip1"},
    {0x1EDE584,"r_internal_upscaled_color"},{0x1EDE588,"r_lanczos_lut"},{0x1EDE58C,"r_luma_instability"},
    {0x1EDE590,"r_rcas_input"},{0x1EDE5D0,"rw_new_locks"},{0x1EDE5E4,"rw_internal_upscaled_color"},{0x1EDE5E8,"rw_upscaled_output"}}};
struct NearMatch {
    Candidate candidate;CommandRecordingFacts recording;descriptor_image_metadata::DispatchEvidence evidence;
    FrameTimingAssociation frame;std::uint32_t thread{},missingMask{},mismatchedMask{},unmapped{};std::size_t matchedBindings{};bool live{};
};
struct RawPair {
    CommandRecordingFacts recording;
    FrameTimingAssociation frame;
    std::array<descriptor_image_metadata::ImageBinding,2> images{};
    descriptor_image_metadata::Key pipeline;
    bool live{};
};
struct DepthIntervals {
    std::uint64_t matched{},missing{},ambiguous{},queried{},untouched{},unknown{},barriers{},attachments{},transfers{},clears{};
};
DepthIntervals depthIntervals;
struct Following {
    CommandRecordingFacts recording;
    descriptor_image_metadata::Key output;
    std::uint64_t callback{};
    unsigned dispatches{};
    bool live{};
};
std::array<Following,16> following;
std::array<FrameTimingAssociation,16> rcasFrames;
std::array<Stage1Source,16> stage1Sources;
bool runtimeMode{},fullGraphMode{};
std::array<RawPair,16> rawPairs;
std::uint64_t rawPairSerial{};
std::mutex mutex;
std::array<Registration,128> registrations;
std::array<Candidate,64> candidates;
auto registrationSlots() noexcept {return std::span(registrations).first(fullGraphMode?128:32);}
auto candidateSlots() noexcept {return std::span(candidates).first(fullGraphMode?64:16);}
DispatchDiagnostics dispatchDiagnostics;
sr_dispatch_shadow::Audit dispatchShadow;
sr_consumers::Audit consumerAudit;
std::array<NearMatch,4> nearMatches;
std::atomic<bool> configured{},sampling{},stopped{},ready{};
std::atomic<bool> consumerFinished{};
std::atomic<std::uint64_t> window{},deadline{},nextPoll{};
struct FileIdentity {std::uint64_t written{},bytes{};friend bool operator==(const FileIdentity&,const FileIdentity&)=default;};
FileIdentity lastFile;bool fileSeen{},captureBaselined{};
std::atomic<std::uint64_t> calls{},parseAttempts{},detailSamples{},failures{},registrationRefusals{},evicted{},matches{},omittedMatches{},forwarded{};
std::atomic<std::uint64_t> registrationCalls{},dispatchCalls{};
std::array<std::atomic<std::uint64_t>,unsigned(p::Pass::Count)-1> passCalls{};
std::uintptr_t module{};std::uint64_t device{},registrationSerial{};
Sink sink{};FrameReader frames{};void* frameUser{};ResolveImages resolveImages{};
rb::Control control;
thread_local bool inside{},insideSink{};
bool gate() noexcept {return sampling.load(std::memory_order_acquire)&&!stopped.load(std::memory_order_acquire);}
bool gate(std::uint64_t admitted) noexcept {return admitted&&gate()&&window.load(std::memory_order_acquire)==admitted;}
bool bump(std::uint64_t admitted,std::atomic<std::uint64_t>& counter,bool requireActive=true) noexcept {
    std::lock_guard lock(mutex);if(window.load()!=admitted||(requireActive&&!gate(admitted)))return false;++counter;return true;
}
std::size_t readMemory(void* context,std::uint64_t address,void* output,std::size_t size,std::size_t budget) {
    if(!context||!active(device)||!gate(*static_cast<const std::uint64_t*>(context))||!address||size>budget||size>UINTPTR_MAX-address)return 0;
    SIZE_T copied{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),output,size,&copied)?copied:0;
}
fsr_probe::Reader reader(std::uint64_t* admitted) noexcept {
    return {readMemory,admitted};
}
bool readPayload(std::uint64_t admitted,std::uint64_t address,void* output,std::size_t bytes) noexcept {
    if(!gate(admitted))return false;try{const auto r=reader(&admitted);return r.read(r.user,address,output,bytes,bytes)==bytes;}catch(...){return false;}
}
bool sameFrame(const FrameTimingAssociation& a,const FrameTimingAssociation& b) noexcept {
    return a.valid&&b.valid&&a.ticket&&a.engineFrame==b.engineFrame&&a.ticket==b.ticket;
}
void emit(const std::string& line,std::uint64_t admitted,bool final=false) noexcept {
    if((!final&&!gate(admitted))||!sink||insideSink)return;insideSink=true;
    try{sink(line.c_str());}catch(...){bump(admitted,failures,false);}insideSink=false;
}
struct ClosedWindow {
    std::uint64_t id{},calls{},parseAttempts{},detailSamples{},matches{},forwarded{},dispatches{};const char* reason{};
    DispatchDiagnostics diagnostics;std::array<NearMatch,4> nearSamples;
    sr_dispatch_shadow::Audit shadow;
    sr_consumers::Audit consumers;
    bool fullGraph{};
};
ClosedWindow closeLocked(const char* reason) noexcept {
    if(!gate())return {};ClosedWindow closed{window.load(),calls.load(),parseAttempts.load(),detailSamples.load(),matches.load(),forwarded.load(),dispatchCalls.load(),reason,dispatchDiagnostics,nearMatches};
    closed.shadow=dispatchShadow;closed.consumers=consumerAudit;closed.fullGraph=fullGraphMode&&!runtimeMode;
    sampling.store(false,std::memory_order_release);control.sampling=false;registrations={};candidates={};return closed;
}
void emitDispatchDiagnostics(const ClosedWindow&) noexcept;
void emitDispatchShadow(const ClosedWindow&) noexcept;
void emitConsumers(const ClosedWindow&) noexcept;
void emitClosed(const ClosedWindow& closed) noexcept {
    if(!closed.id)return;emitDispatchDiagnostics(closed);emitDispatchShadow(closed);emitConsumers(closed);try{std::ostringstream out;out.imbue(std::locale::classic());out<<"{\"event\":\"sr_observation_window_end\",\"window_id\":"<<closed.id<<",\"reason\":\""<<closed.reason
        <<"\",\"callback_calls\":"<<closed.calls<<",\"callback_parse_attempts\":"<<closed.parseAttempts<<",\"callback_detail_samples\":"<<closed.detailSamples<<",\"dispatch_candidate_matches\":"<<closed.matches<<",\"driver_returns\":"<<closed.forwarded
        <<",\"execute_permitted\":false}";emit(out.str(),closed.id,true);}catch(...){bump(closed.id,failures,false);}
}
void closeWindow(std::uint64_t admitted,const char* reason) noexcept {
    ClosedWindow closed;{std::lock_guard lock(mutex);if(window.load()==admitted)closed=closeLocked(reason);}emitClosed(closed);
}
void beginWindow(FileIdentity identity,std::uint64_t now) noexcept {
    ClosedWindow closed;std::uint64_t admitted{};
    {std::lock_guard lock(mutex);if(!ready.load()||stopped.load()||(fileSeen&&lastFile==identity)||window.load()==UINT64_MAX)return;
        closed=closeLocked("capture_replaced");registrations={};candidates={};dispatchDiagnostics={};dispatchShadow={};consumerAudit={};nearMatches={};rawPairs={};rawPairSerial=0;depthIntervals={};following={};rcasFrames={};stage1Sources={};
        calls=0;parseAttempts=0;detailSamples=0;failures=0;registrationRefusals=0;evicted=0;matches=0;omittedMatches=0;forwarded=0;registrationCalls=0;dispatchCalls=0;consumerFinished=false;
        for(auto& count:passCalls)count=0;lastFile=identity;fileSeen=true;admitted=window.load()+1;
        deadline.store(now+captureMilliseconds);window.store(admitted,std::memory_order_release);sampling.store(true,std::memory_order_release);control.sampling=true;
    }
    emitClosed(closed);
    try{std::ostringstream out;out.imbue(std::locale::classic());out<<"{\"event\":\"sr_observation_window_begin\",\"window_id\":"<<admitted<<",\"file_last_write\":"<<identity.written<<",\"file_bytes\":"<<identity.bytes
        <<",\"duration_limit_ms\":"<<captureMilliseconds<<",\"callback_limit\":"<<callbackLimit<<",\"registration_limit\":"<<registrationLimit
        <<",\"dispatch_observation_limit\":"<<dispatchObservationLimit<<",\"dispatch_detail_limit\":"<<dispatchDetailLimit
        <<",\"callback_parse_scope\":\"all_callbacks_within_limit\",\"callback_detail_policy\":\"first_8_then_every_32_per_pass\",\"callback_detail_samples_are_selected_callbacks\":true,\"execute_permitted\":false}";emit(out.str(),admitted);}catch(...){bump(admitted,failures);}
}
void inspectCapture(bool exists,FileIdentity identity,std::uint64_t now) noexcept {
    {std::lock_guard lock(mutex);if(!ready.load()||stopped.load())return;
        if(!captureBaselined){captureBaselined=true;fileSeen=exists;if(exists)lastFile=identity;return;}}
    if(exists)beginWindow(identity,now);
}
void frameFields(std::ostream& out,const FrameTimingAssociation& frame) {
    out<<"\"frame_known\":"<<(frame.valid&&frame.ticket?"true":"false")<<",\"engine_frame\":";
    if(frame.valid&&frame.ticket)out<<frame.engineFrame;else out<<"null";
    out<<",\"sdk_present_id\":";if(frame.valid&&frame.ticket)out<<frame.ticket.sdkId;else out<<"null";
    out<<",\"ticket_generation\":";if(frame.valid&&frame.ticket)out<<frame.ticket.contextGeneration;else out<<"null";
}
void emitDispatchShadow(const ClosedWindow& closed) noexcept {
    if(!closed.fullGraph)return;
    try {
        const auto stats=closed.shadow.stats();std::ostringstream summary;summary.imbue(std::locale::classic());
        summary<<"{\"event\":\"sr_dispatch_shadow_summary\",\"window_id\":"<<closed.id<<",\"frames\":"<<closed.shadow.count()
            <<",\"frame_limit\":"<<sr_dispatch_shadow::frameLimit<<",\"dispatches_per_frame_limit\":"<<sr_dispatch_shadow::dispatchLimit
            <<",\"forwarded\":"<<stats.forwarded<<",\"invalid\":"<<stats.invalid<<",\"duplicates\":"<<stats.duplicates
            <<",\"frame_limit_drops\":"<<stats.frameLimitDrops<<",\"dispatch_limit_drops\":"<<stats.dispatchLimitDrops
            <<",\"prepare_mask\":"<<sr_dispatch_shadow::prepareBit<<",\"pass_masks\":{";
        for(unsigned n=1;n<unsigned(p::Pass::Count);++n){if(n!=1)summary<<',';summary<<'"'<<p::passName(p::Pass(n))<<"\":"<<sr_dispatch_shadow::passBit(p::Pass(n));}
        summary<<"},\"coverage\":\"candidate_driver_returns_only\",\"external_consumer_count\":null,\"consumer_coverage_complete\":false,\"execute_permitted\":false}";
        emit(summary.str(),closed.id,true);
        for(std::size_t n=0;n<closed.shadow.count();++n){const auto& frame=closed.shadow.frame(n);std::ostringstream out;out.imbue(std::locale::classic());
            out<<"{\"event\":\"sr_dispatch_shadow_frame\",\"window_id\":"<<closed.id<<',';frameFields(out,frame.identity);
            out<<",\"dropped_dispatches\":"<<frame.dropped<<",\"multiple_recordings\":"<<(frame.multipleRecordings?"true":"false")
                <<",\"non_increasing_record_order\":"<<(frame.nonIncreasingOrder?"true":"false")<<",\"conflicting_duplicate\":"<<(frame.conflictingDuplicate?"true":"false")
                <<",\"first_actual_fsr_dispatch_proven\":false,\"frame_complete\":false,\"sdk_ready\":null,\"replacement_whitelist_verified\":false,\"would_replace\":false,\"dispatches\":[";
            for(std::size_t d=0;d<frame.count;++d){const auto& entry=frame.dispatches[d];if(d)out<<',';
                out<<"{\"command\":"<<entry.command<<",\"recording\":"<<entry.recording<<",\"incarnation\":"<<entry.incarnation<<",\"order\":"<<entry.order
                    <<",\"pipeline\":"<<entry.pipeline<<",\"pipeline_generation\":"<<entry.pipelineGeneration<<",\"api\":"<<unsigned(entry.api)
                    <<",\"resource_exact_mask\":"<<entry.exactMask<<",\"partial_candidate_mask\":"<<entry.candidateMask<<",\"registered_callback_mask_at_dispatch\":"<<entry.registeredMask
                    <<",\"raw_pair_available\":"<<(entry.rawPairAvailable?"true":"false")<<",\"stage1_source_available\":"<<(entry.stage1SourceAvailable?"true":"false")
                    <<",\"shader_fingerprint_known\":"<<(entry.fingerprintKnown?"true":"false")
                    <<",\"native_policy\":\""<<sr_native_shaders::policyName(entry.policy)<<'"'
                    <<",\"blit_formats_queried\":"<<(entry.blitFormatsQueried?"true":"false")<<",\"blit_static_checks\":"<<entry.blitChecks
                    <<",\"source_format_features\":"<<entry.sourceFormatFeatures<<",\"destination_format_features\":"<<entry.destinationFormatFeatures
                    <<",\"blit_sync_verified\":false,\"driver_forwarded\":true}";
            }
            out<<"],\"external_consumer_count\":null,\"consumer_coverage_complete\":false,\"gpu_complete\":false,\"execute_permitted\":false}";emit(out.str(),closed.id,true);
        }
    }catch(...){bump(closed.id,failures,false);}
}
void emitConsumers(const ClosedWindow& closed) noexcept {
    if(!closed.fullGraph)return;
    try {
        const auto& audit=closed.consumers;const auto s=audit.stats();std::ostringstream out;out.imbue(std::locale::classic());
        out<<"{\"event\":\"sr_consumer_audit\",\"window_id\":"<<closed.id<<",\"commands\":"<<s.commands
            <<",\"reference_site_limit\":"<<sr_consumers::commandLimit<<",\"budget_counts_reference_sites_only\":true"
            <<",\"pre_target_reference_sites\":"<<s.preTargetReferenceSites<<",\"state_commands\":"<<s.stateCommands
            <<",\"incomplete_state_commands\":"<<s.incompleteStateCommands
            <<",\"consumer_shader_requests\":"<<s.shaderRequests<<",\"consumer_shader_request_drops\":"<<s.shaderRequestDrops
            <<",\"references\":"<<s.references<<",\"unknown_commands\":"<<s.unknownCommands<<",\"unknown_memory\":"<<s.unknownMemory
            <<",\"invalid_identities\":"<<s.invalidIdentities<<",\"output_drops\":"<<s.outputDrops<<",\"hit_sample_drops\":"<<s.hitDrops
            <<",\"command_drops\":"<<s.commandDrops<<",\"graphics_recording_drops\":"<<s.graphicsDrops<<",\"producer_frame_drops\":"<<s.frameDrops
            <<",\"ended_recordings\":"<<s.endedRecordings<<",\"graphics_draws_bound_sets_known\":"<<s.graphicsDrawsKnown
            <<",\"graphics_draws_bound_sets_unknown\":"<<s.graphicsDrawsUnknown<<",\"opaque_command_name_drops\":"<<s.opaqueNameDrops<<",\"producer_profiles\":[";
        for(std::size_t n=0;n<sr_native_shaders::profiles.size();++n){if(n)out<<',';const auto& profile=sr_native_shaders::profiles[n];
            out<<"{\"index\":"<<n<<",\"sha256\":\""<<profile.sha256<<"\",\"policy\":\""<<sr_native_shaders::policyName(profile.policy)<<"\"}";}
        const auto& boundary=audit.boundary();
        out<<"],\"producer_frames_recorded\":"<<audit.producerFrames().size()<<",\"producer_frame_limit\":"<<sr_consumers::frameLimit
            <<",\"scope\":\"producer_frames_and_successor_prefix\",\"scope_closed\":"<<(audit.closed()?"true":"false")
            <<",\"boundary_command\":"<<boundary.recording.command<<",\"boundary_recording\":"<<boundary.recording.recording
            <<",\"boundary_order\":"<<boundary.order<<",\"boundary_engine_frame\":"<<boundary.frame.engineFrame<<",\"producer_recordings\":[";
        bool first=true;for(const auto& f:audit.producerFrames()){if(!first)out<<',';first=false;
            out<<"{\"engine_frame\":"<<f.identity.engineFrame<<",\"sdk_id\":"<<f.identity.ticket.sdkId<<",\"ticket_context\":"<<f.identity.ticket.contextGeneration
                <<",\"command\":"<<f.recording.command<<",\"recording\":"<<f.recording.recording
                <<",\"end_observed\":"<<(f.ended?"true":"false")<<",\"multiple_recordings\":"<<(f.multipleRecordings?"true":"false")<<'}';}
        out<<"],\"outputs_recorded\":"<<audit.outputs().size()<<",\"hit_samples_recorded\":"<<audit.hits().size()
            <<",\"reference_semantics\":\"potential_use_not_verified_shader_read\",\"external_consumer_count\":null,\"consumer_coverage_complete\":false,"
            "\"future_frame_coverage_complete\":false,\"graphics_binding_policy\":\"all_bound_sets_superset_from_observed_begin\",\"buffer_dependencies_verified\":false,\"execute_permitted\":false}";
        emit(out.str(),closed.id,true);
        // Each bounded record stays below the existing 32 KiB sink limit.
        for(const auto& o:audit.outputs()){std::ostringstream line;line.imbue(std::locale::classic());
            line<<"{\"event\":\"sr_consumer_output\",\"window_id\":"<<closed.id<<",\"image\":"<<o.image.key.handle<<",\"generation\":"<<o.image.key.generation<<",\"producer_profile_mask\":"<<o.producers
                <<",\"first_command\":"<<o.recording.command<<",\"first_recording\":"<<o.recording.recording<<",\"writes\":"<<o.writes
                <<",\"latest_observed_write_command\":"<<o.latest.recording.command<<",\"latest_observed_write_recording\":"<<o.latest.recording.recording
                <<",\"latest_observed_write_order\":"<<o.latest.order<<",\"latest_observed_write_engine_frame\":"<<o.latest.frame.engineFrame
                <<",\"observed_write_sites\":"<<o.latest.site<<",\"producer_observation_is_gpu_dominance\":false"
                <<",\"internal_references\":"<<o.internalReferences<<",\"potential_external_references\":"<<o.externalReferences
                <<",\"alias_references\":"<<o.aliasReferences<<",\"cross_recording_references\":"<<o.crossRecordingReferences
                <<",\"known_native_cross_recording_references\":"<<o.nativeCrossReferences
                <<",\"memory_queried\":"<<(o.image.memory.queried?"true":"false")<<",\"memory_global_issues\":"<<o.image.memory.globalIssues
                <<",\"memory_local_issues\":"<<o.image.memory.localIssues<<",\"observed_alias_free\":"<<(o.image.memory.binding.observedAliasFree?"true":"false")
                <<",\"execute_permitted\":false}";emit(line.str(),closed.id,true);}
        for(const auto& h:audit.hits()){std::ostringstream line;line.imbue(std::locale::classic());
            line<<"{\"event\":\"sr_consumer_reference\",\"window_id\":"<<closed.id<<",\"output_image\":"<<h.image.handle<<",\"output_generation\":"<<h.image.generation<<",\"reference_image\":"<<h.reference.handle
                <<",\"reference_generation\":"<<h.reference.generation<<",\"kind\":"<<unsigned(h.kind)<<",\"command\":"<<h.recording.command
                <<",\"recording\":"<<h.recording.recording<<",\"sequence\":"<<h.sequence<<",\"record_order\":"<<h.recordOrder
                <<",\"consumer_pipeline\":"<<h.pipeline.handle<<",\"consumer_pipeline_generation\":"<<h.pipeline.generation
                <<",\"set_index\":"<<h.setIndex<<",\"binding\":"<<h.binding<<",\"descriptor_type\":"<<h.descriptorType
                <<",\"producer_command\":"<<h.producer.recording.command<<",\"producer_recording\":"<<h.producer.recording.recording
                <<",\"producer_order\":"<<h.producer.order<<",\"producer_engine_frame\":"<<h.producer.frame.engineFrame
                <<",\"producer_write_site\":"<<h.producer.site<<",\"producer_observation_is_gpu_dominance\":false"
                <<",\"known_native_consumer\":"<<(h.knownNativeConsumer?"true":"false")<<",\"alias\":"<<(h.alias?"true":"false")
                <<",\"cross_recording\":"<<(h.crossRecording?"true":"false")<<",\"execute_permitted\":false}";emit(line.str(),closed.id,true);}
        for(const auto& entry:audit.opaqueCommands()){std::ostringstream line;line.imbue(std::locale::classic());
            line<<"{\"event\":\"sr_consumer_opaque_command\",\"window_id\":"<<closed.id<<",\"name\":\""<<entry.name.data()<<"\",\"count\":"<<entry.count
                <<",\"first_command\":"<<entry.firstRecording.command<<",\"first_recording\":"<<entry.firstRecording.recording
                <<",\"first_record_order\":"<<entry.firstOrder<<",\"execute_permitted\":false}";emit(line.str(),closed.id,true);}
    }catch(...){bump(closed.id,failures,false);}
}
void signaturePropertyFields(std::ostream& out,std::uint32_t mask) {
    out<<'[';bool first=true;for(std::size_t n=0;n<signatureProperties.size();++n)if(mask&(1u<<n)){
        if(!first)out<<',';first=false;out<<"{\"property\":\""<<signatureProperties[n].name<<"\",\"property_rva\":"<<signatureProperties[n].rva<<'}';}
    out<<']';
}
void rememberSignature(const Candidate& c,std::uint32_t missing,std::uint32_t mismatched,std::uint32_t unmapped,bool qualified) noexcept {
    std::uint32_t missingProperties{},mismatchedProperties{};
    for(std::size_t n=0;n<c.count;++n)for(std::size_t p=0;p<signatureProperties.size();++p)if(c.propertyAddresses[n]==module+signatureProperties[p].rva){
        if(missing&(1u<<n))missingProperties|=1u<<p;if(mismatched&(1u<<n))mismatchedProperties|=1u<<p;break;}
    ++dispatchDiagnostics.candidateEvaluations;if(qualified)++dispatchDiagnostics.qualifiedEvaluations;
    for(auto& entry:dispatchDiagnostics.signatures)if(!entry.evaluations||(entry.pass==c.pass&&entry.missing==missingProperties&&entry.mismatched==mismatchedProperties&&entry.unmapped==unmapped)){
        if(!entry.evaluations){entry.pass=c.pass;entry.missing=missingProperties;entry.mismatched=mismatchedProperties;entry.unmapped=unmapped;}
        ++entry.evaluations;if(qualified)++entry.qualified;return;}
    ++dispatchDiagnostics.overflowEvaluations;if(qualified)++dispatchDiagnostics.overflowQualified;
}
void emitDispatchDiagnostics(const ClosedWindow& closed) noexcept {
    try {
        std::ostringstream out;out.imbue(std::locale::classic());out<<"{\"event\":\"sr_dispatch_diagnostics\",\"window_id\":"<<closed.id
            <<",\"dispatch_calls\":"<<closed.dispatches<<",\"gate_counts_exclusive\":true,\"gate_counts\":{";
        for(std::size_t n=0;n<dispatchGateNames.size();++n){if(n)out<<',';out<<'"'<<dispatchGateNames[n]<<"\":"<<closed.diagnostics.gates[n];}
        out<<"},\"descriptor_issue_records\":[";bool first=true;
        for(std::size_t n=0;n<closed.diagnostics.descriptorIssues.size();++n)if(closed.diagnostics.descriptorIssues[n]){
            if(!first)out<<',';first=false;out<<"{\"issue_bit\":"<<(std::uint64_t(1)<<n)<<",\"dispatches\":"<<closed.diagnostics.descriptorIssues[n]<<'}';}
        out<<"],\"near_match_limit\":"<<closed.nearSamples.size()<<",\"resource_signature_histogram_limit\":"<<closed.diagnostics.signatures.size()
            <<",\"same_frame_groups_candidate_evaluations\":"<<closed.diagnostics.candidateEvaluations<<",\"resource_qualified_candidate_evaluations\":"<<closed.diagnostics.qualifiedEvaluations
            <<",\"histogram_overflow_evaluations\":"<<closed.diagnostics.overflowEvaluations<<",\"histogram_overflow_qualified\":"<<closed.diagnostics.overflowQualified
            <<",\"histogram_counts_are_dispatch_counts\":false,\"histogram_scope\":\"metadata_accepted_same_frame_groups_candidates\",\"execute_permitted\":false}";emit(out.str(),closed.id,true);
        for(const auto& entry:closed.diagnostics.signatures)if(entry.evaluations){std::ostringstream row;row.imbue(std::locale::classic());
            row<<"{\"event\":\"sr_dispatch_resource_signature_histogram\",\"window_id\":"<<closed.id<<",\"pass\":\""<<p::passName(entry.pass)
                <<"\",\"candidate_evaluations\":"<<entry.evaluations<<",\"resource_qualified_evaluations\":"<<entry.qualified
                <<",\"resource_refused_evaluations\":"<<(entry.evaluations-entry.qualified)<<",\"missing_properties\":";signaturePropertyFields(row,entry.missing);
            row<<",\"mismatched_properties\":";signaturePropertyFields(row,entry.mismatched);row<<",\"unmapped_image_bindings\":"<<entry.unmapped
                <<",\"shader_usage_verified\":false,\"execute_permitted\":false}";emit(row.str(),closed.id,true);}
        for(const auto& sample:closed.nearSamples)if(sample.live){const auto& c=sample.candidate;const auto& evidence=sample.evidence;
            std::ostringstream line;line.imbue(std::locale::classic());line<<"{\"event\":\"sr_dispatch_near_match\",\"window_id\":"<<closed.id
                <<",\"pass\":\""<<p::passName(c.pass)<<"\",\"callback\":"<<c.callback<<",\"registration\":"<<c.registration
                <<",\"dispatch_thread\":"<<sample.thread<<",\"command\":"<<sample.recording.command<<",\"recording_generation\":"<<sample.recording.rpRecordingGeneration
                <<",\"context_incarnation\":"<<sample.recording.contextIncarnation<<",\"record_ordinal\":"<<evidence.recordOrdinal
                <<",\"pipeline\":"<<evidence.pipeline.handle<<",\"pipeline_generation\":"<<evidence.pipeline.generation
                <<",\"pipeline_layout\":"<<evidence.pipelineLayout.handle<<",\"pipeline_layout_generation\":"<<evidence.pipelineLayout.generation<<',';
            frameFields(line,sample.frame);line<<",\"groups\":["<<evidence.groups[0]<<','<<evidence.groups[1]<<','<<evidence.groups[2]
                <<"],\"owner_ticket_match\":true,\"groups_match\":true,\"callback_binding_count\":"<<c.count
                <<",\"required_callback_bindings\":"<<(c.count-std::popcount(c.optionalMask))
                <<",\"matching_required_callback_bindings\":"<<(c.count-std::popcount(c.optionalMask)-std::popcount((sample.missingMask|sample.mismatchedMask)&~c.optionalMask))
                <<",\"matching_callback_bindings\":"<<sample.matchedBindings<<",\"missing_callback_bindings\":[";bool missingFirst=true;
            for(std::size_t n=0;n<c.count;++n)if(sample.missingMask&(1u<<n)){
                if(!missingFirst)line<<',';missingFirst=false;line<<"{\"property\":\""<<p::propertyName(c.properties[n])<<"\",\"property_address\":"<<c.propertyAddresses[n]
                    <<",\"image\":"<<c.images[n].handle<<",\"generation\":"<<c.images[n].generation<<'}';}
            line<<"],\"mismatched_callback_bindings\":[";bool mismatchFirst=true;
            for(std::size_t n=0;n<c.count;++n)if(sample.mismatchedMask&(1u<<n)){if(!mismatchFirst)line<<',';mismatchFirst=false;
                line<<"{\"property\":\""<<p::propertyName(c.properties[n])<<"\",\"property_address\":"<<c.propertyAddresses[n]<<",\"image\":"<<c.images[n].handle<<",\"generation\":"<<c.images[n].generation<<'}';}
            line<<"],\"unmapped_image_bindings\":"<<sample.unmapped<<",\"descriptor_bindings\":[";
            for(std::size_t n=0;n<evidence.count;++n){if(n)line<<',';const auto& b=evidence.bindings[n];
                line<<"{\"set\":"<<b.set.handle<<",\"set_generation\":"<<b.set.generation<<",\"set_index\":"<<b.setIndex<<",\"binding\":"<<b.binding
                    <<",\"array_element\":"<<b.arrayElement<<",\"descriptor_version\":"<<b.descriptorVersion<<",\"descriptor_type\":"<<b.descriptorType
                    <<",\"view\":"<<b.view.handle<<",\"view_generation\":"<<b.view.generation<<",\"image\":"<<b.image.handle<<",\"generation\":"<<b.image.generation<<'}';}
            line<<"],\"resource_signature_match\":false,\"shader_identity_verified\":false,\"gpu_complete\":false,\"execute_permitted\":false}";emit(line.str(),closed.id,true);
        }
    }catch(...){bump(closed.id,failures,false);}
}
void rememberNearMatch(const Candidate& c,const CommandRecordingFacts& recording,const descriptor_image_metadata::DispatchEvidence& evidence,
    FrameTimingAssociation frame,std::uint32_t thread,std::uint32_t missingMask,std::uint32_t mismatchedMask,std::uint32_t unmapped,std::size_t matchedBindings) noexcept {
    NearMatch* target=nullptr;
    for(auto& sample:nearMatches){
        if(sample.live&&sample.candidate.pass==c.pass&&sample.evidence.pipeline==evidence.pipeline){
            if(sample.matchedBindings>=matchedBindings)return;target=&sample;break;}
        if(!sample.live){if(!target)target=&sample;}
    }
    if(!target){target=&nearMatches[0];for(auto& sample:nearMatches)if(sample.matchedBindings<target->matchedBindings)target=&sample;
        if(target->matchedBindings>=matchedBindings)return;}
    *target={c,recording,evidence,frame,thread,missingMask,mismatchedMask,unmapped,matchedBindings,true};
}
template<std::size_t N>void hex(std::ostream& out,const std::array<unsigned char,N>& bytes){
    constexpr char digits[]="0123456789abcdef";out<<'"';for(auto b:bytes)out<<digits[b>>4]<<digits[b&15];out<<'"';
}
bool before(void* context,const rb::EntrySnapshot& call,rb::BeforeResult& out) {
    const auto pass=static_cast<p::Pass>(reinterpret_cast<std::uintptr_t>(context));const auto spec=p::profile(pass);
    if(!spec.payloadBytes)return false;
    if(!active(device))return false;const auto admitted=window.load(std::memory_order_acquire);if(!gate(admitted))return false;
    if(!fullGraphMode&&pass!=p::Pass::Accumulate&&pass!=p::Pass::Rcas)return false;
    if((call.returnAddress!=module+spec.registrationReturn&&(!spec.alternateRegistrationReturn||call.returnAddress!=module+spec.alternateRegistrationReturn))||!call.r8||!call.rcx){bump(admitted,registrationRefusals);return false;}
    bool exhausted{};{std::lock_guard lock(mutex);if(!gate(admitted))return false;exhausted=registrationCalls.fetch_add(1)>=registrationLimit&&!runtimeMode;}
    if(exhausted){closeWindow(admitted,"registration_limit");return false;}
    const auto first=frames(frameUser,call.threadId);Registration pending;pending.source=call.r8;pending.graph=call.rcx;pending.node=std::uint32_t(call.rdx);
    pending.pass=pass;pending.thread=call.threadId;pending.window=admitted;
    if(!readPayload(admitted,call.r8,pending.bytes.data(),spec.payloadBytes)){bump(admitted,registrationRefusals);return false;}
    const auto last=frames(frameUser,call.threadId);if(sameFrame(first,last))pending.frame=first;
    std::lock_guard lock(mutex);if(!gate(admitted)||registrationSerial==UINT64_MAX)return false;
    for(auto& entry:registrationSlots())if(!entry.constructing&&!entry.published){
        pending.serial=++registrationSerial;pending.constructing=true;entry=pending;
        out.token={admitted,pending.serial};out.associationAvailable=bool(pending.frame.valid&&pending.frame.ticket);out.published=true;return true;
    }
    ++registrationRefusals;return false;
}
bool close(void*,const rb::CloseEvent& call) {
    Registration pending;bool found{};
    {std::lock_guard lock(mutex);for(auto& entry:registrationSlots())if(entry.constructing&&entry.serial==call.token.scope&&entry.window==call.token.owner&&gate(entry.window)){pending=entry;entry={};found=true;break;}}
    if(!found)return false;const auto bytes=p::profile(pending.pass).payloadBytes;
    auto admitted=pending.window;
    const auto copied=call.exit==rb::Exit::Normal?p::registrationPayload(pending.pass,module,pending.graph,pending.node,call.copiedPayload,reader(&admitted)):0;
    std::array<unsigned char,p::maxPayloadBytes> source{},copy{};
    if(!copied||!readPayload(pending.window,pending.source,source.data(),bytes)||
       !readPayload(pending.window,copied,copy.data(),bytes)||source!=pending.bytes||copy!=source){bump(pending.window,registrationRefusals);return false;}
    pending.constructing=false;pending.published=true;pending.copy=copied;
    std::lock_guard lock(mutex);if(!gate(pending.window))return false;
    for(auto& entry:registrationSlots())if(entry.published&&entry.copy==pending.copy){entry={};++registrationRefusals;return false;}
    for(auto& entry:registrationSlots())if(!entry.constructing&&!entry.published){
        entry=pending;
        if(pending.pass==p::Pass::Rcas&&pending.frame.valid&&pending.frame.ticket)
            rcasFrames[pending.frame.engineFrame%rcasFrames.size()]=pending.frame;
        return true;
    }
    ++registrationRefusals;return false;
}
std::uint64_t originalAccumulate(void* graph,std::uint32_t node,const void* source) {
    return reinterpret_cast<rb::Original>(InterlockedCompareExchangePointer(&sr_accumulate_registration_original,nullptr,nullptr))(graph,node,source);
}
std::uint64_t originalRcas(void* graph,std::uint32_t node,const void* source) {
    return reinterpret_cast<rb::Original>(InterlockedCompareExchangePointer(&sr_rcas_registration_original,nullptr,nullptr))(graph,node,source);
}
#define SR_NATIVE_PASS(name, pass) \
std::uint64_t original_##name(void* graph,std::uint32_t node,const void* source) { \
    return reinterpret_cast<rb::Original>(InterlockedCompareExchangePointer(&sr_##name##_registration_original,nullptr,nullptr))(graph,node,source); \
}
#include "sr_native_passes.inc"
#undef SR_NATIVE_PASS
void observe(p::Pass pass,void* payload,void* graph) noexcept {
    if(pass<=p::Pass::Unknown||pass>=p::Pass::Count)return;
    ErrorGuard error;if(!active(device)||inside)return;auto admitted=window.load(std::memory_order_acquire);if(!gate(admitted))return;inside=true;struct Exit {~Exit(){inside=false;}} exit;
    if(!fullGraphMode&&pass!=p::Pass::Accumulate&&pass!=p::Pass::Rcas)return;
    std::uint64_t serial{},passSerial{};Registration association;
    {std::lock_guard lock(mutex);if(!gate(admitted))return;serial=++calls;
        for(auto& entry:registrationSlots())if(entry.published&&entry.window==admitted&&entry.copy==reinterpret_cast<std::uintptr_t>(payload)&&entry.pass==pass){association=entry;entry={};break;}
        passSerial=++passCalls[unsigned(pass)-1];}
    if(!runtimeMode&&serial>callbackLimit){closeWindow(admitted,"callback_limit");return;}
    if(!bump(admitted,parseAttempts))return;
    try {
        p::Request request;request.pass=pass;request.moduleBase=module;request.prepareInputPayload=reinterpret_cast<std::uintptr_t>(payload);
        request.graphContext=reinterpret_cast<std::uintptr_t>(graph);request.nativeTableBase=module+p::nativeTableRva;
        request.nativeLayout=fsr_probe::NativeTableLayout::DirectArrayOf1024PagePointers_1024ObjectPointers;
        const auto parsed=p::read(request,reader(&admitted));Images images;
        const bool parsedOk=parsed.status==p::Status::Parsed&&parsed.snapshotCaptured;
        const bool registered=association.published&&association.bytes==parsed.payload&&parsedOk;
        if(parsedOk&&gate(admitted))resolveImages(device,parsed,images);
        const auto decoded=frame_input::decodeUpscaleConstants(parsed.constants.bytes.data(),parsed.constants.captured?parsed.constants.bytes.size():0);
        Candidate candidate;candidate.callback=serial;candidate.pass=pass;candidate.registration=association.serial;
        candidate.registrationThread=association.thread;candidate.callbackThread=GetCurrentThreadId();candidate.groups=parsed.groups;candidate.window=admitted;
        if(registered)candidate.frame=association.frame;
        bool complete=parsedOk;candidate.count=parsed.bindingCount;
        for(std::size_t n=0;n<parsed.bindingCount;++n){const auto& image=images[n];const auto& binding=parsed.bindings[n];
            const bool exact=image.status==image_inventory::Status::LiveHandleObserved&&image.record.image==binding.native.imageCandidate&&image.record.generation&&
                image.record.description.width==binding.native.widthCandidate&&image.record.description.height==binding.native.heightCandidate;
            if(exact)candidate.images[n]={image.record.image,image.record.generation};else complete=false;
            candidate.properties[n]=binding.property;candidate.propertyAddresses[n]=binding.propertyAddress;
            // The pinned Accumulate graph declares these even when a compiled variant omits them.
            if(pass==p::Pass::Accumulate&&((binding.property==p::Property::LanczosLut&&binding.propertyAddress==module+0x1EDE588)||
                (binding.property==p::Property::CurrentLuma&&binding.propertyAddress==module+0x1EDE544)))candidate.optionalMask|=1u<<n;
        }
        if(parsed.constants.bytesUnchanged&&decoded.status==frame_input::ConstantsStatus::Decoded&&
            (decoded.sample.validityMask&frame_input::Extents)){
            candidate.renderExtent={std::uint32_t(decoded.sample.renderSize[0]),std::uint32_t(decoded.sample.renderSize[1])};
            candidate.outputExtent={std::uint32_t(decoded.sample.upscaleSize[0]),std::uint32_t(decoded.sample.upscaleSize[1])};
            bool outputExact{},inputExact=pass==p::Pass::Rcas;
            for(std::size_t n=0;n<parsed.bindingCount;++n){const auto& binding=parsed.bindings[n];const auto& image=images[n].record.description;
                if(!candidate.images[n].handle)continue;
                if(binding.property==p::Property::Output&&binding.propertyAddress==module+0x1EDE5E8)
                    outputExact=image.width==candidate.outputExtent.width&&image.height==candidate.outputExtent.height;
                if(pass==p::Pass::Accumulate&&binding.property==p::Property::InputColor&&binding.propertyAddress==module+0x1EDE540)
                    inputExact=image.width==candidate.renderExtent.width&&image.height==candidate.renderExtent.height;
            }
            candidate.extentsKnown=complete&&outputExact&&inputExact;candidate.constants=decoded.sample;
        }
        candidate.live=registered&&candidate.frame.valid&&candidate.frame.ticket&&complete;
        {std::lock_guard lock(mutex);if(!gate(admitted))return;
            // A rejected new invocation cannot leave an older candidate for its pass.
            if(!candidate.live){for(auto& c:candidateSlots())if(c.pass==pass)c.live=false;}
            else {
                auto* target=&candidates[0];for(auto& c:candidateSlots())if(!c.live){target=&c;break;}else if(c.callback<target->callback)target=&c;
                if(target->live)++evicted;*target=candidate;}
        }
        // Detail sampling must never suppress callback parsing or candidate refresh.
        if(runtimeMode?passSerial>8:(passSerial>8&&passSerial%32!=1))return;
        if(!bump(admitted,detailSamples))return;
        std::ostringstream out;out.imbue(std::locale::classic());out<<"{\"event\":\"sr_callback_snapshot\",\"window_id\":"<<admitted<<",\"pass\":\""<<p::passName(pass)<<"\",\"device\":"<<device<<",\"callback\":"<<serial
            <<",\"callback_thread\":"<<candidate.callbackThread<<",\"registration_thread\":"<<candidate.registrationThread<<",\"registration\":"<<association.serial
            <<",\"registration_matched\":"<<(registered?"true":"false")<<",\"payload\":"<<request.prepareInputPayload<<",\"graph_context\":"<<request.graphContext<<',';
        frameFields(out,candidate.frame);out<<",\"status\":\""<<p::statusName(parsed.status)<<"\",\"read_status\":\""<<fsr_probe::statusName(parsed.detail)
            <<"\",\"read_calls\":"<<parsed.readCalls<<",\"read_bytes\":"<<parsed.bytesRequested<<",\"kernel_token_raw\":"<<parsed.rawKernelToken
            <<",\"groups\":["<<parsed.groups[0]<<','<<parsed.groups[1]<<','<<parsed.groups[2]<<"],\"inventory_complete\":"<<(complete?"true":"false")<<",\"failure_binding\":";
        if(parsed.failureBindingIndex<p::maxBindings)out<<"{\"index\":"<<parsed.failureBindingIndex<<",\"property_address\":"<<parsed.failurePropertyAddress
            <<",\"graph_handle\":"<<parsed.failureGraphHandle<<",\"graph_key\":"<<parsed.failureGraphKey<<",\"graph_entry\":"<<parsed.failureGraphEntry
            <<",\"entry_type\":"<<parsed.failureEntryType<<",\"texture_id\":"<<parsed.failureTextureId<<",\"graph_status\":\""<<fsr_probe::statusName(parsed.failureGraphStatus)
            <<"\",\"native_status\":\""<<fsr_probe::statusName(parsed.failureNativeStatus)<<"\"}";
        else out<<"null";out<<",\"bindings\":[";
        for(std::size_t n=0;n<parsed.bindingCount;++n){if(n)out<<',';const auto& b=parsed.bindings[n];const auto& image=images[n].record;
            out<<"{\"property\":\""<<p::propertyName(b.property)<<"\",\"property_address\":"<<b.propertyAddress<<",\"graph_handle\":"<<b.graphHandle
                <<",\"graph_key\":"<<b.graphKey<<",\"texture_id\":"<<b.textureId<<",\"image_candidate\":"<<b.native.imageCandidate<<",\"live_key\":";
            if(candidate.images[n].handle)out<<"{\"handle\":"<<candidate.images[n].handle<<",\"generation\":"<<candidate.images[n].generation<<'}';else out<<"null";
            if(candidate.images[n].handle)out<<",\"format\":"<<image.description.format<<",\"usage\":"<<image.description.usage<<",\"width\":"<<image.description.width<<",\"height\":"<<image.description.height;
            else out<<",\"format\":null,\"usage\":null,\"width\":null,\"height\":null";out<<'}';}
        out<<"],\"upscaler_constants_captured\":"<<(parsed.constants.captured?"true":"false")
            <<",\"upscaler_slice\":{\"buffer_id\":"<<parsed.constants.bufferId<<",\"byte_offset\":"<<parsed.constants.byteOffset<<",\"byte_size\":"<<parsed.constants.byteSize<<",\"cpu_pointer\":"<<parsed.constants.cpuPointer<<'}'
            <<",\"upscaler_constants_hex\":";hex(out,parsed.constants.bytes);
        out<<",\"constants_finite_decoded\":"<<(decoded.status==frame_input::ConstantsStatus::Decoded?"true":"false")<<",\"delta_pre_exposure_candidate\":";
        float delta{};if(parsed.constants.captured)std::memcpy(&delta,parsed.constants.bytes.data()+0x74,sizeof(delta));
        if(parsed.constants.captured&&std::isfinite(delta))out<<delta;else out<<"null";
        out<<",\"render_size_candidate\":["<<decoded.sample.renderSize[0]<<','<<decoded.sample.renderSize[1]<<"],\"upscale_size_candidate\":["<<decoded.sample.upscaleSize[0]<<','<<decoded.sample.upscaleSize[1]<<']';
        out<<",\"rcas_coefficients_captured\":"<<(parsed.rcas.captured?"true":"false")<<",\"rcas_coefficients_hex\":";hex(out,parsed.rcas.bytes);
        out<<",\"reactive_constants_captured\":"<<(parsed.reactive.captured?"true":"false")<<",\"reactive_constants_hex\":";hex(out,parsed.reactive.bytes);
        out<<",\"spd_constants_captured\":"<<(parsed.spd.captured?"true":"false")<<",\"spd_constants_hex\":";hex(out,parsed.spd.bytes);
        out<<",\"callback_is_dispatch_boundary\":false,\"snapshot_atomic\":false,\"hdr_verified\":false,\"exposure_verified\":false,\"color_lifetime_pinned\":false,\"execute_permitted\":false}";
        emit(out.str(),admitted);
    } catch(...) {bump(admitted,failures);}
}
}
bool configure(std::uintptr_t base,std::uint64_t selected,Sink output,FrameReader frameReader,void* user,ResolveImages resolver,bool runtime,bool fullGraph,bool replacement) noexcept {
    ErrorGuard error;std::lock_guard lock(mutex);
    if(stopped.load()||configured.load()||!base||base>UINTPTR_MAX-p::nativeTableRva-8192||!selected||!output||!frameReader||!resolver)return false;
    runtimeMode=runtime;fullGraphMode=fullGraph;(void)replacement;module=base;device=selected;sink=output;frames=frameReader;frameUser=user;resolveImages=resolver;control.sampling=false;
    configured.store(true,std::memory_order_release);return true;
}
bool activate() noexcept {
    ErrorGuard error;if(!configured.load(std::memory_order_acquire)||stopped.load())return false;
    if(runtimeMode){window.store(1);deadline.store(UINT64_MAX);sampling.store(true);control.sampling=true;}
    ready.store(true,std::memory_order_release);return !stopped.load();
}
void stop() noexcept {ErrorGuard error;stopped.store(true,std::memory_order_release);sampling.store(false,std::memory_order_release);control.sampling=false;}
bool active(std::uint64_t selected) noexcept {
    ErrorGuard error;if(!configured.load(std::memory_order_acquire)||selected!=device||!gate())return false;
    const auto admitted=window.load(std::memory_order_acquire);
    if(GetTickCount64()>=deadline.load(std::memory_order_acquire)){closeWindow(admitted,"duration_limit");return false;}return gate(admitted);
}
void pollCapture() noexcept {
    ErrorGuard error;if(runtimeMode||!ready.load(std::memory_order_acquire)||stopped.load()||insideSink)return;
    const auto now=GetTickCount64();auto next=nextPoll.load(std::memory_order_relaxed);if(now<next||!nextPoll.compare_exchange_strong(next,now+250))return;
    (void)active(device);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    const bool exists=GetFileAttributesExW(L"C:\\ProgramData\\EndfieldXeSSDiag\\SrObservationCapture",GetFileExInfoStandard,&info)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY);
    inspectCapture(exists,{(std::uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime,
        (std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow},now);
}
DispatchTicket beforeDispatch(std::uint64_t selected,const CommandRecordingFacts& recording,const descriptor_image_metadata::DispatchEvidence& evidence,
    FrameTimingAssociation frame,std::uint32_t thread,RecordedSourceLayoutQuery layouts,resource_memory_observer::SourceQuery memory,RecordedImageIntervalQuery intervals) noexcept {
    ErrorGuard error;DispatchTicket ticket;if(insideSink||!active(selected))return ticket;
    const auto admitted=window.load(std::memory_order_acquire);bool exhausted{};
    if(fullGraphMode&&!runtimeMode)ticket.consumerWindow=admitted;
    try {
        Following previous;
        {std::lock_guard lock(mutex);if(!gate(admitted))return ticket;
            for(auto& f:following)if(f.live&&f.recording.device==recording.device&&f.recording.contextIncarnation==recording.contextIncarnation&&
                f.recording.command==recording.command&&f.recording.rpRecordingGeneration==recording.rpRecordingGeneration&&
                f.recording.commonRecordOrder<recording.commonRecordOrder){previous=f;previous.dispatches=++f.dispatches;if(f.dispatches==2)f.live=false;break;}}
        if(previous.live){bool sampled{},storage{};std::uint32_t bindingMask{};
            if(evidence.metadataAccepted&&!evidence.issues)for(std::size_t n=0;n<evidence.count;++n){const auto& b=evidence.bindings[n];
                if(b.image!=previous.output)continue;
                sampled|=b.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE||b.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                storage|=b.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;if(b.setIndex==0&&b.binding<32)bindingMask|=1u<<b.binding;}
            std::ostringstream out;out.imbue(std::locale::classic());out<<"{\"event\":\"sr_following_dispatch\",\"window_id\":"<<admitted
                <<",\"callback\":"<<previous.callback<<",\"command\":"<<recording.command<<",\"recording_generation\":"<<recording.rpRecordingGeneration
                <<",\"after_accumulate\":"<<previous.dispatches<<",\"pipeline\":"<<evidence.pipeline.handle<<",\"pipeline_generation\":"<<evidence.pipeline.generation
                <<",\"metadata_accepted\":"<<(evidence.metadataAccepted&&!evidence.issues?"true":"false")<<",\"metadata_issues\":"<<evidence.issues
                <<",\"output_image\":"<<previous.output.handle<<",\"output_generation\":"<<previous.output.generation
                <<",\"output_sampled_descriptor\":"<<(sampled?"true":"false")<<",\"output_storage_descriptor\":"<<(storage?"true":"false")
                <<",\"output_set0_binding_mask\":"<<bindingMask<<",\"shader_read_verified\":false,\"execute_permitted\":false}";emit(out.str(),admitted);}
        std::array<Candidate,16> selectedCandidates;std::size_t count{};std::uint32_t detailMask{};
        RawPair raw;std::size_t rawMatches{};
        {std::lock_guard lock(mutex);if(!gate(admitted))return ticket;
            exhausted=dispatchCalls.fetch_add(1)>=dispatchObservationLimit&&!runtimeMode;
            DispatchGate rejected=DispatchGate::Count;
            if(exhausted)rejected=DispatchGate::Limit;
            else {
                for(std::size_t n=0;n<dispatchDiagnostics.descriptorIssues.size();++n)
                    if(evidence.issues&(std::uint64_t(1)<<n))++dispatchDiagnostics.descriptorIssues[n];
                if(!frame.valid||!frame.ticket)rejected=DispatchGate::FrameUnknown;
                else if(!recording.recording||!recording.contextIncarnation||recording.device!=selected||
                    recording.command!=evidence.command.handle||!recording.rpRecordingGeneration)rejected=DispatchGate::RecordingInvalid;
                else if(evidence.filteredOut)rejected=DispatchGate::DescriptorFiltered;
                else if(!evidence.metadataAccepted||evidence.issues)rejected=DispatchGate::DescriptorRejected;
                else if(!evidence.count||evidence.count>evidence.bindings.size())rejected=DispatchGate::BindingsInvalid;
            }
            if(rejected!=DispatchGate::Count)++dispatchDiagnostics.gates[std::size_t(rejected)];
            else {
                const descriptor_image_metadata::ImageBinding* depth=nullptr;const descriptor_image_metadata::ImageBinding* motion=nullptr;
                for(std::size_t n=0;n<evidence.count;++n){const auto& b=evidence.bindings[n];
                    if(b.setIndex==0&&b.binding==5&&b.viewFormat==VK_FORMAT_D32_SFLOAT_S8_UINT)depth=&b;
                    if(b.setIndex==0&&b.binding==7&&b.viewFormat==VK_FORMAT_R16G16_SFLOAT)motion=&b;}
                if(depth&&motion&&depth->image.handle&&depth->view.handle&&motion->image.handle&&motion->view.handle&&
                    depth->imageDescription.width==motion->imageDescription.width&&depth->imageDescription.height==motion->imageDescription.height) {
                    rawPairs[rawPairSerial++%rawPairs.size()]={recording,frame,{*depth,*motion},evidence.pipeline,true};
                    ticket.containsPrepare=true;ticket.command=recording.command;ticket.recording=recording.rpRecordingGeneration;
                    ticket.pipeline=evidence.pipeline.handle;ticket.pipelineGeneration=evidence.pipeline.generation;ticket.window=admitted;ticket.frame=frame;
                    ticket.ordinal=evidence.recordOrdinal;ticket.groups=evidence.groups;
                    ticket.renderExtent={depth->imageDescription.width,depth->imageDescription.height};
                }
                for(const auto& candidate:rawPairs)if(candidate.live&&sameFrame(candidate.frame,frame)&&
                    candidate.recording.device==recording.device&&candidate.recording.contextIncarnation==recording.contextIncarnation&&
                    candidate.recording.command==recording.command&&candidate.recording.rpRecordingGeneration==recording.rpRecordingGeneration&&
                    candidate.recording.commonRecordOrder<recording.commonRecordOrder) {
                    ++rawMatches;if(!raw.live||candidate.recording.commonRecordOrder>raw.recording.commonRecordOrder)raw=candidate;
                }
                bool live{},frameMatched{},groupsMatched{},resourceMatched{};
                for(auto& c:candidateSlots()){if(!c.live||c.window!=admitted)continue;live=true;
                    if(!sameFrame(c.frame,frame))continue;
                    if(fullGraphMode&&!runtimeMode)ticket.shadow.registeredMask|=sr_dispatch_shadow::passBit(c.pass);
                    frameMatched=true;if(c.groups!=evidence.groups)continue;groupsMatched=true;
                    std::uint32_t missingMask{},mismatchedMask{},unmapped{};std::size_t matchedBindings{};
                    for(std::size_t n=0;n<c.count;++n){bool found{},conflict{};for(std::size_t b=0;b<evidence.count;++b)if(evidence.bindings[b].image.handle==c.images[n].handle){
                            if(evidence.bindings[b].image==c.images[n])found=true;else conflict=true;}
                        if(found)++matchedBindings;if(conflict)mismatchedMask|=1u<<n;else if(!found)missingMask|=1u<<n;}
                    for(std::size_t b=0;b<evidence.count;++b){bool mapped{};for(std::size_t n=0;n<c.count;++n)if(evidence.bindings[b].image.handle==c.images[n].handle){mapped=true;break;}if(!mapped)++unmapped;}
                    const bool qualified=!(missingMask&~c.optionalMask)&&!mismatchedMask&&!unmapped;rememberSignature(c,missingMask,mismatchedMask,unmapped,qualified);
                    if(!qualified){
                        if(fullGraphMode&&!runtimeMode&&missingMask&&!mismatchedMask&&!unmapped&&matchedBindings&&
                           evidence.pipeline.handle&&evidence.pipeline.generation){ticket.diagnosticArchiveCandidate=true;ticket.shadow.candidateMask|=sr_dispatch_shadow::passBit(c.pass);}
                        rememberNearMatch(c,recording,evidence,frame,thread,missingMask,mismatchedMask,unmapped,matchedBindings);continue;}
                    if(count==selectedCandidates.size()){++failures;return {};}
                    resourceMatched=true;if(matches.fetch_add(1)<dispatchDetailLimit)detailMask|=1u<<count;else ++omittedMatches;
                    ++c.hits;selectedCandidates[count]=c;selectedCandidates[count++].omittedMask=missingMask;
                    if(fullGraphMode&&!runtimeMode)ticket.shadow.exactMask|=sr_dispatch_shadow::passBit(c.pass);
                }
                const auto result=!live?DispatchGate::NoCandidate:!frameMatched?DispatchGate::CandidateFrameMismatch:
                    !groupsMatched?DispatchGate::GroupsMismatch:!resourceMatched?DispatchGate::ResourceMismatch:DispatchGate::Matched;
                ++dispatchDiagnostics.gates[std::size_t(result)];
            }
        }
        if(exhausted){closeWindow(admitted,"dispatch_limit");return ticket;}
        if(fullGraphMode&&!runtimeMode){
            if(ticket.containsPrepare)ticket.shadow.exactMask|=sr_dispatch_shadow::prepareBit;
            if(ticket.shadow.exactMask|ticket.shadow.candidateMask){
                ticket.shadowObserved=true;ticket.shadow.command=recording.command;ticket.shadow.recording=recording.rpRecordingGeneration;
                ticket.shadow.incarnation=recording.contextIncarnation;ticket.shadow.order=recording.commonRecordOrder;
                ticket.shadow.pipeline=evidence.pipeline.handle;ticket.shadow.pipelineGeneration=evidence.pipeline.generation;
                ticket.shadow.rawPairAvailable=ticket.containsPrepare||rawMatches==1;
                ticket.window=admitted;ticket.frame=frame;
            }
        }
        if(ticket.diagnosticArchiveCandidate){ticket.pipeline=evidence.pipeline.handle;ticket.pipelineGeneration=evidence.pipeline.generation;}
        if(!count)return ticket;
        ticket.command=recording.command;ticket.recording=recording.rpRecordingGeneration;ticket.ordinal=evidence.recordOrdinal;ticket.window=admitted;ticket.detailMask=detailMask;
        ticket.frame=frame;
        if(count){ticket.pipeline=evidence.pipeline.handle;ticket.pipelineGeneration=evidence.pipeline.generation;}
        if(count==1){const auto& c=selectedCandidates[0];ticket.nativePass=c.pass;ticket.groups=c.groups;ticket.renderExtent=c.renderExtent;
            ticket.outputExtent=c.outputExtent;ticket.extentsKnown=c.extentsKnown;
            if(c.pass==p::Pass::Accumulate&&detailMask){descriptor_image_metadata::Key output;
                for(std::size_t n=0;n<c.count;++n)if(c.properties[n]==p::Property::Output)output=c.images[n];
                if(output.handle){std::lock_guard lock(mutex);if(!gate(admitted))return {};
                    auto* target=&following[0];for(auto& f:following)if(!f.live||f.recording.command==recording.command){target=&f;break;}
                    *target={recording,output,c.callback,0,true};}}
        }
        if(count==1){
            const auto& c=selectedCandidates[0];
            std::lock_guard lock(mutex);if(!gate(admitted))return {};
            const bool expected=sameFrame(rcasFrames[frame.engineFrame%rcasFrames.size()],frame);
            ticket.terminalFsr=c.pass==p::Pass::Rcas||(c.pass==p::Pass::Accumulate&&!expected);
            if(c.pass==p::Pass::Accumulate){
                Stage1Source source;source.prepare=raw.recording;source.accumulate=recording;source.frame=frame;
                source.preparePipeline=raw.pipeline;source.accumulatePipeline=evidence.pipeline;
                source.constants=c.constants;source.rcasExpected=expected;
                source.images[1]=raw.images[1];source.images[2]=raw.images[0];
                bool color{},output{};
                for(std::size_t n=0;n<c.count;++n)if(c.properties[n]==p::Property::InputColor||c.properties[n]==p::Property::Output){
                    for(std::size_t b=0;b<evidence.count;++b)if(evidence.bindings[b].image==c.images[n]){
                        if(c.properties[n]==p::Property::InputColor){source.images[0]=evidence.bindings[b];color=true;}
                        else{source.images[3]=evidence.bindings[b];output=true;}
                    }
                }
                source.valid=rawMatches==1&&raw.live&&color&&output&&c.extentsKnown&&
                    c.renderExtent.width&&c.renderExtent.height&&c.renderExtent.width<=c.outputExtent.width&&c.renderExtent.height<=c.outputExtent.height&&
                    ((c.renderExtent.width==c.outputExtent.width||c.renderExtent.height==c.outputExtent.height)?
                        (c.renderExtent.width==c.outputExtent.width&&c.renderExtent.height==c.outputExtent.height):true);
                stage1Sources[frame.engineFrame%stage1Sources.size()]=source;ticket.source=source;
            }else if(c.pass==p::Pass::Rcas){
                const auto& source=stage1Sources[frame.engineFrame%stage1Sources.size()];bool output{};
                for(std::size_t n=0;n<c.count;++n)if(c.properties[n]==p::Property::Output&&c.images[n]==source.images[3].image)output=true;
                if(sameFrame(source.frame,frame)&&source.accumulate.command==recording.command&&
                    source.accumulate.rpRecordingGeneration==recording.rpRecordingGeneration&&source.accumulate.commonRecordOrder<recording.commonRecordOrder&&output)
                    ticket.source=source;
            }
        }
        for(std::size_t n=0;n<count;++n){const auto& c=selectedCandidates[n];ticket.callbacks[ticket.count++]=c.callback;ticket.containsAccumulate|=c.pass==p::Pass::Accumulate;ticket.containsRcas|=c.pass==p::Pass::Rcas;
            if(!(detailMask&(1u<<n)))continue;
            std::ostringstream out;out.imbue(std::locale::classic());out<<"{\"event\":\"sr_dispatch_candidate\",\"window_id\":"<<admitted<<",\"pass\":\""<<p::passName(c.pass)<<"\",\"callback\":"<<c.callback<<",\"registration\":"<<c.registration
                <<",\"callback_thread\":"<<c.callbackThread<<",\"dispatch_thread\":"<<thread<<",\"same_thread\":"<<(c.callbackThread==thread?"true":"false")
                <<",\"command\":"<<recording.command<<",\"recording_generation\":"<<recording.rpRecordingGeneration<<",\"context_incarnation\":"<<recording.contextIncarnation
                <<",\"record_ordinal\":"<<evidence.recordOrdinal<<",\"record_order\":"<<recording.commonRecordOrder<<",\"pipeline\":"<<evidence.pipeline.handle
                <<",\"pipeline_generation\":"<<evidence.pipeline.generation<<",\"candidate_hits\":"<<c.hits<<",\"matching_callbacks\":"<<count<<',';frameFields(out,frame);
            out<<",\"same_frame_verified\":true,\"resource_signature_match\":true,\"resource_signature_scope\":\"required_callback_bindings\",\"callback_binding_count\":"<<c.count
                <<",\"required_callback_bindings\":"<<(c.count-std::popcount(c.optionalMask))<<",\"matching_required_callback_bindings\":"<<(c.count-std::popcount(c.optionalMask))
                <<",\"matching_callback_bindings\":"<<(c.count-std::popcount(c.omittedMask))
                <<",\"omitted_optional_bindings\":[";bool firstOmitted=true;
            for(std::size_t b=0;b<c.count;++b)if(c.omittedMask&(1u<<b)){
                if(!firstOmitted)out<<',';firstOmitted=false;out<<"{\"property\":\""<<p::propertyName(c.properties[b])<<"\",\"property_address\":"<<c.propertyAddresses[b]
                    <<",\"image\":"<<c.images[b].handle<<",\"generation\":"<<c.images[b].generation<<'}';}
            out<<"],\"shader_binding_usage_verified\":false,\"unique_so_far\":"<<(c.hits==1&&count==1?"true":"false")<<",\"bindings\":[";
            for(std::size_t b=0;b<evidence.count;++b){if(b)out<<',';const auto& image=evidence.bindings[b];RecordedSourceLayout layout;resource_memory_observer::Observation observation;
                if(layouts.resolve)layout=layouts.resolve(layouts.user,recording,{image.image.handle,image.image.generation},image.range,image.declaredLayout);
                if(memory.query)observation=memory.query(memory.user,{image.image.handle,image.image.generation});const auto& binding=observation.binding;
                out<<"{\"image\":"<<image.image.handle<<",\"generation\":"<<image.image.generation<<",\"set\":"<<image.set.handle<<",\"set_generation\":"<<image.set.generation
                    <<",\"set_index\":"<<image.setIndex<<",\"binding\":"<<image.binding<<",\"descriptor_version\":"<<image.descriptorVersion<<",\"view\":"<<image.view.handle
                    <<",\"view_generation\":"<<image.view.generation<<",\"format\":"<<image.imageDescription.format<<",\"usage\":"<<image.imageDescription.usage
                    <<",\"width\":"<<image.imageDescription.width<<",\"height\":"<<image.imageDescription.height
                    <<",\"aspect\":"<<image.range.aspectMask<<",\"base_mip\":"<<image.range.baseMipLevel<<",\"level_count\":"<<image.range.levelCount
                    <<",\"base_layer\":"<<image.range.baseArrayLayer<<",\"layer_count\":"<<image.range.layerCount<<",\"declared_layout\":"<<image.declaredLayout
                    <<",\"layout_queried\":"<<(layout.queried?"true":"false")<<",\"layout_resolved\":"<<(layout.recordedLayoutResolved?"true":"false")<<",\"recorded_layout\":";
                if(layout.recordedLayoutResolved)out<<layout.recordedLayout;else out<<"null";
                out
                    <<",\"layout_issues\":"<<layout.issues<<",\"memory_queried\":"<<(observation.queried?"true":"false")<<",\"memory_bounds_known\":"<<(binding.bindingBoundsKnown?"true":"false")
                    <<",\"memory_alias_free_observed\":"<<(binding.observedAliasFree?"true":"false")<<",\"memory_local_issues\":"<<observation.localIssues<<",\"memory_global_issues\":"<<observation.globalIssues
                    <<",\"memory_binding\":";
                if(binding.bindingBoundsKnown)out<<"{\"handle\":"<<binding.memory.handle<<",\"generation\":"<<binding.memory.generation<<",\"offset\":"<<binding.offset<<",\"size\":"<<binding.size
                    <<",\"end\":"<<binding.end<<",\"overlap_count\":"<<binding.overlapCount<<",\"first_overlap_resource\":"<<binding.firstOverlap.key.handle<<",\"first_overlap_generation\":"<<binding.firstOverlap.key.generation<<'}';
                else out<<"null";out<<'}';}
            out<<"],\"shader_identity_verified\":false,\"gpu_complete\":false,\"color_lifetime_pinned\":false,\"hdr_verified\":false,\"exposure_verified\":false,\"execute_permitted\":false}";emit(out.str(),admitted);
        }
        RecordedImageInterval depthInterval;
        if(ticket.containsAccumulate) {
            if(rawMatches==1&&intervals.resolve)depthInterval=intervals.resolve(intervals.user,raw.recording,recording,
                {raw.images[0].image.handle,raw.images[0].image.generation});
            std::lock_guard lock(mutex);if(!gate(admitted))return {};
            ++depthIntervals.matched;
            if(!rawMatches)++depthIntervals.missing;
            else if(rawMatches!=1)++depthIntervals.ambiguous;
            else if(depthInterval.queried){++depthIntervals.queried;
                if(depthInterval.untouched&&!depthInterval.issues)++depthIntervals.untouched;
                else if(depthInterval.issues)++depthIntervals.unknown;
                depthIntervals.barriers+=depthInterval.barriers!=0;depthIntervals.attachments+=depthInterval.attachments!=0;
                depthIntervals.transfers+=depthInterval.transfers!=0;depthIntervals.clears+=depthInterval.clears!=0;
            }else ++depthIntervals.unknown;
        }
        if(ticket.containsAccumulate&&ticket.detailMask) {
            std::ostringstream out;out.imbue(std::locale::classic());out<<"{\"event\":\"sr_raw_inputs_at_accumulate\",\"window_id\":"<<admitted
                <<",\"command\":"<<recording.command<<",\"recording_generation\":"<<recording.rpRecordingGeneration
                <<",\"tail_order\":"<<recording.commonRecordOrder<<",\"same_recording_candidates\":"<<rawMatches
                <<",\"raw_pair_found\":"<<(raw.live?"true":"false")<<",\"prepare_order\":"<<raw.recording.commonRecordOrder
                <<",\"prepare_pipeline\":"<<raw.pipeline.handle<<",\"prepare_pipeline_generation\":"<<raw.pipeline.generation<<',';frameFields(out,frame);
            out<<",\"depth_interval\":{\"queried\":"<<(depthInterval.queried?"true":"false")
                <<",\"untouched\":"<<(depthInterval.untouched&&!depthInterval.issues?"true":"false")<<",\"issues\":"<<depthInterval.issues
                <<",\"barriers\":"<<depthInterval.barriers<<",\"attachments\":"<<depthInterval.attachments
                <<",\"transfers\":"<<depthInterval.transfers<<",\"clears\":"<<depthInterval.clears<<"},\"inputs\":[";
            if(raw.live)for(std::size_t n=0;n<raw.images.size();++n) {
                if(n)out<<',';const auto& image=raw.images[n];RecordedSourceLayout layout;resource_memory_observer::Observation observation;
                if(layouts.resolve)layout=layouts.resolve(layouts.user,recording,{image.image.handle,image.image.generation},image.range,image.declaredLayout);
                if(memory.query)observation=memory.query(memory.user,{image.image.handle,image.image.generation});const auto& binding=observation.binding;
                out<<"{\"role\":\""<<(n?"raw_motion":"raw_depth")<<"\",\"image\":"<<image.image.handle<<",\"generation\":"<<image.image.generation
                    <<",\"view\":"<<image.view.handle<<",\"view_generation\":"<<image.view.generation<<",\"format\":"<<image.viewFormat
                    <<",\"width\":"<<image.imageDescription.width<<",\"height\":"<<image.imageDescription.height<<",\"aspect\":"<<image.range.aspectMask
                    <<",\"usage\":"<<image.imageDescription.usage<<",\"prepare_declared_layout\":"<<image.declaredLayout
                    <<",\"tail_layout_resolved\":"<<(layout.recordedLayoutResolved?"true":"false")<<",\"tail_recorded_layout\":"<<layout.recordedLayout
                    <<",\"layout_issues\":"<<layout.issues<<",\"memory_queried\":"<<(observation.queried?"true":"false")
                    <<",\"memory_bounds_known\":"<<(binding.bindingBoundsKnown?"true":"false")<<",\"memory\":"<<binding.memory.handle
                    <<",\"memory_generation\":"<<binding.memory.generation<<",\"offset\":"<<binding.offset<<",\"end\":"<<binding.end
                    <<",\"overlap_count\":"<<binding.overlapCount<<",\"memory_local_issues\":"<<observation.localIssues<<",\"memory_global_issues\":"<<observation.globalIssues<<'}';
            }
            out<<"],\"prepare_pipeline_verified\":false,\"view_still_live_verified\":false,\"contents_preserved_verified\":false,\"execute_permitted\":false}";
            emit(out.str(),admitted);
        }
    }catch(...){bump(admitted,failures);return {};}
    return ticket;
}
bool consumerActive(std::uint64_t selected) noexcept {
    return selected==device&&configured.load(std::memory_order_acquire)&&fullGraphMode&&!runtimeMode&&!consumerFinished.load(std::memory_order_acquire)&&gate()&&!insideSink;
}
std::uint64_t consumerWindow(std::uint64_t selected) noexcept {return consumerActive(selected)?window.load(std::memory_order_acquire):0;}
void consumerBegin(std::uint64_t selected,std::uint64_t admitted,const sr_tail_commands::Key& key) noexcept {
    ErrorGuard error;if(!consumerActive(selected)||!gate(admitted))return;std::lock_guard lock(mutex);
    if(consumerActive(selected)&&gate(admitted)&&key.device==selected&&key.incarnation&&key.command&&key.recording)consumerAudit.begin(key);
}
namespace {
sr_consumers::Image consumerReference(descriptor_image_metadata::Key key,resource_memory_observer::SourceQuery memory) noexcept {
    sr_consumers::Image out;out.key=key;if(memory.query&&key.handle&&key.generation)out.memory=memory.query(memory.user,{key.handle,key.generation});return out;
}
}
bool consumerDispatch(std::uint64_t selected,std::uint64_t admitted,const CommandRecordingFacts& facts,const descriptor_image_metadata::DispatchEvidence& evidence,
        FrameTimingAssociation frame,int profile,descriptor_image_metadata::Key finalOutput,resource_memory_observer::SourceQuery memory) noexcept {
    ErrorGuard error;if(!consumerActive(selected)||!gate(admitted))return false;std::lock_guard lock(mutex);if(!consumerActive(selected)||!gate(admitted)||facts.device!=selected)return false;
    const auto key=sr_tail_commands::key(facts);
    const bool known=profile>=0&&std::size_t(profile)<sr_native_shaders::profiles.size();
    const bool internal=known&&sr_native_shaders::profiles[profile].policy==sr_native_shaders::Policy::RequiresConsumerProof;
    const bool complete=evidence.metadataAccepted&&!evidence.issues&&!evidence.filteredOut&&evidence.count<=evidence.bindings.size();
    if(internal&&complete&&!consumerAudit.producerFrame(key,frame,facts.commonRecordOrder)){
        consumerFinished.store(consumerAudit.closed(),std::memory_order_release);return false;}
    if(!consumerAudit.command(key,complete,internal))return false;
    bool referenced=false;
    for(std::size_t n=0;n<std::min(evidence.count,evidence.bindings.size());++n){const auto& b=evidence.bindings[n];
        auto image=consumerReference(b.image,memory);image.setIndex=b.setIndex;image.binding=b.binding;image.descriptorType=b.descriptorType;
        referenced|=consumerAudit.use(key,sr_consumers::Kind::Compute,{&image,1},internal,evidence.pipeline,facts.commonRecordOrder);
    }
    if(!known&&referenced)return consumerAudit.requestShader(evidence.pipeline);
    if(!internal||!complete)return false;
    // All storage descriptors are potential writes. Only the exact Stage1
    // final output is excluded; ambiguous output roles remain conservative.
    for(std::size_t n=0;n<std::min(evidence.count,evidence.bindings.size());++n){const auto& b=evidence.bindings[n];
        if(b.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE&&b.image!=finalOutput)
            consumerAudit.produce(key,frame,consumerReference(b.image,memory),std::size_t(profile),facts.commonRecordOrder);
    }
    return false;
}
void consumerCommand(std::uint64_t selected,std::uint64_t admitted,const CommandRecordingFacts& facts,const sr_tail_commands::Command& command,ConsumerQueries queries) noexcept {
    ErrorGuard error;if(!consumerActive(selected)||!gate(admitted))return;std::lock_guard lock(mutex);if(!consumerActive(selected)||!gate(admitted)||facts.device!=selected)return;
    namespace stc=sr_tail_commands;const auto key=stc::key(facts);
    const bool complete=!command.truncated&&!command.missingData&&!command.extensionsOmitted;
    if(command.kind==stc::Kind::BindDescriptorSets){if(consumerAudit.state(key,complete))consumerAudit.bindGraphics(key,command.descriptors,complete);return;}
    if(command.kind==stc::Kind::BindPipeline){
        if(consumerAudit.state(key,complete)&&command.storedArguments>=2&&command.arguments[0]==VK_PIPELINE_BIND_POINT_GRAPHICS)consumerAudit.graphicsPipeline(key,command.arguments[1]);return;}
    if(command.kind==stc::Kind::Barrier||command.kind==stc::Kind::PushConstants||command.kind==stc::Kind::Dispatch||
       command.kind==stc::Kind::DispatchIndirect||command.kind==stc::Kind::DispatchBase){consumerAudit.state(key,complete);return;}
    const std::string_view name(command.otherName.data(),std::find(command.otherName.begin(),command.otherName.end(),'\0')-command.otherName.begin());
    constexpr std::array<std::string_view,18> stateNames{{"vkCmdSetViewport","vkCmdSetScissor","vkCmdSetViewportWithCount","vkCmdSetScissorWithCount",
        "vkCmdSetViewportWithCountEXT","vkCmdSetScissorWithCountEXT","vkCmdSetLineWidth","vkCmdSetDepthBias","vkCmdSetDepthBounds",
        "vkCmdSetStencilCompareMask","vkCmdSetStencilWriteMask","vkCmdSetStencilReference","vkCmdSetBlendConstants",
        "vkCmdEndRenderPass","vkCmdEndRenderPass2","vkCmdEndRenderPass2KHR","vkCmdEndRendering","vkCmdEndRenderingKHR"}};
    if(command.kind==stc::Kind::Other&&std::find(stateNames.begin(),stateNames.end(),name)!=stateNames.end()){consumerAudit.state(key,complete);return;}
    const bool opaqueGraphics=command.kind==stc::Kind::Other&&(name.find("PushDescriptor")!=name.npos||name.find("ExecuteCommands")!=name.npos);
    if(opaqueGraphics&&consumerAudit.state(key,complete)){consumerAudit.opaqueGraphics(key);consumerAudit.noteOpaque(key,command.otherName,facts.commonRecordOrder);}
    if(!consumerAudit.command(key,complete,false))return;
    switch(command.kind){
    case stc::Kind::Draw:case stc::Kind::DrawIndexed:case stc::Kind::DrawIndirect:case stc::Kind::DrawIndexedIndirect:
    case stc::Kind::DrawIndirectCount:case stc::Kind::DrawIndexedIndirectCount:{
        consumerAudit.graphicsDraw(key);const auto* graphics=consumerAudit.graphics(key);if(!graphics||!queries.set){consumerAudit.unknown();return;}
        for(std::size_t index=0;index<graphics->sets.size();++index)if(const auto set=graphics->sets[index]){const auto snapshot=queries.set(queries.user,set);
            if(!snapshot.known||snapshot.issues||snapshot.count>snapshot.bindings.size())consumerAudit.unknown();
            for(std::size_t n=0;n<std::min(snapshot.count,snapshot.bindings.size());++n){auto image=consumerReference(snapshot.bindings[n].image,queries.memory);
                image.setIndex=static_cast<std::uint32_t>(index);image.binding=snapshot.bindings[n].binding;image.descriptorType=snapshot.bindings[n].descriptorType;
                consumerAudit.use(key,sr_consumers::Kind::Graphics,{&image,1},false,{graphics->pipeline,0},facts.commonRecordOrder);}}
        return;}
    case stc::Kind::BeginRenderPass:case stc::Kind::BeginRendering:{
        if(!queries.attachments){consumerAudit.unknown();return;}const auto images=queries.attachments(queries.user,facts,command);
        if(!images.complete||images.count>images.images.size())consumerAudit.unknown();
        for(std::size_t n=0;n<std::min(images.count,images.images.size());++n){const auto image=consumerReference(images.images[n],queries.memory);
            consumerAudit.use(key,sr_consumers::Kind::Attachment,{&image,1},false,{},facts.commonRecordOrder);}return;}
    case stc::Kind::Other:{
        if(!opaqueGraphics){consumerAudit.noteOpaque(key,command.otherName,facts.commonRecordOrder);consumerAudit.unknown();}
        return;} // Opaque buffer/extension commands are not inferred harmless.
    default:return;
    }
}
void consumerImage(std::uint64_t selected,std::uint64_t admitted,const CommandRecordingFacts& facts,descriptor_image_metadata::Key image,resource_memory_observer::SourceQuery memory) noexcept {
    ErrorGuard error;if(!consumerActive(selected)||!gate(admitted))return;std::lock_guard lock(mutex);if(!consumerActive(selected)||!gate(admitted)||facts.device!=selected)return;
    const auto key=sr_tail_commands::key(facts);if(!consumerAudit.command(key,true,false))return;
    const auto reference=consumerReference(image,memory);consumerAudit.use(key,sr_consumers::Kind::Transfer,{&reference,1},false,{},facts.commonRecordOrder);
}
void consumerEnd(std::uint64_t selected,std::uint64_t admitted,const sr_tail_commands::Key& key,VkResult result) noexcept {
    ErrorGuard error;if(!consumerActive(selected)||!gate(admitted))return;std::lock_guard lock(mutex);
    if(consumerActive(selected)&&gate(admitted)&&key.device==selected)consumerAudit.end(key,result==VK_SUCCESS);
}
void afterDispatch(DispatchTicket ticket,const char* api,bool fingerprintKnown) noexcept {
    ErrorGuard error;if(!gate(ticket.window))return;
    if(ticket.shadowObserved){std::lock_guard lock(mutex);if(!gate(ticket.window)||!fullGraphMode||runtimeMode)return;
        auto& d=ticket.shadow;d.fingerprintKnown=fingerprintKnown;d.stage1SourceAvailable=ticket.source.valid;
        if(api){if(!std::strcmp(api,"vkCmdDispatch"))d.api=sr_dispatch_shadow::Api::Direct;
            else if(!std::strcmp(api,"vkCmdDispatchIndirect"))d.api=sr_dispatch_shadow::Api::Indirect;
            else if(!std::strcmp(api,"vkCmdDispatchBase"))d.api=sr_dispatch_shadow::Api::Base;
            else if(!std::strcmp(api,"vkCmdDispatchBaseKHR"))d.api=sr_dispatch_shadow::Api::BaseKhr;}
        dispatchShadow.forwarded(ticket.frame,d);
    }
    if(!ticket.count)return;
    try{for(std::size_t n=0;n<ticket.count;++n){if(!bump(ticket.window,forwarded))return;if(!(ticket.detailMask&(1u<<n)))continue;std::ostringstream out;out.imbue(std::locale::classic());out<<"{\"event\":\"sr_dispatch_return\",\"window_id\":"<<ticket.window<<",\"callback\":"<<ticket.callbacks[n]<<",\"api\":\""<<api
        <<"\",\"command\":"<<ticket.command<<",\"recording_generation\":"<<ticket.recording<<",\"record_ordinal\":"<<ticket.ordinal<<",\"driver_forwarded\":true,\"gpu_complete\":false,\"execute_permitted\":false}";emit(out.str(),ticket.window);}}catch(...){bump(ticket.window,failures);}
}
void summary() noexcept {
    ErrorGuard error;if(insideSink||!configured.load(std::memory_order_acquire))return;
    std::uint64_t admitted{};
    try{std::string line;{std::lock_guard lock(mutex);admitted=window.load();std::ostringstream out;out.imbue(std::locale::classic());out<<"{\"event\":\"sr_observation_summary\",\"window_id\":"<<admitted<<",\"device\":"<<device<<",\"callback_calls\":"<<calls.load()<<",\"callback_parse_attempts\":"<<parseAttempts.load()<<",\"callback_detail_samples\":"<<detailSamples.load()
        <<",\"accumulate_calls\":"<<passCalls[0].load()<<",\"rcas_calls\":"<<passCalls[1].load()
        <<",\"full_graph_observation\":"<<(fullGraphMode?"true":"false")<<",\"auxiliary_calls\":{";
        for(unsigned n=unsigned(p::Pass::Reactive);n<unsigned(p::Pass::Count);++n){if(n!=unsigned(p::Pass::Reactive))out<<',';out<<'"'<<p::passName(p::Pass(n))<<"\":"<<passCalls[n-1].load();}
        out<<'}'
        <<",\"registration_refusals\":"<<registrationRefusals.load()<<",\"candidate_evictions\":"<<evicted.load()<<",\"dispatch_candidate_matches\":"<<matches.load()<<",\"omitted_match_details\":"<<omittedMatches.load()
        <<",\"driver_returns\":"<<forwarded.load()<<",\"failures\":"<<failures.load()<<",\"callback_limit\":"<<callbackLimit<<",\"dispatch_detail_limit\":"<<dispatchDetailLimit
        <<",\"registration_calls\":"<<registrationCalls.load()<<",\"registration_limit\":"<<registrationLimit<<",\"dispatch_calls\":"<<dispatchCalls.load()<<",\"dispatch_observation_limit\":"<<dispatchObservationLimit
        <<",\"registration_bridge_before_failures_lifetime\":"<<control.beforeFailures.load()<<",\"registration_bridge_close_failures_lifetime\":"<<control.closeFailures.load()<<",\"registration_bridge_abnormal_closes_lifetime\":"<<control.abnormalCloses.load()
        <<",\"pinned_mv_jittered_default\":"<<(p::motionVectorsJittered?"true":"false")
        <<",\"depth_intervals\":{\"matched\":"<<depthIntervals.matched<<",\"missing_pair\":"<<depthIntervals.missing<<",\"ambiguous_pair\":"<<depthIntervals.ambiguous
        <<",\"queried\":"<<depthIntervals.queried<<",\"untouched\":"<<depthIntervals.untouched<<",\"unknown\":"<<depthIntervals.unknown
        <<",\"barrier_frames\":"<<depthIntervals.barriers<<",\"attachment_frames\":"<<depthIntervals.attachments
        <<",\"transfer_frames\":"<<depthIntervals.transfers<<",\"clear_frames\":"<<depthIntervals.clears<<'}'
        <<",\"runtime_mode\":"<<(runtimeMode?"true":"false")<<",\"active\":"<<(gate()?"true":"false")<<",\"capture_baselined\":"<<(captureBaselined?"true":"false")<<",\"waiting_capture\":"<<(ready.load()&&!gate()&&!stopped.load()?"true":"false")<<",\"execute_permitted\":false}";line=out.str();}
        emit(line,admitted,true);}catch(...){bump(admitted,failures,false);}
}
#ifdef XESS_SR_OBSERVATION_TEST
void resetForTest() noexcept {
    std::lock_guard lock(mutex);registrations={};candidates={};dispatchDiagnostics={};dispatchShadow={};consumerAudit={};nearMatches={};rawPairs={};rawPairSerial=0;depthIntervals={};following={};rcasFrames={};stage1Sources={};module=0;device=registrationSerial=0;sink=nullptr;frames=nullptr;frameUser=nullptr;resolveImages=nullptr;
    configured=false;sampling=false;stopped=false;ready=false;consumerFinished=false;runtimeMode=fullGraphMode=false;window=0;deadline=0;nextPoll=0;lastFile={};fileSeen=captureBaselined=false;
    calls=0;parseAttempts=0;detailSamples=0;failures=0;registrationRefusals=0;evicted=0;matches=0;omittedMatches=0;forwarded=0;control.sampling=false;
    for(auto& count:passCalls)count=0;
    registrationCalls=0;dispatchCalls=0;
}
void captureForTest(std::uint64_t identity) noexcept {ErrorGuard error;beginWindow({identity,1},GetTickCount64());}
void pollIdentityForTest(std::uint64_t identity,bool exists) noexcept {ErrorGuard error;inspectCapture(exists,{identity,1},GetTickCount64());}
void expireForTest() noexcept {deadline=0;}
#endif
}
extern "C" {
#define SR_NATIVE_PASS(name, pass) \
const xess_vk::registration_bridge::Route grb_sr_##name##_route{xess_vk::sr_observation::original_##name,xess_vk::sr_observation::before,xess_vk::sr_observation::close,reinterpret_cast<void*>(static_cast<std::uintptr_t>(xess_vk::upscale_tail_probe::Pass::pass)),&xess_vk::sr_observation::control}; \
void sr_##name##_observe_before(void* payload,void* graph) noexcept {xess_vk::sr_observation::observe(xess_vk::upscale_tail_probe::Pass::pass,payload,graph);}
#include "sr_native_passes.inc"
#undef SR_NATIVE_PASS
const xess_vk::registration_bridge::Route grb_sr_accumulate_route{xess_vk::sr_observation::originalAccumulate,xess_vk::sr_observation::before,xess_vk::sr_observation::close,reinterpret_cast<void*>(static_cast<std::uintptr_t>(xess_vk::upscale_tail_probe::Pass::Accumulate)),&xess_vk::sr_observation::control};
const xess_vk::registration_bridge::Route grb_sr_rcas_route{xess_vk::sr_observation::originalRcas,xess_vk::sr_observation::before,xess_vk::sr_observation::close,reinterpret_cast<void*>(static_cast<std::uintptr_t>(xess_vk::upscale_tail_probe::Pass::Rcas)),&xess_vk::sr_observation::control};
void sr_accumulate_observe_before(void* payload,void* graph) noexcept {xess_vk::sr_observation::observe(xess_vk::upscale_tail_probe::Pass::Accumulate,payload,graph);}
void sr_rcas_observe_before(void* payload,void* graph) noexcept {xess_vk::sr_observation::observe(xess_vk::upscale_tail_probe::Pass::Rcas,payload,graph);}
}
