#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>
#include <array>
#include <cstdint>
#include <string_view>
#include "spirv_metadata.hpp"

// Records CPU-side Vulkan command metadata only. It neither executes commands
// nor changes their arguments, and does not establish GPU execution or pixels.
namespace xess_vk::compute_observer {
using Sink = void (*)(const char*);
struct Context;
Context* create(VkDevice device, Sink sink) noexcept;
void destroy(Context*) noexcept;
void summary(Context*) noexcept;
void shader(Context*, VkShaderModule, const VkShaderModuleCreateInfo*) noexcept;
void destroyShader(Context*, VkShaderModule) noexcept;
void pipelines(Context*, uint32_t, const VkComputePipelineCreateInfo*, const VkPipeline*, VkResult) noexcept;
void destroyPipeline(Context*, VkPipeline) noexcept;
void allocateCommands(Context*, const VkCommandBufferAllocateInfo*, const VkCommandBuffer*) noexcept;
void freeCommands(Context*, uint32_t, const VkCommandBuffer*) noexcept;
void resetPool(Context*, VkCommandPool, bool destroy) noexcept;
void beginCommand(Context*, VkCommandBuffer) noexcept;
void bindPipeline(Context*, VkCommandBuffer, VkPipelineBindPoint, VkPipeline) noexcept;
void dispatch(Context*, VkCommandBuffer, uint32_t x, uint32_t y, uint32_t z,
              const char* kind, VkBuffer indirect = VK_NULL_HANDLE, VkDeviceSize offset = 0) noexcept;

// Live shader/pipeline identity and access facts are independent of the bounded
// raw-code diagnostic archive; object destruction retires the live metadata.
// Creation metadata only, no command/dispatch trace.
// Call after the original Vulkan API returns, while its input is still readable.
// All entrypoints preserve LastError. The owner serializes destruction with all
// calls; sinks run outside the archive lock and must not destroy the archive.
inline constexpr std::size_t shaderArchiveModuleBytes=1024*1024;
inline constexpr std::size_t shaderArchiveTotalBytes=16*1024*1024;
inline constexpr std::size_t shaderArchiveModuleLimit=32,shaderArchivePipelineLimit=256;
inline constexpr std::size_t shaderArchiveSelectionLimit=4;
inline constexpr std::size_t shaderArchiveFullGraphModuleLimit=128,shaderArchiveFullGraphSelectionLimit=16;
inline constexpr std::size_t shaderArchiveChunkBytes=8192,shaderArchiveOutputBytes=3*1024*1024;
inline constexpr std::size_t shaderArchiveSpecializationMaps=128,shaderArchiveSpecializationDataBytes=64*1024,shaderArchiveSpecializationTotalBytes=1024*1024;
inline constexpr std::size_t shaderArchiveSetLayouts=2048,shaderArchivePipelineLayouts=1024,shaderArchiveLayoutBindings=64,shaderArchiveLayoutSets=8;
inline constexpr std::size_t shaderArchivePushRanges=32,shaderArchiveImmutableSamplers=256,shaderArchiveLayoutTotalBytes=4*1024*1024;
struct ShaderArchive;
ShaderArchive* createShaderArchive(VkDevice,Sink,bool fullGraph=false) noexcept;
void destroyShaderArchive(ShaderArchive*) noexcept;
void shaderArchiveSummary(ShaderArchive*) noexcept;
void archiveShader(ShaderArchive*,VkShaderModule,const VkShaderModuleCreateInfo*) noexcept;
void destroyArchivedShader(ShaderArchive*,VkShaderModule) noexcept;
// Feed successful native layout creation after the descriptor observer records
// its exact generation. All core arrays/sampler handles are owned copies;
// unknown pNext content is explicitly reported, never followed or inferred.
void archiveSetLayout(ShaderArchive*,VkDescriptorSetLayout,std::uint64_t generation,const VkDescriptorSetLayoutCreateInfo*,VkResult) noexcept;
void destroyArchivedSetLayout(ShaderArchive*,VkDescriptorSetLayout) noexcept;
void archivePipelineLayout(ShaderArchive*,VkPipelineLayout,std::uint64_t generation,const VkPipelineLayoutCreateInfo*,VkResult) noexcept;
void destroyArchivedPipelineLayout(ShaderArchive*,VkPipelineLayout) noexcept;
// generation is the exact descriptor-tracker pipeline generation, supplied at
// creation. A non-null partial result is valid even if its batch returned error.
void archivePipeline(ShaderArchive*,VkPipeline,std::uint64_t generation,const VkComputePipelineCreateInfo*,VkResult) noexcept;
void destroyArchivedPipeline(ShaderArchive*,VkPipeline) noexcept;
// At most four different pipeline/generation pairs (16 in full-graph diagnostics). Missing/stale lookups
// permit a later retry and report at most one refusal. All events share the 3 MiB
// emission budget. Code output is bounded hex chunks via
// the existing session sink; returned sink calls do not prove file persistence,
// shader semantics, GPU execution, or permission to Execute.
// A fault sink can retain this bounded selection independently of per-frame log rotation.
void selectArchivedShader(ShaderArchive*,VkPipeline,std::uint64_t generation,Sink faultSink=nullptr) noexcept;
bool archivedShaderMatches(ShaderArchive*,VkPipeline,std::uint64_t generation,std::string_view sha256) noexcept;
spirv::ImageReads archivedImageReads(ShaderArchive*,VkPipeline,std::uint64_t generation) noexcept;
// Owned SHA-256 for cold-path classification; all zero on missing/stale metadata.
std::array<char,64> archivedShaderFingerprint(ShaderArchive*,VkPipeline,std::uint64_t generation) noexcept;
}
