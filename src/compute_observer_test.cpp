#include "compute_observer.hpp"
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace xess_vk::compute_observer;
namespace {
std::vector<std::string> lines;
bool throwSink = false;
void sink(const char* text) { if (throwSink) throw std::runtime_error("sink failure"); lines.emplace_back(text); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T h(uintptr_t n) { return reinterpret_cast<T>(n); }
const std::string& lastDispatch() {
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) if (it->find("compute_dispatch_sample") != std::string::npos) return *it;
    throw std::runtime_error("missing dispatch sample");
}
// Valid, hand-encoded, no-op GLCompute shader, not game code.
constexpr uint32_t code[] = {
    0x07230203,0x00010000,0,5,0,
    (2u<<16)|17,1, (3u<<16)|14,0,1,
    (5u<<16)|15,5,3,0x6e69616d,0,
    (6u<<16)|16,3,17,1,1,1,
    (2u<<16)|19,1, (3u<<16)|33,2,1,
    (5u<<16)|54,1,3,0,2, (2u<<16)|248,4, (1u<<16)|253, (1u<<16)|56
};
}
int main() {
    try {
        auto* c = create(h<VkDevice>(1), sink); require(c != nullptr, "context creation");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=sizeof(code); sm.pCode=code;
        shader(c,h<VkShaderModule>(2),&sm);
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        pi.stage.module=h<VkShaderModule>(2); pi.stage.pName="main";
        VkPipeline pipe=h<VkPipeline>(3);
        // Vulkan permits valid partial pipeline results even if the batch fails.
        pipelines(c,1,&pi,&pipe,VK_ERROR_OUT_OF_DEVICE_MEMORY);
        destroyShader(c,pi.stage.module);
        VkCommandBuffer cb=h<VkCommandBuffer>(4);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool=h<VkCommandPool>(5); ai.commandBufferCount=1;
        allocateCommands(c,&ai,&cb); beginCommand(c,cb);
        bindPipeline(c,cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
        bindPipeline(c,cb,VK_PIPELINE_BIND_POINT_GRAPHICS,h<VkPipeline>(99));
        SetLastError(54321); dispatch(c,cb,1,2,3,"dispatch");
        require(GetLastError()==54321,"metadata changed LastError");
        require(lastDispatch().find("\"pipeline_generation\":0")==std::string::npos,"graphics binding disturbed compute or shader destruction lost pipeline metadata");
        require(lastDispatch().find("\"groups\":[1,2,3]")!=std::string::npos,"group metadata missing");
        const auto first=lastDispatch();
        destroyPipeline(c,pipe); pipelines(c,1,&pi,&pipe,VK_SUCCESS);
        dispatch(c,cb,1,1,1,"dispatch");
        require(lastDispatch().find("\"pipeline_generation\":0")!=std::string::npos,"reused pipeline handle falsely matched stale command binding");
        bindPipeline(c,cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
        dispatch(c,cb,1,1,1,"dispatch_indirect",h<VkBuffer>(6),16);
        require(lastDispatch().find("\"indirect_offset\":16")!=std::string::npos,"indirect metadata missing");
        beginCommand(c,cb); dispatch(c,cb,1,1,1,"dispatch");
        require(lastDispatch().find("\"pipeline_generation\":0")!=std::string::npos,"begin/reset retained binding");
        bindPipeline(c,cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe); resetPool(c,ai.commandPool,false); dispatch(c,cb,1,1,1,"dispatch");
        require(lastDispatch().find("\"pipeline_generation\":0")!=std::string::npos,"pool reset retained binding");
        bindPipeline(c,cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe); freeCommands(c,1,&cb); dispatch(c,cb,1,1,1,"dispatch");
        require(lastDispatch().find("\"recording_generation\":0")!=std::string::npos,"free retained command generation");
        allocateCommands(c,&ai,&cb); bindPipeline(c,cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
        resetPool(c,ai.commandPool,true); dispatch(c,cb,1,1,1,"dispatch");
        require(lastDispatch().find("\"recording_generation\":0")!=std::string::npos,"pool destruction retained command generation");
        allocateCommands(c,&ai,&cb); bindPipeline(c,cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
        const auto oldSize=lines.size();
        for (int i=0;i<100000;++i) dispatch(c,cb,1,1,1,"dispatch");
        require(lines.size()-oldSize<=16,"dispatch samples unbounded");
        throwSink=true; summary(c); throwSink=false;
        summary(c); require(lines.back().find("\"gpu_execution_verified\":false")!=std::string::npos,"metadata claimed GPU execution");
        destroy(c);
        std::cout << "{\"passed\":true,\"stale_handle_and_lifecycle_checks\":true,\"dispatch_recordings_tested\":100000,\"game_tested\":false}\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
