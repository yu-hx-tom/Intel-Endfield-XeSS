#include "xess_sr.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>

namespace {
namespace sr=xess_vk::sr;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void vkCheck(VkResult value,const char* message) {
    if(value!=VK_SUCCESS)throw std::runtime_error(std::string(message)+": VkResult "+std::to_string(value));
}
std::string quote(const std::string& value) {
    std::string out="\"";
    for(const unsigned char c:value) {
        if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
        else if(c<32){const char* h="0123456789abcdef";out+="\\u00";out+=h[c>>4];out+=h[c&15];}
        else out+=char(c);
    }
    return out+'"';
}
std::string vkVersion(std::uint32_t value) {
    return std::to_string(VK_API_VERSION_MAJOR(value))+'.'+std::to_string(VK_API_VERSION_MINOR(value))+'.'+std::to_string(VK_API_VERSION_PATCH(value));
}
std::vector<std::string> names(std::uint32_t count,const char* const* list) {
    require(!count||list,"SDK returned an absent extension list");
    std::vector<std::string> result;
    for(unsigned i=0;i<count;++i){require(list[i]!=nullptr,"SDK returned a null extension name");result.emplace_back(list[i]);}
    return sr::mergeExtensions({},result);
}
std::vector<const char*> pointers(const std::vector<std::string>& list) {
    std::vector<const char*> result;for(const auto& name:list)result.push_back(name.c_str());return result;
}
void printNames(const std::vector<std::string>& list) {
    std::cout<<'[';for(unsigned i=0;i<list.size();++i){if(i)std::cout<<',';std::cout<<quote(list[i]);}std::cout<<']';
}

// shortcut: explicitly known SDK feature structs; reject new structs before
// device creation until this standalone requirements checker is extended.
struct Features {
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableType{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    VkPhysicalDeviceShaderFloat16Int8Features int8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceShaderIntegerDotProductFeatures dot{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES};
    std::set<VkStructureType> types;
    Features()=default;
    Features(const Features&)=delete;
    Features& operator=(const Features&)=delete;
    VkBaseOutStructure* node(VkStructureType type) {
        switch(type) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:return reinterpret_cast<VkBaseOutStructure*>(&core);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:return reinterpret_cast<VkBaseOutStructure*>(&v12);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:return reinterpret_cast<VkBaseOutStructure*>(&v13);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT:return reinterpret_cast<VkBaseOutStructure*>(&mutableType);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:return reinterpret_cast<VkBaseOutStructure*>(&int8);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:return reinterpret_cast<VkBaseOutStructure*>(&dot);
        default:throw std::runtime_error("Unknown XeSS feature structure "+std::to_string(type));
        }
    }
    static std::pair<std::size_t,std::size_t> boolFields(VkStructureType type) {
#define SR_FIELDS(typeName,first,last) return {offsetof(typeName,first),offsetof(typeName,last)+sizeof(VkBool32)}
        switch(type) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:return {offsetof(VkPhysicalDeviceFeatures2,features),offsetof(VkPhysicalDeviceFeatures2,features)+sizeof(VkPhysicalDeviceFeatures)};
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:SR_FIELDS(VkPhysicalDeviceVulkan12Features,samplerMirrorClampToEdge,subgroupBroadcastDynamicId);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:SR_FIELDS(VkPhysicalDeviceVulkan13Features,robustImageAccess,maintenance4);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT:SR_FIELDS(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT,mutableDescriptorType,mutableDescriptorType);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:SR_FIELDS(VkPhysicalDeviceShaderFloat16Int8Features,shaderFloat16,shaderInt8);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:SR_FIELDS(VkPhysicalDeviceShaderIntegerDotProductFeatures,shaderIntegerDotProduct,shaderIntegerDotProduct);
        default:throw std::runtime_error("Unknown feature field range");
        }
#undef SR_FIELDS
    }
    void mirror(void* requested) {
        types.clear();auto* tail=reinterpret_cast<VkBaseOutStructure*>(&core);core.pNext=nullptr;
        for(auto* p=static_cast<VkBaseOutStructure*>(requested);p;p=p->pNext) {
            require(types.size()<16&&types.insert(p->sType).second,"Repeated/cyclic SDK feature chain");
            if(p->sType!=core.sType){auto* next=node(p->sType);next->pNext=nullptr;tail->pNext=next;tail=next;}
        }
        require(types.contains(core.sType),"SDK feature chain lost the caller-owned Features2 node");
        require(!(types.contains(v12.sType)&&types.contains(int8.sType))&&!(types.contains(v13.sType)&&types.contains(dot.sType)),
                "SDK feature chain duplicates promoted Vulkan feature structures");
    }
    void capture(void* requested) {
        mirror(requested);
        // SDK-owned nodes need not survive later context/pipeline API calls.
        for(auto* p=static_cast<VkBaseOutStructure*>(requested);p;p=p->pNext) {
            const auto [begin,end]=boolFields(p->sType);
            std::memcpy(reinterpret_cast<std::uint8_t*>(node(p->sType))+begin,
                reinterpret_cast<const std::uint8_t*>(p)+begin,end-begin);
        }
    }
    void validate(void* requested) {
        for(auto* p=static_cast<VkBaseOutStructure*>(requested);p;p=p->pNext) {
            const auto [begin,end]=boolFields(p->sType);const auto* supported=reinterpret_cast<const std::uint8_t*>(node(p->sType));
            for(auto at=begin;at<end;at+=sizeof(VkBool32)) {
                VkBool32 wanted{},has{};std::memcpy(&wanted,reinterpret_cast<const std::uint8_t*>(p)+at,sizeof(wanted));
                std::memcpy(&has,supported+at,sizeof(has));
                require(wanted==VK_FALSE||wanted==VK_TRUE,"Malformed required Vulkan boolean");
                if(wanted&&!has)throw std::runtime_error("Unsupported required feature: sType="+std::to_string(p->sType)+" field="+std::to_string((at-begin)/4));
            }
        }
    }
};

