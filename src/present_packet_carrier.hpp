#pragma once
#include "continuous_timing.hpp"
#include <array>
#include <cstdint>
#include <mutex>
#include <span>

namespace xess_vk::presentation_packet {
inline constexpr std::size_t maxWaits = 16, packetCapacity = 32;
struct ObjectIdentity {
    std::uint64_t handle{}, generation{};
    friend bool operator==(const ObjectIdentity&, const ObjectIdentity&) = default;
};
struct WaitIdentity {
    ObjectIdentity semaphore;
    std::uint64_t signalSubmission{};
    friend bool operator==(const WaitIdentity&, const WaitIdentity&) = default;
};
struct Key {
    std::uint64_t deviceIncarnation{}, queue{};
    ObjectIdentity swapchain;
    std::uint32_t imageIndex{}, waitCount{};
    std::array<WaitIdentity, maxWaits> waits{};
    bool valid{};
    friend bool operator==(const Key&, const Key&) = default;
};
struct IdentityStats {
    std::uint64_t invalidations{}, unknownObjects{}, duplicateHandles{}, capacityRefused{}, successfulSubmissions{};
    std::uint32_t liveSemaphores{}, liveSwapchains{};
    bool reliable{true};
};
// Actual successful driver create/destroy/submit observations only. Unknown or
// overflowed provenance fails closed; no borrowed handles receive fake epochs.
class IdentityRegistry {
  public:
    struct Submission {
        std::uint64_t serial{};
        std::uint32_t count{};
        std::array<ObjectIdentity, 64> signals{};
        bool valid{};
    };
    explicit IdentityRegistry(std::uint64_t deviceIncarnation);
    bool semaphoreCreated(std::uint64_t handle, std::int32_t driverResult, bool binary = true);
    void semaphoreDestroyed(std::uint64_t handle);
    bool swapchainCreated(std::uint64_t handle, std::int32_t driverResult);
    void swapchainDestroyed(std::uint64_t handle);
    bool successfulSignals(std::span<const std::uint64_t> handles, std::int32_t driverResult);
    // Production brackets the real driver call. A pending signal observation
    // prevents source/consumer from mistaking the previous signal serial for it.
    Submission beginSignals(std::span<const std::uint64_t> handles);
    bool completeSignals(const Submission&, std::int32_t driverResult);
    Key makeKey(std::uint64_t queue, std::uint64_t swapchain, std::uint32_t imageIndex,
                std::span<const std::uint64_t> waits);
    bool currentSwapchain(const Key&) const noexcept;
    // Real driver wait submission (private copy or original native present),
    // even with no carrier claim, retires this observed binary signal cycle.
    void consumed(const Key&);
    // Unsupported packets still retire their actual original wait handles.
    // If those handles cannot be copied safely, caller uses invalidate().
    void consumedWaits(std::span<const std::uint64_t> handles);
    void invalidate();
    IdentityStats stats() const;
  private:
    struct Object {
        ObjectIdentity identity;
        std::uint64_t signal{}, consumedThrough{};
        std::uint32_t pending{};
        bool available{};
        bool binary{true};
    };
    std::array<Object, 4096> semaphores_{};
    std::array<Object, 64> swapchains_{};
    mutable std::mutex mutex_;
    std::uint64_t device_{}, objectSerial_{}, submissionSerial_{};
    IdentityStats stats_;
};
struct Callbacks {
    void* user{};
    continuous_timing::PacketPin (*pin)(void*, FrameTimingAssociation, std::uint64_t contextGeneration){};
    FrameTimingAssociation (*resolve)(void*, continuous_timing::PacketPin){};
    void (*unpin)(void*, continuous_timing::PacketPin){};
};
struct Claim {
    std::uint64_t lease{};
    FrameTimingAssociation association;
    bool valid{};
};
struct Stats {
    std::uint64_t published{}, claimed{}, finished{}, misses{}, duplicates{}, rejected{}, cancelled{}, callbackFailures{};
    std::uint32_t occupied{}, inFlight{}, returnCallbacksInFlight{}, highWater{};
};
// All callbacks run outside this mutex. Caller keeps callback owners alive
// through stop + finish of any claimed lease. This ledger never waits on SDK.
class Ledger {
  public:
    explicit Ledger(Callbacks);
    bool publish(const Key&, FrameTimingAssociation, std::uint64_t contextGeneration) noexcept;
    Claim claim(const Key&, std::uint64_t contextGeneration) noexcept;
    bool claimCurrent(std::uint64_t lease, std::uint64_t contextGeneration) const noexcept;
    void finish(std::uint64_t lease) noexcept;
    void retireSwapchain(ObjectIdentity) noexcept;
    void retireContext(std::uint64_t contextGeneration) noexcept;
    // Reader/unsupported/provenance failures invalidate unclaimed candidates;
    // an in-flight claim is cancelled and stays pinned until finish.
    void invalidateUnclaimed() noexcept;
    void stop() noexcept;
    Stats stats() const;
  private:
    enum class State : std::uint8_t { Free, Publishing, Ready, Resolving, Claimed, Poisoned };
    struct Entry {
        Key key;
        continuous_timing::PacketPin pin;
        std::uint64_t serial{}, contextGeneration{};
        State state{};
        bool cancelled{}, poisoned{};
    };
    Callbacks callbacks_;
    mutable std::mutex mutex_;
    std::array<Entry, packetCapacity> entries_{};
    std::uint64_t serial_{};
    std::uint32_t returnCallbacksInFlight_{};
    bool stopped_{};
    Stats stats_;
    // A valid return is reserved in the same locked interval that clears its
    // entry. A zero inFlight count therefore also proves unpin has returned.
    void reserveReturn(continuous_timing::PacketPin) noexcept;
    void returnPin(continuous_timing::PacketPin) noexcept;
    void retire(bool all, ObjectIdentity, std::uint64_t contextGeneration) noexcept;
};
} // namespace xess_vk::presentation_packet
