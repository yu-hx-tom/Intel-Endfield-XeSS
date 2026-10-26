#define XESS_SR_CREATION_TEST 1
#include "vulkan_observer.cpp"
#include <stdexcept>
#include <iostream>

namespace {
unsigned checks{},loads{},instanceQueries{},deviceQueries{},featureQueries{},instanceCalls{},deviceCalls{};
int instanceKey,deviceKey;
struct Dispatch {void* key;};
Dispatch instanceDispatch{&instanceKey},deviceDispatch{&deviceKey};
const auto fakeInstance=reinterpret_cast<VkInstance>(&instanceDispatch);
const auto fakePhysical=reinterpret_cast<VkPhysicalDevice>(&instanceDispatch);
const auto fakeDevice=reinterpret_cast<VkDevice>(&deviceDispatch);
bool request{},queryFailure{},unsupported{},unknownChain{},expectBridge{},privateOrigin{},differentAlias{},missingAlias{};
Dispatch publicPhysicalDispatch{&deviceKey};
VkPhysicalDevice sdkPhysical=fakePhysical;
const VkInstanceCreateInfo* originalInstance{};
const VkDeviceCreateInfo* originalDevice{};
const VkAllocationCallbacks* expectedAllocator{};
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
xess_result_t instanceRequirements(std::uint32_t* count,const char* const** names,std::uint32_t* api) {
    ++instanceQueries;if(queryFailure)return XESS_RESULT_ERROR_UNSUPPORTED_DRIVER;
    static const char* extension="VK_KHR_get_physical_device_properties2";
    *count=1;*names=&extension;*api=VK_API_VERSION_1_1;return XESS_RESULT_SUCCESS;
}
xess_result_t deviceRequirements(VkInstance instance,VkPhysicalDevice physical,std::uint32_t* count,const char* const** names) {
    ++deviceQueries;require(instance==fakeInstance&&physical==sdkPhysical,"SDK requirements query belongs to actual selected physical device");
    static const char* extension="VK_EXT_mutable_descriptor_type";*count=1;*names=&extension;return XESS_RESULT_SUCCESS;
}
xess_result_t featureRequirements(VkInstance,VkPhysicalDevice,void** head) {
    ++featureQueries;auto* core=static_cast<VkPhysicalDeviceFeatures2*>(*head);
    static VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableType{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    mutableType.mutableDescriptorType=VK_TRUE;core->pNext=&mutableType;core->features.shaderStorageImageWriteWithoutFormat=VK_TRUE;
    return XESS_RESULT_SUCCESS;
}
SrInstanceCreation* factory(){++loads;auto* value=new SrInstanceCreation;value->queries={instanceRequirements,deviceRequirements,featureRequirements};return value;}
VKAPI_ATTR VkResult VKAPI_CALL enumerateVersion(std::uint32_t* api){*api=VK_API_VERSION_1_3;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL instanceExtensions(const char*,std::uint32_t* count,VkExtensionProperties* props) {
    if(!props){*count=1;return VK_SUCCESS;}*count=1;std::strcpy(props->extensionName,"VK_KHR_get_physical_device_properties2");return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL deviceExtensions(VkPhysicalDevice,const char*,std::uint32_t* count,VkExtensionProperties* props) {
    const char* names[]{"VK_KHR_swapchain","VK_EXT_mutable_descriptor_type","VK_KHR_external_memory_win32","VK_KHR_external_semaphore_win32"};
    if(!props){*count=4;return VK_SUCCESS;}*count=4;
    for(unsigned i=0;i<4;++i)std::strcpy(props[i].extensionName,names[i]);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL features(VkPhysicalDevice,VkPhysicalDeviceFeatures2* core) {
    core->features.shaderStorageImageWriteWithoutFormat=VK_TRUE;
    for(auto* p=static_cast<VkBaseOutStructure*>(core->pNext);p;p=p->pNext) {
        if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT)
            reinterpret_cast<VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT*>(p)->mutableDescriptorType=unsupported?VK_FALSE:VK_TRUE;
        if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
            reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(p)->timelineSemaphore=VK_TRUE;
    }
}
VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice,VkPhysicalDeviceProperties* p){p->apiVersion=VK_API_VERSION_1_3;}
VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance,const VkAllocationCallbacks*){}
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice,const VkAllocationCallbacks*){}
VKAPI_ATTR VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo* info,const VkAllocationCallbacks* allocator,VkInstance* out) {
    ++instanceCalls;require(allocator==expectedAllocator,"instance allocator forwarded unchanged");
    require(instanceLink(info)->u.pLayerInfo==nullptr,"exact final instance loader link advanced once");
    if(!request||queryFailure||privateOrigin)require(info==originalInstance,"SR OFF/query failure preserves exact original instance info pointer");
    else require(info!=originalInstance&&info->pApplicationInfo->apiVersion==VK_API_VERSION_1_3&&info->enabledExtensionCount==1,
        "SR ON forwards owned instance plan with SDK extension");
    *out=fakeInstance;SetLastError(0x77665544);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,const VkAllocationCallbacks* allocator,VkDevice* out) {
    ++deviceCalls;require(physical==fakePhysical,"native device creation retains the layer physical handle");require(allocator==expectedAllocator,"device allocator forwarded unchanged");
    require(info->queueCreateInfoCount==1&&info->pQueueCreateInfos[0].queueFamilyIndex==originalDevice->pQueueCreateInfos[0].queueFamilyIndex&&
        info->pQueueCreateInfos[0].queueCount==1&&info->pQueueCreateInfos[0].pQueuePriorities[0]==originalDevice->pQueueCreateInfos[0].pQueuePriorities[0],
        "application queue family/count/priority values preserved through T3/SR plans");
    if(!expectBridge)require(info->pQueueCreateInfos==originalDevice->pQueueCreateInfos,"SR plan itself borrows the exact baseline queue payload");
    require(deviceLink(info)->u.pLayerInfo==nullptr,"only final device loader link advanced");
    const auto requested=xess_vk::sr_caps::requested(*info);
    if(request&&!queryFailure&&!unsupported&&!unknownChain&&!privateOrigin&&!missingAlias)require(requested.mutableDescriptorType&&requested.storageWriteWithoutFormat,"SR device plan enables required feature booleans");
    else require(!requested.mutableDescriptorType&&!requested.storageWriteWithoutFormat,"SR refusal/OFF does not add SR features");
    if(expectBridge)require(featureFlags(info).timeline,"SR plan/refusal preserves T3 timeline feature");
    else if(!request||queryFailure||unsupported||unknownChain||privateOrigin||missingAlias)require(info==originalDevice,"SR OFF/refusal uses exact baseline device pointer");
    *out=fakeDevice;SetLastError(0x11223344);return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice,const char* name) {
    return !std::strcmp(name,"vkDestroyDevice")?reinterpret_cast<PFN_vkVoidFunction>(destroyDevice):nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceProc(VkInstance,const char* name) {
#define PROC(api,fn) if(!std::strcmp(name,#api))return reinterpret_cast<PFN_vkVoidFunction>(fn)
    PROC(vkCreateInstance,createInstance);PROC(vkDestroyInstance,destroyInstance);PROC(vkCreateDevice,createDevice);
    PROC(vkEnumerateInstanceVersion,enumerateVersion);PROC(vkEnumerateInstanceExtensionProperties,instanceExtensions);
    PROC(vkEnumerateDeviceExtensionProperties,deviceExtensions);PROC(vkGetPhysicalDeviceFeatures2,features);
    PROC(vkGetPhysicalDeviceProperties,properties);
#undef PROC
    return nullptr;
}
void run(bool on,bool fail,bool missing,bool unusual,bool bridge,bool probe=false,bool alias=false,bool unmapped=false) {
    request=on;queryFailure=fail;unsupported=missing;unknownChain=unusual;privateOrigin=probe;differentAlias=alias;missingAlias=unmapped;expectBridge=bridge&&!probe;
    sdkPhysical=alias?reinterpret_cast<VkPhysicalDevice>(&publicPhysicalDispatch):fakePhysical;
    loads=instanceQueries=deviceQueries=featureQueries=instanceCalls=deviceCalls=0;
    SetEnvironmentVariableW(L"XESS_SR_REPLACE",on?L"1":nullptr);
    SetEnvironmentVariableW(L"XESS_BRIDGE_DEVICE",bridge?L"1":nullptr);
    srCreationForTest=factory;srPublicPhysicalForTest=[](InstanceState*,VkPhysicalDevice physical){require(physical==fakePhysical,"loader alias resolver receives actual layer handle");return missingAlias?VK_NULL_HANDLE:sdkPhysical;};
    VkAllocationCallbacks allocator{};expectedAllocator=&allocator;
    VkLayerInstanceLink link{};link.pfnNextGetInstanceProcAddr=instanceProc;
    VkLayerInstanceCreateInfo chain{};chain.sType=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;chain.function=VK_LAYER_LINK_INFO;chain.u.pLayerInfo=&link;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,&chain};instance.pApplicationInfo=&app;originalInstance=&instance;
    VkInstance outputInstance{};
    const auto create=[&]{if(privateOrigin){SrSdkScope sdk;return vkCreateInstance(&instance,&allocator,&outputInstance);}return vkCreateInstance(&instance,&allocator,&outputInstance);};
    require(create()==VK_SUCCESS&&instanceCalls==1&&GetLastError()==0x77665544,"actual instance wrapper calls downstream once and preserves LastError");
    require((findInstance(outputInstance)->srCreation!=nullptr)==(on&&!fail&&!privateOrigin),"only successfully planned SR instance retains SDK owner");
    require(findInstance(outputInstance)->srSdkPrivate==privateOrigin&&!srSdkDepth,"probe ownership survives the SDK scope without leaking thread-local depth");
    app.apiVersion=VK_API_VERSION_1_0; // Later device admission must use copied successful facts.
    VkLayerDeviceLink deviceLinkInfo{};deviceLinkInfo.pfnNextGetInstanceProcAddr=instanceProc;deviceLinkInfo.pfnNextGetDeviceProcAddr=deviceProc;
    VkLayerDeviceCreateInfo deviceChain{};deviceChain.sType=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;deviceChain.function=VK_LAYER_LINK_INFO;deviceChain.u.pLayerInfo=&deviceLinkInfo;
    VkBaseInStructure unrelated{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    if(unusual)deviceChain.pNext=&unrelated;
    float priority=1.f;VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue.queueCount=1;queue.pQueuePriorities=&priority;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&deviceChain};device.queueCreateInfoCount=1;device.pQueueCreateInfos=&queue;originalDevice=&device;
    VkDevice outputDevice{};
    require(vkCreateDevice(fakePhysical,&device,&allocator,&outputDevice)==VK_SUCCESS&&deviceCalls==1&&GetLastError()==0x11223344,"actual device wrapper calls downstream once and preserves LastError");
    require(loads==(on&&!privateOrigin?1u:0u)&&instanceQueries==loads&&deviceQueries==(on&&!fail&&!privateOrigin&&!missingAlias?1u:0u)&&featureQueries==deviceQueries,"OFF performs no SR loader or SDK query calls");
    if(privateOrigin){
        const auto* state=findDevice(outputDevice);require(state->srSdkPrivate&&!state->presentationRequested&&!state->srAdmission&&!state->srRuntime&&!state->descriptors,"SDK probe devices do not acquire game presentation, SR contexts or input tracking");
        require(vkGetDeviceProcAddr(outputDevice,"vkDestroyDevice")==reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice),"private device retains only required lifetime bookkeeping");
        require(vkGetInstanceProcAddr(outputInstance,"vkGetPhysicalDeviceProperties")==reinterpret_cast<PFN_vkVoidFunction>(properties),"private instance forwards physical queries directly");
    }
    const auto retiredBefore=inventorySessionRetired;if(privateOrigin)inventorySessionRetired=false;
    vkDestroyDevice(outputDevice,&allocator);vkDestroyInstance(outputInstance,&allocator);
    if(privateOrigin){require(!inventorySessionRetired,"SDK probe destruction cannot retire the game's input session");inventorySessionRetired=retiredBefore;}
    require(!devices&&!instances,"normal destruction releases creation metadata");
}
}
int main(){try {
    for(const auto* name:{L"XESS_SR_REPLACE",L"XESS_BRIDGE_DEVICE",L"XESS_FSR_UPSCALE_OBSERVER",L"XESS_FSR_OBSERVER",L"XESS_COMPUTE_OBSERVER",L"XESS_PRESENTATION_OFF",L"XESS_CONTINUOUS_INPUTS",L"XESS_SL_OBSERVER"})SetEnvironmentVariableW(name,nullptr);
    run(false,false,false,false,false);run(false,false,false,false,true);
    run(true,true,false,false,false);run(true,false,true,false,false);run(true,false,false,true,false);
    run(true,false,false,false,false);run(true,false,false,false,true);run(true,false,true,false,true);
    run(true,false,false,false,true,true);run(true,false,false,false,false,false,true);run(true,false,false,false,false,false,false,true);
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"actualCreationWrappers\":true,\"sdkLoaded\":false,\"gpuExecuted\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
