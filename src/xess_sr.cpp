#include "xess_sr.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace xess_vk::sr {
void check(xess_result_t result,const char* operation) {
    if(result<0)throw std::runtime_error(std::string(operation)+": XeSS result "+std::to_string(result));
}
std::string version(const xess_version_t& value) {
    return std::to_string(value.major)+'.'+std::to_string(value.minor)+'.'+std::to_string(value.patch)+'.'+std::to_string(value.reserved);
}
const Preset& closestPreset(VkExtent2D input,VkExtent2D output) {
    if(!input.width||!input.height||!output.width||!output.height||input.width>output.width||input.height>output.height)
        throw std::invalid_argument("XeSS preset requires nonzero upscale/AA extents");
    const double x=double(output.width)/input.width,y=double(output.height)/input.height;
    if(std::abs(x/y-1)>0.02)throw std::invalid_argument("XeSS input/output aspect ratios disagree");
    return *std::min_element(presets.begin(),presets.end(),[&](const auto& a,const auto& b) {
        return std::abs(a.scale-(x+y)*0.5)<std::abs(b.scale-(x+y)*0.5);
    });
}
std::vector<std::string> mergeExtensions(std::span<const std::string> app,std::span<const std::string> sdk) {
    std::vector<std::string> result;
    for(const auto list:{app,sdk})for(const auto& name:list) {
        if(name.empty()||name.size()>=VK_MAX_EXTENSION_NAME_SIZE||name.find('\0')!=std::string::npos)
            throw std::invalid_argument("Invalid Vulkan extension name");
        if(std::find(result.begin(),result.end(),name)==result.end())result.push_back(name);
    }
    return result;
}
void validateExecute(const xess_vk_execute_params_t& p,VkExtent2D output) {
    if(!p.inputWidth||!p.inputHeight||!output.width||!output.height||p.inputWidth>output.width||p.inputHeight>output.height||
       p.resetHistory>1||!std::isfinite(p.jitterOffsetX)||!std::isfinite(p.jitterOffsetY)||
       std::abs(p.jitterOffsetX)>0.5f||std::abs(p.jitterOffsetY)>0.5f||!std::isfinite(p.exposureScale)||p.exposureScale<=0)
        throw std::invalid_argument("Invalid XeSS execute dimensions, jitter, reset or exposure");
    const std::array<const xess_vk_image_view_info*,4> images{&p.colorTexture,&p.velocityTexture,&p.depthTexture,&p.outputTexture};
    for(unsigned i=0;i<images.size();++i) {
        const auto& image=*images[i];
        const auto size=i==3?output:VkExtent2D{p.inputWidth,p.inputHeight};
        const auto base=i==0?p.inputColorBase:i==1?p.inputMotionVectorBase:i==2?p.inputDepthBase:p.outputColorBase;
        const auto aspect=i==2?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
        const auto format=i==1?VK_FORMAT_R16G16_SFLOAT:i==2?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R16G16B16A16_SFLOAT;
        const bool acceptedFormat=i==0?(image.format==format||image.format==VK_FORMAT_B10G11R11_UFLOAT_PACK32):image.format==format;
        if(!image.image||!image.imageView||!acceptedFormat||image.subresourceRange.aspectMask!=aspect||
           image.subresourceRange.baseMipLevel||image.subresourceRange.levelCount!=1||
           image.subresourceRange.baseArrayLayer||image.subresourceRange.layerCount!=1||
           base.x>image.width||base.y>image.height||size.width>image.width-base.x||size.height>image.height-base.y)
            throw std::invalid_argument("XeSS image metadata does not cover the declared HDR/depth/MV region");
        for(unsigned prior=0;prior<i;++prior)if(images[prior]->image==image.image)
            throw std::invalid_argument("XeSS inputs and output must be distinct images");
    }
}
namespace {
struct File {HANDLE value{INVALID_HANDLE_VALUE};~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
void verifyPin(HANDLE file,std::string_view expected) {
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    struct Cleanup {BCRYPT_ALG_HANDLE& algorithm;BCRYPT_HASH_HANDLE& hash;~Cleanup(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}} cleanup{algorithm,hash};
    if(expected.size()!=64||BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0||
       BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0)throw std::runtime_error("XeSS pin hash initialization failed");
    std::array<unsigned char,65536> bytes{};DWORD count{};
    do {
        if(!ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr))throw std::runtime_error("XeSS pin read failed");
        if(count&&BCryptHashData(hash,bytes.data(),count,0)<0)throw std::runtime_error("XeSS pin hash failed");
    }while(count);
    std::array<unsigned char,32> digest{};
    if(BCryptFinishHash(hash,digest.data(),digest.size(),0)<0)throw std::runtime_error("XeSS pin hash finish failed");
    constexpr char hex[]="0123456789abcdef";std::string actual;
    for(auto byte:digest){actual+=hex[byte>>4];actual+=hex[byte&15];}
    if(actual!=expected)throw std::runtime_error("XeSS SDK does not match the reviewed SHA256 pin");
}
bool moduleAt(HMODULE module,const std::filesystem::path& path) {
    wchar_t loaded[32768]{};const auto count=GetModuleFileNameW(module,loaded,32768);
    return count&&count<32768&&!_wcsicmp(loaded,path.c_str());
}
}
Sdk::Sdk(const std::filesystem::path& bin,std::string_view expectedSha256) {
    if(!bin.is_absolute())throw std::invalid_argument("XeSS SDK bin must be absolute");
    const auto path=bin/L"libxess.dll";
    File pinned;
    if(!expectedSha256.empty()) {
        // Hold the verified file against write/delete until LoadLibrary returns.
        pinned.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(pinned.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot pin XeSS SDK file");
        verifyPin(pinned.value,expectedSha256);
        if(const auto existing=GetModuleHandleW(L"libxess.dll");existing&&!moduleAt(existing,path))
            throw std::runtime_error("Another XeSS SDK module is already loaded from a different path");
    }
    auto module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module)throw std::runtime_error("Load XeSS SDK failed: Win32 "+std::to_string(GetLastError()));
    try {
        if(!expectedSha256.empty()&&!moduleAt(module,path))throw std::runtime_error("Loaded XeSS SDK path differs from the pinned file");
#define XESS_SR_LOAD(name) name=reinterpret_cast<decltype(name)>(GetProcAddress(module,#name)); if(!name)throw std::runtime_error("Missing XeSS export: " #name);
        XESS_SR_FUNCTIONS(XESS_SR_LOAD)
#undef XESS_SR_LOAD
        module_=module;
    } catch(...) {FreeLibrary(module);throw;}
}
bool Sdk::pinUntilProcessExit() const noexcept {
    HMODULE pinned{};
    return module_&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(xessVKCreateContext),&pinned)!=FALSE;
}
Sdk::~Sdk() {if(module_)FreeLibrary(static_cast<HMODULE>(module_));}
std::vector<Module> Sdk::loadedImplementations() const {
    std::vector<Module> result;
    for(const auto name:{L"libxess.dll",L"igxess2.dll",L"igxess.dll"}) {
        const auto module=GetModuleHandleW(name); if(!module)continue;
        wchar_t path[32768]{};const auto length=GetModuleFileNameW(module,path,32768);if(!length||length>=32768)continue;
        Module info;info.name=std::filesystem::path(name).string();
        const auto bytes=WideCharToMultiByte(CP_UTF8,0,path,length,nullptr,0,nullptr,nullptr);
        info.path.resize(bytes);WideCharToMultiByte(CP_UTF8,0,path,length,info.path.data(),bytes,nullptr,nullptr);
        DWORD unused{};const auto size=GetFileVersionInfoSizeW(path,&unused);
        std::vector<std::uint8_t> data(size);
        VS_FIXEDFILEINFO* file{};UINT fileBytes{};
        if(size&&GetFileVersionInfoW(path,0,size,data.data())&&VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&file),&fileBytes)&&
           file&&fileBytes>=sizeof(*file)&&file->dwSignature==0xFEEF04BD)
            info.fileVersion=std::to_string(HIWORD(file->dwFileVersionMS))+'.'+std::to_string(LOWORD(file->dwFileVersionMS))+'.'+
                std::to_string(HIWORD(file->dwFileVersionLS))+'.'+std::to_string(LOWORD(file->dwFileVersionLS));
        result.push_back(std::move(info));
    }
    return result;
}
}
