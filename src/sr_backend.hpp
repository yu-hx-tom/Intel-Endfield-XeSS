#pragma once
#include <windows.h>
#include <string_view>
namespace xess_vk::sr_backend {
enum class Kind {XeSS,Fsr4XmxLite,Invalid};
inline Kind parse(std::wstring_view value)noexcept{
    if(value.empty()||value==L"xess")return Kind::XeSS;
    if(value==L"fsr4_xmx_lite")return Kind::Fsr4XmxLite;
    return Kind::Invalid;
}
inline Kind startup()noexcept{
    static const auto selected=[](){const auto saved=GetLastError();wchar_t value[32]{};
        const auto length=GetEnvironmentVariableW(L"XESS_SR_BACKEND",value,32);SetLastError(saved);
        return length>=32?Kind::Invalid:parse({value,length});}();
    return selected;
}
inline const char* name(Kind value)noexcept{return value==Kind::XeSS?"xess":value==Kind::Fsr4XmxLite?"fsr4_xmx_lite":"invalid";}
}
