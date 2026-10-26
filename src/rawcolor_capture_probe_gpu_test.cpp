// New, isolated real-probe replay fixture. No game, installed layer, or old
// fixture entrypoint. Shared compilation waits for the probe owner's freeze.
#include "rawcolor_capture_probe.hpp"
#include "render_pass_observer.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
namespace rp=xess_vk::render_pass_observer;
namespace di=xess_vk::descriptor_image_observer;
namespace rm=xess_vk::resource_memory_observer;
namespace rf=xess_vk::resource_memory_facts;
namespace probe=xess_vk::rawcolor_capture_probe;
namespace completion=xess_vk::borrowed_capture_completion;
void need(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void vkcheck(VkResult value,const char* why){if(value!=VK_SUCCESS)throw std::runtime_error(std::string(why)+": "+std::to_string(value));}
void check(probe::Result value,const char* why){if(!value.accepted())throw std::runtime_error(std::string(why)+": reasons "+std::to_string(value.reasons));}
template<class T>std::uint64_t bits(T value){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<std::uintptr_t>(value);else return value;}
struct Image {VkImage handle{};VkDeviceMemory memory{};VkImageView view{};VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};xess_vk::image_inventory::Record inventory{};};
struct Buffer {VkBuffer handle{};VkDeviceMemory memory{};bool coherent{};};
struct NativeOwnedTrace {
    unsigned createdImages{},destroyedImages{},createdBuffers{},destroyedBuffers{},allocations{},frees{},createdPools{},destroyedPools{},createdFences{},destroyedFences{};
    unsigned privateReadbackSubmits{},drainSubmits{},stagingCopyCommands{};
    std::array<VkFence,2> fences{};
};
struct Fixture;
Fixture* active{};
// Only probe-owned calls use these wrappers. All application/fixture calls use
// the separately saved real driver directly, avoiding false ownership counts.
VKAPI_ATTR VkResult VKAPI_CALL ownCreateImage(VkDevice,const VkImageCreateInfo*,const VkAllocationCallbacks*,VkImage*);
VKAPI_ATTR void VKAPI_CALL ownDestroyImage(VkDevice,VkImage,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL ownCreateBuffer(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer*);
VKAPI_ATTR void VKAPI_CALL ownDestroyBuffer(VkDevice,VkBuffer,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL ownAllocate(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory*);
VKAPI_ATTR void VKAPI_CALL ownFree(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL ownCreatePool(VkDevice,const VkCommandPoolCreateInfo*,const VkAllocationCallbacks*,VkCommandPool*);
VKAPI_ATTR void VKAPI_CALL ownDestroyPool(VkDevice,VkCommandPool,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL ownCreateFence(VkDevice,const VkFenceCreateInfo*,const VkAllocationCallbacks*,VkFence*);
VKAPI_ATTR void VKAPI_CALL ownDestroyFence(VkDevice,VkFence,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL ownSubmit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence);
VKAPI_ATTR void VKAPI_CALL ownCopyToBuffer(VkCommandBuffer,VkImage,VkImageLayout,VkBuffer,std::uint32_t,const VkBufferImageCopy*);
void sink(const char* line){std::puts(line);}

struct Fixture {
    HMODULE loader{};PFN_vkGetInstanceProcAddr gipa{};PFN_vkGetDeviceProcAddr gdpa{};
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};
    VkPhysicalDeviceProperties properties{};VkPhysicalDeviceMemoryProperties memory{};
    std::uint32_t family{};VkQueueFlags queueFlags{};NativeOwnedTrace owned{};
    xess_vk::image_inventory::Inventory<8> inventory;
    std::array<Image,3> images{}; // exact raw depth/color/motion, all 64x64
    VkCommandPool pool{};std::array<VkCommandBuffer,3> commands{}; // clear, borrowed, oracle
    VkFence initializationFence{},applicationFence{},oracleFence{};VkSemaphore gate{};
    VkDescriptorSetLayout setLayout{};VkDescriptorPool descriptorPool{};VkDescriptorSet set{};
    VkPipelineLayout pipelineLayout{};VkShaderModule shader{};VkPipeline pipeline{};
    Buffer oracle{};
    rp::Context* recordings{};di::Context* descriptors{};rm::Context* memoryObserver{};
    probe::Context* capture{};
    di::DispatchSourceFacts sourceFacts{};completion::RecordingKey key{};
    bool processLeaseClaimed{},sourceInitialized{},pendingGate{},mightPending{},firstReady{},firstStableAfterReplay{},ownedBAfterReplay{},retired{},cleanupDone{},quarantined{};
    bool pendingNoPixels{},pendingNoFile{},drainComplete{},normalReleaseRetained{};
    VkFence actualFirstFence{},actualDrainFence{};
    unsigned originalSubmits{},originalCallsWithUnchangedArguments{},readinessRefusals{},verifiedPixels{},resetBorrowedByFixture{};
    static constexpr std::uint32_t width=64,height=64,pixelA=0x001e0000,pixelB=0x780003c0;
    ~Fixture(){if(!cleanupDone)(void)cleanup();}
    template<class T>T instanceFunction(const char* name){auto value=gipa(instance,name);need(value!=nullptr,name);return reinterpret_cast<T>(value);}
    template<class T>T native(const char* name){auto value=gdpa(device,name);need(value!=nullptr,name);return reinterpret_cast<T>(value);}
    std::uint32_t memoryType(std::uint32_t mask,VkMemoryPropertyFlags flags){for(std::uint32_t i=0;i<memory.memoryTypeCount;++i)if((mask&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&flags)==flags)return i;throw std::runtime_error("memory type unavailable");}
    static xess_vk::image_inventory::Snapshot inventoryLookup(void* user,std::uint64_t handle) noexcept {
        return static_cast<Fixture*>(user)->inventory.lookup(handle);
    }
    static bool claimLease(void* user) noexcept {auto& self=*static_cast<Fixture*>(user);if(self.processLeaseClaimed)return false;self.processLeaseClaimed=true;return true;}
    void initialize();
    void createSourcesAndDescriptors();
    probe::Functions probeFunctions();
    void clear(bool first);
    void recordBorrowed();
    void submitBorrowed();
    void verifyFirstStaging(std::uint32_t expected);
    void recordOracleB(VkImage);
    void verifyOracleB();
    void run();
    bool cleanup() noexcept;
    void begin(VkCommandBuffer command,VkCommandBufferUsageFlags flags){VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};info.flags=flags;vkcheck(native<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command,&info),"begin owned/borrowed command");}
    void submitAndWait(VkCommandBuffer command,VkFence fence){VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&command;mightPending=true;vkcheck(native<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&info,fence),"fixture submit");vkcheck(native<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&fence,VK_TRUE,5'000'000'000),"bounded fixture wait");mightPending=false;}
};

void Fixture::initialize(){
    active=this;need(!GetModuleHandleW(L"xess-vulkan-observer.dll"),"raw fixture loaded production observer");
    loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);need(loader!=nullptr,"system Vulkan loader");
    gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));need(gipa!=nullptr,"gipa");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Rawcolor first-readback replay proof";app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;
    vkcheck(instanceFunction<PFN_vkCreateInstance>("vkCreateInstance")(&ic,nullptr,&instance),"create instance with default allocator");
    std::uint32_t count{};vkcheck(instanceFunction<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance,&count,nullptr),"device count");std::vector<VkPhysicalDevice> devices(count);
    vkcheck(instanceFunction<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance,&count,devices.data()),"devices");
    for(const auto candidate:devices){VkPhysicalDeviceProperties p{};instanceFunction<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate,&p);if(p.apiVersion>=VK_API_VERSION_1_2&&(!physical||p.vendorID==0x8086)){physical=candidate;properties=p;if(p.vendorID==0x8086)break;}}
    need(physical!=VK_NULL_HANDLE,"Vulkan1.2 physical device");
    VkPhysicalDeviceTimelineSemaphoreFeatures available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f.pNext=&available;
    instanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical,&f);need(available.timelineSemaphore==VK_TRUE,"timeline feature");
    instanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);
    instanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&count,families.data());
    auto selected=std::find_if(families.begin(),families.end(),[](const auto& q){return q.queueCount&&(q.queueFlags&3)==3;});need(selected!=families.end(),"graphics/compute family");
    family=static_cast<std::uint32_t>(selected-families.begin());queueFlags=selected->queueFlags;const float priority=1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};timeline.timelineSemaphore=VK_TRUE;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=&timeline;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;
    vkcheck(instanceFunction<PFN_vkCreateDevice>("vkCreateDevice")(physical,&dc,nullptr,&device),"create single physical ordinary device/default allocator");
    need(!GetModuleHandleW(L"xess-vulkan-observer.dll"),"raw fixture unexpectedly loaded the production observer during Vulkan creation");
    gdpa=instanceFunction<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");native<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device,family,0,&queue);
    instanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical,&memory);
    recordings=rp::create(device,sink,{inventoryLookup,this});descriptors=di::create(device,sink);need(recordings&&descriptors,"owned metadata adapters");
    rf::Config memoryConfig;memoryConfig.deviceScope=rf::DeviceScope::SinglePhysicalDevice;memoryConfig.bindingCoverage=rf::BindingCoverage::CompleteOrdinaryCalls;memoryConfig.bufferImageGranularity=properties.limits.bufferImageGranularity;
    memoryObserver=rm::create(device,memory,memoryConfig,sink);need(memoryObserver!=nullptr,"memory adapter");rp::queue(recordings,queue,family);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=family;cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkcheck(native<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device,&cp,nullptr,&pool),"fixture command pool");rp::poolCreated(recordings,pool,&cp,VK_SUCCESS);di::commandPoolCreated(descriptors,pool,&cp,VK_SUCCESS);
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};alloc.commandPool=pool;alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;alloc.commandBufferCount=3;
    vkcheck(native<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device,&alloc,commands.data()),"fixture commands");rp::allocate(recordings,&alloc,commands.data(),VK_SUCCESS);di::allocate(descriptors,&alloc,commands.data(),VK_SUCCESS);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};for(auto fence:{&initializationFence,&applicationFence,&oracleFence})vkcheck(native<PFN_vkCreateFence>("vkCreateFence")(device,&fi,nullptr,fence),"fixture fence");
    VkSemaphoreTypeCreateInfo st{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};st.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};si.pNext=&st;
    vkcheck(native<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device,&si,nullptr,&gate),"host gate");
    createSourcesAndDescriptors();
    probe::Config config;config.device=device;config.queue=queue;config.queueFamily=family;config.queueFlags=queueFlags;config.memory=memory;
    config.nonCoherentAtomSize=properties.limits.nonCoherentAtomSize;config.functions=probeFunctions();config.enabled=true;
    config.singlePhysicalDeviceKnown=config.uniqueOrdinaryExternallySynchronizedQueueKnown=true;
    config.defaultDeviceAndInstanceAllocatorsKnown=config.applicationDeviceMemoryCallbackAbsentKnown=true;
    config.applicationInstanceCallbacksAbsentKnown=ic.pNext==nullptr&&ic.enabledExtensionCount==0;
    VkFormatProperties format{};instanceFunction<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical,VK_FORMAT_B10G11R11_UFLOAT_PACK32,&format);
    const auto formatResult=instanceFunction<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties")(physical,VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&config.targetFormatProperties);
    config.targetFormatTransferSupported=formatResult==VK_SUCCESS&&(format.optimalTilingFeatures&(VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT))==(VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    need(config.targetFormatTransferSupported&&config.targetFormatProperties.maxExtent.width>=width&&config.targetFormatProperties.maxExtent.height>=height
        &&config.targetFormatProperties.maxExtent.depth>=1&&config.targetFormatProperties.maxMipLevels>=1&&config.targetFormatProperties.maxArrayLayers>=1
        &&(config.targetFormatProperties.sampleCounts&VK_SAMPLE_COUNT_1_BIT)&&config.targetFormatProperties.maxResourceSize>=width*height*4,"actual format122 target capabilities");
    config.claimProcessLease=claimLease;config.leaseUser=this;config.sink=sink;
    need(GetFullPathNameW(L"rawcolor-first.bmp",static_cast<DWORD>(config.outputBmpPath.size()),config.outputBmpPath.data(),nullptr)>0,"absolute fixture BMP path");
    capture=probe::create(config);need(capture!=nullptr,"real rawcolor probe context");
    std::printf("{\"event\":\"rawcolor_replay_runtime\",\"vendorId\":%u,\"deviceId\":%u,\"queueFamily\":%u,\"queueFlags\":%u,\"enabledQueueCount\":1,\"observerLoaded\":false,\"sourceFormat\":122,\"width\":64,\"height\":64,\"instanceExtensionsEnabled\":%u,\"instanceCreatePNext\":false,\"applicationInstanceCallbacksAbsentKnown\":%s}\n",properties.vendorID,properties.deviceID,family,queueFlags,ic.enabledExtensionCount,config.applicationInstanceCallbacksAbsentKnown?"true":"false");
    std::printf("{\"event\":\"rawcolor_target_format_query\",\"format\":122,\"queryResult\":%d,\"optimalFeatures\":%u,\"maxExtent\":[%u,%u,%u],\"maxMipLevels\":%u,\"maxArrayLayers\":%u,\"sampleCounts\":%u,\"maxResourceSize\":%llu}\n",
        int(formatResult),format.optimalTilingFeatures,config.targetFormatProperties.maxExtent.width,config.targetFormatProperties.maxExtent.height,config.targetFormatProperties.maxExtent.depth,
        config.targetFormatProperties.maxMipLevels,config.targetFormatProperties.maxArrayLayers,config.targetFormatProperties.sampleCounts,static_cast<unsigned long long>(config.targetFormatProperties.maxResourceSize));
}

