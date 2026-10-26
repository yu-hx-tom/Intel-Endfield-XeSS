#include "sr_observation.hpp"
#include "descriptor_image_observer.hpp"
#include "sr_observation_dispatch02_fixture.hpp"
#include <windows.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <iostream>
#include <locale>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
namespace sr=xess_vk::sr_observation;namespace p=xess_vk::upscale_tail_probe;namespace rb=xess_vk::registration_bridge;
namespace dm=xess_vk::descriptor_image_metadata;namespace di=xess_vk::descriptor_image_observer;
unsigned checks{},layoutCalls{},memoryCalls{};bool throwSink{},reenter{},replaceDuringResolve{};
const sr::Images* replayImages{};std::size_t missingInventoryIndex=p::maxBindings;
std::vector<std::string> lines;std::mutex lineMutex;
xess_vk::FrameTimingAssociation frame{71,{71,9,false},0,true};
constexpr std::uint64_t device=11;constexpr DWORD sentinel=0x342987;
void require(bool okay,const char* message){++checks;if(!okay)throw std::runtime_error(message);}
bool contains(const char* text){std::lock_guard lock(lineMutex);return std::any_of(lines.begin(),lines.end(),[&](const auto& line){return line.find(text)!=std::string::npos;});}
std::size_t matching(const char* text){std::lock_guard lock(lineMutex);return std::count_if(lines.begin(),lines.end(),[&](const auto& line){return line.find(text)!=std::string::npos;});}
std::uint64_t fieldNumber(const std::string& line,const char* field){const auto key='"'+std::string(field)+"\":";const auto at=line.find(key);require(at!=std::string::npos,"histogram numeric field present");return std::stoull(line.substr(at+key.size()));}
void sink(const char* line){SetLastError(999);if(throwSink)throw std::runtime_error("sink failure");if(reenter)sr::summary();std::lock_guard lock(lineMutex);lines.emplace_back(line);}
xess_vk::FrameTimingAssociation frameReader(void*,std::uint32_t){SetLastError(909);return frame;}
void resolver(std::uint64_t selected,const p::Result& parsed,sr::Images& images) noexcept {
    if(selected!=device)return;
    if(replaceDuringResolve){replaceDuringResolve=false;sr::captureForTest(200);}
    for(std::size_t n=0;n<parsed.bindingCount;++n){if(n==missingInventoryIndex)continue;if(replayImages){images[n]=(*replayImages)[n];continue;}
        images[n].status=xess_vk::image_inventory::Status::LiveHandleObserved;
        images[n].record.image=parsed.bindings[n].native.imageCandidate;images[n].record.generation=100+n;
        images[n].record.description.width=320;images[n].record.description.height=180;images[n].record.description.format=VK_FORMAT_R16G16B16A16_SFLOAT;}
}
xess_vk::RecordedSourceLayout layout(void*,const xess_vk::CommandRecordingFacts&,xess_vk::RecordedImageKey image,const VkImageSubresourceRange&,VkImageLayout declared) noexcept {
    ++layoutCalls;xess_vk::RecordedSourceLayout result;result.image=image;result.queried=result.recordedLayoutResolved=true;result.recordedLayout=declared;result.issues=0;return result;
}
xess_vk::resource_memory_observer::Observation memory(void*,xess_vk::RecordedImageKey image) noexcept {
    ++memoryCalls;xess_vk::resource_memory_observer::Observation result;result.queried=true;result.binding.bindingBoundsKnown=true;result.binding.observedAliasFree=true;
    result.binding.memory={0xCC00,18};result.binding.offset=1024*image.generation;result.binding.size=512;result.binding.end=result.binding.offset+512;return result;
}
template<class T>void put(unsigned char* bytes,std::size_t offset,T value){std::memcpy(bytes+offset,&value,sizeof(value));}
struct Fixture {
    std::vector<unsigned char> moduleBytes;
    std::array<unsigned char,p::maxPayloadBytes> source{},copy{};
    std::array<unsigned char,148> constants{};std::array<unsigned char,16> rcas{};std::array<unsigned char,24> spd{};
    std::array<std::uint64_t,3> context{};std::array<unsigned char,256*16> table{};
    std::array<std::uint64_t,2> registrationGraph{};
    std::array<unsigned char,3*48> registrationNodes{};
    std::array<std::uint32_t,12> ids{};std::array<std::uint64_t,1024> page{};
    std::array<std::array<unsigned char,64>,12> objects{};
    p::Pass pass;xess_vk::CommandRecordingFacts recording;dm::DispatchEvidence evidence;
    explicit Fixture(p::Pass selected=p::Pass::Rcas,bool capture=true,bool runtime=false,bool fullGraph=false,bool replacement=false):moduleBytes(0x1EB0000),pass(selected) {
        sr::resetForTest();lines.clear();frame={71,{71,9,false},0,true};layoutCalls=memoryCalls=0;throwSink=reenter=replaceDuringResolve=false;replayImages=nullptr;missingInventoryIndex=p::maxBindings;
        const auto spec=p::profile(pass);const auto base=reinterpret_cast<std::uint64_t>(moduleBytes.data());
        registrationGraph[1]=reinterpret_cast<std::uint64_t>(registrationNodes.data());
        context[2]=reinterpret_cast<std::uint64_t>(table.data());put(moduleBytes.data(),p::nativeTableRva,reinterpret_cast<std::uint64_t>(page.data()));
        put<std::uint32_t>(source.data(),0,777);for(unsigned n=0;n<(spec.implicitGroupZ?2u:3u);++n)put<std::uint32_t>(source.data(),spec.groupsOffset+4*n,n+3);
        const auto properties=p::propertyRvas(pass);
        for(std::size_t n=0;n<spec.bindingCount;++n){const auto d=spec.imageOffset+32*n;ids[n]=11+n;
            put<std::uint64_t>(source.data(),d,n+1);put(source.data(),d+16,base+properties[n]);
            put(table.data(),16*(n+1),reinterpret_cast<std::uint64_t>(&ids[n]));put<std::uint32_t>(table.data(),16*(n+1)+12,2);
            page[ids[n]]=reinterpret_cast<std::uint64_t>(objects[n].data());put<std::uint32_t>(objects[n].data(),20,320);put<std::uint32_t>(objects[n].data(),24,180);put<std::uint64_t>(objects[n].data(),56,0xAB00+n);
            evidence.bindings[n].image={0xAB00+n,100+n};evidence.bindings[n].set={0xCD00,9};evidence.bindings[n].view={0xDE00+n,20+n};
            evidence.bindings[n].binding=n;evidence.bindings[n].declaredLayout=VK_IMAGE_LAYOUT_GENERAL;evidence.bindings[n].descriptorVersion=3;
        }
        if(spec.constantsOffset){put<std::uint32_t>(source.data(),spec.constantsOffset+8,148);put(source.data(),spec.constantsOffset+16,reinterpret_cast<std::uint64_t>(constants.data()));}
        if(spec.rcasOffset){put<std::uint32_t>(source.data(),spec.rcasOffset+8,16);put(source.data(),spec.rcasOffset+16,reinterpret_cast<std::uint64_t>(rcas.data()));}
        if(spec.spdOffset){put<std::uint32_t>(source.data(),spec.spdOffset+8,24);put(source.data(),spec.spdOffset+16,reinterpret_cast<std::uint64_t>(spd.data()));}
        copy=source;recording.device=device;recording.command=0xAAAA;recording.contextIncarnation=50;recording.rpRecordingGeneration=16;recording.recording=true;
        evidence.command={recording.command,12};evidence.pipeline={0xBB,7};evidence.recordOrdinal=20;evidence.metadataAccepted=true;evidence.count=spec.bindingCount;evidence.groups={3,4,spec.implicitGroupZ?1u:5u};
        SetLastError(sentinel);require(sr::configure(base,device,sink,frameReader,nullptr,resolver,runtime,fullGraph,replacement),"configure disabled observer once");require(GetLastError()==sentinel,"configure preserves LastError");
        require(!sr::active(device),"configuration does not activate");require(sr::activate()&&sr::active(device)==runtime,"runtime starts directly; diagnostics still wait for explicit identity");
        if(capture){sr::captureForTest();require(sr::active(device)&&!sr::active(device+1),"capture scoped to selected device");}
    }
    ~Fixture(){sr::stop();}
    const rb::Route& route() const {switch(pass){
#define SR_NATIVE_PASS(name, value) case p::Pass::value:return grb_sr_##name##_route;
#include "sr_native_passes.inc"
#undef SR_NATIVE_PASS
        case p::Pass::Rcas:return grb_sr_rcas_route;default:return grb_sr_accumulate_route;}}
    rb::BeforeResult begin(bool rightReturn=true,bool alternate=false) {
        rb::EntrySnapshot call;call.rcx=reinterpret_cast<std::uint64_t>(registrationGraph.data());call.rdx=2;call.r8=reinterpret_cast<std::uint64_t>(source.data());call.threadId=GetCurrentThreadId();
        const auto spec=p::profile(pass);
        call.returnAddress=reinterpret_cast<std::uint64_t>(moduleBytes.data())+(alternate?spec.alternateRegistrationReturn:spec.registrationReturn)+(rightReturn?0:1);
        rb::BeforeResult token;const auto& r=route();r.before(r.context,call,token);return token;
    }
    std::uint64_t registrationResult(std::uint64_t payload) {
        const auto spec=p::profile(pass);
        put(registrationNodes.data(),2*48+8,reinterpret_cast<std::uint64_t>(moduleBytes.data())+spec.thunk);
        put(registrationNodes.data(),2*48+16,payload);
        return spec.registrationReturnsGraphArray?registrationGraph[1]:payload;
    }
    void close(rb::BeforeResult token,rb::Exit exit=rb::Exit::Normal) {
        rb::CloseEvent call;call.token=token.token;call.exit=exit;call.copiedPayload=registrationResult(reinterpret_cast<std::uint64_t>(copy.data()));const auto& r=route();r.close(r.context,call);
    }
    void observe(){SetLastError(sentinel);switch(pass){
#define SR_NATIVE_PASS(name, value) case p::Pass::value:sr_##name##_observe_before(copy.data(),context.data());break;
#include "sr_native_passes.inc"
#undef SR_NATIVE_PASS
        case p::Pass::Rcas:sr_rcas_observe_before(copy.data(),context.data());break;default:sr_accumulate_observe_before(copy.data(),context.data());break;}
        require(GetLastError()==sentinel,"callback preserves LastError");}
    void publish(){const auto token=begin();require(token.published&&token.associationAvailable,"registration carries exact frame ticket");close(token);observe();}
    sr::DispatchTicket dispatch(xess_vk::RecordedImageIntervalQuery intervals={}){SetLastError(sentinel);const auto result=sr::beforeDispatch(device,recording,evidence,frame,GetCurrentThreadId(),{layout,nullptr},{memory,nullptr},intervals);require(GetLastError()==sentinel,"dispatch diagnostics preserve LastError");return result;}
};
template<class T>T handle(std::uint64_t value){return reinterpret_cast<T>(value);}
struct DescriptorFixture {
    di::Context* context{};VkCommandBuffer command{};
    VkDescriptorSet set=handle<VkDescriptorSet>(0xCD00);VkDescriptorUpdateTemplate update=handle<VkDescriptorUpdateTemplate>(0xCD50);
    std::array<VkDescriptorImageInfo,p::maxBindings> images{};
    DescriptorFixture(const Fixture& f,std::size_t count,std::uint32_t skippedMask=0,bool compact=false):command(handle<VkCommandBuffer>(f.recording.command)) {
        dm::Config config;config.images=config.views=16;config.setLayouts=config.pipelineLayouts=config.pools=config.sets=4;
        config.pipelines=config.commandPools=config.commands=config.templates=4;config.continuousDispatchSelection=true;
        context=di::create(handle<VkDevice>(device),nullptr,config);require(context!=nullptr,"production descriptor context created");
        std::array<VkDescriptorSetLayoutBinding,p::maxBindings> bindings{};
        std::array<VkDescriptorUpdateTemplateEntry,p::maxBindings> entries{};
        for(std::size_t index=0;index<p::profile(f.pass).bindingCount;++index){
            xess_vk::image_inventory::Record image{0xAB00+index,100+index,{VK_FORMAT_R16G16B16A16_SFLOAT,320,180,1,1,1,VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT,0,VK_IMAGE_TILING_OPTIMAL,VK_SHARING_MODE_EXCLUSIVE,VK_IMAGE_LAYOUT_UNDEFINED}};
            di::imageObserved(context,image);VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image=handle<VkImage>(image.image);view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=VK_FORMAT_R16G16B16A16_SFLOAT;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            const auto viewHandle=handle<VkImageView>(0xDE00+index);di::viewCreated(context,viewHandle,&view,VK_SUCCESS);
        }
        constexpr std::array<std::size_t,12> nativeOrder{9,10,11,8,3,5,7,2,0,1,4,6};std::size_t at{};
        for(std::size_t n=0;n<count;++n){while(skippedMask&(1u<<(compact?nativeOrder[at]:at)))++at;const auto index=compact?nativeOrder[at++]:at++;
            const auto type=compact&&index>=9?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            const auto binding=std::uint32_t(compact?n:20+index);
            bindings[n]={binding,type,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            entries[n]={binding,0,1,type,n*sizeof(VkDescriptorImageInfo),sizeof(VkDescriptorImageInfo)};
            images[n]={VK_NULL_HANDLE,handle<VkImageView>(0xDE00+index),type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        const auto setLayout=handle<VkDescriptorSetLayout>(0xCD10);VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount=std::uint32_t(count);sl.pBindings=bindings.data();di::setLayoutCreated(context,setLayout,&sl,VK_SUCCESS);
        const auto layout=handle<VkPipelineLayout>(0xCD20);VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount=1;pl.pSetLayouts=&setLayout;di::pipelineLayoutCreated(context,layout,&pl,VK_SUCCESS);
        const auto pool=handle<VkDescriptorPool>(0xCD30);const std::array<VkDescriptorPoolSize,2> poolSizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,12},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,12}}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=4;pi.poolSizeCount=poolSizes.size();pi.pPoolSizes=poolSizes.data();di::poolCreated(context,pool,&pi,VK_SUCCESS);
        VkDescriptorSetAllocateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};si.descriptorPool=pool;si.descriptorSetCount=1;si.pSetLayouts=&setLayout;di::setsAllocated(context,&si,&set,VK_SUCCESS);
        if(count){VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};ti.descriptorSetLayout=setLayout;
            ti.descriptorUpdateEntryCount=std::uint32_t(count);ti.pDescriptorUpdateEntries=entries.data();di::templateCreated(context,update,&ti,VK_SUCCESS);mutate();}
        const auto cp=handle<VkCommandPool>(0xCD40);VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};di::commandPoolCreated(context,cp,&ci,VK_SUCCESS);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=cp;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;di::allocate(context,&ai,&command,VK_SUCCESS);
        const auto pipeline=handle<VkPipeline>(0xBB);VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pci.layout=layout;
        pci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pci.stage.module=handle<VkShaderModule>(0xCD60);pci.stage.pName="main";
        di::pipelineCreated(context,pipeline,&pci,VK_SUCCESS);VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};di::begin(context,command,&bi,VK_SUCCESS);
        di::bindPipeline(context,command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);di::bindSets(context,command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
    }
    ~DescriptorFixture(){di::destroy(context);}
    void mutate(){di::updateWithTemplate(context,set,update,images.data(),dm::TemplateUpdateKind::Core);}
    dm::DispatchEvidence dispatch(bool diagnostic=true,std::array<std::uint32_t,3> groups={3,4,5}) {
        di::SourceQueryDemand demand;demand.diagnosticAllBindings=diagnostic;
        return di::preDispatch(context,command,groups[0],groups[1],groups[2],di::DispatchKind::Direct,nullptr,{},{},nullptr,demand);
    }
};
void replaySavedDispatch02(){
    namespace saved=sr_dispatch02_fixture;
    Fixture f(p::Pass::Accumulate);sr::Images inventory;
    put(f.source.data(),0,saved::kernelToken);
    for(std::size_t n=0;n<saved::groups.size();++n)put(f.source.data(),16+4*n,saved::groups[n]);
    const auto base=reinterpret_cast<std::uint64_t>(f.moduleBytes.data());
    for(std::size_t n=0;n<saved::callbackBindings.size();++n){const auto& b=saved::callbackBindings[n];const auto d=40+32*n;
        put(f.source.data(),d,b.graphHandle);put(f.source.data(),d+16,base+b.propertyRva);f.ids[n]=b.textureId;
        put(f.table.data(),16*std::uint32_t(b.graphHandle),reinterpret_cast<std::uint64_t>(&f.ids[n]));
        put<std::uint32_t>(f.table.data(),16*std::uint32_t(b.graphHandle)+12,2);
        put(f.moduleBytes.data(),p::nativeTableRva+8*(b.textureId>>10),reinterpret_cast<std::uint64_t>(f.page.data()));
        f.page[b.textureId&1023]=reinterpret_cast<std::uint64_t>(f.objects[n].data());
        put(f.objects[n].data(),20,b.width);put(f.objects[n].data(),24,b.height);put(f.objects[n].data(),56,b.image);
        inventory[n].status=xess_vk::image_inventory::Status::LiveHandleObserved;
        inventory[n].record={b.image,b.generation,{b.format,b.width,b.height,1,1,1,VK_SAMPLE_COUNT_1_BIT,b.usage,0,VK_IMAGE_TILING_OPTIMAL,VK_SHARING_MODE_EXCLUSIVE,VK_IMAGE_LAYOUT_UNDEFINED}};
    }
    const auto digit=[](char c){return unsigned(c>='a'?c-'a'+10:c-'0');};
    static_assert(sizeof(saved::constantsHex)==2*148+1);
    for(std::size_t n=0;n<f.constants.size();++n)f.constants[n]=static_cast<unsigned char>((digit(saved::constantsHex[2*n])<<4)|digit(saved::constantsHex[2*n+1]));
    f.copy=f.source;replayImages=&inventory;frame={saved::engineFrame,{saved::sdkId,saved::ticketGeneration,false},0,true};
    f.recording.device=device;f.recording.command=saved::command;f.recording.contextIncarnation=saved::incarnation;f.recording.rpRecordingGeneration=saved::recording;
    f.evidence={};f.evidence.command={saved::command,1};f.evidence.pipeline={saved::pipeline,saved::pipelineGeneration};
    f.evidence.pipelineLayout={saved::pipelineLayout,saved::pipelineLayoutGeneration};f.evidence.recordOrdinal=saved::ordinal;
    f.evidence.metadataAccepted=true;f.evidence.groups=saved::groups;f.evidence.count=saved::descriptorBindings.size();
    for(std::size_t n=0;n<saved::descriptorBindings.size();++n){const auto& b=saved::descriptorBindings[n];const auto& source=saved::callbackBindings[b.callbackIndex];auto& target=f.evidence.bindings[n];
        target.image={source.image,source.generation};target.set={b.set,b.setGeneration};target.view={b.view,b.viewGeneration};target.binding=b.binding;
        target.descriptorType=VkDescriptorType(b.type);target.descriptorVersion=b.version;target.imageDescription=inventory[b.callbackIndex].record.description;
    }
    f.publish();const auto ticket=f.dispatch();require(ticket.count==1,"saved dispatch-02 exact ten-image variant associates after the fixed two optional declarations are excluded");
    require(ticket.pipeline==saved::pipeline&&ticket.pipelineGeneration==saved::pipelineGeneration&&ticket.containsAccumulate&&!ticket.containsRcas,"matched Accumulate ticket retains exact saved pipeline key and pass for post-driver archive selection");
    require(ticket.extentsKnown&&ticket.renderExtent.width==1128&&ticket.renderExtent.height==634&&
        ticket.outputExtent.width==1920&&ticket.outputExtent.height==1080&&ticket.groups==saved::groups,
        "saved dispatch-02 ticket retains actual constants and typed live resource extents, independent of dispatch group size");
    require(layoutCalls==10&&memoryCalls==10&&contains("\"required_callback_bindings\":10,\"matching_required_callback_bindings\":10,\"matching_callback_bindings\":10")&&
        contains("\"omitted_optional_bindings\":[{\"property\":\"r_lanczos_lut\"")&&contains("\"property\":\"r_current_luma\"")&&contains("\"execute_permitted\":false"),
        "saved callback/dispatch metadata replay retains explicit omitted declarations and candidate-only permission");
    sr::afterDispatch(ticket,"vkCmdDispatch");require(contains("\"driver_forwarded\":true"),"saved metadata replay exercises the separate return-ticket path without a driver call");
    for(const auto optionalIndex:{4u,6u}){const auto& source=saved::callbackBindings[optionalIndex];auto& extra=f.evidence.bindings[10];extra=f.evidence.bindings[0];extra.binding=10;
        extra.image={source.image,source.generation+1};f.evidence.count=11;
        require(!f.dispatch().count&&layoutCalls==10,"saved-window optional image present with wrong generation is refused before source queries");}
    f.evidence.bindings[10].image={0xFFFFFFFF,999};require(!f.dispatch().count&&layoutCalls==10,"saved-window unknown extra image cannot be disguised as an absent optional property");f.evidence.count=10;
    require(ticket.frame.valid&&ticket.frame.engineFrame==saved::engineFrame&&ticket.frame.ticket==frame.ticket,"matched ticket retains the exact frame association used before the original dispatch");
    for(const auto& line:lines)if(line.find("\"event\":\"sr_dispatch_candidate\"")!=std::string::npos)std::cout<<line<<'\n';
    std::cout<<"saved dispatch-02 metadata replay: 12 graph bindings, 10 actual image bindings, only LUT/current-luma omitted; no GPU/game\n";
    sr::captureForTest(2);lines.clear();
    // Saved resource/constants metadata; advancing frame/ticket IDs are CPU fixture labels.
    for(std::uint64_t n=1;n<=192;++n){
        frame.engineFrame=saved::engineFrame+n;frame.ticket.sdkId=saved::sdkId+n;f.publish();
        if(n<=32)continue;
        const auto current=f.dispatch();require(current.count==1&&current.callbacks[0]==n&&current.frame.engineFrame==frame.engineFrame&&current.frame.ticket==frame.ticket,
            n>32+sr::dispatchDetailLimit?"saved metadata candidate association survives the exhausted dispatch detail quota after callback 160":
            "saved metadata callback 34 onward yields a consecutive current-frame candidate even when callback detail is not sampled");
        require(current.containsAccumulate&&!current.containsRcas&&current.extentsKnown&&current.pipelineGeneration==saved::pipelineGeneration,
            "continuous tickets retain exact candidate pass, extent and pipeline generation past the detail quota");
        if(n>33)require(current.detailMask==std::uint32_t(n<=32+sr::dispatchDetailLimit),"dispatch quota controls only the bounded detail mask");
        sr::afterDispatch(current,"vkCmdDispatch");
        ++f.evidence.bindings[0].image.generation;require(!f.dispatch().count,"every unsampled saved callback retains strict mandatory image-generation rejection");--f.evidence.bindings[0].image.generation;
    }
    require(matching("sr_callback_snapshot")==13&&matching("sr_dispatch_candidate")==128&&matching("sr_dispatch_return")==128,
        "192 callback parses and 160 candidate tickets retain only 13 callback details and 128 candidate/return detail pairs");
    require(layoutCalls==10+10*sr::dispatchDetailLimit&&memoryCalls==layoutCalls,"omitted dispatch details perform no layout or memory detail queries");
    sr::summary();require(contains("\"callback_calls\":192,\"callback_parse_attempts\":192,\"callback_detail_samples\":13")&&
        contains("\"dispatch_candidate_matches\":160,\"omitted_match_details\":32,\"driver_returns\":160")&&!contains("snapshot_samples"),
        "callback, parsing, sampled detail, matched-candidate and forwarded-return denominators stay distinct");
    sr::expireForTest();require(!sr::active(device),"continuous saved-fixture replay closes its dedicated window");
    require(contains("\"dispatch_calls\":320,\"gate_counts_exclusive\":true")&&contains("\"resource_mismatch\":160,\"matched\":160")&&
        contains("\"same_frame_groups_candidate_evaluations\":320,\"resource_qualified_candidate_evaluations\":160,\"histogram_overflow_evaluations\":0"),
        "all same-frame same-groups saved candidates contribute to dispatch and histogram denominators, including unsampled callbacks");
    std::uint64_t all{},qualified{};for(const auto& line:lines)if(line.find("\"event\":\"sr_dispatch_resource_signature_histogram\"")!=std::string::npos){
        all+=fieldNumber(line,"candidate_evaluations");qualified+=fieldNumber(line,"resource_qualified_evaluations");}
    require(matching("sr_dispatch_resource_signature_histogram")==2&&all==320&&qualified==160,
        "two exact missing/mismatched property signatures account for all 320 evaluations independently of near-match sampling");
    std::size_t bytes{};for(const auto& line:lines)bytes+=line.size()+1;
    require(bytes<2*1024*1024,"continuous CPU replay keeps callback plus dispatch details below two MiB without increasing stream quotas");
    for(const auto& line:lines)if(line.find("\"event\":\"sr_observation_summary\"")!=std::string::npos||line.find("\"event\":\"sr_dispatch_diagnostics\"")!=std::string::npos||line.find("\"event\":\"sr_dispatch_resource_signature_histogram\"")!=std::string::npos)std::cout<<line<<'\n';
    std::cout<<"saved-metadata cadence CPU replay: 192 callbacks, 160 consecutive frame labels after callback 32, 320 signature evaluations, "<<bytes<<" detail bytes; no new game observations\n";
}
void timestampExtents(){
    for(const auto pass:{p::Pass::Accumulate,p::Pass::Rcas}){
        {Fixture f(pass);put<std::int32_t>(f.constants.data(),0,320);put<std::int32_t>(f.constants.data(),4,180);
            put<std::int32_t>(f.constants.data(),0x10,320);put<std::int32_t>(f.constants.data(),0x14,180);
            f.publish();const auto ticket=f.dispatch();require(ticket.count==1&&ticket.extentsKnown&&ticket.renderExtent.width==320&&ticket.outputExtent.height==180,
                "both pass tickets carry their own matched 148-byte extent constants and typed live output image");}
        {Fixture f(pass);put<std::int32_t>(f.constants.data(),0,320);put<std::int32_t>(f.constants.data(),4,180);
            put<std::int32_t>(f.constants.data(),0x10,640);put<std::int32_t>(f.constants.data(),0x14,360);
            f.publish();const auto ticket=f.dispatch();require(ticket.count==1&&!ticket.extentsKnown,
                "constant/output-image extent disagreement keeps candidate association but refuses timestamp extent admission");}
        {Fixture f(pass);f.publish();const auto ticket=f.dispatch();require(ticket.count==1&&!ticket.extentsKnown,
                "missing positive extent constants never turn dispatch groups into a render/output estimate");}
    }
    {Fixture f(p::Pass::Accumulate);put<std::int32_t>(f.constants.data(),0,640);put<std::int32_t>(f.constants.data(),4,360);
        put<std::int32_t>(f.constants.data(),0x10,320);put<std::int32_t>(f.constants.data(),0x14,180);
        f.publish();require(!f.dispatch().extentsKnown,"Accumulate typed input-color image must also agree with render extent constants");}
}
void signatureHistogram(){
    {Fixture f(p::Pass::Accumulate);f.publish();const auto original=f.evidence;constexpr std::array<unsigned,7> required{0,1,2,3,5,7,8};
        for(unsigned mask=0;mask<=sr::resourceSignatureHistogramLimit;++mask){f.evidence=original;f.evidence.count=0;
            for(unsigned n=0;n<12;++n){bool omit{};for(unsigned bit=0;bit<required.size();++bit)if(required[bit]==n&&(mask&(1u<<bit)))omit=true;if(!omit)f.evidence.bindings[f.evidence.count++]=original.bindings[n];}
            require(bool(f.dispatch().count)==(mask==0),"histogram collection preserves every strict required-property refusal");}
        require(!f.dispatch().count,"an overflowing repeated signature remains refused");
        require(!matching("sr_dispatch_resource_signature_histogram"),"histogram performs no hot-path writes");sr::expireForTest();require(!sr::active(device),"histogram window closes on the normal expiry check");
        require(matching("sr_dispatch_resource_signature_histogram")==sr::resourceSignatureHistogramLimit&&matching("sr_dispatch_near_match")==1,"all bounded property-set histogram rows are retained independently of one near sample");
        std::uint64_t rows{},qualified{};for(const auto& line:lines)if(line.find("\"event\":\"sr_dispatch_resource_signature_histogram\"")!=std::string::npos){rows+=fieldNumber(line,"candidate_evaluations");qualified+=fieldNumber(line,"resource_qualified_evaluations");require(line.size()<32767,"histogram rows fit session sink line bounds");}
        require(rows==64&&qualified==1&&contains("\"same_frame_groups_candidate_evaluations\":66,\"resource_qualified_candidate_evaluations\":1,\"histogram_overflow_evaluations\":2"),"retained histogram plus overflow accounts for every same-frame same-groups candidate evaluation");
        require(contains("r_dilated_motion_vectors")&&contains("r_farthest_depth_mip1")&&contains("\"histogram_counts_are_dispatch_counts\":false"),"histogram uses fixed property names and keeps candidate and dispatch denominators explicit");}
    {Fixture f(p::Pass::Accumulate);f.publish();++f.evidence.bindings[4].image.generation;require(!f.dispatch().count,"present optional LUT generation mismatch refuses");
        sr::expireForTest();require(!sr::active(device),"mismatch histogram window expires");require(contains("\"missing_properties\":[],\"mismatched_properties\":[{\"property\":\"r_lanczos_lut\"")&&contains("\"resource_refused_evaluations\":1"),"histogram distinguishes present-but-wrong optional generation from missing properties");}
}
void accumulateDispatchSignature(){
    constexpr std::uint32_t optional=(1u<<4)|(1u<<6);
    for(const auto omitted:{optional,1u<<4,1u<<6,0u}){Fixture f(p::Pass::Accumulate);f.publish();DescriptorFixture descriptors(f,12-std::popcount(omitted),omitted,true);
        f.evidence=descriptors.dispatch();require(f.evidence.metadataAccepted&&f.evidence.count==std::size_t(12-std::popcount(omitted)),"production compact Accumulate descriptor metadata is complete");
        require(f.dispatch().count==1&&layoutCalls==f.evidence.count&&memoryCalls==layoutCalls,"10/11/12 image variants retain all ten required image generations");
        require(contains("\"resource_signature_scope\":\"required_callback_bindings\"")&&contains("\"required_callback_bindings\":10")&&contains("\"shader_binding_usage_verified\":false"),"association never becomes whole-graph or shader usage proof");
    }
    for(std::size_t n=0;n<p::maxBindings;++n)if(!(optional&(1u<<n))){
        {Fixture f(p::Pass::Accumulate);f.publish();DescriptorFixture descriptors(f,9,optional|(1u<<n),true);f.evidence=descriptors.dispatch();
            require(f.evidence.metadataAccepted&&!f.dispatch().count&&!layoutCalls,"every other graph resource remains mandatory through the real descriptor path");}
        {Fixture f(p::Pass::Accumulate);f.publish();DescriptorFixture descriptors(f,10,optional,true);f.evidence=descriptors.dispatch();
            for(std::size_t b=0;b<f.evidence.count;++b)if(f.evidence.bindings[b].image.handle==0xAB00+n)++f.evidence.bindings[b].image.generation;
            require(!f.dispatch().count&&!layoutCalls,"generation mismatch of every required image remains refused");}
    }
    {Fixture f(p::Pass::Accumulate);f.publish();DescriptorFixture descriptors(f,10,optional,true);f.evidence=descriptors.dispatch();
        ++frame.ticket.contextGeneration;require(!f.dispatch().count,"compact variant retains exact owner ticket guard");--frame.ticket.contextGeneration;
        ++frame.engineFrame;require(!f.dispatch().count,"compact variant retains engine frame guard");--frame.engineFrame;
        ++f.evidence.groups[0];const auto refused=f.dispatch();require(!refused.count&&!refused.pipeline&&!refused.pipelineGeneration&&!refused.containsAccumulate&&!refused.containsRcas&&!refused.frame.valid,"compact variant retains groups guard and never exposes a pipeline key, pass or frame on refusal");}
    for(const auto n:{4u,6u}){Fixture f(p::Pass::Accumulate);missingInventoryIndex=n;f.publish();DescriptorFixture descriptors(f,10,optional,true);f.evidence=descriptors.dispatch();
        require(!f.dispatch().count&&contains("\"inventory_complete\":false"),"optional actual-dispatch declarations still require complete callback inventory");}
    for(unsigned n=0;n<3;++n){Fixture f;f.publish();DescriptorFixture descriptors(f,2,1u<<n);f.evidence=descriptors.dispatch();require(f.evidence.metadataAccepted&&!f.dispatch().count,"RCAS has no optional image declarations");}
    {Fixture f(p::Pass::Accumulate);put(f.source.data(),40+4*32+16,reinterpret_cast<std::uint64_t>(f.moduleBytes.data())+0x1EDE589);f.copy=f.source;f.publish();
        require(!f.dispatch().count&&contains("\"status\":\"UnknownProperty\""),"neighboring or unknown property RVAs cannot opt into the fixed optional pair");}
}
void productionDispatchDiagnostics(){
    Fixture f(p::Pass::Accumulate);f.publish();
    {
        DescriptorFixture descriptors(f,11,1u<<5);f.evidence=descriptors.dispatch(false);
        require(f.evidence.filteredOut&&!f.dispatch().count,"production ordinary dispatch retains the product filter");
        f.evidence=descriptors.dispatch();require(f.evidence.metadataAccepted&&f.evidence.count==11&&f.evidence.diagnosticSelectionBypassed,
            "SR per-call demand sees retained layouts and early template updates outside Prepare shape");
        for(unsigned n=0;n<10;++n)require(!f.dispatch().count,"eleven of twelve images with a mandatory omission never grants association");
        f.evidence=descriptors.dispatch(true,{4,4,5});require(!f.dispatch().count,"production dimensions remain strict");
        f.evidence=descriptors.dispatch();++frame.engineFrame;require(!f.dispatch().count,"production evidence cannot borrow another frame");--frame.engineFrame;
        require(!sr::beforeDispatch(device,f.recording,f.evidence,{},GetCurrentThreadId(),{},{}).count,"missing recording frame remains refused");
        f.recording.recording=false;require(!f.dispatch().count,"invalid command recording remains refused");f.recording.recording=true;
        descriptors.mutate();f.evidence=descriptors.dispatch();require(!f.evidence.metadataAccepted&&(f.evidence.issues&dm::bit(dm::Issue::DescriptorChanged))&&!f.dispatch().count,
            "production mutation refusal reaches the SR descriptor gate");
    }
    {DescriptorFixture descriptors(f,0);f.evidence=descriptors.dispatch();require(f.evidence.metadataAccepted&&!f.evidence.count&&!f.dispatch().count,"accepted empty descriptor evidence is still not an SR association");}
    {DescriptorFixture descriptors(f,12);f.evidence=descriptors.dispatch();require(f.evidence.metadataAccepted&&f.evidence.count==12&&f.dispatch().count==1,"exact production descriptor membership still associates");}
    require(!matching("sr_dispatch_diagnostics")&&!matching("sr_dispatch_near_match")&&layoutCalls==12&&memoryCalls==12,"diagnostics perform no hot-path writes or extra source queries");
    sr::expireForTest();require(!sr::active(device),"production diagnostic window closes");
    require(matching("sr_dispatch_diagnostics")==1&&matching("sr_dispatch_near_match")==1,"one end summary and one deduplicated pipeline near-match");
    require(contains("\"dispatch_calls\":18,\"gate_counts_exclusive\":true")&&contains("\"frame_unknown\":1,\"recording_invalid\":1,\"descriptor_filtered\":1,\"descriptor_rejected\":1,\"bindings_invalid\":1")&&
        contains("\"candidate_frame_mismatch\":1,\"groups_mismatch\":1,\"resource_mismatch\":10,\"matched\":1"),"exclusive production gate counts identify the failed predicate without changing acceptance");
    require(contains("\"issue_bit\":2048,\"dispatches\":1")&&contains("\"callback_binding_count\":12,\"required_callback_bindings\":10,\"matching_required_callback_bindings\":9,\"matching_callback_bindings\":11")&&
        contains("\"missing_callback_bindings\":[{\"property\":\"supported_other\"")&&contains("\"image\":43781,\"generation\":105}"),"closed diagnostics retain an unapproved missing property and generation");
    for(const auto& line:lines)if(line.find("sr_dispatch_diagnostics")!=std::string::npos||line.find("sr_dispatch_near_match")!=std::string::npos||line.find("sr_dispatch_resource_signature_histogram")!=std::string::npos)std::cout<<line<<'\n';
    sr::captureForTest(99);sr::expireForTest();require(!sr::active(device)&&contains("\"dispatch_calls\":0,\"gate_counts_exclusive\":true"),"replacement windows do not inherit gate counts or near-matches");
    {Fixture empty(p::Pass::Accumulate);DescriptorFixture descriptors(empty,12);empty.evidence=descriptors.dispatch();require(!empty.dispatch().count,"real descriptors cannot create a missing callback candidate");
        sr::expireForTest();require(!sr::active(device)&&contains("\"no_candidate\":1"),"missing callback candidates have their own gate");}
}
void positive(){
    for(auto pass:{p::Pass::Rcas,p::Pass::Accumulate}){Fixture f(pass);f.publish();require(contains("\"registration_matched\":true"),"exact copied payload linked to callback");
        require(contains("\"inventory_complete\":true")&&contains("\"upscaler_constants_captured\":true"),"snapshot constants and exact generation metadata retained");
        auto ticket=f.dispatch();require(ticket.count==1&&ticket.callbacks[0]==1,"actual direct dispatch signature and frame match");
        require(ticket.containsAccumulate==(pass==p::Pass::Accumulate)&&ticket.containsRcas==(pass==p::Pass::Rcas),"matched Accumulate and RCAS candidate pass flags are explicit and exclusive");
        require(layoutCalls==p::profile(pass).bindingCount&&memoryCalls==layoutCalls,"queries only matching actual evidence outside callback lock");
        SetLastError(sentinel);sr::afterDispatch(ticket,"vkCmdDispatch");require(GetLastError()==sentinel&&contains("\"driver_forwarded\":true"),"actual driver return separately logged");
        require(contains("\"execute_permitted\":false")&&contains("\"shader_identity_verified\":false")&&contains("\"memory_binding\":{\"handle\":52224"),"candidate semantics and raw memory boundaries explicit");
        (void)f.dispatch();require(contains("\"unique_so_far\":false"),"second matching dispatch never remains unique");
    }
    Fixture f;auto token=f.begin();f.close(token);std::thread callback([&]{f.observe();});callback.join();require(f.dispatch().count==1&&contains("\"same_thread\":false"),"cross-thread association uses copied identity rather than TLS");
}
void rawInputAssociation(){
    for(unsigned mutation=0;mutation<5;++mutation) {
        Fixture f(p::Pass::Accumulate);f.publish();const auto tailEvidence=f.evidence;const auto tailRecording=f.recording;const auto tailFrame=frame;
        f.evidence.count=2;f.evidence.pipeline={0x1234,9};
        for(unsigned n=0;n<2;++n){auto& b=f.evidence.bindings[n];b.image={0xA000+n,100+n};b.view={0xB000+n,200+n};
            b.setIndex=0;b.binding=n?7:5;b.viewFormat=n?VK_FORMAT_R16G16_SFLOAT:VK_FORMAT_D32_SFLOAT_S8_UINT;
            b.imageDescription.width=320;b.imageDescription.height=180;}
        f.recording.commonRecordOrder=2;
        if(mutation==1)++frame.engineFrame;
        if(mutation==2)++f.recording.rpRecordingGeneration;
        if(mutation==3){++f.recording.command;f.evidence.command.handle=f.recording.command;}
        if(mutation==4)f.recording.commonRecordOrder=8;
        require(!f.dispatch().count,"raw input pattern is observation only and cannot substitute an Accumulate signature");
        frame=tailFrame;f.evidence=tailEvidence;f.recording=tailRecording;f.recording.commonRecordOrder=4;
        require(f.dispatch().containsAccumulate,"normal Accumulate signature remains matched");
        require(contains(mutation==0?"\"raw_pair_found\":true":"\"raw_pair_found\":false"),"raw candidates associate only within the same frame/CB/recording and earlier prefix");
        if(!mutation)require(contains("\"role\":\"raw_depth\"")&&contains("\"role\":\"raw_motion\"")&&
            contains("\"prepare_order\":2")&&contains("\"contents_preserved_verified\":false"),"raw game views are re-queried at the tail without inventing content lifetime or Execute permission");
    }
}
void depthIntervalQueries(){
    for(unsigned variant=0;variant<7;++variant){Fixture f(p::Pass::Accumulate);f.publish();const auto tail=f.evidence;
        f.evidence.count=2;f.recording.commonRecordOrder=2;
        for(unsigned n=0;n<2;++n){auto& b=f.evidence.bindings[n];b.image={0xA000+n,100+n};b.view={0xB000+n,200+n};
            b.setIndex=0;b.binding=n?7:5;b.viewFormat=n?VK_FORMAT_R16G16_SFLOAT:VK_FORMAT_D32_SFLOAT_S8_UINT;
            b.imageDescription.width=320;b.imageDescription.height=180;}
        require(f.dispatch().containsPrepare,"raw Prepare candidate is available for shader archival without granting an Accumulate match");
        f.evidence=tail;f.recording.commonRecordOrder=4;
        struct Query {unsigned variant{},calls{};} query{variant};
        const auto resolve=[](void* user,const xess_vk::CommandRecordingFacts& read,const xess_vk::CommandRecordingFacts& use,xess_vk::RecordedImageKey image) noexcept {
            auto& q=*static_cast<Query*>(user);++q.calls;
            xess_vk::RecordedImageInterval r;r.queried=true;r.issues=0;
            if(read.commonRecordOrder!=2||use.commonRecordOrder!=4||image.handle!=0xA000||image.generation!=100)r.issues=1;
            if(q.variant==1)r.barriers=1;if(q.variant==2)r.attachments=1;if(q.variant==3)r.transfers=1;if(q.variant==4)r.clears=1;
            if(q.variant==5)r.issues=xess_vk::bit(xess_vk::RecordedLayoutIssue::OpaqueControl);
            r.untouched=!r.issues&&!r.barriers&&!r.attachments&&!r.transfers&&!r.clears;return r;
        };
        const xess_vk::RecordedImageIntervalQuery interval{variant==6?nullptr:resolve,&query};
        for(unsigned n=0;n<130;++n)require(f.dispatch(interval).containsAccumulate,"interval evidence never changes read-only callback matching");
        require(query.calls==(variant==6?0:130),"every matched frame queries its exact read interval, beyond the detail quota");sr::summary();
        require(contains(variant==0?"\"queried\":130,\"untouched\":130,\"unknown\":0":"\"untouched\":0"),"only a resolved untouched interval is counted unchanged");
        require(contains("\"shader_identity_verified\":false"),"interval success alone cannot grant execution");
    }
}
void followingPasses(){
    Fixture f(p::Pass::Accumulate);f.publish();f.recording.commonRecordOrder=4;require(f.dispatch().containsAccumulate,"arm output-consumer observation");
    f.evidence.groups={9,9,9};f.evidence.pipeline={0xAA,23};f.recording.commonRecordOrder=6;
    f.evidence.bindings[10].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    (void)f.dispatch();require(contains("\"after_accumulate\":1,\"pipeline\":170,\"pipeline_generation\":23")&&contains("\"output_sampled_descriptor\":true"),"first following dispatch records exact pipeline and sampled output binding");
    f.recording.commonRecordOrder=7;f.evidence.bindings[10].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;(void)f.dispatch();
    require(contains("\"after_accumulate\":2")&&contains("\"output_storage_descriptor\":true"),"second output storage binding is distinguished from a proven shader read");
    f.recording.commonRecordOrder=8;(void)f.dispatch();require(matching("sr_following_dispatch")==2,"only the next two dispatches are retained");
}
void rejection(){
    {Fixture f;f.observe();require(!f.dispatch().count&&contains("\"registration_matched\":false"),"unregistered callback has no borrowed latest frame");}
    {Fixture f;const auto token=f.begin(false);require(!token.published,"unexpected producer callsite refused");f.observe();require(!f.dispatch().count,"refused registration grants no association");}
    {Fixture f;auto token=f.begin();f.copy[0]^=1;f.close(token);f.observe();require(!f.dispatch().count,"changed copied payload refused");}
    {Fixture f;auto token=f.begin();f.close(token,rb::Exit::Abnormal);f.observe();require(!f.dispatch().count,"abnormal registration cannot publish");}
    {Fixture f;f.publish();++frame.ticket.contextGeneration;require(!f.dispatch().count&&!layoutCalls,"different frame ticket refuses before queries");}
    {Fixture f;f.publish();++frame.engineFrame;require(!f.dispatch().count,"different engine frame refuses");}
    {Fixture f;f.publish();++f.evidence.bindings[0].image.generation;require(!f.dispatch().count,"reused image handle generation refuses");}
    {Fixture f;f.publish();++f.evidence.groups[0];require(!f.dispatch().count,"different dispatch dimensions refuse");}
    {Fixture f;f.publish();f.evidence.filteredOut=true;require(!f.dispatch().count,"filtered evidence cannot match");}
    {Fixture f;f.publish();f.evidence.metadataAccepted=false;require(!f.dispatch().count,"indirect or unsupported metadata cannot match");}
    {Fixture f;f.publish();f.recording.recording=false;require(!f.dispatch().count,"not recording refuses");}
    {Fixture f;f.publish();f.observe();require(!f.dispatch().count,"once-only copied association cannot be reused by another invocation");}
    {Fixture f;for(unsigned n=0;n<33;++n)f.publish();f.observe();require(!f.dispatch().count,"an unregistered unsampled callback 34 invalidates all older candidates for its pass");}
    {Fixture f;f.publish();sr::stop();const auto before=lines.size();f.observe();require(!f.dispatch().count&&lines.size()==before&&!sr::activate(),"sticky stop rejects callback, dispatch and reactivation");}
    {Fixture f;throwSink=true;f.publish();throwSink=false;sr::summary();require(contains("\"failures\":1"),"sink exception contained and accounted");}
    {Fixture f;reenter=true;f.publish();require(!lines.empty(),"sink summary reentry does not recurse or deadlock");}
}
void bounded(){
    {Fixture f;for(std::uint64_t n=0;n<sr::callbackLimit+1;++n)f.observe();require(!sr::active(device),"finite callback window closes");
        require(lines.size()<=80&&matching("sr_callback_snapshot")==71,"startup and periodic callback detail sampling remain bounded");sr::summary();
        require(contains("\"active\":false")&&contains("\"callback_calls\":2049,\"callback_parse_attempts\":2048,\"callback_detail_samples\":71"),"final summary distinguishes the limit-triggering callback from 2048 parse attempts and 71 detail samples");}
    {Fixture f;for(std::uint64_t n=0;n<sr::registrationLimit+1;++n)(void)f.begin();require(!sr::active(device),"registration budget closes a graph whose callbacks never execute");}
    {Fixture f;for(std::uint64_t n=0;n<sr::dispatchObservationLimit+1;++n)(void)sr::beforeDispatch(device,f.recording,f.evidence,{},GetCurrentThreadId(),{},{});
        require(!sr::active(device),"independent dispatch budget ends full evidence even without FSR callbacks");}
}
void windows(){
    {Fixture f(p::Pass::Rcas,false);sr::pollIdentityForTest(10,true);require(!sr::active(device),"first Present baselines a leftover capture file without arming");
        sr::pollIdentityForTest(10,true);require(!sr::active(device),"unchanged leftover identity remains unarmed");
        sr::pollIdentityForTest(11,true);require(sr::active(device),"new identity after baseline arms the first capture window");}
    {Fixture f(p::Pass::Rcas,false);sr::pollIdentityForTest(0,false);require(!sr::active(device),"first Present with no file still completes baseline");
        sr::pollIdentityForTest(12,true);require(sr::active(device),"newly created capture file after an absent baseline arms");}
    {Fixture f;replaceDuringResolve=true;f.publish();require(!f.dispatch().count&&!contains("sr_callback_snapshot"),"in-progress old callback cannot publish after window replacement");
        sr::summary();require(contains("\"window_id\":2")&&contains("\"callback_calls\":0,\"callback_parse_attempts\":0,\"callback_detail_samples\":0"),"old parse does not increment replacement-window counters");
        f.publish();require(f.dispatch().count,"replacement window accepts only its own new registration");}
    Fixture f(p::Pass::Rcas,false);f.observe();require(!f.begin().published&&!f.dispatch().count,"login callbacks and dispatch do not consume an unarmed window");
    sr::summary();require(contains("\"window_id\":0")&&contains("\"callback_calls\":0"),"waiting counts remain zero");
    sr::captureForTest(10);auto pending=f.begin();sr::captureForTest(11);f.close(pending);f.observe();require(!f.dispatch().count,"old registration close cannot publish into a replacement window");
    f.publish();const auto ticket=f.dispatch();require(ticket.count&&ticket.window==2,"new window has its own callback and dispatch identities");
    sr::captureForTest(11);require(f.dispatch().count,"unchanged file identity does not reset current candidates");
    sr::captureForTest(12);const auto count=lines.size();sr::afterDispatch(ticket,"vkCmdDispatch");require(lines.size()==count&&!f.dispatch().count,"old driver ticket and candidates cannot enter a new window");
    f.publish();sr::expireForTest();require(!sr::active(device)&&contains("\"reason\":\"duration_limit\""),"hard deadline closes observation");
    sr::captureForTest(13);require(sr::active(device),"fresh identity can rearm after a completed bounded window");
    sr::stop();sr::captureForTest(14);require(!sr::active(device),"device stop remains sticky across new file identities");
}
void stage1Sources(){
    for(bool nativeAA:{false,true})for(unsigned withRcas=0;withRcas<2;++withRcas){
        Fixture f(p::Pass::Accumulate);sr::Images snapshots;
        for(unsigned n=0;n<12;++n){snapshots[n].status=xess_vk::image_inventory::Status::LiveHandleObserved;
            snapshots[n].record.image=0xAB00+n;snapshots[n].record.generation=100+n;
            snapshots[n].record.description.width=n==10&&!nativeAA?320:160;snapshots[n].record.description.height=n==10&&!nativeAA?180:90;
            put<std::uint32_t>(f.objects[n].data(),20,snapshots[n].record.description.width);put<std::uint32_t>(f.objects[n].data(),24,snapshots[n].record.description.height);}
        replayImages=&snapshots;put<std::int32_t>(f.constants.data(),0,160);put<std::int32_t>(f.constants.data(),4,90);
        put<std::int32_t>(f.constants.data(),0x10,nativeAA?160:320);put<std::int32_t>(f.constants.data(),0x14,nativeAA?90:180);
        const auto accSource=f.source;const auto accCopy=f.copy;
        std::array<unsigned char,p::maxPayloadBytes> rcasPayload{};
        if(withRcas){
            f.pass=p::Pass::Rcas;f.source={};put<std::uint32_t>(f.source.data(),0,778);
            for(unsigned n=0;n<3;++n){put<std::uint32_t>(f.source.data(),16+4*n,n+3);const unsigned index=n==2?10:n;
                put<std::uint64_t>(f.source.data(),40+32*n,index+1);
                const std::uint64_t names[]{0x1EDE550,0x1EDE590,0x1EDE5E8};put(f.source.data(),56+32*n,reinterpret_cast<std::uint64_t>(f.moduleBytes.data())+names[n]);}
            const auto spec=p::profile(p::Pass::Rcas);put<std::uint32_t>(f.source.data(),spec.constantsOffset+8,148);
            put(f.source.data(),spec.constantsOffset+16,reinterpret_cast<std::uint64_t>(f.constants.data()));
            put<std::uint32_t>(f.source.data(),spec.rcasOffset+8,16);put(f.source.data(),spec.rcasOffset+16,reinterpret_cast<std::uint64_t>(f.rcas.data()));
            f.copy=f.source;rcasPayload=f.copy;const auto registration=f.begin();require(registration.published,"same-frame RCAS registration is published before Accumulate dispatch");
            rb::CloseEvent close;close.token=registration.token;close.exit=rb::Exit::Normal;close.copiedPayload=reinterpret_cast<std::uint64_t>(rcasPayload.data());const auto& route=f.route();route.close(route.context,close);
            f.pass=p::Pass::Accumulate;f.source=accSource;f.copy=accCopy;
        }
        f.publish();const auto accEvidence=f.evidence;
        f.evidence.count=2;f.evidence.pipeline={0x1234,9};f.recording.commonRecordOrder=2;
        for(unsigned n=0;n<2;++n){auto& b=f.evidence.bindings[n];b.image={0xA000+n,100+n};b.view={0xB000+n,200+n};b.setIndex=0;b.binding=n?7:5;
            b.viewFormat=n?VK_FORMAT_R16G16_SFLOAT:VK_FORMAT_D32_SFLOAT_S8_UINT;b.imageDescription.width=160;b.imageDescription.height=90;}
        require(f.dispatch().containsPrepare,"raw Prepare exposes a same-CB source without claiming terminal execution");
        f.evidence=accEvidence;f.recording.commonRecordOrder=4;const auto acc=f.dispatch();
                require(acc.source.valid&&acc.source.preparePipeline.handle==0x1234&&acc.source.accumulatePipeline==accEvidence.pipeline,"stage1 borrows exact same-frame source and pipeline identities");
        require(acc.terminalFsr==!withRcas,"registered RCAS defers overlay beyond Accumulate");
        if(withRcas){f.pass=p::Pass::Rcas;f.copy=rcasPayload;auto rcasImages=snapshots;rcasImages[2]=snapshots[10];replayImages=&rcasImages;sr_rcas_observe_before(rcasPayload.data(),f.context.data());
            f.evidence.count=3;f.evidence.bindings[0]=accEvidence.bindings[0];f.evidence.bindings[1]=accEvidence.bindings[1];f.evidence.bindings[2]=accEvidence.bindings[10];
            f.evidence.pipeline={0x4321,10};f.recording.commonRecordOrder=6;const auto rcas=f.dispatch();
            require(rcas.containsRcas&&rcas.terminalFsr&&rcas.source.valid&&rcas.source.images[3].image==acc.source.images[3].image,"exact same-output RCAS is terminal and retains original raw inputs");}
    }
}
void continuousRuntime(){
    Fixture f(p::Pass::Accumulate,false,true);
    require(sr::active(device),"product SR matching does not require a capture file");
    for(unsigned n=0;n<2200;++n){++frame.engineFrame;++frame.ticket.sdkId;f.publish();require(f.dispatch().count==1,"runtime matching continues beyond diagnostic callback quotas");}
    require(sr::active(device),"diagnostic detail limits do not stop product matching");
}
void auxiliaryJobMatching(){
    for(unsigned value=unsigned(p::Pass::Reactive);value<unsigned(p::Pass::Count);++value){const auto pass=p::Pass(value);
        {Fixture f(pass,true,false,true);f.publish();const auto match=f.dispatch();
            require(match.count==1&&!match.containsAccumulate&&!match.containsRcas&&!match.terminalFsr&&!match.source.valid,
                "auxiliary metadata match never pretends to be a terminal FSR pass or grants an SR source");
            require(match.nativePass==pass,"unique auxiliary ticket retains the exact native pass for bounded shader selection");
            require(contains(p::passName(pass)),"auxiliary callback retains its own pinned identity");
            ++frame.ticket.contextGeneration;require(!f.dispatch().count,"auxiliary dispatch requires exact frame ticket generation");--frame.ticket.contextGeneration;
            ++f.evidence.groups[0];require(!f.dispatch().count,"auxiliary dispatch requires the callback's actual group offset");--f.evidence.groups[0];
            ++f.evidence.bindings[0].image.generation;require(!f.dispatch().count,"auxiliary dispatch refuses a replaced image with reused handle");--f.evidence.bindings[0].image.generation;
            ++f.evidence.count;f.evidence.bindings[f.evidence.count-1]=f.evidence.bindings[0];f.evidence.bindings[f.evidence.count-1].image={0xABCDEF,12};
            {const auto extra=f.dispatch();require(!extra.count&&!extra.diagnosticArchiveCandidate,"an extra unregistered descriptor cannot join the native job or select its shader");}--f.evidence.count;
            --f.evidence.count;const auto subset=f.dispatch();
            require(!subset.count&&subset.diagnosticArchiveCandidate&&subset.nativePass==p::Pass::Unknown&&
                !subset.containsAccumulate&&!subset.containsRcas&&!subset.terminalFsr&&!subset.source.valid&&
                subset.pipeline==f.evidence.pipeline.handle&&subset.pipelineGeneration==f.evidence.pipeline.generation,
                "read-only subset archives the exact observed pipeline without granting a match or an executable source");
            ++frame.ticket.contextGeneration;require(!f.dispatch().diagnosticArchiveCandidate,"archive candidate still requires the exact frame ticket");--frame.ticket.contextGeneration;
            ++f.evidence.bindings[0].image.generation;require(!f.dispatch().diagnosticArchiveCandidate,"archive candidate rejects handle-generation conflict");--f.evidence.bindings[0].image.generation;
            ++f.evidence.groups[0];require(!f.dispatch().diagnosticArchiveCandidate,"archive candidate requires exact native groups");--f.evidence.groups[0];
            f.evidence.filteredOut=true;require(!f.dispatch().diagnosticArchiveCandidate,"archive candidate cannot bypass descriptor filtering");f.evidence.filteredOut=false;
            ++f.evidence.issues;require(!f.dispatch().diagnosticArchiveCandidate,"archive candidate requires accepted descriptor metadata without issues");--f.evidence.issues;
            const auto generation=f.evidence.pipeline.generation;f.evidence.pipeline.generation=0;
            require(!f.dispatch().diagnosticArchiveCandidate,"archive candidate requires an observed pipeline generation");f.evidence.pipeline.generation=generation;
            ++f.evidence.count;
            require(f.dispatch().count==1,"unmodified exact auxiliary match remains usable after refusals");
            sr::summary();require(contains("\"full_graph_observation\":true"),"summary discloses the expensive full graph diagnostic scope");}
        {Fixture f(pass);require(!f.begin().published,"default SR path does not record new auxiliary registrations");f.observe();
            require(!f.dispatch().count&&!contains("sr_callback_snapshot"),"default path ignores the six opt-in auxiliary callbacks");}
        {Fixture f(pass,false,true,true);f.publish();--f.evidence.count;const auto subset=f.dispatch();
            require(!subset.count&&!subset.diagnosticArchiveCandidate,"runtime SR never gains partial-signature archive admission");}
    }
    {Fixture f(p::Pass::Pyramid,true,false,true);auto registration=f.begin(true,true);
        require(registration.published&&registration.associationAvailable,"second audited Pyramid call site has its own exact return address");f.close(registration);f.observe();require(f.dispatch().count==1,"second Pyramid call site reaches normal exact matching");
        require(!f.begin(false,true).published,"one-byte adjacent Pyramid return address is refused");}
    {Fixture f(p::Pass::Reactive,true,false,true);std::array<std::array<unsigned char,p::maxPayloadBytes>,96> copies{};
        const auto firstFrame=frame;
        for(auto& copy:copies){auto token=f.begin();require(token.published,"bounded registration storage holds six-job graph with frames in flight");copy=f.source;
            rb::CloseEvent close;close.token=token.token;close.exit=rb::Exit::Normal;close.copiedPayload=f.registrationResult(reinterpret_cast<std::uint64_t>(copy.data()));
            const auto& route=f.route();require(route.close(route.context,close),"each pending copied native payload retained independently");++frame.engineFrame;++frame.ticket.sdkId;}
        frame=firstFrame;
        for(auto& copy:copies){sr_reactive_observe_before(copy.data(),f.context.data());require(f.dispatch().count==1,"retained full graph registration is consumed by its exact copied address and frame");++frame.engineFrame;++frame.ticket.sdkId;}
    }
    {Fixture f(p::Pass::Reactive,true,false,true);
        for(unsigned n=0;n<18;++n)f.publish();
        require(!f.dispatch().count&&layoutCalls==0&&memoryCalls==0,"too many identical native job candidates refuse the whole match before any source query; no truncated ticket");
        sr::summary();require(contains("\"failures\":1"),"candidate overflow is visible in diagnostics");}
}
void jsonLocale(){
    struct Comma:std::numpunct<char>{char do_decimal_point()const override{return ',';}char do_thousands_sep()const override{return '.';}std::string do_grouping()const override{return "\3";}};
    Fixture f;struct Restore{std::locale old;~Restore(){std::locale::global(old);}} restore{std::locale::global(std::locale(std::locale::classic(),new Comma))};
    put(f.constants.data(),0x74,1.25f);f.publish();const auto ticket=f.dispatch();sr::afterDispatch(ticket,"vkCmdDispatch");sr::summary();sr::captureForTest(11111);
    require(contains("\"delta_pre_exposure_candidate\":1.25")&&contains("\"image_candidate\":43776")&&contains("\"offset\":102400")&&contains("\"file_last_write\":11111"),"JSON numeric fields use classic locale under comma decimals and grouped integers");
}
void consumerAuditBounds(){
    namespace ca=xess_vk::sr_consumers;namespace stc=xess_vk::sr_tail_commands;
    const stc::Key recording{11,1,2,3,4,5};const xess_vk::FrameTimingAssociation identity{1,{1,1,false},0,true};
    const auto image=[](std::uint64_t handle,std::uint64_t offset){ca::Image out;out.key={handle,1};out.memory.queried=true;
        auto& b=out.memory.binding;b.resource={xess_vk::resource_memory_facts::ResourceKind::Image,{handle,1}};
        b.memory={99,1};b.bindingBoundsKnown=b.paddedAliasRangeKnown=true;b.offset=b.paddedAliasBegin=offset;b.size=64;b.end=b.paddedAliasEnd=offset+64;return out;};
    ca::Audit audit;require(audit.command(recording),"consumer audit admits an exact recording");const auto output=image(10,256);
    audit.produce(recording,identity,output,2);require(audit.outputs().size()==1,"consumer audit retains producer");
    require(audit.command(recording),"consumer command advances sequence");audit.use(recording,ca::Kind::Compute,{&output,1},true);
    require(audit.outputs()[0].internalReferences==1&&audit.hits().empty(),"same recording known FSR reference is internal");
    auto reused=output;reused.key.generation=2;reused.memory={};audit.use(recording,ca::Kind::Graphics,{&reused,1});
    require(audit.hits().empty()&&audit.stats().unknownMemory==1,"numeric image handle reuse is not an exact generation match");
    auto alias=image(20,280);alias.memory.binding.issues=xess_vk::resource_memory_facts::bit(xess_vk::resource_memory_facts::Issue::Overlap);
    audit.use(recording,ca::Kind::Transfer,{&alias,1});require(audit.hits().size()==1&&audit.hits()[0].alias,"overlapping known memory is a potential alias consumer");
    alias.memory.binding.memory.generation=2;audit.use(recording,ca::Kind::Transfer,{&alias,1});require(audit.hits().size()==1,"different allocation generation is not the same alias");
    auto cross=recording;++cross.recording;audit.use(cross,ca::Kind::Compute,{&output,1},true);
    require(audit.hits().size()==2&&audit.hits().back().crossRecording,"cross recording is not silently inferred internal");
    for(unsigned n=0;n<ca::hitLimit+10;++n)audit.use(recording,ca::Kind::Graphics,{&output,1});
    require(audit.hits().size()==ca::hitLimit&&audit.stats().hitDrops==12,"hit sample limit retains totals and records dropped samples");
    require(audit.outputs()[0].externalReferences==44,"sample exhaustion does not undercount potential consumers");
    audit.end(recording,true);require(audit.stats().endedRecordings==1,"successful actual recording end is retained");
    ca::Audit bounds;require(!bounds.command({}),"missing recording identity is refused");
    for(unsigned n=0;n<ca::outputLimit+1;++n)bounds.produce(recording,identity,image(100+n,64*n),2);
    require(bounds.outputs().size()==ca::outputLimit&&bounds.stats().outputDrops==1,"output capacity refuses rather than evicts");
    ca::Audit frames;for(unsigned n=0;n<ca::frameLimit+1;++n){auto f=identity;f.engineFrame=n+1;f.ticket.sdkId=n+1;frames.produce(recording,f,image(200+n,64*n),2);}
    require(frames.outputs().size()==ca::frameLimit&&frames.stats().frameDrops==1,"only first eight producer frames are retained");
    for(unsigned n=0;n<ca::commandLimit+2;++n)bounds.command(recording,false);
    require(bounds.stats().commands==ca::commandLimit&&bounds.stats().commandDrops==3,"command limit includes invalid event and cannot wrap");
    require(bounds.stats().unknownCommands==ca::commandLimit-1,"unknown metadata never becomes complete coverage");
    stc::DescriptorSets sets;sets.bindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sets.storedSets=sets.setCount=1;sets.layout=55;sets.sets[0]=99;
    for(unsigned n=0;n<17;++n){auto key=recording;key.command=100+n;bounds.bindGraphics(key,sets,true);}
    require(bounds.stats().graphicsDrops==1,"bounded graphics recording state never evicts silently");
    bounds.end(stc::Key{11,1,100,3,4,5},true);require(!bounds.graphics(stc::Key{11,1,100,3,4,5}),"end clears exact graphics recording");
    ca::Audit graphics;graphics.bindGraphics(recording,sets,true);graphics.graphicsDraw(recording);
    require(graphics.stats().graphicsDrawsUnknown==1,"window starting mid recording cannot assume no earlier bound sets");
    graphics.begin(recording);graphics.bindGraphics(recording,sets,true);graphics.graphicsDraw(recording);
    require(graphics.stats().graphicsDrawsKnown==1&&graphics.graphics(recording)->sets[0]==99,"observed begin and every bind give a complete conservative bound-set superset");
    sets.layout=66;sets.firstSet=1;sets.sets[0]=100;graphics.bindGraphics(recording,sets,true);
    require(graphics.graphics(recording)->sets[0]==99&&graphics.graphics(recording)->sets[1]==100,"layout disturbance retains extra sets rather than dropping possible references");
    graphics.opaqueGraphics(recording);graphics.graphicsDraw(recording);
    require(graphics.stats().graphicsDrawsUnknown==2,"push descriptors or secondary state invalidate the binding proof until begin");
    std::array<char,64> opaqueName{};opaqueName.fill('X');opaqueName[0]='"';graphics.noteOpaque(recording,opaqueName,55);graphics.noteOpaque(recording,opaqueName,56);
    require(graphics.opaqueCommands().size()==1&&graphics.opaqueCommands()[0].count==2&&graphics.opaqueCommands()[0].name[0]=='_'&&graphics.opaqueCommands()[0].name.back()==0,
        "opaque command names are bounded JSON-safe copied facts with retained counts");
}
void consumerWriteRecording(){
    namespace ca=xess_vk::sr_consumers;namespace stc=xess_vk::sr_tail_commands;
    const stc::Key first{11,1,2,3,4,5};auto next=first;++next.recording;
    const xess_vk::FrameTimingAssociation f0{1,{1,1,false},0,true},f1{2,{2,1,false},0,true};
    ca::Image image;image.key={10,1};ca::Audit audit;
    audit.command(first);audit.produce(first,f0,image,2);
    audit.command(next);audit.use(next,ca::Kind::Compute,{&image,1},true);
    require(audit.hits().size()==1&&audit.hits()[0].crossRecording,"a new recording before its write retains the possible old-content reference");
    audit.command(next);audit.produce(next,f1,image,2);
    audit.command(next);audit.use(next,ca::Kind::Compute,{&image,1},true);
    require(audit.outputs()[0].internalReferences==1&&audit.outputs()[0].externalReferences==1,
        "after the next observed write, native use in that recording is not attributed forever to the first producer");
    require(audit.outputs()[0].recording==first&&audit.outputs()[0].latest.recording==next&&audit.outputs()[0].latest.site==2,
        "first and latest observed writers are retained independently");
    require(audit.hits()[0].producer.recording==first&&audit.hits()[0].producer.site==1&&audit.outputs()[0].nativeCrossReferences==1,
        "a saved old-content reference is not rewritten when a later producer arrives");
    audit.produce(next,f1,image,3);
    require(audit.outputs()[0].latest.site==3,"a distinct observed command sequence is a distinct write site");
    audit.produce(next,f1,image,3);
    require(audit.outputs()[0].latest.site==3,"multiple image/mip descriptors of one dispatch do not invent another write site");
    auto sampled=image;sampled.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    audit.command(next);audit.use(next,ca::Kind::Compute,{&sampled,1},false,{20,1},99);
    require(!audit.hits().back().crossRecording&&audit.hits().back().producer.recording==next&&
        audit.hits().back().recordOrder==99&&audit.hits().back().descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        "same recording unknown shader references keep exact descriptor and producer identity");
    audit.end(first,true);audit.end(first,true);audit.end(next,true);
    require(audit.stats().endedRecordings==2,"each observed producer recording end is counted once, including overwritten outputs");
}
void consumerFiniteScope(){
    namespace ca=xess_vk::sr_consumers;namespace stc=xess_vk::sr_tail_commands;
    ca::Audit audit;ca::Image image;image.key={10,1};stc::Key key{11,1,2,1,4,5};
    xess_vk::FrameTimingAssociation frame{1,{1,1,false},0,true};
    for(std::size_t n=0;n<ca::frameLimit;++n){key.recording=n+1;frame.engineFrame=frame.ticket.sdkId=n+1;
        require(audit.producerFrame(key,frame,280),"each of the first eight producer frames is admitted");
        audit.command(key);audit.produce(key,frame,image,2,280);audit.end(key,true);}
    ++key.recording;++frame.engineFrame;++frame.ticket.sdkId;
    require(audit.command(key,true,false)&&audit.use(key,ca::Kind::Compute,{&image,1},false,{20,1},184),
        "successor prefix references are retained before the next producer boundary");
    require(!audit.producerFrame(key,frame,278)&&audit.closed()&&audit.boundary().recording==key&&audit.boundary().order==278,
        "ninth producer closes the finite scope with explicit identity");
    const auto before=audit.stats();
    for(unsigned n=0;n<10000;++n){audit.command(key);audit.state(key);audit.use(key,ca::Kind::Compute,{&image,1});}
    require(audit.stats().commands==before.commands&&audit.stats().references==before.references&&audit.stats().commandDrops==0&&
        audit.stats().stateCommands==before.stateCommands&&audit.stats().frameDrops==1,
        "finite scope does not record the remaining scene as hundreds of thousands of budget failures");
    require(audit.producerFrames().size()==ca::frameLimit&&audit.stats().endedRecordings==ca::frameLimit,
        "producer frame ends remain available independently of a reused image's latest writer");
    ca::Audit split;key.recording=1;frame.engineFrame=frame.ticket.sdkId=1;
    split.producerFrame(key,frame);++key.command;split.producerFrame(key,frame);split.producerFrame(key,frame);
    require(split.stats().unknownCommands==1&&split.producerFrames()[0].multipleRecordings,
        "one frame across multiple recordings is explicitly incomplete, not inferred ordered");
    ca::Audit shaders;
    require(!shaders.requestShader({0,1})&&!shaders.requestShader({1,0}),"unknown shader requests require exact live pipeline identity");
    for(unsigned n=0;n<ca::consumerShaderLimit;++n){require(shaders.requestShader({20+n,1}),"bounded consumer shader admitted");
        require(!shaders.requestShader({20+n,1}),"same pipeline generation is requested once");}
    require(!shaders.requestShader({20,2})&&shaders.stats().shaderRequestDrops==1,
        "pipeline reuse does not reuse another generation's archived identity, and capacity remains explicit");
}
dm::SetImageSnapshot consumerSetSnapshot;sr::ConsumerImages consumerAttachmentsSnapshot;unsigned consumerSetCalls{};
dm::SetImageSnapshot consumerSet(void*,std::uint64_t) noexcept {++consumerSetCalls;SetLastError(999);return consumerSetSnapshot;}
sr::ConsumerImages consumerAttachments(void*,const xess_vk::CommandRecordingFacts&,const xess_vk::sr_tail_commands::Command&) noexcept {return consumerAttachmentsSnapshot;}
void consumerIntegration(){
    namespace stc=xess_vk::sr_tail_commands;
    {
        Fixture f(p::Pass::Accumulate,true,false,true);const auto token=sr::consumerWindow(device);require(token!=0,"full graph arms consumer collector");
        sr::consumerBegin(device,token,stc::key(f.recording));
        auto evidence=f.evidence;evidence.count=2;evidence.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        evidence.bindings[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;const auto output=evidence.bindings[0].image;
        SetLastError(sentinel);sr::consumerDispatch(device,token,f.recording,evidence,frame,2,evidence.bindings[1].image,{memory,nullptr});
        require(GetLastError()==sentinel,"consumer dispatch preserves caller error");
        auto use=evidence;use.count=1;use.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        sr::consumerDispatch(device,token,f.recording,use,frame,-1,{},{});
        consumerSetSnapshot={};consumerSetSnapshot.known=true;consumerSetSnapshot.count=1;consumerSetSnapshot.bindings[0]=use.bindings[0];consumerSetCalls=0;
        stc::Command bind;bind.kind=stc::Kind::BindDescriptorSets;bind.descriptors.bindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        bind.descriptors.layout=1;bind.descriptors.setCount=bind.descriptors.storedSets=1;bind.descriptors.sets[0]=7;
        const sr::ConsumerQueries queries{consumerSet,consumerAttachments,{memory,nullptr},nullptr};
        sr::consumerCommand(device,token,f.recording,bind,queries);stc::Command draw;draw.kind=stc::Kind::Draw;
        sr::consumerCommand(device,token,f.recording,draw,queries);
        require(consumerSetCalls==1&&GetLastError()==sentinel,"draw queries current descriptors and preserves caller error");
        consumerAttachmentsSnapshot={};consumerAttachmentsSnapshot.complete=true;consumerAttachmentsSnapshot.count=1;consumerAttachmentsSnapshot.images[0]=output;
        stc::Command pass;pass.kind=stc::Kind::BeginRendering;sr::consumerCommand(device,token,f.recording,pass,queries);
        stc::Command opaque;opaque.kind=stc::Kind::Other;std::memcpy(opaque.otherName.data(),"vkCmdExecuteCommands",21);sr::consumerCommand(device,token,f.recording,opaque,queries);
        sr::consumerImage(device,token,f.recording,output,{});sr::consumerEnd(device,token,stc::key(f.recording),VK_SUCCESS);
        sr::captureForTest(1234);
        require(contains("\"event\":\"sr_consumer_audit\"")&&contains("\"potential_external_references\":4"),"compute draw attachment and transfer all feed consumer totals");
        require(contains("\"event\":\"sr_consumer_opaque_command\"")&&contains("\"name\":\"vkCmdExecuteCommands\""),"unknown command names remain actionable without relaunching for another log field");
        require(contains("\"external_consumer_count\":null")&&contains("\"graphics_draws_bound_sets_known\":1"),"complete bound-set superset still does not claim complete future consumer proof");
        require(!contains(("\"image\":"+std::to_string(evidence.bindings[1].image.handle)+",").c_str()),"exact final output is excluded from intermediate write set");
        lines.clear();const auto newWindow=sr::consumerWindow(device);require(newWindow!=token,"capture rearm changes consumer epoch");
        sr::consumerDispatch(device,token,f.recording,evidence,frame,2,{},{});
        sr::consumerImage(device,token,f.recording,output,{});sr::consumerCommand(device,token,f.recording,draw,queries);
        sr::captureForTest(1235);require(contains("\"event\":\"sr_consumer_audit\",\"window_id\":2,\"commands\":0"),"stale driver return and snapshot cannot enter a replacement window");
    }
    {
        Fixture f(p::Pass::Accumulate,true,false,true);const auto token=sr::consumerWindow(device);auto evidence=f.evidence;
        evidence.count=1;evidence.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sr::consumerDispatch(device,token,f.recording,evidence,frame,2,{},{});
        auto use=evidence;use.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;use.pipeline={200,1};
        SetLastError(sentinel);
        require(sr::consumerDispatch(device,token,f.recording,use,frame,-1,{},{}),"positive unknown compute reference requests the actual shader");
        require(GetLastError()==sentinel&&!sr::consumerDispatch(device,token,f.recording,use,frame,-1,{},{}),"archive requests preserve caller error and deduplicate generations");
        for(unsigned n=0;n<40;++n)sr::consumerDispatch(device,token,f.recording,use,frame,-1,{},{});
        use.pipeline={201,1};
        require(sr::consumerDispatch(device,token,f.recording,use,frame,-1,{},{}),"unknown shader discovery continues after the 32 reference samples fill");
        use.bindings[0].image={999,999};use.pipeline={202,1};
        require(!sr::consumerDispatch(device,token,f.recording,use,frame,-1,{},{}),"unrelated compute pipeline is not archived");
        for(unsigned n=1;n<=xess_vk::sr_consumers::frameLimit;++n){++frame.engineFrame;++frame.ticket.sdkId;++f.recording.rpRecordingGeneration;
            sr::consumerDispatch(device,token,f.recording,evidence,frame,2,{},{});}
        require(!sr::consumerActive(device)&&!sr::consumerWindow(device),"production collector stops copying dispatch snapshots at its producer boundary");
        sr::captureForTest(909);
        require(contains("\"scope_closed\":true")&&contains("\"consumer_shader_requests\":2")&&contains("\"producer_observation_is_gpu_dominance\":false"),
            "closed scope logs CPU observation boundaries and real shader requests, never GPU dominance");
        require(sr::consumerActive(device)&&!sr::consumerDispatch(device,token,f.recording,evidence,frame,2,{},{}),
            "rearming opens a new scope but cannot admit a stale request");
    }
    {
        Fixture f(p::Pass::Accumulate,true,false,true);auto evidence=f.evidence;evidence.count=1;evidence.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sr::consumerDispatch(device,sr::consumerWindow(device),f.recording,evidence,frame,0,{},{});sr::captureForTest(2);
        require(contains("\"outputs_recorded\":0"),"motion input producer is kept out of removable intermediates");
    }
    for(bool runtime:{false,true}){
        Fixture f(p::Pass::Accumulate,true,runtime,false);require(!sr::consumerActive(device)&&!sr::consumerWindow(device),"ordinary observation and continuous runtime do not enable consumer collector");
        sr::consumerImage(device,1,f.recording,f.evidence.bindings[0].image,{});sr::captureForTest(2);
        require(!contains("\"event\":\"sr_consumer_audit\""),"product route emits no consumer trace");
    }
    {
        Fixture f(p::Pass::Accumulate,true,false,true);const auto token=sr::consumerWindow(device);auto evidence=f.evidence;evidence.count=1;
        evidence.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;f.recording.commonRecordOrder=UINT64_MAX;
        for(unsigned n=0;n<xess_vk::sr_consumers::outputLimit;++n){evidence.bindings[0].image={UINT64_MAX-1024+n,UINT64_MAX};
            sr::consumerDispatch(device,token,f.recording,evidence,frame,2,{},{});}
        for(unsigned n=0;n<xess_vk::sr_consumers::outputLimit;++n)sr::consumerImage(device,token,f.recording,{UINT64_MAX-1024+n,UINT64_MAX},{});
        sr::captureForTest(2);require(matching("\"event\":\"sr_consumer_output\"")==64&&matching("\"event\":\"sr_consumer_reference\"")==32,
            "full bounded consumer output is emitted in separate records without silent sink truncation");
        std::lock_guard lock(lineMutex);for(const auto& line:lines)if(line.find("\"event\":\"sr_consumer_")!=std::string::npos)
            require(line.size()<32767&&line.back()=='}',"every maximum-width consumer record fits the production sink");
    }
}
void consumerPreludeBudget(){
    namespace stc=xess_vk::sr_tail_commands;
    Fixture f(p::Pass::Accumulate,true,false,true);const auto token=sr::consumerWindow(device);
    sr::consumerBegin(device,token,stc::key(f.recording));
    stc::Command bind;bind.kind=stc::Kind::BindDescriptorSets;bind.descriptors.bindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    bind.descriptors.layout=5;bind.descriptors.setCount=bind.descriptors.storedSets=1;bind.descriptors.sets[0]=7;
    consumerSetSnapshot={};consumerSetSnapshot.known=true;consumerSetSnapshot.count=1;consumerSetSnapshot.bindings[0]=f.evidence.bindings[0];consumerSetCalls=0;
    const sr::ConsumerQueries queries{consumerSet,consumerAttachments,{},nullptr};
    stc::Command draw;draw.kind=stc::Kind::Draw;stc::Command viewport;viewport.kind=stc::Kind::Other;
    std::memcpy(viewport.otherName.data(),"vkCmdSetViewport",17);
    for(std::size_t n=0;n<xess_vk::sr_consumers::commandLimit+32;++n){
        sr::consumerCommand(device,token,f.recording,bind,queries);
        sr::consumerCommand(device,token,f.recording,viewport,queries);
        sr::consumerCommand(device,token,f.recording,draw,queries);
    }
    auto evidence=f.evidence;evidence.count=1;evidence.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sr::consumerDispatch(device,token,f.recording,evidence,frame,2,{},{});
    sr::consumerCommand(device,token,f.recording,draw,queries);
    sr::consumerEnd(device,token,stc::key(f.recording),VK_SUCCESS);sr::captureForTest(2);
    require(contains("\"outputs_recorded\":1")&&contains("\"potential_external_references\":1"),
        "pre-FSR scene work must not exhaust consumer budget before the first intermediate producer");
    require(consumerSetCalls==1,"prelude draws do not query descriptor or memory snapshots before there is a watched output");
}
void dispatchShadow(){
    namespace sh=xess_vk::sr_dispatch_shadow;
    namespace catalog=xess_vk::sr_native_shaders;
    require(catalog::profiles.size()==9,"catalog includes all nine retained fingerprints");
    for(std::size_t n=0;n<catalog::profiles.size();++n){const auto& profile=catalog::profiles[n];
        require(profile.sha256.size()==64&&std::all_of(profile.sha256.begin(),profile.sha256.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"catalog hashes are exact lower-case SHA256");
        require(catalog::find(profile.sha256)==&profile,"catalog retains exact fingerprint identity");
        for(std::size_t other=0;other<n;++other)require(profile.sha256!=catalog::profiles[other].sha256,"catalog cannot alias different stages by fingerprint");
        auto changed=std::string(profile.sha256);changed[0]=changed[0]=='a'?'b':'a';
        require(catalog::policy(changed)==catalog::Policy::KeepUnknown,"one-byte mismatched shader is kept, never admitted by stage name");
    }
    require(catalog::policy("d8fcfe8f717806f6fb8272ec85c9f0a81019ec548d8c81304d589f652f068cff")==catalog::Policy::KeepMotionInput,
        "raw motion producer is explicitly retained despite its legacy Reactive callback label");
    require(catalog::profiles[0].callback==p::Pass::Reactive&&!catalog::profiles[0].prepare,"motion decode remains distinct from FSR Prepare");
    require(std::count_if(catalog::profiles.begin(),catalog::profiles.end(),[](const auto& x){return x.callback==p::Pass::Pyramid;})==2,
        "both Pyramid branches have distinct fingerprints");
    require(catalog::policy(catalog::profiles[1].sha256)==catalog::Policy::RequiresConsumerProof&&catalog::policy("")==catalog::Policy::KeepUnknown,
        "remaining shaders need consumer proof; absence is never a skip policy");
    xess_vk::image_inventory::Description source{},destination{};source.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;destination.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    source.samples=destination.samples=VK_SAMPLE_COUNT_1_BIT;
    const auto sourceFeatures=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const auto destinationFeatures=VK_FORMAT_FEATURE_BLIT_DST_BIT;
    require(sh::blitStaticChecks(source,destination,sourceFeatures,destinationFeatures)==63,"blit static checks include both formats, usage, filter and samples");
    source.usage=0;require(!(sh::blitStaticChecks(source,destination,sourceFeatures,destinationFeatures)&sh::SourceUsage),"destination usage alone cannot qualify the blit source");source.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    destination.usage=0;require(!(sh::blitStaticChecks(source,destination,sourceFeatures,destinationFeatures)&sh::DestinationUsage),"blit needs transfer destination usage");destination.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    require(!(sh::blitStaticChecks(source,destination,VK_FORMAT_FEATURE_BLIT_SRC_BIT,destinationFeatures)&sh::LinearFilter),"linear-filter support is independent of blit source support");
    require(!(sh::blitStaticChecks(source,destination,VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,destinationFeatures)&sh::SourceFormat),"filter support cannot stand in for blit source support");
    require(!(sh::blitStaticChecks(source,destination,sourceFeatures,0)&sh::DestinationFormat),"missing destination format capability remains explicit");
    destination.samples=VK_SAMPLE_COUNT_4_BIT;require(!(sh::blitStaticChecks(source,destination,sourceFeatures,destinationFeatures)&sh::SingleSample),"multisampled target cannot be blitted");
    destination.samples=VK_SAMPLE_COUNT_1_BIT;source.samples=VK_SAMPLE_COUNT_2_BIT;
    require(!(sh::blitStaticChecks(source,destination,sourceFeatures,destinationFeatures)&sh::SingleSample),"multisampled source cannot be blitted");
    sh::Audit audit;sh::Dispatch d;d.command=100;d.recording=2;d.incarnation=3;d.order=4;d.pipeline=5;d.pipelineGeneration=6;
    d.exactMask=sh::prepareBit;d.api=sh::Api::Direct;
    xess_vk::FrameTimingAssociation identity{71,{71,9,false},0,true};
    audit.forwarded(identity,d);require(audit.count()==1&&audit.frame(0).count==1,"shadow records only copied driver-return facts");
    audit.forwarded(identity,d);require(audit.stats().duplicates==1&&audit.frame(0).count==1,"duplicate driver-return identity cannot inflate the plan");
    ++d.pipelineGeneration;audit.forwarded(identity,d);require(audit.frame(0).conflictingDuplicate,"same command position with changed pipeline is visibly ambiguous");--d.pipelineGeneration;
    d.order=3;audit.forwarded(identity,d);require(audit.frame(0).nonIncreasingOrder,"recording order is checked instead of treating sink order as GPU order");
    ++d.command;d.order=2;audit.forwarded(identity,d);require(audit.frame(0).multipleRecordings,"different command buffers remain explicit, not falsely ordered by their local ordinals");
    ++identity.ticket.contextGeneration;audit.forwarded(identity,d);require(audit.count()==2,"same engine frame in another ticket generation is separate");
    identity.valid=false;audit.forwarded(identity,d);require(audit.stats().invalid==1,"unknown frame identity is rejected");identity.valid=true;
    d.incarnation=0;audit.forwarded(identity,d);require(audit.stats().invalid==2,"missing incarnation cannot borrow a reused command handle");d.incarnation=3;
    for(unsigned n=0;n<sh::dispatchLimit+4;++n){d.order=10+n;audit.forwarded(identity,d);}
    require(audit.frame(1).count==sh::dispatchLimit&&audit.frame(1).dropped==5&&audit.stats().dispatchLimitDrops==5,"bounded frame truncation is counted, never silent");
    for(unsigned n=0;n<sh::frameLimit+2;++n){identity.engineFrame=80+n;d.order=100+n;audit.forwarded(identity,d);}
    require(audit.count()==sh::frameLimit&&audit.stats().frameLimitDrops==4,"new frames cannot evict the first bounded samples");
    {
        Fixture f(p::Pass::Reactive,true,false,true);f.recording.commonRecordOrder=10;f.publish();
        const std::array apis{"vkCmdDispatch","vkCmdDispatchIndirect","vkCmdDispatchBase","vkCmdDispatchBaseKHR"};
        for(const auto* api:apis){auto ticket=f.dispatch();require(ticket.shadowObserved&&ticket.shadow.exactMask==sh::passBit(p::Pass::Reactive)&&
            ticket.shadow.registeredMask==sh::passBit(p::Pass::Reactive),"full graph stores the currently available plan, not a future completed plan");
            sr::afterDispatch(ticket,api,true);++f.recording.commonRecordOrder;}
        --f.evidence.count;auto partial=f.dispatch();require(partial.shadowObserved&&!partial.count&&partial.shadow.exactMask==0&&
            partial.shadow.candidateMask==sh::passBit(p::Pass::Reactive),"partial signatures remain hints even inside a shadow frame");
        sr::afterDispatch(partial,"vkCmdDispatch");sr::expireForTest();require(!sr::active(device),"finite window closure emits the owned audit");
        require(contains("sr_dispatch_shadow_frame")&&contains("\"consumer_coverage_complete\":false")&&contains("\"external_consumer_count\":null")&&
            contains("\"would_replace\":false")&&contains("\"shader_fingerprint_known\":true"),"emission cannot mistake missing consumer coverage or a known fingerprint for replacement admission");
        for(unsigned api=1;api<=4;++api)require(contains(("\"api\":"+std::to_string(api)).c_str()),"all four actual wrapper labels survive audit output");
        require(contains("\"partial_candidate_mask\":8"),"the partial candidate's audit survives count-zero tickets");
    }
    {Fixture f(p::Pass::Reactive,true,false,true);f.recording.commonRecordOrder=1;f.publish();auto old=f.dispatch();sr::captureForTest(333);
        sr::afterDispatch(old,"vkCmdDispatch");sr::expireForTest();require(!sr::active(device),"replacement window closes independently");
        require(matching("\"frames\":0")==2,"old driver-return tickets cannot enter a replacement capture window");}
    {Fixture f(p::Pass::Accumulate);f.recording.commonRecordOrder=1;f.publish();auto ticket=f.dispatch();
        require(!ticket.shadowObserved,"ordinary diagnostic mode does not enable full graph auditing");sr::afterDispatch(ticket,"vkCmdDispatch");sr::stop();
        require(!contains("sr_dispatch_shadow_frame")&&!contains("sr_dispatch_shadow_summary"),"ordinary diagnostics emit no shadow stream");}
    {Fixture f(p::Pass::Reactive,false,true,true);f.recording.commonRecordOrder=1;f.publish();const auto ticket=f.dispatch();
        require(!ticket.shadowObserved,"runtime mode cannot enable the read-only shadow even with full graph requested");sr::afterDispatch(ticket,"vkCmdDispatch");sr::stop();
        require(!contains("sr_dispatch_shadow_summary"),"product runtime remains outside the bounded shadow capture");}
}
}
int main(){try{consumerWriteRecording();consumerFiniteScope();consumerPreludeBudget();consumerAuditBounds();consumerIntegration();dispatchShadow();stage1Sources();continuousRuntime();auxiliaryJobMatching();replaySavedDispatch02();timestampExtents();accumulateDispatchSignature();signatureHistogram();positive();rawInputAssociation();depthIntervalQueries();followingPasses();rejection();bounded();windows();jsonLocale();productionDispatchDiagnostics();std::cout<<"sr observation CPU: "<<checks<<" checks passed, no GPU or hook installation\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
