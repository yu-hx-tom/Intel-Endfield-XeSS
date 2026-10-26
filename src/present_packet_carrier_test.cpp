#include "present_packet_carrier.hpp"
#include "presenter_frame_timing.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
namespace ct = xess_vk::continuous_timing;
namespace pp = xess_vk::presentation_packet;
using xess_vk::FrameTimingAssociation;
using xess_vk::FrameTimingTicket;
using Stage = xess_vk::game_timing::Stage;
using Table = xess_vk::detail::PresenterFrameTimings;
unsigned checks{};
void require(bool condition, const char* reason) {
    ++checks;
    if (!condition) throw std::runtime_error(reason);
}
xess_vk::game_timing::Event event(Stage stage, std::uint32_t key, std::uint32_t thread = 1) {
    xess_vk::game_timing::Event result;
    result.stage = stage;
    result.engineFrame = key;
    result.threadId = thread;
    result.frameKeyValid = true;
    return result;
}
// Uses both actual bounded production tables. This owner only substitutes the
// SDK calls; no DLL, driver, window, or GPU is loaded by this fixture.
struct Owner {
    std::uint64_t generation{7};
    Table first{7}, second{8};
    std::mutex mutex;
    ct::Coordinator* coordinator{};
    std::vector<FrameTimingTicket> abandoned;
    bool failAbandon{};
    std::function<void(unsigned)> duringMarker;
    Table& table(std::uint64_t context) { return context == 7 ? first : second; }
    ct::Callbacks callbacks() {
        return {this,
            [](void* user) { return static_cast<Owner*>(user)->generation; },
            [](void* user, std::uint32_t id) {
                auto& owner = *static_cast<Owner*>(user);
                (void)owner.coordinator->stats(); // Reentry proves callback is outside its mutex.
                std::lock_guard lock(owner.mutex);
                auto& table = owner.table(owner.generation);
                const auto handle = table.begin(id);
                if (!handle || !table.completeBegin(handle, 0, 0)) return FrameTimingTicket{};
                return handle.ticket;
            },
            [](void* user, FrameTimingTicket ticket) { static_cast<Owner*>(user)->marker(ticket, 1); },
            [](void* user, FrameTimingTicket ticket) { static_cast<Owner*>(user)->marker(ticket, 2); },
            [](void* user, FrameTimingTicket ticket) { static_cast<Owner*>(user)->marker(ticket, 3); },
            [](void* user) {
                auto& owner = *static_cast<Owner*>(user);
                std::lock_guard lock(owner.mutex);
                return owner.table(owner.generation).nextId();
            },
            [](void* user, FrameTimingTicket ticket) {
                auto& owner = *static_cast<Owner*>(user);
                (void)owner.coordinator->stats();
                std::lock_guard lock(owner.mutex);
                owner.abandoned.push_back(ticket);
                if (owner.failAbandon) throw std::runtime_error("fake owner abandon failed");
                owner.table(ticket.contextGeneration).abandon(ticket);
            }};
    }
    void marker(FrameTimingTicket ticket, unsigned index) {
        Table::Handle handle;
        {
            std::lock_guard lock(mutex);
            handle = table(ticket.contextGeneration).marker(ticket, index);
        }
        if (!handle) throw std::runtime_error("fake SDK marker admission rejected");
        if (duringMarker) duringMarker(index);
        std::lock_guard lock(mutex);
        if (!table(ticket.contextGeneration).completeMarker(handle, index, 0))
            throw std::runtime_error("fake SDK marker was cancelled");
    }
};
struct Rig {
    Owner owner;
    ct::Coordinator coordinator{owner.callbacks()};
    pp::IdentityRegistry identities{42};
    std::function<void(ct::PacketPin)> duringPin, duringResolve, duringUnpin;
    unsigned pinReturns{};
    pp::Ledger ledger{{this,
        [](void* user, FrameTimingAssociation association, std::uint64_t generation) {
            auto& rig = *static_cast<Rig*>(user);
            (void)rig.ledger.stats();
            auto pin = rig.coordinator.pinPacket(association, generation);
            if (rig.duringPin) rig.duringPin(pin);
            return pin;
        },
        [](void* user, ct::PacketPin pin) {
            auto& rig = *static_cast<Rig*>(user);
            (void)rig.ledger.stats();
            if (rig.duringResolve) rig.duringResolve(pin);
            return rig.coordinator.resolvePacket(pin);
        },
        [](void* user, ct::PacketPin pin) {
            auto& rig = *static_cast<Rig*>(user);
            (void)rig.ledger.stats();
            ++rig.pinReturns;
            if (rig.duringUnpin) rig.duringUnpin(pin);
            rig.coordinator.unpinPacket(pin);
        }}};
    Rig() {
        owner.coordinator = &coordinator;
        require(identities.swapchainCreated(1000, 0), "test swapchain creation failed");
        require(identities.semaphoreCreated(2000, 0), "test semaphore creation failed");
    }
    ~Rig() {
        ledger.stop();
        coordinator.stop();
    }
    bool stage(Stage stage, std::uint32_t key, std::uint32_t thread = 1) {
        return coordinator.event(event(stage, key, thread));
    }
    void frame(std::uint32_t key, bool renderEnd = true) {
        require(stage(Stage::SimulationStart, key), "SIM start failed");
        require(stage(Stage::SimulationEnd, key), "SIM end failed");
        require(coordinator.registrationAssociation(1, key, true).valid, "post-SIM registration lost exact key");
        require(stage(Stage::RenderSubmitStart, key, 2), "render start failed");
        if (renderEnd) require(stage(Stage::RenderSubmitEnd, key, 2), "render end failed");
    }
    pp::Key nextKey(std::uint32_t image = 0) {
        const std::uint64_t wait = 2000;
        require(identities.successfulSignals({&wait, 1}, 0), "successful signal failed");
        auto key = identities.makeKey(3000, 1000, image, {&wait, 1});
        require(key.valid, "observed binary signal did not make a packet key");
        return key;
    }
    pp::Key publish(std::uint32_t key, std::uint32_t image = 0) {
        require(stage(Stage::NativePresentStart, key, 9), "source native scope did not open");
        auto packet = nextKey(image);
        const auto association = coordinator.packetAssociation(9);
        require(association.valid && association.engineFrame == key, "source did not retain its exact reservation");
        require(ledger.publish(packet, association, owner.generation), "source packet publication failed");
        require(stage(Stage::NativePresentEnd, key, 9), "source native scope did not close");
        return packet;
    }
    void output(pp::Claim claim) {
        require(claim.valid, "worker could not claim exact packet");
        const auto& association = claim.association;
        auto& table = owner.table(owner.generation);
        if (association.ticket) {
            const auto handle = table.admit(association.ticket);
            require(bool(handle), "actual owner rejected a claimed complete ticket");
            table.finish(handle, association.ticket.sdkId, true);
        } else {
            const auto id = table.admitBootstrap(association.bootstrapSdkId);
            require(id == association.bootstrapSdkId && id, "actual bootstrap admission failed");
            table.finish({}, id, true);
        }
        coordinator.presented(association, true);
        ledger.finish(claim.lease);
    }
    void bootstrap(std::uint32_t key = 100) {
        const auto packet = publish(key);
        auto claim = ledger.claim(packet, owner.generation);
        require(claim.valid && !claim.association.ticket && claim.association.bootstrapSdkId == 1,
                "source bootstrap was lost on the unscoped consumer thread");
        identities.consumed(packet);
        output(claim);
    }
};

