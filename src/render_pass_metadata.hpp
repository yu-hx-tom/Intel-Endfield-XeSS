#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vulkan/vulkan_core.h>
#include "recorded_source_layout.hpp"
#include "image_inventory.hpp"

namespace xess_vk::render_pass_metadata {
using Handle = std::uint64_t;
struct Key {
    Handle handle{};
    std::uint64_t generation{};
    friend bool operator==(const Key&, const Key&) = default;
};
enum class Issue : std::uint64_t {
    None = 0, DriverFailed = 1ull << 0, InvalidInput = 1ull << 1,
    Capacity = 1ull << 2, Lifecycle = 1ull << 3, StaleGeneration = 1ull << 4,
    UnknownObject = 1ull << 5, UnsupportedExtension = 1ull << 6,
    UnsupportedImage = 1ull << 7, UnsupportedView = 1ull << 8,
    UnsupportedFormat = 1ull << 9, UnsupportedLayout = 1ull << 10,
    UnsupportedAttachment = 1ull << 11, MultipleSubpasses = 1ull << 12,
    Imageless = 1ull << 13, Secondary = 1ull << 14, DynamicRendering = 1ull << 15,
    Aliasing = 1ull << 16, IncompatibleFramebuffer = 1ull << 17,
    IncompleteRenderPass = 1ull << 18, SimultaneousUse = 1ull << 19,
    OneTimeResubmitted = 1ull << 20, DeviceLost = 1ull << 21, Stopped = 1ull << 22
};
constexpr std::uint64_t bit(Issue value) noexcept { return static_cast<std::uint64_t>(value); }
struct Result {
    Key key{};
    std::uint64_t issues{};
    bool accepted() const noexcept { return issues == 0; }
};

// A copied inventory record, supplied after observed successful image creation
// or an audited swapchain image enumeration. The parent inventory supplies a per-device GLOBAL strictly increasing
// image generation in observation order. Handle reuse never restarts the serial;
// replaying a late lookup of an older live image as a new creation is unsupported.
struct ImageDescription {
    Key key;
    VkImageType type{VK_IMAGE_TYPE_MAX_ENUM};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent3D extent{};
    std::uint32_t mipLevels{}, arrayLayers{};
    VkSampleCountFlagBits samples{};
    VkImageUsageFlags usage{};
    VkImageCreateFlags flags{};
    VkImageTiling tiling{VK_IMAGE_TILING_MAX_ENUM};
    VkSharingMode sharing{VK_SHARING_MODE_MAX_ENUM};
    bool unsupportedPNext{true}; // Explicitly clear only after auditing creation info.
};
enum class SourceKind { InitialLayoutAssertion, SubpassReferenceTransition, FinalLayoutTransition,
    UnusedInitialToFinalTransition }; // implicit RP transition, never an explicit barrier
struct LayoutOperation {
    Key image, view, renderPass, framebuffer, command;
    VkImageSubresourceRange range{};
    VkImageLayout oldLayout{VK_IMAGE_LAYOUT_UNDEFINED}, newLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    SourceKind sourceKind{SourceKind::InitialLayoutAssertion};
    // Supplied by the caller's common command-recording ordinal. Allows later
    // integration to merge these with actual explicit barriers without inventing
    // stage/access scopes. Initial assertion is not itself a transition.
    std::uint64_t recordOrder{};
    std::uint32_t attachment{}, sequenceWithinCall{};
};
inline constexpr std::size_t maxAttachments = 8, maxOperations = 96;
inline constexpr std::size_t projectionSlots = 3, maxProjectionPasses = 256;
inline constexpr std::size_t maxExplicitImageEvents=1024;
struct RecordedPrefixLiveness {
    std::array<image_inventory::Snapshot,maxExplicitImageEvents> images{};
    std::size_t count{};image_inventory::Snapshot source;
};
struct Batch {
    std::array<LayoutOperation, maxOperations> operations{};
    std::size_t count{};
    std::uint64_t issues{};
    bool driverSubmissionSucceeded{}, metadataAccepted{};
    // Deliberately never established by this metadata-only component.
    bool completeHookCoverage{}, gpuComplete{}, lastWriterKnown{}, resourceLifetimePinned{}, copyPermitted{};
};
struct Config {
    std::size_t images{1024}, views{2048}, renderPasses{256}, framebuffers{512}, commands{128};
    std::size_t operationsPerCommand{maxOperations};
    // Full pass identities, including non-candidates, remain retained/rechecked.
    // Independent of the 96 selected-layout-operation limit; no truncation.
    std::size_t projectionPassesPerCommand{maxProjectionPasses};
    std::size_t explicitImageEventsPerCommand{maxExplicitImageEvents};
};
struct Stats { std::uint64_t registrations{}, successfulSubmissions{}, refusedSubmissions{}, globalIssues{};std::size_t storageBytes{}; };

struct ProjectionSelection {
    // Every change, including disable, uses a per-device GLOBAL strictly
    // increasing serial. An image generation and explicit full range are required.
    std::uint64_t selectionSerial{};
    Key image;
    VkImageSubresourceRange range{};
    std::uint64_t sourceToken{}, passToken{}; // opaque correlation only; NOT frame IDs
    bool enabled{};
};
struct ProjectionTicket {
    Key command;
    std::uint64_t serial{}, issues{};
    std::array<ProjectionSelection, projectionSlots> frozenSelections{};
};
enum class ProjectionClassification { Disabled, Refused, DirectAttachmentLayouts, NoDirectAttachmentReference };
struct ProjectionBatch {
    ProjectionSelection selection;
    std::array<LayoutOperation, maxOperations> operations{};
    std::size_t count{}, examinedPasses{}, directPasses{}, noDirectPasses{};
    // Per-CB attempts/complete identity entries/dropped entries, including when
    // the result is refused. A dropped pass always leaves a sticky issue.
    std::uint64_t observedPasses{}, retainedPasses{}, droppedPasses{};
    std::uint64_t issues{};
    ProjectionClassification classification{ProjectionClassification::Disabled};
    bool metadataAccepted{};
    // Different VkImage identities do not prove disjoint memory bindings.
    bool memoryAliasUnverified{true};
    bool completeHookCoverage{}, gpuComplete{}, lastWriterKnown{}, resourceLifetimePinned{}, copyPermitted{};
};
struct ProjectionSubmission {
    std::array<ProjectionBatch, projectionSlots> slots{};
    std::uint64_t observedPasses{}, retainedPasses{}, droppedPasses{};
    std::uint64_t issues{}; // common structural/lifecycle issues, not local exclusions
    bool driverSubmissionSucceeded{};
};

// Read-only, copied diagnostic facts. These describe observed object creation,
// not validity, compatibility or synchronization. No application pointer survives.
struct ImageSnapshot { bool known{}; ImageDescription description; std::uint64_t issues{}; };
struct ViewSnapshot {
    bool known{}, hasPNext{}; Key key,image; std::uint64_t issues{};
    VkImageViewType type{}; VkFormat format{}; VkImageViewCreateFlags flags{};
    VkComponentMapping components{}; VkImageSubresourceRange rawRange{};
};
struct AttachmentSnapshot {
    VkFormat format{}; VkSampleCountFlagBits samples{}; VkAttachmentDescriptionFlags flags{};
    VkAttachmentLoadOp loadOp{},stencilLoadOp{}; VkAttachmentStoreOp storeOp{},stencilStoreOp{};
    VkImageLayout initial{},reference{},final{}; VkImageAspectFlags aspects{};
    std::uint32_t role{},referenceCount{}; bool hasPNext{};
};
struct RenderPassSnapshot {
    bool known{},hasPNext{}; Key key; std::uint64_t issues{}; VkRenderPassCreateFlags flags{};
    std::uint32_t attachmentCount{},subpassCount{},dependencyCount{},copiedAttachmentCount{};
    std::array<AttachmentSnapshot,maxAttachments> attachments{};
};
struct FramebufferSnapshot {
    bool known{},hasPNext{}; Key key,renderPass; std::uint64_t issues{}; VkFramebufferCreateFlags flags{};
    std::uint32_t width{},height{},layers{},attachmentCount{};
    std::array<Key,maxAttachments> views{};
};
struct CommandSnapshot {
    bool known{},inPass{}; Key key,activeRenderPass,activeFramebuffer; std::uint64_t issues{},lastOrder{};
    std::size_t operationCount{};
};

// Pure CPU, no Vulkan functions or GPU handles are dereferenced. One allocation
// at construction; fixed storage and no heap allocation in subsequent methods.
// The owner must serialize ALL calls, including destruction. No internal lock
// may be held across a downstream Vulkan call by a future forwarding adapter.
class Metadata {
public:
    explicit Metadata(Config = {});
    ~Metadata();
    Metadata(const Metadata&) = delete;
    Metadata& operator=(const Metadata&) = delete;
    // Base bookkeeping only; storageBytes() also includes allocated payload pages.
    static std::size_t allocatedStorageBytes() noexcept;
    std::size_t storageBytes() const noexcept;
    Result observeImage(const ImageDescription&, VkResult successfulCreateResult) noexcept;
    Result createView(Handle, const VkImageViewCreateInfo*, VkResult) noexcept;
    Result createRenderPass(Handle, const VkRenderPassCreateInfo*, VkResult) noexcept;
    Result createRenderPass2(Handle, const VkRenderPassCreateInfo2*, VkResult) noexcept; // core/KHR aliases
    Result createFramebuffer(Handle, const VkFramebufferCreateInfo*, VkResult) noexcept;
    void destroyImage(Key) noexcept;
    void destroyView(Handle) noexcept;
    void destroyRenderPass(Handle) noexcept;
    void destroyFramebuffer(Handle) noexcept;

