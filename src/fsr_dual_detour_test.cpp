#include "fsr_detour_entry.hpp"
#include "upscale_detour_entry.hpp"
#include <windows.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>

extern "C" std::uint64_t dual_target_a(std::uint64_t, std::uint64_t);
extern "C" std::uint64_t dual_target_b(std::uint64_t, std::uint64_t);
namespace {
constexpr DWORD incomingError=0x76543210, outgoingA=0x11223344, outgoingB=0x55667788;
std::array<std::atomic<std::uint64_t>,2> observed{},originals{};
std::atomic<std::uint64_t> failures{},exceptions{},nested{},sampled{};
std::atomic<bool> sampling{true},throwing{false};
thread_local bool helperCrossCall=false,originalCrossCall=false,inside=false;
std::uint64_t checks{};
void require(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
std::uint64_t value(unsigned which,std::uint64_t a,std::uint64_t b){return which ? (a*29)^(b+71) : (a+13)^(b*37);}
void observe(unsigned which) noexcept {
    const auto saved=GetLastError();
    ++observed[which];
    if(sampling.load()&&!inside){
        inside=true;
        try{
            ++sampled;
            SetLastError(0xdeadbeef);
            if(throwing.load())throw std::runtime_error("own diagnostic failure");
            if(helperCrossCall){
                SetLastError(incomingError);
                const auto result=which ? dual_target_a(17,23) : dual_target_b(17,23);
                if(result!=value(1-which,17,23))++failures;
                ++nested;
            }
        }catch(...){++exceptions;}
        inside=false;
    }
    SetLastError(saved);
}
}
extern "C" void fsr_observe_before(void*,void*) noexcept{observe(0);}
extern "C" void upscale_observe_before(void*,void*) noexcept{observe(1);}
extern "C" std::uint64_t dual_original_a(std::uint64_t a,std::uint64_t b){
    if(GetLastError()!=incomingError)++failures;
    ++originals[0];
    auto result=value(0,a,b);
    if(originalCrossCall){SetLastError(incomingError);result^=dual_target_b(a+1,b+1);}
    SetLastError(outgoingA);return result;
}
extern "C" std::uint64_t dual_original_b(std::uint64_t a,std::uint64_t b){
    if(GetLastError()!=incomingError)++failures;
    ++originals[1];SetLastError(outgoingB);return value(1,a,b);
}
int main(){
    try{
        require(MH_Initialize()==MH_OK,"initialize private MinHook once");
        require(MH_CreateHook(reinterpret_cast<void*>(&dual_target_a),reinterpret_cast<void*>(&fsr_observer_entry),&fsr_original_trampoline)==MH_OK,"create first hook");
        require(MH_CreateHook(reinterpret_cast<void*>(&dual_target_b),reinterpret_cast<void*>(&upscale_observer_entry),&upscale_original_trampoline)==MH_OK,"create second hook");
        require(fsr_original_trampoline&&upscale_original_trampoline&&fsr_original_trampoline!=upscale_original_trampoline,"independent trampolines");
        require(MH_EnableHook(reinterpret_cast<void*>(&dual_target_a))==MH_OK,"enable first hook");
        require(MH_EnableHook(reinterpret_cast<void*>(&dual_target_b))==MH_OK,"enable second hook");
        for(unsigned which=0;which<2;++which){
            const auto before=originals[which].load(),other=originals[1-which].load();
            for(std::uint64_t i=0;i<1000;++i){SetLastError(incomingError);const auto got=which?dual_target_b(i,i+11):dual_target_a(i,i+11);const auto error=GetLastError();require(got==value(which,i,i+11),"forwarded to wrong original");require(error==(which?outgoingB:outgoingA),"LastError changed");}
            require(originals[which]-before==1000&&originals[1-which]==other,"crossed trampoline storage");
        }
        originalCrossCall=true;SetLastError(incomingError);
        const auto cross=dual_target_a(5,7);const auto error=GetLastError();originalCrossCall=false;
        require(cross==(value(0,5,7)^value(1,6,8))&&error==outgoingA,"original cross-call corrupted");
        helperCrossCall=true;SetLastError(incomingError);const auto reentered=dual_target_a(19,29);helperCrossCall=false;
        require(reentered==value(0,19,29)&&nested==1,"observer cross-call corrupted");
        throwing=true;SetLastError(incomingError);const auto thrown=dual_target_b(31,41);throwing=false;
        require(thrown==value(1,31,41)&&exceptions==1,"observer exception blocked forwarding");
        std::array<std::thread,8> workers;
        for(unsigned t=0;t<workers.size();++t)workers[t]=std::thread([t]{
            for(std::uint64_t i=0;i<4000;++i){const unsigned which=(i+t)&1;SetLastError(incomingError);const auto got=which?dual_target_b(i,t):dual_target_a(i,t);if(got!=value(which,i,t)||GetLastError()!=(which?outgoingB:outgoingA))++failures;}
        });
        for(auto& worker:workers)worker.join();
        const auto previousSamples=sampled.load();sampling=false;
        SetLastError(incomingError);require(dual_target_a(47,53)==value(0,47,53),"first sampling-off forwarding");
        SetLastError(incomingError);require(dual_target_b(59,61)==value(1,59,61),"second sampling-off forwarding");
        require(sampled==previousSamples,"sampling-off still sampled");
        require(failures==0&&observed[0]==originals[0]&&observed[1]==originals[1],"forward counts/error mismatch");
        std::printf("{\"passed\":true,\"checks\":%llu,\"originalA\":%llu,\"originalB\":%llu,\"threads\":8,\"separateTrampolines\":true,\"crossCalls\":true,\"exceptionContained\":true,\"gameTouched\":false,\"gpuTouched\":false}\n",static_cast<unsigned long long>(checks),static_cast<unsigned long long>(originals[0].load()),static_cast<unsigned long long>(originals[1].load()));
        // Own-process trampoline/code storage remains intact until process exit.
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"dual-detour-test: %s\n",e.what());return 1;}
}
