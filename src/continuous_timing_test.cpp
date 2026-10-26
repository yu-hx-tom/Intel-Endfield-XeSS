#include "continuous_timing.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
namespace ct = xess_vk::continuous_timing;
using xess_vk::FrameTimingTicket;
using Stage = xess_vk::game_timing::Stage;
unsigned checks{};
void require(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
xess_vk::game_timing::Event event(Stage stage, std::uint32_t key, std::uint32_t thread = 1, bool valid = true) {
    xess_vk::game_timing::Event value;
    value.stage = stage;
    value.engineFrame = key;
    value.threadId = thread;
    value.frameKeyValid = valid;
    return value;
}
struct Owner {
    std::atomic<std::uint64_t> generation{10};
    std::atomic<unsigned> begins{}, simEnds{}, renderBegins{}, renderEnds{}, reentries{};
    std::atomic<bool> latencyEnabled{};
    std::atomic<std::uint32_t> nextSdkId{1};
    ct::Coordinator* coordinator{};
    std::mutex gateMutex;
    std::condition_variable gate;
    std::uint32_t blockedId{};
    bool entered{}, release{};
    ct::Callbacks callbacks() {
        return {this,
            [](void* user) { return static_cast<Owner*>(user)->generation.load(); },
            [](void* user, std::uint32_t id) {
                auto& owner = *static_cast<Owner*>(user);
                ++owner.begins;
                // Calling the real coordinator from its SDK callback proves
                // there is no coordinator mutex across that callback.
                (void)owner.coordinator->stats();
                ++owner.reentries;
                if (id == owner.blockedId) {
                    std::unique_lock lock(owner.gateMutex);
                    owner.entered = true;
                    owner.gate.notify_all();
                    if (!owner.gate.wait_for(lock, std::chrono::seconds(3), [&] { return owner.release; }))
                        throw std::runtime_error("fake Sleep gate timed out");
                }
                return FrameTimingTicket{id, owner.generation.load(), owner.latencyEnabled.load()};
            },
            [](void* user, FrameTimingTicket) {
                auto& owner = *static_cast<Owner*>(user);
                (void)owner.coordinator->stats();
                ++owner.simEnds;
            },
            [](void* user, FrameTimingTicket) { ++static_cast<Owner*>(user)->renderBegins; },
            [](void* user, FrameTimingTicket) { ++static_cast<Owner*>(user)->renderEnds; },
            [](void* user) { return static_cast<Owner*>(user)->nextSdkId.load(); }};
    }
};
void complete(ct::Coordinator& coordinator, std::uint32_t key) {
    require(coordinator.event(event(Stage::RenderSubmitStart, key, 2)), "real render start rejected");
    require(coordinator.event(event(Stage::SimulationEnd, key)), "overlapping SIM end rejected");
    require(coordinator.event(event(Stage::RenderSubmitEnd, key, 2)), "real render end rejected");
}
void mappingAndConcurrency() {
    Owner owner;
    ct::Coordinator coordinator(owner.callbacks());
    owner.coordinator = &coordinator;
    require(coordinator.event(event(Stage::SimulationStart, 99)), "preanchor SIM scope rejected");
    auto producer = coordinator.producer(1);
    ct::ScopeSnapshot diagnostic;
    const auto sameProducer = coordinator.producer(1, &diagnostic);
    require(sameProducer.engineFrame == producer.engineFrame && diagnostic.threadObserved &&
            diagnostic.inSimulation && !diagnostic.inRender && diagnostic.simulationFrame == 99,
            "diagnostic snapshot did not copy the same active producer scope");
    require(coordinator.event(event(Stage::SimulationEnd, 99)), "preanchor SIM end rejected");
    require(!coordinator.producer(1, &diagnostic).valid && !diagnostic.inSimulation &&
            diagnostic.simulationFrame == 99,
            "inactive diagnostic key was used as a producer association");
    require(coordinator.event(event(Stage::SimulationStart, 99)), "preanchor SIM resumed scope rejected");
    require(producer.valid && producer.engineFrame == 99 && !producer.ticket && owner.begins == 0,
            "preanchor source invented SDK timing");
    require(coordinator.event(event(Stage::NativePresentStart, 100, 9)), "first present did not anchor");
    auto first = coordinator.present(9);
    require(first.valid && !first.ticket && first.bootstrapSdkId == 1, "first native key did not map to SDK 1");
    coordinator.presented(first, true);
    require(coordinator.event(event(Stage::NativePresentEnd, 100, 9)) && !coordinator.present(9).valid,
            "native present scope did not close");
    require(coordinator.event(event(Stage::SimulationStart, 103)), "CPU-ahead frame reserve failed");
    require(coordinator.frame(103).sdkId == 4, "future frame did not retain fixed SDK mapping");
    require(coordinator.event(event(Stage::NativePresentStart, 103, 9)) && !coordinator.present(9).valid,
            "incomplete allocated ticket was downgraded to bootstrap");
    require(coordinator.event(event(Stage::NativePresentEnd, 103, 9)), "incomplete diagnostic packet closes");
    for (std::uint32_t key : {101u, 102u}) {
        require(coordinator.event(event(Stage::NativePresentStart, key, 9)), "older CPU-ahead bootstrap rejected");
        auto bootstrap = coordinator.present(9);
        require(bootstrap.valid && !bootstrap.ticket && bootstrap.bootstrapSdkId == key - 99,
                "bootstrap reused or skipped a reserved future SDK ID");
        coordinator.presented(bootstrap, true);
        require(coordinator.event(event(Stage::NativePresentEnd, key, 9)), "bootstrap packet closes");
    }
    complete(coordinator, 103);
    require(coordinator.event(event(Stage::NativePresentStart, 103, 9)), "completed exact present key rejected");
    auto timed = coordinator.present(9);
    require(timed.valid && timed.ticket.sdkId == 4 && !timed.ticket.latencyEnabled, "first full frame is not timed OFF");
    coordinator.presented(timed, true);
    require(coordinator.event(event(Stage::NativePresentEnd, 103, 9)), "priming packet closes");
    require(!coordinator.frame(103), "presented record was not released");
    owner.latencyEnabled = true;
    require(coordinator.event(event(Stage::SimulationStart, 104)), "next timed frame rejected");
    const auto beginCount = owner.begins.load();
    require(coordinator.event(event(Stage::SimulationStart, 104)) && owner.begins == beginCount,
            "same frame another view created a second SDK ticket");
    require(coordinator.event(event(Stage::RenderSubmitStart, 104, 2)), "render producer scope rejected");
    const auto renderCount = owner.renderBegins.load();
    require(coordinator.event(event(Stage::RenderSubmitStart, 104, 2)) && owner.renderBegins == renderCount,
            "same frame duplicated render marker");
    producer = coordinator.producer(2);
    require(producer.valid && producer.engineFrame == 104 && producer.ticket.sdkId == 5,
            "producer did not use its exact active render scope");
    require(!coordinator.producer(7).valid, "producer borrowed a global latest frame");
    require(coordinator.event(event(Stage::SimulationEnd, 104)), "SIM end rejected");
    require(coordinator.event(event(Stage::RenderSubmitEnd, 104, 2)), "render end rejected");
    require(!coordinator.producer(2).valid, "render scope leaked after end");
    require(coordinator.event(event(Stage::NativePresentStart, 104, 9)), "timed present start rejected");
    timed = coordinator.present(9);
    require(timed.valid && timed.ticket.latencyEnabled, "new ticket lost owner latency activation");
    coordinator.presented(timed, true);
    require(coordinator.event(event(Stage::NativePresentEnd, 104, 9)), "timed packet closes");
    require(coordinator.event(event(Stage::NativePresentStart, 105, 9)) && !coordinator.present(9).valid,
            "missing full ticket after timed admission became fake bootstrap");
    require(coordinator.event(event(Stage::SimulationStart, 105)), "prior concurrent frame rejected");
    require(coordinator.event(event(Stage::SimulationEnd, 105)), "prior SIM end rejected");
    require(coordinator.event(event(Stage::RenderSubmitStart, 105, 2)), "prior render start rejected");
    owner.blockedId = 7; // Key 106's Sleep blocks while key 105 must finish.
    bool newFrame{};
    std::thread ahead([&] { newFrame = coordinator.event(event(Stage::SimulationStart, 106)); });
    bool entered{};
    {
        std::unique_lock lock(owner.gateMutex);
        entered = owner.gate.wait_for(lock, std::chrono::seconds(2), [&] { return owner.entered; });
    }
    const bool earlierRenderCompleted = coordinator.event(event(Stage::RenderSubmitEnd, 105, 2));
    const bool earlierPresentOpened = coordinator.event(event(Stage::NativePresentStart, 105, 9));
    const auto prior = coordinator.present(9);
    if (prior.valid) coordinator.presented(prior, true);
    const bool earlierPresentClosed = prior.valid && coordinator.event(event(Stage::NativePresentEnd, 105, 9));
    {
        std::lock_guard lock(owner.gateMutex);
        owner.release = true;
    }
    owner.gate.notify_all();
    ahead.join();
    require(entered && earlierRenderCompleted && earlierPresentOpened && earlierPresentClosed && prior.valid && newFrame,
            "blocked future Sleep prevented prior render/present progress");
    require(owner.reentries == owner.begins, "SDK callback was not able to reenter coordinator bookkeeping");
    require(!coordinator.event(event(Stage::NativePresentStart, 100, 9)), "backward presented key was accepted");
    require(!coordinator.event(event(Stage::NativePresentStart, 107, 9, false)) && !coordinator.present(9).valid,
            "invalid native key retained a previous present scope");
    require(!coordinator.event(event(Stage::RenderSubmitStart, 107, 2, false)) && !coordinator.producer(2).valid,
            "invalid producer key retained a previous render scope");
    owner.generation = 11;
    require(!coordinator.frame(106) && !coordinator.present(9).valid, "new SDK context retained old records/scopes");
    require(coordinator.event(event(Stage::NativePresentStart, 0, 9)), "valid engine frame zero rejected");
    require(coordinator.present(9).bootstrapSdkId == 1, "new context did not re-anchor frame zero");
    coordinator.stop();
    require(!coordinator.event(event(Stage::SimulationStart, 1)) && !coordinator.producer(1).valid,
            "stopped coordinator admitted a new frame");
}
void capacityAndWrap() {
    Owner owner;
    ct::Coordinator coordinator(owner.callbacks());
    owner.coordinator = &coordinator;
    require(coordinator.event(event(Stage::NativePresentStart, 0, 9)), "zero anchor rejected");
    require(coordinator.event(event(Stage::NativePresentEnd, 0, 9)), "zero anchor packet closes");
    for (std::uint32_t key = 1; key <= 8; ++key) {
        require(coordinator.event(event(Stage::SimulationStart, key)), "bounded record reserve failed early");
        require(coordinator.event(event(Stage::SimulationEnd, key)), "reserved CPU frame completes before next start");
    }
    require(coordinator.event(event(Stage::SimulationStart, 9)) && owner.begins == 9 &&
            coordinator.stats().evicted == 1 && coordinator.stats().occupied == 8 && !coordinator.frame(1),
            "unpresented idle frame was not evicted for the ninth real frame");
    require(coordinator.event(event(Stage::RenderSubmitStart, 2, 2)) &&
            coordinator.event(event(Stage::RenderSubmitEnd, 2, 2)), "older submitted frame render completes");
    require(coordinator.event(event(Stage::NativePresentStart, 2, 9)), "first complete present rejected");
    coordinator.presented(coordinator.present(9), true);
    require(coordinator.event(event(Stage::NativePresentEnd, 2, 9)), "consumed capacity packet closes");
    require(coordinator.event(event(Stage::SimulationStart, 10)) && owner.begins == 10,
            "consumed record capacity was not reused");
    require(!coordinator.event(event(Stage::NativePresentStart, UINT32_MAX, 9)), "overflowed SDK mapping was accepted");
    owner.generation = 12;
    require(coordinator.event(event(Stage::NativePresentStart, UINT32_MAX, 9)), "high new anchor rejected");
    require(!coordinator.event(event(Stage::SimulationStart, 0)), "wrapped engine key was mistaken for a future frame");
}
void lateNativeAnchor() {
    Owner owner;
    ct::Coordinator coordinator(owner.callbacks());
    owner.coordinator = &coordinator;
    require(coordinator.bootstrapAllowed() && !coordinator.present(9).valid,
            "unobserved initial native source prevented safe LL-disabled OFF bootstrap");
    require(coordinator.event(event(Stage::SimulationStart, 200)) && owner.begins == 0 && coordinator.bootstrapAllowed(),
            "preanchor scope reserved a guessed SDK ID");
    owner.nextSdkId = 3; // Two actual ordinary OFF presents consumed IDs 1/2.
    require(coordinator.event(event(Stage::NativePresentStart, 200, 9)), "late first native key rejected");
    auto first = coordinator.present(9);
    require(first.valid && first.bootstrapSdkId == 3 && !coordinator.bootstrapAllowed(),
            "late anchor ignored the actual already-consumed SDK ID base");
    coordinator.presented(first, true);
    require(coordinator.event(event(Stage::NativePresentEnd, 200, 9)), "late initial packet closes");
    require(coordinator.event(event(Stage::SimulationStart, 203)) && coordinator.frame(203).sdkId == 6,
            "CPU-ahead ticket did not use late SDK base");
    for (std::uint32_t key : {201u, 202u}) {
        require(coordinator.event(event(Stage::NativePresentStart, key, 9)), "late older bootstrap key rejected");
        auto previous = coordinator.present(9);
        require(previous.valid && previous.bootstrapSdkId == key - 197,
                "late bootstrap collided with future reserved ticket");
        coordinator.presented(previous, true);
        require(coordinator.event(event(Stage::NativePresentEnd, key, 9)), "late bootstrap packet closes");
    }
    owner.generation = 11;
    owner.nextSdkId = 0;
    require(coordinator.event(event(Stage::NativePresentStart, 0, 9)) &&
            !coordinator.present(9).valid && coordinator.bootstrapAllowed(),
            "unknown SDK base was replaced by an invented one");
}
}
int main() {
    try {
        mappingAndConcurrency();
        capacityAndWrap();
        lateNativeAnchor();
        std::cout << "{\"stage\":\"continuous-timing-cpu\",\"passed\":true,\"checks\":" << checks
                  << ",\"cpuAheadBootstrapVerified\":true,\"sleepDoesNotBlockRender\":true"
                  << ",\"exactThreadScopeVerified\":true,\"sdkCalled\":false,\"gpuExecuted\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
