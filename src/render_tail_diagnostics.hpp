#pragma once
#include "command_recording_facts.hpp"
#include "descriptor_image_metadata.hpp"
#include "frame_timing.hpp"
#include "resource_memory_observer.hpp"
#include "wsi_image_inventory.hpp"
#include <array>
#include <memory>
#include <span>

namespace xess_vk::render_tail {
using Key = descriptor_image_metadata::Key;
inline constexpr std::size_t commandLimit = 16, eventLimit = 32, imageLimit = 16;
inline constexpr std::size_t regionLimit = 8, otherCommandLimit = 4, byteLimit = 1024 * 1024;
enum class Kind { Pass, Dispatch, Copy, Blit, Secondary };
enum Role : unsigned { Attachment = 1, Sampled = 2, Storage = 4, TransferSource = 8, TransferDestination = 16 };
struct Image {
    image_inventory::Record record;
    Key view{}, set{};
    std::uint64_t descriptorVersion{}, issues{};
    std::uint32_t role{}, setIndex{}, binding{}, arrayElement{}, descriptorType{};
    VkImageSubresourceRange range{};
    VkImageLayout layout{};
    resource_memory_observer::Observation memory;
    std::uint64_t layoutPrefix{},layoutIssues{},layoutLastOrder{},srcStage{},dstStage{},srcAccess{},dstAccess{};
    VkImageLayout recordedLayout{},barrierOldLayout{},barrierNewLayout{};
    bool layoutQueried{},layoutResolved{},layoutPrefixSupported{};
};
struct Region {
    VkImageSubresourceLayers source{}, destination{};
    VkOffset3D sourceOffsets[2]{}, destinationOffsets[2]{};
    VkExtent3D extent{};
};
struct Event {
    Kind kind{};
    CommandRecordingFacts recording;
    FrameTimingAssociation frame;
    std::uint64_t ordinal{}, pipeline{}, drawCount{}, imageDrops{}, pipelineChanges{}, issues{};
    Key renderPass{}, framebuffer{};
    VkRect2D renderArea{};
    std::array<Image, imageLimit> images{};
    std::size_t imageCount{};
    std::array<Region, regionLimit> regions{};
    std::size_t regionCount{}, regionDrops{};
    VkImageLayout sourceLayout{}, destinationLayout{};
    VkFilter filter{VK_FILTER_MAX_ENUM};
    std::array<Key, regionLimit> secondary{};
    std::size_t secondaryCount{}, secondaryDrops{};
};
struct Source {
    FrameTimingAssociation frame;
    std::uint64_t producer{}, registration{}, graphContext{};
    Key depth{}, motion{};
    std::array<std::uint64_t, 8> graphHandles{};
    std::array<std::uint32_t, 8> graphKeys{}, textureIds{};
    std::uint32_t renderWidth{}, renderHeight{}, upscaleWidth{}, upscaleHeight{};
    bool accepted{};
};
struct GraphicsState {
    std::uint64_t pipeline{}, layout{};
    std::array<std::uint64_t, descriptor_image_metadata::maxBoundSets> sets{};
    bool known{};
};
struct Ticket { std::uint64_t serial{}; };
struct Stats {
    std::uint64_t observed{}, overwritten{}, refused{}, submittedTargets{}, failedTargets{}, emittedSamples{};
    std::uint64_t emittedBytes{}, omittedLines{}, sourceObservations{}, presentAttempts{}, evictedCommands{};
    bool armed{}, stopped{};
};
using Sink = void (*)(const char*);
// Copied CPU metadata only. No Vulkan call, file, wait, pixel access or shader
// inference. One fixed allocation; no lock is held while delivering a sink.
class Store {
public:
    explicit Store(std::uint64_t device, Sink);
    ~Store();
    bool active() const noexcept;
    void arm() noexcept;
    void source(const Source&) noexcept;
    void wsiAdded(const wsi_image_inventory::OwnedImage&) noexcept;
    void wsiRemoved(Key) noexcept;
    void begin(const CommandRecordingFacts&, VkResult) noexcept;
    void end(const CommandRecordingFacts&, VkResult) noexcept;
    void reset(std::uint64_t command) noexcept;
    void resetPool(std::uint64_t pool) noexcept;
    void record(Event) noexcept;
    void endPass(const CommandRecordingFacts&) noexcept;
    void bindPipeline(const CommandRecordingFacts&, std::uint64_t) noexcept;
    void bindSets(const CommandRecordingFacts&, std::uint64_t layout, std::uint32_t first,
                  std::span<const std::uint64_t>) noexcept;
    GraphicsState graphics(const CommandRecordingFacts&) const noexcept;
    void draw(const CommandRecordingFacts&, std::span<const Image>) noexcept;
    Ticket beforeSubmit(std::uint64_t queue, std::uint64_t token, std::span<const Key>) noexcept;
    void submitted(std::uint64_t queue, std::uint64_t token, std::span<const Key>, VkResult) noexcept;
    void afterSubmit(Ticket, VkResult) noexcept;
    void present() noexcept;
    Stats stats() const noexcept;
    static std::size_t storageBytes() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace xess_vk::render_tail
