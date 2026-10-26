#include "compute_observer.hpp"
#include "research_build.hpp"
#include "spirv_metadata.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace xess_vk::compute_observer {
namespace {
constexpr uint64_t byteBudget = 256ull * 1024 * 1024;
constexpr size_t shaderLimit = 2048, pipelineLimit = 4096, commandLimit = 8192;
constexpr uint64_t eventLimit = 8192;
struct Shader { uint64_t generation{}, fingerprint{}; spirv::Metadata metadata; };
struct Pipeline { uint64_t generation{}, shaderGeneration{}, dispatches{}, samples{}; };
struct Command { VkCommandPool pool{}; uint64_t generation{}; VkPipeline pipeline{}; uint64_t pipelineGeneration{}; };
struct ErrorGuard { DWORD value = GetLastError(); ~ErrorGuard() { SetLastError(value); } };
template<class T> uint64_t bits(T value) { return reinterpret_cast<uint64_t>(value); }
void quoted(std::ostringstream& s, const std::string& str) {
    s << '"';
    for (const unsigned char c : str) {
        if (c == '"' || c == '\\') s << '\\' << static_cast<char>(c);
        else if (c < 32 || c >= 127) {
            constexpr char hex[] = "0123456789abcdef";
            s << "\\u00" << hex[c >> 4] << hex[c & 15];
        } else s << static_cast<char>(c);
    }
    s << '"';
}
void handle(std::ostringstream& s, uint64_t value) { s << '"' << "0x" << std::hex << value << std::dec << '"'; }
uint64_t fingerprint(const uint32_t* words, size_t count) {
    // FNV-1a for run-local content correlation, not a security/identity hash.
    uint64_t value = 14695981039346656037ull;
    for (size_t i = 0; i < count; ++i) for (unsigned b = 0; b != 4; ++b) {
        value ^= (words[i] >> (b * 8)) & 255u;
        value *= 1099511628211ull;
    }
    return value;
}
}
#if XESS_RESEARCH_DIAGNOSTICS
struct Context {
    VkDevice device{}; Sink sink{}; std::mutex lock;
    std::unordered_map<VkShaderModule, Shader> shaders;
    std::unordered_map<VkPipeline, Pipeline> pipes;
    std::unordered_map<VkCommandBuffer, Command> commands;
    uint64_t sequence{}, bytes{}, shaderCalls{}, computeModules{}, parseRefused{}, unsupportedPipelines{},
        dispatchCalls{}, unknownDispatches{}, stateLimitHits{}, events{}, eventLimitHits{};
    std::atomic<uint64_t> failures{};
    bool byteBudgetRefused{};
    void emit(const char* event, const std::string& fields = {}) {
        if (events >= eventLimit) { ++eventLimitHits; return; }
        std::ostringstream s;
        s << "{\"event\":"; quoted(s, event);
        s << ",\"pid\":" << GetCurrentProcessId() << ",\"uptime_ms\":" << GetTickCount64()
          << ",\"thread_id\":" << GetCurrentThreadId() << ",\"device\":";
        handle(s, bits(device)); s << fields << '}';
        const auto result = s.str();
        if (result.size() >= 30000) { ++failures; return; }
        ++events;
        if (sink) sink(result.c_str());
    }
};

