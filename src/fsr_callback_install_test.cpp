#include "fsr_callback_install.hpp"
#include "fsr_detour_entry.hpp"
#include "upscale_detour_entry.hpp"
#include "fsr_menu_control.hpp"
#include "fsr_local_fi_off.hpp"
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "diagnostic_flags.hpp"

namespace xess_vk::fsr_callback_install_fixture_flags {
bool localFiOffEnabled{};
bool srObservationEnabled{};
bool srNativeObservationEnabled{};
bool localFiOff() noexcept { return localFiOffEnabled; }
bool srObservation() noexcept {return srObservationEnabled;}
bool srReplaceRequested() noexcept {return false;}
bool srFullGraphObservation() noexcept {return srNativeObservationEnabled;}
bool srNativeReplacementAvailable() noexcept {return false;}
bool srFatalOnUnproven() noexcept {return false;}
}
// Keep real files/registry out of this guard fixture, including later flag defaults.
// Include the installer in this TU, as the layer/native fixtures do for their stubs.
#define diagnostic_flags fsr_callback_install_fixture_flags
#include "fsr_callback_install.cpp"
#undef diagnostic_flags

namespace {
std::vector<std::string> lines;
unsigned initializeCalls{}, createCalls{}, enableCalls{}, sinkCalls{};
bool throwSink{}, reenterSink{};
constexpr DWORD sentinel = 0x17012026;
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
void sink(const char* line) {
    ++sinkCalls;
    SetLastError(98765);
    if (throwSink) throw std::runtime_error("test diagnostic failure");
    if (reenterSink) {
        xess_vk::fsr_callback_install::summary();
        xess_vk::fsr_callback_install::installAtDeviceCreation(0x987, sink);
        xess_vk::fsr_callback_install::onDeviceDestroyed(0x123);
    }
    lines.emplace_back(line);
}
void sourceSink(std::uint64_t, std::uint64_t, const xess_vk::upscale_probe::Result&, bool) {
    throw std::runtime_error("guard path unexpectedly sampled source metadata");
}
void invocationSink(std::uint64_t, std::uint64_t, std::uintptr_t, std::uintptr_t,
                    const xess_vk::upscale_probe::Result&, bool) {
    throw std::runtime_error("unverified profile delivered an invocation");
}
bool timingSink(void*, const xess_vk::game_timing::Event&) {
    throw std::runtime_error("unverified profile delivered timing");
}
xess_vk::FrameTimingAssociation frameReader(void*, std::uint32_t) {
    throw std::runtime_error("unverified profile read a frame");
}
bool enqueueSink(void*, const xess_vk::game_timing::EnqueueEvent&) {
    throw std::runtime_error("unverified profile delivered an enqueue");
}
void srImages(std::uint64_t,const xess_vk::upscale_tail_probe::Result&,xess_vk::sr_observation::Images&) noexcept {}
void boundary(std::uint64_t device, bool source = false, bool continuous = false, bool admission = false,bool srResolver=false) {
    SetLastError(sentinel);
    xess_vk::fsr_callback_install::installAtDeviceCreation(device, sink, source ? sourceSink : nullptr,
        continuous ? invocationSink : nullptr, continuous ? timingSink : nullptr,
        &lines, continuous ? frameReader : nullptr,
        continuous ? enqueueSink : nullptr,admission,srResolver?srImages:nullptr);
    require(GetLastError() == sentinel, "device boundary changed LastError");
}
void summary() {
    SetLastError(sentinel);
    xess_vk::fsr_callback_install::summary();
    require(GetLastError() == sentinel, "summary changed LastError");
}
void destroyed(std::uint64_t device) {
    SetLastError(sentinel);
    xess_vk::fsr_callback_install::onDeviceDestroyed(device);
    require(GetLastError() == sentinel, "destroy changed LastError");
}
bool contains(const char* needle) { return !lines.empty() && lines.back().find(needle) != std::string::npos; }
void srTrampolineRoutes() {
    namespace install = xess_vk::fsr_callback_install;
    namespace transaction = xess_vk::fsr_hook_transaction;
    install::timingSpecs();
    struct OwnedCode {
        unsigned char* bytes = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        ~OwnedCode() { if (bytes) VirtualFree(bytes, 0, MEM_RELEASE); }
    } code;
    require(code.bytes != nullptr, "allocate nonexecuted SR trampoline bytes");
    const auto saved = install::states;
    const auto jump = [](std::uintptr_t address, std::uintptr_t destination) {
        constexpr std::array<unsigned char, 6> opcode{0xff,0x25,0,0,0,0};
        std::memcpy(reinterpret_cast<void*>(address), opcode.data(), opcode.size());
        std::memcpy(reinterpret_cast<void*>(address + opcode.size()), &destination, sizeof(destination));
    };
    for (std::size_t index = transaction::srFirstIndex; index < transaction::maximumTargetCount; ++index) {
        auto& state = install::states[index]; const auto& spec = install::specs[index];
        state.trampoline = reinterpret_cast<std::uintptr_t>(code.bytes + (index - transaction::srFirstIndex) * 64);
        state.target = state.trampoline + 1024; state.body = state.trampoline + 2048;
        const auto copied = spec.ordinary ? spec.copiedLength : 0;
        state.relay = state.trampoline + copied + 14;
        if (copied) std::memcpy(reinterpret_cast<void*>(state.trampoline), spec.thunkBytes.data(), copied);
        jump(state.trampoline + copied, spec.ordinary ? state.target + copied : state.body);
        jump(state.relay, reinterpret_cast<std::uintptr_t>(spec.entry));
    }
    DWORD previous{};
    require(VirtualProtect(code.bytes, 4096, PAGE_EXECUTE_READ, &previous) != FALSE, "protect nonexecuted SR trampoline bytes");
    for (std::size_t index = transaction::srFirstIndex; index < transaction::maximumTargetCount; ++index) {
        auto& state = install::states[index];
        require(install::trampolineMatches(index), "SR trampoline passes before helper configuration");
        state.configured = true;
        require(install::trampolineMatches(index), "configured SR trampoline retains its own production guard");
        auto& destination = install::specs[index].ordinary ? state.target : state.body;
        ++destination;
        require(!install::trampolineMatches(index), "configured SR trampoline refuses wrong original destination");
        --destination; ++state.relay;
        require(!install::trampolineMatches(index), "configured SR trampoline refuses wrong relay position");
        --state.relay;
    }
    for (std::size_t index = transaction::localFiFirstIndex; index < transaction::localFiTargetCount; ++index) {
        install::states[index].configured = true;
        require(!install::trampolineMatches(index), "configured LocalFi still requires its site certificate");
    }
    install::states = saved;
}
void frameCapControl(){
    namespace install=xess_vk::fsr_callback_install;
    const auto savedShared=install::shared;
    install::Attempt attempt;
    IMAGE_SECTION_HEADER section{};std::memcpy(section.Name,".text",5);
    section.VirtualAddress=0x1000;section.Misc.VirtualSize=0x2000;section.SizeOfRawData=0x2000;section.PointerToRawData=0x600;
    section.Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE;
    attempt.liveHeaders.sections.push_back(section);attempt.diskHeaders.sections.push_back(section);
    install::shared.transaction.completed=true;
    static unsigned optionalInstalls;optionalInstalls=0;
    install::finishAttempt(attempt,+[](install::Attempt& live){
        std::uint64_t raw{};++optionalInstalls;
        require(install::textRange(live.liveHeaders,0x1100,16,raw)&&raw==0x700,"optional FPS hook verifies original live PE metadata before ownership transfer");
        require(live.diskHeaders.sections.size()==1,"optional FPS hook retains locked-file PE metadata");
    });
    require(optionalInstalls==1&&install::shared.liveHeaders.sections.size()==1&&attempt.liveHeaders.sections.empty(),"completed transaction publishes metadata only after optional hooks");
    install::shared.transaction.completed=false;
    install::finishAttempt(attempt,+[](install::Attempt&){++optionalInstalls;});
    require(optionalInstalls==1,"failed main hook transaction must never install optional FPS hook");
    install::shared=savedShared;
    using xess_vk::fsr_callback_install::gameSiblingForPath;
    require(gameSiblingForPath(L"E:\\My Games\\Endfield.exe",L"UnityPlayer.dll")==L"E:\\My Games\\UnityPlayer.dll","custom local game path retains sibling module boundary");
    for(const auto* path:{L"D:\\test\\other.exe",L"\\\\server\\game\\Endfield.exe",L"relative\\Endfield.exe"})require(gameSiblingForPath(path,L"UnityPlayer.dll").empty(),"non-game or network process cannot admit Unity hooks");
    namespace cap=xess_vk::fsr_callback_install::frameCap;
    namespace facts=xess_vk::game_frame_cap;
    static int forwarded;static unsigned count;cap::targetFps=40;
    cap::original=+[](int value){forwarded=value;++count;SetLastError(sentinel);};
    cap::get=+[]{SetLastError(123);return forwarded;};
    for(const auto value:{30,60,0,-1,120,30}){
        cap::entry(value);require(forwarded==(value==30?40:value),"only the 30 FPS slot is changed");
        require(GetLastError()==sentinel&&facts::readback==forwarded,"engine result and last-error preserved");
    }
    require(count==6&&facts::calls==6&&facts::remapped==2,"cap remaps counted independently from FG outputs");
    for(const auto target:{30u,45u,50u,120u}){
        cap::targetFps=target;cap::entry(30);require(forwarded==int(target)&&facts::readback==int(target),"selected target reaches the engine setter and getter");
        cap::entry(60);require(forwarded==60,"60 FPS request remains unchanged for every target");
    }
    cap::targetFps=40;
}
void nativeResolutionControl(){
    namespace nr=xess_vk::fsr_callback_install::nativeResolution;
    namespace nc=xess_vk::sr_native_resolution;
    static std::array<unsigned char,0xb10> camera;
    static std::array<unsigned char,0x450> settings;
    static std::array<unsigned char,0x40> scale;
    static float quality;static unsigned calls,setCalls;
    camera={};settings={};scale={};quality=0.5882353f;calls=setCalls=0;
    const auto put=[](auto& bytes,std::size_t offset,auto value){std::memcpy(bytes.data()+offset,&value,sizeof(value));};
    const auto get=[]<class T>(const auto& bytes,std::size_t offset){T v;std::memcpy(&v,bytes.data()+offset,sizeof(v));return v;};
    put(settings,0x440,reinterpret_cast<std::uintptr_t>(scale.data()));put(scale,0x28,quality);
    nr::original=+[](void* self,void* config,std::uint64_t,const void*){
        require(self==camera.data()&&config==settings.data(),"engine callback preserves object arguments");
        std::uint32_t aa;std::memcpy(&aa,camera.data()+0x96c,4);
        if(aa&0x40)std::memcpy(scale.data()+0x28,&quality,4);++calls;SetLastError(sentinel);
    };
    nr::setScale=+[](void* self,float v,const void* mi){require(self==camera.data()&&v==1.f&&!mi,"native scale uses the engine method on its natural callback");std::memcpy(scale.data()+0x28,&v,4);++setCalls;};
    const std::uint64_t hd=(std::uint64_t(1080)<<32)|1920,qhd=(std::uint64_t(1440)<<32)|2560;
    nc::control.reason=nc::Reason::None;nc::control.applied=0;nc::control.recoveries=0;nc::control.thread=0;nr::observedRecovery=0;nr::applyRetries=0;nr::appliedCamera=0;
    put(camera,0x96c,std::uint32_t(0x40));put(camera,0x66c,hd);
    nr::entry(camera.data(),settings.data(),hd,nullptr);
    require(calls==1&&!setCalls&&!nc::control.applied&&get.template operator()<std::uint32_t>(camera,0x96c)==0x40,"normal XeSS route preserves the game's quality and AA controls");
    nc::control.request(nc::Reason::InputUnavailable);nc::control.request(nc::Reason::SdkFailure);
    put(camera,0x66c,std::uint64_t(640)|(std::uint64_t(360)<<32));
    nr::entry(camera.data(),settings.data(),hd,nullptr);
    require(nc::control.applied==1&&nc::control.configuredExtent==hd&&nc::control.reason==nc::Reason::InputUnavailable&&
        get.template operator()<std::uint32_t>(camera,0x96c)==0&&setCalls==1&&GetLastError()==sentinel,"fallback overrides dynamic size and scale before allocation, retains first cause and engine LastError");
    put(camera,0x96c,std::uint32_t(0x40));nr::entry(camera.data(),settings.data(),qhd,nullptr);
    require(nc::control.applied==2&&nc::control.requested()&&!nc::control.recoveries&&setCalls==1,"target/scale changes cannot erase a request without retirement evidence");
    put(camera,0x96c,std::uint32_t(2));nr::entry(camera.data(),settings.data(),hd,nullptr);
    require(nc::control.applied==2&&get.template operator()<std::uint32_t>(camera,0x96c)==2,"unrelated non-FSR camera is not modified");
    nc::control.reason=nc::Reason::None;quality=1.f;put(camera,0x96c,std::uint32_t(0x40));
    nr::entry(camera.data(),settings.data(),hd,nullptr);require(nc::control.applied==2&&!nc::control.requested()&&get.template operator()<std::uint32_t>(camera,0x96c)==0x40,"native-AA selection preserves temporal AA rather than disabling antialiasing");
    quality=0.6666667f;put(camera,0x96c,std::uint32_t(0x40));nr::entry(camera.data(),settings.data(),hd,nullptr);
    require(nc::control.applied==2&&get.template operator()<std::uint32_t>(camera,0x96c)==0x40,"switching from native AA to an upscale quality returns to XeSS");
    // Reentrant scale notifications call only the real engine function in the nested entry.
    nc::control.request(nc::Reason::InputUnavailable);quality=0.6666667f;
    nr::appliedCamera=0;nr::setScale=+[](void* self,float,const void*){
        nr::entry(self,settings.data(),(std::uint64_t(1080)<<32)|1920,nullptr);
        const float one=1.f;std::memcpy(scale.data()+0x28,&one,4);
    };
    const auto entered=nc::control.reentries.load();put(camera,0x96c,std::uint32_t(0x40));
    nr::entry(camera.data(),settings.data(),hd,nullptr);
    require(nc::control.reentries==entered+1&&!nr::applying,"scale notification reentry is counted and cannot recursively apply a second override");
    // An engine changing scale during notification is an ordinary failed attempt, never a process exit.
    nr::setScale=+[](void*,float,const void*){};nr::applyRetries=0;nr::appliedCamera=0;
    const auto failures=nc::control.failures.load();const auto applied=nc::control.applied.load();
    for(unsigned n=0;n<4;++n){put(camera,0x96c,std::uint32_t(0x40));nr::entry(camera.data(),settings.data(),hd,nullptr);}
    require(nc::control.failures==failures+3&&nr::applyRetries==3&&nc::control.applied==applied,
        "readback mismatch retries three engine frames then refuses, without claiming native success or terminating");
    const auto other=nc::control.foreignThreads.load();
    const auto thread=CreateThread(nullptr,0,+[](void*)->DWORD{
        xess_vk::fsr_callback_install::nativeResolution::entry(camera.data(),settings.data(),(std::uint64_t(1080)<<32)|1920,nullptr);return 0;
    },nullptr,0,nullptr);
    require(thread&&WaitForSingleObject(thread,5000)==WAIT_OBJECT_0,"second callback-thread fixture completes");CloseHandle(thread);
    require(nc::control.foreignThreads==other+1&&nc::control.applied==applied,"a second callback thread forwards only the engine original and never takes ownership");
    require(!nc::validExtent(0)&&!nc::validExtent(32769ull|(720ull<<32))&&nc::validExtent(qhd),"invalid target sizes never become native configuration");
}

}

