// Own-process dispatch fixture only. The real layer source is compiled into this
// EXE so every wrapper below is the production wrapper. No Vulkan loader/driver,
// game module, MinHook, or FSR detour is loaded or invoked.
#define XESS_METADATA_CONSUMER_TEST 1
#include "vulkan_observer.cpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace {
unsigned checks{}, driverCalls{};
void require(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
struct FakeDispatch {void* key;};
int dispatchMarker;
FakeDispatch fakeDevice{&dispatchMarker},fakeCommand{&dispatchMarker},fakeQueue{&dispatchMarker};
VkDevice ownDevice=reinterpret_cast<VkDevice>(&fakeDevice);
VkCommandBuffer ownCommand=reinterpret_cast<VkCommandBuffer>(&fakeCommand);
VkQueue ownQueue=reinterpret_cast<VkQueue>(&fakeQueue);
VkCommandPool ownPool=reinterpret_cast<VkCommandPool>(0x1000);
const void* observedFirst{};const void* observedSecond{};VkResult chosen=VK_SUCCESS;
std::uint32_t observedCount{},observedFlags{};
const char* unavailableProc{};
unsigned procQueries{};
const char* queriedName{};
VKAPI_ATTR VkResult VKAPI_CALL createPoolStub(VkDevice d,const VkCommandPoolCreateInfo* i,const VkAllocationCallbacks* a,VkCommandPool* p){
    require(d==ownDevice&&a==nullptr,"pool parameters unchanged");++driverCalls;observedFirst=i;*p=ownPool;return chosen;
}
VKAPI_ATTR VkResult VKAPI_CALL allocateStub(VkDevice d,const VkCommandBufferAllocateInfo* i,VkCommandBuffer* out){
    require(d==ownDevice,"allocate device unchanged");++driverCalls;observedFirst=i;*out=ownCommand;return chosen;
}
VKAPI_ATTR VkResult VKAPI_CALL beginStub(VkCommandBuffer b,const VkCommandBufferBeginInfo* i){
    require(b==ownCommand,"begin handle unchanged");++driverCalls;observedFirst=i;return chosen;
}
VKAPI_ATTR VkResult VKAPI_CALL endStub(VkCommandBuffer b){require(b==ownCommand,"end handle unchanged");++driverCalls;return chosen;}
VKAPI_ATTR void VKAPI_CALL barrierStub(VkCommandBuffer b,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
    std::uint32_t mc,const VkMemoryBarrier* m,std::uint32_t bc,const VkBufferMemoryBarrier* buffers,std::uint32_t ic,const VkImageMemoryBarrier* images){
    require(b==ownCommand&&src==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT&&dst==VK_PIPELINE_STAGE_TRANSFER_BIT,"barrier stages unchanged");
    require(mc==1&&bc==1&&m&&buffers,"nonimage barriers retained");++driverCalls;observedFlags=flags;observedCount=ic;observedFirst=images;observedSecond=m;
}
VKAPI_ATTR void VKAPI_CALL barrier2Stub(VkCommandBuffer b,const VkDependencyInfo* i){
    require(b==ownCommand,"barrier2 buffer unchanged");++driverCalls;observedFirst=i;
}
VKAPI_ATTR VkResult VKAPI_CALL submitStub(VkQueue q,std::uint32_t count,const VkSubmitInfo* i,VkFence fence){
    require(q==ownQueue&&fence==VK_NULL_HANDLE,"submit queue/fence unchanged");++driverCalls;observedFirst=i;observedCount=count;return chosen;
}
VKAPI_ATTR VkResult VKAPI_CALL submit2Stub(VkQueue q,std::uint32_t count,const VkSubmitInfo2* i,VkFence fence){
    require(q==ownQueue&&fence==VK_NULL_HANDLE,"submit2 queue/fence unchanged");++driverCalls;observedFirst=i;observedCount=count;return chosen;
}
VKAPI_ATTR void VKAPI_CALL waitStub(VkCommandBuffer b,std::uint32_t count,const VkEvent* events,const VkDependencyInfo* i){
    require(b==ownCommand&&count==1,"event parameters unchanged");++driverCalls;observedFirst=events;observedSecond=i;
}
VKAPI_ATTR void VKAPI_CALL renderStub(VkCommandBuffer b,const VkRenderPassBeginInfo* i,const VkSubpassBeginInfo* j){
    require(b==ownCommand,"render pass buffer unchanged");++driverCalls;observedFirst=i;observedSecond=j;
}
VKAPI_ATTR void VKAPI_CALL executeStub(VkCommandBuffer b,std::uint32_t count,const VkCommandBuffer* buffers){
    require(b==ownCommand,"execute primary unchanged");++driverCalls;observedCount=count;observedFirst=buffers;
}
VKAPI_ATTR void VKAPI_CALL unknownStub(){}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gdpaStub(VkDevice d,const char* name){
    require(d==ownDevice,"GDPA receives original device");++procQueries;queriedName=name;
    if(unavailableProc&&std::strcmp(name,unavailableProc)==0)return nullptr;
    if(std::strcmp(name,"vkUnsupportedFixtureEXT")==0)return nullptr;
    return reinterpret_cast<PFN_vkVoidFunction>(unknownStub);
}
void resetObserved(){driverCalls=0;observedFirst=observedSecond=nullptr;observedCount=observedFlags=0;}

namespace rp_fixture {
// Every alias has a distinct downstream stub: a core wrapper accidentally
// selecting its KHR slot (or vice versa) therefore fails the test.
enum Tag : unsigned {
    CreateView=1,DestroyView,CreatePass,CreatePass2,CreatePass2Khr,DestroyPass,
    CreateFramebuffer,DestroyFramebuffer,BeginPass,BeginPass2,BeginPass2Khr,
    NextPass,NextPass2,NextPass2Khr,EndPass,EndPass2,EndPass2Khr,
    BeginRendering,BeginRenderingKhr,EndRendering,EndRenderingKhr
};
const void* expectedFirst{};
const void* expectedSecond{};
const void* expectedOutput{};
const VkAllocationCallbacks* expectedAllocator{};
std::uintptr_t expectedObject{},returnedObject{};
VkSubpassContents expectedContents{};
unsigned actualTag{};
std::vector<std::string> metadataLines;
template<class T>T handle(std::uintptr_t value){return reinterpret_cast<T>(value);}
void called(unsigned tag){++driverCalls;actualTag=tag;}
void reset(){resetObserved();actualTag=0;}
void once(unsigned tag){require(driverCalls==1&&actualTag==tag,"exactly one correct downstream entry point");}
void sink(const char* line){metadataLines.emplace_back(line);}
std::size_t layoutLineCount(){
    std::size_t count=0;
    for(const auto& line:metadataLines)if(line.find("\"event\":\"rp_layout_operation\"")!=std::string::npos){
        ++count;
        for(const auto* flag:{"\"complete_hook_coverage\":false","\"submitted_layout_known\":false",
                             "\"gpu_complete\":false","\"copy_permitted\":false"})
            require(line.find(flag)!=std::string::npos,"RP evidence cannot imply completion, layout certainty or copy permission");
    }
    return count;
}
template<unsigned tag,class Info,class Object>
VkResult created(VkDevice d,const Info* info,const VkAllocationCallbacks* allocator,Object* output){
    require(d==ownDevice&&info==expectedFirst&&allocator==expectedAllocator&&output==expectedOutput,
        "create preserves device, info, nonnull allocator and output pointer");
    called(tag);*output=handle<Object>(returnedObject);return chosen;
}
template<unsigned tag,class Object>
void destroyed(VkDevice d,Object object,const VkAllocationCallbacks* allocator){
    require(d==ownDevice&&reinterpret_cast<std::uintptr_t>(object)==expectedObject&&allocator==expectedAllocator,
        "destroy preserves device, object and nonnull allocator");called(tag);
}
VKAPI_ATTR VkResult VKAPI_CALL createView(VkDevice d,const VkImageViewCreateInfo* i,const VkAllocationCallbacks* a,VkImageView* o){return created<CreateView>(d,i,a,o);}
VKAPI_ATTR void VKAPI_CALL destroyView(VkDevice d,VkImageView o,const VkAllocationCallbacks* a){destroyed<DestroyView>(d,o,a);}
VKAPI_ATTR VkResult VKAPI_CALL createPass(VkDevice d,const VkRenderPassCreateInfo* i,const VkAllocationCallbacks* a,VkRenderPass* o){return created<CreatePass>(d,i,a,o);}
template<unsigned tag>VKAPI_ATTR VkResult VKAPI_CALL createPass2(VkDevice d,const VkRenderPassCreateInfo2* i,const VkAllocationCallbacks* a,VkRenderPass* o){return created<tag>(d,i,a,o);}
VKAPI_ATTR void VKAPI_CALL destroyPass(VkDevice d,VkRenderPass o,const VkAllocationCallbacks* a){destroyed<DestroyPass>(d,o,a);}
VKAPI_ATTR VkResult VKAPI_CALL createFramebuffer(VkDevice d,const VkFramebufferCreateInfo* i,const VkAllocationCallbacks* a,VkFramebuffer* o){return created<CreateFramebuffer>(d,i,a,o);}
VKAPI_ATTR void VKAPI_CALL destroyFramebuffer(VkDevice d,VkFramebuffer o,const VkAllocationCallbacks* a){destroyed<DestroyFramebuffer>(d,o,a);}
VKAPI_ATTR void VKAPI_CALL beginPass(VkCommandBuffer b,const VkRenderPassBeginInfo* i,VkSubpassContents contents){
    require(b==ownCommand&&i==expectedFirst&&contents==expectedContents,"legacy BeginRenderPass exact parameters");called(BeginPass);
}
template<unsigned tag>VKAPI_ATTR void VKAPI_CALL beginPass2(VkCommandBuffer b,const VkRenderPassBeginInfo* i,const VkSubpassBeginInfo* begin){
    require(b==ownCommand&&i==expectedFirst&&begin==expectedSecond,"BeginRenderPass2 exact pointers");called(tag);
}
VKAPI_ATTR void VKAPI_CALL nextPass(VkCommandBuffer b,VkSubpassContents contents){
    require(b==ownCommand&&contents==expectedContents,"legacy NextSubpass exact contents");called(NextPass);
}
template<unsigned tag>VKAPI_ATTR void VKAPI_CALL nextPass2(VkCommandBuffer b,const VkSubpassBeginInfo* begin,const VkSubpassEndInfo* end){
    require(b==ownCommand&&begin==expectedFirst&&end==expectedSecond,"NextSubpass2 exact pointers and order");called(tag);
}
VKAPI_ATTR void VKAPI_CALL endPass(VkCommandBuffer b){require(b==ownCommand,"legacy EndRenderPass exact command");called(EndPass);}
template<unsigned tag>VKAPI_ATTR void VKAPI_CALL endPass2(VkCommandBuffer b,const VkSubpassEndInfo* end){
    require(b==ownCommand&&end==expectedFirst,"EndRenderPass2 exact pointer");called(tag);
}
template<unsigned tag>VKAPI_ATTR void VKAPI_CALL beginRendering(VkCommandBuffer b,const VkRenderingInfo* i){
    require(b==ownCommand&&i==expectedFirst,"BeginRendering exact pointer");called(tag);
}
template<unsigned tag>VKAPI_ATTR void VKAPI_CALL endRendering(VkCommandBuffer b){require(b==ownCommand,"EndRendering exact command");called(tag);}
FakeDispatch fakeInstance{&dispatchMarker};
const auto ownInstance=reinterpret_cast<VkInstance>(&fakeInstance);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gipaStub(VkInstance instance,const char* name){
    require(instance==ownInstance,"GIPA receives original instance");++procQueries;queriedName=name;
    return unavailableProc&&std::strcmp(name,unavailableProc)==0 ? nullptr : reinterpret_cast<PFN_vkVoidFunction>(unknownStub);
}
void initialize(DeviceState& state){
    state.device=ownDevice;state.key=&dispatchMarker;state.gdpa=gdpaStub;
    state.createImageView=createView;state.destroyImageView=destroyView;
    state.createRenderPass=createPass;state.createRenderPass2=createPass2<CreatePass2>;state.createRenderPass2Khr=createPass2<CreatePass2Khr>;
    state.destroyRenderPass=destroyPass;state.createFramebuffer=createFramebuffer;state.destroyFramebuffer=destroyFramebuffer;
    state.cmdBeginRenderPass=beginPass;state.cmdBeginRenderPass2=beginPass2<BeginPass2>;state.cmdBeginRenderPass2Khr=beginPass2<BeginPass2Khr>;
    state.cmdNextSubpass=nextPass;state.cmdNextSubpass2=nextPass2<NextPass2>;state.cmdNextSubpass2Khr=nextPass2<NextPass2Khr>;
    state.cmdEndRenderPass=endPass;state.cmdEndRenderPass2=endPass2<EndPass2>;state.cmdEndRenderPass2Khr=endPass2<EndPass2Khr>;
    state.cmdBeginRendering=beginRendering<BeginRendering>;state.cmdBeginRenderingKhr=beginRendering<BeginRenderingKhr>;
    state.cmdEndRendering=endRendering<EndRendering>;state.cmdEndRenderingKhr=endRendering<EndRenderingKhr>;
    state.allocateCommandBuffers=allocateStub;state.beginCommandBuffer=beginStub;state.endCommandBuffer=endStub;
    state.submit=submitStub;state.submit2=submit2Stub;state.submit2Khr=submit2Stub;
}
template<class Object,class Info,class Create>
void checkCreate(Create wrapper,unsigned tag,const Info& info,const VkAllocationCallbacks& allocator){
    for(const auto result:{VK_SUCCESS,VK_ERROR_OUT_OF_HOST_MEMORY}){
        auto output=handle<Object>(0xdead);const Info before=info;
        expectedFirst=&info;expectedOutput=&output;expectedAllocator=&allocator;returnedObject=0x8000+tag;
        chosen=result;reset();require(wrapper(ownDevice,&info,&allocator,&output)==result,"create literal driver result");once(tag);
        require(output==handle<Object>(returnedObject),"create output is literal driver output even on failure");
        require(std::memcmp(&info,&before,sizeof(info))==0,"create info not rewritten");
    }
}
template<class Object,class Destroy>
void checkDestroy(Destroy wrapper,unsigned tag,const VkAllocationCallbacks& allocator){
    expectedObject=0x8100+tag;expectedAllocator=&allocator;reset();wrapper(ownDevice,handle<Object>(expectedObject),&allocator);once(tag);
}
void forwarding(){
    DeviceState state{};initialize(state);devices=&state;
    // Observer contexts deliberately null here: unsupported extension parameters
    // must reach the driver untouched regardless of diagnostics availability.
    VkBaseInStructure extension{};extension.sType=VK_STRUCTURE_TYPE_MAX_ENUM;
    VkAllocationCallbacks allocator{};allocator.pUserData=&state;
    VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;view.pNext=&extension;view.image=handle<VkImage>(0xa010);
    VkRenderPassCreateInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;pass.pNext=&extension;
    VkRenderPassCreateInfo2 pass2{};pass2.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;pass2.pNext=&extension;
    VkFramebufferCreateInfo framebuffer{};framebuffer.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;framebuffer.pNext=&extension;
    checkCreate<VkImageView>(vkCreateImageView,CreateView,view,allocator);
    checkCreate<VkRenderPass>(vkCreateRenderPass,CreatePass,pass,allocator);
    checkCreate<VkRenderPass>(vkCreateRenderPass2,CreatePass2,pass2,allocator);
    checkCreate<VkRenderPass>(vkCreateRenderPass2KHR,CreatePass2Khr,pass2,allocator);
    checkCreate<VkFramebuffer>(vkCreateFramebuffer,CreateFramebuffer,framebuffer,allocator);
    checkDestroy<VkImageView>(vkDestroyImageView,DestroyView,allocator);
    checkDestroy<VkRenderPass>(vkDestroyRenderPass,DestroyPass,allocator);
    checkDestroy<VkFramebuffer>(vkDestroyFramebuffer,DestroyFramebuffer,allocator);

    VkRenderPassBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;begin.pNext=&extension;
    begin.renderPass=handle<VkRenderPass>(0xa020);begin.framebuffer=handle<VkFramebuffer>(0xa030);begin.renderArea={{3,5},{64,32}};
    VkSubpassBeginInfo subpass{};subpass.sType=VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;subpass.pNext=&extension;
    subpass.contents=VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS;
    VkSubpassEndInfo end{};end.sType=VK_STRUCTURE_TYPE_SUBPASS_END_INFO;end.pNext=&extension;
    expectedContents=subpass.contents;expectedFirst=&begin;
    reset();vkCmdBeginRenderPass(ownCommand,&begin,subpass.contents);once(BeginPass);
    expectedSecond=&subpass;
    reset();vkCmdBeginRenderPass2(ownCommand,&begin,&subpass);once(BeginPass2);
    reset();vkCmdBeginRenderPass2KHR(ownCommand,&begin,&subpass);once(BeginPass2Khr);
    reset();vkCmdNextSubpass(ownCommand,subpass.contents);once(NextPass);
    expectedFirst=&subpass;expectedSecond=&end;
    reset();vkCmdNextSubpass2(ownCommand,&subpass,&end);once(NextPass2);
    reset();vkCmdNextSubpass2KHR(ownCommand,&subpass,&end);once(NextPass2Khr);
    reset();vkCmdEndRenderPass(ownCommand);once(EndPass);
    expectedFirst=&end;reset();vkCmdEndRenderPass2(ownCommand,&end);once(EndPass2);
    reset();vkCmdEndRenderPass2KHR(ownCommand,&end);once(EndPass2Khr);
    VkRenderingAttachmentInfo color{};color.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;color.imageView=handle<VkImageView>(0xa040);
    VkRenderingInfo rendering{};rendering.sType=VK_STRUCTURE_TYPE_RENDERING_INFO;rendering.pNext=&extension;
    rendering.renderArea=begin.renderArea;rendering.layerCount=1;rendering.colorAttachmentCount=1;rendering.pColorAttachments=&color;
    expectedFirst=&rendering;reset();vkCmdBeginRendering(ownCommand,&rendering);once(BeginRendering);
    reset();vkCmdBeginRenderingKHR(ownCommand,&rendering);once(BeginRenderingKhr);
    reset();vkCmdEndRendering(ownCommand);once(EndRendering);
    reset();vkCmdEndRenderingKHR(ownCommand);once(EndRenderingKhr);

    struct Entry{const char* name;PFN_vkVoidFunction hook;};
#define RP_ENTRY(fn) Entry{#fn,reinterpret_cast<PFN_vkVoidFunction>(fn)}
    const Entry entries[]={RP_ENTRY(vkCreateImageView),RP_ENTRY(vkDestroyImageView),RP_ENTRY(vkCreateRenderPass),
        RP_ENTRY(vkCreateRenderPass2),RP_ENTRY(vkCreateRenderPass2KHR),RP_ENTRY(vkDestroyRenderPass),
        RP_ENTRY(vkCreateFramebuffer),RP_ENTRY(vkDestroyFramebuffer),RP_ENTRY(vkCmdBeginRenderPass),
        RP_ENTRY(vkCmdBeginRenderPass2),RP_ENTRY(vkCmdBeginRenderPass2KHR),RP_ENTRY(vkCmdNextSubpass),
        RP_ENTRY(vkCmdNextSubpass2),RP_ENTRY(vkCmdNextSubpass2KHR),RP_ENTRY(vkCmdEndRenderPass),
        RP_ENTRY(vkCmdEndRenderPass2),RP_ENTRY(vkCmdEndRenderPass2KHR),RP_ENTRY(vkCmdBeginRendering),
        RP_ENTRY(vkCmdBeginRenderingKHR),RP_ENTRY(vkCmdEndRendering),RP_ENTRY(vkCmdEndRenderingKHR)};
#undef RP_ENTRY
    InstanceState instance{};instance.instance=ownInstance;instance.key=&dispatchMarker;instance.gipa=gipaStub;instances=&instance;
    for(const auto& entry:entries)for(const bool missing:{false,true}){
        unavailableProc=missing?entry.name:nullptr;procQueries=0;queriedName=nullptr;
        require(vkGetDeviceProcAddr(ownDevice,entry.name)==(missing?nullptr:entry.hook),"GDPA availability follows exact downstream name");
        require(procQueries==1&&queriedName==entry.name,"GDPA makes one unchanged query");
        procQueries=0;queriedName=nullptr;
        require(vkGetInstanceProcAddr(ownInstance,entry.name)==(missing?nullptr:entry.hook),"GIPA availability follows exact downstream name");
        require(procQueries==1&&queriedName==entry.name,"GIPA makes one unchanged query");
    }
    unavailableProc=nullptr;instances=nullptr;devices=nullptr;
}
template<class Info,class Object,class Create>
void invokeCreate(Create wrapper,unsigned tag,const Info& info,const VkAllocationCallbacks& allocator,
                  Object& object,std::uintptr_t value,VkResult result){
    expectedFirst=&info;expectedOutput=&object;expectedAllocator=&allocator;returnedObject=value;
    chosen=result;reset();require(wrapper(ownDevice,&info,&allocator,&object)==result,"metadata create driver result preserved");
    once(tag);require(object==handle<Object>(value),"metadata create driver output preserved");
}
void metadataPublication(){
    namespace rp=xess_vk::render_pass_observer;
    // Fresh contexts isolate all three alias paths. Setup feeds the same copied
    // image inventory contract as the layer; every tested object/command/submit
    // afterward goes through the actual production wrapper above.
    for(unsigned flavor=0;flavor!=3;++flavor){
        DeviceState state{};initialize(state);state.renderPass=rp::create(ownDevice,sink);
        require(state.renderPass!=nullptr,"render-pass metadata context constructed");devices=&state;metadataLines.clear();
        rp::queue(state.renderPass,ownQueue,3);
        VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;image.imageType=VK_IMAGE_TYPE_2D;
        image.format=VK_FORMAT_R8G8B8A8_UNORM;image.extent={64,32,1};image.mipLevels=image.arrayLayers=1;
        image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        xess_vk::image_inventory::Record record{0x9000,1,{37,64,32,1,1,1,1,image.usage,0,0,0,0}};
        rp::imageCreated(state.renderPass,&image,handle<VkImage>(record.image),VK_SUCCESS,&record);
        rp::select(state.renderPass,rp::Role::RealColor,record);
        const auto baseRegistrations=rp::stats(state.renderPass).metadata.registrations;
        require(baseRegistrations==1,"one successful image registered");
        VkAllocationCallbacks allocator{};allocator.pUserData=&state;
        VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image=handle<VkImage>(record.image);view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=image.format;
        view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkAttachmentDescription attachment{};attachment.format=image.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference;
        VkRenderPassCreateInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass.attachmentCount=1;pass.pAttachments=&attachment;pass.subpassCount=1;pass.pSubpasses=&subpass;
        VkAttachmentDescription2 attachment2{};attachment2.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        attachment2.format=attachment.format;attachment2.samples=attachment.samples;
        attachment2.loadOp=attachment.loadOp;attachment2.storeOp=attachment.storeOp;
        attachment2.initialLayout=attachment.initialLayout;attachment2.finalLayout=attachment.finalLayout;
        VkAttachmentReference2 reference2{};reference2.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        reference2.attachment=0;reference2.layout=reference.layout;reference2.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        VkSubpassDescription2 subpass2{};subpass2.sType=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        subpass2.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass2.colorAttachmentCount=1;subpass2.pColorAttachments=&reference2;
        VkRenderPassCreateInfo2 pass2{};pass2.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
        pass2.attachmentCount=1;pass2.pAttachments=&attachment2;pass2.subpassCount=1;pass2.pSubpasses=&subpass2;
        VkImageView viewObject=handle<VkImageView>(0x9001);VkRenderPass passObject=handle<VkRenderPass>(0x9002);
        VkFramebuffer framebufferObject=handle<VkFramebuffer>(0x9003);
        VkFramebufferCreateInfo framebuffer{};framebuffer.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass=passObject;framebuffer.attachmentCount=1;framebuffer.pAttachments=&viewObject;
        framebuffer.width=64;framebuffer.height=32;framebuffer.layers=1;
        invokeCreate(vkCreateImageView,CreateView,view,allocator,viewObject,0x9001,VK_ERROR_OUT_OF_HOST_MEMORY);
        invokeCreate(vkCreateRenderPass,CreatePass,pass,allocator,passObject,0x9002,VK_ERROR_OUT_OF_HOST_MEMORY);
        invokeCreate(vkCreateRenderPass2,CreatePass2,pass2,allocator,passObject,0x9002,VK_ERROR_OUT_OF_HOST_MEMORY);
        invokeCreate(vkCreateRenderPass2KHR,CreatePass2Khr,pass2,allocator,passObject,0x9002,VK_ERROR_OUT_OF_HOST_MEMORY);
        invokeCreate(vkCreateFramebuffer,CreateFramebuffer,framebuffer,allocator,framebufferObject,0x9003,VK_ERROR_OUT_OF_HOST_MEMORY);
        require(rp::stats(state.renderPass).metadata.registrations==baseRegistrations,"all failed creates register zero objects");
        invokeCreate(vkCreateImageView,CreateView,view,allocator,viewObject,0x9001,VK_SUCCESS);
        if(flavor==0)invokeCreate(vkCreateRenderPass,CreatePass,pass,allocator,passObject,0x9002,VK_SUCCESS);
        if(flavor==1)invokeCreate(vkCreateRenderPass2,CreatePass2,pass2,allocator,passObject,0x9002,VK_SUCCESS);
        if(flavor==2)invokeCreate(vkCreateRenderPass2KHR,CreatePass2Khr,pass2,allocator,passObject,0x9002,VK_SUCCESS);
        invokeCreate(vkCreateFramebuffer,CreateFramebuffer,framebuffer,allocator,framebufferObject,0x9003,VK_SUCCESS);
        require(rp::stats(state.renderPass).metadata.registrations==baseRegistrations+3,"successful view/pass/framebuffer registered once each");
        VkCommandBufferAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool=ownPool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
        VkCommandBuffer command{};chosen=VK_SUCCESS;reset();require(vkAllocateCommandBuffers(ownDevice,&allocate,&command)==VK_SUCCESS,"metadata CB allocation");
        require(command==ownCommand&&driverCalls==1&&observedFirst==&allocate,"metadata CB allocated unchanged once");
        VkCommandBufferBeginInfo commandBegin{};commandBegin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VkRenderPassBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass=passObject;begin.framebuffer=framebufferObject;begin.renderArea={{0,0},{64,32}};
        VkSubpassBeginInfo subpassBegin{};subpassBegin.sType=VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;subpassBegin.contents=VK_SUBPASS_CONTENTS_INLINE;
        VkSubpassEndInfo subpassEnd{};subpassEnd.sType=VK_STRUCTURE_TYPE_SUBPASS_END_INFO;
        auto start=[&]{chosen=VK_SUCCESS;reset();require(vkBeginCommandBuffer(command,&commandBegin)==VK_SUCCESS,"metadata CB begin");
            require(driverCalls==1&&observedFirst==&commandBegin,"metadata CB begin unchanged once");};
        auto recordBegin=[&]{expectedFirst=&begin;expectedSecond=&subpassBegin;expectedContents=VK_SUBPASS_CONTENTS_INLINE;reset();
            if(flavor==0){vkCmdBeginRenderPass(command,&begin,expectedContents);once(BeginPass);}
            if(flavor==1){vkCmdBeginRenderPass2(command,&begin,&subpassBegin);once(BeginPass2);}
            if(flavor==2){vkCmdBeginRenderPass2KHR(command,&begin,&subpassBegin);once(BeginPass2Khr);}};
        auto recordEnd=[&]{expectedFirst=&subpassEnd;reset();
            if(flavor==0){vkCmdEndRenderPass(command);once(EndPass);}
            if(flavor==1){vkCmdEndRenderPass2(command,&subpassEnd);once(EndPass2);}
            if(flavor==2){vkCmdEndRenderPass2KHR(command,&subpassEnd);once(EndPass2Khr);}};
        auto endCommand=[&]{chosen=VK_SUCCESS;reset();require(vkEndCommandBuffer(command)==VK_SUCCESS&&driverCalls==1,"metadata CB end once");};
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        VkCommandBufferSubmitInfo commandSubmit{};commandSubmit.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;commandSubmit.commandBuffer=command;
        VkSubmitInfo2 submit2{};submit2.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO_2;submit2.commandBufferInfoCount=1;submit2.pCommandBufferInfos=&commandSubmit;
        auto send=[&](VkResult result){chosen=result;reset();
            const auto actual=flavor==0?vkQueueSubmit(ownQueue,1,&submit,VK_NULL_HANDLE):
                flavor==1?vkQueueSubmit2(ownQueue,1,&submit2,VK_NULL_HANDLE):vkQueueSubmit2KHR(ownQueue,1,&submit2,VK_NULL_HANDLE);
            require(actual==result&&driverCalls==1&&observedCount==1,"metadata submit literal result, no retry");
            require(observedFirst==(flavor==0?static_cast<const void*>(&submit):static_cast<const void*>(&submit2)),"metadata submit original info pointer");};
        start();recordBegin();recordEnd();endCommand();
        require(rp::stats(state.renderPass).emittedOperations==0,"recording alone publishes no submitted layout operation");
        send(VK_ERROR_OUT_OF_HOST_MEMORY);
        auto stats=rp::stats(state.renderPass);
        require(stats.failedDriverSubmissions==1&&stats.acceptedRecordings==0&&stats.emittedOperations==0&&stats.acceptedLayoutOperations==0&&layoutLineCount()==0,"failed submission publishes no layout operation");
        send(VK_SUCCESS);stats=rp::stats(state.renderPass);
        require(stats.acceptedRecordings==1&&stats.emittedOperations==3&&stats.acceptedLayoutOperations==3,"valid successful submit emits complete three-operation batch");
        require(layoutLineCount()==3,"accepted submit reaches metadata sink with exactly three conservative operations");
        const auto published=stats.emittedOperations;
        const auto refused=stats.refusedRecordings;
        start();recordBegin();endCommand();send(VK_SUCCESS);stats=rp::stats(state.renderPass);
        require(stats.acceptedRecordings==1&&stats.refusedRecordings==refused+1&&stats.emittedOperations==published&&stats.acceptedLayoutOperations==3,
            "missing EndRenderPass cannot publish a layout prefix");
        start();VkRenderingInfo rendering{};rendering.sType=VK_STRUCTURE_TYPE_RENDERING_INFO;
        expectedFirst=&rendering;reset();
        if(flavor==0){vkCmdBeginRendering(command,&rendering);once(BeginRendering);}
        else{vkCmdBeginRenderingKHR(command,&rendering);once(BeginRenderingKhr);}
        reset();if(flavor==0){vkCmdEndRendering(command);once(EndRendering);}
        else{vkCmdEndRenderingKHR(command);once(EndRenderingKhr);}
        endCommand();send(VK_SUCCESS);stats=rp::stats(state.renderPass);
        require(stats.acceptedRecordings==1&&stats.refusedRecordings==refused+2&&stats.emittedOperations==published&&stats.acceptedLayoutOperations==3,
            "unsupported dynamic rendering cannot become valid metadata");
        start();recordBegin();expectedFirst=&subpassBegin;expectedSecond=&subpassEnd;expectedContents=VK_SUBPASS_CONTENTS_INLINE;reset();
        if(flavor==0){vkCmdNextSubpass(command,expectedContents);once(NextPass);}
        if(flavor==1){vkCmdNextSubpass2(command,&subpassBegin,&subpassEnd);once(NextPass2);}
        if(flavor==2){vkCmdNextSubpass2KHR(command,&subpassBegin,&subpassEnd);once(NextPass2Khr);}
        recordEnd();endCommand();send(VK_SUCCESS);stats=rp::stats(state.renderPass);
        require(stats.acceptedRecordings==1&&stats.refusedRecordings==refused+3&&stats.emittedOperations==published&&stats.acceptedLayoutOperations==3,
            "unsupported extra subpass cannot publish a layout prefix");
        start();recordBegin();recordEnd();endCommand();
        expectedAllocator=&allocator;expectedObject=0x9001;reset();vkDestroyImageView(ownDevice,viewObject,&allocator);once(DestroyView);
        send(VK_SUCCESS);stats=rp::stats(state.renderPass);
        require(stats.acceptedRecordings==1&&stats.refusedRecordings==refused+4&&stats.emittedOperations==published&&stats.acceptedLayoutOperations==3&&layoutLineCount()==3,
            "destroyed attachment view invalidates recorded generation before submit");
        expectedObject=0x9003;reset();vkDestroyFramebuffer(ownDevice,framebufferObject,&allocator);once(DestroyFramebuffer);
        expectedObject=0x9002;reset();vkDestroyRenderPass(ownDevice,passObject,&allocator);once(DestroyPass);
        devices=nullptr;rp::destroy(state.renderPass);
    }
}
} // namespace rp_fixture

namespace wsi_fixture {
namespace rp=xess_vk::render_pass_observer;
namespace inv=xess_vk::image_inventory;
namespace wsi=xess_vk::wsi_image_inventory;
namespace sync=xess_vk::candidate_sync;
using rp_fixture::handle;

// Expectations live on the caller's stack. A downstream callback may enter a
// second production wrapper; each stub freezes its own expectations first.
struct CreateCall {
    const VkSwapchainCreateInfoKHR* info{};const VkAllocationCallbacks* allocator{};
    VkSwapchainKHR* output{};VkSwapchainKHR returned{};VkResult result{};
    unsigned calls{};std::function<void()> duringDriver;
};
struct ImagesCall {
    VkSwapchainKHR chain{};std::uint32_t* count{};VkImage* images{};
    std::uint32_t inputCount{},returnedCount{};VkResult result{};
    std::array<VkImage,4> returned{};unsigned calls{};std::function<void()> duringDriver;
};
struct DestroyCall {
    VkSwapchainKHR chain{};const VkAllocationCallbacks* allocator{};
    unsigned calls{};std::function<void()> duringDriver;
};
struct ImageCall {
    const VkImageCreateInfo* info{};const VkAllocationCallbacks* allocator{};
    VkImage* output{};VkImage returned{};unsigned calls{};
};
CreateCall* createCall{};ImagesCall* imagesCall{};DestroyCall* destroyCall{};ImageCall* imageCall{};
void unlocked(){
    const auto acquired=TryAcquireSRWLockExclusive(&stateLock);
    if(acquired)ReleaseSRWLockExclusive(&stateLock);
    require(acquired!=0,"WSI downstream call must not run under stateLock");
}
VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice device,const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator,VkSwapchainKHR* output){
    require(createCall!=nullptr,"WSI create has an expectation");auto& expected=*createCall;++expected.calls;
    require(device==ownDevice&&info==expected.info&&allocator==expected.allocator&&output==expected.output,
        "WSI create forwards exact original pointers and allocator");unlocked();
    if(expected.duringDriver)expected.duringDriver();
    *output=expected.returned;return expected.result;
}
VKAPI_ATTR VkResult VKAPI_CALL images(VkDevice device,VkSwapchainKHR chain,std::uint32_t* count,VkImage* output){
    require(imagesCall!=nullptr,"WSI enumeration has an expectation");auto& expected=*imagesCall;++expected.calls;
    require(device==ownDevice&&chain==expected.chain&&count==expected.count&&output==expected.images,
        "WSI enumeration forwards exact original pointers and handle");
    require(*count==expected.inputCount,"WSI enumeration preserves caller input capacity");unlocked();
    if(expected.duringDriver)expected.duringDriver();
    // Deliberately allow an adversarial returnedCount above capacity without
    // writing outside the caller's array, to exercise the wrapper copy bound.
    if(output)for(std::uint32_t i=0;i<expected.returnedCount&&i<expected.inputCount&&i<expected.returned.size();++i)
        output[i]=expected.returned[i];
    *count=expected.returnedCount;return expected.result;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice device,VkSwapchainKHR chain,const VkAllocationCallbacks* allocator){
    require(destroyCall!=nullptr,"WSI destroy has an expectation");auto& expected=*destroyCall;++expected.calls;
    require(device==ownDevice&&chain==expected.chain&&allocator==expected.allocator,
        "WSI destroy forwards exact handle and allocator");unlocked();
    if(expected.duringDriver)expected.duringDriver();
}
VKAPI_ATTR VkResult VKAPI_CALL regularImage(VkDevice device,const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkImage* output){
    require(imageCall!=nullptr,"ordinary image create has an expectation");auto& expected=*imageCall;++expected.calls;
    require(device==ownDevice&&info==expected.info&&allocator==expected.allocator&&output==expected.output,
        "ordinary image create forwards exact pointers");unlocked();*output=expected.returned;return VK_SUCCESS;
}

struct Fixture {
    // The common inventory exceeds the default Windows thread stack size.
    std::unique_ptr<wsi::CommonInventory> common=std::make_unique<wsi::CommonInventory>();
    std::unique_ptr<wsi::Inventory> chains=std::make_unique<wsi::Inventory>(*common);
    DeviceState state{};VkAllocationCallbacks allocator{};
    Fixture(){
        rp_fixture::initialize(state);state.createSwapchain=create;state.swapchainImages=images;
        state.destroySwapchain=destroy;state.createImage=regularImage;
        state.imageInventory=common.get();state.wsiInventory=chains.get();
        state.renderPass=rp::create(ownDevice,nullptr);require(state.renderPass!=nullptr,"WSI real RP adapter allocated");
        state.candidateSync=sync::create(ownDevice,true,nullptr);require(state.candidateSync!=nullptr,"WSI real candidate adapter allocated");
        allocator.pUserData=this;devices=&state;
    }
    ~Fixture(){devices=nullptr;rp::destroy(state.renderPass);sync::destroy(state.candidateSync);}
    VkSwapchainCreateInfoKHR info(VkSwapchainKHR old=VK_NULL_HANDLE) const {
        VkSwapchainCreateInfoKHR value{};value.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        value.surface=handle<VkSurfaceKHR>(0xb000);value.minImageCount=2;value.imageFormat=VK_FORMAT_R8G8B8A8_UNORM;
        value.imageExtent={64,32};value.imageArrayLayers=1;value.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        value.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;value.presentMode=VK_PRESENT_MODE_FIFO_KHR;
        value.oldSwapchain=old;return value;
    }
    VkSwapchainKHR make(std::uintptr_t returned,VkResult result=VK_SUCCESS,VkSwapchainKHR old=VK_NULL_HANDLE,
        std::function<void()> during={},const VkSwapchainCreateInfoKHR* overrideInfo=nullptr){
        auto description=info(old);const auto* original=overrideInfo?overrideInfo:&description;const auto before=*original;
        VkSwapchainKHR output=handle<VkSwapchainKHR>(0xdead);
        CreateCall call{original,&allocator,&output,handle<VkSwapchainKHR>(returned),result,0,std::move(during)};
        auto* saved=createCall;createCall=&call;const auto actual=vkCreateSwapchainKHR(ownDevice,original,&allocator,&output);createCall=saved;
        require(actual==result&&output==call.returned,"WSI create preserves literal result and output even on failure");
        require(call.calls==1,"WSI create calls original driver exactly once");
        require(std::memcmp(original,&before,sizeof(before))==0,"WSI create does not rewrite any input field");return output;
    }
    void enumerate(VkSwapchainKHR chain,std::uint32_t capacity,std::uint32_t returnedCount,VkResult result,
        std::array<VkImage,4> returned,bool array=true,std::function<void()> during={}){
        std::array<VkImage,4> output{};output.fill(handle<VkImage>(0xf00d));std::uint32_t count=capacity;
        ImagesCall call{chain,&count,array?output.data():nullptr,capacity,returnedCount,result,returned,0,std::move(during)};
        auto* saved=imagesCall;imagesCall=&call;const auto actual=vkGetSwapchainImagesKHR(ownDevice,chain,&count,call.images);imagesCall=saved;
        require(actual==result&&count==returnedCount,"WSI enumeration preserves literal result and returned count");
        require(call.calls==1,"WSI enumeration calls original driver exactly once");
        if(array)for(std::size_t i=0;i<output.size();++i)
            require(output[i]==(i<returnedCount&&i<capacity?returned[i]:handle<VkImage>(0xf00d)),
                "WSI enumeration preserves driver output and untouched array tail");
    }
    void drop(VkSwapchainKHR chain,std::function<void()> during={}){
        DestroyCall call{chain,&allocator,0,std::move(during)};auto* saved=destroyCall;destroyCall=&call;
        vkDestroySwapchainKHR(ownDevice,chain,&allocator);destroyCall=saved;
        require(call.calls==1,"WSI destroy calls original driver exactly once");
    }
    inv::Record ordinary(std::uintptr_t value){
        VkImageCreateInfo description{};description.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        description.imageType=VK_IMAGE_TYPE_2D;description.format=VK_FORMAT_R8G8B8A8_UNORM;description.extent={64,32,1};
        description.mipLevels=description.arrayLayers=1;description.samples=VK_SAMPLE_COUNT_1_BIT;
        description.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImage output{};ImageCall call{&description,&allocator,&output,handle<VkImage>(value),0};imageCall=&call;
        const auto result=vkCreateImage(ownDevice,&description,&allocator,&output);imageCall=nullptr;
        require(result==VK_SUCCESS&&output==call.returned&&call.calls==1,"ordinary create original driver once");
        return live(value);
    }
    inv::Record live(std::uint64_t image){
        const auto snapshot=common->lookup(image);
        require(snapshot.status==inv::Status::LiveHandleObserved&&snapshot.record.image==image&&snapshot.record.generation,
            "common inventory contains an exact live image generation");return snapshot.record;
    }
    void absent(std::uint64_t image){require(common->lookup(image).status==inv::Status::NotObserved,"retired or unpublished image absent in common inventory");}
    void selected(const inv::Record& record){
        AcquireSRWLockExclusive(&stateLock);
        rp::select(state.renderPass,rp::Role::RealColor,record);
        const auto notice=sync::select(state.candidateSync,sync::Role::RealColor,record);
        ReleaseSRWLockExclusive(&stateLock);
        // An unchanged binding returns an empty notice; inspect actual retained
        // selection instead of mistaking that idempotent return for rejection.
        require(sync::evidence(state.candidateSync,{record.image,record.generation}).selected,
            "WSI common identity selected in candidate adapter");
        if(notice.emit)require(notice.accepted,"new WSI candidate selection notice accepted");
        require(rp::stats(state.renderPass).projection.selectionRejections==0,"WSI common identity accepted by RP adapter");
    }
    void sameGeneration(const inv::Record& record){
        selected(record);rp::queue(state.renderPass,ownQueue,3);sync::queue(state.candidateSync,ownQueue,3);
        sync::pool(state.candidateSync,ownPool,3);
        VkCommandBufferAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool=ownPool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
        VkCommandBuffer command{};chosen=VK_SUCCESS;resetObserved();
        require(vkAllocateCommandBuffers(ownDevice,&ai,&command)==VK_SUCCESS&&driverCalls==1,"WSI fixture command allocated via production wrapper");
        VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;resetObserved();
        require(vkBeginCommandBuffer(command,&bi)==VK_SUCCESS&&driverCalls==1,"WSI fixture recording started via production wrapper");
        resetObserved();require(vkEndCommandBuffer(command)==VK_SUCCESS&&driverCalls==1,"WSI fixture recording ended via production wrapper");
        VkSubmitInfo si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;si.commandBufferCount=1;si.pCommandBuffers=&command;
        // Read the actual adapter pre-driver ticket. Consume it with literal
        // failure; this does not pretend to submit work or establish GPU state.
        const auto ticket=rp::beginSubmit(state.renderPass,ownQueue,1,&si);
        require(ticket.commandCount==1&&ticket.projections[0].serial!=0,"RP adapter captures one WSI identity ticket");
        const auto selection=ticket.projections[0].frozenSelections[static_cast<unsigned>(rp::Role::RealColor)];
        require(selection.enabled&&selection.image.handle==record.image&&selection.image.generation==record.generation,
            "RP frozen image generation exactly equals shared inventory generation");
        require(selection.sourceToken==0&&selection.passToken==0,"WSI metadata invents no frame or source association");
        rp::finishSubmit(state.renderPass,ticket,VK_NULL_HANDLE,VK_ERROR_OUT_OF_HOST_MEMORY);
        require(rp::stats(state.renderPass).projection.layoutOperations==0,"failed ticket emits no candidate layout operations");
        const auto evidence=sync::evidence(state.candidateSync,{record.image,record.generation});
        require(evidence.selected&&!evidence.copyPermitted&&!evidence.gpuComplete,"WSI identity does not grant copying or GPU completion");
    }
    void retiredBeforeDriver(const inv::Record& record,std::uint64_t expectedRetired){
        absent(record.image);
        require(rp::stats(state.renderPass).projection.swapchainImagesRetired==expectedRetired,"RP identity retired before original driver call");
        require(!sync::evidence(state.candidateSync,{record.image,record.generation}).selected,
            "candidate identity retired before original driver call");
    }
};

void lifecycle(){
    Fixture fixture;
    const auto ordinaryBefore=fixture.ordinary(0xc000);
    const auto chain=fixture.make(0xc100);
    const std::array<VkImage,4> handles{handle<VkImage>(0xc201),handle<VkImage>(0xc202)};
    fixture.enumerate(chain,77,2,VK_SUCCESS,{},false);
    require(fixture.common->lookup(0).active==1,"count-only enumeration publishes no images");
    fixture.enumerate(chain,1,1,VK_INCOMPLETE,handles);fixture.absent(0xc201);
    fixture.enumerate(chain,4,2,VK_ERROR_OUT_OF_HOST_MEMORY,handles);fixture.absent(0xc201);
    fixture.enumerate(chain,4,2,VK_SUCCESS,handles);
    const auto first=fixture.live(0xc201),second=fixture.live(0xc202);
    require(first.generation==ordinaryBefore.generation+1&&second.generation==first.generation+1,
        "ordinary and WSI images share one globally ordered generation namespace");
    const auto registered=rp::stats(fixture.state.renderPass).metadata.registrations;
    fixture.enumerate(chain,2,2,VK_SUCCESS,handles);
    require(fixture.live(first.image).generation==first.generation&&fixture.live(second.image).generation==second.generation,
        "repeat enumeration preserves original generations");
    require(rp::stats(fixture.state.renderPass).metadata.registrations==registered&&
        rp::stats(fixture.state.renderPass).projection.swapchainImagesRegistered==2,
        "repeat enumeration performs no common created or RP duplicate registration");
    const auto ordinaryAfter=fixture.ordinary(0xc003);
    require(ordinaryAfter.generation==second.generation+1,"repeat enumeration consumes no hidden global generation");
    fixture.sameGeneration(first);
    fixture.make(0xc101,VK_ERROR_OUT_OF_HOST_MEMORY,chain,[&]{fixture.retiredBeforeDriver(first,2);fixture.absent(second.image);});
    fixture.absent(first.image);fixture.absent(second.image);
    require(fixture.live(ordinaryBefore.image).generation==ordinaryBefore.generation&&fixture.live(ordinaryAfter.image).generation==ordinaryAfter.generation,
        "failed swapchain replacement preserves unrelated ordinary images");
    // Even a downstream success after retirement cannot revive this old list.
    fixture.enumerate(chain,2,2,VK_SUCCESS,handles);fixture.absent(first.image);
    fixture.enumerate(handle<VkSwapchainKHR>(0xc101),2,2,VK_SUCCESS,handles);fixture.absent(first.image);
    fixture.drop(chain);

    const auto next=fixture.make(0xc110);fixture.enumerate(next,2,2,VK_SUCCESS,handles);
    const auto replacement=fixture.live(first.image);require(replacement.generation>ordinaryAfter.generation,"reused image receives a new common generation");
    fixture.selected(replacement);
    fixture.drop(next,[&]{fixture.retiredBeforeDriver(replacement,4);fixture.absent(second.image);});
    const auto reusedOrdinary=fixture.ordinary(first.image);
    fixture.selected(reusedOrdinary);fixture.drop(next);
    require(fixture.live(first.image).generation==reusedOrdinary.generation&&
        sync::evidence(fixture.state.candidateSync,{first.image,reusedOrdinary.generation}).selected,
        "repeated old-chain destroy cannot remove an ordinary image reusing an old WSI handle");
    require(rp::stats(fixture.state.renderPass).projection.swapchainImagesRetired==4,
        "old-chain destroy cannot misclassify ordinary image as WSI retirement");
}
void aba(){
    Fixture fixture;const auto chain=fixture.make(0xd100);
    const std::array<VkImage,4> oldImages{handle<VkImage>(0xd201),handle<VkImage>(0xd202)};
    const std::array<VkImage,4> newImages{handle<VkImage>(0xd301),handle<VkImage>(0xd302)};
    fixture.enumerate(chain,2,2,VK_SUCCESS,oldImages);const auto old=fixture.live(0xd201);fixture.selected(old);
    std::uint64_t newGeneration{};
    fixture.enumerate(chain,2,2,VK_SUCCESS,oldImages,true,[&]{
        fixture.drop(chain,[&]{fixture.retiredBeforeDriver(old,2);});
        const auto reused=fixture.make(0xd100);require(reused==chain,"fixture reuses exact swapchain handle during original enumeration");
        fixture.enumerate(reused,2,2,VK_SUCCESS,newImages);const auto replacement=fixture.live(0xd301);
        newGeneration=replacement.generation;fixture.selected(replacement);
    });
    fixture.absent(0xd201);fixture.absent(0xd202);
    require(fixture.live(0xd301).generation==newGeneration&&newGeneration>old.generation,
        "stale enumeration completion cannot replace new same-handle chain generation");
    require(rp::stats(fixture.state.renderPass).projection.swapchainImagesRegistered==4&&
        rp::stats(fixture.state.renderPass).projection.swapchainImagesRetired==2,
        "ABA completion neither republishes old images nor retires new images");
    require(sync::evidence(fixture.state.candidateSync,{0xd301,newGeneration}).selected,
        "ABA completion preserves new candidate selection");
    fixture.sameGeneration(fixture.live(0xd301));fixture.drop(chain);
}
void refusalBounds(){
    {
        Fixture fixture;const auto chain=fixture.make(0xe100);
        const std::array<VkImage,4> handles{handle<VkImage>(0xe201),handle<VkImage>(0xe202)};
        fixture.enumerate(chain,1,2,VK_SUCCESS,handles);fixture.absent(0xe201);fixture.absent(0xe202);
        require(rp::stats(fixture.state.renderPass).projection.swapchainImagesRegistered==0,
            "driver returned count above original capacity cannot publish or overread image array");
    }
    {
        Fixture fixture;const auto ordinary=fixture.ordinary(0xe300);const auto chain=fixture.make(0xe101);
        fixture.enumerate(chain,2,2,VK_SUCCESS,{handle<VkImage>(ordinary.image),handle<VkImage>(0xe302)});
        require(fixture.live(ordinary.image).generation==ordinary.generation,"conflicting WSI list cannot overwrite ordinary image");
        fixture.absent(0xe302);fixture.drop(chain);
        require(fixture.live(ordinary.image).generation==ordinary.generation,"rejected WSI list owns no ordinary image to destroy");
        require(rp::stats(fixture.state.renderPass).projection.swapchainImagesRegistered==0,"conflicting list never partially registers RP images");
    }
    for(unsigned mode=0;mode<2;++mode){
        Fixture fixture;const auto chain=fixture.make(0xe110+mode);
        const std::array<VkImage,4> handles{handle<VkImage>(0xe401),handle<VkImage>(0xe402)};
        fixture.enumerate(chain,2,2,VK_SUCCESS,handles);const auto old=fixture.live(0xe401);fixture.selected(old);
        auto unsupported=fixture.info(chain);VkBaseInStructure extension{};extension.sType=VK_STRUCTURE_TYPE_MAX_ENUM;
        if(mode==0)unsupported.pNext=&extension;else unsupported.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        const auto result=fixture.make(0xe120+mode,VK_SUCCESS,chain,[&]{fixture.retiredBeforeDriver(old,2);},&unsupported);
        fixture.enumerate(result,2,2,VK_SUCCESS,handles);fixture.absent(old.image);
        require(rp::stats(fixture.state.renderPass).projection.swapchainImagesRegistered==2,
            "unsupported create is forwarded but cannot acquire WSI identity after retiring old chain");
    }
}
void run(){lifecycle();aba();refusalBounds();}
} // namespace wsi_fixture

