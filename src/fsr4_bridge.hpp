#pragma once
#include "sr_execute_overlay.hpp"
#include <filesystem>
#include <memory>
#include <string>

namespace xess_vk::fsr4 {
struct VulkanContext {
    // All handles and proc addresses must belong to the same dispatch layer.
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};std::uint32_t queueFamily{};
    PFN_vkGetInstanceProcAddr gipa{};PFN_vkGetDeviceProcAddr gdpa{};
};
// The pinned game rebinds its graphics state after SR. The continuation restores
// the compute state used at the SR boundary; it is not a general Vulkan layer.
class Bridge {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    Bridge(VulkanContext,const std::filesystem::path& payload);
    ~Bridge();
    Bridge(const Bridge&)=delete;Bridge& operator=(const Bridge&)=delete;
    // Keep the published facade and recorded continuations until device teardown.
    // Slots retain contexts still referenced by recordings or submitted GPU work.
    void deactivate() noexcept;
    void configure(const sr_overlay::Configuration&);
    void record(const sr_overlay::Frame&,bool reset);
    VkCommandBuffer command(VkCommandBuffer original)const noexcept;
    bool contains(sr_overlay::RecordingKey)const noexcept;
    // Exact handoff's Vulkan tail signalled retired; this is not merely the
    // D3D12 upscale/copy completion. The query itself never changes mappings.
    bool completed(sr_overlay::RecordingKey)const noexcept;
    void retire(sr_overlay::RecordingKey) noexcept;
    void retirePool(std::uint64_t) noexcept;
    void deviceIdle() noexcept;
    VkResult submit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence) noexcept;
    VkResult submit2(VkQueue,std::uint32_t,const VkSubmitInfo2*,VkFence) noexcept;
    std::string lastError()const;
    std::uint64_t submittedFrames()const noexcept;
};
}
