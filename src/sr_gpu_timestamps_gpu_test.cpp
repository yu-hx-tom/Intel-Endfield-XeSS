// Self-owned offscreen Vulkan query lifecycle. No SDK, game, observer layer or FSR shader.
#include "sr_gpu_timestamps.hpp"
#include "render_pass_observer.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace timer=xess_vk::sr_gpu_timestamps;
namespace rp=xess_vk::render_pass_observer;
constexpr std::uint64_t fenceTimeoutNs=5'000'000'000;
unsigned checks{};
std::atomic<unsigned> validationErrors{},validationWarnings{},validationMessages{};
void need(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
void vkcheck(VkResult result,const char* message){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(message)+": "+std::to_string(result));}
template<class T>std::uint64_t bits(T value){return reinterpret_cast<std::uintptr_t>(value);}
VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* data,void*) noexcept {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++validationErrors;else if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)++validationWarnings;
    if(validationMessages.fetch_add(1)<32)std::fprintf(stderr,"VVL severity=%u id=%s: %s\n",severity,
        data&&data->pMessageIdName?data->pMessageIdName:"unknown",data&&data->pMessage?data->pMessage:"no message");
    return VK_FALSE;
}
DWORD WINAPI watchdog(void* event){if(WaitForSingleObject(static_cast<HANDLE>(event),25000)==WAIT_TIMEOUT){
        std::fputs("SR timestamp fixture exceeded its 25-second watchdog.\n",stderr);TerminateProcess(GetCurrentProcess(),124);}return 0;}
