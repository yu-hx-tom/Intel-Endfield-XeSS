// Production timestamp component with inert Vulkan entry points; no loader/GPU/game.
#include "sr_gpu_timestamps.hpp"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace xess_vk;
namespace t=sr_gpu_timestamps;
unsigned checks{},creates{},destroys{},resets{},writes{},reads{},originalDispatches{};
bool createFails{},readFails{},readUnavailable{},readThrows{},destroyThrows{};
const char* swallowedFailure{};
template<class T> T handle(std::uintptr_t value){return reinterpret_cast<T>(value);}
void require(bool condition,const char* message){++checks;if(!condition){if(!swallowedFailure)swallowedFailure=message;throw std::runtime_error(message);}}
struct Query {std::uint64_t tick{},available{};bool initialized{};};
struct Pool {
    VkQueryPool handle{};bool live{},submitted{},complete{};
    std::array<Query,2*t::passCapacity> queries{};
};
struct Op {VkCommandBuffer command{};VkQueryPool pool{};std::uint32_t index{};bool reset{};};
std::vector<Pool> pools;
std::vector<Op> operations;
std::vector<t::Sample> samples;
Pool& pool(VkQueryPool key){for(auto& p:pools)if(p.handle==key&&p.live)return p;throw std::runtime_error("unknown pool");}
VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice,const VkQueryPoolCreateInfo* info,const VkAllocationCallbacks* allocator,VkQueryPool* out){
    SetLastError(0xdeadbeef);++creates;
    require(info&&info->sType==VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO&&!info->pNext&&!info->flags&&
        info->queryType==VK_QUERY_TYPE_TIMESTAMP&&info->queryCount==2*t::passCapacity&&!allocator,
        "bounded timestamp-only pool, no extensions or application allocator");
    if(createFails)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out=handle<VkQueryPool>(0x1000+creates);Pool p;p.handle=*out;p.live=true;pools.push_back(p);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice,VkQueryPool key,const VkAllocationCallbacks* allocator){
    SetLastError(0xdeadbeef);auto& p=pool(key);
    require(!allocator&&(!p.submitted||p.complete),"query availability never authorizes pending pool destruction");
    if(destroyThrows)throw std::runtime_error("injected destroy error");
    ++destroys;p.live=false;
}
VKAPI_ATTR void VKAPI_CALL reset(VkCommandBuffer command,VkQueryPool key,std::uint32_t first,std::uint32_t count){
    SetLastError(0xdeadbeef);++resets;
    require(first%2==0&&first+2<=2*t::passCapacity&&count==2,"reset exactly one owned pair");
    require(pool(key).live,"record reset only live private pool");
    operations.push_back({command,key,first,true});
}
VKAPI_ATTR void VKAPI_CALL write(VkCommandBuffer command,VkPipelineStageFlagBits stage,VkQueryPool key,std::uint32_t index){
    SetLastError(0xdeadbeef);++writes;
    require(index<2*t::passCapacity&&stage==(index%2?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
        "TOP/BOTTOM bracket in the original matched command buffer");
    operations.push_back({command,key,index,false});
}
VKAPI_ATTR VkResult VKAPI_CALL read(VkDevice,VkQueryPool key,std::uint32_t first,std::uint32_t count,
    std::size_t bytes,void* output,VkDeviceSize stride,VkQueryResultFlags flags){
    SetLastError(0xdeadbeef);++reads;auto& p=pool(key);
    require(p.submitted&&p.complete,"reads require a successful submission and actual nonpending boundary");
    require(count==2&&bytes==32&&stride==16&&flags==(VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT),
        "nonwaiting 64-bit query result with explicit availability, never WAIT/PARTIAL");
    require(p.queries[first].initialized&&p.queries[first+1].initialized,"never read fresh uninitialized queries");
    if(readThrows)throw std::runtime_error("injected read error");
    if(readFails)return VK_ERROR_OUT_OF_HOST_MEMORY;
    auto* values=static_cast<std::uint64_t*>(output);
    values[0]=p.queries[first].tick;values[1]=p.queries[first].available;
    values[2]=p.queries[first+1].tick;values[3]=readUnavailable?0:p.queries[first+1].available;
    return readUnavailable?VK_NOT_READY:VK_SUCCESS;
}
void sample(void*,const t::Sample& value) noexcept {SetLastError(0xdeadbeef);samples.push_back(value);}
t::Config config(bool enabled=true){
    t::Config c;c.device=handle<VkDevice>(1);c.incarnation=2;c.window=3;c.queueFamily=4;
    c.validBits=36;c.queueFlags=VK_QUEUE_COMPUTE_BIT;c.periodNs=2;
    c.functions={create,destroy,reset,write,read};c.sink=sample;c.enabled=enabled;return c;
}
CommandRecordingFacts recording(std::uint64_t command=5,std::uint64_t generation=6,bool oneTime=false){
    CommandRecordingFacts f;f.device=1;f.contextIncarnation=2;f.command=command;f.rpRecordingGeneration=generation;
    f.pool=7;f.poolGeneration=8;f.queueFamily=4;f.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    f.beginUsageFlags=oneTime?VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT:0;
    f.reasons=oneTime?0:bit(CommandRecordingReason::NotOneTime);
    f.allocationKnown=f.poolKnown=f.beginFlagsKnown=f.stateKnown=f.recording=f.scopeKnown=true;return f;
}
CommandRecordingFacts executable(CommandRecordingFacts f){f.recording=false;f.executable=f.endSucceeded=true;return f;}
t::Identity identity(t::Pass pass=t::Pass::Accumulate,std::uint64_t ordinal=9){
    t::Identity i;i.window=3;i.ordinal=ordinal;i.callback=10;i.pipeline=11;i.pipelineGeneration=12;i.pass=pass;
    i.frame.engineFrame=13;i.frame.ticket={14,15,false};i.frame.valid=true;
    i.render={1128,634};i.output={1920,1080};i.groups={240,135,1};i.extentsKnown=true;return i;
}
void clear(){
    creates=destroys=resets=writes=reads=originalDispatches=0;
    createFails=readFails=readUnavailable=readThrows=destroyThrows=false;
    pools.clear();operations.clear();samples.clear();
}
t::WriteTicket dispatch(t::Observer& observer,const CommandRecordingFacts& f,const t::Identity& i){
    SetLastError(0x1234);const auto ticket=observer.beforeDispatch(f,i);
    require(GetLastError()==0x1234,"beforeDispatch preserves LastError");
    ++originalDispatches; // The wrapper always forwards the original exactly once.
    if(ticket){observer.afterDispatch(ticket);require(GetLastError()==0x1234,"afterDispatch preserves LastError");}
    return ticket;
}
t::SubmitTicket submit(t::Observer& observer,CommandRecordingFacts f,VkResult result=VK_SUCCESS,std::uint64_t qpcAgeTicks=0){
    f=executable(f);SetLastError(0x1234);
    auto ticket=observer.beforeSubmit(handle<VkQueue>(16),4,{&f,1},true);
    require(GetLastError()==0x1234,"pre-submit result read preserves LastError");
    if(qpcAgeTicks){
        require(ticket.submitQpcTicks>qpcAgeTicks,"synthetic submitted timestamp stays positive");
        ticket.submitQpcTicks-=qpcAgeTicks;
    }
    observer.submitted(ticket,result);require(GetLastError()==0x1234,"literal original submit result observation preserves LastError");
    if(result==VK_SUCCESS)for(const auto& op:operations)if(op.command==handle<VkCommandBuffer>(f.command)){
        auto& p=pool(op.pool);p.submitted=true;p.complete=false;
    }
    return ticket;
}
void execute(std::uint64_t command,std::uint64_t execution,bool complete=true){
    for(const auto& op:operations)if(op.command==handle<VkCommandBuffer>(command)){
        auto& p=pool(op.pool);
        if(op.reset){p.queries[op.index]={0,0,true};p.queries[op.index+1]={0,0,true};}
        else {auto& q=p.queries[op.index];require(q.initialized&&!q.available,"each replay resets query before its new write");
            q.tick=execution*1000+op.index*17;q.available=1;}
        p.complete=complete;
    }
}
void retire(t::Observer& observer,const CommandRecordingFacts& f){
    SetLastError(0x1234);observer.retire(f.command,f.rpRecordingGeneration);
    require(GetLastError()==0x1234,"lifecycle sample and destruction preserve LastError");
}
void run(){
    clear();{
        t::Observer observer(config());auto f=recording();auto start=identity(t::Pass::WholeFsr);
        start.callback=0;start.output={};start.extentsKnown=false;
        const auto token=observer.beforeDispatch(f,start);require(bool(token),"Prepare opens a whole-FSR span before output callback is known");
        auto end=identity(t::Pass::WholeFsr);++end.frame.engineFrame;
        require(!observer.finishSpan(token,end),"another engine frame cannot close the GPU span");
        end=identity(t::Pass::WholeFsr);require(observer.finishSpan(token,end),"exact frame and output callback close whole-FSR interval");
        require(!observer.finishSpan(token,end),"whole interval cannot be closed twice");
        dispatch(observer,f,identity(t::Pass::XeSS,10));submit(observer,f,VK_SUCCESS);execute(f.command,1);retire(observer,f);
        require(samples.size()==2&&samples[0].identity.pass==t::Pass::WholeFsr&&samples[1].identity.pass==t::Pass::XeSS,"whole-FSR and XeSS GPU brackets survive real submission and retirement independently");
        require(samples[0].identity.extentsKnown&&samples[0].gpu.available,"whole-FSR sample carries actual terminal extent and query availability");
    }

    clear();{
        t::Observer observer(config(false));auto f=recording();
        require(!dispatch(observer,f,identity()),"ordinary default OFF records no commands");
        observer.deviceIdle(VK_SUCCESS);observer.stop();require(observer.drained(),"disabled observer has no owned query children");
        require(!creates&&!resets&&!writes&&!reads&&!destroys&&originalDispatches==1,"default OFF forwards original with zero Vulkan diagnostics");
    }
    clear();{
        t::Observer observer(config());const auto f=recording();
        const auto ticket=dispatch(observer,f,identity());require(bool(ticket),"valid matched candidate is bracketed");
        observer.deviceIdle(VK_SUCCESS);require(!reads,"unsubmitted queries are never read even at idle");
        submit(observer,f);require(!reads,"successful submit alone does not poll uninitialized queries");
        execute(f.command,1,false);require(pool(pools[0].handle).queries[1].available,"pass end can be available while CB tail is pending");
        observer.stop();require(!reads&&!destroys&&!observer.drained()&&observer.stats().retainedAtStop==1,
            "window stop neither reads uninitialized/stale data nor destroys pool from query availability");
        require(!observer.releaseForDeviceDestruction(VK_NOT_READY)&&!destroys,"missing real idle completion cannot tear down pending pool");
        pool(pools[0].handle).complete=true;observer.retire(f.command,f.rpRecordingGeneration+1);
        require(!reads&&!destroys,"wrong recording generation cannot retire a retained recording");
        retire(observer,f);require(reads==1&&destroys==1&&observer.drained(),"actual legal reset/free retires complete recording");
        const auto& s=samples.back();require(s.gpu.available&&s.gpu.durationKnown&&s.gpu.durationNs==34&&s.execution==1&&!s.replay,
            "per-pass ticks convert through existing timestamp helper");
        require(s.identity.render.width==1128&&s.identity.render.height==634&&s.identity.output.width==1920&&
            s.identity.groups==std::array<std::uint32_t,3>{240,135,1}&&s.identity.frame.ticket.sdkId==14&&
            s.identity.callback==10&&s.identity.pipelineGeneration==12&&s.recording.rpRecordingGeneration==6,
            "actual render/output/groups and matched frame/ticket/pipeline/callback survive until read");
        require(!s.gpu.calibration.available&&s.completion==t::Completion::RecordingRetired,"no invented calibration or whole-CB inference");
        observer.afterDispatch(ticket);require(writes==2,"retired write ticket cannot touch destroyed/reused resources");
    }
    clear();{
        t::Observer observer(config());const auto f=recording();
        dispatch(observer,f,identity());dispatch(observer,f,identity(t::Pass::Rcas,10));
        require(creates==1&&resets==2&&writes==4,"same CB multipass uses disjoint pairs in one bounded pool");
        submit(observer,f);execute(f.command,1);
        auto replayFacts=executable(f);replayFacts.stateKnown=replayFacts.executable=false;
        replayFacts.reasons|=bit(CommandRecordingReason::CompletionUnknown);replayFacts.successfulSubmissionCount=1;
        auto second=observer.beforeSubmit(handle<VkQueue>(16),4,{&replayFacts,1},true);
        require(samples.size()==2&&samples[0].identity.pass==t::Pass::Accumulate&&samples[1].identity.pass==t::Pass::Rcas&&
            samples[0].completion==t::Completion::SequentialReplay,"pre-replay reads the previous real execution, including both passes");
        observer.submitted(second,VK_SUCCESS);pool(pools[0].handle).complete=false;
        require(reads==2&&pool(pools[0].handle).queries[1].available,"new replay submission leaves stale availability unread");
        observer.deviceIdle(VK_NOT_READY);observer.stop();require(reads==2&&!destroys,"pending replay and closed window retain owned pool");
        execute(f.command,2);observer.deviceIdle(VK_SUCCESS);
        require(reads==4&&samples[2].execution==2&&samples[2].replay&&samples[2].gpu.beginTicks==2000&&
            samples[3].gpu.beginTicks==2034,"real idle reads only the new executed replay, never old availability");
        require(samples[2].identity.frame.engineFrame==13&&samples[2].identity.frame.ticket.contextGeneration==15,
            "replay retains original recording frame; no fabricated new frame association");
        observer.deviceIdle(VK_SUCCESS);require(reads==4&&!destroys,"idle reads each execution once and preserves still-executable CB references");
        retire(observer,f);require(reads==4&&destroys==1&&observer.stats().replays==1,"retire after idle does not duplicate last execution");
    }
    clear();{
        t::Observer observer(config());auto f=recording();dispatch(observer,f,identity());
        submit(observer,f,VK_ERROR_OUT_OF_DEVICE_MEMORY);observer.deviceIdle(VK_SUCCESS);
        require(!reads,"failed first original submit never permits query read");
        retire(observer,f);require(destroys==1&&observer.stats().discardedUnsubmitted==1,"never-submitted recording discarded only at actual retirement");
    }
    clear();{
        t::Observer observer(config());auto f=recording();dispatch(observer,f,identity());
        submit(observer,f,VK_ERROR_OUT_OF_DEVICE_MEMORY);
        auto retry=executable(f);retry.stateKnown=retry.executable=false;retry.reasons|=bit(CommandRecordingReason::DriverStateUnknown);
        const auto ticket=observer.beforeSubmit(handle<VkQueue>(16),4,{&retry,1},true);
        require(ticket.count==1&&!reads,"native OOM no-effect guarantee permits retry despite conservative metadata state");
        observer.submitted(ticket,VK_SUCCESS);pool(pools[0].handle).submitted=true;execute(f.command,1);retire(observer,f);
        require(reads==1&&samples[0].execution==1,"successful first retry measures its own initialized execution");
    }
    clear();{
        t::Observer observer(config());auto f=recording();dispatch(observer,f,identity());submit(observer,f);execute(f.command,1);
        submit(observer,f,VK_ERROR_OUT_OF_HOST_MEMORY);require(reads==1&&samples[0].execution==1,"failed replay preserves previous completed sample");
        submit(observer,f);require(reads==1,"OOM retry cannot reread/relabel the previous execution");
        execute(f.command,2);retire(observer,f);require(reads==2&&samples[1].execution==2,"successful retry uses a new execution identity");
    }
    for(unsigned mode=0;mode<3;++mode){
        clear();t::Observer observer(config());const auto f=recording();dispatch(observer,f,identity());submit(observer,f);execute(f.command,1);
        readFails=mode==0;readUnavailable=mode==1;readThrows=mode==2;retire(observer,f);
        require(samples.size()==1&&!samples[0].gpu.available&&!samples[0].gpu.durationKnown&&
            observer.stats().unavailable==1&&destroys==1,"read error/unavailable/exception is explicit and does not block safe retirement");
    }
    clear();{
        t::Observer observer(config());const auto f=recording(5,6,true);dispatch(observer,f,identity());submit(observer,f);execute(f.command,1);
        submit(observer,f);require(!reads,"invalid one-time replay is refused without reading or relabelling");
        execute(f.command,2);retire(observer,f);require(samples.empty()&&destroys==1,"unsupported replay association is never advertised as a measured frame");
    }
    clear();{
        t::Observer observer(config());auto f=recording();dispatch(observer,f,identity());auto e=executable(f);
        std::array membership{e,e};auto ticket=observer.beforeSubmit(handle<VkQueue>(16),4,membership,true);
        observer.submitted(ticket,VK_SUCCESS);retire(observer,f);
        require(!reads&&observer.stats().skipped[std::size_t(t::Skip::Submission)]==1,"duplicate membership is refused, no guessed query execution");
    }
    clear();{
        t::Observer observer(config());auto f=recording();dispatch(observer,f,identity());submit(observer,f);execute(f.command,1);
        auto e=executable(f);observer.beforeSubmit(handle<VkQueue>(16),4,{&e,1},false);retire(observer,f);
        require(!reads&&destroys==1&&observer.stats().skipped[std::size_t(t::Skip::UnknownAssociation)]==1,
            "unknown submit structure poisons sample association, never silently skips an execution");
    }
    clear();{
        t::Observer observer(config());auto f=recording();dispatch(observer,f,identity());submit(observer,f);execute(f.command,1);
        auto unrelated=executable(recording(50,60));observer.beforeSubmit(handle<VkQueue>(17),99,{&unrelated,1},true);
        retire(observer,f);require(reads==1&&samples[0].execution==1,"unrelated known submissions on another family do not discard relevant observations");
    }
    clear();{
        t::Observer observer(config());auto f=recording();
        for(std::size_t n=0;n<t::passCapacity;++n)require(bool(dispatch(observer,f,identity(t::Pass::Accumulate,9+n))),"bounded pass admitted");
        const auto oldWrites=writes;require(!dispatch(observer,f,identity(t::Pass::Rcas,100))&&writes==oldWrites,
            "full per-recording capacity skips without reset/write");
        for(std::size_t n=1;n<t::recordingCapacity;++n)require(bool(dispatch(observer,recording(50+n,60+n),identity())),"bounded recording admitted");
        require(!dispatch(observer,recording(500,600),identity())&&creates==t::recordingCapacity,
            "full recording capacity skips without another Vulkan allocation");
        observer.retirePool(7,999);require(!destroys,"wrong pool generation cannot retire live recordings");
        observer.retirePool(7,8);require(destroys==t::recordingCapacity&&!reads&&observer.drained(),"actual pool reset safely discards every unsubmitted recording");
        require(observer.stats().skipped[std::size_t(t::Skip::PassCapacity)]==1&&
            observer.stats().skipped[std::size_t(t::Skip::RecordingCapacity)]==1,"both capacity skips have explicit counters");
    }
    clear();{
        t::Observer observer(config());auto f=recording();auto ticket=observer.beforeDispatch(f,identity());
        observer.stop();observer.afterDispatch(ticket);require(writes==2,"window close between brackets still balances recorded pass");
        retire(observer,f);
    }
    clear();{
        t::Observer observer(config());auto f=recording();const auto old=dispatch(observer,f,identity());retire(observer,f);
        auto newer=recording(5,7);const auto current=observer.beforeDispatch(newer,identity());const auto before=writes;
        observer.afterDispatch(old);require(writes==before,"old ticket cannot complete a new recording that reuses the same pool slot");
        observer.afterDispatch(current);require(writes==before+1,"new exact recording ticket still balances its pass");retire(observer,newer);
    }
    clear();{
        t::Observer observer(config());auto f=recording();dispatch(observer,f,identity());auto e=executable(f);
        auto ticket=observer.beforeSubmit(handle<VkQueue>(16),4,{&e,1},true);
        observer.retire(f.command,f.rpRecordingGeneration);require(!destroys&&!observer.releaseForDeviceDestruction(VK_SUCCESS),
            "in-flight original submit call cannot race diagnostic pool release");
        observer.submitted(ticket,VK_SUCCESS);for(auto& p:pools){p.submitted=p.complete=true;}
        require(observer.releaseForDeviceDestruction(VK_SUCCESS)&&destroys==1,"real idle plus imminent device destruction releases remaining references");
    }
    clear();{
        t::Observer observer(config());auto f=recording();createFails=true;
        require(!dispatch(observer,f,identity())&&!resets&&!writes&&observer.drained(),"allocation failure only skips timestamps");
        require(observer.stats().skipped[std::size_t(t::Skip::CreateFailed)]==1&&originalDispatches==1,"allocation failure leaves original dispatch intact");
    }
    for(unsigned field=0;field<9;++field){
        clear();t::Observer observer(config());auto f=recording();
        switch(field){case 0:f.beginUsageFlags=VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;break;
        case 1:f.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;break;case 2:f.scopeKnown=false;break;
        case 3:f.inRenderPass=true;break;case 4:f.inDynamicRendering=true;break;
        case 5:f.poolFlags=VK_COMMAND_POOL_CREATE_PROTECTED_BIT;break;case 6:f.queueFamily=99;break;
        case 7:f.reasons|=bit(CommandRecordingReason::BeginPNext);break;case 8:f.poolGeneration=0;break;}
        require(!dispatch(observer,f,identity())&&!creates,"unsafe or unknown recording is refused before creating or writing queries");
    }
    clear();{
        t::Observer observer(config());auto i=identity();i.extentsKnown=false;
        require(!dispatch(observer,recording(),i)&&!creates&&observer.stats().skipped[std::size_t(t::Skip::Identity)]==1,
            "unknown actual render/output extents are refused, never guessed from groups");
    }
    for(unsigned field=0;field<5;++field){
        clear();auto c=config();switch(field){case 0:c.validBits=0;break;case 1:c.validBits=65;break;
        case 2:c.periodNs=std::numeric_limits<double>::quiet_NaN();break;case 3:c.queueFlags=VK_QUEUE_TRANSFER_BIT;break;
        case 4:c.functions.read=nullptr;break;}
        t::Observer observer(c);require(!dispatch(observer,recording(),identity())&&!creates,"missing capability/function disables all query insertion");
    }
    clear();{
        auto c=config();c.validBits=4;t::Observer observer(c);auto f=recording();dispatch(observer,f,identity());
        LARGE_INTEGER frequency{};
        require(QueryPerformanceFrequency(&frequency)&&frequency.QuadPart>0,"fixture has a real QPC frequency");
        // A 32ns wrap can fit between two reads of the same QPC tick; inject age instead of relying on test execution time.
        const auto ageTicks=static_cast<std::uint64_t>(frequency.QuadPart)/1000+1;
        submit(observer,f,VK_SUCCESS,ageTicks);
        execute(f.command,1);retire(observer,f);
        require(samples[0].gpu.submitQpcTicks&&samples[0].gpu.fenceObservedQpcTicks>=samples[0].gpu.submitQpcTicks+ageTicks,
            "fixture provides at least one millisecond of known CPU-observed age");
        require(samples[0].gpu.available&&!samples[0].gpu.durationKnown,
            "existing helper refuses a possible multiple-wrap interval rather than guessing duration");
    }
}
}
int main(){try{run();if(swallowedFailure)throw std::runtime_error(swallowedFailure);
    std::printf("sr GPU timestamp CPU tests passed: %u checks\n",checks);return 0;}
    catch(const std::exception& e){std::fprintf(stderr,"sr GPU timestamp CPU test failed: %s (%u checks)\n",e.what(),checks);return 1;}}