Context* create(VkDevice device, Sink sink) noexcept {
    ErrorGuard error;
    try {
        auto* result = new Context;
        result->device = device; result->sink = sink;
        // Publish context even if diagnostics output fails.
        try { result->emit("compute_observer_start", ",\"metadata_only\":true,\"gpu_execution_verified\":false,\"shader_byte_budget\":268435456"); }
        catch (...) { ++result->failures; }
        return result;
    } catch (...) { return nullptr; }
}
void summary(Context* c) noexcept {
    if (!c) return; ErrorGuard error;
    try {
        std::lock_guard guard(c->lock);
        std::ostringstream s;
        s << ",\"shader_create_calls\":" << c->shaderCalls << ",\"compute_modules\":" << c->computeModules
          << ",\"shader_bytes_inspected\":" << c->bytes << ",\"parse_refused\":" << c->parseRefused
          << ",\"shader_byte_budget_exhausted\":" << (c->byteBudgetRefused ? "true" : "false")
          << ",\"unsupported_pipeline_metadata\":" << c->unsupportedPipelines
          << ",\"live_shaders\":" << c->shaders.size() << ",\"live_pipelines\":" << c->pipes.size()
          << ",\"live_command_buffers\":" << c->commands.size() << ",\"dispatch_recording_calls\":" << c->dispatchCalls
          << ",\"unknown_pipeline_dispatches\":" << c->unknownDispatches << ",\"state_limit_hits\":" << c->stateLimitHits
          << ",\"event_limit_hits\":" << c->eventLimitHits << ",\"trace_failures\":" << c->failures.load()
          << ",\"gpu_execution_verified\":false,\"resource_semantics_verified\":false";
        // Keep summaries possible after detail exhaustion, but still bounded by
        // the parent log's 32 MiB limit and present sampling interval.
        const auto count = c->events; c->events = 0;
        try { c->emit("compute_observer_summary", s.str()); } catch (...) { c->events = count; throw; }
        c->events = count;
    } catch (...) { ++c->failures; }
}
void destroy(Context* c) noexcept { if (!c) return; ErrorGuard error; summary(c); delete c; }
void shader(Context* c, VkShaderModule module, const VkShaderModuleCreateInfo* info) noexcept {
    if (!c || !info) return; ErrorGuard error;
    try {
        std::lock_guard guard(c->lock); ++c->shaderCalls; c->shaders.erase(module);
        if (info->codeSize > byteBudget - c->bytes) { c->byteBudgetRefused = true; ++c->parseRefused; return; }
        if (!info->pCode || info->codeSize > 1024 * 1024 || info->codeSize % 4) { ++c->parseRefused; return; }
        c->bytes += info->codeSize;
        auto meta = spirv::parse(std::span<const uint32_t>(info->pCode, info->codeSize / 4));
        if (!meta.valid) { ++c->parseRefused; return; }
        if (meta.computeEntryPoints.empty()) return;
        ++c->computeModules;
        if (c->shaders.size() >= shaderLimit) { ++c->stateLimitHits; return; }
        const uint64_t generation = ++c->sequence, hash = fingerprint(info->pCode, info->codeSize / 4);
        std::ostringstream s;
        s << ",\"shader\":"; handle(s, bits(module));
        s << ",\"shader_generation\":" << generation << ",\"code_bytes\":" << info->codeSize
          << ",\"fnv1a64_correlation_only\":"; handle(s, hash);
        s << ",\"metadata_truncated\":" << (meta.truncated ? "true" : "false")
          << ",\"unresolved_decoration_groups\":" << (meta.hasUnresolvedDecorationGroups ? "true" : "false") << ",\"entry_points\":[";
        for (size_t i = 0; i < meta.computeEntryPoints.size(); ++i) { if (i) s << ','; quoted(s, meta.computeEntryPoints[i]); }
        s << "],\"names\":[";
        for (size_t i = 0; i < meta.names.size() && i < 32; ++i) {
            if (i) s << ',';
            s << "{\"id\":" << meta.names[i].id << ",\"kind\":" << static_cast<unsigned>(meta.names[i].kind)
              << ",\"member_index\":";
            if (meta.names[i].memberIndex) s << *meta.names[i].memberIndex; else s << "null";
            s << ",\"name\":"; quoted(s, meta.names[i].name); s << '}';
        }
        s << "],\"bindings\":[";
        for (size_t i = 0; i < meta.bindings.size() && i < 32; ++i) {
            if (i) s << ','; const auto& b = meta.bindings[i];
            s << "{\"id\":" << b.id << ",\"set\":";
            if (b.set) s << *b.set; else s << "null";
            s << ",\"binding\":"; if (b.binding) s << *b.binding; else s << "null";
            s << ",\"name\":"; quoted(s, b.name); s << '}';
        }
        s << "],\"log_names_truncated\":" << (meta.names.size() > 32 ? "true" : "false")
          << ",\"log_bindings_truncated\":" << (meta.bindings.size() > 32 ? "true" : "false")
          << ",\"resource_semantics_verified\":false";
        c->shaders.emplace(module, Shader{generation, hash, std::move(meta)});
        c->emit("compute_shader_metadata", s.str());
    } catch (...) { ++c->failures; }
}
void destroyShader(Context* c, VkShaderModule module) noexcept {
    if (!c) return; ErrorGuard error;
    try { std::lock_guard guard(c->lock); c->shaders.erase(module); } catch (...) { ++c->failures; }
}
void pipelines(Context* c, uint32_t count, const VkComputePipelineCreateInfo* infos, const VkPipeline* pipes, VkResult result) noexcept {
    if (!c || !infos || !pipes) return; ErrorGuard error;
    try {
        std::lock_guard guard(c->lock);
        for (uint32_t i = 0; i < count && i < 4096; ++i) {
            if (!pipes[i]) continue; // Partial pipeline creation may still succeed.
            c->pipes.erase(pipes[i]);
            if (c->pipes.size() >= pipelineLimit) { ++c->stateLimitHits; continue; }
            const auto it = c->shaders.find(infos[i].stage.module);
            const uint64_t shaderId = it == c->shaders.end() ? 0 : it->second.generation;
            if (!shaderId) ++c->unsupportedPipelines;
            const uint64_t generation = ++c->sequence;
            c->pipes.emplace(pipes[i], Pipeline{generation, shaderId});
            std::ostringstream s;
            s << ",\"pipeline\":"; handle(s, bits(pipes[i]));
            s << ",\"pipeline_generation\":" << generation << ",\"shader\":"; handle(s, bits(infos[i].stage.module));
            s << ",\"shader_generation\":" << shaderId << ",\"pipeline_layout\":"; handle(s, bits(infos[i].layout));
            s << ",\"entry_point\":";
            const size_t nameLength = infos[i].stage.pName ? strnlen(infos[i].stage.pName, 256) : 0;
            quoted(s, infos[i].stage.pName ? std::string(infos[i].stage.pName, nameLength) : "");
            s << ",\"entry_point_truncated\":" << (nameLength == 256 ? "true" : "false") << ",\"batch_result\":" << result
              << ",\"shader_metadata_available\":" << (shaderId ? "true" : "false");
            c->emit("compute_pipeline", s.str());
        }
        if (count > 4096) ++c->stateLimitHits;
    } catch (...) { ++c->failures; }
}
void destroyPipeline(Context* c, VkPipeline pipeline) noexcept {
    if (!c) return; ErrorGuard error;
    try { std::lock_guard guard(c->lock); c->pipes.erase(pipeline); } catch (...) { ++c->failures; }
}
void allocateCommands(Context* c, const VkCommandBufferAllocateInfo* info, const VkCommandBuffer* commands) noexcept {
    if (!c || !info || !commands) return; ErrorGuard error;
    try {
        std::lock_guard guard(c->lock);
        if (info->commandBufferCount > commandLimit) c->commands.clear();
        for (uint32_t i = 0; i < info->commandBufferCount && i < commandLimit; ++i) {
            c->commands.erase(commands[i]);
            if (c->commands.size() >= commandLimit) { ++c->stateLimitHits; continue; }
            c->commands.emplace(commands[i], Command{info->commandPool, ++c->sequence});
        }
        if (info->commandBufferCount > commandLimit) ++c->stateLimitHits;
    } catch (...) { ++c->failures; }
}
void freeCommands(Context* c, uint32_t count, const VkCommandBuffer* commands) noexcept {
    if (!c || !commands) return; ErrorGuard error;
    try {
        std::lock_guard guard(c->lock);
        if (count > commandLimit) { c->commands.clear(); ++c->stateLimitHits; return; }
        for (uint32_t i = 0; i < count; ++i) c->commands.erase(commands[i]);
    } catch (...) { ++c->failures; }
}
void resetPool(Context* c, VkCommandPool pool, bool remove) noexcept {
    if (!c) return; ErrorGuard error;
    try {
        std::lock_guard guard(c->lock);
        for (auto it = c->commands.begin(); it != c->commands.end();) {
            if (it->second.pool != pool) { ++it; continue; }
            if (remove) it = c->commands.erase(it);
            else { it->second = Command{pool, ++c->sequence}; ++it; }
        }
    } catch (...) { ++c->failures; }
}
void beginCommand(Context* c, VkCommandBuffer cb) noexcept {
    if (!c) return; ErrorGuard error;
    try {
        std::lock_guard guard(c->lock); const auto it = c->commands.find(cb);
        if (it != c->commands.end()) it->second = Command{it->second.pool, ++c->sequence};
    } catch (...) { ++c->failures; }
}
void bindPipeline(Context* c, VkCommandBuffer cb, VkPipelineBindPoint bind, VkPipeline pipeline) noexcept {
    if (!c || bind != VK_PIPELINE_BIND_POINT_COMPUTE) return; ErrorGuard error;
    try {
        std::lock_guard guard(c->lock); const auto it = c->commands.find(cb); if (it == c->commands.end()) return;
        const auto pipe = c->pipes.find(pipeline); it->second.pipeline = pipeline;
        it->second.pipelineGeneration = pipe == c->pipes.end() ? 0 : pipe->second.generation;
    } catch (...) { ++c->failures; }
}
void dispatch(Context* c, VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z, const char* kind, VkBuffer indirect, VkDeviceSize offset) noexcept {
    if (!c) return; ErrorGuard error;
    try {
        std::lock_guard guard(c->lock); const auto call = ++c->dispatchCalls;
        const auto command = c->commands.find(cb);
        auto pipe = command == c->commands.end() ? c->pipes.end() : c->pipes.find(command->second.pipeline);
        if (pipe != c->pipes.end() && pipe->second.generation != command->second.pipelineGeneration) pipe = c->pipes.end();
        const bool known = pipe != c->pipes.end();
        if (!known) ++c->unknownDispatches;
        const uint64_t pipeCall = known ? ++pipe->second.dispatches : 0;
        const bool sample = known ? (pipe->second.samples < 16 && (pipeCall <= 2 || pipeCall % 300 == 0)) : c->unknownDispatches <= 8;
        if (!sample) return;
        if (known) ++pipe->second.samples;
        std::ostringstream s;
        s << ",\"recording_call\":" << call << ",\"command_buffer\":"; handle(s, bits(cb));
        s << ",\"recording_generation\":" << (command == c->commands.end() ? 0 : command->second.generation)
          << ",\"pipeline\":"; handle(s, command == c->commands.end() ? 0 : bits(command->second.pipeline));
        s << ",\"pipeline_generation\":" << (known ? pipe->second.generation : 0)
          << ",\"shader_generation\":" << (known ? pipe->second.shaderGeneration : 0)
          << ",\"pipeline_dispatch_recordings\":" << pipeCall << ",\"kind\":"; quoted(s, kind);
        s << ",\"groups_known\":" << (indirect ? "false" : "true")
          << ",\"dispatch_base_coordinates_captured\":false"
          << ",\"groups\":[" << x << ',' << y << ',' << z << "],\"indirect_buffer\":"; handle(s, bits(indirect));
        s << ",\"indirect_offset\":" << offset << ",\"gpu_execution_verified\":false";
        c->emit("compute_dispatch_sample", s.str());
    } catch (...) { ++c->failures; }
}

