// Included after Attempt: uses the same pinned Unity hash, PE and file lock.
namespace frameCap {
constexpr DWORD setterRva=0x57e0b0,getterRva=0x19f950;
constexpr std::array<unsigned char,16> setterBytes{0x48,0x83,0xec,0x28,0xe8,0xe7,0x18,0xc2,0xff,0x84,0xc0,0x74,0x20,0x48,0x8b,0x05};
constexpr std::array<unsigned char,16> getterBytes{0xe9,0x0b,0,0,0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc};
using Set=void(*)(int);using Get=int(*)();
Set original{};Get get{};
unsigned targetFps=40;
void entry(int value){
    const auto forwarded=game_frame_cap::map(value,true,targetFps);
    ++game_frame_cap::calls;if(forwarded!=value)++game_frame_cap::remapped;
    game_frame_cap::lastRequested=value;game_frame_cap::lastForwarded=forwarded;
    original(forwarded);
    const auto error=GetLastError();game_frame_cap::readback=get();SetLastError(error);
}
void install(Attempt& attempt) noexcept {
    const auto& selection=launcher_preferences::startupFrameCap();
    if(!selection.requested)return;
    if(selection.invalid){game_frame_cap::state="refused_target_value";return;}
    targetFps=selection.target;
    game_frame_cap::requested=true;game_frame_cap::state="refused_setter_site";
    try{
        const auto module=attempt.module.value;const auto base=attempt.base;
        const auto verify=[&](DWORD rva,const auto& bytes){
            std::uint64_t raw{};std::array<unsigned char,16> disk{};
            return textRange(attempt.liveHeaders,rva,bytes.size(),raw)&&
                fileRead(attempt.file.value,attempt.fileSize,raw,disk.data(),DWORD(bytes.size()))&&disk==bytes&&
                codeRange(base+rva,bytes.size(),module)&&bytesEqual(base+rva,bytes);
        };
        if(!verify(setterRva,setterBytes))return;
        game_frame_cap::state="refused_getter_site";
        if(!verify(getterRva,getterBytes))return;
        // Verify the icall registration as well as code; no signature scan or arbitrary setter.
        std::uintptr_t registered{};
        game_frame_cap::state="refused_registration";
        if(!readObject(base+0x1b68990,registered)||registered!=base+setterRva)return;
        const auto target=reinterpret_cast<void*>(base+setterRva);void* trampoline{};
        game_frame_cap::state="refused_create";
        if(MH_CreateHook(target,reinterpret_cast<void*>(&entry),&trampoline)!=MH_OK||!trampoline)return;
        original=reinterpret_cast<Set>(trampoline);get=reinterpret_cast<Get>(base+getterRva);
        game_frame_cap::state="refused_enable";
        if(MH_EnableHook(target)!=MH_OK)return;
        game_frame_cap::installed=true;game_frame_cap::state="installed";
        // The menu preference may have been loaded before graphics initialization.
        if(get()==30)entry(30);
    }catch(...){game_frame_cap::state="refused_exception";}
}
}
