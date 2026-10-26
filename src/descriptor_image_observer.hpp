#pragma once
#include "descriptor_image_metadata.hpp"
#include "command_recording_facts.hpp"
#include "recorded_source_layout.hpp"
#include "resource_memory_observer.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace xess_vk::descriptor_image_observer {
using Sink=void (*)(const char*);
using Key=descriptor_image_metadata::Key;
using Issue=descriptor_image_metadata::Issue;
using DispatchKind=descriptor_image_metadata::DispatchKind;
using DispatchEvidence=descriptor_image_metadata::DispatchEvidence;
using TemplateUpdateKind=descriptor_image_metadata::TemplateUpdateKind;
using TemplateReader=descriptor_image_metadata::TemplateReader;
enum class Source : unsigned {
    FiMotion, FiDepth, FiRealColor, RawMotion, RawDepth, RawColor,
    DilatedMotion, DilatedDepth, ReconstructedDepth, FarthestDepth, Luma, Count
};
inline constexpr std::size_t sourceCount=static_cast<std::size_t>(Source::Count);
inline constexpr std::uint64_t matchedDetailLimit=32,ordinaryDetailLimit=8,periodicInterval=1200;
inline constexpr std::uint64_t periodicSummaryLimit=128,explicitSummaryLimit=16;
inline constexpr std::uint64_t periodicMatchedDetailLimit=64,healthInterval=20000,healthSummaryLimit=128;
struct SourceSample { Key image{};std::uint64_t counter{}; };
struct DispatchSourceFacts {
    DispatchEvidence evidence;CommandRecordingFacts recording;
    std::array<SourceSample,sourceCount> sources{};
    std::array<std::uint32_t,descriptor_image_metadata::maxImageBindings> sourceMasks{};
    std::array<RecordedSourceLayout,descriptor_image_metadata::maxImageBindings> layouts{};
    std::array<resource_memory_observer::Observation,descriptor_image_metadata::maxImageBindings> memory{};
    bool observed{};
};
struct SourceQueryDemand {
    bool everyMatchedBinding{};bool rawPatternProbeLive{};
    bool diagnosticAllBindings{}; // Per-call bypass only; default product selection stays intact.
    bool (*requestProbeQuery)(void*) noexcept{};void* queryUser{};
};
struct Stats {
    descriptor_image_metadata::Stats metadata;
    std::array<SourceSample,sourceCount> sources{};
    std::uint64_t dispatchCalls{},matchedDispatches{},ordinaryDispatches{},matchedDetails{},ordinaryDetails{};
    std::uint64_t emittedGroups{},emittedBindings{},omittedGroups{},periodicSummaries{},omittedPeriodicSummaries{};
    std::uint64_t periodicMatchedDetails{},omittedPeriodicMatchedDetails{},healthSummaries{},omittedHealthSummaries{},finalSummaries{};
    std::uint64_t explicitSummaries{},omittedExplicitSummaries{},sourceObservations{},sourceChanges{},sourceClears{},invalidSources{};
    std::uint64_t validationCalls{},validationRefusals{},failures{};
    std::array<std::uint64_t,64> refusedIssueRecords{}; // per dispatch/bit, not exclusive categories
    bool retired{};
    struct ReferenceQuery {std::uint64_t calls{},sets{},contended{},totalNs{},waitNs{};};
    std::array<ReferenceQuery,2> referenceQueries{}; // bind and draw refresh; opt-in wall time, includes lock waits
    bool referenceQueryTiming{};
};
struct Context;
// One context per device. All copied metadata is mutex-serialized. The owner
// serializes destruction with EVERY call (including unlocked sink delivery).
// Object/source methods do not emit. preDispatch and summary emit only after
// releasing the mutex and must be called outside the owner's stateLock.
// Sinks may call stats(), but must not destroy/re-enter mutating hooks.
// All entrypoints preserve LastError. No original Vulkan function is called.
Context* create(VkDevice,Sink,descriptor_image_metadata::Config={}) noexcept;
void destroy(Context*) noexcept;
void retire(Context*) noexcept;
Stats stats(Context*) noexcept;
void summary(Context*) noexcept;

