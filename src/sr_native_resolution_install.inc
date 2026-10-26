// Included in fsr_callback_install.cpp after the common PE/file guards.
namespace nativeResolution {
constexpr char assemblyHash[]="ab00f1c41e13edf9816f142b166e3c2536d5bc7707992c39e5ed52d1cc888e2c";
constexpr std::uintptr_t updateRva=0x50f63f0,scaleRva=0xbfb466c;
using Update=void(*)(void*,void*,std::uint64_t,const void*);
using SetScale=void(*)(void*,float,const void*);
Update original{};SetScale setScale{};void* target{};HMODULE module{};
bool created{};
thread_local bool applying{};
std::uint64_t observedRecovery{};unsigned applyRetries{};
std::uintptr_t appliedCamera{};
std::int32_t readGameplay() noexcept {
    ErrorGuard error;
    if(!module)return gameplay_state::unknown;
    return gameplay_state::read(reinterpret_cast<std::uintptr_t>(module)+gameplay_state::typeSlotRva,
        [](std::uintptr_t address,auto& value){return readObject(address,value);});
}
void failedApply(const char* reason) noexcept {
    ++sr_native_resolution::control.failures;++applyRetries;
    try{if(auto sink=output.load()){std::ostringstream log;
        log<<"{\"event\":\"sr_engine_native_apply_failed\",\"schema\":2,\"reason\":\""<<reason
            <<"\",\"attempt\":"<<applyRetries<<",\"retry_exhausted\":"<<(applyRetries>=3?"true":"false")<<"}";sink(log.str().c_str());}}catch(...){}
    if(diagnostic_flags::srFatalOnUnproven())TerminateProcess(GetCurrentProcess(),0xE0550003u);
}
void entry(void* camera,void* settings,std::uint64_t targetSize,const void* method){
    auto& control=sr_native_resolution::control;
    if(applying){++control.reentries;original(camera,settings,targetSize,method);return;}
    const auto thread=GetCurrentThreadId();unsigned first{};control.thread.compare_exchange_strong(first,thread);
    if(control.thread.load()!=thread){++control.foreignThreads;original(camera,settings,targetSize,method);return;}
    applying=true;struct Exit{~Exit(){applying=false;}} exit;
    if(control.recoveries.load()!=observedRecovery){observedRecovery=control.recoveries.load();applyRetries=0;appliedCamera=0;}
    std::uint32_t aa{};std::uintptr_t scaleObject{};float scale{};
    const auto address=reinterpret_cast<std::uintptr_t>(camera);
    const bool known=camera&&settings&&readObject(address+0x96c,aa)&&
        readObject(reinterpret_cast<std::uintptr_t>(settings)+0x440,scaleObject)&&scaleObject;
    original(camera,settings,targetSize,method);
    ErrorGuard error;
    if(!known||!(aa&0x40)||!sr_native_resolution::validExtent(targetSize)||!readObject(scaleObject+0x28,scale))return;
    // Scale drift (including dynamic resolution) never grants recovery or clears content history.
    // Scale 1 alone never changes AA controls or qualifies an unknown Native AA shader.
    if(!control.requested()){appliedCamera=0;return;}
    if(applyRetries>=3)return;
    const std::uint32_t none=0;SIZE_T written{};
    if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address+0x96c),&none,sizeof(none),&written)||written!=sizeof(none)){failedApply("disable_fsr");return;}
    if(scale!=1.0f){
        if(appliedCamera!=address)setScale(camera,1.0f,nullptr);
        else {const float one=1.f;
            if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(scaleObject+0x28),&one,sizeof(one),&written)||written!=sizeof(one)){failedApply("retain_native_scale");return;}}
    }
    if(!WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address+0x66c),&targetSize,sizeof(targetSize),&written)||written!=sizeof(targetSize)){failedApply("restore_target_extent");return;}
    std::uint32_t appliedAa{};std::uint64_t appliedSize{};float appliedScale{};
    if(!readObject(address+0x96c,appliedAa)||!readObject(address+0x66c,appliedSize)||!readObject(scaleObject+0x28,appliedScale)||
       appliedAa||appliedScale!=1.0f||appliedSize!=targetSize){failedApply("native_configuration_readback");return;}
    applyRetries=0;appliedCamera=address;control.targetExtent.store(targetSize);control.configuredExtent.store(appliedSize);
    const auto count=++control.applied;
    if(count==1||count%1800==0)try{if(auto sink=output.load(std::memory_order_acquire)){
        std::ostringstream log;log<<"{\"event\":\"sr_engine_native_applied\",\"schema\":2,\"updates\":"<<count
            <<",\"reason\":\""<<sr_native_resolution::name(control.reason.load())
            <<"\",\"width\":"<<std::uint32_t(targetSize)<<",\"height\":"<<std::uint32_t(targetSize>>32)
            <<",\"render_scale\":1,\"camera_antialiasing\":0,\"engine_thread\":"<<thread
            <<",\"gpu_resource_dimensions_verified\":false}";sink(log.str().c_str());}}catch(...){}
}
bool prepare(){
    const auto assemblyPath=gameSibling(L"GameAssembly.dll");
    module=GetModuleHandleW(L"GameAssembly.dll");if(assemblyPath.empty()||!module||!samePath(module,assemblyPath.c_str()))return false;
    File file;file.value=CreateFileW(assemblyPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    LARGE_INTEGER size{};if(file.value==INVALID_HANDLE_VALUE||!GetFileSizeEx(file.value,&size)||size.QuadPart!=320407528||
       !hashMatches(file.value,size.QuadPart,assemblyHash))return false;
    const auto base=reinterpret_cast<std::uintptr_t>(module);
    constexpr std::array<unsigned char,16> updateBytes{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x57,0x48,0x81,0xec,0x80,0x00};
    constexpr std::array<unsigned char,5> scaleBytes{0x48,0x83,0xec,0x38,0x48};
    constexpr std::array<unsigned char,4> gameplayCompare{0x83,0xf8,0x28,0x75};
    constexpr std::array<unsigned char,11> stateRead{0x8b,0x41,0x20,0x48,0x83,0xc4,0x28,0xc3,0xcc,0xcc,0xcc};
    if(!codeRange(base+updateRva,32,module)||!codeRange(base+scaleRva,32,module)||!bytesEqual(base+updateRva,updateBytes)||!bytesEqual(base+scaleRva,scaleBytes)||
       !codeRange(base+0x5eaf24d,4,module)||!bytesEqual(base+0x5eaf24d,gameplayCompare)||
       !codeRange(base+0x5eaf33b,stateRead.size(),module)||!bytesEqual(base+0x5eaf33b,stateRead))return false;
    HMODULE pin{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(module),&pin))return false;
    target=reinterpret_cast<void*>(base+updateRva);setScale=reinterpret_cast<SetScale>(base+scaleRva);
    void* trampoline{};if(MH_CreateHook(target,reinterpret_cast<void*>(&entry),&trampoline)!=MH_OK||!trampoline)return false;
    original=reinterpret_cast<Update>(trampoline);created=true;return true;
}
bool activate(){
    if(!created||MH_EnableHook(target)!=MH_OK)return false;
    sr_native_resolution::control.installed.store(true);return true;
}
}
