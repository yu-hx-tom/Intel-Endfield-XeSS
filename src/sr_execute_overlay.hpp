#pragma once
#include "command_recording_facts.hpp"
#include "resource_memory_facts.hpp"
#include "xess_sr.hpp"
#include "xess_sr_game_requirements.hpp"
#include <array>
#include <memory>

namespace xess_vk::fsr4 {class Bridge;}

namespace xess_vk::sr_overlay {
enum class Enable { Off, On };
enum class HostPhase { Unknown, OutsideApplicationRecording };
enum class ColorDomain { Unknown, SceneLinearHdr, LinearLdr };
enum class State { Disabled, Building, NeedsInit, Ready, Failed };
enum class Status {
    Disabled, Ready, Building, NeedsInit, PendingCommands, RequirementsMissing,
    WrongHostPhase, WrongThread, InvalidConfiguration, SdkFailure, RecordingUnverified,
    FrameUnverified, ImageUnverified, UnsupportedFormatPair, RestoreUnverified,
    DuplicateFrame, Recorded, SdkFailureCommandsUncertain, UnsupportedInputExtent, BlitFallbackRecorded
};
struct Result {
    Status status{Status::Disabled};
    xess_result_t sdkResult{XESS_RESULT_SUCCESS};
    std::uint64_t attempt{};
    bool executeSucceeded{},bindingsRestored{},resetApplied{},fallbackRecorded{};
};
struct Configuration {
    VkExtent2D input{},output{};
    ColorDomain domain{ColorDomain::Unknown};
    bool invertedDepth{};
    float velocityScaleX{},velocityScaleY{}; // Already verified source-vector to XeSS pixel displacement.
    bool jitteredMotion{}; // Source convention, never inferred just from nonzero cancellation constants.
};
struct Owner {
    // frame is an authoritative monotonically ordered frame identity, not the
    // 148-byte constants' float frameIndex or the latest callback timestamp.
    std::uint64_t device{},incarnation{},command{},recording{},prefix{},frame{};
    friend bool operator==(const Owner&,const Owner&)=default;
};
struct Image {
    xess_vk_image_view_info sdk{};
    Owner owner;
    std::uint64_t imageGeneration{},viewGeneration{};
    resource_memory_facts::BindingFacts memory;
    VkImageUsageFlags usage{};
    VkFormatFeatureFlags formatFeatures{};
    VkImageSubresourceRange barrierRange{};
    VkImageLayout currentLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    bool ordinary2dSingleSample{},identityView{},stateAndOwnerVerified{},separateDepthStencilLayouts{};
};
struct PushConstantWrite {
    VkPipelineLayout layout{};
    std::uint64_t layoutGeneration{};
    VkShaderStageFlags stages{};
    std::uint32_t offset{},size{};
    std::array<std::uint8_t,256> bytes{};
};
struct ComputeRestore {
    Owner owner;
    VkPipeline pipeline{};
    VkPipelineLayout layout{};
    std::uint64_t pipelineGeneration{},layoutGeneration{};
    std::array<VkDescriptorSet,8> sets{};
    std::array<std::uint64_t,8> setGenerations{};
    std::uint32_t setCount{},pushCount{};
    std::array<PushConstantWrite,8> push{};
    bool completeStaticBindings{},completePushConstants{};
};
struct Frame {
    CommandRecordingFacts recording;
    Owner owner;
    Configuration configuration;
    Image color,motion,depth,nativeOutput;
    ComputeRestore restore;
    float jitterX{},jitterY{};
    std::uint64_t historyEpoch{};
    bool gameReset{};
    // These are producer contracts established at this exact prefix. Copied
    // observation tickets, finite constants and usage bits cannot set them.
    bool pinnedModule{},sameFrameInputs{},finalFsrDispatchReturned{};
    bool domainAndDownstreamVerified{},preExposureConventionVerified{},motionConventionVerified{};
    bool cameraHistoryVerified{},applicationReferencesRetained{},aliasCoverageComplete{},sourcesUnchangedAtUse{};
    bool nativeCoreReplaced{},wholeCoreTransactionVerified{},atAccumulateBoundary{};
    float frameTimeMs=16.666667f,cameraNear=0.1f,cameraFar=10000.0f,cameraFov=1.04719755f;
};
struct SdkFunctions {
    sr_game::RequiredQueries requirements;
    decltype(&::xessVKCreateContext) create{};
    decltype(&::xessForceLegacyScaleFactors) currentScale{};
    decltype(&::xessVKBuildPipelines) build{};
    decltype(&::xessGetPipelineBuildStatus) buildStatus{};
    decltype(&::xessGetOptimalInputResolution) inputRange{};
    decltype(&::xessVKInit) init{};
    decltype(&::xessSetVelocityScale) velocityScale{};
    decltype(&::xessVKExecute) execute{};
    decltype(&::xessDestroyContext) destroy{};
};
SdkFunctions functions(const sr::Sdk&) noexcept;
struct VulkanFunctions {
    PFN_vkCmdPipelineBarrier barrier{};
    PFN_vkCmdBindPipeline bindPipeline{};
    PFN_vkCmdBindDescriptorSets bindSets{};
    PFN_vkCmdPushConstants pushConstants{};
    // The adapter excludes SDK-internal commands from its game evidence and
    // recursion path for this exact CB until leaveInjectedRecording returns.
    bool (*enterInjectedRecording)(void*,const Owner&) noexcept{};
    void (*leaveInjectedRecording)(void*) noexcept{};
    void* user{};
    PFN_vkCmdBlitImage blit{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetDeviceProcAddr gdpa{};
    std::uint32_t queueFamily=VK_QUEUE_FAMILY_IGNORED;
    const wchar_t* fsr4Directory{};
    // Downstream GIPA requires the layer's physical handle, not the public
    // loader-wrapped handle in RuntimeIdentity that the XeSS SDK consumes.
    VkPhysicalDevice physicalForInstanceDispatch{};
};
// Only before any SDK commands were recorded: replace an omitted core's color
// output after temporal/depth/restore admission refused. Never for SDK failures.
Result recordPreExecuteColorFallback(const Frame&,const VulkanFunctions&) noexcept;
struct RecordingKey {
    std::uint64_t device{},incarnation{},command{},recording{};
    friend bool operator==(const RecordingKey&,const RecordingKey&)=default;
};
struct Submission {
    RecordingKey recording;
    VkQueue queue{};
    std::uint64_t serial{};
    VkResult actualSubmitResult{VK_NOT_READY};
};
struct Completion {
    RecordingKey recording;
    VkQueue queue{};
    std::uint64_t serial{};
    VkResult actualFenceStatus{VK_NOT_READY};
};
enum class Discard { Unknown, SuccessfulReset, FreeObserved, SuccessfulPoolReset, PoolDestroyObserved };

// Detached and default OFF: no game hooks, environment reads, SDK loading,
// allocation, submission, waits or readback. Caller serializes methods, keeps
// SDK/PFNs and pinned resources alive, and uses unhooked original Vulkan PFNs.
// The first SDK call binds start/poll/init/record/stop and destruction to that
// actual Windows thread. Wrong-thread destruction retains the SDK context.
// Fixed pending records retain the context until the application's real CB
// retirement. The default is one; the game adapter admits up to sixteen on its
// single, ordered graphics queue. No method adds a submission or wait.
class Overlay {
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&)=delete;
    Overlay& operator=(const Overlay&)=delete;
    Result start(Enable,HostPhase,const sr_game::RuntimeIdentity&,
                 const sr_game::RuntimeRequirementsAdmission&,const Configuration&,SdkFunctions,VulkanFunctions,
                 std::size_t pendingLimit=1,bool blockingBuild=false) noexcept;
    Result pollBuild(HostPhase) noexcept;
    Result initialize(HostPhase,const Configuration&) noexcept;
    // No SDK/Vulkan calls or mutations. Ready is validation, not Execute success;
    // callers must still retain native work until record().executeSucceeded.
    Status preflight(const Frame&)const noexcept;
    Status availability(const Configuration&,RecordingKey,std::uint64_t frame=0)const noexcept;
    Result record(const Frame&) noexcept;
    bool submitted(const Submission&) noexcept;
    bool completed(const Completion&) noexcept;
    // FSR4 has its own exact tail-retirement fence. Its admitted CB is one-time;
    // after full retirement, release the SR record and mapping without waiting
    // for Unity to recycle that CB. No CPU wait or copy-only completion proof.
    unsigned pollBackendCompletions() noexcept;
    bool discarded(RecordingKey,Discard) noexcept;
    bool retired(RecordingKey,Discard) noexcept;
    void poolRetired(std::uint64_t,Discard) noexcept;
    // May only follow a successful real vkDeviceWaitIdle for this exact device;
    // it cannot retire recorded, unsubmitted CBs that can still submit later.
    bool deviceIdle(const sr_game::RuntimeIdentity&,VkResult actualWaitResult) noexcept;
    Result stop(HostPhase) noexcept;
    State state()const noexcept{return state_;}
    bool pending()const noexcept;
    bool contains(RecordingKey)const noexcept;
    bool hasContext()const noexcept{return context_!=nullptr||(fsr4_&&state_!=State::Disabled);}
    fsr4::Bridge* fsr4Bridge()const noexcept{return fsr4_.get();}
    xess_quality_settings_t requestedQuality()const noexcept{return requested_;}
    xess_quality_settings_t effectiveQuality()const noexcept{return effective_;}
    const char* lastSdkOperation()const noexcept{return sdkOperation_;}
    const char* lastBackendError()const noexcept{return backendError_.c_str();}
private:
    enum class SubmissionState { Recorded, FailedNotEnqueued, Submitted, DeviceLost, Unknown };
    struct Pending {
        RecordingKey key;
        VkQueue queue{};
        std::uint64_t serial{};
        SubmissionState submission{SubmissionState::Recorded};
        bool active{},uncertain{};
        std::uint64_t pool{};
    };
    std::array<Pending,16> pending_{};
    std::size_t pendingLimit_{1};
    State state_{State::Disabled};
    sr_game::RuntimeIdentity identity_{};
    SdkFunctions sdk_;
    VulkanFunctions vk_;
    xess_context_handle_t context_{};
    std::unique_ptr<fsr4::Bridge> fsr4_;
    std::string backendError_;
    const char* sdkOperation_{"none"};
    Configuration config_,rejectedInput_;
    bool rejectedInputKnown_{};
    std::uint32_t flags_{},initialThread_{};
    xess_quality_settings_t requested_{},effective_{};
    bool resetRequired_{true};
    std::uint64_t attempts_{},lastFrame_{},historyEpoch_{};
    bool onInitialThread()const noexcept;
    void backendError(const char* error)noexcept{try{backendError_=error;}catch(...){}}
    Status validate(const Frame&)const noexcept;
    void barriers(VkCommandBuffer,const Frame&,bool before)const noexcept;
    void restore(VkCommandBuffer,const ComputeRestore&)const noexcept;
};
const char* name(Status) noexcept;
}
