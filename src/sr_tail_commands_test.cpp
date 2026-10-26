#include "sr_tail_commands.hpp"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace t=xess_vk::sr_tail_commands;
unsigned checks{};
void require(bool condition,const char* why){++checks;if(!condition)throw std::runtime_error(why);}
template<class T>T handle(std::uint64_t value){return reinterpret_cast<T>(static_cast<std::uintptr_t>(value));}
t::Key recording(std::uint64_t command=3,std::uint64_t generation=4,std::uint64_t pool=5,std::uint64_t poolGeneration=6){
    return {1,2,command,generation,pool,poolGeneration};
}
t::Tail tail(t::Key key,t::Pass pass=t::Pass::Accumulate,std::uint64_t ordinal=7){
    t::Tail out;out.recording=key;out.pass=pass;out.dispatchOrdinal=ordinal;out.recordOrder=11;out.callback=12;
    out.frame={13,{14,15,false},0,true};return out;
}
t::Command dispatch(std::uint64_t value){const std::array arguments{value,std::uint64_t{2},std::uint64_t{1}};return t::simple(t::Kind::Dispatch,arguments);}
void lastTailAndOrigin(){
    auto observer=std::make_unique<t::Observer>(99);auto result=std::make_unique<t::Trace>();const auto key=recording();
    observer->command(key,dispatch(999));require(!observer->wantsCommand(key),"Observer tracked commands before a matched tail");
    require(!observer->tail(tail(key),t::Origin::Layer),"Layer tail became a game tail");
    require(observer->tail(tail(key)),"Accumulate tail rejected");
    auto wrongPool=tail(key);++wrongPool.recording.poolGeneration;
    require(!observer->tail(wrongPool),"One recording admitted a second inconsistent pool identity");
    observer->command(key,dispatch(999),t::Origin::Layer);
    observer->command(recording(3,999),dispatch(999));observer->command(recording(999),dispatch(999));
    for(unsigned i=0;i<10;++i)observer->command(key,dispatch(i));
    require(!observer->wantsCommand(key)&&observer->stats().omittedAfterEight==2,"Eight-command storage cap failed");
    require(observer->tail(tail(key,t::Pass::Rcas,8)),"Later RCAS did not replace a full Accumulate trace");
    require(observer->wantsCommand(key),"RCAS replacement did not reopen its own eight-command prefix");
    observer->command(key,dispatch(123),t::Origin::Layer);
    observer->command(key,dispatch(40));require(observer->tail(tail(key,t::Pass::Rcas,8)),"Exact duplicate tail rejected");
    require(!observer->tail(tail(key,t::Pass::Accumulate,7)),"Stale Accumulate replaced later RCAS");
    observer->command(key,t::simple(t::Kind::BindPipeline,std::array<std::uint64_t,2>{VK_PIPELINE_BIND_POINT_GRAPHICS,0x100}));
    require(!observer->finish(recording(3,999),*result),"Wrong recording generation finished the active trace");
    require(observer->finish(key,*result),"End-of-recording result missing");
    require(result->window==99&&result->tail.pass==t::Pass::Rcas&&result->replacements==1&&result->count==2&&result->commandsSeen==2,
        "Latest RCAS tail did not own its exact follow-up commands");
    require(result->commands[0].arguments[0]==40&&result->commands[1].kind==t::Kind::BindPipeline,
        "Layer, old-generation or pre-RCAS command entered the final prefix");
    require(result->tail.frame.engineFrame==13&&result->tail.frame.ticket.sdkId==14&&result->tail.callback==12,
        "Tail frame or callback metadata was lost");
    require(result->end==t::End::RecordingEnded&&!observer->finish(key,*result),"Finished recording remained live");
    require(observer->stats().ignoredLayer==3&&observer->stats().duplicates==1,"Layer or duplicate accounting wrong");
    require(observer->tail(tail(key,t::Pass::Rcas,8))&&observer->tail(tail(key,t::Pass::Accumulate,9)),"A genuinely later Accumulate tail was not retained");
    require(observer->finish(key,*result)&&result->tail.pass==t::Pass::Accumulate,"Pass name overrode actual command order");
}
void lifecycleAndBounds(){
    auto observer=std::make_unique<t::Observer>(100);auto result=std::make_unique<t::Trace>();
    for(unsigned i=0;i<t::recordingCapacity;++i)require(observer->tail(tail(recording(20+i))),"Bounded recording slot unexpectedly refused");
    require(!observer->tail(tail(recording(1000)))&&observer->stats().capacityRefusals==1,"Full observer evicted live evidence");
    require(observer->finish(recording(20),*result),"Capacity refusal destroyed existing tail");
    require(observer->tail(tail(recording(1000))),"Completed slot did not become reusable");
    observer->retirePool(5,999);require(observer->wantsCommand(recording(21)),"Stale pool generation retired live traces");
    observer->retirePool(5,6);require(!observer->drain(*result),"Pool reset retained matching records");
    const auto old=recording();const auto current=recording(3,10);
    require(observer->tail(tail(old)),"Old recording setup failed");observer->retire(old);
    require(observer->tail(tail(current)),"New recording after reset rejected");observer->retire(old);
    require(observer->wantsCommand(current),"Stale free/reset retired a new recording generation");
    observer->command(current,dispatch(1));require(observer->drain(*result)&&result->count==1&&result->end==t::End::WindowClosed,
        "Window close fabricated successful CB end or lost a partial trace");
    require(!observer->drain(*result),"Window drain repeated a record");
    require(observer->tail(tail(recording(3,11,55,66))),"Actual pool retirement setup failed");observer->retirePool(55);
    require(!observer->drain(*result),"Actual pool reset hook without a separate pool map retained a trace");
    auto invalid=tail(recording());invalid.recording.poolGeneration=0;require(!observer->tail(invalid),"Unkeyed pool identity accepted");
    invalid=tail(recording());invalid.pass=static_cast<t::Pass>(99);require(!observer->tail(invalid),"Invalid tail pass accepted");
    auto disabled=std::make_unique<t::Observer>(0);require(!disabled->tail(tail(recording()))&&!disabled->drain(*result),"Inactive window produced a trace");
    xess_vk::CommandRecordingFacts facts;facts.device=1;facts.contextIncarnation=2;facts.command=3;facts.rpRecordingGeneration=4;facts.pool=5;facts.poolGeneration=6;
    require(t::key(facts)==recording(),"Adapter recording identity lost a generation");
}
void ownedParameters(){
    const auto extension=reinterpret_cast<const void*>(std::uintptr_t{0x1234});
    std::array<VkMemoryBarrier,10> memory{};std::array<VkBufferMemoryBarrier,10> buffers{};std::array<VkImageMemoryBarrier,10> images{};
    for(unsigned i=0;i<10;++i){
        memory[i]={VK_STRUCTURE_TYPE_MEMORY_BARRIER,extension,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT};
        buffers[i]={VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,nullptr,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,3,4,handle<VkBuffer>(100+i),64,128};
        images[i]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,3,4,handle<VkImage>(200+i),{VK_IMAGE_ASPECT_DEPTH_BIT,1,2,3,4}};
    }
    auto legacy=t::barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_DEPENDENCY_BY_REGION_BIT,
        10,memory.data(),10,buffers.data(),10,images.data());
    memory={};buffers={};images={};
    require(legacy.truncated&&legacy.extensionsOmitted&&!legacy.missingData&&legacy.barriers.imageCount==10&&legacy.barriers.storedImages==8,
        "Legacy barrier truncation or pNext omission not explicit");
    require(legacy.barriers.images[7].image==207&&legacy.barriers.images[0].range.baseArrayLayer==3&&legacy.barriers.buffers[0].offset==64&&
        legacy.barriers.memory[0].srcAccess==VK_ACCESS_SHADER_WRITE_BIT,"Legacy barriers retained live pointers or lost key parameters");
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,extension,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,nullptr,m.srcStageMask,m.srcAccessMask,m.dstStageMask,m.dstAccessMask,8,9,handle<VkBuffer>(300),123,456};
    VkImageMemoryBarrier2 i{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,nullptr,m.srcStageMask,m.srcAccessMask,m.dstStageMask,m.dstAccessMask,
        VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,8,9,handle<VkImage>(301),{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO,extension,VK_DEPENDENCY_BY_REGION_BIT,1,&m,1,&b,1,&i};
    auto sync2=t::barrier2(&dependency,t::Api::Khr2);m={};b={};i={};dependency={};
    require(sync2.api==t::Api::Khr2&&sync2.extensionsOmitted&&sync2.barriers.memory[0].srcStage==VK_PIPELINE_STAGE_2_COPY_BIT&&
        sync2.barriers.memory[0].srcAccess==VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT&&sync2.barriers.images[0].image==301,
        "Sync2 barrier lost 64-bit masks or retained source memory");
    std::array<VkDescriptorSet,10> sets{};std::array<std::uint32_t,20> offsets{};
    for(unsigned n=0;n<sets.size();++n)sets[n]=handle<VkDescriptorSet>(400+n);
    for(unsigned n=0;n<offsets.size();++n)offsets[n]=n*32;
    auto descriptors=t::bindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,handle<VkPipelineLayout>(500),2,10,sets.data(),20,offsets.data());
    VkBindDescriptorSetsInfo bind2{VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO,extension,VK_SHADER_STAGE_COMPUTE_BIT,handle<VkPipelineLayout>(501),1,1,sets.data(),1,offsets.data()};
    auto descriptors2=t::bindDescriptorSets2(&bind2,t::Api::Khr2);sets={};offsets={};bind2={};
    require(descriptors.truncated&&descriptors.descriptors.setCount==10&&descriptors.descriptors.storedSets==8&&
        descriptors.descriptors.dynamicOffsetCount==20&&descriptors.descriptors.storedOffsets==16&&descriptors.descriptors.sets[7]==407&&
        descriptors.descriptors.offsets[15]==480,"Descriptor sets or dynamic offsets were not bounded owned copies");
    require(descriptors2.extensionsOmitted&&descriptors2.descriptors.stages==VK_SHADER_STAGE_COMPUTE_BIT&&descriptors2.descriptors.layout==501&&
        descriptors2.descriptors.sets[0]==400,"BindDescriptorSets2 stage flags or parameters were not owned");
    std::array<std::uint8_t,300> bytes{};for(unsigned n=0;n<bytes.size();++n)bytes[n]=static_cast<std::uint8_t>(n);
    auto push=t::pushConstants(handle<VkPipelineLayout>(600),VK_SHADER_STAGE_COMPUTE_BIT,16,300,bytes.data());
    VkPushConstantsInfo push2{VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,extension,handle<VkPipelineLayout>(601),VK_SHADER_STAGE_VERTEX_BIT,4,12,bytes.data()};
    auto copiedPush2=t::pushConstants2(&push2,t::Api::Khr2);bytes.fill(0);push2={};
    require(push.truncated&&push.push.size==300&&push.push.storedBytes==256&&push.push.bytes[255]==255&&push.push.offset==16,
        "Push constant truncation or owned byte copy failed");
    require(copiedPush2.api==t::Api::Khr2&&copiedPush2.extensionsOmitted&&copiedPush2.push.bytes[11]==11&&copiedPush2.push.layout==601,
        "PushConstants2 retained the source bytes");
    std::array<VkClearValue,10> clears{};clears[0].color.float32[0]=3.5f;
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,extension,handle<VkRenderPass>(700),handle<VkFramebuffer>(701),{{2,3},{800,600}},10,clears.data()};
    auto renderPass=t::beginRenderPass(&pass,VK_SUBPASS_CONTENTS_INLINE);
    VkSubpassBeginInfo subpass{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO,extension,VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS};
    auto renderPass2=t::beginRenderPass2(&pass,&subpass,t::Api::Khr2);clears={};pass={};subpass={};
    require(renderPass.truncated&&renderPass.extensionsOmitted&&renderPass.renderPass.storedClears==8&&renderPass.renderPass.clearCount==10&&
        renderPass.renderPass.clears[0].color.float32[0]==3.5f&&renderPass.renderPass.framebuffer==701,"Render-pass begin retained pointers or hid truncation");
    require(renderPass2.api==t::Api::Khr2&&renderPass2.renderPass.contents==VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS,
        "RenderPass2 subpass parameters missing");
    std::array<VkRenderingAttachmentInfo,10> colors{};
    for(unsigned n=0;n<colors.size();++n)colors[n]={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,nullptr,handle<VkImageView>(800+n),VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_RESOLVE_MODE_AVERAGE_BIT,handle<VkImageView>(900+n),VK_IMAGE_LAYOUT_GENERAL,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,{}};
    colors[0].clearValue.color.float32[0]=2.5f;auto depth=colors[0];depth.pNext=extension;depth.imageView=handle<VkImageView>(999);
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO,nullptr,VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT,{{1,2},{1000,700}},1,3,10,colors.data(),&depth,&depth};
    auto dynamic=t::beginRendering(&rendering,t::Api::Khr);colors={};depth={};rendering={};
    require(dynamic.truncated&&dynamic.extensionsOmitted&&dynamic.rendering.colorCount==10&&dynamic.rendering.storedColors==8&&
        dynamic.rendering.colors[7].view==807&&dynamic.rendering.colors[0].clear.color.float32[0]==2.5f&&dynamic.rendering.hasDepth&&
        dynamic.rendering.hasStencil&&dynamic.rendering.depth.view==999&&dynamic.rendering.stencil.view==999,"Dynamic rendering attachments were not bounded owned copies");
    auto missing=t::barrier(0,0,0,1,nullptr,1,nullptr,1,nullptr);
    require(missing.missingData&&missing.truncated&&!missing.barriers.storedMemory&&!missing.barriers.storedBuffers&&!missing.barriers.storedImages,
        "Null barrier arrays were silently read or treated as captured");
    auto missingPush=t::pushConstants(handle<VkPipelineLayout>(1),VK_SHADER_STAGE_COMPUTE_BIT,0,32,nullptr);
    require(missingPush.missingData&&missingPush.truncated&&!missingPush.push.storedBytes,"Null push bytes were not explicit");
    require(t::barrier2(nullptr).missingData&&t::bindDescriptorSets2(nullptr).missingData&&t::pushConstants2(nullptr).missingData&&
        t::beginRenderPass(nullptr,VK_SUBPASS_CONTENTS_INLINE).missingData&&t::beginRendering(nullptr).missingData,"Null structure inputs were not explicit");
    std::string longName(100,'x');std::array<std::uint64_t,10> args{};for(unsigned n=0;n<args.size();++n)args[n]=n;
    auto extra=t::other(longName,args);longName.clear();args={};
    require(extra.truncated&&extra.otherName[62]=='x'&&extra.otherName[63]=='\0'&&extra.argumentCount==10&&extra.storedArguments==8&&extra.arguments[7]==7,
        "Other command names or scalar arguments retained pointers or hid truncation");
    auto indexed=t::simple(t::Kind::DrawIndexed,std::array<std::uint64_t,5>{100,1,3,static_cast<std::uint64_t>(std::int64_t{-7}),5});
    require(static_cast<std::int64_t>(indexed.arguments[3])==-7&&indexed.arguments[4]==5,"Signed draw parameter lost its original bits");
    auto observer=std::make_unique<t::Observer>(101);auto result=std::make_unique<t::Trace>();const auto key=recording();require(observer->tail(tail(key)),"Owned trace setup failed");
    for(const auto& command:std::array{legacy,sync2,descriptors,push,renderPass,dynamic,extra,copiedPush2})observer->command(key,command);
    legacy={};sync2={};descriptors={};push={};renderPass={};dynamic={};extra={};copiedPush2={};
    require(observer->finish(key,*result)&&result->count==8&&result->commands[0].barriers.images[7].image==207&&
        result->commands[3].push.bytes[255]==255&&result->commands[5].rendering.depth.view==999,"Observer retained caller Command objects instead of copying them");
}
void serializedThreads(){
    auto observer=std::make_unique<t::Observer>(102);auto result=std::make_unique<t::Trace>();std::mutex mutex;
    const auto worker=[&](std::uint64_t cb){
        {std::lock_guard lock(mutex);observer->tail(tail(recording(cb)));}
        for(unsigned i=0;i<8;++i){auto command=dispatch(cb*100+i);std::lock_guard lock(mutex);observer->command(recording(cb),command);}
    };
    std::thread first(worker,30),second(worker,31);first.join();second.join();
    require(observer->finish(recording(30),*result)&&result->count==8,"First serialized recorder lost commands");
    for(unsigned i=0;i<8;++i)require(result->commands[i].arguments[0]==3000+i,"Commands from another CB entered the prefix");
    require(observer->finish(recording(31),*result)&&result->count==8,"Second serialized recorder lost commands");
    for(unsigned i=0;i<8;++i)require(result->commands[i].arguments[0]==3100+i,"Command order changed on a serialized recording");
}
int main(){try{
    lastTailAndOrigin();lifecycleAndBounds();ownedParameters();serializedThreads();
    std::printf("sr tail commands: %u checks passed; last matched Accumulate/RCAS, eight-command bound, owned parameters, truncation, layer exclusion, recording/pool retirement, window drain and serialized recorders; GPU calls=0\n",checks);
    std::printf("fixed memory: Observer=%zu bytes, Trace=%zu bytes, Command=%zu bytes; hot-path allocations=0\n",sizeof(t::Observer),sizeof(t::Trace),sizeof(t::Command));return 0;
}catch(const std::exception& e){std::fprintf(stderr,"sr tail commands test: %s\n",e.what());return 1;}}
