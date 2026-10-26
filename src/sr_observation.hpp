#pragma once
#include "upscale_tail_probe.hpp"
#include "descriptor_image_metadata.hpp"
#include "command_recording_facts.hpp"
#include "frame_timing.hpp"
#include "graph_registration_bridge.hpp"
#include "recorded_source_layout.hpp"
#include "resource_memory_observer.hpp"
#include "upscale_frame_constants.hpp"
#include "sr_dispatch_shadow.hpp"
#include "sr_consumer_audit.hpp"

namespace xess_vk::sr_observation {
using Sink=void(*)(const char*);
using FrameReader=FrameTimingAssociation(*)(void*,std::uint32_t);
using Images=std::array<image_inventory::Snapshot,upscale_tail_probe::maxBindings>;
using ResolveImages=void(*)(std::uint64_t,const upscale_tail_probe::Result&,Images&) noexcept;
inline constexpr std::uint64_t callbackLimit=2048,dispatchDetailLimit=128;
inline constexpr std::uint64_t registrationLimit=4096,dispatchObservationLimit=262144;
inline constexpr std::uint64_t captureMilliseconds=120000;
inline constexpr std::size_t resourceSignatureHistogramLimit=64;
// Pinned installer only. All addresses/callbacks are immutable until process exit.
// After transaction postchecks, observation still waits for a capture-file identity.
bool configure(std::uintptr_t,std::uint64_t,Sink,FrameReader,void*,ResolveImages,bool runtime=false,bool fullGraph=false,bool replacement=false) noexcept;
bool activate() noexcept;
void stop() noexcept;
bool active(std::uint64_t) noexcept;
// Present-owned low-frequency file identity check. Never writes/deletes markers.
void pollCapture() noexcept;
void summary() noexcept;
struct Stage1Source {
    CommandRecordingFacts prepare,accumulate;
    descriptor_image_metadata::Key preparePipeline,accumulatePipeline;
    FrameTimingAssociation frame;
    // Input color, raw MV, raw depth, final game output, already referenced by
    // the application's same command-buffer recording.
    std::array<descriptor_image_metadata::ImageBinding,4> images{};
    frame_input::FrameConstantsSample constants;
    bool valid{},rcasExpected{};
};
struct DispatchTicket {
    upscale_tail_probe::Pass nativePass=upscale_tail_probe::Pass::Unknown;
    std::array<std::uint64_t,16> callbacks{};
    std::size_t count{};
    std::uint32_t detailMask{}; // Candidate/return detail quota never limits association.
    std::uint64_t command{},recording{},ordinal{};
    std::uint64_t window{};
    std::uint64_t consumerWindow{}; // Independent of candidate association; rejects late driver returns after rearm.
    std::uint64_t pipeline{},pipelineGeneration{};
    bool containsAccumulate{}; // Candidate pass only; no shader identity or Execute proof.
    bool containsRcas{};
    FrameTimingAssociation frame{};
    std::array<std::uint32_t,3> groups{};
    VkExtent2D renderExtent{},outputExtent{};
    bool extentsKnown{}; // Exact matched callback constants + typed live image extents.
    bool containsPrepare{}; // Pinned raw-input signature; requests shader evidence, not semantic proof.
    bool diagnosticArchiveCandidate{}; // Full-graph read-only capture only; never grants a pass/source match.
    Stage1Source source;
    bool terminalFsr{}; // Rcas when registered for this frame, otherwise Accumulate.
    sr_dispatch_shadow::Dispatch shadow;
    bool shadowObserved{}; // Full-graph read-only window only; never a suppression decision.
};
// Resource signature + exact frame/ticket match is a candidate association,
// never shader identity, GPU completion, color lifetime or Execute permission.
DispatchTicket beforeDispatch(std::uint64_t,const CommandRecordingFacts&,
    const descriptor_image_metadata::DispatchEvidence&,FrameTimingAssociation,
    std::uint32_t,RecordedSourceLayoutQuery,resource_memory_observer::SourceQuery,RecordedImageIntervalQuery={}) noexcept;
void afterDispatch(DispatchTicket,const char* api,bool fingerprintKnown=false) noexcept;
struct ConsumerImages {std::array<descriptor_image_metadata::Key,32> images{};std::size_t count{};bool complete{};};
struct ConsumerQueries {
    descriptor_image_metadata::SetImageSnapshot (*set)(void*,std::uint64_t) noexcept{};
    ConsumerImages (*attachments)(void*,const CommandRecordingFacts&,const sr_tail_commands::Command&) noexcept{};
    resource_memory_observer::SourceQuery memory;void* user{};
};
// Read-only full-graph windows only. Resolvers copy CPU metadata and must not
// call this observer or a Vulkan driver entrypoint while its mutex is held.
bool consumerActive(std::uint64_t) noexcept;
std::uint64_t consumerWindow(std::uint64_t) noexcept;
void consumerBegin(std::uint64_t,std::uint64_t,const sr_tail_commands::Key&) noexcept;
void consumerCommand(std::uint64_t,std::uint64_t,const CommandRecordingFacts&,const sr_tail_commands::Command&,ConsumerQueries) noexcept;
// True requests archiving of this exact dispatch pipeline after the collector
// lock is released. Bounded/deduplicated independently of reference samples.
bool consumerDispatch(std::uint64_t,std::uint64_t,const CommandRecordingFacts&,const descriptor_image_metadata::DispatchEvidence&,
    FrameTimingAssociation,int profile,descriptor_image_metadata::Key finalOutput,resource_memory_observer::SourceQuery) noexcept;
void consumerImage(std::uint64_t,std::uint64_t,const CommandRecordingFacts&,descriptor_image_metadata::Key,resource_memory_observer::SourceQuery) noexcept;
void consumerEnd(std::uint64_t,std::uint64_t,const sr_tail_commands::Key&,VkResult) noexcept;
#ifdef XESS_SR_OBSERVATION_TEST
void resetForTest() noexcept;
void captureForTest(std::uint64_t identity=1) noexcept;
void pollIdentityForTest(std::uint64_t identity,bool exists) noexcept;
void expireForTest() noexcept;
#endif
}
extern "C" {
#define SR_NATIVE_PASS(name, pass) \
void sr_##name##_observer_entry(); \
extern void* sr_##name##_original_trampoline; \
extern void* sr_##name##_registration_original; \
void sr_##name##_observe_before(void*,void*) noexcept; \
extern const xess_vk::registration_bridge::Route grb_sr_##name##_route; \
std::uint64_t grb_sr_##name##_entry(void*,std::uint32_t,const void*);
#include "sr_native_passes.inc"
#undef SR_NATIVE_PASS
void sr_accumulate_observer_entry();
void sr_rcas_observer_entry();
extern void* sr_accumulate_original_trampoline;
extern void* sr_rcas_original_trampoline;
extern void* sr_accumulate_registration_original;
extern void* sr_rcas_registration_original;
void sr_accumulate_observe_before(void*,void*) noexcept;
void sr_rcas_observe_before(void*,void*) noexcept;
extern const xess_vk::registration_bridge::Route grb_sr_accumulate_route;
extern const xess_vk::registration_bridge::Route grb_sr_rcas_route;
std::uint64_t grb_sr_accumulate_entry(void*,std::uint32_t,const void*);
std::uint64_t grb_sr_rcas_entry(void*,std::uint32_t,const void*);
}
