// Production wrappers, self-owned dispatch objects and downstream stubs only.
// No Vulkan loader/GPU/game process/copy/wait or detour installer is used.
#include "vulkan_observer.cpp"
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace recording_layer_fixture {
namespace ro=xess_vk::render_pass_observer;
namespace di=xess_vk::descriptor_image_observer;
using CR=xess_vk::CommandRecordingReason;
unsigned checks{},calls{};
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class T>T handle(std::uintptr_t n){return reinterpret_cast<T>(n);}
int marker;struct DispatchObject {void* key;};
DispatchObject deviceObject{&marker},commandObject{&marker},queueObject{&marker};
const auto device=reinterpret_cast<VkDevice>(&deviceObject);
const auto command=reinterpret_cast<VkCommandBuffer>(&commandObject);
const auto queue=reinterpret_cast<VkQueue>(&queueObject);
const auto pool=handle<VkCommandPool>(0x701);
enum Tag {CreatePool,Allocate,Free,Begin,End,Reset,ResetPool,DestroyPool,GetQueue,Barrier,Barrier2,Barrier2Khr,
    BeginRP,BeginRP2,BeginRP2Khr,EndRP,EndRP2,EndRP2Khr,BeginDynamic,BeginDynamicKhr,EndDynamic,EndDynamicKhr,
    Direct,Indirect,Base,BaseKhr,Submit,Submit2,Submit2Khr,CreateImage,CreateView,CreateRP,CreateFB,CreateSetLayout,CreatePL,CreateDP,AllocateSet,UpdateSet,CreatePipeline,BindPipeline,BindSets};
struct Call {Tag tag{};std::array<const void*,5> pointers{};std::array<std::uint64_t,8> values{};} last;
VkResult chosen=VK_SUCCESS;ro::Context* active{};di::Context* descriptors{};
bool dispatchBeforeDriver{};std::uint64_t expectedOrder{},expectedDispatchCalls{};
std::array<std::array<char,8192>,32> lines{};std::size_t lineCount{};
const char* unavailable{};
void sink(const char* line){if(lineCount<lines.size()){std::strncpy(lines[lineCount].data(),line,lines[0].size()-1);++lineCount;}SetLastError(0xdead);}
bool contains(const char* fragment){for(std::size_t i=0;i<lineCount;++i)if(std::strstr(lines[i].data(),fragment))return true;return false;}
void hit(Tag tag){
    ++calls;last={};last.tag=tag;
    require(TryAcquireSRWLockExclusive(&stateLock)!=0,"stateLock never spans original call");ReleaseSRWLockExclusive(&stateLock);
    if(active){const auto f=ro::recordingFacts(active,command); // Detects adapter mutex spanning the driver.
        if(dispatchBeforeDriver){require(f.commonRecordOrder==expectedOrder,"RP dispatch point precedes exact original dispatch");
            require(di::stats(descriptors).dispatchCalls==expectedDispatchCalls,"descriptor consumer precedes exact original dispatch");}}
    SetLastError(0x76543210);
}
void checkStart(){calls=0;last={};}
void once(Tag tag){require(calls==1&&last.tag==tag,"original entry point called exactly once, no invented submit/copy/wait");require(GetLastError()==0x76543210,"original LastError preserved");}
VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice d,const VkCommandPoolCreateInfo* i,const VkAllocationCallbacks* a,VkCommandPool* out){hit(CreatePool);last.pointers={d,i,a,out};*out=pool;return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice d,const VkCommandBufferAllocateInfo* i,VkCommandBuffer* out){hit(Allocate);last.pointers={d,i,out};*out=command;return chosen;}
VKAPI_ATTR void VKAPI_CALL free(VkDevice d,VkCommandPool p,std::uint32_t n,const VkCommandBuffer* b){hit(Free);last.pointers={d,b};last.values={bits(p),n};}
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer b,const VkCommandBufferBeginInfo* i){hit(Begin);last.pointers={b,i};return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL end(VkCommandBuffer b){hit(End);last.pointers[0]=b;return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL reset(VkCommandBuffer b,VkCommandBufferResetFlags f){hit(Reset);last.pointers[0]=b;last.values[0]=f;return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL resetPool(VkDevice d,VkCommandPool p,VkCommandPoolResetFlags f){hit(ResetPool);last.pointers[0]=d;last.values={bits(p),f};return chosen;}
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice d,VkCommandPool p,const VkAllocationCallbacks* a){hit(DestroyPool);last.pointers={d,a};last.values[0]=bits(p);}
VKAPI_ATTR void VKAPI_CALL getQueue(VkDevice d,std::uint32_t f,std::uint32_t i,VkQueue* q){hit(GetQueue);last.pointers={d,q};last.values={f,i};*q=queue;}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer b,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags f,
    std::uint32_t m,const VkMemoryBarrier* memory,std::uint32_t n,const VkBufferMemoryBarrier* buffers,std::uint32_t k,const VkImageMemoryBarrier* images){
    hit(Barrier);last.pointers={b,memory,buffers,images};last.values={s,d,f,m,n,k};}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL barrier2(VkCommandBuffer b,const VkDependencyInfo* i){hit(tag);last.pointers={b,i};}
VKAPI_ATTR void VKAPI_CALL beginRP(VkCommandBuffer b,const VkRenderPassBeginInfo* i,VkSubpassContents c){hit(BeginRP);last.pointers={b,i};last.values[0]=c;}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL beginRP2(VkCommandBuffer b,const VkRenderPassBeginInfo* i,const VkSubpassBeginInfo* s){hit(tag);last.pointers={b,i,s};}
VKAPI_ATTR void VKAPI_CALL endRP(VkCommandBuffer b){hit(EndRP);last.pointers[0]=b;}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL endRP2(VkCommandBuffer b,const VkSubpassEndInfo* i){hit(tag);last.pointers={b,i};}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL beginDynamic(VkCommandBuffer b,const VkRenderingInfo* i){hit(tag);last.pointers={b,i};}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL endDynamic(VkCommandBuffer b){hit(tag);last.pointers[0]=b;}
VKAPI_ATTR void VKAPI_CALL direct(VkCommandBuffer b,std::uint32_t x,std::uint32_t y,std::uint32_t z){hit(Direct);last.pointers[0]=b;last.values={x,y,z};}
VKAPI_ATTR void VKAPI_CALL indirect(VkCommandBuffer b,VkBuffer i,VkDeviceSize o){hit(Indirect);last.pointers[0]=b;last.values={bits(i),o};}
template<Tag tag>VKAPI_ATTR void VKAPI_CALL base(VkCommandBuffer b,std::uint32_t x,std::uint32_t y,std::uint32_t z,std::uint32_t nx,std::uint32_t ny,std::uint32_t nz){hit(tag);last.pointers[0]=b;last.values={x,y,z,nx,ny,nz};}
bool resetInsideSubmit{};
template<Tag tag,class Info>VkResult submit(VkQueue q,std::uint32_t n,const Info* i,VkFence f){hit(tag);last.pointers={q,i};last.values={n,bits(f)};
    if(resetInsideSubmit){resetInsideSubmit=false;ro::reset(active,command,VK_SUCCESS);VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};beginInfo.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;ro::begin(active,command,&beginInfo,VK_SUCCESS);}
    SetLastError(0x76543210);return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL submit1(VkQueue q,std::uint32_t n,const VkSubmitInfo* i,VkFence f){return submit<Submit>(q,n,i,f);}
template<Tag tag>VKAPI_ATTR VkResult VKAPI_CALL submit2(VkQueue q,std::uint32_t n,const VkSubmitInfo2* i,VkFence f){return submit<tag>(q,n,i,f);}
VKAPI_ATTR void VKAPI_CALL unknown(){}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice d,const char* name){require(d==device,"GDPA original device");return unavailable&&std::strcmp(unavailable,name)==0?nullptr:reinterpret_cast<PFN_vkVoidFunction>(unknown);}
std::uintptr_t sourceObject{};
template<Tag tag,class Info,class Object>VkResult sourceCreate(VkDevice d,const Info* i,const VkAllocationCallbacks* a,Object* out){hit(tag);last.pointers={d,i,a,out};*out=handle<Object>(sourceObject);return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice d,const VkImageCreateInfo* i,const VkAllocationCallbacks* a,VkImage* out){return sourceCreate<CreateImage>(d,i,a,out);}
VKAPI_ATTR VkResult VKAPI_CALL createView(VkDevice d,const VkImageViewCreateInfo* i,const VkAllocationCallbacks* a,VkImageView* out){return sourceCreate<CreateView>(d,i,a,out);}
VKAPI_ATTR VkResult VKAPI_CALL createRP(VkDevice d,const VkRenderPassCreateInfo* i,const VkAllocationCallbacks* a,VkRenderPass* out){return sourceCreate<CreateRP>(d,i,a,out);}
VKAPI_ATTR VkResult VKAPI_CALL createFB(VkDevice d,const VkFramebufferCreateInfo* i,const VkAllocationCallbacks* a,VkFramebuffer* out){return sourceCreate<CreateFB>(d,i,a,out);}
VKAPI_ATTR VkResult VKAPI_CALL createSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo* i,const VkAllocationCallbacks* a,VkDescriptorSetLayout* out){return sourceCreate<CreateSetLayout>(d,i,a,out);}
VKAPI_ATTR VkResult VKAPI_CALL createPL(VkDevice d,const VkPipelineLayoutCreateInfo* i,const VkAllocationCallbacks* a,VkPipelineLayout* out){return sourceCreate<CreatePL>(d,i,a,out);}
VKAPI_ATTR VkResult VKAPI_CALL createDP(VkDevice d,const VkDescriptorPoolCreateInfo* i,const VkAllocationCallbacks* a,VkDescriptorPool* out){return sourceCreate<CreateDP>(d,i,a,out);}
VKAPI_ATTR VkResult VKAPI_CALL allocateSet(VkDevice d,const VkDescriptorSetAllocateInfo* i,VkDescriptorSet* out){hit(AllocateSet);last.pointers={d,i,out};*out=handle<VkDescriptorSet>(sourceObject);return chosen;}
VKAPI_ATTR void VKAPI_CALL updateSet(VkDevice d,std::uint32_t n,const VkWriteDescriptorSet* w,std::uint32_t m,const VkCopyDescriptorSet* c){hit(UpdateSet);last.pointers={d,w,c};last.values={n,m};}
VKAPI_ATTR VkResult VKAPI_CALL createPipeline(VkDevice d,VkPipelineCache cache,std::uint32_t n,const VkComputePipelineCreateInfo* i,const VkAllocationCallbacks* a,VkPipeline* out){hit(CreatePipeline);last.pointers={d,i,a,out};last.values={bits(cache),n};*out=handle<VkPipeline>(sourceObject);return chosen;}
VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer b,VkPipelineBindPoint p,VkPipeline pipeline){hit(BindPipeline);last.pointers[0]=b;last.values={std::uint64_t(p),bits(pipeline)};}
VKAPI_ATTR void VKAPI_CALL bindSets(VkCommandBuffer b,VkPipelineBindPoint p,VkPipelineLayout l,std::uint32_t first,std::uint32_t n,const VkDescriptorSet* s,std::uint32_t m,const std::uint32_t* o){hit(BindSets);last.pointers={b,s,o};last.values={std::uint64_t(p),bits(l),first,n,m};}
struct Fixture {
    DeviceState state{};VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    std::unique_ptr<xess_vk::image_inventory::Inventory<>> inventory;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkAllocationCallbacks allocator{};
    Fixture(){
        state.device=device;state.key=&marker;state.gdpa=gdpa;state.getQueue=getQueue;
        state.createCommandPool=createPool;state.allocateCommandBuffers=allocate;state.freeCommandBuffers=free;
        state.beginCommandBuffer=begin;state.endCommandBuffer=end;state.resetCommandBuffer=reset;state.resetCommandPool=resetPool;state.destroyCommandPool=destroyPool;
        state.cmdPipelineBarrier=barrier;state.cmdPipelineBarrier2=barrier2<Barrier2>;state.cmdPipelineBarrier2Khr=barrier2<Barrier2Khr>;
        state.cmdBeginRenderPass=beginRP;state.cmdBeginRenderPass2=beginRP2<BeginRP2>;state.cmdBeginRenderPass2Khr=beginRP2<BeginRP2Khr>;
        state.cmdEndRenderPass=endRP;state.cmdEndRenderPass2=endRP2<EndRP2>;state.cmdEndRenderPass2Khr=endRP2<EndRP2Khr>;
        state.cmdBeginRendering=beginDynamic<BeginDynamic>;state.cmdBeginRenderingKhr=beginDynamic<BeginDynamicKhr>;
        state.cmdEndRendering=endDynamic<EndDynamic>;state.cmdEndRenderingKhr=endDynamic<EndDynamicKhr>;
        state.cmdDispatch=direct;state.cmdDispatchIndirect=indirect;state.cmdDispatchBase=base<Base>;state.cmdDispatchBaseKhr=base<BaseKhr>;
        state.submit=submit1;state.submit2=submit2<Submit2>;state.submit2Khr=submit2<Submit2Khr>;state.submits=32;
        state.createImage=createImage;state.createImageView=createView;state.createRenderPass=createRP;state.createFramebuffer=createFB;
        state.createDescriptorSetLayout=createSetLayout;state.createPipelineLayout=createPL;state.createDescriptorPool=createDP;
        state.allocateDescriptorSets=allocateSet;state.updateDescriptorSets=updateSet;state.createComputePipelines=createPipeline;
        state.cmdBindPipeline=bindPipeline;state.cmdBindDescriptorSets=bindSets;
        inventory=std::make_unique<xess_vk::image_inventory::Inventory<>>();state.imageInventory=inventory.get();
        state.renderPass=ro::create(device,nullptr,{recordedLayoutInventory,&state});state.descriptors=di::create(device,sink);require(state.renderPass&&state.descriptors,"adapters created");
        devices=&state;active=state.renderPass;descriptors=state.descriptors;chosen=VK_SUCCESS;lineCount=0;unavailable=nullptr;
        poolInfo.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;poolInfo.queueFamilyIndex=3;
        alloc.commandPool=pool;alloc.commandBufferCount=1;alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;beginInfo.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;allocator.pUserData=this;
        checkStart();VkCommandPool p{};require(vkCreateCommandPool(device,&poolInfo,&allocator,&p)==VK_SUCCESS,"literal pool result");once(CreatePool);
        require(p==pool&&last.pointers[1]==&poolInfo&&last.pointers[2]==&allocator&&last.pointers[3]==&p,"pool original pointers/output unchanged");
        checkStart();VkCommandBuffer cb{};require(vkAllocateCommandBuffers(device,&alloc,&cb)==VK_SUCCESS,"literal allocation result");once(Allocate);
        require(cb==command&&last.pointers[1]==&alloc&&last.pointers[2]==&cb,"allocation original pointers/output unchanged");
        checkStart();VkQueue q{};vkGetDeviceQueue(device,3,1,&q);once(GetQueue);require(q==queue&&last.values[0]==3&&last.values[1]==1,"queue original arguments");
        start();
    }
    ~Fixture(){dispatchBeforeDriver=false;active=nullptr;descriptors=nullptr;devices=nullptr;ro::destroy(state.renderPass);di::destroy(state.descriptors);}
    auto facts(){return ro::recordingFacts(state.renderPass,command);}
    void start(){checkStart();require(vkBeginCommandBuffer(command,&beginInfo)==chosen,"literal begin result");once(Begin);require(last.pointers[0]==command&&last.pointers[1]==&beginInfo,"original begin pointer/flags");}
    void dispatch(){expectedOrder=facts().commonRecordOrder+1;expectedDispatchCalls=di::stats(state.descriptors).dispatchCalls+1;dispatchBeforeDriver=true;
        checkStart();vkCmdDispatch(command,7,8,9);once(Direct);dispatchBeforeDriver=false;require(last.pointers[0]==command&&last.values[0]==7&&last.values[1]==8&&last.values[2]==9,"original direct groups");}
};
void wrapperScopeAndClock(){
    Fixture f;auto facts=f.facts();require(facts.supported()&&facts.poolKnown&&facts.beginUsageFlags==VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT&&facts.queueFamily==3,"actual success hooks produce literal primary ONE_TIME facts");
    f.dispatch();require(contains("\"common_record_order\":1")&&contains("\"rp_command_generation\":1")&&contains("\"begin_usage_flags\":1"),"real descriptor consumer receives pre-dispatch recording facts");
    checkStart();vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,0,nullptr);once(Barrier);
    require(last.values[0]==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&last.values[1]==VK_PIPELINE_STAGE_TRANSFER_BIT,"barrier original stage flags");
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};pass.renderPass=handle<VkRenderPass>(0xdead);pass.framebuffer=handle<VkFramebuffer>(0xbeef);
    checkStart();vkCmdBeginRenderPass(command,&pass,VK_SUBPASS_CONTENTS_INLINE);once(BeginRP);require(last.pointers[1]==&pass,"RP original begin pointer");f.dispatch();facts=f.facts();
    require(facts.commonRecordOrder==4&&facts.scopeKnown&&facts.inRenderPass,"actual RP scope and barrier/dispatch common clock survive missing attachments");
    checkStart();vkCmdEndRenderPass(command);once(EndRP);f.dispatch();require(f.facts().commonRecordOrder==6&&f.facts().scopeKnown&&!f.facts().inRenderPass,"actual RP end/outside facts");
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};info.renderArea.extent={1,1};info.layerCount=1;
    for(bool khr:{false,true}){
        checkStart();if(khr)vkCmdBeginRenderingKHR(command,&info);else vkCmdBeginRendering(command,&info);once(khr?BeginDynamicKhr:BeginDynamic);require(last.pointers[1]==&info,"dynamic original begin pointer");
        f.dispatch();require(f.facts().scopeKnown&&f.facts().inDynamicRendering,"actual core/KHR dynamic inside fact");
        checkStart();if(khr)vkCmdEndRenderingKHR(command);else vkCmdEndRendering(command);once(khr?EndDynamicKhr:EndDynamic);
        f.dispatch();require(f.facts().scopeKnown&&!f.facts().inDynamicRendering,"actual core/KHR dynamic end fact");
    }
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};VkSubpassBeginInfo sub{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};VkSubpassEndInfo endInfo{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
    for(bool khr:{false,true}){
        checkStart();if(khr)vkCmdPipelineBarrier2KHR(command,&dep);else vkCmdPipelineBarrier2(command,&dep);once(khr?Barrier2Khr:Barrier2);require(last.pointers[1]==&dep,"barrier2 core/KHR pointer");
        checkStart();if(khr)vkCmdBeginRenderPass2KHR(command,&pass,&sub);else vkCmdBeginRenderPass2(command,&pass,&sub);once(khr?BeginRP2Khr:BeginRP2);require(last.pointers[1]==&pass&&last.pointers[2]==&sub,"RP2 core/KHR original pointers");
        require(f.facts().inRenderPass,"RP2 observed begin scope");checkStart();if(khr)vkCmdEndRenderPass2KHR(command,&endInfo);else vkCmdEndRenderPass2(command,&endInfo);once(khr?EndRP2Khr:EndRP2);
        require(last.pointers[1]==&endInfo&&f.facts().scopeKnown&&!f.facts().inRenderPass,"RP2 core/KHR original end pointer/outside scope");
    }
}
void dispatchAndSubmitAliases(){
    Fixture f;const auto indirectBuffer=handle<VkBuffer>(0x1234);
    expectedOrder=1;expectedDispatchCalls=1;dispatchBeforeDriver=true;checkStart();vkCmdDispatchIndirect(command,indirectBuffer,123456);once(Indirect);dispatchBeforeDriver=false;
    require(last.values[0]==0x1234&&last.values[1]==123456&&f.facts().commonRecordOrder==1,"indirect original buffer/offset and shared order");
    for(bool khr:{false,true}){
        expectedOrder=f.facts().commonRecordOrder+1;expectedDispatchCalls=di::stats(f.state.descriptors).dispatchCalls+1;dispatchBeforeDriver=true;checkStart();
        if(khr)vkCmdDispatchBaseKHR(command,11,12,13,21,22,23);else vkCmdDispatchBase(command,11,12,13,21,22,23);once(khr?BaseKhr:Base);dispatchBeforeDriver=false;
        require(last.values==std::array<std::uint64_t,8>{11,12,13,21,22,23,0,0},"core/KHR base coordinates/groups untouched");
    }
    for(const char* name:{"vkCmdDispatchBase","vkCmdDispatchBaseKHR","vkCmdBeginRendering","vkCmdBeginRenderingKHR","vkCmdEndRendering","vkCmdEndRenderingKHR"}){
        require(vkGetDeviceProcAddr(device,name)!=nullptr,"real supported alias exposed");unavailable=name;require(vkGetDeviceProcAddr(device,name)==nullptr,"unsupported original alias not invented");unavailable=nullptr;}
    checkStart();require(vkEndCommandBuffer(command)==VK_SUCCESS,"literal end result");once(End);require(f.facts().executable,"successful actual end observed executable");
    VkCommandBuffer cb=command;VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&cb;const auto fence=handle<VkFence>(0xabcd);
    checkStart();require(vkQueueSubmit(queue,1,&info,fence)==VK_SUCCESS,"literal submit result");once(Submit);require(last.pointers[0]==queue&&last.pointers[1]==&info&&last.values[0]==1&&last.values[1]==0xabcd,"original submit/fence unchanged");
    require(f.facts().successfulSubmissionCount==1&&!f.facts().stateKnown&&!f.facts().executable&&!f.facts().gpuComplete,"successful actual submit history does not claim GPU completion/executable");
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cbi.commandBuffer=command;cbi.deviceMask=1;
    VkSubmitInfo2 two{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};two.commandBufferInfoCount=1;two.pCommandBufferInfos=&cbi;
    for(bool khr:{false,true}){f.start();checkStart();require(vkEndCommandBuffer(command)==VK_SUCCESS,"end before submit2");once(End);
        checkStart();require((khr?vkQueueSubmit2KHR(queue,1,&two,fence):vkQueueSubmit2(queue,1,&two,fence))==VK_SUCCESS,"literal core/KHR submit2 result");once(khr?Submit2Khr:Submit2);
        require(last.pointers[1]==&two&&last.values[1]==0xabcd&&f.facts().successfulSubmissionCount==1,"core/KHR submit2 tickets preserve original info/fence and reset per recording");}
    f.start();checkStart();require(vkEndCommandBuffer(command)==VK_SUCCESS,"end before stale submit");once(End);const auto generation=f.facts().rpRecordingGeneration;
    resetInsideSubmit=true;checkStart();require(vkQueueSubmit(queue,1,&info,fence)==VK_SUCCESS,"literal stale original submit");once(Submit);
    require(f.facts().rpRecordingGeneration>generation&&f.facts().recording&&!f.facts().successfulSubmissionCount&&ro::stats(f.state.renderPass).recordingFactsStaleSubmissions==1,"actual before/after submit captures exact generation across reset inside driver stub");
}
void wrapperFailuresAndReuse(){
    Fixture f;auto old=f.facts();chosen=VK_ERROR_OUT_OF_HOST_MEMORY;f.start();require(f.facts().rpRecordingGeneration==old.rpRecordingGeneration&&!f.facts().stateKnown,"actual failed begin cannot invent new identity");
    chosen=VK_SUCCESS;f.start();require(f.facts().supported(),"actual successful begin recovers");chosen=VK_ERROR_OUT_OF_HOST_MEMORY;
    checkStart();require(vkResetCommandBuffer(command,VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT)==chosen,"literal failed reset");once(Reset);require(last.values[0]==VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT&&!f.facts().stateKnown,"original reset flags with unknown failed state");
    chosen=VK_SUCCESS;checkStart();require(vkResetCommandPool(device,pool,VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT)==VK_SUCCESS,"literal pool reset");once(ResetPool);require(!f.facts().rpRecordingGeneration,"successful actual pool reset invalidates key");f.start();
    old=f.facts();checkStart();VkCommandBuffer cb=command;vkFreeCommandBuffers(device,pool,1,&cb);once(Free);require(last.pointers[1]==&cb&&last.values[0]==bits(pool)&&!f.facts().allocationKnown,"actual free invalidates recording with original array");
    checkStart();require(vkAllocateCommandBuffers(device,&f.alloc,&cb)==VK_SUCCESS,"allocate numeric CB reuse");once(Allocate);f.start();require(f.facts().rpRecordingGeneration>old.rpRecordingGeneration,"numeric CB reuse fresh recording generation");
    const auto poolGeneration=f.facts().poolGeneration;checkStart();vkDestroyCommandPool(device,pool,&f.allocator);once(DestroyPool);require(last.pointers[1]==&f.allocator&&!f.facts().allocationKnown,"actual pool destroy invalidates child before driver");
    checkStart();VkCommandPool p{};require(vkCreateCommandPool(device,&f.poolInfo,&f.allocator,&p)==VK_SUCCESS,"pool numeric handle reuse");once(CreatePool);
    checkStart();require(vkAllocateCommandBuffers(device,&f.alloc,&cb)==VK_SUCCESS,"reallocate pool reused child");once(Allocate);f.start();require(f.facts().poolGeneration>poolGeneration&&f.facts().supported(),"actual numeric pool reuse gets new pool identity and healthy recording");
    for(const auto& line:lines)require(!std::strstr(line.data(),"\"copy_permitted\":true")&&!std::strstr(line.data(),"\"gpu_complete\":true"),"all actual wrapper diagnostics retain false GPU/copy claims");
}
void actualRecordedSourcePrefix(){
    for(unsigned variant=0;variant<3;++variant){const bool khr=variant==1;
        Fixture f;if(variant==2){f.beginInfo.flags=0;checkStart();require(vkResetCommandBuffer(command,0)==VK_SUCCESS,"real reusable recording reset");once(Reset);f.start();}
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.format=VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent={64,32,1};imageInfo.mipLevels=imageInfo.arrayLayers=1;imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;imageInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;imageInfo.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImage image{};sourceObject=0x801;checkStart();require(vkCreateImage(device,&imageInfo,&f.allocator,&image)==VK_SUCCESS,"real raw source image creation");once(CreateImage);
        require(last.pointers[1]==&imageInfo&&last.pointers[2]==&f.allocator&&bits(image)==0x801,"actual image original input/output pointers");
        auto otherInfo=imageInfo;otherInfo.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT;VkImage other{};sourceObject=0x802;
        checkStart();require(vkCreateImage(device,&otherInfo,nullptr,&other)==VK_SUCCESS,"real nonattachment image creation");once(CreateImage);
        require(ro::stats(f.state.renderPass).ignoredNonAttachmentImages==1,"ordinary nonattachment remains outside legacy RP registration");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};viewInfo.image=image;viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;viewInfo.format=imageInfo.format;viewInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView view{};sourceObject=0x803;checkStart();require(vkCreateImageView(device,&viewInfo,nullptr,&view)==VK_SUCCESS,"actual source view");once(CreateView);
        VkAttachmentDescription attachment{};attachment.format=imageInfo.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference;
        VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};passInfo.attachmentCount=1;passInfo.pAttachments=&attachment;passInfo.subpassCount=1;passInfo.pSubpasses=&subpass;
        VkRenderPass pass{};sourceObject=0x804;checkStart();require(vkCreateRenderPass(device,&passInfo,nullptr,&pass)==VK_SUCCESS,"actual ordinary pass");once(CreateRP);
        VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fbInfo.renderPass=pass;fbInfo.attachmentCount=1;fbInfo.pAttachments=&view;fbInfo.width=64;fbInfo.height=32;fbInfo.layers=1;
        VkFramebuffer fb{};sourceObject=0x805;checkStart();require(vkCreateFramebuffer(device,&fbInfo,nullptr,&fb)==VK_SUCCESS,"actual ordinary framebuffer");once(CreateFB);
        VkDescriptorSetLayoutBinding binding{5,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};setInfo.bindingCount=1;setInfo.pBindings=&binding;
        VkDescriptorSetLayout setLayout{};sourceObject=0x806;checkStart();require(vkCreateDescriptorSetLayout(device,&setInfo,nullptr,&setLayout)==VK_SUCCESS,"actual set layout");once(CreateSetLayout);
        VkPipelineLayoutCreateInfo plInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plInfo.setLayoutCount=1;plInfo.pSetLayouts=&setLayout;
        VkPipelineLayout pl{};sourceObject=0x807;checkStart();require(vkCreatePipelineLayout(device,&plInfo,nullptr,&pl)==VK_SUCCESS,"actual pipeline layout");once(CreatePL);
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1};VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.maxSets=1;poolInfo.poolSizeCount=1;poolInfo.pPoolSizes=&size;
        VkDescriptorPool dp{};sourceObject=0x808;checkStart();require(vkCreateDescriptorPool(device,&poolInfo,nullptr,&dp)==VK_SUCCESS,"actual descriptor pool");once(CreateDP);
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};alloc.descriptorPool=dp;alloc.descriptorSetCount=1;alloc.pSetLayouts=&setLayout;
        VkDescriptorSet set{};sourceObject=0x809;checkStart();require(vkAllocateDescriptorSets(device,&alloc,&set)==VK_SUCCESS,"actual descriptor set");once(AllocateSet);
        VkDescriptorImageInfo descriptor{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=5;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=&descriptor;
        checkStart();vkUpdateDescriptorSets(device,1,&write,0,nullptr);once(UpdateSet);require(last.pointers[1]==&write&&last.values[0]==1,"actual descriptor original write");
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipelineInfo.layout=pl;pipelineInfo.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pipelineInfo.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipelineInfo.stage.module=handle<VkShaderModule>(0x900);pipelineInfo.stage.pName="main";
        VkPipeline pipeline{};sourceObject=0x80a;checkStart();require(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&pipeline)==VK_SUCCESS,"actual compute pipeline");once(CreatePipeline);
        checkStart();vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);once(BindPipeline);
        checkStart();vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&set,0,nullptr);once(BindSets);
        VkImageMemoryBarrier raw{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};raw.image=image;raw.subresourceRange=viewInfo.subresourceRange;raw.srcQueueFamilyIndex=raw.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;raw.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;raw.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        raw.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;raw.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;auto unrelated=raw;unrelated.image=other;unrelated.srcQueueFamilyIndex=3;unrelated.dstQueueFamilyIndex=4;std::array<VkImageMemoryBarrier,2> before{unrelated,raw};
        checkStart();vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,2,before.data());once(Barrier);
        require(last.pointers[3]==before.data()&&last.values[5]==2,"actual full image barrier array/count unchanged");
        VkRenderPassBeginInfo beginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};beginInfo.renderPass=pass;beginInfo.framebuffer=fb;beginInfo.renderArea.extent={64,32};
        checkStart();vkCmdBeginRenderPass(command,&beginInfo,VK_SUBPASS_CONTENTS_INLINE);once(BeginRP);checkStart();vkCmdEndRenderPass(command);once(EndRP);
        VkImageMemoryBarrier2 after{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};after.image=image;after.subresourceRange=viewInfo.subresourceRange;after.srcQueueFamilyIndex=after.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        after.oldLayout=VK_IMAGE_LAYOUT_GENERAL;after.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;after.srcStageMask=after.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;after.srcAccessMask=VK_ACCESS_2_SHADER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=1;dep.pImageMemoryBarriers=&after;
        checkStart();if(khr)vkCmdPipelineBarrier2KHR(command,&dep);else vkCmdPipelineBarrier2(command,&dep);once(khr?Barrier2Khr:Barrier2);require(last.pointers[1]==&dep,"core/KHR actual image barrier2 info pointer unchanged");
        const auto record=f.inventory->lookup(bits(image)).record;require(!di::stats(f.state.descriptors).sources[std::size_t(di::Source::RawColor)].image.handle,"source selection is late, after full recorded chain");
        di::observeSource(f.state.descriptors,di::Source::RawColor,{record.image,record.generation},99);lineCount=0;f.dispatch();
        require(di::stats(f.state.descriptors).metadata.acceptedDispatches==1&&contains("\"metadata_accepted\":true")&&contains("\"resolved_count\":1")
            &&contains("\"recorded_layout_resolved\":true")&&contains("\"recorded_prefix_supported\":true")&&contains("\"recorded_layout\":5"),"real pre-dispatch consumer resolves matching late source while descriptor acceptance remains independent");
        require(contains("\"prefix_order\":5")&&contains("\"retained_explicit_images\":3")&&contains("\"direct_passes\":1")&&contains("\"copied_aspect\":1"),"same full wrapper clock/inventory side branch and exact source range consumed");
        const auto facts=f.facts();const auto resolved=ro::resolveSourceAt(f.state.renderPass,facts,{record.image,record.generation},viewInfo.subresourceRange,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        require(resolved.recordedLayoutResolved&&resolved.anchor.order==4&&resolved.lastEvent.explicitScopesKnown&&resolved.lastEvent.dstAccess==VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,"actual recorded scope fields are copied, no RP scopes guessed");
        if(variant==2)require(facts.beginUsageFlags==0&&!facts.supported()&&(facts.reasons&xess_vk::bit(CR::NotOneTime))&&resolved.recordedPrefixSupported,"real flags-zero primary recording resolves a layout prefix while preserving reusable/NotOneTime facts");
        f.inventory->destroying(bits(other));f.inventory->created(bits(other),f.inventory->lookup(bits(image)).record.description);lineCount=0;f.dispatch();
        require(di::stats(f.state.descriptors).metadata.acceptedDispatches==2&&contains("\"resolved_count\":0")&&contains("\"recorded_prefix_supported\":false"),"common inventory ABA of nonattachment side branch refuses layout result without changing accepted descriptor");
        for(std::size_t n=0;n<lineCount;++n)require(!std::strstr(lines[n].data(),"\"layout_verified\":true")&&!std::strstr(lines[n].data(),"\"copy_permitted\":true")&&!std::strstr(lines[n].data(),"\"gpu_complete\":true"),"positive and refused actual source results keep all GPU/layout/copy proof claims false");
    }
}
}
#ifndef XESS_RAWCOLOR_LAYER_FIXTURE
int main(){using namespace recording_layer_fixture;try{
    SetEnvironmentVariableW(L"XESS_OBSERVER_LOG",nullptr);
    // Initialize the existing unrelated optional file logger before forwarding
    // checks; adapters and all observed calls still preserve each driver error.
    InitOnceExecuteOnce(&logOnce,initializeLog,nullptr,nullptr);
    wrapperScopeAndClock();dispatchAndSubmitAliases();wrapperFailuresAndReuse();actualRecordedSourcePrefix();
    std::cout<<"{\"stage\":\"command-recording-actual-wrappers\",\"passed\":true,\"checks\":"<<checks<<",\"realVulkanCalls\":0,\"gpuTouched\":false,\"gameTouched\":false,\"copyPermissionGranted\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
#endif
