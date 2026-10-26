// Reuse the existing CPU driver/backend, but run only these origin-boundary cases.
// This includes the production layer; it never loads a Vulkan driver or RTSS.
#define main existingPresentationLayerTestsMain
#include "presentation_runtime_layer_test.cpp"
#undef main

namespace typed_origin_fixture {
using namespace presentation_layer_fixture;
using Stage = xess_vk::game_timing::Stage;

enum class OverlayAction {
    Keep, ConsumeWaits, ClearWithoutSubmit, FailedSubmit, WrongQueueSubmit,
    DifferentSignalCycle, ChangeImage, ChangeWait, ChangeExtension, NestedSource, DuplicateConsumer,
    StopBeforeConsumer, ThrowBeforeConsumer, CancelLease, RecreateIdentity, ChangeContext,
    ConsumeThenCancel, ConsumeThenExtraWait
};
DispatchObject otherQueueObject{&marker};
const auto otherQueue = reinterpret_cast<VkQueue>(&otherQueueObject);

struct OwnedCall;
OwnedCall* activeCall{};
VKAPI_ATTR VkResult VKAPI_CALL overlay(VkQueue, const VkPresentInfoKHR*);

struct OwnedCall {
    Fixture fixture;
    PresentOriginBinding binding;
    VkSwapchainKHR chain{};
    VkSemaphore wait{}, alternateWait{};
    VkResult individual{};
    std::uint32_t index{1};
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    VkBaseInStructure extension{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr};
    OverlayAction action{OverlayAction::Keep};
    xess_vk::FrameTimingTicket expected;
    bool firstTimed{}, secondTimed{}, pinnedDuringStop{}, inFlightWithoutClaim{};

    explicit OwnedCall(bool deferredStartup = false) {
        auto& t = fixture.observation;
        if (deferredStartup) { t.timingGeneration = 0; t.contextAppearsOnCopy = true; }
        t.timingTable = std::make_unique<xess_vk::detail::PresenterFrameTimings>(1, true);
        require(fixture.create(chain) == VK_SUCCESS, "owned origin creates the real Runtime over a CPU driver");
        installBootstrapCoordinator(fixture);
        binding.state = fixture.state;
        binding.next = overlay;
        binding.active.store(true);
        info.swapchainCount = 1;
        info.pSwapchains = &chain;
        info.pImageIndices = &index;
        info.pResults = &individual;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &wait;
        t.appPresent = &info;
        activeCall = this;
        wait = fixture.semaphore(); fixture.signal(wait);
        if (deferredStartup) return;
        require(packetStage(fixture, Stage::NativePresentStart, 100), "bootstrap owns a current native-present scope");
        invoke(PresentOriginRoute::Direct);
        require(packetStage(fixture, Stage::NativePresentEnd, 100) && t.selectedBootstrap == 1 && !t.untimed,
                "the production origin/consumer path consumes its real bootstrap ID");
        completeFuture(fixture, 101);
        expected = fixture.state->continuousTiming->frame(101);
        require(bool(expected), "the next case has a completed exact Coordinator ticket");
        wait = fixture.semaphore(); fixture.signal(wait);
        alternateWait = fixture.semaphore(); fixture.signal(alternateWait);
        require(packetStage(fixture, Stage::NativePresentStart, 101), "the ticket owns the current native-present scope");
        firstTimed = secondTimed = false;
    }
    ~OwnedCall() { activeCall = nullptr; }
    void invoke(PresentOriginRoute route) {
        {
            PresentOriginScope source(binding, queue, &info, route);
            require(binding.next(queue, &info) == VK_SUCCESS, "source calls its actual next PFN and preserves its result");
        }
        require(binding.inFlight.load() == 0 && fixture.state->packets->stats().inFlight == 0 &&
                fixture.state->continuousTiming->stats().packetPins == 0,
                "returning from the outer PFN releases the exact carrier lease and timing pin");
    }
    VkResult invokeWrapper() {
        auto* saved = presentOriginBinding.exchange(&binding);
        VkResult result{};
        try { result = unityOriginPresent(queue, &info); }
        catch (...) { presentOriginBinding.store(saved); throw; }
        presentOriginBinding.store(saved);
        return result;
    }
    void submitWait(VkQueue submitQueue, VkSemaphore semaphore, VkResult result) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &semaphore;
        submit.pWaitDstStageMask = &stage;
        fixture.observation.appSubmitResult = result;
        require(vkQueueSubmit(submitQueue, 1, &submit, VK_NULL_HANDLE) == result,
                "overlay wait consumption crosses the production submit wrapper and actual driver result");
        fixture.observation.appSubmitResult = VK_SUCCESS;
    }
    void consume(VkQueue submitQueue, VkResult result) {
        submitWait(submitQueue, wait, result);
        info.waitSemaphoreCount = 0; info.pWaitSemaphores = nullptr;
    }
};

