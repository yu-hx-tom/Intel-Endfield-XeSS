// Self-owned Vulkan descriptor recordings. The no-op shader does not read images.
// This validates wrapper metadata and legal GPU submission, never game inputs.
#include "native_source_fixture.hpp"
#include <cstdio>
#include <iostream>
#include <memory>
#include <type_traits>

namespace {
using xess_vk::testing::check;
template<class T> unsigned long long bits(T value) {
    if constexpr(std::is_pointer_v<T>) return reinterpret_cast<unsigned long long>(value);
    else return static_cast<unsigned long long>(value);
}
DWORD WINAPI watchdog(void* argument) {
    if(WaitForSingleObject(static_cast<HANDLE>(argument),28000)==WAIT_TIMEOUT) {
        const char message[]="Descriptor GPU fixture exceeded 28 seconds.\n";
        DWORD written{};WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,sizeof(message)-1,&written,nullptr);
        TerminateProcess(GetCurrentProcess(),124);
    }
    return 0;
}
struct Watchdog {
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE thread=event?CreateThread(nullptr,0,watchdog,event,0,nullptr):nullptr;
    Watchdog(){if(!event||!thread)throw std::runtime_error("Could not create fixture watchdog");}
    ~Watchdog(){SetEvent(event);WaitForSingleObject(thread,INFINITE);CloseHandle(thread);CloseHandle(event);}
};
struct Fixture {
    xess_vk::testing::SourceOwner source;
    std::array<VkImage,3> images{};
    std::array<VkDeviceMemory,3> imageMemory{};
    std::array<VkImageView,3> views{};
    VkImageView unsupportedView{};
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout pipelineLayout{};
    VkDescriptorPool descriptorPool{};
    VkPipeline healthyPipeline{},unsupportedPipeline{};
    VkBuffer indirectBuffer{};
    VkDeviceMemory indirectMemory{};
    unsigned dispatches{},submissions{};
    bool cleanupSucceeded{};

