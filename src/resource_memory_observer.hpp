#pragma once
#include "resource_memory_facts.hpp"
#include "recorded_source_layout.hpp"

namespace xess_vk::resource_memory_observer {
using Sink=void(*)(const char*);
struct Context;
struct Observation {
    resource_memory_facts::BindingFacts binding;
    std::uint64_t localIssues{},globalIssues{};bool queried{};
};
using Query=Observation(*)(void*,RecordedImageKey) noexcept;
struct SourceQuery {Query query{};void* user{};};
resource_memory_facts::DeviceScope deviceScope(const VkDeviceCreateInfo*) noexcept;
bool imageRequirementsQuerySupported(const VkImageCreateInfo*) noexcept;
bool bufferRequirementsQuerySupported(const VkBufferCreateInfo*) noexcept;
Context* create(VkDevice,const VkPhysicalDeviceMemoryProperties&,resource_memory_facts::Config,Sink) noexcept;
void destroy(Context*) noexcept;
void retire(Context*) noexcept;
void coverageLost(Context*) noexcept;
void summary(Context*) noexcept;
resource_memory_facts::Stats stats(Context*) noexcept;
// Requirements are an owned result of an additional UNHOOKED core query made
// outside every metadata lock. Registration+exact-key publication is atomic.
void imageCreated(Context*,const image_inventory::Record&,const VkImageCreateInfo*,VkResult,const VkMemoryRequirements*) noexcept;
void bufferCreated(Context*,VkBuffer,const VkBufferCreateInfo*,VkResult,const VkMemoryRequirements*) noexcept;
void imageDestroying(Context*,VkImage) noexcept;
void bufferDestroying(Context*,VkBuffer) noexcept;
void memoryAllocated(Context*,VkDeviceMemory,const VkMemoryAllocateInfo*,VkResult) noexcept;
void memoryFreeing(Context*,VkDeviceMemory) noexcept;
resource_memory_facts::BindTicket beforeImageBind(Context*,resource_memory_facts::BindApi,std::uint32_t,const VkBindImageMemoryInfo*) noexcept;
resource_memory_facts::BindTicket beforeBufferBind(Context*,resource_memory_facts::BindApi,std::uint32_t,const VkBindBufferMemoryInfo*) noexcept;
void afterBind(Context*,resource_memory_facts::BindTicket,VkResult) noexcept;
Observation query(Context*,RecordedImageKey) noexcept;
SourceQuery sourceQuery(Context*) noexcept;
} // namespace xess_vk::resource_memory_observer