// These are historical numeric samples, not dispatch/frame associations. A
// zero key clears the slot; malformed partial-zero keys clear it as invalid.
// Call under the same outer lock as inventory lookup; never retain its pointer.
void observeSource(Context*,Source,Key,std::uint64_t sourceCounter) noexcept;
void imageObserved(Context*,const image_inventory::Record&) noexcept;
void imageRetiring(Context*,Key) noexcept;
void viewCreated(Context*,VkImageView,const VkImageViewCreateInfo*,VkResult) noexcept;
void viewDestroying(Context*,VkImageView) noexcept;
descriptor_image_metadata::Result setLayoutCreated(Context*,VkDescriptorSetLayout,const VkDescriptorSetLayoutCreateInfo*,VkResult) noexcept;
void setLayoutDestroying(Context*,VkDescriptorSetLayout) noexcept;
descriptor_image_metadata::Result pipelineLayoutCreated(Context*,VkPipelineLayout,const VkPipelineLayoutCreateInfo*,VkResult) noexcept;
void pipelineLayoutDestroying(Context*,VkPipelineLayout) noexcept;
void poolCreated(Context*,VkDescriptorPool,const VkDescriptorPoolCreateInfo*,VkResult) noexcept;
void poolReset(Context*,VkDescriptorPool,VkResult) noexcept;
void poolDestroying(Context*,VkDescriptorPool) noexcept;
void setsAllocated(Context*,const VkDescriptorSetAllocateInfo*,const VkDescriptorSet*,VkResult) noexcept;
void setsFreed(Context*,VkDescriptorPool,std::uint32_t,const VkDescriptorSet*,VkResult) noexcept;
void updateDescriptors(Context*,std::uint32_t,const VkWriteDescriptorSet*,std::uint32_t,const VkCopyDescriptorSet*) noexcept;
void templateCreated(Context*,VkDescriptorUpdateTemplate,const VkDescriptorUpdateTemplateCreateInfo*,VkResult) noexcept;
void templateDestroying(Context*,VkDescriptorUpdateTemplate) noexcept;
void updateWithTemplate(Context*,VkDescriptorSet,VkDescriptorUpdateTemplate,const void*,TemplateUpdateKind=TemplateUpdateKind::Core,TemplateReader={}) noexcept;
void setUnsupported(Context*,VkDescriptorSet,Issue=Issue::UnsupportedDescriptor) noexcept;
void commandPoolCreated(Context*,VkCommandPool,const VkCommandPoolCreateInfo*,VkResult) noexcept;
void commandPoolReset(Context*,VkCommandPool,VkResult) noexcept;
void commandPoolDestroying(Context*,VkCommandPool) noexcept;
void allocate(Context*,const VkCommandBufferAllocateInfo*,const VkCommandBuffer*,VkResult) noexcept;
void free(Context*,std::uint32_t,const VkCommandBuffer*) noexcept;
void begin(Context*,VkCommandBuffer,const VkCommandBufferBeginInfo*,VkResult) noexcept;
void end(Context*,VkCommandBuffer,VkResult) noexcept;
void reset(Context*,VkCommandBuffer,VkResult) noexcept;
// Feed each non-null result of vkCreateComputePipelines, even on a failed batch.
descriptor_image_metadata::Result pipelineCreated(Context*,VkPipeline,const VkComputePipelineCreateInfo*,VkResult batchResult) noexcept;
bool allowCompactPipeline(Context*,Key) noexcept;
void pipelineDestroying(Context*,VkPipeline) noexcept;
void bindPipeline(Context*,VkCommandBuffer,VkPipelineBindPoint,VkPipeline) noexcept;
void bindSets(Context*,VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,std::uint32_t firstSet,
              std::uint32_t,const VkDescriptorSet*,std::uint32_t,const std::uint32_t*) noexcept;
void commandUnsupported(Context*,VkCommandBuffer,Issue=Issue::UnsupportedExtension) noexcept;
DispatchEvidence preDispatch(Context*,VkCommandBuffer,std::uint32_t,std::uint32_t,std::uint32_t,
                             DispatchKind=DispatchKind::Direct,const CommandRecordingFacts* recordingFacts=nullptr,
                             RecordedSourceLayoutQuery={},resource_memory_observer::SourceQuery={},DispatchSourceFacts* ownedOutput=nullptr,SourceQueryDemand={}) noexcept;
// Explicit later CPU revalidation only; neither method proves complete CB
// validity, submission permission or GPU use. Success grants no copy permission.
descriptor_image_metadata::Result validateEvidence(Context*,const DispatchEvidence&) noexcept;
descriptor_image_metadata::Result validateImageBinding(Context*,const descriptor_image_metadata::ImageBinding&) noexcept;
descriptor_image_metadata::SetImageSnapshot diagnosticSetImages(Context*,VkDescriptorSet) noexcept;
descriptor_image_metadata::SetImageReferences imageReferences(Context*,VkDescriptorSet) noexcept;
descriptor_image_metadata::Key setIdentity(Context*,VkDescriptorSet) noexcept;
// Even nonzero revision identifies a stable CPU metadata snapshot. Zero is
// uncacheable (a mutation is in progress or the revision has exhausted).
std::uint64_t imageReferenceRevision(Context*) noexcept;
std::uint64_t imageReferences(Context*,std::span<const descriptor_image_metadata::Key>,
                              std::span<descriptor_image_metadata::SetImageReferences>) noexcept;
std::uint64_t setIdentities(Context*,std::span<const VkDescriptorSet>,
                           std::span<descriptor_image_metadata::Key>) noexcept;
std::uint64_t referenceMaskEpoch(Context*) noexcept;
std::uint64_t watchReferenceImages(Context*,std::span<const Key>) noexcept;
enum class ReferenceQuerySource : unsigned {Bind,Draw};
bool referenceBindings(Context*,std::span<const VkDescriptorSet>,std::span<descriptor_image_metadata::ReferenceBinding>,ReferenceQuerySource=ReferenceQuerySource::Bind) noexcept;
} // namespace xess_vk::descriptor_image_observer
