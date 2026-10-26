#include "presenter_frame_timing.hpp"
#include "continuous_timing.hpp"
#include "cpu_wall_time.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using Table = xess_vk::detail::PresenterFrameTimings;
using Ticket = xess_vk::FrameTimingTicket;
using Stage = xess_vk::game_timing::Stage;
namespace ct = xess_vk::continuous_timing;
unsigned checks{};
void require(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
xess_vk::game_timing::Event event(Stage stage, std::uint32_t key, std::uint32_t thread = 1) {
    xess_vk::game_timing::Event result;
    result.stage = stage;
    result.engineFrame = key;
    result.threadId = thread;
    result.frameKeyValid = true;
    return result;
}
void complete(Table& table, Table::Handle handle) {
    require(table.completeBegin(handle, 0, 0), "real table begin completion rejected");
    for (unsigned stage = 1; stage < 4; ++stage) {
        const auto marker = table.marker(handle.ticket, stage);
        require(bool(marker) && table.completeMarker(marker, stage, 0), "real table marker completion rejected");
    }
}
struct Owner {
    std::atomic<std::uint64_t> generation{7};
    std::mutex mutex, gateMutex;
    std::condition_variable gate;
    Table table{7, true}; // Explicit CPU capability, never enables production LL.
    ct::Coordinator* coordinator{};
    std::uint32_t blockedId{};
    bool entered{}, release{}, failBegin{}, failMarker{};
    unsigned returns{};
    ct::Callbacks callbacks() {
        return {this,
            [](void* user) { return static_cast<Owner*>(user)->generation.load(); },
            [](void* user, std::uint32_t id) {
                auto& owner = *static_cast<Owner*>(user);
                Table::Handle handle;
                { std::lock_guard lock(owner.mutex); handle = owner.table.begin(id); }
                if (!handle) return Ticket{};
                if (handle.ticket.sdkId == owner.blockedId) {
                    std::unique_lock gate(owner.gateMutex);
                    owner.entered = true;
                    owner.gate.notify_all();
                    if (!owner.gate.wait_for(gate, std::chrono::seconds(3), [&] { return owner.release; }))
                        throw std::runtime_error("CPU callback gate timed out");
                }
                std::lock_guard lock(owner.mutex);
                if (owner.failBegin) {
                    owner.table.completeBegin(handle, -1, -1);
                    throw std::runtime_error("injected begin failure");
                }
                owner.table.completeBegin(handle, 0, 0);
                // Return the actual reservation even after a concurrent stop;
                // Coordinator must return this lost callback ticket exactly once.
                return handle.ticket;
            },
            [](void* user, Ticket ticket) { static_cast<Owner*>(user)->marker(ticket, 1); },
            [](void* user, Ticket ticket) { static_cast<Owner*>(user)->marker(ticket, 2); },
            [](void* user, Ticket ticket) { static_cast<Owner*>(user)->marker(ticket, 3); },
            [](void* user) {
                auto& owner = *static_cast<Owner*>(user);
                std::lock_guard lock(owner.mutex);
                return owner.table.nextId();
            },
            [](void* user, Ticket ticket) {
                auto& owner = *static_cast<Owner*>(user);
                // Reentry is the lock boundary test: abandon is outside the
                // coordinator mutex, and Color/table owns a separate short lock.
                (void)owner.coordinator->stats();
                std::lock_guard lock(owner.mutex);
                owner.table.abandon(ticket);
                ++owner.returns;
            }};
    }
    void marker(Ticket ticket, unsigned index) {
        std::lock_guard lock(mutex);
        const auto handle = table.marker(ticket, index);
        if (!handle || !table.completeMarker(handle, index, failMarker ? -1 : 0))
            throw std::runtime_error("injected marker failure or missing reservation");
    }
    void anchor(ct::Coordinator& timing) {
        coordinator = &timing;
        require(timing.event(event(Stage::NativePresentStart, 0, 9)), "initial anchor failed");
        require(timing.event(event(Stage::NativePresentEnd, 0, 9)), "initial packet failed to close");
    }
    std::uint32_t off(ct::Coordinator& timing) {
        std::uint32_t id{};
        {
            std::lock_guard lock(mutex);
            id = table.admitUntimed();
            require(id != 0, "idle untimed OFF admission failed");
            table.finish({}, id, true);
        }
        timing.untimedPresented(id);
        return id;
    }
};
void simulation(ct::Coordinator& timing, std::uint32_t key) {
    require(timing.event(event(Stage::SimulationStart, key)), "SIM begin rejected");
    require(timing.event(event(Stage::SimulationEnd, key)), "SIM end rejected");
}
void render(ct::Coordinator& timing, std::uint32_t key) {
    require(timing.event(event(Stage::RenderSubmitStart, key, 2)), "render begin rejected");
    require(timing.event(event(Stage::RenderSubmitEnd, key, 2)), "render end rejected");
}
void pressureAndFailedPresent() {
    Owner owner;
    ct::Coordinator timing(owner.callbacks());
    owner.anchor(timing);
    for (std::uint32_t key = 1; key <= 24; ++key) simulation(timing, key);
    require(timing.stats().occupied == 8 && timing.stats().evicted == 16 &&
            owner.table.stats().occupied == 8 && !timing.frame(16) && bool(timing.frame(17)),
            "unpresented pressure leaked either of the two real eight-slot tables");
    render(timing, 19);
    require(timing.event(event(Stage::NativePresentStart, 19, 9)), "old complete packet rejected");
    const auto frame = timing.present(9);
    require(frame.valid, "old complete frame had no ticket");
    // Native copy can fail before any SDK output. This must still release the
    // Color reservation through the actual abandon callback.
    timing.presented(frame, false);
    require(!timing.frame(19) && owner.table.stats().occupied == 7,
            "failed present before SDK output leaked its owner reservation");
    require(timing.event(event(Stage::NativePresentEnd, 19, 9)), "failed packet end rejected");
    render(timing, 20);
    require(timing.event(event(Stage::NativePresentStart, 20, 9)), "successful packet start failed");
    const auto success = timing.present(9);
    const auto handle = owner.table.admit(success.ticket);
    require(bool(handle), "Color's real table refused the complete timing");
    owner.table.finish(handle, success.ticket.sdkId, true);
    timing.presented(success, true);
    require(timing.stats().occupied == 4 && owner.table.stats().occupied == 4 &&
            bool(timing.frame(21)) && !timing.frame(18), "successful present did not retire stale <=K while preserving future");
    require(timing.event(event(Stage::NativePresentEnd, 20, 9)), "successful packet end failed");
    timing.stop();
    require(timing.stats().occupied == 0 && owner.table.stats().occupied == 0, "stop leaked either table");
}
void pendingCancellation() {
    Table table{7};
    const auto sleeping = table.begin();
    table.abandon(sleeping.ticket);
    require(table.stats().occupied == 1 && table.stats().pending == 1 && !table.admitUntimed(),
            "pending Sleep was reused or raced by untimed SDK admission");
    for (unsigned index = 0; index < 7; ++index) {
        const auto other = table.begin();
        require(bool(other), "idle pressure failed while one Sleep was pending");
        require(table.completeBegin(other, 0, 0), "other begin failed");
    }
    require(!table.begin() && table.stats().pending == 1 && table.stats().occupied == 8,
            "lower table independently evicted a reservation invisible to its scope owner");
    require(!table.completeBegin(sleeping, 0, 0) && table.stats().pending == 0,
            "cancelled Sleep wrote a new slot or was accepted after returning");
    const auto active = table.begin();
    require(table.completeBegin(active, 0, 0), "new reservation failed");
    const auto marker = table.marker(active.ticket, 1);
    table.abandon(active.ticket);
    require(table.stats().pending == 1 && !table.admitUntimed(), "pending marker was released before actual completion");
    require(!table.completeMarker(marker, 1, 0) && table.stats().pending == 0, "cancelled marker revived its ticket");
    const auto present = table.begin();
    complete(table, present);
    const auto admitted = table.admit(present.ticket);
    table.abandon(present.ticket);
    require(bool(admitted) && table.stats().pending == 1 && !table.admitUntimed(), "pending presentation slot was overwritten");
    table.finish(admitted, present.ticket.sdkId, false);
    const auto nextBeforeRefusal = table.nextId();
    require(!table.admitUntimed() && table.stats().occupied == 7 && table.nextId() == nextBeforeRefusal,
            "idle future reservations were abandoned or overtaken by untimed OFF");
    table.abandonAll(); // Explicit scope-owner cancellation, not untimed admission.
    const auto id = table.admitUntimed();
    require(id && !table.begin(), "new SDK timing raced an admitted untimed OFF");
    table.finish({}, id, true);
    const auto afterOff = table.begin();
    require(bool(afterOff) && table.completeBegin(afterOff, 0, 0), "timing admission did not resume after actual OFF finished");
    table.abandon(afterOff.ticket);
    require(table.stats().occupied == 0 && table.stats().pending == 0, "final OFF failed to reclaim idle reservations");
    require(!table.completeBegin(sleeping, 0, 0) && table.stats().staleCompletions == 1,
            "stale serial was allowed to write after reuse");
}
void protectedPresentBeforeColorAdmission() {
    Owner owner;
    ct::Coordinator timing(owner.callbacks());
    owner.anchor(timing);
    for (std::uint32_t key = 1; key <= 8; ++key) simulation(timing, key);
    render(timing, 1);
    require(timing.event(event(Stage::NativePresentStart, 1, 9)), "oldest frame packet did not open");
    const auto protectedFrame = timing.present(9);
    require(protectedFrame.valid, "oldest packet was not complete before private copy");
    // The wrapper is performing its private copy, before Color has pinned
    // presenting. Only Coordinator can see that the oldest record is active.
    simulation(timing, 9);
    require(timing.frame(1) == protectedFrame.ticket && !timing.frame(2) &&
            owner.table.stats().occupied == 8,
            "two full tables chose different victims before native copy completion");
    const auto admitted = owner.table.admit(protectedFrame.ticket);
    require(bool(admitted), "new begin evicted the packet awaiting Color admission");
    owner.table.finish(admitted, protectedFrame.ticket.sdkId, true);
    timing.presented(protectedFrame, true);
    require(timing.event(event(Stage::NativePresentEnd, 1, 9)), "protected packet did not close");
    timing.stop();
    require(owner.table.stats().occupied == 0, "protected-packet pressure leaked reservations");
}
void lateCallback(bool contextReset) {
    Owner owner;
    ct::Coordinator timing(owner.callbacks());
    owner.anchor(timing);
    owner.blockedId = 2;
    bool accepted{};
    std::thread sdk([&] { accepted = timing.event(event(Stage::SimulationStart, 1)); });
    {
        std::unique_lock lock(owner.gateMutex);
        if (!owner.gate.wait_for(lock, std::chrono::seconds(2), [&] { return owner.entered; })) std::terminate();
    }
    require(timing.stats().pending == 1 && owner.table.stats().pending == 1, "blocked real callback did not pin both slots");
    if (contextReset) {
        owner.generation = 8;
        require(!timing.frame(1), "context change kept a pending old reservation");
    } else {
        timing.stop();
        std::lock_guard lock(owner.mutex);
        require(!owner.table.admitUntimed(),"stopped timing cannot reuse a real SDK begin still in flight");
    }
    {
        std::lock_guard lock(owner.gateMutex);
        owner.release = true;
    }
    owner.gate.notify_all();
    sdk.join();
    require(!accepted && owner.returns == 1 && timing.stats().lateCallbacks == 1 &&
            timing.stats().occupied == 0 && owner.table.stats().occupied == 0,
            "lost begin callback was not returned exactly once outside the coordinator lock");
    if(!contextReset) {
        std::lock_guard lock(owner.mutex);const auto id=owner.table.admitUntimed();
        require(id!=0,"visible OFF resumes after the late cancelled begin actually returns");
        owner.table.finish({},id,true);
    }
}
void offSequenceAndFailures() {
    Owner owner;
    ct::Coordinator timing(owner.callbacks());
    owner.anchor(timing);
    simulation(timing, 100); // Reserve a far-future mapped ticket, ID101.
    render(timing, 100);
    const auto old = timing.frame(100);
    require(!owner.table.admitUntimed() && owner.table.stats().occupied == 1 && timing.frame(100) == old,
            "complete future ticket was cancelled or bypassed by untimed OFF");
    require(timing.event(event(Stage::NativePresentStart, 100, 9)), "future packet did not enter its real scope");
    const auto abandoned = timing.present(9);
    timing.presented(abandoned, false); // An actual failed packet explicitly returns its reservation.
    require(timing.event(event(Stage::NativePresentEnd, 100, 9)), "failed future packet scope did not close");
    std::uint32_t last{};
    for (unsigned index = 0; index < 10; ++index) last = owner.off(timing);
    require(last == old.sdkId + 10 && timing.stats().ownerAllocatedIds &&
            timing.stats().occupied == 0 && owner.table.stats().occupied == 0,
            "ten untimed OFF frames collided with reserved IDs or leaked mapped tickets");
    simulation(timing, 101);
    render(timing, 101);
    require(timing.event(event(Stage::NativePresentStart, 101, 9)), "owner-allocated exact key failed");
    auto frame = timing.present(9);
    require(frame.valid && frame.engineFrame == 101 && frame.ticket.sdkId == last + 1 &&
            !frame.ticket.latencyEnabled && owner.table.recoveryNeeded() && !owner.table.admit(old),
            "recovery used mapped arithmetic, revived old IDs or skipped timed OFF priming");
    auto present = owner.table.admit(frame.ticket);
    owner.table.finish(present, frame.ticket.sdkId, true);
    timing.presented(frame, true);
    require(timing.event(event(Stage::NativePresentEnd, 101, 9)), "recovery packet end failed");
    simulation(timing, 102);
    render(timing, 102);
    const auto on = timing.frame(102);
    require(on.latencyEnabled && on.sdkId == last + 2 && owner.table.recoveryNeeded(),
            "CPU-enabled capability did not recover exact-key ON with reset history");
    owner.table.recovered();
    require(!owner.table.recoveryNeeded(), "successful simulated ON failed to acknowledge history reset");
    owner.generation = 8;
    require(!timing.frame(102) && !timing.stats().ownerAllocatedIds && owner.table.stats().occupied == 0,
            "context invalidation kept old owner reservations or the prior ID policy");
    timing.stop();
    Owner failure;
    ct::Coordinator rejected(failure.callbacks());
    failure.anchor(rejected);
    failure.failBegin = true;
    require(!rejected.event(event(Stage::SimulationStart, 1)) && !failure.table.stats().occupied && !rejected.stats().occupied,
            "SDK begin failure leaked a reservation");
    failure.failBegin = false;
    simulation(rejected, 2);
    failure.failMarker = true;
    require(!rejected.event(event(Stage::RenderSubmitStart, 2, 2)) &&
            !failure.table.stats().occupied && !rejected.stats().occupied, "SDK marker failure leaked either reservation");
    Table production{99};
    const auto first = production.begin();
    complete(production, first);
    const auto off = production.admit(first.ticket);
    production.finish(off, first.ticket.sdkId, true);
    const auto next = production.begin();
    require(!production.latencyCapability() && !next.ticket.latencyEnabled,
            "production enabled dynamic LL without a GPU quiescence contract");
}
void persistentAheadPreservesExactPackets() {
    Owner owner;
    ct::Coordinator timing(owner.callbacks());
    owner.anchor(timing);
    for (std::uint32_t key = 1; key <= 4; ++key) simulation(timing, key);
    for (std::uint32_t key = 1; key <= 20; ++key) {
        render(timing, key);
        const auto exact = timing.frame(key);
        const auto future = timing.frame(key + 3);
        const auto next = owner.table.nextId();
        require(exact && future && !owner.table.admitUntimed() && owner.table.nextId() == next &&
                owner.table.stats().occupied == 4 && timing.frame(key + 3) == future,
                "perpetually leading CPU reservations were overtaken by a missing-packet OFF allocation");
        require(timing.event(event(Stage::NativePresentStart, key, 9)), "exact queued packet lost its scope");
        const auto packet = timing.present(9);
        require(packet.valid && packet.ticket == exact, "exact packet could not recover after native-only missing packet");
        const auto admitted = owner.table.admit(packet.ticket);
        require(bool(admitted), "Color table lost the genuine complete packet while refusing untimed OFF");
        owner.table.finish(admitted, packet.ticket.sdkId, true);
        timing.presented(packet, true);
        require(timing.event(event(Stage::NativePresentEnd, key, 9)), "exact packet failed to close");
        require(timing.frame(key + 3) == future && owner.table.stats().occupied == 3,
                "successful exact packet cancelled a newer reservation");
        simulation(timing, key + 4);
    }
    require(timing.stats().timedPresents == 20 && timing.stats().untimedPresents == 0,
            "CPU-ahead pipeline remained permanently OFF despite twenty exact complete packets");
    timing.stop();
    require(owner.table.stats().occupied == 0, "CPU-ahead scope stop leaked future reservations");
}
void cpuWallCounters() {
    xess_vk::CpuWallTime sample{};
    sample.add(11); sample.add(3); sample.add(11);
    require(sample.count == 3 && sample.totalNs == 25 && sample.maxNs == 11,
            "wall counters accumulate count, sum and maximum");
    const auto prior = sample;
    sample.add(2);
    require(sample.count - prior.count == 1 && sample.totalNs - prior.totalNs == 2 && sample.maxNs == 11,
            "the same counter remains cumulative for offline interval differences");
    try { xess_vk::CpuWallScope scope(sample); throw std::runtime_error("owned timing exception"); }
    catch (const std::runtime_error&) { }
    require(sample.count == 5 && sample.totalNs >= 27 && sample.maxNs <= sample.totalNs,
            "wall scope records exceptional exit once without requiring a positive clock interval");
    xess_vk::CpuWallTime shared{};
    std::mutex mutex;
    bool snapshotsValid = true;
    auto update = [&](std::uint64_t ns) {
        for (unsigned i = 0; i < 100; ++i) {
            std::lock_guard lock(mutex); shared.add(ns); const auto snapshot = shared;
            snapshotsValid &= snapshot.count <= snapshot.totalNs && snapshot.maxNs <= snapshot.totalNs;
        }
    };
    std::thread first(update, 1), second(update, 3); first.join(); second.join();
    { std::lock_guard lock(mutex); sample = shared; }
    require(snapshotsValid && sample.count == 200 && sample.totalNs == 400 && sample.maxNs == 3,
            "two callers update and snapshot the plain counter under their external owner mutex");
    sample = {};
    require(!sample.count && !sample.totalNs && !sample.maxNs, "value initialization clears wall statistics");
}
} // namespace
int main() {
    try {
        pressureAndFailedPresent();
        pendingCancellation();
        protectedPresentBeforeColorAdmission();
        lateCallback(false);
        lateCallback(true);
        offSequenceAndFailures();
        persistentAheadPreservesExactPackets();
        cpuWallCounters();
        std::cout << "{\"stage\":\"production-two-layer-frame-timing-cpu\",\"passed\":true,\"checks\":" << checks
                  << ",\"productionOnGated\":true,\"gpuTouched\":false,\"sdkLoaded\":false}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