#endif
namespace {
struct ArchivedCode {
    VkShaderModule module{};std::uint64_t generation{},correlation{};
    std::vector<std::uint32_t> words;std::size_t set0Bindings{};
    std::array<char,64> sha256{};
    spirv::ImageReads reads;
    bool live{},metadataTruncated{},decorationGroups{};
};
struct ArchivedLayoutBinding {
    std::uint32_t binding{},type{},count{},stages{};std::vector<std::uint64_t> immutableSamplers;bool immutable{};
};
struct ArchivedSetLayout {
    VkDescriptorSetLayout handle{};std::uint64_t generation{};std::uint32_t flags{},declaredBindings{};
    std::vector<ArchivedLayoutBinding> bindings;bool coreCaptured{},extensions{};const char* reason="not_captured";
};
struct ArchivedSetLayoutRef {VkDescriptorSetLayout handle{};std::shared_ptr<const ArchivedSetLayout> metadata;};
struct ArchivedPipelineLayout {
    VkPipelineLayout handle{};std::uint64_t generation{};std::uint32_t flags{},declaredSets{},declaredRanges{};
    std::vector<ArchivedSetLayoutRef> sets;std::vector<VkPushConstantRange> ranges;
    bool coreCaptured{},extensions{};const char* reason="not_captured";
    bool complete()const noexcept {if(!coreCaptured||extensions)return false;for(const auto& set:sets)if(!set.metadata||!set.metadata->coreCaptured||set.metadata->extensions)return false;return true;}
};
struct ArchivedSpecialization {
    std::vector<VkSpecializationMapEntry> entries;std::vector<unsigned char> data;
    std::uint32_t declaredEntries{};std::size_t declaredDataBytes{};bool present{},captured=true;const char* reason="absent";
};
struct ArchivedPipeline {
    VkPipeline handle{};std::uint64_t generation{};VkShaderModule module{};
    std::size_t codeIndex=shaderArchiveFullGraphModuleLimit;VkResult batchResult{};
    VkPipelineLayout layout{};std::shared_ptr<const ArchivedPipelineLayout> layoutMetadata;ArchivedSpecialization specialization;
    std::uint32_t flags{},stageFlags{};bool stageExtensions{},pipelineExtensions{};
    std::array<char,257> entry{};bool entryTruncated{};
};
void archiveBounds(std::ostringstream& s,bool fullGraph) {
    s<<",\"module_byte_limit\":"<<shaderArchiveModuleBytes<<",\"retained_byte_limit\":"<<shaderArchiveTotalBytes
     <<",\"module_limit\":"<<(fullGraph?shaderArchiveFullGraphModuleLimit:shaderArchiveModuleLimit)<<",\"pipeline_limit\":"<<shaderArchivePipelineLimit
     <<",\"selection_limit\":"<<(fullGraph?shaderArchiveFullGraphSelectionLimit:shaderArchiveSelectionLimit)
     <<",\"full_graph_observation\":"<<(fullGraph?"true":"false")
     <<",\"chunk_byte_limit\":"<<shaderArchiveChunkBytes<<",\"output_byte_limit\":"<<shaderArchiveOutputBytes
     <<",\"output_chunk_limit\":"<<(shaderArchiveModuleBytes/shaderArchiveChunkBytes)
     <<",\"specialization_map_limit\":"<<shaderArchiveSpecializationMaps<<",\"specialization_data_limit\":"<<shaderArchiveSpecializationDataBytes
     <<",\"specialization_total_limit\":"<<shaderArchiveSpecializationTotalBytes<<",\"layout_metadata_total_limit\":"<<shaderArchiveLayoutTotalBytes
     <<",\"set_layout_limit\":"<<shaderArchiveSetLayouts<<",\"pipeline_layout_limit\":"<<shaderArchivePipelineLayouts
     <<",\"layout_binding_limit\":"<<shaderArchiveLayoutBindings<<",\"layout_set_limit\":"<<shaderArchiveLayoutSets
     <<",\"push_range_limit\":"<<shaderArchivePushRanges<<",\"immutable_sampler_limit\":"<<shaderArchiveImmutableSamplers
     <<",\"compute_dispatch_trace_enabled\":false,\"shader_semantics_verified\":false,\"execute_permitted\":false";
}
}
struct ShaderArchive {
    VkDevice device{};Sink sink{};std::mutex lock;
    struct RuntimeCode {std::array<char,64> sha256{};spirv::ImageReads reads;};
    struct RuntimePipeline {std::uint64_t generation{};RuntimeCode code;bool readsQualified{};};
    // Rendering access facts follow live objects, independently of diagnostic capture quotas.
    std::unordered_map<VkShaderModule,RuntimeCode> runtimeShaders;
    std::unordered_map<VkPipeline,RuntimePipeline> runtimePipelines;
    std::array<ArchivedCode,shaderArchiveFullGraphModuleLimit> codes;
    std::array<ArchivedPipeline,shaderArchivePipelineLimit> pipelines;
    std::unordered_map<VkDescriptorSetLayout,std::shared_ptr<const ArchivedSetLayout>> setLayouts;
    std::unordered_map<VkPipelineLayout,std::shared_ptr<const ArchivedPipelineLayout>> pipelineLayouts;
    std::uint64_t sequence{},shaderCalls{},inspectedBytes{},retainedBytes{},retainedModules{},inputRefused{},moduleSizeRefused{},parseRefused{},
        nonCompute{},bindingFiltered{},moduleLimitRefused{},byteLimitRefused{},pipelineCalls{},pipelineRefused{},pipelineLimitRefused{},
        selections{},selectionRefused{},outputChunks{},outputBytes{},summaries{};
    std::uint64_t setLayoutCalls{},pipelineLayoutCalls{},layoutInputRefused{},layoutBudgetRefused{},layoutCapacityRefused{},layoutBytes{},
        specializationCalls{},specializationRefused{},specializationBytes{},specializationOutputChunks{};
    struct Selected {VkPipeline pipeline{};std::uint64_t generation{};};
    std::array<Selected,shaderArchiveFullGraphSelectionLimit> selected;
    std::size_t selectedCount{};
    bool fullGraph{};
    std::size_t moduleLimit()const noexcept {return fullGraph?shaderArchiveFullGraphModuleLimit:shaderArchiveModuleLimit;}
    std::size_t selectionLimit()const noexcept {return fullGraph?shaderArchiveFullGraphSelectionLimit:shaderArchiveSelectionLimit;}
    std::span<ArchivedCode> codeSlots() noexcept {return {codes.data(),moduleLimit()};}
    std::atomic<std::uint64_t> failures{},emissionBytes{};bool refusalReported{};
    std::string line(const char* event,const std::string& fields)const {
        std::ostringstream s;s.imbue(std::locale::classic());s<<"{\"event\":";quoted(s,event);
        s<<",\"pid\":"<<GetCurrentProcessId()<<",\"uptime_ms\":"<<GetTickCount64()<<",\"thread_id\":"<<GetCurrentThreadId()<<",\"device\":";
        handle(s,bits(device));s<<fields<<'}';return s.str();
    }
    bool emit(const std::string& text,Sink faultSink=nullptr) noexcept {
        const auto output=faultSink?faultSink:sink;
        try {if(!output||text.size()>=32767){++failures;return false;}auto prior=emissionBytes.load();
            do {if(text.size()+1>shaderArchiveOutputBytes-prior){++failures;return false;}}
            while(!emissionBytes.compare_exchange_weak(prior,prior+text.size()+1));
            output(text.c_str());return true;}catch(...){++failures;return false;}
    }
};

