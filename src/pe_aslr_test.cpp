#include <windows.h>
#include "fsr_pe_guard.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string>

namespace {
struct File {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
struct Reservation {
    void* value{};
    ~Reservation(){if(value)VirtualFree(value,0,MEM_RELEASE);}
};
struct Module {
    HMODULE value{};
    ~Module(){if(value)FreeLibrary(value);}
};
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool fileRead(HANDLE file,std::uint64_t size,std::uint64_t offset,void* data,DWORD bytes){
    if(offset>size||bytes>size-offset)return false;
    LARGE_INTEGER at{};at.QuadPart=static_cast<LONGLONG>(offset);DWORD read{};
    return SetFilePointerEx(file,at,nullptr,FILE_BEGIN)&&ReadFile(file,data,bytes,&read,nullptr)&&read==bytes;
}
bool liveRead(const void* at,void* data,SIZE_T bytes){
    SIZE_T read{};return ReadProcessMemory(GetCurrentProcess(),at,data,bytes,&read)&&read==bytes;
}
std::string quoted(const std::string& value){
    std::string out="\"";for(unsigned char c:value){if(c=='"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}else if(c<32)out+='?';else out+=static_cast<char>(c);}return out+'"';
}
}
int main(int argc,char** argv){
    try{
        const bool force=argc==2&&std::string(argv[1])=="--force-rebase";
        require(argc==1||force,"Only --force-rebase is supported; DLL path is fixed beside this self-test");
        std::array<wchar_t,32768> executable{};const auto n=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        require(n&&n<executable.size(),"Cannot locate self-test executable");
        const auto path=std::filesystem::path(executable.data()).parent_path()/L"xess-pe-aslr-fixture.dll";
        File file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
        require(file.value!=INVALID_HANDLE_VALUE,"Cannot open self-owned fixture DLL");
        LARGE_INTEGER length{};require(GetFileSizeEx(file.value,&length)&&length.QuadPart>0&&length.QuadPart<=1024*1024,"Unexpected self-owned fixture size");
        IMAGE_DOS_HEADER diskDos{};IMAGE_NT_HEADERS64 disk{};
        require(fileRead(file.value,length.QuadPart,0,&diskDos,sizeof(diskDos))&&diskDos.e_magic==IMAGE_DOS_SIGNATURE&&diskDos.e_lfanew>=64,"Invalid fixture DOS header");
        require(fileRead(file.value,length.QuadPart,diskDos.e_lfanew,&disk,sizeof(disk))&&disk.Signature==IMAGE_NT_SIGNATURE&&disk.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64&&disk.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC,"Invalid fixture PE header");
        require(disk.OptionalHeader.ImageBase==0x180000000ull,"Fixture preferred base changed");
        require(disk.OptionalHeader.DllCharacteristics&IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE,"Fixture lacks ASLR flag");
        require(!(disk.FileHeader.Characteristics&IMAGE_FILE_RELOCS_STRIPPED)&&disk.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size,"Fixture lacks relocation records");
        const auto preferred=static_cast<std::uintptr_t>(disk.OptionalHeader.ImageBase);
        require(disk.OptionalHeader.SizeOfImage>0&&disk.OptionalHeader.SizeOfImage<16*1024*1024,"Unexpected fixture image size");
        Reservation occupied;
        if(force){
            occupied.value=VirtualAlloc(reinterpret_cast<void*>(preferred),disk.OptionalHeader.SizeOfImage,MEM_RESERVE,PAGE_NOACCESS);
            require(occupied.value==reinterpret_cast<void*>(preferred),"Could not reserve fixture preferred base; forced-rebase experiment is inconclusive");
        }
        Module module{LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32)};
        require(module.value!=nullptr,"Windows loader did not load the self-owned fixture");
        const auto actual=reinterpret_cast<std::uintptr_t>(module.value);
        require(!force||actual!=preferred,"Forced-rebase fixture unexpectedly loaded into its occupied preferred base");
        IMAGE_DOS_HEADER liveDos{};IMAGE_NT_HEADERS64 live{};
        require(liveRead(module.value,&liveDos,sizeof(liveDos))&&liveDos.e_magic==IMAGE_DOS_SIGNATURE&&liveDos.e_lfanew==diskDos.e_lfanew,"Cannot read own loaded DOS header");
        require(liveRead(reinterpret_cast<const void*>(actual+liveDos.e_lfanew),&live,sizeof(live)),"Cannot read own loaded NT header");
        MEMORY_BASIC_INFORMATION memory{};
        require(VirtualQuery(module.value,&memory,sizeof(memory))==sizeof(memory),"VirtualQuery failed on own loaded DLL");
        const auto allocation=reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
        require(allocation==actual&&memory.Type==MEM_IMAGE,"Own loaded DLL is not the expected image allocation");
        const auto anchor=GetProcAddress(module.value,"xess_aslr_anchor");
        const auto pointer=GetProcAddress(module.value,"xess_aslr_pointer");
        std::uintptr_t relocatedPointer{};
        require(anchor&&pointer&&liveRead(reinterpret_cast<const void*>(pointer),&relocatedPointer,sizeof(relocatedPointer)),"Cannot inspect own relocation fixture exports");
        require(relocatedPointer==reinterpret_cast<std::uintptr_t>(anchor),"Windows did not relocate the fixture's absolute pointer");
        unsigned differences=0,outsideImageBase=0;
        constexpr size_t start=offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+offsetof(IMAGE_OPTIONAL_HEADER64,ImageBase);
        const auto* a=reinterpret_cast<const unsigned char*>(&disk);const auto* b=reinterpret_cast<const unsigned char*>(&live);
        for(size_t i=0;i<sizeof(disk);++i)if(a[i]!=b[i]){++differences;if(i<start||i>=start+sizeof(disk.OptionalHeader.ImageBase))++outsideImageBase;}
        namespace pe=xess_vk::fsr_pe_guard;
        struct DiskContext {HANDLE file;std::uint64_t size;} diskContext{file.value,static_cast<std::uint64_t>(length.QuadPart)};
        struct LiveContext {std::uintptr_t base;std::uint32_t size;} liveContext{actual,disk.OptionalHeader.SizeOfImage};
        const pe::Reader diskReader{&diskContext,[](void* context,std::uint64_t offset,void* output,std::uint32_t bytes) noexcept {
            const auto& d=*static_cast<DiskContext*>(context);
            if(offset>d.size||bytes>d.size-offset)return pe::ReadResult{false,0,0,"file_bounds",false};
            LARGE_INTEGER at{};at.QuadPart=static_cast<LONGLONG>(offset);
            if(!SetFilePointerEx(d.file,at,nullptr,FILE_BEGIN))return pe::ReadResult{false,GetLastError(),0,"SetFilePointerEx",true};
            DWORD count{};const BOOL okay=ReadFile(d.file,output,bytes,&count,nullptr);const DWORD error=okay?0:GetLastError();
            return pe::ReadResult{okay!=FALSE,error,count,"ReadFile",true};
        }};
        const pe::Reader liveReader{&liveContext,[](void* context,std::uint64_t offset,void* output,std::uint32_t bytes) noexcept {
            const auto& d=*static_cast<LiveContext*>(context);
            if(offset>d.size||bytes>d.size-offset)return pe::ReadResult{false,0,0,"image_bounds",false};
            SIZE_T count{};const BOOL okay=ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(d.base+offset),output,bytes,&count);const DWORD error=okay?0:GetLastError();
            return pe::ReadResult{okay!=FALSE,error,count,"ReadProcessMemory",true};
        }};
        pe::Headers validatedDisk,validatedLive;pe::Diagnostic guard;
        const bool guardPassed=pe::validate(diskReader,liveReader,actual,disk.OptionalHeader.SizeOfImage,validatedDisk,validatedLive,guard);
        require(guardPassed&&guard.ntDifferencesOutsideImageBase==0&&guard.sectionCountsMatch&&guard.sectionDifferenceCount==0,"Actual loader headers did not pass the exact ImageBase-only guard");
        require(guard.imageBaseNormalizationApplied==(actual!=preferred),"Unexpected normalization decision for actual loader headers");
        require(!std::memcmp(&disk,&validatedDisk.nt,sizeof(disk))&&!std::memcmp(&live,&validatedLive.nt,sizeof(live)),"Guard changed the observed headers");
        std::ostringstream guardJson;pe::writeJson(guardJson,guard,validatedDisk,validatedLive);
        // The reader stays bound to the real DLL. Only the asserted actual base
        // is wrong, which must not authorize ImageBase normalization.
        pe::Diagnostic wrong;pe::Headers wrongDisk,wrongLive;
        const bool wrongAccepted=pe::validate(diskReader,liveReader,actual+0x10000,disk.OptionalHeader.SizeOfImage,wrongDisk,wrongLive,wrong);
        require(!wrongAccepted&&!wrong.imageBaseNormalizationApplied&&std::string(wrong.state)=="refused_pe_nt_mismatch","Wrong claimed actual module base was accepted");
        std::ostringstream wrongJson;pe::writeJson(wrongJson,wrong,wrongDisk,wrongLive);
        std::printf("{\"testPassed\":true,\"fixture\":\"self_compiled_minimal_dll\",\"forcedRebase\":%s,\"preferredRangeReserved\":%s,\"diskPreferredImageBase\":%llu,\"liveHeaderImageBase\":%llu,\"actualModuleBase\":%llu,\"virtualQueryAllocationBase\":%llu,\"imageSize\":%lu,\"actualBaseDiffersFromPreferred\":%s,\"liveImageBaseEqualsActual\":%s,\"liveImageBaseEqualsPreferred\":%s,\"ntDifferentBytes\":%u,\"ntDifferencesOutsideImageBase\":%u,\"absolutePointerRelocatedCorrectly\":true,\"newGuardPassed\":true,\"wrongActualBaseRejected\":true,\"guard\":%s,\"wrongActualBaseGuard\":%s,\"gameModuleLoaded\":false,\"externalProcessOpened\":false}\n",
            force?"true":"false",occupied.value?"true":"false",static_cast<unsigned long long>(disk.OptionalHeader.ImageBase),static_cast<unsigned long long>(live.OptionalHeader.ImageBase),static_cast<unsigned long long>(actual),static_cast<unsigned long long>(allocation),disk.OptionalHeader.SizeOfImage,
            actual!=preferred?"true":"false",live.OptionalHeader.ImageBase==actual?"true":"false",live.OptionalHeader.ImageBase==preferred?"true":"false",differences,outsideImageBase,guardJson.str().c_str(),wrongJson.str().c_str());
        return 0;
    }catch(const std::exception& error){std::printf("{\"testPassed\":false,\"error\":%s,\"gameModuleLoaded\":false,\"externalProcessOpened\":false}\n",quoted(error.what()).c_str());return 1;}
}
