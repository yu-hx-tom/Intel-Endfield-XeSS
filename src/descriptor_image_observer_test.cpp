#include "descriptor_image_observer.hpp"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace {std::atomic<bool> countAllocations{};std::atomic<std::size_t> allocations{};}
void* operator new(std::size_t bytes){if(countAllocations)++allocations;if(void* value=std::malloc(bytes?bytes:1))return value;throw std::bad_alloc();}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* value) noexcept{std::free(value);}void operator delete[](void* value) noexcept{std::free(value);}
void operator delete(void* value,std::size_t) noexcept{std::free(value);}void operator delete[](void* value,std::size_t) noexcept{std::free(value);}
namespace {
namespace ob=xess_vk::descriptor_image_observer;
namespace dm=xess_vk::descriptor_image_metadata;
unsigned cases{},checks{};
std::array<std::array<char,8192>,2048> lines{};std::size_t lineCount{};
ob::Context* readOnlyReentry{};bool throwSink{};bool oversizedLine{};
xess_vk::CommandRecordingFacts* mutateRecordingSource{};
xess_vk::RecordedSourceLayout* mutateLayoutSource{};
xess_vk::resource_memory_observer::Observation* mutateMemorySource{};
void require(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
template<class T>T handle(std::uint64_t value){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(value);else return static_cast<T>(value);}
void sink(const char* line){
    if(readOnlyReentry)(void)ob::stats(readOnlyReentry);
    SetLastError(0xdead);
    if(throwSink)throw 7;
    if(mutateRecordingSource&&std::strstr(line,"\"event\":\"descriptor_pre_dispatch\"")){
        mutateRecordingSource->rpRecordingGeneration=999;mutateRecordingSource->commonRecordOrder=999;
        mutateRecordingSource->recording=false;mutateRecordingSource->reasons=UINT64_MAX;mutateRecordingSource=nullptr;
    }
    if(mutateLayoutSource&&std::strstr(line,"\"event\":\"descriptor_pre_dispatch\"")){
        mutateLayoutSource->recordedLayout=VK_IMAGE_LAYOUT_GENERAL;mutateLayoutSource->recordedLayoutResolved=false;
        mutateLayoutSource->recordedPrefixSupported=false;mutateLayoutSource->issues=UINT64_MAX;mutateLayoutSource=nullptr;
    }
    if(mutateMemorySource&&std::strstr(line,"\"event\":\"descriptor_pre_dispatch\"")){mutateMemorySource->binding.memory.generation=999;mutateMemorySource->binding.observedAliasFree=false;mutateMemorySource=nullptr;}
    const auto size=std::strlen(line);if(size>=8192){oversizedLine=true;return;}
    if(lineCount<lines.size()){std::memcpy(lines[lineCount].data(),line,size+1);++lineCount;}
}
bool contains(const char* fragment){for(std::size_t i=0;i<lineCount;++i)if(std::strstr(lines[i].data(),fragment))return true;return false;}
std::size_t matching(const char* fragment){std::size_t n=0;for(std::size_t i=0;i<lineCount;++i)if(std::strstr(lines[i].data(),fragment))++n;return n;}
struct Fixture {
    ob::Context* context{};
    VkDevice device=handle<VkDevice>(1);VkImage image=handle<VkImage>(10);VkImageView view=handle<VkImageView>(20);
    VkDescriptorSetLayout setLayout=handle<VkDescriptorSetLayout>(30);VkPipelineLayout layout=handle<VkPipelineLayout>(40);
    VkDescriptorPool pool=handle<VkDescriptorPool>(50);VkDescriptorSet set=handle<VkDescriptorSet>(60);
    VkCommandPool commandPool=handle<VkCommandPool>(70);VkCommandBuffer command=handle<VkCommandBuffer>(80);VkPipeline pipeline=handle<VkPipeline>(90);
    xess_vk::image_inventory::Record imageRecord{10,1,{std::uint32_t(VK_FORMAT_R16G16_SFLOAT),64,32,1,1,1,VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,0,VK_IMAGE_TILING_OPTIMAL,VK_SHARING_MODE_EXCLUSIVE,VK_IMAGE_LAYOUT_UNDEFINED}};
    VkImageViewCreateInfo viewInfo{};VkDescriptorSetLayoutBinding binding{};VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    VkPipelineLayoutCreateInfo layoutInfo{};VkDescriptorPoolSize poolSize{};VkDescriptorPoolCreateInfo poolInfo{};
    VkDescriptorSetAllocateInfo setInfo{};VkCommandPoolCreateInfo commandPoolInfo{};VkCommandBufferAllocateInfo commandInfo{};
    VkCommandBufferBeginInfo beginInfo{};VkComputePipelineCreateInfo pipelineInfo{};VkDescriptorImageInfo descriptor{};VkWriteDescriptorSet write{};
    explicit Fixture(bool recording=true,std::size_t setCapacity=16,bool selected=false,bool measured=false){
        lineCount=0;oversizedLine=false;throwSink=false;
        dm::Config config;config.images=16;config.views=16;config.setLayouts=8;config.pipelineLayouts=8;config.pools=8;
        config.sets=setCapacity;config.pipelines=8;config.commandPools=8;config.commands=16;config.continuousDispatchSelection=selected;
        config.measureReferenceConsumption=measured;
        context=ob::create(device,sink,config);require(context!=nullptr,"context creation");
        viewInfo.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;viewInfo.image=image;viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format=VK_FORMAT_R16G16_SFLOAT;viewInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        binding={3,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        setLayoutInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;setLayoutInfo.bindingCount=1;setLayoutInfo.pBindings=&binding;
        layoutInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&setLayout;
        poolSize={VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,16};poolInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;poolInfo.maxSets=16;poolInfo.poolSizeCount=1;poolInfo.pPoolSizes=&poolSize;
        setInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;setInfo.descriptorPool=pool;setInfo.descriptorSetCount=1;setInfo.pSetLayouts=&setLayout;
        commandPoolInfo.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;commandPoolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        commandInfo.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;commandInfo.commandPool=commandPool;
        commandInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commandInfo.commandBufferCount=1;
        beginInfo.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        pipelineInfo.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;pipelineInfo.layout=layout;
        pipelineInfo.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pipelineInfo.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module=handle<VkShaderModule>(100);pipelineInfo.stage.pName="main";
        descriptor.sampler=handle<VkSampler>(0xbad); // Ignored for SAMPLED_IMAGE.
        descriptor.imageView=view;descriptor.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=set;write.dstBinding=3;write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=&descriptor;
        ob::imageObserved(context,imageRecord);ob::viewCreated(context,view,&viewInfo,VK_SUCCESS);
        ob::setLayoutCreated(context,setLayout,&setLayoutInfo,VK_SUCCESS);ob::pipelineLayoutCreated(context,layout,&layoutInfo,VK_SUCCESS);
        ob::poolCreated(context,pool,&poolInfo,VK_SUCCESS);ob::setsAllocated(context,&setInfo,&set,VK_SUCCESS);
        ob::updateDescriptors(context,1,&write,0,nullptr);ob::commandPoolCreated(context,commandPool,&commandPoolInfo,VK_SUCCESS);
        ob::allocate(context,&commandInfo,&command,VK_SUCCESS);
        const auto trackedPipeline=ob::pipelineCreated(context,pipeline,&pipelineInfo,VK_SUCCESS);
        require(trackedPipeline.accepted()&&trackedPipeline.key.handle==reinterpret_cast<std::uint64_t>(pipeline)&&trackedPipeline.key.generation,
            "pipeline creation returns the exact tracker identity for scoped SR shader correlation");
        if(recording)record();
        require(lineCount==0,"object hooks and source state cannot call sink");
    }
    ~Fixture(){readOnlyReentry=nullptr;mutateRecordingSource=nullptr;mutateLayoutSource=nullptr;throwSink=false;ob::destroy(context);}
    void record(){ob::begin(context,command,&beginInfo,VK_SUCCESS);ob::bindPipeline(context,command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        ob::bindSets(context,command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);}
    dm::DispatchEvidence dispatch(){return ob::preDispatch(context,command,8,4,1);}
    void source(ob::Source kind=ob::Source::RawMotion,std::uint64_t counter=17){ob::observeSource(context,kind,{10,1},counter);}
};
void detailsAndBoundedSink(){
    ++cases;Fixture f;readOnlyReentry=f.context;
    for(std::size_t i=0;i<ob::sourceCount;++i)ob::observeSource(f.context,static_cast<ob::Source>(i),{10,1},17+i);
    require(lineCount==0,"all source observations are silent");
    SetLastError(0xbeef);const auto value=f.dispatch();require(GetLastError()==0xbeef,"dispatch preserves LastError across sink");
    require(value.metadataAccepted&&value.count==1,"ordinary image binding accepted");
    require(!value.copyPermitted&&!value.sameFrameVerified&&!value.layoutVerified&&!value.gpuComplete,"no safety proof from metadata");
    auto state=ob::stats(f.context);require(state.matchedDispatches==1&&state.emittedGroups==1&&state.emittedBindings==1,"one matched group and binding");
    require(contains("\"source_match_mask\":2047")&&contains("\"source_key\":\"raw_motion\"")&&contains("\"source_property\":\"r_input_motion_vectors\""),"all source labels retained");
    require(contains("\"source_counter\":20")&&contains("\"same_image_as_source_sample\":true"),"opaque source counter and exact image match");
    require(contains("\"same_frame_verified\":false")&&contains("\"copy_permitted\":false")&&contains("\"submitted\":false"),"all log claims remain observation-only");
    require(!oversizedLine,"every source-rich line fits fixed buffer");
    ob::summary(f.context);require(contains("\"object_usage\":[")&&contains("\"reason\":\"explicit\""),"capacity and final stats visible");
    ob::end(f.context,f.command,VK_SUCCESS);require(ob::validateEvidence(f.context,value).accepted(),"explicit CPU revalidation after end");
}
void sourceEpochAndClear(){
    ++cases;Fixture f;ob::observeSource(f.context,ob::Source::RawMotion,{10,2},1);
    require(f.dispatch().metadataAccepted&&ob::stats(f.context).matchedDispatches==0,"same numeric handle wrong generation never matches");
    f.source();f.dispatch();require(ob::stats(f.context).matchedDispatches==1,"exact key matches");
    ob::imageRetiring(f.context,{10,2});require(ob::stats(f.context).sources[3].image==dm::Key{10,1},"stale image retirement cannot clear current source");
    ob::observeSource(f.context,ob::Source::RawMotion,{10,0},55);auto state=ob::stats(f.context);
    require(state.invalidSources==1&&state.sources[3].image==dm::Key{},"partial zero source fails closed and clears");
    f.source();ob::observeSource(f.context,ob::Source::RawMotion,{},123);require(ob::stats(f.context).sources[3].counter==0,"zero key clears ordinal");
    ob::observeSource(f.context,static_cast<ob::Source>(999),{10,1},1);require(ob::stats(f.context).invalidSources==2,"invalid source enum contained");
    f.source();ob::imageRetiring(f.context,{10,1});require(ob::stats(f.context).sources[3].image==dm::Key{},"exact image retirement clears candidate");
    require(!f.dispatch().metadataAccepted,"destroyed image cannot remain accepted descriptor evidence");
}
void samplingBudgetsAndRefresh(){
    ++cases;Fixture f;
    for(unsigned i=0;i<9;++i)f.dispatch();
    require(ob::stats(f.context).ordinaryDetails==8,"ordinary coverage exact budget");f.source();
    for(std::uint64_t i=0;i<ob::periodicInterval*(ob::periodicSummaryLimit+1);++i)f.dispatch();
    auto state=ob::stats(f.context);require(state.matchedDetails==32&&state.ordinaryDetails==8,"early ordinary cannot consume candidate slots");
    require(state.periodicSummaries==128&&state.omittedPeriodicSummaries==1,"periodic matched summaries remain bounded");
    require(state.matchedDispatches==154800&&state.metadata.dispatches==154809,"counters continue after all detail quotas");
    require(state.periodicMatchedDetails==64&&state.omittedPeriodicMatchedDetails==65,"later candidate details have independent fixed quota");
    require(state.emittedGroups==104&&state.emittedBindings==104,"details limited to initial plus periodic complete groups");
    require(matching("\"reason\":\"matched_dispatch_interval\"")==128,"interval uses matched dispatches");
    require(matching("\"sample_reason\":\"periodic_source_image_match\"")==64,"late matches remain sampled");
    require(state.healthSummaries==7&&matching("\"reason\":\"all_dispatch_health_interval\"")==7,"independent all-dispatch health interval");
    f.source(ob::Source::RawMotion,777777);ob::summary(f.context);
    require(contains("\"source_counter\":777777"),"source cache refresh continues after quota");
    for(unsigned i=0;i<18;++i)ob::summary(f.context);state=ob::stats(f.context);
    require(state.explicitSummaries==16&&state.omittedExplicitSummaries==3,"explicit summaries also bounded");
    ob::retire(f.context);ob::summary(f.context);require(ob::stats(f.context).finalSummaries==1&&contains("\"reason\":\"retired_final\""),"one final record survives exhausted explicit quota");
    require(!oversizedLine,"summary maximum source/object table fits");
}
void mutationAndRetirement(){
    ++cases;Fixture f;f.source();const auto before=f.dispatch();require(before.metadataAccepted,"initial evidence");
    ob::updateDescriptors(f.context,1,&f.write,0,nullptr);
    require(!ob::validateEvidence(f.context,before).accepted(),"ordinary update invalidates earlier evidence");
    require(!f.dispatch().metadataAccepted,"same CB cannot repair itself with newest descriptor");
    f.record();require(f.dispatch().metadataAccepted,"new recording can use updated set");
    ob::setUnsupported(f.context,f.set);require(!f.dispatch().metadataAccepted,"unknown template mutation cannot leave old image usable");
    const auto count=ob::stats(f.context).metadata.dispatches;ob::retire(f.context);
    f.source();const auto retired=f.dispatch();ob::summary(f.context);auto state=ob::stats(f.context);
    require(!retired.metadataAccepted&&(retired.issues&dm::bit(dm::Issue::Stopped)),"retirement refuses dispatch");
    require(state.retired&&state.metadata.dispatches==count&&state.sources[3].image==dm::Key{},"retirement sticky and source slots cleared");
    require(contains("\"retired\":true"),"retired final stats available");
}
void errorsAndHotPath(){
    ++cases;Fixture f;f.source();readOnlyReentry=f.context;
    allocations=0;countAllocations=true;SetLastError(0xcafe);
    for(unsigned i=0;i<40;++i){f.source(ob::Source::RawMotion,i);f.dispatch();}
    ob::summary(f.context);countAllocations=false;
    require(GetLastError()==0xcafe,"all hot hooks preserve LastError");require(allocations==0,"no allocation in hot callbacks/log construction");
    throwSink=true;SetLastError(0xabba);ob::summary(f.context);require(GetLastError()==0xabba,"throwing sink contained and LastError restored");
    require(ob::stats(f.context).failures==1,"sink exception counted");throwSink=false;
    require(ob::create(VK_NULL_HANDLE,sink)==nullptr,"null device rejected");
    ob::summary(nullptr);ob::retire(nullptr);ob::destroy(nullptr);
    require(!ob::preDispatch(nullptr,f.command,1,1,1).metadataAccepted,"null context fails closed");
}
void refusedDispatchHealth(){
    ++cases;Fixture f;ob::updateDescriptors(f.context,1,&f.write,0,nullptr);
    for(std::uint64_t i=0;i<ob::healthInterval;++i)f.dispatch();
    const auto state=ob::stats(f.context);
    require(state.matchedDispatches==0&&state.healthSummaries==1,"all-refused workload still receives health summary");
    require(state.refusedIssueRecords[11]==ob::healthInterval,"DescriptorChanged refusal counted per dispatch");
    require(contains("\"issue_bit\":2048")&&contains("\"reason\":\"all_dispatch_health_interval\""),"late refusal reason remains observable");
}
void currentStateRecovery(){
    ++cases;Fixture f;const auto before=f.dispatch();require(before.metadataAccepted,"initial current-state evidence");
    auto pi=f.pipelineInfo;pi.flags=VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;const auto unsupported=handle<VkPipeline>(91);
    ob::pipelineCreated(f.context,unsupported,&pi,VK_SUCCESS);ob::bindPipeline(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,unsupported);
    auto evidence=f.dispatch();require(!evidence.metadataAccepted&&(evidence.issues&dm::bit(dm::Issue::UnsupportedExtension)),"unsupported current pipeline refuses only current binding");
    ob::bindPipeline(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);require(f.dispatch().metadataAccepted,"known pipeline restores current CPU metadata");
    require(ob::validateEvidence(f.context,before).accepted(),"local pipeline issue does not invalidate earlier evidence");
    auto vi=f.viewInfo;VkImageViewUsageCreateInfo extension{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};extension.usage=VK_IMAGE_USAGE_SAMPLED_BIT;vi.pNext=&extension;
    const auto badView=handle<VkImageView>(21);ob::viewCreated(f.context,badView,&vi,VK_SUCCESS);
    const auto badSet=handle<VkDescriptorSet>(61);ob::setsAllocated(f.context,&f.setInfo,&badSet,VK_SUCCESS);
    auto info=f.descriptor;info.imageView=badView;auto write=f.write;write.dstSet=badSet;write.pImageInfo=&info;ob::updateDescriptors(f.context,1,&write,0,nullptr);
    ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.layout,0,1,&badSet,0,nullptr);evidence=f.dispatch();
    require(!evidence.metadataAccepted&&(evidence.issues&dm::bit(dm::Issue::UnsupportedExtension)),"known ordinary value with opaque view facts rejects current set");
    ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.layout,0,1,&f.set,0,nullptr);require(f.dispatch().metadataAccepted,"healthy set restores same recording's CPU metadata");
    info.imageView=f.view;ob::updateDescriptors(f.context,1,&write,0,nullptr);evidence=f.dispatch();
    require(!evidence.metadataAccepted&&(evidence.issues&dm::bit(dm::Issue::DescriptorChanged)),"repair of historically referenced ordinary value still invalidates old recording");
    f.record();ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.layout,0,1,&badSet,0,nullptr);require(f.dispatch().metadataAccepted,"new recording accepts corrected value");
    for(auto kind:{dm::DispatchKind::Indirect,dm::DispatchKind::Base}){
        evidence=ob::preDispatch(f.context,f.command,1,1,1,kind);require(!evidence.metadataAccepted&&(evidence.issues&dm::bit(dm::Issue::UnsupportedDispatch)),"unsupported dispatch kind refused locally");
        evidence=f.dispatch();require(evidence.metadataAccepted&&!evidence.sameFrameVerified&&!evidence.gpuComplete&&!evidence.copyPermitted,"subsequent direct evidence recovers with false proof claims");}
    ob::summary(f.context);require(contains("\"copy_permitted\":false")&&contains("\"same_frame_verified\":false")&&contains("\"gpu_complete\":false"),"recovery logs keep false proof claims");
}
void referenceOverflowTelemetry(){
    ++cases;Fixture f(false,1024);std::array<VkDescriptorSet,dm::maxReferencedSets+1> sets{};
    for(std::size_t i=0;i<sets.size();++i){sets[i]=handle<VkDescriptorSet>(1000+i);ob::setsAllocated(f.context,&f.setInfo,&sets[i],VK_SUCCESS);
        auto write=f.write;write.dstSet=sets[i];ob::updateDescriptors(f.context,1,&write,0,nullptr);}
    ob::begin(f.context,f.command,&f.beginInfo,VK_SUCCESS);ob::bindPipeline(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);
    allocations=0;countAllocations=true;SetLastError(0xface);
    for(std::size_t i=0;i<dm::maxReferencedSets;++i){ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.layout,0,1,&sets[i],0,nullptr);
        require(f.dispatch().metadataAccepted,"up to 512 adapter historical references accepted");}
    ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.layout,0,1,&sets.back(),0,nullptr);auto evidence=f.dispatch();
    require(!evidence.metadataAccepted&&(evidence.issues&dm::bit(dm::Issue::Capacity)),"513th adapter reference permanently refused");
    ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.layout,0,1,&sets.front(),0,nullptr);require(!f.dispatch().metadataAccepted,"old known reference cannot hide overflow");
    ob::summary(f.context);countAllocations=false;require(!allocations&&GetLastError()==0xface,"full adapter history and summary preserve zero allocations and LastError");
    const auto state=ob::stats(f.context);require(state.metadata.referencedSetsHighWater==512&&state.metadata.referencedSetsCapacity==512&&state.metadata.referenceOverflowRefusals==1,"adapter exposes exact reference counters");
    require(contains("\"referenced_sets_high_water\":512")&&contains("\"referenced_sets_capacity\":512")&&contains("\"reference_overflow_refusals\":1"),"reference telemetry emitted in summary");
    require(!oversizedLine,"reference telemetry remains within fixed summary buffer");f.record();require(f.dispatch().metadataAccepted,"begin clears overflow for next recording");
}
struct ReaderProbe {std::size_t calls{};bool partial{};};
std::size_t templateProbeRead(void* user,const void* source,void* destination,std::size_t bytes){auto& p=*static_cast<ReaderProbe*>(user);++p.calls;SetLastError(0xdead);
    const auto copied=p.partial?bytes-1:bytes;std::memcpy(destination,source,copied);return copied;}
