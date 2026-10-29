#include "fsr4_bridge.hpp"
#include "fsr4_xmx.hpp"
#include "fsr4_command_map.hpp"
#include "sr_interop_submit.hpp"
#include "depth_snapshot_spirv.hpp"
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vulkan/vulkan_win32.h>
#include <array>
#include <cstring>
#include <mutex>
#include <span>

namespace xess_vk::fsr4 {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT result,const char* operation){if(FAILED(result))throw std::runtime_error(std::string(operation)+": HRESULT "+std::to_string(static_cast<unsigned>(result)));}
void check(VkResult result,const char* operation){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": VkResult "+std::to_string(result));}
template<class T>T handle(std::uint64_t n){return reinterpret_cast<T>(static_cast<std::uintptr_t>(n));}
bool same(const sr_overlay::Configuration& a,const sr_overlay::Configuration& b){return a.input.width==b.input.width&&a.input.height==b.input.height&&
    a.output.width==b.output.width&&a.output.height==b.output.height&&a.invertedDepth==b.invertedDepth&&a.jitteredMotion==b.jitteredMotion;}
VkImageMemoryBarrier imageBarrier(VkImage image,VkImageSubresourceRange range,VkImageLayout before,VkImageLayout after,
                                  unsigned from=VK_QUEUE_FAMILY_IGNORED,unsigned to=VK_QUEUE_FAMILY_IGNORED){
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask=b.srcAccessMask;b.oldLayout=before;b.newLayout=after;b.srcQueueFamilyIndex=from;b.dstQueueFamilyIndex=to;b.image=image;b.subresourceRange=range;
    if(before==VK_IMAGE_LAYOUT_UNDEFINED)b.srcAccessMask=0;return b;
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};return b;
}
constexpr VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
}
struct Bridge::Impl {
    VulkanContext vk;std::filesystem::path payload;mutable std::recursive_mutex mutex;
    std::mutex submissionMutex;CommandMap mappings;
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    VkCommandPool commandPool{};VkDescriptorSetLayout depthLayout{};VkPipelineLayout depthPipelineLayout{};VkPipeline depthPipeline{};
    std::shared_ptr<Context> active;sr_overlay::Configuration configuration{};bool configured{};
    std::string error;std::atomic<std::uint64_t> submitted{};
    struct Image {ComPtr<ID3D12Resource> resource;VkImage image{};VkDeviceMemory memory{};VkImageView view{};VkExtent2D extent{};};
    struct Slot {
        sr_overlay::RecordingKey key{};std::uint64_t sourcePool{};bool mapped{},submitted{},initialized{},submitting{};
        std::uint64_t ready=1,done=2,retired=3;
        std::array<Image,4> images;VkCommandBuffer tail{};VkSemaphore semaphore{};
        VkDescriptorPool descriptorPool{};VkDescriptorSet depthSet{};
        ComPtr<ID3D12Fence> fence;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
        std::shared_ptr<Context> context;
    };
    std::vector<std::unique_ptr<Slot>> slots;
    template<class T>T fn(const char* name)const{auto p=reinterpret_cast<T>(vk.gdpa(vk.device,name));if(!p&&std::strcmp(name,"vkQueueSubmit2")==0)p=reinterpret_cast<T>(vk.gdpa(vk.device,"vkQueueSubmit2KHR"));if(!p)throw std::runtime_error(std::string("Missing Vulkan entry: ")+name);return p;}
    template<class T>T instance(const char* name)const{const auto p=reinterpret_cast<T>(vk.gipa(vk.instance,name));if(!p)throw std::runtime_error(std::string("Missing Vulkan instance entry: ")+name);return p;}
    void setError(const char* message)noexcept{try{std::lock_guard lock(mutex);error=message;}catch(...) {}}
    void barriers(VkCommandBuffer command,std::span<const VkImageMemoryBarrier> b){if(!b.empty())fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,unsigned(b.size()),b.data());}
    Impl(VulkanContext context,std::filesystem::path path):vk(context),payload(std::move(path)){
        if(!vk.instance||!vk.physical||!vk.device||!vk.gipa||!vk.gdpa||vk.queueFamily==VK_QUEUE_FAMILY_IGNORED)throw std::invalid_argument("Incomplete FSR4 Vulkan context");
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&id;
        instance<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(vk.physical,&properties);
        if(!id.deviceLUIDValid||properties.properties.vendorID!=0x8086)throw std::runtime_error("FSR4 XMX needs an Intel adapter with a valid LUID");
        LUID luid{};std::memcpy(&luid,id.deviceLUID,sizeof(luid));ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;
        check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"Create FSR4 DXGI factory");check(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)),"Match FSR4 adapter LUID");
        check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"Create FSR4 D3D12 device");
        D3D12_COMMAND_QUEUE_DESC desc{};check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"Create FSR4 D3D12 queue");
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=vk.queueFamily;pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(vk.device,&pool,nullptr,&commandPool),"Create FSR4 Vulkan continuation pool");
        try{createDepthPipeline();}catch(...){destroy();throw;}
    }
    void createDepthPipeline(){
        const std::array<VkDescriptorSetLayoutBinding,2> bindings{{{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=bindings.size();layout.pBindings=bindings.data();
        check(fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(vk.device,&layout,nullptr,&depthLayout),"Create FSR4 depth descriptor layout");
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipelineLayout.setLayoutCount=1;pipelineLayout.pSetLayouts=&depthLayout;
        check(fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(vk.device,&pipelineLayout,nullptr,&depthPipelineLayout),"Create FSR4 depth pipeline layout");
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=sizeof(continuous::depthSnapshotSpirv);shader.pCode=continuous::depthSnapshotSpirv;VkShaderModule module{};
        check(fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(vk.device,&shader,nullptr,&module),"Create FSR4 depth shader");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline.layout=depthPipelineLayout;
        pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
        const auto result=fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(vk.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&depthPipeline);
        fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(vk.device,module,nullptr);check(result,"Create FSR4 depth pipeline");
    }
    void destroySlot(Slot& s)noexcept{
        try{
            if(s.descriptorPool)fn<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(vk.device,s.descriptorPool,nullptr);
            if(s.tail)fn<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(vk.device,commandPool,1,&s.tail);
            if(s.semaphore)fn<PFN_vkDestroySemaphore>("vkDestroySemaphore")(vk.device,s.semaphore,nullptr);
            for(auto& image:s.images){if(image.view)fn<PFN_vkDestroyImageView>("vkDestroyImageView")(vk.device,image.view,nullptr);
                if(image.image)fn<PFN_vkDestroyImage>("vkDestroyImage")(vk.device,image.image,nullptr);
                if(image.memory)fn<PFN_vkFreeMemory>("vkFreeMemory")(vk.device,image.memory,nullptr);}
        }catch(...){}
    }
    void destroy()noexcept{
        for(auto& s:slots)destroySlot(*s);slots.clear();
        try{if(depthPipeline)fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(vk.device,depthPipeline,nullptr);
            if(depthPipelineLayout)fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(vk.device,depthPipelineLayout,nullptr);
            if(depthLayout)fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(vk.device,depthLayout,nullptr);
            if(commandPool)fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(vk.device,commandPool,nullptr);}catch(...){}
        depthPipeline={};depthPipelineLayout={};depthLayout={};commandPool={};active.reset();
    }
    ~Impl(){destroy();}
    bool gpuPending()const noexcept{for(const auto& s:slots)if(s->submitting||(s->submitted&&s->fence->GetCompletedValue()<s->retired))return true;return false;}
    unsigned memoryType(unsigned bits){VkPhysicalDeviceMemoryProperties p{};instance<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(vk.physical,&p);
        for(unsigned n=0;n<p.memoryTypeCount;++n)if((bits&(1u<<n))&&(p.memoryTypes[n].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return n;
        throw std::runtime_error("No compatible FSR4 shared memory type");}
    void createImage(Image& image,VkExtent2D extent,VkFormat format,DXGI_FORMAT dxFormat){
        image.extent=extent;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=extent.width;desc.Height=extent.height;desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=dxFormat;desc.SampleDesc.Count=1;
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&image.resource)),"Create FSR4 shared texture");
        HANDLE shared{};check(device->CreateSharedHandle(image.resource.Get(),nullptr,GENERIC_ALL,nullptr,&shared),"Export FSR4 texture");
        struct Close {HANDLE value;~Close(){if(value)CloseHandle(value);}} close{shared};
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.pNext=&external;info.imageType=VK_IMAGE_TYPE_2D;info.format=format;info.extent={extent.width,extent.height,1};
        info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
        check(fn<PFN_vkCreateImage>("vkCreateImage")(vk.device,&info,nullptr,&image.image),"Import FSR4 shared image");
        VkMemoryRequirements requirements{};fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(vk.device,image.image,&requirements);
        VkMemoryWin32HandlePropertiesKHR properties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        check(fn<PFN_vkGetMemoryWin32HandlePropertiesKHR>("vkGetMemoryWin32HandlePropertiesKHR")(vk.device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,shared,&properties),"FSR4 shared memory properties");
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=image.image;
        VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};import.pNext=&dedicated;import.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;import.handle=shared;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.pNext=&import;allocate.allocationSize=requirements.size;allocate.memoryTypeIndex=memoryType(requirements.memoryTypeBits&properties.memoryTypeBits);
        check(fn<PFN_vkAllocateMemory>("vkAllocateMemory")(vk.device,&allocate,nullptr,&image.memory),"Import FSR4 shared memory");
        check(fn<PFN_vkBindImageMemory>("vkBindImageMemory")(vk.device,image.image,image.memory,0),"Bind FSR4 shared image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=format;view.subresourceRange=colorRange;
        check(fn<PFN_vkCreateImageView>("vkCreateImageView")(vk.device,&view,nullptr,&image.view),"Create FSR4 shared view");
    }
    Slot& allocateSlot(){
        auto slot=std::make_unique<Slot>();
        try{
            const std::array vkFormats{VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT};
            const std::array dxFormats{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
            for(unsigned n=0;n<4;++n)createImage(slot->images[n],n==3?configuration.output:configuration.input,vkFormats[n],dxFormats[n]);
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot->allocator)),"Create FSR4 allocator");
            check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot->allocator.Get(),nullptr,IID_PPV_ARGS(&slot->commands)),"Create FSR4 command list");check(slot->commands->Close(),"Close new FSR4 command list");
            check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&slot->fence)),"Create FSR4 shared fence");
            HANDLE shared{};check(device->CreateSharedHandle(slot->fence.Get(),nullptr,GENERIC_ALL,nullptr,&shared),"Export FSR4 shared fence");
            struct Close {HANDLE value;~Close(){CloseHandle(value);}} close{shared};
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};semaphore.pNext=&type;
            check(fn<PFN_vkCreateSemaphore>("vkCreateSemaphore")(vk.device,&semaphore,nullptr,&slot->semaphore),"Create FSR4 timeline");
            VkImportSemaphoreWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};import.semaphore=slot->semaphore;import.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;import.handle=shared;
            check(fn<PFN_vkImportSemaphoreWin32HandleKHR>("vkImportSemaphoreWin32HandleKHR")(vk.device,&import),"Import FSR4 shared fence");
            VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};commands.commandPool=commandPool;commands.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commands.commandBufferCount=1;
            check(fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(vk.device,&commands,&slot->tail),"Allocate FSR4 continuation");
            const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}}};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=sizes.size();pool.pPoolSizes=sizes.data();
            check(fn<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(vk.device,&pool,nullptr,&slot->descriptorPool),"Create FSR4 depth descriptor pool");
            VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sets.descriptorPool=slot->descriptorPool;sets.descriptorSetCount=1;sets.pSetLayouts=&depthLayout;
            check(fn<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(vk.device,&sets,&slot->depthSet),"Allocate FSR4 depth descriptor");
        }catch(...){destroySlot(*slot);throw;}
        slots.push_back(std::move(slot));return *slots.back();
    }
    Slot& acquire(){
        for(auto i=slots.begin();i!=slots.end();) {
            auto& s=**i;const bool obsolete=s.images[0].extent.width!=configuration.input.width||s.images[0].extent.height!=configuration.input.height||
                s.images[3].extent.width!=configuration.output.width||s.images[3].extent.height!=configuration.output.height;
            if(obsolete&&!s.mapped&&!s.submitting&&(!s.submitted||s.fence->GetCompletedValue()>=s.retired)){destroySlot(s);i=slots.erase(i);}else ++i;
        }
        for(auto& s:slots)if(!s->mapped&&!s->submitting&&(!s->submitted||s->fence->GetCompletedValue()>=s->retired)&&
            s->images[0].extent.width==configuration.input.width&&s->images[0].extent.height==configuration.input.height&&
            s->images[3].extent.width==configuration.output.width&&s->images[3].extent.height==configuration.output.height){
            if(s->submitted){s->ready+=3;s->done+=3;s->retired+=3;}s->submitted=false;return *s;}
        return allocateSlot();
    }
    Slot* find(VkCommandBuffer command)const noexcept{for(auto& s:slots)if(s->mapped&&s->key.command==reinterpret_cast<std::uintptr_t>(command))return s.get();return nullptr;}
    void configure(const sr_overlay::Configuration& c){
        if(configured&&same(c,configuration))return;
        auto next=std::make_shared<Context>(payload);next->initialize(device.Get(),{c.input.width,c.input.height,c.output.width,c.output.height,c.invertedDepth,c.jitteredMotion});
        active=std::move(next);configuration=c;configured=true;
    }
    void record(const sr_overlay::Frame& f,bool reset){
        if(!configured||!same(f.configuration,configuration))throw std::logic_error("FSR4 dimensions need initialization");
        const auto original=handle<VkCommandBuffer>(f.owner.command);
        if(find(original)||f.recording.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY||!f.recording.beginFlagsKnown||
           !(f.recording.beginUsageFlags&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)||
           (f.recording.beginUsageFlags&VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)||f.recording.inRenderPass||f.recording.inDynamicRendering)
            throw std::invalid_argument("FSR4 requires one primary, outside-render-pass boundary per recording");
        if(!(f.color.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)||!(f.motion.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)||!(f.depth.usage&VK_IMAGE_USAGE_SAMPLED_BIT)||!(f.nativeOutput.usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw std::invalid_argument("FSR4 source transfer/sample usages are missing");
        const auto mapping=mappings.reserve();
        if(mapping==CommandMap::capacity)throw std::runtime_error("FSR4 pending command mapping capacity exhausted");
        auto& s=acquire();s.context=active;
        check(s.allocator->Reset(),"Reset FSR4 allocator");check(s.commands->Reset(s.allocator.Get(),nullptr),"Reset FSR4 D3D12 list");
        std::array<D3D12_RESOURCE_BARRIER,4> before{},after{};
        for(unsigned n=0;n<4;++n){const auto state=n==3?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            before[n]=transition(s.images[n].resource.Get(),D3D12_RESOURCE_STATE_COMMON,state);after[n]=transition(s.images[n].resource.Get(),state,D3D12_RESOURCE_STATE_COMMON);}
        s.commands->ResourceBarrier(before.size(),before.data());
        Frame frame;frame.commands=s.commands.Get();frame.color=s.images[0].resource.Get();frame.depth=s.images[1].resource.Get();frame.motion=s.images[2].resource.Get();frame.output=s.images[3].resource.Get();
        frame.jitterX=f.jitterX;frame.jitterY=f.jitterY;frame.velocityScaleX=f.configuration.velocityScaleX;frame.velocityScaleY=f.configuration.velocityScaleY;frame.reset=reset;
        frame.frameTimeMs=f.frameTimeMs;frame.cameraNear=f.cameraNear;frame.cameraFar=f.cameraFar;frame.cameraFov=f.cameraFov;
        s.context->record(frame);s.commands->ResourceBarrier(after.size(),after.data());check(s.commands->Close(),"Close FSR4 D3D12 list");
        check(fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(s.tail,0),"Reset FSR4 continuation");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(s.tail,&begin),"Begin FSR4 continuation");
        const std::array outBefore{
            imageBarrier(s.images[3].image,colorRange,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_EXTERNAL,vk.queueFamily),
            imageBarrier(f.nativeOutput.sdk.image,f.nativeOutput.barrierRange,f.nativeOutput.currentLayout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
        barriers(s.tail,outBefore);VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.extent={configuration.output.width,configuration.output.height,1};
        fn<PFN_vkCmdCopyImage>("vkCmdCopyImage")(s.tail,s.images[3].image,VK_IMAGE_LAYOUT_GENERAL,f.nativeOutput.sdk.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        const std::array outAfter{
            imageBarrier(s.images[3].image,colorRange,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,vk.queueFamily,VK_QUEUE_FAMILY_EXTERNAL),
            imageBarrier(f.nativeOutput.sdk.image,f.nativeOutput.barrierRange,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,f.nativeOutput.currentLayout)};
        barriers(s.tail,outAfter);
        if(f.restore.pipeline)fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(s.tail,VK_PIPELINE_BIND_POINT_COMPUTE,f.restore.pipeline);
        if(f.restore.setCount)fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(s.tail,VK_PIPELINE_BIND_POINT_COMPUTE,f.restore.layout,0,f.restore.setCount,f.restore.sets.data(),0,nullptr);
        for(unsigned n=0;n<f.restore.pushCount;++n){const auto& p=f.restore.push[n];fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(s.tail,p.layout,p.stages,p.offset,p.size,p.bytes.data());}
        const std::array<VkDescriptorImageInfo,2> descriptors{{{VK_NULL_HANDLE,f.depth.sdk.imageView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,s.images[1].view,VK_IMAGE_LAYOUT_GENERAL}}};
        std::array<VkWriteDescriptorSet,2> writes{};for(unsigned n=0;n<2;++n){auto& w=writes[n];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=s.depthSet;w.dstBinding=n;w.descriptorCount=1;w.descriptorType=n?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=&descriptors[n];}
        fn<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(vk.device,writes.size(),writes.data(),0,nullptr);
        std::vector<VkImageMemoryBarrier> inputBefore;inputBefore.reserve(7);
        for(const auto& image:s.images)inputBefore.push_back(imageBarrier(image.image,colorRange,s.initialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_EXTERNAL,vk.queueFamily));
        inputBefore.push_back(imageBarrier(f.color.sdk.image,f.color.barrierRange,f.color.currentLayout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
        inputBefore.push_back(imageBarrier(f.motion.sdk.image,f.motion.barrierRange,f.motion.currentLayout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
        inputBefore.push_back(imageBarrier(f.depth.sdk.image,f.depth.barrierRange,f.depth.currentLayout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        barriers(original,inputBefore);
        VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.srcOffsets[1]=blit.dstOffsets[1]={int(configuration.input.width),int(configuration.input.height),1};
        fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(original,f.color.sdk.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.images[0].image,VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_NEAREST);
        copy.extent={configuration.input.width,configuration.input.height,1};
        fn<PFN_vkCmdCopyImage>("vkCmdCopyImage")(original,f.motion.sdk.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.images[2].image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(original,VK_PIPELINE_BIND_POINT_COMPUTE,depthPipeline);
        fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(original,VK_PIPELINE_BIND_POINT_COMPUTE,depthPipelineLayout,0,1,&s.depthSet,0,nullptr);
        fn<PFN_vkCmdDispatch>("vkCmdDispatch")(original,(configuration.input.width+7)/8,(configuration.input.height+7)/8,1);
        std::vector<VkImageMemoryBarrier> inputAfter;inputAfter.reserve(7);
        for(const auto& image:s.images)inputAfter.push_back(imageBarrier(image.image,colorRange,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,vk.queueFamily,VK_QUEUE_FAMILY_EXTERNAL));
        inputAfter.push_back(imageBarrier(f.color.sdk.image,f.color.barrierRange,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,f.color.currentLayout));
        inputAfter.push_back(imageBarrier(f.motion.sdk.image,f.motion.barrierRange,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,f.motion.currentLayout));
        inputAfter.push_back(imageBarrier(f.depth.sdk.image,f.depth.barrierRange,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,f.depth.currentLayout));barriers(original,inputAfter);
        check(fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(original),"End FSR4 input prefix");
        s.key={f.owner.device,f.owner.incarnation,f.owner.command,f.owner.recording};s.sourcePool=f.recording.pool;s.mapped=true;
        mappings.publish(mapping,s.key,s.tail);
    }
    template<class Submit>VkResult passthrough(VkQueue target,unsigned count,const Submit* infos,VkFence fence){
        if constexpr(std::is_same_v<Submit,VkSubmitInfo>)return fn<PFN_vkQueueSubmit>("vkQueueSubmit")(target,count,infos,fence);
        else return fn<PFN_vkQueueSubmit2>("vkQueueSubmit2")(target,count,infos,fence);
    }
    template<class Submit>VkResult submit(VkQueue target,unsigned count,const Submit* infos,VkFence fence){
        if(mappings.empty())return passthrough(target,count,infos,fence);
        std::unique_lock metadata(mutex);
        std::vector<sr_interop::Handoff> handoffs;std::vector<Slot*> jobs;
        for(auto& s:slots)if(s->mapped&&!s->submitted&&!s->submitting){handoffs.push_back({handle<VkCommandBuffer>(s->key.command),s->tail,s->semaphore,s->ready,s->done,s->retired});jobs.push_back(s.get());}
        bool matched=false;for(unsigned n=0;n<count;++n){
            if constexpr(std::is_same_v<Submit,VkSubmitInfo>){for(unsigned i=0;i<infos[n].commandBufferCount;++i)if(auto* slot=find(infos[n].pCommandBuffers[i])){if(slot->submitted||slot->submitting)throw std::invalid_argument("Duplicate SR command submission");matched=true;}}
            else for(unsigned i=0;i<infos[n].commandBufferInfoCount;++i)if(auto* slot=find(infos[n].pCommandBufferInfos[i].commandBuffer)){if(slot->submitted||slot->submitting)throw std::invalid_argument("Duplicate SR command submission");matched=true;}}
        if(!matched){metadata.unlock();return passthrough(target,count,infos,fence);}
        const auto batches=sr_interop::normalize(std::span(infos,count));const auto segments=sr_interop::plan(batches,handoffs);
        struct Prepared {
            std::vector<VkSemaphore> waits,signals;std::vector<std::uint64_t> waitValues,signalValues;
            std::vector<VkPipelineStageFlags> stages;std::vector<VkCommandBuffer> commands;
            std::vector<VkSemaphoreSubmitInfo> waits2,signals2;std::vector<VkCommandBufferSubmitInfo> commands2;
        };
        // All host allocations and input validation precede the first queue operation.
        std::vector<Prepared> prepared(segments.size());
        for(std::size_t n=0;n<segments.size();++n){auto& p=prepared[n];const auto& segment=segments[n];
            if constexpr(std::is_same_v<Submit,VkSubmitInfo>){
                for(const auto& w:segment.waits){p.waits.push_back(w.semaphore);p.waitValues.push_back(w.value);p.stages.push_back(VkPipelineStageFlags(w.stage));}
                for(const auto& signal:segment.signals){p.signals.push_back(signal.semaphore);p.signalValues.push_back(signal.value);}for(const auto& c:segment.commands)p.commands.push_back(c.buffer);
            }else{
                for(const auto& w:segment.waits)p.waits2.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,nullptr,w.semaphore,w.value,w.stage,0});
                for(const auto& signal:segment.signals)p.signals2.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,nullptr,signal.semaphore,signal.value,signal.stage,0});
                for(const auto& c:segment.commands)p.commands2.push_back({VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,nullptr,c.buffer,c.mask});
            }
        }
        const auto legacy=fn<PFN_vkQueueSubmit>("vkQueueSubmit");PFN_vkQueueSubmit2 modern{};
        if constexpr(!std::is_same_v<Submit,VkSubmitInfo>)modern=fn<PFN_vkQueueSubmit2>("vkQueueSubmit2");
        std::vector<Slot*> reserved;reserved.reserve(segments.size());
        for(const auto& segment:segments)if(segment.runAfter!=SIZE_MAX)reserved.push_back(jobs[segment.runAfter]);
        for(auto* slot:reserved)slot->submitting=true;
        struct Reservations {Impl& owner;std::span<Slot*> slots;~Reservations(){std::lock_guard lock(owner.mutex);for(auto* s:slots)s->submitting=false;}} reservations{*this,reserved};
        metadata.unlock();
        // Serialize only SR handoffs sharing the D3D12 queue. Recording threads
        // and unrelated Vulkan submissions never wait on this execution lock.
        std::lock_guard execution(submissionMutex);
        for(std::size_t index=0;index<segments.size();++index){const auto& segment=segments[index];const auto& p=prepared[index];VkResult result;
            if constexpr(std::is_same_v<Submit,VkSubmitInfo>){
                VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};timeline.waitSemaphoreValueCount=p.waitValues.size();timeline.pWaitSemaphoreValues=p.waitValues.data();timeline.signalSemaphoreValueCount=p.signalValues.size();timeline.pSignalSemaphoreValues=p.signalValues.data();
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&timeline;submit.waitSemaphoreCount=p.waits.size();submit.pWaitSemaphores=p.waits.data();submit.pWaitDstStageMask=p.stages.data();submit.commandBufferCount=p.commands.size();submit.pCommandBuffers=p.commands.data();submit.signalSemaphoreCount=p.signals.size();submit.pSignalSemaphores=p.signals.data();
                result=legacy(target,1,&submit,index+1==segments.size()?fence:VK_NULL_HANDLE);
            }else{
                VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};submit.waitSemaphoreInfoCount=p.waits2.size();submit.pWaitSemaphoreInfos=p.waits2.data();submit.commandBufferInfoCount=p.commands2.size();submit.pCommandBufferInfos=p.commands2.data();submit.signalSemaphoreInfoCount=p.signals2.size();submit.pSignalSemaphoreInfos=p.signals2.data();
                result=modern(target,1,&submit,index+1==segments.size()?fence:VK_NULL_HANDLE);
            }
            if(result!=VK_SUCCESS)return result;
            if(segment.runAfter!=SIZE_MAX){auto& slot=*jobs[segment.runAfter];
                {std::lock_guard lock(mutex);slot.submitted=true;slot.initialized=true;}
                check(queue->Wait(slot.fence.Get(),slot.ready),"Queue FSR4 input wait");ID3D12CommandList* list[]{slot.commands.Get()};queue->ExecuteCommandLists(1,list);
                check(queue->Signal(slot.fence.Get(),slot.done),"Signal FSR4 output completion");submitted.fetch_add(1,std::memory_order_relaxed);}
        }
        return VK_SUCCESS;
    }
};
Bridge::Bridge(VulkanContext context,const std::filesystem::path& payload):impl_(std::make_unique<Impl>(context,payload)){}
Bridge::~Bridge(){if(impl_&&impl_->gpuPending())(void)impl_.release();}
void Bridge::deactivate()noexcept{std::lock_guard lock(impl_->mutex);impl_->active.reset();impl_->configured=false;}
void Bridge::configure(const sr_overlay::Configuration& c){std::lock_guard lock(impl_->mutex);impl_->configure(c);}
void Bridge::record(const sr_overlay::Frame& f,bool reset){std::lock_guard lock(impl_->mutex);impl_->record(f,reset);}
VkCommandBuffer Bridge::command(VkCommandBuffer original)const noexcept{return impl_->mappings.command(original);}
bool Bridge::contains(sr_overlay::RecordingKey key)const noexcept{std::lock_guard lock(impl_->mutex);const auto* s=impl_->find(handle<VkCommandBuffer>(key.command));return s&&s->key==key;}
bool Bridge::completed(sr_overlay::RecordingKey key)const noexcept{
    std::lock_guard lock(impl_->mutex);const auto* s=impl_->find(handle<VkCommandBuffer>(key.command));
    if(!s||s->key!=key||!s->submitted)return false;
    const auto value=s->fence->GetCompletedValue();return value!=UINT64_MAX&&value>=s->retired;
}
void Bridge::retire(sr_overlay::RecordingKey key)noexcept{std::lock_guard lock(impl_->mutex);for(auto& s:impl_->slots)if(s->key==key){impl_->mappings.erase(s->key);s->mapped=false;}}
void Bridge::retirePool(std::uint64_t pool)noexcept{std::lock_guard lock(impl_->mutex);for(auto& s:impl_->slots)if(s->sourcePool==pool){impl_->mappings.erase(s->key);s->mapped=false;}}
void Bridge::deviceIdle()noexcept{std::lock_guard lock(impl_->mutex);for(auto& s:impl_->slots)if(!s->submitting&&s->submitted&&s->fence->GetCompletedValue()>=s->retired)s->context.reset();}
VkResult Bridge::submit(VkQueue q,unsigned count,const VkSubmitInfo* infos,VkFence fence)noexcept{try{return impl_->submit(q,count,infos,fence);}catch(const std::bad_alloc&){impl_->setError("FSR4 submission allocation failed");return VK_ERROR_OUT_OF_HOST_MEMORY;}catch(const std::exception& e){impl_->setError(e.what());return VK_ERROR_UNKNOWN;}}
VkResult Bridge::submit2(VkQueue q,unsigned count,const VkSubmitInfo2* infos,VkFence fence)noexcept{try{return impl_->submit(q,count,infos,fence);}catch(const std::bad_alloc&){impl_->setError("FSR4 submission allocation failed");return VK_ERROR_OUT_OF_HOST_MEMORY;}catch(const std::exception& e){impl_->setError(e.what());return VK_ERROR_UNKNOWN;}}
std::string Bridge::lastError()const{std::lock_guard lock(impl_->mutex);return impl_->error;}
std::uint64_t Bridge::submittedFrames()const noexcept{return impl_->submitted.load(std::memory_order_relaxed);}
}