VkImageMemoryBarrier transition(VkImage image,VkImageAspectFlags aspects,VkImageLayout old,VkImageLayout next,VkAccessFlags source,VkAccessFlags destination){
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.subresourceRange={aspects,0,1,0,1};b.oldLayout=old;b.newLayout=next;
    b.srcAccessMask=source;b.dstAccessMask=destination;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;return b;
}
void Fixture::createSourcesAndDescriptors(){
    const std::array<VkFormat,3> formats{VK_FORMAT_D32_SFLOAT_S8_UINT,VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_R16G16_SFLOAT};
    for(std::size_t i=0;i<images.size();++i){auto& image=images[i];auto& info=image.info;
        info.imageType=VK_IMAGE_TYPE_2D;info.format=formats[i];info.extent={width,height,1};info.mipLevels=info.arrayLayers=1;
        info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        // The actual game's sources belong to the RP adapter's attachment
        // registration class. Query and create that real usage class here too.
        info.usage|=i?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageFormatProperties format{};vkcheck(instanceFunction<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties")(
            physical,info.format,info.imageType,info.tiling,info.usage,info.flags,&format),"actual source image format query");
        need(format.maxExtent.width>=width&&format.maxExtent.height>=height&&(format.sampleCounts&VK_SAMPLE_COUNT_1_BIT),"source format extent/sample");
        std::printf("{\"event\":\"rawcolor_source_format_query\",\"sourceIndex\":%zu,\"format\":%u,\"usage\":%u,\"queryResult\":0,\"maxExtent\":[%u,%u,%u],\"sampleCounts\":%u}\n",
            i,std::uint32_t(info.format),info.usage,format.maxExtent.width,format.maxExtent.height,format.maxExtent.depth,format.sampleCounts);
        vkcheck(native<PFN_vkCreateImage>("vkCreateImage")(device,&info,nullptr,&image.handle),"create source image");
        const xess_vk::image_inventory::Description description{std::uint32_t(info.format),width,height,1,1,1,std::uint32_t(info.samples),info.usage,info.flags,
            std::uint32_t(info.tiling),std::uint32_t(info.sharingMode),std::uint32_t(info.initialLayout)};
        inventory.created(bits(image.handle),description);const auto observed=inventory.lookup(bits(image.handle));
        need(observed.status==xess_vk::image_inventory::Status::LiveHandleObserved,"actual common inventory registration");image.inventory=observed.record;
        rp::imageCreated(recordings,&info,image.handle,VK_SUCCESS,&image.inventory);di::imageObserved(descriptors,image.inventory);
        VkMemoryRequirements requirements{};native<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device,image.handle,&requirements);
        rm::imageCreated(memoryObserver,image.inventory,&info,VK_SUCCESS,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkcheck(native<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&allocation,nullptr,&image.memory),"allocate source image memory");rm::memoryAllocated(memoryObserver,image.memory,&allocation,VK_SUCCESS);
        VkBindImageMemoryInfo bind{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};bind.image=image.handle;bind.memory=image.memory;
        const auto ticket=rm::beforeImageBind(memoryObserver,rf::BindApi::ImageCore,1,&bind);const auto result=native<PFN_vkBindImageMemory>("vkBindImageMemory")(device,image.handle,image.memory,0);
        rm::afterBind(memoryObserver,ticket,result);vkcheck(result,"bind source image memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image.handle;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=info.format;
        view.subresourceRange={i?VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT):VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT),0,1,0,1};
        vkcheck(native<PFN_vkCreateImageView>("vkCreateImageView")(device,&view,nullptr,&image.view),"source view");di::viewCreated(descriptors,image.view,&view,VK_SUCCESS);rp::viewCreated(recordings,&view,image.view,VK_SUCCESS);
        di::observeSource(descriptors,i==0?di::Source::RawDepth:i==1?di::Source::RawColor:di::Source::RawMotion,{image.inventory.image,image.inventory.generation},1);
    }
    std::array<VkDescriptorSetLayoutBinding,3> bindings{};for(std::size_t i=0;i<bindings.size();++i){bindings[i].binding=5+std::uint32_t(i);bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=std::uint32_t(bindings.size());layout.pBindings=bindings.data();
    vkcheck(native<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(device,&layout,nullptr,&setLayout),"source descriptor layout");di::setLayoutCreated(descriptors,setLayout,&layout,VK_SUCCESS);
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&size;
    vkcheck(native<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(device,&dp,nullptr,&descriptorPool),"source descriptor pool");di::poolCreated(descriptors,descriptorPool,&dp,VK_SUCCESS);
    VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=descriptorPool;sa.descriptorSetCount=1;sa.pSetLayouts=&setLayout;
    vkcheck(native<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(device,&sa,&set),"source descriptor set");di::setsAllocated(descriptors,&sa,&set,VK_SUCCESS);
    std::array<VkDescriptorImageInfo,3> imageInfos{};std::array<VkWriteDescriptorSet,3> writes{};
    for(std::size_t i=0;i<images.size();++i){imageInfos[i]={VK_NULL_HANDLE,images[i].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet=set;writes[i].dstBinding=5+std::uint32_t(i);writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;writes[i].pImageInfo=&imageInfos[i];}
    native<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(device,3,writes.data(),0,nullptr);di::updateDescriptors(descriptors,3,writes.data(),0,nullptr);
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&setLayout;
    vkcheck(native<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device,&pl,nullptr,&pipelineLayout),"pipeline layout");di::pipelineLayoutCreated(descriptors,pipelineLayout,&pl,VK_SUCCESS);
    // Same established no-resource GLCompute shader as the old fixtures. It
    // supplies a real dispatch location, and does not sample any source image.
    constexpr std::uint32_t code[]={0x07230203,0x00010000,0,5,0,(2u<<16)|17,1,(3u<<16)|14,0,1,
        (5u<<16)|15,5,3,0x6e69616d,0,(6u<<16)|16,3,17,1,1,1,(2u<<16)|19,1,(3u<<16)|33,2,1,
        (5u<<16)|54,1,3,0,2,(2u<<16)|248,4,(1u<<16)|253,(1u<<16)|56};
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=sizeof(code);sm.pCode=code;
    vkcheck(native<PFN_vkCreateShaderModule>("vkCreateShaderModule")(device,&sm,nullptr,&shader),"no-op shader");
    VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;compute.stage.module=shader;compute.stage.pName="main";compute.layout=pipelineLayout;
    vkcheck(native<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(device,VK_NULL_HANDLE,1,&compute,nullptr,&pipeline),"compute pipeline");di::pipelineCreated(descriptors,pipeline,&compute,VK_SUCCESS);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=width*height*4;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vkcheck(native<PFN_vkCreateBuffer>("vkCreateBuffer")(device,&bi,nullptr,&oracle.handle),"oracle buffer");VkMemoryRequirements requirements{};
    native<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device,oracle.handle,&requirements);rm::bufferCreated(memoryObserver,oracle.handle,&bi,VK_SUCCESS,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;
    try{allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);}catch(...){allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);}
    oracle.coherent=(memory.memoryTypes[allocation.memoryTypeIndex].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
    vkcheck(native<PFN_vkAllocateMemory>("vkAllocateMemory")(device,&allocation,nullptr,&oracle.memory),"oracle allocation");rm::memoryAllocated(memoryObserver,oracle.memory,&allocation,VK_SUCCESS);
    VkBindBufferMemoryInfo bind{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};bind.buffer=oracle.handle;bind.memory=oracle.memory;
    const auto bt=rm::beforeBufferBind(memoryObserver,rf::BindApi::BufferCore,1,&bind);const auto result=native<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device,oracle.handle,oracle.memory,0);
    rm::afterBind(memoryObserver,bt,result);vkcheck(result,"oracle bind");need(rm::stats(memoryObserver).globalIssues==0,"complete actual memory coverage");
}
probe::Functions Fixture::probeFunctions(){
    probe::Functions f;f.createImage=ownCreateImage;f.destroyImage=ownDestroyImage;f.imageRequirements=native<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
    f.createBuffer=ownCreateBuffer;f.destroyBuffer=ownDestroyBuffer;f.bufferRequirements=native<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements");
    f.allocateMemory=ownAllocate;f.freeMemory=ownFree;f.bindImageMemory=native<PFN_vkBindImageMemory>("vkBindImageMemory");f.bindBufferMemory=native<PFN_vkBindBufferMemory>("vkBindBufferMemory");
    f.createPool=ownCreatePool;f.destroyPool=ownDestroyPool;f.allocateCommands=native<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers");
    f.beginCommand=native<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");f.endCommand=native<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
    f.createFence=ownCreateFence;f.destroyFence=ownDestroyFence;f.fenceStatus=native<PFN_vkGetFenceStatus>("vkGetFenceStatus");f.waitFences=native<PFN_vkWaitForFences>("vkWaitForFences");
    f.barrier=native<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");f.copyImage=native<PFN_vkCmdCopyImage>("vkCmdCopyImage");f.copyImageToBuffer=ownCopyToBuffer;
    f.submit=ownSubmit;f.mapMemory=native<PFN_vkMapMemory>("vkMapMemory");f.unmapMemory=native<PFN_vkUnmapMemory>("vkUnmapMemory");f.invalidate=native<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges");return f;
}
void Fixture::clear(bool first){
    if(!first){vkcheck(native<PFN_vkResetFences>("vkResetFences")(device,1,&initializationFence),"reset completed initialization fence");
        vkcheck(native<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands[0],0),"fixture clear CB reset");}
    begin(commands[0],VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);std::array<VkImageMemoryBarrier,3> barriers{};
    for(std::size_t i=0;i<images.size();++i)barriers[i]=transition(images[i].handle,i?VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT):VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT),
        first?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,first?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    native<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands[0],VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,3,barriers.data());
    const VkClearDepthStencilValue depth{first?0.25f:0.75f,first?17u:91u};const VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    native<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(commands[0],images[0].handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&depth,1,&depthRange);
    const VkClearColorValue color=first?VkClearColorValue{{0,1,0,1}}:VkClearColorValue{{1,0,1,1}};
    const VkClearColorValue motion=first?VkClearColorValue{{0.25f,0.5f,0,0}}:VkClearColorValue{{0.75f,0.125f,0,0}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    native<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands[0],images[1].handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
    native<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands[0],images[2].handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&motion,1,&range);
    // Leave TRANSFER_DST as the literal source anchor expected by the same
    // borrowed recording on both executions; do not discard A in that CB.
    vkcheck(native<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands[0]),"end clear");submitAndWait(commands[0],initializationFence);sourceInitialized=true;
    std::printf("{\"event\":\"rawcolor_source_clear_completed\",\"value\":\"%s\",\"actualFenceStatus\":%d,\"finalLayout\":7,\"cpuUpload\":false}\n",first?"A":"B",int(native<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,initializationFence)));
}
void Fixture::recordBorrowed(){
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; // actual flags0, not ONE_TIME
    const auto result=native<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands[1],&beginInfo);rp::begin(recordings,commands[1],&beginInfo,result);di::begin(descriptors,commands[1],&beginInfo,result);vkcheck(result,"begin real flags0 borrowed command");
    std::array<VkImageMemoryBarrier,3> barriers{};for(std::size_t i=0;i<images.size();++i)barriers[i]=transition(images[i].handle,
        i?VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT):VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    native<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands[1],VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,3,barriers.data());
    rp::explicitImageBarriers(recordings,commands[1],VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,3,barriers.data());
    native<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands[1],VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);di::bindPipeline(descriptors,commands[1],VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    native<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands[1],VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);
    di::bindSets(descriptors,commands[1],VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);
    const auto facts=rp::dispatchPoint(recordings,commands[1]);di::SourceQueryDemand demand;demand.everyMatchedBinding=true;demand.rawPatternProbeLive=true;
    const auto evidence=di::preDispatch(descriptors,commands[1],8,8,1,di::DispatchKind::Direct,&facts,rp::sourceLayoutQuery(recordings),rm::sourceQuery(memoryObserver),&sourceFacts,demand);
    need(sourceFacts.observed&&evidence.metadataAccepted&&evidence.count==3&&!evidence.issues,"real accepted raw3 descriptor output");
    const auto inserted=probe::preDispatch(capture,sourceFacts);check(inserted,"real probe inline capture");need(inserted.commandsInserted,"probe issued inline source commands");
    key={device,facts.contextIncarnation,commands[1],facts.rpRecordingGeneration};need(key.contextIncarnation&&key.rpRecordingGeneration&&facts.beginUsageFlags==0,"actual flags0 common recording key");
    native<PFN_vkCmdDispatch>("vkCmdDispatch")(commands[1],8,8,1);
    const auto ended=native<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands[1]);rp::end(recordings,commands[1],ended);di::end(descriptors,commands[1],ended);vkcheck(ended,"end borrowed command");
    const auto status=probe::status(capture);actualFirstFence=status.ownFence;actualDrainFence=status.drainFence;
    need(actualFirstFence&&actualDrainFence&&actualFirstFence!=actualDrainFence,"two distinct own fences");
    std::printf("{\"event\":\"rawcolor_borrowed_recording\",\"device\":\"0x%llx\",\"command\":\"0x%llx\",\"contextIncarnation\":%llu,\"rpRecordingGeneration\":%llu,\"poolGeneration\":%llu,\"beginFlags\":0,\"commonRecordOrder\":%llu,\"descriptorRecordOrdinal\":%llu,\"rawBindings\":3,\"sourceInserted\":true,\"firstFence\":\"0x%llx\",\"drainFence\":\"0x%llx\",\"shaderReadsSources\":false}\n",
        static_cast<unsigned long long>(bits(device)),static_cast<unsigned long long>(bits(commands[1])),static_cast<unsigned long long>(key.contextIncarnation),static_cast<unsigned long long>(key.rpRecordingGeneration),
        static_cast<unsigned long long>(facts.poolGeneration),static_cast<unsigned long long>(facts.commonRecordOrder),static_cast<unsigned long long>(evidence.recordOrdinal),
        static_cast<unsigned long long>(bits(actualFirstFence)),static_cast<unsigned long long>(bits(actualDrainFence)));
}
void Fixture::submitBorrowed(){
    const std::uint64_t gateValue=1;const VkPipelineStageFlags stages=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};timeline.waitSemaphoreValueCount=1;timeline.pWaitSemaphoreValues=&gateValue;
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.pNext=&timeline;info.waitSemaphoreCount=1;info.pWaitSemaphores=&gate;info.pWaitDstStageMask=&stages;info.commandBufferCount=1;info.pCommandBuffers=&commands[1];
    std::array<unsigned char,sizeof(VkSubmitInfo)> saved{};std::array<unsigned char,sizeof(VkTimelineSemaphoreSubmitInfo)> savedTimeline{};
    std::memcpy(saved.data(),&info,sizeof(info));std::memcpy(savedTimeline.data(),&timeline,sizeof(timeline));
    const auto savedCommand=commands[1];const auto savedGate=gate;const auto savedFence=applicationFence;
    const completion::QueueWindow window{queue,family,true,true};const std::array<completion::RecordingKey,1> membership{key};
    const completion::SubmitObservation observation{window,membership,applicationFence,true};const auto ticket=probe::preSubmit(capture,observation);
    need(ticket.relevant&&ticket.associationKnown&&!ticket.reasons,"exact complete normalized membership");
    const auto rpTicket=rp::beginSubmit(recordings,queue,1,&info);mightPending=true;
    const auto original=native<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&info,applicationFence);++originalSubmits;
    const bool unchanged=std::memcmp(&info,saved.data(),sizeof(info))==0&&std::memcmp(&timeline,savedTimeline.data(),sizeof(timeline))==0&&commands[1]==savedCommand&&gate==savedGate&&applicationFence==savedFence;
    need(unchanged,"application original arguments unchanged");++originalCallsWithUnchangedArguments;
    // The probe's result cannot replace the literal application driver result.
    const auto extra=probe::afterOriginalResult(capture,ticket,original,window);rp::finishSubmit(recordings,rpTicket,applicationFence,original);vkcheck(original,"unchanged original app submit");
    need(extra.accepted()||(extra.reasons==probe::bit(probe::Reason::Pending)&&extra.driver==VK_NOT_READY),"private completion append");
    std::printf("{\"event\":\"rawcolor_application_submit\",\"ordinal\":%u,\"actualOriginalResult\":%d,\"originalCalls\":1,\"unchangedArguments\":true,\"waitStages\":2048,\"waitTimelineValue\":1,\"recordingGeneration\":%llu,\"privateReadbackSubmits\":%u}\n",
        originalSubmits,int(original),static_cast<unsigned long long>(key.rpRecordingGeneration),owned.privateReadbackSubmits);
}
void Fixture::verifyFirstStaging(std::uint32_t expected){
    const auto pixels=probe::pixels(capture);need(pixels.size()==width*height,"actual packed staging span after fence");
    for(std::size_t i=0;i<pixels.size();++i)if(pixels[i]!=expected)throw std::runtime_error("first staging packed122 mismatch at "+std::to_string(i)+" actual "+std::to_string(pixels[i]));
    verifiedPixels+=static_cast<unsigned>(pixels.size());
    std::printf("{\"event\":\"rawcolor_first_staging_verified\",\"phase\":\"%s\",\"format\":122,\"packedWord\":%u,\"pixels\":4096,\"allPixelsMatch\":true,\"bmpConversionVerified\":false}\n",firstReady?"after-replay":"first-readback",expected);
}
void Fixture::recordOracleB(VkImage image){
    begin(commands[2],VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);auto barrier=transition(image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    native<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands[2],VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
    native<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands[2],image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,oracle.handle,1,&copy);
    barrier=transition(image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.buffer=oracle.handle;host.offset=0;host.size=VK_WHOLE_SIZE;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    native<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands[2],VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,1,&barrier);
    vkcheck(native<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands[2]),"oracle end");VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&commands[2];
    mightPending=true;vkcheck(native<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&info,oracleFence),"oracle submit preceding final drain");
}
void Fixture::verifyOracleB(){
    void* mapped{};vkcheck(native<PFN_vkMapMemory>("vkMapMemory")(device,oracle.memory,0,VK_WHOLE_SIZE,0,&mapped),"map oracle after actual drain");
    try{if(!oracle.coherent){VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=oracle.memory;range.size=VK_WHOLE_SIZE;
        vkcheck(native<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device,1,&range),"oracle invalidate own entire mapped allocation");}
        const auto* words=static_cast<const std::uint32_t*>(mapped);for(std::size_t i=0;i<width*height;++i)need(words[i]==pixelB,"replayed owned image packedB");}
    catch(...){native<PFN_vkUnmapMemory>("vkUnmapMemory")(device,oracle.memory);throw;}
    native<PFN_vkUnmapMemory>("vkUnmapMemory")(device,oracle.memory);verifiedPixels+=width*height;ownedBAfterReplay=true;
    std::printf("{\"event\":\"rawcolor_owned_replay_verified\",\"format\":122,\"packedWord\":%u,\"pixels\":4096,\"allPixelsMatch\":true,\"actualDrainSucceededBeforeRead\":true}\n",pixelB);
}
void Fixture::run(){
    initialize();clear(true);recordBorrowed();pendingGate=true;submitBorrowed();const auto first=probe::status(capture);
    const auto pending=native<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,first.ownFence);const auto finite=native<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&first.ownFence,VK_TRUE,10'000'000);
    const auto notReady=probe::poll(capture);const auto pendingState=probe::status(capture);pendingNoPixels=probe::pixels(capture).empty();pendingNoFile=GetFileAttributesW(L"rawcolor-first.bmp")==INVALID_FILE_ATTRIBUTES;
    need(pending==VK_NOT_READY&&finite==VK_TIMEOUT&&notReady.driver==VK_NOT_READY&&notReady.reasons==probe::bit(probe::Reason::Pending)
        &&!pendingState.pixelsWritten&&!pendingState.completion.readbackReady&&pendingState.filesWritten==0&&pendingNoPixels&&pendingNoFile,"actual pending gate no host read/output");++readinessRefusals;
    std::printf("{\"event\":\"rawcolor_host_gate_pending\",\"hostGateValue\":0,\"ownFenceStatus\":%d,\"finiteWaitResult\":%d,\"pollDriver\":%d,\"pixelsSpanEmpty\":true,\"stagingReadPerformed\":false,\"bmpExists\":false,\"filesWritten\":0,\"persistentMappingAllowed\":true}\n",int(pending),int(finite),int(notReady.driver));
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};signal.semaphore=gate;signal.value=1;vkcheck(native<PFN_vkSignalSemaphore>("vkSignalSemaphore")(device,&signal),"release host timeline");pendingGate=false;
    vkcheck(native<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&first.ownFence,VK_TRUE,5'000'000'000),"actual first private readback completion");
    vkcheck(native<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&applicationFence,VK_TRUE,5'000'000'000),"actual original first submit completion");mightPending=false;
    check(probe::poll(capture),"actual completed first poll/output");const auto completed=probe::status(capture);
    need(completed.pixelsWritten&&completed.completion.ownFenceSuccessObserved&&completed.completion.readbackConsumed&&completed.filesWritten==1&&completed.stagingCopies==1,"actual private first ready/consumed");
    verifyFirstStaging(pixelA);firstReady=true;const auto image=completed.ownedImage;
    clear(false);vkcheck(native<PFN_vkResetFences>("vkResetFences")(device,1,&applicationFence),"reset only completed application fixture fence");submitBorrowed();
    vkcheck(native<PFN_vkWaitForFences>("vkWaitForFences")(device,1,&applicationFence,VK_TRUE,5'000'000'000),"actual serial replay completion");mightPending=false;
    const auto replay=probe::status(capture);need(replay.completion.successfulOriginalSubmissions==2&&replay.completion.replays==1&&replay.filesWritten==1&&replay.stagingCopies==1
        &&owned.privateReadbackSubmits==1&&owned.stagingCopyCommands==1,"flags0 replay never repeats private readback/staging write");verifyFirstStaging(pixelA);firstStableAfterReplay=true;
    const auto reset=native<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands[1],0);rp::reset(recordings,commands[1],reset);di::reset(descriptors,commands[1],reset);vkcheck(reset,"actual borrowed app retirement after completed replay");
    ++resetBorrowedByFixture;probe::recordingRetired(capture,key);retired=probe::status(capture).completion.recordingRetired;need(retired,"exact old recording retired");
    normalReleaseRetained=probe::status(capture).completion.release==completion::ReleaseDisposition::Retain;need(normalReleaseRetained,"first fence never proves final replay idle/release");
    // This extra GPU oracle uses the retained image AFTER replay and retirement.
    // Its submission must also precede, and be covered by, the final drain.
    recordOracleB(image);const auto drained=probe::beforeDeviceDestroy(capture,2'000'000'000);check(drained,"real final private drain");const auto shutdown=probe::status(capture);
    need(drained.driver==VK_SUCCESS&&shutdown.shutdownDriver==VK_SUCCESS&&shutdown.drainSubmitted&&shutdown.shutdownCleanupComplete&&!shutdown.shutdownQuarantined
        &&shutdown.released&&owned.drainSubmits==1&&owned.destroyedImages==owned.createdImages&&owned.destroyedBuffers==owned.createdBuffers&&owned.frees==owned.allocations
        &&owned.destroyedPools==owned.createdPools&&owned.destroyedFences==owned.createdFences,"actual separate drain then all own-child cleanup");
    drainComplete=true;mightPending=false;need(native<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device,oracleFence)==VK_SUCCESS,"drain covers last oracle GPU submission");verifyOracleB();
    std::printf("{\"event\":\"rawcolor_shutdown_drain_verified\",\"firstFence\":\"0x%llx\",\"drainFence\":\"0x%llx\",\"distinctFences\":true,\"actualDrainWaitResult\":0,\"oracleFenceStatus\":0,\"recordingRetired\":true,\"normalReleaseStillRetainedBeforeDrain\":true,\"queueIdleInvented\":false,\"probeOwnedChildrenClearedBeforeDeviceDestroy\":true,\"privateReadbackSubmits\":%u,\"drainSubmits\":%u}\n",
        static_cast<unsigned long long>(bits(actualFirstFence)),static_cast<unsigned long long>(bits(actualDrainFence)),owned.privateReadbackSubmits,owned.drainSubmits);
    need(cleanup(),"all fixture resource cleanup before native device destroy");
}
bool Fixture::cleanup() noexcept {
    if(cleanupDone)return !quarantined;
    try{
        if(device){
            if(pendingGate&&gate){VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};signal.semaphore=gate;signal.value=1;vkcheck(native<PFN_vkSignalSemaphore>("vkSignalSemaphore")(device,&signal),"error path release owned host gate");pendingGate=false;}
            if(capture){const auto result=probe::beforeDeviceDestroy(capture,2'000'000'000);if(!result.accepted()||probe::status(capture).shutdownQuarantined){quarantined=true;cleanupDone=true;return false;}
                if(probe::status(capture).sourceInserted)mightPending=false;}
            if(mightPending){quarantined=true;cleanupDone=true;return false;} // retain entire unknown GPU owner
            if(pool){rp::resetPool(recordings,pool,true,VK_SUCCESS);di::commandPoolDestroying(descriptors,pool);native<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device,pool,nullptr);pool=VK_NULL_HANDLE;}
            if(pipeline){di::pipelineDestroying(descriptors,pipeline);native<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device,pipeline,nullptr);pipeline=VK_NULL_HANDLE;}
            if(shader){native<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device,shader,nullptr);shader=VK_NULL_HANDLE;}
            if(pipelineLayout){di::pipelineLayoutDestroying(descriptors,pipelineLayout);native<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device,pipelineLayout,nullptr);pipelineLayout=VK_NULL_HANDLE;}
            if(descriptorPool){di::poolDestroying(descriptors,descriptorPool);native<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(device,descriptorPool,nullptr);descriptorPool=VK_NULL_HANDLE;set=VK_NULL_HANDLE;}
            if(setLayout){di::setLayoutDestroying(descriptors,setLayout);native<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(device,setLayout,nullptr);setLayout=VK_NULL_HANDLE;}
            if(oracle.handle){rm::bufferDestroying(memoryObserver,oracle.handle);native<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device,oracle.handle,nullptr);oracle.handle=VK_NULL_HANDLE;}
            if(oracle.memory){rm::memoryFreeing(memoryObserver,oracle.memory);native<PFN_vkFreeMemory>("vkFreeMemory")(device,oracle.memory,nullptr);oracle.memory=VK_NULL_HANDLE;}
            for(auto& image:images){if(image.view){di::viewDestroying(descriptors,image.view);rp::viewDestroying(recordings,image.view);native<PFN_vkDestroyImageView>("vkDestroyImageView")(device,image.view,nullptr);image.view=VK_NULL_HANDLE;}
                if(image.handle){rm::imageDestroying(memoryObserver,image.handle);rp::imageDestroying(recordings,image.handle);di::imageRetiring(descriptors,{image.inventory.image,image.inventory.generation});
                    inventory.destroying(bits(image.handle));native<PFN_vkDestroyImage>("vkDestroyImage")(device,image.handle,nullptr);image.handle=VK_NULL_HANDLE;image.inventory={};}
                if(image.memory){rm::memoryFreeing(memoryObserver,image.memory);native<PFN_vkFreeMemory>("vkFreeMemory")(device,image.memory,nullptr);image.memory=VK_NULL_HANDLE;}}
            for(auto* fence:{&initializationFence,&applicationFence,&oracleFence})if(*fence){native<PFN_vkDestroyFence>("vkDestroyFence")(device,*fence,nullptr);*fence=VK_NULL_HANDLE;}
            if(gate){native<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device,gate,nullptr);gate=VK_NULL_HANDLE;}
            native<PFN_vkDestroyDevice>("vkDestroyDevice")(device,nullptr);device=VK_NULL_HANDLE;
            if(capture){probe::deviceTerminated(capture);need(probe::destroy(capture),"CPU probe release after actual native device destruction");capture=nullptr;}
        }
        if(descriptors){di::destroy(descriptors);descriptors=nullptr;}if(recordings){rp::destroy(recordings);recordings=nullptr;}if(memoryObserver){rm::destroy(memoryObserver);memoryObserver=nullptr;}
        if(instance){instanceFunction<PFN_vkDestroyInstance>("vkDestroyInstance")(instance,nullptr);instance=VK_NULL_HANDLE;}if(loader){FreeLibrary(loader);loader=nullptr;}
        cleanupDone=true;active=nullptr;return true;
    }catch(...){quarantined=true;cleanupDone=true;return false;}
}
VKAPI_ATTR VkResult VKAPI_CALL ownCreateImage(VkDevice d,const VkImageCreateInfo* i,const VkAllocationCallbacks* a,VkImage* o){const auto r=active->native<PFN_vkCreateImage>("vkCreateImage")(d,i,a,o);if(r==VK_SUCCESS)++active->owned.createdImages;return r;}
VKAPI_ATTR void VKAPI_CALL ownDestroyImage(VkDevice d,VkImage i,const VkAllocationCallbacks* a){active->native<PFN_vkDestroyImage>("vkDestroyImage")(d,i,a);++active->owned.destroyedImages;}
VKAPI_ATTR VkResult VKAPI_CALL ownCreateBuffer(VkDevice d,const VkBufferCreateInfo* i,const VkAllocationCallbacks* a,VkBuffer* o){const auto r=active->native<PFN_vkCreateBuffer>("vkCreateBuffer")(d,i,a,o);if(r==VK_SUCCESS)++active->owned.createdBuffers;return r;}
VKAPI_ATTR void VKAPI_CALL ownDestroyBuffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks* a){active->native<PFN_vkDestroyBuffer>("vkDestroyBuffer")(d,b,a);++active->owned.destroyedBuffers;}
VKAPI_ATTR VkResult VKAPI_CALL ownAllocate(VkDevice d,const VkMemoryAllocateInfo* i,const VkAllocationCallbacks* a,VkDeviceMemory* o){const auto r=active->native<PFN_vkAllocateMemory>("vkAllocateMemory")(d,i,a,o);if(r==VK_SUCCESS)++active->owned.allocations;return r;}
VKAPI_ATTR void VKAPI_CALL ownFree(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks* a){active->native<PFN_vkFreeMemory>("vkFreeMemory")(d,m,a);++active->owned.frees;}
VKAPI_ATTR VkResult VKAPI_CALL ownCreatePool(VkDevice d,const VkCommandPoolCreateInfo* i,const VkAllocationCallbacks* a,VkCommandPool* o){const auto r=active->native<PFN_vkCreateCommandPool>("vkCreateCommandPool")(d,i,a,o);if(r==VK_SUCCESS)++active->owned.createdPools;return r;}
VKAPI_ATTR void VKAPI_CALL ownDestroyPool(VkDevice d,VkCommandPool p,const VkAllocationCallbacks* a){active->native<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(d,p,a);++active->owned.destroyedPools;}
VKAPI_ATTR VkResult VKAPI_CALL ownCreateFence(VkDevice d,const VkFenceCreateInfo* i,const VkAllocationCallbacks* a,VkFence* o){const auto r=active->native<PFN_vkCreateFence>("vkCreateFence")(d,i,a,o);if(r==VK_SUCCESS){if(active->owned.createdFences<2)active->owned.fences[active->owned.createdFences]=*o;++active->owned.createdFences;}return r;}
VKAPI_ATTR void VKAPI_CALL ownDestroyFence(VkDevice d,VkFence f,const VkAllocationCallbacks* a){active->native<PFN_vkDestroyFence>("vkDestroyFence")(d,f,a);++active->owned.destroyedFences;}
VKAPI_ATTR VkResult VKAPI_CALL ownSubmit(VkQueue q,std::uint32_t count,const VkSubmitInfo* infos,VkFence fence){
    need(count==1&&infos&&infos[0].sType==VK_STRUCTURE_TYPE_SUBMIT_INFO,"probe native submit shape");
    if(fence==active->owned.fences[0]){need(infos[0].commandBufferCount==1&&!infos[0].waitSemaphoreCount&&!infos[0].signalSemaphoreCount,"private readback shape");++active->owned.privateReadbackSubmits;}
    else if(fence==active->owned.fences[1]){need(infos[0].commandBufferCount==0&&!infos[0].waitSemaphoreCount&&!infos[0].signalSemaphoreCount,"final drain shape");++active->owned.drainSubmits;}
    else throw std::runtime_error("unexpected probe own submission fence");return active->native<PFN_vkQueueSubmit>("vkQueueSubmit")(q,count,infos,fence);
}
VKAPI_ATTR void VKAPI_CALL ownCopyToBuffer(VkCommandBuffer c,VkImage image,VkImageLayout layout,VkBuffer buffer,std::uint32_t count,const VkBufferImageCopy* copies){
    ++active->owned.stagingCopyCommands;active->native<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(c,image,layout,buffer,count,copies);
}
} // namespace

