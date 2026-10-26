#include "fsr_hook_transaction.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace transaction = xess_vk::fsr_hook_transaction;
using Stage = transaction::Stage;
using Failure = transaction::Failure;
using Step = std::pair<Stage, std::size_t>;
std::uint64_t checks{}, cases{};
void require(bool okay, const char* reason) { ++checks; if (!okay) throw std::runtime_error(reason); }
enum class Fault { None, RejectBefore, RejectAfter, ThrowBefore, ThrowAfter };
enum class Gate { Cold, Configured, Active, Stopped };
struct Hook {
    bool validated{}, created{}, published{}, configured{}, checkedBefore{}, enabled{}, checkedAfter{}, activated{};
    std::uintptr_t trampoline{}, forwarding{};
};
struct Fixture {
    std::uint32_t mask=3;
    std::vector<Step> calls;
    std::array<Hook,transaction::maximumTargetCount> hooks;
    std::array<std::atomic<Gate>,transaction::maximumTargetCount> gates{};
    std::atomic<bool> stopped{};
    bool validated{}, pinned{}, initialized{};
    unsigned initializes{}, stopCalls{}, queryCalls{}, activationAttempts{};
    unsigned orderingErrors{};
    Fault fault=Fault::None;
    std::size_t faultCall{}, stopQuery{};
    // Deterministic scheduler: transaction worker pauses inside an admitted
    // operation. The other thread revokes the helpers without holding its lock.
    std::mutex pauseMutex;
    std::condition_variable pauseCv;
    std::size_t pauseCall{};
    bool pauseAfter{}, paused{}, released{};

