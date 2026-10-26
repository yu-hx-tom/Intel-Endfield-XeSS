#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <intrin.h>
#include <cmath>

namespace xess_vk::diagnostics {
inline bool threadCpuSamplingRequested() noexcept {
    const auto error=GetLastError();wchar_t value[2]{};
    const bool enabled=GetEnvironmentVariableW(L"XESS_THREAD_CPU_SAMPLING",value,2)==1&&value[0]==L'1';
    SetLastError(error);return enabled;
}
struct ThreadCpuSample {
    DWORD id{};std::uint64_t creation{},user100ns{},kernel100ns{},cycles{},startAddress{};
    std::wstring name,module;
    bool timesKnown{},cyclesKnown{},startKnown{};
};
struct ThreadCpuDelta {
    ThreadCpuSample identity;
    std::uint64_t userNs{},kernelNs{},cycles{};
    bool cpuKnown{},cyclesKnown{};
};
inline ThreadCpuDelta threadCpuDelta(const ThreadCpuSample& before,const ThreadCpuSample& after) {
    ThreadCpuDelta out;out.identity=after;
    const bool same=before.id==after.id&&before.creation&&before.creation==after.creation;
    out.cpuKnown=same&&before.timesKnown&&after.timesKnown&&after.user100ns>=before.user100ns&&after.kernel100ns>=before.kernel100ns;
    if(out.cpuKnown){out.userNs=(after.user100ns-before.user100ns)*100;out.kernelNs=(after.kernel100ns-before.kernel100ns)*100;}
    out.cyclesKnown=same&&before.cyclesKnown&&after.cyclesKnown&&after.cycles>=before.cycles;
    if(out.cyclesKnown)out.cycles=after.cycles-before.cycles;
    return out;
}
struct CycleCalibration {
    double tscHz{},alternateHz{},busyRatio{};
    bool invariant{},valid{};
    const char* reason{"not_calibrated"};
};
inline bool cycleCalibrationValid(bool invariant,double hz,double alternate,double busyRatio) noexcept {
    return invariant&&std::isfinite(hz)&&hz>1e6&&std::isfinite(alternate)&&
        std::abs(alternate/hz-1)<0.01&&std::isfinite(busyRatio)&&busyRatio>=0.8&&busyRatio<=1.05;
}
inline double cycleEstimateNs(std::uint64_t cycles,const CycleCalibration& c) noexcept {
    return c.valid?double(cycles)*1e9/c.tscHz:0;
}
// Local empirical conversion, never a portable API guarantee. Only the opt-in
// background sampler calibrates; failure leaves cycles raw and SR unaffected.
inline CycleCalibration calibrateCycles() noexcept {
    CycleCalibration out;int info[4]{};__cpuid(info,0x80000000);
    if(static_cast<unsigned>(info[0])<0x80000007u){out.reason="no_invariant_tsc_leaf";return out;}
    __cpuid(info,0x80000007);out.invariant=(info[3]&(1<<8))!=0;
    if(!out.invariant){out.reason="non_invariant_tsc";return out;}
    DWORD_PTR allowed{},system{};LARGE_INTEGER frequency{};
    if(!GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system)||!allowed||!QueryPerformanceFrequency(&frequency)){
        out.reason="affinity_or_qpc_unavailable";return out;}
    const auto first=allowed&(~allowed+1);auto last=first;
    for(auto remaining=allowed;remaining;remaining&=remaining-1)last=remaining&(~remaining+1);
    const auto original=SetThreadAffinityMask(GetCurrentThread(),first);
    if(!original){out.reason="affinity_unavailable";return out;}
    struct Restore {DWORD_PTR mask;~Restore(){SetThreadAffinityMask(GetCurrentThread(),mask);}} restore{original};
    const auto ticks=[](){_mm_lfence();const auto t=__rdtsc();_mm_lfence();return t;};
    const auto measure=[&](){LARGE_INTEGER a{},b{};QueryPerformanceCounter(&a);const auto t=ticks();Sleep(60);
        const auto delta=ticks()-t;QueryPerformanceCounter(&b);return b.QuadPart>a.QuadPart?double(delta)*frequency.QuadPart/(b.QuadPart-a.QuadPart):0;};
    out.tscHz=measure();
    if(!SetThreadAffinityMask(GetCurrentThread(),last)){out.reason="alternate_affinity_unavailable";return out;}
    out.alternateHz=measure();
    ULONG64 aCycles{},bCycles{};LARGE_INTEGER a{},b{};
    const bool before=QueryThreadCycleTime(GetCurrentThread(),&aCycles)!=0;QueryPerformanceCounter(&a);
    do{_mm_pause();QueryPerformanceCounter(&b);}while(b.QuadPart-a.QuadPart<frequency.QuadPart/25);
    const bool after=QueryThreadCycleTime(GetCurrentThread(),&bCycles)!=0;
    const auto seconds=double(b.QuadPart-a.QuadPart)/frequency.QuadPart;
    if(before&&after&&bCycles>=aCycles&&out.tscHz>0&&seconds>0)out.busyRatio=(bCycles-aCycles)/out.tscHz/seconds;
    out.valid=cycleCalibrationValid(out.invariant,out.tscHz,out.alternateHz,out.busyRatio);
    out.reason=out.valid?"local_invariant_tsc_and_busy_thread_validated":"frequency_or_busy_validation_failed";
    return out;
}
struct ThreadCpuWindow {
    std::uint64_t generation{},sequence{},beginQpc{},endQpc{},frequency{},processUserNs{},processKernelNs{},sampleCpuNs{},sampleWallNs{};
    std::vector<ThreadCpuDelta> threads;
    struct Group {std::wstring module,name;std::uint64_t userNs{},kernelNs{},cycles{};unsigned threads{},measured{};};
    std::vector<Group> groups;
    unsigned enumerated{},unreadable{},newThreads{},endedThreads{};
    bool processKnown{},baseline{};
    CycleCalibration calibration;
    std::uint64_t processCycles{},sampleCycles{};
    bool processCyclesKnown{},sampleCyclesKnown{};
};
// Only the worker enumerates/opens threads. No ETW, stack walking, suspension,
// remote memory reads, or handles kept between samples. Entry module classifies
// the thread's origin; it does not attribute every executed instruction to it.
class ThreadCpuSampler {
public:
    using Sink=void(*)(void*,const ThreadCpuWindow&) noexcept;
    ThreadCpuSampler()=default;
    ~ThreadCpuSampler(){stop(INFINITE);}
    ThreadCpuSampler(const ThreadCpuSampler&)=delete;
    ThreadCpuSampler& operator=(const ThreadCpuSampler&)=delete;
    bool start(Sink sink,void* user,DWORD periodMs=5000) noexcept {
        if(thread_||!sink||periodMs<10)return false;
        event_=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!event_)return false;
        sink_=sink;user_=user;periodMs_=periodMs;module_=nullptr;++generation_;
        HMODULE owner{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&entry),&owner)){CloseHandle(event_);event_=nullptr;return false;}
        if(owner!=GetModuleHandleW(nullptr)&&!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&entry),&module_)){CloseHandle(event_);event_=nullptr;return false;}
        thread_=CreateThread(nullptr,0,entry,this,0,nullptr);
        if(!thread_){if(module_)FreeLibrary(module_);module_=nullptr;CloseHandle(event_);event_=nullptr;return false;}
        return true;
    }
    bool stop(DWORD timeoutMs=2000) noexcept {
        if(!thread_)return true;SetEvent(event_);
        if(WaitForSingleObject(thread_,timeoutMs)!=WAIT_OBJECT_0)return false;
        CloseHandle(thread_);CloseHandle(event_);thread_=event_=nullptr;return true;
    }
    static std::uint64_t fileTime(FILETIME value) noexcept {return (std::uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;}
    static std::uint64_t cpuTime(HANDLE thread) noexcept {
        FILETIME creation{},exit{},kernel{},user{};return GetThreadTimes(thread,&creation,&exit,&kernel,&user)?(fileTime(kernel)+fileTime(user))*100:0;
    }
private:
    HANDLE thread_{},event_{};HMODULE module_{};Sink sink_{};void* user_{};DWORD periodMs_{5000};
    std::uint64_t generation_{};
    using StartQuery=LONG (WINAPI*)(HANDLE,ULONG,void*,ULONG,ULONG*);
    static ThreadCpuSample inspect(DWORD id) {
        ThreadCpuSample out;out.id=id;
        HANDLE handle=OpenThread(THREAD_QUERY_INFORMATION,FALSE,id);
        if(!handle)handle=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,id);
        if(!handle)return out;
        struct Close {HANDLE h;~Close(){CloseHandle(h);}} close{handle};
        if(GetProcessIdOfThread(handle)!=GetCurrentProcessId())return out; // ID may have been reused since enumeration.
        FILETIME creation{},exit{},kernel{},user{};
        out.timesKnown=GetThreadTimes(handle,&creation,&exit,&kernel,&user)!=0;
        if(out.timesKnown){out.creation=fileTime(creation);out.user100ns=fileTime(user);out.kernel100ns=fileTime(kernel);}
        ULONG64 cycles{};out.cyclesKnown=QueryThreadCycleTime(handle,&cycles)!=0;out.cycles=cycles;
        PWSTR description{};if(SUCCEEDED(GetThreadDescription(handle,&description))&&description){
            struct Free {PWSTR p;~Free(){LocalFree(p);}} free{description};out.name=description;}
        const auto query=reinterpret_cast<StartQuery>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationThread"));
        void* start{};out.startKnown=query&&query(handle,9,&start,sizeof(start),nullptr)>=0;
        if(out.startKnown){out.startAddress=reinterpret_cast<std::uint64_t>(start);HMODULE module{};
            if(start&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(start),&module)){
                wchar_t path[32768]{};const auto length=GetModuleFileNameW(module,path,32768);if(length&&length<32768)out.module.assign(path,length);}}
        return out;
    }
    static std::vector<ThreadCpuSample> collect(unsigned& enumerated,unsigned& unreadable) {
        std::vector<ThreadCpuSample> out;
        const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
        if(snapshot==INVALID_HANDLE_VALUE){++unreadable;return out;}
        struct Close {HANDLE h;~Close(){CloseHandle(h);}} close{snapshot};
        THREADENTRY32 entry{sizeof(entry)};
        if(Thread32First(snapshot,&entry))do{if(entry.th32OwnerProcessID==GetCurrentProcessId()){
            ++enumerated;auto row=inspect(entry.th32ThreadID);if(!row.timesKnown)++unreadable;out.push_back(std::move(row));}
            entry.dwSize=sizeof(entry);
        }while(Thread32Next(snapshot,&entry));
        std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.id<b.id;});return out;
    }
    void run() noexcept {
        SetThreadDescription(GetCurrentThread(),L"XeSS CPU sampler");
        const auto calibration=calibrateCycles();
        std::vector<ThreadCpuSample> previous;
        std::uint64_t beginQpc{},previousUser{},previousKernel{},sequence{},previousCycles{};
        bool previousProcessKnown=false,previousCyclesKnown=false;
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
        for(;;){
            if(WaitForSingleObject(event_,0)!=WAIT_TIMEOUT)break;
            const auto cpuBegin=cpuTime(GetCurrentThread());ULONG64 cycleBegin{};
            const bool cycleBeginKnown=QueryThreadCycleTime(GetCurrentThread(),&cycleBegin)!=0;
            LARGE_INTEGER wallBegin{};QueryPerformanceCounter(&wallBegin);
            try{
                ThreadCpuWindow window;window.generation=generation_;window.sequence=++sequence;window.baseline=sequence==1;window.frequency=frequency.QuadPart;window.beginQpc=beginQpc;
                window.calibration=calibration;
                auto current=collect(window.enumerated,window.unreadable);
                FILETIME creation{},exit{},kernel{},user{};
                const bool processKnown=GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user)!=0;
                const auto user100=fileTime(user),kernel100=fileTime(kernel);
                window.processKnown=processKnown&&previousProcessKnown&&user100>=previousUser&&kernel100>=previousKernel;
                if(window.processKnown){window.processUserNs=(user100-previousUser)*100;window.processKernelNs=(kernel100-previousKernel)*100;}
                ULONG64 processCycles{};const bool cyclesKnown=QueryProcessCycleTime(GetCurrentProcess(),&processCycles)!=0;
                window.processCyclesKnown=cyclesKnown&&previousCyclesKnown&&processCycles>=previousCycles;
                if(window.processCyclesKnown)window.processCycles=processCycles-previousCycles;
                for(const auto& after:current){const auto found=std::lower_bound(previous.begin(),previous.end(),after.id,[](const auto& row,DWORD id){return row.id<id;});
                    if(found!=previous.end()&&found->id==after.id&&found->creation==after.creation)window.threads.push_back(threadCpuDelta(*found,after));
                    else{++window.newThreads;ThreadCpuDelta row;row.identity=after;window.threads.push_back(std::move(row));}}
                for(const auto& before:previous){const auto found=std::lower_bound(current.begin(),current.end(),before.id,[](const auto& row,DWORD id){return row.id<id;});
                    window.endedThreads+=found==current.end()||found->id!=before.id||found->creation!=before.creation;}
                for(const auto& row:window.threads){auto group=std::find_if(window.groups.begin(),window.groups.end(),[&](const auto& value){return value.module==row.identity.module&&value.name==row.identity.name;});
                    if(group==window.groups.end()){window.groups.push_back({row.identity.module,row.identity.name});group=window.groups.end()-1;}
                    ++group->threads;if(row.cpuKnown){++group->measured;group->userNs+=row.userNs;group->kernelNs+=row.kernelNs;}if(row.cyclesKnown)group->cycles+=row.cycles;}
                LARGE_INTEGER end{};QueryPerformanceCounter(&end);window.endQpc=end.QuadPart;
                previous=std::move(current);beginQpc=window.endQpc;previousUser=user100;previousKernel=kernel100;previousProcessKnown=processKnown;
                previousCycles=processCycles;previousCyclesKnown=cyclesKnown;
                // Includes collection/aggregation; final callback cost is added
                // to the next window's sampler thread CPU delta.
                window.sampleCpuNs=cpuTime(GetCurrentThread())-cpuBegin;
                ULONG64 cycleEnd{};window.sampleCyclesKnown=cycleBeginKnown&&QueryThreadCycleTime(GetCurrentThread(),&cycleEnd)&&cycleEnd>=cycleBegin;
                if(window.sampleCyclesKnown)window.sampleCycles=cycleEnd-cycleBegin;
                window.sampleWallNs=frequency.QuadPart?static_cast<std::uint64_t>((end.QuadPart-wallBegin.QuadPart)*1e9/frequency.QuadPart):0;
                sink_(user_,window);
            }catch(...){ThreadCpuWindow failed;failed.generation=generation_;failed.sequence=sequence;failed.unreadable=1;sink_(user_,failed);previous.clear();previousProcessKnown=previousCyclesKnown=false;beginQpc=0;}
            if(WaitForSingleObject(event_,periodMs_)!=WAIT_TIMEOUT)break;
        }
    }
    static DWORD WINAPI entry(void* argument) noexcept {
        auto* self=static_cast<ThreadCpuSampler*>(argument);self->run();
        if(const auto module=self->module_)FreeLibraryAndExitThread(module,0);return 0;
    }
};
}