void bootstrapAndPersistentAhead() {
    Rig rig;
    // Source publishes SDK1 before the main CPU has advanced to key103. The
    // consumer has no source scope; it must claim the captured bootstrap.
    const auto first = rig.publish(100);
    rig.frame(103);
    auto claim = rig.ledger.claim(first, 7);
    require(claim.valid && claim.association.bootstrapSdkId == 1 && !claim.association.ticket,
            "CPU-ahead reservation replaced the first bootstrap association");
    rig.identities.consumed(first);
    rig.output(claim);
    for (std::uint32_t key : {101u, 102u}) {
        const auto packet = rig.publish(key);
        auto bootstrap = rig.ledger.claim(packet, 7);
        require(bootstrap.valid && bootstrap.association.bootstrapSdkId == key - 99,
                "older bootstrap changed a future reserved SDK ID");
        rig.identities.consumed(packet);
        rig.output(bootstrap);
    }
    rig.frame(104);
    rig.frame(105);
    rig.frame(106);
    const auto key103 = rig.publish(103);
    rig.output(rig.ledger.claim(key103, 7));
    rig.identities.consumed(key103);
    // A direct/unsupported native packet has no source carrier. It consumes its
    // semaphore cycle but must not invent an OFF ID over reservations104..106.
    const auto missing = rig.nextKey(1);
    require(!rig.ledger.claim(missing, 7).valid, "missing source carrier guessed latest ticket");
    rig.identities.consumedWaits({&missing.waits[0].semaphore.handle, 1});
    const auto before = rig.owner.first.nextId();
    require(!rig.owner.first.admitUntimed() && rig.owner.first.nextId() == before,
            "missing packet overtook future reservations with an untimed OFF ID");
    // Keep the CPU exactly three keys ahead while worker resumes key104.
    rig.frame(107);
    const auto key104 = rig.publish(104);
    claim = rig.ledger.claim(key104, 7);
    require(claim.valid && claim.association.engineFrame == 104 && claim.association.ticket.sdkId == 5,
            "persistent CPU-ahead did not recover the exact old packet");
    require(!claim.association.ticket.latencyEnabled, "production ON gate was accidentally enabled");
    rig.output(claim);
    rig.identities.consumed(key104);
    for (std::uint32_t key : {105u, 106u, 107u}) {
        const auto packet = rig.publish(key);
        rig.output(rig.ledger.claim(packet, 7));
        rig.identities.consumed(packet);
    }
    require(!rig.coordinator.stats().occupied && !rig.owner.first.stats().occupied && !rig.ledger.stats().occupied,
            "persistent-ahead recovery leaked either timing table or carrier");
    require(rig.coordinator.stats().bootstrapPresents == 3 && rig.coordinator.stats().timedPresents == 5,
            "bootstrap/timed production transitions were not actually exercised");
}

