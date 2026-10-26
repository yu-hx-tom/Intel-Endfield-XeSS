#pragma once
#include "interop.hpp"
#include <windows.h>
#include <atomic>
#include <cmath>

namespace xess_vk::detail {
// One query pair belongs to Interop's existing single command buffer, not the provider or SDK work.
class InteropGpuTimestamps {
    VkDevice device_{};
    VkQueryPool pool_{};
    PFN_vkDestroyQueryPool destroy_{};
    PFN_vkCmdResetQueryPool reset_{};
    PFN_vkCmdWriteTimestamp write_{};
    PFN_vkGetQueryPoolResults read_{};
    bool active_{}, recorded_{}, staged_{}, pending_{}, uncertain_{};
    std::uint64_t serial_{}, context_{}, generation_{1}, qpcFrequency_{};
    std::uint32_t validBits_{};
    double period_{};
    GpuTimestampSample recording_, pendingSample_, latest_;

    void disable() noexcept {
        active_ = recorded_ = staged_ = false;
        // Preserve the last completed sample; a later diagnostic failure cannot relabel it.
    }
    static std::uint64_t qpc() noexcept {
        LARGE_INTEGER value{};
        return QueryPerformanceCounter(&value) ? static_cast<std::uint64_t>(value.QuadPart) : 0;
    }
public:
    void resetRecording() noexcept { recorded_ = staged_ = false; }
    void initialize(const BorrowedVulkanContext& context) noexcept {
        latest_.requested = context.gpuTimestamps;
        if (!context.gpuTimestamps) return;
        device_ = context.device;
        try {
            if (!context.getInstanceProcAddr || !context.getDeviceProcAddr) return;
            const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                context.getInstanceProcAddr(context.instance, "vkGetPhysicalDeviceProperties"));
            const auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                context.getInstanceProcAddr(context.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
            const auto create = reinterpret_cast<PFN_vkCreateQueryPool>(context.getDeviceProcAddr(device_, "vkCreateQueryPool"));
            destroy_ = reinterpret_cast<PFN_vkDestroyQueryPool>(context.getDeviceProcAddr(device_, "vkDestroyQueryPool"));
            reset_ = reinterpret_cast<PFN_vkCmdResetQueryPool>(context.getDeviceProcAddr(device_, "vkCmdResetQueryPool"));
            write_ = reinterpret_cast<PFN_vkCmdWriteTimestamp>(context.getDeviceProcAddr(device_, "vkCmdWriteTimestamp"));
            read_ = reinterpret_cast<PFN_vkGetQueryPoolResults>(context.getDeviceProcAddr(device_, "vkGetQueryPoolResults"));
            if (!properties || !families || !create || !destroy_ || !reset_ || !write_ || !read_) return;
            VkPhysicalDeviceProperties props{};
            properties(context.physicalDevice, &props);
            std::array<VkQueueFamilyProperties, 64> queues{};
            std::uint32_t count{};
            families(context.physicalDevice, &count, nullptr);
            if (!count || count > queues.size() || context.queueFamilyIndex >= count) return;
            families(context.physicalDevice, &count, queues.data());
            if (count > queues.size() || context.queueFamilyIndex >= count) return;
            validBits_ = queues[context.queueFamilyIndex].timestampValidBits;
            period_ = props.limits.timestampPeriod;
            LARGE_INTEGER frequency{};
            if (!validBits_ || validBits_ > 64 || !(period_ > 0) || !std::isfinite(period_) ||
                !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return;
            qpcFrequency_ = frequency.QuadPart;
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP; info.queryCount = 2;
            if (create(device_, &info, nullptr, &pool_) != VK_SUCCESS) { pool_ = {}; return; }
            static std::atomic<std::uint64_t> contexts{};
            context_ = ++contexts;
            active_ = true;
        } catch (...) { disable(); }
    }
    std::uint64_t contextGeneration() const noexcept { return context_; }
    void begin(VkCommandBuffer command, const GpuTimestampSample& identity,
               std::uint64_t sequence = 0, std::uint64_t context = 0, std::uint64_t generation = 0) noexcept {
        if (!active_) return;
        if (pending_ || staged_ || recorded_) { disable(); return; }
        if (sequence && sequence <= serial_) { disable(); return; }
        if (serial_ == UINT64_MAX) { disable(); return; }
        try {
            reset_(command, pool_, 0, 2);
            write_(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_, 0);
            recording_ = {};
            recording_.requested = true;
            serial_ = sequence ? sequence : serial_ + 1;
            recording_.sequence = serial_; recording_.contextGeneration = context ? context : context_;
            recording_.generation = generation ? generation : generation_;
            recording_.sourceEngineFrame = identity.sourceEngineFrame;
            recording_.sourceEngineFrameKnown = identity.sourceEngineFrameKnown;
            recording_.sdkPresentId = identity.sdkPresentId; recording_.sdkPresentIdKnown = identity.sdkPresentIdKnown;
            recording_.sourceCopyToken = identity.sourceCopyToken; recording_.sourceCopyTokenKnown = identity.sourceCopyTokenKnown;
            recording_.validBits = validBits_; recording_.timestampPeriodNs = period_;
            recording_.qpcFrequency = qpcFrequency_;
            recorded_ = true;
        } catch (...) { disable(); }
    }
    void end(VkCommandBuffer command) noexcept {
        if (!recorded_) return;
        try {
            write_(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, 1);
            recorded_ = false; staged_ = true;
        } catch (...) { disable(); }
    }
    void beforeSubmit() noexcept {
        if (staged_) recording_.submitQpcTicks = qpc();
    }
    void submitted(VkResult result) noexcept {
        if (!staged_) return;
        staged_ = false;
        if (result != VK_SUCCESS) {
            uncertain_ = result != VK_ERROR_OUT_OF_HOST_MEMORY && result != VK_ERROR_OUT_OF_DEVICE_MEMORY;
            disable(); return;
        }
        pendingSample_ = recording_; pending_ = true;
    }
    // Called only after the pre-existing D3D consumed wait succeeded, before command/query reuse.
    void consumed() noexcept {
        if (!pending_) return;
        pending_ = false;
        pendingSample_.fenceObservedQpcTicks = qpc();
        try {
            struct Query { std::uint64_t ticks{}, available{}; } values[2];
            const auto result = read_(device_, pool_, 0, 2, sizeof(values), values, sizeof(Query),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (result != VK_SUCCESS || !values[0].available || !values[1].available) { disable(); return; }
            pendingSample_.beginTicks = values[0].ticks; pendingSample_.endTicks = values[1].ticks;
            pendingSample_.available = true;
            timestampDuration(pendingSample_);
            if(pendingSample_.durationKnown&&pendingSample_.sdkPresentIdKnown)measurement::observe(measurement::Metric::BridgeGpu,
                static_cast<std::uint64_t>(pendingSample_.durationNs),pendingSample_.sdkPresentId,pendingSample_.contextGeneration);
            latest_ = pendingSample_;
        } catch (...) { disable(); }
    }
    void rebuilt() noexcept {
        if (!latest_.requested) return;
        if (generation_ == UINT64_MAX) { disable(); return; }
        ++generation_;
    }
    // Parent cleanup already proved completion; an uncertain failed submit retains its pool with the device.
    void releaseCompleted() noexcept {
        if (pool_ && !pending_ && !uncertain_) {
            try { destroy_(device_, pool_, nullptr); pool_ = {}; } catch (...) {}
        }
        disable();
    }
    const GpuTimestampSample& latest() const noexcept { return latest_; }
};
} // namespace xess_vk::detail