ShaderArchive* createShaderArchive(VkDevice device,Sink sink,bool fullGraph) noexcept {
    ErrorGuard error;
    try {auto* c=new ShaderArchive;c->device=device;c->sink=sink;c->fullGraph=fullGraph;
        try {std::ostringstream s;s.imbue(std::locale::classic());archiveBounds(s,c->fullGraph);c->emit(c->line("sr_shader_archive_start",s.str()));}catch(...){++c->failures;}
        return c;
    }catch(...){return nullptr;}
}
void shaderArchiveSummary(ShaderArchive* c) noexcept {
    if(!c)return;ErrorGuard error;
    try {std::ostringstream s;s.imbue(std::locale::classic());{
        std::lock_guard guard(c->lock);if(c->summaries++>=4)return;
        s<<",\"shader_create_calls\":"<<c->shaderCalls<<",\"shader_bytes_inspected\":"<<c->inspectedBytes
         <<",\"retained_modules\":"<<c->retainedModules<<",\"retained_bytes\":"<<c->retainedBytes
         <<",\"input_refused\":"<<c->inputRefused<<",\"module_size_refused\":"<<c->moduleSizeRefused<<",\"parse_refused\":"<<c->parseRefused
         <<",\"non_compute\":"<<c->nonCompute<<",\"set0_binding_filtered\":"<<c->bindingFiltered
         <<",\"module_limit_refused\":"<<c->moduleLimitRefused<<",\"byte_limit_refused\":"<<c->byteLimitRefused
         <<",\"pipeline_create_calls\":"<<c->pipelineCalls<<",\"pipeline_refused\":"<<c->pipelineRefused<<",\"pipeline_limit_refused\":"<<c->pipelineLimitRefused
         <<",\"set_layout_create_calls\":"<<c->setLayoutCalls<<",\"pipeline_layout_create_calls\":"<<c->pipelineLayoutCalls
         <<",\"layout_input_refused\":"<<c->layoutInputRefused<<",\"layout_budget_refused\":"<<c->layoutBudgetRefused<<",\"layout_capacity_refused\":"<<c->layoutCapacityRefused
         <<",\"layout_bytes_retained_cumulative\":"<<c->layoutBytes<<",\"specialization_calls\":"<<c->specializationCalls
         <<",\"specialization_refused\":"<<c->specializationRefused<<",\"specialization_bytes_retained_cumulative\":"<<c->specializationBytes<<",\"specialization_output_chunks\":"<<c->specializationOutputChunks
         <<",\"selection_calls\":"<<c->selections<<",\"selection_refused\":"<<c->selectionRefused
         <<",\"selected_pipelines\":"<<c->selectedCount
         <<",\"creation_tracking_stopped_after_selection\":"<<(c->selectedCount==c->selectionLimit()?"true":"false")
         <<",\"output_chunks\":"<<c->outputChunks<<",\"output_bytes_attempted\":"<<c->outputBytes<<",\"archive_emission_bytes_attempted\":"<<c->emissionBytes.load()<<",\"sink_failures\":"<<c->failures.load();
        archiveBounds(s,c->fullGraph);
    }c->emit(c->line("sr_shader_archive_summary",s.str()));}catch(...){++c->failures;}
}
void destroyShaderArchive(ShaderArchive* c) noexcept {if(!c)return;ErrorGuard error;shaderArchiveSummary(c);delete c;}