void lateMarkerAndCapacityPin() {
    Rig rig;
    rig.bootstrap();
    rig.frame(101, false);
    const auto packet = rig.publish(101);
    require(rig.coordinator.stats().packetPins == 1, "published incomplete reservation was not pinned");
    require(rig.stage(Stage::RenderSubmitEnd, 101, 2), "late actual render marker did not complete");
    for (std::uint32_t key = 102; key <= 110; ++key) rig.frame(key);
    auto claim = rig.ledger.claim(packet, 7);
    require(claim.valid && claim.association.engineFrame == 101,
            "bounded pressure evicted a packet before consumer admission");
    rig.output(claim);
    rig.identities.consumed(packet);
    const auto returns = rig.pinReturns;
    rig.ledger.finish(claim.lease);
    require(rig.pinReturns == returns, "duplicate finish returned pin twice");
    rig.coordinator.stop();
    require(!rig.owner.first.stats().occupied, "unpresented pressure victims leaked lower reservations");
}

void cancellationKeepsOwnerUntilFinish() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        Rig rig;
        rig.bootstrap();
        rig.frame(101);
        const auto packet = rig.publish(101);
        auto claim = rig.ledger.claim(packet, 7);
        require(claim.valid, "cancellation fixture could not claim complete ticket");
        const auto ticket = claim.association.ticket;
        if (mode == 0) rig.coordinator.untimedPresented(ticket.sdkId);
        if (mode == 1) rig.coordinator.stop();
        if (mode == 2) {
            rig.owner.generation = 8;
            require(rig.stage(Stage::NativePresentStart, 100, 10), "new context bootstrap scope failed");
            require(rig.stage(Stage::NativePresentEnd, 100, 10), "new context scope did not close");
            rig.frame(101);
        }
        require(rig.owner.first.stats().occupied == 1 && rig.coordinator.stats().packetPins == 1,
                "cancelled claimed ticket was returned before lease finish");
        rig.ledger.finish(claim.lease);
        require(!rig.owner.first.stats().occupied, "last old pin did not return cancelled owner reservation");
        if (mode == 2) require(rig.coordinator.scopeSnapshot(1).cpuFrameActive &&
                rig.coordinator.registrationAssociation(1, 101, true).valid,
                "old-context pin release cleared the new same-key CPU scope");
    }
    Rig future;
    future.bootstrap();
    future.frame(104);
    const auto packet = future.publish(104);
    future.coordinator.untimedPresented(2); // Notification can race a newly allocated higher reservation.
    auto claim = future.ledger.claim(packet, 7);
    require(claim.valid && claim.association.ticket.sdkId == 5,
            "successful older OFF invalidated a legal higher ticket/pin");
    future.output(claim);
}

