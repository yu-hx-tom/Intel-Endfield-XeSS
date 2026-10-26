#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fixture {
std::wstring directory;
unsigned attributes{}, opens{}, reads{}, environments{};
bool singleChain{true};
std::wstring redirected(const wchar_t* path) {
    constexpr std::wstring_view markerDirectory=L"C:\\ProgramData\\EndfieldXeSSDiag\\";
    const std::wstring_view value(path);
    return value.starts_with(markerDirectory) ? directory+std::wstring(value.substr(markerDirectory.size())) : std::wstring(value);
}
DWORD WINAPI attributesW(LPCWSTR path) {
    ++attributes;
    return ::GetFileAttributesW(redirected(path).c_str());
}
HANDLE WINAPI createFileW(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD disposition,DWORD flags,HANDLE templateFile) {
    ++opens;
    return ::CreateFileW(redirected(path).c_str(),access,share,security,disposition,flags,templateFile);
}
BOOL WINAPI readFile(HANDLE file,LPVOID bytes,DWORD size,LPDWORD count,LPOVERLAPPED overlapped) {
    ++reads;
    return ::ReadFile(file,bytes,size,count,overlapped);
}
DWORD WINAPI environmentW(LPCWSTR name,LPWSTR value,DWORD size) {
    if(std::wcscmp(name,L"XESS_FAKE_SWAPCHAIN"))return ::GetEnvironmentVariableW(name,value,size);
    ++environments;
    if(!singleChain)return 0;
    if(size<2)return 2;
    value[0]=L'1';value[1]=0;return 1;
}
}
// Only this CPU executable redirects the fixed marker directory to owned temp
// files. The production cache still executes real Win32 reads, counted here.
#define GetFileAttributesW fixture::attributesW
#define CreateFileW fixture::createFileW
#define ReadFile fixture::readFile
#define GetEnvironmentVariableW fixture::environmentW
#include "diagnostic_flags.hpp"
#undef GetFileAttributesW
#undef CreateFileW
#undef ReadFile
#undef GetEnvironmentVariableW

