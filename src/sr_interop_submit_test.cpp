#include "sr_interop_submit.hpp"
#include <array>
#include <iostream>
using namespace xess_vk::sr_interop;
template<class T>T handle(std::uintptr_t n){return reinterpret_cast<T>(n);}
void check(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
int main(){try{
    const auto a=handle<VkCommandBuffer>(1),b=handle<VkCommandBuffer>(2),c=handle<VkCommandBuffer>(3),d=handle<VkCommandBuffer>(4);
    const auto sem=handle<VkSemaphore>(5),app=handle<VkSemaphore>(6),other=handle<VkSemaphore>(7);
    Batch batch;batch.waits={{app,100,VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT}};batch.signals={{app,101}};batch.commands={{a,1},{b,1},{c,1}};
    const Handoff first{b,d,sem,1,2,3};auto p=plan(std::span(&batch,1),std::span(&first,1));
    check(p.size()==2&&p[0].commands.size()==2&&p[1].commands[0].buffer==d&&p[1].commands[1].buffer==c,"Commands must continue after the exact split");
    check(p[0].waits[0].semaphore==app&&p[0].waits[0].value==100&&p[0].waits[0].stage==VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,"App wait values and stages preserved");
    check(p[0].signals[0].value==1&&p[0].runAfter==0&&p[1].waits[0].value==2,"D3D12 ready/done dependency");
    check(p[1].signals.size()==2&&p[1].signals[0].value==101&&p[1].signals[1].value==3,"App signal and resource retirement must follow continuation");
    const std::array handoffs{first,Handoff{c,handle<VkCommandBuffer>(8),other,4,5,6}};
    p=plan(std::span(&batch,1),handoffs);check(p.size()==3&&p[1].commands[0].buffer==d&&p[1].commands[1].buffer==c&&p[2].signals.size()==3,"Multiple cameras preserve dependency ordering");
    auto duplicate=batch;duplicate.commands.push_back({b,1});bool rejected=false;
    try{(void)plan(std::span(&duplicate,1),std::span(&first,1));}catch(const std::invalid_argument&){rejected=true;}check(rejected,"Duplicate one-time recording rejected before submission");
    VkPipelineStageFlags stages=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;std::uint64_t wait=44,signal=45;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,nullptr,1,&wait,1,&signal};
    VkSubmitInfo legacy{VK_STRUCTURE_TYPE_SUBMIT_INFO};legacy.pNext=&timeline;legacy.waitSemaphoreCount=1;legacy.pWaitSemaphores=&app;legacy.pWaitDstStageMask=&stages;
    legacy.signalSemaphoreCount=1;legacy.pSignalSemaphores=&app;legacy.commandBufferCount=1;legacy.pCommandBuffers=&b;
    const auto normalized=normalize(std::span(&legacy,1));check(normalized[0].waits[0].value==44&&normalized[0].signals[0].value==45&&timeline.pWaitSemaphoreValues==&wait,"Caller timeline not mutated");
    VkBaseInStructure unknown{VK_STRUCTURE_TYPE_APPLICATION_INFO};legacy.pNext=&unknown;rejected=false;
    try{(void)normalize(std::span(&legacy,1));}catch(const std::invalid_argument&){rejected=true;}check(rejected,"Unknown submit extensions are not silently dropped");
    VkSemaphoreSubmitInfo wait2{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};wait2.semaphore=app;wait2.value=62;wait2.stageMask=VK_PIPELINE_STAGE_2_COPY_BIT;
    VkCommandBufferSubmitInfo command2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};command2.commandBuffer=b;command2.deviceMask=1;
    VkSubmitInfo2 submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};submit2.waitSemaphoreInfoCount=1;submit2.pWaitSemaphoreInfos=&wait2;submit2.commandBufferInfoCount=1;submit2.pCommandBufferInfos=&command2;
    const auto modern=normalize(std::span(&submit2,1));check(modern[0].waits[0].stage==VK_PIPELINE_STAGE_2_COPY_BIT&&modern[0].waits[0].value==62,"Submit2 64-bit stages and timeline preserved");
    std::cout<<"SR interop submit checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
