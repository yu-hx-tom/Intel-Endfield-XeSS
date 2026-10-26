#include "present_packet_carrier.hpp"
#include <algorithm>
#include <limits>

namespace xess_vk::presentation_packet {
namespace {
template<class Range> auto object(Range& range, std::uint64_t handle) {
    return std::find_if(range.begin(), range.end(), [=](const auto& v) { return v.identity.handle == handle; });
}
}
IdentityRegistry::IdentityRegistry(std::uint64_t deviceIncarnation) : device_(deviceIncarnation) {
    stats_.reliable = device_ != 0;
}
bool IdentityRegistry::semaphoreCreated(std::uint64_t handle, std::int32_t result, bool binary) {
    if (result != 0) return false;
    std::lock_guard lock(mutex_);
    if (!stats_.reliable || !handle || objectSerial_ == UINT64_MAX) { stats_.reliable = false; return false; }
    if (object(semaphores_, handle) != semaphores_.end()) {
        ++stats_.duplicateHandles; stats_.reliable = false; return false;
    }
    auto free = object(semaphores_, 0);
    if (free == semaphores_.end()) { ++stats_.capacityRefused; stats_.reliable = false; return false; }
    free->identity = {handle, ++objectSerial_};
    free->binary = binary;
    ++stats_.liveSemaphores;
    return true;
}
void IdentityRegistry::semaphoreDestroyed(std::uint64_t handle) {
    if (!handle) return;
    std::lock_guard lock(mutex_);
    const auto found = object(semaphores_, handle);
    if (found == semaphores_.end()) { ++stats_.unknownObjects; stats_.reliable = false; return; }
    *found = {};
    --stats_.liveSemaphores;
}
bool IdentityRegistry::swapchainCreated(std::uint64_t handle, std::int32_t result) {
    if (result != 0) return false;
    std::lock_guard lock(mutex_);
    if (!stats_.reliable || !handle || objectSerial_ == UINT64_MAX) { stats_.reliable = false; return false; }
    if (object(swapchains_, handle) != swapchains_.end()) {
        ++stats_.duplicateHandles; stats_.reliable = false; return false;
    }
    auto free = object(swapchains_, 0);
    if (free == swapchains_.end()) { ++stats_.capacityRefused; stats_.reliable = false; return false; }
    free->identity = {handle, ++objectSerial_};
    ++stats_.liveSwapchains;
    return true;
}
void IdentityRegistry::swapchainDestroyed(std::uint64_t handle) {
    if (!handle) return;
    std::lock_guard lock(mutex_);
    const auto found = object(swapchains_, handle);
    if (found == swapchains_.end()) { ++stats_.unknownObjects; stats_.reliable = false; return; }
    *found = {};
    --stats_.liveSwapchains;
}
IdentityRegistry::Submission IdentityRegistry::beginSignals(std::span<const std::uint64_t> handles) {
    Submission result;
    if (handles.empty()) { result.valid = true; return result; }
    std::lock_guard lock(mutex_);
    if (!stats_.reliable || submissionSerial_ == UINT64_MAX) {
        stats_.reliable = false;
        ++stats_.capacityRefused;
        return result;
    }
    for (std::size_t index = 0; index < handles.size(); ++index) {
        const auto found = object(semaphores_, handles[index]);
        if (!handles[index] || found == semaphores_.end()) { ++stats_.unknownObjects; continue; }
        if (!found->binary) continue;
        if (result.count == result.signals.size()) {
            stats_.reliable = false;
            ++stats_.capacityRefused;
            return {};
        }
        if (found->pending == UINT32_MAX ||
            std::find(result.signals.begin(), result.signals.begin() + result.count, found->identity) != result.signals.begin() + result.count) {
            found->available = false;
            stats_.reliable = false;
            ++stats_.duplicateHandles;
            return {};
        }
        result.signals[result.count++] = found->identity;
    }
    result.serial = ++submissionSerial_;
    result.valid = true;
    for (std::size_t index = 0; index < result.count; ++index) ++object(semaphores_, result.signals[index].handle)->pending;
    return result;
}
bool IdentityRegistry::completeSignals(const Submission& submission, std::int32_t result) {
    if (!submission.valid) return false;
    std::lock_guard lock(mutex_);
    if (submission.count > submission.signals.size()) { stats_.reliable = false; return false; }
    bool complete = stats_.reliable;
    for (std::size_t index = 0; index < submission.count; ++index) {
        const auto found = object(semaphores_, submission.signals[index].handle);
        if (found == semaphores_.end() || found->identity != submission.signals[index] || !found->pending) {
            complete = stats_.reliable = false;
            ++stats_.unknownObjects;
            continue;
        }
        --found->pending;
        if (result == 0) {
            // Concurrent successful submits to one binary semaphore have no
            // ordered observation proof here. Never publish an older completion.
            if (submission.serial <= found->signal) { complete = stats_.reliable = false; continue; }
            found->signal = submission.serial;
            found->available = submission.serial > found->consumedThrough;
        }
    }
    if (result == 0 && submission.count) ++stats_.successfulSubmissions;
    return complete && result == 0;
}
bool IdentityRegistry::successfulSignals(std::span<const std::uint64_t> handles, std::int32_t result) {
    if (result != 0) return false;
    return completeSignals(beginSignals(handles), result);
}
Key IdentityRegistry::makeKey(std::uint64_t queue, std::uint64_t swapchain, std::uint32_t imageIndex,
                              std::span<const std::uint64_t> waits) {
    Key result;
    std::lock_guard lock(mutex_);
    if (!stats_.reliable || !queue || waits.empty() || waits.size() > maxWaits) return result;
    const auto chain = object(swapchains_, swapchain);
    if (!swapchain || chain == swapchains_.end()) { ++stats_.unknownObjects; stats_.reliable = false; return result; }
    result.deviceIncarnation = device_;
    result.queue = queue;
    result.swapchain = chain->identity;
    result.imageIndex = imageIndex;
    result.waitCount = static_cast<std::uint32_t>(waits.size());
    for (std::size_t index = 0; index < waits.size(); ++index) {
        const auto found = object(semaphores_, waits[index]);
        if (!waits[index] || found == semaphores_.end()) { ++stats_.unknownObjects; return {}; }
        if (!found->binary || found->pending || !found->signal || !found->available ||
            std::find(waits.begin(), waits.begin() + index, waits[index]) != waits.begin() + index) return {};
        result.waits[index] = {found->identity, found->signal};
    }
    result.valid = true;
    return result;
}
bool IdentityRegistry::currentSwapchain(const Key& key) const noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (!key.valid || !stats_.reliable || key.deviceIncarnation != device_) return false;
        const auto found = object(swapchains_, key.swapchain.handle);
        return found != swapchains_.end() && found->identity == key.swapchain;
    } catch (...) { return false; }
}
void IdentityRegistry::consumed(const Key& key) {
    if (!key.valid || key.deviceIncarnation != device_ || key.waitCount > maxWaits) return;
    std::lock_guard lock(mutex_);
    for (std::size_t index = 0; index < key.waitCount; ++index) {
        const auto found = object(semaphores_, key.waits[index].semaphore.handle);
        if (found != semaphores_.end() && found->identity == key.waits[index].semaphore &&
            found->signal == key.waits[index].signalSubmission) {
            found->available = false;
            found->consumedThrough = std::max(found->consumedThrough, found->signal);
        }
    }
}
void IdentityRegistry::consumedWaits(std::span<const std::uint64_t> handles) {
    std::lock_guard lock(mutex_);
    for (const auto handle : handles) {
        const auto found = object(semaphores_, handle);
        if (!handle || found == semaphores_.end()) { ++stats_.unknownObjects; stats_.reliable = false; continue; }
        found->available = false;
        found->consumedThrough = std::max(found->consumedThrough,
            found->pending ? submissionSerial_ : found->signal);
    }
}
void IdentityRegistry::invalidate() {
    std::lock_guard lock(mutex_);
    stats_.reliable = false;
    ++stats_.invalidations;
}
IdentityStats IdentityRegistry::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

