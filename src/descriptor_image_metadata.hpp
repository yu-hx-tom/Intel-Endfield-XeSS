#pragma once
#include "image_inventory.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vulkan/vulkan_core.h>

// Device-scoped, copied CPU facts only. No Vulkan call, pointer retention, pixels,
// shader semantic inference, descriptor lifetime pin, or GPU completion proof.
namespace xess_vk::descriptor_image_metadata {
using Handle=std::uint64_t;
struct Key { Handle handle{}; std::uint64_t generation{}; friend bool operator==(const Key&,const Key&)=default; };
// One word is a fast-path extent, never a resource/admission limit. Larger
// watched sets use the original complete membership query.
inline constexpr std::size_t referenceMaskBits=64;
struct ReferenceMaskSnapshot {Key set{};std::uint64_t version{},epoch{},mask{};bool known{};std::uint64_t publication{};};
// Stable paged-slot address; every published field is atomic so a concurrent
// reader either gets one coherent publication or falls back to the locked query.
class ReferenceMask {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> sequence_{2};
    std::array<std::atomic<std::uint64_t>,6> fields_{};
    mutable std::atomic<std::uint64_t> consumed_{};
public:
    ReferenceMask() noexcept=default;
    ReferenceMask(const ReferenceMask& other) noexcept {publish(other.read());}
    ReferenceMask& operator=(const ReferenceMask& other) noexcept {if(this!=&other)publish(other.read());return *this;}
    ReferenceMaskSnapshot read()const noexcept {
        const auto sequence=sequence_.load(std::memory_order_acquire);if(!sequence||(sequence&1))return {};
        std::array<std::uint64_t,6> values;for(unsigned n=0;n<values.size();++n)values[n]=fields_[n].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if(sequence!=sequence_.load(std::memory_order_relaxed))return {};
        return {{values[0],values[1]},values[2],values[3],values[4],values[5]!=0,sequence};
    }
    // Diagnostic only: identity probes do not consume a mask. Mark only the
    // coherent publication actually used by the draw guard.
    void consume(const ReferenceMaskSnapshot& value) const noexcept {
        if(!value.known||!value.publication)return;
        auto old=consumed_.load(std::memory_order_relaxed);
        while(old<value.publication&&!consumed_.compare_exchange_weak(old,value.publication,std::memory_order_relaxed)){}
    }
    bool consumed() const noexcept {return consumed_.load(std::memory_order_relaxed)==sequence_.load(std::memory_order_relaxed);}
    void publish(const ReferenceMaskSnapshot& value) noexcept {
        auto sequence=sequence_.load(std::memory_order_relaxed);
        if(sequence&&sequence<UINT64_MAX-1)sequence_.fetch_add(1,std::memory_order_acq_rel);
        else {sequence=0;sequence_.store(0,std::memory_order_release);}
        const std::array<std::uint64_t,6> values{value.set.handle,value.set.generation,value.version,value.epoch,value.mask,value.known};
        for(unsigned n=0;n<values.size();++n)fields_[n].store(values[n],std::memory_order_relaxed);
        sequence_.store(sequence?sequence+2:0,std::memory_order_release);
    }
};
struct ReferenceBinding {Key set{};const ReferenceMask* published{};};
// Device/runtime-owned hint. A stale publication falls back to the locked query.
class ReferenceBindingCache {
    std::array<ReferenceBinding,256> entries_{};
    static std::size_t index(Handle h) noexcept {return ((h>>4)^(h>>12)^(h>>20)^(h>>32))&255;}
public:
    ReferenceBinding find(Handle handle,std::uint64_t epoch) const noexcept {
        if(!handle||!epoch)return {};
        const auto entry=entries_[index(handle)];
        if(entry.set.handle!=handle||!entry.published)return {};
        const auto value=entry.published->read();
        return value.known&&value.set==entry.set&&value.epoch==epoch?entry:ReferenceBinding{};
    }
    void remember(ReferenceBinding entry) noexcept {if(entry.set.handle)entries_[index(entry.set.handle)]=entry;}
};
enum class Issue : std::uint64_t {
    None=0, DriverFailed=1ull<<0, InvalidInput=1ull<<1, Capacity=1ull<<2,
    UnknownObject=1ull<<3, StaleGeneration=1ull<<4, Lifecycle=1ull<<5,
    UnsupportedExtension=1ull<<6, UnsupportedDescriptor=1ull<<7,
    ImmutableSampler=1ull<<8, CrossBinding=1ull<<9, UndefinedDescriptor=1ull<<10,
    DescriptorChanged=1ull<<11, IncompatibleLayout=1ull<<12,
    Secondary=1ull<<13, UnsupportedDispatch=1ull<<14, UnsupportedView=1ull<<15,
    UnsupportedLayout=1ull<<16, Stopped=1ull<<17, GenerationExhausted=1ull<<18,
    MissingBinding=1ull<<19, SimultaneousUse=1ull<<20, TemplateReadFailed=1ull<<21
};
constexpr std::uint64_t bit(Issue value) noexcept { return static_cast<std::uint64_t>(value); }
struct Result { Key key{}; std::uint64_t issues{}; bool accepted()const noexcept{return issues==0;} };
// Keep compact graphics membership capacity separate from typed compute evidence.
inline constexpr std::size_t maxImageBindings=33,maxSetImageBindings=64,maxLayoutBindings=64,maxBoundSets=8,maxReferencedSets=512,maxUpdates=256;
inline constexpr std::size_t maxTemplateEntries=64,maxTemplateElements=256,maxTemplateBytes=16384,maxTemplateSpan=1048576;
inline constexpr std::size_t maxDescriptorSets=65536;
enum class TemplateUpdateKind {Core,Khr};
// Reads typed elements, coalescing adjacent bytes. A protected reader may opt in
// to bounded padded-span probes; failed probes fall back to exact element runs.
// Failed exact reads refuse the mutation. nullptr uses memcpy
// from the legal Vulkan call's live pData. No application pointer is retained.
using TemplateRead=std::size_t (*)(void* user,const void* source,void* destination,std::size_t bytes);
struct TemplateReader {TemplateRead read{};void* user{};bool allowSpanRead{};};
struct Config {
    std::size_t images{16384},views{32768},setLayouts{2048},pipelineLayouts{1024},pools{512},sets{maxDescriptorSets};
    std::size_t pipelines{4096},commandPools{256},commands{512},templates{1024};
    std::size_t referenceEdges{1u<<18}; // exact lifetime relations; objects beyond it fall back to bucket scans
    bool continuousDispatchSelection{}; // Keeps every mutation/binding; filters only known non-input Direct dispatches.
    bool srDispatchSelection{}; // Also admit the fixed three-image RCAS and ten-plus-image Accumulate layouts.
    bool compactOrdinaryDispatch{}; // Opt-in: retain ordinary consumer identities without SDK-ready evidence.
    bool measureReferenceConsumption{}; // Development counters only; never changes mask admission.
};
struct ImageBinding {
    std::uint32_t setIndex{},binding{},arrayElement{};
    VkDescriptorType descriptorType{VK_DESCRIPTOR_TYPE_MAX_ENUM};
    Key set{},setLayout{},pool{},view{},image{};
    std::uint64_t descriptorVersion{};
    VkImageViewType viewType{VK_IMAGE_VIEW_TYPE_MAX_ENUM};
    VkFormat viewFormat{VK_FORMAT_UNDEFINED};
    VkComponentMapping components{};
    VkImageSubresourceRange range{};
    VkImageLayout declaredLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    image_inventory::Description imageDescription{};
};
enum class DispatchKind { Direct, Indirect, Base, Unknown };
struct DispatchEvidence {
    Key command{},pipeline{},pipelineLayout{};
    std::uint64_t recordOrdinal{},issues{};
    std::array<std::uint32_t,3> groups{};
    std::array<ImageBinding,maxImageBindings> bindings{};
    std::size_t count{},ignoredNonImageBindingCount{};
    bool metadataAccepted{},filteredOut{};
    bool diagnosticSelectionBypassed{}; // Full read-only evidence for an otherwise filtered product dispatch.
    bool membershipOnly{}; // Guard bindings only; never accepted as SR/FG inputs or SDK restore state.
    std::size_t guardImageCount{};
    bool guardImagesKnown{}; // bindings[].image superset only; rejected count stays zero.
    // Full currently bound compute range, including opaque sets. Only static
    // descriptors qualify; unknown/dynamic state keeps the transfer fallback.
    std::array<Key,maxBoundSets> computeRestoreSets{};
    std::uint32_t computeRestoreSetCount{};
    std::uint32_t computeBoundSetHighWater{},computePipelineSetCount{};
    bool computeRestoreKnown{};
    bool computePushConstantsAbsent{};
    // Always false: even accepted evidence describes ordinary image bindings
    // only. It does not prove whole-command-buffer validity, submission
    // permission, shader usage, an atomic resource set, or a frame.
    bool completeResourceSet{},sameFrameVerified{},layoutVerified{},gpuComplete{};
    bool resourceLifetimePinned{},copyPermitted{},shaderSemanticsVerified{};
};
struct SetImageSnapshot {
    Key set{}, layout{}, pool{};
    std::uint64_t version{}, issues{};
    std::array<ImageBinding, maxImageBindings> bindings{};
    std::size_t count{};
    bool known{};
    // Image membership only: unused unwritten slots name no image. This never
    // qualifies the typed bindings or permits SDK/copy use.
    std::array<Key,maxSetImageBindings> referencedImages{};
    std::size_t referencedCount{},unwrittenCount{},layoutBindingCount{};
    std::uint64_t declaredImageCount{},setIssues{},layoutIssues{};
    bool imageReferencesKnown{};
};
// Consumer guards need image identities, not SDK-ready view/format payloads.
struct SetImageReferences {
    Key set{},layout{},pool{};
    std::uint64_t version{},issues{},declaredImageCount{},setIssues{},layoutIssues{};
    std::array<Key,maxSetImageBindings> referencedImages{};
    std::size_t referencedCount{},unwrittenCount{},layoutBindingCount{};
    bool known{},imageReferencesKnown{};
};
enum class ObjectKind : std::size_t { Images,Views,SetLayouts,PipelineLayouts,Pools,Sets,Pipelines,CommandPools,Commands,Templates,Count };
const char* objectName(ObjectKind) noexcept;
struct ObjectUsage { std::size_t live{},highWater{},capacity{}; std::uint64_t capacityRefusals{}; };
struct Stats {
    std::uint64_t referenceMaskBuilds{},referenceMaskImages{};
    std::uint64_t referenceMaskMeasuredBuilds{},referenceMaskRetiredBuilds{},referenceMaskConsumedBuilds{},referenceMaskUnconsumedBuilds{},referenceMaskPendingBuilds{};
    bool referenceConsumptionMeasured{};
    std::uint64_t referenceLifetimeScans{},referenceLifetimeCandidates{},referenceLifetimeInvalidations{};
    // Live exact image/view->set relations, and objects that fell back to bucket scans.
    std::uint64_t referenceEdges{},referenceEdgeOverflows{};
    std::size_t storageBytes{};
    std::array<ObjectUsage,static_cast<std::size_t>(ObjectKind::Count)> objects{};
    std::uint64_t registrations{},retirements{},mutations{},dispatches{},acceptedDispatches{},refusedDispatches{},filteredDispatches{};
    bool continuousDispatchSelection{};
    bool compactOrdinaryDispatch{};
    std::uint64_t compactPipelineClassifications{},compactDispatches{},compactUnknownDispatches{},compactImageReferences{};
    std::uint64_t capacityRefusals{},failedDriverCalls{},observedIssues{},fatalIssues{};
    // Successful driver calls, independent of metadata admission. Pool maxSets
    // is a declared upper bound; it is not the measured number of live sets.
    std::uint64_t driverSetAllocationCalls{},driverSetsAllocated{},driverSetFreeCalls{},driverSetsFreed{};
    std::uint64_t driverPoolResetCalls{},driverPoolDestroyCalls{},driverLiveSets{},driverSetHighWater{},declaredPoolMaxSets{};
    bool driverSetLedgerComplete{true};
    // Largest unique descriptor-set history of any one recording, fixed limit,
    // and refused new references. Overflow permanently refuses that recording.
    std::size_t referencedSetsHighWater{},referencedSetsCapacity{maxReferencedSets};
    std::uint64_t referenceOverflowRefusals{};
    std::uint64_t restoreRangeDisturbances{};
    std::uint64_t ordinaryUpdateCalls{},coreTemplateUpdateCalls{},khrTemplateUpdateCalls{},templateUpdateRefusals{},templateReadRefusals{};
    std::uint64_t templateReadCalls{},templateReadBytes{},templateReadElements{};
    std::uint64_t templateSpanReadCalls{},templateSpanReadSuccesses{},templateSpanReadFallbacks{};
    std::uint64_t templateSpanReadOversize{},templateSpanMaxBytes{};
    std::array<std::uint64_t,64> templateRefusedIssueRecords{};
    std::size_t liveImages{},liveViews{},liveSetLayouts{},livePipelineLayouts{},livePools{},liveSets{};
    std::size_t livePipelines{},liveCommandPools{},liveCommands{},liveTemplates{};
};

class Tracker {
public:
    // One fixed heap allocation at construction; Config may lower hard limits,
    // never raise them. Methods allocate nothing. Caller serializes EVERY method
    // and destruction (adapter mutex); no lock may span an original driver call.
    explicit Tracker(Config={}); ~Tracker();
    Tracker(const Tracker&)=delete; Tracker& operator=(const Tracker&)=delete;
    // Base bookkeeping only; storageBytes() also includes allocated payload pages.
    static std::size_t allocatedStorageBytes() noexcept;
    std::size_t storageBytes() const noexcept;
    void stop(Issue=Issue::Stopped) noexcept;
    Stats stats()const noexcept;