struct Resolution {
    xess_quality_settings_t quality{};
    xess_2d_t optimal{},minimum{},maximum{};
    bool contains(VkExtent2D input) const {
        return input.width&&input.height&&input.width>=minimum.x&&input.width<=maximum.x&&
            input.height>=minimum.y&&input.height<=maximum.y;
    }
};
struct ImplementationIdentity {
    bool reportedIntelVersionAvailable{};
    const char* implementation="unknown";
};
ImplementationIdentity implementationIdentity(const xess_version_t& version) {
    return {bool(version.major||version.minor||version.patch||version.reserved)};
}
Resolution resolution(sr::Sdk& sdk,xess_context_handle_t context,VkExtent2D output,xess_quality_settings_t quality) {
    Resolution range;range.quality=quality;const xess_2d_t target{output.width,output.height};
    sr::check(sdk.xessGetOptimalInputResolution(context,&target,quality,&range.optimal,&range.minimum,&range.maximum),"Query current quality dimensions");
    require(range.contains({range.optimal.x,range.optimal.y})&&range.minimum.x&&range.minimum.y,
        "SDK returned an inconsistent input-resolution range");
    return range;
}
const Resolution& selectResolution(VkExtent2D input,std::span<const Resolution> candidates) {
    for(const auto& range:candidates)if(range.contains(input))return range;
    throw std::runtime_error("Exact requested input dimensions outside all queried SDK quality ranges");
}
void printResolution(const char* event,const Resolution& range) {
    std::cout<<"{\"event\":"<<quote(event)<<",\"quality\":"<<range.quality<<",\"optimal\":["<<range.optimal.x<<','<<range.optimal.y
        <<"],\"minimum\":["<<range.minimum.x<<','<<range.minimum.y<<"],\"maximum\":["<<range.maximum.x<<','<<range.maximum.y<<"]}\n";
}