    // command.generation is supplied after successful vkBeginCommandBuffer from
    // a per-device GLOBAL strictly increasing recording serial, including across
    // different command handles. Handle reuse/free never restarts the serial.
    Result beginCommand(Key command, VkCommandBufferLevel, const VkCommandBufferBeginInfo*, VkResult) noexcept;
    Result endCommand(Key, VkResult) noexcept;
    void resetCommand(Handle, VkResult) noexcept;
    void freeCommand(Handle) noexcept; // also call for pool reset/destroy/free
    Result beginRenderPass(Key, std::uint64_t order, const VkRenderPassBeginInfo*, VkSubpassContents) noexcept;
    Result beginRenderPass2(Key, std::uint64_t order, const VkRenderPassBeginInfo*, const VkSubpassBeginInfo*) noexcept;
    Result endRenderPass(Key, std::uint64_t order) noexcept;
    Result endRenderPass2(Key, std::uint64_t order, const VkSubpassEndInfo*) noexcept;
    Result nextSubpass(Key, std::uint64_t order) noexcept; // all legacy/2/KHR forms rejected in R0
    Result dynamicRendering(Key, std::uint64_t order) noexcept; // all Begin/End core/KHR forms rejected
    Result unsupported(Key, Issue) noexcept; // e.g. secondary execution or unaudited pNext