    // Exact common-inventory generation, never a second image generation domain.
    // Same-key identical observations are idempotent (e.g. WSI enumeration).
    // New registrations require that inventory's global increasing generation.
    Result imageObserved(const image_inventory::Record&) noexcept;
    void imageRetiring(Key) noexcept; // stale key never deletes a new generation
    Result viewCreated(VkImageView,const VkImageViewCreateInfo*,VkResult) noexcept;
    void viewDestroying(VkImageView) noexcept;
    Result setLayoutCreated(VkDescriptorSetLayout,const VkDescriptorSetLayoutCreateInfo*,VkResult) noexcept;
    void setLayoutDestroying(VkDescriptorSetLayout) noexcept;
    Result pipelineLayoutCreated(VkPipelineLayout,const VkPipelineLayoutCreateInfo*,VkResult) noexcept;
    void pipelineLayoutDestroying(VkPipelineLayout) noexcept;
    Result poolCreated(VkDescriptorPool,const VkDescriptorPoolCreateInfo*,VkResult) noexcept;
    void poolReset(VkDescriptorPool,VkResult) noexcept;
    void poolDestroying(VkDescriptorPool) noexcept;
    Result setsAllocated(const VkDescriptorSetAllocateInfo*,const VkDescriptorSet*,VkResult) noexcept;
    void setsFreed(VkDescriptorPool,std::uint32_t,const VkDescriptorSet*,VkResult) noexcept;
    // Actual core update ordering: all writes in order, then all copies in order.
    // Cross-binding writes/copies are conservatively rejected, not truncated.
    // An unknown/structurally unsupported mutation poisons the destination set
    // until reallocation. Known image-value errors are replaced by later writes.
    Result updateDescriptors(std::uint32_t,const VkWriteDescriptorSet*,std::uint32_t,const VkCopyDescriptorSet*) noexcept;
    Result templateCreated(VkDescriptorUpdateTemplate,const VkDescriptorUpdateTemplateCreateInfo*,VkResult) noexcept;
    void templateDestroying(VkDescriptorUpdateTemplate) noexcept;
    // Ordinary DESCRIPTOR_SET templates only. All entries/address arithmetic
    // and all typed reads succeed before any descriptor values are published.
    // Read/structure refusal poisons the destination without partial values;
    // destroying/reusing a template does not revoke already published writes.
    Result updateWithTemplate(VkDescriptorSet,VkDescriptorUpdateTemplate,const void*,TemplateUpdateKind=TemplateUpdateKind::Core,TemplateReader={}) noexcept;
    void setUnsupported(VkDescriptorSet,Issue=Issue::UnsupportedDescriptor) noexcept; // opaque mutation

