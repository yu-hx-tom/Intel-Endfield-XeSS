#pragma once
#include <xess/xess_vk.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xess_vk::sr {
struct Preset {
    const char* game;
    xess_quality_settings_t quality;
    double scale;
};
inline constexpr std::array presets{
    Preset{"native_aa",XESS_QUALITY_SETTING_AA,1.0},
    Preset{"quality",XESS_QUALITY_SETTING_ULTRA_QUALITY,1.5},
    Preset{"balanced",XESS_QUALITY_SETTING_QUALITY,1.7},
    Preset{"performance",XESS_QUALITY_SETTING_BALANCED,2.0},
    Preset{"ultra_performance",XESS_QUALITY_SETTING_ULTRA_PERFORMANCE,3.0}};
const Preset& closestPreset(VkExtent2D input,VkExtent2D output);
std::vector<std::string> mergeExtensions(std::span<const std::string>,std::span<const std::string>);
void validateExecute(const xess_vk_execute_params_t&,VkExtent2D output);
void check(xess_result_t,const char* operation);
std::string version(const xess_version_t&);
struct Module {
    std::string name,path,fileVersion;
};

// Independent opt-in prototype loader. The caller owns the Vulkan graph and
// must complete its real submissions before destroying its XeSS context.
class Sdk {
    void* module_{};
public:
    explicit Sdk(const std::filesystem::path& absoluteBin,std::string_view expectedSha256={});
    ~Sdk();
    Sdk(const Sdk&)=delete;
    Sdk& operator=(const Sdk&)=delete;
    std::vector<Module> loadedImplementations() const;
    bool pinUntilProcessExit() const noexcept;
#define XESS_SR_FUNCTIONS(X) \
    X(xessGetVersion) X(xessGetIntelXeFXVersion) X(xessSetLoggingCallback) \
    X(xessVKGetRequiredInstanceExtensions) X(xessVKGetRequiredDeviceExtensions) X(xessVKGetRequiredDeviceFeatures) \
    X(xessVKCreateContext) X(xessVKBuildPipelines) X(xessGetPipelineBuildStatus) X(xessVKInit) X(xessVKExecute) \
    X(xessGetOptimalInputResolution) X(xessForceLegacyScaleFactors) X(xessGetJitterScale) X(xessGetVelocityScale) X(xessSetVelocityScale) X(xessDestroyContext)
#define XESS_SR_MEMBER(name) decltype(&::name) name{};
    XESS_SR_FUNCTIONS(XESS_SR_MEMBER)
#undef XESS_SR_MEMBER
};
}