namespace {
namespace flags=xess_vk::diagnostic_flags;
unsigned checks{};
void require(bool value,const char* message) {
    ++checks;
    if(!value)throw std::runtime_error(message);
}
struct TemporaryFiles {
    std::wstring path;
    TemporaryFiles() {
        wchar_t root[MAX_PATH]{},name[MAX_PATH]{};
        const auto length=GetTempPathW(MAX_PATH,root);
        require(length&&length<MAX_PATH&&GetTempFileNameW(root,L"xcf",0,name),"create owned temporary directory name");
        require(DeleteFileW(name)&&CreateDirectoryW(name,nullptr),"create owned temporary directory");
        path=name;path+=L'\\';fixture::directory=path;
    }
    ~TemporaryFiles() {
        for(const auto name:{L"NumericConfig",L"AsyncCopyPath",L"SharedCopyPath",L"SyncCopyPath",L"DisableSharedCopyPath",L"SharedCopySync",L"AsyncCopyFrames",L"DepthComputePath",L"DisableDepthComputePath"})
            DeleteFileW((path+name).c_str());
        RemoveDirectoryW(path.substr(0,path.size()-1).c_str());
    }
    std::wstring file(const wchar_t* name)const {return path+name;}
    void write(const wchar_t* name,std::string_view text)const {
        const auto handle=CreateFileW(file(name).c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(handle!=INVALID_HANDLE_VALUE,"open owned temporary configuration");
        DWORD count{};
        const auto written=WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&count,nullptr);
        CloseHandle(handle);
        require(written&&count==text.size(),"write complete owned temporary configuration");
    }
};
void policy() {
    using S=flags::CopyPathSource;
    constexpr auto D=S::Default,F=S::File;
    struct Case {flags::CopyPathInputs input;flags::CopyPathSelection expected;};
    constexpr Case cases[]{
        {{true},{false,true,0,D,D,D}},
        {{false},{false,false,0,D,D,D}},
        {{false,true},{false,false,0,D,D,D}},
        {{false,false,true},{false,false,0,D,D,D}},
        {{false,true,true},{false,false,0,D,D,D}},
        {{true,true,true},{true,true,2,F,D,F}},
        {{true,false,false,true},{false,false,0,F,F,F}},
        {{true,true,true,true,true,1,true},{false,false,0,F,F,F}},
        {{false,true,true,true,false,1,true},{false,false,0,D,D,D}},
        {{true,false,false,false,true},{false,false,0,D,D,F}},
        {{false,true,true,false,true},{false,false,0,D,D,D}},
        {{true,false,false,false,false,1,true},{true,true,1,F,F,D}},
        {{true,false,false,false,false,2,true},{true,true,2,F,F,D}},
        {{true,false,false,false,false,1,false},{false,true,0,D,D,D}},
        {{true,false,false,false,false,3,true},{false,true,0,D,D,D}},
        {{false,false,false,false,false,1,true},{false,false,0,D,D,D}},
        {{false,true,false,false,false,1,true},{false,false,0,D,D,D}},
        {{true,false,false,false,false,2,false,true},{false,true,0,F,F,F}},
        {{true,true,true,false,false,1,true,true},{false,true,0,F,F,F}},
        {{true,true,true,false,true,1,true,true},{false,false,0,F,F,F}},
        {{true,true,true,true,false,1,true,true},{false,false,0,F,F,F}},
        {{false,true,true,false,false,1,true,true},{false,false,0,D,D,D}},
    };
    for(const auto& c:cases) {
        const auto value=flags::selectCopyPath(c.input);
        require(value.async==c.expected.async,"single-chain default and legacy route exclusion policy");
        require(value.shared==c.expected.shared,"shared default and independent disable policy");
        require(value.inflightLimit==c.expected.inflightLimit,"effective inflight limit follows route and valid numeric file");
        require(value.routeSource==c.expected.routeSource,"copy route provenance");
        require(value.inflightSource==c.expected.inflightSource,"inflight provenance");
        require(value.sharedSource==c.expected.sharedSource,"shared provenance");
    }
    require(std::string_view(flags::copyPathSourceName(D))=="default"&&std::string_view(flags::copyPathSourceName(F))=="file","stable session log provenance values");
    for(unsigned bits=0;bits<32;++bits) {
        flags::CopyPathInputs input{bool(bits&1),bool(bits&2),bool(bits&4),true,bool(bits&8),1,true,bool(bits&16)};
        const auto value=flags::selectCopyPath(input);
        require(!value.async&&!value.shared&&!value.inflightLimit,"SyncCopyPath has highest priority over every enable or frame-limit combination");
    }
}
void numericFiles(const TemporaryFiles& files) {
    struct Case {std::string_view text;std::uint32_t frames;bool valid;};
    const Case cases[]{
        {"",2,false},{" \t\r\n",2,false},{"1",1,true},{"2",2,true},{" 1\r\n",1,true},{" 2\r\n",2,true},
        {"\xEF\xBB\xBF" "1\r\n",1,true},{"0",2,false},{"3",2,false},{"01",2,false},{"1x",2,false},{"1 2",2,false},
        {std::string_view{"1\0",2},2,false},{"9999999999999999",2,false}
    };
    const auto path=files.file(L"NumericConfig");
    for(const auto& c:cases) {
        const auto parsed=flags::parseAsyncCopyFramesReading(c.text);
        require(parsed.frames==c.frames&&parsed.valid==c.valid,"pure numeric parser distinguishes configured values from default fallback");
        require(flags::parseAsyncCopyFrames(c.text)==c.frames,"existing numeric parser API remains compatible");
        files.write(L"NumericConfig",c.text);SetLastError(0x12345678);
        const auto reading=flags::readAsyncCopyFramesReading(path.c_str());
        require(reading.frames==c.frames&&reading.valid==c.valid&&GetLastError()==0x12345678,"real bounded numeric file read and LastError preservation");
    }
    files.write(L"NumericConfig",'1'+std::string(16,' '));
    const auto oversized=flags::readAsyncCopyFramesReading(path.c_str());
    require(oversized.frames==2&&!oversized.valid,"one excess byte refuses a truncated valid numeric prefix");
    files.write(L"NumericConfig","1");
    const auto locked=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(locked!=INVALID_HANDLE_VALUE,"lock owned configuration against reads");
    const auto unreadable=flags::readAsyncCopyFramesReading(path.c_str());
    CloseHandle(locked);
    require(unreadable.frames==2&&!unreadable.valid,"unreadable numeric file has default provenance");
    require(DeleteFileW(path.c_str()),"remove owned numeric configuration");
    const auto missing=flags::readAsyncCopyFramesReading(path.c_str());
    require(missing.frames==2&&!missing.valid,"missing numeric file has default provenance");
}
void realFiles(const TemporaryFiles& files) {
    files.write(L"AsyncCopyPath","");files.write(L"SharedCopyPath","");files.write(L"AsyncCopyFrames","1");
    SetLastError(0x12345678);
    const auto input=flags::readCopyPathInputs();
    require(GetLastError()==0x12345678&&input.singleChain&&input.asyncFile&&input.sharedFile&&!input.syncFile&&!input.disableSharedFile&&input.framesFile&&input.frames==1,
        "real Win32 file reads retain exact opt-in and valid K1 facts");
    const auto k1=flags::selectCopyPath(input);
    require(k1.async&&k1.shared&&k1.inflightLimit==1&&k1.routeSource==flags::CopyPathSource::File&&k1.inflightSource==flags::CopyPathSource::File&&k1.sharedSource==flags::CopyPathSource::File,
        "real old opt-ins remain compatible with single-chain K1 and file provenance");
    files.write(L"DisableSharedCopyPath","");
    const auto disabled=flags::selectCopyPath(flags::readCopyPathInputs());
    require(disabled.async&&!disabled.shared&&disabled.inflightLimit==1&&disabled.sharedSource==flags::CopyPathSource::File,"real shared escape file leaves async K1 active");
    require(DeleteFileW(files.file(L"DisableSharedCopyPath").c_str()),"remove owned shared disable for synchronous shared opt-in");
    files.write(L"SharedCopySync","");
    const auto sharedSync=flags::selectCopyPath(flags::readCopyPathInputs());
    require(!sharedSync.async&&sharedSync.shared&&!sharedSync.inflightLimit&&sharedSync.routeSource==flags::CopyPathSource::File&&
        sharedSync.inflightSource==flags::CopyPathSource::File&&sharedSync.sharedSource==flags::CopyPathSource::File,
        "real SharedCopySync opts into shared planes with the synchronous route and no async capacity");
    files.write(L"DisableSharedCopyPath","");
    files.write(L"SyncCopyPath","");
    const auto sync=flags::selectCopyPath(flags::readCopyPathInputs());
    require(!sync.async&&!sync.shared&&!sync.inflightLimit&&sync.routeSource==flags::CopyPathSource::File,"real SyncCopyPath dominates all enable and limit files");
    fixture::singleChain=false;
    const auto legacy=flags::selectCopyPath(flags::readCopyPathInputs());
    require(!legacy.async&&!legacy.shared&&!legacy.inflightLimit&&legacy.routeSource==flags::CopyPathSource::Default,"real legacy route ignores single-chain copy-policy files");
    fixture::singleChain=true;
    for(const auto name:{L"AsyncCopyPath",L"SharedCopyPath",L"SyncCopyPath",L"DisableSharedCopyPath",L"SharedCopySync",L"AsyncCopyFrames"})
        require(DeleteFileW(files.file(name).c_str()),"remove owned policy input before cache test");
}
void once(const TemporaryFiles& files) {
    fixture::attributes=fixture::opens=fixture::reads=fixture::environments=0;
    SetLastError(0x12345678);
    const auto* first=&flags::copyPathSelection();
    require(GetLastError()==0x12345678&&!first->async&&first->shared&&!first->inflightLimit,"first production snapshot defaults fake single-chain to synchronous/shared and preserves LastError");
    require(fixture::attributes==5&&fixture::opens==1&&!fixture::reads&&fixture::environments==1,"one real file/environment read pass initializes all copy-policy inputs");
    files.write(L"AsyncCopyPath","");files.write(L"SharedCopyPath","");files.write(L"SyncCopyPath","");
    files.write(L"DisableSharedCopyPath","");files.write(L"AsyncCopyFrames","1");fixture::singleChain=false;
    files.write(L"SharedCopySync","");
    SetLastError(0x12345678);
    require(&flags::copyPathSelection()==first&&!flags::asyncCopyPath()&&flags::sharedCopyPath()&&flags::asyncCopyFrames()==2,"later file/environment edits cannot change the process snapshot");
    require(GetLastError()==0x12345678&&fixture::attributes==5&&fixture::opens==1&&!fixture::reads&&fixture::environments==1,"all compatibility getters reuse the exact cached read without I/O");
    require(first->routeSource==flags::CopyPathSource::Default&&first->inflightSource==flags::CopyPathSource::Default&&first->sharedSource==flags::CopyPathSource::Default,"cached source provenance remains with the effective snapshot");
}
void depthComputeFiles(const TemporaryFiles& files) {
    for(unsigned bits=0;bits<8;++bits) {
        fixture::singleChain=bool(bits&4);
        for(const auto name:{L"DepthComputePath",L"DisableDepthComputePath"})DeleteFileW(files.file(name).c_str());
        if(bits&1)files.write(L"DepthComputePath","");
        if(bits&2)files.write(L"DisableDepthComputePath","");
        fixture::attributes=fixture::opens=fixture::reads=fixture::environments=0;
        SetLastError(0x12345678);
        require(flags::readDepthComputePath()==bool(((bits&1)||(bits&4))&&!(bits&2))&&GetLastError()==0x12345678,
            "depth compute defaults on only for single-chain, retains legacy opt-in and gives disable highest priority");
        require(fixture::attributes==2&&!fixture::opens&&!fixture::reads&&fixture::environments==1,
            "depth compute reads its two files and single-chain mode once");
    }
    for(const auto name:{L"DepthComputePath",L"DisableDepthComputePath"})require(DeleteFileW(files.file(name).c_str()),"remove owned depth compute policy inputs");
    fixture::attributes=fixture::opens=fixture::reads=fixture::environments=0;
    SetLastError(0x12345678);
    require(flags::depthComputePath()&&GetLastError()==0x12345678&&fixture::attributes==2,"first production single-chain depth compute snapshot defaults on");
    files.write(L"DepthComputePath","");files.write(L"DisableDepthComputePath","");SetLastError(0x12345678);
    fixture::singleChain=false;
    require(flags::depthComputePath()&&flags::depthComputePath()&&GetLastError()==0x12345678&&fixture::attributes==2&&
        !fixture::opens&&!fixture::reads&&fixture::environments==1,"later depth compute file or environment edits cannot trigger a reread or change the process snapshot");
}
}
int main() {
    try {
        TemporaryFiles files;policy();numericFiles(files);realFiles(files);once(files);depthComputeFiles(files);
        std::printf("{\"event\":\"copy_path_policy_cpu\",\"passed\":true,\"checks\":%u,\"real_win32_reads\":true,\"live_markers_changed\":false,\"gpu_executed\":false,\"game_touched\":false}\n",checks);
        return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