VKAPI_ATTR VkResult VKAPI_CALL overlay(VkQueue q, const VkPresentInfoKHR* packet) {
    auto& c = *activeCall;
    auto& t = c.fixture.observation;
    require(q == queue && packet == &c.info, "the overlay receives this exact synchronous outer packet");
    c.inFlightWithoutClaim = c.binding.inFlight.load() == 1 &&
                            c.fixture.state->packets->stats().inFlight == 0;
    switch (c.action) {
    case OverlayAction::Keep: break;
    case OverlayAction::ConsumeWaits: c.consume(q, VK_SUCCESS); break;
    case OverlayAction::ClearWithoutSubmit:
        c.info.waitSemaphoreCount = 0; c.info.pWaitSemaphores = nullptr; break;
    case OverlayAction::FailedSubmit: c.consume(q, VK_ERROR_OUT_OF_HOST_MEMORY); break;
    case OverlayAction::WrongQueueSubmit: c.consume(otherQueue, VK_SUCCESS); break;
    case OverlayAction::DifferentSignalCycle:
        c.fixture.signal(c.wait); c.consume(q, VK_SUCCESS); break;
    case OverlayAction::ChangeImage: c.index = 0; break;
    case OverlayAction::ChangeWait: c.info.pWaitSemaphores = &c.alternateWait; break;
    case OverlayAction::ChangeExtension: c.info.pNext = &c.extension; break;
    case OverlayAction::NestedSource: {
        // An indistinguishable reentrant call cannot certify either invocation.
        t.copySubmit = false;
        PresentOriginScope nested(c.binding, q, packet, PresentOriginRoute::Direct);
        require(vkQueuePresentKHR(q, packet) == VK_SUCCESS && t.untimed,
                "a nested origin cannot consume the outer ticket");
        break;
    }
    case OverlayAction::DuplicateConsumer: break;
    case OverlayAction::StopBeforeConsumer:
        c.binding.active.store(false);
        c.fixture.state->packets->stop(); c.fixture.state->continuousTiming->stop();
        c.pinnedDuringStop = c.fixture.state->packets->stats().inFlight == 1 &&
                             c.fixture.state->continuousTiming->stats().packetPins == 1 &&
                             c.binding.inFlight.load() == 1;
        break;
    case OverlayAction::ThrowBeforeConsumer: throw std::runtime_error("owned overlay failure");
    case OverlayAction::CancelLease: c.fixture.state->packets->invalidateUnclaimed(); break;
    case OverlayAction::RecreateIdentity:
        c.fixture.state->packetIdentities->swapchainDestroyed(bits(c.chain));
        require(c.fixture.state->packetIdentities->swapchainCreated(bits(c.chain), VK_SUCCESS),
                "the registry observes a new lifetime of the same native handle");
        break;
    case OverlayAction::ChangeContext: ++t.timingGeneration; break;
    case OverlayAction::ConsumeThenCancel:
        c.consume(q, VK_SUCCESS); c.fixture.state->packets->invalidateUnclaimed(); break;
    case OverlayAction::ConsumeThenExtraWait:
        c.consume(q, VK_SUCCESS); c.submitWait(q, c.alternateWait, VK_SUCCESS); break;
    }
    const auto result = vkQueuePresentKHR(q, packet);
    c.firstTimed = !t.untimed && t.selectedTiming == c.expected;
    if (c.action == OverlayAction::DuplicateConsumer) {
        require(c.firstTimed, "the first consumer obtains the exact source ticket");
        // The second packet is intentionally invalid app behavior. No real GPU
        // wait is issued by this CPU driver; only timing authorization is tested.
        t.copySubmit = false;
        require(vkQueuePresentKHR(q, packet) == VK_SUCCESS, "duplicate still forwards through the actual layer");
        c.secondTimed = !t.untimed;
    }
    return result;
}

