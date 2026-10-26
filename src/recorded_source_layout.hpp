#pragma once
#include "command_recording_facts.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vulkan/vulkan_core.h>

namespace xess_vk {
struct RecordedImageKey {std::uint64_t handle{},generation{};friend bool operator==(const RecordedImageKey&,const RecordedImageKey&)=default;};
enum class RecordedLayoutIssue : std::uint64_t {
    QueryMissing=1ull<<0, IdentityMismatch=1ull<<1, PrefixMismatch=1ull<<2,
    RecordingUnsupported=1ull<<3, UnknownObject=1ull<<4, StaleObject=1ull<<5,
    UnsupportedShape=1ull<<6, RangeAmbiguous=1ull<<7, NoTransitionAnchor=1ull<<8,
    ChainMismatch=1ull<<9, DeclaredLayoutMismatch=1ull<<10, UnsupportedPNext=1ull<<11,
    UnsupportedDependency=1ull<<12, OwnershipTransfer=1ull<<13, JournalOverflow=1ull<<14,
    OpaqueControl=1ull<<15, PassIdentityUnknown=1ull<<16, PassUnsupported=1ull<<17,
    CombinedAspectRequired=1ull<<18, UnsupportedBarrierScope=1ull<<19,
    UnsupportedLayout=1ull<<20
};
constexpr std::uint64_t bit(RecordedLayoutIssue r) noexcept{return static_cast<std::uint64_t>(r);}
enum class RecordedLayoutEventKind : std::uint32_t {Unknown,LegacyBarrier,Sync2Barrier,RenderPassInitialAssertion,RenderPassReference,RenderPassFinal};
struct RecordedLayoutEvent {
    std::uint64_t order{};std::uint32_t sequence{};
    RecordedLayoutEventKind kind{RecordedLayoutEventKind::Unknown};
    VkImageLayout oldLayout{VK_IMAGE_LAYOUT_UNDEFINED},newLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags2 srcStage{},dstStage{};VkAccessFlags2 srcAccess{},dstAccess{};
    bool explicitScopesKnown{};
};
struct RecordedSourceLayout {
    std::uint64_t device{},contextIncarnation{},command{},rpRecordingGeneration{},prefixOrder{};
    RecordedImageKey image;VkImageAspectFlags copiedAspect{};VkImageSubresourceRange resolvedLayoutRange{};
    VkImageLayout recordedLayout{VK_IMAGE_LAYOUT_UNDEFINED},declaredLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    RecordedLayoutEvent anchor,lastEvent;
    std::uint64_t issues{bit(RecordedLayoutIssue::QueryMissing)};
    std::uint64_t observedExplicitImages{},retainedExplicitImages{},droppedExplicitImages{};
    std::uint64_t explicitImageHighWater{},explicitImageCapacity{},explicitCallRefusals{};
    std::uint64_t examinedPasses{},directPasses{},noDirectPasses{},matchedEvents{},ignoredEqualSync2Events{};
    bool queried{},recordedLayoutResolved{},recordedPrefixSupported{};
    bool layoutVerified{},gpuComplete{},ownerVerified{},resourceLifetimePinned{},copyPermitted{};
};
using ResolveRecordedSourceLayout=RecordedSourceLayout(*)(void*,const CommandRecordingFacts&,RecordedImageKey,const VkImageSubresourceRange&,VkImageLayout) noexcept;
struct RecordedSourceLayoutQuery {ResolveRecordedSourceLayout resolve{};void* user{};};
struct RecordedImageInterval {
    std::uint64_t issues{bit(RecordedLayoutIssue::QueryMissing)};
    std::uint64_t barriers{},attachments{},transfers{},clears{};
    std::uint64_t lastExplicitOverflowOrder{},lastTouchOverflowOrder{};
    bool precedingOverflowIgnored{};
    bool queried{},untouched{};
};
using ResolveRecordedImageInterval=RecordedImageInterval(*)(void*,const CommandRecordingFacts&,const CommandRecordingFacts&,RecordedImageKey) noexcept;
struct RecordedImageIntervalQuery {ResolveRecordedImageInterval resolve{};void* user{};};
enum class RecordedImageTouch {Transfer,Clear};
static_assert(std::is_trivially_copyable_v<RecordedSourceLayout>);
} // namespace xess_vk
