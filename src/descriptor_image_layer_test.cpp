// Own-process driver stubs exercise the production wrappers. No Vulkan loader,
// GPU, game module, detour installer, or external process is used.
#define XESS_METADATA_CONSUMER_TEST 1
#include "vulkan_observer.cpp"
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <limits>

namespace descriptor_layer_fixture {
namespace di=xess_vk::descriptor_image_observer;
namespace dm=xess_vk::descriptor_image_metadata;
unsigned checks{},driverCalls{};
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class T>T handle(std::uintptr_t v){return reinterpret_cast<T>(v);}
struct Dispatch {void* key;};
int marker;
Dispatch deviceDispatch{&marker},commandDispatch{&marker},instanceDispatch{&marker};
const auto device=reinterpret_cast<VkDevice>(&deviceDispatch);
const auto command=reinterpret_cast<VkCommandBuffer>(&commandDispatch);
const auto instance=reinterpret_cast<VkInstance>(&instanceDispatch);
enum Tag : unsigned {CreateLayout=1,DestroyLayout,CreatePL,DestroyPL,CreatePool,ResetPool,DestroyPool,
    AllocateSets,FreeSets,UpdateSets,BindSets,Template,TemplateKhr,Push,PushKhr,PushTemplate,PushTemplateKhr,
    Bind2,Bind2Khr,Push2,Push2Khr,PushTemplate2,PushTemplate2Khr,CreateImage,DestroyImage,CreateView,DestroyView,
    CreatePipeline,DestroyPipeline,CreateCP,DestroyCP,ResetCP,AllocateCB,FreeCB,BeginCB,EndCB,ResetCB,
    BindPipeline,DispatchDirect,DispatchIndirect,DispatchBase,DispatchBaseKhr,CreateChain,DestroyChain,GetImages,
    CreateTemplate,DestroyTemplate,CreateTemplateKhr,DestroyTemplateKhr,Acquire,Acquire2,GetStatus,
    FakeBarrier,FakeBarrier2,FakeBarrier2Khr,FakeWait,FakeWait2,FakeWait2Khr,FakePass,FakePass2,FakePass2Khr,
    GatedSubmit,GatedSubmit2,GatedSubmit2Khr,GatedSparse,GatedPresent,GatedQueueIdle,GatedDeviceIdle,FakeSet2,FakeSet2Khr,
    CreateShader,DestroyShader};
struct Call {Tag tag{};std::array<const void*,5> pointers{};std::array<std::uint64_t,8> numbers{};} last;
VkResult chosen=VK_SUCCESS;
std::uintptr_t returned=0x5000;
di::Context* activeContext{};
bool checkPreDispatch{};
std::uint64_t expectedDispatches{};
const char* unavailable{};
std::vector<std::string> lines;
void sink(const char* line){lines.emplace_back(line);}
void hit(Tag tag){
    ++driverCalls;last={};last.tag=tag;
    require(TryAcquireSRWLockExclusive(&stateLock)!=0,"stateLock must not span original driver call");
    ReleaseSRWLockExclusive(&stateLock);
    if(activeContext){const auto s=di::stats(activeContext); // Also detects adapter mutex held across driver.
        if(checkPreDispatch)require(s.metadata.dispatches==expectedDispatches,"preDispatch runs before original dispatch");}
    SetLastError(0x76543210);
}
void beginCheck(){driverCalls=0;last={};}
void once(Tag tag){require(driverCalls==1&&last.tag==tag,"exactly one matching original entry point");require(GetLastError()==0x76543210,"metadata preserves original driver LastError");}
template<Tag tag,class Info,class Object>
VkResult create(VkDevice d,const Info* info,const VkAllocationCallbacks* allocator,Object* out){
    hit(tag);last.pointers={d,info,allocator,out,nullptr};*out=handle<Object>(returned);return chosen;
}
template<Tag tag,class Object>
void destroy(VkDevice d,Object object,const VkAllocationCallbacks* allocator){hit(tag);last.pointers={d,allocator};last.numbers[0]=bits(object);}
VKAPI_ATTR VkResult VKAPI_CALL createLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo* i,const VkAllocationCallbacks* a,VkDescriptorSetLayout* o){return create<CreateLayout>(d,i,a,o);}
VKAPI_ATTR VkResult VKAPI_CALL createShader(VkDevice d,const VkShaderModuleCreateInfo* i,const VkAllocationCallbacks* a,VkShaderModule* o){return create<CreateShader>(d,i,a,o);}
VKAPI_ATTR void VKAPI_CALL destroyShader(VkDevice d,VkShaderModule h,const VkAllocationCallbacks* a){destroy<DestroyShader>(d,h,a);}
VKAPI_ATTR VkResult VKAPI_CALL createPL(VkDevice d,const VkPipelineLayoutCreateInfo* i,const VkAllocationCallbacks* a,VkPipelineLayout* o){return create<CreatePL>(d,i,a,o);}
VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice d,const VkDescriptorPoolCreateInfo* i,const VkAllocationCallbacks* a,VkDescriptorPool* o){return create<CreatePool>(d,i,a,o);}
VKAPI_ATTR void VKAPI_CALL destroyLayout(VkDevice d,VkDescriptorSetLayout h,const VkAllocationCallbacks* a){destroy<DestroyLayout>(d,h,a);}
VKAPI_ATTR void VKAPI_CALL destroyPL(VkDevice d,VkPipelineLayout h,const VkAllocationCallbacks* a){destroy<DestroyPL>(d,h,a);}
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice d,VkDescriptorPool h,const VkAllocationCallbacks* a){destroy<DestroyPool>(d,h,a);}
template<Tag tag>VKAPI_ATTR VkResult VKAPI_CALL createUpdateTemplate(VkDevice d,const VkDescriptorUpdateTemplateCreateInfo* i,const VkAllocationCallbacks* a,VkDescriptorUpdateTemplate* o){return create<tag>(d,i,a,o);}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL destroyUpdateTemplate(VkDevice d,VkDescriptorUpdateTemplate h,const VkAllocationCallbacks* a){destroy<tag>(d,h,a);}
VKAPI_ATTR VkResult VKAPI_CALL resetPool(VkDevice d,VkDescriptorPool p,VkDescriptorPoolResetFlags f){hit(ResetPool);last.pointers[0]=d;last.numbers={bits(p),f};return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL allocateSets(VkDevice d,const VkDescriptorSetAllocateInfo* i,VkDescriptorSet* o){hit(AllocateSets);last.pointers={d,i,o};for(std::uint32_t n=0;n<i->descriptorSetCount;++n)o[n]=handle<VkDescriptorSet>(returned+n);return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL freeSets(VkDevice d,VkDescriptorPool p,std::uint32_t n,const VkDescriptorSet* s){hit(FreeSets);last.pointers={d,s};last.numbers={bits(p),n};return chosen;}
VKAPI_ATTR void VKAPI_CALL updateSets(VkDevice d,std::uint32_t n,const VkWriteDescriptorSet* w,std::uint32_t m,const VkCopyDescriptorSet* c){hit(UpdateSets);last.pointers={d,w,c};last.numbers={n,m};}
VKAPI_ATTR void VKAPI_CALL bindSets(VkCommandBuffer b,VkPipelineBindPoint p,VkPipelineLayout l,std::uint32_t first,std::uint32_t n,const VkDescriptorSet* s,std::uint32_t m,const std::uint32_t* o){hit(BindSets);last.pointers={b,s,o};last.numbers={std::uint64_t(p),bits(l),first,n,m};}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL updateTemplate(VkDevice d,VkDescriptorSet s,VkDescriptorUpdateTemplate t,const void* data){hit(tag);last.pointers={d,data};last.numbers={bits(s),bits(t)};}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer b,VkPipelineBindPoint p,VkPipelineLayout l,std::uint32_t s,std::uint32_t n,const VkWriteDescriptorSet* w){hit(tag);last.pointers={b,w};last.numbers={std::uint64_t(p),bits(l),s,n};}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL pushTemplate(VkCommandBuffer b,VkDescriptorUpdateTemplate t,VkPipelineLayout l,std::uint32_t s,const void* data){hit(tag);last.pointers={b,data};last.numbers={bits(t),bits(l),s};}
template<Tag tag,class Info>VKAPI_ATTR void VKAPI_CALL commandInfo(VkCommandBuffer b,const Info* i){hit(tag);last.pointers={b,i};}
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice d,const VkImageCreateInfo* i,const VkAllocationCallbacks* a,VkImage* o){return create<CreateImage>(d,i,a,o);}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice d,VkImage h,const VkAllocationCallbacks* a){destroy<DestroyImage>(d,h,a);}
VKAPI_ATTR VkResult VKAPI_CALL createView(VkDevice d,const VkImageViewCreateInfo* i,const VkAllocationCallbacks* a,VkImageView* o){return create<CreateView>(d,i,a,o);}
VKAPI_ATTR void VKAPI_CALL destroyView(VkDevice d,VkImageView h,const VkAllocationCallbacks* a){destroy<DestroyView>(d,h,a);}
VKAPI_ATTR VkResult VKAPI_CALL createPipelines(VkDevice d,VkPipelineCache cache,std::uint32_t n,const VkComputePipelineCreateInfo* i,const VkAllocationCallbacks* a,VkPipeline* o){hit(CreatePipeline);last.pointers={d,i,a,o};last.numbers={bits(cache),n};for(std::uint32_t j=0;j<n;++j)o[j]=j==0?handle<VkPipeline>(returned):VK_NULL_HANDLE;return chosen;}
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice d,VkPipeline h,const VkAllocationCallbacks* a){destroy<DestroyPipeline>(d,h,a);}
VKAPI_ATTR VkResult VKAPI_CALL createCP(VkDevice d,const VkCommandPoolCreateInfo* i,const VkAllocationCallbacks* a,VkCommandPool* o){return create<CreateCP>(d,i,a,o);}
VKAPI_ATTR void VKAPI_CALL destroyCP(VkDevice d,VkCommandPool h,const VkAllocationCallbacks* a){destroy<DestroyCP>(d,h,a);}
VKAPI_ATTR VkResult VKAPI_CALL resetCP(VkDevice d,VkCommandPool h,VkCommandPoolResetFlags f){hit(ResetCP);last.pointers[0]=d;last.numbers={bits(h),f};return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL allocateCB(VkDevice d,const VkCommandBufferAllocateInfo* i,VkCommandBuffer* o){hit(AllocateCB);last.pointers={d,i,o};*o=command;return chosen;}
VKAPI_ATTR void VKAPI_CALL freeCB(VkDevice d,VkCommandPool p,std::uint32_t n,const VkCommandBuffer* b){hit(FreeCB);last.pointers={d,b};last.numbers={bits(p),n};}
VKAPI_ATTR VkResult VKAPI_CALL beginCB(VkCommandBuffer b,const VkCommandBufferBeginInfo* i){hit(BeginCB);last.pointers={b,i};return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL endCB(VkCommandBuffer b){hit(EndCB);last.pointers[0]=b;return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL resetCB(VkCommandBuffer b,VkCommandBufferResetFlags f){hit(ResetCB);last.pointers[0]=b;last.numbers[0]=f;return chosen;}
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer b,VkPipelineBindPoint p,VkPipeline h){hit(BindPipeline);last.pointers[0]=b;last.numbers={std::uint64_t(p),bits(h)};}
VKAPI_ATTR void VKAPI_CALL dispatch(VkCommandBuffer b,std::uint32_t x,std::uint32_t y,std::uint32_t z){hit(DispatchDirect);last.pointers[0]=b;last.numbers={x,y,z};}
VKAPI_ATTR void VKAPI_CALL dispatchIndirect(VkCommandBuffer b,VkBuffer buffer,VkDeviceSize offset){hit(DispatchIndirect);last.pointers[0]=b;last.numbers={bits(buffer),offset};}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL dispatchBase(VkCommandBuffer b,std::uint32_t x,std::uint32_t y,std::uint32_t z,std::uint32_t nx,std::uint32_t ny,std::uint32_t nz){hit(tag);last.pointers[0]=b;last.numbers={x,y,z,nx,ny,nz};}
VKAPI_ATTR VkResult VKAPI_CALL createChain(VkDevice d,const VkSwapchainCreateInfoKHR* i,const VkAllocationCallbacks* a,VkSwapchainKHR* o){return create<CreateChain>(d,i,a,o);}
VKAPI_ATTR void VKAPI_CALL destroyChain(VkDevice d,VkSwapchainKHR h,const VkAllocationCallbacks* a){destroy<DestroyChain>(d,h,a);}
VKAPI_ATTR VkResult VKAPI_CALL getImages(VkDevice d,VkSwapchainKHR h,std::uint32_t* n,VkImage* images){hit(GetImages);last.pointers={d,n,images};last.numbers[0]=bits(h);if(images&&*n)images[0]=handle<VkImage>(returned);*n=1;return chosen;}
VKAPI_ATTR void VKAPI_CALL unknown(){}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice d,const char* name){require(d==device,"GDPA original device");return unavailable&&std::strcmp(unavailable,name)==0?nullptr:reinterpret_cast<PFN_vkVoidFunction>(unknown);}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance i,const char* name){require(i==instance,"GIPA original instance");return unavailable&&std::strcmp(unavailable,name)==0?nullptr:reinterpret_cast<PFN_vkVoidFunction>(unknown);}

struct Fixture {
    DeviceState state{};InstanceState instanceState{};
    std::unique_ptr<xess_vk::image_inventory::Inventory<>> inventory;
    std::unique_ptr<xess_vk::wsi_image_inventory::Inventory> wsi;
    explicit Fixture(bool observe=true,bool productRoute=false,bool renderTail=false){
        state.device=device;state.key=&marker;state.gdpa=gdpa;
        state.createDescriptorSetLayout=createLayout;state.destroyDescriptorSetLayout=destroyLayout;
        state.createPipelineLayout=createPL;state.destroyPipelineLayout=destroyPL;
        state.createDescriptorPool=createPool;state.resetDescriptorPool=resetPool;state.destroyDescriptorPool=destroyPool;
        state.allocateDescriptorSets=allocateSets;state.freeDescriptorSets=freeSets;state.updateDescriptorSets=updateSets;state.cmdBindDescriptorSets=bindSets;
        state.createDescriptorUpdateTemplate=createUpdateTemplate<CreateTemplate>;state.destroyDescriptorUpdateTemplate=destroyUpdateTemplate<DestroyTemplate>;
        state.createDescriptorUpdateTemplateKhr=createUpdateTemplate<CreateTemplateKhr>;state.destroyDescriptorUpdateTemplateKhr=destroyUpdateTemplate<DestroyTemplateKhr>;
        state.updateDescriptorSetWithTemplate=updateTemplate<Template>;state.updateDescriptorSetWithTemplateKhr=updateTemplate<TemplateKhr>;
        state.cmdPushDescriptorSet=push<Push>;state.cmdPushDescriptorSetKHR=push<PushKhr>;
        state.cmdPushDescriptorSetWithTemplate=pushTemplate<PushTemplate>;state.cmdPushDescriptorSetWithTemplateKHR=pushTemplate<PushTemplateKhr>;
        state.cmdBindDescriptorSets2=commandInfo<Bind2,VkBindDescriptorSetsInfo>;state.cmdBindDescriptorSets2KHR=commandInfo<Bind2Khr,VkBindDescriptorSetsInfo>;
        state.cmdPushDescriptorSet2=commandInfo<Push2,VkPushDescriptorSetInfo>;state.cmdPushDescriptorSet2KHR=commandInfo<Push2Khr,VkPushDescriptorSetInfo>;
        state.cmdPushDescriptorSetWithTemplate2=commandInfo<PushTemplate2,VkPushDescriptorSetWithTemplateInfo>;state.cmdPushDescriptorSetWithTemplate2KHR=commandInfo<PushTemplate2Khr,VkPushDescriptorSetWithTemplateInfo>;
        state.createImage=createImage;state.destroyImage=destroyImage;state.createImageView=createView;state.destroyImageView=destroyView;
        state.createComputePipelines=createPipelines;state.destroyPipeline=destroyPipeline;
        state.createCommandPool=createCP;state.destroyCommandPool=destroyCP;state.resetCommandPool=resetCP;
        state.allocateCommandBuffers=allocateCB;state.freeCommandBuffers=freeCB;state.beginCommandBuffer=beginCB;state.endCommandBuffer=endCB;state.resetCommandBuffer=resetCB;
        state.cmdBindPipeline=bindPipeline;state.cmdDispatch=dispatch;state.cmdDispatchIndirect=dispatchIndirect;
        state.cmdDispatchBase=dispatchBase<DispatchBase>;state.cmdDispatchBaseKhr=dispatchBase<DispatchBaseKhr>;
        state.createSwapchain=createChain;state.destroySwapchain=destroyChain;state.swapchainImages=getImages;
        instanceState.instance=instance;instanceState.key=&marker;instanceState.gipa=gipa;
        if(observe){
            inventory=std::make_unique<xess_vk::image_inventory::Inventory<>>();wsi=std::make_unique<xess_vk::wsi_image_inventory::Inventory>(*inventory);
            state.imageInventory=inventory.get();state.wsiInventory=wsi.get();state.descriptors=di::create(device,sink,descriptorDispatchConfig(productRoute,renderTail));require(state.descriptors!=nullptr,"adapter allocation");
        }
        devices=&state;instances=&instanceState;inventorySessionRetired=false;activeContext=state.descriptors;chosen=VK_SUCCESS;lines.clear();
    }
    ~Fixture(){checkPreDispatch=false;activeContext=nullptr;devices=nullptr;instances=nullptr;di::destroy(state.descriptors);}
};

void shaderArchiveForwarding() {
    Fixture f;
    di::destroy(f.state.descriptors);auto compact=descriptorDispatchConfig(true,false,true);
    f.state.descriptors=di::create(device,sink,compact);activeContext=f.state.descriptors;
    require(f.state.descriptors&&!descriptorDispatchConfig(false,false,true).compactOrdinaryDispatch&&
        !descriptorDispatchConfig(true,true,true).compactOrdinaryDispatch,"compact tracking stays out of non-product and render-tail diagnostics");
    f.state.createShaderModule=createShader;f.state.destroyShaderModule=destroyShader;
    f.state.srShaders=xess_vk::compute_observer::createShaderArchive(device,sink);
    require(f.state.srShaders!=nullptr&&!f.state.compute,"SR archive does not enable ordinary compute trace");
    struct ArchiveCleanup {DeviceState& state;~ArchiveCleanup(){xess_vk::compute_observer::destroyShaderArchive(state.srShaders);state.srShaders=nullptr;}} cleanup{f.state};
    // Bounded CPU metadata fixture; never sent to a real Vulkan driver.
    std::vector<std::uint32_t> code{0x07230203,0x00010000,0,1024,0,(5u<<16)|15,5,3,0x6e69616d,0};
    for(unsigned n=0;n<10;++n){const std::uint32_t binding[]{(4u<<16)|71,10+n,34,0,(4u<<16)|71,10+n,33,n};
        code.insert(code.end(),std::begin(binding),std::end(binding));}
    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shaderInfo.codeSize=code.size()*4;shaderInfo.pCode=code.data();
    VkShaderModule shader{};returned=0x6101;beginCheck();require(vkCreateShaderModule(device,&shaderInfo,nullptr,&shader)==VK_SUCCESS,"archive shader forwards success");once(CreateShader);
    require(last.pointers[1]==&shaderInfo&&last.pointers[3]==&shader,"archive preserves shader input/output pointers");
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};VkPipelineLayout layout{};returned=0x6102;
    beginCheck();require(vkCreatePipelineLayout(device,&layoutInfo,nullptr,&layout)==VK_SUCCESS,"archive fixture creates real tracked layout");once(CreatePL);
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pi.layout=layout;
    pi.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
    VkPipeline pipe{};returned=0x6103;chosen=VK_ERROR_OUT_OF_DEVICE_MEMORY;beginCheck();
    require(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&pipe)==chosen&&pipe,"archive preserves valid partial pipeline result");once(CreatePipeline);chosen=VK_SUCCESS;
    beginCheck();vkDestroyShaderModule(device,shader,nullptr);once(DestroyShader);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};VkCommandPool pool{};returned=0x6104;
    beginCheck();require(vkCreateCommandPool(device,&poolInfo,nullptr,&pool)==VK_SUCCESS,"archive fixture command pool");once(CreateCP);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocation.commandPool=pool;allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocation.commandBufferCount=1;VkCommandBuffer cb{};
    beginCheck();require(vkAllocateCommandBuffers(device,&allocation,&cb)==VK_SUCCESS,"archive fixture command buffer");once(AllocateCB);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginCheck();require(vkBeginCommandBuffer(cb,&begin)==VK_SUCCESS,"archive fixture recording");once(BeginCB);
    beginCheck();vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);once(BindPipeline);
    di::SourceQueryDemand full;full.diagnosticAllBindings=true;
    const auto evidence=di::preDispatch(f.state.descriptors,cb,1,1,1,di::DispatchKind::Direct,nullptr,{},{},nullptr,full);
    require((xess_vk::diagnostic_flags::fsrPipelineTracking()?evidence.membershipOnly:evidence.metadataAccepted)&&
        evidence.pipeline.handle==bits(pipe)&&evidence.pipeline.generation,"production opt-in and archive selection use the actual descriptor generation");
    const auto hash=xess_vk::compute_observer::archivedShaderFingerprint(f.state.srShaders,pipe,evidence.pipeline.generation);
    require(hash[0]&&xess_vk::compute_observer::archivedShaderMatches(f.state.srShaders,pipe,evidence.pipeline.generation,{hash.data(),hash.size()}),
        "cold classification reads the existing exact fingerprint after shader-module destruction");
    require(!xess_vk::compute_observer::archivedShaderFingerprint(f.state.srShaders,pipe,evidence.pipeline.generation+1)[0]&&
        !xess_vk::compute_observer::archivedShaderFingerprint(nullptr,pipe,evidence.pipeline.generation)[0],"missing/stale archive cannot classify a pipeline as ordinary");
    require(xess_vk::sr_native_shaders::needsTypedEvidence({})&&xess_vk::sr_native_shaders::needsTypedEvidence(std::string(64,'\0')),
        "unknown fingerprints keep typed evidence");
    for(const auto& profile:xess_vk::sr_native_shaders::profiles)require(xess_vk::sr_native_shaders::needsTypedEvidence(profile.sha256),"every fixed FSR/motion module retains typed evidence");
    require(xess_vk::sr_native_shaders::needsTypedEvidence(xess_vk::sr_native_shaders::auxiliaryWriterSha256)&&
        xess_vk::sr_native_shaders::needsTypedEvidence(xess_vk::sr_native_shaders::photoWriterSha256)&&
        xess_vk::sr_native_shaders::needsTypedEvidence(xess_vk::sr_native_shaders::characterMenuWriterSha256),"auxiliary and photo write exceptions require full typed destinations");
    SetLastError(0x12345678);classifyCompactPipeline(&f.state,evidence.pipeline);
    const auto membership=di::preDispatch(f.state.descriptors,cb,1,1,1,di::DispatchKind::Direct,nullptr,{},{},nullptr,full);
    require(GetLastError()==0x12345678&&membership.membershipOnly&&membership.guardImagesKnown&&!membership.metadataAccepted,
        "production archive classifier and adapter select guard-only ordinary evidence without changing LastError");
    SetLastError(0x12345678);xess_vk::compute_observer::selectArchivedShader(f.state.srShaders,pipe,evidence.pipeline.generation);
    require(GetLastError()==0x12345678,"archive selection preserves driver error state");
    require(std::any_of(lines.begin(),lines.end(),[](const auto& line){return line.find("\"event\":\"sr_shader_archive_chunk\"")!=std::string::npos;}),
        "production shader/pipeline hooks retain the exact code after module destruction");
    beginCheck();vkDestroyPipeline(device,pipe,nullptr);once(DestroyPipeline);
    require(!xess_vk::compute_observer::archivedShaderFingerprint(f.state.srShaders,pipe,evidence.pipeline.generation)[0],"destroyed pipeline loses fingerprint classification");
}

