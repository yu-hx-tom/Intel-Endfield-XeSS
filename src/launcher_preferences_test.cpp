#include "launcher_preferences.hpp"
#include <cstdio>
#include "game_frame_cap.hpp"
#include "sr_backend.hpp"
int main(){
    using namespace xess_vk::launcher_preferences;unsigned checks{};
    auto check=[&](bool good){++checks;if(!good)throw std::runtime_error("Launcher preference check failed");};
    try {
        using xess_vk::sr_backend::Kind;
        check(xess_vk::sr_backend::parse(L"")==Kind::XeSS);
        check(xess_vk::sr_backend::parse(L"xess")==Kind::XeSS);
        check(xess_vk::sr_backend::parse(L"fsr4_xmx_lite")==Kind::Fsr4XmxLite);
        check(xess_vk::sr_backend::parse(L"fsr4")==Kind::Invalid);
        check(xess_vk::sr_backend::parse(L"fsr4_xmx_lite ")==Kind::Invalid);
        for(const auto value:{"2","3\n"," 4\r\n"})check(parse(value,2,4,0)>=2);
        for(const auto value:{"","1","5","23","2garbage","2 3"})check(parse(value,2,4,0)==0);
        for(unsigned selected=2;selected<=4;++selected)
            for(unsigned supported=1;supported<=3;++supported)
                check(generatedCapacity(selected,supported)==std::min(selected-1,supported));
        check(generatedCapacity(0,3)==1&&generatedCapacity(5,3)==1);
        unsigned setterCalls{};auto threshold=[&](float){++setterCalls;return 0;};auto debug=[&](unsigned){++setterCalls;return 0;};
        const auto defaults=xess_vk::fg_tuning::apply({},threshold,debug);
        check(!setterCalls&&!defaults.thresholdAttempted&&!defaults.debugAttempted);
        for(unsigned i=1;i<=6;++i){const auto applied=xess_vk::fg_tuning::apply({i,0,0},threshold,debug);check(applied.thresholdApplied&&!applied.debugAttempted);}
        for(unsigned i=1;i<=3;++i){const auto applied=xess_vk::fg_tuning::apply({0,i,0},threshold,debug);check(applied.debugApplied&&!applied.thresholdAttempted);}
        const auto failed=xess_vk::fg_tuning::apply({2,2,0},[](float){return -4;},[](unsigned){return -7;});
        check(!failed.thresholdApplied&&!failed.debugApplied&&failed.thresholdResult==-4&&failed.debugResult==-7);
        const auto invalid=xess_vk::fg_tuning::apply({99,99,0},threshold,debug);check(invalid.requested.invalid==3&&!invalid.thresholdAttempted&&!invalid.debugAttempted);
        xess_vk::fg_tuning::Totals totals;totals.observe(false,0,false,1,0);check(!totals.requested);
        totals.observe(true,0,true,2,0);totals.observe(true,0,true,1,3);totals.observe(true,5,false,0,0);totals.observe(true,0,false,1,0);
        check(totals.requested==4&&totals.enabled==2&&totals.multiOutput==1&&totals.singleOutput==1&&totals.queryUnavailable==1&&totals.resultWarnings==1);
        for(const auto cap:{-1,0,20,30,40,60,120,240}){
            check(xess_vk::game_frame_cap::map(cap,false,40)==cap);
            check(xess_vk::game_frame_cap::map(cap,true,40)==(cap==30?40:cap));
        }
        for(unsigned target=30;target<=120;++target){
            const auto text=std::to_string(target);check(parse(text,30,120,0)==target);
            check(xess_vk::game_frame_cap::map(30,true,target)==int(target));
            for(const auto input:{-1,0,40,60,120,240})check(xess_vk::game_frame_cap::map(input,true,target)==input);
            check(xess_vk::game_frame_cap::map(30,false,target)==30);
        }
        for(const auto invalidText:{"","29","121","150","-30","+40","45.5","3e1","1 20","1234567890"})check(parse(invalidText,30,120,0)==0);
        check(parse("02",2,4,0)==0); // Existing single-digit settings remain strict.
        for(const auto invalidTarget:{0u,29u,121u,999u})check(xess_vk::game_frame_cap::map(30,true,invalidTarget)==30);
        SetEnvironmentVariableW(L"XESS_FRAME_CAP_OVERRIDE",L"1");SetEnvironmentVariableW(L"XESS_FRAME_CAP_TARGET_FPS",nullptr);
        check(readFrameCap().requested&&!readFrameCap().invalid&&readFrameCap().target==40);
        for(const auto value:{L"30",L"45",L"50",L"120"}){SetEnvironmentVariableW(L"XESS_FRAME_CAP_TARGET_FPS",value);check(!readFrameCap().invalid);}
        SetEnvironmentVariableW(L"XESS_FRAME_CAP_TARGET_FPS",L"45");
        SetLastError(0xbeef);check(startupFrameCap().requested&&startupFrameCap().target==45&&GetLastError()==0xbeef);
        for(const auto value:{L"",L"29",L"121",L"50x",L"999999999999999999999999999",L"四十"}){SetEnvironmentVariableW(L"XESS_FRAME_CAP_TARGET_FPS",value);check(readFrameCap().invalid);}
        SetEnvironmentVariableW(L"XESS_FRAME_CAP_OVERRIDE",L"0");SetEnvironmentVariableW(L"XESS_FRAME_CAP_TARGET_FPS",L"120");
        check(!readFrameCap().requested&&readFrameCap().target==120);check(startupFrameCap().requested&&startupFrameCap().target==45);
        SetEnvironmentVariableW(L"XESS_FG_SCENE_THRESHOLD",L"2");SetEnvironmentVariableW(L"XESS_FG_DEBUG_MODE",L"3");
        check(readFgTuning().threshold==2&&readFgTuning().debug==3);
        check(startupFgTuning().threshold==2&&startupFgTuning().debug==3);
        SetEnvironmentVariableW(L"XESS_FG_SCENE_THRESHOLD",L"2x");SetEnvironmentVariableW(L"XESS_FG_DEBUG_MODE",L"4");
        check(readFgTuning().invalid==3&&readFgTuning().threshold==0&&readFgTuning().debug==0);
        check(startupFgTuning().threshold==2&&startupFgTuning().debug==3);
        wchar_t root[MAX_PATH]{},file[MAX_PATH]{};check(GetTempPathW(MAX_PATH,root)&&GetTempFileNameW(root,L"xfp",0,file));
        save(file,3);SetLastError(0xbeef);check(read(file,2,4,2)==3&&GetLastError()==0xbeef);
        const auto interrupted=std::wstring(file)+L".interrupted.tmp";save(interrupted.c_str(),4);
        check(read(file,2,4,2)==3);check(DeleteFileW(interrupted.c_str()));
        const auto overwrite=[&](const char* bytes,DWORD size){const auto f=CreateFileW(file,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);DWORD n{};check(f!=INVALID_HANDLE_VALUE);check(WriteFile(f,bytes,size,&n,nullptr)&&n==size);CloseHandle(f);};
        overwrite("x",1);check(read(file,0,6,0)==0);
        overwrite("00000000000000000",17);check(read(file,0,6,0)==0);
        overwrite("",0);check(read(file,0,6,0)==0);
        for(const auto target:{30u,40u,45u,50u,120u}){save(file,target);check(read(file,30,120,0)==target);}
        save(file,0);check(read(file,0,1,1)==0);check(DeleteFileW(file));check(read(file,2,4,2)==2);
        std::printf("{\"passed\":true,\"checks\":%u,\"gameTouched\":false}\n",checks);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
