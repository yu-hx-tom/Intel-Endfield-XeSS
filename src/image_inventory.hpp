#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace xess_vk::image_inventory {
// Metadata from successful vkCreateImage calls only. A match is a point-in-time
// handle observation, not a resource pin, pixel interpretation or GPU fence.
struct Description {
    std::uint32_t format{}, width{}, height{}, depth{}, mipLevels{}, arrayLayers{};
    std::uint32_t samples{}, usage{}, flags{}, tiling{}, sharingMode{}, initialLayout{};
};
struct Record {
    std::uint64_t image{}, generation{};
    Description description{};
};
enum class Status { LiveHandleObserved, NotObserved, CapacityExceeded, DuplicateCreate, GenerationExhausted, Stopped };
inline const char* name(Status status) noexcept {
    switch (status) {
    case Status::LiveHandleObserved: return "live_handle_observed";
    case Status::NotObserved: return "not_observed";
    case Status::CapacityExceeded: return "capacity_exceeded";
    case Status::DuplicateCreate: return "duplicate_create";
    case Status::GenerationExhausted: return "generation_exhausted";
    default: return "stopped";
    }
}
struct Snapshot { Status status{Status::NotObserved}; Record record{}; std::size_t active{}; };

// Fixed storage, no allocation during image creation/destruction or lookup.
// Backshift deletion avoids a growing tombstone chain in long game sessions.
template<std::size_t Capacity = 32768> class Inventory {
    static_assert(Capacity >= 4 && (Capacity & (Capacity - 1)) == 0);
    std::array<Record, Capacity> slots_{};
    mutable std::mutex mutex_;
    std::size_t active_{};
    std::uint64_t generation_{};
    Status fault_{Status::NotObserved};
    static std::size_t bucket(std::uint64_t value) noexcept {
        value ^= value >> 30; value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value >> 27; value *= UINT64_C(0x94d049bb133111eb);
        return static_cast<std::size_t>(value ^ (value >> 31)) & (Capacity - 1);
    }
    static std::size_t next(std::size_t value) noexcept { return (value + 1) & (Capacity - 1); }
    std::size_t find(std::uint64_t image) const noexcept {
        std::size_t at = bucket(image);
        while (slots_[at].image && slots_[at].image != image) at = next(at);
        return at;
    }
public:
    static constexpr std::size_t maximumActive = Capacity / 2;
    void created(std::uint64_t image, const Description& description) {
        if (!image) return;
        std::lock_guard lock(mutex_);
        if (fault_ != Status::NotObserved) return;
        const auto at = find(image);
        if (slots_[at].image) { fault_ = Status::DuplicateCreate; return; }
        if (active_ >= maximumActive) { fault_ = Status::CapacityExceeded; return; }
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) { fault_ = Status::GenerationExhausted; return; }
        slots_[at] = {image, ++generation_, description}; ++active_;
    }
    // Caller removes the record BEFORE forwarding vkDestroyImage, so another
    // thread cannot receive a reused driver handle while the old record exists.
    void destroying(std::uint64_t image) {
        if (!image) return;
        std::lock_guard lock(mutex_);
        if (fault_ != Status::NotObserved) return;
        auto hole = find(image);
        if (!slots_[hole].image) return;
        for (auto scan = next(hole); slots_[scan].image; scan = next(scan)) {
            const auto home = bucket(slots_[scan].image);
            const auto distanceToScan = (scan - home) & (Capacity - 1);
            const auto distanceToHole = (hole - home) & (Capacity - 1);
            if (distanceToHole < distanceToScan) { slots_[hole] = slots_[scan]; hole = scan; }
        }
        slots_[hole] = {}; --active_;
    }
    Snapshot lookup(std::uint64_t image) const {
        std::lock_guard lock(mutex_);
        if (fault_ != Status::NotObserved) return {fault_, {}, active_};
        if (!image) return {Status::NotObserved, {}, active_};
        const auto& record = slots_[find(image)];
        return {record.image ? Status::LiveHandleObserved : Status::NotObserved, record, active_};
    }
    void stop() { std::lock_guard lock(mutex_); fault_ = Status::Stopped; }
};
} // namespace xess_vk::image_inventory