VKAPI_ATTR VkResult VKAPI_CALL acquireImage(VkDevice d,VkSwapchainKHR chain,std::uint64_t timeout,VkSemaphore semaphore,VkFence fence,std::uint32_t* output) {
    hit(Acquire);last.pointers={d,output};last.numbers={bits(chain),timeout,bits(semaphore),bits(fence)};
    if((chosen==VK_SUCCESS||chosen==VK_SUBOPTIMAL_KHR)&&output)*output=2;
    return chosen;
}
VKAPI_ATTR VkResult VKAPI_CALL acquireImage2(VkDevice d,const VkAcquireNextImageInfoKHR* info,std::uint32_t* output) {
    hit(Acquire2);last.pointers={d,info,output};if(info)last.numbers={bits(info->swapchain),info->timeout,bits(info->semaphore),bits(info->fence),info->deviceMask};
    if((chosen==VK_SUCCESS||chosen==VK_SUBOPTIMAL_KHR)&&output)*output=1;
    return chosen;
}
VKAPI_ATTR VkResult VKAPI_CALL chainStatus(VkDevice d,VkSwapchainKHR chain){hit(GetStatus);last.pointers[0]=d;last.numbers[0]=bits(chain);return chosen;}
struct LogCapture {
    wchar_t path[MAX_PATH]{};HANDLE previous{logFile};ULONGLONG bytes{logBytes};
    LogCapture(){wchar_t folder[MAX_PATH]{};require(GetTempPathW(MAX_PATH,folder)!=0&&GetTempFileNameW(folder,L"waq",0,path)!=0,"owned log temp path");
        logFile=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);require(logFile!=INVALID_HANDLE_VALUE,"owned log capture");logBytes=0;}
    ~LogCapture(){CloseHandle(logFile);logFile=previous;logBytes=bytes;DeleteFileW(path);}
    std::string read(){LARGE_INTEGER zero{};require(SetFilePointerEx(logFile,zero,nullptr,FILE_BEGIN)!=0,"read own diagnostic log");std::string text(static_cast<std::size_t>(logBytes),'\0');DWORD read{};
        require(ReadFile(logFile,text.data(),static_cast<DWORD>(text.size()),&read,nullptr)!=0&&read==text.size(),"read exact own log bytes");return text;}
};
struct MeasurementJson {
    std::string_view text;std::size_t at{};
    static void need(bool value){if(!value)throw std::runtime_error("invalid production measurement JSON");}
    static bool digit(char value){return value>='0'&&value<='9';}
    void space(){while(at<text.size()&&(text[at]==' '||text[at]=='\n'||text[at]=='\r'||text[at]=='\t'))++at;}
    bool take(char value){space();if(at==text.size()||text[at]!=value)return false;++at;return true;}
    std::string_view string(){
        need(take('"'));const auto begin=at;
        while(at<text.size()){
            const auto value=static_cast<unsigned char>(text[at++]);
            if(value=='"')return text.substr(begin,at-begin-1);
            need(value>=32);
            if(value=='\\'){
                need(at<text.size());const auto escaped=text[at++];
                if(escaped=='u')for(unsigned i=0;i<4;++i){need(at<text.size());const auto hex=text[at++];
                    need(digit(hex)||(hex>='a'&&hex<='f')||(hex>='A'&&hex<='F'));}
                else need(std::string_view("\"\\/bfnrt").find(escaped)!=std::string_view::npos);
            }
        }
        need(false);return {};
    }
    void value(unsigned depth=0){
        need(depth<32);space();need(at<text.size());
        if(text[at]=='"'){string();return;}
        if(take('{')){if(take('}'))return;do{string();need(take(':'));value(depth+1);if(take('}'))return;}while(take(','));need(false);}
        if(take('[')){if(take(']'))return;do{value(depth+1);if(take(']'))return;}while(take(','));need(false);}
        for(const auto literal:{std::string_view("true"),std::string_view("false"),std::string_view("null")})
            if(text.substr(at,literal.size())==literal){at+=literal.size();return;}
        take('-');need(at<text.size());
        if(text[at]=='0')++at;else{need(text[at]>='1'&&text[at]<='9');while(at<text.size()&&digit(text[at]))++at;}
        if(at<text.size()&&text[at]=='.'){++at;need(at<text.size()&&digit(text[at]));while(at<text.size()&&digit(text[at]))++at;}
        if(at<text.size()&&(text[at]=='e'||text[at]=='E')){++at;if(at<text.size()&&(text[at]=='+'||text[at]=='-'))++at;
            need(at<text.size()&&digit(text[at]));while(at<text.size()&&digit(text[at]))++at;}
    }
    using Members=std::vector<std::pair<std::string_view,std::string_view>>;
    Members object(){
        Members result;need(take('{'));
        if(!take('}'))do{const auto key=string();need(take(':'));space();const auto begin=at;value();
            require(std::none_of(result.begin(),result.end(),[&](const auto& member){return member.first==key;}),"summary object keys are unique");
            result.emplace_back(key,text.substr(begin,at-begin));if(take('}'))break;need(take(','));}while(true);
        space();need(at==text.size());return result;
    }
};
std::string_view measurementMember(const MeasurementJson::Members& members,std::string_view key){
    const auto found=std::find_if(members.begin(),members.end(),[&](const auto& member){return member.first==key;});
    require(found!=members.end(),"production summary retains required field");return found->second;
}
void measurementSummarySink(const char* data,DWORD size){lines.emplace_back(data,size);}
struct MeasurementSummaryCapture {
    decltype(measurementSummaryTestSink) previous{measurementSummaryTestSink};
    MeasurementSummaryCapture(){lines.clear();measurementSummaryTestSink=measurementSummarySink;}
    ~MeasurementSummaryCapture(){measurementSummaryTestSink=previous;}
    MeasurementJson::Members read(){require(lines.size()==1&&lines.front().back()=='\n',"one complete production summary emitted");return MeasurementJson{lines.front()}.object();}
};
void threadCpuBatching(){
    MeasurementSummaryCapture capture;xess_vk::diagnostics::ThreadCpuWindow window;
    window.generation=1;window.sequence=2;window.beginQpc=10;window.endQpc=60;window.frequency=10;window.enumerated=220;
    window.calibration={3e9,3e9,0.99,true,true,"fixture_validated"};window.processCyclesKnown=true;window.processCycles=3000;
    for(unsigned i=0;i<220;++i){xess_vk::diagnostics::ThreadCpuDelta row;row.identity.id=i+1;row.identity.creation=i+100;
        row.identity.name=L"worker \"test\"";row.identity.module=L"C:\\fixture\\module.dll";row.cpuKnown=true;row.userNs=i+1;row.cyclesKnown=true;row.cycles=300;window.threads.push_back(row);}
    window.groups.push_back({L"C:\\fixture\\module.dll",L"worker \"test\"",24310,0,0,220,220});
    threadCpuSampleSink(nullptr,window);
    require(lines.size()>1&&lines.size()<16,"220 thread rows use a few bounded queue entries, not one entry per thread");
    unsigned threads{},groups{},windows{};
    for(const auto& batch:lines){require(batch.size()<=32768&&batch.back()=='\n',"every JSONL batch is bounded and complete");
        std::string_view remaining=batch;
        while(!remaining.empty()){const auto end=remaining.find('\n');require(end!=std::string_view::npos,"batched record retains its delimiter");
            const auto object=MeasurementJson{remaining.substr(0,end)}.object();const auto event=measurementMember(object,"event");
            threads+=event=="\"thread_cpu_thread\"";groups+=event=="\"thread_cpu_group\"";windows+=event=="\"thread_cpu_window\"";
            if(event=="\"thread_cpu_thread\"")require(measurementMember(object,"schema")=="2"&&measurementMember(object,"cycle_runtime_known")=="true",
                "thread schema separates calibrated runtime from API ledger");
            if(event=="\"thread_cpu_window\"")require(measurementMember(object,"cycle_calibration_valid")=="true"&&measurementMember(object,"process_cycles_known")=="true",
                "window carries conversion provenance and process cross-check");
            remaining.remove_prefix(end+1);}}
    require(threads==220&&groups==1&&windows==1,"batching preserves every original JSON event and its schema");
}
void measurementSummarySchema(){
    using namespace xess_vk::measurement;
    MeasurementSummaryCapture capture;DeviceState state{};state.device=device;
    auto window=std::make_unique<Window>(1);state.measurements=window.get();
    constexpr const char* expectedNames[]{"present_entry_interval","present_call","captured_frame_submit_interval","cap_wait","copy_fence_wait","consumed_wait","dxgi_present",
        "app_transfer_commands","depth_dispatches","provider_depth_compute_gpu","provider_depth_transfer_gpu","provider_aux_gpu","native_gpu","bridge_gpu","caller_gpu","statistics_emit_cpu","sr_gpu"};
    require(std::size(expectedNames)==metricCount&&std::size(measurementMetricNames)==metricCount,"every metric including appended SrGpu has a serializer name");
    for(unsigned i=0;i<metricCount;++i)window->add(static_cast<Metric>(i),1000+i);
    window->addCommands(CopyRole::Color,Commands{2});window->addCommands(CopyRole::Sdk,Commands{999});
    emitMeasurementSummary(&state,summaryDurationNs);
    require(lines.empty(),"production summary does not emit before 60 seconds");
    emitMeasurementSummary(&state,summaryDurationNs+1);
    const auto document=capture.read();
    require(measurementMember(document,"event")=="\"measurement_summary\""&&measurementMember(document,"partial")=="false","production summary event and cadence survive serialization");
    const auto metrics=MeasurementJson{measurementMember(document,"metrics")}.object();
    require(metrics.size()==metricCount,"production serializer emits every metric exactly once");
    for(unsigned i=0;i<metricCount;++i){
        require(std::string_view(measurementMetricNames[i])==expectedNames[i],"metric enum order matches the summary schema");
        const auto metric=MeasurementJson{measurementMember(metrics,expectedNames[i])}.object();
        require(measurementMember(metric,"count")=="1"&&measurementMember(metric,"sum")==std::to_string(1000+i),"production metric index preserves its own observation");
    }
    const auto copies=MeasurementJson{measurementMember(document,"copy_commands")}.object();
    const auto color=MeasurementJson{measurementMember(copies,"color")}.object();
    require(measurementMember(color,"image_copy")=="2"&&measurementMember(copies,"sdk")=="null"&&
        measurementMember(document,"sdk_internal_copy_commands")=="null"&&measurementMember(document,"sdk_internal_copy_count_available")=="false",
        "measured app copies remain numeric while SDK internal copies are unknown, never inferred or zero");
    for(const auto malformed:{"{\"a\":1,}","{\"a\":01}","{\"a\":[null,]}","{\"a\":true} trailing"}){
        bool rejected{};try{MeasurementJson{malformed}.object();}catch(const std::exception&){rejected=true;}
        require(rejected,"summary JSON checker rejects malformed structure and trailing data");
    }
}
void measurementFrameClassification(){
    using namespace xess_vk::measurement;
    MeasurementSummaryCapture capture;DeviceState state{};state.device=device;
    auto window=std::make_unique<Window>(xess_vk::cpuWallNowNs());auto detail=std::make_unique<Snapshot>();
    state.measurements=window.get();state.measurementSnapshot=detail.get();Scope measured(window.get());
    xess_vk::presentation::FrameStatus frame;std::uint64_t attempt{};
    const auto present=[&](bool paired,bool on,bool fresh,bool valid,unsigned frames){
        frame.menuPaired=paired;frame.menuRequestedEnabled=on;frame.sdkStatusValid=valid;frame.framesPresented=frames;
        recordMeasurements(&state,frame,fresh,xess_vk::cpuWallNowNs(),++attempt,{});
    };
    present(true,true,true,true,2);present(true,false,true,true,1);present(false,true,false,true,2);
    present(true,true,true,false,2);present(true,false,false,true,1);
    xess_vk::continuous::Stats inputs;inputs.saturated=2;inputs.copyFailures=4;
    frame.sharedPlanes.available=true;frame.sharedPlanes.ringGeneration=1;frame.sharedPlanes.noFreeFallbacks=3;
    recordMeasurementFailures(&state,inputs,frame);
    frame.sharedPlanes={};recordMeasurementFailures(&state,inputs,frame);
    frame.sharedPlanes.available=true;frame.sharedPlanes.ringGeneration=1;frame.sharedPlanes.noFreeFallbacks=3;
    recordMeasurementFailures(&state,inputs,frame);
    frame.sharedPlanes.ringGeneration=2;recordMeasurementFailures(&state,inputs,frame);
    inputs.saturated=5;inputs.copyFailures=5;frame.sharedPlanes.noFreeFallbacks=4;
    recordMeasurementFailures(&state,inputs,frame);recordMeasurementFailures(&state,inputs,frame);
    emitMeasurementSummary(&state,xess_vk::cpuWallNowNs(),true);
    const auto document=capture.read();const auto counters=MeasurementJson{measurementMember(document,"counters")}.object();
    require(measurementMember(counters,"menu_on_frames")=="2"&&measurementMember(counters,"menu_off_frames")=="2"&&measurementMember(counters,"menu_unknown_frames")=="1",
        "menu classification does not reuse a stale successful output");
    require(measurementMember(counters,"sdk_single_frames")=="1"&&measurementMember(counters,"sdk_double_frames")=="1",
        "only fresh valid SDK output contributes to the single/double ratio");
    const auto ratio=MeasurementJson{measurementMember(document,"sdk_double_frame_ratio")}.object();
    require(measurementMember(ratio,"numerator")=="1"&&measurementMember(ratio,"denominator")=="2","SDK ratio denominator excludes stale and invalid status");
    require(measurementMember(counters,"no_free_slots")=="9"&&measurementMember(counters,"copy_failures")=="5",
        "independent Provider/shared deltas skip unavailable snapshots and do not recount cumulative ring history or duplicates");
    const auto metrics=MeasurementJson{measurementMember(document,"metrics")}.object();
    const auto transfers=MeasurementJson{measurementMember(metrics,"app_transfer_commands")}.object();
    require(measurementMember(transfers,"count")=="3","only fresh frames contribute per-frame command samples");
}
void measurementIntervalClassification(){
    using namespace xess_vk::presentation;using namespace xess_vk::frame_input;
    FrameStatus frame{};frame.menuPaired=frame.menuRequestedEnabled=frame.menuGatesConsistent=true;
    frame.menuGatesReady=frame.menuInputObserved=frame.inputMatched=frame.historyResetKnown=true;
    frame.inputValidity=Extents|Jitter|MotionScale|View|Projection|CameraPlanes|Reset;
    frame.controlEpoch=7;frame.viewportGeneration=9;frame.sdkStatusValid=true;
    MeasurementInterval interval{};
    require(classifyMeasurementFrame(interval,frame,true,true)==MeasurementFrameState::Unknown,"initial interval lacks preceding state proof");
    require(classifyMeasurementFrame(interval,frame,true,true)==MeasurementFrameState::Steady,"known valid ON input enters steady without requiring two SDK frames");
    frame.framesPresented=1;frame.queryResult=3;
    require(classifyMeasurementFrame(interval,frame,true,true)==MeasurementFrameState::Steady&&interval.steadyWarnings==1,
        "too few frames is an outcome, never a filter that conceals steady warnings");
    frame.queryResult=-7;frame.sdkStatusValid=false;frame.sdkResultKnown=true;
    require(classifyMeasurementFrame(interval,frame,true,false)==MeasurementFrameState::Steady&&interval.steadyErrors==1&&interval.outputFailures==1,
        "failed SDK output with admitted inputs remains eligible and retains its error");
    frame.sdkStatusValid=true;
    frame.queryResult=3;frame.historyResetReasons=HistoryResetLongFrame;
    require(classifyMeasurementFrame(interval,frame,true,true)==MeasurementFrameState::Steady&&interval.resets==0&&interval.historyResets==1&&interval.fallbackWarnings==1,
        "a stall-triggered remedy preserves the hitch in display statistics and separately counts expected warnings");
    frame.historyResetReasons=HistoryResetInput;
    require(classifyMeasurementFrame(interval,frame,true,true)==MeasurementFrameState::Transition&&interval.resets==1,
        "actual content history reset is excluded separately from stall remedies");
    frame.historyResetReasons=0;frame.controlEpoch=8;
    require(classifyMeasurementFrame(interval,frame,true,true)==MeasurementFrameState::Transition,"control epoch change is explicit transition");
    frame.inputMatched=false;
    require(classifyMeasurementFrame(interval,frame,true,true)==MeasurementFrameState::Invalid,"missing current inputs cannot qualify");
    frame.inputMatched=true;frame.inputValidity&=~Reset;
    require(classifyMeasurementFrame(interval,frame,true,true)==MeasurementFrameState::Invalid,"incomplete constants cannot qualify");
    frame.inputValidity|=Reset;frame.onBlockedReason="frame_inputs_not_current";
    require(classifyMeasurementFrame(interval,frame,true,true)==MeasurementFrameState::Invalid,"native preflight refusal cannot qualify");
    frame.onBlockedReason=nullptr;frame.menuRequestedEnabled=false;
    require(classifyMeasurementFrame(interval,frame,true,true)==MeasurementFrameState::Off,"menu OFF remains distinct from missing inputs");
    require(classifyMeasurementFrame(interval,frame,false,false)==MeasurementFrameState::Unknown&&!interval.baselineKnown,
        "no current output attempt cannot reuse previous frame status");
    frame.menuRequestedEnabled=true;frame.historyResetKnown=false;
    require(classifyMeasurementFrame(interval,frame,true,true)==MeasurementFrameState::Unknown,"unknown history does not fabricate steady evidence");
    const auto previous=measurementDetailTestSink;measurementDetailTestSink=measurementSummarySink;
    lines.clear();JsonLine status("measurement_status");measurementIntervalFields(status,interval,100,200);status.write(true);
    measurementDetailTestSink=previous;
    require(lines.size()==1,"compact status has one complete production JSON line");
    const auto parsed=MeasurementJson{lines.front()}.object();
    require(measurementMember(parsed,"status_interval_state")=="\"mixed\""&&measurementMember(parsed,"status_begin_ns")=="100"&&
        measurementMember(parsed,"status_end_ns")=="200","whole-interval coverage and mixed state survive production serialization");
    require(measurementMember(parsed,"status_interval_attempts")=="13"&&measurementMember(parsed,"status_interval_steady_sdk_errors")=="1"&&
        measurementMember(parsed,"status_interval_output_failures")=="1","compact status keeps actual failure counts, including in steady input windows");
    MeasurementInterval steady{};steady.baselineKnown=true;steady.controlEpoch=frame.controlEpoch;steady.viewportGeneration=frame.viewportGeneration;
    frame.historyResetKnown=true;frame.queryResult=0;classifyMeasurementFrame(steady,frame,true,true);
    lines.clear();JsonLine allSteady("measurement_status");measurementIntervalFields(allSteady,steady,200,300);
    measurementDetailTestSink=measurementSummarySink;allSteady.write(true);measurementDetailTestSink=previous;
    require(measurementMember(MeasurementJson{lines.front()}.object(),"status_interval_state")=="\"steady_fg_input_valid\"",
        "only an interval with every input attempt steady receives the steady label");
}
void wsiAcquireObservation() {
    namespace wa=xess_vk::wsi_acquire;
    for(const auto* flag:{static_cast<const wchar_t*>(nullptr),L"0",L"01",L"11",L"1 ",L"1"}) {
        SetEnvironmentVariableW(L"XESS_WSI_ACQUIRE_OBSERVER",flag);
        require(wsiAcquireRequested()==(flag&&std::wcscmp(flag,L"1")==0),"acquire observation requires exact env value 1");
    }
    SetEnvironmentVariableW(L"XESS_WSI_ACQUIRE_OBSERVER",nullptr);
    Fixture f;
    f.state.acquireNextImage=acquireImage;f.state.acquireNextImage2=acquireImage2;f.state.getSwapchainStatus=chainStatus;
    const auto chain=handle<VkSwapchainKHR>(0x8765);const auto semaphore=handle<VkSemaphore>(0x5432);const auto fence=handle<VkFence>(0x4321);
    const auto created=f.wsi->beforeCreate(0,{true,false,0,3,37,64,32,1,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,0});
    const auto generation=f.wsi->afterCreate(created.createTicket,VK_SUCCESS,bits(chain)).chain.generation;
    require(generation!=0,"acquire fixture uses existing WSI generation");
    LogCapture log;
    std::uint32_t imageIndex=99;
    VkAcquireNextImageInfoKHR info{VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR};info.swapchain=chain;info.timeout=UINT64_MAX;info.semaphore=semaphore;info.fence=fence;info.deviceMask=3;
    VkBaseInStructure extension{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr};info.pNext=&extension;
    beginCheck();require(vkAcquireNextImageKHR(device,chain,0,semaphore,VK_NULL_HANDLE,&imageIndex)==VK_SUCCESS&&imageIndex==2,"default off KHR forwards output");once(Acquire);
    beginCheck();require(vkAcquireNextImage2KHR(device,&info,&imageIndex)==VK_SUCCESS&&imageIndex==1,"default off acquire2 forwards output");once(Acquire2);
    beginCheck();require(vkGetSwapchainStatusKHR(device,chain)==VK_SUCCESS,"default off status forwards");once(GetStatus);
    require(!f.state.wsiAcquire&&!logBytes,"default off produces no observer and no acquire records");
    wa::Observer observer;f.state.wsiAcquire=&observer;
    for(const auto* name:{"vkAcquireNextImageKHR","vkAcquireNextImage2KHR","vkGetSwapchainStatusKHR"}) {
        const auto expected=std::strcmp(name,"vkAcquireNextImageKHR")==0?reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImageKHR):
            std::strcmp(name,"vkAcquireNextImage2KHR")==0?reinterpret_cast<PFN_vkVoidFunction>(vkAcquireNextImage2KHR):reinterpret_cast<PFN_vkVoidFunction>(vkGetSwapchainStatusKHR);
        require(vkGetDeviceProcAddr(device,name)==expected&&vkGetInstanceProcAddr(instance,name)==expected,"both hook registration paths expose available acquire API");
        unavailable=name;require(!vkGetDeviceProcAddr(device,name)&&!vkGetInstanceProcAddr(instance,name),"unavailable optional API is never invented");unavailable=nullptr;
    }
    beginCheck();imageIndex=99;chosen=VK_SUBOPTIMAL_KHR;
    require(vkAcquireNextImageKHR(device,chain,UINT64_MAX,semaphore,fence,&imageIndex)==chosen&&imageIndex==2,"enabled KHR forwards SUBOPTIMAL output");once(Acquire);
    require(last.pointers[0]==device&&last.pointers[1]==&imageIndex&&last.numbers[0]==bits(chain)&&last.numbers[1]==UINT64_MAX&&last.numbers[2]==bits(semaphore)&&last.numbers[3]==bits(fence),"KHR preserves device chain timeout sem fence and output pointer");
    auto stats=observer.snapshot();const auto first=stats.api[0].latest;
    require(first.generation==generation&&first.imageIndexKnown&&first.imageIndex==2&&first.presentThread==0&&first.entryNs>0,"sample keeps existing identity success index timing and unknown present relation");
    f.state.wsiPresentThread=static_cast<LONG>(GetCurrentThreadId());
    beginCheck();chosen=VK_SUCCESS;require(vkAcquireNextImage2KHR(device,&info,&imageIndex)==chosen&&imageIndex==1,"enabled acquire2 success");once(Acquire2);
    require(last.pointers[0]==device&&last.pointers[1]==&info&&last.pointers[2]==&imageIndex&&info.pNext==&extension&&info.deviceMask==3,"acquire2 preserves original info pNext deviceMask and output pointers");
    stats=observer.snapshot();require(stats.api[1].threadSame==1&&stats.api[1].pNextCalls==1&&stats.api[1].deviceMaskNotOneCalls==1,"Acquire2 extra arguments and same observed thread counted");
    // Inaccessible output is legal for our downstream failure stub: the wrapper must not read it.
    void* inaccessible=VirtualAlloc(nullptr,4096,MEM_RESERVE,PAGE_NOACCESS);require(inaccessible!=nullptr,"reserve inaccessible failure output");
    struct Reservation {void* address;~Reservation(){VirtualFree(address,0,MEM_RELEASE);}} reservation{inaccessible};
    auto* unreadable=static_cast<std::uint32_t*>(inaccessible);
    for(const auto result:{VK_TIMEOUT,VK_NOT_READY,VK_ERROR_OUT_OF_DATE_KHR,VK_ERROR_DEVICE_LOST}) {
        chosen=result;beginCheck();require(vkAcquireNextImageKHR(device,chain,17,VK_NULL_HANDLE,fence,unreadable)==result,"KHR failure timeout and NOT_READY forwarded without output read");once(Acquire);
        beginCheck();require(vkAcquireNextImage2KHR(device,&info,unreadable)==result,"acquire2 failure forwarded without output read");once(Acquire2);
    }
    chosen=VK_ERROR_SURFACE_LOST_KHR;beginCheck();require(vkGetSwapchainStatusKHR(device,chain)==chosen,"get status error untouched");once(GetStatus);
    require(last.pointers[0]==device&&last.numbers[0]==bits(chain),"status device and chain identity preserved");
    std::thread other([&]{chosen=VK_TIMEOUT;vkAcquireNextImageKHR(device,chain,0,VK_NULL_HANDLE,VK_NULL_HANDLE,unreadable);});other.join();
    stats=observer.snapshot();require(stats.api[0].threadDifferent==1&&stats.api[0].threadUnknown==1&&stats.api[0].synchronization[3]==1&&stats.api[0].synchronization[2]==4&&stats.api[0].synchronization[0]==1,"thread comparison and all observed sync combinations bounded counters");
    require(!stats.api[0].latest.imageIndexKnown&&!stats.api[1].latest.imageIndexKnown&&stats.api[2].calls==1,"failed calls never mark an image index known and status remains separate");
    wsiAcquireSummary(&f.state,"fixture_final");
    const auto text=log.read();require(text.find("\"timeout_ns\":\"18446744073709551615\"")!=std::string::npos&&text.find("\"result\":-1000001004")!=std::string::npos&&text.find("\"reason\":\"fixture_final\"")!=std::string::npos,"bounded JSON samples and final counters preserve unsigned timeout and error result");
    f.state.wsiAcquire=nullptr;
    wa::Observer bounded;wa::Call call;call.argumentsKnown=true;
    unsigned samples{},summaries{};
    for(unsigned i=0;i<(wa::periodicSummaryLimit+1)*wa::summaryInterval;++i){call.timeout=i;call.result=static_cast<std::int32_t>(i);const auto emitted=bounded.record(call);samples+=emitted.sample;summaries+=emitted.summary;}
    const auto boundedStats=bounded.snapshot();
    require(samples==wa::sampleLimitPerApi&&summaries==wa::periodicSummaryLimit&&boundedStats.omittedPeriodicSummaries==1&&boundedStats.api[0].timeoutValueOverflow>0&&boundedStats.api[0].resultValueOverflow>0,"sample summary and distinct-value limits remain bounded without dropping totals");
}

