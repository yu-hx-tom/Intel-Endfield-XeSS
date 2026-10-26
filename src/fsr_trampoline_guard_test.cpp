#include "fsr_trampoline_guard.hpp"
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
namespace guard=xess_vk::fsr_trampoline_guard;
using Function=unsigned(*)();
Function original{};
unsigned checks{},patterns{},corruptions{};
void require(bool okay,const char* reason) {++checks;if(!okay)throw std::runtime_error(reason);}
void mh(MH_STATUS status,const char* reason) {require(status==MH_OK,reason);}
unsigned replacement() {return original()+1000;}
std::uintptr_t address(const void* p) {return reinterpret_cast<std::uintptr_t>(p);}

// Regression oracle: the d74dd86 length==7 branch required this RIP-only form.
bool oldSevenMatches(std::span<const unsigned char> bytes,std::uintptr_t trampoline,
                     std::uintptr_t target,std::span<const unsigned char> expected) {
    if(bytes[0]!=0x83||bytes[1]!=0x3d||bytes[6]!=expected[6])return false;
    std::int32_t before{},after{};
    std::memcpy(&before,expected.data()+2,4);std::memcpy(&after,bytes.data()+2,4);
    return static_cast<std::int64_t>(target)+7+before==static_cast<std::int64_t>(trampoline)+7+after;
}
void exercise(unsigned char* target,std::uint8_t length,unsigned expectedValue,bool rip) {
    ++patterns;
    const auto function=reinterpret_cast<Function>(target);
    require(function()==expectedValue,"owned baseline computes expected result");
    mh(MH_CreateHook(target,reinterpret_cast<void*>(&replacement),reinterpret_cast<void**>(&original)),"create owned prefix hook");
    const auto trampoline=reinterpret_cast<const unsigned char*>(original);
    const auto trampolineAddress=address(trampoline),targetAddress=address(target);
    const auto detourAddress=reinterpret_cast<std::uintptr_t>(&replacement);
    const std::span<const unsigned char> expected(target,16),actual(trampoline,length+28);
    require(guard::ordinaryMatches(actual,trampolineAddress,targetAddress,detourAddress,expected,length),
            "production validator accepts actual pinned-MinHook trampoline");
    if(length==7)require(oldSevenMatches(actual,trampolineAddress,targetAddress,expected)==rip,
        "old RIP-only branch rejects enqueue seven-byte prefix and accepts RIP comparison");
    require(original()==expectedValue,"actual trampoline preserves owned computation");
    for(std::size_t i=0;i<actual.size();++i) {
        std::array<unsigned char,35> changed{};std::copy(actual.begin(),actual.end(),changed.begin());changed[i]^=1;
        require(!guard::ordinaryMatches(std::span(changed).first(actual.size()),trampolineAddress,targetAddress,detourAddress,expected,length),
                "each corrupted copied instruction, jump-back or relay byte is refused");++corruptions;
    }
    require(!guard::ordinaryMatches(actual,trampolineAddress,targetAddress+1,detourAddress,expected,length),"wrong exact return target refused");
    require(!guard::ordinaryMatches(actual,trampolineAddress,targetAddress,detourAddress+1,expected,length),"wrong exact relay destination refused");
    require(!guard::ordinaryMatches(actual.first(actual.size()-1),trampolineAddress,targetAddress,detourAddress,expected,length),"partial trampoline refused");
    require(!guard::ordinaryMatches(actual,UINTPTR_MAX,targetAddress,detourAddress,expected,length),"trampoline range overflow refused");
    if(length==7&&!rip) {
        std::array<unsigned char,35> unknown{};std::copy(actual.begin(),actual.end(),unknown.begin());
        std::array<unsigned char,16> unknownPrefix{};std::copy(expected.begin(),expected.end(),unknownPrefix.begin());
        unknown[6]=unknownPrefix[6]=0x10;
        require(!guard::ordinaryMatches(unknown,trampolineAddress,targetAddress,detourAddress,unknownPrefix,length),
                "unknown seven-byte literal form is not silently admitted");
    }
    mh(MH_EnableHook(target),"enable owned hook");
    require(function()==expectedValue+1000&&original()==expectedValue,"owned detour and original remain correct");
    mh(MH_DisableHook(target),"disable owned hook");require(function()==expectedValue,"owned target restored");
    mh(MH_RemoveHook(target),"remove owned hook");original=nullptr;
}
}
int main() {
    try {
        require(!GetModuleHandleW(L"UnityPlayer.dll")&&!GetModuleHandleW(L"GameAssembly.dll"),"fixture contains no game DLL");
        SYSTEM_INFO system{};GetSystemInfo(&system);const auto page=system.dwPageSize;
        auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        require(memory!=nullptr,"allocate owned code and data pages");std::fill_n(memory,page,0xcc);
        constexpr std::array<unsigned char,5> literal5{0x48,0x89,0x5c,0x24,0x08};
        constexpr std::array<unsigned char,7> enqueue7{0x48,0x8b,0xc4,0x48,0x89,0x58,0x08};
        constexpr std::array<unsigned char,7> renderParams7{0x48,0x8b,0xc4,0x48,0x89,0x58,0x20};
        constexpr std::array<unsigned char,6> returns42{0xb8,42,0,0,0,0xc3};
        std::copy(literal5.begin(),literal5.end(),memory);std::copy(returns42.begin(),returns42.end(),memory+5);
        std::copy(enqueue7.begin(),enqueue7.end(),memory+64);std::copy(returns42.begin(),returns42.end(),memory+71);
        std::copy(renderParams7.begin(),renderParams7.end(),memory+192);std::copy(returns42.begin(),returns42.end(),memory+199);
        auto* compare=memory+128;compare[0]=0x83;compare[1]=0x3d;compare[6]=0;
        const auto displacement=static_cast<std::int32_t>(page-128-7);std::memcpy(compare+2,&displacement,4);
        constexpr std::array<unsigned char,7> comparisonResult{0x0f,0x94,0xc0,0x0f,0xb6,0xc0,0xc3};
        std::copy(comparisonResult.begin(),comparisonResult.end(),compare+7);
        DWORD previous{};require(VirtualProtect(memory,page,PAGE_EXECUTE_READ,&previous)!=FALSE,"make owned code executable");
        require(FlushInstructionCache(GetCurrentProcess(),memory,page)!=FALSE,"flush owned code");
        mh(MH_Initialize(),"initialize private MinHook");
        exercise(memory,5,42,false);exercise(memory+64,7,42,false);exercise(memory+192,7,42,false);exercise(compare,7,1,true);
        mh(MH_Uninitialize(),"release private MinHook");require(VirtualFree(memory,0,MEM_RELEASE)!=FALSE,"free owned pages");
        std::cout<<"{\"passed\":true,\"patterns\":"<<patterns<<",\"checks\":"<<checks<<",\"corruptionsRejected\":"<<corruptions
            <<",\"oldEnqueueRejected\":true,\"actualMinHook\":true,\"sharedProductionValidator\":true,\"gameTouched\":false,\"gpuTouched\":false}\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
