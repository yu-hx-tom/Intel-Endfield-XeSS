#pragma once
#include "frame_timing.hpp"
#include "game_frame_timing.hpp"
#include <cstdint>
#include <memory>

namespace xess_vk::continuous_timing {
struct Callbacks {
    void* user{};
    std::uint64_t (*contextGeneration)(void*){};
    FrameTimingTicket (*begin)(void*, std::uint32_t mappedSdkId){};
    void (*simulationEnd)(void*, FrameTimingTicket){};
    void (*renderBegin)(void*, FrameTimingTicket){};
    void (*renderEnd)(void*, FrameTimingTicket){};
    // Actual SDK ID the next ordinary OFF present will consume. Earlier
    // unassociated loading-screen presents can already have advanced it.
    std::uint32_t (*nextSdkId)(void*){};
    void (*abandon)(void*, FrameTimingTicket){};
};
struct Stats {
    std::uint64_t events{}, invalidKeys{}, rejected{}, saturated{}, callbackFailures{};
    std::uint64_t timedPresents{}, bootstrapPresents{}, contextResets{};
    std::uint64_t evicted{}, abandoned{}, lateCallbacks{}, untimedPresents{};
    std::uint64_t missingRecord{}, incompleteRecord{}, wrongOwner{}, outOfOrder{}, missingScope{};
    std::uint64_t scopeSaturated{}, recordSaturated{};
    std::uint64_t startupGapDeferrals{};
    std::uint32_t packetPins{};
    std::uint32_t occupied{}, pending{}, highWater{};
    bool ownerAllocatedIds{};
};
struct ScopeSnapshot {
    std::uint32_t simulationFrame{}, renderFrame{}, presentFrame{}, cpuFrame{};
    std::uint64_t contextGeneration{};
    bool threadObserved{}, inSimulation{}, inRender{}, inPresent{}, anchored{}, cpuFrameActive{};
};
struct PacketPin {
    FrameTimingAssociation association;
    std::uint64_t contextGeneration{}, recordSerial{}, policyEpoch{};
    bool valid{};
};
// Owns only bounded key/scope bookkeeping. The caller keeps callback owners
// alive. No coordinator mutex is held across a callback, including XeLL Sleep.
class Coordinator {
  public:
    explicit Coordinator(Callbacks);
    ~Coordinator();
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;
    bool event(const game_timing::Event&) noexcept;
    FrameTimingAssociation producer(std::uint32_t thread, ScopeSnapshot* diagnostic = nullptr) noexcept;
    FrameTimingAssociation registrationAssociation(std::uint32_t thread, std::uint32_t actualCpuKey,
                                                  bool keyValid, ScopeSnapshot* diagnostic = nullptr) noexcept;
    FrameTimingAssociation recordingAssociation(std::uint32_t thread,
                                               ScopeSnapshot* diagnostic = nullptr) noexcept;
    FrameTimingAssociation present(std::uint32_t thread) noexcept;
    FrameTimingAssociation packetAssociation(std::uint32_t sourceThread) noexcept;
    PacketPin pinPacket(FrameTimingAssociation, std::uint64_t contextGeneration) noexcept;
    FrameTimingAssociation resolvePacket(PacketPin) noexcept;
    void unpinPacket(PacketPin) noexcept;
    FrameTimingTicket frame(std::uint32_t engineKey) noexcept;
    bool bootstrapAllowed() noexcept;
    void presented(FrameTimingAssociation, bool success) noexcept;
    // Actual successful visible OFF consumes one owner ID. Old mapped tickets
    // are abandoned; all later begin callbacks receive 0 for owner allocation.
    void untimedPresented(std::uint32_t actualSdkId) noexcept;
    // Called only after a proven direct packet's completed native copy/output
    // reports untimed reservation pressure. No ticket or SDK ID is fabricated.
    bool deferStartupGap(std::uint32_t sourceThread) noexcept;
    void stop() noexcept;
    Stats stats() const;
    // Raw diagnostic flags/keys only. Inactive keys never grant association.
    ScopeSnapshot scopeSnapshot(std::uint32_t thread) const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xess_vk::continuous_timing