int main(){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);std::setvbuf(stdout,nullptr,_IONBF,0);Fixture fixture;
    try{fixture.run();std::printf("{\"stage\":\"rawcolor-capture-probe\",\"passed\":true,\"selfOwnedSourcePixelsCaptured\":true,\"actualProbeApiUsed\":true,\"flags0SerialReplayTested\":true,\"firstPackedStagingA\":true,\"firstStagingAAfterReplay\":true,\"ownedImageBAfterReplay\":true,\"pendingHostReadRefused\":true,\"pendingBmpAbsent\":true,\"separateShutdownDrainSucceeded\":true,\"normalReplayReleaseNotFaked\":true,\"borrowedRecordingRetiredByFixture\":true,\"applicationSubmits\":%u,\"originalUnchangedCalls\":%u,\"privateReadbackSubmits\":%u,\"stagingCopyCommands\":%u,\"drainSubmits\":%u,\"mappedPackedPixelsVerified\":%u,\"probeOwnImagesCreated\":%u,\"probeOwnImagesDestroyed\":%u,\"probeOwnBuffersCreated\":%u,\"probeOwnBuffersDestroyed\":%u,\"probeOwnMemoryAllocations\":%u,\"probeOwnMemoryFrees\":%u,\"probeOwnPoolsCreated\":%u,\"probeOwnPoolsDestroyed\":%u,\"probeOwnFencesCreated\":%u,\"probeOwnFencesDestroyed\":%u,\"cleanupSucceeded\":true,\"shutdownQuarantined\":false,\"bmpConversionVerified\":false,\"gameTested\":false,\"productionLinked\":false,\"gamePixelsCaptured\":false,\"xefgIntegrated\":false,\"gameSameFrameVerified\":false,\"motionSemanticsVerified\":false,\"depthConverted\":false,\"installedPayloadChanged\":false}\n",
        fixture.originalSubmits,fixture.originalCallsWithUnchangedArguments,fixture.owned.privateReadbackSubmits,fixture.owned.stagingCopyCommands,fixture.owned.drainSubmits,fixture.verifiedPixels,
        fixture.owned.createdImages,fixture.owned.destroyedImages,fixture.owned.createdBuffers,fixture.owned.destroyedBuffers,fixture.owned.allocations,fixture.owned.frees,
        fixture.owned.createdPools,fixture.owned.destroyedPools,fixture.owned.createdFences,fixture.owned.destroyedFences);return 0;}
    catch(const std::exception& error){const bool cleaned=fixture.cleanup();std::fprintf(stderr,"rawcolor replay fixture failed: %s\n",error.what());
        std::printf("{\"stage\":\"rawcolor-capture-probe\",\"passed\":false,\"cleanupSucceeded\":%s,\"shutdownQuarantined\":%s,\"gameTested\":false,\"gamePixelsCaptured\":false,\"xefgIntegrated\":false,\"installedPayloadChanged\":false}\n",cleaned?"true":"false",fixture.quarantined?"true":"false");return 1;}
}
