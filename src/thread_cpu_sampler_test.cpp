#include "thread_cpu_sampler.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

using namespace xess_vk::diagnostics;
namespace {
unsigned checks{};
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Result {
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<unsigned> windows{},measured{},named{},modules{},cycles{},sampler{};
    std::atomic<std::uint64_t> collectionCpu{},collectionWall{},user{},kernel{};
    std::atomic<std::uint64_t> samplerCpu{},intervalNs{};
    std::atomic<bool> invalidGroups{};
    std::atomic<unsigned> calibrated{},processCycles{};
    CycleCalibration calibration;
    ~Result(){CloseHandle(ready);}
};
void receive(void* user,const ThreadCpuWindow& value) noexcept {
    auto& result=*static_cast<Result*>(user);
    result.collectionCpu+=value.sampleCpuNs;result.collectionWall+=value.sampleWallNs;
    result.calibration=value.calibration;
    if(value.calibration.valid)++result.calibrated;
    if(value.processCyclesKnown)++result.processCycles;
    for(const auto& row:value.threads){
        if(row.cpuKnown){++result.measured;result.user+=row.userNs;result.kernel+=row.kernelNs;}
        if(row.cyclesKnown)++result.cycles;
        if(row.identity.name==L"XeSS sample test worker")++result.named;
        if(row.identity.name==L"XeSS CPU sampler"){++result.sampler;if(row.cpuKnown)result.samplerCpu+=row.userNs+row.kernelNs;}
        if(!row.identity.module.empty())++result.modules;
    }
    std::uint64_t threadTotal{},groupTotal{};for(const auto& row:value.threads)if(row.cpuKnown)threadTotal+=row.userNs+row.kernelNs;
    for(const auto& group:value.groups)groupTotal+=group.userNs+group.kernelNs;
    if(threadTotal!=groupTotal)result.invalidGroups=true;
    if(value.beginQpc&&value.frequency)result.intervalNs+=static_cast<std::uint64_t>((value.endQpc-value.beginQpc)*1e9/value.frequency);
    if(++result.windows>=4)SetEvent(result.ready);
}
}
int main(int argc,char** argv){try{
    const bool production=argc==2&&std::strcmp(argv[1],"--production-period")==0;
    require(cycleCalibrationValid(true,3e9,3.001e9,0.99),"stable invariant clock and live cycle relation qualify local estimate");
    require(!cycleCalibrationValid(false,3e9,3e9,1)&&!cycleCalibrationValid(true,3e9,4e9,1)&&
        !cycleCalibrationValid(true,3e9,3e9,0.5)&&!cycleCalibrationValid(true,0,0,1),"unsupported or unvalidated conversion stays unavailable");
    CycleCalibration calibration;require(cycleEstimateNs(3000,calibration)==0,"uncalibrated cycles never silently convert");
    calibration.valid=true;calibration.tscHz=3e9;require(cycleEstimateNs(3000,calibration)==1000,"calibrated cycles convert without mixing user/kernel ledgers");
    ThreadCpuSample before;before.id=7;before.creation=123;before.timesKnown=before.cyclesKnown=true;before.user100ns=100;before.kernel100ns=70;before.cycles=1000;
    auto after=before;after.user100ns+=13;after.kernel100ns+=5;after.cycles+=234;
    auto delta=threadCpuDelta(before,after);require(delta.cpuKnown&&delta.userNs==1300&&delta.kernelNs==500&&delta.cyclesKnown&&delta.cycles==234,"separate kernel, user and cycle deltas");
    ++after.creation;require(!threadCpuDelta(before,after).cpuKnown,"thread ID reuse never subtracts another generation");
    after=before;after.user100ns=0;require(!threadCpuDelta(before,after).cpuKnown,"counter regression is unavailable, not wrapped CPU time");
    wchar_t saved[32768]{};const auto savedCount=GetEnvironmentVariableW(L"XESS_THREAD_CPU_SAMPLING",saved,32768);
    for(const wchar_t* value:{static_cast<const wchar_t*>(nullptr),L"",L"0",L"true",L"11",L"1"}){SetEnvironmentVariableW(L"XESS_THREAD_CPU_SAMPLING",value);SetLastError(1234);
        require(threadCpuSamplingRequested()==(value&&std::wcscmp(value,L"1")==0)&&GetLastError()==1234,"only exact opt-in enables sampling, preserving LastError");}
    SetEnvironmentVariableW(L"XESS_THREAD_CPU_SAMPLING",savedCount&&savedCount<32768?saved:nullptr);
    DWORD handlesBefore{},handlesAfter{};GetProcessHandleCount(GetCurrentProcess(),&handlesBefore);
    Result result;
    std::jthread worker([](std::stop_token stop){SetThreadDescription(GetCurrentThread(),L"XeSS sample test worker");
        while(!stop.stop_requested()){const auto until=GetTickCount64()+30;while(GetTickCount64()<until)YieldProcessor();Sleep(10);}});
    ThreadCpuSampler sampler;require(!sampler.start(nullptr,nullptr,100),"no worker without a sink");
    require(sampler.start(receive,&result,production?5000:100),"live self-process sampler starts");
    require(!sampler.start(receive,&result,100),"starting twice does not leak a worker");
    require(WaitForSingleObject(result.ready,production?25000:10000)==WAIT_OBJECT_0,"live worker emits baseline and interval deltas");
    require(sampler.stop()&&sampler.stop(),"shutdown is prompt and idempotent");
    worker.request_stop();worker.join();
    require(result.measured&&result.named&&result.modules&&result.cycles&&result.sampler,"live sampling obtains names, modules, cycles and self overhead");
    require(!result.invalidGroups,"module/name groups sum to the same CPU as per-thread records");
    require(result.processCycles,"live process cycle deltas are available for thread coverage cross-check");
    GetProcessHandleCount(GetCurrentProcess(),&handlesAfter);
    require(handlesAfter<=handlesBefore+1,"per-thread and snapshot handles are closed (one result event remains)");
    std::printf("{\"cycle_calibration_valid\":%s,\"tsc_hz\":%.0f,\"alternate_tsc_hz\":%.0f,\"busy_ratio\":%.6f,\"calibrated_windows\":%u,\"process_cycle_windows\":%u}\n",
        result.calibration.valid?"true":"false",result.calibration.tscHz,result.calibration.alternateHz,result.calibration.busyRatio,result.calibrated.load(),result.processCycles.load());
    std::printf("{\"passed\":true,\"checks\":%u,\"windows\":%u,\"collection_cpu_ns\":%llu,\"collection_wall_ns\":%llu,\"observed_user_ns\":%llu,\"observed_kernel_ns\":%llu,\"sampler_thread_cpu_ns\":%llu,\"interval_ns\":%llu,\"etw\":false,\"game_touched\":false}\n",
        checks,result.windows.load(),static_cast<unsigned long long>(result.collectionCpu.load()),static_cast<unsigned long long>(result.collectionWall.load()),
        static_cast<unsigned long long>(result.user.load()),static_cast<unsigned long long>(result.kernel.load()),static_cast<unsigned long long>(result.samplerCpu.load()),static_cast<unsigned long long>(result.intervalNs.load()));return 0;
}catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}}