void reentrantPublishingAndResolving() {
    Rig rig;
    rig.bootstrap();
    rig.frame(101);
    require(rig.stage(Stage::NativePresentStart, 101, 9), "reentry source scope failed");
    const auto packet = rig.nextKey();
    const auto association = rig.coordinator.packetAssociation(9);
    rig.duringPin = [&](ct::PacketPin) {
        require(!rig.ledger.claim(packet, 7).valid, "consumer claimed a Publishing packet before pin publication");
    };
    require(!rig.ledger.publish(packet, association, 7), "reentrant consume left a later ghost Ready packet");
    require(!rig.ledger.claim(packet, 7).valid && !rig.coordinator.stats().packetPins,
            "Publishing cancellation leaked a pin or resurrected packet");
    rig.duringPin = {};
    const auto next = rig.nextKey();
    require(rig.ledger.publish(next, association, 7), "fresh signal could not recover after publish cancellation");
    rig.duringResolve = [&](ct::PacketPin) {
        rig.coordinator.stop();
        require(rig.owner.first.stats().occupied == 1, "stop returned reservation while resolve callback was live");
        rig.ledger.stop();
    };
    require(!rig.ledger.claim(next, 7).valid && !rig.owner.first.stats().occupied &&
            !rig.ledger.stats().occupied && !rig.coordinator.stats().packetPins,
            "Resolving cancellation did not defer and release its owner exactly once");

    Rig wrongContext;
    wrongContext.bootstrap();
    wrongContext.frame(101);
    const auto wrong = wrongContext.publish(101);
    require(!wrongContext.ledger.claim(wrong, 8).valid && !wrongContext.coordinator.stats().packetPins,
            "wrong-context consume retained an obsolete carrier pin");
    require(!wrongContext.ledger.stats().occupied, "wrong-context consume left a candidate for later reuse");
}

void pendingMarkerCancellation() {
    Rig rig;
    rig.bootstrap();
    rig.frame(101, false);
    const auto packet = rig.publish(101);
    rig.owner.duringMarker = [&](unsigned marker) {
        if (marker != 3) return;
        rig.coordinator.stop();
        rig.ledger.stop(); // Ready pin returns, but the actual marker callback is still pending.
        require(rig.coordinator.stats().pending == 1 && rig.owner.first.stats().pending == 1,
                "stop reused a reservation while the actual marker was pending");
    };
    require(!rig.stage(Stage::RenderSubmitEnd, 101, 2), "late marker revived a stopped reservation");
    require(!rig.owner.first.stats().occupied && !rig.coordinator.stats().occupied &&
            !rig.ledger.claim(packet, 7).valid, "late marker return leaked or revived the old packet");
}

