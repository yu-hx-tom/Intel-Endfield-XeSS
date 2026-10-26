// CPU-only exact production Coordinator. Event thread IDs model the observed
// CPU/worker split; no Vulkan, SDK, window, hook or game is loaded.
#include "continuous_timing.hpp"
#include <iostream>
#include <stdexcept>

namespace {
namespace ct = xess_vk::continuous_timing;
using Stage = xess_vk::game_timing::Stage;
constexpr std::uint32_t cpu = 31, worker = 32;
unsigned checks{};
void require(bool condition, const char* reason) {
    ++checks;
    if (!condition) throw std::runtime_error(reason);
}
struct Fixture {
    std::uint64_t generation{7};
    unsigned begins{}, simulationEnds{}, renderBegins{}, renderEnds{};
    bool refuseBegin{}, failSimulationEnd{}, failRenderBegin{};
    ct::Coordinator coordinator;
    Fixture() : coordinator({this,
        [](void* user) { return static_cast<Fixture*>(user)->generation; },
        [](void* user, std::uint32_t id) {
            auto& fixture = *static_cast<Fixture*>(user);
            ++fixture.begins;
            return fixture.refuseBegin ? xess_vk::FrameTimingTicket{} :
                xess_vk::FrameTimingTicket{id, fixture.generation, true};
        },
        [](void* user, xess_vk::FrameTimingTicket) {
            auto& fixture = *static_cast<Fixture*>(user);
            ++fixture.simulationEnds;
            if (fixture.failSimulationEnd) throw std::runtime_error("injected SIM end refusal");
        },
        [](void* user, xess_vk::FrameTimingTicket) {
            auto& fixture = *static_cast<Fixture*>(user);
            ++fixture.renderBegins;
            if (fixture.failRenderBegin) throw std::runtime_error("injected render begin refusal");
        },
        [](void* user, xess_vk::FrameTimingTicket) { ++static_cast<Fixture*>(user)->renderEnds; },
        [](void*) -> std::uint32_t { return 1; }}) {
        // An observed native packet anchors key 0 to SDK ID 1. This fixture
        // does not submit a DXGI present or report it as consumed.
        require(event(Stage::NativePresentStart, 0, worker), "zero native key did not anchor");
        const auto anchor = coordinator.present(worker);
        require(anchor.valid && anchor.engineFrame == 0 && anchor.bootstrapSdkId == 1,
                "real key zero was mistaken for an absent key");
        require(event(Stage::NativePresentEnd, 0, worker), "anchor packet did not close");
    }
    bool event(Stage stage, std::uint32_t key, std::uint32_t thread, bool valid = true) {
        xess_vk::game_timing::Event value;
        value.stage = stage;
        value.engineFrame = key;
        value.threadId = thread;
        value.frameKeyValid = valid;
        return coordinator.event(value);
    }
    void simulation(std::uint32_t key, std::uint32_t thread = cpu) {
        require(event(Stage::SimulationStart, key, thread), "SIM start was refused");
        require(event(Stage::SimulationEnd, key, thread), "SIM end was refused");
    }
    void render(std::uint32_t key) {
        require(event(Stage::RenderSubmitStart, key, worker), "render start was refused");
        require(event(Stage::RenderSubmitEnd, key, worker), "render end was refused");
    }
};

void registrationAfterSimulation() {
    Fixture fixture;
    fixture.simulation(1);
    ct::ScopeSnapshot scope;
    const auto association = fixture.coordinator.registrationAssociation(cpu, 1, true, &scope);
    require(association.valid && association.engineFrame == 1 && association.ticket == fixture.coordinator.frame(1),
            "registration after real SIM end did not retain the owned frame/ticket");
    require(scope.threadObserved && !scope.inSimulation && scope.cpuFrameActive && scope.cpuFrame == 1 &&
            scope.contextGeneration == fixture.generation,
            "CPU frame identity was confused with an active simulation marker interval");
    require(!fixture.coordinator.producer(cpu).valid,
            "closed SIM interval was falsely reported as an active legacy producer scope");
    require(!fixture.coordinator.registrationAssociation(cpu, 2, true).valid,
            "C8 advanced to K+1 but registration borrowed K's ticket");
    require(!fixture.coordinator.registrationAssociation(cpu, 1, false).valid,
            "failed actual C8 read borrowed an owned frame");
    require(!fixture.coordinator.registrationAssociation(worker, 1, true).valid,
            "wrong thread borrowed CPU frame ownership");
    // Another thread repeating the same key may not acquire the first thread's
    // successful SIM ticket, even if it creates a local CPU identity.
    (void)fixture.event(Stage::SimulationStart, 1, 33);
    (void)fixture.event(Stage::SimulationEnd, 1, 33);
    require(!fixture.coordinator.registrationAssociation(33, 1, true).valid,
            "a repeated SIM key transferred original SIM-owner provenance");
    require(fixture.begins == 1 && fixture.simulationEnds == 1,
            "same frame allocated another SDK ID or duplicated SIM markers");
    require(fixture.event(Stage::SimulationStart, 2, cpu), "next CPU frame did not start");
    require(!fixture.coordinator.registrationAssociation(cpu, 1, true).valid &&
            !fixture.coordinator.registrationAssociation(cpu, 2, true).valid,
            "next SIM start retained old ownership or skipped its own SIM end");
    require(fixture.event(Stage::SimulationEnd, 2, cpu), "next CPU SIM end did not complete");
    const auto next = fixture.coordinator.registrationAssociation(cpu, 2, true, &scope);
    require(next.valid && next.engineFrame == 2 && scope.cpuFrame == 2 && !scope.inSimulation,
            "completed next CPU frame did not replace old identity");
}

void missingOrFailedSimulation() {
    Fixture fixture;
    require(!fixture.coordinator.registrationAssociation(cpu, 1, true).valid, "registration without SIM begin was admitted");
    (void)fixture.event(Stage::SimulationEnd, 1, cpu);
    require(!fixture.coordinator.registrationAssociation(cpu, 1, true).valid, "standalone SIM end invented a ticket");
    require(fixture.event(Stage::SimulationStart, 1, cpu), "actual SIM start was refused");
    require(!fixture.coordinator.registrationAssociation(cpu, 1, true).valid, "registration before SIM end was admitted");
    fixture.failSimulationEnd = true;
    require(!fixture.event(Stage::SimulationEnd, 1, cpu) &&
            !fixture.coordinator.registrationAssociation(cpu, 1, true).valid,
            "failed original marker callback counted as a complete SIM ticket");
    Fixture refused;
    refused.refuseBegin = true;
    require(!refused.event(Stage::SimulationStart, 1, cpu) &&
            !refused.coordinator.registrationAssociation(cpu, 1, true).valid,
            "absent SDK begin ticket was replaced by a guessed association");
}

void simulationEndOwner() {
    Fixture fixture;
    require(fixture.event(Stage::SimulationStart, 1, cpu), "owner SIM start was refused");
    require(!fixture.event(Stage::SimulationEnd, 1, worker),
            "thread without CPU ownership completed another thread's SIM ticket");
    require(fixture.simulationEnds == 0,
            "wrong-thread SIM end reached the SDK marker callback");
    require(!fixture.coordinator.registrationAssociation(cpu, 1, true).valid,
            "wrong-thread SIM end marked the owner's ticket complete");
    require(fixture.event(Stage::SimulationEnd, 1, cpu), "real owner SIM end did not recover");
    const auto association = fixture.coordinator.registrationAssociation(cpu, 1, true);
    require(association.valid && association.engineFrame == 1 && fixture.simulationEnds == 1,
            "real owner SIM end did not complete exactly one usable SIM ticket");
}

void packetRecordingAndCpuAhead() {
    Fixture fixture;
    fixture.simulation(1);
    fixture.render(1);
    ct::ScopeSnapshot scope;
    require(!fixture.coordinator.recordingAssociation(worker, &scope).valid && !scope.inRender,
            "recording after render end guessed a global/latest frame before its packet");
    require(fixture.event(Stage::NativePresentStart, 1, worker), "worker packet did not begin");
    const auto beginTime = fixture.coordinator.recordingAssociation(worker, &scope);
    require(beginTime.valid && beginTime.engineFrame == 1 && beginTime.ticket == fixture.coordinator.frame(1) &&
            scope.inPresent && !scope.inRender && !scope.inSimulation,
            "dispatch inside actual worker packet did not select that exact frame");
    fixture.simulation(2);
    const auto nextCpu = fixture.coordinator.registrationAssociation(cpu, 2, true);
    const auto dispatch = fixture.coordinator.recordingAssociation(worker);
    require(nextCpu.valid && nextCpu.engineFrame == 2 && dispatch.valid && dispatch.engineFrame == 1 &&
            dispatch.ticket == beginTime.ticket && dispatch.ticket != nextCpu.ticket,
            "CPU ahead K+1 changed worker packet K's dispatch association");
    require(!fixture.coordinator.recordingAssociation(cpu).valid,
            "CPU owner without a native packet was granted a dispatch frame");
    require(fixture.event(Stage::NativePresentEnd, 1, worker), "worker packet did not end");
    require(!fixture.coordinator.recordingAssociation(worker, &scope).valid && !scope.inPresent,
            "dispatch borrowed a closed native packet");
    fixture.render(2);
    require(fixture.event(Stage::NativePresentStart, 2, worker), "next worker packet did not start");
    const auto laterDispatch = fixture.coordinator.recordingAssociation(worker);
    // A vkBeginCommandBuffer during packet 1 must not freeze its frame for the
    // later dispatch. The production API is queried at the dispatch itself.
    require(laterDispatch.valid && laterDispatch.engineFrame == 2 && laterDispatch.ticket != beginTime.ticket,
            "a command-buffer begin-time association fixed the later dispatch to the prior frame");
    fixture.coordinator.presented(fixture.coordinator.present(worker), true);
    require(!fixture.coordinator.recordingAssociation(worker).valid &&
            !fixture.coordinator.registrationAssociation(cpu, 2, true).valid,
            "consumed/presented record remained available to CPU registration or dispatch");
}

void renderRecordingAndCpuAhead() {
    Fixture fixture;
    fixture.simulation(1);
    require(fixture.event(Stage::RenderSubmitStart, 1, worker), "render scope did not open");
    ct::ScopeSnapshot scope;
    const auto first = fixture.coordinator.recordingAssociation(worker, &scope);
    require(first.valid && first.engineFrame == 1 && first.ticket == fixture.coordinator.frame(1) &&
            scope.inRender && !scope.inPresent,
            "dispatch inside active render scope requires that worker's exact live render ticket");
    fixture.simulation(2);
    const auto ahead = fixture.coordinator.registrationAssociation(cpu, 2, true);
    const auto dispatch = fixture.coordinator.recordingAssociation(worker);
    require(ahead.valid && dispatch.valid && dispatch.engineFrame == 1 && dispatch.ticket == first.ticket &&
            dispatch.ticket != ahead.ticket,
            "CPU K+1 cannot replace worker render K's recording association");
    require(!fixture.coordinator.recordingAssociation(cpu).valid &&
            !fixture.coordinator.recordingAssociation(99).valid,
            "CPU-only or unknown threads cannot borrow a worker's active render frame");
    require(fixture.event(Stage::RenderSubmitEnd, 1, worker), "render scope did not close");
    require(!fixture.coordinator.recordingAssociation(worker, &scope).valid &&
            !scope.inRender && scope.renderFrame == 1,
            "inactive render key cannot survive render end as a recording association");

    Fixture conflict;
    conflict.simulation(1); conflict.simulation(2);
    require(conflict.event(Stage::RenderSubmitStart, 1, worker) &&
            conflict.event(Stage::NativePresentStart, 2, worker), "overlapping test scopes did not open");
    require(!conflict.coordinator.recordingAssociation(worker).valid,
            "different active render/present frames cannot choose a preferred or latest ticket");
    require(conflict.event(Stage::NativePresentEnd, 2, worker) &&
            conflict.coordinator.recordingAssociation(worker).ticket == conflict.coordinator.frame(1),
            "closing the conflicting packet restores only the still-active exact render frame");
    require(!conflict.event(Stage::RenderSubmitEnd, 1, worker, false) &&
            !conflict.coordinator.recordingAssociation(worker).valid,
            "invalid render key revokes the active render association");

    Fixture failed;
    failed.simulation(1); failed.failRenderBegin = true;
    require(!failed.event(Stage::RenderSubmitStart, 1, worker) &&
            !failed.coordinator.recordingAssociation(worker).valid,
            "failed render marker cannot publish a recording ticket");
    Fixture replaced;
    replaced.simulation(1);
    require(replaced.event(Stage::RenderSubmitStart, 1, worker), "pre-reset render scope did not open");
    ++replaced.generation;
    require(!replaced.coordinator.recordingAssociation(worker).valid,
            "context replacement revokes an otherwise active render ticket");
}

void revocationAndScopeReuse() {
    Fixture fixture;
    fixture.simulation(1);
    require(fixture.event(Stage::RenderSubmitStart, 1, worker), "worker scope did not open");
    // Keep the first scope busy; an additional thread must use an empty scope,
    // rather than reclaim CPU's SIM-ended-but-still-owned identity.
    require(fixture.event(Stage::NativePresentStart, 0, 40), "additional source thread scope was refused");
    require(fixture.coordinator.registrationAssociation(cpu, 1, true).valid,
            "inactive SIM marker interval caused reclamation of an owned CPU identity");
    require(!fixture.event(Stage::SimulationEnd, 1, cpu, false) &&
            !fixture.coordinator.registrationAssociation(cpu, 1, true).valid,
            "invalid SIM event retained CPU ownership");
    const auto revoked = fixture.coordinator.scopeSnapshot(cpu);
    require(!revoked.cpuFrameActive && !revoked.inSimulation, "invalid SIM did not revoke the identity flags");
    Fixture packet;
    packet.simulation(1);
    packet.render(1);
    require(packet.event(Stage::NativePresentStart, 1, worker), "packet for revocation case did not open");
    (void)packet.event(Stage::NativePresentEnd, 2, worker);
    require(!packet.coordinator.recordingAssociation(worker).valid,
            "mismatched native end retained an old active packet");
    require(packet.event(Stage::NativePresentStart, 1, worker), "packet could not reopen for invalid-key case");
    require(!packet.event(Stage::NativePresentStart, 1, worker, false) &&
            !packet.coordinator.recordingAssociation(worker).valid,
            "invalid native key retained a previous worker packet");
    Fixture reset;
    reset.simulation(1);
    reset.render(1);
    require(reset.event(Stage::NativePresentStart, 1, worker), "pre-reset packet could not open");
    ++reset.generation;
    require(!reset.coordinator.registrationAssociation(cpu, 1, true).valid &&
            !reset.coordinator.recordingAssociation(worker).valid && !reset.coordinator.frame(1),
            "context replacement revived prior CPU identity/ticket/packet");
    Fixture stopped;
    stopped.simulation(1);
    stopped.render(1);
    require(stopped.event(Stage::NativePresentStart, 1, worker), "pre-stop packet could not open");
    stopped.coordinator.stop();
    require(!stopped.coordinator.registrationAssociation(cpu, 1, true).valid &&
            !stopped.coordinator.recordingAssociation(worker).valid,
            "stopped coordinator retained an association");
}

void zeroCpuKey() {
    Fixture fixture;
    fixture.simulation(0);
    const auto zero = fixture.coordinator.registrationAssociation(cpu, 0, true);
    require(zero.valid && zero.engineFrame == 0 && zero.ticket.sdkId == 1,
            "valid owned C8 zero was mistaken for a missing frame key");
}
}
int main() {
    try {
        registrationAfterSimulation();
        missingOrFailedSimulation();
        simulationEndOwner();
        packetRecordingAndCpuAhead();
        renderRecordingAndCpuAhead();
        revocationAndScopeReuse();
        zeroCpuKey();
        std::cout << "{\"stage\":\"continuous-frame-association-cpu\",\"passed\":true,\"checks\":" << checks
                  << ",\"postSimulationRegistrationVerified\":true,\"activePacketDispatchVerified\":true"
                  << ",\"activeRenderDispatchVerified\":true,\"cpuAheadExactFrameVerified\":true,\"zeroFrameKeyVerified\":true"
                  << ",\"sdkCalled\":false,\"gpuExecuted\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
