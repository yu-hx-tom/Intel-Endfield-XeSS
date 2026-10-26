// Production Provider bookkeeping and selection with a CPU-only Vulkan resolver.
// No loader/driver/GPU executes; fake fences cannot establish GPU completion or
// pixel correctness. Actual format conversion has a separate GPU fixture.
#include "continuous_provider.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <latch>
#include <stdexcept>
#include <thread>

namespace {
namespace cp = xess_vk::continuous;
namespace dm = xess_vk::descriptor_image_metadata;
using xess_vk::FrameTimingAssociation;
template<class T> T handle(std::uint64_t value) { return reinterpret_cast<T>(value); }
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::atomic<std::uint64_t> nextHandle{0x100000}, copyCalls{};
std::atomic<VkResult> fenceStatus{VK_SUCCESS};
std::vector<VkImageMemoryBarrier> recordedBarriers;
bool failAllocationOnce{},failBufferBindingOnce{};
bool failQueriesOnce{},queriesUnavailable{};
unsigned queryCreates{},queryReads{},queryWrites{},queryDestroys{};
VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice,const VkQueryPoolCreateInfo* info,const VkAllocationCallbacks*,VkQueryPool* out){
    require(info->queryCount==4&&info->queryType==VK_QUERY_TYPE_TIMESTAMP,"exact four-query Provider scope");++queryCreates;
    if(failQueriesOnce){failQueriesOnce=false;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}*out=handle<VkQueryPool>(nextHandle++);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice,VkQueryPool,const VkAllocationCallbacks*){++queryDestroys;}
VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer,VkQueryPool,std::uint32_t first,std::uint32_t count){require(!first&&count==4,"reset exact owned query range");}
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer,VkPipelineStageFlagBits,VkQueryPool,std::uint32_t index){require(index<4,"owned timestamp index");++queryWrites;}
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice,VkQueryPool,std::uint32_t first,std::uint32_t count,std::size_t size,void* data,VkDeviceSize stride,VkQueryResultFlags flags){
    require(!first&&count==4&&size==64&&stride==16&&!(flags&VK_QUERY_RESULT_WAIT_BIT),"diagnostics never add a query wait");++queryReads;
    if(queriesUnavailable)return VK_NOT_READY;const std::uint64_t values[]{100,1,150,1,10,1,200,1};std::memcpy(data,values,sizeof(values));return VK_SUCCESS;
}
unsigned destroyedBuffers{},freedMemory{};
unsigned depthDispatches{},depthDescriptorWrites{},depthPipelineCreates{},depthPipelinesDestroyed{};
bool failDepthPipeline{},throwDepthDispatch{};
VkPipeline boundComputePipeline{};
std::array<VkDescriptorSet,2> boundComputeSets{};
constexpr std::uint64_t appPipeline=0xD001,appLayout=0xD002,appSet0=0xD003,appSet1=0xD004;

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* out) {
    *out = handle<VkImage>(nextHandle.fetch_add(1)); return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer* out) {
    *out = handle<VkBuffer>(nextHandle.fetch_add(1)); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements* out) { *out = {4096, 256, 1}; }
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements* out) { *out = {4096, 256, 1}; }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* out) {
    if(failAllocationOnce){failAllocationOnce=false;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}
    *out = handle<VkDeviceMemory>(nextHandle.fetch_add(1)); return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
    if(failBufferBindingOnce){failBufferBindingOnce=false;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {++destroyedBuffers;}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {++freedMemory;}
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice, VkFence) { return fenceStatus.load(); }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
    VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*,
    std::uint32_t count, const VkImageMemoryBarrier* images) {
    recordedBarriers.insert(recordedBarriers.end(),images,images+count);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer,
    std::uint32_t, const VkBufferImageCopy*) { copyCalls.fetch_add(1); }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout,
    std::uint32_t, const VkBufferImageCopy*) { copyCalls.fetch_add(1); }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
    std::uint32_t, const VkImageCopy*) { copyCalls.fetch_add(1); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice,const VkDescriptorSetLayoutCreateInfo* info,const VkAllocationCallbacks*,VkDescriptorSetLayout* out) {
    require(info->bindingCount==2&&info->pBindings[0].descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE&&
        info->pBindings[1].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,"depth layout uses samplerless source and typed storage output");
    *out=handle<VkDescriptorSetLayout>(nextHandle++);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice,VkDescriptorSetLayout,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice,const VkPipelineLayoutCreateInfo* info,const VkAllocationCallbacks*,VkPipelineLayout* out) {
    require(info->setLayoutCount==1&&!info->pushConstantRangeCount,"depth pipeline uses no application push-constant bytes");
    *out=handle<VkPipelineLayout>(nextHandle++);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice,VkPipelineLayout,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice,const VkShaderModuleCreateInfo* info,const VkAllocationCallbacks*,VkShaderModule* out) {
    require(info->codeSize>20&&info->pCode[0]==0x07230203,"depth shader is embedded SPIR-V");
    *out=handle<VkShaderModule>(nextHandle++);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice,VkShaderModule,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice,VkPipelineCache,std::uint32_t,const VkComputePipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline* out) {
    ++depthPipelineCreates;
    *out=handle<VkPipeline>(nextHandle++);return failDepthPipeline?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice,VkPipeline,const VkAllocationCallbacks*) {++depthPipelinesDestroyed;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice,const VkDescriptorPoolCreateInfo*,const VkAllocationCallbacks*,VkDescriptorPool* out) {
    *out=handle<VkDescriptorPool>(nextHandle++);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice,VkDescriptorPool,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* out) {
    for(unsigned i=0;i<info->descriptorSetCount;++i)out[i]=handle<VkDescriptorSet>(nextHandle++);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice,std::uint32_t count,const VkWriteDescriptorSet* writes,std::uint32_t,const VkCopyDescriptorSet*) {
    require(count==2&&writes[0].pImageInfo->imageLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&
        writes[1].pImageInfo->imageLayout==VK_IMAGE_LAYOUT_GENERAL,"source sampled layout and GENERAL storage output are exact");++depthDescriptorWrites;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer,VkPipelineBindPoint point,VkPipeline pipeline) {
    require(point==VK_PIPELINE_BIND_POINT_COMPUTE,"only compute state is changed");boundComputePipeline=pipeline;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout layout,std::uint32_t first,
        std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t dynamic,const std::uint32_t*) {
    require(!first&&!dynamic&&count<=2,"depth insertion uses static descriptors and restores the full saved range");
    if(layout!=handle<VkPipelineLayout>(appLayout))boundComputeSets={};
    for(unsigned i=0;i<count;++i)boundComputeSets[i]=sets[i];
}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer,std::uint32_t x,std::uint32_t y,std::uint32_t z) {
    require(x==1&&y==1&&z==1&&boundComputePipeline!=handle<VkPipeline>(appPipeline)&&!boundComputeSets[1],
        "shader dispatch uses own set0 after incompatible bind disturbed the higher application set");
    ++depthDispatches;if(throwDepthDispatch)throw std::runtime_error("injected depth command exception");
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL resolver(VkDevice, const char* name) {
#define FUNCTION(symbol) if (!std::strcmp(name, #symbol)) return reinterpret_cast<PFN_vkVoidFunction>(&symbol)
    FUNCTION(vkCreateImage); FUNCTION(vkCreateBuffer);
    FUNCTION(vkGetImageMemoryRequirements); FUNCTION(vkGetBufferMemoryRequirements);
    FUNCTION(vkAllocateMemory); FUNCTION(vkBindImageMemory); FUNCTION(vkBindBufferMemory);
    FUNCTION(vkDestroyImage); FUNCTION(vkDestroyBuffer); FUNCTION(vkFreeMemory);
    FUNCTION(vkGetFenceStatus); FUNCTION(vkCmdPipelineBarrier);
    FUNCTION(vkCreateQueryPool);FUNCTION(vkDestroyQueryPool);FUNCTION(vkCmdResetQueryPool);FUNCTION(vkCmdWriteTimestamp);FUNCTION(vkGetQueryPoolResults);
    FUNCTION(vkCmdCopyImageToBuffer); FUNCTION(vkCmdCopyBufferToImage); FUNCTION(vkCmdCopyImage);
    FUNCTION(vkCreateDescriptorSetLayout);FUNCTION(vkDestroyDescriptorSetLayout);FUNCTION(vkCreatePipelineLayout);FUNCTION(vkDestroyPipelineLayout);
    FUNCTION(vkCreateShaderModule);FUNCTION(vkDestroyShaderModule);FUNCTION(vkCreateComputePipelines);FUNCTION(vkDestroyPipeline);
    FUNCTION(vkCreateDescriptorPool);FUNCTION(vkDestroyDescriptorPool);FUNCTION(vkAllocateDescriptorSets);FUNCTION(vkUpdateDescriptorSets);
    FUNCTION(vkCmdBindPipeline);FUNCTION(vkCmdBindDescriptorSets);FUNCTION(vkCmdDispatch);
#undef FUNCTION
    return nullptr;
}
cp::Config config() {
    cp::Config result;
    result.device = handle<VkDevice>(1); result.queue = handle<VkQueue>(2);
    result.queueFamily = 0; result.functions = resolver;
    result.memory.memoryTypeCount = 1;
    result.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    return result;
}
struct Fixture {
    cp::Provider provider;
    explicit Fixture(cp::Config settings=config()):provider(settings){}
    ~Fixture() { fenceStatus = VK_SUCCESS; provider.stop(handle<VkFence>(3)); }
};
cp::SourcePair pair(std::uint32_t frame, std::uint64_t invocation) {
    cp::SourcePair result;
    result.depth = {0x100, 11}; result.motion = {0x200, 22};
    result.invocation = invocation; result.engineFrame = frame;
    result.timing = {frame + 1000, 7, true}; result.frameValid = result.valid = true;
    result.constants.producerToken = invocation;
    result.constants.jitter = {static_cast<float>(invocation % 127) / 256.0f, -0.125f};
    return result;
}
FrameTimingAssociation scope(const cp::SourcePair& source) {
    FrameTimingAssociation result;
    result.engineFrame = source.engineFrame; result.ticket = source.timing; result.valid = true;
    return result;
}
xess_vk::CommandRecordingFacts facts(std::uint64_t ordinal) {
    xess_vk::CommandRecordingFacts result;
    result.device = 1; result.contextIncarnation = 31; // Not the SDK context namespace.
    result.command = 0x300 + ordinal; result.rpRecordingGeneration = 20000 + ordinal;
    result.pool = 0x400; result.poolGeneration = 1; result.queueFamily = 0;
    result.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    result.beginUsageFlags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result.allocationKnown = result.poolKnown = result.beginFlagsKnown = result.stateKnown = true;
    result.recording = result.scopeKnown = true; result.reasons = 0;
    return result;
}
dm::DispatchEvidence evidence(const cp::SourcePair& source, const xess_vk::CommandRecordingFacts& recording) {
    dm::DispatchEvidence result;
    result.metadataAccepted = true; result.count = 2;
    result.command = {recording.command, 90000}; // Descriptor generation is distinct from RP generation.
    for (std::size_t i = 0; i != result.count; ++i) {
        auto& binding = result.bindings[i];
        const auto format = i ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        binding.setIndex = 0; binding.binding = i ? 7 : 5;
        binding.image = i ? source.motion : source.depth;
        binding.viewType = VK_IMAGE_VIEW_TYPE_2D; binding.viewFormat = format;
        binding.range = {i ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        binding.declaredLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        auto& d = binding.imageDescription;
        d.format = format; d.width = 2; d.height = 2; d.depth = d.mipLevels = d.arrayLayers = 1;
        d.samples = VK_SAMPLE_COUNT_1_BIT; d.tiling = VK_IMAGE_TILING_OPTIMAL;
        d.sharingMode = VK_SHARING_MODE_EXCLUSIVE; d.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    return result;
}
void refuse(cp::Provider& provider, const cp::SourcePair& source, FrameTimingAssociation recordingFrame,
            dm::DispatchEvidence dispatch, std::uint64_t ordinal, const char* message) {
    const auto before = copyCalls.load();
    require(!provider.record(GetCurrentThreadId(), facts(ordinal), dispatch, recordingFrame), message);
    require(copyCalls.load() == before, "refused association recorded no copy commands");
    (void)source;
}
cp::Ticket capture(cp::Provider& provider, const cp::SourcePair& wanted, std::uint64_t ordinal) {
    const auto recording = facts(ordinal);
    const auto before = copyCalls.load();
    require(provider.record(GetCurrentThreadId(), recording, evidence(wanted, recording), scope(wanted)),
            "exact frame/ticket/image generations record");
    require(copyCalls.load() == before + 3, "successful production record emits its three copies");
    const auto signal = handle<VkSemaphore>(0x500 + ordinal);
    provider.submitted({recording.command, recording.rpRecordingGeneration}, std::span(&signal, 1),
                       handle<VkQueue>(2), ordinal, VK_SUCCESS);
    const auto bundle = provider.acquireForPresent(handle<VkQueue>(2), std::span(&signal, 1), handle<VkFence>(3), provider.submittedCutoff(handle<VkQueue>(2)));
    require(bundle.ready && bundle.engineFrame == wanted.engineFrame && bundle.timing == wanted.timing &&
            bundle.invocation == wanted.invocation && bundle.constants.producerToken == wanted.invocation &&
            bundle.constants.jitter == wanted.constants.jitter, "bundle retains exactly the selected producer");
    require(bundle.constants.commandBuffer == recording.command &&
            bundle.constants.recordingGeneration == recording.rpRecordingGeneration &&
            bundle.constants.deviceIncarnation == recording.contextIncarnation,
            "selected packet is bound to the actual RP recording identity");
    provider.releaseAfterBridge(bundle.ticket);
    const auto consumed = provider.stats().consumed;
    provider.releaseAfterBridge(bundle.ticket);
    require(provider.stats().consumed == consumed &&
            !provider.acquireForPresent(handle<VkQueue>(2), std::span(&signal, 1), handle<VkFence>(3), provider.submittedCutoff(handle<VkQueue>(2))).ready,
            "snapshot ticket and submitted producer cannot be consumed twice");
    return bundle.ticket;
}
void submitCapture(cp::Provider& provider, cp::SourcePair source, std::uint64_t ordinal,
                   VkSemaphore signal, VkResult result = VK_SUCCESS) {
    source.thread = GetCurrentThreadId();
    provider.invocation(source);
    const auto recording = facts(ordinal);
    require(provider.record(GetCurrentThreadId(), recording, evidence(source, recording), scope(source)),
            "test capture records through actual Provider");
    provider.submitted({recording.command, recording.rpRecordingGeneration}, std::span(&signal, 1),
                       handle<VkQueue>(2), ordinal, result);
}
void measurementQueries(){
    auto settings=config();settings.gpuTimestamps=true;settings.timestampPeriodNs=2;settings.timestampValidBits=64;
    const auto reads=queryReads,created=queryCreates,destroyed=queryDestroys,written=queryWrites;
    {
        Fixture f(settings);
        auto window=std::make_unique<xess_vk::measurement::Window>(1);auto result=std::make_unique<xess_vk::measurement::Snapshot>();
        xess_vk::measurement::Scope measured(window.get());
        for(unsigned i=0;i<2;++i){
            const auto source=pair(3500+i,4500+i);const auto recording=facts(3500+i);f.provider.invocation(source);
            require(f.provider.record(0,recording,evidence(source,recording),scope(source)),"timestamp capture remains functional");
            const auto signal=handle<VkSemaphore>(9000+i);
            f.provider.submitted({recording.command,recording.rpRecordingGeneration},std::span(&signal,1),handle<VkQueue>(2),i+1,VK_SUCCESS,16'000'000ull*(i+1));
            const auto bundle=f.provider.acquireQueuedForPresent(handle<VkQueue>(2),f.provider.submittedCutoff(handle<VkQueue>(2)),scope(source));
            require(bundle.ready&&bundle.commands[0]==1&&bundle.commands[1]==1&&bundle.commands[2]==1,"actual three issued input copies retain same-frame identity");
            require(queryReads==reads+i,"pending producer queried before existing completion proof");
            queriesUnavailable=i==1;require(f.provider.releaseAfterBridge(bundle.ticket),"completed producer released");queriesUnavailable=false;
        }
        const auto stats=f.provider.stats();require(stats.timestampCompleted==1&&stats.timestampUnavailable==1&&stats.depthGpu.durationNs==100&&stats.auxGpu.durationNs==380,
            "completed depth/aux scopes and unavailable result remain distinct");
        require(window->take(1'000'000'001,*result),"measurement window complete");
        require(result->metrics[static_cast<unsigned>(xess_vk::measurement::Metric::SourceSubmitInterval)].count==1,"real captured submission interval sampled once");
    }
    require(queryCreates>created&&queryDestroys>destroyed,"query resources follow Provider cleanup");
    require(queryWrites==written+8,"each successful recording emits exactly four timestamp commands");
    {Fixture f(settings);failQueriesOnce=true;const auto source=pair(3600,4600);f.provider.invocation(source);const auto recording=facts(3600);
        require(f.provider.record(0,recording,evidence(source,recording),scope(source))&&f.provider.stats().timestampFailures==1,
            "optional query allocation failure does not prevent original input capture");f.provider.retirePool(recording.pool);}
    std::puts("PASS Provider diagnostic queries: exact identity, actual command counts, nonblocking completed reads, unavailable/allocation fallback and cleanup; CPU mocks only");
}
void measuredSubmissionCommands() {
    using namespace xess_vk::measurement;
    unsigned cases{};
    const auto run=[&](VkResult result,VkQueue queue,bool oversizedSignals,bool discard) {
        auto window=std::make_unique<Window>(1);Scope measured(window.get());
        Fixture f;auto source=pair(9000+cases,9100+cases);source.thread=GetCurrentThreadId();
        f.provider.invocation(source);const auto recording=facts(9200+cases);const auto before=copyCalls.load();
        require(f.provider.record(GetCurrentThreadId(),recording,evidence(source,recording),scope(source)),
            "measured production capture records");
        require(copyCalls.load()==before+3,"measurement follows three actual transfer commands");
        SummarySnapshot summary;
        require(window->takeSummary(2,summary,true)&&summary.copyCommands==decltype(summary.copyCommands){},
            "recording without successful submission never counts executed copies");
        if(discard)f.provider.retireUnsubmitted({recording.command,recording.rpRecordingGeneration});
        std::array<VkSemaphore,cp::maximumSignals+1> signals{};
        for(unsigned i=0;i<signals.size();++i)signals[i]=handle<VkSemaphore>(0xAB000+i);
        const auto count=oversizedSignals?signals.size():1;
        for(unsigned repeat=0;repeat<2;++repeat)
            f.provider.submitted({recording.command,recording.rpRecordingGeneration},std::span(signals.data(),count),
                queue,cases+repeat+1,result);
        require(window->takeSummary(3,summary,true),"submitted command summary available");
        decltype(summary.copyCommands) expected{};
        if(result==VK_SUCCESS&&!discard) {
            expected[static_cast<unsigned>(CopyRole::Depth)][static_cast<unsigned>(Command::ImageToBuffer)]=1;
            expected[static_cast<unsigned>(CopyRole::Depth)][static_cast<unsigned>(Command::BufferToImage)]=1;
            expected[static_cast<unsigned>(CopyRole::Motion)][static_cast<unsigned>(Command::ImageCopy)]=1;
        }
        require(summary.copyCommands==expected,
            "only successful submitted commands count once, including rejected ownership; failure/discard never count");
        require(f.provider.stats().copyFailures==std::uint64_t(result!=VK_SUCCESS&&!discard),
            "failed copy submission is counted once; repeated observations and ownership refusals do not invent failures");
        ++cases;
    };
    for(auto result:std::array{VK_SUCCESS,VK_ERROR_OUT_OF_HOST_MEMORY,VK_ERROR_OUT_OF_DEVICE_MEMORY,VK_ERROR_DEVICE_LOST,VK_ERROR_UNKNOWN})
        for(bool oversized:std::array{false,true})run(result,handle<VkQueue>(2),oversized,false);
    run(VK_SUCCESS,handle<VkQueue>(3),false,false);
    run(VK_SUCCESS,handle<VkQueue>(2),false,true);
    std::printf("PASS Provider measured submissions: %u success/OOM/lost/unknown, duplicate, signal-limit, queue and discard cases; CPU mocks only\n",cases);
}
void observedOffConsumesWithoutCopies() {
    Fixture f;
    const auto recorded=pair(900,9001),submitted=pair(901,9002),consuming=pair(902,9003);
    f.provider.invocation(recorded);const auto first=facts(900);
    require(f.provider.record(0,first,evidence(recorded,first),scope(recorded)),"existing Recorded owner established");
    const auto firstSignal=handle<VkSemaphore>(0x9901),secondSignal=handle<VkSemaphore>(0x9902);
    submitCapture(f.provider,submitted,901,firstSignal);submitCapture(f.provider,consuming,902,secondSignal);
    const auto held=f.provider.acquireForPresent(handle<VkQueue>(2),std::span(&secondSignal,1),handle<VkFence>(3),
        f.provider.submittedCutoff(handle<VkQueue>(2)));
    require(held.ready,"existing Consuming owner established");
    const auto states=f.provider.stats().states;
    const auto copies=copyCalls.load(),handles=nextHandle.load();
    for(unsigned i=0;i<64;++i) {
        const auto source=pair(1000+i,10000+i);const auto recording=facts(1000+i);
        f.provider.invocation(source);
        require(!f.provider.record(0,recording,evidence(source,recording),scope(source),false),
                "explicit OFF matches and consumes the source without producing a bundle");
    }
    const auto skipped=f.provider.stats();
    require(skipped.skippedOffCaptures==64 && skipped.matchedDispatches==67 && skipped.captured==3 &&
            !skipped.invocationCapacityRefusals && !skipped.saturated && skipped.states==states &&
            copyCalls.load()==copies && nextHandle.load()==handles,
            "OFF longer than invocation capacity adds no GPU copies/allocations or slot mutations even when all GPU slots are held");
    const auto previous=pair(1063,10063);const auto previousRecording=facts(1063);
    require(!f.provider.record(0,previousRecording,evidence(previous,previousRecording),scope(previous),false) &&
            f.provider.stats().skippedOffCaptures==64,
            "an OFF source is consumed exactly once rather than retained as an old candidate");
    f.provider.releaseAfterBridge(held.ticket);
    const auto next=pair(1064,10064);const auto recording=facts(1064);f.provider.invocation(next);
    require(f.provider.record(0,recording,evidence(next,recording),scope(next)) && copyCalls.load()==copies+3,
            "capture remains the default and resumes after OFF on a normally returned slot");
    const auto duplicate=pair(1100,11000);f.provider.invocation(duplicate);f.provider.invocation(duplicate);
    const auto duplicateRecording=facts(1100);
    require(!f.provider.record(0,duplicateRecording,evidence(duplicate,duplicateRecording),scope(duplicate),false) &&
            f.provider.stats().skippedOffCaptures==64 && f.provider.stats().ambiguousDispatches==1,
            "OFF policy does not bypass ambiguity checks or count an invalid source as skipped");
}
void queuedExactLease() {
    const auto queue=handle<VkQueue>(2),otherQueue=handle<VkQueue>(4);
    const auto fence=handle<VkFence>(3);
    const auto signal=handle<VkSemaphore>(0xFA01);
    {
        Fixture f;
        const auto source=pair(1300,13001);
        submitCapture(f.provider,source,1300,signal);
        const auto cutoff=f.provider.submittedCutoff(queue);
        fenceStatus=VK_NOT_READY;
        require(!f.provider.acquireForPresent(queue,std::span(&signal,1),fence,cutoff,scope(source)).ready,
                "synchronous acquisition still requires real fence completion");
        auto wrong=scope(source);++wrong.ticket.contextGeneration;
        require(!f.provider.acquireQueuedForPresent(otherQueue,cutoff,scope(source)).ready &&
                !f.provider.acquireQueuedForPresent(queue,cutoff,wrong).ready &&
                !f.provider.acquireQueuedForPresent(queue,0,scope(source)).ready,
                "queued acquisition refuses another queue/context and post-cutoff submission");
        wrong=scope(source);++wrong.engineFrame;
        require(!f.provider.acquireQueuedForPresent(queue,cutoff,wrong).ready,
                "queued acquisition requires the exact engine frame as well as ticket");
        const auto held=f.provider.acquireQueuedForPresent(queue,cutoff,scope(source));
        require(held.ready && held.timing==source.timing && held.constants.producerToken==source.invocation &&
                f.provider.stats().states[held.ticket.slot]==cp::State::Consuming && !f.provider.stats().consumed,
                "successful Submitted metadata may be pinned while its GPU producer remains pending");
        require(!f.provider.acquireQueuedForPresent(queue,cutoff,scope(source)).ready,
                "a held queued lease cannot be borrowed twice");
        f.provider.rollbackQueuedAcquire({held.ticket.slot,held.ticket.generation+1});
        require(f.provider.stats().states[held.ticket.slot]==cp::State::Consuming,
                "stale generation cannot roll back another queued owner");
        f.provider.rollbackQueuedAcquire(held.ticket);
        require(f.provider.stats().states[held.ticket.slot]==cp::State::Submitted && !f.provider.stats().consumed,
                "unpublished rollback preserves GPU resource ownership instead of freeing or counting consumption");
        const auto retried=f.provider.acquireQueuedForPresent(queue,cutoff,scope(source));
        require(retried.ready && retried.ticket==held.ticket,"rollback retains the same generation and exact source");
        fenceStatus=VK_SUCCESS; // The caller's modeled Bridge read/restore completion proof.
        f.provider.releaseAfterBridge(retried.ticket);
        require(f.provider.stats().states[held.ticket.slot]==cp::State::Free && f.provider.stats().consumed==1,
                "actual completion finally returns the queued slot once");
    }
    {
        Fixture f;
        const auto older=pair(1400,14001),wanted=pair(1401,14002),future=pair(1402,14003);
        submitCapture(f.provider,older,1400,signal);submitCapture(f.provider,wanted,1401,signal);
        const auto cutoff=f.provider.submittedCutoff(queue);
        submitCapture(f.provider,future,1402,signal);
        fenceStatus=VK_NOT_READY;
        const auto held=f.provider.acquireQueuedForPresent(queue,cutoff,scope(wanted));
        require(held.ready && !f.provider.stats().cancelled,"queued claim never retires older pending GPU writes");
        f.provider.retireCompletedForPresent(queue,fence,cutoff,scope(wanted));
        require(!f.provider.stats().cancelled,"pending copy fence cannot recycle older queued inputs");
        fenceStatus=VK_SUCCESS;
        f.provider.retireCompletedForPresent(otherQueue,fence,cutoff,scope(wanted));
        require(!f.provider.stats().cancelled,"another queue cannot supply retirement proof");
        f.provider.retireCompletedForPresent(queue,fence,cutoff,scope(wanted));
        const auto state=f.provider.stats();
        require(state.cancelled==1 && state.states[held.ticket.slot]==cp::State::Consuming &&
                std::count(state.states.begin(),state.states.end(),cp::State::Submitted)==1,
                "real completion retires only older covered IDs, retaining held and post-cutoff future inputs");
        f.provider.releaseAfterBridge(held.ticket);
        const auto next=f.provider.acquireQueuedForPresent(queue,f.provider.submittedCutoff(queue),scope(future));
        require(next.ready,"future producer remains usable after earlier queued retirement");
        f.provider.releaseAfterBridge(next.ticket);
    }
    {
        Fixture f;
        const auto source=pair(1500,15001);
        submitCapture(f.provider,source,1500,signal);submitCapture(f.provider,source,1501,signal);
        fenceStatus=VK_NOT_READY;
        const auto bundle=f.provider.acquireQueuedForPresent(queue,f.provider.submittedCutoff(queue),scope(source));
        const auto state=f.provider.stats();
        require(!bundle.ready && !state.cancelled &&
                std::count(state.states.begin(),state.states.end(),cp::State::Free)==1,
                "ambiguous queued sources are refused without unproved GPU-slot recycling");
    }
    std::puts("PASS queued exact Provider lease: pending/same-queue/cutoff/full-ticket, unpublished rollback, real-completion retirement, ambiguity/future/held protection");
}
void unkeyedFailureRevokesPending() {
    for(const bool parsedValid:{false,true}) {
        Fixture f;
        auto recorded=pair(900,9001);f.provider.invocation(recorded);const auto recording=facts(900);
        require(f.provider.record(0,recording,evidence(recorded,recording),scope(recorded)),"owned Recorded slot established");
        const auto submitted=pair(901,9002),consuming=pair(902,9003);
        const auto firstSignal=handle<VkSemaphore>(0x9901),secondSignal=handle<VkSemaphore>(0x9902);
        submitCapture(f.provider,submitted,901,firstSignal);submitCapture(f.provider,consuming,902,secondSignal);
        const auto held=f.provider.acquireForPresent(handle<VkQueue>(2),std::span(&secondSignal,1),handle<VkFence>(3),
            f.provider.submittedCutoff(handle<VkQueue>(2)));
        require(held.ready,"owned Consuming slot established");
        const auto before=f.provider.stats().states;
        require(std::count(before.begin(),before.end(),cp::State::Recorded)==1&&
                std::count(before.begin(),before.end(),cp::State::Submitted)==1&&
                std::count(before.begin(),before.end(),cp::State::Consuming)==1,"all occupied GPU ownership classes present");
        const std::array pending{pair(910,9101),pair(911,9102)};
        for(const auto& source:pending)f.provider.invocation(source);
        auto failed=pair(912,9103);failed.valid=parsedValid;
        if(parsedValid)failed.timing={};else failed.frameValid=false;
        f.provider.invocation(failed);
        for(std::size_t i=0;i<pending.size();++i)
            refuse(f.provider,pending[i],scope(pending[i]),evidence(pending[i],facts(910+i)),910+i,
                "unknown camera/frame failure revokes every pending CPU association");
        require(f.provider.stats().states==before,"unkeyed failure never releases or alters an occupied GPU slot");
        f.provider.releaseAfterBridge(held.ticket);
        auto recovered=pair(913,9104);f.provider.invocation(recovered);capture(f.provider,recovered,913);
        require(f.provider.stats().consumed==2,"a new exact association recovers after conservative invalidation");
    }
}
void completedSubmissionBoundaries() {
    const auto queue = handle<VkQueue>(2);
    const auto fence = handle<VkFence>(3);
    const auto forwarded = handle<VkSemaphore>(0x800);
    {
        Fixture f;
        std::array sources{pair(700, 7001), pair(701, 7002), pair(702, 7003)};
        for (std::size_t i = 0; i < sources.size(); ++i)
            submitCapture(f.provider, sources[i], 100 + i, handle<VkSemaphore>(0x900 + i));
        const auto cutoff = f.provider.submittedCutoff(queue); // Before the modeled private submit.
        auto future = pair(703, 7004); future.thread = GetCurrentThreadId();
        f.provider.invocation(future);
        refuse(f.provider, future, scope(future), evidence(future, facts(110)), 110,
               "three submitted captures exhaust slots before a consumer completes");
        for (const auto incomplete : {VK_NOT_READY, VK_TIMEOUT, VK_ERROR_DEVICE_LOST}) {
            fenceStatus = incomplete;
            require(!f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence, cutoff, scope(sources[2])).ready,
                    "unfinished or failed private copy cannot select or recycle captures");
            const auto state = f.provider.stats();
            require(std::count(state.states.begin(), state.states.end(), cp::State::Submitted) == 3 && !state.cancelled,
                    "no completed-fence proof preserves all submitted captures");
        }
        fenceStatus = VK_SUCCESS;
        require(!f.provider.acquireForPresent(handle<VkQueue>(4), std::span(&forwarded, 1), fence, cutoff, scope(sources[2])).ready &&
                !f.provider.stats().cancelled, "another queue cannot supply completion proof");
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto bundle = f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence, cutoff, scope(sources[i]));
            require(bundle.ready && bundle.timing == sources[i].timing && bundle.engineFrame == sources[i].engineFrame &&
                    bundle.invocation == sources[i].invocation && bundle.submission == 100 + i,
                    "S1-to-S2 forwarding selects the exact completed frame, never the latest");
            const auto state = f.provider.stats();
            require(static_cast<std::size_t>(std::count(state.states.begin(), state.states.end(), cp::State::Submitted)) == 2 - i && !state.cancelled,
                    "normal three-frame pre-submission preserves every future capture");
            f.provider.releaseAfterBridge(bundle.ticket);
        }
        submitCapture(f.provider, future, 110, forwarded);
        const auto bundle = f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence,
                                                         f.provider.submittedCutoff(queue), scope(future));
        require(bundle.ready && f.provider.stats().consumed == 3, "capture continues after the three-slot forwarding sequence");
        f.provider.releaseAfterBridge(bundle.ticket);
    }
    {
        Fixture f;
        for (std::uint32_t i = 0; i < 3; ++i)
            submitCapture(f.provider, pair(800 + i, 8001 + i), 120 + i, handle<VkSemaphore>(0xA00 + i));
        const auto current = pair(803, 8004);
        require(!f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence,
                    f.provider.submittedCutoff(queue), scope(current)).ready,
                "missing current capture never substitutes an older completed frame");
        const auto state = f.provider.stats();
        require(state.cancelled == 3 && std::count(state.states.begin(), state.states.end(), cp::State::Free) == 3,
                "only completed obsolete SDK IDs retire and unblock the full table");
        auto next = pair(804, 8005);
        submitCapture(f.provider, next, 124, forwarded);
        const auto bundle = f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence,
                                                         f.provider.submittedCutoff(queue), scope(next));
        require(bundle.ready, "retired completed slots can capture again");
        f.provider.releaseAfterBridge(bundle.ticket);
    }
    {
        Fixture f;
        auto old = pair(900, 9001), late = pair(901, 9002);
        submitCapture(f.provider, old, 130, forwarded);
        late.thread = GetCurrentThreadId(); f.provider.invocation(late);
        const auto recording = facts(131);
        require(f.provider.record(GetCurrentThreadId(), recording, evidence(late, recording), scope(late)),
                "later submission can already have an older recording reservation");
        const auto cutoff = f.provider.submittedCutoff(queue);
        std::thread submitter([&] {
            f.provider.submitted({recording.command, recording.rpRecordingGeneration}, std::span(&forwarded, 1), queue, 131, VK_SUCCESS);
        });
        submitter.join();
        require(!f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence, cutoff, scope(late)).ready,
                "post-cutoff submission is not selected even after signal-handle reuse");
        auto state = f.provider.stats();
        require(state.cancelled == 1 && std::count(state.states.begin(), state.states.end(), cp::State::Submitted) == 1,
                "old fence proof retires old input but leaves the concurrently submitted input live");
        const auto bundle = f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence,
                                                         f.provider.submittedCutoff(queue), scope(late));
        require(bundle.ready && bundle.invocation == late.invocation, "a later copy proof can admit that exact new submission");
        f.provider.releaseAfterBridge(bundle.ticket);
    }
    {
        Fixture f;
        auto source = pair(950, 9501); source.thread = GetCurrentThreadId();
        f.provider.invocation(source); const auto recording = facts(135);
        require(f.provider.record(GetCurrentThreadId(), recording, evidence(source, recording), scope(source)),
                "capture can be recorded in a batch without signal semaphores");
        f.provider.submitted({recording.command, recording.rpRecordingGeneration}, {}, queue, 135, VK_SUCCESS);
        const auto cutoff = f.provider.submittedCutoff(queue);
        require(!f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence, cutoff).ready,
                "no-ticket wait matching cannot select a signal-free capture");
        const auto bundle = f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence, cutoff, scope(source));
        require(bundle.ready && bundle.invocation == source.invocation,
                "exact completed ticket selects a signal-free producer followed by a signalling batch");
        f.provider.releaseAfterBridge(bundle.ticket);
    }
    {
        Fixture f;
        auto wanted = pair(1000, 10001), duplicate = pair(1000, 10002);
        duplicate.depth.handle += 0x10; duplicate.motion.handle += 0x10;
        submitCapture(f.provider, wanted, 140, forwarded);
        submitCapture(f.provider, duplicate, 141, handle<VkSemaphore>(0xB00));
        require(!f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence,
                    f.provider.submittedCutoff(queue), scope(wanted)).ready && f.provider.stats().ambiguousPresent == 1,
                "duplicate exact ticket/frame remains ambiguous despite different original signals");
        auto foreign = pair(1001, 10003); foreign.timing.contextGeneration += 1;
        submitCapture(f.provider, foreign, 142, forwarded);
        auto wrong = scope(foreign); wrong.ticket.contextGeneration -= 1;
        const auto foreignMatch = f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence,
                    f.provider.submittedCutoff(queue), wrong);
        const auto foreignState = f.provider.stats();
        require(!foreignMatch.ready &&
                std::count(foreignState.states.begin(), foreignState.states.end(), cp::State::Submitted) == 1,
                "same frame from another SDK context never matches or retires");
        wrong = scope(foreign); --wrong.ticket.sdkId;
        require(!f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence,
                    f.provider.submittedCutoff(queue), wrong).ready,
                "same engine frame with another ticket never matches by semaphore fallback");
        wrong = scope(foreign); ++wrong.engineFrame;
        require(!f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence,
                    f.provider.submittedCutoff(queue), wrong).ready,
                "same complete ticket with another engine frame never matches");
    }
    {
        Fixture f;
        const auto consuming = pair(1100, 11001);
        submitCapture(f.provider, consuming, 150, forwarded);
        const auto bundle = f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence,
                                                         f.provider.submittedCutoff(queue), scope(consuming));
        require(bundle.ready, "retained consuming test obtains its actual lease");
        submitCapture(f.provider, pair(1101, 11002), 151, forwarded, VK_ERROR_DEVICE_LOST);
        auto recorded = pair(1102, 11003); recorded.thread = GetCurrentThreadId();
        f.provider.invocation(recorded); const auto recording = facts(152);
        require(f.provider.record(GetCurrentThreadId(), recording, evidence(recorded, recording), scope(recorded)), "record-only slot exists");
        const auto cutoff = f.provider.submittedCutoff(queue);
        require(cutoff == 1 && !f.provider.acquireForPresent(queue, std::span(&forwarded, 1), fence,
                    cutoff, scope(pair(1200, 12001))).ready,
                "failed submit does not advance the success cutoff");
        const auto state = f.provider.stats();
        require(std::count(state.states.begin(), state.states.end(), cp::State::Consuming) == 1 &&
                std::count(state.states.begin(), state.states.end(), cp::State::Recorded) == 1 &&
                std::count(state.states.begin(), state.states.end(), cp::State::Uncertain) == 1 && !state.cancelled,
                "completed-copy retirement never releases Consuming, Recorded or Uncertain storage");
        f.provider.releaseAfterBridge(bundle.ticket);
    }
    std::puts("PASS completed capture cutoff: forwarded semaphores, exact unique frames, future preservation, obsolete recycling, pending/error/queue refusal, concurrent submission and protected states; CPU mock only");
}

