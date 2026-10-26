// Reuse downstream fixture drivers; this translation unit includes the REAL
// production layer and invokes its dispatch/submit/retirement wrappers.
#define XESS_RAWCOLOR_LAYER_FIXTURE
#include "command_recording_layer_test.cpp"
#include "rawcolor_capture_probe_test.cpp"
#include <string>
namespace rawcolor_layer_fixture {
namespace layer=recording_layer_fixture;namespace driver=rawcolor_cpu_fixture;
namespace memory=xess_vk::resource_memory_observer;namespace mf=xess_vk::resource_memory_facts;
unsigned checks{},appDestroys{};
void need(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
VKAPI_ATTR VkResult VKAPI_CALL allocation(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory* out){*out=layer::handle<VkDeviceMemory>(layer::sourceObject);SetLastError(0x76543210);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL bind(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize){SetLastError(0x76543210);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice,const VkAllocationCallbacks*){++appDestroys;SetLastError(0xabcdef);}
VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance,const VkAllocationCallbacks*){++appDestroys;}
void armGate(){
    wchar_t directory[1024]{},temp[1024]{};need(GetTempPathW(1024,directory)!=0&&GetTempFileNameW(directory,L"xrc",0,temp)!=0,"private own-process marker fixture");DeleteFileW(temp);
    DeviceState state{};std::wmemcpy(state.rawArmPath,temp,std::wcslen(temp)+1);
    need(!rawCheckArm(&state)&&!state.rawArmed&&!InterlockedCompareExchange(&rawProcessLease,0,0),"missing marker consumes no lease");
    auto write=[&](const char* text,DWORD count){auto file=CreateFileW(temp,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);need(file!=INVALID_HANDLE_VALUE,"marker own file opened");DWORD bytes{};need(WriteFile(file,text,count,&bytes,nullptr)&&bytes==count,"marker literal written");CloseHandle(file);state.rawArmLastCheck=0;};
    write("END_FIELD_RAWCOLOR_ONCE_V",24);need(!rawCheckArm(&state),"incomplete token rejected");write("END_FIELD_RAWCOLOR_ONCE_V2",24);need(!rawCheckArm(&state),"wrong literal rejected");
    constexpr char literal[]="END_FIELD_RAWCOLOR_ONCE_V1";write(literal,sizeof(literal)-1);need(rawCheckArm(&state)&&state.rawArmed,"exact marker latches once");DeleteFileW(temp);need(rawCheckArm(&state),"latched arm stops file polling");
    DeviceState other{};std::swprintf(other.rawArmPath,std::size(other.rawArmPath),L"%ls-another-session",temp);need(!rawCheckArm(&other),"other-session marker cannot arm");
    VkInstanceCreateInfo i{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};need(rawInstanceCallbacksAbsent(&i),"actual empty instance callback surface allowed");const char* debug=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;i.enabledExtensionCount=1;i.ppEnabledExtensionNames=&debug;
    need(!rawInstanceCallbacksAbsent(&i),"enabled debug extension rejects future application callback lifetime");i.enabledExtensionCount=0;VkDebugReportCallbackCreateInfoEXT callback{VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT};i.pNext=&callback;need(!rawInstanceCallbacksAbsent(&i),"explicit callback create chain refused");
    VkBaseInStructure unknown{VK_STRUCTURE_TYPE_MAX_ENUM,nullptr};i.pNext=&unknown;need(!rawInstanceCallbacksAbsent(&i),"unknown application instance chain refuses deferred lifetime");
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueCount=1;VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;need(rawDeviceQueueShape(&di),"literal single total queue admitted");qi.queueCount=2;need(!rawDeviceQueueShape(&di),"additional same-family queue denied");qi.queueCount=1;di.queueCreateInfoCount=2;need(!rawDeviceQueueShape(&di),"second family info denied");
}
using CaptureHook=void(*)(layer::Fixture&,VkCommandBuffer,VkQueue);
void actualDispatchAndSubmit(bool quarantine,CaptureHook beforeDispatch=nullptr,CaptureHook captureReady=nullptr){
    layer::Fixture f;driver::reset();driver::device=layer::device;driver::queue=layer::queue;driver::borrowedCommand=layer::command;driver::expectedSource=layer::handle<VkImage>(0x811);
    // Observe exact pool replacement through wrappers, then use a fresh RP
    // context so the fixture's old family3 queue cannot contaminate family0.
    vkDestroyCommandPool(layer::device,layer::pool,&f.allocator);layer::ro::destroy(f.state.renderPass);f.state.renderPass=layer::ro::create(layer::device,nullptr,{recordedLayoutInventory,&f.state});layer::active=f.state.renderPass;
    f.poolInfo.queueFamilyIndex=0;VkCommandPool pool{};vkCreateCommandPool(layer::device,&f.poolInfo,nullptr,&pool);VkCommandBuffer command{};vkAllocateCommandBuffers(layer::device,&f.alloc,&command);f.beginInfo.flags=0;f.start();VkQueue q{};vkGetDeviceQueue(layer::device,0,0,&q);
    auto config=driver::config();f.state.rawQueue=q;f.state.rawUniqueQueue=true;f.state.rawProbe=raw::create(config);f.state.rawArmed=true;
    mf::Config mc;mc.deviceScope=mf::DeviceScope::SinglePhysicalDevice;mc.bindingCoverage=mf::BindingCoverage::CompleteOrdinaryCalls;mc.bufferImageGranularity=1;
    f.state.resourceMemory=memory::create(layer::device,config.memory,mc,nullptr);f.state.allocateMemory=allocation;f.state.bindImageMemory=bind;f.state.imageMemoryRequirements=driver::imageRequirements;
    std::array<VkImage,3> images{};std::array<VkImageView,3> views{};std::array<VkDescriptorImageInfo,3> descriptors{};std::array<VkDescriptorSetLayoutBinding,3> bindings{};
    for(unsigned n=0;n<3;++n){VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=static_cast<VkFormat>(n==0?130:n==1?122:83);image.extent={4,4,1};image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        image.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|(n?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);layer::sourceObject=0x810+n;need(vkCreateImage(layer::device,&image,nullptr,&images[n])==VK_SUCCESS,"source creation actual wrapper+query");
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=256;layer::sourceObject=0x910+n;VkDeviceMemory memory{};need(vkAllocateMemory(layer::device,&allocate,nullptr,&memory)==VK_SUCCESS&&vkBindImageMemory(layer::device,images[n],memory,0)==VK_SUCCESS,"source allocation+bind actual wrapper");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=images[n];view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=image.format;view.subresourceRange={n?VK_IMAGE_ASPECT_COLOR_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};layer::sourceObject=0x820+n;need(vkCreateImageView(layer::device,&view,nullptr,&views[n])==VK_SUCCESS,"source view actual wrapper");
        descriptors[n]={VK_NULL_HANDLE,views[n],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};bindings[n]={5+n,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};const auto r=f.inventory->lookup(bits(images[n])).record;
        layer::di::observeSource(f.state.descriptors,n==0?layer::di::Source::RawDepth:n==1?layer::di::Source::RawColor:layer::di::Source::RawMotion,{r.image,r.generation},1+n);
    }
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};si.bindingCount=3;si.pBindings=bindings.data();VkDescriptorSetLayout sl{};layer::sourceObject=0x830;vkCreateDescriptorSetLayout(layer::device,&si,nullptr,&sl);
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&sl;VkPipelineLayout pl{};layer::sourceObject=0x831;vkCreatePipelineLayout(layer::device,&pi,nullptr,&pl);
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&poolSize;VkDescriptorPool descriptorPool{};layer::sourceObject=0x832;vkCreateDescriptorPool(layer::device,&dp,nullptr,&descriptorPool);
    VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=descriptorPool;sa.descriptorSetCount=1;sa.pSetLayouts=&sl;VkDescriptorSet set{};layer::sourceObject=0x833;vkAllocateDescriptorSets(layer::device,&sa,&set);
    std::array<VkWriteDescriptorSet,3> writes{};for(unsigned n=0;n<3;++n){auto& w=writes[n];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;w.dstBinding=5+n;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=&descriptors[n];}vkUpdateDescriptorSets(layer::device,3,writes.data(),0,nullptr);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=pl;ci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=layer::handle<VkShaderModule>(0x900);ci.stage.pName="main";VkPipeline pipeline{};layer::sourceObject=0x834;vkCreateComputePipelines(layer::device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&set,0,nullptr);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=images[1];barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    if(beforeDispatch)beforeDispatch(f,command,q);
    layer::lineCount=0;f.dispatch();need(driver::inlineCopies==1&&raw::status(f.state.rawProbe).sourceInserted&&layer::di::stats(f.state.descriptors).metadata.acceptedDispatches==1,"actual direct wrapper consumes same owned accepted raw3+layout+memory then inserts exactly one source copy before original dispatch");
    need(layer::contains("\"memory_generation\":")&&layer::contains("\"recorded_layout_resolved\":true")&&GetLastError()==0x76543210,"same sampled facts emitted and original LastError preserved");
    vkEndCommandBuffer(command);VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&timeline;submit.commandBufferCount=1;submit.pCommandBuffers=&command;
    layer::checkStart();need(vkQueueSubmit(q,1,&submit,layer::handle<VkFence>(999))==VK_SUCCESS,"original timeline submit literal result");layer::once(layer::Submit);need(driver::privateSubmits==1&&raw::pixels(f.state.rawProbe).empty(),"typed timeline actual wrapper appends first private readback, pending pixels unread");
    driver::fenceResult=VK_SUCCESS;raw::poll(f.state.rawProbe);layer::checkStart();vkQueueSubmit(q,1,&submit,layer::handle<VkFence>(999));layer::once(layer::Submit);need(driver::privateSubmits==1&&raw::status(f.state.rawProbe).completion.replays==1,"actual serial flags0 submit replay never rewrites staging");
    VkSubmitInfo unknown=submit;VkDeviceGroupSubmitInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO};unknown.pNext=&group;need(!rawMembership(&f.state,1,&unknown).known,"device-group semantics explicitly rejected without altering original args");
    if(captureReady)captureReady(f,command,q);
    // Move the fixture's already observed owner into real layer heap records so
    // production vkDestroyDevice/vkDestroyInstance can exercise their teardown.
    auto* live=static_cast<DeviceState*>(HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(DeviceState)));auto* parent=static_cast<InstanceState*>(HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(InstanceState)));need(live&&parent,"teardown bookkeeping allocated");
    *live=f.state;live->destroy=destroyDevice;live->parent=parent;parent->instance=reinterpret_cast<VkInstance>(layer::device);parent->key=&layer::marker;parent->destroy=destroyInstance;parent->defaultAllocator=parent->applicationCallbacksAbsentKnown=true;
    devices=live;instances=parent;f.inventory.release();f.state.imageInventory=nullptr;f.state.renderPass=nullptr;f.state.descriptors=nullptr;f.state.resourceMemory=nullptr;f.state.rawProbe=nullptr;layer::active=nullptr;layer::descriptors=nullptr;
    driver::waitResult=quarantine?VK_TIMEOUT:VK_SUCCESS;appDestroys=0;vkDestroyDevice(layer::device,nullptr);
    if(quarantine){need(!appDestroys&&!driver::destroys&&live->rawQuarantined&&parent->rawQuarantined&&raw::status(live->rawProbe).shutdownQuarantined,"actual original DestroyDevice withheld with every child/device/parent retained on timeout");vkDestroyInstance(parent->instance,nullptr);need(!appDestroys&&instances==parent&&devices==live,"quarantined parent originalDestroyInstance withheld; bounded owner retained until process exit");}
    else{need(appDestroys==1&&driver::destroys==7&&driver::drainSubmits==1&&driver::waits==1&&devices==nullptr&&GetLastError()==0xabcdef,"actual original DestroyDevice exactly once after latest-work own drain/child teardown, original LastError preserved");vkDestroyInstance(parent->instance,nullptr);need(appDestroys==2&&!instances,"normal originalDestroyInstance follows completed child cleanup");}
}
}
int main(){using namespace rawcolor_layer_fixture;try{SetEnvironmentVariableW(L"XESS_OBSERVER_LOG",nullptr);InitOnceExecuteOnce(&logOnce,initializeLog,nullptr,nullptr);armGate();actualDispatchAndSubmit(false);inventorySessionRetired=false;actualDispatchAndSubmit(true);std::cout<<"{\"stage\":\"rawcolor-actual-layer\",\"passed\":true,\"checks\":"<<checks+recording_layer_fixture::checks+rawcolor_cpu_fixture::checks<<",\"gpuTouched\":false,\"gameTouched\":false}\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
