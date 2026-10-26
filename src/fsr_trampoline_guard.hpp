#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace xess_vk::fsr_trampoline_guard {
static_assert(sizeof(std::uintptr_t)==8);
// Pinned MinHook x64 forms only; callers still validate executable ranges and
// the fixed module/target identity before reading these bytes.
inline bool absoluteJumpMatches(std::span<const unsigned char> bytes, std::uintptr_t destination) noexcept {
    constexpr std::array<unsigned char,6> opcode{0xff,0x25,0,0,0,0};
    std::uintptr_t actual{};
    if(bytes.size()!=14 || std::memcmp(bytes.data(),opcode.data(),opcode.size()))return false;
    std::memcpy(&actual,bytes.data()+6,sizeof(actual));return actual==destination;
}
inline bool ripDestination(std::uintptr_t address,std::int32_t displacement,std::uintptr_t& result) noexcept {
    constexpr auto maximum=std::numeric_limits<std::uintptr_t>::max();
    if(address>maximum-7)return false;
    address+=7;
    if(displacement<0) {
        const auto distance=static_cast<std::uintptr_t>(-static_cast<std::int64_t>(displacement));
        if(address<distance)return false;
        result=address-distance;
    } else {
        if(address>maximum-static_cast<std::uint32_t>(displacement))return false;
        result=address+static_cast<std::uint32_t>(displacement);
    }
    return result!=0;
}
inline bool ordinaryMatches(std::span<const unsigned char> bytes,std::uintptr_t trampoline,
    std::uintptr_t target,std::uintptr_t detour,std::span<const unsigned char> expected,std::uint8_t length) noexcept {
    if((length!=5&&length!=7)||expected.size()<length||bytes.size()!=length+28u||
       !trampoline||!target||!detour||target>UINTPTR_MAX-length||trampoline>UINTPTR_MAX-(length+28u))return false;
    if(length==7 && expected[0]==0x83 && expected[1]==0x3d) {
        if(bytes[0]!=0x83||bytes[1]!=0x3d||bytes[6]!=expected[6])return false;
        std::int32_t original{},relocated{};std::uintptr_t before{},after{};
        std::memcpy(&original,expected.data()+2,4);std::memcpy(&relocated,bytes.data()+2,4);
        if(!ripDestination(target,original,before)||!ripDestination(trampoline,relocated,after)||before!=after)return false;
    } else {
        // Enqueue (+0x08) and render-parameter construction (+0x20) copy two
        // ordinary instructions, mov rax,rsp; mov [rax+disp8],rbx, with no relocation.
        constexpr std::array<unsigned char,6> frameSetup{0x48,0x8b,0xc4,0x48,0x89,0x58};
        if(length==7 && (std::memcmp(expected.data(),frameSetup.data(),frameSetup.size()) ||
                         (expected[6]!=0x08 && expected[6]!=0x20)))return false;
        if(std::memcmp(bytes.data(),expected.data(),length))return false;
    }
    return absoluteJumpMatches(bytes.subspan(length,14),target+length)&&
        absoluteJumpMatches(bytes.subspan(length+14,14),detour);
}
} // namespace xess_vk::fsr_trampoline_guard