void gpuTimestampArithmetic() {
    xess_vk::GpuTimestampSample sample;sample.requested=sample.available=true;sample.validBits=4;
    sample.timestampPeriodNs=2;sample.beginTicks=14;sample.endTicks=2;
    require(xess_vk::timestampDuration(sample)&&sample.durationNs==8,"finite timestamp counter wrap preserves modulo duration");
    sample.qpcFrequency=1'000'000'000;sample.submitQpcTicks=1;sample.fenceObservedQpcTicks=101;
    require(!xess_vk::timestampDuration(sample)&&sample.beginTicks==14&&sample.endTicks==2,"possible multiple wraps preserve raw values and unknown duration");
    sample.qpcFrequency=0;sample.timestampPeriodNs=std::numeric_limits<double>::infinity();
    require(!xess_vk::timestampDuration(sample),"non-finite timestamp period cannot become JSON duration");
    sample.timestampPeriodNs=1;sample.validBits=65;require(!xess_vk::timestampDuration(sample),"unsupported valid-bit count is unavailable");
    sample.validBits=64;sample.beginTicks=UINT64_MAX-2;sample.endTicks=1;
    require(xess_vk::timestampDuration(sample)&&sample.durationNs==4,"full-width unsigned timestamp wrap is defined");
    sample.calibration={true,false,1,1000,1'000'000,101,0};double aligned{};
    require(xess_vk::timestampQpcUs(sample,sample.beginTicks,aligned)&&std::abs(aligned-999.996)<1e-6,"calibrated counter wrap aligns with explicit QPC frequency");
    sample.calibration.available=false;require(!xess_vk::timestampQpcUs(sample,sample.beginTicks,aligned),"uncalibrated GPU ticks never invent QPC alignment");
    LogCapture capture;JsonLine log("fixture_gpu_sample");gpuTimestampFields(log,"gpu",sample);log.write();
    const auto text=capture.read();
    require(text.find("\"begin_ticks\":\"18446744073709551613\"")!=std::string::npos&&text.find("\"qpc_alignment_known\":false")!=std::string::npos&&
        text.find("\"max_deviation_ns\"")==std::string::npos,"raw 64-bit ticks are exact text and absent deviation stays unknown");
    xess_vk::presentation::FrameStatus frame;frame.vkInteropCopyGpu=sample;
    JsonLine interop("fixture_interop_sample");cpuWallFields(interop,frame);interop.write();
    const auto interopText=capture.read();
    require(interopText.find("\"vk_interop_copy_gpu\":{")!=std::string::npos&&
        interopText.find("\"vk_private_copy_gpu\":{")==std::string::npos&&
        interopText.find("\"d3d_caller_copy_tag_gpu\":{")==std::string::npos,
        "Interop copy scope logs independently without inventing other GPU samples");
}
void asyncInflightCounters() {
    Fixture f;LogCapture capture;
    xess_vk::presentation::FrameStatus frame;
    frame.asyncCopyPath=true;frame.inflightFrames=frame.inflightHighWater=frame.inflightLimit=2;
    frame.inflightCapWaitCpu.add(5'000'000);frame.inflightCapWaitCpu.add(7'000'000);
    JsonLine status("fixture_async_status");cpuWallFields(status,frame);status.write();
    frame.sharedCopyPath=frame.sharedColorDirect=frame.sharedAuxiliaryDirect=frame.depthCompute=true;
    frame.sharedPlaneKey={2,17,23};frame.sharedPlanes.available=frame.sharedPlanes.ready=frame.sharedPlanes.depthStorage=true;
    frame.sharedPlanes.ringGeneration=17;frame.sharedPlanes.resourceBytes=12288;frame.sharedPlanes.leases=2;
    frame.sharedPlanes.directFrames=11;frame.sharedPlanes.legacySourceFrames=3;frame.sharedPlanes.noFreeFallbacks=5;
    frame.sharedPlanes.released=7;frame.sharedPlanes.poisoned=1;
    JsonLine shared("fixture_shared_status");cpuWallFields(shared,frame);shared.write();
    frame.sharedPlaneKey.index=xess_vk::sharedPlaneSlotCount;frame.sharedFallbackReason="shared_color_unavailable";
    frame.inflightLimit=1;
    f.state.frameSpikeValid=true;f.state.frameSpikeCapWaits=1;f.state.frameSpikePresentation[9]=5'000'000;
    const auto spike=[&]{f.state.frameSpikePresentNs=steadyNowNs()-30'000'000;
        recordFrameSpike(&f.state,frame,false,steadyNowNs()-1'000'000,88,4);};
    spike();spike();
    frame.inflightCapWaitCpu={};frame.inflightCapWaitCpu.add(3'000'000);spike();
    const auto text=capture.read();
    const auto unknownAt=text.find("\"event\":\"fixture_async_status\"");
    const auto unknown=text.substr(unknownAt,text.find('\n',unknownAt)-unknownAt);
    require(unknown.find("\"shared_plane_key\":null")!=std::string::npos&&
            unknown.find("\"shared_planes\":{\"available\":false}")!=std::string::npos&&
            unknown.find("\"resource_bytes\"")==std::string::npos&&unknown.find("\"direct_frames\"")==std::string::npos,
            "unallocated shared plane counters remain unknown instead of measurements of zero");
    require(text.find("\"shared_plane_key\":{\"index\":2,\"ring_generation\":\"17\",\"lease_generation\":\"23\"}")!=std::string::npos&&
            text.find("\"shared_color_direct\":true")!=std::string::npos&&text.find("\"shared_auxiliary_direct\":true")!=std::string::npos&&
            text.find("\"depth_snapshot_compute\":true")!=std::string::npos&&text.find("\"depth_storage\":true")!=std::string::npos&&
            text.find("\"resource_bytes\":12288,\"leases\":2")!=std::string::npos&&
            text.find("\"direct_frames\":11,\"legacy_source_frames\":3")!=std::string::npos&&
            text.find("\"shared_fallback_reason\":null")!=std::string::npos,
            "known shared status preserves exact lease generations and direct/fallback resource counters");
    require(text.find("\"async_copy_path\":true")!=std::string::npos&&text.find("\"inflight_frames\":2")!=std::string::npos&&
        text.find("\"inflight_high_water\":2")!=std::string::npos&&text.find("\"inflight_cap_waits_total\":2")!=std::string::npos&&
        text.find("\"inflight_limit\":2")!=std::string::npos&&
        text.find("\"cpu_inflight_cap_wait\":{\"count\":2,\"total_ns\":12000000,\"max_ns\":7000000}")!=std::string::npos,
        "periodic status exposes the exact K2 occupancy and cumulative capacity-wait wall counters");
    std::size_t position{};
    for(const auto [count,total,wall]:{std::array{1,2,7000},std::array{0,2,0},std::array{1,1,3000}}) {
        position=text.find("\"event\":\"frame_spike\"",position);
        require(position!=std::string::npos,"capacity counter fixture emitted each bounded spike");
        const auto line=text.substr(position,text.find('\n',position)-position);++position;
        require(line.find("\"inflight_cap_waits\":"+std::to_string(count))!=std::string::npos&&
            line.find("\"shared_plane_key\":null")!=std::string::npos&&
            line.find("\"shared_fallback_reason\":\"shared_color_unavailable\"")!=std::string::npos&&
            line.find("\"inflight_limit\":1")!=std::string::npos&&
            line.find("\"inflight_cap_waits_total\":"+std::to_string(total))!=std::string::npos&&
            line.find("\"inflight_cap_wait_us\":"+std::to_string(wall))!=std::string::npos,
            "spike counts difference one frame, preserve a failed-output baseline and handle a new owner counter reset");
    }
}
void gpuCopySlowObservation() {
    Fixture f;LogCapture log;f.state.fakeEnabled=true;
    xess_vk::presentation::CopyFenceTiming copy;
    copy.sequence=9;copy.generation=2;copy.engineFrame=77;copy.waitResult=VK_SUCCESS;
    copy.waitEntryNs=1'000'000'000;copy.waitReturnNs=1'030'000'000;
    copy.privateCopyQueryMatch=true;
    auto& sample=copy.privateCopyGpu;
    sample.requested=sample.available=sample.durationKnown=sample.sourceCopyTokenKnown=true;
    sample.queryStateKnown=sample.diagnosticActive=sample.queryReadAttempted=sample.queryResultKnown=sample.queryAvailabilityKnown=true;
    sample.sourceCopyToken=9;sample.generation=2;sample.sequence=4;sample.contextGeneration=3;
    sample.queryResult=VK_SUCCESS;sample.beginAvailability=sample.endAvailability=1;
    sample.validBits=64;sample.timestampPeriodNs=2;sample.beginTicks=100;sample.endTicks=140;sample.durationNs=80;
    copyFenceSlow(&f.state,copy);
    auto text=log.read();
    require(text.find("\"query_match\":true")!=std::string::npos&&text.find("\"diagnostic_active\":true")!=std::string::npos&&
                text.find("\"query_result\":0")!=std::string::npos&&text.find("\"begin_availability\":1")!=std::string::npos&&
                text.find("\"end_availability\":1")!=std::string::npos&&text.find("\"duration_ns\":80")!=std::string::npos,
            "slow fence event carries the matched query, active state, raw result and both availability values");
    require(text.find("\"qpc_alignment_known\":false")!=std::string::npos&&text.find("\"begin_qpc_us\":")==std::string::npos&&
                text.find("\"end_qpc_us\":")==std::string::npos,"uncalibrated slow copy emits no QPC-correlated GPU boundary");
    sample.available=sample.durationKnown=sample.diagnosticActive=false;sample.queryResult=VK_NOT_READY;
    sample.beginAvailability=0;copy.waitReturnNs+=30'000'000;copyFenceSlow(&f.state,copy);
    text=log.read();const auto last=text.substr(text.find_last_of('\n',text.size()-2)+1);
    require(last.find("\"query_match\":true")!=std::string::npos&&last.find("\"query_result\":1")!=std::string::npos&&
                last.find("\"begin_availability\":0")!=std::string::npos&&last.find("\"duration_ns\":")==std::string::npos,
            "matched but unavailable query emits its failure status without an earlier duration");
}
void slowCallObservation() {
    namespace wa=xess_vk::wsi_acquire;
    Fixture f;LogCapture log;xess_vk::QueueGate gate;f.state.queueGate=&gate;
    const auto queue=reinterpret_cast<VkQueue>(&commandDispatch);
    wa::QueueCall fast{1'000'000'000,1'000'000'000,1'016'700'000,VK_SUCCESS};
    wa::QueueCall split{1'000'000'000,1'010'000'000,1'025'000'000,VK_TIMEOUT};
    require(!fast.slow()&&split.slow(),"25ms total threshold excludes normal 60fps and includes split gate/driver delay");
    queueCallSlow(&f.state,"default_off",bits(queue),split);
    require(!logBytes&&!f.state.slowQueueCalls,"default off never emits slow diagnostics");
    f.state.fakeEnabled=true;
    queueCallSlow(&f.state,"fast",bits(queue),fast);
    require(!logBytes&&!f.state.slowQueueCalls,"normal call does not consume slow-event budget");
    const auto result=queueDriver(&f.state,"fixture_driver_submit",queue,[&]{Sleep(30);SetLastError(0x76543210);return VK_TIMEOUT;});
    require(result==VK_TIMEOUT&&GetLastError()==0x76543210,"timed queue driver preserves exact result and LastError");
    bool gateReleased{};std::thread check([&]{gateReleased=gate.try_lock();if(gateReleased)gate.unlock();});check.join();
    require(gateReleased&&f.state.slowQueueCalls==1,"slow queue call has released its gate on return");
    wa::Call acquire;acquire.entryNs=1'000'000'000;acquire.elapsedNs=16'700'000;
    fakeAcquireSlow(&f.state,acquire);require(!f.state.slowAcquireCalls,"normal acquire does not consume slow budget");
    acquire.elapsedNs=wa::slowCallThresholdNs;acquire.timeout=UINT64_MAX;acquire.noSlotWaitCpu.add(10'000'000);
    fakeAcquireSlow(&f.state,acquire);
    xess_vk::presentation::CopyFenceTiming copy;copy.sequence=9;copy.engineFrame=77;copy.queue=bits(queue);copy.submit=split;
    copy.waitEntryNs=1'030'000'000;copy.waitReturnNs=1'055'000'000;copy.waitResult=VK_TIMEOUT;
    copyFenceSlow(&f.state,copy);copyFenceSlow(&f.state,copy);
    require(f.state.slowCopyCalls==1,"same previous copy wait is not emitted twice");
    std::vector<std::thread> writers;
    for(unsigned i=0;i<8;++i)writers.emplace_back([&]{for(unsigned n=0;n<40;++n)queueCallSlow(&f.state,"synthetic_gate",bits(queue),split);});
    for(auto& writer:writers)writer.join();
    const auto text=log.read();
    const auto occurrences=[&](const char* needle){unsigned count{};std::size_t pos{};while((pos=text.find(needle,pos))!=std::string::npos){++count;pos+=std::strlen(needle);}return count;};
    require(f.state.slowQueueCalls==321&&occurrences("\"event\":\"fake_queue_call_slow\"")==wa::slowCallLimit&&
        occurrences("\"event\":\"fake_slow_call_log_limit\"")==1,"concurrent queue records obey one device budget and one limit marker");
    require(occurrences("\"event\":\"fake_acquire_slow\"")==1&&text.find("\"no_slot_wait_cpu\":{\"count\":1,\"total_ns\":10000000")!=std::string::npos&&
        text.find("\"timeout_ns\":\"18446744073709551615\"")!=std::string::npos,"slow acquire keeps bounded no-slot detail and exact MAX timeout");
    require(occurrences("\"event\":\"fake_copy_fence_slow\"")==1&&text.find("\"source_copy_token\":9")!=std::string::npos&&
        text.find("\"source_engine_frame\":77")!=std::string::npos&&text.find("cpu_fence_wait_return_not_gpu_timestamp")!=std::string::npos,
        "copy record retains source sequence/frame and never calls a timeout GPU completion");
}

std::uint64_t excludedDriverNs{};
DeviceState* timingState{};
VKAPI_ATTR void VKAPI_CALL timedUpdate(VkDevice d,std::uint32_t n,const VkWriteDescriptorSet* writes,
                                     std::uint32_t copies,const VkCopyDescriptorSet* values) {
    require(d==device && !n && !writes && !copies && !values,"timed wrapper preserves original descriptor arguments");
    require(TryAcquireSRWLockExclusive(&timingState->trackingCpuLock)!=0,"aggregation lock never spans the driver");
    ReleaseSRWLockExclusive(&timingState->trackingCpuLock);
    const auto start=xess_vk::CpuWallClock::now();
    Sleep(80);
    excludedDriverNs=xess_vk::cpuWallElapsedNs(start);
    SetLastError(0x76543210);
}
void ownCpuTiming() {
    Fixture f(false);
    calibrateTrackingCpu(&f.state);
    require(f.state.trackingEmptyScope.count==64 &&
            f.state.trackingCpu[static_cast<unsigned>(TrackingCpuGroup::Bind)].count==0,
            "bounded empty-scope baseline includes instrumentation without polluting real counters");
    timingState=&f.state;
    f.state.updateDescriptorSets=timedUpdate;
    vkUpdateDescriptorSets(device,0,nullptr,0,nullptr);
    const auto own=f.state.trackingCpu[static_cast<unsigned>(TrackingCpuGroup::DescriptorUpdate)];
    require(own.count==1 && own.totalNs<excludedDriverNs/2 && GetLastError()==0x76543210,
            "actual descriptor wrapper excludes an 80ms original call and preserves its LastError");
    bool threw{};
    try {
        TrackingCpuScope scope(&f.state,TrackingCpuGroup::Recording,xess_vk::CpuWallClock::now());
        scope.driver([]()->int {SetLastError(0x1234);throw std::runtime_error("driver failure");});
    } catch(const std::runtime_error& error) {threw=std::string(error.what())=="driver failure";}
    require(threw && GetLastError()==0x1234 &&
            f.state.trackingCpu[static_cast<unsigned>(TrackingCpuGroup::Recording)].count==1,
            "driver exception passes through and the surrounding own work is counted exactly once");
    std::atomic<unsigned> mismatches{};
    std::array<std::thread,4> threads;
    for(auto& thread:threads) thread=std::thread([&] {
        for(unsigned i=0;i<100;++i) {
            SetLastError(0x4321);
            {TrackingCpuScope scope(&f.state,TrackingCpuGroup::Bind,xess_vk::CpuWallClock::now());scope.driver([] {});}
            if(GetLastError()!=0x4321)++mismatches;
            AcquireSRWLockShared(&f.state.trackingCpuLock);
            const auto snapshot=f.state.trackingCpu[static_cast<unsigned>(TrackingCpuGroup::Bind)];
            if(snapshot.maxNs>snapshot.totalNs)++mismatches;
            ReleaseSRWLockShared(&f.state.trackingCpuLock);
        }
    });
    for(auto& thread:threads)thread.join();
    require(!mismatches && f.state.trackingCpu[static_cast<unsigned>(TrackingCpuGroup::Bind)].count==400,
            "recording threads aggregate and snapshot safely without lost calls or LastError changes");
    const auto first=takeTrackingCpuReport(&f.state),second=takeTrackingCpuReport(&f.state);
    const auto recording=static_cast<unsigned>(TrackingCpuGroup::Recording);
    require(first.peaks[recording].ns&&first.peaks[recording].qpc<=first.endQpc&&first.peaks[recording].thread&&
        !second.peaks[recording].ns&&second.beginQpc==first.endQpc&&second.sequence==first.sequence+1&&
        second.totals[recording].count==first.totals[recording].count&&second.totals[recording].maxNs==first.totals[recording].maxNs,
        "report maxima reset with endpoint timestamps while cumulative time and lifetime maxima are preserved");
}

template<class Object,class Info,class Function>
void checkCreate(Function fn,Tag tag,const Info& info,const VkAllocationCallbacks& allocator){
    for(auto result:{VK_SUCCESS,VK_ERROR_OUT_OF_HOST_MEMORY}){
        Object object{};const auto before=info;chosen=result;returned=0x6000+tag;beginCheck();
        require(fn(device,&info,&allocator,&object)==result,"literal creation result");once(tag);
        require(last.pointers[0]==device&&last.pointers[1]==&info&&last.pointers[2]==&allocator&&last.pointers[3]==&object,"create original pointers");
        require(bits(object)==returned&&std::memcmp(&info,&before,sizeof(info))==0,"driver output and input info preserved on success/failure");
    }
}
template<class Object,class Function>void checkDestroy(Function fn,Tag tag,const VkAllocationCallbacks& allocator){
    const auto object=handle<Object>(0x7000+tag);beginCheck();fn(device,object,&allocator);once(tag);
    require(last.pointers[0]==device&&last.pointers[1]==&allocator&&last.numbers[0]==bits(object),"destroy original object/allocator");
}
void forwarding(){
    Fixture f(false);VkAllocationCallbacks allocator{};allocator.pUserData=&f;
    VkBaseInStructure extension{VK_STRUCTURE_TYPE_MAX_ENUM,nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,&extension};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,&extension};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,&extension};
    checkCreate<VkDescriptorSetLayout>(vkCreateDescriptorSetLayout,CreateLayout,li,allocator);
    checkCreate<VkPipelineLayout>(vkCreatePipelineLayout,CreatePL,pi,allocator);
    checkCreate<VkDescriptorPool>(vkCreateDescriptorPool,CreatePool,pool,allocator);
    checkDestroy<VkDescriptorSetLayout>(vkDestroyDescriptorSetLayout,DestroyLayout,allocator);
    checkDestroy<VkPipelineLayout>(vkDestroyPipelineLayout,DestroyPL,allocator);
    checkDestroy<VkDescriptorPool>(vkDestroyDescriptorPool,DestroyPool,allocator);
    VkDescriptorUpdateTemplateCreateInfo templateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,&extension};
    checkCreate<VkDescriptorUpdateTemplate>(vkCreateDescriptorUpdateTemplate,CreateTemplate,templateInfo,allocator);
    checkCreate<VkDescriptorUpdateTemplate>(vkCreateDescriptorUpdateTemplateKHR,CreateTemplateKhr,templateInfo,allocator);
    checkDestroy<VkDescriptorUpdateTemplate>(vkDestroyDescriptorUpdateTemplate,DestroyTemplate,allocator);
    checkDestroy<VkDescriptorUpdateTemplate>(vkDestroyDescriptorUpdateTemplateKHR,DestroyTemplateKhr,allocator);
    const auto dp=handle<VkDescriptorPool>(0x8100);const auto set=handle<VkDescriptorSet>(0x8200);const auto layout=handle<VkPipelineLayout>(0x8300);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,&extension};ai.descriptorPool=dp;ai.descriptorSetCount=1;
    for(auto result:{VK_SUCCESS,VK_ERROR_OUT_OF_HOST_MEMORY}){
        chosen=result;VkDescriptorSet out{};beginCheck();require(vkAllocateDescriptorSets(device,&ai,&out)==result,"allocate result preserved");once(AllocateSets);
        require(last.pointers[1]==&ai&&last.pointers[2]==&out&&bits(out)==returned,"allocate pointers/output preserved");
        beginCheck();require(vkFreeDescriptorSets(device,dp,1,&set)==result,"free result preserved");once(FreeSets);
        require(last.pointers[1]==&set&&last.numbers[0]==bits(dp)&&last.numbers[1]==1,"free exact arguments");
        beginCheck();require(vkResetDescriptorPool(device,dp,0x1234)==result,"reset pool result preserved");once(ResetPool);require(last.numbers[1]==0x1234,"reset flags preserved");
    }
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};std::uint32_t dynamic[2]{5,19};
    beginCheck();vkUpdateDescriptorSets(device,1,&write,1,&copy);once(UpdateSets);require(last.pointers[1]==&write&&last.pointers[2]==&copy&&last.numbers[0]==1&&last.numbers[1]==1,"write/copy arrays unchanged");
    beginCheck();vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,3,1,&set,2,dynamic);once(BindSets);
    require(last.pointers[0]==command&&last.pointers[1]==&set&&last.pointers[2]==dynamic&&last.numbers[0]==VK_PIPELINE_BIND_POINT_GRAPHICS&&last.numbers[1]==bits(layout)&&last.numbers[2]==3&&last.numbers[3]==1&&last.numbers[4]==2,"bind exact arguments including graphics and offsets");
    const auto update=handle<VkDescriptorUpdateTemplate>(0x8400);
    auto templateCase=[&](auto fn,Tag tag){beginCheck();fn(device,set,update,dynamic);once(tag);require(last.pointers[1]==dynamic&&last.numbers[0]==bits(set)&&last.numbers[1]==bits(update),"template alias original fields");};
    templateCase(vkUpdateDescriptorSetWithTemplate,Template);templateCase(vkUpdateDescriptorSetWithTemplateKHR,TemplateKhr);
    auto pushCase=[&](auto fn,Tag tag){beginCheck();fn(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,5,1,&write);once(tag);require(last.pointers[1]==&write&&last.numbers[0]==VK_PIPELINE_BIND_POINT_GRAPHICS&&last.numbers[1]==bits(layout)&&last.numbers[2]==5&&last.numbers[3]==1,"push alias exact fields");};
    pushCase(vkCmdPushDescriptorSet,Push);pushCase(vkCmdPushDescriptorSetKHR,PushKhr);
    auto pushTemplateCase=[&](auto fn,Tag tag){beginCheck();fn(command,update,layout,6,dynamic);once(tag);require(last.pointers[1]==dynamic&&last.numbers[0]==bits(update)&&last.numbers[1]==bits(layout)&&last.numbers[2]==6,"push-template alias exact fields");};
    pushTemplateCase(vkCmdPushDescriptorSetWithTemplate,PushTemplate);pushTemplateCase(vkCmdPushDescriptorSetWithTemplateKHR,PushTemplateKhr);
    VkBindDescriptorSetsInfo bi{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO};VkPushDescriptorSetInfo qi{VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO};VkPushDescriptorSetWithTemplateInfo ti{VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO};
    auto infoCase=[&](auto fn,Tag tag,const auto& info){beginCheck();fn(command,&info);once(tag);require(last.pointers[0]==command&&last.pointers[1]==&info,"v2 core/KHR exact info pointer");};
    infoCase(vkCmdBindDescriptorSets2,Bind2,bi);infoCase(vkCmdBindDescriptorSets2KHR,Bind2Khr,bi);
    infoCase(vkCmdPushDescriptorSet2,Push2,qi);infoCase(vkCmdPushDescriptorSet2KHR,Push2Khr,qi);
    infoCase(vkCmdPushDescriptorSetWithTemplate2,PushTemplate2,ti);infoCase(vkCmdPushDescriptorSetWithTemplate2KHR,PushTemplate2Khr,ti);
    const std::array<std::pair<const char*,PFN_vkVoidFunction>,28> hooks{{
#define PAIR(fn) {#fn,reinterpret_cast<PFN_vkVoidFunction>(fn)}
        PAIR(vkCreateDescriptorSetLayout),PAIR(vkDestroyDescriptorSetLayout),PAIR(vkCreatePipelineLayout),PAIR(vkDestroyPipelineLayout),
        PAIR(vkCreateDescriptorPool),PAIR(vkResetDescriptorPool),PAIR(vkDestroyDescriptorPool),PAIR(vkAllocateDescriptorSets),PAIR(vkFreeDescriptorSets),
        PAIR(vkUpdateDescriptorSets),PAIR(vkCmdBindDescriptorSets),PAIR(vkUpdateDescriptorSetWithTemplate),PAIR(vkUpdateDescriptorSetWithTemplateKHR),
        PAIR(vkCreateDescriptorUpdateTemplate),PAIR(vkDestroyDescriptorUpdateTemplate),PAIR(vkCreateDescriptorUpdateTemplateKHR),PAIR(vkDestroyDescriptorUpdateTemplateKHR),
        PAIR(vkCmdPushDescriptorSet),PAIR(vkCmdPushDescriptorSetKHR),PAIR(vkCmdPushDescriptorSetWithTemplate),PAIR(vkCmdPushDescriptorSetWithTemplateKHR),
        PAIR(vkCmdBindDescriptorSets2),PAIR(vkCmdBindDescriptorSets2KHR),PAIR(vkCmdPushDescriptorSet2),PAIR(vkCmdPushDescriptorSet2KHR),
        PAIR(vkCmdPushDescriptorSetWithTemplate2),PAIR(vkCmdPushDescriptorSetWithTemplate2KHR),PAIR(vkCmdDispatch)
#undef PAIR
    }};
    for(const auto& [name,fn]:hooks){unavailable=nullptr;require(vkGetDeviceProcAddr(device,name)==fn&&vkGetInstanceProcAddr(instance,name)==fn,"available wrapper exposed");
        unavailable=name;require(vkGetDeviceProcAddr(device,name)==nullptr&&vkGetInstanceProcAddr(instance,name)==nullptr,"missing downstream alias not invented");}
    unavailable=nullptr;require(vkGetDeviceProcAddr(device,"vkFixtureUnknownEXT")==reinterpret_cast<PFN_vkVoidFunction>(unknown),"unknown entry forwarded");
}

