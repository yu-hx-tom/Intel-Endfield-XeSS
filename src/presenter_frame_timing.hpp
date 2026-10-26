#pragma once
#include "frame_timing.hpp"
#include <algorithm>
#include <array>
#include <cstdint>

namespace xess_vk::detail {
struct PresenterTimingStats {
    std::uint32_t occupied{}, pending{}, highWater{};
    std::uint64_t evicted{}, cancelled{}, deferredCancellations{}, saturated{}, staleCompletions{};
    std::uint64_t invalidTicket{}, incompleteTicket{}, outOfOrder{}, duplicateMarker{};
};
// The caller holds its short timing mutex. Handles, never slot pointers, cross
// SDK calls. Cancelled slots stay pinned until every actual call has returned.
class PresenterFrameTimings {
  public:
    struct Timing {
        FrameTimingTicket ticket;
        std::uint64_t serial{};
        std::array<int, 4> results{};
        std::array<bool, 4> marked{}, pending{};
        int sleepResult{};
        bool slept{}, sleeping{}, presenting{}, cancelled{};
        bool busy() const {
            return sleeping || presenting || std::any_of(pending.begin(), pending.end(), [](bool v) { return v; });
        }
    };
    struct Handle {
        FrameTimingTicket ticket;
        std::uint64_t serial{};
        explicit operator bool() const { return bool(ticket) && serial; }
    };
    // Capability is fixed by fresh-context initialization after GPU quiescence.
    // Existing contexts never change sleep mode as timing reservations advance.
    explicit PresenterFrameTimings(std::uint64_t generation, bool latencyCapability = false)
        : generation_(generation), latencyCapability_(latencyCapability) {}
    Handle begin(std::uint32_t requested = 0) {
        if (untimedPending_) { ++counters_.saturated; return {}; }
        if (allocated_ == UINT32_MAX || (requested && requested <= allocated_)) {
            ++counters_.outOfOrder;
            return {};
        }
        auto available = std::find_if(slots_.begin(), slots_.end(), [](const auto& v) { return !v.ticket; });
        if (available == slots_.end()) {
            // Only Coordinator knows which native-present packet is protected
            // before Color admission. It must abandon a chosen victim first.
            ++counters_.saturated;
            return {};
        }
        *available = {};
        allocated_ = requested ? requested : allocated_ + 1;
        available->ticket = {allocated_, generation_, latencyCapability_ && timedPresented_};
        available->serial = ++serial_;
        available->sleeping = true;
        timingStarted_ = true;
        counters_.highWater = std::max(counters_.highWater, occupancy());
        return {available->ticket, available->serial};
    }
    bool completeBegin(Handle handle, int sleep, int simulation) {
        auto* value = find(handle);
        if (!value) { ++counters_.staleCompletions; return false; }
        value->sleeping = false;
        value->sleepResult = sleep;
        value->slept = sleep == 0;
        value->results[0] = simulation;
        value->marked[0] = sleep == 0 && simulation == 0;
        const bool accepted = !value->cancelled && value->marked[0];
        if (!accepted) cancel(*value);
        return accepted;
    }
    Handle marker(FrameTimingTicket ticket, unsigned index) {
        auto* value = find(ticket);
        if (!value || value->cancelled) { ++counters_.invalidTicket; return {}; }
        if (!index || index >= 4 || value->marked[index] || value->pending[index]) {
            ++counters_.duplicateMarker;
            return {};
        }
        if (!value->slept || !value->marked[0] || (index == 3 && !value->marked[2])) {
            ++counters_.incompleteTicket;
            return {};
        }
        value->pending[index] = true;
        return {value->ticket, value->serial};
    }
    bool completeMarker(Handle handle, unsigned index, int result) {
        auto* value = find(handle);
        if (!value || index >= 4) { ++counters_.staleCompletions; return false; }
        value->pending[index] = false;
        value->results[index] = result;
        value->marked[index] = result == 0;
        const bool accepted = !value->cancelled && result == 0;
        if (!accepted) cancel(*value);
        return accepted;
    }
    Handle admit(FrameTimingTicket ticket) {
        auto* value = find(ticket);
        if (!value || value->cancelled) { ++counters_.invalidTicket; return {}; }
        if (ticket.sdkId <= lastPresented_) { ++counters_.outOfOrder; return {}; }
        if (value->busy() || !value->slept ||
            !std::all_of(value->marked.begin(), value->marked.end(), [](bool v) { return v; })) {
            ++counters_.incompleteTicket;
            return {};
        }
        value->presenting = true;
        return {value->ticket, value->serial};
    }
    std::uint32_t admitBootstrap(std::uint32_t id = 0) {
        if (timedPresented_ || (timingStarted_ && !id)) { ++counters_.incompleteTicket; return 0; }
        if (id ? id <= lastPresented_ : allocated_ == UINT32_MAX) { ++counters_.outOfOrder; return 0; }
        allocated_ = id ? std::max(allocated_, id) : allocated_ + 1;
        return id ? id : allocated_;
    }
    std::uint32_t admitUntimed() {
        if (allocated_ == UINT32_MAX) { ++counters_.outOfOrder; return 0; }
        if (untimedPending_ || std::any_of(slots_.begin(), slots_.end(),
                [](const auto& value) { return bool(value.ticket); })) {
            ++counters_.incompleteTicket;
            // Neither a pending call nor a completed future reservation may be
            // overtaken by an invented OFF ID. The exact packet can still use
            // its original reservation; this frame remains native-only.
            return 0;
        }
        untimedPending_ = true;
        timedPresented_ = false;
        recoveryNeeded_ = true;
        return ++allocated_;
    }
    Timing snapshot(Handle handle) const {
        for (const auto& value : slots_)
            if (value.ticket == handle.ticket && value.serial == handle.serial) return value;
        return {};
    }
    void finish(Handle handle, std::uint32_t id, bool success) {
        if (handle) {
            auto* value = find(handle);
            if (!value) { ++counters_.staleCompletions; return; }
            value->presenting = false;
            if (!success) { cancel(*value); return; }
            *value = {};
            timedPresented_ = true;
        } else untimedPending_ = false;
        if (success) lastPresented_ = std::max(lastPresented_, id);
    }
    void abandon(FrameTimingTicket ticket) {
        if (auto* value = find(ticket)) cancel(*value);
    }
    void abandonAll() {
        for (auto& value : slots_) if (value.ticket) cancel(value);
    }
    std::uint32_t nextId() const { return allocated_ == UINT32_MAX ? 0 : allocated_ + 1; }
    std::uint32_t lastPresented() const { return lastPresented_; }
    bool recoveryNeeded() const { return recoveryNeeded_; }
    bool latencyCapability() const { return latencyCapability_; }
    void recovered() { recoveryNeeded_ = false; }
    PresenterTimingStats stats() const {
        auto result = counters_;
        result.occupied = occupancy();
        result.pending = static_cast<std::uint32_t>(std::count_if(slots_.begin(), slots_.end(),
            [](const auto& value) { return value.ticket && value.busy(); }));
        return result;
    }
  private:
    std::array<Timing, 8> slots_{};
    PresenterTimingStats counters_;
    std::uint64_t generation_{}, serial_{};
    std::uint32_t allocated_{}, lastPresented_{};
    bool timedPresented_{}, timingStarted_{}, recoveryNeeded_{}, untimedPending_{}, latencyCapability_{};
    std::uint32_t occupancy() const {
        return static_cast<std::uint32_t>(std::count_if(slots_.begin(), slots_.end(),
            [](const auto& value) { return bool(value.ticket); }));
    }
    Timing* find(FrameTimingTicket ticket) {
        if (!ticket || ticket.contextGeneration != generation_) return nullptr;
        for (auto& value : slots_) if (value.ticket == ticket) return &value;
        return nullptr;
    }
    Timing* find(Handle handle) {
        auto* value = find(handle.ticket);
        return value && value->serial == handle.serial ? value : nullptr;
    }
    void cancel(Timing& value) {
        if (!value.cancelled) {
            ++counters_.cancelled;
            if (value.busy()) ++counters_.deferredCancellations;
            value.cancelled = true;
        }
        if (!value.busy()) value = {};
    }
};
} // namespace xess_vk::detail