namespace upscale_consumer_fixture {
// Capture the actual JsonLine/writeLogLine path in a self-owned temporary file.
// The handle and byte counter are restored even when an assertion throws.
struct CapturedLog {
    HANDLE file=INVALID_HANDLE_VALUE,previous=INVALID_HANDLE_VALUE;
    ULONGLONG previousBytes{};
    CapturedLog(){
        require(InitOnceExecuteOnce(&logOnce,initializeLog,nullptr,nullptr)!=0,"logger initialized without game log path");
        wchar_t directory[MAX_PATH]{},path[MAX_PATH]{};
        const auto size=GetTempPathW(MAX_PATH,directory);
        require(size&&size<MAX_PATH,"own-process temporary directory available");
        require(GetTempFileNameW(directory,L"xef",0,path)!=0,"own-process temporary log created");
        file=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(file==INVALID_HANDLE_VALUE){DeleteFileW(path);require(false,"own-process temporary log opened");}
        previous=logFile;previousBytes=logBytes;logFile=file;logBytes=0;
    }
    ~CapturedLog(){logFile=previous;logBytes=previousBytes;if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    void clear(){
        LARGE_INTEGER zero{};
        require(SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)&&SetEndOfFile(file),"temporary log reset");logBytes=0;
    }
    std::vector<std::string> rows(){
        LARGE_INTEGER length{},zero{};
        require(FlushFileBuffers(file)&&GetFileSizeEx(file,&length),"temporary log flush and size");
        require(length.QuadPart>=0&&length.QuadPart<=65536,"consumer output stays within fixture read bound");
        require(SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)!=0,"temporary log rewind");
        std::string contents(static_cast<std::size_t>(length.QuadPart),'\0');DWORD copied{};
        require(ReadFile(file,contents.data(),static_cast<DWORD>(contents.size()),&copied,nullptr)&&copied==contents.size(),
            "exact temporary log contents read");
        std::vector<std::string> output;std::size_t begin=0;
        while(begin<contents.size()){
            const auto end=contents.find('\n',begin);require(end!=std::string::npos,"actual JSON output contains complete lines");
            output.push_back(contents.substr(begin,end-begin));begin=end+1;
        }
        return output;
    }
};
struct SessionRetirement {
    bool previous=inventorySessionRetired;
    ~SessionRetirement(){inventorySessionRetired=previous;}
};
bool contains(const std::string& row,const std::string& field){return row.find(field)!=std::string::npos;}
void checkRows(const std::vector<std::string>& rows,const std::array<xess_vk::image_inventory::Record,8>& records,
    std::uint64_t callback,const char* reason,bool retired=false){
    require(rows.size()==8,"periodic or initial metadata produces exactly eight actual log lines");
    for(std::size_t i=0;i<rows.size();++i){
        const auto& row=rows[i];char image[32]{};
        std::snprintf(image,sizeof(image),"0x%llx",static_cast<unsigned long long>(records[i].image));
        require(contains(row,"\"event\":\"upscale_image_metadata_match\"")&&
            contains(row,"\"source\":\"upscaler_prepare_eb49d0\""),"actual event and callback source retained");
        require(contains(row,"\"callback_call\":"+std::to_string(callback)+",")&&
            contains(row,std::string("\"sample_reason\":\"")+reason+"\""),"literal callback ordinal and sampling reason retained");
        require(contains(row,std::string("\"property\":\"")+xess_vk::upscale_probe::propertyName(
            static_cast<xess_vk::upscale_probe::Property>(i+1))+"\"")&&
            contains(row,std::string("\"image_candidate\":\"")+image+"\""),"each property has its own current numeric candidate");
        if(retired){
            require(contains(row,"\"match_status\":\"device_session_retired\"")&&
                contains(row,"\"numeric_live_handle_match\":false")&&!contains(row,"\"image_generation\":"),
                "retired device never reports live identity or a stale generation");
        }else{
            require(contains(row,"\"match_status\":\"live_handle_observed\"")&&
                contains(row,"\"numeric_live_handle_match\":true")&&
                contains(row,"\"image_generation\":"+std::to_string(records[i].generation)+",")&&
                contains(row,"\"candidate_extent_matches\":true"),"actual common inventory generation and extent match recorded");
        }
        for(const auto* flag:{"resource_lifetime_pinned","snapshot_atomic","undilated_verified","motion_semantics_verified",
                             "same_frame_verified","fi_roles_updated","copy_permitted","xeFG_integrated"})
            require(contains(row,std::string("\"")+flag+"\":false"),"consumer output preserves unverified evidence boundaries");
    }
}
void run(){
    // Referencing the other exposed consumer avoids unused-function warnings;
    // no FI callback is invoked by this fixture.
    (void)&imageMetadataLog;
    wsi_fixture::Fixture fixture;SessionRetirement session;inventorySessionRetired=false;
    std::array<xess_vk::image_inventory::Record,8> records{};
    xess_vk::upscale_probe::Result result;
    result.status=xess_vk::upscale_probe::Status::SnapshotParsed;
    result.snapshotCaptured=result.allPropertiesPresent=result.contextTableUnchanged=result.graphIndexedEntries=true;
    result.descriptorsUnchanged=result.graphEntriesUnchanged=result.bindingMetadataUnchanged=true;
    result.graphTextureIdsRead=result.nativeCandidatesRead=8;
    for(std::size_t i=0;i<records.size();++i){
        records[i]=fixture.ordinary(0xf001+i);
        auto& binding=result.bindings[i];binding.property=static_cast<xess_vk::upscale_probe::Property>(i+1);
        binding.native.status=xess_vk::fsr_probe::Status::NativeCandidateReadUnverified;
        binding.native.imageCandidate=records[i].image;binding.native.widthCandidate=64;binding.native.heightCandidate=32;
    }
    CapturedLog capture;
    for(const std::uint64_t callback:{0,120,1199,1201}){
        upscaleImageMetadataLog(bits(ownDevice),callback,result,false);
        require(capture.rows().empty(),"zero or non-periodic callback with no detail emits no metadata lines");
    }
    upscaleImageMetadataLog(bits(ownDevice),1200,result,false);
    checkRows(capture.rows(),records,1200,"periodic_refresh");capture.clear();
    upscaleImageMetadataLog(bits(ownDevice),3,result,true);
    checkRows(capture.rows(),records,3,"initial_detail");capture.clear();
    const auto previousGeneration=records[0].generation;
    records[0]=fixture.ordinary(0xf101);result.bindings[0].native.imageCandidate=records[0].image;
    require(records[0].generation>previousGeneration,"changed binding has a later common inventory generation");
    capture.clear(); // The ordinary-image production wrapper has its own event.
    upscaleImageMetadataLog(bits(ownDevice),2400,result,false);
    checkRows(capture.rows(),records,2400,"periodic_refresh");capture.clear();
    const auto calls=fixture.state.upscaleMetadataCalls,lookups=fixture.state.upscaleMetadataLookups;
    inventorySessionRetired=true;
    upscaleImageMetadataLog(bits(ownDevice),3600,result,false);
    checkRows(capture.rows(),records,3600,"periodic_refresh",true);capture.clear();
    require(fixture.state.upscaleMetadataCalls==calls&&fixture.state.upscaleMetadataLookups==lookups,
        "retired session performs no live inventory accounting");inventorySessionRetired=false;
    result.bindingMetadataUnchanged=false;
    upscaleImageMetadataLog(bits(ownDevice),4800,result,false);
    require(capture.rows().empty(),"unstable typed result cannot emit even at periodic boundary");
    for(const auto& record:records)
        require(!xess_vk::candidate_sync::evidence(fixture.state.candidateSync,{record.image,record.generation}).selected,
            "upscaler metadata consumer never selects an FI candidate role");
    require(xess_vk::render_pass_observer::stats(fixture.state.renderPass).projection.selectionChanges==0,
        "upscaler metadata consumer never changes RP candidate binding epoch");
}
} // namespace upscale_consumer_fixture
}
int main(){try{
    SetEnvironmentVariableW(L"XESS_OBSERVER_LOG",nullptr);
    DeviceState state{};state.device=ownDevice;state.key=&dispatchMarker;state.gdpa=gdpaStub;
    state.createCommandPool=createPoolStub;state.allocateCommandBuffers=allocateStub;state.beginCommandBuffer=beginStub;
    state.endCommandBuffer=endStub;state.cmdPipelineBarrier=barrierStub;state.cmdPipelineBarrier2=barrier2Stub;
    state.cmdPipelineBarrier2Khr=barrier2Stub;state.submit=submitStub;state.submit2=submit2Stub;state.submit2Khr=submit2Stub;
    state.cmdWaitEvents2=waitStub;state.cmdWaitEvents2Khr=waitStub;state.cmdBeginRenderPass2=renderStub;
    state.cmdBeginRenderPass2Khr=renderStub;state.cmdExecuteCommands=executeStub;
    state.candidateSync=xess_vk::candidate_sync::create(ownDevice,true,nullptr);require(state.candidateSync,"helper constructed");devices=&state;
    xess_vk::candidate_sync::queue(state.candidateSync,ownQueue,3);
    xess_vk::image_inventory::Record record{0x2000,1,{83,1920,1080,1,1,1,1,159,0,0,0,0}};
    require(xess_vk::candidate_sync::select(state.candidateSync,xess_vk::candidate_sync::Role::Motion,record).accepted,"fixture selected");
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=3;VkCommandPool p{};
    resetObserved();require(vkCreateCommandPool(ownDevice,&pci,nullptr,&p)==VK_SUCCESS&&p==ownPool,"pool output/result forwarded");
    require(driverCalls==1&&observedFirst==&pci,"pool one downstream call");
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=p;ai.commandBufferCount=1;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer b{};resetObserved();require(vkAllocateCommandBuffers(ownDevice,&ai,&b)==VK_SUCCESS&&b==ownCommand,"allocate output preserved");
    require(driverCalls==1&&observedFirst==&ai,"allocate one downstream call");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};resetObserved();require(vkBeginCommandBuffer(b,&bi)==VK_SUCCESS,"begin result");
    require(driverCalls==1&&observedFirst==&bi,"begin exact pointer once");
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib.image=reinterpret_cast<VkImage>(0x2000);
    ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    resetObserved();vkCmdPipelineBarrier(b,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,1,&bb,1,&ib);
    require(driverCalls==1&&observedFirst==&ib&&observedSecond==&mb&&observedCount==1&&observedFlags==0,"barrier arrays unchanged once");
    VkImageMemoryBarrier2 ib2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};ib2.image=ib.image;ib2.oldLayout=ib2.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    ib2.srcQueueFamilyIndex=ib2.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ib2.subresourceRange=ib.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};di.imageMemoryBarrierCount=1;di.pImageMemoryBarriers=&ib2;
    for(auto fn:{vkCmdPipelineBarrier2,vkCmdPipelineBarrier2KHR}){resetObserved();fn(b,&di);require(driverCalls==1&&observedFirst==&di,"core/KHR barrier2 exact pointer once");}
    VkEvent event=reinterpret_cast<VkEvent>(0x5000);
    for(auto fn:{vkCmdWaitEvents2,vkCmdWaitEvents2KHR}){resetObserved();fn(b,1,&event,&di);require(driverCalls==1&&observedFirst==&event&&observedSecond==&di,"event core/KHR forwarding");}
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};VkSubpassBeginInfo sp{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};
    for(auto fn:{vkCmdBeginRenderPass2,vkCmdBeginRenderPass2KHR}){resetObserved();fn(b,&rp,&sp);require(driverCalls==1&&observedFirst==&rp&&observedSecond==&sp,"render-pass core/KHR forwarding");}
    resetObserved();vkCmdExecuteCommands(b,1,&b);require(driverCalls==1&&observedFirst==&b&&observedCount==1,"secondary opaque preserves downstream");
    resetObserved();require(vkEndCommandBuffer(b)==VK_SUCCESS&&driverCalls==1,"end forwarded once");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&b;
    resetObserved();require(vkQueueSubmit(ownQueue,1,&si,VK_NULL_HANDLE)==VK_SUCCESS,"submit result unchanged");
    require(driverCalls==1&&observedFirst==&si&&observedCount==1,"submit exact pointer once");
    auto evidence=xess_vk::candidate_sync::evidence(state.candidateSync,{0x2000,1});require(evidence.barrierSubmitted&&evidence.commandGeneration==1,"layer submit reaches metadata tracker");
    require(!evidence.copyPermitted&&!evidence.gpuComplete&&!evidence.submittedLayoutKnown,"no claim from stub submit");
    chosen=VK_ERROR_OUT_OF_HOST_MEMORY;VkCommandBufferSubmitInfo csi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};csi.commandBuffer=b;
    VkSubmitInfo2 si2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};si2.commandBufferInfoCount=1;si2.pCommandBufferInfos=&csi;
    for(auto fn:{vkQueueSubmit2,vkQueueSubmit2KHR}){resetObserved();require(fn(ownQueue,1,&si2,VK_NULL_HANDLE)==chosen,"submit2 failure forwarded");
        require(driverCalls==1&&observedFirst==&si2&&observedCount==1,"submit2 core/KHR no retry");}
    require(xess_vk::candidate_sync::stats(state.candidateSync).failedSubmissions==2,"failed submit accounting only");
    require(vkGetDeviceProcAddr(ownDevice,"vkCmdPipelineBarrier2KHR")==reinterpret_cast<PFN_vkVoidFunction>(vkCmdPipelineBarrier2KHR),"available alias hook exposed");
    require(vkGetDeviceProcAddr(ownDevice,"vkUnknownFixtureEXT")==reinterpret_cast<PFN_vkVoidFunction>(unknownStub),"unknown extension forwarding unchanged");
    require(vkGetDeviceProcAddr(ownDevice,"vkUnsupportedFixtureEXT")==nullptr,"missing downstream function stays missing");
    devices=nullptr;xess_vk::candidate_sync::destroy(state.candidateSync);
    rp_fixture::forwarding();
    rp_fixture::metadataPublication();
    wsi_fixture::run();
    upscale_consumer_fixture::run();
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"realVulkanCalls\":0,\"gameModulesLoaded\":false,\"copyPermissionGranted\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
