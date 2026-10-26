#pragma once
#include <cstdint>
#include <limits>

namespace xess_vk::gameplay_state {
// Pinned GameAssembly: GameInstance.get_isInGameplay is get_curState() == 40.
inline constexpr std::uintptr_t typeSlotRva=0x1068c0c8;
inline constexpr std::int32_t unknown=-1,active=40;
// Read the existing state only; never initialize IL2CPP types or call game code.
template<class Read> std::int32_t read(std::uintptr_t slot,Read&& source) noexcept {
    const auto field=[&](std::uintptr_t object,std::uintptr_t offset,auto& value){
        return object&&object<=std::numeric_limits<std::uintptr_t>::max()-offset-sizeof(value)&&source(object+offset,value);
    };
    std::uintptr_t type{},fields{},instance{},stateObject{};
    std::uint32_t initialized{};std::int32_t state{};
    if(!field(slot,0,type)||!field(type,0xe0,initialized)||!initialized||
       !field(type,0xb8,fields)||!field(fields,0,instance)||!field(instance,0x48,stateObject)||
       !field(stateObject,0x20,state))return unknown;
    return state;
}
}
