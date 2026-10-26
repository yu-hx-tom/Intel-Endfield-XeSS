#include "render_pass_observer.hpp"
#include "render_pass_diagnostics.hpp"
#include "render_tail_diagnostics.hpp"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <thread>

namespace {std::atomic<bool> countAllocations{};std::atomic<std::size_t> allocations{};}
void* operator new(std::size_t size){if(countAllocations)++allocations;if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace {
namespace ro=xess_vk::render_pass_observer;namespace rp=xess_vk::render_pass_metadata;
unsigned checks{},cases{};std::size_t lineCount{};
std::array<std::array<char,8192>,2048> lines{};
ro::Context* readOnlyReentry{};
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class T>T handle(std::uint64_t value){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(value);else return static_cast<T>(value);}
void sink(const char* line){
    if(readOnlyReentry)(void)ro::stats(readOnlyReentry); // Deadlocks if callback is under adapter mutex.
    if(lineCount<lines.size()){std::strncpy(lines[lineCount].data(),line,lines[0].size()-1);lines[lineCount][lines[0].size()-1]=0;++lineCount;}
    SetLastError(0xdead);
}
bool contains(const char* needle){for(std::size_t i=0;i<lineCount;++i)if(std::strstr(lines[i].data(),needle))return true;return false;}
unsigned matching(const char* needle){unsigned n=0;for(std::size_t i=0;i<lineCount;++i)if(std::strstr(lines[i].data(),needle))++n;return n;}
bool sameLine(const char* first,const char* second){for(std::size_t i=0;i<lineCount;++i)if(std::strstr(lines[i].data(),first)&&std::strstr(lines[i].data(),second))return true;return false;}
xess_vk::image_inventory::Record inventory(const VkImageCreateInfo& info,std::uint64_t image,std::uint64_t generation){
    return {image,generation,{std::uint32_t(info.format),info.extent.width,info.extent.height,info.extent.depth,info.mipLevels,info.arrayLayers,
        std::uint32_t(info.samples),info.usage,info.flags,std::uint32_t(info.tiling),std::uint32_t(info.sharingMode),std::uint32_t(info.initialLayout)}};
}
struct Fixture {
    ro::Context* c{};
    VkDevice device=handle<VkDevice>(1);VkImage image=handle<VkImage>(10);VkImageView view=handle<VkImageView>(20);
    VkRenderPass pass=handle<VkRenderPass>(30);VkFramebuffer fb=handle<VkFramebuffer>(40);
    VkCommandBuffer command=handle<VkCommandBuffer>(50);VkCommandPool pool=handle<VkCommandPool>(60);VkQueue queue=handle<VkQueue>(70);
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};xess_vk::image_inventory::Record record;
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkAttachmentDescription attachment{};VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkCommandBufferBeginInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkRenderPassBeginInfo beginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    explicit Fixture(bool setup=true,bool logging=true){
        lineCount=0;c=ro::create(device,logging?sink:nullptr);require(c!=nullptr,"adapter construction");
        imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.format=VK_FORMAT_R8G8B8A8_UNORM;imageInfo.extent={64,32,1};
        imageInfo.mipLevels=imageInfo.arrayLayers=1;imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;imageInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;record=inventory(imageInfo,10,1);
        viewInfo.image=image;viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;viewInfo.format=imageInfo.format;
        viewInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        attachment.format=imageInfo.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference;
        passInfo.attachmentCount=1;passInfo.pAttachments=&attachment;passInfo.subpassCount=1;passInfo.pSubpasses=&subpass;
        fbInfo.renderPass=pass;fbInfo.attachmentCount=1;fbInfo.pAttachments=&view;fbInfo.width=64;fbInfo.height=32;fbInfo.layers=1;
        allocateInfo.commandPool=pool;allocateInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocateInfo.commandBufferCount=1;
        beginInfo.renderPass=pass;beginInfo.framebuffer=fb;beginInfo.renderArea={{0,0},{64,32}};
        submitInfo.commandBufferCount=1;submitInfo.pCommandBuffers=&command;
        if(setup)objects();
    }
    ~Fixture(){readOnlyReentry=nullptr;ro::destroy(c);}
    void objects(){
        ro::imageCreated(c,&imageInfo,image,VK_SUCCESS,&record);ro::viewCreated(c,&viewInfo,view,VK_SUCCESS);
        ro::renderPassCreated(c,&passInfo,pass,VK_SUCCESS);ro::framebufferCreated(c,&fbInfo,fb,VK_SUCCESS);
        ro::queue(c,queue,0);ro::allocate(c,&allocateInfo,&command,VK_SUCCESS);
    }
    void recordCommand(){
        ro::begin(c,command,&commandInfo,VK_SUCCESS);ro::explicitBarrier(c,command);
        ro::beginRenderPass(c,command,&beginInfo,VK_SUBPASS_CONTENTS_INLINE);
        ro::explicitBarrier(c,command);ro::endRenderPass(c,command);ro::end(c,command,VK_SUCCESS);
    }
    ro::SubmitTicket ticket(){return ro::beginSubmit(c,queue,1,&submitInfo);}
    void submit(VkResult result=VK_SUCCESS){auto t=ticket();ro::finishSubmit(c,t,VK_NULL_HANDLE,result);}
};
void happyAndSinkBoundary(){
    ++cases;Fixture f;require(lineCount==0,"object hooks produce no I/O under outer stateLock");f.recordCommand();
    readOnlyReentry=f.c;SetLastError(0xbeef);f.submit();require(GetLastError()==0xbeef,"submit sink preserves LastError");
    auto s=ro::stats(f.c);require(s.acceptedLayoutOperations==3&&s.acceptedRecordings==1&&s.emittedOperations==0,"unselected real RP counts parsed operations");
    ro::select(f.c,ro::Role::RealColor,f.record);f.submit();s=ro::stats(f.c);
    require(s.acceptedLayoutOperations==6&&s.emittedOperations==3,"selection controls logs not parsing");
    require(contains("initial_layout_assertion")&&contains("subpass_reference_transition")&&contains("final_layout_transition"),"three source kinds logged");
    require(contains("\"record_order\":2")&&contains("\"record_order\":4"),"explicit barrier calls preserve ordinal gaps");
    require(contains("\"image_generation_scope\":\"image_inventory\"")&&contains("\"rp_command_generation\":1"),"generation scopes labelled");
    require(contains("\"copy_permitted\":false")&&!contains("\"copy_permitted\":true"),"never grants copy");
    ro::summary(f.c);require(contains("\"accepted_layout_operations\":6"),"summary exposes unfiltered operation count");
}
void diagnosticPassSnapshot(){++cases;Fixture f;ro::begin(f.c,f.command,&f.commandInfo,VK_SUCCESS);
    ro::beginRenderPass(f.c,f.command,&f.beginInfo,VK_SUBPASS_CONTENTS_INLINE);
    const auto before=ro::recordingFacts(f.c,f.command);const auto snapshot=ro::diagnosticPassSnapshot(f.c,f.command,&f.beginInfo);const auto after=ro::recordingFacts(f.c,f.command);
    require(snapshot.count==1&&snapshot.pass.known&&snapshot.framebuffer.known&&snapshot.views[0].known&&snapshot.images[0].known,"read-only active-pass attachment snapshot uses existing object tables");
    require(snapshot.images[0].description.key.handle==f.record.image&&snapshot.images[0].description.key.generation==f.record.generation,"pass snapshot preserves common image generation");
    require(before.commonRecordOrder==after.commonRecordOrder&&before.rpRecordingGeneration==after.rpRecordingGeneration,"diagnostic pass read does not alter production recording order");
    const auto view=ro::diagnosticViewSnapshot(f.c,f.view);require(view.known&&view.image==snapshot.images[0].description.key,"dynamic-rendering view snapshot shares the same exact identity");
    require(!ro::diagnosticPassSnapshot(f.c,f.command,nullptr).count,"missing Begin input cannot inherit an active attachment");
}
void failureAndUnsupportedLocality(){
    ++cases;Fixture f(false);const auto initial=ro::stats(f.c).metadata.registrations;
    ro::imageCreated(f.c,nullptr,VK_NULL_HANDLE,VK_ERROR_OUT_OF_HOST_MEMORY,nullptr);
    ro::viewCreated(f.c,nullptr,VK_NULL_HANDLE,VK_ERROR_OUT_OF_HOST_MEMORY);
    ro::renderPassCreated(f.c,nullptr,VK_NULL_HANDLE,VK_ERROR_OUT_OF_HOST_MEMORY);
    ro::framebufferCreated(f.c,nullptr,VK_NULL_HANDLE,VK_ERROR_OUT_OF_HOST_MEMORY);
    require(ro::stats(f.c).metadata.registrations==initial,"failed driver creates never register");f.objects();
    auto badView=f.viewInfo;badView.image=handle<VkImage>(999);ro::viewCreated(f.c,&badView,handle<VkImageView>(21),VK_SUCCESS);
    auto badPass=f.passInfo;badPass.pNext=&f.beginInfo;ro::renderPassCreated(f.c,&badPass,handle<VkRenderPass>(31),VK_SUCCESS);
    f.recordCommand();f.submit(VK_ERROR_OUT_OF_HOST_MEMORY);auto s=ro::stats(f.c);
    require(!s.acceptedLayoutOperations&&!s.successfulDriverSubmissions&&s.failedDriverSubmissions==1,"failed submit cannot publish");
    f.submit();s=ro::stats(f.c);require(s.acceptedLayoutOperations==3&&!s.fatalIssues,"unknown and unsupported objects do not poison other records");
    ro::reset(f.c,f.command,VK_SUCCESS);ro::begin(f.c,f.command,&f.commandInfo,VK_SUCCESS);ro::dynamicRendering(f.c,f.command);ro::end(f.c,f.command,VK_SUCCESS);f.submit();
    require(ro::stats(f.c).acceptedLayoutOperations==3,"dynamic recording rejected");
    ro::reset(f.c,f.command,VK_SUCCESS);f.recordCommand();f.submit();require(ro::stats(f.c).acceptedLayoutOperations==6,"later supported recording recovers after local refusal");
}
void staleAndDestroyed(){
    ++cases;Fixture f;f.recordCommand();auto t=f.ticket();
    ro::reset(f.c,f.command,VK_SUCCESS);f.recordCommand();ro::finishSubmit(f.c,t,VK_NULL_HANDLE,VK_SUCCESS);
    require(ro::stats(f.c).acceptedLayoutOperations==0,"pre-driver captured generation cannot become later recording");
    f.submit();require(ro::stats(f.c).acceptedLayoutOperations==3,"new ticket captures later recording");
    auto dying=f.ticket();ro::imageDestroying(f.c,f.image);ro::finishSubmit(f.c,dying,VK_NULL_HANDLE,VK_SUCCESS);
    require(ro::stats(f.c).acceptedLayoutOperations==3,"destroyed image invalidates pending metadata snapshot");
    Fixture pool;pool.recordCommand();auto old=pool.ticket();ro::resetPool(pool.c,pool.pool,true,VK_SUCCESS);ro::finishSubmit(pool.c,old,VK_NULL_HANDLE,VK_SUCCESS);
    require(!ro::stats(pool.c).acceptedLayoutOperations,"destroyed pool cannot leave live command metadata");
}
void incompleteAndSecondary(){
    ++cases;for(unsigned variant=0;variant<5;++variant){
        Fixture f;ro::begin(f.c,f.command,&f.commandInfo,VK_SUCCESS);
        if(variant==0)ro::beginRenderPass(f.c,f.command,&f.beginInfo,VK_SUBPASS_CONTENTS_INLINE);
        if(variant==1)ro::nextSubpass(f.c,f.command);
        if(variant==2)ro::unsupported(f.c,f.command,rp::Issue::Secondary);
        if(variant==3)ro::unsupported(f.c,f.command,rp::Issue::UnsupportedExtension);
        if(variant==4)ro::endRenderPass(f.c,f.command);
        ro::end(f.c,f.command,VK_SUCCESS);f.submit();require(!ro::stats(f.c).acceptedLayoutOperations,"unsupported/incomplete recording cannot publish");
    }
}
void ticketsAndSubmit2(){
    ++cases;Fixture f;f.recordCommand();ro::select(f.c,ro::Role::RealColor,f.record);
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};command.commandBuffer=f.command;command.deviceMask=1;
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=1;info.pCommandBufferInfos=&command;
    auto t=ro::beginSubmit2(f.c,f.queue,1,&info);ro::finishSubmit(f.c,t,VK_NULL_HANDLE,VK_SUCCESS);
    require(ro::stats(f.c).acceptedLayoutOperations==3,"submit2 supported provenance");
    command.deviceMask=2;t=ro::beginSubmit2(f.c,f.queue,1,&info);ro::finishSubmit(f.c,t,VK_NULL_HANDLE,VK_SUCCESS);
    require(ro::stats(f.c).acceptedLayoutOperations==3,"device-group submit2 rejected");
    f.submitInfo.pNext=&info;f.submit();require(ro::stats(f.c).acceptedLayoutOperations==3,"unknown submit pNext rejected");f.submitInfo.pNext=nullptr;
    const std::array<VkCommandBuffer,2> commands{f.command,handle<VkCommandBuffer>(999)};
    f.submitInfo.commandBufferCount=2;f.submitInfo.pCommandBuffers=commands.data();f.submit();
    require(ro::stats(f.c).acceptedLayoutOperations==3,"unknown command refuses entire captured submit transaction");
    f.submitInfo.commandBufferCount=1;f.submitInfo.pCommandBuffers=&f.command;auto duplicate=f.ticket();ro::finishSubmit(f.c,duplicate,VK_NULL_HANDLE,VK_SUCCESS);
    const auto before=ro::stats(f.c).acceptedLayoutOperations;ro::finishSubmit(f.c,duplicate,VK_NULL_HANDLE,VK_SUCCESS);
    require(ro::stats(f.c).acceptedLayoutOperations==before&&ro::stats(f.c).fatalIssues,"same ticket cannot finish twice");
    Fixture overlapping;overlapping.recordCommand();auto a=overlapping.ticket();auto b=overlapping.ticket();
    ro::finishSubmit(overlapping.c,a,VK_NULL_HANDLE,VK_SUCCESS);ro::finishSubmit(overlapping.c,b,VK_NULL_HANDLE,VK_SUCCESS);
    require(!ro::stats(overlapping.c).acceptedLayoutOperations&&ro::stats(overlapping.c).queueOverlaps==1,"overlapping same queue cannot be claimed ordered");
}
void emptySubmissions(){
    ++cases;Fixture f;
    auto t=ro::beginSubmit(f.c,f.queue,0,nullptr);ro::finishSubmit(f.c,t,VK_NULL_HANDLE,VK_SUCCESS);
    VkSubmitInfo legacy{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    t=ro::beginSubmit(f.c,f.queue,1,&legacy);ro::finishSubmit(f.c,t,VK_NULL_HANDLE,VK_SUCCESS);
    t=ro::beginSubmit2(f.c,f.queue,0,nullptr);ro::finishSubmit(f.c,t,VK_NULL_HANDLE,VK_SUCCESS);
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};signal.semaphore=handle<VkSemaphore>(80);
    signal.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;signal.value=42;
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.signalSemaphoreInfoCount=1;info.pSignalSemaphoreInfos=&signal;
    t=ro::beginSubmit2(f.c,f.queue,1,&info);ro::finishSubmit(f.c,t,VK_NULL_HANDLE,VK_SUCCESS);
    auto s=ro::stats(f.c);
    require(s.successfulDriverSubmissions==4&&!s.failedDriverSubmissions,"empty and semaphore-only submissions preserve driver counts");
    require(!s.acceptedRecordings&&!s.refusedRecordings&&!s.acceptedLayoutOperations,"empty submissions create no synthetic recording");
    require(!s.observedIssues&&!s.fatalIssues&&contains("\"has_captured_recording\":false"),"legal empty submission leaves no unknown-object pollution");
    t=ro::beginSubmit(f.c,f.queue,0,nullptr);ro::finishSubmit(f.c,t,VK_NULL_HANDLE,VK_ERROR_OUT_OF_HOST_MEMORY);
    s=ro::stats(f.c);require(s.failedDriverSubmissions==1&&!s.refusedRecordings&&s.observedIssues==rp::bit(rp::Issue::DriverFailed),"failed empty submission reports only real driver failure");
    f.recordCommand();f.submit();require(ro::stats(f.c).acceptedLayoutOperations==3&&!ro::stats(f.c).fatalIssues,"empty driver failure does not poison later recording");
}
void submit2SemaphoreGuards(){
    ++cases;Fixture f;f.recordCommand();
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};command.commandBuffer=f.command;command.deviceMask=1;
    for(unsigned direction=0;direction<2;++direction)for(unsigned variant=0;variant<9;++variant){
        VkSemaphoreSubmitInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};semaphore.semaphore=handle<VkSemaphore>(80);
        semaphore.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=1;info.pCommandBufferInfos=&command;
        auto& count=direction?info.signalSemaphoreInfoCount:info.waitSemaphoreInfoCount;
        auto& pointer=direction?info.pSignalSemaphoreInfos:info.pWaitSemaphoreInfos;
        count=1;pointer=&semaphore;
        if(variant==0)semaphore.pNext=&info;
        if(variant==1)semaphore.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
        if(variant==2)semaphore.deviceIndex=1;
        if(variant==3)semaphore.stageMask=VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        if(variant==4)semaphore.semaphore=VK_NULL_HANDLE;
        if(variant==5)pointer=nullptr;
        if(variant==6)count=ro::maxSubmittedSemaphores+1;
        if(variant==7)info.flags=VK_SUBMIT_PROTECTED_BIT;
        if(variant==8)info.pNext=&semaphore;
        auto t=ro::beginSubmit2(f.c,f.queue,1,&info);ro::finishSubmit(f.c,t,VK_NULL_HANDLE,VK_SUCCESS);
        require(t.issues&&!ro::stats(f.c).acceptedLayoutOperations&&!ro::stats(f.c).fatalIssues,"unsupported wait/signal metadata refuses whole transaction locally");
    }
    std::array<VkSemaphoreSubmitInfo,ro::maxSubmittedSemaphores> semaphores{};
    for(auto& semaphore:semaphores){semaphore.sType=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;semaphore.semaphore=handle<VkSemaphore>(80);semaphore.stageMask=VK_PIPELINE_STAGE_2_NONE;}
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=1;info.pCommandBufferInfos=&command;
    info.waitSemaphoreInfoCount=ro::maxSubmittedSemaphores;info.pWaitSemaphoreInfos=semaphores.data();
    info.signalSemaphoreInfoCount=1;info.pSignalSemaphoreInfos=semaphores.data();
    auto t=ro::beginSubmit2(f.c,f.queue,1,&info);ro::finishSubmit(f.c,t,VK_NULL_HANDLE,VK_SUCCESS);
    require((t.issues&rp::bit(rp::Issue::Capacity))&&!ro::stats(f.c).acceptedLayoutOperations,"combined semaphore budget checked");
    info.waitSemaphoreInfoCount=info.signalSemaphoreInfoCount=1;semaphores[0].value=123;
    t=ro::beginSubmit2(f.c,f.queue,1,&info);ro::finishSubmit(f.c,t,VK_NULL_HANDLE,VK_SUCCESS);
    require(!t.issues&&ro::stats(f.c).acceptedLayoutOperations==3,"core single-device semaphore description accepted without interpreting payload");
}
void quotaAndFiltering(){
    ++cases;Fixture f;f.recordCommand();ro::select(f.c,ro::Role::RealColor,f.record);
    for(unsigned i=0;i<140;++i)f.submit();auto s=ro::stats(f.c);
    require(s.acceptedLayoutOperations==420&&s.emittedOperations==256&&s.omittedOperations==164,"log budget does not truncate accepted operation count");
    require(s.emittedSummaries==128&&s.omittedSummaries==12,"submission summary budget bounded");
    require(matching("\"event\":\"rp_layout_operation\"")==256,"actual output respects quota");
    Fixture irrelevant(false);auto info=irrelevant.imageInfo;info.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
    auto record=inventory(info,999,999);ro::imageCreated(irrelevant.c,&info,handle<VkImage>(999),VK_SUCCESS,&record);
    require(ro::stats(irrelevant.c).ignoredNonAttachmentImages==1&&!ro::stats(irrelevant.c).metadata.registrations,"sampled-only images do not consume RP inventory");
    irrelevant.objects();irrelevant.recordCommand();irrelevant.submit();require(ro::stats(irrelevant.c).acceptedLayoutOperations==3,"skipped inventory does not advance RP monotonic serial");
}
void registrationGuardsAndRetention(){
    ++cases;Fixture mismatch(false);auto incorrect=mismatch.record;incorrect.description.width=1;
    ro::imageCreated(mismatch.c,&mismatch.imageInfo,mismatch.image,VK_SUCCESS,&incorrect);
    require(!ro::stats(mismatch.c).metadata.registrations&&ro::stats(mismatch.c).objectRejections==1,"mismatched inventory record rejected");
    mismatch.objects();mismatch.recordCommand();mismatch.submit();require(ro::stats(mismatch.c).acceptedLayoutOperations==3,"one rejected image input does not taint device");
    Fixture allocation(false);allocation.objects();VkCommandBuffer extra=handle<VkCommandBuffer>(51);auto ai=allocation.allocateInfo;ai.pNext=&allocation.beginInfo;
    ro::allocate(allocation.c,&ai,&extra,VK_SUCCESS);ro::begin(allocation.c,extra,&allocation.commandInfo,VK_SUCCESS);ro::end(allocation.c,extra,VK_SUCCESS);
    auto si=allocation.submitInfo;si.pCommandBuffers=&extra;auto t=ro::beginSubmit(allocation.c,allocation.queue,1,&si);ro::finishSubmit(allocation.c,t,VK_NULL_HANDLE,VK_SUCCESS);
    allocation.recordCommand();allocation.submit();require(ro::stats(allocation.c).acceptedLayoutOperations==3&&!ro::stats(allocation.c).fatalIssues,"unsupported allocation is command-local");
    auto before=ro::stats(allocation.c).acceptedLayoutOperations;ro::retire(allocation.c);allocation.submit();
    require(ro::stats(allocation.c).acceptedLayoutOperations==before&&ro::stats(allocation.c).fatalIssues,"retired context cannot emit more accepted work");
    ro::summary(allocation.c);require(contains("rp_metadata_summary"),"retired summary remains readable");
}
void noAllocAndCapacity(){
    ++cases;Fixture f(false,false);f.objects();f.recordCommand();f.submit();allocations=0;countAllocations=true;
    f.recordCommand();f.submit();ro::summary(f.c);countAllocations=false;
    require(ro::stats(f.c).acceptedLayoutOperations==6&&allocations==0,"adapter recording reuses registered payload pages without hot allocation");
    Fixture cap(false,false);
    for(std::uint64_t i=1;i<=1025;++i){auto record=inventory(cap.imageInfo,1000+i,i);ro::imageCreated(cap.c,&cap.imageInfo,handle<VkImage>(1000+i),VK_SUCCESS,&record);}
    require(ro::stats(cap.c).fatalIssues&rp::bit(rp::Issue::Capacity),"image capacity visible and fail closed");
}
void rejectionDiagnostics(){
    ++cases;Fixture f(false);f.viewInfo.components={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A};
    f.objects();f.viewInfo.components={};require(lineCount==0,"rejected objects still do not log inside caller locks");
    ro::begin(f.c,f.command,&f.commandInfo,VK_SUCCESS);ro::reset(f.c,f.command,VK_SUCCESS);
    readOnlyReentry=f.c;allocations=0;countAllocations=true;f.recordCommand();f.submit();ro::summary(f.c);countAllocations=false;
    auto s=ro::stats(f.c);require(!s.acceptedLayoutOperations&&!s.fatalIssues&&!s.failures&&allocations==0,"diagnostics do not relax rejection or allocate on rejection path");
    require(sameLine("\"site\":\"view\"","\"swizzle\":[3,4,5,6]"),"view diagnostic owns original copied components");
    require(sameLine("\"site\":\"begin_render_pass\"","\"incoming_command_issues\":0,\"added_issue_bits\":256"),"first rejection distinguished from inherited command issues");
    require(sameLine("\"site\":\"end_render_pass\"","\"incoming_command_issues\":256,\"added_issue_bits\":16")
        &&sameLine("\"site\":\"end_render_pass\"","\"active_rp_established\":false"),"diagnostic identifies existing cascade without changing acceptance rules");
    const auto emitted=matching("\"event\":\"rp_rejection_sample\"");ro::summary(f.c);ro::summary(f.c);
    require(matching("\"event\":\"rp_rejection_sample\"")==emitted&&s.diagnosticSamples==s.diagnosticEmitted,"immutable details emitted once while counts continue");
    require(contains("\"event\":\"rp_rejection_counts\"")&&contains("\"exact_rp_identity_matches\":true"),"frequencies and exact framebuffer relation exposed");
    for(std::size_t i=0;i<lineCount;++i)require(lines[i][std::strlen(lines[i].data())-1]=='}',"diagnostic JSON fits bounded complete line");
    Fixture unknown(false);unknown.viewInfo.image=handle<VkImage>(999);unknown.objects();unknown.recordCommand();unknown.submit();
    require(sameLine("\"site\":\"ignored_view\"","\"image\":\"0x3e7\"")&&sameLine("\"site\":\"framebuffer\"","\"ignored_view_creation_known\":true"),"unknown image relation survives for external swapchain correlation");
    require(!ro::stats(unknown.c).acceptedLayoutOperations&&!ro::stats(unknown.c).fatalIssues,"diagnostic cache cannot register an ignored view as supported");
    Fixture evictions(false);auto view=evictions.viewInfo;view.image=handle<VkImage>(999);
    for(std::uint64_t i=0;i<257;++i)ro::viewCreated(evictions.c,&view,handle<VkImageView>(1000+i),VK_SUCCESS);
    require(ro::stats(evictions.c).diagnosticIgnoredViewEvictions==1,"recent unknown view cache reports overwrite without coverage claim");
}
void diagnosticAggregation(){
    ++cases;namespace rd=xess_vk::render_pass_diagnostics;rd::Store store;rd::Sample sample;sample.site=rd::Site::View;sample.issues=rp::bit(rp::Issue::UnsupportedView);
    for(unsigned i=0;i<100;++i){sample.view.key={100+i,1+i};store.observe(sample);}
    for(unsigned i=1;i<=10;++i){sample.view.format=static_cast<VkFormat>(i);store.observe(sample);}
    require(store.totals.observations==110&&store.totals.storedSamples==8&&store.totals.omittedSamples==3,"semantic frequency groups exclude handles and enforce per-site budget");
    sample.site=rd::Site::Image;store.observe(sample);require(store.totals.storedSamples==9,"one busy site cannot consume another site's diagnostic quota");
    rd::Sample copy;std::uint64_t count{};const auto first=static_cast<std::size_t>(rd::Site::View)*rd::samplesPerSite;
    require(store.take(first,copy,count)&&count==100&&!store.take(first,copy,count),"sample drained only once; repeated observations remain counted");
    const auto signature=rd::signature(sample);sample.pass.attachments.back().hasPNext=true;
    require(!(signature==rd::signature(sample)),"last attachment field is inside exact bounded signature");
    sample.pass.attachments.back().hasPNext=false;sample.image.description.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    require(!(signature==rd::signature(sample)),"image shape flags stay distinct");
    store.totals.observations=UINT64_MAX;store.observe(sample);require(store.totals.observations==UINT64_MAX,"diagnostic counters saturate without wrapping");
}
void projectionBindingAndEvidence(){
    ++cases;Fixture f;ro::select(f.c,ro::Role::RealColor,f.record);require(lineCount==0,"selection hook emits no I/O");
    ro::begin(f.c,f.command,&f.commandInfo,VK_SUCCESS);ro::select(f.c,ro::Role::RealColor,f.record);
    ro::beginRenderPass(f.c,f.command,&f.beginInfo,VK_SUBPASS_CONTENTS_INLINE);ro::select(f.c,ro::Role::RealColor,f.record);
    ro::endRenderPass(f.c,f.command);ro::end(f.c,f.command,VK_SUCCESS);
    auto ticket=f.ticket();ro::select(f.c,ro::Role::RealColor,f.record);readOnlyReentry=f.c;SetLastError(0xbeef);
    ro::finishSubmit(f.c,ticket,VK_NULL_HANDLE,VK_SUCCESS);require(GetLastError()==0xbeef,"projection sink preserves LastError");
    auto s=ro::stats(f.c);require(s.acceptedLayoutOperations==3&&s.projection.acceptedSlots==1&&s.projection.layoutOperations==3,"projection and legacy counters are independent");
    require(s.projection.selectionChanges==1&&s.projection.capturedCommands==1&&s.projection.completedCommands==1&&s.projection.disabledSlots==2,"identical callbacks do not change selection epoch");
    require(ticket.projections[0].frozenSelections[2].selectionSerial==1&&ticket.projections[0].frozenSelections[2].image==rp::Key{10,1},"ticket froze actual selection before driver");
    require(matching("\"event\":\"rp_projection_layout_operation\"")==3&&sameLine("\"event\":\"rp_projection_layout_operation\"","\"source_token\":0,\"pass_token\":0"),"projection logs have distinct event and unknown source tokens");
    require(sameLine("\"event\":\"rp_projection_layout_operation\"","\"source_association_known\":false,\"candidate_semantics_verified\":false")
        &&sameLine("\"event\":\"rp_projection_submission\"","\"memory_alias_unverified\":true"),"projection never asserts frame, alias or copy semantics");
    ro::summary(f.c);require(sameLine("\"event\":\"rp_projection_summary\"","\"layout_operations\":3"),"separate projection summary emitted");
    Fixture late;late.recordCommand();ro::select(late.c,ro::Role::RealColor,late.record);late.submit();
    require(ro::stats(late.c).acceptedLayoutOperations==3&&!ro::stats(late.c).projection.layoutOperations&&ro::stats(late.c).projection.refusedSlots==1,"late selection cannot backfill old recording");
    late.recordCommand();late.submit();require(ro::stats(late.c).projection.layoutOperations==3,"next recording freezes the selection");
    Fixture depth(false);depth.imageInfo.format=depth.viewInfo.format=depth.attachment.format=VK_FORMAT_R32_SFLOAT;depth.record=inventory(depth.imageInfo,10,1);depth.objects();
    ro::select(depth.c,ro::Role::Depth,depth.record);depth.recordCommand();depth.submit();
    require(ro::stats(depth.c).projection.layoutOperations==3&&sameLine("\"event\":\"rp_projection_layout_operation\"","\"aspect_mask\":1"),"semantic depth in R32F still uses COLOR aspect");
}
void projectionLongRecordingJournal(){
    ++cases;Fixture f;
    auto otherRecord=inventory(f.imageInfo,11,2);
    ro::imageCreated(f.c,&f.imageInfo,handle<VkImage>(11),VK_SUCCESS,&otherRecord);
    auto otherViewInfo=f.viewInfo;otherViewInfo.image=handle<VkImage>(11);
    const auto otherView=handle<VkImageView>(21),otherFramebufferView=otherView;
    ro::viewCreated(f.c,&otherViewInfo,otherView,VK_SUCCESS);
    auto otherFbInfo=f.fbInfo;otherFbInfo.pAttachments=&otherFramebufferView;
    const auto otherFb=handle<VkFramebuffer>(41);
    ro::framebufferCreated(f.c,&otherFbInfo,otherFb,VK_SUCCESS);
    auto unrelated=f.beginInfo;unrelated.framebuffer=otherFb;
    ro::select(f.c,ro::Role::RealColor,f.record);
    auto record=[&](std::size_t count){
        ro::begin(f.c,f.command,&f.commandInfo,VK_SUCCESS);
        for(std::size_t i=0;i<count;++i){
            ro::beginRenderPass(f.c,f.command,i+1==count?&f.beginInfo:&unrelated,VK_SUBPASS_CONTENTS_INLINE);
            ro::endRenderPass(f.c,f.command);
        }
        ro::end(f.c,f.command,VK_SUCCESS);f.submit();
    };
    ro::begin(f.c,f.command,&f.commandInfo,VK_SUCCESS);ro::reset(f.c,f.command,VK_SUCCESS);
    countAllocations=true;record(149);countAllocations=false;
    auto s=ro::stats(f.c);
    require(s.projection.layoutOperations==3&&s.projection.acceptedSlots==1,"late selected attachment survives 148 unrelated render passes");
    require(s.projection.journalObservedPasses==149&&s.projection.journalRetainedPasses==149&&!s.projection.journalDroppedPasses
        &&s.projection.journalMaxObservedPasses==149,"journal counters count commands once, not three roles");
    require(!s.acceptedLayoutOperations,"legacy 96-operation limit remains independent and conservative");
    require(sameLine("\"event\":\"rp_projection_submission\"","\"journal_observed_passes\":149,\"journal_retained_passes\":149,\"journal_dropped_passes\":0"),"accepted projection emits exact journal coverage");
    record(rp::maxProjectionPasses+1);s=ro::stats(f.c);
    require(s.projection.layoutOperations==3&&s.projection.refusedSlots==1&&s.projection.journalDroppedPasses==1,"overflow never publishes a partial layout sequence");
    require((s.projection.issues&rp::bit(rp::Issue::Capacity))&&!(s.projection.issues&rp::bit(rp::Issue::IncompleteRenderPass)),"capacity is not misreported as a missing matching End");
    record(1);s=ro::stats(f.c);
    require(s.projection.layoutOperations==6&&s.projection.acceptedSlots==2,"fresh recording recovers from prior local overflow");
    ro::summary(f.c);
    require(sameLine("\"event\":\"rp_projection_summary\"","\"journal_max_observed_passes\":257,\"journal_pass_capacity\":256"),"bounded journal saturation remains visible after detail logging");
    require(!allocations,"long recording adds no hook-path allocation");
}
void projectionIsolationAndUnused(){
    ++cases;
    for(bool unused:{false,true}) {
        Fixture f(false);ro::imageCreated(f.c,&f.imageInfo,f.image,VK_SUCCESS,&f.record);ro::viewCreated(f.c,&f.viewInfo,f.view,VK_SUCCESS);
        auto ds=f.imageInfo;ds.format=VK_FORMAT_D32_SFLOAT_S8_UINT;ds.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        const auto dsRecord=inventory(ds,11,2);ro::imageCreated(f.c,&ds,handle<VkImage>(11),VK_SUCCESS,&dsRecord);
        auto dsView=f.viewInfo;dsView.image=handle<VkImage>(11);dsView.format=ds.format;dsView.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
        ro::viewCreated(f.c,&dsView,handle<VkImageView>(21),VK_SUCCESS);
        std::array<VkAttachmentDescription,2> attachments{f.attachment,f.attachment};attachments[1].format=ds.format;
        VkAttachmentReference depth{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};auto sub=f.subpass;sub.pDepthStencilAttachment=&depth;
        if(unused){sub.colorAttachmentCount=0;sub.pColorAttachments=nullptr;}
        auto pass=f.passInfo;pass.attachmentCount=2;pass.pAttachments=attachments.data();pass.pSubpasses=&sub;
        ro::renderPassCreated(f.c,&pass,f.pass,VK_SUCCESS);std::array<VkImageView,2> views{f.view,handle<VkImageView>(21)};
        auto fb=f.fbInfo;fb.attachmentCount=2;fb.pAttachments=views.data();ro::framebufferCreated(f.c,&fb,f.fb,VK_SUCCESS);
        ro::queue(f.c,f.queue,0);ro::allocate(f.c,&f.allocateInfo,&f.command,VK_SUCCESS);
        ro::select(f.c,ro::Role::RealColor,f.record);ro::select(f.c,ro::Role::Depth,dsRecord);
        const auto absentRecord=inventory(f.imageInfo,12,3);ro::imageCreated(f.c,&f.imageInfo,handle<VkImage>(12),VK_SUCCESS,&absentRecord);
        ro::select(f.c,ro::Role::Motion,absentRecord);f.recordCommand();f.submit();const auto s=ro::stats(f.c);
        require(!s.acceptedLayoutOperations&&s.refusedRecordings==1,"legacy keeps all-CB DS rejection");
        require(s.projection.acceptedSlots==2&&s.projection.noDirectSlots==1&&s.projection.selectionRejections==1
            &&s.projection.layoutOperations==(unused?2u:3u),"known unrelated DS isolated; unsupported selected DS rejected before binding");
        require(sameLine("\"event\":\"rp_projection_submission\"","\"classification\":\"no_direct_attachment_reference\""),"absent candidate logged with limited classification");
        if(unused)require(sameLine("\"event\":\"rp_projection_layout_operation\"","\"source_kind\":\"unused_initial_to_final_transition\""),"unused transition has distinct source kind");
    }
}
void projectionTransactionsAndLifetime(){
    ++cases;Fixture f;ro::select(f.c,ro::Role::RealColor,f.record);f.recordCommand();f.submit(VK_ERROR_OUT_OF_HOST_MEMORY);
    require(!ro::stats(f.c).projection.layoutOperations&&ro::stats(f.c).projection.refusedSlots==1,"failed original driver result exposes zero projected operations");
    f.submit();require(ro::stats(f.c).projection.layoutOperations==3,"failed ticket consumed without preventing later submit");
    for(unsigned variant=0;variant<3;++variant) {
        Fixture invalid;ro::select(invalid.c,ro::Role::RealColor,invalid.record);invalid.recordCommand();
        std::array<VkCommandBuffer,2> commands{invalid.command,handle<VkCommandBuffer>(999)};
        if(variant==0)invalid.submitInfo.pNext=&invalid.beginInfo;
        if(variant==1){invalid.submitInfo.commandBufferCount=2;invalid.submitInfo.pCommandBuffers=commands.data();}
        auto ticket=invalid.ticket();
        if(variant==2)++ticket.commands[0].generation;
        ro::finishSubmit(invalid.c,ticket,VK_NULL_HANDLE,VK_SUCCESS);require(!ro::stats(invalid.c).projection.layoutOperations,"invalid enclosing transaction suppresses all projection output");
        invalid.submitInfo.pNext=nullptr;invalid.submitInfo.commandBufferCount=1;invalid.submitInfo.pCommandBuffers=&invalid.command;
        invalid.submit();require(ro::stats(invalid.c).projection.layoutOperations==3,"suppressed transaction still consumed captured core ticket");
    }
    for(unsigned variant=0;variant<3;++variant) {
        Fixture stale;ro::select(stale.c,ro::Role::RealColor,stale.record);stale.recordCommand();const auto ticket=stale.ticket();
        if(variant==0){ro::reset(stale.c,stale.command,VK_SUCCESS);stale.recordCommand();}
        if(variant==1)ro::imageDestroying(stale.c,stale.image);
        if(variant==2){auto record=inventory(stale.imageInfo,11,2);ro::imageCreated(stale.c,&stale.imageInfo,handle<VkImage>(11),VK_SUCCESS,&record);ro::select(stale.c,ro::Role::RealColor,record);}
        ro::finishSubmit(stale.c,ticket,VK_NULL_HANDLE,VK_SUCCESS);require(!ro::stats(stale.c).projection.layoutOperations&&ro::stats(stale.c).projection.refusedSlots==1,"reset, destruction and changed selection cannot relabel pre-driver work");
    }
    Fixture empty;ro::select(empty.c,ro::Role::RealColor,empty.record);auto ticket=ro::beginSubmit(empty.c,empty.queue,0,nullptr);ro::finishSubmit(empty.c,ticket,VK_NULL_HANDLE,VK_SUCCESS);
    require(!ro::stats(empty.c).projection.capturedCommands&&!ro::stats(empty.c).projection.completedCommands&&!ro::stats(empty.c).projection.acceptedSlots,"empty submit never creates projection tickets");
    Fixture submit2;ro::select(submit2.c,ro::Role::RealColor,submit2.record);submit2.recordCommand();
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=submit2.command;cb.deviceMask=1;
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=1;info.pCommandBufferInfos=&cb;
    ticket=ro::beginSubmit2(submit2.c,submit2.queue,1,&info);ro::finishSubmit(submit2.c,ticket,VK_NULL_HANDLE,VK_SUCCESS);
    require(ro::stats(submit2.c).projection.layoutOperations==3,"Submit2 pre/post integration consumes one per-CB projection ticket");
}
void projectionQuotaAndAllocation(){
    ++cases;Fixture f;ro::select(f.c,ro::Role::RealColor,f.record);f.recordCommand();
    for(unsigned n=0;n<140;++n)f.submit();const auto s=ro::stats(f.c);
    require(s.projection.layoutOperations==420&&s.projection.emittedOperations==256&&s.projection.omittedOperations==164,"projection output budget does not cap semantic accounting");
    require(s.projection.emittedSummaries==128&&s.projection.omittedSummaries==12&&matching("\"event\":\"rp_projection_layout_operation\"")==256,"projection summaries and operations have separate bounded quotas");
    require(s.emittedOperations==256&&s.omittedOperations==164&&s.acceptedLayoutOperations==420,"legacy counters and quotas unchanged");
    Fixture noHeap(false);noHeap.objects();ro::select(noHeap.c,ro::Role::RealColor,noHeap.record);
    ro::begin(noHeap.c,noHeap.command,&noHeap.commandInfo,VK_SUCCESS);ro::reset(noHeap.c,noHeap.command,VK_SUCCESS);
    allocations=0;countAllocations=true;
    noHeap.recordCommand();noHeap.submit();ro::summary(noHeap.c);countAllocations=false;
    require(ro::stats(noHeap.c).projection.layoutOperations==3&&allocations==0,"integrated projection has no per-hook heap allocations");
}
rp::ImageDescription swapchainDescription(const Fixture& f) {
    rp::ImageDescription d;d.key={10,1};d.type=f.imageInfo.imageType;d.format=f.imageInfo.format;d.extent=f.imageInfo.extent;
    d.mipLevels=f.imageInfo.mipLevels;d.arrayLayers=f.imageInfo.arrayLayers;d.samples=f.imageInfo.samples;d.usage=f.imageInfo.usage;
    d.flags=f.imageInfo.flags;d.tiling=f.imageInfo.tiling;d.sharing=f.imageInfo.sharingMode;d.unsupportedPNext=false;return d;
}
void diagnosticTailAfterRejectedPass(){
    ++cases;
    for(unsigned submit2=0;submit2<2;++submit2){
        Fixture f(false);ro::swapchainImageObserved(f.c,swapchainDescription(f));
        ro::viewCreated(f.c,&f.viewInfo,f.view,VK_SUCCESS);ro::renderPassCreated(f.c,&f.passInfo,f.pass,VK_SUCCESS);
        ro::framebufferCreated(f.c,&f.fbInfo,f.fb,VK_SUCCESS);ro::queue(f.c,f.queue,0);ro::allocate(f.c,&f.allocateInfo,&f.command,VK_SUCCESS);
        xess_vk::render_tail::Store tail(1,sink);xess_vk::wsi_image_inventory::OwnedImage wsi{};
        wsi.record=f.record;wsi.owner={300,4};tail.wsiAdded(wsi);tail.arm();
        ro::begin(f.c,f.command,&f.commandInfo,VK_SUCCESS);tail.begin(ro::recordingFacts(f.c,f.command),VK_SUCCESS);
        ro::beginRenderPass(f.c,f.command,&f.beginInfo,VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);ro::endRenderPass(f.c,f.command);
        ro::beginRenderPass(f.c,f.command,&f.beginInfo,VK_SUBPASS_CONTENTS_INLINE);
        const auto snapshot=ro::diagnosticPassSnapshot(f.c,f.command,&f.beginInfo);
        require(snapshot.command.issues&&!snapshot.command.activeFramebuffer.handle,"earlier rejected pass keeps production metadata refused");
        require(snapshot.count==1&&snapshot.views[0].image==snapshot.images[0].description.key,"diagnostic current WSI attachment survives earlier pass rejection");
        xess_vk::render_tail::Event pass{};pass.kind=xess_vk::render_tail::Kind::Pass;pass.recording=snapshot.recording;
        pass.issues=snapshot.command.issues|snapshot.pass.issues|snapshot.framebuffer.issues;pass.imageCount=snapshot.count;
        const auto& image=snapshot.images[0].description;
        pass.images[0].record={image.key.handle,image.key.generation,{std::uint32_t(image.format),image.extent.width,image.extent.height,image.extent.depth,image.mipLevels,image.arrayLayers,std::uint32_t(image.samples),image.usage,image.flags,std::uint32_t(image.tiling),std::uint32_t(image.sharing)}};
        pass.images[0].view={snapshot.views[0].key.handle,snapshot.views[0].key.generation};
        pass.images[0].role=xess_vk::render_tail::Attachment;tail.record(pass);tail.draw(snapshot.recording,{});
        ro::endRenderPass(f.c,f.command);tail.endPass(ro::recordingFacts(f.c,f.command));
        ro::end(f.c,f.command,VK_SUCCESS);tail.end(ro::recordingFacts(f.c,f.command),VK_SUCCESS);
        VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};command.commandBuffer=f.command;command.deviceMask=1;
        VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=1;info.pCommandBufferInfos=&command;
        const auto ticket=submit2?ro::beginSubmit2(f.c,f.queue,1,&info):f.ticket();
        std::array<xess_vk::render_tail::Key,ro::maxSubmittedCommands> keys{};
        for(std::size_t n=0;n<ticket.commandCount;++n)keys[n]={ticket.commands[n].handle,ticket.commands[n].generation};
        const auto captured=tail.beforeSubmit(ticket.queue,ticket.token,{keys.data(),ticket.commandCount});require(captured.serial,"real submit ticket retains diagnostic WSI write anchor");
        ro::finishSubmit(f.c,ticket,VK_NULL_HANDLE,VK_SUCCESS);tail.submitted(ticket.queue,ticket.token,{keys.data(),ticket.commandCount},VK_SUCCESS);tail.afterSubmit(captured,VK_SUCCESS);
        require(tail.stats().emittedSamples==1&&!ro::stats(f.c).acceptedRecordings,"diagnostic sample does not admit rejected production recording");
        require(contains("\"event\":\"render_tail_sample\"")&&contains("\"copy_permitted\":false")&&!contains("\"copy_permitted\":true"),"diagnostic sample remains observation only");
        ro::reset(f.c,f.command,VK_SUCCESS);ro::begin(f.c,f.command,&f.commandInfo,VK_SUCCESS);tail.begin(ro::recordingFacts(f.c,f.command),VK_SUCCESS);
        require(!ro::diagnosticPassSnapshot(f.c,f.command,nullptr).count,"fresh recording does not inherit prior attachment");
    }
}
void swapchainIdentityAdapter(){
    ++cases;Fixture f(false);const auto d=swapchainDescription(f);ro::swapchainImageObserved(f.c,d);ro::swapchainImageObserved(f.c,d);
    require(lineCount==0&&ro::stats(f.c).projection.swapchainImagesRegistered==1&&ro::stats(f.c).projection.swapchainImageDuplicates==1,"WSI identity hooks are silent and exact repeated observations idempotent");
    ro::viewCreated(f.c,&f.viewInfo,f.view,VK_SUCCESS);ro::renderPassCreated(f.c,&f.passInfo,f.pass,VK_SUCCESS);ro::framebufferCreated(f.c,&f.fbInfo,f.fb,VK_SUCCESS);
    ro::queue(f.c,f.queue,0);ro::allocate(f.c,&f.allocateInfo,&f.command,VK_SUCCESS);ro::select(f.c,ro::Role::RealColor,f.record);f.recordCommand();
    readOnlyReentry=f.c;f.submit();auto s=ro::stats(f.c);
    require(s.projection.layoutOperations==3&&!s.ignoredUntrackedViews&&s.projection.swapchainSamplesEmitted==1,"explicit WSI identity enables known view relationship without fake creation");
    require(sameLine("\"event\":\"rp_swapchain_image_observation\"","\"identity_source\":\"audited_swapchain_enumeration\""),"WSI diagnostic source differs from vkCreateImage");
    const auto pending=f.ticket();ro::imageDestroying(f.c,f.image);ro::finishSubmit(f.c,pending,VK_NULL_HANDLE,VK_SUCCESS);
    require(ro::stats(f.c).projection.layoutOperations==3,"destroyed swapchain image invalidates projected facts");
    for(unsigned variant=0;variant<4;++variant){
        Fixture bad(false);auto desc=swapchainDescription(bad);
        if(variant==0)desc.unsupportedPNext=true;if(variant==1)desc.flags=VK_IMAGE_CREATE_ALIAS_BIT;
        if(variant==2)desc.usage=VK_IMAGE_USAGE_SAMPLED_BIT;if(variant==3)desc.key.generation=0;
        ro::swapchainImageObserved(bad.c,desc);require(!ro::stats(bad.c).projection.swapchainImagesRegistered&&ro::stats(bad.c).projection.swapchainImageRejections==1,"unaudited WSI descriptor never registers identity");
    }
    Fixture conflict(false);ro::swapchainImageObserved(conflict.c,swapchainDescription(conflict));auto changed=swapchainDescription(conflict);++changed.extent.width;
    ro::swapchainImageObserved(conflict.c,changed);require(ro::stats(conflict.c).fatalIssues&&ro::stats(conflict.c).projection.swapchainImageRejections==1,"conflicting same-generation WSI facts fail closed");
    Fixture bounded(false);auto desc=swapchainDescription(bounded);
    for(std::uint64_t n=1;n<=10;++n){desc.key={100+n,n};ro::swapchainImageObserved(bounded.c,desc);}ro::summary(bounded.c);ro::summary(bounded.c);
    require(ro::stats(bounded.c).projection.swapchainSamplesEmitted==8&&ro::stats(bounded.c).projection.swapchainSamplesOmitted==2
        &&matching("\"event\":\"rp_swapchain_image_observation\"")==8,"WSI diagnostic samples bounded and drained once");
    Fixture aba(false);auto first=swapchainDescription(aba);ro::swapchainImageObserved(aba.c,first);ro::swapchainImageRetiring(aba.c,first.key);
    auto second=first;second.key.generation=2;ro::swapchainImageObserved(aba.c,second);ro::swapchainImageRetiring(aba.c,first.key);
    ro::viewCreated(aba.c,&aba.viewInfo,aba.view,VK_SUCCESS);ro::renderPassCreated(aba.c,&aba.passInfo,aba.pass,VK_SUCCESS);ro::framebufferCreated(aba.c,&aba.fbInfo,aba.fb,VK_SUCCESS);
    ro::queue(aba.c,aba.queue,0);ro::allocate(aba.c,&aba.allocateInfo,&aba.command,VK_SUCCESS);aba.record.generation=2;
    ro::select(aba.c,ro::Role::RealColor,aba.record);aba.recordCommand();aba.submit();
    require(ro::stats(aba.c).projection.layoutOperations==3&&ro::stats(aba.c).projection.swapchainImagesRetired==1
        &&ro::stats(aba.c).projection.swapchainRetirementsIgnored==1,"stale WSI removal cannot erase a reused handle's new generation");
    const auto waiting=aba.ticket();ro::swapchainImageRetiring(aba.c,second.key);ro::finishSubmit(aba.c,waiting,VK_NULL_HANDLE,VK_SUCCESS);
    require(ro::stats(aba.c).projection.layoutOperations==3&&ro::stats(aba.c).projection.swapchainImagesRetired==2,"exact WSI removal invalidates pending projection");
    Fixture wrongOrigin;ro::swapchainImageRetiring(wrongOrigin.c,{10,1});ro::select(wrongOrigin.c,ro::Role::RealColor,wrongOrigin.record);
    wrongOrigin.recordCommand();wrongOrigin.submit();require(ro::stats(wrongOrigin.c).projection.layoutOperations==3
        &&ro::stats(wrongOrigin.c).projection.swapchainRetirementsIgnored==1,"WSI retirement cannot erase a vkCreateImage identity");
}
struct RecordingFixture {
    ro::Context* c=ro::create(handle<VkDevice>(0x900),nullptr);
    VkCommandPool pool=handle<VkCommandPool>(0x901);VkCommandBuffer command=handle<VkCommandBuffer>(0x902);
    VkQueue queue=handle<VkQueue>(0x903);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    explicit RecordingFixture(bool start=true){
        require(c!=nullptr,"recording context creation");poolInfo.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;poolInfo.queueFamilyIndex=3;
        alloc.commandPool=pool;alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;alloc.commandBufferCount=1;
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        ro::poolCreated(c,pool,&poolInfo,VK_SUCCESS);ro::allocate(c,&alloc,&command,VK_SUCCESS);ro::queue(c,queue,3);
        if(start)ro::begin(c,command,&begin,VK_SUCCESS);
    }
    ~RecordingFixture(){ro::destroy(c);}
    auto facts(){return ro::recordingFacts(c,command);}
    auto ticket(){return ro::beginSubmit(c,queue,1,&submit);}
    void finish(VkResult result=VK_SUCCESS){const auto t=ticket();ro::finishSubmit(c,t,VK_NULL_HANDLE,result);}
};
using CR=xess_vk::CommandRecordingReason;
bool has(const xess_vk::CommandRecordingFacts& f,CR reason){return (f.reasons&xess_vk::bit(reason))!=0;}
void actualRecordingFactsAndScope(){
    ++cases;RecordingFixture f;auto facts=f.facts();
    require(facts.supported()&&facts.allocationKnown&&facts.poolKnown&&facts.beginFlagsKnown&&facts.stateKnown&&facts.recording&&!facts.executable,"literal primary ONE_TIME recording facts");
    require(facts.device==0x900&&facts.contextIncarnation&&facts.command==0x902&&facts.rpRecordingGeneration==1
        &&facts.poolGeneration&&facts.poolFlags==f.poolInfo.flags&&facts.queueFamily==3&&facts.level==VK_COMMAND_BUFFER_LEVEL_PRIMARY,"device/context, exact pool and original flags copied");
    require(facts.scopeKnown&&!facts.inRenderPass&&!facts.inDynamicRendering&&!facts.completeHookCoverage&&!facts.gpuComplete&&!facts.layoutVerified&&!facts.copyPermitted,"narrow initial outside scope grants no GPU proof");
    SetLastError(0xbeef);facts=ro::dispatchPoint(f.c,f.command);require(GetLastError()==0xbeef&&facts.commonRecordOrder==1,"dispatch point preserves LastError and starts common clock");
    ro::explicitBarrier(f.c,f.command);VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};pass.renderPass=handle<VkRenderPass>(0xff0);pass.framebuffer=handle<VkFramebuffer>(0xff1);
    ro::beginRenderPass(f.c,f.command,&pass,VK_SUBPASS_CONTENTS_INLINE);facts=ro::dispatchPoint(f.c,f.command);
    require(facts.commonRecordOrder==4&&facts.scopeKnown&&facts.inRenderPass&&!facts.inDynamicRendering,"attachment rejection cannot erase actual inside-RP scope");
    ro::endRenderPass(f.c,f.command);facts=ro::dispatchPoint(f.c,f.command);
    require(facts.commonRecordOrder==6&&facts.scopeKnown&&!facts.inRenderPass,"RP end and dispatch preserve strict shared order");
    VkRenderingInfo dynamic{VK_STRUCTURE_TYPE_RENDERING_INFO};ro::beginDynamicRendering(f.c,f.command,&dynamic);facts=ro::dispatchPoint(f.c,f.command);
    require(facts.commonRecordOrder==8&&facts.scopeKnown&&facts.inDynamicRendering&&!facts.inRenderPass&&has(facts,CR::DynamicRendering),"dynamic begin records actual scope despite unsupported coverage");
    ro::endDynamicRendering(f.c,f.command);facts=ro::dispatchPoint(f.c,f.command);
    require(facts.commonRecordOrder==10&&facts.scopeKnown&&!facts.inDynamicRendering,"dynamic end restores known narrow outside scope");
    ro::unsupported(f.c,f.command,rp::Issue::UnsupportedExtension);ro::unsupported(f.c,f.command,rp::Issue::Secondary);ro::nextSubpass(f.c,f.command);
    facts=f.facts();require(has(facts,CR::Events)&&has(facts,CR::SecondaryExecution)&&has(facts,CR::NextSubpass)&&!facts.completeHookCoverage,"opaque controls never grant coverage");
    ro::begin(f.c,f.command,&f.begin,VK_SUCCESS);facts=f.facts();require(facts.supported()&&facts.rpRecordingGeneration==2&&facts.commonRecordOrder==0,"fresh successful recording removes old per-recording unsupported history");
    ro::endRenderPass(f.c,f.command);require(!f.facts().scopeKnown&&has(f.facts(),CR::ScopeUnknown),"unmatched scope end fails closed");
    ro::begin(f.c,f.command,&f.begin,VK_SUCCESS);ro::end(f.c,f.command,VK_SUCCESS);facts=f.facts();
    require(facts.stateKnown&&!facts.recording&&facts.executable&&facts.endSucceeded,"observed successful end reports executable before any submit");
    f.finish();facts=f.facts();require(facts.successfulSubmissionCount==1&&facts.lastSubmitQueue==0x903&&facts.lastSubmitSerial==1&&facts.lastSubmitQueueKnown
        &&facts.endSucceeded&&!facts.stateKnown&&!facts.executable&&has(facts,CR::OneTimeAlreadySubmitted)&&has(facts,CR::CompletionUnknown),"successful submit is history with unknown current/pending/completion state");
    f.finish();facts=f.facts();require(facts.successfulSubmissionCount==2&&has(facts,CR::DuplicateSubmission),"one-time replay remains explicit and unsupported");
}
void recordingFailureAndSupportFacts(){
    ++cases;
    for(unsigned variant=0;variant<6;++variant){
        RecordingFixture f(false);
        if(variant==0)f.begin.flags=0;
        if(variant==1)f.begin.flags|=VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        if(variant==2)f.begin.pNext=&f.poolInfo;
        if(variant==3){ro::free(f.c,1,&f.command);f.alloc.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;ro::allocate(f.c,&f.alloc,&f.command,VK_SUCCESS);}
        if(variant==4){ro::free(f.c,1,&f.command);f.alloc.commandPool=handle<VkCommandPool>(0xeee);ro::allocate(f.c,&f.alloc,&f.command,VK_SUCCESS);}
        if(variant==5){ro::free(f.c,1,&f.command);f.alloc.pNext=&f.poolInfo;ro::allocate(f.c,&f.alloc,&f.command,VK_SUCCESS);}
        ro::begin(f.c,f.command,&f.begin,VK_SUCCESS);const auto facts=f.facts();
        const CR reasons[]={CR::NotOneTime,CR::SimultaneousUse,CR::BeginPNext,CR::Secondary,CR::UnknownPool,CR::AllocationPNext};
        require(!facts.supported()&&has(facts,reasons[variant])&&facts.beginUsageFlags==f.begin.flags,"reusable/secondary/simultaneous/pNext/unknown-pool literal facts remain unsupported");
        if(variant==3)require(!facts.scopeKnown&&facts.level==VK_COMMAND_BUFFER_LEVEL_SECONDARY,"secondary inherited scope is never guessed");
    }
    RecordingFixture f;auto key=f.facts().rpRecordingGeneration;
    ro::begin(f.c,f.command,&f.begin,VK_ERROR_OUT_OF_HOST_MEMORY);require(f.facts().rpRecordingGeneration==key&&!f.facts().stateKnown&&has(f.facts(),CR::DriverStateUnknown),"failed begin cannot generate identity or restore recording eligibility");
    ro::begin(f.c,f.command,&f.begin,VK_SUCCESS);require(f.facts().supported()&&f.facts().rpRecordingGeneration==key+1,"next successful begin recovers after failure");
    ro::end(f.c,f.command,VK_ERROR_OUT_OF_HOST_MEMORY);require(!f.facts().executable&&!f.facts().stateKnown,"failed end cannot invent executable state");
    ro::begin(f.c,f.command,&f.begin,VK_SUCCESS);key=f.facts().rpRecordingGeneration;
    ro::reset(f.c,f.command,VK_ERROR_OUT_OF_HOST_MEMORY);require(f.facts().rpRecordingGeneration==key&&!f.facts().stateKnown,"failed reset preserves identity but invalidates state certainty");
    ro::reset(f.c,f.command,VK_SUCCESS);require(!f.facts().rpRecordingGeneration&&f.facts().stateKnown&&!f.facts().recording&&!f.facts().beginFlagsKnown,"successful reset invalidates recording identity and flags");
    ro::begin(f.c,f.command,&f.begin,VK_SUCCESS);require(f.facts().supported(),"successful reset followed by begin recovers");
    ro::resetPool(f.c,f.pool,false,VK_ERROR_OUT_OF_HOST_MEMORY);require(!f.facts().stateKnown,"failed pool reset cannot restore certainty");
    ro::resetPool(f.c,f.pool,false,VK_SUCCESS);ro::begin(f.c,f.command,&f.begin,VK_SUCCESS);require(f.facts().supported(),"successful pool reset followed by begin recovers");
    ro::end(f.c,f.command,VK_SUCCESS);f.finish(VK_ERROR_OUT_OF_HOST_MEMORY);require(!f.facts().successfulSubmissionCount&&!f.facts().stateKnown,"failed driver submit never counts success or infers completion");
    ro::begin(f.c,f.command,&f.begin,VK_SUCCESS);ro::end(f.c,f.command,VK_SUCCESS);ro::queue(f.c,handle<VkQueue>(0x999),9);f.queue=handle<VkQueue>(0x999);f.finish();
    require(f.facts().successfulSubmissionCount==1&&has(f.facts(),CR::QueueFamilyMismatch),"known wrong queue family retains success history and explicit refusal");
}
void recordingIdentityTicketsAndNoAllocation(){
    ++cases;
    for(unsigned variant=0;variant<4;++variant){
        RecordingFixture f;ro::end(f.c,f.command,VK_SUCCESS);const auto ticket=f.ticket();const auto generation=f.facts().rpRecordingGeneration;const auto poolGeneration=f.facts().poolGeneration;
        if(variant==0)ro::reset(f.c,f.command,VK_SUCCESS);
        if(variant==1){ro::free(f.c,1,&f.command);ro::allocate(f.c,&f.alloc,&f.command,VK_SUCCESS);}
        if(variant==2){ro::resetPool(f.c,f.pool,true,VK_SUCCESS);ro::poolCreated(f.c,f.pool,&f.poolInfo,VK_SUCCESS);ro::allocate(f.c,&f.alloc,&f.command,VK_SUCCESS);}
        if(variant==3)ro::resetPool(f.c,f.pool,false,VK_SUCCESS);
        ro::begin(f.c,f.command,&f.begin,VK_SUCCESS);ro::finishSubmit(f.c,ticket,VK_NULL_HANDLE,VK_SUCCESS);
        require(f.facts().rpRecordingGeneration>generation&&f.facts().successfulSubmissionCount==0&&f.facts().recording&&ro::stats(f.c).recordingFactsStaleSubmissions==1,"reset/free/pool reset/ABA after capture cannot relabel old outcome as new recording");
        if(variant==2)require(f.facts().poolGeneration>poolGeneration,"pool numeric handle reuse gets fresh identity");
    }
    RecordingFixture first,second;require(first.facts().contextIncarnation!=second.facts().contextIncarnation,"same numeric VkDevice contexts have distinct incarnations");
    ro::end(first.c,first.command,VK_SUCCESS);const auto ticket=first.ticket();ro::finishSubmit(second.c,ticket,VK_NULL_HANDLE,VK_SUCCESS);
    require(second.facts().recording&&!second.facts().successfulSubmissionCount&&ro::stats(second.c).recordingFactsStaleSubmissions==1,"cross-context ticket cannot relabel matching numeric CB/generation");
    ro::finishSubmit(first.c,ticket,VK_NULL_HANDLE,VK_SUCCESS);require(first.facts().successfulSubmissionCount==1,"original context consumes its own captured ticket");
    const auto saved=first.facts();ro::finishSubmit(first.c,ticket,VK_NULL_HANDLE,VK_SUCCESS);require(first.facts().successfulSubmissionCount==saved.successfulSubmissionCount,"duplicate finish cannot count a second successful original call");
    RecordingFixture noHeap;allocations=0;countAllocations=true;
    auto facts=ro::dispatchPoint(noHeap.c,noHeap.command);ro::explicitBarrier(noHeap.c,noHeap.command);ro::end(noHeap.c,noHeap.command,VK_SUCCESS);noHeap.finish();
    ro::reset(noHeap.c,noHeap.command,VK_SUCCESS);ro::begin(noHeap.c,noHeap.command,&noHeap.begin,VK_SUCCESS);facts=noHeap.facts();countAllocations=false;
    require(facts.supported()&&allocations==0,"owned recording facts and ticket path allocate no memory after context construction");
    RecordingFixture bounded;for(std::uint64_t n=1;n<=128;++n)ro::poolCreated(bounded.c,handle<VkCommandPool>(0x5000+n),&bounded.poolInfo,VK_SUCCESS);
    require(has(bounded.facts(),CR::Capacity)&&!bounded.facts().stateKnown,"bounded pool capacity failure closes recording support");
    RecordingFixture parallel(false);VkCommandBuffer other=handle<VkCommandBuffer>(0x904);ro::allocate(parallel.c,&parallel.alloc,&other,VK_SUCCESS);
    std::thread a([&]{ro::begin(parallel.c,parallel.command,&parallel.begin,VK_SUCCESS);for(unsigned n=0;n<300;++n)(void)ro::dispatchPoint(parallel.c,parallel.command);});
    std::thread b([&]{ro::begin(parallel.c,other,&parallel.begin,VK_SUCCESS);for(unsigned n=0;n<300;++n)(void)ro::dispatchPoint(parallel.c,other);});a.join();b.join();
    facts=parallel.facts();const auto otherFacts=ro::recordingFacts(parallel.c,other);
    require(facts.supported()&&otherFacts.supported()&&facts.commonRecordOrder==300&&otherFacts.commonRecordOrder==300&&facts.rpRecordingGeneration!=otherFacts.rpRecordingGeneration,"legally parallel different CBs preserve separate common clocks and fresh global keys");
    RecordingFixture lost;VkCommandBuffer unaffected=handle<VkCommandBuffer>(0x905);ro::allocate(lost.c,&lost.alloc,&unaffected,VK_SUCCESS);ro::begin(lost.c,unaffected,&lost.begin,VK_SUCCESS);
    ro::end(lost.c,lost.command,VK_SUCCESS);lost.finish(VK_ERROR_DEVICE_LOST);const auto lostFacts=ro::recordingFacts(lost.c,unaffected);
    require(has(lostFacts,CR::DeviceLost)&&!lostFacts.stateKnown&&!lostFacts.recording&&!lostFacts.executable&&!lostFacts.scopeKnown,"device lost on A submit closes all facts for otherwise unsubmitted B");
}
// Handles chosen to share one hint slot: the hint is only a candidate, so every
// lookup still resolves to the command's own slot through free and reuse.
void commandHintCollisions(){
    ++cases;RecordingFixture f(false);std::array<VkCommandBuffer,40> buffers{};
    for(std::size_t n=0;n<buffers.size();++n)buffers[n]=handle<VkCommandBuffer>(0x1000+(std::uint64_t(n+1)<<40)); // equal hint index
    ro::free(f.c,1,&f.command);for(const auto b:buffers)ro::allocate(f.c,&f.alloc,&b,VK_SUCCESS);
    std::array<std::uint64_t,buffers.size()> generations{};
    for(std::size_t n=0;n<buffers.size();++n){ro::begin(f.c,buffers[n],&f.begin,VK_SUCCESS);generations[n]=ro::recordingFacts(f.c,buffers[n]).rpRecordingGeneration;}
    const auto verify=[&](const char* step){for(std::size_t round=0;round<3;++round)for(std::size_t n=0;n<buffers.size();++n){
        const auto x=ro::recordingFacts(f.c,buffers[n]);
        require(x.command==reinterpret_cast<std::uint64_t>(buffers[n])&&x.rpRecordingGeneration==generations[n]&&x.recording,step);}};
    verify("colliding hints resolve each command to its own recording");
    const auto scans=ro::recordingFactsCounters(f.c).lookupScans;require(scans>0,"collisions fall back to the scan");
    for(std::size_t n=0;n<buffers.size();n+=2)ro::free(f.c,1,&buffers[n]);
    for(std::size_t n=0;n<buffers.size();n+=2){buffers[n]=handle<VkCommandBuffer>(0x2000+(std::uint64_t(n+1)<<40));ro::allocate(f.c,&f.alloc,&buffers[n],VK_SUCCESS);
        ro::begin(f.c,buffers[n],&f.begin,VK_SUCCESS);generations[n]=ro::recordingFacts(f.c,buffers[n]).rpRecordingGeneration;}
    verify("freed and reused slots never answer for a stale hint");
    for(std::size_t n=0;n<buffers.size();n+=2){const auto old=handle<VkCommandBuffer>(0x1000+(std::uint64_t(n+1)<<40));
        require(!ro::recordingFacts(f.c,old).allocationKnown,"a freed handle is not found through its old hint");}
    const auto counters=ro::recordingFactsCounters(f.c);require(counters.queries>=buffers.size()*6&&counters.lockContended==0,"query counters advance single-threaded without contention");
}
void sourceFactsBegin(Fixture& f){
    ro::free(f.c,1,&f.command);VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=0;
    ro::poolCreated(f.c,f.pool,&pool,VK_SUCCESS);ro::allocate(f.c,&f.allocateInfo,&f.command,VK_SUCCESS);
    f.commandInfo.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;ro::begin(f.c,f.command,&f.commandInfo,VK_SUCCESS);
}
VkImageMemoryBarrier recordedBarrier(Fixture& f,VkImageLayout old,VkImageLayout next){
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=f.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.oldLayout=old;b.newLayout=next;b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;return b;
}
void recordBarrier(Fixture& f,const VkImageMemoryBarrier& b){ro::explicitImageBarriers(f.c,f.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b);}
using LI=xess_vk::RecordedLayoutIssue;
void recordedSourceLayoutAdapter(){
    ++cases;Fixture f(false);f.attachment.initialLayout=f.attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;f.objects();sourceFactsBegin(f);
    recordBarrier(f,recordedBarrier(f,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL));ro::beginRenderPass(f.c,f.command,&f.beginInfo,VK_SUBPASS_CONTENTS_INLINE);
    ro::endRenderPass(f.c,f.command);recordBarrier(f,recordedBarrier(f,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    ro::select(f.c,ro::Role::RealColor,f.record);const auto facts=ro::dispatchPoint(f.c,f.command);const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    allocations=0;countAllocations=true;SetLastError(0xbeef);const auto result=ro::resolveSourceAt(f.c,facts,{10,1},range,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);countAllocations=false;
    require(result.recordedLayoutResolved&&result.recordedPrefixSupported&&result.device==1&&result.contextIncarnation==facts.contextIncarnation&&result.prefixOrder==5
        &&result.lastEvent.order==4&&!result.copyPermitted&&!result.ownerVerified&&!allocations&&GetLastError()==0xbeef,"adapter resolves late-selected current prefix and preserves LastError/zero allocations");
    for(unsigned variant=0;variant<4;++variant){auto wrong=facts;if(variant==0)++wrong.contextIncarnation;if(variant==1)++wrong.rpRecordingGeneration;if(variant==2)--wrong.commonRecordOrder;if(variant==3)wrong.inDynamicRendering=true;
        const auto r=ro::resolveSourceAt(f.c,wrong,{10,1},range,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        require(!r.recordedLayoutResolved&&r.issues,"context/CB generation/current point/outside validation refuses forged or stale query");}
    recordBarrier(f,recordedBarrier(f,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL));
    require(ro::resolveSourceAt(f.c,facts,{10,1},range,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL).issues&xess_vk::bit(LI::PrefixMismatch),"later common event prevents masquerading historical prefix as current query");
}
struct InventoryQueryProbe {
    xess_vk::image_inventory::Inventory<16> inventory;ro::Context* c{};VkCommandBuffer command{};bool invalidate{};std::size_t reads{};
};
xess_vk::image_inventory::Snapshot queryInventory(void* user,std::uint64_t image) noexcept {
    auto& p=*static_cast<InventoryQueryProbe*>(user);++p.reads;if(p.c)(void)ro::stats(p.c);
    if(p.invalidate){p.invalidate=false;ro::reset(p.c,p.command,VK_SUCCESS);}
    try{return p.inventory.lookup(image);}catch(...){return {};}
}
void recordedSourceCommonInventory(){
    ++cases;Fixture f(false);InventoryQueryProbe probe;ro::destroy(f.c);f.c=ro::create(f.device,nullptr,{queryInventory,&probe});require(f.c!=nullptr,"inventory-backed RP context");probe.c=f.c;probe.command=f.command;
    probe.inventory.created(10,f.record.description);auto unrelated=f.record.description;unrelated.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;probe.inventory.created(11,unrelated);
    f.attachment.initialLayout=f.attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;f.objects();sourceFactsBegin(f);
    auto extra=f.imageInfo;extra.usage=unrelated.usage;const auto extraRecord=probe.inventory.lookup(11).record;ro::imageCreated(f.c,&extra,handle<VkImage>(11),VK_SUCCESS,&extraRecord);
    auto b=recordedBarrier(f,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);auto other=b;other.image=handle<VkImage>(11);std::array<VkImageMemoryBarrier,2> barriers{other,b};
    ro::explicitImageBarriers(f.c,f.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,2,barriers.data());
    ro::beginRenderPass(f.c,f.command,&f.beginInfo,VK_SUBPASS_CONTENTS_INLINE);ro::endRenderPass(f.c,f.command);recordBarrier(f,recordedBarrier(f,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    auto facts=ro::dispatchPoint(f.c,f.command);const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    const auto result=ro::resolveSourceAt(f.c,facts,{10,1},range,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    require(result.recordedLayoutResolved&&result.retainedExplicitImages==3&&ro::stats(f.c).ignoredNonAttachmentImages==1&&probe.reads>=7,"common inventory proves unrelated nonattachment identity without changing old RP registration or rejection");
    other.srcQueueFamilyIndex=3;other.dstQueueFamilyIndex=4;recordBarrier(f,other);facts=ro::dispatchPoint(f.c,f.command);
    require(ro::resolveSourceAt(f.c,facts,{10,1},range,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL).recordedLayoutResolved,"ordinary ownership operation on an exact different image is excluded only from layout query, without owner/alias proof");
    probe.inventory.destroying(11);probe.inventory.created(11,unrelated);const auto stale=ro::resolveSourceAt(f.c,facts,{10,1},range,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    require(!stale.recordedLayoutResolved&&(stale.issues&xess_vk::bit(LI::StaleObject)),"unrelated retained exact image generation is rechecked and ABA refused");
    probe.invalidate=true;const auto raced=ro::resolveSourceAt(f.c,facts,{10,1},range,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    require(!raced.recordedLayoutResolved&&(raced.issues&xess_vk::bit(LI::IdentityMismatch)),"RP mutex is released during inventory lookup and reset prevents old-point relabelling");
    ro::begin(f.c,f.command,&f.commandInfo,VK_SUCCESS);recordBarrier(f,recordedBarrier(f,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    other.pNext=&f.beginInfo;recordBarrier(f,other);facts=ro::dispatchPoint(f.c,f.command);
    const auto opaque=ro::resolveSourceAt(f.c,facts,{10,1},range,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    require(!opaque.recordedLayoutResolved&&(opaque.issues&xess_vk::bit(LI::UnsupportedPNext)),"unread pNext control stays prefix-sticky even for an exact unrelated image");
}
}
int main(){try{
    happyAndSinkBoundary();diagnosticPassSnapshot();diagnosticTailAfterRejectedPass();failureAndUnsupportedLocality();staleAndDestroyed();incompleteAndSecondary();ticketsAndSubmit2();emptySubmissions();submit2SemaphoreGuards();quotaAndFiltering();registrationGuardsAndRetention();noAllocAndCapacity();rejectionDiagnostics();diagnosticAggregation();
    projectionBindingAndEvidence();projectionLongRecordingJournal();projectionIsolationAndUnused();projectionTransactionsAndLifetime();projectionQuotaAndAllocation();swapchainIdentityAdapter();
    actualRecordingFactsAndScope();recordingFailureAndSupportFacts();recordingIdentityTicketsAndNoAllocation();
    commandHintCollisions();
    recordedSourceLayoutAdapter();recordedSourceCommonInventory();
    std::cout<<"{\"stage\":\"pure-render-pass-observer-adapter\",\"testPassed\":true,\"cases\":"<<cases<<",\"checks\":"<<checks
        <<",\"hotPathAllocations\":"<<allocations<<",\"vulkanLoaded\":false,\"gpuTouched\":false,\"gameTouched\":false}\n";
}catch(const std::exception& e){countAllocations=false;std::cerr<<"RP adapter test failed: "<<e.what()<<'\n';return 1;}}
