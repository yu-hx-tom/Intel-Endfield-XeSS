#include "sr_execute_overlay.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace xess_vk::sr_overlay {
namespace {
template<class T> std::uint64_t bits(T handle) noexcept {return reinterpret_cast<std::uintptr_t>(handle);}
template<class T> T handle(std::uint64_t value) noexcept {return reinterpret_cast<T>(static_cast<std::uintptr_t>(value));}
bool valid(const Configuration& c) noexcept {
    return c.input.width&&c.input.height&&c.output.width&&c.output.height&&
        c.input.width<=c.output.width&&c.input.height<=c.output.height&&
        ((c.input.width==c.output.width||c.input.height==c.output.height)?
            (c.input.width==c.output.width&&c.input.height==c.output.height):
            std::abs((double(c.output.width)/c.input.width)/(double(c.output.height)/c.input.height)-1)<=0.02)&&
        (c.domain==ColorDomain::SceneLinearHdr||c.domain==ColorDomain::LinearLdr)&&std::isfinite(c.velocityScaleX)&&std::isfinite(c.velocityScaleY)&&
        c.velocityScaleX!=0&&c.velocityScaleY!=0;
}
std::uint32_t flags(const Configuration& c) noexcept {
    return XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE|(c.invertedDepth?XESS_INIT_FLAG_INVERTED_DEPTH:0u)|
        (c.domain==ColorDomain::LinearLdr?XESS_INIT_FLAG_LDR_INPUT_COLOR:0u)|
        (c.jitteredMotion?XESS_INIT_FLAG_JITTERED_MV:0u);
}
bool same(const Configuration& a,const Configuration& b) noexcept {
    return a.input.width==b.input.width&&a.input.height==b.input.height&&a.output.width==b.output.width&&a.output.height==b.output.height&&
        a.domain==b.domain&&a.invertedDepth==b.invertedDepth&&a.velocityScaleX==b.velocityScaleX&&a.velocityScaleY==b.velocityScaleY&&
        a.jitteredMotion==b.jitteredMotion;
}
RecordingKey key(const Owner& owner) noexcept {return {owner.device,owner.incarnation,owner.command,owner.recording};}
bool depthFormat(VkFormat format) noexcept {
    return format==VK_FORMAT_D16_UNORM||format==VK_FORMAT_D32_SFLOAT||format==VK_FORMAT_X8_D24_UNORM_PACK32||
        format==VK_FORMAT_D16_UNORM_S8_UINT||format==VK_FORMAT_D24_UNORM_S8_UINT||format==VK_FORMAT_D32_SFLOAT_S8_UINT;
}
bool colorFormat(VkFormat format) noexcept {
    return format==VK_FORMAT_R16G16B16A16_SFLOAT||format==VK_FORMAT_B10G11R11_UFLOAT_PACK32||
        format==VK_FORMAT_R8G8B8A8_UNORM;
}
bool imageValid(const Image& image,const Owner& owner,VkExtent2D extent,unsigned index) noexcept {
    const auto& s=image.sdk;const auto& r=s.subresourceRange;const auto& b=image.barrierRange;const auto& m=image.memory;
    const VkImageAspectFlags aspect=index==2?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageUsageFlags usage=index<3?VK_IMAGE_USAGE_SAMPLED_BIT:VK_IMAGE_USAGE_STORAGE_BIT;
    const VkFormatFeatureFlags features=index<3?VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT:VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    const VkImageAspectFlags barrierAspect=index==2&&!image.separateDepthStencilLayouts&&(s.format==VK_FORMAT_D16_UNORM_S8_UINT||s.format==VK_FORMAT_D24_UNORM_S8_UINT||s.format==VK_FORMAT_D32_SFLOAT_S8_UINT)?
        VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:aspect;
    if(image.owner!=owner||!image.imageGeneration||!image.viewGeneration||!s.image||!s.imageView||
       !image.ordinary2dSingleSample||!image.identityView||!image.stateAndOwnerVerified||
       s.width!=extent.width||s.height!=extent.height||r.aspectMask!=aspect||r.baseMipLevel||r.levelCount!=1||r.baseArrayLayer||r.layerCount!=1||
       b.aspectMask!=barrierAspect||b.baseMipLevel||b.levelCount!=1||b.baseArrayLayer||b.layerCount!=1||
       (image.usage&usage)!=usage||(image.formatFeatures&features)!=features||
       (image.currentLayout!=VK_IMAGE_LAYOUT_GENERAL&&image.currentLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&
        image.currentLayout!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL&&image.currentLayout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&
        image.currentLayout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)||
       bits(m.device)!=owner.device||m.resource.kind!=resource_memory_facts::ResourceKind::Image||
       m.resource.key.handle!=bits(s.image)||m.resource.key.generation!=image.imageGeneration||
       !m.memory.handle||!m.memory.generation||m.issues||!m.bindingBoundsKnown||!m.observedAliasFree||
       !m.size||m.offset>=m.end||m.end-m.offset!=m.size||
       (index!=2&&image.currentLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL))return false;
    return index==0?colorFormat(s.format):index==1?s.format==VK_FORMAT_R16G16_SFLOAT:index==2?depthFormat(s.format):colorFormat(s.format);
}
VkImageMemoryBarrier barrier(const Image& image,VkImageLayout oldLayout,VkImageLayout newLayout,VkAccessFlags src,VkAccessFlags dst) noexcept {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask=src;b.dstAccessMask=dst;b.oldLayout=oldLayout;b.newLayout=newLayout;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image.sdk.image;b.subresourceRange=image.barrierRange;return b;
}
}
SdkFunctions functions(const sr::Sdk& sdk) noexcept {
    return {{sdk.xessVKGetRequiredInstanceExtensions,sdk.xessVKGetRequiredDeviceExtensions,sdk.xessVKGetRequiredDeviceFeatures},
        sdk.xessVKCreateContext,sdk.xessForceLegacyScaleFactors,sdk.xessVKBuildPipelines,sdk.xessGetPipelineBuildStatus,
        sdk.xessGetOptimalInputResolution,sdk.xessVKInit,sdk.xessSetVelocityScale,sdk.xessVKExecute,sdk.xessDestroyContext};
}
Overlay::~Overlay() {
    // Retain unsafe references rather than calling SDK destruction on the wrong
    // thread or while a recorded CB can still use the context.
    if(context_&&!pending()&&onInitialThread())sdk_.destroy(context_);
}
bool Overlay::onInitialThread()const noexcept {return !initialThread_||initialThread_==GetCurrentThreadId();}
Result Overlay::start(Enable enabled,HostPhase phase,const sr_game::RuntimeIdentity& identity,
                      const sr_game::RuntimeRequirementsAdmission& admission,const Configuration& config,SdkFunctions sdk,VulkanFunctions vk,std::size_t pendingLimit,bool blockingBuild) noexcept {
    if(!onInitialThread())return {Status::WrongThread};
    if(context_||pending())return {Status::PendingCommands};
    if(enabled==Enable::Off){state_=State::Disabled;return {Status::Disabled};}
    if(phase!=HostPhase::OutsideApplicationRecording)return {Status::WrongHostPhase};
    if(!admission.admitted()||!admission.matches(identity)||!admission.matchesQueries(sdk.requirements))return {Status::RequirementsMissing};
    if(!pendingLimit||pendingLimit>pending_.size()||!valid(config)||!sdk.create||!sdk.currentScale||!sdk.build||!sdk.buildStatus||!sdk.inputRange||!sdk.init||
       !sdk.velocityScale||!sdk.execute||!sdk.destroy||!vk.barrier||!vk.bindPipeline||!vk.bindSets||!vk.pushConstants||
       !vk.enterInjectedRecording||!vk.leaveInjectedRecording)return {Status::InvalidConfiguration};
    pendingLimit_=pendingLimit;identity_=identity;sdk_=sdk;vk_=vk;config_=config;flags_=flags(config);resetRequired_=true;rejectedInputKnown_=false;lastFrame_=historyEpoch_=0;
    initialThread_=GetCurrentThreadId();
    sdkOperation_="xessVKCreateContext";
    auto r=sdk_.create(identity.instance,identity.physicalDevice,identity.device,&context_);
    if(r<0||!context_){state_=State::Failed;return {Status::SdkFailure,r};}
    sdkOperation_="xessForceLegacyScaleFactors";r=sdk_.currentScale(context_,false);
    if(r>=0){sdkOperation_="xessVKBuildPipelines";r=sdk_.build(context_,VK_NULL_HANDLE,blockingBuild,flags_);}
    if(r<0){state_=State::Failed;return {Status::SdkFailure,r};}
    // A successful blocking build has already completed; only async work is polled.
    state_=blockingBuild?State::NeedsInit:State::Building;return {blockingBuild?Status::NeedsInit:Status::Building,r};
}
Result Overlay::pollBuild(HostPhase phase) noexcept {
    if(!onInitialThread())return {Status::WrongThread};
    if(state_==State::Disabled)return {Status::Disabled};
    if(phase!=HostPhase::OutsideApplicationRecording)return {Status::WrongHostPhase};
    if(state_!=State::Building)return {state_==State::Ready?Status::Ready:state_==State::NeedsInit?Status::NeedsInit:Status::SdkFailure};
    sdkOperation_="xessGetPipelineBuildStatus";const auto r=sdk_.buildStatus(context_);
    if(r==XESS_RESULT_ERROR_OPERATION_IN_PROGRESS)return {Status::Building,r};
    if(r!=XESS_RESULT_SUCCESS){state_=State::Failed;return {Status::SdkFailure,r};}
    state_=State::NeedsInit;return {Status::NeedsInit};
}
Result Overlay::initialize(HostPhase phase,const Configuration& c) noexcept {
    if(!onInitialThread())return {Status::WrongThread};
    if(state_==State::Disabled)return {Status::Disabled};
    if(phase!=HostPhase::OutsideApplicationRecording)return {Status::WrongHostPhase};
    if(pending())return {Status::PendingCommands};
    if(state_==State::Building)return {Status::Building};
    if(!context_||state_==State::Failed)return {Status::SdkFailure};
    if(!valid(c)||flags(c)!=flags_)return {Status::InvalidConfiguration};
    if(state_==State::Ready&&same(config_,c))return {Status::Ready};
    if(rejectedInputKnown_&&same(rejectedInput_,c))return {Status::UnsupportedInputExtent};
    const auto requested=sr::closestPreset(c.input,c.output).quality;auto effective=requested;
    const xess_2d_t output{c.output.width,c.output.height};xess_2d_t optimal{},minimum{},maximum{};
    auto range=[&](xess_quality_settings_t q){sdkOperation_="xessGetOptimalInputResolution";return sdk_.inputRange(context_,&output,q,&optimal,&minimum,&maximum);};
    auto inRange=[&]{return minimum.x&&minimum.y&&minimum.x<=maximum.x&&minimum.y<=maximum.y&&
        c.input.width>=minimum.x&&c.input.height>=minimum.y&&c.input.width<=maximum.x&&c.input.height<=maximum.y;};
    auto r=range(effective);
    if(r<0){state_=State::Failed;return {Status::SdkFailure,r};}
    // Game extents are rounded independently of SDK minima at every preset.
    while(!inRange()&&effective>XESS_QUALITY_SETTING_ULTRA_PERFORMANCE){
        effective=static_cast<xess_quality_settings_t>(unsigned(effective)-1);r=range(effective);
        if(r<0){state_=State::Failed;return {Status::SdkFailure,r};}
    }
    if(!inRange()){rejectedInput_=c;rejectedInputKnown_=true;resetRequired_=true;return {Status::UnsupportedInputExtent};}
    xess_vk_init_params_t init{};init.outputResolution=output;init.qualitySetting=effective;init.initFlags=flags_;
    sdkOperation_="xessVKInit";r=sdk_.init(context_,&init);
    if(r>=0){sdkOperation_="xessSetVelocityScale";r=sdk_.velocityScale(context_,c.velocityScaleX,c.velocityScaleY);}
    if(r<0){state_=State::Failed;return {Status::SdkFailure,r};}
    config_=c;requested_=requested;effective_=effective;rejectedInputKnown_=false;resetRequired_=true;state_=State::Ready;return {Status::Ready,r};
}
Status Overlay::validate(const Frame& f)const noexcept {
    const auto& c=f.recording;const auto& o=f.owner;
    if(!same(f.configuration,config_))return Status::NeedsInit;
    if(c.reasons||!c.allocationKnown||!c.poolKnown||!c.beginFlagsKnown||!c.stateKnown||!c.recording||!c.scopeKnown||
       c.inRenderPass||c.inDynamicRendering||c.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY||
       c.beginUsageFlags!=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT||c.queueFamily==VK_QUEUE_FAMILY_IGNORED||
       c.device!=bits(identity_.device)||
       o.device!=c.device||o.incarnation!=c.contextIncarnation||o.command!=c.command||o.recording!=c.rpRecordingGeneration||
       o.prefix!=c.commonRecordOrder||!o.command||!o.recording||!o.prefix)return Status::RecordingUnverified;
    const bool producer=f.nativeCoreReplaced&&!f.finalFsrDispatchReturned&&f.wholeCoreTransactionVerified&&f.atAccumulateBoundary;
    if(!o.frame||!f.historyEpoch||!f.pinnedModule||!f.sameFrameInputs||!producer||!f.domainAndDownstreamVerified||
       !f.preExposureConventionVerified||!f.motionConventionVerified||!f.cameraHistoryVerified||!f.applicationReferencesRetained||!f.aliasCoverageComplete||!f.sourcesUnchangedAtUse||
       !std::isfinite(f.jitterX)||!std::isfinite(f.jitterY)||std::abs(f.jitterX)>0.5f||std::abs(f.jitterY)>0.5f)return Status::FrameUnverified;
    if(o.frame<=lastFrame_)return Status::DuplicateFrame;
    const std::array images{&f.color,&f.motion,&f.depth,&f.nativeOutput};
    for(unsigned i=0;i<images.size();++i) {
        if(!imageValid(*images[i],o,i<3?config_.input:config_.output,i))return Status::ImageUnverified;
        for(unsigned j=0;j<i;++j) {
            const auto& a=*images[i];const auto& b=*images[j];
            if(a.sdk.image==b.sdk.image||a.sdk.imageView==b.sdk.imageView||
               (a.memory.memory.handle==b.memory.memory.handle&&a.memory.memory.generation!=b.memory.memory.generation)||
               (a.memory.memory==b.memory.memory&&a.memory.offset<b.memory.end&&b.memory.offset<a.memory.end))return Status::ImageUnverified;
        }
    }
    // The guide lists packed float input, and the official Vulkan sample uses
    // distinct linear input/output formats. Exact pair GPU validation is separate.
    const bool packedGameColor=f.color.sdk.format==VK_FORMAT_B10G11R11_UFLOAT_PACK32&&f.nativeOutput.sdk.format==VK_FORMAT_R16G16B16A16_SFLOAT;
    if(!packedGameColor&&f.color.sdk.format!=f.nativeOutput.sdk.format)return Status::UnsupportedFormatPair;
    if(f.nativeCoreReplaced&&(!vk_.blit||!(f.color.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)||!(f.nativeOutput.usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT)||
        (f.color.formatFeatures&(VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))!=
            (VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)||
        !(f.nativeOutput.formatFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT)))return Status::UnsupportedFormatPair;
    const auto& r=f.restore;
    if(r.owner!=o||!r.completeStaticBindings||!r.completePushConstants||!r.pipeline||!r.layout||!r.pipelineGeneration||!r.layoutGeneration||
       !r.setCount||r.setCount>r.sets.size()||r.pushCount>r.push.size())return Status::RestoreUnverified;
    for(unsigned i=0;i<r.setCount;++i)if(!r.sets[i]||!r.setGenerations[i])return Status::RestoreUnverified;
    for(unsigned i=0;i<r.pushCount;++i) {
        const auto& p=r.push[i];
        if(!p.layout||!p.layoutGeneration||!p.stages||!p.size||p.size%4||p.offset%4||p.offset>256||p.size>256-p.offset)return Status::RestoreUnverified;
    }
    return Status::Ready;
}
void Overlay::restore(VkCommandBuffer command,const ComputeRestore& r)const noexcept {
    vk_.bindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipeline);
    vk_.bindSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,r.layout,0,r.setCount,r.sets.data(),0,nullptr);
    for(unsigned i=0;i<r.pushCount;++i){const auto& p=r.push[i];vk_.pushConstants(command,p.layout,p.stages,p.offset,p.size,p.bytes.data());}
}
void Overlay::barriers(VkCommandBuffer command,const Frame& f,bool before)const noexcept {
    std::array<VkImageMemoryBarrier,4> b{};unsigned count{};
    const std::array inputs{&f.color,&f.motion,&f.depth};
    for(const auto* i:inputs)b[count++]=before?barrier(*i,i->currentLayout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_SHADER_READ_BIT):
        barrier(*i,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,i->currentLayout,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    const auto& output=f.nativeOutput;
    b[count++]=before?barrier(output,output.currentLayout,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT):
        barrier(output,VK_IMAGE_LAYOUT_GENERAL,output.currentLayout,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    vk_.barrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        before?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,count,b.data());
}
Status Overlay::availability(const Configuration& configuration,RecordingKey recording,std::uint64_t frame)const noexcept {
    if(!onInitialThread())return Status::WrongThread;
    if(state_==State::Disabled)return Status::Disabled;
    bool available=false;
    for(std::size_t n=0;n<pendingLimit_;++n) {
        if(pending_[n].active&&pending_[n].key==recording)return Status::PendingCommands;
        available|=!pending_[n].active;
    }
    if(!available)return Status::PendingCommands;
    if(state_!=State::Ready)return state_==State::Building?Status::Building:state_==State::NeedsInit?Status::NeedsInit:Status::SdkFailure;
    if(!same(configuration,config_))return Status::NeedsInit;
    if(frame&&frame<=lastFrame_)return Status::DuplicateFrame;
    return Status::Ready;
}
Status Overlay::preflight(const Frame& f)const noexcept {
    const auto available=availability(f.configuration,key(f.owner));
    if(available!=Status::Ready)return available;
    const auto status=validate(f);
    if(status!=Status::Ready)return status;
    return attempts_==std::numeric_limits<std::uint64_t>::max()?Status::InvalidConfiguration:Status::Ready;
}
Result recordPreExecuteColorFallback(const Frame& f,const VulkanFunctions& vk) noexcept {
    const auto& c=f.recording;const auto& o=f.owner;
    if(!f.nativeCoreReplaced||!f.wholeCoreTransactionVerified||!f.atAccumulateBoundary||f.finalFsrDispatchReturned||!f.pinnedModule||!f.sameFrameInputs||
       !f.domainAndDownstreamVerified||!f.applicationReferencesRetained||!f.aliasCoverageComplete||!f.sourcesUnchangedAtUse||
       f.configuration.domain!=ColorDomain::SceneLinearHdr||!valid(f.configuration))return {Status::FrameUnverified};
    if(c.reasons||!c.allocationKnown||!c.poolKnown||!c.beginFlagsKnown||!c.stateKnown||!c.recording||!c.scopeKnown||
       c.inRenderPass||c.inDynamicRendering||c.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY||
       c.beginUsageFlags!=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT||c.queueFamily==VK_QUEUE_FAMILY_IGNORED||
       !o.frame||!o.prefix||!o.command||!o.recording||!o.device||!o.incarnation||o.device!=c.device||o.incarnation!=c.contextIncarnation||o.command!=c.command||
       o.recording!=c.rpRecordingGeneration||o.prefix!=c.commonRecordOrder)return {Status::RecordingUnverified};
    if(!imageValid(f.color,o,f.configuration.input,0)||!imageValid(f.nativeOutput,o,f.configuration.output,3)||
       f.color.sdk.image==f.nativeOutput.sdk.image||f.color.sdk.imageView==f.nativeOutput.sdk.imageView||
       (f.color.memory.memory.handle==f.nativeOutput.memory.memory.handle&&
        (f.color.memory.memory.generation!=f.nativeOutput.memory.memory.generation||
         (f.color.memory.offset<f.nativeOutput.memory.end&&f.nativeOutput.memory.offset<f.color.memory.end))))return {Status::ImageUnverified};
    constexpr auto source=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if(f.color.sdk.format!=VK_FORMAT_B10G11R11_UFLOAT_PACK32||f.nativeOutput.sdk.format!=VK_FORMAT_R16G16B16A16_SFLOAT||
       !(f.color.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)||!(f.nativeOutput.usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT)||
       (f.color.formatFeatures&source)!=source||!(f.nativeOutput.formatFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT)||
       !vk.barrier||!vk.blit||!vk.enterInjectedRecording||!vk.leaveInjectedRecording)return {Status::UnsupportedFormatPair};
    if(!vk.enterInjectedRecording(vk.user,o))return {Status::RecordingUnverified};
    struct Exit {const VulkanFunctions& vk;~Exit(){vk.leaveInjectedRecording(vk.user);}} exit{vk};
    const auto command=handle<VkCommandBuffer>(o.command);
    const std::array before{
        barrier(f.color,f.color.currentLayout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT),
        barrier(f.nativeOutput,f.nativeOutput.currentLayout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT)};
    vk.barrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,before.data());
    VkImageBlit region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.dstSubresource=region.srcSubresource;
    region.srcOffsets[1]={int(f.configuration.input.width),int(f.configuration.input.height),1};
    region.dstOffsets[1]={int(f.configuration.output.width),int(f.configuration.output.height),1};
    vk.blit(command,f.color.sdk.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,f.nativeOutput.sdk.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region,VK_FILTER_LINEAR);
    const std::array after{
        barrier(f.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,f.color.currentLayout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT),
        barrier(f.nativeOutput,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,f.nativeOutput.currentLayout,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT)};
    vk.barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,after.data());
    return {Status::BlitFallbackRecorded,XESS_RESULT_SUCCESS,0,false,true,false,true};
}
Result Overlay::record(const Frame& f) noexcept {
    const auto status=preflight(f);
    if(status!=Status::Ready){
        if(status!=Status::WrongThread&&status!=Status::Disabled&&status!=Status::DuplicateFrame)resetRequired_=true;
        if(status==Status::InvalidConfiguration)state_=State::Failed;
        return {status};
    }
    Pending* pending=nullptr;
    for(std::size_t n=0;n<pendingLimit_;++n)if(!pending_[n].active){pending=&pending_[n];break;}
    if(!vk_.enterInjectedRecording(vk_.user,f.owner)){resetRequired_=true;return {Status::RecordingUnverified};}
    struct ScopeExit {const VulkanFunctions& vk;~ScopeExit(){vk.leaveInjectedRecording(vk.user);}} scope{vk_};
    const auto command=handle<VkCommandBuffer>(f.owner.command);const bool reset=resetRequired_||f.gameReset||f.historyEpoch!=historyEpoch_;
    xess_vk_execute_params_t p{};p.colorTexture=f.color.sdk;p.velocityTexture=f.motion.sdk;p.depthTexture=f.depth.sdk;p.outputTexture=f.nativeOutput.sdk;
    p.inputWidth=config_.input.width;p.inputHeight=config_.input.height;p.jitterOffsetX=f.jitterX;p.jitterOffsetY=f.jitterY;p.exposureScale=1.0f;p.resetHistory=reset;
    *pending={key(f.owner),{},0,SubmissionState::Recorded,true,false,f.recording.pool};++attempts_;
    barriers(command,f,true);
    sdkOperation_="xessVKExecute";const auto r=sdk_.execute(context_,command,&p);
    if(r<0&&r!=XESS_RESULT_ERROR_UNINITIALIZED){
        state_=State::Failed;resetRequired_=true;
        return {Status::SdkFailureCommandsUncertain,r,attempts_,false,false,reset};
    }
    restore(command,f.restore);
    if(r>=0) {
        barriers(command,f,false);resetRequired_=false;lastFrame_=f.owner.frame;historyEpoch_=f.historyEpoch;
        return {Status::Recorded,r,attempts_,true,true,reset};
    }
    // Only this real SDK error has a validated continue-recording fallback.
    // Device, allocation and unknown failures must not record speculative work.
    const bool fallback=r==XESS_RESULT_ERROR_UNINITIALIZED;
    if(fallback){
        const std::array toTransfer{
            barrier(f.color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT),
            barrier(f.nativeOutput,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT)};
        vk_.barrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,toTransfer.data());
        VkImageBlit region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.dstSubresource=region.srcSubresource;
        region.srcOffsets[1]={int(config_.input.width),int(config_.input.height),1};region.dstOffsets[1]={int(config_.output.width),int(config_.output.height),1};
        vk_.blit(command,f.color.sdk.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,f.nativeOutput.sdk.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region,VK_FILTER_LINEAR);
        const std::array fromTransfer{
            barrier(f.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT),
            barrier(f.nativeOutput,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT)};
        vk_.barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,fromTransfer.data());
    }
    barriers(command,f,false);
    state_=State::Failed;resetRequired_=true;
    // A negative Execute may have already changed commands and game output.
    // Retain references and stop SR; this cannot roll back the FSR output.
    return {fallback?Status::BlitFallbackRecorded:Status::SdkFailureCommandsUncertain,r,attempts_,false,true,reset,fallback};
}
bool Overlay::pending() const noexcept {
    for(const auto& entry:pending_)if(entry.active)return true;
    return false;
}
bool Overlay::contains(RecordingKey key) const noexcept {
    for(const auto& entry:pending_)if(entry.active&&entry.key==key)return true;
    return false;
}
bool Overlay::submitted(const Submission& s) noexcept {
    for(auto& pending:pending_)if(pending.active&&pending.key==s.recording) {
        if(pending.submission!=SubmissionState::Recorded&&pending.submission!=SubmissionState::FailedNotEnqueued) {
            pending.uncertain=true;state_=State::Failed;resetRequired_=true;return false;
        }
        if(s.actualSubmitResult==VK_ERROR_OUT_OF_HOST_MEMORY||s.actualSubmitResult==VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            pending.submission=SubmissionState::FailedNotEnqueued;
            pending.uncertain=true;state_=State::Failed;resetRequired_=true;return false;
        }
        pending.submission=s.actualSubmitResult==VK_SUCCESS?SubmissionState::Submitted:
            s.actualSubmitResult==VK_ERROR_DEVICE_LOST?SubmissionState::DeviceLost:SubmissionState::Unknown;
        pending.queue=s.queue;pending.serial=s.serial;
        pending.uncertain=s.actualSubmitResult!=VK_SUCCESS||!s.queue||!s.serial;
        if(pending.uncertain){state_=State::Failed;resetRequired_=true;}
        return !pending.uncertain;
    }
    return false;
}
bool Overlay::completed(const Completion& c) noexcept {
    for(auto& pending:pending_)if(pending.active&&pending.submission==SubmissionState::Submitted&&!pending.uncertain&&
        pending.key==c.recording&&pending.queue==c.queue&&pending.serial==c.serial&&c.actualFenceStatus==VK_SUCCESS) {
        pending={};return true;
    }
    return false;
}
bool Overlay::discarded(RecordingKey k,Discard reason) noexcept {
    const bool known=reason==Discard::SuccessfulReset||reason==Discard::FreeObserved||reason==Discard::SuccessfulPoolReset||reason==Discard::PoolDestroyObserved;
    for(auto& pending:pending_)if(pending.active&&pending.key==k&&known&&
        (pending.submission==SubmissionState::Recorded||pending.submission==SubmissionState::FailedNotEnqueued)) {
        pending={};resetRequired_=true;return true;
    }
    return false;
}
bool Overlay::retired(RecordingKey k,Discard reason) noexcept {
    if(discarded(k,reason))return true;
    const bool known=reason==Discard::SuccessfulReset||reason==Discard::FreeObserved||reason==Discard::SuccessfulPoolReset||reason==Discard::PoolDestroyObserved;
    // A successful real reset/rebegin or legal free guarantees nonpending.
    // Mere fence polling or a private T3 slot completion cannot enter here.
    for(auto& pending:pending_)if(known&&pending.active&&pending.key==k&&
        pending.submission==SubmissionState::Submitted&&!pending.uncertain) {
        pending={};return true;
    }
    return false;
}
void Overlay::poolRetired(std::uint64_t pool,Discard reason) noexcept {
    if(!pool)return;
    for(auto& pending:pending_)if(pending.active&&pending.pool==pool)(void)retired(pending.key,reason);
}
bool Overlay::deviceIdle(const sr_game::RuntimeIdentity& identity,VkResult r) noexcept {
    if(identity.instance!=identity_.instance||identity.physicalDevice!=identity_.physicalDevice||identity.device!=identity_.device||
       identity.incarnation!=identity_.incarnation||r!=VK_SUCCESS)return false;
    bool released=false;
    for(auto& pending:pending_)if(pending.active&&
        (pending.submission==SubmissionState::Submitted||pending.submission==SubmissionState::DeviceLost)) {
        pending={};released=true;
    }
    return released;
}
Result Overlay::stop(HostPhase phase) noexcept {
    if(!onInitialThread())return {Status::WrongThread};
    if(state_==State::Disabled)return {Status::Disabled};
    if(phase!=HostPhase::OutsideApplicationRecording)return {Status::WrongHostPhase};
    if(pending())return {Status::PendingCommands};
    if(context_){const auto r=sdk_.destroy(context_);if(r<0){state_=State::Failed;return {Status::SdkFailure,r};}context_=nullptr;}
    state_=State::Disabled;resetRequired_=true;return {Status::Disabled};
}
const char* name(Status status) noexcept {
    switch(status) {
#define SR_STATUS(x) case Status::x:return #x;
        SR_STATUS(Disabled) SR_STATUS(Ready) SR_STATUS(Building) SR_STATUS(NeedsInit) SR_STATUS(PendingCommands)
        SR_STATUS(RequirementsMissing) SR_STATUS(WrongHostPhase) SR_STATUS(WrongThread) SR_STATUS(InvalidConfiguration) SR_STATUS(SdkFailure)
        SR_STATUS(RecordingUnverified) SR_STATUS(FrameUnverified) SR_STATUS(ImageUnverified) SR_STATUS(UnsupportedFormatPair)
        SR_STATUS(RestoreUnverified) SR_STATUS(DuplicateFrame) SR_STATUS(Recorded) SR_STATUS(SdkFailureCommandsUncertain) SR_STATUS(UnsupportedInputExtent) SR_STATUS(BlitFallbackRecorded)
#undef SR_STATUS
    }return "Unknown";
}
}