struct SharedInputs {
    enum class State { Free,Recorded,Submitted,Poisoned };
    struct Slot { xess_vk::SharedPlaneKey key;State state{}; };
    std::array<Slot,xess_vk::sharedPlaneSlotCount> slots;
    std::uint64_t serial{};
    unsigned writes{},cancelled{},abandoned{},released{},poisoned{};
    bool tailComplete{};
    bool depthStorage{};
    Slot* find(xess_vk::SharedPlaneKey key) {
        return key.valid()&&slots[key.index].key==key?&slots[key.index]:nullptr;
    }
    cp::SharedInputAccess access() {
        cp::SharedInputAccess out;out.context=this;
        out.reserve=[](void* context,VkExtent2D extent,xess_vk::SharedPlaneLease& lease) noexcept {
            auto& self=*static_cast<SharedInputs*>(context);lease={};
            for(unsigned i=0;i<self.slots.size();++i)if(self.slots[i].state==State::Free) {
                auto& slot=self.slots[i];slot.key={i,17,++self.serial};slot.state=State::Recorded;
                lease.key=slot.key;lease.auxiliary=true;
                lease.depthStorage=self.depthStorage;if(self.depthStorage)lease.depthStorageView=handle<VkImageView>(0xE000+i);
                for(unsigned p=1;p<=2;++p) {
                    auto& image=lease.planes[p];image.device=handle<VkDevice>(1);
                    image.image=handle<VkImage>(0xA000+i*3+p);image.ownerQueueFamily=VK_QUEUE_FAMILY_EXTERNAL;
                    image.currentLayout=VK_IMAGE_LAYOUT_GENERAL;
                    auto& info=image.successfulImageCreateInfo;
                    info.format=p==1?VK_FORMAT_R32_SFLOAT:VK_FORMAT_R16G16_SFLOAT;
                    info.extent={extent.width,extent.height,1};info.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                }
                return xess_vk::SharedBorrowResult::Acquired;
            }
            return xess_vk::SharedBorrowResult::Unavailable;
        };
        out.submitted=[](void* context,xess_vk::SharedPlaneKey key,std::uint32_t mask) {
            auto& self=*static_cast<SharedInputs*>(context);auto* slot=self.find(key);
            require(slot&&slot->state==State::Recorded&&mask==6,"actual source submit marks exactly the shared depth/MV planes");
            slot->state=State::Submitted;++self.writes;
        };
        out.cancel=[](void* context,xess_vk::SharedPlaneKey key) noexcept {
            auto& self=*static_cast<SharedInputs*>(context);auto* slot=self.find(key);
            if(!slot||slot->state!=State::Recorded)return false;
            slot->state=State::Free;++self.cancelled;return true;
        };
        out.abandon=[](void* context,xess_vk::SharedPlaneKey key,VkFence completed) noexcept {
            auto& self=*static_cast<SharedInputs*>(context);auto* slot=self.find(key);
            if(!slot||slot->state!=State::Submitted||!completed||fenceStatus!=VK_SUCCESS)return false;
            slot->state=State::Free;++self.abandoned;return true;
        };
        out.release=[](void* context,xess_vk::SharedPlaneKey key,const xess_vk::FrameCompletion& receipt) noexcept {
            auto& self=*static_cast<SharedInputs*>(context);auto* slot=self.find(key);
            if(!slot||slot->state!=State::Submitted||!self.tailComplete||receipt.sharedKey!=key||
               !receipt.key.valid()||receipt.key.generation!=91||!receipt.producedValid||receipt.producedValue!=11||
               !receipt.consumedValid||receipt.consumedValue!=12||!receipt.sdkTailValid||receipt.sdkTailValue!=13)return false;
            slot->state=State::Free;++self.released;return true;
        };
        out.poison=[](void* context,xess_vk::SharedPlaneKey key) noexcept {
            auto& self=*static_cast<SharedInputs*>(context);if(auto* slot=self.find(key))slot->state=State::Poisoned;
            ++self.poisoned;
        };
        return out;
    }
};
void sharedInputLeases() {
    const auto queue=handle<VkQueue>(2);
    const auto fence=handle<VkFence>(3);
    const auto signal=handle<VkSemaphore>(4);
    for(unsigned failure=0;failure<3;++failure) {
        SharedInputs shared;Fixture f;require(f.provider.setSharedInputs(shared.access()),"shared preparation callbacks attach");
        const auto destroyed=destroyedBuffers,freed=freedMemory;
        if(failure) {
            failAllocationOnce=failure==1;failBufferBindingOnce=failure==2;
            require(!f.provider.prepareSharedInputs({2,2})&&destroyedBuffers==destroyed+1&&freedMemory==freed+(failure==2),
                    "partial shared carrier allocation or bind failure destroys every installed handle");
        }
        const auto source=pair(1900,19001);const auto recording=facts(1900);f.provider.invocation(source);
        require(f.provider.record(0,recording,evidence(source,recording),scope(source))&&shared.cancelled==1&&
                f.provider.stats().sharedCaptures==0&&f.provider.stats().sharedFallbacks==1,
                "missing or failed shared intermediate preparation cancels its lease and captures through private fallback");
        require(f.provider.prepareSharedInputs({2,2}),"shared carrier preparation can retry after the unsubmitted allocation failure");
        const auto handles=nextHandle.load();const auto next=pair(1901,19002);const auto nextRecording=facts(1901);f.provider.invocation(next);
        require(f.provider.record(0,nextRecording,evidence(next,nextRecording),scope(next))&&nextHandle.load()==handles&&
                f.provider.stats().sharedCaptures==1,"a prepared shared record allocates no Vulkan buffers, memory or images");
        f.provider.retirePool(recording.pool);
    }
    {
        SharedInputs shared;Fixture f;require(f.provider.setSharedInputs(shared.access()),"concrete shared callbacks attach");
        require(f.provider.prepareSharedInputs({2,2}),"shared intermediate buffers prepare outside recording");
        for(unsigned i=0;i<6;++i)submitCapture(f.provider,pair(2000+i,20001+i),2000+i,signal);
        auto state=f.provider.stats();
        require(shared.writes==3&&state.sharedCaptures==3&&state.sharedFallbacks==3&&state.captured==6&&
                std::count(state.states.begin(),state.states.end(),cp::State::Submitted)==3,
                "three shared leases and the original private three slots have independent capacity");
        const auto extra=pair(2006,20007);const auto recording=facts(2006);f.provider.invocation(extra);
        require(!f.provider.record(0,recording,evidence(extra,recording),scope(extra))&&f.provider.stats().saturated==1,
                "only both bounded stores being full refuses another capture");
        const auto first=pair(2000,20001);const auto cutoff=f.provider.submittedCutoff(queue);
        require(!f.provider.acquireForPresent(queue,std::span(&signal,1),fence,cutoff,scope(first)).ready,
                "synchronous legacy acquisition does not borrow a shared source without a receipt");
        fenceStatus=VK_NOT_READY;
        const auto held=f.provider.acquireQueuedForPresent(queue,cutoff,scope(first));
        require(held.ready&&held.ticket.shared&&held.shared.key.valid()&&held.depth.currentLayout==VK_IMAGE_LAYOUT_GENERAL&&
                held.depth.ownerQueueFamily==0&&held.constants.producerToken==20001,
                "queued shared bundle preserves exact producer and Vulkan-owned GENERAL sources while pending");
        require(!f.provider.releaseAfterBridge(held.ticket),"shared source cannot be released by the old receipt-free API");
        fenceStatus=VK_SUCCESS;
        require(f.provider.stop(fence)==VK_SUCCESS&&shared.abandoned==2&&!shared.released,
                "real shutdown Vulkan completion abandons unused submitted sources but retains the Consuming lease");
        require(!f.provider.setSharedInputs({}),"live SDK source retains its callback owner on detach");
        xess_vk::FrameCompletion receipt;receipt.key={0,91};receipt.sharedKey=held.shared.key;
        receipt.producedValid=receipt.consumedValid=receipt.sdkTailValid=true;
        receipt.producedValue=11;receipt.consumedValue=12;receipt.sdkTailValue=13;
        require(!f.provider.releaseAfterBridge(held.ticket,&receipt),"a matching private fence does not substitute for the SDK tail");
        shared.tailComplete=true;auto wrong=receipt;++wrong.sharedKey.leaseGeneration;
        require(!f.provider.releaseAfterBridge(held.ticket,&wrong),"another shared lease generation cannot release the held source");
        require(f.provider.releaseAfterBridge(held.ticket,&receipt)&&shared.released==1&&
                !f.provider.releaseAfterBridge(held.ticket,&receipt)&&f.provider.setSharedInputs({}),
                "only matching full receipt releases once and permits callback-owner detachment");
    }
    {
        SharedInputs shared;Fixture f;require(f.provider.setSharedInputs(shared.access()),"shared cancellation callbacks attach");
        require(f.provider.prepareSharedInputs({2,2}),"shared cancellation carriers prepare outside recording");
        recordedBarriers.clear();
        for(unsigned i=0;i<3;++i) {
            const auto source=pair(2100+i,21001+i);const auto recording=facts(2100+i);f.provider.invocation(source);
            require(f.provider.record(0,recording,evidence(source,recording),scope(source)),"shared record without submit");
        }
        require(recordedBarriers.size()==24,"three shared captures record their source and destination barriers");
        for(unsigned i=0;i<3;++i) {
            const auto* before=recordedBarriers.data()+i*8;const auto* after=before+4;
            require(before[2].srcQueueFamilyIndex==VK_QUEUE_FAMILY_EXTERNAL&&before[2].dstQueueFamilyIndex==0&&
                    before[2].newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&
                    after[0].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&
                    after[1].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&
                    after[2].newLayout==VK_IMAGE_LAYOUT_GENERAL&&after[3].newLayout==VK_IMAGE_LAYOUT_GENERAL,
                    "borrowed targets acquire external ownership, and the original source layouts are restored");
        }
        f.provider.retireCompletedForPresent(queue,fence,0,scope(pair(2200,22001)));
        require(!shared.cancelled&&!shared.abandoned,"a GPU fence never cancels recorded commands that might submit later");
        auto recording=facts(2100);f.provider.retireUnsubmitted({recording.command,recording.rpRecordingGeneration+1});
        require(!shared.cancelled,"stale recording generation cannot cancel shared reservation");
        f.provider.retireUnsubmitted({recording.command,recording.rpRecordingGeneration});f.provider.retirePool(recording.pool);
        require(shared.cancelled==3&&f.provider.setSharedInputs({}),"exact reset/free and pool cancellation return all unsubmitted shared leases");
    }
    {
        SharedInputs shared;Fixture f;require(f.provider.setSharedInputs(shared.access()),"shared failure callbacks attach");
        require(f.provider.prepareSharedInputs({2,2}),"shared failure carrier prepares outside recording");
        submitCapture(f.provider,pair(2300,23001),2300,signal,VK_ERROR_DEVICE_LOST);
        require(shared.poisoned==1&&!shared.writes&&f.provider.stats().sharedStates[0]==cp::State::Uncertain&&
                f.provider.stop(fence)==VK_ERROR_UNKNOWN&&!f.provider.setSharedInputs({}),
                "unknown application submission retains and poisons the shared owner instead of pretending idle");
    }
    std::puts("PASS shared Provider leases: independent private3/shared3, GENERAL/source layouts, exact receipt and tail gate, recorded cancellation, detach/shutdown and uncertain-submit retention; CPU mock only");
}
void depthComputeSnapshot() {
    for(unsigned mode=0;mode<7;++mode) {
        SharedInputs shared;shared.depthStorage=mode!=3;auto settings=config();settings.depthCompute=true;Fixture f(settings);
        require(f.provider.setSharedInputs(shared.access()),"depth shader callbacks attach");
        const auto destroyed=depthPipelinesDestroyed;failDepthPipeline=mode==4;
        require(f.provider.prepareSharedInputs({2,2},shared.depthStorage),"depth resources prepare at present boundary");failDepthPipeline=false;
        if(mode==4)require(depthPipelinesDestroyed==destroyed+1,"failed pipeline creation destroys a partial nonnull pipeline and keeps transfer available");
        const auto source=pair(2400+mode,24001+mode);const auto recording=facts(2400+mode);auto e=evidence(source,recording);
        e.pipeline={appPipeline,1};e.pipelineLayout={appLayout,1};e.computeRestoreKnown=mode!=1;e.computeRestoreSetCount=2;
        e.computeRestoreSets[0]={appSet0,1};e.computeRestoreSets[1]={appSet1,1};
        e.bindings[0].view={0xF001,mode==6?0u:1u};e.bindings[0].imageDescription.usage|=VK_IMAGE_USAGE_SAMPLED_BIT;
        if(mode==2)e.bindings[0].imageDescription.usage&=~VK_IMAGE_USAGE_SAMPLED_BIT;
        const auto handles=nextHandle.load(),copies=copyCalls.load();const auto dispatches=depthDispatches,writes=depthDescriptorWrites;
        boundComputePipeline=handle<VkPipeline>(appPipeline);boundComputeSets={handle<VkDescriptorSet>(appSet0),handle<VkDescriptorSet>(appSet1)};
        recordedBarriers.clear();f.provider.invocation(source);throwDepthDispatch=mode==5;
        const bool captured=f.provider.record(0,recording,e,scope(source));throwDepthDispatch=false;
        const bool compute=mode==0||mode==5;
        require(nextHandle.load()==handles,"recording performs no shader/resource/descriptor allocation");
        require(boundComputePipeline==handle<VkPipeline>(appPipeline)&&boundComputeSets==
            std::array{handle<VkDescriptorSet>(appSet0),handle<VkDescriptorSet>(appSet1)},
            "own pipeline/set0 insertion restores the full application pipeline and higher opaque set on success or exception");
        require(depthDispatches==dispatches+(compute?1u:0u)&&copyCalls.load()==copies+(mode==5?0u:compute?1u:3u),
            "compute replaces both depth transfers; every unsupported/unknown case keeps the original three snapshot copies");
        require(depthDescriptorWrites==writes+(compute?1u:0u),"only an admitted shader updates its already-owned descriptor set");
        const auto stats=f.provider.stats();
        if(mode==5)require(!captured&&stats.sharedStates[0]==cp::State::Uncertain&&shared.poisoned&&
            recordedBarriers.size()==2&&recordedBarriers[0].oldLayout==recordedBarriers[0].newLayout,
            "a partial compute command restores bindings, leaves game source layouts unchanged and quarantines the shared lease");
        else {
            require(captured&&stats.depthComputeCaptures==(compute?1u:0u)&&stats.depthTransferCaptures==(compute?0u:1u),
                "actual capture statistics report shader versus transfer, not only resource capability");
            if(compute)require(recordedBarriers.front().oldLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&
                recordedBarriers.front().newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&recordedBarriers.back().newLayout==VK_IMAGE_LAYOUT_GENERAL,
                "shader sampling preserves the original declared depth layout and leaves shared output GENERAL");
            f.provider.retirePool(recording.pool);
        }
    }
    std::puts("PASS depth compute Provider CPU: no record allocation/push constants, one dispatch versus two depth copies, full opaque-set/pipeline restore, unchanged source layout, unsupported/partial failure fallback");
}

