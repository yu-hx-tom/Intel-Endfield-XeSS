// Production Interop query lifecycle with inert Vulkan entry points. No loader, GPU or game.
#include "interop_gpu_timestamps.hpp"
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace xess_vk;
unsigned checks{}, lookups{}, propertiesCalls{}, creates{}, destroys{}, resets{}, writes{}, reads{};
unsigned submissions{}, copies{}, existingWaits{}, failure{};
bool consumedProof{};
std::uint32_t bits = 36;
float period = 2;
std::uint64_t beginTick = 20, endTick = 60;
std::vector<unsigned> order;
std::vector<VkQueryPool> resetPools, readPools;
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class T> T h(std::uintptr_t value) { return reinterpret_cast<T>(value); }
VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) {
    ++propertiesCalls; out->limits.timestampPeriod = period;
}
VKAPI_ATTR void VKAPI_CALL families(VkPhysicalDevice, std::uint32_t* count, VkQueueFamilyProperties* out) {
    ++propertiesCalls;
    if (out) { out[0] = {}; out[0].timestampValidBits = bits; }
    *count = 1;
}
VKAPI_ATTR VkResult VKAPI_CALL create(VkDevice, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks*, VkQueryPool* out) {
    ++creates;
    require(info->queryType == VK_QUERY_TYPE_TIMESTAMP && info->queryCount == 2, "one independent Interop pair");
    if (failure == 2) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = h<VkQueryPool>(0x10 + creates); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkDevice, VkQueryPool, const VkAllocationCallbacks*) { ++destroys; order.push_back(6); }
