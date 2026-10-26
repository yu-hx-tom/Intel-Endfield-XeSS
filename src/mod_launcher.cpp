#include <windows.h>
#include "launcher_preferences.hpp"
#include "build_flavor.hpp"
#include "refresh_warning.hpp"
#include "launcher_instance.hpp"
#include "launcher_ui.hpp"
#include <dwmapi.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <aclapi.h>
#include <sddl.h>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <winver.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr size_t receiptLimit=64*1024, scriptLimit=256*1024, statusLimit=256*1024, outputLimit=64*1024;
constexpr UINT trayMessage=WM_APP+1, timerId=1;
constexpr int startId=101, trayId=102, logsId=103, exitId=104, showId=105, multiplierId=106, applyId=107, noDitherId=108,
    pathChangeId=109,pathDetectId=110,sceneId=111,debugId=112,advancedId=113,defaultsId=114,frameCapId=115,frameCapTargetId=116,diagnosticId=117;
constexpr UINT detectPathMessage=WM_APP+43;
const wchar_t* productName=L"Endfield XeSS Launcher";
constexpr char currentEfuSha256[]="65d34a67ae1e82b03e22ea7f38baffd5d75bcf5706a4f896e4633b55e2597e94";
constexpr char currentEfuCommit[]="eff32bbc89baaf25fcb25f2a8903990e172f319e";
struct Handle {
    HANDLE h=INVALID_HANDLE_VALUE;
    Handle()=default; explicit Handle(HANDLE value):h(value){}
    Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:h(std::exchange(other.h,INVALID_HANDLE_VALUE)){}
    Handle& operator=(Handle&& other) noexcept { if(this!=&other){ reset(); h=std::exchange(other.h,INVALID_HANDLE_VALUE); } return *this; }
    ~Handle(){reset();}
    void reset(){if(h && h!=INVALID_HANDLE_VALUE) CloseHandle(h); h=INVALID_HANDLE_VALUE;}
    explicit operator bool()const{return h && h!=INVALID_HANDLE_VALUE;}
};
std::runtime_error failure(const char* what){return std::runtime_error(std::string(what)+" (Win32 "+std::to_string(GetLastError())+")");}
std::wstring wide(std::string_view text){
    if(text.empty())return{};
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!n)throw failure("Invalid UTF-8");
    std::wstring result(n,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),n); return result;
}
std::string utf8(std::wstring_view text){
    if(text.empty())return{};
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(!n)throw failure("Invalid Unicode");
    std::string result(n,'\0'); WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),n,nullptr,nullptr);return result;
}
std::wstring decodeHelperText(std::string_view bytes){
    try{return wide(bytes);}catch(...){
        // Preserve diagnostics from a native child that used the legacy code page.
        if(bytes.empty())return{};
        const int n=MultiByteToWideChar(CP_ACP,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
        if(n<=0)return L"助手输出无法解码，请查看会话日志。";
        std::wstring text(n,L'\0');MultiByteToWideChar(CP_ACP,0,bytes.data(),static_cast<int>(bytes.size()),text.data(),n);return text;
    }
}
std::wstring diagnosticText(std::string_view bytes){return decodeHelperText(bytes.substr(0,8192));}
struct Json {
    enum Kind{Null,String,Number,Boolean,Object,Array} kind=Null;
    std::string text; bool flag=false;
    std::map<std::string,Json> object; std::vector<Json> array;
    const Json& at(const char* key)const{auto it=object.find(key);if(kind!=Object||it==object.end())throw std::runtime_error(std::string("Missing JSON field: ")+key);return it->second;}
    std::string str()const{if(kind!=String)throw std::runtime_error("Expected JSON string");return text;}
    bool boolean()const{if(kind!=Boolean)throw std::runtime_error("Expected JSON bool");return flag;}
    uint64_t uint64()const{
        if(kind!=Number||text.empty())throw std::runtime_error("Expected unsigned integer");
        uint64_t v=0;for(char c:text){if(c<'0'||c>'9'||v>(UINT64_MAX-static_cast<unsigned>(c-'0'))/10)throw std::runtime_error("Integer overflow");v=v*10+static_cast<unsigned>(c-'0');}return v;
    }
    uint32_t uint32()const{
        if(kind!=Number||text.empty()||text.size()>10)throw std::runtime_error("Expected unsigned integer");
        uint64_t v=0;for(char c:text){if(c<'0'||c>'9')throw std::runtime_error("Expected unsigned integer");v=v*10+static_cast<unsigned>(c-'0');if(v>UINT32_MAX)throw std::runtime_error("Integer overflow");}return static_cast<uint32_t>(v);
    }
};
class Parser {
    std::string_view input;size_t pos=0,nodes=0;
    [[noreturn]]void invalid()const{throw std::runtime_error("Malformed/bounded JSON");}
    void space(){while(pos<input.size()&&(input[pos]==' '||input[pos]=='\r'||input[pos]=='\n'||input[pos]=='\t'))++pos;}
    char take(){if(pos==input.size())invalid();return input[pos++];}
    uint32_t hex(){uint32_t value=0;for(int i=0;i<4;++i){char c=take();value<<=4;if(c>='0'&&c<='9')value|=c-'0';else if(c>='a'&&c<='f')value|=c-'a'+10;else if(c>='A'&&c<='F')value|=c-'A'+10;else invalid();}return value;}
    std::string string(){
        if(take()!='"')invalid();std::string value;
        while(true){char c=take();if(c=='"')break;if(static_cast<unsigned char>(c)<32)invalid();
            if(c!='\\'){value+=c;continue;}c=take();
            switch(c){case '"':case '\\':case '/':value+=c;break;case 'b':value+='\b';break;case 'f':value+='\f';break;case 'n':value+='\n';break;case 'r':value+='\r';break;case 't':value+='\t';break;
            case 'u':{uint32_t u=hex();std::wstring w;
                if(u>=0xd800&&u<=0xdbff){if(take()!='\\'||take()!='u')invalid();const auto lo=hex();if(lo<0xdc00||lo>0xdfff)invalid();w+=static_cast<wchar_t>(u);w+=static_cast<wchar_t>(lo);}
                else{if(u>=0xdc00&&u<=0xdfff)invalid();w+=static_cast<wchar_t>(u);}value+=utf8(w);break;}
            default:invalid();}
        }return value;
    }
    Json value(unsigned depth){
        if(depth>16||++nodes>4096)invalid();space();Json out;char c=take();
        if(c=='"'){--pos;out.kind=Json::String;out.text=string();return out;}
        if(c=='{'||c=='['){out.kind=c=='{'?Json::Object:Json::Array;space();const char end=c=='{'?'}':']';if(pos<input.size()&&input[pos]==end){++pos;return out;}
            while(true){space();if(c=='{'){std::string key=string();space();if(take()!=':')invalid();if(!out.object.emplace(std::move(key),value(depth+1)).second)invalid();}
                else out.array.push_back(value(depth+1));space();char next=take();if(next==end)break;if(next!=',')invalid();}return out;}
        if(c=='t'||c=='f'||c=='n'){const std::string_view token=c=='t'?"true":c=='f'?"false":"null";--pos;if(input.substr(pos,token.size())!=token)invalid();pos+=token.size();out.kind=c=='n'?Json::Null:Json::Boolean;out.flag=c=='t';return out;}
        --pos;const size_t begin=pos;
        if(input[pos]=='-')++pos;if(pos==input.size())invalid();
        if(input[pos]=='0')++pos;else{if(input[pos]<'1'||input[pos]>'9')invalid();while(pos<input.size()&&input[pos]>='0'&&input[pos]<='9')++pos;}
        if(pos<input.size()&&input[pos]=='.'){++pos;const auto start=pos;while(pos<input.size()&&input[pos]>='0'&&input[pos]<='9')++pos;if(pos==start)invalid();}
        if(pos<input.size()&&(input[pos]=='e'||input[pos]=='E')){++pos;if(pos<input.size()&&(input[pos]=='+'||input[pos]=='-'))++pos;const auto start=pos;while(pos<input.size()&&input[pos]>='0'&&input[pos]<='9')++pos;if(pos==start)invalid();}
        out.kind=Json::Number;out.text=std::string(input.substr(begin,pos-begin));return out;
    }
public:
    explicit Parser(std::string_view source):input(source){}
    Json parse(){if(input.size()>statusLimit)invalid();(void)wide(input);if(input.starts_with("\xef\xbb\xbf"))pos=3;auto result=value(0);space();if(pos!=input.size())invalid();return result;}
};
bool hexString(std::string_view s,size_t size){if(s.size()!=size)return false;return std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');});}
std::string lower(std::string s){for(char& c:s)if(c>='A'&&c<='Z')c+=32;return s;}
std::wstring quoteArg(std::wstring_view value){
    std::wstring out=L"\"";size_t slashes=0;
    for(wchar_t c:value){if(c==L'\\'){++slashes;continue;}if(c==L'"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}
    out.append(slashes*2,L'\\');out+=L'"';return out;
}
std::wstring commandLine(const std::vector<std::wstring>& args){std::wstring result;for(const auto& arg:args){if(!result.empty())result+=L' ';result+=quoteArg(arg);}return result;}
std::wstring knownFolder(REFKNOWNFOLDERID id){PWSTR p=nullptr;if(FAILED(SHGetKnownFolderPath(id,KF_FLAG_DEFAULT,nullptr,&p)))throw std::runtime_error("Cannot locate Windows known folder");std::wstring value(p);CoTaskMemFree(p);return value;}
std::wstring newSession(){GUID id{};if(FAILED(CoCreateGuid(&id)))throw std::runtime_error("Cannot generate session GUID");wchar_t text[40]{};if(!StringFromGUID2(id,text,40))throw std::runtime_error("Cannot encode session GUID");std::wstring result;for(wchar_t c:text)if(iswxdigit(c))result+=static_cast<wchar_t>(towlower(c));if(!hexString(utf8(result),32))throw std::runtime_error("Invalid session GUID");return result;}
void noReparse(const std::wstring& path){
    auto part=std::filesystem::path(path);
    while(!part.empty()){const auto attrs=GetFileAttributesW(part.c_str());if(attrs==INVALID_FILE_ATTRIBUTES)throw failure("Missing protected path");if(attrs&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Reparse point in protected path");const auto parent=part.parent_path();if(parent==part)break;part=parent;}
}
bool trustedSid(PSID sid){return IsWellKnownSid(sid,WinBuiltinAdministratorsSid)||IsWellKnownSid(sid,WinLocalSystemSid);}
void protectedDescriptor(PSECURITY_DESCRIPTOR descriptor){
    PSID owner=nullptr;BOOL defaulted=FALSE,present=FALSE;PACL acl=nullptr;SECURITY_DESCRIPTOR_CONTROL control{};DWORD revision{};
    if(!GetSecurityDescriptorOwner(descriptor,&owner,&defaulted)||!owner||!IsWellKnownSid(owner,WinBuiltinAdministratorsSid)||
       !GetSecurityDescriptorControl(descriptor,&control,&revision)||(control&SE_DACL_PROTECTED)==0||
       !GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted)||!present||!acl)throw std::runtime_error("Installed path lacks protected Administrator ownership/DACL");
    constexpr DWORD writes=GENERIC_WRITE|GENERIC_ALL|WRITE_DAC|WRITE_OWNER|DELETE|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES|FILE_DELETE_CHILD;
    for(DWORD i=0;i<acl->AceCount;++i){void* raw=nullptr;if(!GetAce(acl,i,&raw))throw failure("Read ACL");auto* header=static_cast<ACE_HEADER*>(raw);
        if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;
        if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE)throw std::runtime_error("Unsupported protected-path ACL entry");
        auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);if((ace->Mask&writes)&&!trustedSid(&ace->SidStart))throw std::runtime_error("Installed path is writable by an untrusted principal");
    }
}
Handle lockProtected(const std::wstring& path,bool directory=false){
    noReparse(path);Handle file(CreateFileW(path.c_str(),READ_CONTROL|(directory?0:GENERIC_READ),FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:FILE_ATTRIBUTE_NORMAL),nullptr));
    if(!file)throw failure("Open protected installed file");
    BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file.h,&info))throw failure("Read protected file identity");if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Protected handle points to a reparse point");
    PSECURITY_DESCRIPTOR descriptor=nullptr;const auto status=GetSecurityInfo(file.h,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,nullptr,nullptr,nullptr,nullptr,&descriptor);
    if(status!=ERROR_SUCCESS)throw std::runtime_error("Cannot inspect protected file ACL: "+std::to_string(status));
    try{protectedDescriptor(descriptor);}catch(...){LocalFree(descriptor);throw;}LocalFree(descriptor);return file;
}
std::string readHandle(HANDLE file,size_t limit){
    LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size)||size.QuadPart<0||static_cast<uint64_t>(size.QuadPart)>limit)throw std::runtime_error("File exceeds read limit");
    LARGE_INTEGER zero{};if(!SetFilePointerEx(file,zero,nullptr,FILE_BEGIN))throw failure("Seek file");
    std::string bytes(static_cast<size_t>(size.QuadPart),'\0');DWORD read=0;if(!bytes.empty()&&(!ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=bytes.size()))throw failure("Read complete file");return bytes;
}
std::string sha256(std::string_view bytes){
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::array<UCHAR,32> digest{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("Cannot open SHA256 provider");
    const auto status=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
    const auto hashed=status>=0?BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),static_cast<ULONG>(bytes.size()),0):status;
    const auto finished=hashed>=0?BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0):hashed;
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    if(finished<0)throw std::runtime_error("SHA256 failed");std::string result;constexpr char hex[]="0123456789abcdef";for(auto b:digest){result+=hex[b>>4];result+=hex[b&15];}return result;
}
std::string receiptScriptHash(const Json& receipt,const std::wstring& installRoot,bool requireSingleChainPolicy=false){
    if(receipt.at("productId").str()!="endfield-xess-vulkan-observer-v1"||receipt.at("schemaVersion").uint32()!=1||
       _wcsicmp(wide(receipt.at("installRoot").str()).c_str(),installRoot.c_str())!=0)throw std::runtime_error("Installation receipt identity mismatch");
    if(requireSingleChainPolicy&&receipt.at("singleChainPolicyVersion").uint32()!=1)
        throw std::runtime_error("Install the complete single-chain LocalFiOff package before using this XeFG route");
    const auto& mod=receipt.at("modLauncher");
    const auto efuHash=mod.at("efuSha256").str(),efuCommit=mod.at("efuSourceCommit").str();
    const bool legacyEfu=efuHash=="a686b5b7704341e7bfb016794df074a39f6195b8d80a4a99ca4f8d8de9eca5ae"&&efuCommit=="1165a377f7915528b1ae9d2c3e41631bfd59b957";
    const bool previousPreviewEfu=efuHash=="fe27f71582726fb21cb51076a9ed3e16c6c6eae32588364daff3c5a8078d24f9"&&efuCommit==currentEfuCommit;
    const bool previousDitherEfu=efuHash=="2ef1cd8066c62ebe42b41d098dfe8c36a559e5845626f7e6d98ddd78f4dd21fa"&&efuCommit==currentEfuCommit;
    const bool currentEfu=hexString(currentEfuSha256,64)&&efuHash==currentEfuSha256&&efuCommit==currentEfuCommit;
    if(mod.at("version").uint32()!=1||mod.at("efuVersion").str()!="5.1.0"||(!legacyEfu&&!previousPreviewEfu&&!previousDitherEfu&&!currentEfu))throw std::runtime_error("Install a reviewed standalone launcher package first");
    const auto& files=receipt.at("files");if(files.kind!=Json::Array||files.array.size()>32)throw std::runtime_error("Malformed receipt file list");
    std::string result;unsigned matches=0;for(const auto& entry:files.array)if(entry.at("name").str()=="manage-observer-install.ps1"){result=lower(entry.at("sha256").str());++matches;}
    if(matches!=1||!hexString(result,64)||lower(receipt.at("sourceScriptSha256").str())!=result)throw std::runtime_error("Receipt helper hash mismatch");return result;
}
enum class LauncherMode { Gui, Official, ObserveInputs, CaptureRawColorOnce, PresentationOff, XeFG, TraceRenderTail, TraceAcquire, FakeSwapchain, LowResOutput, FullResOutput, LegacyChain, LegacyChainFullRes, SelfTest, Readonly };
LauncherMode launcherMode(const std::vector<std::wstring>& options){
    if(options.empty())return LauncherMode::Gui;
    const auto legacy=std::count(options.begin(),options.end(),L"--legacy-chain");
    if(legacy){
        if(legacy!=1||options.size()>3)throw std::runtime_error("Invalid legacy-chain modifiers");
        auto base=options;base.erase(std::find(base.begin(),base.end(),L"--legacy-chain"));
        const auto mode=launcherMode(base);
        if(mode==LauncherMode::XeFG||mode==LauncherMode::LowResOutput)return LauncherMode::LegacyChain;
        if(mode==LauncherMode::FullResOutput)return LauncherMode::LegacyChainFullRes;
        throw std::runtime_error("Legacy chain requires --xefg and permits only an output-resolution modifier");
    }
    if(options.size()==2&&((options[0]==L"--xefg"&&options[1]==L"--trace-render-tail")||
        (options[1]==L"--xefg"&&options[0]==L"--trace-render-tail")))return LauncherMode::TraceRenderTail;
    if(options.size()==2&&((options[0]==L"--xefg"&&options[1]==L"--trace-acquire")||
        (options[1]==L"--xefg"&&options[0]==L"--trace-acquire")))return LauncherMode::TraceAcquire;
    if(options.size()==2&&((options[0]==L"--xefg"&&options[1]==L"--fake-swapchain")||
        (options[1]==L"--xefg"&&options[0]==L"--fake-swapchain")))return LauncherMode::FakeSwapchain;
    if(options.size()==2&&((options[0]==L"--xefg"&&options[1]==L"--low-res-output")||
        (options[1]==L"--xefg"&&options[0]==L"--low-res-output")))return LauncherMode::LowResOutput;
    if(options.size()==2&&((options[0]==L"--xefg"&&options[1]==L"--full-res-output")||
        (options[1]==L"--xefg"&&options[0]==L"--full-res-output")))return LauncherMode::FullResOutput;
    if(options.size()==1){
        if(options[0]==L"--capture-rawcolor-once")return LauncherMode::CaptureRawColorOnce;
        if(options[0]==L"--presentation-off")return LauncherMode::PresentationOff;
        if(options[0]==L"--xefg")return LauncherMode::XeFG;
        if(options[0]==L"--official")return LauncherMode::Official;
        if(options[0]==L"--observe-inputs")return LauncherMode::ObserveInputs;
        if(options[0]==L"--self-test")return LauncherMode::SelfTest;
        if(options[0]==L"--check-helper-readonly")return LauncherMode::Readonly;
    }
    throw std::runtime_error("Accepts one launch mode; --xefg permits one of --trace-render-tail, --trace-acquire, --fake-swapchain, --low-res-output or --full-res-output; --legacy-chain permits either output-resolution modifier. No external paths, values or commands");
}
// Low-resolution output is the default XeFG route; --full-res-output keeps the
// 2880-wide baseline, and render-tail diagnostics stay full resolution.
bool xeFGRoute(LauncherMode mode){
    return mode==LauncherMode::XeFG||mode==LauncherMode::TraceRenderTail||mode==LauncherMode::TraceAcquire||mode==LauncherMode::FakeSwapchain||mode==LauncherMode::LowResOutput||mode==LauncherMode::FullResOutput||mode==LauncherMode::LegacyChain||mode==LauncherMode::LegacyChainFullRes;
}
bool lowResOutputRoute(LauncherMode mode){return mode==LauncherMode::XeFG||mode==LauncherMode::LowResOutput||mode==LauncherMode::TraceAcquire||mode==LauncherMode::FakeSwapchain||mode==LauncherMode::LegacyChain;}
// Trace modes keep their existing driver swapchain; all regular XeFG output modes use one chain.
bool fakeSwapchainRoute(LauncherMode mode){return mode==LauncherMode::XeFG||mode==LauncherMode::LowResOutput||mode==LauncherMode::FullResOutput||mode==LauncherMode::FakeSwapchain;}
bool gpuTimestampsFromEnvironment(){wchar_t value[2]{};return GetEnvironmentVariableW(L"XESS_GPU_TIMESTAMPS",value,2)==1&&value[0]==L'1';}
bool threadCpuSamplingFromEnvironment(){wchar_t value[2]{};return GetEnvironmentVariableW(L"XESS_THREAD_CPU_SAMPLING",value,2)==1&&value[0]==L'1';}
bool trackingCpuTimingFromEnvironment(){wchar_t value[2]{};return GetEnvironmentVariableW(L"XESS_TRACKING_CPU_TIMING",value,2)==1&&value[0]==L'1';}
std::vector<std::wstring> helperArgs(const std::wstring& powershell,const std::wstring& script,const std::wstring& session,bool captureRawColorOnce=false,bool presentationOff=false,bool xeFG=false,bool official=false,bool observeInputs=false,bool traceRenderTail=false,bool lowResOutput=false,bool traceAcquire=false,bool fakeSwapchain=false,bool gpuTimestamps=false,bool trackingCpuTiming=false,bool xessSr=false,bool disableNoDither=false,bool threadCpuSampling=false,unsigned fgDebugMode=0,int fgSceneIndex=-1,bool frameCapEnabled=false,unsigned frameCapTarget=40,bool diagnosticMode=false){
    if(!hexString(utf8(session),32))throw std::runtime_error("Invalid session ID");
    if(disableNoDither&&official)throw std::runtime_error("Official launch does not load the no-dither mod");
    if(xessSr&&(!xeFG||!fakeSwapchain))throw std::runtime_error("XeSS SR requires the single-chain XeFG route");
    if(gpuTimestamps&&!xeFG)throw std::runtime_error("GPU timestamps require the explicit XeFG route");
    if(threadCpuSampling&&!xeFG)throw std::runtime_error("Thread CPU sampling requires the explicit XeFG route");
    if(trackingCpuTiming&&!xeFG)throw std::runtime_error("Tracking CPU timing requires the explicit XeFG route");
    if(traceRenderTail&&!xeFG)throw std::runtime_error("Render-tail diagnostics require the explicit XeFG route");
    if(traceAcquire&&(!xeFG||traceRenderTail))throw std::runtime_error("Acquire observation requires XeFG without render-tail diagnostics");
    if(fakeSwapchain&&(!xeFG||traceRenderTail||traceAcquire))throw std::runtime_error("Fake swapchain requires XeFG without trace diagnostics");
    if(lowResOutput&&(!xeFG||traceRenderTail))throw std::runtime_error("Low-resolution output requires XeFG without render-tail diagnostics");
    if(observeInputs&&(captureRawColorOnce||presentationOff||xeFG||official))throw std::runtime_error("Input observation cannot combine with other launch modes");
    if(xeFG&&presentationOff)throw std::runtime_error("Select the continuous OFF route without other experimental switches");
    if(captureRawColorOnce&&(presentationOff||xeFG))throw std::runtime_error("XeFG/presentation and rawcolor capture cannot be combined");
    if(official&&(captureRawColorOnce||presentationOff||xeFG))throw std::runtime_error("Official fallback cannot combine with experimental modes");
    std::vector<std::wstring> result{powershell,L"-NoProfile",L"-NonInteractive",L"-ExecutionPolicy",L"Bypass",L"-File",script,L"-Action",L"LaunchMod"};
    // Legacy capture/presentation switches retain their record-only behavior.
    if(captureRawColorOnce||presentationOff)result.push_back(L"-BridgeDevice");
    if(presentationOff)result.push_back(L"-PresentationOff");
    if(captureRawColorOnce){result.push_back(L"-TraceFsr");result.push_back(L"-TraceFsrUpscale");}
    if(xeFG)result.push_back(L"-XeFG");
    if constexpr(releaseMinimal)if(xeFG&&fakeSwapchain){
        result.push_back(L"-ReleaseMinimal");
        if(diagnosticMode)result.push_back(L"-DiagnosticMode");
    }
    if(xessSr)result.push_back(L"-XessSr");
    if(disableNoDither)result.push_back(L"-DisableNoDither");
    if(traceRenderTail)result.push_back(L"-TraceRenderTail");
    if(traceAcquire)result.push_back(L"-WsiAcquireTrace");
    if(fakeSwapchain)result.push_back(L"-FakeSwapchain");
    if(gpuTimestamps)result.push_back(L"-GpuTimestamps");
    if(trackingCpuTiming)result.push_back(L"-TrackingCpuTiming");
    if(threadCpuSampling)result.push_back(L"-ThreadCpuSampling");
    if(fgDebugMode){if(fgDebugMode>3||!xeFG||official)throw std::runtime_error("Invalid FG debug request");result.push_back(L"-FgDebugMode");result.push_back(std::to_wstring(fgDebugMode));}
    if(frameCapTarget<30||frameCapTarget>120)throw std::runtime_error("Frame cap target must be 30..120");
    if(frameCapEnabled){if(!xeFG||official)throw std::runtime_error("Frame cap mapping requires independent XeFG");result.push_back(L"-Override30Fps");result.push_back(L"-FrameCapTargetFps");result.push_back(std::to_wstring(frameCapTarget));}
    if(fgSceneIndex>=0){if(fgSceneIndex>6||!xeFG)throw std::runtime_error("Invalid scene threshold selection");result.push_back(L"-FgSceneIndex");result.push_back(std::to_wstring(fgSceneIndex));}
    if(lowResOutput)result.push_back(L"-LowResOutput");
    if(official)result.push_back(L"-OfficialFallback");
    if(observeInputs)result.push_back(L"-ObserveInputs");
    result.push_back(L"-SessionId");result.push_back(session);
    if(captureRawColorOnce)result.push_back(L"-CaptureRawColor");return result;
}
std::vector<std::wstring> statusArgs(const std::wstring& powershell,const std::wstring& script){
    return{powershell,L"-NoProfile",L"-NonInteractive",L"-ExecutionPolicy",L"Bypass",L"-File",script,L"-Action",L"Status"};
}
struct EnvironmentNameLess {
    bool operator()(const std::wstring& a,const std::wstring& b)const{
        return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_LESS_THAN;
    }
};
std::vector<wchar_t> helperEnvironment(const std::vector<std::wstring>& inherited,const std::wstring& powershell){
    std::map<std::wstring,std::wstring,EnvironmentNameLess> entries;
    for(const auto& entry:inherited){
        const auto separator=entry.find(L'=',1); // Preserve Windows' hidden =C:=... entries.
        if(separator==std::wstring::npos||entry.find(L'\0')!=std::wstring::npos)throw std::runtime_error("Malformed inherited environment entry");
        entries[entry.substr(0,separator)]=entry.substr(separator+1);
    }
    // Resolve helper modules only from the verified PowerShell 7 installation.
    entries[L"PSModulePath"]=(std::filesystem::path(powershell).parent_path()/L"Modules").wstring();
    entries.erase(L"EFU_NO_DITHER");
    entries.erase(L"XESS_SR_REPLACE");
    entries.erase(L"XESS_CONTINUOUS_INPUTS");
    entries.erase(L"XESS_FSR_NATIVE_OFF");
    entries.erase(L"XESS_XEFG_LOW_RES_OUTPUT");
    entries.erase(L"XESS_WSI_ACQUIRE_OBSERVER");
    entries.erase(L"XESS_FAKE_SWAPCHAIN");
    entries.erase(L"XESS_GPU_TIMESTAMPS");
    entries.erase(L"XESS_TRACKING_CPU_TIMING");
    entries.erase(L"XESS_THREAD_CPU_SAMPLING");
    entries.erase(L"XESS_FG_DEBUG_MODE");
    entries.erase(L"XESS_FRAME_CAP_30_TO_40");
    entries.erase(L"XESS_FRAME_CAP_OVERRIDE");entries.erase(L"XESS_FRAME_CAP_TARGET_FPS");
    entries.erase(L"XESS_FG_SCENE_THRESHOLD");
    std::vector<wchar_t> block;
    for(const auto& [name,value]:entries){
        if(block.size()+name.size()+value.size()+3>1024*1024)throw std::runtime_error("Inherited environment exceeds bounded size");
        block.insert(block.end(),name.begin(),name.end());block.push_back(L'=');
        block.insert(block.end(),value.begin(),value.end());block.push_back(L'\0');
    }
    block.push_back(L'\0');return block;
}
std::vector<wchar_t> helperEnvironment(const std::wstring& powershell){
    auto* raw=GetEnvironmentStringsW();if(!raw)throw failure("Read child environment");
    std::vector<std::wstring> entries;
    try{size_t total=0;for(const wchar_t* entry=raw;*entry;entry+=wcslen(entry)+1){
        const size_t length=wcslen(entry);total+=length+1;if(total>1024*1024)throw std::runtime_error("Inherited environment exceeds bounded size");
        entries.emplace_back(entry,length);
    }}catch(...){FreeEnvironmentStringsW(raw);throw;}
    FreeEnvironmentStringsW(raw);return helperEnvironment(entries,powershell);
}
std::wstring powerShell7Path(){return knownFolder(FOLDERID_ProgramFiles)+L"\\PowerShell\\7\\pwsh.exe";}
Handle lockPowerShell7(const std::wstring& path){
    if(_wcsicmp(path.c_str(),powerShell7Path().c_str())!=0)throw std::runtime_error("Only installed Program Files PowerShell 7 is supported");
    noReparse(path);Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file)throw failure("Open installed PowerShell 7; Windows PowerShell fallback is not supported");
    DWORD ignored=0;const auto bytes=GetFileVersionInfoSizeW(path.c_str(),&ignored);
    std::vector<uint8_t> version(bytes);VS_FIXEDFILEINFO* fixed=nullptr;UINT fixedBytes=0;
    if(!bytes||!GetFileVersionInfoW(path.c_str(),0,bytes,version.data())||
       !VerQueryValueW(version.data(),L"\\",reinterpret_cast<void**>(&fixed),&fixedBytes)||
       fixedBytes<sizeof(*fixed)||fixed->dwSignature!=0xfeef04bd||HIWORD(fixed->dwFileVersionMS)!=7)
        throw std::runtime_error("The trusted pwsh.exe must be PowerShell 7");
    WINTRUST_FILE_INFO info{};info.cbStruct=sizeof(info);info.pcwszFilePath=path.c_str();info.hFile=file.h;
    WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;trust.fdwRevocationChecks=WTD_REVOKE_NONE;
    trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&info;trust.dwStateAction=WTD_STATEACTION_VERIFY;
    trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto verified=WinVerifyTrust(nullptr,&policy,&trust);bool microsoft=false;
    if(verified==ERROR_SUCCESS){
        auto* provider=WTHelperProvDataFromStateData(trust.hWVTStateData);
        auto* signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;wchar_t subject[256]{};
        microsoft=signer&&signer->csCertChain&&CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,subject,256)>1&&
            std::wcscmp(subject,L"Microsoft Corporation")==0;
    }
    trust.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&policy,&trust);
    if(verified!=ERROR_SUCCESS||!microsoft)throw std::runtime_error("PowerShell 7 requires a valid Microsoft Authenticode signature");
    return file;
}
struct TrustedHelper {
    Handle runtimeLock,directoryLock,receiptLock,scriptLock,process,pipe;
    void releaseLocks(){scriptLock.reset();receiptLock.reset();directoryLock.reset();runtimeLock.reset();}
};
TrustedHelper launchTrustedHelper(const std::wstring& installRoot,const std::wstring& powershell,bool readonly,const std::wstring& session={},bool captureRawColorOnce=false,bool presentationOff=false,bool xeFG=false,bool official=false,bool observeInputs=false,bool traceRenderTail=false,bool lowResOutput=false,bool traceAcquire=false,bool fakeSwapchain=false,bool gpuTimestamps=false,bool trackingCpuTiming=false,bool xessSr=false,bool disableNoDither=false,bool threadCpuSampling=false,unsigned fgDebugMode=0,const std::wstring& settingPath={},int fgSceneIndex=-1,bool frameCapEnabled=false,unsigned frameCapTarget=40,bool diagnosticMode=false){
    if(readonly&&(captureRawColorOnce||presentationOff||xeFG||official||observeInputs||traceRenderTail||lowResOutput||traceAcquire||fakeSwapchain||gpuTimestamps||trackingCpuTiming||threadCpuSampling||fgDebugMode||fgSceneIndex>=0||frameCapEnabled||!settingPath.empty()))throw std::runtime_error("Readonly helper cannot request a launch mode");
    TrustedHelper result;
    result.runtimeLock=lockPowerShell7(powershell);
    result.directoryLock=lockProtected(installRoot,true);
    result.receiptLock=lockProtected(installRoot+L"\\install-receipt.json");
    const auto expected=receiptScriptHash(Parser(readHandle(result.receiptLock.h,receiptLimit)).parse(),installRoot,fakeSwapchain);
    const auto script=installRoot+L"\\manage-observer-install.ps1";result.scriptLock=lockProtected(script);
    if(sha256(readHandle(result.scriptLock.h,scriptLimit))!=expected)throw std::runtime_error("Installed helper does not match protected receipt");
    auto environment=helperEnvironment(powershell);
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE read=nullptr,write=nullptr;
    if(!CreatePipe(&read,&write,&security,0))throw failure("Create helper output pipe");result.pipe=Handle(read);Handle pipeWriter(write);
    if(!SetHandleInformation(result.pipe.h,HANDLE_FLAG_INHERIT,0))throw failure("Protect helper read handle");
    Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));if(!input)throw failure("Open null input");
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;
    startup.StartupInfo.hStdInput=input.h;startup.StartupInfo.hStdOutput=pipeWriter.h;startup.StartupInfo.hStdError=pipeWriter.h;
    SIZE_T attributeBytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&attributeBytes);std::vector<uint8_t> attributes(attributeBytes);startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attributeBytes))throw failure("Create inherited-handle list");
    HANDLE inherited[]{input.h,pipeWriter.h};BOOL updated=UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr);
    auto arguments=readonly?statusArgs(powershell,script):settingPath.empty()?
        helperArgs(powershell,script,session,captureRawColorOnce,presentationOff,xeFG,official,observeInputs,traceRenderTail,lowResOutput,traceAcquire,fakeSwapchain,gpuTimestamps,trackingCpuTiming,xessSr,disableNoDither,threadCpuSampling,fgDebugMode,fgSceneIndex,frameCapEnabled,frameCapTarget,diagnosticMode):
        std::vector<std::wstring>{powershell,L"-NoProfile",L"-NonInteractive",L"-ExecutionPolicy",L"Bypass",L"-File",script,L"-Action",L"SetGamePath",L"-GameDirectory",settingPath};
    auto command=commandLine(arguments);PROCESS_INFORMATION child{};
    const BOOL created=updated&&CreateProcessW(powershell.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,environment.data(),installRoot.c_str(),&startup.StartupInfo,&child);
    const DWORD createError=GetLastError();DeleteProcThreadAttributeList(startup.lpAttributeList);
    if(!created){SetLastError(createError);throw failure("Start trusted helper");}
    CloseHandle(child.hThread);result.process=Handle(child.hProcess);return result;
}
void drainHelperOutput(Handle& pipe,std::string& output){
    if(!pipe)return;
    for(unsigned chunk=0;chunk<32;++chunk){DWORD available=0;if(!PeekNamedPipe(pipe.h,nullptr,0,nullptr,&available,nullptr)||!available)break;
        char buffer[2048];DWORD read=0;if(!ReadFile(pipe.h,buffer,std::min<DWORD>(available,sizeof(buffer)),&read,nullptr)||!read)break;
        const size_t keep=std::min<size_t>(read,outputLimit-output.size());output.append(buffer,keep);
    }
}
std::wstring boundedDiagnostic(std::string_view bytes,size_t limit){
    auto text=diagnosticText(bytes);if(text.size()>limit){text.resize(limit);text+=L"\r\n[诊断输出已截断]";}return text;
}
std::wstring systemPowerShell(){
    const auto path=powerShell7Path();auto lock=lockPowerShell7(path);return path;
}
const char* optionalText(const Json& record,const char* key,const char* fallback){
    const auto i=record.object.find(key);return i!=record.object.end()&&i->second.kind==Json::String?i->second.text.c_str():fallback;
}
std::wstring gameCompatibilityText(std::string_view state){
    if(state=="unreviewed_game_build"||state=="unreviewed_size")
        return L"此游戏版本尚未适配，独立启动已禁用。若游戏刚更新，请等待兼容版本；也可自行使用官方启动器。请勿修改校验值绕过检查。";
    if(state=="unverified_publisher")
        return L"游戏签名未通过校验，独立启动已禁用。请核对游戏来源并检查文件完整性。";
    return L"游戏文件缺失或无法校验。请点击“更改…”选择包含 Endfield.exe 的游戏目录，并检查文件完整性。";
}
unsigned monitorRefresh(HWND window) noexcept {
    MONITORINFOEXW monitor{};monitor.cbSize=sizeof(monitor);DEVMODEW mode{};mode.dmSize=sizeof(mode);
    return GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTOPRIMARY),&monitor)&&EnumDisplaySettingsW(monitor.szDevice,ENUM_CURRENT_SETTINGS,&mode)&&mode.dmDisplayFrequency>1?mode.dmDisplayFrequency:0;
}
struct App {
    xess_vk::launcher_ui::Theme theme;
    UINT activationMessage{};
    HWND window{},status{},start{},multiplier{},noDitherControl{},pathControl{},pathStatus{},sceneControl{},debugControl{},debugWarning{},frameCapControl{},frameCapTargetControl{},diagnosticControl{};
    unsigned frameCapTarget=40;
    bool pathValid{},advanced{};unsigned pathJob{};std::wstring configuredGameDirectory,preferenceWarning;HFONT font{};bool tray=false,busy=false,captureRawColorOnce=false,presentationOff=false,xeFG=false,official=false,observeInputs=false,traceRenderTail=false,lowResOutput=false,traceAcquire=false,fakeSwapchain=false,gpuTimestamps=false,trackingCpuTiming=false,xessSr=false,threadCpuSampling=false;UINT taskbarCreated{};
    std::wstring installRoot,sessionRoot,session,sessionDirectory,powershell;std::string helperOutput;
    TrustedHelper helper;
    bool noDither=true,diagnosticActive=false;
    void setStatus(const std::wstring& text){
        std::wstring message=text;
        if constexpr(releaseMinimal){
            if(diagnosticActive)message=L"诊断模式已开启（本局记录日志，下次默认关闭）\r\n"+message;
            else if(diagnosticControl&&SendMessageW(diagnosticControl,BM_GETCHECK,0,0)==BST_CHECKED)message=L"诊断模式已开启（仅下一次启动记录日志）\r\n"+message;
            const auto selected=unsigned(SendMessageW(multiplier,CB_GETCURSEL,0,0))+2;
            const auto warning=xess_vk::refresh_warning::text(monitorRefresh(window),selected,SendMessageW(frameCapControl,BM_GETCHECK,0,0)==BST_CHECKED,frameCapTarget);
            if(!warning.empty())message+=L"\r\n"+warning;
        }
        SetWindowTextW(status,(message+(preferenceWarning.empty()?L"":L"\r\n"+preferenceWarning)).c_str());
    }
    void show(){ShowWindow(window,IsIconic(window)?SW_RESTORE:SW_SHOW);SetForegroundWindow(window);}
    void trayAdd(){if(tray)return;NOTIFYICONDATAW data{};data.cbSize=sizeof(data);data.hWnd=window;data.uID=1;data.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;data.uCallbackMessage=trayMessage;data.hIcon=theme.smallIcon;wcscpy_s(data.szTip,L"终末地 · XeSS\n双击显示启动器");tray=Shell_NotifyIconW(NIM_ADD,&data)!=FALSE;}
    void hide(){trayAdd();if(tray)ShowWindow(window,SW_HIDE);else ShowWindow(window,SW_MINIMIZE);}
    void beginPathCheck(){
        if(busy)return;
        try{helperOutput.clear();helper=launchTrustedHelper(installRoot,powershell,true);pathJob=1;busy=true;SetTimer(window,timerId,500,nullptr);EnableWindow(start,FALSE);SetWindowTextW(pathStatus,L"正在只读校验路径与游戏版本…");}
        catch(const std::exception& e){pathValid=false;EnableWindow(start,FALSE);setStatus(L"路径校验无法启动："+wide(e.what()));}
    }
    void savePath(const std::wstring& directory){
        if(busy)return;
        try{helperOutput.clear();helper=launchTrustedHelper(installRoot,powershell,false,{},false,false,false,false,false,false,false,false,false,false,false,false,false,false,0,directory);
            pathJob=2;busy=true;SetTimer(window,timerId,500,nullptr);EnableWindow(start,FALSE);setStatus(L"请在系统授权中确认保存游戏路径；只保存通过签名和固定哈希校验的目录。");}
        catch(const std::exception& e){setStatus(L"路径未保存："+wide(e.what()));}
    }
    void choosePath(){
        if(busy)return;wchar_t file[32768]{};
        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;dialog.lpstrFile=file;dialog.nMaxFile=std::size(file);
        dialog.lpstrFilter=L"终末地主程序 (Endfield.exe)\0Endfield.exe\0";dialog.lpstrTitle=L"选择游戏目录中的 Endfield.exe";
        dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
        if(GetOpenFileNameW(&dialog)){
            const auto selected=std::filesystem::path(file);
            if(_wcsicmp(selected.filename().c_str(),L"Endfield.exe")){setStatus(L"请选择 Endfield.exe，不能选择其他程序。");return;}
            savePath(selected.parent_path().wstring());
        }
    }
    void detectPath(){
        if(busy)return;
        std::vector<std::wstring> candidates;
        const auto add=[&](const std::wstring& root){if(root.size()<3||root[1]!=L':'||(root[2]!=L'\\'&&root[2]!=L'/'))return;
            for(const auto suffix:{L"",L"games\\Arknights Endfield"}){
                const auto directory=(std::filesystem::path(root)/suffix).lexically_normal().wstring();
                const auto file=directory+L"\\Endfield.exe";
                try{noReparse(file);if(GetFileAttributesW(file.c_str())!=INVALID_FILE_ATTRIBUTES&&
                    std::find(candidates.begin(),candidates.end(),directory)==candidates.end())candidates.push_back(directory);}catch(...){}
            }};
        add(knownFolder(FOLDERID_ProgramFiles)+L"\\Hypergryph Launcher");
        for(const auto hive:{HKEY_LOCAL_MACHINE,HKEY_CURRENT_USER})for(const auto view:{KEY_WOW64_64KEY,KEY_WOW64_32KEY}){
            HKEY key{};if(RegOpenKeyExW(hive,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",0,KEY_READ|view,&key)!=ERROR_SUCCESS)continue;
            for(DWORD index=0;index<10000;++index){wchar_t sub[256]{};DWORD size=std::size(sub);
                const auto result=RegEnumKeyExW(key,index,sub,&size,nullptr,nullptr,nullptr,nullptr);if(result==ERROR_NO_MORE_ITEMS)break;if(result!=ERROR_SUCCESS)continue;
                HKEY entry{};if(RegOpenKeyExW(key,sub,0,KEY_READ,&entry)!=ERROR_SUCCESS)continue;
                wchar_t name[1024]{},location[32768]{};DWORD type{},bytes=sizeof(name);
                if(RegQueryValueExW(entry,L"DisplayName",nullptr,&type,reinterpret_cast<BYTE*>(name),&bytes)==ERROR_SUCCESS&&type==REG_SZ){
                    name[std::size(name)-1]=0;std::wstring lower=name;std::transform(lower.begin(),lower.end(),lower.begin(),towlower);
                    if(lower.find(L"hypergryph")!=lower.npos||lower.find(L"endfield")!=lower.npos||lower.find(L"终末地")!=lower.npos||lower.find(L"鹰角")!=lower.npos){
                        bytes=sizeof(location);if(RegQueryValueExW(entry,L"InstallLocation",nullptr,&type,reinterpret_cast<BYTE*>(location),&bytes)==ERROR_SUCCESS&&type==REG_SZ){location[std::size(location)-1]=0;add(location);}
                    }}RegCloseKey(entry);
            }RegCloseKey(key);
        }
        for(const auto& candidate:candidates){const auto choice=MessageBoxW(window,(L"检测到游戏目录：\r\n"+candidate+L"\r\n\r\n是否使用？保存时需要系统授权，并重新校验版本。").c_str(),L"确认游戏路径",MB_YESNOCANCEL|MB_ICONQUESTION);
            if(choice==IDYES){savePath(candidate);return;}if(choice==IDCANCEL)return;}
        setStatus(L"没有选定可用的检测结果。请点击“更改…”选择 Endfield.exe。");
    }
    void pollPath(){
        if(WaitForSingleObject(helper.process.h,0)!=WAIT_OBJECT_0)return;
        DWORD code{};GetExitCodeProcess(helper.process.h,&code);drainOutput();helper.releaseLocks();helper.process.reset();helper.pipe.reset();
        const auto job=pathJob;pathJob=0;busy=false;KillTimer(window,timerId);
        if(code){EnableWindow(start,pathValid);setStatus(L"路径操作未完成："+boundedDiagnostic(helperOutput,500));return;}
        if(job==2){beginPathCheck();return;}
        try{const auto record=Parser(utf8(decodeHelperText(helperOutput))).parse();
            configuredGameDirectory=wide(record.at("configuredGameDirectory").str());
            const bool configured=record.at("gamePathConfigured").boolean();
            pathValid=configured&&(official||record.at("gameCompatibility").at("efuLaunchCompatible").boolean());
            SetWindowTextW(pathControl,configuredGameDirectory.c_str());
            SetWindowTextW(pathStatus,!configured?L"未设置游戏路径":pathValid?L"已验证；启动时仍会重新核对游戏版本":L"文件缺失或版本未验证；当前不能独立启动");
            EnableWindow(start,pathValid);if(!configured)PostMessageW(window,detectPathMessage,0,0);
            else if(!pathValid)setStatus(gameCompatibilityText(optionalText(record.at("gameCompatibility"),"state","unavailable")));
            else setStatus(L"路径已验证。设置保存后仅下次启动游戏生效。");
        }catch(const std::exception& e){pathValid=false;EnableWindow(start,FALSE);SetWindowTextW(pathStatus,L"路径状态不可用；启动按钮已禁用");setStatus(L"路径状态不可用："+wide(e.what()));}
    }
    void toggleAdvanced(){advanced=!advanced;ShowWindow(debugControl,advanced?SW_SHOW:SW_HIDE);ShowWindow(debugWarning,advanced?SW_SHOW:SW_HIDE);}
    void restoreDefaults(){
        SendMessageW(diagnosticControl,BM_SETCHECK,BST_UNCHECKED,0);
        SendMessageW(multiplier,CB_SETCURSEL,0,0);SendMessageW(sceneControl,CB_SETCURSEL,0,0);SendMessageW(debugControl,CB_SETCURSEL,0,0);
        SendMessageW(noDitherControl,BM_SETCHECK,BST_CHECKED,0);SendMessageW(frameCapControl,BM_SETCHECK,BST_UNCHECKED,0);frameCapTarget=40;SetWindowTextW(frameCapTargetControl,L"40");EnableWindow(frameCapTargetControl,FALSE);saveMultiplier();
    }
    void openLogs(){
        if constexpr(releaseMinimal)if(sessionDirectory.empty()){
            MessageBoxW(window,L"默认不记录日志。需要排错时，请勾选“诊断模式”，仅下一次启动会保存日志。",L"诊断模式关闭",MB_OK|MB_ICONINFORMATION);return;
        }
        const auto folder=!sessionDirectory.empty()&&GetFileAttributesW(sessionDirectory.c_str())!=INVALID_FILE_ATTRIBUTES?sessionDirectory:sessionRoot;
        if(GetFileAttributesW(folder.c_str())==INVALID_FILE_ATTRIBUTES){MessageBoxW(window,(L"尚未生成诊断会话。\n\n"+diagnosticText(helperOutput.empty()?"No backend output captured.":helperOutput)).c_str(),L"诊断信息",MB_OK|MB_ICONINFORMATION);return;}
        if(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)MessageBoxW(window,folder.c_str(),L"日志目录",MB_OK|MB_ICONINFORMATION);
    }
    bool saveMultiplier(){
        try{
            const auto selection=SendMessageW(multiplier,CB_GETCURSEL,0,0);
            if(selection<0||selection>2)throw std::runtime_error("Invalid frame-generation multiplier");
            const bool capEnabled=SendMessageW(frameCapControl,BM_GETCHECK,0,0)==BST_CHECKED;
            wchar_t capText[17]{};GetWindowTextW(frameCapTargetControl,capText,17);
            const auto capValue=xess_vk::launcher_preferences::parse(utf8(capText),30,120,0);
            if(capEnabled&&!capValue){SetFocus(frameCapTargetControl);setStatus(L"帧数请输入30至120的整数，例如45、50；本次设置未保存，也未启动游戏。");return false;}
            if(capValue)frameCapTarget=capValue;
            xess_vk::launcher_preferences::save(xess_vk::launcher_preferences::frameCapTargetPath,frameCapTarget);
            if(!capValue)SetWindowTextW(frameCapTargetControl,std::to_wstring(frameCapTarget).c_str());
            xess_vk::launcher_preferences::save(xess_vk::launcher_preferences::multiplierPath,unsigned(selection)+2);
            const auto scene=SendMessageW(sceneControl,CB_GETCURSEL,0,0),debug=SendMessageW(debugControl,CB_GETCURSEL,0,0);
            if(scene<0||scene>6||debug<0||debug>3)throw std::runtime_error("Invalid FG setting selection");
            xess_vk::launcher_preferences::save(xess_vk::launcher_preferences::sceneThresholdPath,unsigned(scene));
            noDither=SendMessageW(noDitherControl,BM_GETCHECK,0,0)==BST_CHECKED;
            xess_vk::launcher_preferences::save(xess_vk::launcher_preferences::noDitherPath,noDither?1:0);
            xess_vk::launcher_preferences::save(xess_vk::launcher_preferences::frameCapEnabledPath,SendMessageW(frameCapControl,BM_GETCHECK,0,0)==BST_CHECKED?1:0);
            preferenceWarning.clear();setStatus(releaseMinimal?L"设置已保存，下次启动游戏生效。":L"设置已保存，仅下次启动生效。高级调试只随本次启动请求传递，启动后自动关闭选择。");
            return true;
        }catch(const std::exception& e){setStatus(L"设置未全部保存，请检查后重试："+wide(e.what()));return false;}
    }
    void frameCapChanged(){
        EnableWindow(frameCapTargetControl,xeFG&&!official&&SendMessageW(frameCapControl,BM_GETCHECK,0,0)==BST_CHECKED);
        setStatus(L"30帧档替换设置仅下次启动生效；可输入30至120的整数，游戏内60帧档不变。");
    }
    void noDitherChanged(){
        noDither=SendMessageW(noDitherControl,BM_GETCHECK,0,0)==BST_CHECKED;
        setStatus(L"已更改免虚化选择。点击“保存设置”或“启动游戏”保存；仅下次启动生效。");
    }
    void startGame(){
        if(busy)return;
        try{
            if(!pathValid){setStatus(L"请先设置并验证游戏路径。");return;}
            if(!saveMultiplier())return;
            noDither=SendMessageW(noDitherControl,BM_GETCHECK,0,0)==BST_CHECKED;
            diagnosticActive=releaseMinimal&&SendMessageW(diagnosticControl,BM_GETCHECK,0,0)==BST_CHECKED;
            SendMessageW(diagnosticControl,BM_SETCHECK,BST_UNCHECKED,0);
            session=newSession();sessionDirectory=(!releaseMinimal||diagnosticActive)?sessionRoot+L"\\"+session:L"";helperOutput.clear();
            helper=launchTrustedHelper(installRoot,powershell,false,session,captureRawColorOnce,presentationOff,xeFG,official,observeInputs,traceRenderTail,lowResOutput,traceAcquire,fakeSwapchain,gpuTimestamps,trackingCpuTiming,xessSr,!noDither&&!official,threadCpuSampling,unsigned(SendMessageW(debugControl,CB_GETCURSEL,0,0)),{},xeFG?int(SendMessageW(sceneControl,CB_GETCURSEL,0,0)):-1,xeFG&&!official&&SendMessageW(frameCapControl,BM_GETCHECK,0,0)==BST_CHECKED,frameCapTarget,diagnosticActive);SendMessageW(debugControl,CB_SETCURSEL,0,0);busy=true;SetTimer(window,timerId,500,nullptr);EnableWindow(start,FALSE);
            setStatus(L"正在等待启动助手 / 正常系统授权…\r\n这一步仅提交启动请求，不表示画面功能已生效。");
        }catch(const std::exception& error){helper.releaseLocks();helper.pipe.reset();helper.process.reset();busy=false;KillTimer(window,timerId);EnableWindow(start,pathValid);setStatus(L"无法启动：\r\n"+wide(error.what()));}
    }
    void drainOutput(){drainHelperOutput(helper.pipe,helperOutput);}
    void tick(){
        if(!busy)return;
        drainOutput();if(pathJob){pollPath();return;}
        if(WaitForSingleObject(helper.process.h,0)!=WAIT_OBJECT_0)return;
        DWORD code{};if(!GetExitCodeProcess(helper.process.h,&code))return;
        drainOutput();helper.releaseLocks();KillTimer(window,timerId);
        if(code)setStatus(L"启动失败（退出码 "+std::to_wstring(code)+L"）：\r\n"+boundedDiagnostic(helperOutput.empty()?"No backend output captured.":helperOutput,280));
        else setStatus(diagnosticActive?L"启动请求已完成，本局诊断日志已开启。":L"启动请求已完成。");
        busy=false;EnableWindow(start,pathValid);helper.process.reset();helper.pipe.reset();
    }

};
App* app=nullptr;
LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);app->window=window;}
    if(!app)return DefWindowProcW(window,message,wp,lp);
    if(message==app->activationMessage){app->show();return 1;}
    if(message==app->taskbarCreated){app->tray=false;app->trayAdd();return 0;}
    switch(message){
    case WM_PAINT:app->theme.paint(window);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:{
        const auto dc=reinterpret_cast<HDC>(wp);SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,xess_vk::launcher_ui::ink);
        if(reinterpret_cast<HWND>(lp)==app->pathControl){SetBkColor(dc,RGB(248,250,253));return reinterpret_cast<LRESULT>(app->theme.field);}
        RECT bounds{};GetWindowRect(reinterpret_cast<HWND>(lp),&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);
        const bool onCard=bounds.top>=app->theme.px(84)&&bounds.top<app->theme.px(app->theme.development?502:406);
        SetBkColor(dc,onCard?xess_vk::launcher_ui::paper:xess_vk::launcher_ui::canvas);
        if(reinterpret_cast<HWND>(lp)==app->pathStatus||reinterpret_cast<HWND>(lp)==app->status)SetTextColor(dc,xess_vk::launcher_ui::muted);
        return reinterpret_cast<LRESULT>(onCard?app->theme.card:app->theme.background);}
    case WM_CTLCOLOREDIT:SetTextColor(reinterpret_cast<HDC>(wp),xess_vk::launcher_ui::ink);SetBkColor(reinterpret_cast<HDC>(wp),RGB(248,250,253));return reinterpret_cast<LRESULT>(app->theme.field);
    case WM_DRAWITEM:if(app->theme.drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp),int(wp)==startId))return TRUE;break;
    case WM_DPICHANGED:{
        app->theme.setDpi(HIWORD(wp));app->theme.loadIcons(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window,GWLP_HINSTANCE)));
        SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(app->theme.icon));SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(app->theme.smallIcon));
        if(app->tray){NOTIFYICONDATAW data{};data.cbSize=sizeof(data);data.hWnd=window;data.uID=1;data.uFlags=NIF_ICON;data.hIcon=app->theme.smallIcon;Shell_NotifyIconW(NIM_MODIFY,&data);}
        const auto bounds=reinterpret_cast<RECT*>(lp);SetWindowPos(window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOACTIVATE|SWP_NOZORDER);InvalidateRect(window,nullptr,TRUE);return 0;}
    case WM_SIZE:if(wp==SIZE_MINIMIZED){app->hide();return 0;}break;
    case detectPathMessage:app->detectPath();return 0;
    case WM_COMMAND:switch(LOWORD(wp)){
        case diagnosticId:app->setStatus(L"诊断选择仅下一次启动生效，启动后自动取消勾选。");break;case frameCapId:app->frameCapChanged();break;case applyId:app->saveMultiplier();break;case noDitherId:app->noDitherChanged();break;
        case pathChangeId:app->choosePath();break;case pathDetectId:app->detectPath();break;
        case defaultsId:app->restoreDefaults();break;case advancedId:app->toggleAdvanced();break;
        case startId:app->startGame();break;case trayId:app->hide();break;case logsId:app->openLogs();break;case showId:app->show();break;
        case exitId:if(app->busy)MessageBoxW(window,L"启动助手仍在运行。请先完成或取消系统授权，再退出启动窗口。",L"等待启动助手",MB_OK|MB_ICONINFORMATION);else DestroyWindow(window);break;
        }return 0;
    case trayMessage:if(lp==WM_LBUTTONDBLCLK)app->show();else if(lp==WM_RBUTTONUP){HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,showId,L"显示窗口");AppendMenuW(menu,MF_STRING,logsId,L"打开诊断日志");AppendMenuW(menu,MF_STRING,exitId,L"退出启动器");POINT point{};GetCursorPos(&point);SetForegroundWindow(window);TrackPopupMenu(menu,TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);DestroyMenu(menu);PostMessageW(window,WM_NULL,0,0);}return 0;
    case WM_TIMER:if(wp==timerId)app->tick();return 0;
    case WM_CLOSE:app->hide();return 0;
    case WM_DESTROY:{KillTimer(window,timerId);NOTIFYICONDATAW data{};data.cbSize=sizeof(data);data.hWnd=window;data.uID=1;if(app->tray)Shell_NotifyIconW(NIM_DELETE,&data);PostQuitMessage(0);return 0;}
    }
    return DefWindowProcW(window,message,wp,lp);
}