    Result commandPoolCreated(VkCommandPool,const VkCommandPoolCreateInfo*,VkResult) noexcept;
    void commandPoolReset(VkCommandPool,VkResult) noexcept;
    void commandPoolDestroying(VkCommandPool) noexcept;
    Result commandsAllocated(const VkCommandBufferAllocateInfo*,const VkCommandBuffer*,VkResult) noexcept;
    void commandsFreed(std::uint32_t,const VkCommandBuffer*) noexcept;
    Result begin(VkCommandBuffer,const VkCommandBufferBeginInfo*,VkResult) noexcept;
    Result end(VkCommandBuffer,VkResult) noexcept;
    void reset(VkCommandBuffer,VkResult) noexcept;
    // VkCreateComputePipelines permits partial success: a nonnull returned
    // pipeline is a created object even if the original batch result failed.
    Result pipelineCreated(VkPipeline,const VkComputePipelineCreateInfo*,VkResult batchResult) noexcept;
    // Cold-path classification of an exact, archived non-FSR pipeline. Unknown
    // fingerprints retain full evidence; destruction/reuse loses this permission.
    bool allowCompactPipeline(Key) noexcept;
    void pipelineDestroying(VkPipeline) noexcept;
    void bindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline) noexcept;
    void bindSets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,std::uint32_t firstSet,
                  std::uint32_t,const VkDescriptorSet*,std::uint32_t dynamicOffsetCount,const std::uint32_t*) noexcept;
    void commandUnsupported(VkCommandBuffer,Issue=Issue::UnsupportedExtension) noexcept;
    DispatchEvidence preDispatch(VkCommandBuffer,std::uint32_t,std::uint32_t,std::uint32_t,
                                 DispatchKind=DispatchKind::Direct,bool diagnosticAllBindings=false) noexcept;
    // Diagnostic declaration snapshot, independent of compute dispatch state.
    SetImageSnapshot diagnosticSetImages(VkDescriptorSet) const noexcept;
    SetImageReferences imageReferences(VkDescriptorSet) const noexcept;
    Key setIdentity(VkDescriptorSet) const noexcept;
    std::uint64_t referenceEpoch()const noexcept;
    std::uint64_t watchReferenceImages(std::span<const Key>) noexcept;
    // Writes invalidate only. This query builds a dirty set's mask and exact
    // lifetime edges under the adapter lock, using its current copied facts.
    ReferenceBinding referenceBinding(VkDescriptorSet) noexcept;
    // Recheck copied CPU facts: exact objects, CB recording generation,
    // historical set revisions and view/image identities. This is not complete
    // CB lifetime/validity validation or permission for a later submission.
    Result validateEvidence(const DispatchEvidence&)const noexcept;
    Result validateImageBinding(const ImageBinding&)const noexcept;
    // Counts exact IMAGE KEY matches in accepted fixed evidence only; no frame,
    // property, shader-use or copy qualification follows from a positive count.
    static std::size_t matchingImage(const DispatchEvidence&,Key) noexcept;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace xess_vk::descriptor_image_metadata