    void reportModule() {
        const auto module=GetModuleHandleW(L"xess-vulkan-observer.dll");
        std::string encoded="null";
        if(module) {
            std::array<wchar_t,32768> path{};
            const auto length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
            if(!length||length>=path.size())throw std::runtime_error("Could not inspect actual loaded observer module");
            const auto bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data(),static_cast<int>(length),nullptr,0,nullptr,nullptr);
            if(bytes<=0)throw std::runtime_error("Could not encode loaded module path");
            std::string utf8(static_cast<std::size_t>(bytes),'\0');
            if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data(),static_cast<int>(length),utf8.data(),bytes,nullptr,nullptr)!=bytes)
                throw std::runtime_error("Could not encode complete loaded module path");
            encoded="\"";
            for(const unsigned char character:utf8) {
                if(character=='\\'||character=='\"')encoded+='\\';
                if(character<0x20)throw std::runtime_error("Unexpected control character in module path");
                encoded+=static_cast<char>(character);
            }
            encoded+='\"';
        }
        std::cout<<"{\"event\":\"fixture_module_provenance\",\"observer_loaded\":"<<(module?"true":"false")
            <<",\"actual_observer_path\":"<<encoded<<"}\n";
    }

    std::uint32_t memoryType(std::uint32_t types,VkMemoryPropertyFlags properties) {
        for(std::uint32_t i=0;i<source.memory.memoryTypeCount;++i)
            if((types&(1u<<i))&&(source.memory.memoryTypes[i].propertyFlags&properties)==properties)return i;
        throw std::runtime_error("Required fixture memory type unavailable");
    }
    void initialize() {
        source.initialize({64,64,5000});
        std::uint32_t familyCount{};
        source.inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(source.physical,&familyCount,nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        source.inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(source.physical,&familyCount,families.data());
        if(source.family>=families.size()||!(families[source.family].queueFlags&VK_QUEUE_COMPUTE_BIT))
            throw std::runtime_error("Selected source queue does not support compute");
        reportModule();
        for(std::size_t i=0;i<images.size();++i) {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType=VK_IMAGE_TYPE_2D;info.format=VK_FORMAT_R8G8B8A8_UNORM;info.extent={64,64,1};
            info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
            info.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            check(source.dev<PFN_vkCreateImage>("vkCreateImage")(source.device,&info,nullptr,&images[i]),"create descriptor image");
            VkMemoryRequirements requirements{};
            source.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(source.device,images[i],&requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(source.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(source.device,&allocation,nullptr,&imageMemory[i]),"allocate descriptor image");
            check(source.dev<PFN_vkBindImageMemory>("vkBindImageMemory")(source.device,images[i],imageMemory[i],0),"bind descriptor image");
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image=images[i];view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=info.format;
            view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            check(source.dev<PFN_vkCreateImageView>("vkCreateImageView")(source.device,&view,nullptr,&views[i]),"create descriptor view");
            if(i==0) {
                VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};usage.usage=VK_IMAGE_USAGE_STORAGE_BIT;
                view.pNext=&usage;
                check(source.dev<PFN_vkCreateImageView>("vkCreateImageView")(source.device,&view,nullptr,&unsupportedView),"create legal pNext view");
            }
        }
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount=1;layout.pBindings=&binding;
        check(source.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(source.device,&layout,nullptr,&setLayout),"create descriptor layout");
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount=1;pipelineLayoutInfo.pSetLayouts=&setLayout;
        check(source.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(source.device,&pipelineLayoutInfo,nullptr,&pipelineLayout),"create pipeline layout");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,6};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets=2;pool.poolSizeCount=1;pool.pPoolSizes=&poolSize;
        check(source.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(source.device,&pool,nullptr,&descriptorPool),"create descriptor pool");
        // Same no-resource GLCompute program used by vulkan_wsi_smoke.cpp.
        constexpr std::uint32_t code[]={
            0x07230203,0x00010000,0,5,0,(2u<<16)|17,1,(3u<<16)|14,0,1,
            (5u<<16)|15,5,3,0x6e69616d,0,(6u<<16)|16,3,17,1,1,1,
            (2u<<16)|19,1,(3u<<16)|33,2,1,(5u<<16)|54,1,3,0,2,
            (2u<<16)|248,4,(1u<<16)|253,(1u<<16)|56};
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shaderInfo.codeSize=sizeof(code);shaderInfo.pCode=code;
        VkShaderModule shader{};
        check(source.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(source.device,&shaderInfo,nullptr,&shader),"create no-op shader");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline.stage.module=shader;pipeline.stage.pName="main";pipeline.layout=pipelineLayout;
        auto result=source.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(source.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&healthyPipeline);
        if(result==VK_SUCCESS) {
            pipeline.flags=VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
            result=source.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(source.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&unsupportedPipeline);
        }
        source.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(source.device,shader,nullptr);
        check(result,"create healthy/unsupported metadata pipelines");
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=sizeof(VkDispatchIndirectCommand);
        buffer.usage=VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(source.dev<PFN_vkCreateBuffer>("vkCreateBuffer")(source.device,&buffer,nullptr,&indirectBuffer),"create indirect buffer");
        VkMemoryRequirements requirements{};
        source.dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(source.device,indirectBuffer,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(source.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(source.device,&allocation,nullptr,&indirectMemory),"allocate indirect arguments");
        check(source.dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(source.device,indirectBuffer,indirectMemory,0),"bind indirect arguments");
        void* mapped{};
        check(source.dev<PFN_vkMapMemory>("vkMapMemory")(source.device,indirectMemory,0,sizeof(VkDispatchIndirectCommand),0,&mapped),"map indirect arguments");
        const VkDispatchIndirectCommand arguments{1,1,1};std::memcpy(mapped,&arguments,sizeof(arguments));
        source.dev<PFN_vkUnmapMemory>("vkUnmapMemory")(source.device,indirectMemory);
    }
    VkDescriptorSet allocateSet(bool badView=false) {
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool=descriptorPool;allocation.descriptorSetCount=1;allocation.pSetLayouts=&setLayout;
        VkDescriptorSet set{};
        check(source.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(source.device,&allocation,&set),"allocate image descriptor set");
        std::array<VkDescriptorImageInfo,3> imageInfo{};
        for(std::size_t i=0;i<imageInfo.size();++i)imageInfo[i]={VK_NULL_HANDLE,badView&&i==0?unsupportedView:views[i],VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorCount=3;
        write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=imageInfo.data();
        source.dev<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(source.device,1,&write,0,nullptr);
        return set;
    }
    void begin(bool initializeImages) {
        source.wait();
        check(source.dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(source.command,0),"reset fixture command");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(source.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(source.command,&info),"begin fixture command");
        if(initializeImages) {
            std::array<VkImageMemoryBarrier,3> barriers{};
            for(std::size_t i=0;i<barriers.size();++i) {
                auto& barrier=barriers[i];barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
                barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
                barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
                barrier.image=images[i];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            }
            source.dev<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(source.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,3,barriers.data());
        }
    }
    void dispatch(const char* label,VkPipeline pipeline,VkDescriptorSet set,bool accepted,bool indirect=false,bool badView=false) {
        source.dev<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(source.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        source.dev<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(source.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);
        if(indirect)source.dev<PFN_vkCmdDispatchIndirect>("vkCmdDispatchIndirect")(source.command,indirectBuffer,0);
        else source.dev<PFN_vkCmdDispatch>("vkCmdDispatch")(source.command,1,1,1);
        ++dispatches;
        std::printf("{\"event\":\"fixture_dispatch\",\"dispatch_call\":%u,\"label\":\"%s\",\"expected_accepted\":%s,"
            "\"indirect\":%s,\"command\":\"0x%llx\",\"pipeline\":\"0x%llx\",\"set\":\"0x%llx\","
            "\"images\":[\"0x%llx\",\"0x%llx\",\"0x%llx\"],\"views\":[\"0x%llx\",\"0x%llx\",\"0x%llx\"],"
            "\"format\":%u,\"declared_layout\":%u,\"aspect_mask\":%u,\"level_count\":1,\"layer_count\":1,"
            "\"groups\":[1,1,1],\"shader_reads_images\":false}\n",
            dispatches,label,accepted?"true":"false",indirect?"true":"false",bits(source.command),bits(pipeline),bits(set),
            bits(images[0]),bits(images[1]),bits(images[2]),bits(badView?unsupportedView:views[0]),bits(views[1]),bits(views[2]),
            unsigned(VK_FORMAT_R8G8B8A8_UNORM),unsigned(VK_IMAGE_LAYOUT_GENERAL),unsigned(VK_IMAGE_ASPECT_COLOR_BIT));
    }
    void submit() {
        check(source.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(source.command),"end descriptor fixture");
        check(source.dev<PFN_vkResetFences>("vkResetFences")(source.device,1,&source.completion),"reset descriptor fence");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&source.command;
        check(source.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(source.queue,1,&info,source.completion),"submit descriptor fixture");
        source.wait();++submissions;
    }
    void run() {
        const auto healthySet=allocateSet();const auto badSet=allocateSet(true);
        begin(true);
        dispatch("healthy",healthyPipeline,healthySet,true);
        dispatch("unsupported_pipeline",unsupportedPipeline,healthySet,false);
        dispatch("healthy_pipeline_recovered",healthyPipeline,healthySet,true);
        dispatch("unsupported_view",healthyPipeline,badSet,false,false,true);
        dispatch("healthy_set_recovered",healthyPipeline,healthySet,true);
        submit();
        check(source.dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(source.command,0),"reset before descriptor pool");
        check(source.dev<PFN_vkResetDescriptorPool>("vkResetDescriptorPool")(source.device,descriptorPool,0),"reset descriptor pool");
        const auto newSet=allocateSet();
        begin(false);
        dispatch("new_set_generation",healthyPipeline,newSet,true);
        dispatch("legal_indirect",healthyPipeline,newSet,false,true);
        dispatch("direct_after_indirect",healthyPipeline,newSet,true);
        submit();
    }
    bool cleanup() noexcept {
        if(cleanupSucceeded)return true;
        try {
            if(!source.device||!source.drainQueue(5000))return false;
            source.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(source.device,descriptorPool,nullptr);
            source.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(source.device,healthyPipeline,nullptr);
            source.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(source.device,unsupportedPipeline,nullptr);
            source.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(source.device,pipelineLayout,nullptr);
            source.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(source.device,setLayout,nullptr);
            source.dev<PFN_vkDestroyImageView>("vkDestroyImageView")(source.device,unsupportedView,nullptr);
            for(std::size_t i=0;i<images.size();++i) {
                source.dev<PFN_vkDestroyImageView>("vkDestroyImageView")(source.device,views[i],nullptr);
                source.dev<PFN_vkDestroyImage>("vkDestroyImage")(source.device,images[i],nullptr);
                source.dev<PFN_vkFreeMemory>("vkFreeMemory")(source.device,imageMemory[i],nullptr);
            }
            source.dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(source.device,indirectBuffer,nullptr);
            source.dev<PFN_vkFreeMemory>("vkFreeMemory")(source.device,indirectMemory,nullptr);
            cleanupSucceeded=true;return true;
        }catch(...){return false;}
    }
};
}
int main() {
    Watchdog bounded;
    auto fixture=std::make_unique<Fixture>();
    try {
        fixture->initialize();fixture->run();
        if(!fixture->cleanup())throw std::runtime_error("Descriptor GPU tail/cleanup not verified");
        fixture.reset();
        std::cout<<"{\"testPassed\":true,\"dispatches\":8,\"expectedAccepted\":5,\"expectedRefused\":3,\"submissions\":2,"
            "\"cleanupSucceeded\":true,\"shaderReadsImages\":false,\"pixelsReadBack\":false,\"gameTested\":false,\"xefgIntegrated\":false}\n";
        return 0;
    }catch(const std::exception& error){fixture.release();std::cerr<<error.what()<<'\n';return 1;}
}