VKAPI_ATTR void VKAPI_CALL reset(VkCommandBuffer, VkQueryPool pool, std::uint32_t first, std::uint32_t count) {
    ++resets; order.push_back(1);
    resetPools.push_back(pool);
    require(first == 0 && count == 2, "reset only the Interop pair");
    if (failure == 6) throw std::runtime_error("injected reset failure");
}
VKAPI_ATTR void VKAPI_CALL write(VkCommandBuffer, VkPipelineStageFlagBits stage, VkQueryPool, std::uint32_t index) {
    ++writes; order.push_back(index ? 3 : 2);
    require((index == 0 && stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) ||
        (index == 1 && stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT), "copy command timestamps bracket existing work");
    if (failure == 7 && index == 1) throw std::runtime_error("injected end failure");
}
VKAPI_ATTR VkResult VKAPI_CALL read(VkDevice, VkQueryPool pool, std::uint32_t first, std::uint32_t count,
    std::size_t bytes, void* data, VkDeviceSize stride, VkQueryResultFlags flags) {
    ++reads; order.push_back(5);
    readPools.push_back(pool);
    require(consumedProof, "query read requires the existing consumed completion proof");
    require(first == 0 && count == 2 && bytes == 32 && stride == 16 &&
        flags == (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT), "read is nonwaiting with availability");
    if (failure == 3) return VK_NOT_READY;
    if (failure == 5) throw std::runtime_error("injected read failure");
    auto* values = static_cast<std::uint64_t*>(data);
    values[0] = beginTick; values[1] = 1; values[2] = endTick; values[3] = failure == 4 ? 0 : 1;
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceFunction(VkDevice, const char* name) {
    ++lookups;
    if (failure == 1 && !std::strcmp(name, "vkCmdWriteTimestamp")) return nullptr;
    if (!std::strcmp(name, "vkCreateQueryPool")) return reinterpret_cast<PFN_vkVoidFunction>(create);
    if (!std::strcmp(name, "vkDestroyQueryPool")) return reinterpret_cast<PFN_vkVoidFunction>(destroy);
    if (!std::strcmp(name, "vkCmdResetQueryPool")) return reinterpret_cast<PFN_vkVoidFunction>(reset);
    if (!std::strcmp(name, "vkCmdWriteTimestamp")) return reinterpret_cast<PFN_vkVoidFunction>(write);
    if (!std::strcmp(name, "vkGetQueryPoolResults")) return reinterpret_cast<PFN_vkVoidFunction>(read);
    throw std::runtime_error("query code must not resolve submit/wait functions");
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceFunction(VkInstance, const char* name) {
    ++lookups;
    if (!std::strcmp(name, "vkGetPhysicalDeviceProperties")) return reinterpret_cast<PFN_vkVoidFunction>(properties);
    if (!std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties")) return reinterpret_cast<PFN_vkVoidFunction>(families);
    throw std::runtime_error("unexpected diagnostic capability query");
}
BorrowedVulkanContext context(bool requested = true) {
    BorrowedVulkanContext result;
    result.instance = h<VkInstance>(1); result.physicalDevice = h<VkPhysicalDevice>(2); result.device = h<VkDevice>(3);
    result.queueFamilyIndex = 0; result.getInstanceProcAddr = instanceFunction; result.getDeviceProcAddr = deviceFunction;
    result.gpuTimestamps = requested; return result;
}
void clear() {
    lookups = propertiesCalls = creates = destroys = resets = writes = reads = 0;
    submissions = copies = existingWaits = failure = 0; consumedProof = false;
    bits = 36; period = 2; beginTick = 20; endTick = 60; order.clear(); resetPools.clear(); readPools.clear();
}
GpuTimestampSample identity(std::uint32_t frame) {
    GpuTimestampSample result;
    result.sourceEngineFrameKnown = result.sdkPresentIdKnown = result.sourceCopyTokenKnown = true;
    result.sourceEngineFrame = frame; result.sdkPresentId = frame + 100; result.sourceCopyToken = frame + 200;
    return result;
}
void publish(detail::InteropGpuTimestamps& timer, std::uint32_t frame, VkResult result = VK_SUCCESS,
             std::uint64_t sequence = 0, std::uint64_t context = 0, std::uint64_t generation = 0) {
    consumedProof = false;
    timer.resetRecording();
    timer.begin(h<VkCommandBuffer>(4), identity(frame), sequence, context, generation);
    ++copies; // The unchanged original copy continues even if a diagnostic entry throws.
    timer.end(h<VkCommandBuffer>(4)); timer.beforeSubmit();
    ++submissions; SetLastError(0x1234); timer.submitted(result);
    require(GetLastError() == 0x1234, "post-driver diagnostic state preserves the queue observer's restored LastError");
}
void consumed(detail::InteropGpuTimestamps& timer) {
    ++existingWaits; consumedProof = true; order.push_back(4); timer.consumed();
}
void run() {
    clear();
    {
        detail::InteropGpuTimestamps timer; timer.initialize(context(false));
        publish(timer, 1); consumed(timer); timer.rebuilt(); timer.releaseCompleted();
        require(!timer.latest().requested && !lookups && !propertiesCalls && !creates && !resets && !writes && !reads && !destroys,
            "default OFF adds no capability, query, QPC or cleanup calls");
        require(copies == 1 && submissions == 1 && existingWaits == 1, "disabled mode preserves original work");
    }
    clear();
    {
        detail::InteropGpuTimestamps timer; timer.initialize(context());
        require(timer.latest().requested && !timer.latest().available && !reads, "first frame explicitly has no completed sample");
        publish(timer, 7);
        require(!reads && !timer.latest().available, "submit never adds a current-frame wait or query read");
        consumed(timer); const auto first = timer.latest();
        require(first.available && first.durationKnown && first.durationNs == 80 && first.sequence == 1 && first.generation == 1,
            "completed Interop query yields its own duration and resource generation");
        require(first.sourceEngineFrame == 7 && first.sdkPresentId == 107 && first.sourceCopyToken == 207 &&
            first.sourceEngineFrameKnown && first.sdkPresentIdKnown && first.sourceCopyTokenKnown,
            "record-time source token/frame/ticket are retained");
        require(first.submitQpcTicks && first.fenceObservedQpcTicks >= first.submitQpcTicks && !first.calibration.available,
            "host association is explicit without invented GPU QPC calibration");
        auto wrapped = first; wrapped.validBits = 4; wrapped.timestampPeriodNs = 2;
        wrapped.qpcFrequency = 1'000'000'000; wrapped.submitQpcTicks = 1; wrapped.fenceObservedQpcTicks = 101;
        require(!timestampDuration(wrapped) && wrapped.beginTicks == first.beginTicks && wrapped.endTicks == first.endTicks,
            "possible multiple wraps keep raw ticks and unknown duration");
        timer.rebuilt();
        require(timer.latest().generation == 1 && timer.latest().sourceEngineFrame == 7, "rebuild never relabels last-good history");
        publish(timer, 8); require(timer.latest().sequence == 1, "pending next frame preserves previous sample identity");
        consumed(timer); const auto second = timer.latest();
        require(second.sequence == 2 && second.generation == 2 && second.contextGeneration == first.contextGeneration && second.sourceEngineFrame == 8,
            "rebuilt resources use a new generation without resetting context or serial");
        require(order == std::vector<unsigned>({1,2,3,4,5,1,2,3,4,5}), "existing consumed completion precedes read and subsequent reset");
        const auto count = reads; timer.consumed(); require(reads == count, "repeated completion cannot reread a query");
        timer.releaseCompleted(); require(destroys == 1 && existingWaits == 2 && submissions == 2, "completed shutdown frees pair without new wait or submit");
    }
    for (unsigned injected = 1; injected <= 7; ++injected) {
        clear(); detail::InteropGpuTimestamps timer;
        if (injected <= 2) failure = injected;
        timer.initialize(context());
        if (injected >= 3 && injected <= 5) { publish(timer, 1); consumed(timer); require(timer.latest().available, "seed last-good query"); }
        const auto previous = timer.latest(); failure = injected;
        publish(timer, 2); consumed(timer); timer.rebuilt();
        require(copies == submissions && existingWaits == submissions, "query failure does not skip original publish work or add waits");
        require(timer.latest().requested && timer.latest().sequence == previous.sequence && timer.latest().generation == previous.generation &&
            timer.latest().sourceEngineFrame == previous.sourceEngineFrame && timer.latest().available == previous.available,
            "NOT_READY/unavailable/exception preserves last-good identity across rebuild");
        const auto oldWrites = writes, oldReads = reads; publish(timer, 3); consumed(timer);
        require(writes == oldWrites && reads == oldReads, "failed diagnostics stop only future query commands");
        timer.releaseCompleted(); require(destroys == (creates && injected != 2 ? 1u : 0u), "diagnostic failure leaves normal completed cleanup available");
    }
    for (const auto invalid : {0u, 65u}) {
        clear(); bits = invalid; detail::InteropGpuTimestamps timer; timer.initialize(context());
        publish(timer, 1); consumed(timer); timer.releaseCompleted(); require(!creates && !writes, "unsupported queue valid bits disable only query work");
    }
    clear(); period = std::numeric_limits<float>::infinity();
    { detail::InteropGpuTimestamps timer; timer.initialize(context()); require(!creates, "non-finite period cannot create an invalid duration"); }
    clear(); bits = 4; period = 1e9f; beginTick = 14; endTick = 2;
    { detail::InteropGpuTimestamps timer; timer.initialize(context()); publish(timer, 1); consumed(timer);
      require(timer.latest().durationKnown && timer.latest().durationNs == 4e9, "limited-bit counter wraps modulo without a new wait"); timer.releaseCompleted(); }
    clear();
    { detail::InteropGpuTimestamps timer; timer.initialize(context()); publish(timer, 1); timer.releaseCompleted();
      require(!destroys && !reads && !existingWaits, "uncompleted shutdown neither frees pool nor invents a completion wait");
      consumed(timer); timer.releaseCompleted(); require(destroys == 1, "later existing completion permits cleanup"); }
    for (const auto result : {VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST}) {
        clear(); detail::InteropGpuTimestamps timer; timer.initialize(context()); publish(timer, 1, result); timer.releaseCompleted();
        require(!reads && !existingWaits && !timer.latest().available && destroys == (result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? 1u : 0u),
            "failed submit cannot fabricate samples; uncertain GPU use retains the query pool");
    }
    clear();
    { detail::InteropGpuTimestamps timer; timer.initialize(context());
      timer.begin(h<VkCommandBuffer>(4), identity(12)); timer.end(h<VkCommandBuffer>(4));
      timer.resetRecording(); // Original recording was abandoned; the same CB now records a CPU upload.
      timer.beforeSubmit(); timer.submitted(VK_SUCCESS); consumed(timer);
      require(!reads && !timer.latest().available, "CPU upload cannot claim an abandoned native recording's query identity");
      publish(timer, 13); consumed(timer);
      require(timer.latest().available && timer.latest().sourceEngineFrame == 13 && timer.latest().sequence == 2,
          "later native publication resumes with its own recorded identity"); timer.releaseCompleted(); }
    clear();
    {
        std::array<detail::InteropGpuTimestamps, frameSlotCount> timers;
        for (auto& timer : timers) timer.initialize(context());
        publish(timers[0],71,VK_SUCCESS,30,77,9); publish(timers[1],72,VK_SUCCESS,31,77,9);
        require(creates == 2 && !reads && resetPools.size() == 2 && resetPools[0] != resetPools[1],
                "Two pending frame slots must reset separate physical query pairs without a completion read");
        consumed(timers[1]); consumed(timers[0]);
        require(readPools.size() == 2 && readPools[0] == resetPools[1] && readPools[1] == resetPools[0],
                "Reverse retirement must read the corresponding slot's pair, never the last selected query");
        require(timers[0].latest().sourceEngineFrame == 71 && timers[0].latest().sequence == 30 && timers[0].latest().generation == 9 &&
                timers[1].latest().sourceEngineFrame == 72 && timers[1].latest().sequence == 31 && timers[1].latest().generation == 9 &&
                timers[0].latest().contextGeneration == 77 && timers[1].latest().contextGeneration == 77,
                "Per-slot queries retain owner context/global serial/current resource generation and their original frame source");
        for (auto& timer : timers) timer.releaseCompleted();
        require(destroys == 2, "Completed per-slot pools must both release after their own completion proof");
    }
}
} // namespace
int main() {
    try { run(); std::printf("{\"passed\":true,\"checks\":%u,\"realVulkanCalls\":0,\"gpuExecuted\":false,\"gameTouched\":false}\n", checks); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "Interop timestamps: %s\n", error.what()); return 1; }
}