struct Watchdog {
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr),thread=event?CreateThread(nullptr,0,watchdog,event,0,nullptr):nullptr;
    Watchdog(){need(event&&thread,"fixture watchdog creation");}
    ~Watchdog(){SetEvent(event);WaitForSingleObject(thread,1000);CloseHandle(thread);CloseHandle(event);}
};
struct Trace {
    timer::Functions actual;
    unsigned creates{},destroys{},resets{},writes{},reads{};
    bool valid{true},deviceDestroyed{};
} trace;
VKAPI_ATTR VkResult VKAPI_CALL createQuery(VkDevice d,const VkQueryPoolCreateInfo* info,const VkAllocationCallbacks* a,VkQueryPool* out){
    trace.valid&=!trace.deviceDestroyed&&info&&info->queryType==VK_QUERY_TYPE_TIMESTAMP&&info->queryCount==2*timer::passCapacity;
    const auto result=trace.actual.create(d,info,a,out);if(result==VK_SUCCESS)++trace.creates;return result;
}
VKAPI_ATTR void VKAPI_CALL destroyQuery(VkDevice d,VkQueryPool pool,const VkAllocationCallbacks* a){
    trace.valid&=!trace.deviceDestroyed;++trace.destroys;trace.actual.destroy(d,pool,a);
}
VKAPI_ATTR void VKAPI_CALL resetQuery(VkCommandBuffer c,VkQueryPool pool,std::uint32_t first,std::uint32_t count){
    trace.valid&=!trace.deviceDestroyed&&first%2==0&&count==2;++trace.resets;trace.actual.reset(c,pool,first,count);
}
VKAPI_ATTR void VKAPI_CALL writeQuery(VkCommandBuffer c,VkPipelineStageFlagBits stage,VkQueryPool pool,std::uint32_t index){
    trace.valid&=!trace.deviceDestroyed&&stage==(index%2?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    ++trace.writes;trace.actual.write(c,stage,pool,index);
}
VKAPI_ATTR VkResult VKAPI_CALL readQuery(VkDevice d,VkQueryPool pool,std::uint32_t first,std::uint32_t count,std::size_t size,void* data,VkDeviceSize stride,VkQueryResultFlags flags){
    trace.valid&=!trace.deviceDestroyed&&count==2&&flags==(VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    ++trace.reads;return trace.actual.read(d,pool,first,count,size,data,stride,flags);
}
struct Fixture {
    HMODULE loader{};PFN_vkGetInstanceProcAddr gipa{};PFN_vkGetDeviceProcAddr gdpa{};
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};
    VkDebugUtilsMessengerEXT messenger{};VkPhysicalDeviceProperties properties{};VkQueueFamilyProperties queueProperties{};
    std::uint32_t family{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    VkShaderModule shader{};VkPipelineLayout layout{};VkPipeline pipeline{};
    std::uint64_t pipelineGeneration{},callbackSerial{},windowSerial{};
    rp::Context* recordings{};std::unique_ptr<timer::Observer> observer;
    std::array<timer::Sample,4> samples{};std::size_t sampleCount{};bool sampleOverflow{};
    bool pending{},cleanupCompleted{},validationLoaded{},stopRetainedSubmitted{},stopRetainedUnsubmitted{};
    VkResult fenceAtStop{VK_ERROR_UNKNOWN};
    template<class T>T inst(const char* name){auto result=gipa(instance,name);need(result!=nullptr,name);return reinterpret_cast<T>(result);}
    template<class T>T dev(const char* name){auto result=gdpa(device,name);need(result!=nullptr,name);return reinterpret_cast<T>(result);}
    static void sample(void* user,const timer::Sample& value) noexcept {auto& f=*static_cast<Fixture*>(user);
        if(f.sampleCount<f.samples.size())f.samples[f.sampleCount++]=value;else f.sampleOverflow=true;}
    void initialize(){
        need(!GetModuleHandleW(L"xess-vulkan-observer.dll"),"production observer must not be loaded");
        loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);need(loader!=nullptr,"system Vulkan loader");
        gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));need(gipa!=nullptr,"real GIPA");
        const auto enumerateLayers=inst<PFN_vkEnumerateInstanceLayerProperties>("vkEnumerateInstanceLayerProperties");
        std::uint32_t count{};vkcheck(enumerateLayers(&count,nullptr),"instance layer count");need(count&&count<=128,"bounded layer inventory");
        std::vector<VkLayerProperties> layers(count);vkcheck(enumerateLayers(&count,layers.data()),"instance layers");bool found{};
        for(const auto& layer:layers)if(!std::strcmp(layer.layerName,"VK_LAYER_KHRONOS_validation"))found=true;
        need(found,"local Khronos validation layer must be available");
        const char* layer="VK_LAYER_KHRONOS_validation";const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback=validationMessage;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Owned SR timestamp query fixture";app.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;
        ic.enabledExtensionCount=1;ic.ppEnabledExtensionNames=&extension;ic.pNext=&debug;
        vkcheck(inst<PFN_vkCreateInstance>("vkCreateInstance")(&ic,nullptr,&instance),"create actual instance");
        vkcheck(inst<PFN_vkCreateDebugUtilsMessengerEXT>("vkCreateDebugUtilsMessengerEXT")(instance,&debug,nullptr,&messenger),"actual validation messenger");
        const auto validationModule=GetModuleHandleW(L"VkLayer_khronos_validation.dll");need(validationModule!=nullptr,"actual validation DLL loaded");
        std::array<wchar_t,32768> modulePath{},directory{};need(GetModuleFileNameW(validationModule,modulePath.data(),modulePath.size())>0,"loaded validation DLL path");
        const auto length=GetEnvironmentVariableW(L"VK_LAYER_PATH",directory.data(),directory.size());need(length&&length<directory.size(),"child-private validation directory");
        need(std::filesystem::equivalent(std::filesystem::path(directory.data())/L"VkLayer_khronos_validation.dll",std::filesystem::path(modulePath.data())),"loaded layer is the specified local layer");
        validationLoaded=true;std::cout<<"{\"event\":\"sr_timestamp_validation_loaded\",\"path\":"<<std::quoted(std::filesystem::path(modulePath.data()).generic_string())<<"}\n";
        const auto enumerate=inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");vkcheck(enumerate(instance,&count,nullptr),"physical count");need(count&&count<=32,"bounded physical inventory");
        std::vector<VkPhysicalDevice> devices(count);vkcheck(enumerate(instance,&count,devices.data()),"actual physical devices");
        for(const auto candidate:devices){VkPhysicalDeviceProperties p{};inst<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate,&p);
            if(p.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU||!(p.limits.timestampPeriod>0)||!std::isfinite(p.limits.timestampPeriod))continue;
            std::uint32_t n{};const auto families=inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");families(candidate,&n,nullptr);
            if(!n||n>64)continue;std::array<VkQueueFamilyProperties,64> values{};families(candidate,&n,values.data());if(n>values.size())continue;
            for(std::uint32_t q=0;q<n;++q)if(values[q].queueCount&&(values[q].queueFlags&VK_QUEUE_COMPUTE_BIT)&&values[q].timestampValidBits&&values[q].timestampValidBits<=64){
                if(!physical||p.vendorID==0x8086){physical=candidate;properties=p;family=q;queueProperties=values[q];}break;}
            if(physical&&properties.vendorID==0x8086)break;
        }
        need(physical!=VK_NULL_HANDLE,"real non-CPU compute device with timestamp support");
        const float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;
        vkcheck(inst<PFN_vkCreateDevice>("vkCreateDevice")(physical,&dc,nullptr,&device),"actual owned device");
        gdpa=inst<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device,family,0,&queue);
        need(!GetModuleHandleW(L"xess-vulkan-observer.dll"),"production observer layer remains absent");
        recordings=rp::create(device,nullptr);need(recordings!=nullptr,"actual lifecycle metadata owner");rp::queue(recordings,queue,family);
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=family;
        vkcheck(dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device,&cp,nullptr,&pool),"actual command pool");rp::poolCreated(recordings,pool,&cp,VK_SUCCESS);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
        vkcheck(dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device,&ai,&command),"actual primary CB");rp::allocate(recordings,&ai,&command,VK_SUCCESS);
        constexpr std::uint32_t code[]{0x07230203,0x00010000,0,5,0,(2u<<16)|17,1,(3u<<16)|14,0,1,
            (5u<<16)|15,5,3,0x6e69616d,0,(6u<<16)|16,3,17,1,1,1,(2u<<16)|19,1,(3u<<16)|33,2,1,
            (5u<<16)|54,1,3,0,2,(2u<<16)|248,4,(1u<<16)|253,(1u<<16)|56};
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};si.codeSize=sizeof(code);si.pCode=code;
        vkcheck(dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(device,&si,nullptr,&shader),"actual standard no-op compute shader");
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};vkcheck(dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device,&li,nullptr,&layout),"actual empty pipeline layout");
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pi.layout=layout;pi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pi.stage.module=shader;pi.stage.pName="main";
        vkcheck(dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline),"actual no-op compute pipeline");++pipelineGeneration;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkcheck(dev<PFN_vkCreateFence>("vkCreateFence")(device,&fi,nullptr,&fence),"fixture-owned fence");
        trace.actual={dev<PFN_vkCreateQueryPool>("vkCreateQueryPool"),dev<PFN_vkDestroyQueryPool>("vkDestroyQueryPool"),dev<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool"),
            dev<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp"),dev<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")};
        std::cout<<"{\"event\":\"sr_timestamp_owned_runtime\",\"device_name\":"<<std::quoted(properties.deviceName)<<",\"vendor_id\":"<<properties.vendorID
            <<",\"device_id\":"<<properties.deviceID<<",\"queue_family\":"<<family<<",\"queue_flags\":"<<queueProperties.queueFlags
            <<",\"timestamp_valid_bits\":"<<queueProperties.timestampValidBits<<",\"timestamp_period_ns\":"<<properties.limits.timestampPeriod
            <<",\"actual_pipeline\":"<<bits(pipeline)<<",\"pipeline_generation\":"<<pipelineGeneration<<",\"observer_layer_loaded\":false,\"sdk_used\":false}\n";
    }
    void record(){
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};const auto begin=dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command,&bi);
        rp::begin(recordings,command,&bi,begin);vkcheck(begin,"actual CB begin");
        dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        const auto facts=rp::dispatchPoint(recordings,command);need(facts.recording&&facts.command==bits(command)&&facts.rpRecordingGeneration&&facts.poolGeneration,"facts come from actual create/begin/dispatch observations");
        timer::Config config;config.device=device;config.incarnation=facts.contextIncarnation;config.window=++windowSerial;config.queueFamily=family;
        config.queueFlags=queueProperties.queueFlags;config.validBits=queueProperties.timestampValidBits;config.periodNs=properties.limits.timestampPeriod;
        config.functions={createQuery,destroyQuery,resetQuery,writeQuery,readQuery};config.sink=sample;config.sinkUser=this;config.enabled=true;
        observer=std::make_unique<timer::Observer>(config);need(observer->active(),"actual capabilities admitted by production timestamp component");
        timer::Identity identity;identity.window=windowSerial;identity.ordinal=facts.commonRecordOrder;identity.callback=++callbackSerial;
        identity.pipeline=bits(pipeline);identity.pipelineGeneration=pipelineGeneration;identity.pass=timer::Pass::Accumulate;
        // Fixture-local sequence/grid labels satisfy the component interface;
        // these are explicitly NOT a game callback, SDK ticket or SR resolution.
        identity.frame={std::uint32_t(callbackSerial),{std::uint32_t(callbackSerial),facts.contextIncarnation,false},0,true};
        identity.groups={1,1,1};identity.render=identity.output={1,1};identity.extentsKnown=true;
        SetLastError(0x12345678);const auto ticket=observer->beforeDispatch(facts,identity);need(bool(ticket)&&GetLastError()==0x12345678,"real query insertion preserves caller LastError");
        dev<PFN_vkCmdDispatch>("vkCmdDispatch")(command,1,1,1);const auto error=GetLastError();observer->afterDispatch(ticket);need(GetLastError()==error,"real end-query call preserves original dispatch LastError");
        const auto end=dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command);rp::end(recordings,command,end);vkcheck(end,"actual CB end");
    }
    void run(){
        record();VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&command;
        const auto actual=rp::beginSubmit(recordings,queue,1,&info);need(actual.queueKnown&&!actual.issues&&actual.commandCount==1,"complete real submit membership");
        const auto ticket=observer->beforeSubmit(queue,family,{actual.recordingFacts.data(),actual.commandCount},!actual.issues);
        const auto submitted=dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&info,fence);pending=submitted==VK_SUCCESS;
        observer->submitted(ticket,submitted);rp::finishSubmit(recordings,actual,fence,submitted);vkcheck(submitted,"actual owned submit");
        observer->stop();fenceAtStop=dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,fence);
        stopRetainedSubmitted=!observer->drained()&&observer->stats().retainedAtStop==1&&trace.destroys==0&&trace.reads==0;
        need(stopRetainedSubmitted,"window stop retains submitted recording without assuming its fence state");
        vkcheck(dev<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,fenceTimeoutNs),"bounded wait on only fixture fence");pending=false;
        vkcheck(dev<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,fence),"actual owned fence completion");
        const auto old=rp::recordingFacts(recordings,command);const auto reset=dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command,0);
        vkcheck(reset,"actual completed CB reset");observer->retire(old.command,old.rpRecordingGeneration);rp::reset(recordings,command,reset);
        need(observer->drained()&&trace.destroys==1&&trace.reads==1&&sampleCount==1&&!sampleOverflow,"actual reset retires query pool and reads exactly one submitted interval");
        const auto& s=samples[0];need(s.recording.command==bits(command)&&s.recording.rpRecordingGeneration==old.rpRecordingGeneration&&s.identity.pipeline==bits(pipeline)&&
            s.identity.pipelineGeneration==pipelineGeneration&&s.execution==1&&!s.replay&&s.completion==timer::Completion::RecordingRetired,"returned interval preserves real native recording and pipeline identity");
        need(s.gpu.available&&s.gpu.durationKnown&&std::isfinite(s.gpu.durationNs)&&s.gpu.durationNs>=0&&s.gpu.beginAvailability&&s.gpu.endAvailability,"real submitted timestamp values are available and duration is known");
        std::cout<<"{\"event\":\"sr_timestamp_owned_sample\",\"actual_command\":"<<s.recording.command<<",\"recording_generation\":"<<s.recording.rpRecordingGeneration
            <<",\"actual_pipeline\":"<<s.identity.pipeline<<",\"begin_ticks\":"<<s.gpu.beginTicks<<",\"end_ticks\":"<<s.gpu.endTicks<<",\"duration_ns\":"<<s.gpu.durationNs
            <<",\"available\":true,\"duration_known\":true,\"native_submit_success\":true,\"native_fence_completed\":true,\"native_reset_before_retire\":true,\"fixture_sequence_labels_only\":true,\"game_fsr_verified\":false}\n";
        observer.reset();record();observer->stop();stopRetainedUnsubmitted=!observer->drained()&&observer->stats().retainedAtStop==1&&trace.destroys==1&&trace.reads==1;
        need(stopRetainedUnsubmitted,"never-submitted executable recording retains pool at window stop");
        const auto unsubmitted=rp::recordingFacts(recordings,command);dev<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(device,pool,1,&command);
        observer->retire(unsubmitted.command,unsubmitted.rpRecordingGeneration);rp::free(recordings,1,&command);command=VK_NULL_HANDLE;
        need(observer->drained()&&observer->stats().discardedUnsubmitted==1&&trace.destroys==2&&trace.reads==1,"actual free discards unsubmitted queries without reading them");
        need(trace.valid&&trace.creates==2&&trace.resets==2&&trace.writes==4,"real query APIs forwarded once per owned operation with no WAIT reads");
    }
    bool cleanup() noexcept {
        if(cleanupCompleted)return true;if(pending)return false;
        try {
            if(device){if(pool){const auto old=pool;dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device,pool,nullptr);pool=VK_NULL_HANDLE;command=VK_NULL_HANDLE;
                    if(observer)observer->retirePool(bits(old));rp::resetPool(recordings,old,true,VK_SUCCESS);}
                if(observer&&!observer->drained())return false;observer.reset();
                if(fence)dev<PFN_vkDestroyFence>("vkDestroyFence")(device,fence,nullptr);
                if(pipeline)dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device,pipeline,nullptr);
                if(shader)dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device,shader,nullptr);
                if(layout)dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device,layout,nullptr);
                dev<PFN_vkDestroyDevice>("vkDestroyDevice")(device,nullptr);device=VK_NULL_HANDLE;trace.deviceDestroyed=true;}
            rp::destroy(recordings);recordings=nullptr;
            if(instance){if(messenger)inst<PFN_vkDestroyDebugUtilsMessengerEXT>("vkDestroyDebugUtilsMessengerEXT")(instance,messenger,nullptr);
                inst<PFN_vkDestroyInstance>("vkDestroyInstance")(instance,nullptr);instance=VK_NULL_HANDLE;}
            if(loader){FreeLibrary(loader);loader=nullptr;}cleanupCompleted=true;return true;
        }catch(...){return false;}
    }
};
}
int main(int argc,char** argv){
    if(argc!=2||std::string(argv[1])!="--run-owned-gpu"){std::cerr<<"Explicit --run-owned-gpu required; use scripts/test-sr-gpu-timestamps.ps1.\n";return 2;}
    Watchdog watchdog;Fixture fixture;bool passed{};std::string error;
    try {fixture.initialize();fixture.run();passed=true;}catch(const std::exception& e){error=e.what();}
    const bool cleaned=fixture.cleanup();passed=passed&&cleaned&&fixture.validationLoaded&&!validationErrors&&trace.valid;
    std::cout<<"{\"event\":\"sr_timestamp_gpu_result\",\"passed\":"<<(passed?"true":"false")<<",\"checks\":"<<checks<<",\"validation_layer_loaded\":"<<(fixture.validationLoaded?"true":"false")
        <<",\"validation_errors\":"<<validationErrors<<",\"validation_warnings\":"<<validationWarnings<<",\"parent_cleanup_completed\":"<<(cleaned?"true":"false")
        <<",\"quarantined_until_process_exit\":"<<(!cleaned?"true":"false")<<",\"query_pools_created\":"<<trace.creates<<",\"query_pools_destroyed\":"<<trace.destroys
        <<",\"query_reads\":"<<trace.reads<<",\"fence_status_at_stop\":"<<fixture.fenceAtStop<<",\"stop_retained_submitted\":"<<(fixture.stopRetainedSubmitted?"true":"false")
        <<",\"stop_retained_unsubmitted\":"<<(fixture.stopRetainedUnsubmitted?"true":"false")<<",\"fixture_fence_timeout_ns\":"<<fenceTimeoutNs
        <<",\"device_wait_idle_calls\":0,\"game_fsr_verified\":false,\"sdk_used\":false,\"error\":"<<std::quoted(error)<<"}\n";
    return passed?0:1;
}