Ledger::Ledger(Callbacks callbacks) : callbacks_(callbacks) {}
void Ledger::reserveReturn(continuous_timing::PacketPin pin) noexcept {
    if (pin.valid) ++returnCallbacksInFlight_;
}
void Ledger::returnPin(continuous_timing::PacketPin pin) noexcept {
    if (!pin.valid) return;
    bool failed{};
    try { if (callbacks_.unpin) callbacks_.unpin(callbacks_.user, pin); }
    catch (...) { failed = true; }
    std::lock_guard lock(mutex_);
    if (failed) ++stats_.callbackFailures;
    --returnCallbacksInFlight_;
}
bool Ledger::publish(const Key& key, FrameTimingAssociation association, std::uint64_t generation) noexcept {
    if (!key.valid || !association.valid || !generation) { invalidateUnclaimed(); return false; }
    std::uint64_t serial{};
    continuous_timing::PacketPin duplicate;
    bool full{};
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || serial_ == UINT64_MAX) { ++stats_.rejected; return false; }
        auto existing = std::find_if(entries_.begin(), entries_.end(), [&](const auto& v) { return v.state != State::Free && v.key == key; });
        if (existing != entries_.end()) {
            ++stats_.duplicates;
            existing->cancelled = true;
            existing->poisoned = true;
            if (existing->state == State::Ready) {
                duplicate = existing->pin;
                reserveReturn(duplicate);
                existing->pin = {};
                existing->state = State::Poisoned;
            }
        } else {
            auto free = std::find_if(entries_.begin(), entries_.end(), [](const auto& v) { return v.state == State::Free; });
            if (free == entries_.end()) { ++stats_.rejected; full = true; }
            else {
                free->key = key;
                free->contextGeneration = generation;
                free->serial = serial = ++serial_;
                free->state = State::Publishing;
                stats_.highWater = std::max(stats_.highWater,
                    static_cast<std::uint32_t>(std::count_if(entries_.begin(), entries_.end(), [](const auto& v) { return v.state != State::Free; })));
            }
        }
    }
    returnPin(duplicate);
    if (full) { invalidateUnclaimed(); return false; }
    if (!serial) return false;
    continuous_timing::PacketPin pin;
    try { if (callbacks_.pin) pin = callbacks_.pin(callbacks_.user, association, generation); }
    catch (...) { std::lock_guard lock(mutex_); ++stats_.callbackFailures; }
    bool accepted{};
    {
        std::lock_guard lock(mutex_);
        auto found = std::find_if(entries_.begin(), entries_.end(), [=](const auto& v) { return v.serial == serial; });
        if (found != entries_.end() && found->state == State::Publishing) {
            if (pin.valid && !found->cancelled && !stopped_) {
                found->pin = pin;
                found->state = State::Ready;
                accepted = true;
                ++stats_.published;
            } else {
                found->state = found->poisoned && !stopped_ ? State::Poisoned : State::Free;
                if (found->state == State::Free) *found = {};
            }
        }
        if (!accepted) reserveReturn(pin);
    }
    if (!accepted) returnPin(pin);
    return accepted;
}
Claim Ledger::claim(const Key& key, std::uint64_t generation) noexcept {
    if (!key.valid || !generation) { invalidateUnclaimed(); return {}; }
    std::uint64_t serial{};
    continuous_timing::PacketPin pin;
    continuous_timing::PacketPin refused;
    {
        std::lock_guard lock(mutex_);
        auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& v) { return v.state != State::Free && v.key == key; });
        if (stopped_ || found == entries_.end() || found->state != State::Ready || found->contextGeneration != generation) {
            ++stats_.misses;
            if (found != entries_.end()) {
                if (found->state == State::Publishing || found->state == State::Resolving || found->state == State::Claimed)
                    found->cancelled = true;
                else {
                    refused = found->pin;
                    reserveReturn(refused);
                    *found = {};
                }
            }
        } else {
            found->state = State::Resolving;
            serial = found->serial;
            pin = found->pin;
        }
    }
    returnPin(refused);
    if (!serial) return {};
    FrameTimingAssociation association;
    try { if (callbacks_.resolve) association = callbacks_.resolve(callbacks_.user, pin); }
    catch (...) { std::lock_guard lock(mutex_); ++stats_.callbackFailures; }
    Claim result;
    {
        std::lock_guard lock(mutex_);
        auto found = std::find_if(entries_.begin(), entries_.end(), [=](const auto& v) { return v.serial == serial; });
        if (found != entries_.end() && found->state == State::Resolving) {
            if (!found->cancelled && !stopped_ && association.valid) {
                found->state = State::Claimed;
                result = {serial, association, true};
                ++stats_.claimed;
            } else *found = {};
        }
        if (!result.valid) reserveReturn(pin);
    }
    if (!result.valid) returnPin(pin);
    return result;
}
bool Ledger::claimCurrent(std::uint64_t lease, std::uint64_t context) const noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (!lease || !context || stopped_) return false;
        const auto found = std::find_if(entries_.begin(), entries_.end(), [=](const auto& entry) {
            return entry.serial == lease && entry.state == State::Claimed && entry.contextGeneration == context &&
                !entry.cancelled && !entry.poisoned;
        });
        return found != entries_.end();
    } catch (...) { return false; }
}
void Ledger::finish(std::uint64_t serial) noexcept {
    continuous_timing::PacketPin pin;
    {
        std::lock_guard lock(mutex_);
        auto found = std::find_if(entries_.begin(), entries_.end(), [=](const auto& v) { return v.serial == serial && v.state == State::Claimed; });
        if (found == entries_.end()) return;
        pin = found->pin;
        reserveReturn(pin);
        *found = {};
        ++stats_.finished;
    }
    returnPin(pin);
}
void Ledger::retire(bool all, ObjectIdentity chain, std::uint64_t generation) noexcept {
    std::array<continuous_timing::PacketPin, packetCapacity> pins{};
    std::size_t count{};
    {
        std::lock_guard lock(mutex_);
        for (auto& entry : entries_) {
            if (entry.state == State::Free || (!all && entry.key.swapchain != chain && entry.contextGeneration != generation)) continue;
            ++stats_.cancelled;
            if (entry.state == State::Publishing || entry.state == State::Resolving || entry.state == State::Claimed) entry.cancelled = true;
            else {
                if (entry.pin.valid) {
                    reserveReturn(entry.pin);
                    pins[count++] = entry.pin;
                }
                entry = {};
            }
        }
    }
    for (std::size_t index = 0; index < count; ++index) returnPin(pins[index]);
}
void Ledger::retireSwapchain(ObjectIdentity chain) noexcept { retire(false, chain, 0); }
void Ledger::retireContext(std::uint64_t generation) noexcept { retire(false, {}, generation); }
void Ledger::invalidateUnclaimed() noexcept { retire(true, {}, 0); }
void Ledger::stop() noexcept {
    { std::lock_guard lock(mutex_); stopped_ = true; }
    retire(true, {}, 0);
}
Stats Ledger::stats() const {
    std::lock_guard lock(mutex_);
    auto result = stats_;
    result.returnCallbacksInFlight = returnCallbacksInFlight_;
    result.inFlight += returnCallbacksInFlight_;
    for (const auto& entry : entries_) {
        if (entry.state != State::Free) ++result.occupied;
        if (entry.state == State::Publishing || entry.state == State::Resolving || entry.state == State::Claimed) ++result.inFlight;
    }
    return result;
}
} // namespace xess_vk::presentation_packet