struct Resources {
    VkImage image{};VkImageView view{};VkDescriptorSetLayout layout{};VkPipelineLayout pipelineLayout{};VkDescriptorPool pool{};VkDescriptorSet set{};VkPipeline pipeline{};VkCommandPool commandPool{};
};
VkImageCreateInfo imageInfo(){VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};i.imageType=VK_IMAGE_TYPE_2D;i.format=VK_FORMAT_R8G8B8A8_UNORM;i.extent={64,32,1};i.mipLevels=i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;i.sharingMode=VK_SHARING_MODE_EXCLUSIVE;return i;}
Resources setup(Fixture& f){
    Resources r;chosen=VK_SUCCESS;auto ii=imageInfo();returned=0x10001;require(vkCreateImage(device,&ii,nullptr,&r.image)==VK_SUCCESS,"image create");
    auto record=f.inventory->lookup(bits(r.image));require(record.status==xess_vk::image_inventory::Status::LiveHandleObserved&&di::stats(f.state.descriptors).metadata.liveImages==1,"ordinary image enters common generation and descriptor inventory");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=r.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};returned=0x10002;require(vkCreateImageView(device,&vi,nullptr,&r.view)==VK_SUCCESS,"view create");
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=1;li.pBindings=&binding;returned=0x10003;require(vkCreateDescriptorSetLayout(device,&li,nullptr,&r.layout)==VK_SUCCESS,"layout create");
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&r.layout;returned=0x10004;require(vkCreatePipelineLayout(device,&pi,nullptr,&r.pipelineLayout)==VK_SUCCESS,"pipeline layout create");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,4};VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpi.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;dpi.maxSets=4;dpi.poolSizeCount=1;dpi.pPoolSizes=&size;returned=0x10005;require(vkCreateDescriptorPool(device,&dpi,nullptr,&r.pool)==VK_SUCCESS,"descriptor pool create");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=r.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&r.layout;returned=0x10006;require(vkAllocateDescriptorSets(device,&ai,&r.set)==VK_SUCCESS,"set allocate");
    VkDescriptorImageInfo image{VK_NULL_HANDLE,r.view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=r.set;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;w.pImageInfo=&image;vkUpdateDescriptorSets(device,1,&w,0,nullptr);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=r.pipelineLayout;ci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
    const std::array<VkComputePipelineCreateInfo,2> infos{ci,ci};std::array<VkPipeline,2> outputs{};returned=0x10007;chosen=VK_ERROR_OUT_OF_HOST_MEMORY;
    require(vkCreateComputePipelines(device,handle<VkPipelineCache>(0x9999),2,infos.data(),nullptr,outputs.data())==chosen,"partial pipeline failure forwarded");
    r.pipeline=outputs[0];require(r.pipeline&&outputs[1]==VK_NULL_HANDLE&&di::stats(f.state.descriptors).metadata.livePipelines==1,"nonnull partial pipeline registered despite batch failure");chosen=VK_SUCCESS;
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;returned=0x10008;require(vkCreateCommandPool(device,&cp,nullptr,&r.commandPool)==VK_SUCCESS,"command pool create");
    VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cb.commandPool=r.commandPool;cb.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cb.commandBufferCount=1;VkCommandBuffer out{};require(vkAllocateCommandBuffers(device,&cb,&out)==VK_SUCCESS&&out==command,"command allocation wrapper");
    return r;
}
void record(const Resources& r){VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};chosen=VK_SUCCESS;require(vkBeginCommandBuffer(command,&bi)==VK_SUCCESS,"begin wrapper");vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipelineLayout,0,1,&r.set,0,nullptr);}
void direct(Fixture& f,bool accepted){
    const auto before=di::stats(f.state.descriptors);expectedDispatches=before.metadata.dispatches+1;checkPreDispatch=true;beginCheck();vkCmdDispatch(command,3,4,5);checkPreDispatch=false;once(DispatchDirect);
    require(last.pointers[0]==command&&last.numbers[0]==3&&last.numbers[1]==4&&last.numbers[2]==5,"direct dispatch unchanged");const auto after=di::stats(f.state.descriptors);
    require(after.metadata.dispatches==expectedDispatches&&after.metadata.acceptedDispatches==before.metadata.acceptedDispatches+(accepted?1:0),"metadata acceptance matches expected lifetime");
}

