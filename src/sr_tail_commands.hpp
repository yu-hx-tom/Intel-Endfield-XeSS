#pragma once
#include "command_recording_facts.hpp"
#include "frame_timing.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace xess_vk::sr_tail_commands {
inline constexpr std::size_t recordingCapacity=16,commandCapacity=8,barrierCapacity=8;
inline constexpr std::size_t setCapacity=8,dynamicOffsetCapacity=16,pushByteCapacity=256,attachmentCapacity=8;
enum class Pass {Unknown,Accumulate,Rcas};
enum class Origin {Game,Layer};
enum class Api {Core,Core2,Khr,Khr2};
enum class Kind {Other,Barrier,BindPipeline,BindDescriptorSets,PushConstants,BeginRenderPass,BeginRendering,
    Dispatch,DispatchIndirect,DispatchBase,Draw,DrawIndexed,DrawIndirect,DrawIndexedIndirect,DrawIndirectCount,DrawIndexedIndirectCount};
const char* name(Kind) noexcept;
const char* name(Pass) noexcept;
struct Key {
    std::uint64_t device{},incarnation{},command{},recording{},pool{},poolGeneration{};
    friend bool operator==(const Key&,const Key&)=default;
};
Key key(const CommandRecordingFacts&) noexcept;
struct MemoryBarrier {VkPipelineStageFlags2 srcStage{},dstStage{};VkAccessFlags2 srcAccess{},dstAccess{};};
struct BufferBarrier {
    MemoryBarrier scopes;
    std::uint64_t buffer{};VkDeviceSize offset{},size{};
    std::uint32_t srcQueueFamily{},dstQueueFamily{};
};
struct ImageBarrier {
    MemoryBarrier scopes;
    std::uint64_t image{};VkImageLayout oldLayout{},newLayout{};
    std::uint32_t srcQueueFamily{},dstQueueFamily{};
    VkImageSubresourceRange range{};
};
struct Barriers {
    VkDependencyFlags dependencyFlags{};
    VkPipelineStageFlags2 srcStage{},dstStage{};
    std::uint32_t memoryCount{},bufferCount{},imageCount{},storedMemory{},storedBuffers{},storedImages{};
    std::array<MemoryBarrier,barrierCapacity> memory{};
    std::array<BufferBarrier,barrierCapacity> buffers{};
    std::array<ImageBarrier,barrierCapacity> images{};
};
struct DescriptorSets {
    VkPipelineBindPoint bindPoint{};VkShaderStageFlags stages{};
    std::uint64_t layout{};
    std::uint32_t firstSet{},setCount{},storedSets{},dynamicOffsetCount{},storedOffsets{};
    std::array<std::uint64_t,setCapacity> sets{};
    std::array<std::uint32_t,dynamicOffsetCapacity> offsets{};
};
struct PushConstants {
    std::uint64_t layout{};VkShaderStageFlags stages{};
    std::uint32_t offset{},size{},storedBytes{};
    std::array<std::uint8_t,pushByteCapacity> bytes{};
};
struct Attachment {
    std::uint64_t view{},resolveView{};
    VkImageLayout layout{},resolveLayout{};VkResolveModeFlagBits resolveMode{};
    VkAttachmentLoadOp loadOp{};VkAttachmentStoreOp storeOp{};VkClearValue clear{};
};
struct RenderPass {
    std::uint64_t renderPass{},framebuffer{};VkRect2D area{};VkSubpassContents contents{};
    std::uint32_t clearCount{},storedClears{};
    std::array<VkClearValue,attachmentCapacity> clears{};
};
struct Rendering {
    VkRenderingFlags flags{};VkRect2D area{};
    std::uint32_t layerCount{},viewMask{},colorCount{},storedColors{};
    std::array<Attachment,attachmentCapacity> colors{};
    Attachment depth,stencil;bool hasDepth{},hasStencil{};
};
struct Command {
    Kind kind{Kind::Other};Api api{Api::Core};
    // Simple commands keep original scalar arguments in Vulkan signature order.
    // Signed scalars (e.g. DrawIndexed vertexOffset) use their sign-extended bits.
    std::array<std::uint64_t,8> arguments{};std::uint32_t argumentCount{},storedArguments{};
    std::array<char,64> otherName{};
    Barriers barriers;DescriptorSets descriptors;PushConstants push;RenderPass renderPass;Rendering rendering;
    bool truncated{},missingData{},extensionsOmitted{};
};
// Factories copy only bounded prefixes of the legal call's live input arrays.
// No Vulkan call, pNext pointer, application byte pointer or allocation is retained.
Command simple(Kind,std::span<const std::uint64_t> arguments={},Api=Api::Core) noexcept;
Command other(std::string_view,std::span<const std::uint64_t> arguments={}) noexcept;
Command barrier(VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,
    std::uint32_t,const VkMemoryBarrier*,std::uint32_t,const VkBufferMemoryBarrier*,
    std::uint32_t,const VkImageMemoryBarrier*) noexcept;