void poisoningAndBoundedRecovery() {
    Rig rig;
    rig.bootstrap();
    rig.frame(101);
    require(rig.stage(Stage::NativePresentStart, 101, 9), "poison source scope failed");
    const auto association = rig.coordinator.packetAssociation(9);
    for (unsigned index = 0; index < pp::packetCapacity; ++index) {
        const auto packet = rig.nextKey(index);
        require(rig.ledger.publish(packet, association, 7), "poison setup publish failed");
        require(!rig.ledger.publish(packet, association, 7), "duplicate immutable packet overwrote a candidate");
    }
    require(rig.ledger.stats().occupied == pp::packetCapacity && !rig.coordinator.stats().packetPins,
            "poison tombstones kept an owner reservation pinned");
    const auto overflow = rig.nextKey(99);
    require(!rig.ledger.publish(overflow, association, 7) && !rig.ledger.stats().occupied,
            "overflow did not clear unclaimed poison tombstones");
    const auto recovered = rig.nextKey();
    require(rig.ledger.publish(recovered, association, 7), "bounded ledger permanently saturated after duplicates");
    auto reordered = recovered;
    reordered.queue++;
    require(!rig.ledger.claim(reordered, 7).valid && rig.coordinator.stats().packetPins == 1,
            "different queue tuple incorrectly claimed or cancelled another packet");
    rig.output(rig.ledger.claim(recovered, 7));
}

