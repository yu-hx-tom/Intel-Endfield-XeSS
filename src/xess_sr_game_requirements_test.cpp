#include "xess_sr_game_requirements.hpp"
#include "sr_game_creation.hpp"
#include <vulkan/vk_layer.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {
namespace game=xess_vk::sr_game;
constexpr const char* instanceExtension="VK_KHR_get_physical_device_properties2";
constexpr const char* deviceExtension="VK_EXT_mutable_descriptor_type";
enum class Mode {Normal,InstanceFailure,DeviceFailure,FeatureFailure,Unknown,Cycle,Duplicate,PromotedConflict,InvalidBoolean,ExtraRequired,Split};
struct FakeSdk {
    Mode mode{};
    unsigned instanceCalls{},deviceCalls{},featureCalls{};
    std::string instanceName=instanceExtension,deviceName=deviceExtension;
    std::array<const char*,1> instanceNames{},deviceNames{};
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableType{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features int8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES};
    VkPhysicalDeviceShaderIntegerDotProductFeatures dot{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES};
    VkBaseOutStructure unknown{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkBaseOutStructure duplicate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
} sdk;
xess_result_t instanceQuery(std::uint32_t* count,const char* const** names,std::uint32_t* minimum) {
    ++sdk.instanceCalls;
    if(sdk.mode==Mode::InstanceFailure)return XESS_RESULT_ERROR_UNSUPPORTED_DRIVER;
    sdk.instanceNames[0]=sdk.instanceName.c_str();*count=1;*names=sdk.instanceNames.data();*minimum=VK_API_VERSION_1_1;
    return XESS_RESULT_SUCCESS;
}
xess_result_t deviceQuery(VkInstance,VkPhysicalDevice,std::uint32_t* count,const char* const** names) {
    ++sdk.deviceCalls;
    if(sdk.mode==Mode::DeviceFailure)return XESS_RESULT_ERROR_UNSUPPORTED_DRIVER;
    sdk.deviceNames[0]=sdk.deviceName.c_str();*count=1;*names=sdk.deviceNames.data();return XESS_RESULT_SUCCESS;
}
xess_result_t featureQuery(VkInstance,VkPhysicalDevice,void** head) {
    ++sdk.featureCalls;
    if(sdk.mode==Mode::FeatureFailure)return XESS_RESULT_ERROR_UNSUPPORTED_DRIVER;
    auto* core=static_cast<VkPhysicalDeviceFeatures2*>(*head);
    core->features.shaderStorageImageWriteWithoutFormat=VK_TRUE;core->pNext=&sdk.mutableType;
    sdk.mutableType.mutableDescriptorType=VK_TRUE;sdk.mutableType.pNext=&sdk.v12;
    sdk.v12.shaderInt8=VK_TRUE;sdk.v12.scalarBlockLayout=VK_TRUE;sdk.v12.pNext=&sdk.v13;
    sdk.v13.shaderIntegerDotProduct=VK_TRUE;
    switch(sdk.mode) {
    case Mode::Unknown:sdk.v13.pNext=&sdk.unknown;break;
    case Mode::Cycle:sdk.v13.pNext=core;break;
    case Mode::Duplicate:sdk.v13.pNext=&sdk.duplicate;break;
    case Mode::PromotedConflict:sdk.v13.pNext=&sdk.int8;break;
    case Mode::InvalidBoolean:sdk.v12.shaderInt8=2;break;
    case Mode::ExtraRequired:sdk.v12.timelineSemaphore=VK_TRUE;break;
    case Mode::Split:
        sdk.mutableType.pNext=&sdk.int8;sdk.int8.shaderInt8=VK_TRUE;sdk.int8.pNext=&sdk.scalar;
        sdk.scalar.scalarBlockLayout=VK_TRUE;sdk.scalar.pNext=&sdk.dot;sdk.dot.shaderIntegerDotProduct=VK_TRUE;break;
    default:break;
    }
    return XESS_RESULT_SUCCESS;
}
const game::RequiredQueries queries{instanceQuery,deviceQuery,featureQuery};
enum class SupportMode {Normal,MissingExtension,MissingFeature,EnumerationFailure,Incomplete};
SupportMode supportMode{};
unsigned supportExtensionCalls{},supportFeatureCalls{};
VkPhysicalDevice supportPhysical{};
VkResult VKAPI_CALL extensionSupport(VkPhysicalDevice physical,const char*,std::uint32_t* count,VkExtensionProperties* properties) {
    ++supportExtensionCalls;supportPhysical=physical;
    if(supportMode==SupportMode::EnumerationFailure)return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(!properties) {*count=1;return VK_SUCCESS;}
    if(supportMode==SupportMode::Incomplete)return VK_INCOMPLETE;
    *count=1;std::strcpy(properties[0].extensionName,supportMode==SupportMode::MissingExtension?"VK_other":deviceExtension);
    return VK_SUCCESS;
}
template<class T> void allSupported(T* target,std::size_t first,std::size_t last) {
    const VkBool32 enabled=VK_TRUE;
    for(auto at=first;at<last;at+=sizeof(VkBool32))std::memcpy(reinterpret_cast<unsigned char*>(target)+at,&enabled,sizeof(enabled));
}
void VKAPI_CALL featureSupport(VkPhysicalDevice physical,VkPhysicalDeviceFeatures2* core) {
    ++supportFeatureCalls;supportPhysical=physical;
    for(auto* base=reinterpret_cast<VkBaseOutStructure*>(core);base;base=base->pNext) {
#define SUPPORT(T,first,last) allSupported(reinterpret_cast<T*>(base),offsetof(T,first),offsetof(T,last)+sizeof(VkBool32))
        switch(base->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            allSupported(core,offsetof(VkPhysicalDeviceFeatures2,features),offsetof(VkPhysicalDeviceFeatures2,features)+sizeof(VkPhysicalDeviceFeatures));break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            SUPPORT(VkPhysicalDeviceVulkan12Features,samplerMirrorClampToEdge,subgroupBroadcastDynamicId);
            if(supportMode==SupportMode::MissingFeature)reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(base)->shaderInt8=VK_FALSE;break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:SUPPORT(VkPhysicalDeviceVulkan13Features,robustImageAccess,maintenance4);break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT:SUPPORT(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT,mutableDescriptorType,mutableDescriptorType);break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:SUPPORT(VkPhysicalDeviceShaderFloat16Int8Features,shaderFloat16,shaderInt8);break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:SUPPORT(VkPhysicalDeviceShaderIntegerDotProductFeatures,shaderIntegerDotProduct,shaderIntegerDotProduct);break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:SUPPORT(VkPhysicalDeviceScalarBlockLayoutFeatures,scalarBlockLayout,scalarBlockLayout);break;
        default:throw std::runtime_error("Unexpected support query structure");
        }
#undef SUPPORT
    }
}
const game::DeviceSupportQueries supportQueries{extensionSupport,featureSupport};
game::DeviceRequirementsPlan devicePlanFor(const game::RuntimeRequirements& required,const VkDeviceCreateInfo& original,
    xess_vk::AllowBorrowedLoaderChain loader=xess_vk::AllowBorrowedLoaderChain::No) {
    return game::planDeviceRequirements(required,original,true,loader,supportQueries);
}
struct Fixture {
    game::RuntimeIdentity identity{reinterpret_cast<VkInstance>(std::uintptr_t{1}),reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{2}),
        reinterpret_cast<VkDevice>(std::uintptr_t{3}),7};
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableType{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features int8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES};
    VkPhysicalDeviceShaderIntegerDotProductFeatures dot{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES};
    VkDevicePrivateDataCreateInfo unrelated{VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO};
    const char* instanceNames[2]{"VK_APP_unrelated",instanceExtension};
    const char* deviceNames[2]{"VK_APP_unrelated",deviceExtension};
    Fixture() {
        application.apiVersion=VK_API_VERSION_1_3;instance.pApplicationInfo=&application;
        instance.enabledExtensionCount=2;instance.ppEnabledExtensionNames=instanceNames;
        device.enabledExtensionCount=2;device.ppEnabledExtensionNames=deviceNames;
        device.pNext=&unrelated;unrelated.pNext=&core;
        core.pNext=&mutableType;core.features.shaderStorageImageWriteWithoutFormat=VK_TRUE;
        mutableType.mutableDescriptorType=VK_TRUE;mutableType.pNext=&v12;
        v12.shaderInt8=VK_TRUE;v12.scalarBlockLayout=VK_TRUE;v12.pNext=&v13;v13.shaderIntegerDotProduct=VK_TRUE;
    }
    game::RuntimeRequirements query(Mode mode=Mode::Normal) {
        sdk=FakeSdk{};sdk.mode=mode;
        return game::queryRuntimeRequirements(queries,identity.instance,identity.physicalDevice);
    }
    game::RuntimeRequirementsAdmission admit(const game::RuntimeRequirements& required,VkResult result=VK_SUCCESS) {
        return game::admitRuntimeRequirements(required,identity,instance,device,VK_SUCCESS,result);
    }
};
} // namespace
int main() {
    unsigned checks{};
    const auto check=[&](bool passed,const char* reason) {++checks;if(!passed)throw std::runtime_error(reason);};
    using Status=game::RequirementsStatus;
    Fixture f;
    {
        sdk=FakeSdk{};
        game::Creation creation;
        check(creation.prepareInstance(false,queries,f.instance)==&f.instance&&!sdk.instanceCalls&&!sdk.deviceCalls&&!sdk.featureCalls,
            "OFF forwards exact instance parameters without querying the SDK");
        creation.instanceCreated(f.identity.instance,VK_SUCCESS);
        check(!creation.available()&&!creation.deviceRequirements(f.identity.physicalDevice).queried(),"OFF cannot admit later device requirements");
    }
    {
        sdk=FakeSdk{};
        game::Creation creation;
        auto app=f.application;auto instance=f.instance;instance.pApplicationInfo=&app;
        const char* names[]{"VK_APP_unrelated",instanceExtension};instance.ppEnabledExtensionNames=names;
        const auto* effective=creation.prepareInstance(true,queries,instance);
        check(effective!=&instance&&effective->pApplicationInfo!=&app&&sdk.instanceCalls==1&&!sdk.deviceCalls,
            "instance requirements and enabled facts are captured before original create");
        creation.instanceCreated(f.identity.instance,VK_SUCCESS);
        app.apiVersion=VK_API_VERSION_1_0;names[0]=names[1]="overwritten";
        auto first=creation.deviceRequirements(f.identity.physicalDevice);
        auto second=creation.deviceRequirements(f.identity.physicalDevice);
        check(first.queried()&&second.queried()&&sdk.instanceCalls==1&&sdk.deviceCalls==2&&sdk.featureCalls==2,
            "each device uses immutable pre-instance requirements without re-querying instance requirements");
        check(creation.admit(first,f.identity,f.device,VK_SUCCESS).admitted(),"successful creation admission survives overwritten application stack parameters");
        check(!creation.admit(first,f.identity,f.device,VK_ERROR_DEVICE_LOST).admitted(),"failed downstream device creation never admits SR");
    }
    for(const auto mode:{Mode::InstanceFailure,Mode::Normal}) {
        sdk=FakeSdk{};sdk.mode=mode;game::Creation creation;
        const auto* effective=creation.prepareInstance(true,queries,f.instance);
        check((effective==&f.instance)==(mode==Mode::InstanceFailure),"instance query failure falls back before downstream creation");
        creation.instanceCreated(f.identity.instance,VK_ERROR_INITIALIZATION_FAILED);
        check(!creation.available()&&!creation.deviceRequirements(f.identity.physicalDevice).queried(),"failed instance creation cannot leak admission");
    }
    game::RuntimeRequirements empty;
    check(!f.admit(empty).admitted(),"Unqueried requirements refuse");
    auto required=f.query();
    check(required.queried()&&sdk.instanceCalls==1&&sdk.deviceCalls==1&&sdk.featureCalls==1,"Every runtime requirement API must execute exactly once");
    const auto admission=f.admit(required);
    check(admission.admitted()&&admission.matches(f.identity)&&admission.matchesQueries(queries),"Successful effective creation contract admits exact identity and SDK exports");
    check(f.device.pNext==&f.unrelated&&f.unrelated.pNext==&f.core
        &&f.core.pNext==&f.mutableType&&f.mutableType.pNext==&f.v12&&f.v12.pNext==&f.v13,
        "Validation leaves every borrowed application chain link unchanged");
    sdk=FakeSdk{};sdk.instanceName="overwritten";sdk.deviceName="overwritten";
    check(required.instanceExtensions().size()==1&&required.instanceExtensions()[0]==instanceExtension
        &&required.deviceExtensions()[0]==deviceExtension&&f.admit(required).admitted(),"Owned snapshot survives overwritten SDK names, boolean fields and pNext links");
    auto moved=std::move(required);
    check(!required.queried()&&!f.admit(required).admitted()&&f.admit(moved).admitted(),"Moved snapshot keeps its evidence in one owner");
    auto other=f.identity;++other.incarnation;check(!admission.matches(other),"Handle reuse with another incarnation refuses");
    other=f.identity;other.device=reinterpret_cast<VkDevice>(std::uintptr_t{33});check(!admission.matches(other),"Another device refuses");
    auto otherQueries=queries;otherQueries.deviceFeatures=nullptr;check(!admission.matchesQueries(otherQueries),"Another SDK export set refuses");
    auto savedIdentity=f.identity;f.identity.physicalDevice=reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{22});
    check(f.admit(moved).status()==Status::IdentityMismatch,"Another physical device cannot reuse query evidence");f.identity=savedIdentity;
    f.identity.incarnation=0;check(!f.admit(moved).admitted(),"Missing incarnation refuses");f.identity=savedIdentity;
    check(f.admit(moved,VK_ERROR_FEATURE_NOT_PRESENT).status()==Status::CreationFailed,"Failed creation never establishes enabled capabilities");
    check(game::admitRuntimeRequirements(moved,f.identity,f.instance,f.device,VK_ERROR_INITIALIZATION_FAILED,VK_SUCCESS).status()==Status::CreationFailed,"Failed instance creation refuses");
    f.application.apiVersion=VK_API_VERSION_1_0;check(f.admit(moved).status()==Status::InstanceApiTooLow,"Minimum runtime API is enforced");f.application.apiVersion=VK_API_VERSION_1_3;
    f.instance.enabledExtensionCount=1;check(f.admit(moved).status()==Status::MissingInstanceExtension,"Missing SDK instance extension refuses");f.instance.enabledExtensionCount=2;
    f.device.enabledExtensionCount=1;check(f.admit(moved).status()==Status::MissingDeviceExtension,"Supported but disabled mutable descriptor extension refuses");f.device.enabledExtensionCount=2;
    f.mutableType.mutableDescriptorType=VK_FALSE;check(f.admit(moved).status()==Status::MissingEnabledFeature,"Supported but disabled mutable descriptor feature refuses");f.mutableType.mutableDescriptorType=VK_TRUE;
    f.core.features.shaderStorageImageWriteWithoutFormat=VK_FALSE;check(!f.admit(moved).admitted(),"Missing core feature refuses");f.core.features.shaderStorageImageWriteWithoutFormat=VK_TRUE;
    f.v12.shaderInt8=VK_FALSE;check(!f.admit(moved).admitted(),"Optional supported feature requested by this SDK still must be enabled");f.v12.shaderInt8=VK_TRUE;
    f.v12.scalarBlockLayout=VK_FALSE;check(!f.admit(moved).admitted(),"Scalar layout requested by runtime SDK is checked");f.v12.scalarBlockLayout=VK_TRUE;
    f.v13.shaderIntegerDotProduct=VK_FALSE;check(!f.admit(moved).admitted(),"Integer dot product requested by runtime SDK is checked");f.v13.shaderIntegerDotProduct=VK_TRUE;
    f.core.features.robustBufferAccess=2;check(f.admit(moved).status()==Status::InvalidBoolean,"Malformed effective boolean refuses");f.core.features.robustBufferAccess=VK_FALSE;
    f.device.pEnabledFeatures=&f.core.features;check(f.admit(moved).status()==Status::InvalidFeatureChain,"Legacy core plus Features2 duplicate refuses");f.device.pEnabledFeatures=nullptr;
    f.v13.pNext=&f.core;check(f.admit(moved).status()==Status::InvalidFeatureChain,"Effective cyclic chain refuses");f.v13.pNext=nullptr;
    f.v13.pNext=&f.int8;check(f.admit(moved).status()==Status::InvalidFeatureChain,"Effective aggregate and promoted features conflict");f.v13.pNext=nullptr;
    f.mutableType.pNext=&f.int8;f.int8.shaderInt8=VK_TRUE;f.int8.pNext=&f.scalar;f.scalar.scalarBlockLayout=VK_TRUE;
    f.scalar.pNext=&f.dot;f.dot.shaderIntegerDotProduct=VK_TRUE;
    check(f.admit(moved).admitted(),"Runtime aggregate requirements compare with equivalent split enabled features");
    auto split=f.query(Mode::Split);
    check(f.admit(split).admitted(),"Split SDK requirements compare with split enabled features");
    f.mutableType.pNext=&f.v12;check(f.admit(split).admitted(),"Split SDK requirements compare with aggregate enabled features");
    auto extra=f.query(Mode::ExtraRequired);
    check(f.admit(extra).status()==Status::MissingEnabledFeature,"All required boolean fields are checked beyond the diagnostic subset");
    f.v12.timelineSemaphore=VK_TRUE;check(f.admit(extra).admitted(),"Additional SDK required boolean can be enabled");f.v12.timelineSemaphore=VK_FALSE;
    for(const auto mode:{Mode::InstanceFailure,Mode::DeviceFailure,Mode::FeatureFailure})check(f.query(mode).status()==Status::QueryFailed,"Failed runtime requirement queries refuse");
    check(f.query(Mode::Unknown).status()==Status::UnsupportedFeatureStructure,"Unknown SDK feature node refuses");
    for(const auto mode:{Mode::Cycle,Mode::Duplicate,Mode::PromotedConflict})check(f.query(mode).status()==Status::InvalidFeatureChain,"Cyclic, duplicate and aggregate conflicting SDK chains refuse");
    check(f.query(Mode::InvalidBoolean).status()==Status::InvalidBoolean,"Malformed SDK boolean refuses");
    check(!game::queryRuntimeRequirements(otherQueries,f.identity.instance,f.identity.physicalDevice).queried(),"Missing SDK export refuses");
    check(!game::queryRuntimeRequirements(queries,VK_NULL_HANDLE,f.identity.physicalDevice).queried(),"Missing instance refuses");

    Fixture creation;sdk=FakeSdk{};
    auto instanceRequirements=game::queryInstanceRequirements(queries);
    check(instanceRequirements.instanceQueried()&&!instanceRequirements.queried()&&sdk.instanceCalls==1
        &&sdk.deviceCalls==0&&sdk.featureCalls==0,"Instance requirements are queried before an instance/physical device exists");
    creation.application.apiVersion=VK_API_VERSION_1_0;creation.instance.enabledExtensionCount=1;
    VkBaseInStructure instanceLoader{VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO};creation.instance.pNext=&instanceLoader;
    check(!game::planInstanceRequirements(instanceRequirements,creation.instance).ready(),"Instance mutation is default OFF");
    auto instancePlan=game::planInstanceRequirements(instanceRequirements,creation.instance,true);
    check(instancePlan.ready()&&instancePlan.createInfo()->pNext==&instanceLoader
        &&instancePlan.createInfo()->pApplicationInfo->apiVersion==VK_API_VERSION_1_1
        &&instancePlan.createInfo()->enabledExtensionCount==2&&creation.application.apiVersion==VK_API_VERSION_1_0
        &&creation.instance.enabledExtensionCount==1,"Instance plan owns the API/extension additions and preserves the original chain");
    const auto* stableInstance=instancePlan.createInfo();auto movedInstancePlan=std::move(instancePlan);
    check(!instancePlan.ready()&&!instancePlan.createInfo()&&movedInstancePlan.createInfo()==stableInstance,"Moving instance plan preserves changed pointer targets");
    auto complete=game::queryDeviceRequirements(std::move(instanceRequirements),creation.identity.instance,creation.identity.physicalDevice);
    check(complete.queried()&&sdk.instanceCalls==1&&sdk.deviceCalls==1&&sdk.featureCalls==1,"Device requirements use the selected instance and physical device after instance creation");
    creation.device.enabledExtensionCount=1;creation.mutableType.mutableDescriptorType=VK_FALSE;
    creation.v12.shaderInt8=VK_FALSE;creation.v12.scalarBlockLayout=VK_FALSE;creation.v13.shaderIntegerDotProduct=VK_FALSE;
    creation.core.features.robustBufferAccess=VK_TRUE;creation.unrelated.privateDataSlotRequestCount=13;
    check(!game::planDeviceRequirements(complete,creation.device).ready(),"Device mutation is default OFF");
    auto devicePlan=devicePlanFor(complete,creation.device);
    check(devicePlan.ready()&&devicePlan.createInfo()!=&creation.device&&devicePlan.createInfo()->pNext!=creation.device.pNext
        &&devicePlan.createInfo()->enabledExtensionCount==2&&!devicePlan.createInfo()->pEnabledFeatures,
        "Device plan owns merged extensions/features and emits Features2 form");
    check(game::admitRuntimeRequirements(complete,creation.identity,*movedInstancePlan.createInfo(),*devicePlan.createInfo(),VK_SUCCESS,VK_SUCCESS).admitted(),
        "The effective create plans satisfy every queried runtime SDK requirement");
    auto* preserved=static_cast<const VkDevicePrivateDataCreateInfo*>(devicePlan.createInfo()->pNext);
    const auto* preservedCore=static_cast<const VkPhysicalDeviceFeatures2*>(preserved->pNext);
    check(preserved->privateDataSlotRequestCount==13&&preservedCore->features.robustBufferAccess==VK_TRUE
        &&creation.mutableType.mutableDescriptorType==VK_FALSE&&creation.v12.shaderInt8==VK_FALSE
        &&creation.core.pNext==&creation.mutableType&&creation.device.enabledExtensionCount==1,
        "Merge preserves application data and booleans without modifying original nodes or names");
    const auto* stableDevice=devicePlan.createInfo();auto movedDevicePlan=std::move(devicePlan);
    sdk=FakeSdk{};
    check(!devicePlan.ready()&&!devicePlan.createInfo()&&movedDevicePlan.createInfo()==stableDevice
        &&game::admitRuntimeRequirements(complete,creation.identity,*movedInstancePlan.createInfo(),*movedDevicePlan.createInfo(),VK_SUCCESS,VK_SUCCESS).admitted(),
        "Owned merged nodes survive moving the plan and later overwriting SDK memory");
    creation.device.pNext=&creation.v12;creation.device.pEnabledFeatures=&creation.core.features;
    auto legacyPlan=devicePlanFor(complete,creation.device);
    check(legacyPlan.ready()&&!legacyPlan.createInfo()->pEnabledFeatures
        &&game::admitRuntimeRequirements(complete,creation.identity,*movedInstancePlan.createInfo(),*legacyPlan.createInfo(),VK_SUCCESS,VK_SUCCESS).admitted()
        &&creation.device.pEnabledFeatures==&creation.core.features,"Legacy core features are copied into Features2, preserving the legacy source");
    creation.device.pEnabledFeatures=nullptr;creation.device.pNext=&creation.core;
    creation.mutableType.pNext=&creation.int8;creation.int8.shaderFloat16=VK_TRUE;creation.int8.pNext=&creation.scalar;
    creation.scalar.pNext=&creation.dot;
    auto splitPlan=devicePlanFor(complete,creation.device);
    check(splitPlan.ready()&&game::admitRuntimeRequirements(complete,creation.identity,*movedInstancePlan.createInfo(),*splitPlan.createInfo(),VK_SUCCESS,VK_SUCCESS).admitted(),
        "Aggregate SDK fields merge into existing promoted structures without duplicate aggregate nodes");
    auto splitRequirements=creation.query(Mode::Split);creation.mutableType.pNext=&creation.v12;
    auto aggregatePlan=devicePlanFor(splitRequirements,creation.device);
    check(aggregatePlan.ready()&&game::admitRuntimeRequirements(splitRequirements,creation.identity,*movedInstancePlan.createInfo(),*aggregatePlan.createInfo(),VK_SUCCESS,VK_SUCCESS).admitted(),
        "Promoted SDK structures merge into existing aggregate structures without duplicates");
    VkLayerDeviceLink tail{},link{};link.pNext=&tail;
    VkLayerDeviceCreateInfo loader{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO};loader.function=VK_LAYER_LINK_INFO;
    loader.u.pLayerInfo=&link;loader.pNext=&creation.core;creation.device.pNext=&loader;
    check(devicePlanFor(complete,creation.device).status()==Status::BorrowedLoaderChainNotAllowed,"Loader payload borrowing requires the existing explicit synchronous-call option");
    auto loaderPlan=devicePlanFor(complete,creation.device,xess_vk::AllowBorrowedLoaderChain::SynchronousCreateDeviceCallOnly);
    check(loaderPlan.ready()&&loaderPlan.requiresSynchronousLoaderCall(),"Known loader create payload is cloned for one synchronous downstream call");
    auto* copiedLoader=const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(loaderPlan.createInfo()->pNext));
    copiedLoader->u.pLayerInfo=copiedLoader->u.pLayerInfo->pNext;
    check(loader.u.pLayerInfo==&link&&copiedLoader->u.pLayerInfo==&tail,"Advancing copied loader LINK_INFO preserves the original link");
    VkDeviceDeviceMemoryReportCreateInfoEXT pointerRich{VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT};
    pointerRich.pNext=&creation.core;creation.device.pNext=&pointerRich;
    check(devicePlanFor(complete,creation.device).status()==Status::UnsupportedApplicationChain
        &&creation.device.pNext==&pointerRich&&pointerRich.pNext==&creation.core,"Unknown pointer-rich application payload refuses the entire plan unchanged");
    creation.device.pNext=&creation.core;creation.v13.pNext=&creation.int8;
    check(devicePlanFor(complete,creation.device).status()==Status::InvalidFeatureChain,"Preexisting application promoted/aggregate conflict refuses");creation.v13.pNext=nullptr;
    auto extraRequirements=creation.query(Mode::ExtraRequired);
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};timeline.timelineSemaphore=VK_TRUE;
    creation.mutableType.pNext=&timeline;timeline.pNext=&creation.dot;
    check(devicePlanFor(extraRequirements,creation.device).status()==Status::UnsupportedFeatureStructure,
        "An unhandled aggregate field refuses a split-feature merge instead of guessing or adding conflicting nodes");
    creation.mutableType.pNext=&creation.v12;
    check(game::planDeviceRequirements(complete,creation.device,true).status()==Status::CapabilityQueryMissing,
        "Opt-in device plan refuses when the real feature support query is absent");
    auto missingExtensionQuery=supportQueries;missingExtensionQuery.extensions=nullptr;
    check(game::planDeviceRequirements(complete,creation.device,true,xess_vk::AllowBorrowedLoaderChain::No,missingExtensionQuery).status()==Status::CapabilityQueryMissing,
        "New SDK extensions require an actual extension support query");
    supportMode=SupportMode::MissingExtension;
    check(devicePlanFor(complete,creation.device).status()==Status::RequiredExtensionUnsupported,"Unsupported newly requested extension refuses before downstream creation");
    supportMode=SupportMode::MissingFeature;
    check(devicePlanFor(complete,creation.device).status()==Status::RequiredFeatureUnsupported,"Unsupported required feature refuses before downstream creation");
    supportMode=SupportMode::EnumerationFailure;
    check(devicePlanFor(complete,creation.device).status()==Status::CapabilityQueryFailed,"Failed support enumeration refuses before downstream creation");
    supportMode=SupportMode::Incomplete;supportExtensionCalls=0;
    check(devicePlanFor(complete,creation.device).status()==Status::CapabilityQueryFailed&&supportExtensionCalls==6,
        "Repeated incomplete extension enumeration stops after three bounded attempts");
    supportMode=SupportMode::Normal;supportExtensionCalls=0;supportFeatureCalls=0;
    check(devicePlanFor(complete,creation.device).ready()&&supportExtensionCalls==2&&supportFeatureCalls==1
        &&supportPhysical==creation.identity.physicalDevice,"Only the selected physical device's support is queried with the required feature shape");
    creation.device.enabledExtensionCount=2;supportExtensionCalls=0;supportFeatureCalls=0;
    check(game::planDeviceRequirements(complete,creation.device,true,xess_vk::AllowBorrowedLoaderChain::No,missingExtensionQuery).ready()
        &&supportExtensionCalls==0&&supportFeatureCalls==1,"Already-enabled SDK extensions do not require added-extension enumeration");
    supportFeatureCalls=0;
    check(!game::planDeviceRequirements(complete,creation.device).ready()&&supportFeatureCalls==0,"Default OFF performs no support query");
    std::printf("{\"event\":\"sr_game_requirements_cpu\",\"passed\":true,\"checks\":%u,\"sdkFunctions\":\"stubs\",\"vulkanCalls\":0,\"gameChanged\":false}\n",checks);
}