void archiveShader(ShaderArchive* c,VkShaderModule module,const VkShaderModuleCreateInfo* info) noexcept {
    if(!c)return;ErrorGuard error;
    try {std::lock_guard guard(c->lock);++c->shaderCalls;c->runtimeShaders.erase(module);const auto generation=++c->sequence;
        for(auto& code:c->codeSlots())if(code.live&&code.module==module)code.live=false;
        if(!module||!info||info->sType!=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO||!info->pCode||
            reinterpret_cast<std::uintptr_t>(info->pCode)%alignof(std::uint32_t)||info->codeSize<20||info->codeSize%4){++c->inputRefused;return;}
        if(info->codeSize>shaderArchiveModuleBytes){++c->moduleSizeRefused;return;}
        c->inspectedBytes+=info->codeSize;
        const auto words=std::span<const std::uint32_t>(info->pCode,info->codeSize/4);const auto metadata=spirv::parse(words);
        if(!metadata.valid){++c->parseRefused;return;}if(metadata.computeEntryPoints.empty()){++c->nonCompute;return;}
        ShaderArchive::RuntimeCode runtime;runtime.reads=spirv::imageReads(metadata);
        std::array<unsigned char,32> digest{};
        BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0&&
           BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)==0&&
           BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<std::uint32_t*>(words.data())),static_cast<ULONG>(info->codeSize),0)==0&&
           BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)==0){
            constexpr char hex[]="0123456789abcdef";
            for(std::size_t n=0;n<digest.size();++n){runtime.sha256[2*n]=hex[digest[n]>>4];runtime.sha256[2*n+1]=hex[digest[n]&15];}
        }
        if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
        c->runtimeShaders.insert_or_assign(module,runtime);
        if(c->selectedCount==c->selectionLimit())return;
        if(c->retainedModules>=c->moduleLimit()){++c->moduleLimitRefused;return;}
        if(info->codeSize>shaderArchiveTotalBytes-c->retainedBytes){++c->byteLimitRefused;return;}
        std::array<std::uint32_t,128> bindings{};std::size_t count{};
        for(const auto& binding:metadata.bindings)if(binding.set&&*binding.set==0&&binding.binding&&
            std::find(bindings.begin(),bindings.begin()+count,*binding.binding)==bindings.begin()+count)bindings[count++]=*binding.binding;
        const bool prepare=count==8&&std::all_of(bindings.begin(),bindings.begin()+8,[](auto b){return b<8;});
        const bool retain=c->fullGraph?count!=0:count>=10||prepare;
        if(!retain){++c->bindingFiltered;return;}
        auto& code=c->codes[c->retainedModules];code.words.assign(words.begin(),words.end());code.sha256=runtime.sha256;
        code.module=module;code.generation=generation;code.correlation=fingerprint(code.words.data(),code.words.size());code.set0Bindings=count;
        code.metadataTruncated=metadata.truncated;code.decorationGroups=metadata.hasUnresolvedDecorationGroups;code.live=true;
        code.reads=runtime.reads;
        ++c->retainedModules;c->retainedBytes+=info->codeSize;
    }catch(...){++c->failures;}
}
void destroyArchivedShader(ShaderArchive* c,VkShaderModule module) noexcept {
    if(!c)return;ErrorGuard error;
    try {std::lock_guard guard(c->lock);c->runtimeShaders.erase(module);for(auto& code:c->codeSlots())if(code.live&&code.module==module)code.live=false;}catch(...){++c->failures;}
}
void archiveSetLayout(ShaderArchive* c,VkDescriptorSetLayout handle,std::uint64_t generation,const VkDescriptorSetLayoutCreateInfo* info,VkResult result) noexcept {
    if(!c)return;ErrorGuard error;if(result!=VK_SUCCESS)return;
    try {std::lock_guard guard(c->lock);++c->setLayoutCalls;if(c->selectedCount==c->selectionLimit())return;
        if(!handle||!generation||!info||info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO){++c->layoutInputRefused;return;}
        c->setLayouts.erase(handle);if(c->setLayouts.size()>=shaderArchiveSetLayouts){++c->layoutCapacityRefused;return;}
        auto record=std::make_shared<ArchivedSetLayout>();record->handle=handle;record->generation=generation;record->flags=info->flags;record->declaredBindings=info->bindingCount;record->extensions=info->pNext!=nullptr;
        std::size_t samplers{},cost=sizeof(ArchivedSetLayout);const char* refusal{};
        if(info->bindingCount>shaderArchiveLayoutBindings)refusal="binding_limit";
        else if(info->bindingCount&&!info->pBindings)refusal="missing_bindings";
        else for(std::size_t n=0;n<info->bindingCount;++n){const auto& binding=info->pBindings[n];
            for(std::size_t p=0;p<n;++p)if(info->pBindings[p].binding==binding.binding)refusal="duplicate_binding";
            if(binding.pImmutableSamplers){if(binding.descriptorCount>shaderArchiveImmutableSamplers-samplers){refusal="immutable_sampler_limit";break;}samplers+=binding.descriptorCount;}}
        if(!refusal)cost+=info->bindingCount*sizeof(ArchivedLayoutBinding)+samplers*sizeof(std::uint64_t);
        if(cost>shaderArchiveLayoutTotalBytes-c->layoutBytes){++c->layoutBudgetRefused;return;}
        if(refusal){record->reason=refusal;++c->layoutInputRefused;}
        else {record->bindings.reserve(info->bindingCount);for(std::size_t n=0;n<info->bindingCount;++n){const auto& b=info->pBindings[n];ArchivedLayoutBinding binding;
                binding.binding=b.binding;binding.type=b.descriptorType;binding.count=b.descriptorCount;binding.stages=b.stageFlags;binding.immutable=b.pImmutableSamplers!=nullptr;
                if(b.pImmutableSamplers){binding.immutableSamplers.reserve(b.descriptorCount);for(std::size_t s=0;s<b.descriptorCount;++s)binding.immutableSamplers.push_back(bits(b.pImmutableSamplers[s]));}
                record->bindings.push_back(std::move(binding));}
            record->coreCaptured=true;record->reason=record->extensions?"extensions_not_captured":"captured";}
        c->layoutBytes+=cost;c->setLayouts.emplace(handle,std::move(record));
    }catch(...){++c->failures;}
}
void destroyArchivedSetLayout(ShaderArchive* c,VkDescriptorSetLayout handle) noexcept {
    if(!c)return;ErrorGuard error;try{std::lock_guard guard(c->lock);c->setLayouts.erase(handle);}catch(...){++c->failures;}
}
void archivePipelineLayout(ShaderArchive* c,VkPipelineLayout handle,std::uint64_t generation,const VkPipelineLayoutCreateInfo* info,VkResult result) noexcept {
    if(!c)return;ErrorGuard error;if(result!=VK_SUCCESS)return;
    try{std::lock_guard guard(c->lock);++c->pipelineLayoutCalls;if(c->selectedCount==c->selectionLimit())return;
        if(!handle||!generation||!info||info->sType!=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO){++c->layoutInputRefused;return;}
        c->pipelineLayouts.erase(handle);if(c->pipelineLayouts.size()>=shaderArchivePipelineLayouts){++c->layoutCapacityRefused;return;}
        auto record=std::make_shared<ArchivedPipelineLayout>();record->handle=handle;record->generation=generation;record->flags=info->flags;
        record->declaredSets=info->setLayoutCount;record->declaredRanges=info->pushConstantRangeCount;record->extensions=info->pNext!=nullptr;
        std::size_t cost=sizeof(ArchivedPipelineLayout);const char* refusal{};
        if(info->setLayoutCount>shaderArchiveLayoutSets)refusal="set_limit";
        else if(info->pushConstantRangeCount>shaderArchivePushRanges)refusal="push_range_limit";
        else if((info->setLayoutCount&&!info->pSetLayouts)||(info->pushConstantRangeCount&&!info->pPushConstantRanges))refusal="missing_core_array";
        else {for(std::size_t n=0;n<info->pushConstantRangeCount;++n){const auto& r=info->pPushConstantRanges[n];if(!r.size||!r.stageFlags||(r.offset%4)||(r.size%4)||r.size>UINT32_MAX-r.offset)refusal="invalid_push_range";}
            cost+=info->setLayoutCount*sizeof(ArchivedSetLayoutRef)+info->pushConstantRangeCount*sizeof(VkPushConstantRange);}
        if(refusal)cost=sizeof(ArchivedPipelineLayout);
        if(cost>shaderArchiveLayoutTotalBytes-c->layoutBytes){++c->layoutBudgetRefused;return;}
        if(refusal){record->reason=refusal;++c->layoutInputRefused;}
        else {record->sets.reserve(info->setLayoutCount);for(std::size_t n=0;n<info->setLayoutCount;++n){const auto found=c->setLayouts.find(info->pSetLayouts[n]);
                record->sets.push_back({info->pSetLayouts[n],found==c->setLayouts.end()?nullptr:found->second});}
            if(info->pushConstantRangeCount)record->ranges.assign(info->pPushConstantRanges,info->pPushConstantRanges+info->pushConstantRangeCount);
            record->coreCaptured=true;record->reason=record->extensions?"extensions_not_captured":"captured";}
        c->layoutBytes+=cost;c->pipelineLayouts.emplace(handle,std::move(record));
    }catch(...){++c->failures;}
}
void destroyArchivedPipelineLayout(ShaderArchive* c,VkPipelineLayout handle) noexcept {
    if(!c)return;ErrorGuard error;try{std::lock_guard guard(c->lock);c->pipelineLayouts.erase(handle);}catch(...){++c->failures;}
}
namespace {
ArchivedSpecialization copySpecialization(ShaderArchive& archive,const VkSpecializationInfo* info) {
    ArchivedSpecialization result;if(!info)return result;++archive.specializationCalls;result.present=true;result.captured=false;
    result.declaredEntries=info->mapEntryCount;result.declaredDataBytes=info->dataSize;const char* refusal{};
    if(info->mapEntryCount>shaderArchiveSpecializationMaps)refusal="map_limit";
    else if(info->dataSize>shaderArchiveSpecializationDataBytes)refusal="data_limit";
    else if((info->mapEntryCount&&!info->pMapEntries)||(info->dataSize&&!info->pData))refusal="missing_specialization_array";
    else for(std::size_t n=0;n<info->mapEntryCount;++n){const auto& e=info->pMapEntries[n];
        if(e.offset>info->dataSize||e.size>info->dataSize-e.offset)refusal="map_out_of_data_bounds";
        for(std::size_t p=0;p<n;++p)if(info->pMapEntries[p].constantID==e.constantID)refusal="duplicate_constant_id";}
    const auto cost=refusal?0:info->dataSize+info->mapEntryCount*sizeof(VkSpecializationMapEntry);
    if(!refusal&&cost>shaderArchiveSpecializationTotalBytes-archive.specializationBytes)refusal="specialization_total_budget";
    if(refusal){result.reason=refusal;++archive.specializationRefused;return result;}
    if(info->mapEntryCount)result.entries.assign(info->pMapEntries,info->pMapEntries+info->mapEntryCount);
    if(info->dataSize){const auto* bytes=static_cast<const unsigned char*>(info->pData);result.data.assign(bytes,bytes+info->dataSize);}
    result.captured=true;result.reason="captured";archive.specializationBytes+=cost;return result;
}
}
void archivePipeline(ShaderArchive* c,VkPipeline pipeline,std::uint64_t generation,const VkComputePipelineCreateInfo* info,VkResult result) noexcept {
    if(!c)return;ErrorGuard error;
    try {std::lock_guard guard(c->lock);++c->pipelineCalls;c->runtimePipelines.erase(pipeline);ArchivedPipeline* target{};
        for(auto& p:c->pipelines)if(p.handle==pipeline&&pipeline){p={};target=&p;break;}
        if(!pipeline||!generation||!info||info->sType!=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO||info->stage.stage!=VK_SHADER_STAGE_COMPUTE_BIT){++c->pipelineRefused;return;}
        const auto runtime=c->runtimeShaders.find(info->stage.module);
        if(runtime!=c->runtimeShaders.end())c->runtimePipelines.insert_or_assign(pipeline,ShaderArchive::RuntimePipeline{generation,runtime->second,
            result==VK_SUCCESS&&!info->pNext&&!info->stage.pNext&&info->stage.pName&&strnlen(info->stage.pName,256)<256});
        if(c->selectedCount==c->selectionLimit())return;
        std::size_t codeIndex=shaderArchiveFullGraphModuleLimit;
        for(std::size_t n=0;n<c->retainedModules;++n)if(c->codes[n].live&&c->codes[n].module==info->stage.module){codeIndex=n;break;}
        if(codeIndex==shaderArchiveFullGraphModuleLimit){++c->pipelineRefused;return;}
        if(!target)for(auto& p:c->pipelines)if(!p.handle){target=&p;break;}
        if(!target){++c->pipelineLimitRefused;return;}
        ArchivedPipeline record;record.handle=pipeline;record.generation=generation;record.module=info->stage.module;record.batchResult=result;record.codeIndex=codeIndex;
        record.layout=info->layout;const auto layout=c->pipelineLayouts.find(info->layout);if(layout!=c->pipelineLayouts.end())record.layoutMetadata=layout->second;
        record.specialization=copySpecialization(*c,info->stage.pSpecializationInfo);record.flags=info->flags;record.stageFlags=info->stage.flags;record.stageExtensions=info->stage.pNext!=nullptr;record.pipelineExtensions=info->pNext!=nullptr;
        const auto length=info->stage.pName?strnlen(info->stage.pName,256):0;
        if(length)std::memcpy(record.entry.data(),info->stage.pName,length);
        record.entryTruncated=length==256;
        *target=std::move(record); // Allocation failure must leave no selectable partial pipeline.
    }catch(...){++c->failures;}
}
void destroyArchivedPipeline(ShaderArchive* c,VkPipeline pipeline) noexcept {
    if(!c)return;ErrorGuard error;
    try {std::lock_guard guard(c->lock);c->runtimePipelines.erase(pipeline);for(auto& p:c->pipelines)if(p.handle==pipeline)p={};}catch(...){++c->failures;}
}
bool archivedShaderMatches(ShaderArchive* c,VkPipeline pipeline,std::uint64_t generation,std::string_view sha256) noexcept {
    if(!c||sha256.size()!=64)return false;ErrorGuard error;
    try {std::lock_guard guard(c->lock);
        const auto p=c->runtimePipelines.find(pipeline);
        if(p!=c->runtimePipelines.end()&&p->second.generation==generation)
            return std::equal(sha256.begin(),sha256.end(),p->second.code.sha256.begin());
    }catch(...){++c->failures;}return false;
}
spirv::ImageReads archivedImageReads(ShaderArchive* c,VkPipeline pipeline,std::uint64_t generation) noexcept {
    if(!c||!pipeline||!generation)return {};ErrorGuard error;
    try {std::lock_guard guard(c->lock);
        const auto p=c->runtimePipelines.find(pipeline);
        if(p!=c->runtimePipelines.end()&&p->second.generation==generation&&p->second.readsQualified)return p->second.code.reads;
    }catch(...){++c->failures;}return {};
}
std::array<char,64> archivedShaderFingerprint(ShaderArchive* c,VkPipeline pipeline,std::uint64_t generation) noexcept {
    if(!c||!pipeline||!generation)return {};ErrorGuard error;
    try {std::lock_guard guard(c->lock);
        const auto p=c->runtimePipelines.find(pipeline);
        if(p!=c->runtimePipelines.end()&&p->second.generation==generation)return p->second.code.sha256;
    }catch(...){++c->failures;}return {};
}
void selectArchivedShader(ShaderArchive* c,VkPipeline pipeline,std::uint64_t generation,Sink faultSink) noexcept {
    if(!c)return;ErrorGuard error;
    try {ArchivedPipeline selected;const ArchivedCode* code{};const char* refusal{};bool reportRefusal{};{
        std::lock_guard guard(c->lock);++c->selections;if(c->selectedCount==c->selectionLimit())return;
        for(std::size_t n=0;n<c->selectedCount;++n)
            if(c->selected[n].pipeline==pipeline&&c->selected[n].generation==generation)return;
        for(const auto& p:c->pipelines)if(p.handle==pipeline&&pipeline){selected=p;break;}
        if(!selected.handle)refusal="pipeline_not_recorded";
        else if(!generation||selected.generation!=generation)refusal="pipeline_generation_mismatch";
        else if(selected.codeIndex>=c->retainedModules)refusal="shader_candidate_not_retained";
        else if(!faultSink&&!c->sink)refusal="sink_unavailable";
        else {code=&c->codes[selected.codeIndex];c->selected[c->selectedCount++]={pipeline,generation};}
        if(refusal){++c->selectionRefused;reportRefusal=!c->refusalReported;c->refusalReported=true;}
    }
        if(refusal&&!reportRefusal)return;
        std::ostringstream common;common.imbue(std::locale::classic());common<<",\"pipeline\":";handle(common,bits(pipeline));common<<",\"pipeline_generation\":"<<generation;
        if(refusal){common<<",\"reason\":";quoted(common,refusal);archiveBounds(common,c->fullGraph);c->emit(c->line("sr_shader_archive_refused",common.str()),faultSink);shaderArchiveSummary(c);return;}
        common<<",\"shader\":";handle(common,bits(code->module));common<<",\"shader_generation\":"<<code->generation<<",\"fnv1a64_correlation_only\":";handle(common,code->correlation);
        const auto key=common.str();const auto bytes=code->words.size()*4;const auto chunks=(bytes+shaderArchiveChunkBytes-1)/shaderArchiveChunkBytes;
        std::size_t outputBytes{},outputChunks{},specializationChunks{};const auto& specialization=selected.specialization;const auto& layout=selected.layoutMetadata;
        const bool pipelineMetadataComplete=layout&&layout->complete()&&specialization.captured&&!selected.stageExtensions&&!selected.pipelineExtensions&&!selected.entryTruncated;
        auto output=[&](const char* event,const std::string& fields){const auto line=c->line(event,key+fields);
            if(line.size()+1>shaderArchiveOutputBytes-outputBytes)return false;outputBytes+=line.size()+1;return c->emit(line,faultSink);};
        std::ostringstream begin;begin.imbue(std::locale::classic());begin<<",\"code_bytes\":"<<bytes<<",\"chunk_count\":"<<chunks<<",\"encoding\":\"hex_little_endian\",\"pipeline_entry_point\":";
        quoted(begin,selected.entry.data());begin<<",\"entry_point_truncated\":"<<(selected.entryTruncated?"true":"false")<<",\"specialization_present\":"<<(specialization.present?"true":"false")
            <<",\"specialization_metadata_captured\":"<<(specialization.captured?"true":"false")<<",\"specialization_data_captured\":"<<(specialization.present&&specialization.captured?"true":"false")
            <<",\"pipeline_layout\":";handle(begin,bits(selected.layout));begin<<",\"pipeline_layout_generation\":"<<(layout?layout->generation:0)
            <<",\"pipeline_layout_metadata_complete\":"<<(layout&&layout->complete()?"true":"false")<<",\"pipeline_metadata_complete\":"<<(pipelineMetadataComplete?"true":"false")
            <<",\"pipeline_flags\":"<<selected.flags<<",\"stage_flags\":"<<selected.stageFlags<<",\"pipeline_extensions_present\":"<<(selected.pipelineExtensions?"true":"false")
            <<",\"stage_extensions_present\":"<<(selected.stageExtensions?"true":"false")<<",\"extension_metadata_captured\":false,\"batch_result\":"<<selected.batchResult<<",\"direct_set0_binding_count\":"<<code->set0Bindings
            <<",\"metadata_truncated\":"<<(code->metadataTruncated?"true":"false")<<",\"unresolved_decoration_groups\":"<<(code->decorationGroups?"true":"false")
            <<",\"full_spirv_validation\":false,\"file_persistence_verified\":false";archiveBounds(begin,c->fullGraph);
        bool delivered=output("sr_shader_archive_begin",begin.str());
        constexpr char hex[]="0123456789abcdef";
        std::ostringstream layoutLine;layoutLine.imbue(std::locale::classic());layoutLine<<",\"pipeline_layout\":";handle(layoutLine,bits(selected.layout));
        layoutLine<<",\"pipeline_layout_generation\":"<<(layout?layout->generation:0)<<",\"core_metadata_captured\":"<<(layout&&layout->coreCaptured?"true":"false")
            <<",\"metadata_complete\":"<<(layout&&layout->complete()?"true":"false")<<",\"reason\":";quoted(layoutLine,layout?layout->reason:"layout_not_recorded");
        layoutLine<<",\"flags\":";if(layout)layoutLine<<layout->flags;else layoutLine<<"null";
        layoutLine<<",\"pnext_present\":";if(layout)layoutLine<<(layout->extensions?"true":"false");else layoutLine<<"null";
        layoutLine<<",\"extension_metadata_captured\":false,\"declared_set_count\":";if(layout)layoutLine<<layout->declaredSets;else layoutLine<<"null";
        layoutLine<<",\"declared_push_range_count\":";if(layout)layoutLine<<layout->declaredRanges;else layoutLine<<"null";
        layoutLine<<",\"push_constant_ranges\":[";if(layout)for(std::size_t n=0;n<layout->ranges.size();++n){if(n)layoutLine<<',';const auto& range=layout->ranges[n];layoutLine<<"{\"stage_flags\":"<<range.stageFlags<<",\"offset\":"<<range.offset<<",\"size\":"<<range.size<<'}';}
        layoutLine<<"],\"set_layouts\":[";if(layout)for(std::size_t n=0;n<layout->sets.size();++n){if(n)layoutLine<<',';const auto& set=layout->sets[n];layoutLine<<"{\"set_index\":"<<n<<",\"layout\":";handle(layoutLine,bits(set.handle));layoutLine<<",\"generation\":"<<(set.metadata?set.metadata->generation:0)<<'}';}
        layoutLine<<']';if(delivered)delivered=output("sr_shader_archive_pipeline_layout",layoutLine.str());
        if(layout)for(std::size_t n=0;delivered&&n<layout->sets.size();++n){const auto& set=layout->sets[n];const auto& meta=set.metadata;std::ostringstream setLine;setLine.imbue(std::locale::classic());
            setLine<<",\"set_index\":"<<n<<",\"layout\":";handle(setLine,bits(set.handle));setLine<<",\"layout_generation\":"<<(meta?meta->generation:0)
                <<",\"core_metadata_captured\":"<<(meta&&meta->coreCaptured?"true":"false")<<",\"metadata_complete\":"<<(meta&&meta->coreCaptured&&!meta->extensions?"true":"false")<<",\"reason\":";quoted(setLine,meta?meta->reason:"set_layout_not_recorded");
            setLine<<",\"flags\":";if(meta)setLine<<meta->flags;else setLine<<"null";setLine<<",\"pnext_present\":";if(meta)setLine<<(meta->extensions?"true":"false");else setLine<<"null";
            setLine<<",\"extension_metadata_captured\":false,\"declared_binding_count\":";if(meta)setLine<<meta->declaredBindings;else setLine<<"null";
            setLine<<",\"bindings\":[";if(meta){for(std::size_t b=0;b<meta->bindings.size();++b){if(b)setLine<<',';const auto& binding=meta->bindings[b];setLine<<"{\"binding\":"<<binding.binding<<",\"descriptor_type\":"<<binding.type
                    <<",\"descriptor_count\":"<<binding.count<<",\"stage_flags\":"<<binding.stages<<",\"immutable_samplers_present\":"<<(binding.immutable?"true":"false")<<",\"immutable_samplers\":[";
                for(std::size_t s=0;s<binding.immutableSamplers.size();++s){if(s)setLine<<',';handle(setLine,binding.immutableSamplers[s]);}setLine<<"]}";}}setLine<<']';delivered=output("sr_shader_archive_set_layout",setLine.str());}
        std::ostringstream specializationLine;specializationLine.imbue(std::locale::classic());specializationLine<<",\"present\":"<<(specialization.present?"true":"false")<<",\"metadata_captured\":"<<(specialization.captured?"true":"false")
            <<",\"reason\":";quoted(specializationLine,specialization.reason);specializationLine<<",\"declared_map_count\":"<<specialization.declaredEntries<<",\"declared_data_bytes\":"<<specialization.declaredDataBytes<<",\"data_bytes\":"<<specialization.data.size()<<",\"encoding\":\"hex\",\"map_entries\":[";
        for(std::size_t n=0;n<specialization.entries.size();++n){if(n)specializationLine<<',';const auto& entry=specialization.entries[n];specializationLine<<"{\"constant_id\":"<<entry.constantID<<",\"offset\":"<<entry.offset<<",\"size\":"<<entry.size<<'}';}specializationLine<<']';
        if(delivered)delivered=output("sr_shader_archive_specialization",specializationLine.str());
        for(std::size_t offset=0;delivered&&offset<specialization.data.size();offset+=shaderArchiveChunkBytes){const auto size=std::min(shaderArchiveChunkBytes,specialization.data.size()-offset);std::string data;data.reserve(size*2);
            for(std::size_t n=offset;n<offset+size;++n){data.push_back(hex[specialization.data[n]>>4]);data.push_back(hex[specialization.data[n]&15]);}
            std::ostringstream chunk;chunk.imbue(std::locale::classic());chunk<<",\"offset_bytes\":"<<offset<<",\"data_bytes\":"<<size<<",\"hex\":\""<<data<<'"';delivered=output("sr_shader_archive_specialization_chunk",chunk.str());++specializationChunks;}
        for(std::size_t offset=0;delivered&&offset<bytes;offset+=shaderArchiveChunkBytes){const auto size=std::min(shaderArchiveChunkBytes,bytes-offset);std::string data;data.reserve(2*size);
            for(std::size_t i=offset;i<offset+size;++i){const auto value=(code->words[i/4]>>(8*(i%4)))&255u;data.push_back(hex[value>>4]);data.push_back(hex[value&15]);}
            std::ostringstream chunk;chunk.imbue(std::locale::classic());chunk<<",\"offset_bytes\":"<<offset<<",\"code_bytes\":"<<size<<",\"hex\":\""<<data<<'"';
            delivered=output("sr_shader_archive_chunk",chunk.str());++outputChunks;
        }
        std::ostringstream end;end.imbue(std::locale::classic());end<<",\"code_bytes\":"<<bytes<<",\"chunk_count\":"<<chunks<<",\"chunks_attempted\":"<<outputChunks
            <<",\"all_chunk_sink_calls_returned\":"<<(delivered?"true":"false")<<",\"file_persistence_verified\":false,\"shader_semantics_verified\":false,\"execute_permitted\":false";
        const bool ended=output("sr_shader_archive_end",end.str());
        {std::lock_guard guard(c->lock);c->outputBytes+=outputBytes;c->outputChunks+=outputChunks;c->specializationOutputChunks+=specializationChunks;if(!delivered||!ended)++c->selectionRefused;}
        if(!delivered||!ended)shaderArchiveSummary(c);
    }catch(...){++c->failures;}
}
}