VkImageBlit fallbackRegion(VkExtent2D input,VkExtent2D output) {
    require(input.width&&input.height&&output.width&&output.height&&
        input.width<=INT32_MAX&&input.height<=INT32_MAX&&output.width<=INT32_MAX&&output.height<=INT32_MAX,"Invalid full-frame blit extent");
    VkImageBlit region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    region.srcOffsets[1]={static_cast<int32_t>(input.width),static_cast<int32_t>(input.height),1};
    region.dstOffsets[1]={static_cast<int32_t>(output.width),static_cast<int32_t>(output.height),1};return region;
}
bool expectedBlitPixels(std::span<const std::uint16_t> pixels) {
    constexpr std::array<std::uint16_t,4> expected{0x4000,0x3800,0x3400,0x3c00}; // 2, .5, .25, 1 in RGBA16F.
    if(pixels.empty()||pixels.size()%4)return false;
    for(std::size_t n=0;n<pixels.size();++n)if(pixels[n]!=expected[n%4])return false;
    return true;
}
struct Options {std::filesystem::path sdk,validationLayerDir;unsigned warmup{4},samples{12},timeoutMs{5000};bool validation{},gameColorFormat{},blockingBuild{},blitFallback{};};
struct Image {VkImage image{};VkDeviceMemory memory{};VkImageView view{};VkFormat format{};VkExtent2D extent{};VkImageAspectFlags aspects{};};
struct Owner {
    sr::Sdk sdk;
    HMODULE loader{};
    PFN_vkGetInstanceProcAddr get{};PFN_vkGetDeviceProcAddr gdpa{};
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};
    VkPhysicalDeviceProperties properties{};VkPhysicalDeviceMemoryProperties memory{};
    VkDebugUtilsMessengerEXT messenger{};std::atomic<unsigned> validationErrors{},validationWarnings{};std::string validationModule;
    VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};VkQueryPool queries{};
    VkBuffer readback{};VkDeviceMemory hostMemory{};VkDeviceSize readbackBytes{};bool coherent{};
    std::array<Image,4> images{};xess_context_handle_t context{};
    unsigned family{},validBits{};bool pending{},closed{};unsigned frames{},resetFrames{};
    Options options;
    std::array<std::uint64_t,2> lastTicks{};
    explicit Owner(Options o):sdk(o.sdk),options(std::move(o)){}
    template<class T>T inst(const char* name) const {auto fn=get(instance,name);require(fn!=nullptr,name);return reinterpret_cast<T>(fn);}
    template<class T>T dev(const char* name) const {auto fn=gdpa(device,name);require(fn!=nullptr,name);return reinterpret_cast<T>(fn);}
    static VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* message,void* user) {
        if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++static_cast<Owner*>(user)->validationErrors;
        if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)++static_cast<Owner*>(user)->validationWarnings;
        std::fprintf(stderr,"SR validation: %.2048s\n",message&&message->pMessage?message->pMessage:"");return VK_FALSE;
    }
    static void sdkLog(const char* message,xess_logging_level_t level) {
        static std::atomic<unsigned> count{};if(count.fetch_add(1)<96)std::fprintf(stderr,"XeSS-SR[%u]: %.2048s\n",unsigned(level),message?message:"");
    }
    void initialize() {
        if(!options.validationLayerDir.empty()) {
            require(options.validationLayerDir.is_absolute()&&std::filesystem::is_regular_file(options.validationLayerDir/L"VkLayer_khronos_validation.json")&&
                std::filesystem::is_regular_file(options.validationLayerDir/L"VkLayer_khronos_validation.dll"),"Validation layer directory lacks the explicit manifest/DLL");
            // Only this owned fixture process changes; the parent and registry stay intact.
            require(SetEnvironmentVariableW(L"VK_LAYER_PATH",options.validationLayerDir.c_str()),"Set fixture-only validation layer path");
        }
        loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);require(loader!=nullptr,"Load Vulkan system loader");
        get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));require(get!=nullptr,"Vulkan loader export");
        unsigned count{},minimum{};const char* const* required{};
        sr::check(sdk.xessVKGetRequiredInstanceExtensions(&count,&required,&minimum),"XeSS required instance extensions");
        auto instanceExtensions=names(count,required);
        if(options.validation)instanceExtensions=sr::mergeExtensions(instanceExtensions,std::array<std::string,1>{VK_EXT_DEBUG_UTILS_EXTENSION_NAME});
        unsigned loaderApi=VK_API_VERSION_1_0;
        if(auto enumerate=reinterpret_cast<PFN_vkEnumerateInstanceVersion>(get(nullptr,"vkEnumerateInstanceVersion")))vkCheck(enumerate(&loaderApi),"Vulkan version");
        const auto api=std::max(minimum,VK_API_VERSION_1_3);require(loaderApi>=api,"Loader cannot meet the runtime SDK API version");
        std::vector<VkExtensionProperties> instanceAvailable;
        vkCheck(inst<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties")(nullptr,&count,nullptr),"Instance extension count");
        instanceAvailable.resize(count);vkCheck(inst<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties")(nullptr,&count,instanceAvailable.data()),"Instance extensions");
        for(const auto& name:instanceExtensions)require(std::any_of(instanceAvailable.begin(),instanceAvailable.end(),[&](const auto& e){return name==e.extensionName;}),"Required instance extension unsupported");
        const char* layer="VK_LAYER_KHRONOS_validation";
        if(options.validation) {
            vkCheck(inst<PFN_vkEnumerateInstanceLayerProperties>("vkEnumerateInstanceLayerProperties")(&count,nullptr),"Validation layer count");
            std::vector<VkLayerProperties> layers(count);vkCheck(inst<PFN_vkEnumerateInstanceLayerProperties>("vkEnumerateInstanceLayerProperties")(&count,layers.data()),"Validation layers");
            require(std::any_of(layers.begin(),layers.end(),[&](const auto& l){return !std::strcmp(layer,l.layerName);}),"Requested validation layer unavailable");
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Owned XeSS SR phase0";app.apiVersion=api;
        const auto instancePointers=pointers(instanceExtensions);
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};debug.pUserData=this;debug.pfnUserCallback=diagnostic;
        debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};create.pApplicationInfo=&app;
        create.enabledExtensionCount=instancePointers.size();create.ppEnabledExtensionNames=instancePointers.data();
        if(options.validation){create.pNext=&debug;create.enabledLayerCount=1;create.ppEnabledLayerNames=&layer;}
        vkCheck(inst<PFN_vkCreateInstance>("vkCreateInstance")(&create,nullptr,&instance),"Create owned SR instance");
        if(options.validation) {
            vkCheck(inst<PFN_vkCreateDebugUtilsMessengerEXT>("vkCreateDebugUtilsMessengerEXT")(instance,&debug,nullptr,&messenger),"Create validation messenger");
            wchar_t path[32768]{};const auto module=GetModuleHandleW(L"VkLayer_khronos_validation.dll");
            const auto length=module?GetModuleFileNameW(module,path,32768):0;require(length&&length<32768,"Requested validation DLL was not loaded");
            const auto loaded=std::filesystem::weakly_canonical(path);
            if(!options.validationLayerDir.empty()){const auto expected=std::filesystem::weakly_canonical(options.validationLayerDir/L"VkLayer_khronos_validation.dll");
                require(CompareStringOrdinal(loaded.c_str(),-1,expected.c_str(),-1,TRUE)==CSTR_EQUAL,"Vulkan loaded a different validation DLL");}
            const auto utf8=loaded.u8string();validationModule.assign(reinterpret_cast<const char*>(utf8.data()),utf8.size());
        }
        vkCheck(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance,&count,nullptr),"Physical device count");
        std::vector<VkPhysicalDevice> devices(count);vkCheck(inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance,&count,devices.data()),"Physical devices");
        for(auto candidate:devices){VkPhysicalDeviceProperties p{};inst<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate,&p);if(p.vendorID==0x8086&&p.apiVersion>=api){physical=candidate;properties=p;break;}}
        require(physical!=VK_NULL_HANDLE,"No Intel physical device meets the runtime SDK version");
        inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&count,nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&count,queues.data());
        auto chosen=std::find_if(queues.begin(),queues.end(),[](const auto& q){return q.queueCount&&(q.queueFlags&VK_QUEUE_COMPUTE_BIT)&&q.timestampValidBits;});
        require(chosen!=queues.end(),"No compute timestamp queue");family=chosen-queues.begin();validBits=chosen->timestampValidBits;
        sr::check(sdk.xessVKGetRequiredDeviceExtensions(instance,physical,&count,&required),"XeSS required device extensions");
        const auto deviceExtensions=names(count,required);const auto devicePointers=pointers(deviceExtensions);
        vkCheck(inst<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties")(physical,nullptr,&count,nullptr),"Device extension count");
        std::vector<VkExtensionProperties> available(count);vkCheck(inst<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties")(physical,nullptr,&count,available.data()),"Device extensions");
        for(const auto& name:deviceExtensions)require(std::any_of(available.begin(),available.end(),[&](const auto& e){return name==e.extensionName;}),"Required device extension unsupported");
        VkPhysicalDeviceFeatures2 seed{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};void* sdkHead=&seed;
        sr::check(sdk.xessVKGetRequiredDeviceFeatures(instance,physical,&sdkHead),"XeSS required device features");
        Features requested;requested.capture(sdkHead);void* requestedHead=&requested.core;
        Features support;support.mirror(requestedHead);inst<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical,&support.core);support.validate(requestedHead);
        const float priority=1;VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queueInfo.queueFamilyIndex=family;queueInfo.queueCount=1;queueInfo.pQueuePriorities=&priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=requestedHead;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&queueInfo;
        dc.enabledExtensionCount=devicePointers.size();dc.ppEnabledExtensionNames=devicePointers.data();
        vkCheck(inst<PFN_vkCreateDevice>("vkCreateDevice")(physical,&dc,nullptr,&device),"Create owned SR device");
        gdpa=inst<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device,family,0,&queue);
        inst<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical,&memory);
        sr::check(sdk.xessVKCreateContext(instance,physical,device,&context),"Create XeSS Vulkan context");require(context!=nullptr,"SDK returned null context");
        sr::check(sdk.xessSetLoggingCallback(context,XESS_LOGGING_LEVEL_DEBUG,sdkLog),"XeSS logging");
        sr::check(sdk.xessForceLegacyScaleFactors(context,true),"Query legacy XeSS scale factors");
        printResolution("sr_legacy_scale_probe",resolution(sdk,context,{1920,1080},XESS_QUALITY_SETTING_QUALITY));
        sr::check(sdk.xessForceLegacyScaleFactors(context,false),"Restore current XeSS scale factors");
        printResolution("sr_current_scale_probe",resolution(sdk,context,{1920,1080},XESS_QUALITY_SETTING_QUALITY));
        const auto flags=(XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE);
        sr::check(sdk.xessVKBuildPipelines(context,VK_NULL_HANDLE,options.blockingBuild,flags),"XeSS pipeline build");
        const auto deadline=GetTickCount64()+30000;
        for(;!options.blockingBuild;){const auto status=sdk.xessGetPipelineBuildStatus(context);if(status==XESS_RESULT_SUCCESS)break;
            require(status==XESS_RESULT_ERROR_OPERATION_IN_PROGRESS,"Unexpected XeSS pipeline build status");
            require(GetTickCount64()<deadline,"XeSS pipeline build exceeded finite deadline");Sleep(10);}
        std::cout<<"{\"event\":\"sr_pipeline_build\",\"blocking\":"<<(options.blockingBuild?"true":"false")<<",\"asyncStatusPolling\":"<<(options.blockingBuild?"false":"true")<<",\"passed\":true}\n";
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCheck(dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device,&pc,nullptr,&pool),"Create SR command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        vkCheck(dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device,&ca,&command),"Allocate SR command");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkCheck(dev<PFN_vkCreateFence>("vkCreateFence")(device,&fi,nullptr,&fence),"Create SR fence");
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;
        vkCheck(dev<PFN_vkCreateQueryPool>("vkCreateQueryPool")(device,&qi,nullptr,&queries),"Create execute timestamp queries");
        xess_version_t version{};sr::check(sdk.xessGetVersion(&version),"SDK version");
        std::cout<<"{\"event\":\"sr_requirements\",\"runtimeMinimumVk\":"<<quote(vkVersion(minimum))<<",\"enabledVk\":"<<quote(vkVersion(api))
            <<",\"loaderVk\":"<<quote(vkVersion(loaderApi))<<",\"physicalVk\":"<<quote(vkVersion(properties.apiVersion))<<",\"device\":"<<quote(properties.deviceName)
            <<",\"vendorId\":"<<properties.vendorID<<",\"deviceId\":"<<properties.deviceID<<",\"driverVersionRaw\":"<<properties.driverVersion
            <<",\"timestampPeriodNs\":"<<properties.limits.timestampPeriod<<",\"timestampValidBits\":"<<validBits
            <<",\"sdkVersion\":"<<quote(sr::version(version))<<",\"instanceExtensions\":";printNames(instanceExtensions);std::cout<<",\"deviceExtensions\":";printNames(deviceExtensions);
        std::cout<<",\"requestedFeaturesSupported\":true,\"featureSnapshotCallerOwned\":true,\"pEnabledFeaturesNull\":true,\"features\":[";
        unsigned n{};for(auto* p=static_cast<VkBaseOutStructure*>(requestedHead);p;p=p->pNext) {
            if(n++)std::cout<<',';std::cout<<"{\"sType\":"<<p->sType<<",\"enabledFieldIndices\":[";
            const auto [begin,end]=Features::boolFields(p->sType);unsigned enabled{};
            for(auto at=begin;at<end;at+=sizeof(VkBool32)){VkBool32 value{};std::memcpy(&value,reinterpret_cast<const std::uint8_t*>(p)+at,sizeof(value));
                if(value){if(enabled++)std::cout<<',';std::cout<<(at-begin)/sizeof(VkBool32);}}
            std::cout<<"]}";
        }
        std::cout<<"],\"validationEnabled\":"<<(options.validation?"true":"false")<<",\"validationLayerRequested\":"<<(options.validation?quote(layer):"null")
            <<",\"validationModule\":"<<quote(validationModule)<<",\"debugUtilsMessengerCreated\":"<<(messenger?"true":"false")<<",\"gameTouched\":false}\n";
    }
    unsigned memoryType(unsigned bits,VkMemoryPropertyFlags wanted) const {
        for(unsigned n=0;n<memory.memoryTypeCount;++n)if((bits&(1u<<n))&&(memory.memoryTypes[n].propertyFlags&wanted)==wanted)return n;
        throw std::runtime_error("Required SR memory type absent");
    }
    void destroyImages() {
        for(auto& image:images){if(image.view)dev<PFN_vkDestroyImageView>("vkDestroyImageView")(device,image.view,nullptr);
            if(image.image)dev<PFN_vkDestroyImage>("vkDestroyImage")(device,image.image,nullptr);if(image.memory)dev<PFN_vkFreeMemory>("vkFreeMemory")(device,image.memory,nullptr);image={};}
        if(readback)dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device,readback,nullptr);if(hostMemory)dev<PFN_vkFreeMemory>("vkFreeMemory")(device,hostMemory,nullptr);
        readback={};hostMemory={};
    }
    void makeImage(Image& image,VkFormat format,VkExtent2D size,bool output) {
        image.format=format;image.extent=size;image.aspects=format==VK_FORMAT_D32_SFLOAT_S8_UINT?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
        VkFormatProperties props{};inst<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical,format,&props);
        require(props.optimalTilingFeatures&(output?VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT:VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),"SR image format capability missing");
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={size.width,size.height,1};
        ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        ci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|(output?VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT:VK_IMAGE_USAGE_SAMPLED_BIT);
        if(options.blitFallback&&format==VK_FORMAT_B10G11R11_UFLOAT_PACK32)ci.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vkCheck(dev<PFN_vkCreateImage>("vkCreateImage")(device,&ci,nullptr,&image.image),"Create HDR SR image");
        VkMemoryRequirements mr{};dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device,image.image,&mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=memoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&ai,nullptr,&image.memory),"Allocate HDR SR image");
        vkCheck(dev<PFN_vkBindImageMemory>("vkBindImageMemory")(device,image.image,image.memory,0),"Bind HDR SR image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
        vi.subresourceRange={format==VK_FORMAT_D32_SFLOAT_S8_UINT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCheck(dev<PFN_vkCreateImageView>("vkCreateImageView")(device,&vi,nullptr,&image.view),"Create SR image view");
    }
    void configure(VkExtent2D input,VkExtent2D output,xess_quality_settings_t quality) {
        require(!pending,"Reinitialize with an in-flight SR execution");
        xess_vk_init_params_t init{};init.outputResolution={output.width,output.height};init.qualitySetting=quality;init.initFlags=(XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE);
        sr::check(sdk.xessVKInit(context,&init),"Initialize/reinitialize XeSS HDR");
        destroyImages();makeImage(images[0],options.gameColorFormat?VK_FORMAT_B10G11R11_UFLOAT_PACK32:VK_FORMAT_R16G16B16A16_SFLOAT,input,false);
        makeImage(images[1],VK_FORMAT_R16G16_SFLOAT,input,false);
        makeImage(images[2],VK_FORMAT_D32_SFLOAT_S8_UINT,input,false);makeImage(images[3],VK_FORMAT_R16G16B16A16_SFLOAT,output,true);
        readbackBytes=VkDeviceSize(output.width)*output.height*8;
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=readbackBytes;bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bc.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(dev<PFN_vkCreateBuffer>("vkCreateBuffer")(device,&bc,nullptr,&readback),"Create SR output readback");
        VkMemoryRequirements mr{};dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device,readback,&mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=memoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        coherent=(memory.memoryTypes[ai.memoryTypeIndex].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        vkCheck(dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&ai,nullptr,&hostMemory),"Allocate SR output readback");
        vkCheck(dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device,readback,hostMemory,0),"Bind SR output readback");
        initializeImages();
    }
    static xess_vk_image_view_info view(const Image& image) {
        return {image.view,image.image,{image.format==VK_FORMAT_D32_SFLOAT_S8_UINT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},image.format,image.extent.width,image.extent.height};
    }
    void barrier(Image& image,VkImageLayout oldLayout,VkImageLayout newLayout,VkAccessFlags from,VkAccessFlags to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image.image;b.oldLayout=oldLayout;b.newLayout=newLayout;
        b.srcAccessMask=from;b.dstAccessMask=to;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={image.aspects,0,1,0,1};
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    }
    void beginCommand() {
        require(!pending,"Resetting an unfinished SR command/query");
        vkCheck(dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command,0),"Reset SR command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command,&begin),"Begin SR command");
    }
    void submitAndWait() {
        vkCheck(dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command),"End SR command");vkCheck(dev<PFN_vkResetFences>("vkResetFences")(device,1,&fence),"Reset SR fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        const auto submitted=dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&submit,fence);pending=true;vkCheck(submitted,"Submit SR execution");
        vkCheck(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,std::uint64_t(options.timeoutMs)*1000000),"Finite SR fence wait");pending=false;
    }
    void initializeImages() {
        // Keep input uploads out of every timestamped execution, including warmup.
        beginCommand();
        for(unsigned i=0;i<images.size();++i){auto& image=images[i];const auto final=i==3?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if(i<3){barrier(image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
                const VkImageSubresourceRange range{image.aspects,0,1,0,1};
                if(i==2){const VkClearDepthStencilValue depth{0.5f,0};dev<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(command,image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&depth,1,&range);}
                else {VkClearColorValue color{};if(i==0){color.float32[0]=2;color.float32[1]=0.5f;color.float32[2]=0.25f;color.float32[3]=1;}
                    dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command,image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);}
                barrier(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,final,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
            } else barrier(image,VK_IMAGE_LAYOUT_UNDEFINED,final,0,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        }
        submitAndWait();
    }
    double execute(bool reset,bool readPixels) {
        beginCommand();
        VkMemoryBarrier reuse{VK_STRUCTURE_TYPE_MEMORY_BARRIER};reuse.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        reuse.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&reuse,0,nullptr,0,nullptr);
        xess_vk_execute_params_t params{};params.colorTexture=view(images[0]);params.velocityTexture=view(images[1]);params.depthTexture=view(images[2]);params.outputTexture=view(images[3]);
        params.exposureScale=1;params.resetHistory=reset;params.inputWidth=images[0].extent.width;params.inputHeight=images[0].extent.height;
        // Static HDR/MV/depth isolates execute cost; this is not game quality evidence.
        sr::validateExecute(params,images[3].extent);
        dev<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(command,queries,0,2);
        dev<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
        sr::check(sdk.xessVKExecute(context,command,&params),"Record XeSS Vulkan Execute");
        dev<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);
        if(readPixels){barrier(images[3],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={images[3].extent.width,images[3].extent.height,1};
            dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(command,images[3].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
            barrier(images[3],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
            VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=readback;host.size=VK_WHOLE_SIZE;
            dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);}
        submitAndWait();
        std::array<std::uint64_t,4> ticks{};
        vkCheck(dev<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(device,queries,0,2,sizeof(ticks),ticks.data(),16,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT),"Read completed execute timestamps");
        require(ticks[1]&&ticks[3],"Completed SR query unavailable");
        lastTicks={ticks[0],ticks[2]};
        const auto mask=validBits==64?UINT64_MAX:(std::uint64_t(1)<<validBits)-1;
        const auto duration=double((ticks[2]-ticks[0])&mask)*properties.limits.timestampPeriod;
        require(std::isfinite(duration)&&duration>0,"Invalid XeSS execute GPU interval");++frames;if(reset)++resetFrames;
        return duration;
    }
    void verifyOutput(unsigned caseId) {
        void* mapped{};vkCheck(dev<PFN_vkMapMemory>("vkMapMemory")(device,hostMemory,0,VK_WHOLE_SIZE,0,&mapped),"Map XeSS output");
        if(!coherent){VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=hostMemory;range.size=VK_WHOLE_SIZE;
            vkCheck(dev<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device,1,&range),"Invalidate XeSS output");}
        const auto* pixels=static_cast<const std::uint16_t*>(mapped);bool valid=true;std::uint64_t checksum=1469598103934665603ull;bool nonblack{};
        for(std::size_t n=0;n<readbackBytes/2;++n){const auto half=pixels[n];valid&=(half&0x7c00)!=0x7c00;if(n%4==3)valid&=half==0x3c00;else nonblack|=(half&0x7fff)!=0;checksum=(checksum^half)*1099511628211ull;}
        dev<PFN_vkUnmapMemory>("vkUnmapMemory")(device,hostMemory);require(valid&&nonblack,"XeSS HDR output was nonfinite, transparent, or entirely black");
        std::cout<<"{\"event\":\"sr_output_readback\",\"case\":"<<caseId<<",\"everyOutputComponentFinite\":true,\"alphaOne\":true,\"notAllBlack\":true,\"checksum\":"<<checksum<<",\"bytes\":"<<readbackBytes<<"}\n";
    }
    void blitFallbackTrial(unsigned mode) {
        require(options.blitFallback&&options.gameColorFormat&&mode<3,"Explicit standalone blit test only");
        VkFormatProperties source{},destination{};
        inst<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical,images[0].format,&source);
        inst<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical,images[3].format,&destination);
        const auto need=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        require((source.optimalTilingFeatures&need)==need&&(destination.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT),"Actual formats cannot perform the fallback blit");
        const auto region=fallbackRegion(images[0].extent,images[3].extent);
        xess_context_handle_t negativeContext{};
        if(mode==1)sr::check(sdk.xessVKCreateContext(instance,physical,device,&negativeContext),"Create valid, uninitialized negative-test context");
        beginCommand();
        barrier(images[3],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue poison{{1,0,1,0}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        dev<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command,images[3].image,VK_IMAGE_LAYOUT_GENERAL,&poison,1,&range);
        barrier(images[3],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        xess_result_t result=XESS_RESULT_ERROR_UNKNOWN;
        if(mode<2){
            xess_vk_execute_params_t p{};p.colorTexture=view(images[0]);p.velocityTexture=view(images[1]);p.depthTexture=view(images[2]);p.outputTexture=view(images[3]);
            p.exposureScale=1;p.resetHistory=1;p.inputWidth=images[0].extent.width;p.inputHeight=images[0].extent.height;
            result=sdk.xessVKExecute(mode==1?negativeContext:context,command,&p);
            require(mode==0?result>=0:result==XESS_RESULT_ERROR_UNINITIALIZED,"SDK failure test did not return the expected status");
        }
        // Mode 2 models a failed command prefix that already wrote poison. It
        // does not pretend to force an internal failure in the real SDK.
        barrier(images[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        barrier(images[3],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        dev<PFN_vkCmdBlitImage>("vkCmdBlitImage")(command,images[0].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            images[3].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region,VK_FILTER_LINEAR);
        barrier(images[3],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={images[3].extent.width,images[3].extent.height,1};
        dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(command,images[3].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=readback;host.size=VK_WHOLE_SIZE;
        dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        barrier(images[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT);
        barrier(images[3],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        submitAndWait();
        if(negativeContext)sr::check(sdk.xessDestroyContext(negativeContext),"Destroy completed negative-test context");
        void* mapped{};vkCheck(dev<PFN_vkMapMemory>("vkMapMemory")(device,hostMemory,0,VK_WHOLE_SIZE,0,&mapped),"Map blit output");
        if(!coherent){VkMappedMemoryRange mappedRange{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};mappedRange.memory=hostMemory;mappedRange.size=VK_WHOLE_SIZE;
            vkCheck(dev<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device,1,&mappedRange),"Invalidate blit readback");}
        const bool correct=expectedBlitPixels({static_cast<const std::uint16_t*>(mapped),static_cast<std::size_t>(readbackBytes/2)});
        dev<PFN_vkUnmapMemory>("vkUnmapMemory")(device,hostMemory);require(correct,"Blit did not replace every poisoned HDR output component");
        std::cout<<"{\"event\":\"sr_blit_fallback_trial\",\"mode\":"<<mode<<",\"sdkResult\":"<<result
            <<",\"realSdkCall\":"<<(mode<2?"true":"false")<<",\"simulatedWrittenPrefix\":"<<(mode==2?"true":"false")
            <<",\"input\":["<<images[0].extent.width<<','<<images[0].extent.height<<"],\"output\":["<<images[3].extent.width<<','<<images[3].extent.height
            <<"],\"everyOutputComponentChecked\":true,\"bytes\":"<<readbackBytes<<",\"fenceComplete\":true,\"gameIntegrationTested\":false}\n";
    }
    void implementation() {
        xess_version_t selected{};sr::check(sdk.xessGetIntelXeFXVersion(context,&selected),"Selected Intel XeFX version");
        const auto identity=implementationIdentity(selected);
        std::cout<<"{\"event\":\"sr_implementation\",\"intelContextVersion\":"<<quote(sr::version(selected))
            <<",\"reportedIntelVersionAvailable\":"<<(identity.reportedIntelVersionAvailable?"true":"false")
            <<",\"implementation\":"<<quote(identity.implementation)<<",\"loadedModules\":[";
        bool first=true;for(const auto& module:sdk.loadedImplementations()){if(!first)std::cout<<',';first=false;
            std::cout<<"{\"name\":"<<quote(module.name)<<",\"path\":"<<quote(module.path)<<",\"fileVersion\":"<<quote(module.fileVersion)<<'}';}
        std::cout<<"],\"moduleOriginSelectionProven\":false}\n";
    }
    void close() {
        require(!pending,"An uncertain SR submission retains its complete device/SDK graph");
        sr::check(sdk.xessDestroyContext(context),"Destroy completed XeSS context");context={};destroyImages();
        dev<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(device,queries,nullptr);dev<PFN_vkDestroyFence>("vkDestroyFence")(device,fence,nullptr);
        dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device,pool,nullptr);dev<PFN_vkDestroyDevice>("vkDestroyDevice")(device,nullptr);device={};
        if(messenger)inst<PFN_vkDestroyDebugUtilsMessengerEXT>("vkDestroyDebugUtilsMessengerEXT")(instance,messenger,nullptr);
        inst<PFN_vkDestroyInstance>("vkDestroyInstance")(instance,nullptr);instance={};FreeLibrary(loader);loader={};closed=true;
        require(!validationErrors.load(),"Validation reported SR errors");
    }
};

void selfTest() {
    unsigned checks{};const auto test=[&](bool value,const char* message){require(value,message);++checks;};
    const auto region=fallbackRegion({1128,634},{1920,1080});
    test(region.srcSubresource.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&region.srcSubresource.layerCount==1&&
        region.dstSubresource.layerCount==1&&region.srcOffsets[0].x==0&&region.srcOffsets[1].x==1128&&region.srcOffsets[1].y==634&&
        region.dstOffsets[1].x==1920&&region.dstOffsets[1].y==1080&&region.srcOffsets[1].z==1&&region.dstOffsets[1].z==1,"Fallback blit covers the complete source/output color region");
    for(const auto bad:std::array{VkExtent2D{0,634},VkExtent2D{1128,0},VkExtent2D{UINT32_MAX,634}}){bool rejected=false;
        try{(void)fallbackRegion(bad,{1920,1080});}catch(...){rejected=true;}test(rejected,"Malformed source extent rejected before recording");
        rejected=false;try{(void)fallbackRegion({1128,634},bad);}catch(...){rejected=true;}test(rejected,"Malformed output extent rejected before recording");}
    std::array<std::uint16_t,8> good{0x4000,0x3800,0x3400,0x3c00,0x4000,0x3800,0x3400,0x3c00};
    test(expectedBlitPixels(good),"Exact HDR components and alpha are checked across the full readback");
    for(std::size_t at=0;at<good.size();++at){auto wrong=good;wrong[at]=0;test(!expectedBlitPixels(wrong),"Any retained poisoned component fails fallback verification");}
    test(!expectedBlitPixels({})&&!expectedBlitPixels(std::span(good).first(7)),"Empty or truncated readback is never a pass");
    for(const auto& preset:sr::presets){const VkExtent2D out{1920,1080},input{unsigned(std::round(1920/preset.scale)),unsigned(std::round(1080/preset.scale))};test(sr::closestPreset(input,out).quality==preset.quality,"Five quality mappings including AA");}
    test(sr::closestPreset({1128,634},{1920,1080}).quality==XESS_QUALITY_SETTING_QUALITY,"Actual balanced dimensions map to XeSS Quality");
    const std::vector<std::string> app{"app","shared"},sdk{"shared","sr"};test(sr::mergeExtensions(app,sdk)==std::vector<std::string>({"app","shared","sr"}),"Required extensions merge without pointer-based duplicates");
    VkPhysicalDeviceFeatures2 req{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};req.features.shaderStorageImageWriteWithoutFormat=VK_TRUE;
    Features support;support.mirror(&req);bool refused{};try{support.validate(&req);}catch(...){refused=true;}test(refused,"Required unsupported feature rejected");
    support.core.features.shaderStorageImageWriteWithoutFormat=VK_TRUE;support.validate(&req);++checks;
    req.pNext=&req;refused=false;try{support.mirror(&req);}catch(...){refused=true;}test(refused,"Cyclic feature merge rejected");req.pNext=nullptr;
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableType{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    mutableType.mutableDescriptorType=VK_TRUE;req.pNext=&mutableType;Features chained;chained.mirror(&req);
    chained.core.features=req.features;chained.mutableType.mutableDescriptorType=VK_TRUE;chained.validate(&req);
    test(chained.core.pNext==&chained.mutableType&&chained.types.size()==2,"Caller core and required extension feature structs both queried");
    VkBaseOutStructure unknown{VK_STRUCTURE_TYPE_APPLICATION_INFO};Features captured;captured.capture(&req);
    mutableType.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;mutableType.pNext=&unknown;mutableType.mutableDescriptorType=VK_FALSE;req.features={};req.pNext=nullptr;
    test(captured.core.pNext==&captured.mutableType&&captured.mutableType.pNext==nullptr&&
        captured.core.features.shaderStorageImageWriteWithoutFormat&&captured.mutableType.mutableDescriptorType==VK_TRUE&&
        captured.mutableType.sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT,
        "Caller-owned feature snapshot survives overwritten SDK nodes and links");
    chained.validate(&captured.core);++checks;
    mutableType.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT;mutableType.pNext=nullptr;req.pNext=&mutableType;
    mutableType.mutableDescriptorType=2;refused=false;try{chained.validate(&req);}catch(...){refused=true;}test(refused,"Malformed required feature boolean rejected");
    req.pNext=&unknown;refused=false;try{chained.mirror(&req);}catch(...){refused=true;}
    test(refused,"Unknown SDK feature struct rejected before device creation");req.pNext=nullptr;
    const std::array ranges{Resolution{XESS_QUALITY_SETTING_QUALITY,{1129,636},{1129,636},{1920,1080}},
        Resolution{XESS_QUALITY_SETTING_BALANCED,{960,540},{960,540},{1920,1080}}};
    test(selectResolution({1129,636},ranges).quality==XESS_QUALITY_SETTING_QUALITY,"Legal nominal-quality input retains the requested quality");
    test(!ranges[0].contains({1128,634})&&selectResolution({1128,634},ranges).quality==XESS_QUALITY_SETTING_BALANCED,
        "Rounded game extent preserves exact pixels and selects only a queried legal adjacent quality range");
    refused=false;try{(void)selectResolution({1128,634},std::span(ranges).first(1));}catch(...){refused=true;}
    test(refused,"Exact game dimensions outside every queried range remain rejected");
    test(!ranges[1].contains({0,634})&&!ranges[1].contains({959,540})&&!ranges[1].contains({1920,1081}),"Resolution range boundaries remain strict");
    const auto stub=implementationIdentity({});test(!stub.reportedIntelVersionAvailable&&std::string(stub.implementation)=="unknown",
        "Zero Intel-version stub cannot identify cross-vendor or Intel backend");
    const auto reported=implementationIdentity({2,0,2,64});test(reported.reportedIntelVersionAvailable&&std::string(reported.implementation)=="unknown",
        "A reported version alone cannot establish the selected implementation module");
    xess_vk_execute_params_t p{};p.inputWidth=1128;p.inputHeight=634;p.exposureScale=1;
    std::array<xess_vk_image_view_info*,4> views{&p.colorTexture,&p.velocityTexture,&p.depthTexture,&p.outputTexture};
    for(unsigned n=0;n<4;++n){auto& v=*views[n];v.image=reinterpret_cast<VkImage>(std::uintptr_t(100+n));v.imageView=reinterpret_cast<VkImageView>(std::uintptr_t(200+n));
        v.format=n==1?VK_FORMAT_R16G16_SFLOAT:n==2?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R16G16B16A16_SFLOAT;
        v.width=n==3?1920:1128;v.height=n==3?1080:634;v.subresourceRange={n==2?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
    sr::validateExecute(p,{1920,1080});++checks;
    auto packedColor=p;packedColor.colorTexture.format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    sr::validateExecute(packedColor,{1920,1080});++checks;
    test(packedColor.outputTexture.format==VK_FORMAT_R16G16B16A16_SFLOAT,
        "Game packed-float input retains the distinct RGBA16F output format");
    for(unsigned mutation=0;mutation<7;++mutation){auto bad=p;if(mutation==0)bad.jitterOffsetX=0.6f;if(mutation==1)bad.exposureScale=0;if(mutation==2)bad.resetHistory=2;
        if(mutation==3)bad.outputTexture.image=bad.colorTexture.image;if(mutation==4)bad.depthTexture.subresourceRange.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;
        if(mutation==5)bad.colorTexture.format=VK_FORMAT_R8G8B8A8_UNORM;if(mutation==6)bad.inputWidth=1129;
        refused=false;try{sr::validateExecute(bad,{1920,1080});}catch(...){refused=true;}test(refused,"Invalid HDR execute metadata rejected");}
    std::cout<<"{\"event\":\"sr_cpu_self_test\",\"passed\":true,\"checks\":"<<checks<<",\"gpuExecuted\":false,\"sdkLoaded\":false}\n";
}
}
int main(int argc,char** argv) {
    std::cout<<std::setprecision(12);
    std::unique_ptr<Owner> owner;
    try {
        Options options;
        for(int n=1;n<argc;++n){const std::string arg=argv[n];if(arg=="--self-test"){selfTest();return 0;}
            if(arg=="--help"){std::puts("xess-sr-vulkan-test --sdk-bin ABSOLUTE_PATH [--warmup 4] [--samples 12] [--timeout-ms 5000] [--game-color-format] [--test-blit-fallback] [--validation] [--validation-layer-dir ABSOLUTE_PATH]; --self-test runs without SDK/GPU");return 0;}
            if(arg=="--test-blit-fallback"){options.blitFallback=options.gameColorFormat=true;continue;}
            if(arg=="--game-color-format"){options.gameColorFormat=true;continue;}
            if(arg=="--blocking-build"){options.blockingBuild=true;continue;}
            if(arg=="--validation"){options.validation=true;continue;}require(n+1<argc,"Missing option value");const std::string value=argv[++n];
            if(arg=="--sdk-bin")options.sdk=value;else if(arg=="--validation-layer-dir"){options.validationLayerDir=value;options.validation=true;}
            else {require(!value.empty()&&value.find_first_not_of("0123456789")==std::string::npos,"Expected numeric benchmark option");const auto number=std::stoul(value);
                if(arg=="--warmup"){require(number>=2&&number<=32,"Warmup range2..32");options.warmup=number;}
                else if(arg=="--samples"){require(number>=4&&number<=120,"Sample range4..120");options.samples=number;}
                else if(arg=="--timeout-ms"){require(number>=100&&number<=60000,"Timeout range100..60000");options.timeoutMs=number;}else throw std::invalid_argument("Unknown SR option");}}
        require(options.sdk.is_absolute(),"--sdk-bin must be an absolute trusted SDK directory");owner=std::make_unique<Owner>(options);owner->initialize();
        if(options.blitFallback){
            const VkExtent2D input{1128,634},output{1920,1080};auto quality=XESS_QUALITY_SETTING_QUALITY;
            for(;;){const auto range=resolution(owner->sdk,owner->context,output,quality);if(range.contains(input))break;
                require(quality>XESS_QUALITY_SETTING_ULTRA_PERFORMANCE,"No SDK range contains the exact game input");quality=static_cast<xess_quality_settings_t>(unsigned(quality)-1);}
            owner->configure(input,output,quality);
            for(unsigned mode=0;mode<3;++mode)owner->blitFallbackTrial(mode);
            owner->close();
            std::cout<<"{\"event\":\"sr_blit_fallback_summary\",\"passed\":true,\"cases\":3,\"validationEnabled\":"<<(options.validation?"true":"false")
                <<",\"validationErrors\":"<<owner->validationErrors.load()<<",\"gameIntegrationTested\":false,\"allSdkFailuresRecoverableProven\":false}\n";return 0;
        }
        unsigned caseId{};
        const auto run=[&](const sr::Preset& preset,VkExtent2D output,VkExtent2D explicitInput,bool resized) {
            ++caseId;
            const auto requested=resolution(owner->sdk,owner->context,output,preset.quality);printResolution("sr_requested_input_range",requested);
            const VkExtent2D input=explicitInput.width?explicitInput:VkExtent2D{requested.optimal.x,requested.optimal.y};
            std::array<Resolution,7> candidates{};candidates[0]=requested;unsigned count=1;
            for(auto q=unsigned(preset.quality);!candidates[count-1].contains(input)&&q>unsigned(XESS_QUALITY_SETTING_ULTRA_PERFORMANCE);){
                candidates[count]=resolution(owner->sdk,owner->context,output,static_cast<xess_quality_settings_t>(--q));
                printResolution("sr_adjacent_input_range",candidates[count++]);
            }
            const auto& selected=selectResolution(input,std::span(candidates).first(count));
            const auto quality=selected.quality;const bool rangeFallback=quality!=preset.quality;
            const auto optimal=selected.optimal,minimum=selected.minimum,maximum=selected.maximum;
            std::cout<<"{\"event\":\"sr_input_policy\",\"case\":"<<caseId<<",\"requestedQuality\":"<<preset.quality<<",\"effectiveQuality\":"<<quality
                <<",\"exactInputPreserved\":true,\"input\":["<<input.width<<','<<input.height<<"],\"output\":["<<output.width<<','<<output.height
                <<"],\"legacyScaleFactorsForced\":false,\"rangeFallback\":"<<(rangeFallback?"true":"false")
                <<",\"reason\":"<<quote(rangeFallback?"exact_input_outside_nominal_quality_range":"requested_quality_range_contains_input")<<"}\n";
            owner->configure(input,output,quality);owner->implementation();std::vector<double> durations;
            float jitterX{},jitterY{},velocityX{},velocityY{};
            sr::check(owner->sdk.xessGetJitterScale(owner->context,&jitterX,&jitterY),"Query configured jitter scale");
            sr::check(owner->sdk.xessGetVelocityScale(owner->context,&velocityX,&velocityY),"Query configured velocity scale");
            std::cout<<"{\"event\":\"sr_case_begin\",\"case\":"<<caseId<<",\"preset\":"<<quote(preset.game)<<",\"quality\":"<<quality<<",\"requestedQuality\":"<<preset.quality
                <<",\"input\":["<<input.width<<','<<input.height<<"],\"output\":["<<output.width<<','<<output.height<<"],\"sdkOptimal\":["<<optimal.x<<','<<optimal.y
                <<"],\"sdkMin\":["<<minimum.x<<','<<minimum.y<<"],\"sdkMax\":["<<maximum.x<<','<<maximum.y<<"],\"jitterScale\":["<<jitterX<<','<<jitterY
                <<"],\"velocityScale\":["<<velocityX<<','<<velocityY<<"],\"initFlags\":"<<(XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE)
                <<",\"colorFormat\":"<<owner->images[0].format<<",\"outputFormat\":"<<owner->images[3].format<<",\"inputPreparationFenceCompleted\":true}\n";
            for(unsigned frame=0;frame<options.warmup+options.samples+2;++frame){const bool reset=frame==0||frame==options.warmup+options.samples;
                const bool sampled=frame>=options.warmup&&frame<options.warmup+options.samples;
                const bool last=frame==options.warmup+options.samples+1;const auto duration=owner->execute(reset,last);if(sampled)durations.push_back(duration);
                std::cout<<"{\"event\":\"sr_execute\",\"case\":"<<caseId<<",\"preset\":"<<quote(preset.game)<<",\"quality\":"<<quality<<",\"frame\":"<<owner->frames
                    <<",\"reset\":"<<(reset?"true":"false")<<",\"sampled\":"<<(sampled?"true":"false")<<",\"durationNs\":"<<duration
                    <<",\"beginTick\":"<<owner->lastTicks[0]<<",\"endTick\":"<<owner->lastTicks[1]
                    <<",\"queryAvailable\":true,\"fenceComplete\":true,\"intervalOnlyXeSSExecute\":true}\n";}
            owner->verifyOutput(caseId);std::sort(durations.begin(),durations.end());
            const auto percentile=[&](double p){return durations[static_cast<std::size_t>(std::ceil(p*durations.size()))-1];};
            std::cout<<"{\"event\":\"sr_case\",\"case\":"<<caseId<<",\"preset\":"<<quote(preset.game)<<",\"quality\":"<<quality<<",\"requestedQuality\":"<<preset.quality
                <<",\"rangeFallback\":"<<(rangeFallback?"true":"false")<<",\"input\":["<<input.width<<','<<input.height
                <<"],\"output\":["<<output.width<<','<<output.height<<"],\"sdkOptimal\":["<<optimal.x<<','<<optimal.y<<"],\"samples\":"<<durations.size()
                <<",\"p50Ns\":"<<percentile(0.5)<<",\"p95Ns\":"<<percentile(0.95)<<",\"minNs\":"<<durations.front()<<",\"maxNs\":"<<durations.back()
                <<",\"resized\":"<<(resized?"true":"false")<<",\"hdrRgba16f\":"<<(options.gameColorFormat?"false":"true")
                <<",\"colorFormat\":"<<owner->images[0].format<<",\"outputFormat\":"<<owner->images[3].format<<",\"staticFixtureOnly\":true}\n";
        };
        for(const auto& preset:sr::presets)run(preset,{1920,1080},preset.quality==XESS_QUALITY_SETTING_QUALITY?VkExtent2D{1128,634}:VkExtent2D{},false);
        run(sr::presets[2],{1280,720},{},true);run(sr::presets[2],{1920,1080},{1128,634},true);
        run(sr::presets[1],{1280,720},{852,480},true);run(sr::presets[1],{1920,1080},{1280,720},true);
        owner->close();std::cout<<"{\"event\":\"sr_summary\",\"passed\":true,\"cases\":9,\"frames\":"<<owner->frames<<",\"resetFrames\":"<<owner->resetFrames
            <<",\"finiteFenceCleanup\":true,\"validationEnabled\":"<<(options.validation?"true":"false")<<",\"validationErrors\":"<<owner->validationErrors.load()<<",\"validationWarnings\":"<<owner->validationWarnings.load()
            <<",\"gameIntegrationTested\":false,\"dispatchReplacementEnabled\":false}\n";return 0;
    } catch(const std::exception& error) {
        const bool retained=owner&&!owner->closed;if(retained)owner.release();
        std::cout<<"{\"event\":\"sr_summary\",\"passed\":false,\"error\":"<<quote(error.what())<<",\"completeGraphRetainedUntilExit\":"<<(retained?"true":"false")
            <<",\"gameIntegrationTested\":false,\"dispatchReplacementEnabled\":false}\n";return 1;
    }
}