void templateAdapter(){
    ++cases;Fixture f;VkDescriptorUpdateTemplateEntry entry{3,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,0};
    VkDescriptorUpdateTemplateCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};info.descriptorSetLayout=f.setLayout;info.descriptorUpdateEntryCount=1;info.pDescriptorUpdateEntries=&entry;
    const auto handle=::handle<VkDescriptorUpdateTemplate>(95);std::array<unsigned char,32> data{};std::memcpy(data.data()+1,&f.descriptor,sizeof(f.descriptor));ReaderProbe reader;
    SetLastError(0xbeef);ob::templateCreated(f.context,handle,&info,VK_SUCCESS);
    require(GetLastError()==0xbeef,"template page registration preserves LastError");allocations=0;countAllocations=true;
    ob::updateWithTemplate(f.context,f.set,handle,data.data(),dm::TemplateUpdateKind::Core,{templateProbeRead,&reader});countAllocations=false;
    require(!allocations&&GetLastError()==0xbeef&&reader.calls==1&&lineCount==0,"template decode preserves LastError, allocates nothing and emits no hot update log");
    auto evidence=f.dispatch();require(!evidence.metadataAccepted&&(evidence.issues&dm::bit(dm::Issue::DescriptorChanged)),"actual template update invalidates old ordinary CB");
    f.record();const auto accepted=f.dispatch();require(accepted.metadataAccepted&&accepted.count==1,"new recording sees decoded ordinary image binding");
    ob::templateDestroying(f.context,handle);require(ob::validateEvidence(f.context,accepted).accepted(),"destroyed template does not invalidate published values");
    ob::templateCreated(f.context,handle,&info,VK_SUCCESS);ob::updateWithTemplate(f.context,f.set,handle,data.data(),dm::TemplateUpdateKind::Khr);f.record();require(f.dispatch().metadataAccepted,"KHR alias shares ordinary decoding after template handle reuse");
    reader.partial=true;ob::updateWithTemplate(f.context,f.set,handle,data.data(),dm::TemplateUpdateKind::Core,{templateProbeRead,&reader});f.record();evidence=f.dispatch();
    require(!evidence.metadataAccepted&&(evidence.issues&dm::bit(dm::Issue::TemplateReadFailed)),"reader partial failure has exact diagnostic bit and no false accepted value");
    const auto state=ob::stats(f.context).metadata;require(state.ordinaryUpdateCalls==1&&state.coreTemplateUpdateCalls==2&&state.khrTemplateUpdateCalls==1&&state.templateUpdateRefusals==1&&state.templateReadRefusals==1,"adapter tracks ordinary/core/KHR/refusal/read counters independently");
    require(state.templateReadCalls==2&&state.templateReadBytes==48&&state.templateReadElements==2,"protected-reader counters include failed attempts but exclude direct memcpy updates");
    ob::summary(f.context);require(contains("\"ordinary_update_calls\":1")&&contains("\"core_template_update_calls\":2")&&contains("\"khr_template_update_calls\":1")&&contains("\"template_refused_issue_records\":[{\"issue_bit\":2097152,\"count\":1}]"),"summary exposes specific template read failure without generic unsupported category");
    require(contains("\"template_read_calls\":2")&&contains("\"template_read_bytes\":48")&&contains("\"template_read_elements\":2"),"summary publishes exact read coalescing counters within the existing bounded line");
    require(!oversizedLine&&contains("\"copy_permitted\":false"),"template diagnostics fit fixed summary and grant no copy proof");
}
void recordingFactsAssociation(){
    ++cases;Fixture f;for(std::size_t i=0;i<ob::sourceCount;++i)ob::observeSource(f.context,static_cast<ob::Source>(i),{10,1},i+1);
    auto evidence=f.dispatch();require(evidence.metadataAccepted&&contains("\"recording_facts\":{")&&contains("\"context_incarnation\":0")&&contains("\"begin_flags_known\":false"),"optional absent RP facts explicitly unknown without changing acceptance");
    xess_vk::CommandRecordingFacts facts;facts.device=1;facts.contextIncarnation=77;facts.command=80;facts.rpRecordingGeneration=19;facts.commonRecordOrder=53;
    facts.pool=70;facts.poolGeneration=8;facts.poolFlags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;facts.queueFamily=3;facts.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    facts.beginUsageFlags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;facts.reasons=0;facts.allocationKnown=facts.poolKnown=facts.beginFlagsKnown=facts.stateKnown=facts.recording=facts.scopeKnown=true;
    lineCount=0;mutateRecordingSource=&facts;SetLastError(0xbeef);allocations=0;countAllocations=true;
    evidence=ob::preDispatch(f.context,f.command,8,4,1,ob::DispatchKind::Direct,&facts);countAllocations=false;
    require(evidence.metadataAccepted&&!allocations&&GetLastError()==0xbeef&&facts.rpRecordingGeneration==999,"production consumer copies facts before sink mutation with zero hot allocations and LastError intact");
    require(matching("\"rp_command_generation\":19")==2&&matching("\"common_record_order\":53")==2&&!contains("\"rp_command_generation\":999"),"entire original group and binding share the same owned recording snapshot");
    require(contains("\"command_generation_scope\":\"descriptor_tracker\"")&&contains("\"rp_command_generation_scope\":\"render_pass_observer_recording\"")
        &&contains("\"record_ordinal_scope\":\"descriptor_dispatch_only\"")&&contains("\"common_record_order_scope\":\"hooked_control_barrier_dispatch_events\""),"different generation and ordinal namespaces explicitly labelled");
    require(contains("\"source_match_mask\":2047")&&!oversizedLine&&contains("\"copy_permitted\":false")&&!contains("\"copy_permitted\":true"),"source-rich facts lines fit bounded buffers and keep false proof claims");
    facts.device=2;facts.command=80;facts.reasons=0;facts.stateKnown=facts.recording=facts.scopeKnown=true;lineCount=0;
    evidence=ob::preDispatch(f.context,f.command,8,4,1,ob::DispatchKind::Direct,&facts);
    require(evidence.metadataAccepted&&contains("\"state_known\":false")&&contains("\"supported\":false"),"wrong device fact association fails closed independently of descriptor metadata acceptance");
}
struct LayoutQueryProbe {ob::Context* context{};std::size_t calls{};bool exact{true};xess_vk::RecordedSourceLayout result;};
xess_vk::RecordedSourceLayout layoutQuery(void* user,const xess_vk::CommandRecordingFacts& facts,xess_vk::RecordedImageKey image,const VkImageSubresourceRange& range,VkImageLayout declared) noexcept {
    auto& probe=*static_cast<LayoutQueryProbe*>(user);++probe.calls;const auto stats=ob::stats(probe.context); // No descriptor mutex may be held here.
    probe.exact&=facts.device==1&&facts.contextIncarnation==77&&facts.command==80&&facts.rpRecordingGeneration==19
        &&facts.commonRecordOrder==53&&image==xess_vk::RecordedImageKey{10,1}&&range.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&range.baseMipLevel==0
        &&range.levelCount==1&&range.baseArrayLayer==0&&range.layerCount==1&&declared==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&stats.metadata.acceptedDispatches>0;
    SetLastError(0xdead);auto result=probe.result;result.device=facts.device;result.contextIncarnation=facts.contextIncarnation;result.command=facts.command;
    result.rpRecordingGeneration=facts.rpRecordingGeneration;result.prefixOrder=facts.commonRecordOrder;result.image=image;result.copiedAspect=range.aspectMask;result.declaredLayout=declared;return result;
}
void sourceLayoutOwnedConsumer(){
    ++cases;Fixture f;LayoutQueryProbe probe;probe.context=f.context;auto& r=probe.result;r.queried=r.recordedLayoutResolved=r.recordedPrefixSupported=true;r.issues=0;r.recordedLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    r.resolvedLayoutRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};r.anchor.order=r.lastEvent.order=52;r.anchor.kind=r.lastEvent.kind=xess_vk::RecordedLayoutEventKind::LegacyBarrier;
    r.anchor.oldLayout=r.lastEvent.oldLayout=VK_IMAGE_LAYOUT_GENERAL;r.anchor.newLayout=r.lastEvent.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    r.anchor.explicitScopesKnown=r.lastEvent.explicitScopesKnown=true;r.lastEvent.srcStage=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;r.lastEvent.dstAccess=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    xess_vk::CommandRecordingFacts facts;facts.device=1;facts.contextIncarnation=77;facts.command=80;facts.rpRecordingGeneration=19;facts.commonRecordOrder=53;facts.reasons=0;facts.recording=facts.stateKnown=facts.scopeKnown=true;facts.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    const xess_vk::RecordedSourceLayoutQuery query{layoutQuery,&probe};auto evidence=ob::preDispatch(f.context,f.command,8,4,1,ob::DispatchKind::Direct,&facts,query);
    require(evidence.metadataAccepted&&!probe.calls&&contains("\"status\":\"not_requested\""),"unmatched ordinary descriptor stays accepted and skips layout query explicitly");
    for(std::size_t i=0;i<ob::sourceCount;++i)ob::observeSource(f.context,static_cast<ob::Source>(i),{10,1},i+1);
    lineCount=0;mutateLayoutSource=&probe.result;allocations=0;countAllocations=true;SetLastError(0xbeef);
    evidence=ob::preDispatch(f.context,f.command,8,4,1,ob::DispatchKind::Direct,&facts,query);countAllocations=false;
    require(evidence.metadataAccepted&&probe.calls==1&&probe.exact&&!allocations&&GetLastError()==0xbeef,"exact accepted source query occurs once outside mutex, preserving LastError and zero hot allocations");
    require(contains("\"resolved_count\":1")&&contains("\"recorded_layout\":5")&&contains("\"recorded_layout_resolved\":true")&&probe.result.recordedLayout==VK_IMAGE_LAYOUT_GENERAL,"group aggregate and binding serialize one owned pre-sink result despite later source mutation");
    require(!oversizedLine&&contains("\"source_match_mask\":2047")&&!contains("\"layout_verified\":true")&&!contains("\"owner_verified\":true")&&!contains("\"copy_permitted\":true"),"source-rich owned layout result fits fixed line buffer with all proof claims false");
    lineCount=0;probe.result.recordedLayoutResolved=probe.result.recordedPrefixSupported=false;probe.result.issues=xess_vk::bit(xess_vk::RecordedLayoutIssue::NoTransitionAnchor);
    evidence=ob::preDispatch(f.context,f.command,8,4,1,ob::DispatchKind::Direct,&facts,query);
    require(evidence.metadataAccepted&&contains("\"resolved_count\":0")&&contains("\"recorded_prefix_supported\":false"),"missing layout anchor never changes descriptor metadata acceptance");
    const auto calls=probe.calls;for(unsigned n=0;n<40;++n)(void)ob::preDispatch(f.context,f.command,8,4,1,ob::DispatchKind::Direct,&facts,query);
    require(probe.calls==ob::matchedDetailLimit&&probe.calls<calls+40&&ob::stats(f.context).matchedDetails==ob::matchedDetailLimit,"default layout queries stop after existing detail quota instead of running on every accepted matched dispatch");
}
struct MemoryProbe {ob::Context* context{};unsigned calls{};xess_vk::resource_memory_observer::Observation value;};
xess_vk::resource_memory_observer::Observation memoryQuery(void* user,xess_vk::RecordedImageKey key) noexcept {
    auto& p=*static_cast<MemoryProbe*>(user);++p.calls;(void)ob::stats(p.context);auto result=p.value;result.binding.resource={xess_vk::resource_memory_facts::ResourceKind::Image,{key.handle,key.generation}};SetLastError(0xdead);return result;
}
void ownedMemoryConsumer(){
    ++cases;Fixture f;ob::observeSource(f.context,ob::Source::RawColor,{10,1},1);MemoryProbe memory;memory.context=f.context;memory.value.queried=true;
    memory.value.binding.device=handle<VkDevice>(1);memory.value.binding.memory={100,22};memory.value.binding.offset=64;memory.value.binding.size=1024;memory.value.binding.end=1088;
    memory.value.binding.bindingBoundsKnown=memory.value.binding.observedAliasFree=true;
    LayoutQueryProbe layout;layout.context=f.context;layout.result.issues=0;layout.result.queried=layout.result.recordedLayoutResolved=layout.result.recordedPrefixSupported=true;layout.result.recordedLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    xess_vk::CommandRecordingFacts facts;facts.device=1;facts.command=80;facts.contextIncarnation=77;facts.rpRecordingGeneration=19;facts.commonRecordOrder=53;
    ob::DispatchSourceFacts output;ob::SourceQueryDemand demand;demand.everyMatchedBinding=true;
    mutateMemorySource=&memory.value;SetLastError(0xbeef);allocations=0;countAllocations=true;
    const auto evidence=ob::preDispatch(f.context,f.command,8,4,1,ob::DispatchKind::Direct,&facts,{layoutQuery,&layout},{memoryQuery,&memory},&output,demand);countAllocations=false;
    require(evidence.metadataAccepted&&output.observed&&output.evidence.metadataAccepted&&!allocations&&GetLastError()==0xbeef&&memory.calls==1,"owned layout/memory output comes from one accepted dispatch outside locks, no allocations, preserved LastError");
    require(output.recording.rpRecordingGeneration==19&&output.memory[0].binding.resource.key==xess_vk::resource_memory_facts::Key{10,1}&&output.memory[0].binding.memory.generation==22&&output.memory[0].binding.observedAliasFree,"same owned output survives sink mutation of later memory source");
    require(memory.value.binding.memory.generation==999&&contains("\"memory_generation\":22")&&contains("\"bounds_known_count\":1")&&!oversizedLine,"group aggregate and binding share same owned memory snapshot within existing bounded quota");
    for(unsigned n=0;n<40;++n)(void)ob::preDispatch(f.context,f.command,8,4,1,ob::DispatchKind::Direct,&facts,{layoutQuery,&layout},{memoryQuery,&memory});
    require(memory.calls==ob::matchedDetailLimit,"ordinary memory/layout query demand ends with finite original sample quota");
}
void poolResetWalk(){
    ++cases;Fixture f(false,64);const auto other=handle<VkDescriptorPool>(51);ob::poolCreated(f.context,other,&f.poolInfo,VK_SUCCESS);
    std::array<VkDescriptorSet,20> sets{};
    for(std::size_t i=0;i<sets.size();++i){sets[i]=handle<VkDescriptorSet>(2000+i);auto info=f.setInfo;info.descriptorPool=i%2?other:f.pool;
        ob::setsAllocated(f.context,&info,&sets[i],VK_SUCCESS);auto write=f.write;write.dstSet=sets[i];ob::updateDescriptors(f.context,1,&write,0,nullptr);}
    require(ob::stats(f.context).metadata.liveSets==21,"both pools populated");
    ob::poolReset(f.context,f.pool,VK_SUCCESS);require(ob::stats(f.context).metadata.liveSets==10,"reset retires exactly its own pool's sets");
    f.record();ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.layout,0,1,&sets[19],0,nullptr);
    require(f.dispatch().metadataAccepted,"a surviving set of the other pool stays accepted");
    f.record();ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.layout,0,1,&sets[18],0,nullptr);
    require(!f.dispatch().metadataAccepted,"a retired set of the reset pool is refused");
    ob::poolReset(f.context,other,VK_SUCCESS);require(ob::stats(f.context).metadata.liveSets==0,"second reset empties the table");
}

