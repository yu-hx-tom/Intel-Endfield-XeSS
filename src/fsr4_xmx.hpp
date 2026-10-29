#pragma once
#include <d3d12.h>
#include <ffx_api.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>
#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace xess_vk::fsr4 {
inline constexpr char modelName[]="4.1.2X Lite";
inline constexpr char libraryHash[]="74d18f09285150fa5531d1e5ffbad6a21b677e4eb1bc34cb580b2963f560e740";
inline constexpr char weightsHash[]="c33a287f3048bf02311cae430b6a977500b94ca7b315cd7bff751e6b03e483e3";
struct Version {std::uint64_t id{};std::string name;};
struct Configuration {
    unsigned inputWidth{},inputHeight{},outputWidth{},outputHeight{};
    bool invertedDepth=true,jitteredMotion=true;
};
struct Frame {
    ID3D12GraphicsCommandList* commands{};
    ID3D12Resource *color{},*depth{},*motion{},*output{};
    float jitterX{},jitterY{},velocityScaleX{},velocityScaleY{};
    float frameTimeMs=16.666667f,preExposure=1.0f;
    float cameraNear=0.1f,cameraFar=10000.0f,cameraFov=1.04719755f;
    bool reset{};
};
// Records into the caller's D3D12 list. Caller serializes calls and completes
// all GPU use before destroying/reinitializing this context.
class Context {
    void* module_{};
    ffxContext context_{};
    PfnFfxQuery query_{};PfnFfxDispatch dispatch_{};PfnFfxDestroyContext destroy_{};
    ffxCreateContextDescUpscale create_{};
    ffxCreateBackendDX12Desc backend_{};
    ffxOverrideVersion override_{};
    Version selected_;
    Configuration configuration_;
public:
    explicit Context(const std::filesystem::path& absoluteDirectory);
    ~Context();
    Context(const Context&)=delete;Context& operator=(const Context&)=delete;
    std::vector<Version> versions(ID3D12Device*);
    void initialize(ID3D12Device*,Configuration);
    void record(const Frame&);
    const Version& selected()const noexcept{return selected_;}
    bool ready()const noexcept{return context_!=nullptr;}
};
bool validConfiguration(Configuration) noexcept;
}
