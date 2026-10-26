#include "diagnostic_flags.hpp"
#include "launcher_preferences.hpp"
#include "game_frame_cap.hpp"
#include "cpu_wall_time.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv) {
    try {
        unsigned checks{};
        auto check=[&](bool value){++checks;if(!value)throw std::runtime_error("release profile check failed");};
        check(releaseMinimal);
        const bool diagnostics=argc==2&&std::string_view(argv[1])=="--diagnostics-on";
        const bool explicitOff=argc==2&&std::string_view(argv[1])=="--diagnostics-off";
        SetEnvironmentVariableW(L"XESS_DIAGNOSTIC_MODE",diagnostics?L"1":explicitOff?L"0":nullptr);
        SetLastError(12345);check(xess_vk::diagnostic_flags::runtimeDiagnostics()==diagnostics);check(GetLastError()==12345);
        xess_vk::CpuWallTime timing;
        {xess_vk::CpuWallScope scope(timing);}
        check(timing.count==unsigned(diagnostics));

        SetEnvironmentVariableW(L"XESS_FAKE_SWAPCHAIN",L"1");
        SetEnvironmentVariableW(L"XESS_FSR_PIPELINE_TRACKING",L"0");
        SetEnvironmentVariableW(L"XESS_FG_DEBUG_MODE",L"3");
        SetEnvironmentVariableW(L"XESS_FG_SCENE_THRESHOLD",L"2");
        namespace flags=xess_vk::diagnostic_flags;
        check(flags::fsrPipelineTracking());
        check(flags::localFiOff());
        check(flags::depthComputePath());
        check(!flags::asyncCopyPath()&&flags::sharedCopyPath());
        check(flags::copyPathSelection().inflightLimit==0);
        for(const auto name:{L"SrObservation",L"SrFullGraphObservation",L"SrFatalOnUnproven",L"KeepNativeAlgo",L"SkipInputSnapshot"}){
            SetLastError(123);check(!flags::read(name).on);check(GetLastError()==123);
        }
        const auto tuning=xess_vk::launcher_preferences::readFgTuning();
        check(tuning.debug==0&&tuning.threshold==2);
        for(unsigned fps=30;fps<=120;++fps){
            SetEnvironmentVariableW(L"XESS_FRAME_CAP_OVERRIDE",L"1");
            SetEnvironmentVariableW(L"XESS_FRAME_CAP_TARGET_FPS",std::to_wstring(fps).c_str());
            const auto cap=xess_vk::launcher_preferences::readFrameCap();
            check(cap.requested&&!cap.invalid&&cap.target==fps);
            check(xess_vk::game_frame_cap::map(30,true,cap.target)==int(fps));
            check(xess_vk::game_frame_cap::map(60,true,cap.target)==60);
        }
        std::cout<<"{\"passed\":true,\"release_profile_checks\":"<<checks<<"}\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