void identityCyclesAndSubmissionRaces() {
    pp::IdentityRegistry registry(55);
    require(registry.swapchainCreated(10, 0) && registry.semaphoreCreated(20, 0) &&
            registry.semaphoreCreated(21, 0) && registry.semaphoreCreated(30, 0, false), "identity creation failed");
    const std::uint64_t binary[] = {20, 21};
    const std::uint64_t mixed[] = {30, 30, 20, 21, 999};
    auto pending = registry.beginSignals(mixed);
    require(pending.valid && pending.count == 2 && registry.stats().reliable,
            "unrelated timeline or unknown signal poisoned observed binary identities");
    require(!registry.makeKey(40, 10, 0, binary).valid, "pending successful-submit observation lent an old signal serial");
    require(registry.completeSignals(pending, 0), "binary submit completion failed");
    const auto first = registry.makeKey(40, 10, 0, binary);
    const std::uint64_t reverse[] = {21, 20};
    require(first.valid && first != registry.makeKey(40, 10, 0, reverse), "ordered wait identities were collapsed");
    require(!registry.makeKey(40, 10, 0, {}).valid, "zero-wait tuple manufactured provenance");
    registry.consumed(first);
    require(!registry.makeKey(40, 10, 0, binary).valid, "consumed binary signal cycle was reused");
    pending = registry.beginSignals(binary);
    registry.consumedWaits(binary); // Consumer reaches driver before successful submit observation commits.
    require(registry.completeSignals(pending, 0) && !registry.makeKey(40, 10, 0, binary).valid,
            "late submit completion resurrected an already consumed signal cycle");
    require(registry.successfulSignals(binary, 0), "next observed signal cycle failed");
    const auto next = registry.makeKey(40, 10, 0, binary);
    require(next.valid && next != first, "next signal cycle reused the previous packet key");
    registry.consumed(first);
    require(registry.makeKey(40, 10, 0, binary) == next, "old packet consumption destroyed a newer signal cycle");
    pending = registry.beginSignals(binary);
    require(!registry.completeSignals(pending, -4) && registry.makeKey(40, 10, 0, binary) == next,
            "failed driver submission changed the last successful signal identity");
    registry.consumedWaits(binary); // Submit waits are observed before signal observations begin.
    pending = registry.beginSignals(binary);
    require(!registry.completeSignals(pending, -4) && !registry.makeKey(40, 10, 0, binary).valid,
            "failed wait-and-signal submit resurrected the consumed old cycle");
    registry.semaphoreDestroyed(20);
    require(registry.semaphoreCreated(20, 0) && registry.successfulSignals(binary, 0), "semaphore handle reuse failed");
    const auto reused = registry.makeKey(40, 10, 0, binary);
    require(reused.valid && reused.waits[0].semaphore.generation != next.waits[0].semaphore.generation,
            "semaphore ABA reused the old object generation");
    registry.swapchainDestroyed(10);
    require(registry.swapchainCreated(10, 0), "swapchain handle reuse failed");
    require(registry.makeKey(40, 10, 0, binary).swapchain != reused.swapchain, "swapchain ABA reused the old generation");
    std::array<std::uint64_t, 66> manyTimeline{};
    manyTimeline.fill(30);
    manyTimeline.back() = 20;
    pending = registry.beginSignals(manyTimeline);
    require(pending.valid && pending.count == 1 && registry.completeSignals(pending, 0),
            "more than64 unrelated timeline signals exhausted the binary identity batch");
    registry.invalidate();
    require(!registry.makeKey(40, 10, 0, binary).valid, "unreadable provenance was later treated as reliable");

    pp::IdentityRegistry ambiguous(66);
    require(ambiguous.swapchainCreated(10, 0) && ambiguous.semaphoreCreated(20, 0) &&
            ambiguous.semaphoreCreated(21, 0) && ambiguous.successfulSignals(binary, 0),
            "ambiguous-batch setup failed");
    const std::uint64_t other = 21;
    require(ambiguous.makeKey(40, 10, 0, {&other, 1}).valid, "other binary old cycle was not initially available");
    const std::uint64_t duplicate[] = {20, 21, 20};
    require(!ambiguous.beginSignals(duplicate).valid && !ambiguous.stats().reliable &&
            !ambiguous.makeKey(40, 10, 0, {&other, 1}).valid,
            "duplicate binary signal retained another signal's stale packet identity");
}
void failedVictimReturnHasNoPhantomPending() {
    Rig rig;
    rig.bootstrap();
    for (std::uint32_t key = 101; key <= 108; ++key) rig.frame(key);
    rig.owner.failAbandon = true;
    require(!rig.stage(Stage::SimulationStart, 109), "new begin proceeded after victim-return failure");
    require(!rig.coordinator.stats().pending && rig.coordinator.stats().occupied == 7,
            "failed victim return left a phantom pending callback on the new record");
}
void returnedEntryStillKeepsCallbackOwnerAlive() {
    Rig rig;
    rig.bootstrap();
    rig.frame(101);
    const auto packet = rig.publish(101);
    const auto claim = rig.ledger.claim(packet, 7);
    require(claim.valid, "return callback fixture could not claim its packet");
    rig.coordinator.presented(claim.association, false);
    std::mutex mutex;
    std::condition_variable gate;
    bool entered{}, release{};
    rig.duringUnpin = [&](ct::PacketPin) {
        std::unique_lock lock(mutex);
        entered = true;
        gate.notify_all();
        if (!gate.wait_for(lock, std::chrono::seconds(3), [&] { return release; }))
            throw std::runtime_error("unpin callback gate timed out");
    };
    std::thread finish([&] { rig.ledger.finish(claim.lease); });
    bool observed{};
    {
        std::unique_lock lock(mutex);
        observed = gate.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
    }
    const auto pending = rig.ledger.stats();
    const auto retained = rig.coordinator.stats().packetPins;
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    gate.notify_all();
    finish.join();
    require(observed && !pending.occupied && pending.returnCallbacksInFlight == 1 && pending.inFlight == 1 && retained == 1,
            "empty ledger reported drained before its lock-free unpin callback returned");
    require(!rig.ledger.stats().inFlight && !rig.coordinator.stats().occupied && !rig.owner.first.stats().occupied,
            "returned pin callback did not drain both timing owners");
}
} // namespace
int main() {
    try {
        bootstrapAndPersistentAhead();
        lateMarkerAndCapacityPin();
        cancellationKeepsOwnerUntilFinish();
        reentrantPublishingAndResolving();
        pendingMarkerCancellation();
        poisoningAndBoundedRecovery();
        identityCyclesAndSubmissionRaces();
        failedVictimReturnHasNoPhantomPending();
        returnedEntryStillKeepsCallbackOwnerAlive();
        std::cout << "PASS async packet carrier: " << checks << " checks; production two-table/bootstrap/ahead recovery, pin cancellation, identity cycles\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
