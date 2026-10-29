#include "sr_execute_overlay.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <thread>
#include <vector>

namespace so=xess_vk::sr_overlay;
namespace sg=xess_vk::sr_game;
namespace rm=xess_vk::resource_memory_facts;
namespace {
unsigned checks{},creates{},builds{},inits{},executes{},copies{},destroys{},rangeQueries{},sdkCalls{},buildStatusCalls{};
template<class T>T h(std::uint64_t value){return reinterpret_cast<T>(static_cast<std::uintptr_t>(value));}
template<class T>std::uint64_t bits(T value){return reinterpret_cast<std::uintptr_t>(value);}
void require(bool ok,const char* text){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",text);std::exit(1);}}
const sg::RuntimeIdentity identity{h<VkInstance>(1),h<VkPhysicalDevice>(2),h<VkDevice>(3),4};
constexpr so::HostPhase outside=so::HostPhase::OutsideApplicationRecording;
xess_result_t pipelineStatus=XESS_RESULT_SUCCESS,executeStatus=XESS_RESULT_SUCCESS,initStatus=XESS_RESULT_SUCCESS;
xess_result_t createStatus=XESS_RESULT_SUCCESS,scaleStatus=XESS_RESULT_SUCCESS,buildResult=XESS_RESULT_SUCCESS,velocityStatus=XESS_RESULT_SUCCESS;
std::uint32_t builtFlags{},lastReset{};
float velocityX{},velocityY{};
VkPipeline boundPipeline{};std::array<VkDescriptorSet,2> boundSets{};
std::array<std::uint8_t,4> pushBytes{};
bool entered{},canEnter{true},expectedBlocking{};
VkCommandBuffer expectedCommand=h<VkCommandBuffer>(10);
unsigned enters{},leaves{};
VkExtent2D expectedInput{1128,634};
std::map<VkImage,VkImageLayout> layouts;
std::vector<VkImageMemoryBarrier> recordedBarriers;
VkPhysicalDeviceFeatures2 requested{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
xess_result_t requiredInstance(std::uint32_t* count,const char* const** names,std::uint32_t* api){*count=0;*names=nullptr;*api=VK_API_VERSION_1_3;return XESS_RESULT_SUCCESS;}
xess_result_t requiredDevice(VkInstance,VkPhysicalDevice,std::uint32_t* count,const char* const** names){*count=0;*names=nullptr;return XESS_RESULT_SUCCESS;}
xess_result_t requiredFeatures(VkInstance,VkPhysicalDevice,void** head){requested.features.shaderStorageImageWriteWithoutFormat=VK_TRUE;*head=&requested;return XESS_RESULT_SUCCESS;}
sg::RequiredQueries queries{requiredInstance,requiredDevice,requiredFeatures};
sg::RuntimeRequirementsAdmission admission(){
    const auto requirements=sg::queryRuntimeRequirements(queries,identity.instance,identity.physicalDevice);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};instance.pApplicationInfo=&app;
    VkPhysicalDeviceFeatures2 enabled=requested;VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device.pNext=&enabled;
    const auto a=sg::admitRuntimeRequirements(requirements,identity,instance,device,VK_SUCCESS,VK_SUCCESS);
    require(a.admitted(),"mock effective device actually admits every runtime requirement");return a;
}
xess_result_t create(VkInstance i,VkPhysicalDevice p,VkDevice d,xess_context_handle_t* context){
    ++sdkCalls;
    require(i==identity.instance&&p==identity.physicalDevice&&d==identity.device,"SDK context uses exact game instance/physical/device");
    ++creates;*context=h<xess_context_handle_t>(7);return createStatus;
}
xess_result_t currentScale(xess_context_handle_t,bool legacy){++sdkCalls;require(!legacy,"new scale factors explicitly selected");return scaleStatus;}
xess_result_t build(xess_context_handle_t,VkPipelineCache cache,bool blocking,std::uint32_t flags){
    ++sdkCalls;
    require(!cache&&blocking==expectedBlocking,"pipeline build starts asynchronously without app pipeline cache");
    require((flags&XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE)&&!(flags&XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE),"build enables automatic exposure without an exposure texture");
    ++builds;builtFlags=flags;return buildResult;
}
xess_result_t status(xess_context_handle_t){++sdkCalls;++buildStatusCalls;return pipelineStatus;}
xess_result_t range(xess_context_handle_t,const xess_2d_t* output,xess_quality_settings_t quality,xess_2d_t* optimal,xess_2d_t* minimum,xess_2d_t* maximum){
    ++sdkCalls;
    ++rangeQueries;*maximum=*output;
    if(quality==XESS_QUALITY_SETTING_ULTRA_QUALITY)*optimal=*minimum={static_cast<unsigned>(std::ceil(output->x/1.5)),static_cast<unsigned>(std::ceil(output->y/1.5))};
    else if(quality==XESS_QUALITY_SETTING_QUALITY)*optimal=*minimum={static_cast<unsigned>(std::ceil(output->x/1.7)),static_cast<unsigned>(std::ceil(output->y/1.7))};
    else if(quality==XESS_QUALITY_SETTING_BALANCED)*optimal=*minimum={static_cast<unsigned>(std::ceil(output->x/2.0)),static_cast<unsigned>(std::ceil(output->y/2.0))};
    else if(quality==XESS_QUALITY_SETTING_ULTRA_PERFORMANCE)*optimal=*minimum={static_cast<unsigned>(std::ceil(output->x/3.0)),static_cast<unsigned>(std::ceil(output->y/3.0))};
    else if(quality==XESS_QUALITY_SETTING_AA)*optimal=*minimum=*output;
    else *optimal=*minimum={1,1};return XESS_RESULT_SUCCESS;
}
xess_result_t init(xess_context_handle_t,const xess_vk_init_params_t* p){
    ++sdkCalls;
    ++inits;require(p->initFlags==builtFlags&&!p->pipelineCache&&!p->tempBufferHeap&&!p->tempTextureHeap,"Init matches prebuilt flags and uses private SDK storage");return initStatus;
}
xess_result_t velocity(xess_context_handle_t,float x,float y){++sdkCalls;velocityX=x;velocityY=y;return velocityStatus;}
xess_result_t execute(xess_context_handle_t,VkCommandBuffer command,const xess_vk_execute_params_t* p){
    ++sdkCalls;
    ++executes;require(command==expectedCommand&&entered,"Execute records into original game CB inside observer exclusion scope");
    require(p->inputWidth==expectedInput.width&&p->inputHeight==expectedInput.height&&p->outputTexture.image==h<VkImage>(103)&&p->colorTexture.image==h<VkImage>(100),"real dimensions retained and SDK writes directly into the game output");
    require(layouts[p->colorTexture.image]==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&layouts[p->velocityTexture.image]==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&
        layouts[p->depthTexture.image]==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&layouts[p->outputTexture.image]==VK_IMAGE_LAYOUT_GENERAL,"I/O barriers provide SDK-required layouts");
    require(p->jitterOffsetX==0.125f&&p->jitterOffsetY==-0.25f&&p->exposureScale==1.0f&&!p->exposureScaleTexture.image&&!p->responsivePixelMaskTexture.image,"auto exposure leaves the input scalar at one with verified jitter and no optional textures");
    lastReset=p->resetHistory;boundPipeline=h<VkPipeline>(900);boundSets={h<VkDescriptorSet>(901),h<VkDescriptorSet>(902)};pushBytes.fill(0xEE);
    return executeStatus;
}
xess_result_t destroy(xess_context_handle_t){++sdkCalls;++destroys;return XESS_RESULT_SUCCESS;}
void VKAPI_PTR cmdBarrier(VkCommandBuffer command,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,std::uint32_t,const VkMemoryBarrier*,std::uint32_t,const VkBufferMemoryBarrier*,std::uint32_t count,const VkImageMemoryBarrier* barriers){
    require(command==expectedCommand,"barriers use original game CB");
    for(unsigned i=0;i<count;++i){const auto& b=barriers[i];require(layouts[b.image]==b.oldLayout,"every barrier starts at exact recorded old layout");layouts[b.image]=b.newLayout;recordedBarriers.push_back(b);}
}
void VKAPI_PTR cmdPipeline(VkCommandBuffer,VkPipelineBindPoint point,VkPipeline pipeline){require(point==VK_PIPELINE_BIND_POINT_COMPUTE,"restore only compute pipeline");boundPipeline=pipeline;}
void VKAPI_PTR cmdSets(VkCommandBuffer,VkPipelineBindPoint point,VkPipelineLayout layout,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t dynamic,const std::uint32_t*){
    require(point==VK_PIPELINE_BIND_POINT_COMPUTE&&layout==h<VkPipelineLayout>(201)&&!first&&count==2&&!dynamic,"restore entire static descriptor range including opaque higher set");boundSets={sets[0],sets[1]};
}
void VKAPI_PTR cmdPush(VkCommandBuffer,VkPipelineLayout layout,VkShaderStageFlags stages,std::uint32_t offset,std::uint32_t size,const void* bytes){
    require(layout==h<VkPipelineLayout>(201)&&stages==VK_SHADER_STAGE_COMPUTE_BIT&&!offset&&size==4,"restore exact prior push-constant layout/stages/range");
    const auto* p=static_cast<const std::uint8_t*>(bytes);std::copy(p,p+4,pushBytes.begin());
}
so::SdkFunctions sdk(){return {queries,create,currentScale,build,status,range,init,velocity,execute,destroy};}
bool enterInjected(void*,const so::Owner& owner)noexcept {if(!canEnter||entered||owner.command!=bits(expectedCommand))return false;entered=true;++enters;return true;}
void leaveInjected(void*)noexcept {entered=false;++leaves;}
so::VulkanFunctions vk();
void VKAPI_PTR cmdBlit(VkCommandBuffer,VkImage source,VkImageLayout sl,VkImage destination,VkImageLayout dl,std::uint32_t count,const VkImageBlit* region,VkFilter filter){
    require(entered&&count==1&&filter==VK_FILTER_LINEAR&&source==h<VkImage>(100)&&destination==h<VkImage>(103),"fallback covers the same pinned color/output inside exclusion scope");
    require(sl==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&dl==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&layouts[source]==sl&&layouts[destination]==dl,"fallback follows transfer transitions");
    require(region->srcOffsets[1].x==1128&&region->srcOffsets[1].y==634&&region->dstOffsets[1].x==1920&&region->dstOffsets[1].y==1080,"fallback writes full output, not input-sized corner");++copies;
}
so::VulkanFunctions vk(){auto v=so::VulkanFunctions{cmdBarrier,cmdPipeline,cmdSets,cmdPush,enterInjected,leaveInjected,nullptr};v.blit=cmdBlit;return v;}
so::Configuration config(){return {{1128,634},{1920,1080},so::ColorDomain::SceneLinearHdr,true,1128.f,-634.f};}
so::Frame frame(std::uint64_t frameId=50){
    so::Frame f;f.owner={3,4,10,11,12,frameId};f.configuration=config();auto& c=f.recording;
    c.device=3;c.contextIncarnation=4;c.command=10;c.rpRecordingGeneration=11;c.commonRecordOrder=12;c.pool=9;c.poolGeneration=1;c.queueFamily=0;
    c.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;c.beginUsageFlags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;c.reasons=0;
    c.allocationKnown=c.poolKnown=c.beginFlagsKnown=c.stateKnown=c.recording=c.scopeKnown=c.completeHookCoverage=true;
    const std::array images{&f.color,&f.motion,&f.depth,&f.nativeOutput};
    for(unsigned i=0;i<images.size();++i){auto& image=*images[i];const bool depth=i==2;const auto extent=i<3?config().input:config().output;
        image.sdk={h<VkImageView>(110+i),h<VkImage>(100+i),{depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
            i==1?VK_FORMAT_R16G16_SFLOAT:depth?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R16G16B16A16_SFLOAT,extent.width,extent.height};
        image.owner=f.owner;image.imageGeneration=10+i;image.viewGeneration=20+i;
        image.ordinary2dSingleSample=image.identityView=image.stateAndOwnerVerified=true;
        image.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.formatFeatures=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        image.currentLayout=depth?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL;
        image.barrierRange={static_cast<VkImageAspectFlags>(depth?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
        auto& m=image.memory;m.device=identity.device;m.resource={rm::ResourceKind::Image,{100+i,image.imageGeneration}};m.memory={300+i,1};m.size=m.end=4096;
        m.bindingBoundsKnown=m.observedAliasFree=true;
    }
    auto& r=f.restore;r.owner=f.owner;r.pipeline=h<VkPipeline>(200);r.layout=h<VkPipelineLayout>(201);r.pipelineGeneration=r.layoutGeneration=1;
    r.setCount=2;r.sets={h<VkDescriptorSet>(202),h<VkDescriptorSet>(203)};r.setGenerations={1,1};r.completeStaticBindings=r.completePushConstants=true;
    r.pushCount=1;r.push[0].layout=r.layout;r.push[0].layoutGeneration=1;r.push[0].stages=VK_SHADER_STAGE_COMPUTE_BIT;r.push[0].size=4;r.push[0].bytes={1,2,3,4};
    f.jitterX=0.125f;f.jitterY=-0.25f;f.historyEpoch=5;
    f.nativeCoreReplaced=f.wholeCoreTransactionVerified=f.atAccumulateBoundary=true;
    f.pinnedModule=f.sameFrameInputs=f.domainAndDownstreamVerified=f.preExposureConventionVerified=f.motionConventionVerified=true;
    f.cameraHistoryVerified=f.applicationReferencesRetained=f.aliasCoverageComplete=f.sourcesUnchangedAtUse=true;return f;
}
void prepareLayouts(const so::Frame& f){
    layouts.clear();recordedBarriers.clear();for(const auto* image:{&f.color,&f.motion,&f.depth,&f.nativeOutput})layouts[image->sdk.image]=image->currentLayout;
    boundPipeline=f.restore.pipeline;boundSets={f.restore.sets[0],f.restore.sets[1]};pushBytes={1,2,3,4};
}
void restored(const so::Frame& f){
    require(boundPipeline==f.restore.pipeline&&boundSets==std::array{f.restore.sets[0],f.restore.sets[1]}&&pushBytes==std::array<std::uint8_t,4>{1,2,3,4},"SDK pipeline/descriptor/push mutations fully restored");
    for(const auto* image:{&f.color,&f.motion,&f.depth,&f.nativeOutput})require(layouts[image->sdk.image]==image->currentLayout,"all game original layouts restored");
}
void ready(so::Overlay& overlay,const sg::RuntimeRequirementsAdmission& a){
    require(overlay.start(so::Enable::On,outside,identity,a,config(),sdk(),vk()).status==so::Status::Building,"opt-in starts compilation only");
    require(overlay.pollBuild(outside).status==so::Status::NeedsInit,"successful prebuild permits host initialization");
    require(overlay.initialize(outside,config()).status==so::Status::Ready,"host initializes exact dimensions");
}
so::RecordingKey recording(){return {3,4,10,11};}
void retire(so::Overlay& o){
    require(o.submitted({recording(),h<VkQueue>(500),700,VK_SUCCESS}),"successful exact submit receipt retained");
    require(!o.completed({recording(),h<VkQueue>(501),700,VK_SUCCESS})&&!o.completed({recording(),h<VkQueue>(500),701,VK_SUCCESS})&&
        !o.completed({recording(),h<VkQueue>(500),700,VK_NOT_READY}),"wrong queue/serial or unfinished fence never frees resources");
    require(o.completed({recording(),h<VkQueue>(500),700,VK_SUCCESS}),"only exact successful fence completion retires SDK references");
}
}
namespace {
VkPhysicalDevice interopPhysicalObserved{};
void VKAPI_PTR interopProperties(VkPhysicalDevice physical,VkPhysicalDeviceProperties2* properties){
    interopPhysicalObserved=physical;properties->properties.vendorID=0x8086;
    // Deliberately unavailable LUID ends the mock before any DXGI/GPU access.
    static_cast<VkPhysicalDeviceIDProperties*>(properties->pNext)->deviceLUIDValid=VK_FALSE;
}
PFN_vkVoidFunction VKAPI_PTR interopInstanceProc(VkInstance instance,const char* name){
    require(instance==identity.instance&&std::string_view(name)=="vkGetPhysicalDeviceProperties2","interop uses the expected downstream instance query");
    return reinterpret_cast<PFN_vkVoidFunction>(interopProperties);
}
PFN_vkVoidFunction VKAPI_PTR interopDeviceProc(VkDevice,const char*){require(false,"LUID refusal precedes device dispatch");return nullptr;}
}
int main(int argc,char** argv){
    if(argc==2&&std::string_view(argv[1])=="--fsr4-layer-handle-test"){
        SetEnvironmentVariableW(L"XESS_SR_BACKEND",L"fsr4_xmx_lite");
        const auto admitted=admission();auto functions=vk();functions.gipa=interopInstanceProc;functions.gdpa=interopDeviceProc;functions.queueFamily=0;
        {so::Overlay missing;require(missing.start(so::Enable::On,outside,identity,admitted,config(),sdk(),functions).status==so::Status::SdkFailure,"missing downstream physical handle refuses before query");
            require(!interopPhysicalObserved,"missing handle never falls back to SDK public handle");}
        functions.physicalForInstanceDispatch=h<VkPhysicalDevice>(0x2002);
        {so::Overlay fsr4;require(fsr4.start(so::Enable::On,outside,identity,admitted,config(),sdk(),functions).status==so::Status::SdkFailure,"mock LUID refusal is reported");
            require(interopPhysicalObserved==functions.physicalForInstanceDispatch&&interopPhysicalObserved!=identity.physicalDevice,"bridge receives raw layer handle, not loader wrapper");
            require(std::string_view(fsr4.lastBackendError()).find("valid LUID")!=std::string_view::npos,"mock reached the physical-device query");}
        require(sdkCalls==0,"FSR4 handle-domain test never creates XeSS SR context");
        std::printf("{\"passed\":true,\"checks\":%u,\"publicAndLayerPhysicalDistinct\":true,\"gpuExecuted\":false}\n",checks);return 0;
    }
    const auto admitted=admission();
    {
        so::Overlay o;require(o.state()==so::State::Disabled&&o.record({}).status==so::Status::Disabled&&!creates&&!executes,"default OFF performs no SDK/Vulkan work");
        require(o.start(so::Enable::Off,so::HostPhase::Unknown,{},{},{},{},{}).status==so::Status::Disabled&&!creates,"typed OFF does not need runtime or valid handles");
        require(o.start(so::Enable::On,outside,identity,{},config(),sdk(),vk()).status==so::Status::RequirementsMissing&&!creates,"supported-only or absent effective-device admission refuses context creation");
        auto other=identity;++other.incarnation;
        require(o.start(so::Enable::On,outside,other,admitted,config(),sdk(),vk()).status==so::Status::RequirementsMissing&&!creates,"device-incarnation mismatch refuses creation");
        auto wrongSdk=sdk();wrongSdk.requirements.instanceExtensions=nullptr;
        require(o.start(so::Enable::On,outside,identity,admitted,config(),wrongSdk,vk()).status==so::Status::RequirementsMissing&&!creates,"requirements admission belongs to exact SDK exports");
        require(o.start(so::Enable::On,so::HostPhase::Unknown,identity,admitted,config(),sdk(),vk()).status==so::Status::WrongHostPhase&&!creates,"cannot initialize SDK from an unknown recording phase");
        auto native=config();native.input=native.output;native.input.height--;
        require(o.start(so::Enable::On,outside,identity,admitted,native,sdk(),vk()).status==so::Status::InvalidConfiguration&&!creates,
            "one equal axis never admits a near-native mixed-size configuration");
    }
    {
        so::Overlay o;pipelineStatus=XESS_RESULT_ERROR_OPERATION_IN_PROGRESS;
        require(o.start(so::Enable::On,outside,identity,admitted,config(),sdk(),vk()).status==so::Status::Building,"nonblocking build starts");
        require(o.pollBuild(outside).status==so::Status::Building&&o.initialize(outside,config()).status==so::Status::Building&&!inits,"in-progress build never blocks or initializes in first dispatch");
        require(o.record(frame()).status==so::Status::Building&&!executes,"FSR retained while pipelines compile");
        pipelineStatus=XESS_RESULT_SUCCESS;require(o.pollBuild(outside).status==so::Status::NeedsInit,"poll acknowledges completed prebuild");
        require(o.initialize(outside,config()).status==so::Status::Ready&&o.requestedQuality()==XESS_QUALITY_SETTING_QUALITY&&o.effectiveQuality()==XESS_QUALITY_SETTING_BALANCED&&
            velocityX==1128.f&&velocityY==-634.f&&rangeQueries==2,"exact1128x634 retained with queried103-to102 fallback and explicit pixel MV scales");
        const auto callsBeforePreflight=sdkCalls;
        const auto barriersBeforePreflight=recordedBarriers.size();
        for(unsigned n=0;n<60;++n)require(o.preflight(frame())==so::Status::Ready&&!o.pending()&&
            sdkCalls==callsBeforePreflight&&recordedBarriers.size()==barriersBeforePreflight,
            "preflight validates without SDK calls, command writes, slot reservation or history changes");
        for(unsigned mutation=0;mutation<23;++mutation){auto f=frame();switch(mutation){
            case 0:f.sameFrameInputs=false;break;case 1:f.finalFsrDispatchReturned=true;break;case 2:f.domainAndDownstreamVerified=false;break;
            case 3:f.preExposureConventionVerified=false;break;case 4:f.motionConventionVerified=false;break;case 5:f.cameraHistoryVerified=false;break;
            case 6:f.applicationReferencesRetained=false;break;case 7:f.aliasCoverageComplete=false;break;case 8:f.color.owner.frame++;break;
            case 9:f.recording.inRenderPass=true;break;case 10:f.recording.beginUsageFlags|=VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;break;
            case 11:f.nativeOutput.memory.bindingBoundsKnown=false;break;case 12:f.depth.currentLayout=VK_IMAGE_LAYOUT_UNDEFINED;break;
            case 13:f.color.sdk.format=VK_FORMAT_R8G8B8A8_UNORM;break;case 14:f.restore.completePushConstants=false;break;
            case 15:f.restore.completeStaticBindings=false;break;case 16:f.restore.setGenerations[1]=0;break;
            case 17:f.jitterX=std::numeric_limits<float>::quiet_NaN();break;case 18:f.color.usage&=~VK_IMAGE_USAGE_SAMPLED_BIT;break;
            case 19:f.nativeOutput.memory.memory=f.color.memory.memory;break;case 20:f.depth.barrierRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;break;
            case 21:f.nativeOutput.usage&=~VK_IMAGE_USAGE_STORAGE_BIT;break;
            case 22:f.configuration.velocityScaleY=-f.configuration.velocityScaleY;break;}
            const auto calls=sdkCalls,e=executes,c=copies;const auto preflight=o.preflight(f);
            require(preflight!=so::Status::Ready&&sdkCalls==calls&&!o.pending(),"invalid native replacement preflight has no side effects");
            const auto result=o.record(f);require(result.status==preflight,"record and replacement preflight share all validation gates");
            require(result.status!=so::Status::Recorded&&!o.pending()&&executes==e&&copies==c,"missing factual boundary leaves the command buffer untouched and cannot enable overlay");
            if(mutation==13)require(result.status==so::Status::UnsupportedFormatPair,"unrequested mixed pair is not automatically generalized from packed game color");
        }
        const auto beforeInit=inits;require(o.initialize(outside,config()).status==so::Status::Ready&&inits==beforeInit,"unchanged host polling does not reinitialize/reset every frame");
        canEnter=false;const auto beforeExecute=executes;
        require(o.record(frame()).status==so::Status::RecordingUnverified&&executes==beforeExecute&&!o.pending(),"missing real observer exclusion scope leaves FSR and metadata untouched");canEnter=true;
        auto f=frame();f.color.sdk.format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        f.color.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;f.nativeOutput.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        prepareLayouts(f);auto r=o.record(f);require(r.status==so::Status::Recorded&&r.resetApplied&&r.bindingsRestored&&r.executeSucceeded&&lastReset==1,"documented actual122-to97 pair uses sampled non-storage game input, resets history and commits after SDK success");restored(f);
        const auto i=inits,d=destroys;require(o.initialize(outside,config()).status==so::Status::PendingCommands&&o.stop(outside).status==so::Status::PendingCommands&&inits==i&&destroys==d,"recorded work prevents reinit/destroy before any submit");
        require(!o.deviceIdle(identity,VK_SUCCESS),"device-idle cannot retire a still-unsubmitted CB");
        auto stale=recording();++stale.recording;require(!o.discarded(stale,so::Discard::SuccessfulReset),"stale reset cannot release exact recording");
        require(!o.completed({recording(),h<VkQueue>(500),700,VK_SUCCESS}),"unsolicited completion cannot retire unsubmitted commands");
        retire(o);require(o.record(f).status==so::Status::DuplicateFrame,"duplicate same-frame callback never executes twice");
        f=frame(51);prepareLayouts(f);r=o.record(f);require(r.status==so::Status::Recorded&&!r.resetApplied&&!lastReset,"continuous completed frame retains history");restored(f);retire(o);
        f=frame(52);++f.historyEpoch;prepareLayouts(f);r=o.record(f);require(r.status==so::Status::Recorded&&r.resetApplied,"verified camera-epoch change resets history");retire(o);
        f=frame(53);prepareLayouts(f);r=o.record(f);require(r.status==so::Status::Recorded,"next frame records");
        require(o.record(frame(54)).status==so::Status::PendingCommands,"occupied single slot skips without waiting");retire(o);
        f=frame(55);prepareLayouts(f);r=o.record(f);require(r.resetApplied,"skipped frames reset next successful history");
        require(o.discarded(recording(),so::Discard::SuccessfulReset),"successful exact unsubmitted-reset releases slot");
        auto resized=config();resized.output={1280,720};resized.input={753,424};
        require(o.initialize(outside,resized).status==so::Status::Ready,"resolution/preset reinit allowed after real quiescence");
        resized.input={852,480};const auto queriesBeforeResize=rangeQueries;
        require(o.initialize(outside,resized).status==so::Status::Ready&&o.requestedQuality()==XESS_QUALITY_SETTING_ULTRA_QUALITY&&
            o.effectiveQuality()==XESS_QUALITY_SETTING_QUALITY&&rangeQueries==queriesBeforeResize+2,
            "rounded 720p high-quality input selects a queried legal adjacent range without changing pixels");
        auto quality=config();quality.input={1280,720};
        require(o.initialize(outside,quality).status==so::Status::Ready&&o.effectiveQuality()==XESS_QUALITY_SETTING_ULTRA_QUALITY,
            "full-size high quality returns to its nominal SDK quality after resize");
        resized.input={426,240};const auto initBeforeUnsupported=inits;
        require(o.initialize(outside,resized).status==so::Status::UnsupportedInputExtent&&inits==initBeforeUnsupported&&o.state()==so::State::Ready,
            "input below every SDK range preserves the live context while refusing unsupported input, without enabling FSR");
        const auto queriesAfterUnsupported=rangeQueries;
        for(unsigned n=0;n<60;++n)require(o.initialize(outside,resized).status==so::Status::UnsupportedInputExtent&&rangeQueries==queriesAfterUnsupported,
            "same unsupported resize never polls SDK ranges on every host boundary");
        require(o.initialize(outside,quality).status==so::Status::Ready&&rangeQueries==queriesAfterUnsupported,
            "returning to the already initialized configuration immediately recovers after unsupported input");
        require(o.initialize(outside,config()).status==so::Status::Ready,"restore original input/output config before further recording");
        auto flagsChanged=config();flagsChanged.domain=so::ColorDomain::LinearLdr;
        require(o.initialize(outside,flagsChanged).status==so::Status::InvalidConfiguration,"changed flags require fresh asynchronous context/prebuild");
        require(o.stop(outside).status==so::Status::Disabled,"idle context destroys normally");
    }
    {
        so::Overlay o;auto jittered=config();jittered.jitteredMotion=true;
        require(o.start(so::Enable::On,outside,identity,admitted,jittered,sdk(),vk()).status==so::Status::Building&&
            (builtFlags&XESS_INIT_FLAG_JITTERED_MV),"verified jittered source vectors select the SDK motion convention during prebuild");
        require(o.pollBuild(outside).status==so::Status::NeedsInit&&o.initialize(outside,jittered).status==so::Status::Ready,
            "jittered motion initializes with the identical prebuilt flags");
        auto f=frame();f.configuration=jittered;prepareLayouts(f);
        require(o.record(f).status==so::Status::Recorded,"jittered motion executes without an extra image conversion or copy");retire(o);
        f=frame(51);const auto before=executes;
        require(o.record(f).status==so::Status::NeedsInit&&executes==before,
            "a different source motion convention cannot reuse the initialized context");
        require(o.initialize(outside,config()).status==so::Status::InvalidConfiguration,
            "changing the jittered motion flag needs a fresh context and asynchronous prebuild");
        require(o.stop(outside).status==so::Status::Disabled,"jittered context retires normally");
    }
    {so::Overlay o;ready(o,admitted);auto f=frame();f.nativeCoreReplaced=false;f.finalFsrDispatchReturned=true;
        const auto before=executes;require(o.record(f).status==so::Status::FrameUnverified&&executes==before,"retired FSR-plus-XeSS overlay is rejected by the SDK boundary itself");
        require(o.stop(outside).status==so::Status::Disabled,"rejected overlay creates no pending work");}
    {
        so::Overlay o;ready(o,admitted);auto f=frame();prepareLayouts(f);executeStatus=XESS_RESULT_ERROR_DEVICE;const auto copied=copies;
        const auto result=o.record(f);require(result.status==so::Status::SdkFailureCommandsUncertain&&!result.executeSucceeded&&copies==copied&&o.pending()&&o.state()==so::State::Failed,"SDK failure reports uncertain game output and retains references");
        retire(o);require(o.record(frame(51)).status==so::Status::SdkFailure,"failed context stays disabled until explicit stop/start");
        require(o.stop(outside).status==so::Status::Disabled,"failed context destroys only after exact GPU completion");executeStatus=XESS_RESULT_SUCCESS;
    }
    for(const auto error:{VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        so::Overlay o;ready(o,admitted);auto f=frame();prepareLayouts(f);o.record(f);const auto beforeDestroy=destroys;
        require(!o.submitted({recording(),h<VkQueue>(500),700,error})&&o.pending(),"OOM failure retains an executable, unsubmitted recording");
        require(!o.deviceIdle(identity,VK_SUCCESS)&&o.pending()&&!o.completed({recording(),h<VkQueue>(500),700,VK_SUCCESS}),"successful idle/fence cannot retire commands that failed to enqueue");
        require(o.initialize(outside,config()).status==so::Status::PendingCommands&&o.stop(outside).status==so::Status::PendingCommands&&destroys==beforeDestroy,"OOM plus idle cannot enable reinit or context destruction");
        require(o.discarded(recording(),so::Discard::SuccessfulReset)&&o.stop(outside).status==so::Status::Disabled,"exact successful reset removes future-submit OOM references");
    }
    {
        so::Overlay o;ready(o,admitted);auto f=frame();prepareLayouts(f);o.record(f);
        require(!o.submitted({recording(),h<VkQueue>(500),700,VK_ERROR_OUT_OF_HOST_MEMORY})&&!o.deviceIdle(identity,VK_SUCCESS),"failed enqueue stays recorded through an idle wait");
        require(o.submitted({recording(),h<VkQueue>(500),701,VK_SUCCESS}),"real successful retry changes an unsubmitted recording to submitted");
        require(!o.discarded(recording(),so::Discard::FreeObserved)&&!o.completed({recording(),h<VkQueue>(500),700,VK_SUCCESS}),"retry retires neither through discard nor through the failed first serial");
        require(o.completed({recording(),h<VkQueue>(500),701,VK_SUCCESS})&&o.stop(outside).status==so::Status::Disabled,"successful retry needs its exact successful fence before destruction");
    }
    {
        so::Overlay o;ready(o,admitted);auto f=frame();prepareLayouts(f);o.record(f);const auto beforeDestroy=destroys;
        require(!o.submitted({recording(),h<VkQueue>(500),700,VK_ERROR_UNKNOWN}),"unclassified failure is not invented as successfully submitted");
        require(!o.deviceIdle(identity,VK_SUCCESS)&&!o.discarded(recording(),so::Discard::FreeObserved)&&o.pending(),"unknown submission cannot retire through idle-only or discard-only proof");
        require(o.stop(outside).status==so::Status::PendingCommands&&destroys==beforeDestroy,"unclassified failure conservatively retains context");
    }
    {
        so::Overlay o;ready(o,admitted);auto f=frame();prepareLayouts(f);o.record(f);
        require(!o.submitted({recording(),h<VkQueue>(500),700,VK_ERROR_DEVICE_LOST})&&o.pending(),"device-lost submit retains possibly-pending context/resources, unlike unchanged OOM recording");
        require(!o.completed({recording(),h<VkQueue>(500),700,VK_SUCCESS})&&!o.discarded(recording(),so::Discard::FreeObserved),"device loss is not proof of an unsubmitted/reset recording");
        require(!o.deviceIdle(identity,VK_ERROR_DEVICE_LOST)&&o.pending(),"failed wait retains uncertain commands");
        require(o.deviceIdle(identity,VK_SUCCESS)&&!o.pending(),"actual successful whole-device idle can release submitted uncertainty");
        require(o.stop(outside).status==so::Status::Disabled,"quarantined context destroys after genuine idle");
    }
    {
        so::Overlay o;ready(o,admitted);auto f=frame();prepareLayouts(f);o.record(f);
        require(o.submitted({recording(),h<VkQueue>(500),700,VK_SUCCESS})&&!o.submitted({recording(),h<VkQueue>(500),701,VK_SUCCESS}),"duplicate one-time recording submission quarantines context");
        require(!o.completed({recording(),h<VkQueue>(500),700,VK_SUCCESS})&&o.pending()&&o.state()==so::State::Failed,"first receipt cannot hide later duplicate use");
        require(o.deviceIdle(identity,VK_SUCCESS)&&o.stop(outside).status==so::Status::Disabled,"duplicate-use uncertainty retires only after actual device idle");
    }
    {
        so::Overlay o;
        require(o.start(so::Enable::On,outside,identity,admitted,config(),sdk(),vk()).status==so::Status::Building,"initial actual caller thread owns SDK lifetime");
        const auto calls=sdkCalls;std::array<so::Status,5> results{};
        std::thread worker([&]{
            results={o.start(so::Enable::On,outside,identity,admitted,config(),sdk(),vk()).status,o.pollBuild(outside).status,
                o.initialize(outside,config()).status,o.record(frame()).status,o.stop(outside).status};
        });worker.join();
        for(const auto result:results)require(result==so::Status::WrongThread,"start/poll/init/record/stop reject a serialized foreign thread");
        require(sdkCalls==calls&&o.state()==so::State::Building&&!o.pending(),"wrong-thread calls neither touch SDK nor mutate lifecycle");
        require(o.pollBuild(outside).status==so::Status::NeedsInit&&o.initialize(outside,config()).status==so::Status::Ready,"initial thread still initializes after rejected foreign calls");
        auto f=frame();prepareLayouts(f);require(o.record(f).status==so::Status::Recorded,"initial thread records after successful initialization");retire(o);
        const auto beforeWrongRecord=sdkCalls,beforeEnters=enters;so::Status foreignRecord{};
        std::thread recordingWorker([&]{foreignRecord=o.record(frame(51)).status;});recordingWorker.join();
        require(foreignRecord==so::Status::WrongThread&&sdkCalls==beforeWrongRecord&&enters==beforeEnters&&!o.pending(),"wrong-thread ready-frame Execute is rejected before SDK, exclusion scope or commands");
        f=frame(51);prepareLayouts(f);const auto result=o.record(f);
        require(result.status==so::Status::Recorded&&!result.resetApplied,"foreign rejection does not poison owner-thread temporal state");retire(o);
        require(o.stop(outside).status==so::Status::Disabled,"owner thread destroys its idle context");
        const auto beforeRestart=sdkCalls;so::Status foreignRestart{};
        std::thread restartingWorker([&]{foreignRestart=o.start(so::Enable::On,outside,identity,admitted,config(),sdk(),vk()).status;});restartingWorker.join();
        require(foreignRestart==so::Status::WrongThread&&sdkCalls==beforeRestart,"stop does not silently migrate a component to another SDK thread");
    }
    {
        auto* o=new so::Overlay;ready(*o,admitted);const auto calls=sdkCalls,beforeDestroy=destroys;
        std::thread worker([o]{delete o;});worker.join();
        require(sdkCalls==calls&&destroys==beforeDestroy,"wrong-thread destructor retains context without calling SDK destroy");
    }
    {
        const auto beforeDestroy=destroys;
        {so::Overlay o;ready(o,admitted);}
        require(destroys==beforeDestroy+1,"idle destructor on initial SDK thread destroys normally");
    }
    for(unsigned failure=0;failure<6;++failure){
        so::Overlay o;const auto beforeDestroy=destroys,beforeExecute=executes;
        if(failure==0)createStatus=XESS_RESULT_ERROR_DEVICE;
        if(failure==1)scaleStatus=XESS_RESULT_ERROR_DEVICE;
        if(failure==2)buildResult=XESS_RESULT_ERROR_DEVICE;
        if(failure==3)pipelineStatus=XESS_RESULT_ERROR_DEVICE;
        if(failure==4)initStatus=XESS_RESULT_ERROR_DEVICE;
        if(failure==5)velocityStatus=XESS_RESULT_ERROR_DEVICE;
        auto result=o.start(so::Enable::On,outside,identity,admitted,config(),sdk(),vk());
        if(result.status==so::Status::Building)result=o.pollBuild(outside);
        if(result.status==so::Status::NeedsInit)result=o.initialize(outside,config());
        require(result.status==so::Status::SdkFailure&&o.record(frame()).status==so::Status::SdkFailure&&executes==beforeExecute,"each startup/init SDK failure closes Execute path and retains FSR");
        require(o.stop(outside).status==so::Status::Disabled&&destroys==beforeDestroy+1,"failed startup cleans its partial non-null context outside recording");
        createStatus=scaleStatus=buildResult=pipelineStatus=initStatus=velocityStatus=XESS_RESULT_SUCCESS;
    }
    {
        so::Overlay o;expectedBlocking=true;
        const auto polls=buildStatusCalls;pipelineStatus=XESS_RESULT_ERROR_WRONG_CALL_ORDER;
        require(o.start(so::Enable::On,outside,identity,admitted,config(),sdk(),vk(),3,true).status==so::Status::NeedsInit,"successful blocking build is ready for init without async status polling");
        require(o.pollBuild(outside).status==so::Status::NeedsInit&&buildStatusCalls==polls&&o.initialize(outside,config()).status==so::Status::Ready,"blocking game startup never asks for a nonexistent async build result");
        pipelineStatus=XESS_RESULT_SUCCESS;
        std::array<so::RecordingKey,3> keys;
        for(unsigned n=0;n<3;++n){auto f=frame(80+n);f.owner.command=f.recording.command=10+n;
            for(auto* i:{&f.color,&f.motion,&f.depth,&f.nativeOutput})i->owner=f.owner;f.restore.owner=f.owner;
            expectedCommand=h<VkCommandBuffer>(10+n);prepareLayouts(f);
            require(o.record(f).executeSucceeded,"three distinct CBs may retain SDK references without waiting");
            keys[n]={3,4,10+n,11};require(o.submitted({keys[n],h<VkQueue>(500),800+n,VK_SUCCESS}),"each native submission retains its own reference");
        }
        auto f=frame(84);f.owner.command=f.recording.command=13;for(auto* i:{&f.color,&f.motion,&f.depth,&f.nativeOutput})i->owner=f.owner;f.restore.owner=f.owner;
        expectedCommand=h<VkCommandBuffer>(13);prepareLayouts(f);const auto count=executes;
        require(!o.record(f).executeSucceeded&&executes==count,"full ring retains FSR rather than waiting or evicting a pending frame");
        require(!o.retired({3,4,10,12},so::Discard::SuccessfulReset)&&o.pending(),"stale recording retirement cannot release a live reference");
        require(o.retired(keys[0],so::Discard::SuccessfulReset),"actual successful CB reset releases exactly one recording");
        require(o.record(f).executeSucceeded,"retired slot is reusable without a GPU wait");
        require(o.retired({3,4,13,11},so::Discard::SuccessfulReset),"discarding an unsubmitted recording releases its slot");
        o.poolRetired(9,so::Discard::SuccessfulPoolReset);require(!o.pending(),"actual pool reset releases all its submitted references");
        require(o.stop(outside).status==so::Status::Disabled,"drained ring destroys SDK context on its owner thread");
        expectedBlocking=false;expectedCommand=h<VkCommandBuffer>(10);
    }
    for(const auto status:{XESS_RESULT_SUCCESS,XESS_RESULT_ERROR_UNINITIALIZED,XESS_RESULT_ERROR_DEVICE}){
        so::Overlay o;auto v=vk();v.blit=cmdBlit;
        require(o.start(so::Enable::On,outside,identity,admitted,config(),sdk(),v).status==so::Status::Building,"replacement uses same admitted SDK context");
        require(o.pollBuild(outside).status==so::Status::NeedsInit&&o.initialize(outside,config()).status==so::Status::Ready,"replacement context initialized outside command recording");
        auto f=frame();f.nativeCoreReplaced=f.wholeCoreTransactionVerified=f.atAccumulateBoundary=true;f.finalFsrDispatchReturned=false;
        f.color.formatFeatures|=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        f.nativeOutput.formatFeatures|=VK_FORMAT_FEATURE_BLIT_DST_BIT;prepareLayouts(f);
        auto bad=f;bad.finalFsrDispatchReturned=true;require(o.preflight(bad)==so::Status::FrameUnverified,"replacement cannot claim native dispatch returned");
        bad=f;bad.wholeCoreTransactionVerified=false;require(o.preflight(bad)==so::Status::FrameUnverified,"replacement requires whole-frame transaction receipt");
        const auto previousCopies=copies;executeStatus=status;const auto result=o.record(f);executeStatus=XESS_RESULT_SUCCESS;
        require(result.executeSucceeded==(status>=0)&&result.fallbackRecorded==(status==XESS_RESULT_ERROR_UNINITIALIZED),"only validated SDK failure uses blit, never a device failure");
        require(copies==previousCopies+unsigned(status==XESS_RESULT_ERROR_UNINITIALIZED),"exactly one fallback only when recording remains legal");
        if(status==XESS_RESULT_ERROR_DEVICE)require(recordedBarriers.size()==4&&!result.bindingsRestored,"device failure records no speculative restore or extra barriers");
        else for(const auto* i:{&f.color,&f.motion,&f.depth,&f.nativeOutput})require(layouts[i->sdk.image]==i->currentLayout,"replacement/fallback restores every game layout");
        retire(o);require(o.stop(outside).status==so::Status::Disabled,"replacement references use existing submission/retirement lifetime");
    }
    {
        executeStatus=XESS_RESULT_SUCCESS;so::Overlay o;ready(o,admitted);auto f=frame(1000);prepareLayouts(f);
        require(o.record(f).executeSucceeded,"duplicate admission fixture starts with one valid frame");retire(o);
        require(o.availability(config(),recording(),1000)==so::Status::DuplicateFrame&&
            o.availability(config(),recording(),999)==so::Status::DuplicateFrame&&
            o.availability(config(),recording(),1001)==so::Status::Ready,
            "Prepare admission refuses already executed frames before any native core is suppressed");
        require(o.stop(outside).status==so::Status::Disabled,"duplicate admission check leaves the SDK context intact");
    }
    {
        auto f=frame(500);f.color.sdk.format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        f.nativeCoreReplaced=f.wholeCoreTransactionVerified=f.atAccumulateBoundary=true;f.finalFsrDispatchReturned=false;
        f.color.formatFeatures|=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        f.nativeOutput.formatFeatures|=VK_FORMAT_FEATURE_BLIT_DST_BIT;
        // Menu transitions can invalidate temporal inputs or compute restoration,
        // while the same-frame final color pair remains fully usable by transfer.
        f.motion={};f.depth={};f.restore={};auto v=vk();v.blit=cmdBlit;
        const auto beforeSdk=sdkCalls,beforeCopies=copies;prepareLayouts(f);
        const auto result=so::recordPreExecuteColorFallback(f,v);
        require(result.fallbackRecorded&&!result.executeSucceeded&&copies==beforeCopies+1&&sdkCalls==beforeSdk,
            "pre-Execute refusal fills the exact output without any SDK call or temporal input");
        require(layouts[f.color.sdk.image]==f.color.currentLayout&&layouts[f.nativeOutput.sdk.image]==f.nativeOutput.currentLayout,
            "pre-Execute fallback restores both actual color layouts");
        for(unsigned variant=0;variant<9;++variant){auto bad=f;
            if(variant==0)bad.sameFrameInputs=false;
            if(variant==1)bad.recording.inRenderPass=true;
            if(variant==2)++bad.color.viewGeneration,bad.color.imageGeneration=0;
            if(variant==3)bad.nativeOutput.memory.memory=bad.color.memory.memory;
            if(variant==4)bad.nativeOutput.usage&=~VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if(variant==5)bad.owner.prefix=0;
            if(variant==6)bad.color.sdk.format=VK_FORMAT_R8G8B8A8_UNORM;
            if(variant==7)bad.finalFsrDispatchReturned=true;
            if(variant==8)bad.color.sdk.subresourceRange.levelCount=2;
            const auto before=recordedBarriers.size();const auto count=copies;
            require(!so::recordPreExecuteColorFallback(bad,v).fallbackRecorded&&copies==count&&recordedBarriers.size()==before&&sdkCalls==beforeSdk,
                "invalid ownership, alias, format, usage or scope never records a speculative fallback");
        }
    }
    require(!entered&&enters==leaves,"every admitted SDK recording leaves observer exclusion scope including negative SDK result");
    {
        so::Overlay aa;auto c=config();c.input=c.output;expectedInput=c.input;c.velocityScaleX=float(c.input.width);c.velocityScaleY=-float(c.input.height);
        require(aa.start(so::Enable::On,outside,identity,admitted,c,sdk(),vk()).status==so::Status::Building,"1:1 AA starts without a scaling workaround");
        require(aa.pollBuild(outside).status==so::Status::NeedsInit&&aa.initialize(outside,c).status==so::Status::Ready&&
            aa.requestedQuality()==XESS_QUALITY_SETTING_AA&&aa.effectiveQuality()==XESS_QUALITY_SETTING_AA,"native AA uses exact requested/effective quality 106");
        auto f=frame();f.configuration=c;
        for(auto* image:{&f.color,&f.motion,&f.depth}){image->sdk.width=c.input.width;image->sdk.height=c.input.height;}
        prepareLayouts(f);require(aa.record(f).executeSucceeded,"1920x1080 to 1920x1080 executes and restores game bindings");restored(f);retire(aa);
        require(aa.stop(outside).status==so::Status::Disabled,"AA context retires on real completion");
    }
    std::printf("{\"event\":\"sr_overlay_cpu_test\",\"passed\":true,\"checks\":%u,\"sdkCallsMocks\":%u,\"contextCreates\":%u,\"buildMocks\":%u,\"executeMocks\":%u,\"copiesMocks\":%u,\"gpuExecuted\":false,\"sdkLoaded\":false}\n",checks,sdkCalls,creates,builds,executes,copies);
}