void exactAndConsumedPackets() {
    for (const auto action : {OverlayAction::Keep, OverlayAction::ConsumeWaits}) {
        OwnedCall c; c.action = action;
        const auto before = c.fixture.observation.sdkId;
        c.invoke(PresentOriginRoute::Direct);
        require(c.firstTimed, action == OverlayAction::Keep ?
                "unchanged packet selects the exact source ticket" :
                "successful same-queue wait consumption selects the exact source ticket");
        require(c.fixture.observation.sdkId == before + 1 &&
                c.fixture.state->continuousTiming->stats().timedPresents == 1,
                "the selected ticket completes one timed output with one SDK ID advance");
        require(c.fixture.observation.copiedPresent == &c.info &&
                c.fixture.observation.copiedImage == c.fixture.observation.live.at(c.chain)[1],
                "private copy receives the actual downstream packet and exact image");
        if (action == OverlayAction::ConsumeWaits)
            require(c.info.waitSemaphoreCount == 0 && !c.info.pWaitSemaphores &&
                    !c.fixture.observation.consumed.count(c.wait),
                    "private copy never replays the binary wait already consumed by the overlay submit");
    }
}
void refusedTransformations() {
    for (const auto action : {OverlayAction::ClearWithoutSubmit, OverlayAction::FailedSubmit,
             OverlayAction::WrongQueueSubmit, OverlayAction::DifferentSignalCycle,
             OverlayAction::ChangeImage, OverlayAction::ChangeWait, OverlayAction::ChangeExtension,
             OverlayAction::NestedSource, OverlayAction::CancelLease,
             OverlayAction::RecreateIdentity, OverlayAction::ChangeContext,
             OverlayAction::ConsumeThenCancel, OverlayAction::ConsumeThenExtraWait}) {
        OwnedCall c; c.action = action;
        c.invoke(PresentOriginRoute::Direct);
        require(!c.firstTimed && !c.fixture.state->continuousTiming->stats().timedPresents,
                "unproved wait changes, changed image/cycle or nested source cannot authorize the outer ticket");
    }
}
void leaseAndShutdown() {
    {
        OwnedCall c; c.action = OverlayAction::DuplicateConsumer;
        c.invoke(PresentOriginRoute::Direct);
        require(c.firstTimed && !c.secondTimed, "an outer lease is consumable once even if the next PFN calls twice");
    }
    {
        OwnedCall c; c.action = OverlayAction::StopBeforeConsumer;
        c.invoke(PresentOriginRoute::Direct);
        require(c.pinnedDuringStop && !c.firstTimed,
                "stop refuses new timing while retaining the cancelled lease until its outer call returns");
    }
    {
        OwnedCall c; c.action = OverlayAction::ThrowBeforeConsumer;
        bool threw{};
        try { c.invoke(PresentOriginRoute::Direct); }
        catch (const std::runtime_error& error) { threw = std::string(error.what()) == "owned overlay failure"; }
        require(threw && !c.fixture.state->packets->stats().inFlight &&
                !c.fixture.state->continuousTiming->stats().packetPins,
                "an exception before the layer still releases the source lease and restores TLS");
    }
}
void asynchronousSourceAndExpiredScope() {
    {
        OwnedCall c;
        require(enqueuePacket(c.fixture, c.chain, c.wait), "the existing asynchronous source publishes this exact packet");
        c.invoke(PresentOriginRoute::Queued);
        require(c.firstTimed, "a queued origin claims its existing exact source instead of publishing current scope");
    }
    {
        OwnedCall c;
        c.invoke(PresentOriginRoute::Queued);
        require(!c.firstTimed && c.inFlightWithoutClaim,
                "a source without a claim stays counted until return and cannot borrow the active direct scope");
    }
    {
        OwnedCall c;
        require(vkQueuePresentKHR(queue, &c.info) == VK_SUCCESS && c.fixture.observation.untimed,
                "a layer call outside any typed source cannot use an active native-present scope as authority");
    }
    {
        OwnedCall c;
        require(enqueuePacket(c.fixture, c.chain, c.wait),
                "the unknown-caller counterexample starts with a live exact asynchronous row");
        require(c.invokeWrapper() == VK_SUCCESS && c.fixture.observation.untimed && !presentOriginScope &&
                !c.fixture.state->packets->stats().inFlight &&
                c.fixture.state->continuousTiming->stats().timedPresents == 0,
                "an unknown source cannot borrow a live asynchronous row through the legacy consumer");
    }
    {
        OwnedCall c; c.binding.active.store(false);
        require(c.invokeWrapper() == VK_SUCCESS && c.fixture.observation.untimed && !presentOriginScope &&
                !c.binding.inFlight.load() && !c.fixture.state->packets->stats().inFlight,
                "the disabled real source wrapper forwards its original packet without entering owner state");
    }
}
void deferredStartupGap() {
    OwnedCall c(true);
    auto& f = c.fixture; auto& t = f.observation;
    for (const auto key : {6u, 7u, 8u}) completeFuture(f, key);
    require(packetStage(f, Stage::NativePresentStart, 6), "deferred context observes its actual initial present scope");
    c.invoke(PresentOriginRoute::Direct);
    require(t.sdkId == 1 && f.state->presentation->lastSdkOutputSucceeded(),
            "context-zero typed source reaches first real OFF output and initializes its owner");
    require(c.inFlightWithoutClaim, "context-zero source remains counted despite having no carrier claim");
    require(!packetStage(f, Stage::NativePresentEnd, 6),
            "initializing the new context invalidates its preceding context-zero scope");
    completeFuture(f, 9); c.expected = f.state->continuousTiming->frame(9);
    require(bool(c.expected), "the deferred owner reserves its actual future ticket");
    for (const auto key : {7u, 8u}) {
        c.wait = f.semaphore(); f.signal(c.wait);
        require(packetStage(f, Stage::NativePresentStart, key), "older packet has its own active present scope");
        c.invoke(PresentOriginRoute::Direct);
        require(!f.state->continuousTimingRetired && !f.state->presentation->lastSdkOutputSucceeded() &&
                t.sdkId == 1 && t.timingTable->stats().occupied == 1 && c.inFlightWithoutClaim &&
                packetStage(f, Stage::NativePresentEnd, key),
                "typed startup gaps preserve the future reservation without claiming a ticket or advancing SDK ID");
    }
    c.wait = f.semaphore(); f.signal(c.wait);
    require(packetStage(f, Stage::NativePresentStart, 9), "the exact future frame reaches its own scope");
    c.invoke(PresentOriginRoute::Direct);
    require(c.firstTimed && t.sdkId == 2 && !f.state->continuousTimingRetired &&
            f.state->continuousTiming->stats().startupGapDeferrals == 2,
            "the original future ticket produces the next timed output after the two typed startup gaps");
}
} // namespace typed_origin_fixture

int main() {
    using namespace typed_origin_fixture;
    try {
        SetEnvironmentVariableW(L"XESS_OBSERVER_LOG", nullptr);
        SetEnvironmentVariableW(L"XESS_PRESENTATION_OFF", nullptr);
        InitOnceExecuteOnce(&logOnce, initializeLog, nullptr, nullptr);
        exactAndConsumedPackets(); refusedTransformations(); leaseAndShutdown(); asynchronousSourceAndExpiredScope();
        deferredStartupGap();
        std::cout << "{\"stage\":\"presentation-typed-origin-actual-layer\",\"passed\":true,\"checks\":" << checks
                  << ",\"realVulkanCalls\":0,\"gpuTouched\":false,\"gameTouched\":false,\"realRtssWrapperTested\":false}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