// Instrument only guard paths: any attempted MinHook action is an independent
// test failure. This EXE neither loads MinHook nor patches even its own code.
extern "C" {
void* fsr_original_trampoline = nullptr;
void fsr_observer_entry() {}
void* upscale_original_trampoline = nullptr;
void upscale_observer_entry() {}
MH_STATUS WINAPI MH_Initialize() { ++initializeCalls; return MH_ERROR_MEMORY_ALLOC; }
MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) { ++createCalls; return MH_ERROR_UNSUPPORTED_FUNCTION; }
MH_STATUS WINAPI MH_EnableHook(LPVOID) { ++enableCalls; return MH_ERROR_NOT_CREATED; }
const char* WINAPI MH_StatusToString(MH_STATUS) { return "TEST_UNEXPECTED_STATUS_QUERY"; }
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "pass one installer guard mode");
        const std::string mode = argv[1];
        if(mode=="native_resolution"){frameCapControl();nativeResolutionControl();std::cout<<"{\"passed\":true,\"mode\":\"native_resolution\",\"gpuTouched\":false}\n";return 0;}
        const bool nativeSr=mode=="current_sr_native"||mode=="current_local_fi_sr_native";
        const bool sr=nativeSr||mode=="current_sr"||mode=="current_local_fi_sr"||mode=="current_sr_missing_resolver";
        const bool localFi = mode == "current_local_fi" || mode=="current_local_fi_sr" || mode=="current_local_fi_sr_native" || mode == "current_local_fi_denied" ||
                             mode == "current_local_fi_missing_callbacks";
        xess_vk::fsr_callback_install_fixture_flags::localFiOffEnabled = localFi;
        xess_vk::fsr_callback_install_fixture_flags::srObservationEnabled=sr;
        xess_vk::fsr_callback_install_fixture_flags::srNativeObservationEnabled=nativeSr;
        require(!GetModuleHandleW(L"UnityPlayer.dll"), "guard EXE unexpectedly contains UnityPlayer");
        if (sr) srTrampolineRoutes();
        require(SetEnvironmentVariableW(L"XESS_FSR_OBSERVER", nullptr) != FALSE, "clear opt-in");
        require(SetEnvironmentVariableW(L"XESS_FSR_UPSCALE_OBSERVER", nullptr) != FALSE, "clear upscale opt-in");
        require(SetEnvironmentVariableW(L"XESS_CONTINUOUS_INPUTS", nullptr) != FALSE, "clear continuous opt-in");
        require(SetEnvironmentVariableW(L"XESS_FSR_NATIVE_OFF", nullptr) != FALSE, "clear suppression opt-in");
        for (const auto* name : {L"XESS_BRIDGE_DEVICE", L"XESS_PRESENTATION_OFF", L"XESS_RAWCOLOR_CAPTURE",L"XESS_COMPUTE_OBSERVER",L"XESS_SL_OBSERVER",L"XESS_SDK_BIN"})
            require(SetEnvironmentVariableW(name, nullptr) != FALSE, "clear mutation opt-in");
        if(mode=="current_continuous"||mode=="current_denied"||mode=="current_incomplete"||mode=="current_missing_callbacks"||localFi||sr) {
            const bool denied = mode == "current_denied" || mode == "current_local_fi_denied";
            const bool missingCallbacks = mode == "current_missing_callbacks" || mode == "current_local_fi_missing_callbacks";
            for(const auto* name:{L"XESS_FSR_UPSCALE_OBSERVER",L"XESS_CONTINUOUS_INPUTS",L"XESS_FSR_NATIVE_OFF",L"XESS_BRIDGE_DEVICE",L"XESS_PRESENTATION_OFF"})
                require(SetEnvironmentVariableW(name,L"1")!=FALSE,"current continuous flags");
            require(SetEnvironmentVariableW(L"XESS_SDK_BIN",L"C:\\owned-fixture")!=FALSE,"fixed SDK path fixture");
            if(mode=="current_incomplete")SetEnvironmentVariableW(L"XESS_BRIDGE_DEVICE",nullptr);
            SetLastError(sentinel);
            require(xess_vk::fsr_callback_install::currentContinuousRequested()==(mode!="current_incomplete"),"current exact flags contract");
            if(mode=="current_continuous"||mode=="current_local_fi") {
                for(const auto* name:{L"XESS_FSR_OBSERVER",L"XESS_RAWCOLOR_CAPTURE",L"XESS_COMPUTE_OBSERVER",L"XESS_SL_OBSERVER"}) {
                    SetEnvironmentVariableW(name,L"1");
                    require(!xess_vk::fsr_callback_install::currentContinuousRequested(),"current profile accepted a forbidden side route");
                    SetEnvironmentVariableW(name,nullptr);
                }
            }
            SetLastError(sentinel);
            require(!xess_vk::fsr_callback_install::currentContinuousProfileReady()&&GetLastError()==sentinel,
                "own process profile preflight refuses before loading modules and preserves error");
            boundary(0x123,true,!missingCallbacks,!denied,sr&&mode!="current_sr_missing_resolver");
            if (localFi) {
                require(lines.size()==(nativeSr?30:sr?18:14),"local FI/SR mode requests its exact non-FI targets");
                const auto localSites = std::count_if(lines.begin(), lines.end(), [](const auto& line) {
                    return line.find("\"event\":\"fsr_local_fi_off_install\"") != std::string::npos;
                });
                require(localSites==2,"local FI mode requests exactly its two additional decisions");
            } else require(lines.size()==(nativeSr?28:sr?16:12),"current/SR mode requests its exact non-FI targets");
            const auto* reason=mode=="current_sr_missing_resolver"?"refused_sr_observation_image_resolver":
                (mode=="current_continuous"||mode=="current_local_fi"||sr)?"refused_process_path":denied?
                "refused_current_continuous_device_admission":"refused_current_continuous_contract";
            for(const auto& line:lines)require(line.find(reason)!=std::string::npos&&line.find("\"event\":\"fsr_observer_install\"")==std::string::npos,
                "current refusal touches no old FI target");
            if(mode=="current_continuous")require(contains("\"transaction_requested_mask\":8190"),"current transaction did not omit FI");
            if(mode=="current_local_fi")require(contains("\"transaction_requested_mask\":32766"),"local FI transaction did not request its exact current-profile mask");
            if(sr&&mode!="current_sr_missing_resolver"){
                namespace ft=xess_vk::fsr_hook_transaction;
                const auto mask=nativeSr?(localFi?ft::localFiSrNativeObservationMask:ft::srNativeObservationMask):(localFi?ft::localFiSrObservationMask:ft::srObservationMask);
                require(contains(("\"transaction_requested_mask\":"+std::to_string(mask)).c_str()),"SR transaction requests only its exact selected routes");
            }
            require(!xess_vk::fsr_callback_install::currentContinuousReady(0x123),"refused installation admitted a hidden surface");
        } else if (mode == "continuous_unverified") {
            for (const auto* name : {L"XESS_FSR_OBSERVER", L"XESS_FSR_UPSCALE_OBSERVER",
                                     L"XESS_CONTINUOUS_INPUTS", L"XESS_FSR_NATIVE_OFF"})
                require(SetEnvironmentVariableW(name, L"1") != FALSE, "complete continuous opt-ins");
            boundary(0x123, true, true);
            require(lines.size() == 13, "unverified profile did not refuse all thirteen requested sites");
            for (const auto& line : lines) {
                require(line.find("refused_unity_1_6_input_layout_unverified") != std::string::npos,
                        "unverified profile did not report its fixed readiness reason");
                for (const auto* field : {"\"attempted\":false", "\"hook_created\":false", "\"enable_called\":false", "\"modules_pinned\":false"})
                    require(line.find(field) != std::string::npos, "readiness refusal reached hook preparation");
            }
        } else if (mode == "upscale_conflict" || mode == "upscale_callback_conflict") {
            require(SetEnvironmentVariableW(L"XESS_FSR_UPSCALE_OBSERVER", L"1") != FALSE, "upscale opt-in");
            if (mode == "upscale_conflict")
                require(SetEnvironmentVariableW(L"XESS_PRESENTATION_OFF", L"1") != FALSE, "conflicting presentation opt-in");
            boundary(0x123, true, mode == "upscale_callback_conflict");
            require(lines.size()==1 && contains("refused_upscale_observation_conflict"), "pure observation accepted mutation/callbacks");
            require(contains("\"attempted\":false"), "conflict reached module preparation");
        } else if (mode == "upscale_only" || mode == "dual_refused" || mode == "missing_upscale_consumer") {
            require(SetEnvironmentVariableW(L"XESS_FSR_UPSCALE_OBSERVER", L"1") != FALSE, "upscale opt-in");
            if (mode != "upscale_only") require(SetEnvironmentVariableW(L"XESS_FSR_OBSERVER", L"1") != FALSE, "dual opt-in");
            boundary(0x123, mode != "missing_upscale_consumer");
            require(lines.size() == (mode == "upscale_only" ? 1 : 2), "dual/refusal event count");
            require(contains("\"event\":\"upscale_observer_install\""), "missing independent upscale event");
            require(contains(mode == "missing_upscale_consumer" ? "refused_missing_upscale_source_sink" : "refused_process_path"), "wrong early refusal");
            if (mode == "upscale_only") require(contains("\"transaction_requested_mask\":2"), "pure upscale did not select only target index one");
            require(!xess_vk::fsr_callback_observer::enableSampling() && !xess_vk::upscale_callback_observer::enableSampling(),
                "transaction refusal left sampling activatable");
            summary(); require(contains("upscale_observer_install_summary"), "independent summary missing");
            boundary(0x456, true); require(contains("stopped_additional_device_creation"), "dual lifecycle stop missing");
        } else if (mode == "disabled" || mode == "invalid") {
            if (mode == "invalid") require(SetEnvironmentVariableW(L"XESS_FSR_OBSERVER", L"01") != FALSE, "set invalid value");
            boundary(0x123); summary();
            require(lines.empty() && sinkCalls == 0, "default-off/invalid emitted events");
            require(SetEnvironmentVariableW(L"XESS_FSR_OBSERVER", L"1") != FALSE, "set late opt-in");
            boundary(0x456); destroyed(0x123); summary();
            require(lines.empty() && sinkCalls == 0, "late opt-in retried or emitted disabled events");
        } else {
            require(mode == "refused" || mode == "repeated" || mode == "throwing" || mode == "reentry", "unknown mode");
            require(SetEnvironmentVariableW(L"XESS_FSR_OBSERVER", L"1") != FALSE, "set opt-in");
            throwSink = mode == "throwing"; reenterSink = mode == "reentry";
            boundary(0x123);
            if (throwSink) {
                require(sinkCalls == 1 && lines.empty(), "throwing sink was not contained");
                throwSink = false; summary();
                require(contains("\"trace_failures\":1"), "throwing sink failure not counted");
            } else require(sinkCalls == 1 && lines.size() == 1, "install event/reentry unexpected");
            require(contains("\"state\":\"refused_process_path\""), "own EXE was not refused before module lookup");
            require(contains("\"device_creation_boundaries\":1"), "first/reentrant boundary count incorrect");
            for (const auto* field : std::array{"\"mh_initialize\":null", "\"mh_create\":null", "\"mh_enable\":null",
                "\"hook_created\":false", "\"enable_called\":false", "\"modules_pinned\":false"})
                require(contains(field), "refusal incorrectly reports a hook or MinHook action");
            reenterSink = false;
            if (mode == "repeated") {
                boundary(0x456);
                require(contains("\"state\":\"stopped_additional_device_creation\""), "second device not stopped");
                require(contains("\"device_creation_boundaries\":2"), "second boundary count wrong");
                require(contains("\"stop_requested\":true"), "second device did not permanently stop sampling");
            }
            const auto before = lines.size(); destroyed(0x999);
            require(lines.size() == before, "unrelated device destroy logged or stopped selected device");
            destroyed(0x123);
            require(contains("\"state\":\"stopped_selected_device_destroyed\""), "selected device destruction not reported");
            summary(); require(contains("\"event\":\"fsr_observer_install_summary\""), "summary event name mismatch");
        }
        require(initializeCalls == 0 && createCalls == 0 && enableCalls == 0, "identity guard called MinHook");
        require(fsr_original_trampoline == nullptr, "identity guard published forwarding state");
        require(upscale_original_trampoline == nullptr, "identity guard published upscale forwarding state");
        require(!xess_vk::fsr_menu_control::snapshot().bothGatesReady,"refusal activated native FI suppression");
        require(!xess_vk::fsr_local_fi_off::active() && !xess_vk::fsr_local_fi_off::activate(),
                "refused/default-off installation cannot publish a local FI certificate");
        require(!GetModuleHandleW(L"UnityPlayer.dll"), "guard loaded a game module");
        std::cout << "{\"passed\":true,\"mode\":\"" << mode << "\",\"minHookActions\":0,\"minHookActionStubs\":true,"
                     "\"gameModuleLoaded\":false,\"gamePatched\":false,\"lastErrorPreserved\":true,"
                     "\"diagnosticFlagsMocked\":true,\"localFiOffRequested\":" << (localFi ? "true" : "false") << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
