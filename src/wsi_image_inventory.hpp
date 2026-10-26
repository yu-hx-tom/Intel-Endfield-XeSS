#pragma once
#include "image_inventory.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace xess_vk::wsi_image_inventory {
using CommonInventory = image_inventory::Inventory<>;
using ImageRecord = image_inventory::Record;
inline constexpr std::size_t maximumChains = 16, maximumImages = 32, maximumPendingCreates = 16;
inline constexpr std::size_t maximumRemoved = maximumChains * maximumImages;

enum class Status {
    NotAttempted, CreatePending, ChainObserved, CountObserved, IncompleteIgnored,
    ImagesRegistered, ImagesRepeated, OldChainRetired, ChainDestroyed,
    UnknownChain, RetiredChain, StaleToken, InvalidToken, UnsupportedParameters,
    InvalidEnumeration, EnumerationChanged, DuplicateImage, ConflictingImage,
    OwnershipLost, DuplicateChain, CapacityExceeded, GenerationExhausted,
    InventoryFault, DriverFailure, Stopped, Exception
};
const char* name(Status) noexcept;

// Copy BEFORE the driver call. No Vulkan pointers are retained or dereferenced.
// First-version scope: copied=true, pNext absent, flags=0, arrayLayers=1,
// exclusive sharing (0), positive dimensions/count/format and known color-image
// usage bits only (0x9f). Other surface/presentation parameters are not inspected.
struct CreateDescription {
    bool copied{}, pNextPresent{true};
    std::uint32_t flags{}, minImageCount{}, format{}, width{}, height{};
    std::uint32_t arrayLayers{}, usage{}, sharingMode{};
};
struct CreateTicket { std::uint64_t serial{}; };
struct ChainToken { std::uint64_t chain{}, generation{}; };
struct EnumerationTicket {
    Status status{Status::UnknownChain};
    ChainToken chain;
    std::uint32_t inputCapacity{};
    bool hasImageArray{};
};
struct Enumeration {
    // Raw VkResult: 0=VK_SUCCESS, 5=VK_INCOMPLETE; every other value is failure.
    std::int32_t driverResult{};
    bool countCopied{};
    std::uint32_t returnedCount{};
    // Exactly returnedCount copied handles for an image-array call. The caller
    // must never copy beyond the captured input capacity or maximumImages.
    // Count-only/error calls need no handles. Lifetime lasts only for this call.
    std::span<const std::uint64_t> images;
};
struct OwnedImage {
    ImageRecord record;
    ChainToken owner;
    std::uint32_t index{};
};
struct Changes {
    Status status{Status::NotAttempted};
    image_inventory::Status inventoryStatus{image_inventory::Status::NotObserved};
    CreateTicket createTicket;
    ChainToken chain;
    CreateDescription description;
    std::array<OwnedImage, maximumImages> added{};
    std::array<OwnedImage, maximumRemoved> removed{};
    std::size_t addedCount{}, removedCount{};
    std::size_t ownershipMismatches{};
    bool oldSwapchainRetired{};
    // Metadata only. A matching generation does not pin an image or prove any
    // acquire/present/completion, layout, last writer, pixel or copy permission.
    bool acquiredVerified{}, presentedVerified{}, lifetimePinned{}, gpuCompletionVerified{}, copyPermitted{};
};

// No internal lock. Caller MUST serialize all methods and corresponding common
// inventory mutations under the SAME device state lock, without holding it over
// driver calls. Emit Changes to downstream metadata adapters before releasing
// that lock. CommonInventory must outlive this object. No allocation/driver/GPU
// call is performed here. Common inventory's created() supplies every image
// generation; there is no parallel WSI image-generation namespace.
class Inventory {
public:
    explicit Inventory(CommonInventory& inventory) noexcept : common_(inventory) {}
    Inventory(const Inventory&) = delete;
    Inventory& operator=(const Inventory&) = delete;
    // MUST run before vkCreateSwapchainKHR: retires/removes old metadata even if
    // parameters are unsupported or the subsequent driver call fails. No revive.
    Changes beforeCreate(std::uint64_t oldSwapchain, const CreateDescription&) noexcept;
    Changes afterCreate(CreateTicket, std::int32_t driverResult, std::uint64_t newSwapchain) noexcept;
    // Capture generation and input capacity BEFORE vkGetSwapchainImagesKHR; use
    // the returned ticket after the driver, rejecting destroy/recreate ABA.
    EnumerationTicket beforeGetImages(std::uint64_t chain, bool hasImageArray, std::uint32_t inputCapacity) const noexcept;
    Changes afterGetImages(const EnumerationTicket&, const Enumeration&) noexcept;
    // MUST run before forwarding vkDestroySwapchainKHR.
    Changes beforeDestroy(std::uint64_t chain) noexcept;
    Changes stop() noexcept;
#ifdef XESS_WSI_INVENTORY_TEST
    // Quiescent, empty fixture only. Does not change common image generations.
    bool setNextSerialForTest(std::uint64_t current) noexcept;
#endif
private:
    enum class Life { Empty, Ready, Retired };
    struct Chain {
        Life life{Life::Empty};
        ChainToken token;
        CreateDescription description;
        std::uint32_t knownCount{};
        bool complete{};
        std::array<ImageRecord, maximumImages> images{};
    };
    struct Pending { std::uint64_t serial{}; CreateDescription description; };
    CommonInventory& common_;
    std::array<Chain, maximumChains> chains_{};
    std::array<Pending, maximumPendingCreates> pending_{};
    std::uint64_t serial_{};
    Status fault_{Status::NotAttempted};
    Chain* find(std::uint64_t) noexcept;
    const Chain* find(std::uint64_t) const noexcept;
    bool nextSerial(std::uint64_t&, Changes&);
    void remove(Chain&, Changes&);
    void retire(Chain&, Changes&);
    void fault(Status, Changes&);
    void exception(Changes&) noexcept;
};
} // namespace xess_vk::wsi_image_inventory