    // Call only AFTER forwarding the original driver submission. A failed
    // result never exposes a batch. Successful metadata is not GPU completion.
    Batch submitted(Key, VkResult driverResult) noexcept;
    // Independent, opt-in pure layout projection. These methods do not change
    // submitted() acceptance, counters or its one-time-submit accounting.
    // Successful beginCommand freezes selections. Any later change invalidates
    // that slot for that recording, even if the same image is selected again.
    Result setProjectionSelection(std::size_t slot, const ProjectionSelection&) noexcept;
    // One outstanding ticket per CB. Capture BEFORE forwarding, complete once
    // AFTER forwarding. Owner serialization still applies; tickets own no objects.
    ProjectionTicket captureProjectionSubmission(Key) noexcept;
    ProjectionSubmission projectSubmitted(const ProjectionTicket&, VkResult driverResult) noexcept;
    // Independent recorded-prefix query, reusing this command's all-pass journal.
    // No selection epoch or submitted() acceptance is used to seed a layout.
    void recordingPoint(Key,std::uint64_t order) noexcept;
    void unknownExplicitBarrier(Key,std::uint64_t order) noexcept;
    void explicitImageBarriers(Key,std::uint64_t order,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,
        std::uint32_t,const VkImageMemoryBarrier*,const image_inventory::Snapshot* ownedIdentities=nullptr) noexcept;
    void explicitImageBarriers2(Key,std::uint64_t order,const VkDependencyInfo*,const image_inventory::Snapshot* ownedIdentities=nullptr) noexcept;
    bool prefixImageKeys(Key,std::uint64_t prefixOrder,std::array<RecordedImageKey,maxExplicitImageEvents>&,std::size_t&) const noexcept;
    void imageTouched(Key,std::uint64_t order,Handle image,RecordedImageTouch) noexcept;
    RecordedImageInterval imageInterval(Key,std::uint64_t afterRead,std::uint64_t beforeUse,Key image) const noexcept;
    RecordedSourceLayout resolveSourceAt(Key,std::uint64_t prefixOrder,RecordedImageKey,const VkImageSubresourceRange&,VkImageLayout,
        const RecordedPrefixLiveness* ownedLiveIdentities=nullptr) noexcept;
    static std::size_t explicitJournalStorageBytesPerCommand() noexcept;
    Stats stats() const noexcept;
    ImageSnapshot imageSnapshot(Handle) const noexcept;
    ViewSnapshot viewSnapshot(Handle) const noexcept;
    RenderPassSnapshot renderPassSnapshot(Handle) const noexcept;
    FramebufferSnapshot framebufferSnapshot(Handle) const noexcept;
    CommandSnapshot commandSnapshot(Key) const noexcept;
    void stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xess_vk::render_pass_metadata
