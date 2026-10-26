#include "candidate_sync_observer.hpp"
#include <windows.h>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xess_vk;
unsigned checks{};std::vector<std::string> logs;
void sink(const char* s){logs.emplace_back(s);}
void throwingSink(const char*){throw std::runtime_error("fixture sink failure");}
void require(bool test,const char* reason){++checks;if(!test)throw std::runtime_error(reason);}
template<class T>T h(std::uint64_t value){return reinterpret_cast<T>(value);}
using Owned=std::unique_ptr<candidate_sync::Context,decltype(&candidate_sync::destroy)>;
const VkCommandBuffer cb=h<VkCommandBuffer>(0x100);
const VkQueue queue=h<VkQueue>(0x200);
const VkCommandPool pool=h<VkCommandPool>(0x300);
const VkFence fence=h<VkFence>(0x400);
image_inventory::Record record(std::uint64_t image,std::uint64_t generation,candidate_sync::Role role) {
    image_inventory::Record r;r.image=image;r.generation=generation;auto& d=r.description;
    d.format=role==candidate_sync::Role::Motion?83:role==candidate_sync::Role::Depth?100:37;
    d.width=1920;d.height=1080;d.depth=d.mipLevels=d.arrayLayers=d.samples=1;d.usage=159;
    d.tiling=VK_IMAGE_TILING_OPTIMAL;d.sharingMode=VK_SHARING_MODE_EXCLUSIVE;return r;
}
Owned create(candidate_sync::Sink output=sink) {
    Owned c(candidate_sync::create(h<VkDevice>(1),true,output),candidate_sync::destroy);
    require(c!=nullptr,"context constructed");candidate_sync::queue(c.get(),queue,3);candidate_sync::pool(c.get(),pool,3);
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};info.commandPool=pool;
    info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;info.commandBufferCount=1;candidate_sync::allocate(c.get(),&info,&cb);return c;
}
void begin(candidate_sync::Context* c) {
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};candidate_sync::begin(c,cb,&info,VK_SUCCESS);
}
void barrier(candidate_sync::Context* c,std::uint64_t image,bool sync2=false) {
    if(!sync2){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=h<VkImage>(image);
        b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        candidate_sync::barriers(c,cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&b);
    }else {VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.image=h<VkImage>(image);
        b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;b.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask=VK_ACCESS_2_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};info.imageMemoryBarrierCount=1;info.pImageMemoryBarriers=&b;
        candidate_sync::barriers2(c,cb,&info);}
}
void submit(candidate_sync::Context* c,bool sync2=false,VkResult result=VK_SUCCESS) {
    candidate_sync::end(c,cb,VK_SUCCESS);const auto ticket=candidate_sync::beginSubmit(c,queue);
    if(!sync2){VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&cb;
        candidate_sync::submit(c,ticket,1,&info,fence,result);
    }else {VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};command.commandBuffer=cb;command.deviceMask=1;
        VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};info.commandBufferInfoCount=1;info.pCommandBufferInfos=&command;
        candidate_sync::submit2(c,ticket,1,&info,fence,result);}
}
void noClaims(const image_sync::Evidence& e){require(!e.copyPermitted&&!e.gpuComplete&&!e.lastWriterKnown&&!e.resourceLifetimePinned&&!e.submittedLayoutKnown,"no copy/completion/layout authorization");}
std::size_t eventCount(const char* event){std::size_t n=0;for(const auto& s:logs)if(s.find(event)!=std::string::npos)++n;return n;}
void allRolesAndBothSubmits(){
    logs.clear();auto c=create();
    for(unsigned i=0;i<3;++i){auto role=static_cast<candidate_sync::Role>(i);auto n=candidate_sync::select(c.get(),role,record(10+i,i+1,role));
        require(n.accepted&&n.emit,"three exact role formats accepted");candidate_sync::reportSelection(n,sink);}
    begin(c.get());for(unsigned i=0;i<3;++i)barrier(c.get(),10+i,i==1);submit(c.get());
    for(unsigned i=0;i<3;++i){auto e=candidate_sync::evidence(c.get(),{10+i,i+1});
        require(e.barrierSubmitted&&e.queue==0x200&&e.fence==0x400&&e.commandGeneration==1,"actual primary submit provenance");noClaims(e);}
    require(eventCount("image_sync_submit_evidence")==3,"one detail per role");
    begin(c.get());barrier(c.get(),11,true);submit(c.get(),true);
    auto e=candidate_sync::evidence(c.get(),{11,2});require(e.submissionSerial==2&&e.commandGeneration==2,"submit2 generation retained");
    require(eventCount("image_sync_submit_evidence")==3,"same selection second submit is sampled out");
}
void replacementAndRetirement(){
    logs.clear();auto c=create();using R=candidate_sync::Role;
    candidate_sync::select(c.get(),R::Motion,record(10,1,R::Motion));begin(c.get());barrier(c.get(),10);
    candidate_sync::select(c.get(),R::Motion,record(11,2,R::Motion));
    candidate_sync::select(c.get(),R::Motion,record(12,3,R::Motion));submit(c.get());
    require(!candidate_sync::evidence(c.get(),{10,1}).selected,"third candidate retires old observation only");
    require(candidate_sync::stats(c.get()).retiredOperationsSkipped==1,"retired command operation skipped");
    require((candidate_sync::stats(c.get()).issues&image_sync::bit(image_sync::Issue::StaleImageGeneration))==0,"retirement does not contaminate new candidate");
    require(candidate_sync::stats(c.get()).selected==2,"current and previous bounded");
    for(std::uint64_t i=13;i<400;++i){auto n=candidate_sync::select(c.get(),R::Motion,record(i,i,R::Motion));candidate_sync::reportSelection(n,sink);}
    require(candidate_sync::stats(c.get()).selected==2,"startup replacement cannot fill candidate quota");
    require(eventCount("image_sync_candidate")<=256,"candidate details bounded independently of updates");
    require(candidate_sync::evidence(c.get(),{399,399}).selected,"candidate updates continue after detail cap");
    candidate_sync::destroyingImage(c.get(),399);require(!candidate_sync::evidence(c.get(),{399,399}).selected,"real destroy retires current image");
    auto bad=record(500,500,R::Depth);bad.description.format=130;
    require(!candidate_sync::select(c.get(),R::Depth,bad).accepted,"D32S8 not silently R32F");
    candidate_sync::retire(c.get());require(!candidate_sync::select(c.get(),R::Motion,record(501,501,R::Motion)).accepted,"retirement sticky");
}
void lateSelectionAndFailures(){
    auto c=create();begin(c.get());using R=candidate_sync::Role;
    candidate_sync::select(c.get(),R::RealColor,record(10,1,R::RealColor));barrier(c.get(),10);submit(c.get());
    auto e=candidate_sync::evidence(c.get(),{10,1});require((e.lastRecordingIssues&image_sync::bit(image_sync::Issue::NotSelectedAtBegin))!=0,"same recording late selection explicit");
    begin(c.get());barrier(c.get(),10);submit(c.get());e=candidate_sync::evidence(c.get(),{10,1});
    require((e.lastRecordingIssues&image_sync::bit(image_sync::Issue::NotSelectedAtBegin))==0,"later recording has selection before begin");
    const auto previous=e.submissionSerial;begin(c.get());barrier(c.get(),10);submit(c.get(),false,VK_ERROR_OUT_OF_HOST_MEMORY);
    require(candidate_sync::evidence(c.get(),{10,1}).submissionSerial==previous,"failed submit does not advance evidence");
    const auto a=candidate_sync::beginSubmit(c.get(),queue),b=candidate_sync::beginSubmit(c.get(),queue);
    candidate_sync::submit(c.get(),b,0,nullptr,VK_NULL_HANDLE,VK_SUCCESS);candidate_sync::submit(c.get(),a,0,nullptr,VK_NULL_HANDLE,VK_SUCCESS);
    require((candidate_sync::stats(c.get()).issues&image_sync::bit(image_sync::Issue::SubmitOrderAmbiguous))!=0,"overlap refuses host return-order as queue order");
}
void boundsAndErrorPreservation(){
    auto c=create(throwingSink);using R=candidate_sync::Role;
    SetLastError(7123);auto n=candidate_sync::select(c.get(),R::Depth,record(10,1,R::Depth));candidate_sync::reportSelection(n,throwingSink);
    require(GetLastError()==7123,"diagnostic sink preserves LastError and contains exceptions");
    begin(c.get());barrier(c.get(),10);submit(c.get());noClaims(candidate_sync::evidence(c.get(),{10,1}));
    const auto ticket=candidate_sync::beginSubmit(c.get(),queue);candidate_sync::submit(c.get(),ticket,UINT32_MAX,nullptr,fence,VK_SUCCESS);
    require((candidate_sync::stats(c.get()).issues&image_sync::bit(image_sync::Issue::Capacity))!=0,"extreme count rejected without reading array");
    candidate_sync::summary(c.get());
}
}
int main(int argc,char** argv){try{
    if(argc>2||(argc==2&&std::string(argv[1])!="--emit-jsonl"))throw std::runtime_error("only --emit-jsonl is supported");
    allRolesAndBothSubmits();replacementAndRetirement();lateSelectionAndFailures();boundsAndErrorPreservation();
    if(argc==2)for(const auto& line:logs)std::cout<<line<<'\n';
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"vulkanCalls\":0,\"copyPermissionGranted\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
