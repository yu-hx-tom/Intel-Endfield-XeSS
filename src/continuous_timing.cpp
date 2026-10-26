#include "continuous_timing.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>

namespace xess_vk::continuous_timing {
struct Coordinator::Impl {
    struct Record {
        std::uint32_t key{};
        std::uint32_t simulationOwnerThread{};
        std::uint64_t serial{};
        std::uint32_t packetPins{};
        std::uint64_t contextGeneration{};
        FrameTimingTicket ticket;
        std::array<bool, 4> marked{}, pending{};
        bool used{}, cancelled{};
    };
    struct Scope {
        std::uint32_t thread{}, simulation{}, render{}, present{}, cpuFrame{};
        bool inSimulation{}, inRender{}, inPresent{}, cpuFrameActive{};
    };
    Callbacks callbacks;
    mutable std::mutex mutex;
    std::array<Record, 8> records{};
    std::array<Scope, 16> scopes{};
    Stats counters;
    std::uint64_t generation{}, serial{}, policyEpoch{};
    std::uint32_t anchor{}, anchorSdkId{}, lastPresented{}, lastSdkPresented{};
    bool anchored{}, presentedAny{}, timedReady{}, stopped{}, ownerAllocatedIds{};
    struct Returns {
        Impl& owner;
        std::array<FrameTimingTicket, 9> tickets{};
        std::size_t count{};
        void add(FrameTimingTicket ticket) { if (ticket && count < tickets.size()) tickets[count++] = ticket; }
        bool flush() {
            bool complete = true;
            const auto pending = count;
            count = 0;
            for (std::size_t index = 0; index < pending; ++index) {
                bool success = true;
                try { if (owner.callbacks.abandon) owner.callbacks.abandon(owner.callbacks.user, tickets[index]); }
                catch (...) { success = false; }
                std::lock_guard lock(owner.mutex);
                ++owner.counters.abandoned;
                if (!success) ++owner.counters.callbackFailures;
                complete = complete && success;
            }
            return complete;
        }
        ~Returns() { (void)flush(); }
    };
    void retire(Record& value, Returns& returns) {
        const bool closeScopes = !value.cancelled && value.contextGeneration == generation;
        value.cancelled = true;
        if (closeScopes) {
            for (auto& scope : scopes) {
                if (scope.cpuFrameActive && scope.cpuFrame == value.key) scope.cpuFrameActive = scope.inSimulation = false;
                if (scope.inRender && scope.render == value.key) scope.inRender = false;
            }
        }
        if (!value.packetPins && !std::any_of(value.pending.begin(), value.pending.end(), [](bool v) { return v; })) {
            returns.add(value.ticket);
            value = {};
        }
    }
    bool activePresent(std::uint32_t key) const {
        return std::any_of(scopes.begin(), scopes.end(), [=](const auto& scope) { return scope.inPresent && scope.present == key; });
    }
    explicit Impl(Callbacks value) : callbacks(value) {}
    bool synchronize() noexcept {
        Returns returns{*this};
        { std::lock_guard lock(mutex); if (stopped) return false; }
        std::uint64_t next{};
        try {
            if (callbacks.contextGeneration) next = callbacks.contextGeneration(callbacks.user);
        } catch (...) {
            std::lock_guard lock(mutex);
            ++counters.callbackFailures;
            return false;
        }
        std::lock_guard lock(mutex);
        if (stopped) return false;
        if (next != generation) {
            for (auto& record : records) if (record.used) retire(record, returns);
            scopes = {};
            generation = next;
            anchored = presentedAny = timedReady = false;
            ownerAllocatedIds = false;
            ++policyEpoch;
            lastSdkPresented = 0;
            ++counters.contextResets;
        }
        return true;
    }
    Scope* scope(std::uint32_t thread, bool create) {
        if (!thread) return nullptr;
        auto found = std::find_if(scopes.begin(), scopes.end(), [&](const auto& item) { return item.thread == thread; });
        if (found != scopes.end()) return &*found;
        if (!create) return nullptr;
        found = std::find_if(scopes.begin(), scopes.end(), [](const auto& item) {
            return !item.thread || (!item.inSimulation && !item.inRender && !item.inPresent && !item.cpuFrameActive);
        });
        if (found == scopes.end()) return nullptr;
        *found = {};
        found->thread = thread;
        return &*found;
    }
    Record* record(std::uint32_t key) {
        auto found = std::find_if(records.begin(), records.end(), [&](const auto& item) {
            return item.used && !item.cancelled && item.key == key && item.contextGeneration == generation;
        });
        return found == records.end() ? nullptr : &*found;
    }
    ScopeSnapshot snapshot(std::uint32_t thread) {
        ScopeSnapshot result;
        result.contextGeneration = generation;
        result.anchored = anchored;
        if (const auto* value = scope(thread, false)) {
            result.threadObserved = true;
            result.simulationFrame = value->simulation;
            result.renderFrame = value->render;
            result.presentFrame = value->present;
            result.cpuFrame = value->cpuFrame;
            result.inSimulation = value->inSimulation;
            result.inRender = value->inRender;
            result.inPresent = value->inPresent;
            result.cpuFrameActive = value->cpuFrameActive;
        }
        return result;
    }
    std::uint32_t mapped(std::uint32_t key) const {
        if (!anchored || key < anchor) return 0; // No unsigned wrap or backward guessing.
        const auto delta = static_cast<std::uint64_t>(key) - anchor;
        const auto id = delta + anchorSdkId;
        if (!anchorSdkId || id > std::numeric_limits<std::uint32_t>::max()) return 0;
        return static_cast<std::uint32_t>(id);
    }
    bool completed(const Record& value) const {
        return value.ticket && std::all_of(value.marked.begin(), value.marked.end(), [](bool marked) { return marked; });
    }
    FrameTimingAssociation association(std::uint32_t key, bool forPresent) {
        FrameTimingAssociation result;
        result.engineFrame = key;
        const auto id = mapped(key);
        if (forPresent && ((!ownerAllocatedIds && !id) || (presentedAny && key <= lastPresented))) {
            ++counters.outOfOrder;
            return result;
        }
        auto* value = record(key);
        if (forPresent && value && !completed(*value)) { ++counters.incompleteRecord; return result; }
        if (value) result.ticket = value->ticket;
        if (!forPresent) {
            result.valid = true; // A real producer scope can precede the SDK anchor.
            return result;
        }
        if (result.ticket) result.valid = true;
        else if (!timedReady && !ownerAllocatedIds) {
            result.bootstrapSdkId = id;
            result.valid = true;
        }
        if (!result.valid) ++counters.missingRecord;
        return result;
    }
};
Coordinator::Coordinator(Callbacks callbacks) : impl_(std::make_unique<Impl>(callbacks)) {}
Coordinator::~Coordinator() = default;

bool Coordinator::event(const game_timing::Event& event) noexcept {
    if (event.stage == game_timing::Stage::NativePresentEnqueue) return false;
    auto& p = *impl_;
    if (!p.synchronize()) return false;
    Impl::Returns returns{p};
    std::uint32_t anchorSdkId{};
    if (event.stage == game_timing::Stage::NativePresentStart && event.frameKeyValid) {
        bool needsAnchor{};
        { std::lock_guard lock(p.mutex); needsAnchor = !p.anchored && p.generation; }
        if (needsAnchor && p.callbacks.nextSdkId) {
            try { anchorSdkId = p.callbacks.nextSdkId(p.callbacks.user); }
            catch (...) {
                std::lock_guard lock(p.mutex);
                ++p.counters.callbackFailures;
                return false;
            }
        }
    }
    FrameTimingTicket ticket;
    std::uint64_t serial{}, generation{};
    std::uint32_t id{};
    unsigned marker{};
    bool begin{};
    {
        std::lock_guard lock(p.mutex);
        if (p.stopped) return false;
        ++p.counters.events;
        if (!event.frameKeyValid) {
            ++p.counters.invalidKeys;
            // A failed key read closes this thread's affected scope; it never
            // leaves a previous frame available as an accidental fallback.
            if (auto* scope = p.scope(event.threadId, false)) {
                switch (event.stage) {
                case game_timing::Stage::SimulationStart:
                case game_timing::Stage::SimulationEnd:
                    if (auto* record = p.record(scope->cpuFrame); record && record->simulationOwnerThread == event.threadId)
                        p.retire(*record, returns);
                    scope->inSimulation = false;
                    scope->cpuFrameActive = false;
                    break;
                case game_timing::Stage::RenderSubmitStart:
                case game_timing::Stage::RenderSubmitEnd: scope->inRender = false; break;
                case game_timing::Stage::NativePresentStart:
                case game_timing::Stage::NativePresentEnd: scope->inPresent = false; break;
                case game_timing::Stage::NativePresentEnqueue: break;
                }
            }
            return false;
        }
        auto* scope = p.scope(event.threadId, true);
        if (!scope) { ++p.counters.saturated; ++p.counters.scopeSaturated; return false; }
        const auto key = event.engineFrame;
        switch (event.stage) {
        case game_timing::Stage::SimulationStart:
            scope->cpuFrame = key;
            scope->cpuFrameActive = true;
            scope->simulation = key;
            scope->inSimulation = true;
            marker = 0;
            begin = true;
            break;
        case game_timing::Stage::SimulationEnd:
            if (scope->inSimulation && scope->simulation == key) scope->inSimulation = false;
            marker = 1;
            break;
        case game_timing::Stage::RenderSubmitStart:
            scope->render = key;
            scope->inRender = true;
            marker = 2;
            break;
        case game_timing::Stage::RenderSubmitEnd:
            if (scope->inRender && scope->render == key) scope->inRender = false;
            marker = 3;
            break;
        case game_timing::Stage::NativePresentStart:
            if (scope->inPresent && scope->present != key) {
                scope->inPresent = false;
                ++p.counters.rejected;
                return false;
            }
            scope->present = key;
            scope->inPresent = true;
            if (!p.anchored && p.generation && anchorSdkId) {
                p.anchor = key;
                p.anchorSdkId = anchorSdkId;
                p.anchored = true;
            }
            if (p.anchored && ((!p.ownerAllocatedIds && !p.mapped(key)) || (p.presentedAny && key <= p.lastPresented))) {
                scope->inPresent = false;
                ++p.counters.rejected;
                return false;
            }
            return true;
        case game_timing::Stage::NativePresentEnd:
            {
                const bool matched = scope->inPresent && scope->present == key;
                scope->inPresent = false;
                if (!matched)
                    ++p.counters.rejected;
                return matched;
            }
        case game_timing::Stage::NativePresentEnqueue: return false;
        }
        if (!p.anchored || !p.generation) return true;
        id = p.mapped(key);
        if ((!p.ownerAllocatedIds && !id) || (p.presentedAny && key <= p.lastPresented)) {
            ++p.counters.rejected; ++p.counters.outOfOrder; return false;
        }
        if (p.ownerAllocatedIds) id = 0;
        auto* record = p.record(key);
        if (begin && !record) {
            auto free = std::find_if(p.records.begin(), p.records.end(), [](const auto& item) { return !item.used; });
            if (free == p.records.end()) {
                for (auto it = p.records.begin(); it != p.records.end(); ++it)
                    if (!it->packetPins && !p.activePresent(it->key) &&
                        !std::any_of(it->pending.begin(), it->pending.end(), [](bool v) { return v; }) &&
                        (free == p.records.end() || it->serial < free->serial)) free = it;
                if (free == p.records.end()) { ++p.counters.saturated; ++p.counters.recordSaturated; return false; }
                p.retire(*free, returns);
                ++p.counters.evicted;
            }
            *free = {};
            free->used = true;
            free->key = key;
            free->contextGeneration = p.generation;
            free->serial = ++p.serial;
            record = &*free;
            p.counters.highWater = std::max(p.counters.highWater,
                static_cast<std::uint32_t>(std::count_if(p.records.begin(), p.records.end(), [](const auto& v) { return v.used; })));
        }
        if (!record || (!begin && (!record->marked[0] || !record->ticket)) ||
            (marker == 3 && !record->marked[2])) { ++p.counters.rejected; ++p.counters.missingRecord; return false; }
        if (event.stage == game_timing::Stage::SimulationEnd &&
            (!scope->cpuFrameActive || scope->cpuFrame != key || record->simulationOwnerThread != event.threadId)) {
            scope->cpuFrameActive = scope->inSimulation = false;
            ++p.counters.rejected;
            ++p.counters.wrongOwner;
            return false;
        }
        if (begin) {
            if (record->simulationOwnerThread && record->simulationOwnerThread != event.threadId) {
                scope->cpuFrameActive = scope->inSimulation = false;
                ++p.counters.rejected;
                return false;
            }
            record->simulationOwnerThread = event.threadId;
        }
        if (record->marked[marker] || record->pending[marker]) return true; // Same frame, another view.
        record->pending[marker] = true;
        ticket = record->ticket;
        serial = record->serial;
        generation = p.generation;
    }
    // Return the chosen victim before reserving another Color slot. Otherwise
    // the two full tables could independently evict different frames, including
    // a native-present packet protected here but not yet admitted by Color.
    if (!returns.flush()) {
        std::lock_guard lock(p.mutex);
        auto record = std::find_if(p.records.begin(), p.records.end(), [=](const auto& row) {
            return row.used && row.serial == serial;
        });
        if (record != p.records.end()) {
            // No begin/marker callback was entered. This reservation cannot
            // retain a pending call that will never come back to release it.
            record->pending[marker] = false;
            p.retire(*record, returns);
        }
        return false;
    }
    bool success{};
    try {
        if (begin) {
            if (p.callbacks.begin) ticket = p.callbacks.begin(p.callbacks.user, id);
            success = ticket && (!id || ticket.sdkId == id) && ticket.contextGeneration == generation;
        } else {
            const auto callback = marker == 1 ? p.callbacks.simulationEnd :
                                  marker == 2 ? p.callbacks.renderBegin : p.callbacks.renderEnd;
            if (callback) { callback(p.callbacks.user, ticket); success = true; }
        }
    } catch (...) {}
    {
        std::lock_guard lock(p.mutex);
        auto found = std::find_if(p.records.begin(), p.records.end(), [=](const auto& row) { return row.used && row.serial == serial; });
        auto* record = found == p.records.end() ? nullptr : &*found;
        if (p.stopped || p.generation != generation || !record || record->cancelled) {
            if (record) {
                record->pending[marker] = false;
                if (begin) record->ticket = ticket;
                p.retire(*record, returns);
            } else if (begin) returns.add(ticket);
            ++p.counters.lateCallbacks;
            return false;
        }
        if (begin && success && p.ownerAllocatedIds && ticket.sdkId <= p.lastSdkPresented) success = false;
        record->pending[marker] = false;
        record->marked[marker] = success;
        if (begin && success) record->ticket = ticket;
        if (!success) {
            ++p.counters.callbackFailures;
            if (begin) returns.add(ticket);
            p.retire(*record, returns);
        }
    }
    return success;
}
FrameTimingAssociation Coordinator::producer(std::uint32_t thread, ScopeSnapshot* diagnostic) noexcept {
    if (diagnostic)
        *diagnostic = {};
    if (!impl_->synchronize()) return {};
    std::lock_guard lock(impl_->mutex);
    if (diagnostic) {
        *diagnostic = impl_->snapshot(thread);
    }
    if (auto* scope = impl_->scope(thread, false)) {
        if (scope->inRender) return impl_->association(scope->render, false);
        if (scope->inSimulation) return impl_->association(scope->simulation, false);
    }
    return {};
}
FrameTimingAssociation Coordinator::registrationAssociation(std::uint32_t thread, std::uint32_t actualCpuKey,
                                                            bool keyValid, ScopeSnapshot* diagnostic) noexcept {
    if (diagnostic)
        *diagnostic = {};
    if (!impl_->synchronize())
        return {};
    std::lock_guard lock(impl_->mutex);
    auto& p = *impl_;
    if (diagnostic)
        *diagnostic = p.snapshot(thread);
    const auto* scope = p.scope(thread, false);
    if (!keyValid || !scope || !scope->cpuFrameActive || scope->cpuFrame != actualCpuKey)
        return {};
    const auto* record = p.record(actualCpuKey);
    if (!record || record->simulationOwnerThread != thread || !record->marked[0] || !record->marked[1] ||
        !record->ticket || record->ticket.contextGeneration != p.generation ||
        (p.presentedAny && actualCpuKey <= p.lastPresented))
        return {};
    return {actualCpuKey, record->ticket, 0, true};
}
FrameTimingAssociation Coordinator::recordingAssociation(std::uint32_t thread, ScopeSnapshot* diagnostic) noexcept {
    if (diagnostic)
        *diagnostic = {};
    if (!impl_->synchronize())
        return {};
    std::lock_guard lock(impl_->mutex);
    auto& p = *impl_;
    if (diagnostic)
        *diagnostic = p.snapshot(thread);
    const auto* scope = p.scope(thread, false);
    if (!scope || (!scope->inRender && !scope->inPresent) ||
        (scope->inRender && scope->inPresent && scope->render != scope->present))
        return {};
    const auto key = scope->inRender ? scope->render : scope->present;
    const auto* record = p.record(key);
    if (!record || !record->ticket || record->ticket.contextGeneration != p.generation ||
        (scope->inRender && !record->marked[2]) ||
        (p.presentedAny && key <= p.lastPresented))
        return {};
    return {key, record->ticket, 0, true};
}
FrameTimingAssociation Coordinator::present(std::uint32_t thread) noexcept {
    if (!impl_->synchronize()) return {};
    std::lock_guard lock(impl_->mutex);
    if (auto* scope = impl_->scope(thread, false); scope && scope->inPresent)
        return impl_->association(scope->present, true);
    ++impl_->counters.missingScope;
    return {};
}
FrameTimingAssociation Coordinator::packetAssociation(std::uint32_t thread) noexcept {
    if (!impl_->synchronize()) return {};
    auto& p = *impl_;
    std::lock_guard lock(p.mutex);
    const auto* scope = p.scope(thread, false);
    if (!scope || !scope->inPresent) return {};
    if (const auto* record = p.record(scope->present); record && record->ticket &&
        record->ticket.contextGeneration == p.generation && record->ticket.sdkId > p.lastSdkPresented)
        return {record->key, record->ticket, 0, true};
    return p.association(scope->present, true);
}
PacketPin Coordinator::pinPacket(FrameTimingAssociation association, std::uint64_t generation) noexcept {
    if (!association.valid || !impl_->synchronize()) return {};
    auto& p = *impl_;
    std::lock_guard lock(p.mutex);
    if (!generation || generation != p.generation || (p.presentedAny && association.engineFrame <= p.lastPresented)) return {};
    PacketPin result{association, generation, 0, p.policyEpoch, true};
    if (association.ticket) {
        auto* record = p.record(association.engineFrame);
        if (!record || record->ticket != association.ticket || record->ticket.sdkId <= p.lastSdkPresented ||
            record->packetPins == UINT32_MAX) return {};
        ++record->packetPins;
        result.recordSerial = record->serial;
    } else if (p.ownerAllocatedIds || p.timedReady || !association.bootstrapSdkId ||
               association.bootstrapSdkId != p.mapped(association.engineFrame) ||
               association.bootstrapSdkId <= p.lastSdkPresented) return {};
    return result;
}
FrameTimingAssociation Coordinator::resolvePacket(PacketPin pin) noexcept {
    if (!pin.valid || !impl_->synchronize()) return {};
    auto& p = *impl_;
    std::lock_guard lock(p.mutex);
    if (pin.contextGeneration != p.generation || (!pin.association.ticket && pin.policyEpoch != p.policyEpoch) ||
        (p.presentedAny && pin.association.engineFrame <= p.lastPresented)) return {};
    if (pin.association.ticket) {
        const auto* record = p.record(pin.association.engineFrame);
        if (!record || record->serial != pin.recordSerial || !record->packetPins ||
            record->ticket != pin.association.ticket || record->ticket.sdkId <= p.lastSdkPresented || !p.completed(*record)) return {};
    } else if (p.ownerAllocatedIds || p.timedReady || !pin.association.bootstrapSdkId ||
               pin.association.bootstrapSdkId != p.mapped(pin.association.engineFrame) ||
               pin.association.bootstrapSdkId <= p.lastSdkPresented) return {};
    return pin.association;
}
void Coordinator::unpinPacket(PacketPin pin) noexcept {
    if (!pin.valid || !pin.association.ticket) return;
    auto& p = *impl_;
    Impl::Returns returns{p};
    std::lock_guard lock(p.mutex);
    auto found = std::find_if(p.records.begin(), p.records.end(), [&](const auto& row) {
        return row.used && row.serial == pin.recordSerial && row.contextGeneration == pin.contextGeneration;
    });
    if (found != p.records.end() && found->packetPins) {
        --found->packetPins;
        if (found->cancelled) p.retire(*found, returns);
    }
}
FrameTimingTicket Coordinator::frame(std::uint32_t key) noexcept {
    if (!impl_->synchronize()) return {};
    std::lock_guard lock(impl_->mutex);
    if (auto* record = impl_->record(key)) return record->ticket;
    return {};
}
bool Coordinator::bootstrapAllowed() noexcept {
    if (!impl_->synchronize()) return false;
    std::lock_guard lock(impl_->mutex);
    return !impl_->stopped && !impl_->anchored &&
           std::none_of(impl_->records.begin(), impl_->records.end(), [](const auto& value) { return value.used; });
}
void Coordinator::presented(FrameTimingAssociation association, bool success) noexcept {
    if (!association.valid || !impl_->synchronize()) return;
    auto& p = *impl_;
    Impl::Returns returns{p};
    std::lock_guard lock(p.mutex);
    const auto id = p.ownerAllocatedIds ? association.ticket.sdkId : p.mapped(association.engineFrame);
    if (!id || (p.presentedAny && association.engineFrame <= p.lastPresented) ||
        (association.ticket && (association.ticket.contextGeneration != p.generation || association.ticket.sdkId != id)) ||
        (!association.ticket && association.bootstrapSdkId != id)) return;
    auto* record = p.record(association.engineFrame);
    if (association.ticket && (!record || record->ticket != association.ticket || !p.completed(*record))) return;
    if (record) p.retire(*record, returns);
    if (!success) return;
    p.lastPresented = association.engineFrame;
    p.lastSdkPresented = id;
    p.presentedAny = true;
    for (auto& prior : p.records)
        if (prior.used && prior.key <= association.engineFrame) p.retire(prior, returns);
    if (association.ticket) { p.timedReady = true; ++p.counters.timedPresents; }
    else ++p.counters.bootstrapPresents;
}
void Coordinator::untimedPresented(std::uint32_t actualSdkId) noexcept {
    auto& p = *impl_;
    if (!p.synchronize()) return;
    Impl::Returns returns{p};
    std::lock_guard lock(p.mutex);
    if (!actualSdkId || actualSdkId <= p.lastSdkPresented) { ++p.counters.outOfOrder; return; }
    for (auto& record : p.records)
        if (record.used && !(!record.ticket && record.pending[0]) &&
            (!record.ticket || record.ticket.sdkId <= actualSdkId)) p.retire(record, returns);
    p.ownerAllocatedIds = p.anchored = true;
    ++p.policyEpoch;
    p.timedReady = false;
    p.lastSdkPresented = actualSdkId;
    ++p.counters.untimedPresents;
}
bool Coordinator::deferStartupGap(std::uint32_t sourceThread) noexcept {
    if (!impl_->synchronize())
        return false;
    auto& p = *impl_;
    std::lock_guard lock(p.mutex);
    if (p.stopped || p.counters.timedPresents || !p.ownerAllocatedIds ||
        p.counters.startupGapDeferrals >= p.records.size())
        return false;
    const auto* scope = p.scope(sourceThread, false);
    if (!scope || !scope->inPresent || p.record(scope->present))
        return false;
    const bool future = std::any_of(p.records.begin(), p.records.end(), [&](const auto& record) {
        return record.used && !record.cancelled && record.key > scope->present && record.ticket &&
            record.ticket.contextGeneration == p.generation && record.ticket.sdkId > p.lastSdkPresented;
    });
    if (!future)
        return false;
    ++p.counters.startupGapDeferrals;
    return true;
}
void Coordinator::stop() noexcept {
    Impl::Returns returns{*impl_};
    std::lock_guard lock(impl_->mutex);
    impl_->stopped = true;
    ++impl_->policyEpoch;
    for (auto& record : impl_->records) if (record.used) impl_->retire(record, returns);
    impl_->scopes = {};
}
Stats Coordinator::stats() const {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->counters;
    result.occupied = static_cast<std::uint32_t>(std::count_if(impl_->records.begin(), impl_->records.end(), [](const auto& v) { return v.used; }));
    result.pending = static_cast<std::uint32_t>(std::count_if(impl_->records.begin(), impl_->records.end(), [](const auto& v) {
        return v.used && std::any_of(v.pending.begin(), v.pending.end(), [](bool p) { return p; });
    }));
    result.ownerAllocatedIds = impl_->ownerAllocatedIds;
    for (const auto& record : impl_->records) result.packetPins += record.packetPins;
    return result;
}
ScopeSnapshot Coordinator::scopeSnapshot(std::uint32_t thread) const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot(thread);
}
} // namespace xess_vk::continuous_timing