void selectedDispatches(){
    require(!descriptorDispatchConfig(false,false).continuousDispatchSelection&&descriptorDispatchConfig(true,false).continuousDispatchSelection&&
        !descriptorDispatchConfig(true,true).continuousDispatchSelection,"same product Config selects only product without render-tail");
    Fixture f(true,true);auto r=setup(f);record(r);const auto before=di::stats(f.state.descriptors);
    beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);
    auto after=di::stats(f.state.descriptors);
    require(after.metadata.filteredDispatches==1&&after.metadata.acceptedDispatches==before.metadata.acceptedDispatches&&
        after.metadata.refusedDispatches==before.metadata.refusedDispatches&&last.numbers[0]==3&&last.numbers[1]==4&&last.numbers[2]==5,
        "actual product wrapper filters ordinary evidence while forwarding one unchanged driver dispatch");
    const std::array<VkDescriptorSetLayoutBinding,2> bindings{{{5,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{7,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=bindings.size();li.pBindings=bindings.data();VkDescriptorSetLayout layout{};returned=0x11003;
    require(vkCreateDescriptorSetLayout(device,&li,nullptr,&layout)==VK_SUCCESS,"selected candidate layout through real wrapper");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=r.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;VkDescriptorSet set{};returned=0x11006;
    require(vkAllocateDescriptorSets(device,&ai,&set)==VK_SUCCESS,"candidate allocation preserved before pipeline exists");
    VkDescriptorImageInfo image{VK_NULL_HANDLE,r.view,VK_IMAGE_LAYOUT_GENERAL};std::array<VkWriteDescriptorSet,2> writes{};
    for(std::size_t i=0;i<writes.size();++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;w.dstBinding=i?7:5;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;w.pImageInfo=&image;}
    vkUpdateDescriptorSets(device,writes.size(),writes.data(),0,nullptr);
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&layout;VkPipelineLayout pipelineLayout{};returned=0x11004;
    require(vkCreatePipelineLayout(device,&pi,nullptr,&pipelineLayout)==VK_SUCCESS,"selected candidate pipeline layout");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=pipelineLayout;ci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;VkPipeline pipeline{};returned=0x11007;
    require(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline)==VK_SUCCESS,"selected candidate pipeline");
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    beginCheck();vkCmdDispatch(command,3,4,5);once(DispatchDirect);after=di::stats(f.state.descriptors);
    require(after.metadata.acceptedDispatches==1&&after.metadata.filteredDispatches==1&&!after.metadata.refusedDispatches&&after.metadata.liveSets==2&&
        after.metadata.driverLiveSets==2,"actual selected candidate accepts pre-pipeline writes and pre-pipeline binding, keeping ordinary set history");
    const auto evidence=di::preDispatch(f.state.descriptors,command,3,4,5);
    require(evidence.metadataAccepted&&!evidence.filteredOut&&evidence.recordOrdinal==3&&evidence.count==2&&evidence.bindings[0].binding==5&&evidence.bindings[1].binding==7,
        "selected real hook path preserves dispatch ordinal and both candidate bindings");
}
void trackingCpuEnvironment() {
    for (const auto* value : {static_cast<const wchar_t*>(nullptr), L"", L"0", L"true", L"01", L"11", L"1 ", L"1"}) {
        SetEnvironmentVariableW(L"XESS_TRACKING_CPU_TIMING", value);
        SetLastError(0x12345678);
        require(trackingCpuTimingRequested() == (value && std::wcscmp(value,L"1")==0) &&
                GetLastError()==0x12345678,"tracking timing accepts only exact 1 and preserves LastError");
    }
    SetEnvironmentVariableW(L"XESS_TRACKING_CPU_TIMING", nullptr);
    require(!trackingCpuTimingEnabled(),"tracking timing is disabled by default");
    SetEnvironmentVariableW(L"XESS_TRACKING_CPU_TIMING", L"1");
    require(!trackingCpuTimingEnabled(),"tracking timing is frozen instead of following environment changes");
    SetEnvironmentVariableW(L"XESS_TRACKING_CPU_TIMING", nullptr);
    trackingCpuTimingOverride=1; // The rest of this legacy fixture exercises enabled counters.
}
template<Tag tag> VKAPI_ATTR VkResult VKAPI_CALL timingSubmit2(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* infos,VkFence fence) {
    hit(tag);last.pointers={queue,infos};last.numbers={count,bits(fence)};return chosen;
}
void trackingCpuModes() {
    for (const bool enabled : {false,true}) {
        trackingCpuTimingOverride=enabled;
        Fixture f;
        const auto r=setup(f);
        f.state.trackingCpu={};
        TrackingCpuAudit audit;
        trackingCpuAudit=&audit;
        calibrateTrackingCpu(&f.state);
        require(enabled ? audit.clocks>0 && audit.counterLocks==64 && f.state.trackingEmptyScope.count==64 :
                          !audit.clocks && !audit.counterLocks && !f.state.trackingEmptyScope.count,
                "disabled startup calibration reads no clock and acquires no aggregation lock");
        const auto calibration=audit;
        audit={};
        record(r);direct(f,true);
        VkDescriptorImageInfo image{VK_NULL_HANDLE,r.view,VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=r.set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&image;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        direct(f,false); // Updating a descriptor still invalidates an old recording when timing is OFF.
        record(r);direct(f,true);
        const auto queue=reinterpret_cast<VkQueue>(device);
        f.state.submit2=timingSubmit2<GatedSubmit2>;f.state.submit2Khr=timingSubmit2<GatedSubmit2Khr>;
        for(const bool khr : {false,true}) {
            beginCheck();
            require((khr?vkQueueSubmit2KHR(queue,0,nullptr,VK_NULL_HANDLE):vkQueueSubmit2(queue,0,nullptr,VK_NULL_HANDLE))==VK_SUCCESS,
                    "both actual Submit2 aliases preserve the original result in each timing mode");
            once(khr?GatedSubmit2Khr:GatedSubmit2);
            require(last.pointers[0]==queue && !last.pointers[1] && !last.numbers[0] && !last.numbers[1],
                    "both actual Submit2 aliases preserve the original empty-submit arguments");
        }
        const auto snapshot=trackingCpuSnapshot(&f.state);
        require(enabled ? audit.clocks==4*audit.counterLocks && audit.counterLocks>0 && audit.snapshotLocks==1 &&
                          snapshot[static_cast<unsigned>(TrackingCpuGroup::Dispatch)].count==3 :
                          !audit.clocks && !audit.counterLocks && !audit.snapshotLocks &&
                          snapshot[static_cast<unsigned>(TrackingCpuGroup::Dispatch)].count==0,
                "actual wrappers bypass all timing clocks/locks OFF and retain the original four-clock accounting ON");
        const auto stats=di::stats(f.state.descriptors).metadata;
        require(stats.acceptedDispatches==2 && stats.refusedDispatches==1,
                "timer mode never disables descriptor generation, binding, recording or dispatch tracking");
        std::cout << "{\"stage\":\"tracking-cpu-timing-wrapper-audit\",\"passed\":true,\"enabled\":"
                  << (enabled?"true":"false") << ",\"clocks\":" << audit.clocks
                  << ",\"counterLocks\":" << audit.counterLocks << ",\"snapshotLocks\":" << audit.snapshotLocks
                  << ",\"calibrationClocks\":" << calibration.clocks << ",\"calibrationCounterLocks\":" << calibration.counterLocks
                  << ",\"metadataAcceptedDispatches\":" << stats.acceptedDispatches
                  << ",\"metadataRefusedDispatches\":" << stats.refusedDispatches << ",\"realVulkanCalls\":0}\n";
        trackingCpuAudit=nullptr;
    }
    trackingCpuTimingOverride=1;
}
std::uint64_t benchmarkDriverCalls{};
VKAPI_ATTR void VKAPI_CALL benchmarkUpdate(VkDevice,std::uint32_t,const VkWriteDescriptorSet*,std::uint32_t,const VkCopyDescriptorSet*) {
    ++benchmarkDriverCalls;SetLastError(0x76543210);
}
VKAPI_ATTR VkResult VKAPI_CALL benchmarkBegin(VkCommandBuffer,const VkCommandBufferBeginInfo*) {
    ++benchmarkDriverCalls;SetLastError(0x76543210);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL benchmarkEnd(VkCommandBuffer) {
    ++benchmarkDriverCalls;SetLastError(0x76543210);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL benchmarkBind(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,std::uint32_t,std::uint32_t,const VkDescriptorSet*,std::uint32_t,const std::uint32_t*) {
    ++benchmarkDriverCalls;SetLastError(0x76543210);
}
void trackingCpuBenchmark() {
    constexpr unsigned pairs=8,iterations=20000,wrappersPerIteration=4;
    LARGE_INTEGER frequency{};
    require(QueryPerformanceFrequency(&frequency)!=0 && frequency.QuadPart>0,"independent benchmark QPC frequency");
    trackingCpuAudit=nullptr; // Audit counters are not part of the measured workload.
    std::array<std::array<double,2>,pairs> nsPerWrapper{};
    for (unsigned pair=0;pair<pairs;++pair) for (unsigned order=0;order<2;++order) {
        const bool enabled=(pair+order)%2;
        trackingCpuTimingOverride=enabled;
        Fixture f;
        const auto r=setup(f);
        f.state.updateDescriptorSets=benchmarkUpdate;f.state.beginCommandBuffer=benchmarkBegin;
        f.state.endCommandBuffer=benchmarkEnd;f.state.cmdBindDescriptorSets=benchmarkBind;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkDescriptorImageInfo image{VK_NULL_HANDLE,r.view,VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=r.set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&image;
        const auto workload=[&](unsigned count) {
            for(unsigned i=0;i<count;++i) {
                vkUpdateDescriptorSets(device,1,&write,0,nullptr);
                vkBeginCommandBuffer(command,&begin);
                vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipelineLayout,0,1,&r.set,0,nullptr);
                vkEndCommandBuffer(command);
            }
        };
        workload(1000);
        const auto calls=benchmarkDriverCalls;
        LARGE_INTEGER start{},end{};
        QueryPerformanceCounter(&start);workload(iterations);QueryPerformanceCounter(&end);
        require(benchmarkDriverCalls-calls==iterations*wrappersPerIteration && GetLastError()==0x76543210,
                "independent benchmark forwards the same fixed workload and LastError in both modes");
        nsPerWrapper[pair][enabled]=double(end.QuadPart-start.QuadPart)*1e9/double(frequency.QuadPart)/
                                   double(iterations*wrappersPerIteration);
    }
    trackingCpuTimingOverride=1;
    std::cout << "{\"stage\":\"tracking-cpu-timing-outer-qpc\",\"passed\":true,\"pairs\":" << pairs
              << ",\"wrappersPerBatch\":" << iterations*wrappersPerIteration
              << ",\"qpcFrequency\":" << frequency.QuadPart
              << ",\"auditCountersDuringBenchmark\":false,\"realVulkanCalls\":0,\"gameTested\":false"
              << ",\"workload\":\"update-one-image-descriptor/begin/bind/end using production wrappers and owned stubs\""
              << ",\"measurementScope\":\"outer wall time includes driver stubs and metadata; not a game performance estimate\",\"samples\":[";
    for(unsigned pair=0;pair<pairs;++pair) {
        if(pair)std::cout << ',';
        std::cout << "{\"pair\":" << pair+1 << ",\"offNsPerWrapper\":" << nsPerWrapper[pair][0]
                  << ",\"onNsPerWrapper\":" << nsPerWrapper[pair][1]
                  << ",\"onMinusOffNsPerWrapper\":" << nsPerWrapper[pair][1]-nsPerWrapper[pair][0] << '}';
    }
    std::cout << "]}\n";
}
void lifetime(){
    Fixture f;auto r=setup(f);record(r);direct(f,true);
    chosen=VK_ERROR_OUT_OF_HOST_MEMORY;beginCheck();require(vkFreeDescriptorSets(device,r.pool,1,&r.set)==chosen,"failed free result");once(FreeSets);require(di::stats(f.state.descriptors).metadata.liveSets==1,"failed free retains set");direct(f,true);
    beginCheck();require(vkResetDescriptorPool(device,r.pool,0)==chosen,"failed pool reset result");once(ResetPool);direct(f,true);
    chosen=VK_SUCCESS;VkDescriptorImageInfo image{VK_NULL_HANDLE,r.view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=r.set;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;w.pImageInfo=&image;vkUpdateDescriptorSets(device,1,&w,0,nullptr);direct(f,false);
    require(di::stats(f.state.descriptors).metadata.observedIssues&dm::bit(dm::Issue::DescriptorChanged),"ordinary update refusal visible");record(r);direct(f,true);
    auto previous=f.inventory->lookup(bits(r.image)).record;vkDestroyImage(device,r.image,nullptr);require(di::stats(f.state.descriptors).metadata.liveImages==0,"destroy retires descriptor image");direct(f,false);
    auto ii=imageInfo();returned=bits(r.image);require(vkCreateImage(device,&ii,nullptr,&r.image)==VK_SUCCESS,"reused image handle create");auto newer=f.inventory->lookup(bits(r.image)).record;require(newer.generation>previous.generation,"image reuse uses shared new generation");record(r);direct(f,false);
    chosen=VK_SUCCESS;require(vkFreeDescriptorSets(device,r.pool,1,&r.set)==VK_SUCCESS,"successful free");require(di::stats(f.state.descriptors).metadata.liveSets==0,"successful free removes set");
    require(vkResetCommandPool(device,r.commandPool,0)==VK_SUCCESS,"command pool reset");direct(f,false);
    vkFreeCommandBuffers(device,r.commandPool,1,&command);require(di::stats(f.state.descriptors).metadata.liveCommands==0,"command free removes lifecycle");
    vkDestroyCommandPool(device,r.commandPool,nullptr);vkDestroyPipeline(device,r.pipeline,nullptr);vkDestroyImageView(device,r.view,nullptr);vkDestroyDescriptorPool(device,r.pool,nullptr);vkDestroyPipelineLayout(device,r.pipelineLayout,nullptr);vkDestroyDescriptorSetLayout(device,r.layout,nullptr);
    const auto s=di::stats(f.state.descriptors).metadata;require(s.liveCommands==0&&s.liveCommandPools==0&&s.livePipelines==0&&s.liveViews==0&&s.livePools==0&&s.livePipelineLayouts==0&&s.liveSetLayouts==0,"destroy wrappers retire all dependent metadata objects");
}
void dispatchVariants(){
    Fixture f;auto r=setup(f);
    auto run=[&](auto invoke,Tag tag){record(r);expectedDispatches=di::stats(f.state.descriptors).metadata.dispatches+1;checkPreDispatch=true;beginCheck();invoke();checkPreDispatch=false;once(tag);require(di::stats(f.state.descriptors).metadata.acceptedDispatches==0,"unsupported dispatch never accepted");};
    run([&]{vkCmdDispatchIndirect(command,handle<VkBuffer>(0x5555),UINT64_C(0x123456789));},DispatchIndirect);require(last.numbers[0]==0x5555&&last.numbers[1]==UINT64_C(0x123456789),"indirect offset preserves 64 bits");
    run([&]{vkCmdDispatchBase(command,11,12,13,21,22,23);},DispatchBase);require(last.numbers==std::array<std::uint64_t,8>{11,12,13,21,22,23,0,0},"core base all six counts preserved");
    run([&]{vkCmdDispatchBaseKHR(command,31,32,33,41,42,43);},DispatchBaseKhr);require(last.numbers==std::array<std::uint64_t,8>{31,32,33,41,42,43,0,0},"KHR base uses separate original slot and six counts");
    require(f.state.trackingCpu[static_cast<unsigned>(TrackingCpuGroup::Dispatch)].count==3 &&
            f.state.trackingCpu[static_cast<unsigned>(TrackingCpuGroup::Bind)].count==6 &&
            f.state.trackingCpu[static_cast<unsigned>(TrackingCpuGroup::DescriptorUpdate)].count==1 &&
            f.state.trackingCpu[static_cast<unsigned>(TrackingCpuGroup::DescriptorLifetime)].count>0 &&
            f.state.trackingCpu[static_cast<unsigned>(TrackingCpuGroup::Recording)].count==5,
            "actual core/KHR wrappers classify dispatch, bind, descriptor and recording own work independently");
}
void ordinaryTemplates(){
    Fixture f;auto r=setup(f);record(r);direct(f,true);
    VkDescriptorUpdateTemplateEntry entry{0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3,sizeof(VkDescriptorImageInfo)+5};
    VkDescriptorUpdateTemplateCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
    info.descriptorUpdateEntryCount=1;info.pDescriptorUpdateEntries=&entry;info.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;info.descriptorSetLayout=r.layout;
    VkDescriptorUpdateTemplate update{};returned=0x10051;chosen=VK_ERROR_OUT_OF_HOST_MEMORY;
    const auto before=di::stats(f.state.descriptors).metadata;beginCheck();
    require(vkCreateDescriptorUpdateTemplate(device,&info,nullptr,&update)==chosen,"failed template creation original result");once(CreateTemplate);
    require(di::stats(f.state.descriptors).metadata.liveTemplates==0&&di::stats(f.state.descriptors).metadata.failedDriverCalls==before.failedDriverCalls+1,"failed template creates no metadata object");
    chosen=VK_SUCCESS;beginCheck();require(vkCreateDescriptorUpdateTemplate(device,&info,nullptr,&update)==VK_SUCCESS,"core ordinary template create");once(CreateTemplate);
    require(di::stats(f.state.descriptors).metadata.liveTemplates==1,"successful template copied into metadata");
    VkDescriptorImageInfo image{VK_NULL_HANDLE,r.view,VK_IMAGE_LAYOUT_GENERAL};std::array<std::byte,3+sizeof(image)> data{};std::memcpy(data.data()+3,&image,sizeof(image));
    entry.offset=9999;beginCheck();vkUpdateDescriptorSetWithTemplate(device,r.set,update,data.data());once(Template);
    require(di::stats(f.state.descriptors).metadata.coreTemplateUpdateCalls==1&&di::stats(f.state.descriptors).metadata.templateUpdateRefusals==0,"core wrapper decodes unaligned data using owned template entries");
    direct(f,false);require(di::stats(f.state.descriptors).metadata.observedIssues&dm::bit(dm::Issue::DescriptorChanged),"actual template mutation invalidates existing recording");record(r);direct(f,true);
    beginCheck();vkDestroyDescriptorUpdateTemplate(device,update,nullptr);once(DestroyTemplate);require(di::stats(f.state.descriptors).metadata.liveTemplates==0,"core template destroy removes only template object");direct(f,true);
    entry.offset=3;returned=0x10052;beginCheck();require(vkCreateDescriptorUpdateTemplateKHR(device,&info,nullptr,&update)==VK_SUCCESS,"KHR ordinary template create");once(CreateTemplateKhr);
    beginCheck();vkUpdateDescriptorSetWithTemplateKHR(device,r.set,update,data.data());once(TemplateKhr);record(r);direct(f,true);
    auto* unreadable=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);require(unreadable!=nullptr,"self-owned unreadable reader fixture allocation");
    const auto reads=di::stats(f.state.descriptors).metadata.templateReadRefusals;beginCheck();vkUpdateDescriptorSetWithTemplateKHR(device,r.set,update,unreadable);once(TemplateKhr);
    require(di::stats(f.state.descriptors).metadata.templateReadRefusals==reads+1&&di::stats(f.state.descriptors).metadata.templateUpdateRefusals==1,"failed exact typed read has specific refusal counters after one original call");require(VirtualFree(unreadable,0,MEM_RELEASE)!=0,"reader fixture memory released");direct(f,false);
    beginCheck();vkUpdateDescriptorSetWithTemplateKHR(device,r.set,update,data.data());once(TemplateKhr);record(r);direct(f,false);
    require(vkFreeDescriptorSets(device,r.pool,1,&r.set)==VK_SUCCESS,"refused set retirement");VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=r.pool;allocation.descriptorSetCount=1;allocation.pSetLayouts=&r.layout;returned=bits(r.set);
    require(vkAllocateDescriptorSets(device,&allocation,&r.set)==VK_SUCCESS,"new set generation after refusal");beginCheck();vkUpdateDescriptorSetWithTemplateKHR(device,r.set,update,data.data());once(TemplateKhr);record(r);direct(f,true);
    beginCheck();vkDestroyDescriptorUpdateTemplateKHR(device,update,nullptr);once(DestroyTemplateKhr);direct(f,true);
    const auto final=di::stats(f.state.descriptors).metadata;require(final.khrTemplateUpdateCalls==4&&final.liveTemplates==0,"KHR calls counted separately and no template retained");
}
void graphicsIsolation(){
    Fixture f;auto r=setup(f);
    VkDescriptorImageInfo image{VK_NULL_HANDLE,r.view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=r.set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&image;
    auto run=[&](auto invoke,Tag tag,bool accepted){record(r);beginCheck();invoke();once(tag);direct(f,accepted);};
    for(auto point:{VK_PIPELINE_BIND_POINT_GRAPHICS,VK_PIPELINE_BIND_POINT_COMPUTE}){
        const bool accepted=point==VK_PIPELINE_BIND_POINT_GRAPHICS;
        run([&]{vkCmdPushDescriptorSet(command,point,r.pipelineLayout,0,1,&write);},Push,accepted);
        run([&]{vkCmdPushDescriptorSetKHR(command,point,r.pipelineLayout,0,1,&write);},PushKhr,accepted);
    }
    VkBindDescriptorSetsInfo bind{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO};bind.layout=r.pipelineLayout;bind.descriptorSetCount=1;bind.pDescriptorSets=&r.set;
    VkPushDescriptorSetInfo pushInfo{VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO};pushInfo.layout=r.pipelineLayout;pushInfo.descriptorWriteCount=1;pushInfo.pDescriptorWrites=&write;
    for(auto stages:{VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT),VkShaderStageFlags(VK_SHADER_STAGE_TASK_BIT_EXT|VK_SHADER_STAGE_MESH_BIT_EXT),VkShaderStageFlags(VK_SHADER_STAGE_COMPUTE_BIT),VkShaderStageFlags(0)}){
        const bool accepted=stages!=0&&!(stages&VK_SHADER_STAGE_COMPUTE_BIT);bind.stageFlags=pushInfo.stageFlags=stages;
        run([&]{vkCmdBindDescriptorSets2(command,&bind);},Bind2,accepted);
        run([&]{vkCmdBindDescriptorSets2KHR(command,&bind);},Bind2Khr,accepted);
        run([&]{vkCmdPushDescriptorSet2(command,&pushInfo);},Push2,accepted);
        run([&]{vkCmdPushDescriptorSet2KHR(command,&pushInfo);},Push2Khr,accepted);
    }
    run([&]{vkCmdBindDescriptorSets2(command,nullptr);},Bind2,false);
    run([&]{vkCmdBindDescriptorSets2KHR(command,nullptr);},Bind2Khr,false);
    run([&]{vkCmdPushDescriptorSet2(command,nullptr);},Push2,false);
    run([&]{vkCmdPushDescriptorSet2KHR(command,nullptr);},Push2Khr,false);
    const auto update=handle<VkDescriptorUpdateTemplate>(0x8400);std::uint32_t data{};
    run([&]{vkCmdPushDescriptorSetWithTemplate(command,update,r.pipelineLayout,0,&data);},PushTemplate,false);
    run([&]{vkCmdPushDescriptorSetWithTemplateKHR(command,update,r.pipelineLayout,0,&data);},PushTemplateKhr,false);
    VkPushDescriptorSetWithTemplateInfo templateInfo{VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO};templateInfo.descriptorUpdateTemplate=update;templateInfo.layout=r.pipelineLayout;templateInfo.pData=&data;
    run([&]{vkCmdPushDescriptorSetWithTemplate2(command,&templateInfo);},PushTemplate2,false);
    run([&]{vkCmdPushDescriptorSetWithTemplate2KHR(command,&templateInfo);},PushTemplate2Khr,false);
}
unsigned tailCalls{},tailTag{};const void* tailInfoPointer{};std::array<std::uint64_t,8> tailArguments{};
template<unsigned tag,class Info>VKAPI_ATTR void VKAPI_CALL tailInfo(VkCommandBuffer b,const Info* info){++tailCalls;tailTag=tag;tailInfoPointer=info;tailArguments[0]=bits(b);SetLastError(0x7132);}
VKAPI_ATTR void VKAPI_CALL tailCopy(VkCommandBuffer b,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,std::uint32_t count,const VkImageCopy* regions){++tailCalls;tailTag=1;tailInfoPointer=regions;tailArguments={bits(b),bits(src),std::uint64_t(sl),bits(dst),std::uint64_t(dl),count};SetLastError(0x7132);}
VKAPI_ATTR void VKAPI_CALL tailBlit(VkCommandBuffer b,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,std::uint32_t count,const VkImageBlit* regions,VkFilter filter){++tailCalls;tailTag=4;tailInfoPointer=regions;tailArguments={bits(b),bits(src),std::uint64_t(sl),bits(dst),std::uint64_t(dl),count,std::uint64_t(filter)};SetLastError(0x7132);}
VKAPI_ATTR void VKAPI_CALL tailDraw(VkCommandBuffer b,std::uint32_t v,std::uint32_t i,std::uint32_t fv,std::uint32_t fi){++tailCalls;tailTag=7;tailArguments={bits(b),v,i,fv,fi};SetLastError(0x7132);}
VKAPI_ATTR void VKAPI_CALL tailIndexed(VkCommandBuffer b,std::uint32_t v,std::uint32_t i,std::uint32_t first,std::int32_t offset,std::uint32_t fi){++tailCalls;tailTag=8;tailArguments={bits(b),v,i,first,std::uint64_t(std::int64_t(offset)),fi};SetLastError(0x7132);}
template<unsigned tag>VKAPI_ATTR void VKAPI_CALL tailIndirect(VkCommandBuffer b,VkBuffer buffer,VkDeviceSize offset,std::uint32_t count,std::uint32_t stride){++tailCalls;tailTag=tag;tailArguments={bits(b),bits(buffer),offset,count,stride};SetLastError(0x7132);}
void renderTailForwarding(bool enabled){
    Fixture f(false);xess_vk::render_tail::Store tail(bits(device),nullptr);if(enabled){tail.arm();f.state.renderTail=&tail;}
    f.state.cmdCopyImage=tailCopy;f.state.cmdCopyImage2=tailInfo<2,VkCopyImageInfo2>;f.state.cmdCopyImage2Khr=tailInfo<3,VkCopyImageInfo2>;
    f.state.cmdBlitImage=tailBlit;f.state.cmdBlitImage2=tailInfo<5,VkBlitImageInfo2>;f.state.cmdBlitImage2Khr=tailInfo<6,VkBlitImageInfo2>;
    f.state.cmdDraw=tailDraw;f.state.cmdDrawIndexed=tailIndexed;f.state.cmdDrawIndirect=tailIndirect<9>;f.state.cmdDrawIndexedIndirect=tailIndirect<10>;
    const std::array<const char*,10> names{"vkCmdCopyImage","vkCmdCopyImage2","vkCmdCopyImage2KHR","vkCmdBlitImage","vkCmdBlitImage2","vkCmdBlitImage2KHR","vkCmdDraw","vkCmdDrawIndexed","vkCmdDrawIndirect","vkCmdDrawIndexedIndirect"};
    for(const auto* name:names){require((deviceHook(name)!=nullptr)==enabled,"new render-tail entrypoints are exposed only by the diagnostic child flag");unavailable=name;require(!vkGetDeviceProcAddr(device,name),"new hook preserves downstream command availability");unavailable=nullptr;}
    VkImageCopy region{};region.extent={1920,1080,1};const auto src=handle<VkImage>(100),dst=handle<VkImage>(200);
    auto check=[&](unsigned tag,auto invoke){tailCalls=0;SetLastError(0x4321);invoke();require(tailCalls==1&&tailTag==tag&&GetLastError()==0x7132,"diagnostic wrapper forwards exactly once and preserves original LastError");};
    check(1,[&]{vkCmdCopyImage(command,src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);});
    require(tailInfoPointer==&region&&tailArguments[1]==100&&tailArguments[3]==200&&tailArguments[5]==1,"copy keeps original image/layout/region arguments");
    VkCopyImageInfo2 copy{VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};copy.srcImage=src;copy.dstImage=dst;
    check(2,[&]{vkCmdCopyImage2(command,&copy);});require(tailInfoPointer==&copy,"core copy2 preserves info pointer");
    check(3,[&]{vkCmdCopyImage2KHR(command,&copy);});require(tailInfoPointer==&copy,"KHR copy2 preserves info pointer");
    VkImageBlit blit{};check(4,[&]{vkCmdBlitImage(command,src,VK_IMAGE_LAYOUT_GENERAL,dst,VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_LINEAR);});
    require(tailInfoPointer==&blit&&tailArguments[6]==VK_FILTER_LINEAR,"blit preserves filter and original regions");
    VkBlitImageInfo2 info{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};info.srcImage=src;info.dstImage=dst;
    check(5,[&]{vkCmdBlitImage2(command,&info);});check(6,[&]{vkCmdBlitImage2KHR(command,&info);});
    check(7,[&]{vkCmdDraw(command,6,2,4,3);});require(tailArguments[1]==6&&tailArguments[2]==2&&tailArguments[3]==4&&tailArguments[4]==3,"draw argument values unchanged");
    check(8,[&]{vkCmdDrawIndexed(command,8,3,7,-2,5);});require(tailArguments[4]==std::uint64_t(std::int64_t(-2)),"indexed draw signed vertex offset unchanged");
    const auto indirect=handle<VkBuffer>(400);check(9,[&]{vkCmdDrawIndirect(command,indirect,64,3,16);});check(10,[&]{vkCmdDrawIndexedIndirect(command,indirect,128,4,20);});
    require(tailArguments[1]==400&&tailArguments[2]==128&&tailArguments[3]==4&&tailArguments[4]==20,"indirect draw metadata buffer and offset unchanged");
    require(!tail.stats().observed,"missing recording identity remains unknown rather than fabricating a journal");
}
VKAPI_ATTR void VKAPI_CALL tailBeginPass(VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents){++tailCalls;SetLastError(0x7132);}
VKAPI_ATTR void VKAPI_CALL tailEndPass(VkCommandBuffer){++tailCalls;SetLastError(0x7132);}
VKAPI_ATTR VkResult VKAPI_CALL tailSubmit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence){++tailCalls;SetLastError(0x7132);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL tailSubmit2(VkQueue,std::uint32_t,const VkSubmitInfo2*,VkFence){++tailCalls;SetLastError(0x7132);return VK_SUCCESS;}
void renderTailRejectedPass(bool enabled){
    namespace ro=xess_vk::render_pass_observer;namespace rp=xess_vk::render_pass_metadata;
    for(unsigned submit2=0;submit2<2;++submit2){
        Fixture f;std::unique_ptr<ro::Context,decltype(&ro::destroy)> context(ro::create(device,nullptr),ro::destroy);
        require(context!=nullptr,"render-tail wrapper metadata context");f.state.renderPass=context.get();
        xess_vk::render_tail::Store tail(bits(device),sink);if(enabled){tail.arm();f.state.renderTail=&tail;}
        f.state.cmdBeginRenderPass=tailBeginPass;f.state.cmdEndRenderPass=tailEndPass;f.state.cmdDraw=tailDraw;
        f.state.submit=tailSubmit;f.state.submit2=tailSubmit2;
        VkSwapchainCreateInfoKHR chainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};chainInfo.surface=handle<VkSurfaceKHR>(0x4500);
        chainInfo.minImageCount=1;chainInfo.imageFormat=VK_FORMAT_R8G8B8A8_UNORM;chainInfo.imageExtent={64,32};chainInfo.imageArrayLayers=1;
        chainInfo.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;chainInfo.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
        chainInfo.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;chainInfo.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;chainInfo.presentMode=VK_PRESENT_MODE_FIFO_KHR;
        VkSwapchainKHR chain{};returned=0x20001;require(vkCreateSwapchainKHR(device,&chainInfo,nullptr,&chain)==VK_SUCCESS,"wrapper registers native WSI chain");
        VkImage image{};std::uint32_t count=1;returned=0x20002;require(vkGetSwapchainImagesKHR(device,chain,&count,&image)==VK_SUCCESS,"wrapper registers exact WSI image generation");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};viewInfo.image=image;viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format=chainInfo.imageFormat;viewInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView view{};returned=0x20003;require(vkCreateImageView(device,&viewInfo,nullptr,&view)==VK_SUCCESS,"wrapper registers WSI attachment view");
        VkAttachmentDescription attachment{};attachment.format=chainInfo.imageFormat;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription subpass{};
        subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference;
        VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};passInfo.attachmentCount=1;passInfo.pAttachments=&attachment;passInfo.subpassCount=1;passInfo.pSubpasses=&subpass;
        const auto pass=handle<VkRenderPass>(0x20004);ro::renderPassCreated(context.get(),&passInfo,pass,VK_SUCCESS);
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};framebufferInfo.renderPass=pass;framebufferInfo.attachmentCount=1;framebufferInfo.pAttachments=&view;
        framebufferInfo.width=64;framebufferInfo.height=32;framebufferInfo.layers=1;const auto framebuffer=handle<VkFramebuffer>(0x20005);
        ro::framebufferCreated(context.get(),&framebufferInfo,framebuffer,VK_SUCCESS);
        const auto queue=reinterpret_cast<VkQueue>(&commandDispatch);ro::queue(context.get(),queue,0);
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocation.commandPool=handle<VkCommandPool>(0x20006);allocation.commandBufferCount=1;
        ro::allocate(context.get(),&allocation,&command,VK_SUCCESS);
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};require(vkBeginCommandBuffer(command,&beginInfo)==VK_SUCCESS,"wrapper begins recording");
        VkRenderPassBeginInfo beginPass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};beginPass.renderPass=pass;beginPass.framebuffer=framebuffer;beginPass.renderArea.extent={64,32};
        tailCalls=0;vkCmdBeginRenderPass(command,&beginPass,VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);vkCmdEndRenderPass(command);
        vkCmdBeginRenderPass(command,&beginPass,VK_SUBPASS_CONTENTS_INLINE);vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
        require(tailCalls==5&&GetLastError()==0x7132,"actual pass/draw wrappers forward once and preserve LastError after rejected pass");
        const auto snapshot=ro::diagnosticPassSnapshot(context.get(),command,&beginPass);
        require(snapshot.command.issues&&!snapshot.command.activeFramebuffer.handle,"wrapper keeps accumulated production refusal");
        const auto facts=ro::recordingFacts(context.get(),command);
        const auto consumerPass=srConsumerAttachments(&f.state,facts,xess_vk::sr_tail_commands::beginRenderPass(&beginPass,VK_SUBPASS_CONTENTS_INLINE));
        require(consumerPass.count==1&&consumerPass.images[0].handle==bits(image)&&consumerPass.images[0].generation==snapshot.views[0].image.generation,
            "consumer resolver uses actual command identity and keeps attachment evidence after unrelated accumulated recording refusal");
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};color.imageView=view;color.resolveImageView=view;color.resolveMode=VK_RESOLVE_MODE_AVERAGE_BIT;
        VkRenderingInfo dynamic{VK_STRUCTURE_TYPE_RENDERING_INFO};dynamic.colorAttachmentCount=1;dynamic.pColorAttachments=&color;
        auto dynamicCommand=xess_vk::sr_tail_commands::beginRendering(&dynamic);
        const auto consumerDynamic=srConsumerAttachments(&f.state,facts,dynamicCommand);
        require(consumerDynamic.complete&&consumerDynamic.count==2&&consumerDynamic.images[0]==consumerDynamic.images[1],"dynamic attachment and resolve references are both resolved");
        dynamicCommand.rendering.storedColors=UINT32_MAX;
        require(!srConsumerAttachments(&f.state,facts,dynamicCommand).complete,"malformed copied attachment count stays bounded and unknown");
        dynamicCommand=xess_vk::sr_tail_commands::beginRendering(&dynamic);dynamicCommand.extensionsOmitted=true;
        require(!srConsumerAttachments(&f.state,facts,dynamicCommand).complete,"omitted attachment extension cannot become complete coverage");
        require(vkEndCommandBuffer(command)==VK_SUCCESS,"wrapper ends recording");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};commandInfo.commandBuffer=command;commandInfo.deviceMask=1;
        VkSubmitInfo2 info2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info2.commandBufferInfoCount=1;info2.pCommandBufferInfos=&commandInfo;
        tailCalls=0;const auto result=submit2?vkQueueSubmit2(queue,1,&info2,VK_NULL_HANDLE):vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE);
        require(result==VK_SUCCESS&&tailCalls==1&&GetLastError()==0x7132,"actual legacy/submit2 wrappers preserve original success and LastError");
        require(!ro::stats(context.get()).acceptedRecordings,"diagnostic wrapper does not admit rejected production recording");
        require(tail.stats().emittedSamples==(enabled?1u:0u)&&tail.stats().observed==(enabled?2u:0u),"WSI draw through actual wrappers emits one sample only when explicitly armed");
        const auto expectedIssues="\"issues\":"+std::to_string(snapshot.command.issues);
        bool observation=false,permission=false,refusal=false;for(const auto& line:lines){observation|=line.find("\"event\":\"render_tail_sample\"")!=std::string::npos;permission|=line.find("\"copy_permitted\":true")!=std::string::npos;refusal|=line.find(expectedIssues)!=std::string::npos;}
        require(observation==enabled&&refusal==enabled&&!permission,"actual wrapper diagnostic sample retains refusal and grants no copy permission");
        require(vkResetCommandBuffer(command,0)==VK_SUCCESS&&vkBeginCommandBuffer(command,&beginInfo)==VK_SUCCESS,"wrapper reset starts fresh recording");
        require(!ro::diagnosticPassSnapshot(context.get(),command,nullptr).count,"wrapper fresh Begin/reset retains no stale attachment");
    }
}
void sourcesAndWsi(){
    Fixture f;auto r=setup(f);const auto ordinary=f.inventory->lookup(bits(r.image)).record;
    xess_vk::fsr_probe::Result fi;fi.status=xess_vk::fsr_probe::Status::SnapshotParsed;fi.allPassPropertiesPresent=true;fi.contextBoundsUnchanged=true;fi.pass=xess_vk::fsr_probe::PassKind::ReconstructPreviousDepth;
    fi.bindings[0].property=xess_vk::fsr_probe::Property::ReadDilatedMotionVectors;fi.bindings[0].native.status=xess_vk::fsr_probe::Status::NativeCandidateReadUnverified;fi.bindings[0].native.imageCandidate=ordinary.image;
    imageMetadataLog(bits(device),120,fi,false);auto s=di::stats(f.state.descriptors);require(s.sources[std::size_t(di::Source::FiMotion)].image==dm::Key{ordinary.image,ordinary.generation},"FI continuous refresh uses common image generation");
    xess_vk::upscale_probe::Result up;up.status=xess_vk::upscale_probe::Status::SnapshotParsed;up.allPropertiesPresent=up.contextTableUnchanged=up.graphIndexedEntries=up.descriptorsUnchanged=up.graphEntriesUnchanged=up.bindingMetadataUnchanged=true;
    up.bindings[0].property=xess_vk::upscale_probe::Property::InputMotionVectors;up.bindings[0].native=fi.bindings[0].native;
    upscaleImageMetadataLog(bits(device),240,up,false);s=di::stats(f.state.descriptors);require(s.sources[std::size_t(di::Source::RawMotion)].image==dm::Key{ordinary.image,ordinary.generation},"upscale source refresh is independent");require(s.sources[std::size_t(di::Source::FiMotion)].counter==120,"upscale refresh does not overwrite FI source counter");
    record(r);direct(f,true);require(di::stats(f.state.descriptors).matchedDispatches==1,"descriptor exact image generation matches numeric source sample");
    up.bindings[0].native.status=xess_vk::fsr_probe::Status::Unreadable;upscaleImageMetadataLog(bits(device),360,up,false);require(di::stats(f.state.descriptors).sources[std::size_t(di::Source::RawMotion)].image==dm::Key{},"missing upscale native candidate revokes old cache");
    fi.bindings[0].native.status=xess_vk::fsr_probe::Status::Unreadable;imageMetadataLog(bits(device),400,fi,false);s=di::stats(f.state.descriptors);require(s.sources[std::size_t(di::Source::FiMotion)].image==dm::Key{}&&s.sources[std::size_t(di::Source::FiMotion)].counter==0,"unreadable FI native candidate revokes old cache despite matching numeric handle");
    fi.bindings[0].native.status=xess_vk::fsr_probe::Status::NativeCandidateReadUnverified;imageMetadataLog(bits(device),420,fi,false);require(di::stats(f.state.descriptors).sources[std::size_t(di::Source::FiMotion)].image==dm::Key{ordinary.image,ordinary.generation},"readable FI candidate refreshes source after invalid native sample");
    fi.bindings[0].native.imageCandidate=0xdeadbeef;imageMetadataLog(bits(device),480,fi,false);require(di::stats(f.state.descriptors).sources[std::size_t(di::Source::FiMotion)].image==dm::Key{},"unmatched FI native candidate revokes old cache");
    fi.bindings[0].native.imageCandidate=ordinary.image;imageMetadataLog(bits(device),600,fi,false);vkDestroyImage(device,r.image,nullptr);require(di::stats(f.state.descriptors).sources[std::size_t(di::Source::FiMotion)].image==dm::Key{},"image retirement clears exact source generation");
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=handle<VkSurfaceKHR>(0x4500);ci.minImageCount=1;ci.imageFormat=VK_FORMAT_R8G8B8A8_UNORM;ci.imageExtent={64,32};ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainKHR chain{};returned=0x20001;require(vkCreateSwapchainKHR(device,&ci,nullptr,&chain)==VK_SUCCESS,"swapchain wrapper creation");VkImage swapImage{};std::uint32_t count=1;returned=0x20002;require(vkGetSwapchainImagesKHR(device,chain,&count,&swapImage)==VK_SUCCESS,"swapchain full enumeration");auto wsi=f.inventory->lookup(bits(swapImage)).record;require(wsi.generation>ordinary.generation&&di::stats(f.state.descriptors).metadata.liveImages==1,"WSI shares ordinary image generation domain");
    di::observeSource(f.state.descriptors,di::Source::FiRealColor,{wsi.image,wsi.generation},720);
    ci.oldSwapchain=chain;chosen=VK_ERROR_OUT_OF_DEVICE_MEMORY;VkSwapchainKHR replacement{};returned=0x20003;require(vkCreateSwapchainKHR(device,&ci,nullptr,&replacement)==chosen,"failed replacement forwarded");
    require(di::stats(f.state.descriptors).metadata.liveImages==0&&di::stats(f.state.descriptors).sources[std::size_t(di::Source::FiRealColor)].image==dm::Key{},"old WSI image/source retired before failed replacement");
    vkDestroySwapchainKHR(device,chain,nullptr);
    for(const auto& line:lines){require(line.find("\"copy_permitted\":true")==std::string::npos&&line.find("\"same_frame_verified\":true")==std::string::npos,"wrapper observations never grant copy or frame claims");}
}
namespace fake_wsi {
namespace p=xess_vk::presentation;
namespace ro=xess_vk::render_pass_observer;
p::QueueGate* gate{};
void checkGate(bool held) {
    const auto error=GetLastError();
    bool available{};
    std::thread other([&] { available=gate->try_lock();if(available)gate->unlock(); });other.join();
    require(available!=held,"only an actual queue call holds the shared device gate");
    require(TryAcquireSRWLockExclusive(&stateLock)!=0,"global state lock never spans a queue call or backend wait");
    ReleaseSRWLockExclusive(&stateLock);
    SetLastError(error);
}
template<Tag tag>VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence){hit(tag);checkGate(true);return chosen;}
template<Tag tag>VKAPI_ATTR VkResult VKAPI_CALL submit2(VkQueue,std::uint32_t,const VkSubmitInfo2*,VkFence){hit(tag);checkGate(true);return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL sparse(VkQueue,std::uint32_t,const VkBindSparseInfo*,VkFence){hit(GatedSparse);checkGate(true);return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue,const VkPresentInfoKHR*){hit(GatedPresent);checkGate(true);return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL queueIdle(VkQueue){hit(GatedQueueIdle);checkGate(true);return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL deviceIdle(VkDevice){hit(GatedDeviceIdle);checkGate(true);return chosen;}
struct Backend final:p::Backend {
    unsigned signals{},copies{},outputs{},destroyed{};std::uint64_t completion{};
    p::ShutdownState shutdown;
    p::FrameStatus status;
    VkResult copyResult{VK_SUCCESS},signalResult{VK_SUCCESS},windowResult{VK_SUCCESS};
    VkResult prepare(const VkSwapchainCreateInfoKHR&)override{checkGate(false);return VK_SUCCESS;}
    VkResult createFakeImages(const VkSwapchainCreateInfoKHR& info,std::uint64_t generation,std::vector<VkImage>& images)override{
        for(std::uint32_t i=0;i<std::max(3u,info.minImageCount);++i)images.push_back(handle<VkImage>(0x51000+generation*16+i));return VK_SUCCESS;
    }
    VkResult destroyFakeImages(std::uint64_t)override{++destroyed;return VK_SUCCESS;}
    VkResult signalAcquire(VkSemaphore,VkFence,xess_vk::wsi_acquire::Call* timing = nullptr)override{
        checkGate(false);{std::lock_guard lock(*gate);checkGate(true);++signals;}
        if(timing)timing->signal={1'000'000'000,1'002'000'000,1'003'000'000,signalResult};
        SetLastError(0x76543210);return signalResult;
    }
    VkResult windowStatus(VkExtent2D)const override{SetLastError(0x76543210);return windowResult;}
    VkResult copy(const VkPresentInfoKHR&,VkImage,std::uint64_t)override{checkGate(false);++copies;if(copyResult!=VK_SUCCESS)return copyResult;++completion;return VK_SUCCESS;}
    std::uint64_t copyCompletionToken()const override{return completion;}
    VkResult pollCopyCompletion(std::uint64_t)override{checkGate(false);return VK_SUCCESS;}
    VkResult consumePresentWaits(const VkPresentInfoKHR&)override{return VK_SUCCESS;}
    p::CopyProgress copyProgress()const override{return copyResult==VK_SUCCESS?p::CopyProgress::Completed:p::CopyProgress::NotSubmitted;}
    VkSemaphore copyCompletionSemaphore()const override{return VK_NULL_HANDLE;}
    VkResult output()override{checkGate(false);++outputs;return VK_SUCCESS;}
    p::FrameStatus frameStatus()const override{return status;}
    VkResult drain()override{checkGate(false);shutdown={true,true,true,false,false};return VK_SUCCESS;}
    p::ShutdownState shutdownState()const override{return shutdown;}
};
void acquireErrors() {
    Fixture f(false);p::QueueGate ownedGate;gate=&ownedGate;f.state.queueGate=gate;
    const auto queue=reinterpret_cast<VkQueue>(&commandDispatch);const auto surface=handle<VkSurfaceKHR>(0x63000);
    Backend backend;p::Runtime runtime(device,surface,queue,{createChain,destroyChain,getImages,present},backend,true);
    f.state.fakeEnabled=true;f.state.presentation=&runtime;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=surface;ci.minImageCount=3;
    ci.imageFormat=VK_FORMAT_R8G8B8A8_UNORM;ci.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;ci.imageExtent={64,32};
    ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainKHR chain{};require(runtime.create(&ci,nullptr,&chain)==VK_SUCCESS,"error logger owns an actual Runtime fake pool");
    const auto semaphore=handle<VkSemaphore>(0x64000);const auto fence=handle<VkFence>(0x64001);
    VkAcquireNextImageInfoKHR info{VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR};info.swapchain=chain;
    info.timeout=UINT64_MAX;info.semaphore=semaphore;info.fence=fence;info.deviceMask=1;
    LogCapture capture;
    xess_vk::wsi_acquire::Call quiet;
    for(const auto result:{VK_SUCCESS,VK_SUBOPTIMAL_KHR,VK_NOT_READY,VK_TIMEOUT}) {
        quiet.result=result;fakeAcquireError(&f.state,quiet);
    }
    f.state.fakeEnabled=false;quiet.result=VK_ERROR_DEVICE_LOST;fakeAcquireError(&f.state,quiet);f.state.fakeEnabled=true;
    require(!logBytes&&!f.state.acquireOutOfDateErrors&&!f.state.acquireOtherErrors,
            "non-errors and non-fake routes emit no JSON or error-budget increments");
    std::uint32_t index=99;
    require(vkAcquireNextImageKHR(device,chain,0,semaphore,VK_NULL_HANDLE,&index)==VK_SUCCESS&&GetLastError()==0x76543210,
            "ordinary fake acquire preserves success and downstream LastError");
    require(vkAcquireNextImage2KHR(device,&info,&index)==VK_SUCCESS&&GetLastError()==0x76543210,
            "ordinary fake acquire2 preserves success and downstream LastError");
    require(capture.read().find("\"event\":\"fake_acquire_error\"")==std::string::npos&&
            !f.state.acquireOutOfDateErrors&&!f.state.acquireOtherErrors,"successful wrappers emit no acquire error or error-budget increments");
    void* inaccessible=VirtualAlloc(nullptr,4096,MEM_RESERVE,PAGE_NOACCESS);
    require(inaccessible!=nullptr,"reserve inaccessible failed-acquire output");
    struct Reservation{void* address;~Reservation(){VirtualFree(address,0,MEM_RELEASE);}} reservation{inaccessible};
    auto* unreadable=static_cast<std::uint32_t*>(inaccessible);
    const auto failed=[&](unsigned api,VkResult expected) {
        SetLastError(0x12345678);
        const auto result=api%2 ? vkAcquireNextImage2KHR(device,&info,unreadable) :
            vkAcquireNextImageKHR(device,chain,UINT64_MAX,semaphore,VK_NULL_HANDLE,unreadable);
        const auto error=GetLastError();
        require(result==expected&&error==0x76543210,"failed actual acquire wrapper preserves exact result and downstream LastError without reading output");
    };
    f.state.slowAcquireCalls=xess_vk::wsi_acquire::slowCallLimit;
    backend.windowResult=VK_ERROR_OUT_OF_DATE_KHR;
    for(unsigned i=0;i<10;++i)failed(i,VK_ERROR_OUT_OF_DATE_KHR);
    require(f.state.acquireOutOfDateErrors==10&&!f.state.acquireOtherErrors,
            "expected window errors cannot consume the separate other-negative budget");
    backend.windowResult=VK_ERROR_DEVICE_LOST;failed(0,VK_ERROR_DEVICE_LOST);
    backend.windowResult=VK_ERROR_UNKNOWN;failed(1,VK_ERROR_UNKNOWN);
    backend.windowResult=VK_SUCCESS;
    for(unsigned i=0;i<10;++i){backend.signalResult=i%2?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_ERROR_OUT_OF_HOST_MEMORY;failed(i,backend.signalResult);}
    require(f.state.acquireOtherErrors==12,
            "negative errors remain observable after the separate slow-call budget is exhausted");
    backend.signalResult=VK_SUCCESS;
    require(vkAcquireNextImageKHR(device,chain,0,semaphore,VK_NULL_HANDLE,&index)==VK_SUCCESS,
            "logged unsubmitted OOM leaves the original valid acquisition retry available");
    index=77;SetLastError(0x12345678);
    require(vkAcquireNextImageKHR(device,chain,0,semaphore,VK_NULL_HANDLE,&index)==VK_NOT_READY&&index==77&&GetLastError()==0x76543210,
            "ordinary no-slot positive status remains unchanged after logging errors");
    require(f.state.acquireOutOfDateErrors==10&&f.state.acquireOtherErrors==12,
            "positive no-slot status never consumes an exhausted error budget");
    const auto text=capture.read();
    const auto count=[&](const char* needle){unsigned total{};std::size_t offset{};while((offset=text.find(needle,offset))!=std::string::npos){++total;offset+=std::strlen(needle);}return total;};
    require(count("\"event\":\"fake_acquire_error\"")==12&&count("\"error_budget\":\"out_of_date\"")==4&&
            count("\"error_budget\":\"other_negative\"")==8,"both acquire APIs share exactly four expected and eight other-negative records per device");
    require(text.find("\"api\":\"vkAcquireNextImageKHR\"")!=std::string::npos&&text.find("\"api\":\"vkAcquireNextImage2KHR\"")!=std::string::npos&&
            text.find("\"result\":-1000001004")!=std::string::npos&&text.find("\"result\":-4")!=std::string::npos&&text.find("\"result\":-13")!=std::string::npos,
            "fast OUT_OF_DATE device-lost and unknown literal results remain directly observable");
    require(text.find("\"swapchain_generation_known\":false")!=std::string::npos&&text.find("\"signal_observed\":false")!=std::string::npos&&
            text.find("\"swapchain_generation_known\":true")!=std::string::npos&&text.find("\"signal_observed\":true")!=std::string::npos&&
            text.find("\"timeout_ns\":\"18446744073709551615\"")!=std::string::npos&&text.find("\"fence_nonnull\":true")!=std::string::npos&&
            text.find("\"signal_result\":-1")!=std::string::npos&&text.find("\"signal_result\":-2")!=std::string::npos&&
            text.find("\"signal_entry_qpc_us\":1000000")!=std::string::npos&&text.find("\"signal_driver_entry_qpc_us\":1002000")!=std::string::npos&&
            text.find("\"signal_return_qpc_us\":1003000")!=std::string::npos,
            "error records retain only known generation and existing signal timestamps with exact timeout and synchronization mode");
}
std::array<VkImageLayout,4> layouts{};
const void* driverInfo{};
template<class Barrier>void capture(std::uint32_t count,const Barrier* images){
    require(count==2&&images,"both image barriers forwarded");layouts={images[0].oldLayout,images[0].newLayout,images[1].oldLayout,images[1].newLayout};
}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,std::uint32_t,const VkMemoryBarrier*,std::uint32_t,const VkBufferMemoryBarrier*,std::uint32_t count,const VkImageMemoryBarrier* images){hit(FakeBarrier);driverInfo=images;capture(count,images);}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL barrier2(VkCommandBuffer,const VkDependencyInfo* info){hit(tag);driverInfo=info;capture(info->imageMemoryBarrierCount,info->pImageMemoryBarriers);}
VKAPI_ATTR void VKAPI_CALL wait(VkCommandBuffer,std::uint32_t,const VkEvent*,VkPipelineStageFlags,VkPipelineStageFlags,std::uint32_t,const VkMemoryBarrier*,std::uint32_t,const VkBufferMemoryBarrier*,std::uint32_t count,const VkImageMemoryBarrier* images){hit(FakeWait);driverInfo=images;capture(count,images);}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL wait2(VkCommandBuffer,std::uint32_t count,const VkEvent*,const VkDependencyInfo* infos){hit(tag);require(count==2,"WaitEvents2 retains every dependency");driverInfo=infos;capture(infos[1].imageMemoryBarrierCount,infos[1].pImageMemoryBarriers);}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL set2(VkCommandBuffer,VkEvent event,const VkDependencyInfo* info){hit(tag);driverInfo=info;last.numbers[0]=bits(event);capture(info->imageMemoryBarrierCount,info->pImageMemoryBarriers);}
template<Tag tag,class Info>VkResult pass(const Info* info,VkRenderPass* output){hit(tag);driverInfo=info;layouts[0]=info->pAttachments[0].initialLayout;layouts[1]=info->pAttachments[0].finalLayout;*output=handle<VkRenderPass>(0x62000+tag);return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL pass1(VkDevice,const VkRenderPassCreateInfo* info,const VkAllocationCallbacks*,VkRenderPass* output){return pass<FakePass>(info,output);}
template<Tag tag>VKAPI_ATTR VkResult VKAPI_CALL pass2(VkDevice,const VkRenderPassCreateInfo2* info,const VkAllocationCallbacks*,VkRenderPass* output){return pass<tag>(info,output);}
void run() {
    for(const auto* value:{static_cast<const wchar_t*>(nullptr),L"0",L"01",L"11",L"1 ",L"1"}){
        SetEnvironmentVariableW(L"XESS_FAKE_SWAPCHAIN",value);
        require(fakeSwapchainRequested()==(value&&std::wcscmp(value,L"1")==0),"fake swapchain requires exact env value 1");
    }
    SetEnvironmentVariableW(L"XESS_FAKE_SWAPCHAIN",nullptr);
    Fixture f(true,true);require(di::stats(f.state.descriptors).metadata.continuousDispatchSelection,"fake mapping also runs with product dispatch selection");p::QueueGate ownedGate;gate=&ownedGate;f.state.queueGate=gate;
    const auto queue=reinterpret_cast<VkQueue>(&commandDispatch);const auto surface=handle<VkSurfaceKHR>(0x63000);
    f.state.submit=submit<GatedSubmit>;f.state.submit2=submit2<GatedSubmit2>;f.state.submit2Khr=submit2<GatedSubmit2Khr>;
    f.state.bindSparse=sparse;f.state.present=present;f.state.queueWaitIdle=queueIdle;f.state.deviceWaitIdle=deviceIdle;
    f.state.acquireNextImage=acquireImage;f.state.acquireNextImage2=acquireImage2;f.state.getSwapchainStatus=chainStatus;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};VkSubmitInfo2 si2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    beginCheck();require(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE)==VK_SUCCESS,"gated legacy submit");once(GatedSubmit);
    beginCheck();require(vkQueueSubmit2(queue,1,&si2,VK_NULL_HANDLE)==VK_SUCCESS,"gated core submit2");once(GatedSubmit2);
    beginCheck();require(vkQueueSubmit2KHR(queue,1,&si2,VK_NULL_HANDLE)==VK_SUCCESS,"gated KHR submit2");once(GatedSubmit2Khr);
    beginCheck();require(vkQueueBindSparse(queue,0,nullptr,VK_NULL_HANDLE)==VK_SUCCESS,"gated sparse bind");once(GatedSparse);
    beginCheck();require(vkQueueWaitIdle(queue)==VK_SUCCESS,"gated queue idle");once(GatedQueueIdle);
    beginCheck();require(vkDeviceWaitIdle(device)==VK_SUCCESS,"gated device idle");once(GatedDeviceIdle);
    VkSwapchainKHR realChain=handle<VkSwapchainKHR>(0x12345);std::uint32_t index{};
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.swapchainCount=1;pi.pSwapchains=&realChain;pi.pImageIndices=&index;
    beginCheck();require(vkQueuePresentKHR(queue,&pi)==VK_SUCCESS,"gated driver present");require(driverCalls==1&&last.tag==GatedPresent,"driver present gated once");

    std::unique_ptr<ro::Context,decltype(&ro::destroy)> context(ro::create(device,nullptr,{recordedLayoutInventory,&f.state}),ro::destroy);
    require(context!=nullptr,"fake layout metadata context");f.state.renderPass=context.get();
    Backend backend;p::Runtime runtime(device,surface,queue,{createChain,destroyChain,getImages,present},backend,true);
    f.state.fakeEnabled=true;f.state.presentationRequested=true;f.state.presentation=&runtime;
    f.state.rawEnabled=true;f.state.rawUniqueQueue=true;rawQueueObserved(&f.state,queue,0,0,0,true);
    require(!f.state.rawQueue&&!f.state.rawProbe,"fake mode refuses raw-capture private-submit registration even with stale diagnostic admission");f.state.rawEnabled=false;f.state.rawUniqueQueue=false;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=surface;ci.minImageCount=3;
    ci.imageFormat=VK_FORMAT_R8G8B8A8_UNORM;ci.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;ci.imageExtent={64,32};
    ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;
    ci.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VkSwapchainKHR chain{};beginCheck();require(vkCreateSwapchainKHR(device,&ci,nullptr,&chain)==VK_SUCCESS&&driverCalls==0&&runtime.ownsFakeChain(chain),"fake create never reaches driver WSI");
    std::uint32_t count{};require(vkGetSwapchainImagesKHR(device,chain,&count,nullptr)==VK_SUCCESS&&count==3,"fake image count");
    std::array<VkImage,3> images{};count=images.size();require(vkGetSwapchainImagesKHR(device,chain,&count,images.data())==VK_SUCCESS&&driverCalls==0,"fake images never reach driver WSI");
    const auto identity=f.inventory->lookup(bits(images[0])).record;require(identity.generation&&runtime.isFakeImage(images[0]),"fake image retains common WSI identity");
    const auto semaphore=handle<VkSemaphore>(0x64000);
    beginCheck();require(vkAcquireNextImageKHR(device,chain,UINT64_MAX,semaphore,VK_NULL_HANDLE,&index)==VK_SUCCESS&&driverCalls==0,"fake acquire never reaches driver WSI");
    VkAcquireNextImageInfoKHR ai{VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR};ai.swapchain=chain;ai.timeout=0;ai.semaphore=semaphore;ai.deviceMask=1;std::uint32_t index2{};
    require(vkAcquireNextImage2KHR(device,&ai,&index2)==VK_SUCCESS&&driverCalls==0&&index2!=index,"fake acquire2 uses layer pool");
    f.state.getSwapchainStatus=nullptr;require(vkGetSwapchainStatusKHR(device,chain)==VK_SUCCESS&&driverCalls==0,"fake status works without a downstream implementation");
    unavailable="vkGetSwapchainStatusKHR";require(!vkGetDeviceProcAddr(device,unavailable),"fake mode does not invent optional extension availability");unavailable=nullptr;
    pi.pSwapchains=&chain;pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&semaphore;
    beginCheck();require(vkQueuePresentKHR(queue,&pi)==VK_SUCCESS&&driverCalls==0&&backend.copies==1&&backend.outputs==1,"fake present reaches backend without driver WSI or outer queue gate");
    {
        LogCapture capture;
        require(!f.state.longFrameResetTracesLogged,"default fake present produces no long-frame reset trace");
        auto& trace=backend.status.longFrameReset;
        trace.requested=trace.emit=trace.outputEntryKnown=trace.intervalKnown=trace.longFrame=trace.eligible=true;
        trace.resetRequested=trace.outputSucceeded=true;trace.traceSequence=1;
        trace.engineFrame=51;trace.sdkPresentId=501;trace.copyToken=101;trace.copyGeneration=7;trace.timingContext=9;
        trace.outputEntryQpcTicks=123000;trace.qpcFrequency=1'000'000;trace.outputIntervalNs=100'000'000;trace.thresholdNs=40'000'000;
        trace.resetReasons=p::HistoryResetLongFrame;
        backend.status.sdkStatusValid=backend.status.requestedEnabled=backend.status.sdkEnabled=backend.status.queuedOn=true;
        backend.status.framesPresented=2;backend.status.interpolatedFrames=1;
        backend.status.copyFenceWaitCpu.add(222);backend.status.copyConsumedWaitCpu.add(333);
        backend.status.previousD3DWaitCpu.add(444);backend.status.bridgePublishCpu.add(555);
        backend.status.dxgiPresentCpu.add(666);backend.status.xellSleepCpu.add(777);
        pi.pImageIndices=&index2;
        require(vkQueuePresentKHR(queue,&pi)==VK_SUCCESS&&f.state.longFrameResetTracesLogged==1,
            "real observer present path emits an N0 diagnostic independent of periodic logging");
        const auto outputsBeforeFailure=backend.outputs;
        require(vkAcquireNextImageKHR(device,chain,UINT64_MAX,semaphore,VK_NULL_HANDLE,&index)==VK_SUCCESS,"trace retry owns a freshly acquired fake slot");
        pi.pImageIndices=&index;backend.copyResult=VK_ERROR_OUT_OF_HOST_MEMORY;
        require(vkQueuePresentKHR(queue,&pi)==VK_ERROR_OUT_OF_HOST_MEMORY&&backend.outputs==outputsBeforeFailure&&
                    f.state.longFrameResetTracesLogged==1&&!runtime.failed(),
            "copy failure before output cannot re-emit the retained previous Native trace");
        const auto failedSnapshot=runtime.frameStatus();
        backend.copyResult=VK_SUCCESS;trace.traceSequence=2;trace.policyEnabled=true;trace.longFrame=false;
        trace.engineFrame=52;trace.sdkPresentId=502;trace.copyToken=102;trace.outputEntryQpcTicks=124000;
        trace.extraRequested=trace.extraSucceeded=trace.pendingBefore=true;trace.triggerBefore={51,501,101};
        trace.resetReasons=p::HistoryResetFollowup;
        require(vkQueuePresentKHR(queue,&pi)==VK_SUCCESS&&f.state.longFrameResetTracesLogged==2,
            "successful retry emits a new N1 trace with its original trigger identity");
        const auto again=[&]{require(vkAcquireNextImageKHR(device,chain,UINT64_MAX,semaphore,VK_NULL_HANDLE,&index)==VK_SUCCESS,
                "bounded trace fixture acquires its own slot");require(vkQueuePresentKHR(queue,&pi)==VK_SUCCESS,"bounded trace fixture uses the real present wrapper");};
        trace.traceSequence=3;trace.emit=false;again();require(f.state.longFrameResetTracesLogged==2,"non-emitted output does not consume logger budget");
        trace.traceSequence=4;trace.emit=true;trace.outputEntryKnown=trace.intervalKnown=trace.resetRequested=trace.extraRequested=trace.extraSucceeded=trace.outputSucceeded=false;
        trace.clearReason="not_admitted_single_chain_on";again();
        require(f.state.longFrameResetTracesLogged==3,"clear trace without output-entry QPC still receives one unique record");
        again();require(f.state.longFrameResetTracesLogged==3,"same clear trace sequence never repeats on another attempt");
        f.state.longFrameResetTracesLogged=kLongFrameResetTraceLogLimit-1;
        trace.traceSequence=5;again();trace.traceSequence=6;again();
        const auto text=capture.read();
        const auto occurrences=[&](const char* needle){unsigned count{};std::size_t position{};while((position=text.find(needle,position))!=std::string::npos){++count;position+=std::strlen(needle);}return count;};
        require(occurrences("\"event\":\"long_frame_reset_trace\"")==4&&f.state.longFrameResetTracesLogged==kLongFrameResetTraceLogLimit+1,
            "production logger caps unique emitted snapshots at the device budget");
        require(text.find("\"followup_policy_n\":0")!=std::string::npos&&text.find("\"followup_policy_n\":1")!=std::string::npos&&
                    text.find("\"history_reset_requested\":true")!=std::string::npos&&text.find("tagged")==std::string::npos&&
                    text.find("\"trigger_before_engine_frame\":51")!=std::string::npos&&text.find("\"trigger_before_copy_token\":101")!=std::string::npos&&
                    text.find("\"output_entry_qpc_ticks\":123000")!=std::string::npos&&text.find("\"threshold_ns\":40000000")!=std::string::npos&&
                    text.find("\"output_interval_ns\":100000000")!=std::string::npos,
            "trace fields preserve policy, requested reset, trigger, QPC and threshold semantics");
        require(text.find("\"sdk_result_current\":true")!=std::string::npos&&text.find("\"sdk_query_result\":0")!=std::string::npos&&
                    text.find("\"sdk_framegen_result\":0")!=std::string::npos&&text.find("\"sdk_result_current\":false")!=std::string::npos&&
                    text.find("\"clear_reason\":\"not_admitted_single_chain_on\"")!=std::string::npos,
            "clear/no-output trace does not borrow stale SDK result fields from the previous output");
        const auto clearPosition=text.find("\"trace_sequence\":4");
        const auto clearLine=text.substr(clearPosition,text.find('\n',clearPosition)-clearPosition);
        require(clearLine.find("\"sdk_result_current\":false")!=std::string::npos&&clearLine.find("\"sdk_query_result\":")==std::string::npos&&
                    clearLine.find("\"sdk_framegen_result\":")==std::string::npos&&clearLine.find("\"output_entry_qpc_ticks\":")==std::string::npos&&
                    clearLine.find("\"cpu_wall_totals_current\":false")!=std::string::npos&&clearLine.find("\"cpu_dxgi_present\":")==std::string::npos,
            "the specific clear record omits unavailable current SDK results and output-entry QPC");
        for(const auto* counter:{"cpu_runtime_copy","cpu_native_present","cpu_output","cpu_copy_fence_wait","cpu_copy_consumed_wait",
                                "cpu_previous_d3d_wait","cpu_bridge_publish","cpu_dxgi_present","cpu_xell_sleep"})
            require(text.find(std::string("\"")+counter+"\":{\"count\":")!=std::string::npos,"fresh long-frame trace contains all nine existing cumulative CPU counters");
        require(text.find("\"cpu_wall_totals_current\":true")!=std::string::npos&&text.find("\"cpu_wall_nested_not_additive\":true")!=std::string::npos&&
                    text.find("\"cpu_copy_fence_wait\":{\"count\":1,\"total_ns\":222")!=std::string::npos,
            "trace CPU counters preserve cumulative totals and overlapping-scope semantics");
        trace.traceSequence=7;trace.emit=false;trace.outputEntryKnown=trace.intervalKnown=true;
        trace.outputIntervalNs=38'000'000;trace.thresholdNs=40'000'000;trace.longFrame=false;trace.resetReasons=0;
        const auto freshSnapshot=runtime.frameStatus();
        const bool outputFresh=longFrameResetTrace(&f.state,freshSnapshot,VK_SUCCESS,123);
        require(outputFresh&&f.state.lastLongFrameResetTraceSequence==7&&f.state.longFrameResetTracesLogged==kLongFrameResetTraceLogLimit+1,
            "fresh output decision remains observable when emit is false or trace budget is exhausted");
        f.state.frameSpikeValid=true;f.state.frameSpikePresentNs=steadyNowNs()-30'000'000;
        recordFrameSpike(&f.state,freshSnapshot,outputFresh,steadyNowNs()-1'000'000,trace.engineFrame,123);
        require(!longFrameResetTrace(&f.state,failedSnapshot,VK_ERROR_OUT_OF_HOST_MEMORY,124),"older failed-output snapshot is never fresh");
        f.state.frameSpikePresentNs=steadyNowNs()-30'000'000;
        recordFrameSpike(&f.state,failedSnapshot,false,steadyNowNs()-1'000'000,0,124);
        const auto fullText=capture.read();
        const auto spikePosition=fullText.find("\"event\":\"frame_spike\"");
        const auto spikeLine=fullText.substr(spikePosition,fullText.find('\n',spikePosition)-spikePosition);
        require(spikeLine.find("\"long_frame_trace_current\":true")!=std::string::npos&&spikeLine.find("\"long_frame_trace_sequence\":7")!=std::string::npos&&
                    spikeLine.find("\"output_interval_ns\":38000000")!=std::string::npos&&spikeLine.find("\"output_threshold_ns\":40000000")!=std::string::npos&&
                    spikeLine.find("\"long_frame\":false")!=std::string::npos&&spikeLine.find("\"history_reset_reasons\":0")!=std::string::npos,
            "frame spike below Native long-frame threshold exposes this fresh decision even without an emitted trace");
        const auto oldPosition=fullText.find("\"event\":\"frame_spike\"",spikePosition+1);
        const auto oldLine=fullText.substr(oldPosition,fullText.find('\n',oldPosition)-oldPosition);
        require(oldLine.find("\"long_frame_trace_current\":false")!=std::string::npos&&oldLine.find("\"long_frame_trace_sequence\":")==std::string::npos&&
                    oldLine.find("\"output_threshold_ns\":")==std::string::npos,"failed attempt frame spike cannot borrow the prior output decision");
        backend.status={};
    }

    f.state.cmdPipelineBarrier=barrier;f.state.cmdPipelineBarrier2=barrier2<FakeBarrier2>;f.state.cmdPipelineBarrier2Khr=barrier2<FakeBarrier2Khr>;
    f.state.cmdWaitEvents=wait;f.state.cmdWaitEvents2=wait2<FakeWait2>;f.state.cmdWaitEvents2Khr=wait2<FakeWait2Khr>;
    f.state.cmdSetEvent2=set2<FakeSet2>;f.state.cmdSetEvent2Khr=set2<FakeSet2Khr>;
    VkImageCreateInfo ordinaryInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ordinaryInfo.imageType=VK_IMAGE_TYPE_2D;ordinaryInfo.format=ci.imageFormat;
    ordinaryInfo.extent={64,32,1};ordinaryInfo.mipLevels=ordinaryInfo.arrayLayers=1;ordinaryInfo.samples=VK_SAMPLE_COUNT_1_BIT;ordinaryInfo.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage ordinaryImage{};returned=0x77777;require(vkCreateImage(device,&ordinaryInfo,nullptr,&ordinaryImage)==VK_SUCCESS,"ordinary barrier peer has a live metadata identity");
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolInfo.queueFamilyIndex=0;
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocation.commandPool=handle<VkCommandPool>(0x65000);allocation.commandBufferCount=1;
    ro::poolCreated(context.get(),allocation.commandPool,&poolInfo,VK_SUCCESS);ro::allocate(context.get(),&allocation,&command,VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};require(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS,"begin fake-image layout recording");
    std::array<VkImageMemoryBarrier,2> barriers{};for(auto& b:barriers){b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
    barriers[0].image=images[0];barriers[0].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barriers[0].newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].image=ordinaryImage;barriers[1].oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barriers[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    beginCheck();vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,barriers.data());once(FakeBarrier);
    require(layouts[1]==VK_IMAGE_LAYOUT_GENERAL&&layouts[2]==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR&&driverInfo!=barriers.data()&&barriers[0].newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"legacy driver copy maps only fake image PRESENT");
    const auto facts=ro::dispatchPoint(context.get(),command);const auto logical=ro::resolveSourceAt(context.get(),facts,{identity.image,identity.generation},barriers[0].subresourceRange,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    require(logical.recordedLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR&&logical.lastEvent.newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"logical barrier metadata retains PRESENT");
    std::array<VkImageMemoryBarrier2,2> barriers2{};for(unsigned i=0;i<2;++i){auto& b=barriers2[i];b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;b.oldLayout=barriers[i].oldLayout;b.newLayout=barriers[i].newLayout;b.image=barriers[i].image;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange=barriers[i].subresourceRange;}
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=2;dep.pImageMemoryBarriers=barriers2.data();
    for(const bool khr:{false,true}){beginCheck();if(khr)vkCmdPipelineBarrier2KHR(command,&dep);else vkCmdPipelineBarrier2(command,&dep);once(khr?FakeBarrier2Khr:FakeBarrier2);require(layouts[1]==VK_IMAGE_LAYOUT_GENERAL&&layouts[2]==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR&&driverInfo!=&dep&&barriers2[0].newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"sync2 aliases map a driver-only image copy");}
    const std::array<VkEvent,2> events{handle<VkEvent>(0x66001),handle<VkEvent>(0x66002)};
    beginCheck();vkCmdWaitEvents(command,events.size(),events.data(),VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,nullptr,0,nullptr,2,barriers.data());once(FakeWait);
    require(layouts[1]==VK_IMAGE_LAYOUT_GENERAL&&layouts[2]==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR&&barriers[0].newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"WaitEvents maps only driver copy");
    const std::array<VkDependencyInfo,2> deps{dep,dep};for(const bool khr:{false,true}){beginCheck();if(khr)vkCmdWaitEvents2KHR(command,events.size(),events.data(),deps.data());else vkCmdWaitEvents2(command,events.size(),events.data(),deps.data());once(khr?FakeWait2Khr:FakeWait2);require(layouts[1]==VK_IMAGE_LAYOUT_GENERAL&&layouts[2]==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR&&driverInfo!=deps.data()&&barriers2[0].newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"WaitEvents2 aliases retain all logical dependencies");}
    for(const bool khr:{false,true}){
        beginCheck();if(khr)vkCmdSetEvent2KHR(command,events[0],&dep);else vkCmdSetEvent2(command,events[0],&dep);once(khr?FakeSet2Khr:FakeSet2);
        const auto setLayouts=layouts;require(driverInfo!=&dep&&layouts[1]==VK_IMAGE_LAYOUT_GENERAL&&last.numbers[0]==bits(events[0]),"SetEvent2 aliases map driver copy and preserve event");
        beginCheck();if(khr)vkCmdWaitEvents2KHR(command,events.size(),events.data(),deps.data());else vkCmdWaitEvents2(command,events.size(),events.data(),deps.data());once(khr?FakeWait2Khr:FakeWait2);
        require(layouts==setLayouts&&barriers2[0].newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"event set/wait use matching physical layouts and unchanged logical input");
    }
    for(const auto* name:{"vkCmdSetEvent2","vkCmdSetEvent2KHR"}){require(deviceHook(name)&&instanceHook(name),"SetEvent2 aliases registered in both tables");unavailable=name;require(!vkGetDeviceProcAddr(device,name)&&!vkGetInstanceProcAddr(instance,name),"SetEvent2 optional availability preserved");unavailable=nullptr;}
    f.state.fakeEnabled=false;
    require(vkResetCommandBuffer(command,0)==VK_SUCCESS&&vkBeginCommandBuffer(command,&begin)==VK_SUCCESS,"fresh default event metadata recording");
    const auto beforeSet=ro::recordingFacts(context.get(),command);
    for(const bool khr:{false,true}){
        beginCheck();if(khr)vkCmdSetEvent2KHR(command,events[0],&dep);else vkCmdSetEvent2(command,events[0],&dep);once(khr?FakeSet2Khr:FakeSet2);
        const auto afterSet=ro::recordingFacts(context.get(),command);
        require(driverInfo==&dep&&layouts[1]==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR&&afterSet.reasons==beforeSet.reasons&&afterSet.commonRecordOrder==beforeSet.commonRecordOrder,"default SetEvent2 aliases preserve dependency pointer and prior metadata issues/order");
    }
    f.state.fakeEnabled=true;
    barriers[0].oldLayout=VK_IMAGE_LAYOUT_GENERAL;barriers[0].newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    beginCheck();vkCmdPipelineBarrier(command,1,1,0,0,nullptr,0,nullptr,2,barriers.data());once(FakeBarrier);require(driverInfo==barriers.data(),"legal fake image layouts require no copy");
    f.state.fakeEnabled=false;barriers[0].newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    beginCheck();vkCmdPipelineBarrier(command,1,1,0,0,nullptr,0,nullptr,2,barriers.data());once(FakeBarrier);require(driverInfo==barriers.data()&&layouts[1]==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"default mode retains original barrier pointer and PRESENT");f.state.fakeEnabled=true;

    f.state.createRenderPass=pass1;f.state.createRenderPass2=pass2<FakePass2>;f.state.createRenderPass2Khr=pass2<FakePass2Khr>;
    VkAttachmentDescription attachment{};attachment.format=ci.imageFormat;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference;
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};passInfo.attachmentCount=1;passInfo.pAttachments=&attachment;passInfo.subpassCount=1;passInfo.pSubpasses=&subpass;VkRenderPass rp{};
    beginCheck();require(vkCreateRenderPass(device,&passInfo,nullptr,&rp)==VK_SUCCESS,"mapped renderpass create");once(FakePass);require(driverInfo!=&passInfo&&layouts[0]==VK_IMAGE_LAYOUT_GENERAL&&layouts[1]==VK_IMAGE_LAYOUT_GENERAL&&attachment.finalLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"RP initial/final map only in driver copy");
    VkRenderPassBeginInfo passBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};passBegin.renderPass=rp;auto passSnapshot=ro::diagnosticPassSnapshot(context.get(),command,&passBegin);require(passSnapshot.pass.attachments[0].initial==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR&&passSnapshot.pass.attachments[0].final==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"RP logical metadata unchanged");
    VkAttachmentDescription2 attachment2{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};attachment2.format=ci.imageFormat;attachment2.samples=VK_SAMPLE_COUNT_1_BIT;attachment2.initialLayout=attachment2.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference2 ref2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};ref2.attachment=0;ref2.layout=reference.layout;ref2.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubpassDescription2 sub2{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};sub2.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub2.colorAttachmentCount=1;sub2.pColorAttachments=&ref2;
    VkRenderPassCreateInfo2 passInfo2{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};passInfo2.attachmentCount=1;passInfo2.pAttachments=&attachment2;passInfo2.subpassCount=1;passInfo2.pSubpasses=&sub2;
    for(const bool khr:{false,true}){beginCheck();require((khr?vkCreateRenderPass2KHR(device,&passInfo2,nullptr,&rp):vkCreateRenderPass2(device,&passInfo2,nullptr,&rp))==VK_SUCCESS,"mapped RP2 alias create");once(khr?FakePass2Khr:FakePass2);require(driverInfo!=&passInfo2&&layouts[0]==VK_IMAGE_LAYOUT_GENERAL&&layouts[1]==VK_IMAGE_LAYOUT_GENERAL&&attachment2.finalLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"RP2 aliases map owned attachment copy");passBegin.renderPass=rp;passSnapshot=ro::diagnosticPassSnapshot(context.get(),command,&passBegin);require(passSnapshot.pass.attachments[0].final==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"RP2 metadata retains logical PRESENT");}
    f.state.fakeEnabled=false;beginCheck();require(vkCreateRenderPass(device,&passInfo,nullptr,&rp)==VK_SUCCESS,"default RP create");once(FakePass);require(driverInfo==&passInfo&&layouts[1]==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,"default RP preserves pointer and layouts");f.state.fakeEnabled=true;
    f.state.cmdBeginRendering=tailInfo<11,VkRenderingInfo>;f.state.cmdBeginRenderingKhr=tailInfo<12,VkRenderingInfo>;
    VkRenderingAttachmentInfo renderingAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};renderingAttachment.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};rendering.colorAttachmentCount=1;rendering.pColorAttachments=&renderingAttachment;
    for(const bool khr:{false,true}){if(khr)vkCmdBeginRenderingKHR(command,&rendering);else vkCmdBeginRendering(command,&rendering);require(tailInfoPointer==&rendering&&renderingAttachment.imageLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,"legal dynamic rendering is unchanged");}

    p::fake_layout::failNextCopy=true;beginCheck();vkCmdPipelineBarrier(command,1,1,0,0,nullptr,0,nullptr,2,barriers.data());require(driverCalls==0&&fakeRecordingError(&f.state)==VK_ERROR_OUT_OF_HOST_MEMORY,"void mapper OOM latches failure without forwarding PRESENT");
    require(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE)==VK_ERROR_OUT_OF_HOST_MEMORY&&vkQueueSubmit2(queue,1,&si2,VK_NULL_HANDLE)==VK_ERROR_OUT_OF_HOST_MEMORY&&vkQueueSubmit2KHR(queue,1,&si2,VK_NULL_HANDLE)==VK_ERROR_OUT_OF_HOST_MEMORY&&driverCalls==0,"failed recording never submits incomplete command buffers");
    require(vkAcquireNextImageKHR(device,chain,0,semaphore,VK_NULL_HANDLE,&index)==VK_ERROR_OUT_OF_HOST_MEMORY&&vkQueuePresentKHR(queue,&pi)==VK_ERROR_OUT_OF_HOST_MEMORY&&driverCalls==0,"failed recording rejects fake acquire and present");
    beginCheck();require(vkQueueWaitIdle(queue)==VK_SUCCESS,"recording failure retains cleanup idle");once(GatedQueueIdle);f.state.fakeRecordingError=0;
    beginCheck();vkDestroySwapchainKHR(device,chain,nullptr);require(driverCalls==0&&backend.destroyed==1,"fake destroy never reaches driver WSI");
    f.state.presentation=nullptr;f.state.presentationRequested=false;beginCheck();require(vkCreateSwapchainKHR(device,&ci,nullptr,&chain)<0&&driverCalls==0,"frozen fake mode cannot fall back after context failure");
    f.state.queueGate=nullptr;f.state.renderPass=nullptr;
}
}
namespace device_boundary {
const VkDeviceCreateInfo* expectedInfo{};
const VkWin32SurfaceCreateInfoKHR* expectedSurface{};
unsigned creates{}, destroys{}, queries{}, surfaces{};
VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice,const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkDevice* output) {
    ++creates;require(info==expectedInfo&&!allocator,"pure/conflicting observation forwards original device request");
    require(info->enabledExtensionCount==1&&std::strcmp(info->ppEnabledExtensionNames[0],"VK_KHR_swapchain")==0,
        "pure/conflicting observation adds no bridge extensions");
    *output=device;SetLastError(0x76543210);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice original,const VkAllocationCallbacks* allocator) {
    ++destroys;require(original==device&&!allocator,"original device destruction forwarded");
}
VKAPI_ATTR VkResult VKAPI_CALL createSurface(VkInstance original,const VkWin32SurfaceCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator,VkSurfaceKHR* output) {
    ++surfaces;require(original==instance&&info==expectedSurface&&!allocator,"conflict preserves original surface and HWND");
    *output=handle<VkSurfaceKHR>(0x9898);return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceProc(VkInstance,const char* name) {
    if(std::strcmp(name,"vkCreateDevice")==0)return reinterpret_cast<PFN_vkVoidFunction>(createDevice);
    ++queries;return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice,const char* name) {
    return std::strcmp(name,"vkDestroyDevice")==0?reinterpret_cast<PFN_vkVoidFunction>(destroyDevice):nullptr;
}
void run() {
    constexpr const wchar_t* options[]{L"XESS_FSR_OBSERVER",L"XESS_FSR_UPSCALE_OBSERVER",L"XESS_COMPUTE_OBSERVER",
        L"XESS_BRIDGE_DEVICE",L"XESS_PRESENTATION_OFF",L"XESS_CONTINUOUS_INPUTS",L"XESS_RAWCOLOR_CAPTURE",L"XESS_FSR_NATIVE_OFF"};
    for(const auto* conflict : {static_cast<const wchar_t*>(nullptr),L"XESS_BRIDGE_DEVICE",L"XESS_PRESENTATION_OFF",
                               L"XESS_CONTINUOUS_INPUTS",L"XESS_RAWCOLOR_CAPTURE",L"XESS_FSR_NATIVE_OFF"}) {
        for(const auto* name:options)SetEnvironmentVariableW(name,nullptr);
        SetEnvironmentVariableW(L"XESS_WSI_ACQUIRE_OBSERVER",conflict?nullptr:L"1");
        SetEnvironmentVariableW(L"XESS_FSR_UPSCALE_OBSERVER",L"1");
        if(conflict)SetEnvironmentVariableW(conflict,L"1");
        InstanceState parent{};parent.instance=instance;parent.key=&marker;parent.gipa=instanceProc;
        parent.defaultAllocator=true;parent.createWin32Surface=createSurface;instances=&parent;
        devices=nullptr;inventoryDeviceCreations=0;inventorySessionRetired=false;creates=destroys=queries=surfaces=0;
        if(conflict&&std::wcscmp(conflict,L"XESS_PRESENTATION_OFF")==0) {
            VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
            surfaceInfo.hwnd=reinterpret_cast<HWND>(0x1234);expectedSurface=&surfaceInfo;VkSurfaceKHR surface{};
            require(vkCreateWin32SurfaceKHR(instance,&surfaceInfo,nullptr,&surface)==VK_SUCCESS&&surfaces==1&&!parent.presentationSurface,
                "pure observation conflict never creates a hidden surface");
        }
        VkLayerDeviceLink link{};link.pfnNextGetInstanceProcAddr=instanceProc;link.pfnNextGetDeviceProcAddr=deviceProc;
        VkLayerDeviceCreateInfo chain{};chain.sType=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
        chain.function=VK_LAYER_LINK_INFO;chain.u.pLayerInfo=&link;
        const char* extension="VK_KHR_swapchain";
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&chain};info.enabledExtensionCount=1;info.ppEnabledExtensionNames=&extension;
        expectedInfo=&info;VkDevice output{};
        require(vkCreateDevice(reinterpret_cast<VkPhysicalDevice>(&instanceDispatch),&info,nullptr,&output)==VK_SUCCESS&&creates==1,
            "actual CreateDevice wrapper performs one original call");
        require(GetLastError()==0x76543210,"device metadata preserves original LastError");
        auto* state=findDevice(output);require(state!=nullptr,"actual device state published");
        require((state->wsiAcquire!=nullptr)==!conflict&&!state->acquireNextImage&&!state->acquireNextImage2&&!state->getSwapchainStatus,
            "actual device creation gates acquire metadata and preserves missing downstream optional functions");
        require(!state->presentationRequested&&!state->presentationRecord&&!state->presentation&&!state->continuous&&
                !state->continuousTiming&&!state->packets&&!state->rawEnabled&&!state->rawProbe&&!state->compute,
                "pure/conflicting observation creates no presenter/provider/raw resources");
        if(conflict)require(queries==1&&!state->imageInventory&&!state->descriptors&&!state->candidateSync,
            "conflict performs only the ordinary image-function lookup, no bridge queries or observation allocations");
        else require(state->imageInventory&&state->descriptors&&state->candidateSync&&state->renderPass&&state->resourceMemory,
            "single upscale flag creates actual dispatch/inventory metadata without compute observer");
        vkDestroyDevice(output,nullptr);require(destroys==1&&!devices,"created metadata device tears down normally");
        instances=nullptr;
    }
    SetEnvironmentVariableW(L"XESS_WSI_ACQUIRE_OBSERVER",nullptr);
    for(const auto* name:options)SetEnvironmentVariableW(name,nullptr);
    // This CPU wrapper has no compiled detour installer: even complete flags
    // cannot certify the current profile or redirect either surface ordering.
    for(const bool surfaceFirst:{false,true}) {
        for(const auto* name:{L"XESS_FSR_UPSCALE_OBSERVER",L"XESS_CONTINUOUS_INPUTS",L"XESS_FSR_NATIVE_OFF",L"XESS_BRIDGE_DEVICE",L"XESS_PRESENTATION_OFF"})
            SetEnvironmentVariableW(name,L"1");
        SetEnvironmentVariableW(L"XESS_SDK_BIN",L"C:\\owned-fixture");
        InstanceState parent{};parent.instance=instance;parent.key=&marker;parent.gipa=instanceProc;
        parent.defaultAllocator=true;parent.createWin32Surface=createSurface;instances=&parent;
        devices=nullptr;inventoryDeviceCreations=0;inventorySessionRetired=false;creates=destroys=queries=surfaces=0;
        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hwnd=reinterpret_cast<HWND>(0x1234);expectedSurface=&surfaceInfo;VkSurfaceKHR surface{};
        const auto originalSurface=[&] {
            require(vkCreateWin32SurfaceKHR(instance,&surfaceInfo,nullptr,&surface)==VK_SUCCESS&&surfaces==1&&
                    !parent.presentationSurface&&parent.continuousSurfaceRefused,
                    "unready continuous device preserves HWND and permanently refuses late takeover");
        };
        if(surfaceFirst)originalSurface();
        VkLayerDeviceLink link{};link.pfnNextGetInstanceProcAddr=instanceProc;link.pfnNextGetDeviceProcAddr=deviceProc;
        VkLayerDeviceCreateInfo chain{};chain.sType=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;chain.function=VK_LAYER_LINK_INFO;chain.u.pLayerInfo=&link;
        const char* extension="VK_KHR_swapchain";VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&chain};
        info.enabledExtensionCount=1;info.ppEnabledExtensionNames=&extension;expectedInfo=&info;VkDevice output{};
        require(vkCreateDevice(reinterpret_cast<VkPhysicalDevice>(&instanceDispatch),&info,nullptr,&output)==VK_SUCCESS&&creates==1,
                "unverified continuous profile leaves the original device creation usable");
        auto* state=findDevice(output);
        require(state&&!state->presentationRequested&&!state->presentationRecord&&!state->continuous&&!state->continuousTiming&&!state->packets,
                "surface-first or unready-device fallback creates no takeover or menu consumer");
        if(!surfaceFirst)originalSurface();
        require(!currentContinuousPresentationReady(&parent),"failed admission cannot redirect a later surface");
        vkDestroyDevice(output,nullptr);require(destroys==1&&!devices,"fallback device remains normally destructible");instances=nullptr;
        for(const auto* name:options)SetEnvironmentVariableW(name,nullptr);
        SetEnvironmentVariableW(L"XESS_SDK_BIN",nullptr);
    }
}
}
}
int main(int argc,char** argv){using namespace descriptor_layer_fixture;try{
    SetEnvironmentVariableW(L"XESS_OBSERVER_LOG",nullptr);
    SetEnvironmentVariableW(L"XESS_WSI_ACQUIRE_OBSERVER",nullptr);
    SetEnvironmentVariableW(L"XESS_FAKE_SWAPCHAIN",nullptr);
    const bool tailOnly=argc==2&&std::strcmp(argv[1],"--render-tail-enabled")==0;
    const bool timingBenchmarkOnly=argc==2&&std::strcmp(argv[1],"--tracking-cpu-timing-benchmark")==0;
    SetEnvironmentVariableW(L"XESS_RENDER_TAIL_DIAGNOSTIC",tailOnly?L"1":nullptr);
    trackingCpuEnvironment();
    if(timingBenchmarkOnly){trackingCpuBenchmark();return 0;}
    if(tailOnly){renderTailForwarding(true);renderTailRejectedPass(true);std::cout<<"{\"passed\":true,\"renderTailEnabledForwardingChecks\":"<<checks<<",\"realVulkanCalls\":0}\n";return 0;}
    trackingCpuModes();ownCpuTiming();selectedDispatches();forwarding();lifetime();dispatchVariants();ordinaryTemplates();graphicsIsolation();sourcesAndWsi();device_boundary::run();wsiAcquireObservation();gpuTimestampArithmetic();asyncInflightCounters();gpuCopySlowObservation();slowCallObservation();fake_wsi::acquireErrors();fake_wsi::run();shaderArchiveForwarding();
    threadCpuBatching();measurementSummarySchema();measurementFrameClassification();measurementIntervalClassification();
    renderTailForwarding(false);
    renderTailRejectedPass(false);
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"realVulkanCalls\":0,\"gameModulesLoaded\":false,\"copyPermissionGranted\":false}\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