int selfTest(){
    unsigned checks=0;auto check=[&](bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);};
    auto refuses=[&](auto action){try{action();return false;}catch(...){return true;}};
    check(sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 implementation");
    check(gameCompatibilityText("unreviewed_game_build").find(L"尚未适配")!=std::wstring::npos,"Unverified build has a clear localized update explanation");
    check(gameCompatibilityText("unverified_publisher").find(L"签名")!=std::wstring::npos,"Publisher failure is distinct from version mismatch");
    check(gameCompatibilityText("unavailable").find(L"Endfield.exe")!=std::wstring::npos,"Missing files explain how to select the game directory");
    check(xess_vk::refresh_warning::text(120,2,false,30).empty(),"60 x2 at 120 Hz needs no warning");
    check(xess_vk::refresh_warning::text(120,3,true,50).find(L"50×3=150")!=std::wstring::npos,"50 x3 refresh warning");
    check(!xess_vk::refresh_warning::exceeds(40,3,120)&&!xess_vk::refresh_warning::exceeds(30,4,120),"40 x3 and 30 x4 do not exceed 120 Hz");

    const std::vector<std::wstring> args{L"tool.exe",L"",L"C:\\Program Files\\EndfieldXeSSObserver\\",L"quoted\"value",L"ends\\",L"双引号\"与空 格",L"a\\\\\"b"};
    const auto joined=commandLine(args);int count=0;auto* parsed=CommandLineToArgvW(joined.c_str(),&count);check(parsed&&count==static_cast<int>(args.size()),"Command line count");for(int i=0;i<count;++i)check(parsed[i]==args[i],"Command line quoting");LocalFree(parsed);
    const std::wstring session=L"0123456789abcdef0123456789abcdef";
    const auto launch=commandLine(helperArgs(L"C:\\Program Files\\PowerShell\\7\\pwsh.exe",L"C:\\Program Files\\EndfieldXeSSObserver\\manage-observer-install.ps1",session));
    check(launch.find(L"LaunchMod")!=std::wstring::npos&&launch.find(L"-BridgeDevice")==std::wstring::npos&&launch.find(L"-TraceFsr")==std::wstring::npos&&launch.find(L"-TraceFsrUpscale")==std::wstring::npos,"Default independent request omits graphics research options");
    check(launch.find(L"DX11")==std::wstring::npos&&launch.find(L"-Verb")==std::wstring::npos,"No renderer change/forced elevation in frontend");
    check(launch.find(L"-CaptureRawColor")==std::wstring::npos,"Default GUI helper cannot opt in through inheritance");
    check(launch.find(L"-PresentationOff")==std::wstring::npos,"Presentation remains explicit opt-in");
    check(launch.find(L"-XeFG")==std::wstring::npos,"XeFG remains explicit opt-in");
    check(launcherMode({L"--xefg"})==LauncherMode::XeFG,"Explicit XeFG mode");
    for(const auto mode:{LauncherMode::XeFG,LauncherMode::LowResOutput,LauncherMode::FullResOutput,LauncherMode::FakeSwapchain})
        check(fakeSwapchainRoute(mode),"Regular XeFG output modes default to one swapchain");
    check(!fakeSwapchainRoute(LauncherMode::Gui)&&!fakeSwapchainRoute(LauncherMode::Official)&&
        !fakeSwapchainRoute(LauncherMode::ObserveInputs)&&!fakeSwapchainRoute(LauncherMode::TraceAcquire)&&
        !fakeSwapchainRoute(LauncherMode::TraceRenderTail),"Non-XeFG and existing trace routes keep their swapchain policy");
    for(const auto& options:std::vector<std::vector<std::wstring>>{{L"--xefg",L"--legacy-chain"},
        {L"--legacy-chain",L"--xefg"},{L"--low-res-output",L"--legacy-chain",L"--xefg"}}){
        const auto mode=launcherMode(options);
        check(mode==LauncherMode::LegacyChain&&xeFGRoute(mode)&&lowResOutputRoute(mode)&&!fakeSwapchainRoute(mode),
            "Legacy chain keeps the low-resolution XeFG route in either order");
    }
    {const auto mode=launcherMode({L"--legacy-chain",L"--full-res-output",L"--xefg"});
        check(mode==LauncherMode::LegacyChainFullRes&&xeFGRoute(mode)&&!lowResOutputRoute(mode)&&!fakeSwapchainRoute(mode),
            "Legacy chain supports the full-resolution baseline");}
    {const auto mode=launcherMode({L"--xefg"});const auto defaults=helperArgs(L"ps",L"script",session,
        false,false,xeFGRoute(mode),false,false,false,lowResOutputRoute(mode),false,fakeSwapchainRoute(mode));
        check(std::count(defaults.begin(),defaults.end(),L"-FakeSwapchain")==1&&
            std::count(defaults.begin(),defaults.end(),L"-LowResOutput")==1,"Unchanged --xefg selects single-chain low-resolution helper request");}
    check(launcherMode({L"--official"})==LauncherMode::Official,"Explicit official fallback mode");
    check(launcherMode({L"--observe-inputs"})==LauncherMode::ObserveInputs,"Explicit input observation mode");
    const auto observeArgs=helperArgs(L"ps",L"script",session,false,false,false,false,true);
    check(std::count(observeArgs.begin(),observeArgs.end(),L"-ObserveInputs")==1,"Input observation carries exactly one typed switch");
    for(const auto* option:{L"-TraceFsr",L"-TraceFsrUpscale",L"-TraceCompute",L"-BridgeDevice",L"-PresentationOff",L"-XeFG",L"-CaptureRawColor",L"-OfficialFallback"})
        check(std::count(observeArgs.begin(),observeArgs.end(),option)==0,"Input observation omits other experimental switches");
    check(refuses([&]{helperArgs(L"ps",L"script",session,true,false,false,false,true);}),"Input observation refuses capture");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,true,false,false,true);}),"Input observation refuses presentation");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,true,false,true);}),"Input observation refuses XeFG");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,true,true);}),"Input observation refuses official handoff");
    check(refuses([&]{launchTrustedHelper(L"unused",L"ps",true,session,false,false,false,false,true);}),"Readonly refuses observation before opening paths");
    const auto officialArgs=helperArgs(L"ps",L"script",session,false,false,false,true);
    check(std::count(officialArgs.begin(),officialArgs.end(),L"-OfficialFallback")==1&&std::count(officialArgs.begin(),officialArgs.end(),L"-BridgeDevice")==0,"Official fallback is explicit and carries no graphics options");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,true,true);}),"Official fallback cannot combine with experimental modes");
    const auto xeFGArgs=helperArgs(L"ps",L"script",session,false,false,true);
    check(std::count(xeFGArgs.begin(),xeFGArgs.end(),L"-XeFG")==1,"Continuous OFF uses the single existing XeFG switch");
    check(std::count(xeFGArgs.begin(),xeFGArgs.end(),L"-TraceRenderTail")==0,"Normal XeFG does not request render-tail diagnostics");
    check(std::count(xeFGArgs.begin(),xeFGArgs.end(),L"-WsiAcquireTrace")==0,"Normal XeFG does not request acquire observation");
    check(std::count(xeFGArgs.begin(),xeFGArgs.end(),L"-FakeSwapchain")==0,"Normal XeFG does not request fake swapchain");
    check(std::count(xeFGArgs.begin(),xeFGArgs.end(),L"-LowResOutput")==0,"Helper XeFG without the typed low-resolution flag stays full resolution");
    check(launcherMode({L"--xefg",L"--low-res-output"})==LauncherMode::LowResOutput&&
        launcherMode({L"--low-res-output",L"--xefg"})==LauncherMode::LowResOutput,"Explicit low-resolution modifier accepts either order");
    check(launcherMode({L"--xefg",L"--full-res-output"})==LauncherMode::FullResOutput&&
        launcherMode({L"--full-res-output",L"--xefg"})==LauncherMode::FullResOutput,"Explicit full-resolution modifier accepts either order");
    check(lowResOutputRoute(LauncherMode::XeFG)&&lowResOutputRoute(LauncherMode::LowResOutput)&&lowResOutputRoute(LauncherMode::TraceAcquire),"Plain --xefg and acquire observation keep low-resolution output");
    check(!lowResOutputRoute(LauncherMode::FullResOutput)&&!lowResOutputRoute(LauncherMode::TraceRenderTail)&&
        !lowResOutputRoute(LauncherMode::Gui)&&!lowResOutputRoute(LauncherMode::Official)&&!lowResOutputRoute(LauncherMode::ObserveInputs),
        "Full-resolution, render-tail and non-XeFG routes never request low-resolution output");
    check(xeFGRoute(LauncherMode::XeFG)&&xeFGRoute(LauncherMode::LowResOutput)&&xeFGRoute(LauncherMode::FullResOutput)&&xeFGRoute(LauncherMode::TraceRenderTail)&&xeFGRoute(LauncherMode::TraceAcquire)&&
        !xeFGRoute(LauncherMode::Gui)&&!xeFGRoute(LauncherMode::Official)&&!xeFGRoute(LauncherMode::ObserveInputs),"Only explicit --xefg routes select XeFG");
    const auto lowResArgs=helperArgs(L"ps",L"script",session,false,false,true,false,false,false,true);
    check(std::count(lowResArgs.begin(),lowResArgs.end(),L"-LowResOutput")==1&&
        std::count(lowResArgs.begin(),lowResArgs.end(),L"-XeFG")==1&&
        std::count(lowResArgs.begin(),lowResArgs.end(),L"-TraceRenderTail")==0,"Low-resolution carries only its typed modifier and XeFG route");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,false,false,false,true);}),"Low-resolution refuses default route");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,true,false,false,true);}),"Low-resolution refuses official route");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,false,true,false,true);}),"Low-resolution refuses input observation");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,true,false,false,true,true);}),"Low-resolution refuses render-tail diagnostics");
    check(refuses([&]{launchTrustedHelper(L"unused",L"ps",true,session,false,false,false,false,false,false,true);}),"Readonly refuses low-resolution before opening paths");
    check(launcherMode({L"--xefg",L"--trace-render-tail"})==LauncherMode::TraceRenderTail&&
        launcherMode({L"--trace-render-tail",L"--xefg"})==LauncherMode::TraceRenderTail,"Explicit render-tail modifier accepts either order");
    const auto tailArgs=helperArgs(L"ps",L"script",session,false,false,true,false,false,true);
    check(std::count(tailArgs.begin(),tailArgs.end(),L"-TraceRenderTail")==1&&
        std::count(tailArgs.begin(),tailArgs.end(),L"-XeFG")==1,"Render-tail carries only the typed diagnostic modifier and existing route");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,false,false,true);}),"Render-tail refuses the default route");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,false,true,true);}),"Render-tail refuses input-only observation");
    check(refuses([&]{launchTrustedHelper(L"unused",L"ps",true,session,false,false,true,false,false,true);}),"Readonly refuses render-tail before opening paths");
    check(launcherMode({L"--xefg",L"--trace-acquire"})==LauncherMode::TraceAcquire&&
        launcherMode({L"--trace-acquire",L"--xefg"})==LauncherMode::TraceAcquire,"Acquire observation accepts either order");
    const auto acquireArgs=helperArgs(L"ps",L"script",session,false,false,true,false,false,false,true,true);
    check(std::count(acquireArgs.begin(),acquireArgs.end(),L"-WsiAcquireTrace")==1&&
        std::count(acquireArgs.begin(),acquireArgs.end(),L"-XeFG")==1&&
        std::count(acquireArgs.begin(),acquireArgs.end(),L"-LowResOutput")==1&&
        std::count(acquireArgs.begin(),acquireArgs.end(),L"-TraceRenderTail")==0,"Acquire observation keeps XeFG low-resolution output and omits render-tail");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,false,false,false,false,true);}),"Acquire observation refuses the default route");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,false,true,false,false,true);}),"Acquire observation refuses input-only observation");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,true,false,false,true,false,true);}),"Acquire observation refuses render-tail diagnostics");
    check(refuses([&]{launchTrustedHelper(L"unused",L"ps",true,session,false,false,false,false,false,false,false,true);}),"Readonly refuses acquire observation before opening paths");
    check(launcherMode({L"--xefg",L"--fake-swapchain"})==LauncherMode::FakeSwapchain&&
        launcherMode({L"--fake-swapchain",L"--xefg"})==LauncherMode::FakeSwapchain,"Fake swapchain accepts either order");
    check(xeFGRoute(LauncherMode::FakeSwapchain)&&lowResOutputRoute(LauncherMode::FakeSwapchain),"Fake swapchain retains the existing low-resolution XeFG route");
    check(launch.find(L"-GpuTimestamps")==std::wstring::npos,"GPU timestamps default OFF");
    for(bool fake:{false,true}){
        const auto gpuArgs=helperArgs(L"ps",L"script",session,false,false,true,false,false,false,true,false,fake,true);
        check(std::count(gpuArgs.begin(),gpuArgs.end(),L"-GpuTimestamps")==1,"GPU timestamps use one typed modifier for native and fake");
    }
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,false,false,false,false,false,false,true);}),"GPU timestamps reject non-XeFG route");
    { wchar_t saved[32768]{};const auto savedCount=GetEnvironmentVariableW(L"XESS_GPU_TIMESTAMPS",saved,32768);
      for(const auto* value:{static_cast<const wchar_t*>(nullptr),L"0",L"01",L"11",L"1 ",L"1"}){
        SetEnvironmentVariableW(L"XESS_GPU_TIMESTAMPS",value);
        check(gpuTimestampsFromEnvironment()==(value&&std::wcscmp(value,L"1")==0),"GUI accepts exact GPU timestamp request only");
      }
      SetEnvironmentVariableW(L"XESS_GPU_TIMESTAMPS",savedCount&&savedCount<32768?saved:nullptr);
    }
    check(launch.find(L"-TrackingCpuTiming")==std::wstring::npos,"Tracking CPU timing defaults OFF");
    check(launch.find(L"-ThreadCpuSampling")==std::wstring::npos,"Thread CPU sampling defaults OFF");
    {const auto sampled=helperArgs(L"ps",L"script",session,false,false,true,false,false,false,true,false,true,false,false,true,false,true);
        check(std::count(sampled.begin(),sampled.end(),L"-ThreadCpuSampling")==1&&std::count(sampled.begin(),sampled.end(),L"-TrackingCpuTiming")==0,"Thread sampling is typed and independent of hook timing");}
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,false,false,false,false,false,false,false,false,false,false,true);}),"Thread sampling rejects a non-XeFG route");
    {wchar_t saved[32768]{};const auto count=GetEnvironmentVariableW(L"XESS_THREAD_CPU_SAMPLING",saved,32768);
        for(const auto* value:{static_cast<const wchar_t*>(nullptr),L"0",L"11",L"true",L"1"}){SetEnvironmentVariableW(L"XESS_THREAD_CPU_SAMPLING",value);check(threadCpuSamplingFromEnvironment()==(value&&std::wcscmp(value,L"1")==0),"Only exact thread sampler opt-in is accepted");}
        SetEnvironmentVariableW(L"XESS_THREAD_CPU_SAMPLING",count&&count<32768?saved:nullptr);}
    for(bool fake:{false,true})for(bool gpu:{false,true}){
        const auto timedArgs=helperArgs(L"ps",L"script",session,false,false,true,false,false,false,true,false,fake,gpu,true);
        check(std::count(timedArgs.begin(),timedArgs.end(),L"-TrackingCpuTiming")==1&&
            std::count(timedArgs.begin(),timedArgs.end(),L"-GpuTimestamps")==unsigned(gpu),"Tracking CPU timing is independent of GPU timestamps on native/fake routes");
    }
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,false,false,false,false,false,false,false,true);}),"Tracking timing rejects non-XeFG route");
    check(refuses([&]{launchTrustedHelper(L"unused",L"ps",true,session,false,false,false,false,false,false,false,false,false,false,true);}),"Readonly rejects tracking timing before opening paths");
    { wchar_t saved[32768]{};const auto savedCount=GetEnvironmentVariableW(L"XESS_TRACKING_CPU_TIMING",saved,32768);
      for(const auto* value:{static_cast<const wchar_t*>(nullptr),L"0",L"01",L"11",L"1 ",L"1"}){
        SetEnvironmentVariableW(L"XESS_TRACKING_CPU_TIMING",value);
        check(trackingCpuTimingFromEnvironment()==(value&&std::wcscmp(value,L"1")==0),"GUI accepts exact tracking timing request only");
      }
      SetEnvironmentVariableW(L"XESS_TRACKING_CPU_TIMING",savedCount&&savedCount<32768?saved:nullptr);
    }
    const auto fakeArgs=helperArgs(L"ps",L"script",session,false,false,true,false,false,false,true,false,true);
    check(std::count(fakeArgs.begin(),fakeArgs.end(),L"-FakeSwapchain")==1&&
        std::count(fakeArgs.begin(),fakeArgs.end(),L"-XeFG")==1&&std::count(fakeArgs.begin(),fakeArgs.end(),L"-LowResOutput")==1&&
        std::count(fakeArgs.begin(),fakeArgs.end(),L"-WsiAcquireTrace")==0&&std::count(fakeArgs.begin(),fakeArgs.end(),L"-TraceRenderTail")==0,"Fake swapchain carries its typed option without diagnostics");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,false,false,false,false,false,true);}),"Fake swapchain refuses the default route");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,true,false,false,false,true,true,true);}),"Fake swapchain refuses acquire diagnostics");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,true,false,false,true,false,false,true);}),"Fake swapchain refuses render-tail diagnostics");
    check(refuses([&]{launchTrustedHelper(L"unused",L"ps",true,session,false,false,false,false,false,false,false,false,true);}),"Readonly refuses fake swapchain before opening paths");
    for(const auto* option:{L"-BridgeDevice",L"-PresentationOff",L"-TraceFsr",L"-TraceFsrUpscale",L"-TraceCompute",L"-ObserveInputs"})
        check(std::count(xeFGArgs.begin(),xeFGArgs.end(),option)==0,"Continuous OFF does not mix legacy switches or pure observation");
    check(std::count(xeFGArgs.begin(),xeFGArgs.end(),L"-CaptureRawColor")==0,"XeFG helper omits one-shot capture");
    check(refuses([&]{helperArgs(L"ps",L"script",session,true,false,true);}),"XeFG/capture conflict rejected");
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,true,true);}),"Continuous OFF refuses a parallel presentation switch");
    check(refuses([&]{launchTrustedHelper(L"unused",L"ps",true,session,false,false,true);}),"Readonly refuses XeFG before opening paths");
    check(launcherMode({L"--presentation-off"})==LauncherMode::PresentationOff,"Explicit presentation OFF mode");
    const auto offArgs=helperArgs(L"ps",L"script",session,false,true);
    check(std::count(offArgs.begin(),offArgs.end(),L"-PresentationOff")==1&&std::count(offArgs.begin(),offArgs.end(),L"-BridgeDevice")==1,"Typed presentation OFF and bridge switches");
    check(std::count(offArgs.begin(),offArgs.end(),L"-CaptureRawColor")==0&&std::count(offArgs.begin(),offArgs.end(),L"-TraceFsr")==0&&std::count(offArgs.begin(),offArgs.end(),L"-TraceFsrUpscale")==0,"OFF helper omits capture and FSR trace");
    check(refuses([&]{helperArgs(L"ps",L"script",session,true,true);}),"OFF/capture conflict rejected");
    check(refuses([&]{launchTrustedHelper(L"unused",L"ps",true,session,false,true);}),"Readonly refuses OFF before opening paths");
    for(bool gpu:{false,true}){
        const auto srArgs=helperArgs(L"ps",L"script",session,false,false,true,false,false,false,true,false,true,gpu,false,true);
        check(std::count(srArgs.begin(),srArgs.end(),L"-XessSr")==1&&std::count(srArgs.begin(),srArgs.end(),L"-GpuTimestamps")==unsigned(gpu),"SR activation and explicit GPU timestamp request are independent");
    }
    check(refuses([&]{helperArgs(L"ps",L"script",session,false,false,false,false,false,false,false,false,false,false,false,true);}),"SR cannot enter a legacy or non-XeFG route");
    const auto cleanSrEnvironment=helperEnvironment({L"XESS_SR_REPLACE=1"},L"C:\\Program Files\\PowerShell\\7\\pwsh.exe");
    check(std::wstring(cleanSrEnvironment.data(),cleanSrEnvironment.size()).find(L"XESS_SR_REPLACE")==std::wstring::npos,"normal helper does not inherit SR activation");
    const auto captureArgs=helperArgs(L"ps",L"script",session,true);
    check(std::count(captureArgs.begin(),captureArgs.end(),L"-CaptureRawColor")==1&&captureArgs.back()==L"-CaptureRawColor","Exact one no-value capture switch");
    check(launcherMode({})==LauncherMode::Gui&&launcherMode({L"--capture-rawcolor-once"})==LauncherMode::CaptureRawColorOnce,"Default and explicit capture modes");
    check(launcherMode({L"--self-test"})==LauncherMode::SelfTest&&launcherMode({L"--check-helper-readonly"})==LauncherMode::Readonly,"Diagnostic modes separate from capture");
    for(const auto& options:std::vector<std::vector<std::wstring>>{{L"--capture-rawcolor-once",L"1"},{L"--capture-rawcolor-once=1"},{L"--capture-rawcolor-once",L"--capture-rawcolor-once"},
        {L"--self-test",L"--capture-rawcolor-once"},{L"--capture-rawcolor-once",L"--check-helper-readonly"},
        {L"--presentation-off",L"1"},{L"--presentation-off=1"},{L"--presentation-off",L"--capture-rawcolor-once"},
        {L"--presentation-off",L"--self-test"},{L"--presentation-off",L"--presentation-off"},
        {L"--xefg",L"1"},{L"--xefg=1"},{L"--xefg",L"--presentation-off"},{L"--xefg",L"--capture-rawcolor-once"},
        {L"--xefg",L"--xefg"},{L"--xefg",L"--self-test"},{L"--official",L"--xefg"},{L"--official",L"1"},
        {L"--observe-inputs",L"1"},{L"--observe-inputs=1"},{L"--observe-inputs",L"--observe-inputs"},
        {L"--observe-inputs",L"--official"},{L"--observe-inputs",L"--xefg"},{L"--observe-inputs",L"--presentation-off"},
        {L"--observe-inputs",L"--capture-rawcolor-once"},{L"--observe-inputs",L"--self-test"},
        {L"--trace-render-tail"},{L"--trace-render-tail=1"},{L"--trace-render-tail",L"1"},
        {L"--trace-render-tail",L"--trace-render-tail"},{L"--trace-render-tail",L"--official"},
        {L"--trace-render-tail",L"--observe-inputs"},{L"--trace-render-tail",L"--self-test"},
        {L"--xefg",L"--trace-render-tail",L"1"},{L"--low-res-output"},{L"--low-res-output=1"},{L"--low-res-output",L"1"},
        {L"--low-res-output",L"--low-res-output"},{L"--low-res-output",L"--official"},
        {L"--low-res-output",L"--observe-inputs"},{L"--low-res-output",L"--self-test"},
        {L"--low-res-output",L"--check-helper-readonly"},{L"--low-res-output",L"--capture-rawcolor-once"},
        {L"--low-res-output",L"--presentation-off"},{L"--xefg",L"--low-res-output",L"1"},
        {L"--xefg",L"--low-res-output",L"--trace-render-tail"},{L"--full-res-output"},{L"--full-res-output=1"},
        {L"--full-res-output",L"--full-res-output"},{L"--full-res-output",L"--official"},{L"--full-res-output",L"--observe-inputs"},
        {L"--full-res-output",L"--low-res-output"},{L"--full-res-output",L"--trace-render-tail"},{L"--xefg",L"--full-res-output",L"1"},
        {L"--xefg",L"--full-res-output",L"--low-res-output"},{L"--xefg",L"--full-res-output",L"--trace-render-tail"},
        {L"--trace-acquire"},{L"--trace-acquire=1"},{L"--trace-acquire",L"1"},
        {L"--trace-acquire",L"--trace-acquire"},{L"--trace-acquire",L"--official"},
        {L"--trace-acquire",L"--observe-inputs"},{L"--trace-acquire",L"--self-test"},
        {L"--trace-acquire",L"--check-helper-readonly"},{L"--trace-acquire",L"--presentation-off"},
        {L"--trace-acquire",L"--capture-rawcolor-once"},{L"--xefg",L"--trace-acquire",L"1"},
        {L"--xefg",L"--trace-acquire",L"--trace-render-tail"},{L"--xefg",L"--trace-acquire",L"--low-res-output"},
        {L"--xefg",L"--trace-acquire",L"--full-res-output"},{L"--xefg",L"--trace-acquire",L"--trace-acquire"},
        {L"--fake-swapchain"},{L"--fake-swapchain=1"},{L"--fake-swapchain",L"1"},
        {L"--fake-swapchain",L"--official"},{L"--fake-swapchain",L"--self-test"},
        {L"--xefg",L"--fake-swapchain",L"--trace-acquire"},{L"--xefg",L"--fake-swapchain",L"--trace-render-tail"},
        {L"--xefg",L"--fake-swapchain",L"--full-res-output"},{L"--xefg",L"--fake-swapchain",L"--low-res-output"},
        {L"--xefg",L"--fake-swapchain",L"--fake-swapchain"},
        {L"--legacy-chain"},{L"--legacy-chain=1"},{L"--xefg",L"--legacy-chain",L"--legacy-chain"},
        {L"--xefg",L"--legacy-chain",L"--fake-swapchain"},{L"--xefg",L"--legacy-chain",L"--trace-acquire"},
        {L"--xefg",L"--legacy-chain",L"--trace-render-tail"},{L"--official",L"--legacy-chain"},
        {L"--xefg",L"--legacy-chain",L"--low-res-output",L"--full-res-output"},{L"--unknown"}}){check(refuses([&]{launcherMode(options);}),"Extra values/duplicate/mixed options refused");}
    check(refuses([&]{helperArgs(L"ps",L"script",L"x';Start-Process anything");}),"Session injection refusal");
    const std::wstring ps=L"C:\\Program Files\\PowerShell\\7\\pwsh.exe";
    check(refuses([&]{lockPowerShell7(L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");}),"Windows PowerShell fallback refused");
    check(refuses([&]{lockPowerShell7(L"D:\\untrusted\\pwsh.exe");}),"Untrusted PowerShell location refused");
    check(systemPowerShell()==powerShell7Path(),"Installed PowerShell 7 version and Microsoft signature verified");
    const auto readonlyArgs=commandLine(statusArgs(ps,L"C:\\Program Files\\EndfieldXeSSObserver\\manage-observer-install.ps1"));
    check(readonlyArgs.find(L"Status")!=std::wstring::npos&&readonlyArgs.find(L"LaunchMod")==std::wstring::npos&&readonlyArgs.find(L"CaptureRawColor")==std::wstring::npos,"Readonly probe cannot launch game or capture");
    const auto environment=helperEnvironment({L"psmodulepath=C:\\PowerShell7\\Modules",L"PSMODULEPATH=C:\\wrong",L"PATH=C:\\Windows",L"=C:=C:\\example",L"KEEP=unchanged",
        L"xess_continuous_inputs=1",L"XESS_FSR_NATIVE_OFF=1",L"xess_xefg_low_res_output=1",L"xess_wsi_acquire_observer=1",L"xess_fake_swapchain=1",L"xess_gpu_timestamps=1",L"xess_tracking_cpu_timing=1",L"DISABLE_RTSS_LAYER=0"},ps);
    std::map<std::wstring,std::wstring,EnvironmentNameLess> parsedEnvironment;
    for(const wchar_t* entry=environment.data();*entry;entry+=wcslen(entry)+1){const std::wstring value=entry;const auto at=value.find(L'=',1);parsedEnvironment[value.substr(0,at)]=value.substr(at+1);}
    check(parsedEnvironment.size()==5,"Module path duplicates replaced case-insensitively");
    check(parsedEnvironment.at(L"DISABLE_RTSS_LAYER")==L"0","Helper preserves inherited RTSS choice for the selected route to handle");
    check(parsedEnvironment.at(L"PSModulePath")==L"C:\\Program Files\\PowerShell\\7\\Modules","Child uses trusted PowerShell 7 inbox modules");
    check(parsedEnvironment.at(L"PATH")==L"C:\\Windows"&&parsedEnvironment.at(L"KEEP")==L"unchanged","Unrelated child environment preserved");
    check(parsedEnvironment.at(L"=C:")==L"C:\\example","Hidden drive environment preserved");
    check(!parsedEnvironment.contains(L"XESS_CONTINUOUS_INPUTS")&&!parsedEnvironment.contains(L"XESS_FSR_NATIVE_OFF"),"Helper cannot inherit another mode's continuous/native-off switches");
    check(!parsedEnvironment.contains(L"XESS_XEFG_LOW_RES_OUTPUT"),"Helper cannot inherit low-resolution output opt-in");
    check(!parsedEnvironment.contains(L"XESS_WSI_ACQUIRE_OBSERVER"),"Helper cannot inherit acquire observation opt-in");
    check(!parsedEnvironment.contains(L"XESS_FAKE_SWAPCHAIN"),"Helper cannot inherit fake swapchain opt-in");
    check(!parsedEnvironment.contains(L"XESS_GPU_TIMESTAMPS"),"GPU timestamp request is typed, never arbitrary inheritance");
    check(!parsedEnvironment.contains(L"XESS_TRACKING_CPU_TIMING"),"Tracking timing request is typed, never arbitrary inheritance");
    check(environment.size()>1&&environment.back()==0&&environment[environment.size()-2]==0,"Unicode environment double NUL");
    check(refuses([&]{helperEnvironment({L"BROKEN"},ps);}),"Malformed environment refused");
    check(boundedDiagnostic(std::string(10000,'x'),100).size()<150,"Visible diagnostic text is bounded");
    check(Parser("{\"s\":\"\\u4e2d\\u6587\\ud83d\\ude00\",\"b\":true,\"a\":[null,12]}").parse().at("s").str()==utf8(L"中文😀"),"JSON Unicode");
    for(const auto* bad:{"{\"x\":1,\"x\":2}","{\"x\":01}","[] x","{\"x\":\"\\ud800\"}","{\"x\":}","[true,]"})check(refuses([&]{Parser(bad).parse();}),"Malformed JSON refusal");
    check(refuses([&]{Parser(std::string(statusLimit+1,' ')).parse();}),"Oversized JSON refusal");
    // Nested Status JSON can include substantial whitespace.
    // The human-visible diagnostic preview is deliberately shorter than a valid
    // helper response; parsing must consume the complete bounded pipe output.
    const std::string longStatus="{\"installed\":true,\"standaloneLaunchAvailable\":true,"+std::string(9000,' ')+"\"gameCompatibility\":{\"files\":[{\"name\":\"Endfield.exe\"},{\"name\":\"UnityPlayer.dll\"},{\"name\":\"OneChannelLC.dll\"}]}}";
    const auto completeStatus=Parser(utf8(decodeHelperText(longStatus))).parse();
    check(completeStatus.at("installed").boolean()&&completeStatus.at("gameCompatibility").at("files").array.size()==3,"Structured Status beyond 8192 bytes must not use the diagnostic preview");
    check(diagnosticText(longStatus).size()==8192,"Human-readable diagnostic preview remains bounded");
    check(refuses([&]{Parser(utf8(diagnosticText(longStatus))).parse();}),"Large Status fixture reproduces the previous preview truncation failure");
    const std::string hash(64,'a');
    const std::string receipt="{\"productId\":\"endfield-xess-vulkan-observer-v1\",\"schemaVersion\":1,\"installRoot\":\"C:\\\\Program Files\\\\EndfieldXeSSObserver\",\"modLauncher\":{\"version\":1,\"efuVersion\":\"5.1.0\",\"efuSha256\":\"a686b5b7704341e7bfb016794df074a39f6195b8d80a4a99ca4f8d8de9eca5ae\",\"efuSourceCommit\":\"1165a377f7915528b1ae9d2c3e41631bfd59b957\"},\"sourceScriptSha256\":\""+hash+"\",\"files\":[{\"name\":\"manage-observer-install.ps1\",\"sha256\":\""+hash+"\"}]}";
    check(receiptScriptHash(Parser(receipt).parse(),L"C:\\Program Files\\EndfieldXeSSObserver")==hash,"Protected receipt hash selection");
    check(refuses([&]{receiptScriptHash(Parser(receipt).parse(),L"C:\\Users\\test");}),"Wrong receipt path refusal");
    check(hexString(currentEfuSha256,64),"Current preview build identity must be pinned before distribution");
    auto previewReceipt=receipt;
    previewReceipt.replace(previewReceipt.find("a686b5b7704341e7bfb016794df074a39f6195b8d80a4a99ca4f8d8de9eca5ae"),64,currentEfuSha256);
    previewReceipt.replace(previewReceipt.find("1165a377f7915528b1ae9d2c3e41631bfd59b957"),40,currentEfuCommit);
    check(receiptScriptHash(Parser(previewReceipt).parse(),L"C:\\Program Files\\EndfieldXeSSObserver")==hash,"Preview receipt shares the reviewed helper trust boundary");
    check(refuses([&]{receiptScriptHash(Parser(previewReceipt).parse(),L"C:\\Program Files\\EndfieldXeSSObserver",true);}),
        "Old receipt cannot launch the new single-chain default through an old helper");
    auto policyReceipt=Parser(previewReceipt).parse();policyReceipt.object.emplace("singleChainPolicyVersion",Parser("1").parse());
    check(receiptScriptHash(policyReceipt,L"C:\\Program Files\\EndfieldXeSSObserver",true)==hash,
        "New single-chain package receipt permits the reviewed helper");
    for(const auto* version:{"0","2","\"1\"","true","null"}){
        auto wrongPolicy=policyReceipt;wrongPolicy.object.at("singleChainPolicyVersion")=Parser(version).parse();
        check(refuses([&]{receiptScriptHash(wrongPolicy,L"C:\\Program Files\\EndfieldXeSSObserver",true);}),
            "Missing, changed or malformed single-chain policy refuses before helper execution");
    }

    for(const auto* priorEfu:{"fe27f71582726fb21cb51076a9ed3e16c6c6eae32588364daff3c5a8078d24f9","2ef1cd8066c62ebe42b41d098dfe8c36a559e5845626f7e6d98ddd78f4dd21fa"}) {
        auto priorReceipt=previewReceipt;
        priorReceipt.replace(priorReceipt.find(currentEfuSha256),64,priorEfu);
        check(receiptScriptHash(Parser(priorReceipt).parse(),L"C:\\Program Files\\EndfieldXeSSObserver")==hash,"Prior reviewed preview receipt remains readable for removal");
    }
    auto descriptorAccepted=[](const wchar_t* sddl){PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&descriptor,nullptr))throw failure("Create ACL fixture");try{protectedDescriptor(descriptor);}catch(...){LocalFree(descriptor);throw;}LocalFree(descriptor);};
    descriptorAccepted(L"O:BAD:P(A;;FA;;;BA)(A;;FA;;;SY)(A;;FRFX;;;BU)");++checks;
    for(const auto* sddl:{L"O:BAD:(A;;FA;;;BA)(A;;FR;;;BU)",L"O:BUD:P(A;;FA;;;BA)(A;;FR;;;BU)",L"O:BAD:P(A;;FA;;;BA)(A;;FW;;;BU)",L"O:BAD:P(A;;FA;;;BA)(A;;WD;;;AU)"})check(refuses([&]{descriptorAccepted(sddl);}),"Weak ACL refusal");
    check(!diagnosticText(std::string("\xff\xfe\x80",3)).empty(),"Legacy console output decoding");
    std::printf("{\"passed\":true,\"checks\":%u,\"guiShown\":false,\"gameStarted\":false,\"helperExecuted\":false,\"effectsVerified\":false,\"xeFGIntegrated\":false}\n",checks);return 0;
}
int checkHelperReadonly(){
    const auto root=knownFolder(FOLDERID_ProgramFiles)+L"\\EndfieldXeSSObserver";
    auto child=launchTrustedHelper(root,systemPowerShell(),true);
    std::string output;const auto started=GetTickCount64();DWORD exitCode=0;
    for(;;){
        drainHelperOutput(child.pipe,output);
        const auto state=WaitForSingleObject(child.process.h,10);
        if(state==WAIT_OBJECT_0)break;
        if(state!=WAIT_TIMEOUT||GetTickCount64()-started>30000){
            // This is only our fixed, non-elevated Status child. Never apply
            // timeout termination to the GUI LaunchMod helper or its children.
            TerminateProcess(child.process.h,124);
            if(WaitForSingleObject(child.process.h,5000)!=WAIT_OBJECT_0)
                throw std::runtime_error("Readonly Status wait failed and its child termination could not be confirmed");
            throw std::runtime_error("Readonly installed-helper Status timed out or failed its wait; its child was stopped");
        }
    }
    drainHelperOutput(child.pipe,output);
    if(!GetExitCodeProcess(child.process.h,&exitCode))throw failure("Read readonly helper exit code");
    if(exitCode!=0)throw std::runtime_error("Readonly installed-helper Status exit "+std::to_string(exitCode)+": "+utf8(boundedDiagnostic(output,8192)));
    const auto status=Parser(utf8(decodeHelperText(output))).parse();
    if(!status.at("installed").boolean()||!status.at("standaloneLaunchAvailable").boolean())throw std::runtime_error("Readonly helper did not confirm a valid installed standalone package");
    const auto& runtime=status.at("powerShellRuntime");
    if(runtime.at("edition").str()!="Core"||runtime.at("major").uint32()!=7||
       _wcsicmp(wide(runtime.at("executable").str()).c_str(),powerShell7Path().c_str())!=0)
        throw std::runtime_error("Readonly helper did not run in the trusted PowerShell 7 runtime");
    child.releaseLocks();
    std::printf("{\"passed\":true,\"mode\":\"installed-helper-readonly-status\",\"helperExit\":0,\"fileLocksHeldUntilExit\":true,\"powerShell7Verified\":true,\"helperOutputBytes\":%zu,\"guiShown\":false,\"gameStarted\":false,\"uacRequested\":false,\"installedFilesModified\":false}\n",output.size());
    return 0;
}
int runGui(HINSTANCE instance,bool captureRawColorOnce=false,bool presentationOff=false,bool xeFG=false,bool official=false,bool observeInputs=false,bool traceRenderTail=false,bool lowResOutput=false,bool traceAcquire=false,bool fakeSwapchain=false,bool gpuTimestamps=false,bool trackingCpuTiming=false,bool xessSr=false,bool threadCpuSampling=false){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto activation=RegisterWindowMessageW(xess_vk::launcher_instance::activationName);
    if(!activation)throw failure("Register launcher activation");
    xess_vk::launcher_instance::Gate instanceGate(xess_vk::launcher_instance::mutexName(),xess_vk::launcher_instance::windowClass,activation);
    if(!instanceGate.enter())return 0;
    App state;state.activationMessage=activation;
    state.captureRawColorOnce=captureRawColorOnce;state.presentationOff=presentationOff;state.xeFG=xeFG;
    state.official=official;state.observeInputs=observeInputs;state.traceRenderTail=traceRenderTail;
    state.lowResOutput=lowResOutput;state.traceAcquire=traceAcquire;state.fakeSwapchain=fakeSwapchain;
    state.gpuTimestamps=gpuTimestamps;state.trackingCpuTiming=trackingCpuTiming;
    state.threadCpuSampling=threadCpuSampling;state.xessSr=xessSr;
    state.installRoot=knownFolder(FOLDERID_ProgramFiles)+L"\\EndfieldXeSSObserver";
    state.sessionRoot=knownFolder(FOLDERID_ProgramData)+L"\\EndfieldXeSSObserver\\Sessions";
    state.powershell=systemPowerShell();state.theme.development=!releaseMinimal;
    state.theme.setDpi(GetDpiForSystem());state.theme.loadIcons(instance);
    if(!state.theme.icon||!state.theme.smallIcon)throw failure("Load launcher icon resource");
    state.taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc{};wc.lpfnWndProc=windowProc;wc.hInstance=instance;
    wc.lpszClassName=xess_vk::launcher_instance::windowClass;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hIcon=state.theme.icon;wc.hbrBackground=state.theme.background;
    if(!RegisterClassW(&wc))throw failure("Register launcher window");
    const int offset=releaseMinimal?0:96;
    constexpr DWORD windowStyle=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN;
    RECT bounds{0,0,state.theme.px(560),state.theme.px(552+offset)};
    if(!AdjustWindowRectExForDpi(&bounds,windowStyle,FALSE,0,GetDpiForSystem()))throw failure("Size launcher window");
    const auto title=releaseMinimal?L"终末地 · XeSS":xessSr?L"终末地 · XeSS 超分（开发）":official?L"终末地 · 官方备用入口":L"终末地 · XeSS（开发）";
    HWND window=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,title,windowStyle,
        CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,instance,&state);
    if(!window)throw failure("Create launcher window");
    SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(state.theme.icon));
    SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(state.theme.smallIcon));
    const COLORREF caption=xess_vk::launcher_ui::navy,captionText=xess_vk::launcher_ui::paper;
    DwmSetWindowAttribute(window,35,&caption,sizeof(caption));DwmSetWindowAttribute(window,36,&captionText,sizeof(captionText));
    auto control=[&](const wchar_t* klass,const wchar_t* text,int x,int y,int width,int height,int id,DWORD style=0,
                     xess_vk::launcher_ui::Font font=xess_vk::launcher_ui::Font::Body){
        if(wcscmp(klass,L"BUTTON")==0&&(style&BS_TYPEMASK)==BS_PUSHBUTTON)style|=BS_OWNERDRAW;
        DWORD extended=wcscmp(klass,L"EDIT")==0?WS_EX_CLIENTEDGE:0;
        auto handle=CreateWindowExW(extended,klass,text,WS_CHILD|WS_VISIBLE|style,state.theme.px(x),state.theme.px(y),
            state.theme.px(width),state.theme.px(height),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
        if(!handle)throw failure("Create launcher control");
        state.theme.add(handle,x,y,width,height,font);return handle;
    };
    using Font=xess_vk::launcher_ui::Font;
    namespace preferences=xess_vk::launcher_preferences;
    control(L"STATIC",L"游戏目录",30,94,300,22,0,0,Font::Section);
    state.pathControl=control(L"EDIT",L"",30,122,326,28,0,ES_READONLY|ES_AUTOHSCROLL);
    control(L"BUTTON",L"更改…",366,122,70,28,pathChangeId,WS_TABSTOP);
    control(L"BUTTON",L"重新检测",444,122,86,28,pathDetectId,WS_TABSTOP);
    state.pathStatus=control(L"STATIC",L"正在检查游戏路径…",30,155,500,20,0,0,Font::Small);
    control(L"STATIC",L"画面与启动设置",30,204,340,22,0,0,Font::Section);
    control(L"BUTTON",L"恢复默认",448,202,82,24,defaultsId,WS_TABSTOP);
    control(L"STATIC",L"帧生成倍率",30,241,80,22,0,0,Font::Small);
    state.multiplier=control(L"COMBOBOX",L"",112,236,130,150,multiplierId,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
    for(const auto label:{L"2×  ·  双倍帧率",L"3×  ·  三倍帧率",L"4×  ·  四倍帧率"})SendMessageW(state.multiplier,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
    const auto multiplier=preferences::read(preferences::multiplierPath,2,4,99);
    SendMessageW(state.multiplier,CB_SETCURSEL,(multiplier<=4?multiplier:2)-2,0);
    control(L"STATIC",L"场景切换阈值",268,241,84,22,0,0,Font::Small);
    state.sceneControl=control(L"COMBOBOX",L"",354,236,176,180,sceneId,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
    for(const auto label:{L"默认（推荐）",L"0.3",L"0.5",L"0.6",L"0.7",L"0.8",L"0.9"})SendMessageW(state.sceneControl,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
    const auto scene=preferences::read(preferences::sceneThresholdPath,0,6,99);
    SendMessageW(state.sceneControl,CB_SETCURSEL,scene<=6?scene:0,0);
    state.noDitherControl=control(L"BUTTON",L"启动时开启免虚化",30,278,220,26,noDitherId,BS_AUTOCHECKBOX|WS_TABSTOP);
    const auto noDither=preferences::read(preferences::noDitherPath,0,1,99);
    state.noDither=noDither!=0;SendMessageW(state.noDitherControl,BM_SETCHECK,state.noDither?BST_CHECKED:BST_UNCHECKED,0);
    control(L"STATIC",L"游戏内选择 AMD FSR3；\r\n开启帧生成后由 XeSS 接管。",30,311,216,34,0,0,Font::Small);
    if(multiplier==99&&GetFileAttributesW(preferences::multiplierPath)!=INVALID_FILE_ATTRIBUTES)state.preferenceWarning=L"倍率配置无效，已使用2×默认值。";
    if(scene==99&&GetFileAttributesW(preferences::sceneThresholdPath)!=INVALID_FILE_ATTRIBUTES)state.preferenceWarning+=L" 阈值配置无效，已使用SDK默认。";
    if(noDither==99&&GetFileAttributesW(preferences::noDitherPath)!=INVALID_FILE_ATTRIBUTES)state.preferenceWarning+=L" 免虚化配置无效，已使用开启默认值。";
    state.frameCapControl=control(L"BUTTON",L"将游戏的30帧档替换为",264,278,266,26,frameCapId,BS_AUTOCHECKBOX|WS_TABSTOP);
    const auto cap=preferences::read(preferences::frameCapEnabledPath,0,1,99);
    SendMessageW(state.frameCapControl,BM_SETCHECK,cap==1?BST_CHECKED:BST_UNCHECKED,0);
    if(cap==99&&GetFileAttributesW(preferences::frameCapEnabledPath)!=INVALID_FILE_ATTRIBUTES)state.preferenceWarning+=L" 30帧档替换开关无效，已关闭。";
    state.frameCapTarget=preferences::read(preferences::frameCapTargetPath,30,120,40);
    state.frameCapTargetControl=control(L"EDIT",std::to_wstring(state.frameCapTarget).c_str(),264,311,72,28,frameCapTargetId,ES_NUMBER|ES_AUTOHSCROLL|WS_TABSTOP);
    SendMessageW(state.frameCapTargetControl,EM_SETLIMITTEXT,16,0);
    control(L"STATIC",L"帧  ·  30–120",348,314,178,22,0,0,Font::Small);
    if(preferences::read(preferences::frameCapTargetPath,30,120,0)==0&&GetFileAttributesW(preferences::frameCapTargetPath)!=INVALID_FILE_ATTRIBUTES)state.preferenceWarning+=L" 替换帧数无效，已恢复40。";
    EnableWindow(state.frameCapTargetControl,xeFG&&!official&&cap==1);EnableWindow(state.frameCapControl,xeFG&&!official);
    state.diagnosticControl=control(L"BUTTON",L"诊断模式（仅本次启动记录日志）",30,374,370,24,diagnosticId,BS_AUTOCHECKBOX|WS_TABSTOP);
    control(L"BUTTON",L"打开诊断日志",414,372,116,28,logsId,WS_TABSTOP);
    if constexpr(!releaseMinimal){
        ShowWindow(state.diagnosticControl,SW_HIDE);
        control(L"BUTTON",L"高级／测试 ▸",30,430,124,28,advancedId,WS_TABSTOP);
        state.debugControl=control(L"COMBOBOX",L"",164,430,366,150,debugId,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
        for(const auto label:{L"关闭调试（默认）",L"插出帧加色块",L"插帧失败显示黑帧",L"只显示插出的帧"})SendMessageW(state.debugControl,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        SendMessageW(state.debugControl,CB_SETCURSEL,0,0);
        state.debugWarning=control(L"STATIC",L"仅用于测试；下一次启动自动关闭，不影响已保存的倍率。",30,468,500,22,0,0,Font::Small);
        ShowWindow(state.debugControl,SW_HIDE);ShowWindow(state.debugWarning,SW_HIDE);
    }
    state.status=control(L"STATIC",L"正在校验游戏路径…",18,416+offset,524,48,0,SS_EDITCONTROL,Font::Small);
    state.start=control(L"BUTTON",official?L"打开官方启动器":L"启动游戏",16,472+offset,210,38,startId,WS_TABSTOP);
    control(L"BUTTON",L"保存设置",234,472+offset,98,38,applyId,WS_TABSTOP);
    control(L"BUTTON",L"收起到托盘",340,472+offset,118,38,trayId,WS_TABSTOP);
    control(L"BUTTON",L"退出",466,472+offset,78,38,exitId,WS_TABSTOP);
    control(L"STATIC",L"设置下次启动生效  ·  关闭窗口收起到托盘，不会退出游戏",16,522+offset,528,22,0,0,Font::Small);
    EnableWindow(state.multiplier,xeFG);EnableWindow(state.sceneControl,xeFG);EnableWindow(state.debugControl,xeFG);
    EnableWindow(state.noDitherControl,!official);EnableWindow(state.start,FALSE);
    state.trayAdd();ShowWindow(window,SW_SHOW);UpdateWindow(window);state.beginPathCheck();
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    app=nullptr;return 0;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int){
    bool test=false;
    try{int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)throw failure("Parse arguments");std::vector<std::wstring> options;for(int i=1;i<count;++i)options.emplace_back(args[i]);LocalFree(args);
        const auto srCount=std::count(options.begin(),options.end(),L"--xess-sr");
        if(srCount>1)throw std::runtime_error("Duplicate XeSS SR modifier");
        bool xessSr=srCount==1;if(xessSr)options.erase(std::find(options.begin(),options.end(),L"--xess-sr"));
        if constexpr(releaseMinimal){if(options.empty()){options.push_back(L"--xefg");xessSr=true;}}
        test=std::any_of(options.begin(),options.end(),[](const auto& arg){return arg==L"--self-test"||arg==L"--check-helper-readonly";});const auto mode=launcherMode(options);
        if constexpr(releaseMinimal){if(mode!=LauncherMode::XeFG&&mode!=LauncherMode::FakeSwapchain&&mode!=LauncherMode::LowResOutput&&mode!=LauncherMode::SelfTest&&mode!=LauncherMode::Readonly)throw std::runtime_error("This release supports single-chain XeSS only; developer launch modes are unavailable");if(xeFGRoute(mode))xessSr=true;}
        if(xessSr&&(!xeFGRoute(mode)||!fakeSwapchainRoute(mode)))throw std::runtime_error("--xess-sr requires --xefg single-chain output");
        return mode==LauncherMode::SelfTest?selfTest():(mode==LauncherMode::Readonly?checkHelperReadonly():runGui(instance,mode==LauncherMode::CaptureRawColorOnce,mode==LauncherMode::PresentationOff,xeFGRoute(mode),mode==LauncherMode::Official,mode==LauncherMode::ObserveInputs,mode==LauncherMode::TraceRenderTail,lowResOutputRoute(mode),mode==LauncherMode::TraceAcquire,fakeSwapchainRoute(mode),!releaseMinimal&&xeFGRoute(mode)&&gpuTimestampsFromEnvironment(),!releaseMinimal&&xeFGRoute(mode)&&trackingCpuTimingFromEnvironment(),xessSr,!releaseMinimal&&xeFGRoute(mode)&&threadCpuSamplingFromEnvironment()));}
    catch(const std::exception& error){if(test)std::fprintf(stderr,"mod-launcher diagnostic: %s\n",error.what());else MessageBoxW(nullptr,wide(error.what()).c_str(),productName,MB_OK|MB_ICONERROR);return 1;}
}
