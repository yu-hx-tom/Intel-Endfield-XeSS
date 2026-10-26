// CPU-only production Tracker: fixed storage, >8192 set admission and a driver
// success ledger independent of metadata capacity/structural refusals.
#include "descriptor_image_metadata.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace {
std::atomic<bool> measuring{};
std::atomic<std::size_t> allocations{},allocatedBytes{};
}
void* operator new(std::size_t bytes) {
    if (measuring) { ++allocations; allocatedBytes += bytes; }
    if (auto* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
namespace dm = xess_vk::descriptor_image_metadata;
unsigned checks{};
std::size_t constructionAllocations{},constructionBytes{},expandedSetStorageBytes{},hotDispatchAllocations{};
void require(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
template<class T> T handle(std::uint64_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
void accepted(dm::Result result) { require(result.accepted(), "metadata call was unexpectedly refused"); }
void capacity(dm::Result result) { require((result.issues & dm::bit(dm::Issue::Capacity)) != 0, "metadata capacity refusal absent"); }
dm::Config config(std::size_t sets = dm::maxDescriptorSets) {
    dm::Config result;
    result.sets = sets;
    return result;
}
struct Fixture {
    dm::Tracker tracker;
    const VkDescriptorSetLayout layout = handle<VkDescriptorSetLayout>(100);
    const VkPipelineLayout pipelineLayout = handle<VkPipelineLayout>(200);
    const VkPipeline pipeline = handle<VkPipeline>(300);
    const VkCommandPool commandPool = handle<VkCommandPool>(400);
    const VkCommandBuffer command = handle<VkCommandBuffer>(500);
    const VkImageView view = handle<VkImageView>(600);
    const VkDescriptorUpdateTemplate update = handle<VkDescriptorUpdateTemplate>(700);
    explicit Fixture(std::size_t sets = dm::maxDescriptorSets) : tracker(config(sets)) {
        const xess_vk::image_inventory::Record image{800,1,
            {VK_FORMAT_R16G16_SFLOAT,64,32,1,1,1,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_USAGE_SAMPLED_BIT,
             0,VK_IMAGE_TILING_OPTIMAL,VK_SHARING_MODE_EXCLUSIVE,VK_IMAGE_LAYOUT_UNDEFINED}};
        accepted(tracker.imageObserved(image));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = handle<VkImage>(800);
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R16G16_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        accepted(tracker.viewCreated(view, &vi, VK_SUCCESS));
        const VkDescriptorSetLayoutBinding binding{5,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &binding;
        accepted(tracker.setLayoutCreated(layout, &li, VK_SUCCESS));
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 1;
        pi.pSetLayouts = &layout;
        accepted(tracker.pipelineLayoutCreated(pipelineLayout, &pi, VK_SUCCESS));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout = pipelineLayout;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = handle<VkShaderModule>(900);
        ci.stage.pName = "main";
        accepted(tracker.pipelineCreated(pipeline, &ci, VK_SUCCESS));
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        accepted(tracker.commandPoolCreated(commandPool, &cp, VK_SUCCESS));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = commandPool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        accepted(tracker.commandsAllocated(&ca, &command, VK_SUCCESS));
        const VkDescriptorUpdateTemplateEntry entry{5,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,0,sizeof(VkDescriptorImageInfo)};
        VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        ti.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        ti.descriptorSetLayout = layout;
        ti.descriptorUpdateEntryCount = 1;
        ti.pDescriptorUpdateEntries = &entry;
        accepted(tracker.templateCreated(update, &ti, VK_SUCCESS));
    }
    VkDescriptorPoolCreateInfo poolInfo(std::uint32_t maximum) const {
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = maximum;
        return info;
    }
    void pool(VkDescriptorPool pool, std::uint32_t maximum) {
        const auto info = poolInfo(maximum);
        accepted(tracker.poolCreated(pool, &info, VK_SUCCESS));
    }
    dm::Result allocate(VkDescriptorPool pool, VkDescriptorSet set, VkResult result = VK_SUCCESS) {
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        return tracker.setsAllocated(&info, &set, result);
    }
    dm::DispatchEvidence dispatch(VkDescriptorSet set) {
        const VkDescriptorImageInfo value{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        accepted(tracker.updateWithTemplate(set, update, &value));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        accepted(tracker.begin(command, &begin, VK_SUCCESS));
        tracker.bindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        tracker.bindSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        return tracker.preDispatch(command, 1, 1, 1);
    }
};
bool sameLedger(const dm::Stats& a, const dm::Stats& b) {
    return a.driverSetAllocationCalls == b.driverSetAllocationCalls && a.driverSetsAllocated == b.driverSetsAllocated &&
           a.driverSetFreeCalls == b.driverSetFreeCalls && a.driverSetsFreed == b.driverSetsFreed &&
           a.driverPoolResetCalls == b.driverPoolResetCalls && a.driverPoolDestroyCalls == b.driverPoolDestroyCalls &&
           a.driverLiveSets == b.driverLiveSets && a.driverSetHighWater == b.driverSetHighWater &&
           a.declaredPoolMaxSets == b.declaredPoolMaxSets && a.driverSetLedgerComplete == b.driverSetLedgerComplete;
}
void aboveOldCapacity() {
    allocations = allocatedBytes = 0;
    measuring = true;
    Fixture fixture;
    measuring = false;
    constructionAllocations = allocations;
    constructionBytes = allocatedBytes;
    require(constructionAllocations > 1 && constructionBytes == fixture.tracker.storageBytes()&&constructionBytes<8*1024*1024,
            "registered fixture allocations must equal base bookkeeping plus touched payload pages");
    allocations = allocatedBytes = 0;
    measuring = true;
    const auto storageBefore=fixture.tracker.storageBytes();
    const auto pool = handle<VkDescriptorPool>(1000);
    fixture.pool(pool, 20000);
    VkDescriptorSet latest{};
    for (std::uint32_t index = 0; index < 8193; ++index) {
        latest = handle<VkDescriptorSet>(2000 + index);
        accepted(fixture.allocate(pool, latest));
    }
    measuring=false;require(allocatedBytes==fixture.tracker.storageBytes()-storageBefore,"large set registration allocates only retained payload pages");
    allocations=allocatedBytes=0;measuring=true;
    const auto evidence = fixture.dispatch(latest);
    const auto stats = fixture.tracker.stats();
    expandedSetStorageBytes=fixture.tracker.storageBytes();
    measuring = false;
    require(evidence.metadataAccepted && evidence.count == 1 && evidence.bindings[0].set.handle == 10192,
            "set beyond old 8192 limit did not survive template update/bind/dispatch");
    require(stats.objects[static_cast<std::size_t>(dm::ObjectKind::Sets)].capacity == 65536 && stats.liveSets == 8193 &&
            stats.driverSetAllocationCalls == 8193 && stats.driverSetsAllocated == 8193 && stats.driverLiveSets == 8193 &&
            stats.driverSetHighWater == 8193 && stats.declaredPoolMaxSets == 20000 && stats.driverSetLedgerComplete,
            "driver live demand was confused with pool declaration or metadata capacity");
    hotDispatchAllocations=allocations;
    require(!allocations && !allocatedBytes, "expanded set hot paths allocated heap storage");
}
void twoPoolsAndRefusals() {
    Fixture fixture(2);
    const auto storageBefore=fixture.tracker.storageBytes();
    const auto a = handle<VkDescriptorPool>(1000), b = handle<VkDescriptorPool>(1001);
    const auto first = handle<VkDescriptorSet>(2000), second = handle<VkDescriptorSet>(2001), refused = handle<VkDescriptorSet>(2002);
    allocations = allocatedBytes = 0;
    measuring = true;
    fixture.pool(a, 64);
    fixture.pool(b, 64);
    accepted(fixture.allocate(a, first));
    accepted(fixture.allocate(b, second));
    const auto old = fixture.dispatch(first);
    capacity(fixture.allocate(a, refused));
    auto stats = fixture.tracker.stats();
    require(stats.liveSets == 2 && stats.driverLiveSets == 3 && stats.driverSetsAllocated == 3 &&
            stats.driverSetHighWater == 3 && stats.declaredPoolMaxSets == 128 && stats.driverSetLedgerComplete,
            "metadata refusal hid a real successful allocation");
    fixture.tracker.setsFreed(a, 1, &refused, VK_SUCCESS);
    require(fixture.tracker.stats().driverLiveSets == 2 && fixture.tracker.stats().liveSets == 2,
            "untracked successful set free was not counted in its owner pool");
    fixture.tracker.poolReset(a, VK_SUCCESS);
    require(fixture.tracker.stats().driverLiveSets == 1 && fixture.tracker.stats().liveSets == 1 &&
            fixture.tracker.stats().declaredPoolMaxSets == 128, "one pool reset cleared another pool or its declared upper bound");
    accepted(fixture.allocate(a, first));
    const auto reused = fixture.dispatch(first);
    require(reused.metadataAccepted && reused.bindings[0].set != old.bindings[0].set &&
            reused.bindings[0].pool != old.bindings[0].pool && !fixture.tracker.validateEvidence(old).accepted(),
            "pool reset/set reuse revived old generations");
    fixture.tracker.setsFreed(b, 1, &second, VK_SUCCESS);
    accepted(fixture.allocate(a, refused));
    const auto beforeFailure = fixture.tracker.stats();
    (void)fixture.allocate(a, second, VK_ERROR_OUT_OF_HOST_MEMORY);
    fixture.tracker.setsFreed(a, 1, &first, VK_ERROR_OUT_OF_HOST_MEMORY);
    fixture.tracker.poolReset(a, VK_ERROR_OUT_OF_HOST_MEMORY);
    const auto poolInfo = fixture.poolInfo(32);
    (void)fixture.tracker.poolCreated(handle<VkDescriptorPool>(1002), &poolInfo, VK_ERROR_OUT_OF_HOST_MEMORY);
    require(sameLedger(beforeFailure, fixture.tracker.stats()), "failed driver API changed successful-call ledger");
    fixture.tracker.poolDestroying(a);
    require(fixture.tracker.stats().driverLiveSets == 0 && fixture.tracker.stats().liveSets == 0 &&
            fixture.tracker.stats().declaredPoolMaxSets == 64, "destroy failed to subtract just its live sets/declaration");
    fixture.tracker.poolDestroying(b);
    stats = fixture.tracker.stats();
    measuring = false;
    require(stats.driverSetsAllocated == 5 && stats.driverSetsFreed == 2 && stats.driverSetAllocationCalls == 5 &&
            stats.driverSetFreeCalls == 2 && stats.driverPoolResetCalls == 1 && stats.driverPoolDestroyCalls == 2 &&
            !stats.driverLiveSets && !stats.declaredPoolMaxSets && stats.driverSetLedgerComplete,
            "two-pool success ledger or retirement counts disagree");
    require(allocatedBytes==fixture.tracker.storageBytes()-storageBefore, "ledger/retirement has no allocations beyond object registration pages");
}
void oversizedMetadataBatch() {
    Fixture fixture(2);
    const auto storageBefore=fixture.tracker.storageBytes();
    allocations = allocatedBytes = 0;
    measuring = true;
    const auto pool = handle<VkDescriptorPool>(1000);
    fixture.pool(pool, 1024);
    std::array<VkDescriptorSet, 300> sets{};
    std::array<VkDescriptorSetLayout, 300> layouts{};
    for (std::size_t index = 0; index < sets.size(); ++index) {
        sets[index] = handle<VkDescriptorSet>(2000 + index);
        layouts[index] = fixture.layout;
    }
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = pool;
    info.descriptorSetCount = static_cast<std::uint32_t>(sets.size());
    info.pSetLayouts = layouts.data();
    require(!fixture.tracker.setsAllocated(&info, sets.data(), VK_SUCCESS).accepted(),
            "oversized metadata batch was unexpectedly admitted");
    auto stats = fixture.tracker.stats();
    require(stats.driverSetAllocationCalls == 1 && stats.driverSetsAllocated == 300 && stats.driverLiveSets == 300 &&
            stats.driverSetHighWater == 300 && !stats.liveSets && stats.driverSetLedgerComplete,
            "successful native batch above metadata scratch limit was not counted");
    fixture.tracker.setsFreed(pool, 300, sets.data(), VK_SUCCESS);
    stats = fixture.tracker.stats();
    require(stats.driverSetFreeCalls == 1 && stats.driverSetsFreed == 300 && !stats.driverLiveSets && stats.driverSetLedgerComplete,
            "oversized successful free did not update ledger before metadata refusal");
    fixture.tracker.poolReset(pool, VK_SUCCESS);
    fixture.tracker.poolDestroying(pool);
    require(!fixture.tracker.stats().declaredPoolMaxSets && fixture.tracker.stats().driverSetLedgerComplete,
            "ledger stopped following real success calls after metadata refusal");
    measuring = false;
    require(allocatedBytes==fixture.tracker.storageBytes()-storageBefore, "oversized metadata refusals allocate no storage beyond the pool registration page");
}
void incompleteLedger() {
    Fixture fixture;
    const auto storageBefore=fixture.tracker.storageBytes();
    allocations = allocatedBytes = 0;
    measuring = true;
    const auto pool = handle<VkDescriptorPool>(1000), unknown = handle<VkDescriptorPool>(1001);
    const auto set = handle<VkDescriptorSet>(2000);
    fixture.pool(pool, 8);
    accepted(fixture.allocate(pool, set));
    fixture.tracker.poolDestroying(VK_NULL_HANDLE);
    require(fixture.tracker.stats().driverSetLedgerComplete && fixture.tracker.stats().driverLiveSets == 1,
            "legal null-pool destroy tainted live ledger");
    fixture.tracker.setsFreed(pool, 2, &set, VK_SUCCESS);
    auto stats = fixture.tracker.stats();
    require(!stats.driverSetLedgerComplete && stats.driverLiveSets == 1,
            "driver ledger underflow wrapped or remained complete");
    fixture.tracker.poolReset(pool, VK_SUCCESS);
    fixture.tracker.poolReset(unknown, VK_SUCCESS);
    fixture.tracker.poolDestroying(pool);
    stats = fixture.tracker.stats();
    require(!stats.driverSetLedgerComplete && !stats.driverLiveSets && !stats.declaredPoolMaxSets,
            "later valid reset/destroy cleared sticky incomplete state");
    measuring = false;
    require(allocatedBytes==fixture.tracker.storageBytes()-storageBefore, "incomplete ledger allocates only object registration pages");
}
}
int main() {
    try {
        aboveOldCapacity();
        twoPoolsAndRefusals();
        oversizedMetadataBatch();
        incompleteLedger();
        std::cout << "{\"stage\":\"descriptor-set-capacity-ledger-cpu\",\"passed\":true,\"checks\":" << checks
                  << ",\"setCapacity\":65536,\"fixedStorageBytes\":" << dm::Tracker::allocatedStorageBytes()
                  << ",\"constructionAllocations\":" << constructionAllocations
                  << ",\"constructionBytes\":" << constructionBytes
                  << ",\"expandedSetStorageBytes\":" << expandedSetStorageBytes
                  << ",\"hotPathAllocations\":" << hotDispatchAllocations
                  << ",\"sdkCalled\":false,\"gpuExecuted\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        measuring = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
