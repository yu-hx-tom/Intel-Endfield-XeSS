#include "borrowed_capture_completion.hpp"
#include <atomic>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#endif

namespace xess_vk::borrowed_capture_completion {
namespace {
std::atomic<std::uint64_t> nextContext{1};
std::uint64_t contextSerial() noexcept {
    auto value=nextContext.load(std::memory_order_relaxed);
    while(value && value!=std::numeric_limits<std::uint64_t>::max()) {
        if(nextContext.compare_exchange_weak(value,value+1,std::memory_order_relaxed))return value;
    }
    return 0;
}
void count(std::uint64_t& value) noexcept {if(value!=std::numeric_limits<std::uint64_t>::max())++value;}
bool validKey(const RecordingKey& key) noexcept {
    return key.device&&key.contextIncarnation&&key.command&&key.rpRecordingGeneration;
}
bool sameWindow(const QueueWindow& a,const QueueWindow& b) noexcept {
    return a.queue==b.queue&&a.queueFamily==b.queueFamily&&a.queueKnown==b.queueKnown&&
        a.externallySynchronizedWindowKnown==b.externallySynchronizedWindowKnown;
}
bool sameTicket(const SubmitTicket& a,const SubmitTicket& b) noexcept {
    if(a.contextSerial!=b.contextSerial||a.callSerial!=b.callSerial||a.reasons!=b.reasons||
       a.applicationFence!=b.applicationFence||a.membershipCount!=b.membershipCount||
       a.relevant!=b.relevant||a.associationKnown!=b.associationKnown||!sameWindow(a.window,b.window))return false;
    for(std::size_t i=0;i<a.membershipCount;++i)if(a.membership[i]!=b.membership[i])return false;
    return true;
}
struct ErrorGuard {
#ifdef _WIN32
    DWORD saved{GetLastError()};
    ~ErrorGuard(){SetLastError(saved);}
#endif
};
}
Context::Context() noexcept:contextSerial_(contextSerial()){}
Result Context::refuse(Reason reason,VkResult driver,bool uncertain) noexcept {
    reasons_|=bit(reason);uncertain_|=uncertain;
    if(driver==VK_ERROR_DEVICE_LOST){reasons_|=bit(Reason::DeviceLost);uncertain_=true;}
    return {bit(reason)|(driver==VK_ERROR_DEVICE_LOST?bit(Reason::DeviceLost):0),driver};
}
bool Context::exactWindow(const QueueWindow& window) const noexcept {
    return window.queueKnown&&window.externallySynchronizedWindowKnown&&window.queue==owner_.queue&&
        window.queueFamily==owner_.queueFamily;
}
Result Context::initialize(const PreparedOwner& owner) noexcept {
    ErrorGuard guard;
    if(initialized_||released_)return refuse(Reason::WrongState);
    if(!contextSerial_)return refuse(Reason::GenerationExhausted);
    if(!owner.device||!owner.queue||!owner.image||!owner.completionFence||owner.queueFamily==VK_QUEUE_FAMILY_IGNORED)
        return refuse(Reason::InvalidInput);
    if(!owner.originalQueueSubmit||!owner.originalFenceStatus)return refuse(Reason::MissingFunction);
    if(!owner.singlePhysicalDeviceKnown||!owner.ordinaryUnprotectedQueueKnown||
       !owner.uniqueExternallySynchronizedQueueKnown||!owner.inlineTargetStartsUndefined||
       !owner.inlineTargetRestoresFinalLayout||!owner.inlineSourceRestoresOriginalLayout)
        return refuse(Reason::UnsupportedOwnerRecipe);
    if(owner.mode==Mode::Readback&&(!owner.staging||!owner.readbackCommand||!owner.imageAndStagingOwned||
       !owner.readbackExecutableKnown||!owner.readbackSameFamilyKnown||!owner.readbackWritesStagingOnce||
       !owner.readbackRestoresOwnedFinalLayout||!owner.transferToHostBarrierKnown||!owner.hostCoherenceResponsibilityKnown))
        return refuse(Reason::UnsupportedOwnerRecipe);
    if(owner.mode!=Mode::Readback&&owner.mode!=Mode::FenceMarkerOnly)return refuse(Reason::InvalidInput);
    const auto driver=owner.originalFenceStatus(owner.device,owner.completionFence);
    if(driver!=VK_NOT_READY)return refuse(Reason::FenceNotUnsignaled,driver);
    owner_=owner;initialized_=true;return {};
}
Result Context::armBeforeSourceCommands(const RecordingAdmission& admission) noexcept {
    if(!initialized_||armed_||released_||terminated_||uncertain_)return refuse(Reason::WrongState);
    if(!validKey(admission.key)||admission.key.device!=owner_.device||!admission.poolGeneration||
       admission.key.command==owner_.readbackCommand)return refuse(Reason::InvalidInput);
    if(!admission.allocationKnown||!admission.poolKnown||!admission.beginFlagsKnown||
       !admission.currentlyRecordingKnown||!admission.outsideRenderingKnown||
       !admission.ordinaryUnprotectedPoolKnown||!admission.controlSupported||
       admission.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY||admission.queueFamily!=owner_.queueFamily||
       (admission.actualBeginFlags&~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)) {
        count(stats_.admissionRefusals);return refuse(Reason::UnsupportedRecording);
    }
    borrowed_=admission;armed_=true;retired_=false;return {};
}
Result Context::sourceCommandsRecorded() noexcept {
    if(!armed_||sourceCommands_||retired_||released_||uncertain_)return refuse(Reason::WrongState);
    sourceCommands_=true;return {};
}
Result Context::sourceRecordingFailed(bool anyCommandsMayHaveBeenIssued) noexcept {
    if(!armed_||sourceCommands_||originalInProgress_||privateAttempted_||released_)return refuse(Reason::WrongState);
    if(anyCommandsMayHaveBeenIssued){sourceCommands_=true;return refuse(Reason::SourceRecordingFailed,VK_SUCCESS,true);}
    armed_=false;borrowed_={};retired_=true;return refuse(Reason::SourceRecordingFailed);
}
SubmitTicket Context::preSubmit(const SubmitObservation& observation) noexcept {
    SubmitTicket ticket{};ticket.contextSerial=contextSerial_;ticket.window=observation.window;
    ticket.applicationFence=observation.applicationFence;
    if(!armed_||!sourceCommands_||released_||terminated_)return ticket;
    if(originalInProgress_||privateInProgress_){ticket.reasons=bit(Reason::SubmissionInProgress);
        refuse(Reason::SubmissionInProgress,VK_SUCCESS,true);count(stats_.associationRefusals);return ticket;}
    if(observation.completeMembership.size()<=maxMembership) {
        ticket.membershipCount=observation.completeMembership.size();
        for(std::size_t i=0;i<ticket.membershipCount;++i)ticket.membership[i]=observation.completeMembership[i];
    } else ticket.reasons|=bit(Reason::MembershipCapacity);
    std::size_t matches{};
    for(std::size_t i=0;i<ticket.membershipCount;++i)if(ticket.membership[i]==borrowed_.key)++matches;
    ticket.relevant=matches!=0||!observation.structureKnown||observation.completeMembership.size()>maxMembership;
    if(!ticket.relevant)return ticket;
    if(!observation.structureKnown)ticket.reasons|=bit(Reason::UnknownMembership);
    for(std::size_t i=0;i<ticket.membershipCount;++i)if(!validKey(ticket.membership[i]))ticket.reasons|=bit(Reason::UnknownMembership);
    if(matches!=1)ticket.reasons|=matches>1?bit(Reason::DuplicateRecording):bit(Reason::UnknownMembership);
    if(!observation.window.queueKnown||!observation.window.externallySynchronizedWindowKnown)ticket.reasons|=bit(Reason::UnknownQueue);
    else if(!exactWindow(observation.window))ticket.reasons|=bit(Reason::WrongQueue);
    if(observation.applicationFence==owner_.completionFence)ticket.reasons|=bit(Reason::ApplicationFenceAliasesPrivate);
    if(retired_)ticket.reasons|=bit(Reason::RetiredRecording);
    if(successfulOriginals_&&(borrowed_.actualBeginFlags&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))ticket.reasons|=bit(Reason::OneTimeReplay);
    if(callSerial_==std::numeric_limits<std::uint64_t>::max()) {
        ticket.reasons|=bit(Reason::GenerationExhausted);refuse(Reason::GenerationExhausted,VK_SUCCESS,true);return ticket;
    }
    ticket.callSerial=++callSerial_;ticket.associationKnown=ticket.reasons==0;
    activeCallSerial_=ticket.callSerial;activeTicket_=ticket;originalInProgress_=true;
    count(stats_.matchingOriginalCalls);
    if(ticket.reasons){reasons_|=ticket.reasons;uncertain_=true;count(stats_.associationRefusals);}
    return ticket;
}
Result Context::afterOriginalResult(const SubmitTicket& ticket,VkResult driver,const QueueWindow& window) noexcept {
    ErrorGuard guard;
    if(!ticket.relevant) {
        if(driver==VK_ERROR_DEVICE_LOST&&ticket.contextSerial==contextSerial_)
            return refuse(Reason::DeviceLost,driver,true);
        return {ticket.reasons,driver};
    }
    if(!originalInProgress_||ticket.callSerial!=activeCallSerial_||!sameTicket(ticket,activeTicket_))
        return refuse(Reason::StaleTicket,driver,true);
    originalInProgress_=false;activeCallSerial_=0;activeTicket_={};
    if(terminated_)return refuse(Reason::DeviceTerminated,driver,true);
    if(driver!=VK_SUCCESS){count(stats_.originalFailures);return refuse(Reason::OriginalFailed,driver,true);}
    count(successfulOriginals_);idleKnown_=false;
    if(successfulOriginals_>1){count(replays_);count(stats_.replaySubmissions);}
    if(!ticket.associationKnown)return {ticket.reasons,driver};
    if(!exactWindow(window)||!sameWindow(window,ticket.window))return refuse(Reason::WrongQueue,driver,true);
    if(uncertain_)return {reasons_,driver};
    if(privateAttempted_)return {};
    privateAttempted_=true;privateInProgress_=true;count(stats_.privateSubmitCalls);
    VkSubmitInfo own{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if(owner_.mode==Mode::Readback){own.commandBufferCount=1;own.pCommandBuffers=&owner_.readbackCommand;}
    const auto ownDriver=owner_.originalQueueSubmit(owner_.queue,1,&own,owner_.completionFence);
    privateInProgress_=false;
    if(ownDriver!=VK_SUCCESS){count(stats_.privateSubmitFailures);return refuse(Reason::PrivateSubmitFailed,ownDriver,true);}
    privateSucceeded_=true;count(stats_.privateSubmitSuccesses);return {};
}
Result Context::pollOwnCompletion() noexcept {
    ErrorGuard guard;
    if(!initialized_||!privateSucceeded_||originalInProgress_||privateInProgress_||released_||terminated_)
        return refuse(Reason::WrongState);
    if(uncertain_)return {reasons_,VK_SUCCESS};
    if(ownFenceSuccess_)return {};
    count(stats_.fenceQueries);const auto driver=owner_.originalFenceStatus(owner_.device,owner_.completionFence);
    if(driver==VK_NOT_READY||driver==VK_TIMEOUT){count(stats_.pendingFenceQueries);return {bit(Reason::Pending),driver};}
    if(driver!=VK_SUCCESS)return refuse(Reason::FenceFailed,driver,true);
    ownFenceSuccess_=true;count(stats_.fenceCompletions);return {};
}
Result Context::readbackConsumed() noexcept {
    if(!status().readbackReady)return refuse(Reason::WrongState);
    consumed_=true;return {};
}
void Context::observeRecordingRetired(const RecordingKey& key) noexcept {
    if(armed_&&key==borrowed_.key&&!retired_){retired_=true;count(stats_.retirements);}
}
void Context::observeQueueIdle(VkQueue queue,VkResult driver) noexcept {
    if(!initialized_||released_||queue!=owner_.queue)return;
    if(driver!=VK_SUCCESS){refuse(Reason::FenceFailed,driver,true);return;}
    if(!originalInProgress_&&!privateInProgress_){idleKnown_=true;idleAfterSubmission_=successfulOriginals_;}
}
void Context::observeDeviceIdle(VkDevice device,VkResult driver) noexcept {
    if(device==owner_.device)observeQueueIdle(owner_.queue,driver);
}
void Context::observeDeviceTerminated(VkDevice device) noexcept {
    if(initialized_&&device==owner_.device){terminated_=true;retired_=true;uncertain_=true;reasons_|=bit(Reason::DeviceTerminated);}
}
ReleaseDisposition Context::releaseDisposition() const noexcept {
    if(!initialized_||released_||originalInProgress_||privateInProgress_)return ReleaseDisposition::Retain;
    if(terminated_)return ReleaseDisposition::DropAfterDeviceTerminated;
    if(uncertain_)return ReleaseDisposition::Retain;
    if((!armed_||retired_)&&!successfulOriginals_&&!privateAttempted_&&stats_.matchingOriginalCalls==0)
        return ReleaseDisposition::DiscardRetiredNeverSubmitted;
    if(!retired_||!ownFenceSuccess_||(owner_.mode==Mode::Readback&&!consumed_))return ReleaseDisposition::Retain;
    if(replays_&&(!idleKnown_||idleAfterSubmission_!=successfulOriginals_))return ReleaseDisposition::Retain;
    return ReleaseDisposition::ReleaseCompleted;
}
Result Context::markOwnerReleased() noexcept {
    if(releaseDisposition()==ReleaseDisposition::Retain)return refuse(Reason::OwnerNotReleasable);
    released_=true;count(stats_.releases);return {};
}
Status Context::status() const noexcept {
    Status value{};value.borrowed=borrowed_;value.reasons=reasons_;value.successfulOriginalSubmissions=successfulOriginals_;
    value.replays=replays_;value.initialized=initialized_;value.armed=armed_;value.sourceCommandsMayExist=sourceCommands_;
    value.recordingRetired=retired_;value.originalCallInProgress=originalInProgress_;value.privateCallInProgress=privateInProgress_;
    value.privateSubmitSucceeded=privateSucceeded_;value.ownFenceSuccessObserved=ownFenceSuccess_;
    value.readbackReady=ownFenceSuccess_&&privateSucceeded_&&owner_.mode==Mode::Readback&&!uncertain_&&!terminated_&&!released_;
    value.readbackConsumed=consumed_;value.uncertain=uncertain_;value.deviceTerminated=terminated_;value.ownerReleased=released_;
    value.release=releaseDisposition();return value;
}
} // namespace xess_vk::borrowed_capture_completion
