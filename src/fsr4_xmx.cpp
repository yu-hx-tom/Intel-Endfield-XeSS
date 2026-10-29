#include "fsr4_xmx.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace xess_vk::fsr4 {
namespace {
void check(ffxReturnCode_t value,const char* operation) {
    if(value!=FFX_API_RETURN_OK)throw std::runtime_error(std::string(operation)+" failed: "+std::to_string(value));
}
void verify(const std::filesystem::path& path,const char* expected) {
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("FSR4 XMX payload is missing");
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 unavailable");
    struct Cleanup {BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Cleanup(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}} cleanup{algorithm,hash};
    if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0)throw std::runtime_error("SHA256 initialization failed");
    std::array<char,65536> bytes;
    while(file){file.read(bytes.data(),bytes.size());if(file.gcount()&&BCryptHashData(hash,reinterpret_cast<PUCHAR>(bytes.data()),ULONG(file.gcount()),0)<0)throw std::runtime_error("SHA256 read failed");}
    if(!file.eof())throw std::runtime_error("FSR4 XMX payload read failed");
    std::array<unsigned char,32> digest{};if(BCryptFinishHash(hash,digest.data(),digest.size(),0)<0)throw std::runtime_error("SHA256 finish failed");
    std::string actual;for(auto b:digest){actual+="0123456789abcdef"[b>>4];actual+="0123456789abcdef"[b&15];}
    if(actual!=expected)throw std::runtime_error("FSR4 XMX payload hash mismatch");
}
}
bool validConfiguration(Configuration c) noexcept {
    return c.inputWidth&&c.inputHeight&&c.outputWidth&&c.outputHeight&&
        c.inputWidth<=c.outputWidth&&c.inputHeight<=c.outputHeight&&c.outputWidth<=16384&&c.outputHeight<=16384;
}
Context::Context(const std::filesystem::path& directory) {
    if(!directory.is_absolute())throw std::invalid_argument("FSR4 XMX requires an absolute payload directory");
    const auto dll=directory/L"amd_fidelityfx_upscaler_dx12.dll";
    verify(dll,libraryHash);verify(directory/L"fsr-original.dll",weightsHash);
    const auto module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module)throw std::runtime_error("FSR4 XMX LoadLibrary failed: "+std::to_string(GetLastError()));
    query_=reinterpret_cast<PfnFfxQuery>(GetProcAddress(module,"ffxQuery"));
    dispatch_=reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module,"ffxDispatch"));
    destroy_=reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module,"ffxDestroyContext"));
    if(!query_||!dispatch_||!destroy_||!GetProcAddress(module,"ffxCreateContext")){FreeLibrary(module);throw std::runtime_error("FSR4 XMX FFX entry point missing");}
    module_=module;
}
Context::~Context(){if(context_)destroy_(&context_,nullptr);if(module_)FreeLibrary(static_cast<HMODULE>(module_));}
std::vector<Version> Context::versions(ID3D12Device* device) {
    if(!device)throw std::invalid_argument("FSR4 XMX version query needs a device");
    ffxQueryDescGetVersions q{};q.header.type=FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;q.device=device;std::uint64_t count{};q.outputCount=&count;
    check(query_(nullptr,&q.header),"FSR4 version count");
    if(!count||count>256)throw std::runtime_error("FSR4 XMX returned an invalid version count");
    std::vector<std::uint64_t> ids(count);std::vector<const char*> names(count);q.versionIds=ids.data();q.versionNames=names.data();
    check(query_(nullptr,&q.header),"FSR4 version list");
    if(count>ids.size())throw std::runtime_error("FSR4 version list changed during query");
    std::vector<Version> result;for(std::size_t n=0;n<count;++n){if(!names[n])throw std::runtime_error("FSR4 version has no name");result.push_back({ids[n],names[n]});}
    return result;
}
void Context::initialize(ID3D12Device* device,Configuration configuration) {
    if(context_)throw std::logic_error("FSR4 XMX context already initialized");
    if(!validConfiguration(configuration))throw std::invalid_argument("FSR4 XMX input/output extent is invalid");
    const auto available=versions(device);
    const auto found=std::find_if(available.begin(),available.end(),[](const Version& v){return v.name==modelName;});
    if(found==available.end())throw std::runtime_error("FSR4 4.1.2X Lite is not supported on this device");
    selected_=*found;configuration_=configuration;
    override_.header.type=FFX_API_DESC_TYPE_OVERRIDE_VERSION;override_.versionId=selected_.id;
    backend_.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;backend_.header.pNext=&override_.header;backend_.device=device;
    create_.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;create_.header.pNext=&backend_.header;
    create_.flags=FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE|FFX_UPSCALE_ENABLE_AUTO_EXPOSURE|
        (configuration.invertedDepth?FFX_UPSCALE_ENABLE_DEPTH_INVERTED:0)|
        (configuration.jitteredMotion?FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION:0);
    create_.maxRenderSize={configuration.inputWidth,configuration.inputHeight};create_.maxUpscaleSize={configuration.outputWidth,configuration.outputHeight};
    const auto create=reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(static_cast<HMODULE>(module_),"ffxCreateContext"));
    check(create(&context_,&create_.header,nullptr),"FSR4 XMX create context");
    ffxQueryGetProviderVersion actual{};actual.header.type=FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    check(query_(&context_,&actual.header),"FSR4 active provider");
    if(actual.versionId!=selected_.id)throw std::runtime_error("FSR4 XMX did not select the requested Lite provider");
}
void Context::record(const Frame& f) {
    if(!context_||!f.commands||!f.color||!f.depth||!f.motion||!f.output)throw std::invalid_argument("FSR4 XMX dispatch resources are incomplete");
    if(!std::isfinite(f.jitterX)||!std::isfinite(f.jitterY)||!std::isfinite(f.velocityScaleX)||!std::isfinite(f.velocityScaleY)||
       !std::isfinite(f.frameTimeMs)||f.frameTimeMs<=0||!std::isfinite(f.preExposure)||f.preExposure<=0)
        throw std::invalid_argument("FSR4 XMX dispatch constants are invalid");
    ffxDispatchDescUpscale p{};p.header.type=FFX_API_DISPATCH_DESC_TYPE_UPSCALE;p.commandList=f.commands;
    p.color=ffxApiGetResourceDX12(f.color,FFX_API_RESOURCE_STATE_COMPUTE_READ);
    p.depth=ffxApiGetResourceDX12(f.depth,FFX_API_RESOURCE_STATE_COMPUTE_READ);
    p.motionVectors=ffxApiGetResourceDX12(f.motion,FFX_API_RESOURCE_STATE_COMPUTE_READ);
    p.output=ffxApiGetResourceDX12(f.output,FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    p.jitterOffset={f.jitterX,f.jitterY};p.motionVectorScale={f.velocityScaleX,f.velocityScaleY};
    p.renderSize={configuration_.inputWidth,configuration_.inputHeight};p.upscaleSize={configuration_.outputWidth,configuration_.outputHeight};
    p.frameTimeDelta=f.frameTimeMs;p.preExposure=f.preExposure;p.reset=f.reset;
    p.cameraNear=configuration_.invertedDepth?f.cameraFar:f.cameraNear;
    p.cameraFar=configuration_.invertedDepth?f.cameraNear:f.cameraFar;
    p.cameraFovAngleVertical=f.cameraFov;p.viewSpaceToMetersFactor=1.0f;
    check(dispatch_(&context_,&p.header),"FSR4 XMX dispatch");
}
}