Command barrier2(const VkDependencyInfo*,Api=Api::Core2) noexcept;
Command bindDescriptorSets(VkPipelineBindPoint,VkPipelineLayout,std::uint32_t,std::uint32_t,
    const VkDescriptorSet*,std::uint32_t,const std::uint32_t*) noexcept;
Command bindDescriptorSets2(const VkBindDescriptorSetsInfo*,Api=Api::Core2) noexcept;
Command pushConstants(VkPipelineLayout,VkShaderStageFlags,std::uint32_t,std::uint32_t,const void*) noexcept;
Command pushConstants2(const VkPushConstantsInfo*,Api=Api::Core2) noexcept;
Command beginRenderPass(const VkRenderPassBeginInfo*,VkSubpassContents,Api=Api::Core) noexcept;
Command beginRenderPass2(const VkRenderPassBeginInfo*,const VkSubpassBeginInfo*,Api=Api::Core2) noexcept;
Command beginRendering(const VkRenderingInfo*,Api=Api::Core) noexcept;
struct Tail {
    Key recording;Pass pass{Pass::Unknown};
    std::uint64_t dispatchOrdinal{},recordOrder{},callback{};
    FrameTimingAssociation frame;
};
enum class End {RecordingEnded,WindowClosed};
struct Trace {
    std::uint64_t window{},replacements{},commandsSeen{};
    Tail tail;std::uint32_t count{};
    std::array<Command,commandCapacity> commands{};
    End end{End::WindowClosed};
};
static_assert(std::is_trivially_copyable_v<Command>&&std::is_trivially_copyable_v<Trace>);
struct Stats {
    std::uint64_t tails{},replacements{},duplicates{},commands{},ignoredLayer{},omittedAfterEight{},
        invalidInputs{},capacityRefusals{},finished{},retired{};
};
// One device, one dedicated SrObservation window. Construct only while that
// window is active; caller serializes methods and calls originals outside locks.
// Keep the latest matched tail until end-of-recording: RCAS can replace an
// Accumulate trace even after its eight-command prefix has filled.
class Observer {
    struct Slot {bool live{};Trace trace;};
    std::uint64_t window_{};
    std::array<Slot,recordingCapacity> slots_{};
    Stats stats_;
public:
    explicit Observer(std::uint64_t window) noexcept:window_(window){}
    std::uint64_t window() const noexcept{return window_;}
    bool tail(const Tail&,Origin=Origin::Game) noexcept;
    bool wantsCommand(const Key&) const noexcept;
    void command(const Key&,const Command&,Origin=Origin::Game) noexcept;
    bool finish(const Key&,Trace&) noexcept; // After actual successful vkEndCommandBuffer.
    void retire(const Key&) noexcept; // Successful reset/rebegin, or free observed.
    // Zero generation is for the actual synchronous reset/destroy hook; it
    // retires every captured recording of that currently live pool handle.
    void retirePool(std::uint64_t pool,std::uint64_t poolGeneration=0) noexcept;
    bool drain(Trace&) noexcept; // Window close: partial, never end-of-CB evidence.
    Stats stats() const noexcept{return stats_;}
};
}
