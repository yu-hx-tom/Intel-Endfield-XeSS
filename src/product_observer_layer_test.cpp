#include "vulkan_observer.cpp"
#include <iostream>
#include <stdexcept>

namespace {
void* marker=reinterpret_cast<void*>(0x123400);
VkDevice device=reinterpret_cast<VkDevice>(&marker);
VkCommandBuffer command=reinterpret_cast<VkCommandBuffer>(&marker);
const VkDependencyInfo* expected{};unsigned calls{},checks{};
void need(bool v){++checks;if(!v)throw std::runtime_error("product observer boundary check failed");}
void VKAPI_PTR barrier(VkCommandBuffer b,const VkDependencyInfo* i){need(b==command&&i==expected);++calls;SetLastError(4321);}
void VKAPI_PTR pipeline(VkCommandBuffer b,VkPipelineBindPoint point,VkPipeline p){need(b==command&&point==VK_PIPELINE_BIND_POINT_COMPUTE&&bits(p)==0x456);++calls;SetLastError(4321);}
}
int main(){try{
    need(!researchDiagnostics&&xessObserverResearchDiagnosticsV1()==0);
    SetEnvironmentVariableW(L"XESS_COMPUTE_OBSERVER",L"1");SetEnvironmentVariableW(L"XESS_RENDER_TAIL_DIAGNOSTIC",L"1");
    SetEnvironmentVariableW(L"XESS_RAWCOLOR_CAPTURE",L"1");
    need(!computeObserverRequested()&&!renderTailRequested()&&!rawCaptureRequested(true));
    DeviceState state{};state.device=device;state.key=marker;state.cmdPipelineBarrier2=barrier;state.cmdPipelineBarrier2Khr=barrier;state.cmdBindPipeline=pipeline;
    devices=&state;
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};expected=&info;
    vkCmdPipelineBarrier2(command,&info);need(calls==1&&GetLastError()==4321);
    vkCmdPipelineBarrier2KHR(command,&info);need(calls==2&&GetLastError()==4321);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,reinterpret_cast<VkPipeline>(0x456));need(calls==3&&GetLastError()==4321);
    need(!state.compute&&!state.candidateSync&&!state.rawProbe&&!state.renderTail);
    {RawOperation operation(&state,true,true);need(!operation.acquired&&!state.rawOperation&&!state.rawObservationMissed);}
    wchar_t folder[MAX_PATH]{},path[MAX_PATH]{};need(GetTempPathW(MAX_PATH,folder)&&GetTempFileNameW(folder,L"t54",0,path));
    logFile=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);need(logFile!=INVALID_HANDLE_VALUE);logBytes=0;
    xess_vk::presentation::FrameStatus frame;state.frameSpikeValid=true;
    unsigned id=0;
    for(const auto ms:{60,30,90,45,80,100}){++id;state.frameSpikePresentNs=steadyNowNs()-ms*1'000'000;recordFrameSpike(&state,frame,false,steadyNowNs()-1'000'000,id,id);}
    need(state.frameSpikeWindow.spikes==6&&state.frameSpikeWindow.intervals==6&&logBytes==0);
    flushFrameSpikes(&state,steadyNowNs(),"fixture");need(!state.frameSpikeWindow.spikes&&logBytes>0);
    LARGE_INTEGER zero{};need(SetFilePointerEx(logFile,zero,nullptr,FILE_BEGIN));std::string text(static_cast<std::size_t>(logBytes),'\0');DWORD bytes{};
    need(ReadFile(logFile,text.data(),DWORD(text.size()),&bytes,nullptr)&&bytes==text.size());CloseHandle(logFile);logFile=INVALID_HANDLE_VALUE;DeleteFileW(path);
    need(text.find("\"spikes_over_25ms\":6")!=std::string::npos&&text.find("\"retained_details\":3")!=std::string::npos);
    for(const auto n:{3,5,6})need(text.find("\"engine_frame\":"+std::to_string(n))!=std::string::npos);
    for(const auto n:{1,2,4})need(text.find("\"engine_frame\":"+std::to_string(n))==std::string::npos);
    devices=nullptr;
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"driverCalls\":"<<calls<<",\"researchContextsCreated\":0,\"gpuTouched\":false}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