    bool every(bool Hook::*member) const {
        for(std::size_t index=0;index<hooks.size();++index) if((mask&(1u<<index)) && !(hooks[index].*member))return false;
        return true;
    }
    void check(bool condition) { if(!condition)++orderingErrors; }
    void externalStop() noexcept {
        stopped.store(true);
        for(auto& gate:gates)gate.store(Gate::Stopped);
    }
    void pause(std::size_t call,bool after) {
        if(pauseCall!=call || pauseAfter!=after)return;
        std::unique_lock lock(pauseMutex);paused=true;pauseCv.notify_all();
        pauseCv.wait(lock,[&]{return released;});
    }
    bool effect(Stage stage,std::size_t index) {
        auto& hook=hooks[index<hooks.size()?index:0];
        const auto shared = transaction::sharedFor(mask);
        switch(stage) {
        case Stage::ValidateShared:
            check(index==shared && !validated);validated=true;return true;
        case Stage::ValidateTarget:
            check(validated && !initialized && index<hooks.size() && !hook.validated);hook.validated=true;return true;
        case Stage::PrepareShared:
            check(index==shared && every(&Hook::validated) && !initialized);
            pinned=true;initialized=true;++initializes;return true;
        case Stage::CreateTarget:
            check(pinned && initialized && initializes==1 && every(&Hook::validated) && !hook.created);
            hook.created=true;hook.trampoline=0x1000+index*0x100;return true;
        case Stage::PublishTarget:
            check(every(&Hook::created) && !hook.published && hook.trampoline);
            hook.forwarding=hook.trampoline;hook.published=true;return true;
        case Stage::ConfigureTarget: {
            check(every(&Hook::published) && !hook.configured);
            auto expected=Gate::Cold;
            if(!gates[index].compare_exchange_strong(expected,Gate::Configured))return false;
            hook.configured=true;return true;
        }
        case Stage::CheckBeforeEnable:
            check(every(&Hook::configured) && hook.forwarding==hook.trampoline && !hook.enabled);
            hook.checkedBefore=true;return true;
        case Stage::EnableTarget:
            check(every(&Hook::checkedBefore) && every(&Hook::configured) && every(&Hook::published) && !hook.enabled);
            hook.enabled=true;return true;
        case Stage::CheckAfterEnable:
            check(every(&Hook::enabled));hook.checkedAfter=true;return true;
        case Stage::ActivateTarget: {
            check(every(&Hook::checkedAfter) && every(&Hook::enabled));++activationAttempts;
            auto expected=Gate::Configured;
            if(!gates[index].compare_exchange_strong(expected,Gate::Active))return false;
            hook.activated=true;return true;
        }
        case Stage::Complete: ++orderingErrors;return false;
        }
        return false;
    }
    bool step(Stage stage,std::size_t index) {
        calls.emplace_back(stage,index);
        const auto call=calls.size();pause(call,false);
        if(call==faultCall && fault==Fault::RejectBefore)return false;
        if(call==faultCall && fault==Fault::ThrowBefore)throw std::runtime_error("injected before side effect");
        const auto okay=effect(stage,index);pause(call,true);
        if(call==faultCall && fault==Fault::RejectAfter)return false;
        if(call==faultCall && fault==Fault::ThrowAfter)throw std::runtime_error("injected after side effect");
        return okay;
    }
    transaction::Operations operations() {
        return {this,
            [](void* user,Stage stage,std::size_t index){return static_cast<Fixture*>(user)->step(stage,index);},
            [](void* user) noexcept {
                auto& f=*static_cast<Fixture*>(user);
                if(++f.queryCalls==f.stopQuery)f.externalStop();
                return f.stopped.load();
            },
            [](void* user) noexcept {auto& f=*static_cast<Fixture*>(user);++f.stopCalls;f.externalStop();}};
    }
    transaction::Result run() {return transaction::run(mask,operations());}
};
std::vector<Step> expected(std::uint32_t mask) {
    const auto shared = transaction::sharedFor(mask);
    std::vector<Step> sequence{{Stage::ValidateShared,shared}};
    const auto add=[&](Stage stage){for(std::size_t i=0;i<transaction::maximumTargetCount;++i)if(mask&(1u<<i))sequence.emplace_back(stage,i);};
    add(Stage::ValidateTarget);sequence.emplace_back(Stage::PrepareShared,shared);
    for(const auto stage:std::array{Stage::CreateTarget,Stage::PublishTarget,Stage::ConfigureTarget,Stage::CheckBeforeEnable,
                                  Stage::EnableTarget,Stage::CheckAfterEnable,Stage::ActivateTarget})add(stage);
    return sequence;
}
void verifyPrefix(const Fixture& f,const std::vector<Step>& sequence) {
    require(f.calls.size()<=sequence.size(),"extra transaction operations");
    for(std::size_t i=0;i<f.calls.size();++i)require(f.calls[i]==sequence[i],"transaction stage order changed");
    require(f.orderingErrors==0,"preconditions/postconditions violated");
    require(f.initializes<=1,"private initialization repeated");
    for(const auto& hook:f.hooks) {
        require(!hook.published || (hook.created && hook.trampoline && hook.forwarding==hook.trampoline),"published forwarding was removed/corrupted");
        require(!hook.enabled || (hook.created && hook.published && f.pinned && f.initialized),"enabled code lost trampoline or pin");
    }
}
void verifyFailed(const Fixture& f,const transaction::Result& result,const std::vector<Step>& sequence) {
    verifyPrefix(f,sequence);
    require(!result.completed && result.failure!=Failure::None && f.stopped,"failed transaction did not stop");
    require(f.stopCalls==1,"runner did not invoke stop once");
    for(const auto& gate:f.gates)require(gate.load()==Gate::Stopped,"one helper remained active/configurable after failure");
}
void successfulSchedules() {
    for(const auto mask:std::array<std::uint32_t,11>{1,2,3,transaction::continuousMask,transaction::timedContinuousMask,transaction::currentContinuousMask,transaction::localFiOffMask,transaction::srObservationMask,transaction::localFiSrObservationMask,transaction::srNativeObservationMask,transaction::localFiSrNativeObservationMask}) {
        ++cases;Fixture f;f.mask=mask;const auto result=f.run();const auto sequence=expected(mask);
        verifyPrefix(f,sequence);
        require(f.calls==sequence && result.completed && result.failure==Failure::None && result.stage==Stage::Complete,"successful schedule incomplete");
        require(f.initializes==1 && f.stopCalls==0 && !f.stopped,"success stopped/reinitialized");
        for(unsigned i=0;i<f.gates.size();++i)require(f.gates[i].load()==((mask&(1u<<i))?Gate::Active:Gate::Cold),"unrequested/required helper activated incorrectly");
        if(mask==3)require(f.hooks[0].forwarding!=f.hooks[1].forwarding,"shared forwarding route");
    }
}
void allRejectionAndExceptionStages() {
    for(const auto mask:std::array<std::uint32_t,9>{1,2,3,transaction::currentContinuousMask,transaction::localFiOffMask,transaction::srObservationMask,transaction::localFiSrObservationMask,transaction::srNativeObservationMask,transaction::localFiSrNativeObservationMask}) {
        const auto sequence=expected(mask);
        for(std::size_t at=1;at<=sequence.size();++at)for(const auto fault:std::array{Fault::RejectBefore,Fault::RejectAfter,Fault::ThrowBefore,Fault::ThrowAfter}) {
            ++cases;Fixture f;f.mask=mask;f.fault=fault;f.faultCall=at;const auto result=f.run();
            verifyFailed(f,result,sequence);
            require(f.calls.size()==at && result.stage==sequence[at-1].first && result.hookIndex==sequence[at-1].second,"failure location lost or later stage executed");
            require(result.failure==((fault==Fault::ThrowBefore||fault==Fault::ThrowAfter)?Failure::Exception:Failure::Rejected),"failure classification");
            // Once a first target is enabled, failing the peer must retain the
            // first forwarder but never leave either helper sampling.
            if(mask==3 && sequence[at-1]==Step{Stage::EnableTarget,1})
                require(f.hooks[0].enabled && f.hooks[0].published && !f.hooks[0].activated && !f.hooks[1].activated,"partial enable failure behavior");
        }
    }
}
void everyStopBoundary() {
    for(const auto mask:std::array<std::uint32_t,9>{1,2,3,transaction::currentContinuousMask,transaction::localFiOffMask,transaction::srObservationMask,transaction::localFiSrObservationMask,transaction::srNativeObservationMask,transaction::localFiSrNativeObservationMask}) {
        const auto sequence=expected(mask);
        for(std::size_t query=1;query<=sequence.size()*2;++query) {
            ++cases;Fixture f;f.mask=mask;f.stopQuery=query;const auto result=f.run();
            verifyFailed(f,result,sequence);
            require(result.failure==Failure::Stopped,"before/after-stage stop not recognized");
            require(f.calls.size()==query/2,"stop allowed extra operation");
            const auto failedIndex=(query-1)/2;
            require(result.stage==sequence[failedIndex].first && result.hookIndex==sequence[failedIndex].second,"stop stage attribution wrong");
        }
    }
}
void concurrentStops() {
    const auto sequence=expected(3);
    // Pause inside every stage, both before and after its side effects. This
    // includes both enables and both activations; external stop never waits on
    // transaction's execution thread and uses sticky helper gates.
    for(std::size_t at=1;at<=sequence.size();++at)for(const auto after:std::array{false,true}) {
        ++cases;Fixture f;f.pauseCall=at;f.pauseAfter=after;transaction::Result result;
        std::thread worker([&]{result=f.run();});
        bool reached{};
        {std::unique_lock lock(f.pauseMutex);reached=f.pauseCv.wait_for(lock,std::chrono::seconds(10),[&]{return f.paused;});}
        f.externalStop();
        {std::lock_guard lock(f.pauseMutex);f.released=true;}f.pauseCv.notify_all();worker.join();
        require(reached,"pause scheduling failure");verifyFailed(f,result,sequence);
        require(f.calls.size()==at,"concurrent stop allowed another stage");
        require(result.failure==Failure::Stopped || result.failure==Failure::Rejected,"concurrent stop unexpected outcome");
    }
}
void badContracts() {
    for(const auto mask:std::array<std::uint32_t,7>{0,4,7,8,0xffffffffu,transaction::currentContinuousMask|(1u<<13),transaction::currentContinuousMask|(1u<<14)}) {
        ++cases;Fixture f;f.mask=mask;const auto result=f.run();
        require(result.failure==Failure::InvalidContract && f.calls.empty() && f.stopCalls==1,"bad mask called stages");
        require(f.gates[0]==Gate::Stopped && f.gates[1]==Gate::Stopped,"bad mask did not revoke helpers");
    }
    for(unsigned missing=0;missing<3;++missing) {
        ++cases;Fixture f;auto operations=f.operations();
        if(missing==0)operations.step=nullptr;
        if(missing==1)operations.stopped=nullptr;
        if(missing==2)operations.stopAll=nullptr;
        const auto result=transaction::run(3,operations);
        require(result.failure==Failure::InvalidContract && f.calls.empty(),"missing callback executed stages");
        require(f.stopCalls==(missing==2?0:1),"invalid-contract stop callback count");
    }
    ++cases;Fixture f;f.externalStop();const auto result=f.run();
    verifyFailed(f,result,expected(3));require(f.calls.empty(),"pre-stopped transaction executed stage");
}
void continuousPostcheckFailure() {
    ++cases;
    Fixture fixture;
    fixture.mask = transaction::continuousMask;
    fixture.fault = Fault::RejectBefore;
    const auto sequence = expected(fixture.mask);
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        if (sequence[i] == Step{Stage::CheckAfterEnable, 4}) fixture.faultCall = i + 1;
    }
    const auto result = fixture.run();
    verifyFailed(fixture, result, sequence);
    require(result.hookIndex == 4 && result.stage == Stage::CheckAfterEnable,
            "camera postcheck failure attributed to a shared legacy index");
    for (std::size_t index = 0; index < fixture.hooks.size(); ++index) {
        const auto& hook = fixture.hooks[index];
        if (!(fixture.mask & (1u << index))) continue;
        require(hook.enabled && hook.published && !hook.activated,
                "continuous postcheck failure removed forwarding or activated a peer");
    }
}
void timedPostcheckFailure() {
    ++cases;
    Fixture fixture;
    fixture.mask = transaction::timedContinuousMask;
    fixture.fault = Fault::RejectBefore;
    const auto sequence = expected(fixture.mask);
    for (std::size_t i = 0; i < sequence.size(); ++i)
        if (sequence[i] == Step{Stage::CheckAfterEnable, transaction::timedTargetCount - 1}) fixture.faultCall = i + 1;
    const auto result = fixture.run();
    verifyFailed(fixture, result, sequence);
    require(result.hookIndex == transaction::timedTargetCount - 1 && result.stage == Stage::CheckAfterEnable,
            "enqueue target postcheck error attribution lost");
    for (std::size_t i = 0; i < fixture.hooks.size(); ++i)
        if (fixture.mask & (1u << i)) require(fixture.hooks[i].enabled && fixture.hooks[i].published && !fixture.hooks[i].activated,
                "enqueue target postcheck failure prematurely activated a peer");
}
void localPairPostcheckFailure() {
    ++cases;
    Fixture fixture;
    fixture.mask = transaction::localFiOffMask;
    fixture.fault = Fault::RejectAfter;
    const auto sequence = expected(fixture.mask);
    for (std::size_t i = 0; i < sequence.size(); ++i)
        if (sequence[i] == Step{Stage::CheckAfterEnable, transaction::localFiTargetCount - 1}) fixture.faultCall = i + 1;
    const auto result = fixture.run();
    verifyFailed(fixture, result, sequence);
    require(fixture.activationAttempts == 0 && result.hookIndex == 14,
            "the second local decision postcheck cannot activate any helper or commit a pair");
    require(fixture.hooks[13].published && fixture.hooks[14].published &&
            fixture.hooks[13].enabled && fixture.hooks[14].enabled,
            "partially enabled local forwarders stay retained and passive after failure");
}
} // namespace
int main() {
    try {
        successfulSchedules();allRejectionAndExceptionStages();everyStopBoundary();concurrentStops();badContracts();continuousPostcheckFailure();timedPostcheckFailure();localPairPostcheckFailure();
        std::cout<<"{\"passed\":true,\"cases\":"<<cases<<",\"checks\":"<<checks
                 <<",\"productionStageRunnerUsed\":true,\"simulatedPlatformOperations\":true,\"realMinHookUsed\":false"
                 <<",\"gameTouched\":false,\"gpuTouched\":false}\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<" cases="<<cases<<" checks="<<checks<<'\n';return 1;}
}
