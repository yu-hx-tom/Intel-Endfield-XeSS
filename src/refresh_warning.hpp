#pragma once
#include <algorithm>
#include <cmath>
#include <string>

namespace xess_vk::refresh_warning {
inline bool exceeds(unsigned base,unsigned multiplier,unsigned hz) noexcept {return hz&&base*multiplier>hz;}
inline unsigned suggested(unsigned multiplier,unsigned hz) noexcept {return multiplier?std::clamp(hz/multiplier,30u,120u):30;}
inline std::wstring text(unsigned hz,unsigned multiplier,bool mapped,unsigned target){
    if(!hz||multiplier<2||multiplier>4)return {};
    const auto thirty=mapped?target:30;std::wstring result;
    if(exceeds(thirty,multiplier,hz))result=L"30帧档："+std::to_wstring(thirty)+L"×"+std::to_wstring(multiplier)+L"="+std::to_wstring(thirty*multiplier);
    if(exceeds(60,multiplier,hz)){if(!result.empty())result+=L"；";result+=L"若选60帧档：60×"+std::to_wstring(multiplier)+L"="+std::to_wstring(60*multiplier);}
    if(result.empty())return {};
    result+=L" 超过显示器 "+std::to_wstring(hz)+L" Hz，可能撕裂并增加功耗。";
    const auto base=suggested(multiplier,hz);
    if(base*multiplier<=hz)result+=L" 建议30帧档映射为"+std::to_wstring(base)+L"×"+std::to_wstring(multiplier)+L"。";
    else result+=L" 当前倍率在最低30帧时也超过刷新率，请考虑降低倍率。";
    return result;
}
}