template<class Record> void crossThread(cp::Provider& provider, std::span<const cp::SourcePair> sources, Record record) {
    std::latch published{1}, recorded{1};
    DWORD publisherThread{}, recordingThread{};
    std::exception_ptr publisherError, recordingError;
    std::thread publisher([&] {
        try {
            publisherThread = GetCurrentThreadId();
            for (auto value : sources) { value.thread = publisherThread; provider.invocation(value); }
        } catch (...) { publisherError = std::current_exception(); }
        published.count_down();
        recorded.wait(); // Do not allow Windows to recycle A's ID for B.
    });
    std::thread recorder([&] {
        published.wait();
        try {
            if (publisherError) std::rethrow_exception(publisherError);
            recordingThread = GetCurrentThreadId();
            require(recordingThread != publisherThread, "two simultaneously live actual CPU threads");
            record();
        } catch (...) { recordingError = std::current_exception(); }
        recorded.count_down();
    });
    recorder.join(); publisher.join();
    if (recordingError) std::rethrow_exception(recordingError);
}
}

int main() {
    try {
        require(!config().depthCompute,"Provider depth compute is an independent default-off opt-in");
        {
            Fixture f;
            const std::array sources{pair(100, 1001), pair(101, 1002)};
            crossThread(f.provider, sources, [&] {
                // Both frames are already queued with identical resources.
                capture(f.provider, sources[0], 1);
                refuse(f.provider, sources[0], scope(sources[0]), evidence(sources[0], facts(2)), 2,
                       "a consumed invocation cannot record again");
                capture(f.provider, sources[1], 3);
            });
            require(f.provider.stats().captured == 2, "N and N+1 each captured exactly once across threads");
        }
        {
            Fixture f;
            const auto source = pair(200, 2001);
            const std::array sources{source};
            crossThread(f.provider, sources, [&] {
                auto dispatch = evidence(source, facts(4));
                ++dispatch.bindings[0].image.generation;
                refuse(f.provider, source, scope(source), dispatch, 4, "stale depth generation refuses");
                dispatch = evidence(source, facts(5)); ++dispatch.bindings[1].image.generation;
                refuse(f.provider, source, scope(source), dispatch, 5, "stale motion generation refuses");
                auto wrong = scope(source); ++wrong.engineFrame;
                refuse(f.provider, source, wrong, evidence(source, facts(6)), 6, "wrong engine frame refuses");
                wrong = scope(source); ++wrong.ticket.sdkId;
                refuse(f.provider, source, wrong, evidence(source, facts(7)), 7, "wrong SDK id refuses");
                wrong = scope(source); ++wrong.ticket.contextGeneration;
                refuse(f.provider, source, wrong, evidence(source, facts(8)), 8, "wrong SDK context generation refuses");
                wrong = scope(source); wrong.ticket.latencyEnabled = false;
                refuse(f.provider, source, wrong, evidence(source, facts(8)), 8, "full SDK ticket must match");
                wrong = scope(source); wrong.valid = false;
                refuse(f.provider, source, wrong, evidence(source, facts(9)), 9, "missing real recording scope refuses");
                dispatch = evidence(source, facts(20)); ++dispatch.command.handle;
                refuse(f.provider, source, scope(source), dispatch, 20, "descriptor and RP command handles must match");
                dispatch = evidence(source, facts(21)); dispatch.command.generation = 0;
                refuse(f.provider, source, scope(source), dispatch, 21, "descriptor command generation must be known");
                for (std::size_t index = 0; index < 2; ++index) {
                    dispatch = evidence(source, facts(22 + index));
                    dispatch.bindings[2] = dispatch.bindings[index]; dispatch.count = 3;
                    refuse(f.provider, source, scope(source), dispatch, 22 + index,
                           "duplicate depth or motion binding refuses");
                    dispatch = evidence(source, facts(24 + index)); dispatch.bindings[index].arrayElement = 1;
                    refuse(f.provider, source, scope(source), dispatch, 24 + index,
                           "array depth or motion binding refuses");
                }
                capture(f.provider, source, 10); // Refusals never consume the valid pending producer.
            });
        }
        {
            Fixture f;
            const std::array sources{pair(300, 3001), pair(300, 3002)};
            crossThread(f.provider, sources, [&] {
                refuse(f.provider, sources[0], scope(sources[0]), evidence(sources[0], facts(11)), 11,
                       "same-frame same-resource different invocations are ambiguous");
                require(f.provider.stats().ambiguousDispatches == 1 && !f.provider.stats().captured,
                        "ambiguity is explicit and never chooses the newest producer");
            });
        }
        {
            Fixture f;
            std::array<cp::SourcePair, cp::invocationCapacity + 1> sources;
            for (std::size_t i = 0; i < sources.size(); ++i) sources[i] = pair(400 + i, 4001 + i);
            crossThread(f.provider, sources, [&] {
                require(f.provider.stats().invocationCapacityRefusals == 1, "bounded capacity refuses overflow");
                refuse(f.provider, sources.back(), scope(sources.back()), evidence(sources.back(), facts(12)), 12,
                       "overflow producer cannot overwrite an older entry");
                for (std::size_t i = 0; i < cp::invocationCapacity; ++i) capture(f.provider, sources[i], 30 + i);
                require(f.provider.stats().captured == cp::invocationCapacity,
                        "unrelated overflow preserves every exact older candidate");
            });
        }
        {
            Fixture f;
            std::array<cp::SourcePair, cp::invocationCapacity + 1> sources;
            for (std::size_t i = 0; i < cp::invocationCapacity; ++i) sources[i] = pair(500 + i, 5001 + i);
            // The candidate which cannot fit would collide with the oldest.
            sources.back() = pair(500, 5999);
            crossThread(f.provider, sources, [&] {
                require(f.provider.stats().invocationCapacityRefusals == 1, "overflow duplicate refused at capacity");
                refuse(f.provider, sources.front(), scope(sources.front()), evidence(sources.front(), facts(15)), 15,
                       "dropped duplicate cannot leave the older candidate falsely unique");
                require(f.provider.stats().ambiguousDispatches == 1 && !f.provider.stats().captured,
                        "capacity collision fails closed without any copy");
                capture(f.provider, sources[1], 16);
                // Exercise the production completed-present retirement API;
                // this CPU fixture establishes bookkeeping, not real present.
                auto wrongContext = scope(sources.front()); ++wrongContext.ticket.contextGeneration;
                f.provider.retireFrame(wrongContext);
                refuse(f.provider, sources.front(), scope(sources.front()), evidence(sources.front(), facts(17)), 17,
                       "wrong context retirement cannot erase the ambiguity");
                f.provider.retireFrame(scope(sources.front()));
                refuse(f.provider, sources.front(), scope(sources.front()), evidence(sources.front(), facts(18)), 18,
                       "retired old frame has no candidate");
                auto future = pair(516, 6001); future.thread = GetCurrentThreadId();
                f.provider.invocation(future);
                capture(f.provider, future, 19);
            });
        }
        {
            Fixture f;
            std::array sources{pair(600, 6001), pair(600, 6002)};
            sources[1].depth.handle += 0x10; sources[1].motion.handle += 0x10;
            crossThread(f.provider, sources, [&] {
                const auto sharedSignal = handle<VkSemaphore>(0x700);
                for (std::size_t i = 0; i < sources.size(); ++i) {
                    const auto recording = facts(70 + i);
                    require(f.provider.record(GetCurrentThreadId(), recording, evidence(sources[i], recording), scope(sources[i])),
                            "same frame with two distinct resource pairs records independently");
                    f.provider.submitted({recording.command, recording.rpRecordingGeneration}, std::span(&sharedSignal, 1),
                                         handle<VkQueue>(2), 70 + i, VK_SUCCESS);
                }
                fenceStatus = VK_NOT_READY;
                require(!f.provider.acquireForPresent(handle<VkQueue>(2), std::span(&sharedSignal, 1), handle<VkFence>(3), f.provider.submittedCutoff(handle<VkQueue>(2))).ready,
                        "unfinished copy fence never admits a bundle or recycling");
                f.provider.retireFrame(scope(sources[0]));
                auto state = f.provider.stats();
                require(std::count(state.states.begin(), state.states.end(), cp::State::Submitted) == 2 &&
                        !state.cancelled && !state.ambiguousPresent,
                        "CPU frame retirement cannot recycle unfinished submitted slots");
                fenceStatus = VK_SUCCESS;
                require(!f.provider.acquireForPresent(handle<VkQueue>(2), std::span(&sharedSignal, 1), handle<VkFence>(3), f.provider.submittedCutoff(handle<VkQueue>(2))).ready,
                        "completed shared wait remains ambiguous and admits no bundle");
                state = f.provider.stats();
                require(state.ambiguousPresent == 1 && state.cancelled == 2 && !state.presentMatches && !state.consumed &&
                        std::count(state.states.begin(), state.states.end(), cp::State::Free) == cp::slotCount,
                        "completed ambiguous producers are cancelled and all slots are free");
                require(!f.provider.acquireForPresent(handle<VkQueue>(2), std::span(&sharedSignal, 1), handle<VkFence>(3), f.provider.submittedCutoff(handle<VkQueue>(2))).ready &&
                        f.provider.stats().cancelled == 2, "completed ambiguous producers are not cancelled twice");
                std::array<cp::SourcePair, cp::slotCount> next;
                for (std::size_t i = 0; i < next.size(); ++i) {
                    next[i] = pair(601, 6101 + i); next[i].thread = GetCurrentThreadId();
                    next[i].depth.handle += 0x100 + i * 0x10; next[i].motion.handle += 0x100 + i * 0x10;
                    f.provider.invocation(next[i]);
                }
                for (std::size_t i = 0; i < next.size(); ++i) {
                    const auto recording = facts(80 + i);
                    require(f.provider.record(GetCurrentThreadId(), recording, evidence(next[i], recording), scope(next[i])),
                            "next frame can reuse every slot after completed ambiguity");
                }
                state = f.provider.stats();
                require(std::count(state.states.begin(), state.states.end(), cp::State::Recorded) == cp::slotCount &&
                        !state.saturated, "three next-frame captures fit without saturation");
                for (std::size_t i = 0; i < next.size(); ++i) {
                    const auto recording = facts(80 + i);
                    f.provider.retireUnsubmitted({recording.command, recording.rpRecordingGeneration});
                }
            });
        }
        measurementQueries();measuredSubmissionCommands();observedOffConsumesWithoutCopies();queuedExactLease();unkeyedFailureRevokesPending();completedSubmissionBoundaries();sharedInputLeases();
        require(!depthPipelineCreates&&!depthDescriptorWrites&&!depthDispatches,
            "default private and T3.2 transfer routes do not prepare, update or dispatch a compute pipeline");
        depthComputeSnapshot();
        std::printf("PASS continuous Provider CPU: live A->B, queued N/N+1, exact frame/ticket/generations, duplicate/array and command-identity refusals, one consumption, bounded capacity, completed-fence ambiguity recycling; copyCommands=%llu; no GPU/game/SDK\n",
                    static_cast<unsigned long long>(copyCalls.load()));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL continuous Provider cross-thread CPU: %s\n", e.what());
        return 1;
    }
}
