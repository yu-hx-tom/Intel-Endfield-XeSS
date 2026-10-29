#pragma once
#include "build_flavor.hpp"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include "fg_tuning.hpp"

namespace xess_vk::launcher_preferences {
inline constexpr wchar_t multiplierPath[]=L"C:\\ProgramData\\EndfieldXeSSDiag\\FgMultiplier";
inline constexpr wchar_t noDitherPath[]=L"C:\\ProgramData\\EndfieldXeSSDiag\\NoDitherEnabled";
inline constexpr wchar_t sceneThresholdPath[]=L"C:\\ProgramData\\EndfieldXeSSDiag\\FgSceneThreshold";
// Retain the old enabled marker; missing numeric preference migrates to 40.
inline constexpr wchar_t frameCapEnabledPath[]=L"C:\\ProgramData\\EndfieldXeSSDiag\\FrameCap30To40";
inline constexpr wchar_t frameCapTargetPath[]=L"C:\\ProgramData\\EndfieldXeSSDiag\\FrameCapTargetFps";
inline constexpr wchar_t srBackendPath[]=L"C:\\ProgramData\\EndfieldXeSSDiag\\SrBackend";
inline unsigned parse(std::string_view text,unsigned low,unsigned high,unsigned fallback) noexcept {
    const auto first=text.find_first_not_of(" \t\r\n"),last=text.find_last_not_of(" \t\r\n");
    if(first==std::string_view::npos||last-first>=3||(high<=9&&first!=last))return fallback;
    unsigned value{};
    for(auto index=first;index<=last;++index){if(text[index]<'0'||text[index]>'9')return fallback;value=value*10+unsigned(text[index]-'0');}
    return value>=low&&value<=high?value:fallback;
}
inline unsigned read(const wchar_t* path,unsigned low,unsigned high,unsigned fallback) noexcept {
    const auto saved=GetLastError();unsigned result=fallback;
    const auto file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){char bytes[17]{};DWORD count{};LARGE_INTEGER size{};
        if(GetFileSizeEx(file,&size)&&size.QuadPart<=16&&ReadFile(file,bytes,16,&count,nullptr)&&count==size.QuadPart)
            result=parse({bytes,count},low,high,fallback);
        CloseHandle(file);
    }
    SetLastError(saved);return result;
}
inline void save(const wchar_t* path,unsigned value) {
    if(value>120)throw std::invalid_argument("Invalid launcher preference");
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    const auto temporary=std::wstring(path)+L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    const auto file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot save launcher preference");
    const auto bytes=std::to_string(value)+'\n';DWORD count{};
    const bool written=WriteFile(file,bytes.data(),DWORD(bytes.size()),&count,nullptr)&&count==bytes.size()&&FlushFileBuffers(file);
    CloseHandle(file);
    if(!written||!MoveFileExW(temporary.c_str(),path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        DeleteFileW(temporary.c_str());throw std::runtime_error("Cannot replace launcher preference");
    }
}
inline unsigned startupMultiplier() noexcept {
    static const auto selected=read(multiplierPath,2,4,2);
    return selected;
}
struct FrameCapSelection {bool requested{},invalid{};unsigned target=40;};
inline FrameCapSelection readFrameCap() noexcept {
    const auto error=GetLastError();FrameCapSelection out;wchar_t value[17]{};
    const auto enabledSize=GetEnvironmentVariableW(L"XESS_FRAME_CAP_OVERRIDE",value,17);
    out.requested=enabledSize==1&&value[0]==L'1';
    if(enabledSize&&(enabledSize!=1||(value[0]!=L'0'&&value[0]!=L'1')))out.invalid=true;
    SetLastError(ERROR_SUCCESS);
    const auto targetSize=GetEnvironmentVariableW(L"XESS_FRAME_CAP_TARGET_FPS",value,17);
    if(targetSize||GetLastError()!=ERROR_ENVVAR_NOT_FOUND){char digits[16]{};
        if(targetSize<17)for(DWORD index=0;index<targetSize;++index){if(value[index]>127){out.invalid=true;break;}digits[index]=char(value[index]);}
        const auto target=parse({digits,targetSize<17?targetSize:0},30,120,0);
        if(!target)out.invalid=true;else out.target=target;
    }
    SetLastError(error);return out;
}
inline const FrameCapSelection& startupFrameCap() noexcept {
    static const auto selected=readFrameCap();
    return selected;
}
inline fg_tuning::Selection readFgTuning() noexcept {
    const auto error=GetLastError();fg_tuning::Selection out;wchar_t value[3]{};
    const auto sceneLength=GetEnvironmentVariableW(L"XESS_FG_SCENE_THRESHOLD",value,3);
    if(sceneLength==1&&value[0]>=L'0'&&value[0]<=L'6')out.threshold=unsigned(value[0]-L'0');
    else if(sceneLength)out.invalid|=1;
    else {
        const auto selected=read(sceneThresholdPath,0,6,99);
        if(selected<=6)out.threshold=selected;
        else if(GetFileAttributesW(sceneThresholdPath)!=INVALID_FILE_ATTRIBUTES)out.invalid|=1;
    }
    if constexpr(!releaseMinimal){
        const auto length=GetEnvironmentVariableW(L"XESS_FG_DEBUG_MODE",value,3);
        if(length==1&&value[0]>=L'0'&&value[0]<=L'3')out.debug=unsigned(value[0]-L'0');
        else if(length)out.invalid|=2;
    }
    SetLastError(error);return out;
}
inline const fg_tuning::Selection& startupFgTuning() noexcept {static const auto value=readFgTuning();return value;}
inline unsigned generatedCapacity(unsigned multiplier,unsigned supported) noexcept {
    if(multiplier<2||multiplier>4)multiplier=2;
    return std::max(1u,std::min(multiplier-1,supported));
}
}
