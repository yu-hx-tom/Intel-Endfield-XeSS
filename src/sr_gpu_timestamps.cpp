#include "sr_gpu_timestamps.hpp"
#include <windows.h>
#include <cmath>

namespace xess_vk::sr_gpu_timestamps {
namespace {
struct ErrorGuard {DWORD value=GetLastError();~ErrorGuard(){SetLastError(value);}};
template<class T> std::uint64_t bits(T value) noexcept{return reinterpret_cast<std::uintptr_t>(value);}
std::uint64_t qpc() noexcept {LARGE_INTEGER value{};return QueryPerformanceCounter(&value)?value.QuadPart:0;}
bool sameRecording(const CommandRecordingFacts& a,const CommandRecordingFacts& b) noexcept {
    return a.device==b.device&&a.contextIncarnation==b.contextIncarnation&&a.command==b.command&&
        a.rpRecordingGeneration==b.rpRecordingGeneration&&a.pool==b.pool&&a.poolGeneration==b.poolGeneration&&
        a.queueFamily==b.queueFamily&&a.level==b.level&&a.beginUsageFlags==b.beginUsageFlags;
}
}
void Observer::skip(Skip why) noexcept {++stats_.skipped[std::size_t(why)];}
Observer::Observer(const Config& config) noexcept:config_(config) {
    ErrorGuard error;
    if(!config.enabled)return;
    const auto& f=config.functions;
    LARGE_INTEGER frequency{};
    active_=config.device&&config.incarnation&&config.window&&config.queueFamily!=VK_QUEUE_FAMILY_IGNORED&&
        (config.queueFlags&VK_QUEUE_COMPUTE_BIT)&&config.validBits&&config.validBits<=64&&
        config.periodNs>0&&std::isfinite(config.periodNs)&&f.create&&f.destroy&&f.reset&&f.write&&f.read&&
        config.sink&&QueryPerformanceFrequency(&frequency)&&frequency.QuadPart>0;
    if(active_)qpcFrequency_=frequency.QuadPart;
}
WriteTicket Observer::beforeDispatch(const CommandRecordingFacts& f,const Identity& identity) noexcept {
    ErrorGuard error;
    if(!active_){skip(Skip::Disabled);return {};}
    const bool whole=identity.pass==Pass::WholeFsr;
    if(identity.window!=config_.window||identity.pass==Pass::Unknown||!identity.ordinal||(!whole&&!identity.callback)||
        !identity.pipeline||!identity.pipelineGeneration||!identity.frame.valid||!identity.frame.ticket||
        (!whole&&(!identity.extentsKnown||!identity.output.width||!identity.output.height))||!identity.render.width||!identity.render.height||
        !identity.groups[0]||!identity.groups[1]||!identity.groups[2]){
        skip(Skip::Identity);return {};
    }
    if(f.device!=bits(config_.device)||f.contextIncarnation!=config_.incarnation||!f.command||!f.rpRecordingGeneration||
        !f.pool||!f.poolGeneration||!f.allocationKnown||!f.poolKnown||!f.beginFlagsKnown||!f.stateKnown||!f.recording||
        !f.scopeKnown||f.inRenderPass||f.inDynamicRendering||f.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY||
        f.queueFamily!=config_.queueFamily||f.poolFlags&VK_COMMAND_POOL_CREATE_PROTECTED_BIT||
        f.beginUsageFlags&~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT||
        f.reasons&~bit(CommandRecordingReason::NotOneTime)){
        stats_.rejectedRecordingReasons|=f.reasons;skip(Skip::Recording);return {};
    }
    std::size_t index=recordingCapacity,empty=recordingCapacity;
    for(std::size_t n=0;n<recordings_.size();++n){const auto& r=recordings_[n];
        if(!r.pool){if(empty==recordingCapacity)empty=n;continue;}
        if(r.facts.command==f.command){if(!sameRecording(r.facts,f)){skip(Skip::Recording);return {};}index=n;break;}
    }
    if(index==recordingCapacity){
        if(empty==recordingCapacity||serial_==UINT64_MAX){skip(Skip::RecordingCapacity);return {};}
        VkQueryPool pool{};
        const VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,nullptr,0,VK_QUERY_TYPE_TIMESTAMP,
            static_cast<std::uint32_t>(2*passCapacity),0};
        VkResult result=VK_ERROR_UNKNOWN;
        try {result=config_.functions.create(config_.device,&info,nullptr,&pool);}catch(...){}
        if(result!=VK_SUCCESS||!pool){skip(Skip::CreateFailed);return {};}
        index=empty;auto& r=recordings_[index];r={};r.pool=pool;r.facts=f;r.serial=++serial_;
        ++stats_.creates;++stats_.recordings;
    }
    auto& r=recordings_[index];
    if(r.inFlight||r.executions||r.associationLost){skip(Skip::Recording);return {};}
    if(r.count&&!r.pairs[r.count-1].ended){skip(Skip::OpenPass);return {};}
    if(r.count==passCapacity||passSerial_==UINT64_MAX){skip(Skip::PassCapacity);return {};}
    const auto pairIndex=r.count++;
    auto& pair=r.pairs[pairIndex];pair.identity=identity;pair.serial=++passSerial_;
    const WriteTicket ticket{r.serial,pair.serial,static_cast<std::uint32_t>(index),static_cast<std::uint32_t>(pairIndex)};
    try {
        const auto command=reinterpret_cast<VkCommandBuffer>(static_cast<std::uintptr_t>(f.command));
        config_.functions.reset(command,r.pool,static_cast<std::uint32_t>(2*pairIndex),2);
        config_.functions.write(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,r.pool,static_cast<std::uint32_t>(2*pairIndex));
        ++stats_.passes;return ticket;
    }catch(...){r.associationLost=true;skip(Skip::Recording);return {};}
}
bool Observer::finishSpan(WriteTicket ticket,const Identity& terminal) noexcept {
    ErrorGuard error;
    if(!ticket||ticket.slot>=recordingCapacity||ticket.pass>=passCapacity)return false;
    auto& r=recordings_[ticket.slot];auto& p=r.pairs[ticket.pass];
    if(!r.pool||r.serial!=ticket.recordingSerial||ticket.pass>=r.count||p.serial!=ticket.passSerial||p.ended||r.associationLost||
       p.identity.pass!=Pass::WholeFsr||!terminal.extentsKnown||!terminal.output.width||!terminal.output.height||!terminal.callback||
       !terminal.frame.valid||terminal.frame.engineFrame!=p.identity.frame.engineFrame||terminal.frame.ticket!=p.identity.frame.ticket||
       terminal.window!=p.identity.window||terminal.render.width!=p.identity.render.width||terminal.render.height!=p.identity.render.height)return false;
    p.identity.output=terminal.output;p.identity.extentsKnown=true;p.identity.callback=terminal.callback;
    p.identity.nativeCoreSuppressed=terminal.nativeCoreSuppressed;
    afterDispatch(ticket);return p.ended;
}
void Observer::afterDispatch(WriteTicket ticket,bool emitSample) noexcept {
    ErrorGuard error;
    if(!ticket||ticket.slot>=recordingCapacity||ticket.pass>=passCapacity){skip(Skip::StaleTicket);return;}
    auto& r=recordings_[ticket.slot];
    if(!r.pool||r.serial!=ticket.recordingSerial||ticket.pass>=r.count||r.pairs[ticket.pass].serial!=ticket.passSerial||
        r.pairs[ticket.pass].ended||r.associationLost){skip(Skip::StaleTicket);return;}
    try {
        config_.functions.write(reinterpret_cast<VkCommandBuffer>(static_cast<std::uintptr_t>(r.facts.command)),
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,r.pool,2*ticket.pass+1);
        r.pairs[ticket.pass].ended=true;r.pairs[ticket.pass].suppressed=!emitSample;
    }catch(...){r.associationLost=true;skip(Skip::Recording);}
}
void Observer::collect(Recording& r,Completion completion) noexcept {
    if(!r.executions||r.readExecution==r.executions||r.inFlight||r.associationLost)return;
    // Only a real nonpending lifecycle event reaches here. Availability never
    // authorizes pool destruction, query reuse, or a claim about the CB tail.
    r.readExecution=r.executions;
    const auto observed=qpc();
    for(std::size_t n=0;n<r.count;++n){const auto& pair=r.pairs[n];if(!pair.ended||pair.suppressed)continue;
        Sample sample;sample.recording=r.facts;sample.identity=pair.identity;sample.execution=r.executions;
        sample.queue=r.queue;sample.replay=r.executions>1;sample.completion=completion;
        auto& gpu=sample.gpu;gpu.requested=gpu.queryStateKnown=gpu.diagnosticActive=gpu.queryReadAttempted=true;
        gpu.sequence=pair.serial;gpu.contextGeneration=config_.incarnation;gpu.generation=r.serial;
        gpu.sourceEngineFrame=pair.identity.frame.engineFrame;gpu.sourceEngineFrameKnown=pair.identity.frame.valid;
        gpu.sdkPresentId=pair.identity.frame.ticket.sdkId;gpu.sdkPresentIdKnown=bool(pair.identity.frame.ticket);
        gpu.validBits=config_.validBits;gpu.timestampPeriodNs=config_.periodNs;
        gpu.qpcFrequency=qpcFrequency_;gpu.submitQpcTicks=r.submitQpcTicks;gpu.fenceObservedQpcTicks=observed;
        struct Query {std::uint64_t ticks{},available{};} values[2];
        VkResult result=VK_ERROR_UNKNOWN;
        ++stats_.reads;
        try {result=config_.functions.read(config_.device,r.pool,static_cast<std::uint32_t>(2*n),2,sizeof(values),
            values,sizeof(Query),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);}catch(...){}
        gpu.queryResultKnown=true;gpu.queryResult=result;
        gpu.queryAvailabilityKnown=result==VK_SUCCESS||result==VK_NOT_READY;
        if(gpu.queryAvailabilityKnown){gpu.beginAvailability=values[0].available;gpu.endAvailability=values[1].available;}
        gpu.available=result==VK_SUCCESS&&values[0].available&&values[1].available;
        if(gpu.available){gpu.beginTicks=values[0].ticks;gpu.endTicks=values[1].ticks;
            if(r.submitQpcTicks&&observed>=r.submitQpcTicks)timestampDuration(gpu);}
        else ++stats_.unavailable;
        if(!gpu.durationKnown)++stats_.durationUnknown;
        ++stats_.samples;config_.sink(config_.sinkUser,sample);
    }
}
void Observer::loseAssociations() noexcept {
    skip(Skip::UnknownAssociation);for(auto& r:recordings_)if(r.pool)r.associationLost=true;
}
SubmitTicket Observer::beforeSubmit(VkQueue queue,std::uint32_t family,
    std::span<const CommandRecordingFacts> membership,bool structureKnown) noexcept {
    ErrorGuard error;SubmitTicket ticket;
    if(drained())return ticket;
    if(!structureKnown||membership.size()>submitCapacity||!queue||submitSerial_==UINT64_MAX){
        loseAssociations();return ticket;
    }
    ticket.serial=++submitSerial_;ticket.queue=bits(queue);
    for(std::size_t n=0;n<recordings_.size();++n){auto& r=recordings_[n];if(!r.pool)continue;
        const CommandRecordingFacts* found{};std::size_t occurrences{};
        for(const auto& f:membership)if(f.command==r.facts.command){found=&f;++occurrences;}
        if(!occurrences)continue;
        const bool immutable=found&&sameRecording(r.facts,*found)&&found->allocationKnown&&found->poolKnown&&found->beginFlagsKnown;
        const bool firstExecutable=r.executableObserved||(found&&found->stateKnown&&found->executable&&found->endSucceeded);
        const bool oneTimeReplay=r.executions&&(r.facts.beginUsageFlags&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        if(occurrences!=1||family!=config_.queueFamily||!immutable||!firstExecutable||oneTimeReplay||r.inFlight||r.associationLost||
            !r.count||!r.pairs[r.count-1].ended){r.associationLost=true;skip(Skip::Submission);continue;}
        r.executableObserved=true; // OOM leaves the original executable recording unchanged.
        if(r.executions)collect(r,Completion::SequentialReplay);
        r.inFlight=ticket.serial;ticket.entries[ticket.count++]={r.serial,static_cast<std::uint32_t>(n)};
    }
    ticket.submitQpcTicks=qpc();return ticket;
}
void Observer::submitted(const SubmitTicket& ticket,VkResult result) noexcept {
    ErrorGuard error;
    if(!ticket.serial||ticket.count>recordingCapacity){skip(Skip::StaleTicket);return;}
    for(std::size_t n=0;n<ticket.count;++n){const auto& entry=ticket.entries[n];
        if(entry.slot>=recordingCapacity){skip(Skip::StaleTicket);continue;}
        auto& r=recordings_[entry.slot];
        if(!r.pool||r.serial!=entry.recordingSerial||r.inFlight!=ticket.serial){skip(Skip::StaleTicket);continue;}
        r.inFlight=0;
        if(result==VK_SUCCESS){
            if(r.executions==UINT64_MAX){r.associationLost=true;skip(Skip::Submission);continue;}
            if(r.executions)++stats_.replays;
            ++r.executions;++stats_.successfulExecutions;r.queue=ticket.queue;r.submitQpcTicks=ticket.submitQpcTicks;
        }else{
            ++stats_.failedSubmits;
            if(result!=VK_ERROR_OUT_OF_HOST_MEMORY&&result!=VK_ERROR_OUT_OF_DEVICE_MEMORY)r.associationLost=true;
        }
    }
}
void Observer::release(Recording& r) noexcept {
    if(!r.pool)return;
    if(!r.executions)++stats_.discardedUnsubmitted;
    try {config_.functions.destroy(config_.device,r.pool,nullptr);++stats_.destroys;}catch(...){return;}
    r={};++stats_.retired;
}
void Observer::retire(std::uint64_t command,std::uint64_t recording) noexcept {
    ErrorGuard error;if(!command||!recording)return;
    for(auto& r:recordings_)if(r.pool&&r.facts.command==command&&r.facts.rpRecordingGeneration==recording){
        if(r.inFlight){r.associationLost=true;skip(Skip::Submission);return;}
        collect(r,Completion::RecordingRetired);release(r);return;
    }
}
void Observer::retirePool(std::uint64_t pool,std::uint64_t generation) noexcept {
    ErrorGuard error;if(!pool)return;
    for(auto& r:recordings_)if(r.pool&&r.facts.pool==pool&&(!generation||r.facts.poolGeneration==generation)){
        if(r.inFlight){r.associationLost=true;skip(Skip::Submission);continue;}
        collect(r,Completion::RecordingRetired);release(r);
    }
}
void Observer::deviceIdle(VkResult result) noexcept {
    ErrorGuard error;if(result!=VK_SUCCESS)return;
    for(auto& r:recordings_)if(r.pool)collect(r,Completion::DeviceIdle);
}
void Observer::stop() noexcept {
    ErrorGuard error;active_=false;stats_.retainedAtStop=0;
    for(const auto& r:recordings_)if(r.pool)++stats_.retainedAtStop;
}
bool Observer::releaseForDeviceDestruction(VkResult result) noexcept {
    ErrorGuard error;if(result!=VK_SUCCESS)return false;
    for(const auto& r:recordings_)if(r.inFlight)return false;
    active_=false;
    for(auto& r:recordings_)if(r.pool){collect(r,Completion::DeviceIdle);release(r);}
    return drained();
}
bool Observer::drained() const noexcept {for(const auto& r:recordings_)if(r.pool)return false;return true;}
}