void selectedOrdinaryDispatch(){
    ++cases;Fixture f(true,16,true);f.source();LayoutQueryProbe layout;layout.context=f.context;MemoryProbe memory;memory.context=f.context;
    xess_vk::CommandRecordingFacts facts;facts.device=1;facts.command=80;facts.contextIncarnation=77;facts.recording=facts.stateKnown=facts.scopeKnown=true;
    ob::SourceQueryDemand demand;demand.everyMatchedBinding=true;ob::DispatchSourceFacts output;output.observed=true;
    allocations=0;countAllocations=true;SetLastError(0xbeef);
    const auto e=ob::preDispatch(f.context,f.command,8,4,1,ob::DispatchKind::Direct,&facts,{layoutQuery,&layout},{memoryQuery,&memory},&output,demand);
    countAllocations=false;
    require(e.filteredOut&&!e.metadataAccepted&&!e.issues&&!e.count&&e.recordOrdinal==1&&GetLastError()==0xbeef&&!allocations,"selected ordinary path preserves ordinal/LastError without hot allocation");
    require(!output.observed&&!layout.calls&&!memory.calls&&!lineCount,"filtered dispatch never publishes owned source facts, queries or details even with explicit query demand");
    const auto s=ob::stats(f.context);
    require(s.dispatchCalls==1&&s.metadata.dispatches==1&&s.metadata.filteredDispatches==1&&!s.metadata.acceptedDispatches&&!s.metadata.refusedDispatches&&
        !s.matchedDispatches&&!s.ordinaryDispatches,"selection counts are separate while every mutation is retained");
    ob::summary(f.context);require(contains("\"continuous_dispatch_selection\":true")&&contains("\"filtered_dispatches\":1"),"existing summary exposes selection and filter count without per-dispatch IO");
}
void continuousCompanionSets(bool selected=false){
    ++cases;Fixture f(false,16,selected);
    const std::array<VkDescriptorSetLayoutBinding,2> images{{
        {5,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {7,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
    auto imageInfo=f.setLayoutInfo;imageInfo.bindingCount=2;imageInfo.pBindings=images.data();
    const VkDescriptorSetLayoutBinding buffer{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    auto bufferInfo=f.setLayoutInfo;bufferInfo.pBindings=&buffer;
    const std::array layouts{handle<VkDescriptorSetLayout>(31),handle<VkDescriptorSetLayout>(32)};
    ob::setLayoutCreated(f.context,layouts[0],&imageInfo,VK_SUCCESS);
    ob::setLayoutCreated(f.context,layouts[1],&bufferInfo,VK_SUCCESS);
    const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,8},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,4}}};
    auto poolInfo=f.poolInfo;poolInfo.poolSizeCount=2;poolInfo.pPoolSizes=sizes.data();
    const auto pool=handle<VkDescriptorPool>(51);ob::poolCreated(f.context,pool,&poolInfo,VK_SUCCESS);
    const std::array sets{handle<VkDescriptorSet>(62),handle<VkDescriptorSet>(63)};
    auto allocate=f.setInfo;allocate.descriptorPool=pool;
    for(std::size_t i=0;i<sets.size();++i){
        allocate.pSetLayouts=&layouts[i];
        ob::setsAllocated(f.context,&allocate,&sets[i],VK_SUCCESS);
    }
    const std::array<VkDescriptorUpdateTemplateEntry,2> imageEntries{{
        {5,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,0,sizeof(VkDescriptorImageInfo)},
        {7,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,sizeof(VkDescriptorImageInfo),sizeof(VkDescriptorImageInfo)}}};
    const VkDescriptorUpdateTemplateEntry bufferEntry{0,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,0,sizeof(VkDescriptorBufferInfo)};
    VkDescriptorUpdateTemplateCreateInfo update{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
    update.descriptorSetLayout=layouts[0];update.descriptorUpdateEntryCount=2;update.pDescriptorUpdateEntries=imageEntries.data();
    const auto imageTemplate=handle<VkDescriptorUpdateTemplate>(96);ob::templateCreated(f.context,imageTemplate,&update,VK_SUCCESS);
    update.descriptorSetLayout=layouts[1];update.descriptorUpdateEntryCount=1;update.pDescriptorUpdateEntries=&bufferEntry;
    const auto bufferTemplate=handle<VkDescriptorUpdateTemplate>(97);ob::templateCreated(f.context,bufferTemplate,&update,VK_SUCCESS);
    const std::array<VkDescriptorImageInfo,2> imageData{f.descriptor,f.descriptor};
    const VkDescriptorBufferInfo bufferData{handle<VkBuffer>(101),0,64};ReaderProbe reader;
    ob::updateWithTemplate(f.context,sets[0],imageTemplate,imageData.data(),dm::TemplateUpdateKind::Core,{templateProbeRead,&reader});
    ob::updateWithTemplate(f.context,sets[1],bufferTemplate,&bufferData,dm::TemplateUpdateKind::Khr,{templateProbeRead,&reader});
    require(reader.calls==2,"image entries coalesce while the companion update remains a separate required read");
    VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
    copy.srcSet=f.set;copy.srcBinding=3;copy.dstSet=sets[0];copy.dstBinding=5;copy.descriptorCount=1;
    ob::updateDescriptors(f.context,0,nullptr,1,&copy);
    auto layoutInfo=f.layoutInfo;layoutInfo.setLayoutCount=2;layoutInfo.pSetLayouts=layouts.data();
    const auto layout=handle<VkPipelineLayout>(41);ob::pipelineLayoutCreated(f.context,layout,&layoutInfo,VK_SUCCESS);
    auto pipelineInfo=f.pipelineInfo;pipelineInfo.layout=layout;
    const auto pipeline=handle<VkPipeline>(92);ob::pipelineCreated(f.context,pipeline,&pipelineInfo,VK_SUCCESS);
    ob::begin(f.context,f.command,&f.beginInfo,VK_SUCCESS);ob::bindPipeline(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,2,sets.data(),0,nullptr);
    auto evidence=f.dispatch();
    require(evidence.metadataAccepted&&evidence.count==2&&evidence.ignoredNonImageBindingCount==1,
        "candidate image set plus explicitly bound buffer-only set accepts early templates and an external descriptor copy");
    require(evidence.bindings[0].setIndex==0&&evidence.bindings[0].binding==5&&evidence.bindings[1].binding==7,
        "accepted evidence retains the exact set-zero depth and motion bindings");
    require(ob::stats(f.context).metadata.liveSets==3,
        "production keeps companion and pre-pipeline allocations");
    ob::begin(f.context,f.command,&f.beginInfo,VK_SUCCESS);ob::bindPipeline(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,1,1,&sets[1],0,nullptr);
    evidence=f.dispatch();
    require(!evidence.metadataAccepted&&(evidence.issues&dm::bit(dm::Issue::MissingBinding)),
        "restoring full tracking does not accept a missing required image set");
    const auto incomplete=handle<VkDescriptorSet>(64);allocate.pSetLayouts=&layouts[0];
    ob::setsAllocated(f.context,&allocate,&incomplete,VK_SUCCESS);
    auto write=f.write;write.dstSet=incomplete;write.dstBinding=5;ob::updateDescriptors(f.context,1,&write,0,nullptr);
    const std::array incompleteSets{incomplete,sets[1]};
    ob::begin(f.context,f.command,&f.beginInfo,VK_SUCCESS);ob::bindPipeline(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,2,incompleteSets.data(),0,nullptr);
    evidence=f.dispatch();
    require(!evidence.metadataAccepted&&(evidence.issues&dm::bit(dm::Issue::UndefinedDescriptor)),
        "restoring full tracking still rejects an unwritten required motion binding");
}
void allDescriptorLayoutsTracked(){
    ++cases;Fixture f(false);
    std::array<VkDescriptorSetLayoutBinding,2> bindings{{{5,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {7,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
    auto candidateInfo=f.setLayoutInfo;candidateInfo.bindingCount=2;candidateInfo.pBindings=bindings.data();
    const auto candidateLayout=handle<VkDescriptorSetLayout>(31);ob::setLayoutCreated(f.context,candidateLayout,&candidateInfo,VK_SUCCESS);
    auto graphics=bindings;for(auto& b:graphics)b.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;auto graphicsInfo=candidateInfo;graphicsInfo.pBindings=graphics.data();
    const auto graphicsLayout=handle<VkDescriptorSetLayout>(32);ob::setLayoutCreated(f.context,graphicsLayout,&graphicsInfo,VK_SUCCESS);
    const auto before=ob::stats(f.context).metadata.liveSets;
    auto info=f.setInfo;const auto plain=handle<VkDescriptorSet>(62);ob::setsAllocated(f.context,&info,&plain,VK_SUCCESS);
    info.pSetLayouts=&graphicsLayout;const auto graphicsSet=handle<VkDescriptorSet>(63);ob::setsAllocated(f.context,&info,&graphicsSet,VK_SUCCESS);
    auto state=ob::stats(f.context);
    require(state.metadata.liveSets==before+2,"ordinary and graphics layouts are tracked independently of continuous input shape");
    info.pSetLayouts=&candidateLayout;const auto candidateSet=handle<VkDescriptorSet>(64);ob::setsAllocated(f.context,&info,&candidateSet,VK_SUCCESS);
    require(ob::stats(f.context).metadata.liveSets==before+3,"candidate layout set is tracked alongside ordinary layouts");
    const std::array<VkDescriptorUpdateTemplateEntry,2> entries{{{5,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,0,sizeof(VkDescriptorImageInfo)},
        {7,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,sizeof(VkDescriptorImageInfo),sizeof(VkDescriptorImageInfo)}}};
    const std::array<VkDescriptorImageInfo,2> data{f.descriptor,f.descriptor};
    VkDescriptorUpdateTemplateCreateInfo templateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
    templateInfo.descriptorUpdateEntryCount=2;templateInfo.pDescriptorUpdateEntries=entries.data();templateInfo.descriptorSetLayout=candidateLayout;
    const auto candidateTemplate=handle<VkDescriptorUpdateTemplate>(96);ob::templateCreated(f.context,candidateTemplate,&templateInfo,VK_SUCCESS);
    VkDescriptorUpdateTemplateEntry plainEntry{3,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,0,sizeof(VkDescriptorImageInfo)};
    auto plainInfo=templateInfo;plainInfo.descriptorUpdateEntryCount=1;plainInfo.pDescriptorUpdateEntries=&plainEntry;plainInfo.descriptorSetLayout=f.setLayout;
    const auto plainTemplate=handle<VkDescriptorUpdateTemplate>(97);ob::templateCreated(f.context,plainTemplate,&plainInfo,VK_SUCCESS);
    ReaderProbe reader;SetLastError(0xbeef);allocations=0;countAllocations=true;
    ob::updateWithTemplate(f.context,plain,plainTemplate,&f.descriptor,dm::TemplateUpdateKind::Core,{templateProbeRead,&reader});countAllocations=false;
    require(reader.calls==1&&!allocations&&GetLastError()==0xbeef&&ob::stats(f.context).metadata.coreTemplateUpdateCalls==1,"ordinary template update is retained without hot allocation and preserves LastError");
    ob::updateWithTemplate(f.context,candidateSet,candidateTemplate,data.data(),dm::TemplateUpdateKind::Core,{templateProbeRead,&reader});
    require(reader.calls==2&&ob::stats(f.context).metadata.coreTemplateUpdateCalls==2,"candidate template update reaches the tracker with its adjacent image entries coalesced");
    auto pipelineLayoutInfo=f.layoutInfo;pipelineLayoutInfo.pSetLayouts=&candidateLayout;const auto candidatePipelineLayout=handle<VkPipelineLayout>(41);
    ob::pipelineLayoutCreated(f.context,candidatePipelineLayout,&pipelineLayoutInfo,VK_SUCCESS);
    auto pipelineInfo=f.pipelineInfo;pipelineInfo.layout=candidatePipelineLayout;const auto candidatePipeline=handle<VkPipeline>(92);
    ob::pipelineCreated(f.context,candidatePipeline,&pipelineInfo,VK_SUCCESS);
    ob::begin(f.context,f.command,&f.beginInfo,VK_SUCCESS);ob::bindPipeline(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,candidatePipeline);
    ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,candidatePipelineLayout,0,1,&candidateSet,0,nullptr);
    auto evidence=f.dispatch();require(evidence.metadataAccepted&&evidence.count==2&&evidence.bindings[0].binding==5&&evidence.bindings[1].binding==7,"candidate set yields accepted evidence");
    ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_GRAPHICS,f.layout,0,1,&plain,0,nullptr);
    ob::bindPipeline(f.context,f.command,VK_PIPELINE_BIND_POINT_GRAPHICS,f.pipeline);
    require(f.dispatch().metadataAccepted,"graphics binds leave compute evidence intact");
    ob::bindPipeline(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);
    ob::bindSets(f.context,f.command,VK_PIPELINE_BIND_POINT_COMPUTE,f.layout,0,1,&plain,0,nullptr);
    evidence=f.dispatch();require(evidence.metadataAccepted&&evidence.count==1&&evidence.bindings[0].binding==3,"ordinary set retains its template update when later bound for compute");
    ob::poolReset(f.context,f.pool,VK_SUCCESS);require(ob::stats(f.context).metadata.liveSets==0,"pool reset retires every tracked set");
    ob::summary(f.context);require(contains("\"core_template_update_calls\":2")&&!contains("\"narrowed_to_continuous_inputs\""),"summary retains current update telemetry without retired selector fields");
}
}
int main(){try{
    for(bool measured:{false,true}){Fixture f(false,16,false,measured);
        const std::array<VkDescriptorSet,1> handles{f.set};std::array<dm::ReferenceBinding,1> tokens;
        SetLastError(0x76543210);require(ob::referenceBindings(f.context,handles,tokens),"bind identity query succeeds");
        require(ob::referenceBindings(f.context,handles,tokens,ob::ReferenceQuerySource::Draw),"draw refresh query succeeds");
        require(GetLastError()==0x76543210,"query diagnostics preserve LastError");
        const auto stats=ob::stats(f.context);require(stats.referenceQueries[0].calls==unsigned(measured)&&stats.referenceQueries[1].calls==unsigned(measured),"query counters are opt-in and distinguish bind from draw");
        if(measured)require(stats.referenceQueries[0].totalNs>=stats.referenceQueries[0].waitNs,"query duration includes lock acquisition");
        ob::summary(f.context);require(!oversizedLine,"new query and consumption fields fit the summary record");}
    detailsAndBoundedSink();sourceEpochAndClear();samplingBudgetsAndRefresh();mutationAndRetirement();errorsAndHotPath();refusedDispatchHealth();currentStateRecovery();referenceOverflowTelemetry();templateAdapter();recordingFactsAssociation();sourceLayoutOwnedConsumer();ownedMemoryConsumer();
    poolResetWalk();selectedOrdinaryDispatch();continuousCompanionSets();continuousCompanionSets(true);allDescriptorLayoutsTracked();
    std::cout<<"{\"stage\":\"descriptor-image-observer\",\"passed\":true,\"cases\":"<<cases<<",\"checks\":"<<checks
        <<",\"hot_path_allocations\":"<<allocations<<",\"vulkan_loaded\":false,\"gpu_touched\":false,\"game_touched\":false}\n";return 0;
}catch(const std::exception& error){countAllocations=false;std::cerr<<"descriptor observer fixture: "<<error.what()<<"\n";return 1;}}
