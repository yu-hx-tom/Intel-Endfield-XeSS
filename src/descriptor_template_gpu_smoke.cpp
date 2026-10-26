// Reuses the self-owned descriptor fixture's resource owner and GPU boundaries.
// Only ordinary template updates are new; the shader never reads descriptors.
#define main descriptor_image_base_fixture_main
#include "descriptor_image_gpu_smoke.cpp"
#undef main

namespace {
struct TemplateFixture : Fixture {
    VkBuffer mixedBuffer{};VkDeviceMemory mixedMemory{};VkBufferView texelView{};VkSampler sampler{};
    VkDescriptorUpdateTemplate coreTemplate{},khrTemplate{};
    unsigned coreUpdates{},khrUpdates{};
    bool khrExtensionVerified{},extraCleanupSucceeded{};
    static constexpr std::size_t imageOffset=3,imageStride=sizeof(VkDescriptorImageInfo)+5;
    static constexpr std::size_t bufferOffset=95,samplerOffset=123,texelOffset=155;

    void initializeTemplates() {
        // SourceOwner enables two names. This fixture has no external semaphore
        // borrower, so use its first slot for the explicit KHR template extension.
        source.enabledExtensions[0]=VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME;
        source.enabledExtensions[1]=VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
        initialize();
        std::uint32_t count{};
        check(source.inst<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties")(source.physical,nullptr,&count,nullptr),"enumerate template extension count");
        std::vector<VkExtensionProperties> extensions(count);
        check(source.inst<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties")(source.physical,nullptr,&count,extensions.data()),"enumerate template extensions");
        khrExtensionVerified=std::any_of(extensions.begin(),extensions.end(),[](const auto& value){return std::strcmp(value.extensionName,VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME)==0;});
        if(!khrExtensionVerified)throw std::runtime_error("VK_KHR_descriptor_update_template unsupported; no core alias fallback allowed");
        (void)source.dev<PFN_vkCreateDescriptorUpdateTemplateKHR>("vkCreateDescriptorUpdateTemplateKHR");
        (void)source.dev<PFN_vkDestroyDescriptorUpdateTemplateKHR>("vkDestroyDescriptorUpdateTemplateKHR");
        (void)source.dev<PFN_vkUpdateDescriptorSetWithTemplateKHR>("vkUpdateDescriptorSetWithTemplateKHR");
        std::cout<<"{\"event\":\"fixture_template_capability\",\"khrExtensionEnumerated\":true,\"khrExtensionEnabled\":true,\"khrAliasesAvailable\":true,\"coreFallbackUsed\":false}\n";
        source.dev<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(source.device,descriptorPool,nullptr);descriptorPool={};
        source.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(source.device,healthyPipeline,nullptr);healthyPipeline={};
        source.dev<PFN_vkDestroyPipeline>("vkDestroyPipeline")(source.device,unsupportedPipeline,nullptr);unsupportedPipeline={};
        source.dev<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(source.device,pipelineLayout,nullptr);pipelineLayout={};
        source.dev<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(source.device,setLayout,nullptr);setLayout={};
        const std::array<VkDescriptorSetLayoutBinding,4> bindings{{
            {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {2,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {3,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=bindings.size();layout.pBindings=bindings.data();
        check(source.dev<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(source.device,&layout,nullptr,&setLayout),"create mixed descriptor layout");
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&setLayout;
        check(source.dev<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(source.device,&pl,nullptr,&pipelineLayout),"create mixed pipeline layout");
        const std::array<VkDescriptorPoolSize,4> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,9},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,3},{VK_DESCRIPTOR_TYPE_SAMPLER,3},{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,3}}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.maxSets=3;poolInfo.poolSizeCount=sizes.size();poolInfo.pPoolSizes=sizes.data();
        check(source.dev<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(source.device,&poolInfo,nullptr,&descriptorPool),"create mixed descriptor pool");
        constexpr std::uint32_t code[]={0x07230203,0x00010000,0,5,0,(2u<<16)|17,1,(3u<<16)|14,0,1,(5u<<16)|15,5,3,0x6e69616d,0,
            (6u<<16)|16,3,17,1,1,1,(2u<<16)|19,1,(3u<<16)|33,2,1,(5u<<16)|54,1,3,0,2,(2u<<16)|248,4,(1u<<16)|253,(1u<<16)|56};
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shaderInfo.codeSize=sizeof(code);shaderInfo.pCode=code;VkShaderModule shader{};
        check(source.dev<PFN_vkCreateShaderModule>("vkCreateShaderModule")(source.device,&shaderInfo,nullptr,&shader),"create mixed no-op shader");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline.stage.module=shader;pipeline.stage.pName="main";pipeline.layout=pipelineLayout;
        const auto result=source.dev<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(source.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&healthyPipeline);
        source.dev<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(source.device,shader,nullptr);check(result,"create mixed no-op pipeline");
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=64;buffer.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(source.dev<PFN_vkCreateBuffer>("vkCreateBuffer")(source.device,&buffer,nullptr,&mixedBuffer),"create valid mixed buffer");
        VkMemoryRequirements requirements{};source.dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(source.device,mixedBuffer,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(source.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(source.device,&allocation,nullptr,&mixedMemory),"allocate valid mixed buffer");
        check(source.dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(source.device,mixedBuffer,mixedMemory,0),"bind valid mixed buffer");
        VkBufferViewCreateInfo bv{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};bv.buffer=mixedBuffer;bv.format=VK_FORMAT_R32_SFLOAT;bv.range=64;
        check(source.dev<PFN_vkCreateBufferView>("vkCreateBufferView")(source.device,&bv,nullptr,&texelView),"create valid texel buffer view");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=1;
        check(source.dev<PFN_vkCreateSampler>("vkCreateSampler")(source.device,&si,nullptr,&sampler),"create valid mixed sampler");
    }
    VkDescriptorUpdateTemplate createTemplate(bool khr) {
        const std::array<VkDescriptorUpdateTemplateEntry,4> entries{{
            {0,0,3,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,imageOffset,imageStride},
            {1,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,bufferOffset,sizeof(VkDescriptorBufferInfo)},
            {2,0,1,VK_DESCRIPTOR_TYPE_SAMPLER,samplerOffset,sizeof(VkDescriptorImageInfo)},
            {3,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,texelOffset,sizeof(VkBufferView)}}};
        VkDescriptorUpdateTemplateCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        info.descriptorUpdateEntryCount=entries.size();info.pDescriptorUpdateEntries=entries.data();info.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;info.descriptorSetLayout=setLayout;
        VkDescriptorUpdateTemplate out{};
        if(khr)check(source.dev<PFN_vkCreateDescriptorUpdateTemplateKHR>("vkCreateDescriptorUpdateTemplateKHR")(source.device,&info,nullptr,&out),"create KHR mixed ordinary template");
        else check(source.dev<PFN_vkCreateDescriptorUpdateTemplate>("vkCreateDescriptorUpdateTemplate")(source.device,&info,nullptr,&out),"create core mixed ordinary template");
        return out;
    }
    VkDescriptorSet allocateMixedSet() {
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};info.descriptorPool=descriptorPool;info.descriptorSetCount=1;info.pSetLayouts=&setLayout;VkDescriptorSet set{};
        check(source.dev<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(source.device,&info,&set),"allocate mixed descriptor set");return set;
    }
    void update(VkDescriptorSet set,VkDescriptorUpdateTemplate object,bool khr,bool badView=false) {
        std::array<std::byte,192> payload{};
        for(std::size_t i=0;i<images.size();++i){const VkDescriptorImageInfo image{VK_NULL_HANDLE,badView&&i==0?unsupportedView:views[i],VK_IMAGE_LAYOUT_GENERAL};std::memcpy(payload.data()+imageOffset+i*imageStride,&image,sizeof(image));}
        const VkDescriptorBufferInfo buffer{mixedBuffer,0,64};const VkDescriptorImageInfo sampled{sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};
        std::memcpy(payload.data()+bufferOffset,&buffer,sizeof(buffer));std::memcpy(payload.data()+samplerOffset,&sampled,sizeof(sampled));std::memcpy(payload.data()+texelOffset,&texelView,sizeof(texelView));
        if(khr){source.dev<PFN_vkUpdateDescriptorSetWithTemplateKHR>("vkUpdateDescriptorSetWithTemplateKHR")(source.device,set,object,payload.data());++khrUpdates;}
        else{source.dev<PFN_vkUpdateDescriptorSetWithTemplate>("vkUpdateDescriptorSetWithTemplate")(source.device,set,object,payload.data());++coreUpdates;}
    }
    void destroyTemplates() {
        if(coreTemplate){source.dev<PFN_vkDestroyDescriptorUpdateTemplate>("vkDestroyDescriptorUpdateTemplate")(source.device,coreTemplate,nullptr);coreTemplate={};}
        if(khrTemplate){source.dev<PFN_vkDestroyDescriptorUpdateTemplateKHR>("vkDestroyDescriptorUpdateTemplateKHR")(source.device,khrTemplate,nullptr);khrTemplate={};}
    }
    void runTemplates() {
        const auto coreSet=allocateMixedSet(),khrSet=allocateMixedSet(),valueSet=allocateMixedSet();
        coreTemplate=createTemplate(false);khrTemplate=createTemplate(true);
        update(coreSet,coreTemplate,false);update(valueSet,coreTemplate,false,true);update(khrSet,khrTemplate,true);destroyTemplates();
        begin(true);dispatch("core_mixed_after_template_destroy",healthyPipeline,coreSet,true);
        dispatch("khr_mixed_after_template_destroy",healthyPipeline,khrSet,true);
        dispatch("valid_pnext_view_local_refusal",healthyPipeline,valueSet,false,false,true);submit();
        check(source.dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(source.command,0),"reset completed template recording before real mutation");
        khrTemplate=createTemplate(true);update(valueSet,khrTemplate,true);destroyTemplates();
        begin(false);dispatch("khr_view_value_recovered_new_recording",healthyPipeline,valueSet,true);submit();
        if(coreUpdates!=2||khrUpdates!=2||dispatches!=4||submissions!=2)throw std::runtime_error("Template fixture call counts mismatch");
    }
    bool cleanupTemplates() noexcept {
        if(extraCleanupSucceeded)return true;
        try{destroyTemplates();if(!cleanup())return false;
            if(texelView)source.dev<PFN_vkDestroyBufferView>("vkDestroyBufferView")(source.device,texelView,nullptr);
            if(sampler)source.dev<PFN_vkDestroySampler>("vkDestroySampler")(source.device,sampler,nullptr);
            if(mixedBuffer)source.dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(source.device,mixedBuffer,nullptr);
            if(mixedMemory)source.dev<PFN_vkFreeMemory>("vkFreeMemory")(source.device,mixedMemory,nullptr);
            extraCleanupSucceeded=true;return true;
        }catch(...){return false;}
    }
};
}
int main() {
    Watchdog bounded;auto fixture=std::make_unique<TemplateFixture>();
    try{fixture->initializeTemplates();fixture->runTemplates();if(!fixture->cleanupTemplates())throw std::runtime_error("Template GPU tail/cleanup not verified");fixture.reset();
        std::cout<<"{\"testPassed\":true,\"dispatches\":4,\"expectedAccepted\":3,\"expectedRefused\":1,\"submissions\":2,\"coreTemplateUpdates\":2,\"khrTemplateUpdates\":2,"
            "\"cleanupSucceeded\":true,\"shaderReadsImages\":false,\"pixelsReadBack\":false,\"gameTested\":false,\"xefgIntegrated\":false}\n";return 0;
    }catch(const std::exception& error){const auto clean=fixture->cleanupTemplates();if(!clean)fixture.release();std::cerr<<error.what()<<'\n';return 1;}
}
