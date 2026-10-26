#include "compute_observer.hpp"
#include "spirv_metadata.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <locale>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace archive_allocation_test {
thread_local std::size_t countdown=std::numeric_limits<std::size_t>::max();
unsigned failures{};
void* allocate(std::size_t bytes){
    if(countdown!=std::numeric_limits<std::size_t>::max()){
        if(!countdown){countdown=std::numeric_limits<std::size_t>::max();++failures;throw std::bad_alloc();}--countdown;
    }
    if(auto* result=std::malloc(bytes?bytes:1))return result;throw std::bad_alloc();
}
}
void* operator new(std::size_t bytes){return archive_allocation_test::allocate(bytes);}
void* operator new[](std::size_t bytes){return archive_allocation_test::allocate(bytes);}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete[](void* value) noexcept {std::free(value);}
void operator delete(void* value,std::size_t) noexcept {std::free(value);}
void operator delete[](void* value,std::size_t) noexcept {std::free(value);}

namespace {
using namespace xess_vk::compute_observer;
unsigned checks{};std::vector<std::string> lines;bool throwChunk{},reenter{},dumpMetadata{};ShaderArchive* reentryArchive{};
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class T>T h(std::uintptr_t value){return reinterpret_cast<T>(value);}
void sink(const char* line){SetLastError(1234);
    if(throwChunk&&std::strstr(line,"sr_shader_archive_chunk")){throwChunk=false;throw std::runtime_error("synthetic sink failure");}
    lines.emplace_back(line);if(reenter&&std::strstr(line,"sr_shader_archive_begin")){reenter=false;shaderArchiveSummary(reentryArchive);}
}
bool contains(const std::string& value){return std::any_of(lines.begin(),lines.end(),[&](const auto& line){return line.find(value)!=std::string::npos;});}
std::string text(const std::string& line,const char* name){const auto key='"'+std::string(name)+"\":\"";const auto at=line.find(key);require(at!=std::string::npos,"expected string field present");
    const auto end=line.find('"',at+key.size());require(end!=std::string::npos,"expected string field terminated");return line.substr(at+key.size(),end-at-key.size());}
std::uint64_t number(const std::string& line,const char* name){const auto key='"'+std::string(name)+"\":";const auto at=line.find(key);require(at!=std::string::npos,"expected numeric field present");return std::stoull(line.substr(at+key.size()));}
const std::string& event(const char* name){for(auto it=lines.rbegin();it!=lines.rend();++it)if(it->find('"'+std::string(name)+'"')!=std::string::npos)return *it;throw std::runtime_error("expected archive event missing");}
template<class Call>void preserved(Call call){SetLastError(54321);call();require(GetLastError()==54321,"archive preserves downstream LastError even when sink changes it");}
struct Fixture {
    ShaderArchive* archive{};
    explicit Fixture(bool fullGraph=false){lines.clear();throwChunk=reenter=false;reentryArchive=nullptr;preserved([&]{archive=createShaderArchive(h<VkDevice>(1),sink,fullGraph);});require(archive!=nullptr,"archive created");}
    ~Fixture(){destroyShaderArchive(archive);}
    void add(VkShaderModule module,const std::vector<std::uint32_t>& code){VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=code.size()*4;info.pCode=code.data();preserved([&]{archiveShader(archive,module,&info);});}
    void pipeline(VkPipeline pipe,std::uint64_t generation,VkShaderModule module,VkResult result=VK_SUCCESS,const VkSpecializationInfo* specialization=nullptr,VkPipelineLayout layout=VK_NULL_HANDLE){VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=module;info.stage.pName="main";info.stage.pSpecializationInfo=specialization;info.layout=layout;preserved([&]{archivePipeline(archive,pipe,generation,&info,result);});}
    void setLayout(VkDescriptorSetLayout layout,std::uint64_t generation,const VkDescriptorSetLayoutCreateInfo* info,VkResult result=VK_SUCCESS){preserved([&]{archiveSetLayout(archive,layout,generation,info,result);});}
    void pipelineLayout(VkPipelineLayout layout,std::uint64_t generation,const VkPipelineLayoutCreateInfo* info,VkResult result=VK_SUCCESS){preserved([&]{archivePipelineLayout(archive,layout,generation,info,result);});}
    void select(VkPipeline pipe,std::uint64_t generation){preserved([&]{selectArchivedShader(archive,pipe,generation);});}
    void summary(){preserved([&]{shaderArchiveSummary(archive);});}
};
// Hand-encoded no-op compute module with directly decorated sampled images.
// CPU metadata fixture only; no driver compilation, game code, or GPU execution.
std::vector<std::uint32_t> makeCode(unsigned bindings=10,unsigned set=0,bool compute=true){
    std::vector<std::uint32_t> code{0x07230203,0x00010000,0,1024,0,(2u<<16)|17,1,(3u<<16)|14,0,1,
        (5u<<16)|15,compute?5u:4u,3,0x6e69616d,0,(6u<<16)|16,3,17,1,1,1};
    for(unsigned n=0;n<bindings;++n){const std::uint32_t decoration[]{(4u<<16)|71,10+n,34,set,(4u<<16)|71,10+n,33,n};code.insert(code.end(),std::begin(decoration),std::end(decoration));}
    const std::uint32_t types[]{(2u<<16)|19,1,(3u<<16)|33,2,1,(3u<<16)|22,5,32,(9u<<16)|25,6,5,1,0,0,0,1,0,(4u<<16)|32,7,0,6};
    code.insert(code.end(),std::begin(types),std::end(types));
    for(unsigned n=0;n<bindings;++n){const std::uint32_t variable[]{(4u<<16)|59,7,10+n,0};code.insert(code.end(),std::begin(variable),std::end(variable));}
    const std::uint32_t body[]{(5u<<16)|54,1,3,0,2,(2u<<16)|248,4,(1u<<16)|253,(1u<<16)|56};code.insert(code.end(),std::begin(body),std::end(body));return code;
}
std::vector<unsigned char> bytes(const std::vector<std::uint32_t>& code){std::vector<unsigned char> result;for(const auto word:code)for(unsigned n=0;n<4;++n)result.push_back(static_cast<unsigned char>(word>>(8*n)));return result;}
std::vector<unsigned char> restored(const char* eventName="sr_shader_archive_chunk",const char* sizeField="code_bytes"){std::vector<unsigned char> result;const auto eventKey="\"event\":\""+std::string(eventName)+'"';
    for(const auto& line:lines)if(line.find(eventKey)!=std::string::npos){require(line.size()<32767,"existing session sink line bound retained");
        require(number(line,"offset_bytes")==result.size(),"chunks have contiguous byte offsets");const auto value=text(line,"hex");require(value.size()==2*number(line,sizeField)&&value.size()<=2*shaderArchiveChunkBytes,"chunk length matches its bounded declaration");
        const auto digit=[](char c){return unsigned(c>='a'?c-'a'+10:c-'0');};for(std::size_t n=0;n<value.size();n+=2)result.push_back(static_cast<unsigned char>((digit(value[n])<<4)|digit(value[n+1])));}
    return result;
}
void shaderDigestAdmission(){
    Fixture f;f.add(h<VkShaderModule>(2),makeCode());f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));
    constexpr auto expected="096654388ab5f1e9b3e2bd564458a33c52b65bd77ace4df6a3aa3be56e6626be";
    require(archivedShaderMatches(f.archive,h<VkPipeline>(3),10,expected),"retained SPIR-V matches independently computed SHA256 fixture");
    require(!archivedShaderMatches(f.archive,h<VkPipeline>(3),11,expected)&&!archivedShaderMatches(f.archive,h<VkPipeline>(4),10,expected),"SHA admission also checks exact pipeline generation and handle");
    require(!archivedShaderMatches(f.archive,h<VkPipeline>(3),10,std::string(64,'0')),"wrong semantic shader digest is refused");
    destroyArchivedShader(f.archive,h<VkShaderModule>(2));require(archivedShaderMatches(f.archive,h<VkPipeline>(3),10,expected),"native shader module destruction does not erase owned pipeline code identity");
    destroyArchivedPipeline(f.archive,h<VkPipeline>(3));require(!archivedShaderMatches(f.archive,h<VkPipeline>(3),10,expected),"destroyed pipeline cannot borrow prior shader identity");
}
void shaderImageReads(){
    Fixture f;auto code=makeCode();
    // Insert a direct NonReadable declaration before OpTypeVoid.
    auto at=std::find(code.begin()+5,code.end(),(2u<<16)|19);
    require(at!=code.end(),"fixture type section found");
    code.insert(at,{(3u<<16)|71,10,25});
    f.add(h<VkShaderModule>(2),code);f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));
    auto reads=archivedImageReads(f.archive,h<VkPipeline>(3),10);
    require(reads.known&&!reads.mayRead(0,0)&&reads.mayRead(0,1)&&!reads.mayRead(0,11),
        "owned pipeline exposes direct access without treating bound-but-undeclared images as reads");
    require(!archivedImageReads(f.archive,h<VkPipeline>(3),11).known,"pipeline generation reuse cannot borrow access facts");
    destroyArchivedShader(f.archive,h<VkShaderModule>(2));
    require(archivedImageReads(f.archive,h<VkPipeline>(3),10).known,"shader module retirement preserves owned pipeline access");
    destroyArchivedPipeline(f.archive,h<VkPipeline>(3));
    require(!archivedImageReads(f.archive,h<VkPipeline>(3),10).known,"pipeline retirement removes access authority");
}
void runtimeAccessAfterArchiveLimits(){
    for(unsigned budget=0;budget<4;++budget){
        Fixture f(true);const auto ordinary=makeCode(2);const auto shader=h<VkShaderModule>(9000);const auto pipeline=h<VkPipeline>(9001);
        if(budget==0)for(unsigned n=0;n<shaderArchiveFullGraphModuleLimit;++n){
            const auto module=h<VkShaderModule>(100+n);f.add(module,ordinary);destroyArchivedShader(f.archive,module);
        }
        if(budget==1||budget==2){
            f.add(h<VkShaderModule>(2),ordinary);
            for(unsigned n=0;n<(budget==1?shaderArchivePipelineLimit:shaderArchiveFullGraphSelectionLimit);++n){
                f.pipeline(h<VkPipeline>(100+n),100+n,h<VkShaderModule>(2));
                if(budget==2)f.select(h<VkPipeline>(100+n),100+n);
            }
        }
        auto code=makeCode(2,budget==3?1:0);
        code.insert(std::find(code.begin()+5,code.end(),(2u<<16)|19),{(3u<<16)|71,10,25});
        f.add(shader,code);f.pipeline(pipeline,77,shader);
        const auto reads=archivedImageReads(f.archive,pipeline,77);
        const auto set=budget==3?1u:0u;
        require(reads.known&&!reads.mayRead(set,0)&&reads.mayRead(set,1),
            "live write-only and readable bindings survive exhausted module/pipeline/selection budgets and diagnostic filters");
        const auto digest=archivedShaderFingerprint(f.archive,pipeline,77);
        require(digest[0]&&archivedShaderMatches(f.archive,pipeline,77,{digest.data(),digest.size()}),
            "functional identity does not depend on raw-code archive capacity");
        destroyArchivedShader(f.archive,shader);
        require(archivedImageReads(f.archive,pipeline,77).known,"pipeline owns access facts after shader destruction");
        require(!archivedImageReads(f.archive,pipeline,78).known,"stale generation still refuses after capture exhaustion");
        destroyArchivedPipeline(f.archive,pipeline);
        require(!archivedImageReads(f.archive,pipeline,77).known,"destroyed pipeline loses access authority");
        f.pipeline(pipeline,78,shader);
        require(!archivedImageReads(f.archive,pipeline,78).known,"destroyed shader cannot grant facts to a reused pipeline");
        auto readable=makeCode(2,set);f.add(shader,readable);f.pipeline(pipeline,79,shader);
        require(archivedImageReads(f.archive,pipeline,79).mayRead(set,0),"new readable shader never inherits an old write-only exemption");
    }
}
void faultSelectionSink(){
    Fixture f;f.add(h<VkShaderModule>(2),makeCode());f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));
    static std::vector<std::string> retained;retained.clear();lines.clear();
    const auto fault=+[](const char* line){retained.emplace_back(line);SetLastError(1111);};
    preserved([&]{selectArchivedShader(f.archive,h<VkPipeline>(3),10,fault);});
    require(lines.empty()&&retained.size()>3,"fault selection uses the independent sink for all code and metadata chunks");
    lines=retained;require(restored()==bytes(makeCode()),"independent fault sink retains the exact shader bytes");
    const auto count=retained.size();preserved([&]{selectArchivedShader(f.archive,h<VkPipeline>(3),10,fault);});
    require(retained.size()==count,"independent sink does not bypass per-pipeline selection limits");
    f.pipeline(h<VkPipeline>(4),11,h<VkShaderModule>(2));
    preserved([&]{selectArchivedShader(f.archive,h<VkPipeline>(4),12,fault);});
    require(retained.back().find("pipeline_generation_mismatch")!=std::string::npos,"fault sink keeps refusal metadata without borrowing a stale pipeline");
}
void checkCorrelation(const std::vector<unsigned char>& original){std::uint64_t fnv=14695981039346656037ull;for(auto byte:original){fnv^=byte;fnv*=1099511628211ull;}
    require(std::stoull(text(event("sr_shader_archive_begin"),"fnv1a64_correlation_only"),nullptr,16)==fnv,"correlation hash covers original complete code bytes");}

void ownedPipelineMetadata(){
    Fixture f;f.add(h<VkShaderModule>(2),makeCode());const auto set=h<VkDescriptorSetLayout>(30);const auto layout=h<VkPipelineLayout>(31);
    std::array<VkSampler,2> samplers{h<VkSampler>(71),h<VkSampler>(72)};
    std::array<VkDescriptorSetLayoutBinding,2> bindings{{{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {7,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,samplers.data()}}};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};setInfo.bindingCount=bindings.size();setInfo.pBindings=bindings.data();f.setLayout(set,111,&setInfo);
    std::array<VkPushConstantRange,2> ranges{{{VK_SHADER_STAGE_COMPUTE_BIT,0,64},{VK_SHADER_STAGE_FRAGMENT_BIT,64,16}}};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&set;layoutInfo.pushConstantRangeCount=ranges.size();layoutInfo.pPushConstantRanges=ranges.data();f.pipelineLayout(layout,222,&layoutInfo);
    std::array<unsigned char,16> data{};for(unsigned n=0;n<data.size();++n)data[n]=static_cast<unsigned char>(0xA0+n);const std::vector<unsigned char> original(data.begin(),data.end());
    std::array<VkSpecializationMapEntry,2> entries{{{3,0,4},{11,8,8}}};VkSpecializationInfo specialization{std::uint32_t(entries.size()),entries.data(),data.size(),data.data()};
    f.pipeline(h<VkPipeline>(3),9001,h<VkShaderModule>(2),VK_SUCCESS,&specialization,layout);
    bindings[0].binding=99;samplers[0]=VK_NULL_HANDLE;ranges[0].size=4;entries[0].constantID=99;data.fill(0xFF);
    preserved([&]{destroyArchivedSetLayout(f.archive,set);destroyArchivedPipelineLayout(f.archive,layout);destroyArchivedShader(f.archive,h<VkShaderModule>(2));});
    f.setLayout(set,112,&setInfo);f.pipelineLayout(layout,223,&layoutInfo);f.select(h<VkPipeline>(3),9001);
    require(contains("\"pipeline_layout_generation\":222")&&contains("\"layout_generation\":111")&&!contains("\"layout_generation\":112"),"pipeline owns exact original layout generations after destroy and reuse");
    require(contains("\"stage_flags\":32,\"offset\":0,\"size\":64")&&contains("\"binding\":0,\"descriptor_type\":3,\"descriptor_count\":1")&&contains("\"immutable_samplers\":[\"0x47\",\"0x48\"]"),"push ranges, set layout bindings and immutable sampler handles are owned copies");
    require(contains("\"constant_id\":3,\"offset\":0,\"size\":4")&&contains("\"constant_id\":11,\"offset\":8,\"size\":8")&&restored("sr_shader_archive_specialization_chunk","data_bytes")==original,"specialization map entries and all data bytes survive caller mutation");
    require(contains("\"pipeline_metadata_complete\":true")&&contains("\"specialization_data_captured\":true")&&contains("\"shader_semantics_verified\":false"),"complete bounded core metadata remains separate from semantic and Execute proof");
    if(dumpMetadata)for(const auto& line:lines)std::cout<<line<<'\n';
}
void specializationRefusals(){
    for(unsigned mode=0;mode<8;++mode){Fixture f;f.add(h<VkShaderModule>(2),makeCode());std::array<unsigned char,8> data{};std::array<VkSpecializationMapEntry,2> entries{{{1,0,4},{2,4,4}}};
        VkSpecializationInfo info{1,entries.data(),data.size(),data.data()};
        if(mode==0){info={};}
        if(mode==1){info.mapEntryCount=shaderArchiveSpecializationMaps+1;info.pMapEntries=reinterpret_cast<const VkSpecializationMapEntry*>(1);}
        if(mode==2){info.dataSize=shaderArchiveSpecializationDataBytes+1;info.pData=reinterpret_cast<const void*>(1);}
        if(mode==3)info.pMapEntries=nullptr;
        if(mode==4)info.pData=nullptr;
        if(mode==5)entries[0].offset=UINT32_MAX;
        if(mode==6)entries[0].size=SIZE_MAX;
        if(mode==7){info.mapEntryCount=2;entries[1].constantID=1;}
        f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2),VK_SUCCESS,&info);f.select(h<VkPipeline>(3),10);f.summary();
        require(number(event("sr_shader_archive_summary"),"specialization_refused")==unsigned(mode!=0),"specialization input limits and invalid mappings have explicit refusal counts");
        require(contains(mode?"\"specialization_metadata_captured\":false":"\"specialization_metadata_captured\":true")&&!restored().empty(),"incomplete specialization never masquerades as complete and does not discard raw shader bytes");}
    {Fixture f;f.add(h<VkShaderModule>(2),makeCode());std::vector<unsigned char> data(shaderArchiveSpecializationDataBytes,0x5A);VkSpecializationInfo info{0,nullptr,data.size(),data.data()};
        for(unsigned n=0;n<17;++n)f.pipeline(h<VkPipeline>(100+n),100+n,h<VkShaderModule>(2),VK_SUCCESS,&info);
        f.select(h<VkPipeline>(116),116);f.summary();require(contains("specialization_total_budget")&&number(event("sr_shader_archive_summary"),"specialization_bytes_retained_cumulative")==shaderArchiveSpecializationTotalBytes,"specialization owned data has a cumulative 1 MiB admission limit");}
}
void layoutRefusals(){
    constexpr const char* reasons[]{"binding_limit","missing_bindings","immutable_sampler_limit","set_limit","push_range_limit","invalid_push_range","extensions_not_captured","set_layout_not_recorded"};
    for(unsigned mode=0;mode<8;++mode){Fixture f;f.add(h<VkShaderModule>(2),makeCode());const auto set=h<VkDescriptorSetLayout>(30);const auto layout=h<VkPipelineLayout>(31);
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};setInfo.bindingCount=1;setInfo.pBindings=&binding;
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,4};VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&set;layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&range;
        if(mode==0){setInfo.bindingCount=shaderArchiveLayoutBindings+1;setInfo.pBindings=reinterpret_cast<const VkDescriptorSetLayoutBinding*>(1);}
        if(mode==1)setInfo.pBindings=nullptr;
        if(mode==2){binding.descriptorCount=shaderArchiveImmutableSamplers+1;binding.pImmutableSamplers=reinterpret_cast<const VkSampler*>(1);}
        if(mode==3){layoutInfo.setLayoutCount=shaderArchiveLayoutSets+1;layoutInfo.pSetLayouts=reinterpret_cast<const VkDescriptorSetLayout*>(1);}
        if(mode==4){layoutInfo.pushConstantRangeCount=shaderArchivePushRanges+1;layoutInfo.pPushConstantRanges=reinterpret_cast<const VkPushConstantRange*>(1);}
        if(mode==5)range.offset=1;
        if(mode==6)setInfo.pNext=reinterpret_cast<const void*>(1);
        if(mode!=7)f.setLayout(set,111,&setInfo);f.pipelineLayout(layout,222,&layoutInfo);f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2),VK_SUCCESS,nullptr,layout);f.select(h<VkPipeline>(3),10);
        require(contains(reasons[mode])&&contains("\"pipeline_layout_metadata_complete\":false")&&!restored().empty(),"missing, unsupported or oversized layout metadata is explicit and never invented");}
    {Fixture f;VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        for(unsigned n=0;n<=shaderArchiveSetLayouts;++n)f.setLayout(h<VkDescriptorSetLayout>(10000+n),n+1,&info);f.summary();
        require(number(event("sr_shader_archive_summary"),"layout_capacity_refused")==1,"live set-layout snapshot capacity is fixed");}
    {Fixture f;VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        for(unsigned n=0;n<=shaderArchivePipelineLayouts;++n)f.pipelineLayout(h<VkPipelineLayout>(10000+n),n+1,&info);f.summary();
        require(number(event("sr_shader_archive_summary"),"layout_capacity_refused")==1,"live pipeline-layout snapshot capacity is fixed");}
    {Fixture f;std::array<VkDescriptorSetLayoutBinding,shaderArchiveLayoutBindings> bindings{};for(unsigned n=0;n<bindings.size();++n)bindings[n]={n,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};info.bindingCount=bindings.size();info.pBindings=bindings.data();
        for(unsigned n=0;n<2048;++n){const auto handle=h<VkDescriptorSetLayout>(10000+n);f.setLayout(handle,n+1,&info);destroyArchivedSetLayout(f.archive,handle);}f.summary();
        require(number(event("sr_shader_archive_summary"),"layout_bytes_retained_cumulative")<=shaderArchiveLayoutTotalBytes&&number(event("sr_shader_archive_summary"),"layout_budget_refused")>0,"retired layout snapshots cannot reopen the cumulative metadata admission budget");}
}

void retryAfterLookupRefusal(){
    Fixture f;f.add(h<VkShaderModule>(2),makeCode(3));f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));f.select(h<VkPipeline>(3),10);
    require(contains("pipeline_not_recorded")&&contains("\"creation_tracking_stopped_after_selection\":false"),"missing RCAS-style archive lookup must leave later creation and selection enabled");
    const auto refusedLines=lines.size();for(unsigned n=0;n<1000;++n)selectArchivedShader(f.archive,h<VkPipeline>(3),10);
    require(lines.size()==refusedLines,"repeated unavailable lookups emit only one bounded refusal and summary");
    const auto code=makeCode();f.add(h<VkShaderModule>(4),code);f.pipeline(h<VkPipeline>(5),20,h<VkShaderModule>(4));f.select(h<VkPipeline>(5),20);
    require(restored()==bytes(code)&&number(event("sr_shader_archive_begin"),"pipeline_generation")==20,"later retained Accumulate code succeeds after failed RCAS-style lookup");
    const auto selectedLines=lines.size();for(unsigned n=0;n<1000;++n)selectArchivedShader(f.archive,h<VkPipeline>(5),20);
    require(lines.size()==selectedLines,"an already selected exact pipeline never repeats output");
}
void multipleSelections(){
    {Fixture prepare;const auto code=makeCode(8);prepare.add(h<VkShaderModule>(2),code);
        prepare.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));prepare.select(h<VkPipeline>(3),10);
        require(restored()==bytes(code),"fixed dense eight-binding Prepare module is retained for MV semantic inspection");}
    Fixture f;const auto code=makeCode();f.add(h<VkShaderModule>(2),code);
    for(unsigned n=0;n<5;++n)f.pipeline(h<VkPipeline>(3+n),10+n,h<VkShaderModule>(2));
    for(unsigned n=0;n<4;++n){f.select(h<VkPipeline>(3+n),10+n);const auto count=lines.size();
        f.select(h<VkPipeline>(3+n),10+n);require(lines.size()==count,"each exact variant is emitted once");}
    const auto count=lines.size();f.select(h<VkPipeline>(7),14);require(lines.size()==count,"fifth distinct variant cannot exceed archive selection capacity");
    f.summary();const auto& summary=event("sr_shader_archive_summary");
    require(number(summary,"selected_pipelines")==4&&number(summary,"selection_limit")==4&&
        summary.find("\"creation_tracking_stopped_after_selection\":true")!=std::string::npos,"four variants close creation tracking");
    require(std::count_if(lines.begin(),lines.end(),[](const auto& line){return line.find("\"event\":\"sr_shader_archive_begin\"")!=std::string::npos;})==4,
        "four different pipeline identities produce four archives");
    const auto inspected=number(summary,"shader_bytes_inspected");f.add(h<VkShaderModule>(99),code);f.summary();
    require(number(event("sr_shader_archive_summary"),"shader_bytes_inspected")>inspected,"live access parsing continues after diagnostic selection stops");
}
void lifecycle(){
    {Fixture f;auto code=makeCode();const auto original=bytes(code);f.add(h<VkShaderModule>(2),code);f.pipeline(h<VkPipeline>(3),9001,h<VkShaderModule>(2),VK_ERROR_OUT_OF_DEVICE_MEMORY);
        preserved([&]{destroyArchivedShader(f.archive,h<VkShaderModule>(2));});std::fill(code.begin(),code.end(),0);
        reentryArchive=f.archive;reenter=true;f.select(h<VkPipeline>(3),9001);require(!reenter,"sink may query a summary outside archive lock");require(restored()==original,"pipeline retains owned module code after module destruction and caller mutation");
        require(number(event("sr_shader_archive_begin"),"pipeline_generation")==9001&&number(event("sr_shader_archive_begin"),"shader_generation")==1,"descriptor pipeline generation is not confused with archive module generation");
        require(contains("\"batch_result\":-2")&&contains("\"all_chunk_sink_calls_returned\":true"),"nonnull partial pipeline success remains capturable");checkCorrelation(original);
        require(contains("\"file_persistence_verified\":false")&&contains("\"shader_semantics_verified\":false")&&contains("\"execute_permitted\":false"),"code capture grants no persistence, semantic or Execute proof");
        const auto inspected=number(event("sr_shader_archive_summary"),"shader_bytes_inspected");f.add(h<VkShaderModule>(20),makeCode());f.pipeline(h<VkPipeline>(30),9002,h<VkShaderModule>(20));f.summary();
        require(number(event("sr_shader_archive_summary"),"shader_bytes_inspected")>inspected&&
            number(event("sr_shader_archive_summary"),"selected_pipelines")==1&&contains("\"creation_tracking_stopped_after_selection\":false"),
            "the first selection retains bounded creation tracking for later pipeline variants");
        const auto count=lines.size();for(unsigned n=0;n<100000;++n)selectArchivedShader(f.archive,h<VkPipeline>(3),9001);require(lines.size()==count,"repeated selected pipeline calls never repeat code output");}
    for(const bool selectOld:{true,false}){Fixture f;auto old=makeCode(),fresh=old;fresh.push_back(1u<<16);f.add(h<VkShaderModule>(2),old);f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));
        preserved([&]{destroyArchivedShader(f.archive,h<VkShaderModule>(2));});f.add(h<VkShaderModule>(2),fresh);f.pipeline(h<VkPipeline>(4),11,h<VkShaderModule>(2));
        f.select(h<VkPipeline>(selectOld?3:4),selectOld?10:11);require(restored()==bytes(selectOld?old:fresh),"reused module handle preserves old pipeline code and binds new pipeline to new generation");
        require(number(event("sr_shader_archive_begin"),"shader_generation")==unsigned(selectOld?1:2),"pipeline identifies exact owned module generation");}
    {Fixture f;f.add(h<VkShaderModule>(2),makeCode());f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));f.pipeline(h<VkPipeline>(3),11,h<VkShaderModule>(2));f.select(h<VkPipeline>(3),10);
        require(contains("pipeline_generation_mismatch")&&!contains("sr_shader_archive_chunk"),"reused pipeline handle cannot satisfy stale selected generation");f.select(h<VkPipeline>(3),11);require(restored()==bytes(makeCode()),"correct generation can retry after a stale selected key refusal");}
    {Fixture f;f.add(h<VkShaderModule>(2),makeCode());f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));preserved([&]{destroyArchivedPipeline(f.archive,h<VkPipeline>(3));});
        f.add(h<VkShaderModule>(2),makeCode(9));f.pipeline(h<VkPipeline>(3),11,h<VkShaderModule>(2));f.select(h<VkPipeline>(3),11);
        require(contains("pipeline_not_recorded")&&!contains("sr_shader_archive_chunk"),"destroyed or unretained module cannot be resurrected through pipeline handle reuse");}
}
void fullGraphDiagnostics(){
    {Fixture f;f.add(h<VkShaderModule>(2),makeCode(2));f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));f.select(h<VkPipeline>(3),10);f.summary();
        require(!contains("sr_shader_archive_begin")&&number(event("sr_shader_archive_summary"),"selection_limit")==4&&
            number(event("sr_shader_archive_summary"),"module_limit")==32,"ordinary SR retains its small-shader filter and original archive budgets");}
    {Fixture f(true);const auto code=makeCode(2);f.add(h<VkShaderModule>(2),code);f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));f.select(h<VkPipeline>(3),10);
        require(restored()==bytes(code)&&contains("\"full_graph_observation\":true"),"full-graph captures a complete small native job without granting semantics");
        require(number(event("sr_shader_archive_begin"),"selection_limit")==16&&number(event("sr_shader_archive_begin"),"module_limit")==128,
            "full-graph evidence declares its separate bounded capacities");}
    {Fixture f(true);f.add(h<VkShaderModule>(2),makeCode(2));
        for(unsigned n=0;n<shaderArchiveFullGraphSelectionLimit;++n){f.pipeline(h<VkPipeline>(100+n),100+n,h<VkShaderModule>(2));f.select(h<VkPipeline>(100+n),100+n);}
        const auto begins=[&]{return std::count_if(lines.begin(),lines.end(),[](const auto& line){return line.find("\"event\":\"sr_shader_archive_begin\"")!=std::string::npos;});};
        require(begins()==16,"native graph retains more than the original four shader selections");
        f.pipeline(h<VkPipeline>(999),999,h<VkShaderModule>(2));f.select(h<VkPipeline>(999),999);f.summary();
        require(begins()==16&&number(event("sr_shader_archive_summary"),"selected_pipelines")==16&&contains("\"creation_tracking_stopped_after_selection\":true"),
            "full-graph selection capacity remains exclusive and stops creation tracking");}
    {Fixture f(true);const auto code=makeCode(2);
        for(unsigned n=0;n<shaderArchiveFullGraphModuleLimit;++n)f.add(h<VkShaderModule>(2+n),code);
        f.add(h<VkShaderModule>(1000),code);f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(1000));f.select(h<VkPipeline>(3),10);
        require(!contains("sr_shader_archive_begin"),"absent code cannot borrow a diagnostic slot beyond the old 32-slot sentinel");
        f.pipeline(h<VkPipeline>(4),11,h<VkShaderModule>(34));f.select(h<VkPipeline>(4),11);
        require(number(event("sr_shader_archive_begin"),"shader_generation")==33,"first code beyond ordinary module capacity is selectable by exact ownership");
        f.pipeline(h<VkPipeline>(5),12,h<VkShaderModule>(129));f.select(h<VkPipeline>(5),12);f.summary();
        require(number(event("sr_shader_archive_begin"),"shader_generation")==128&&number(event("sr_shader_archive_summary"),"module_limit_refused")==1,
            "last diagnostic module is retained and the next is explicitly refused");}
    {Fixture f(true);f.add(h<VkShaderModule>(2),makeCode(2,1));f.add(h<VkShaderModule>(3),makeCode(2,0,false));f.summary();
        require(number(event("sr_shader_archive_summary"),"retained_modules")==0,"full-graph still excludes non-set0 and non-compute shader modules");}
}
void qualification(){
    fullGraphDiagnostics();
    for(unsigned mode=0;mode<5;++mode){Fixture f;auto code=makeCode(mode==0?9:10,mode==1?1:0,mode!=2);
        if(mode==3)for(std::size_t n=0;n<10;++n)code[21+8*n+7]=0; // All IDs alias the same explicit binding.
        if(mode==4)code[21+6]=35; // Remove one explicit Binding decoration.
        require(xess_vk::spirv::parse(std::span<const std::uint32_t>(code)).valid,"qualification fixture remains valid bounded metadata");
        f.add(h<VkShaderModule>(2),code);f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));f.select(h<VkPipeline>(3),10);f.summary();
        require(!contains("sr_shader_archive_chunk")&&number(event("sr_shader_archive_summary"),"retained_modules")==0,"non-compute, non-set0 or fewer than ten distinct explicit bindings are excluded");}
    {Fixture f;auto ignored=makeCode(9);for(unsigned n=0;n<1000;++n){f.add(h<VkShaderModule>(2),ignored);f.pipeline(h<VkPipeline>(100+n),100+n,h<VkShaderModule>(2));}
        f.add(h<VkShaderModule>(2),makeCode());f.pipeline(h<VkPipeline>(3),9999,h<VkShaderModule>(2));f.select(h<VkPipeline>(3),9999);require(!restored().empty(),"ordinary noncandidate pipelines cannot consume archive pipeline slots");}
    {Fixture f;VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        preserved([&]{archiveShader(f.archive,h<VkShaderModule>(2),nullptr);});preserved([&]{archiveShader(f.archive,h<VkShaderModule>(2),&info);});
        info.pCode=reinterpret_cast<const std::uint32_t*>(1);info.codeSize=20;preserved([&]{archiveShader(f.archive,h<VkShaderModule>(2),&info);});
        info.pCode=reinterpret_cast<const std::uint32_t*>(4096);info.codeSize=shaderArchiveModuleBytes+4;preserved([&]{archiveShader(f.archive,h<VkShaderModule>(2),&info);});
        auto code=makeCode();info.pCode=code.data();info.codeSize=19;archiveShader(f.archive,h<VkShaderModule>(2),&info);info.codeSize=21;archiveShader(f.archive,h<VkShaderModule>(2),&info);
        code[0]=0;info.codeSize=code.size()*4;archiveShader(f.archive,h<VkShaderModule>(2),&info);f.summary();
        require(number(event("sr_shader_archive_summary"),"input_refused")==5&&number(event("sr_shader_archive_summary"),"module_size_refused")==1&&number(event("sr_shader_archive_summary"),"parse_refused")==1,"invalid pointers, sizes and malformed metadata have explicit bounded refusal counts");}
}
void bounds(){
    {Fixture f;const auto code=makeCode();for(std::size_t n=0;n<shaderArchiveModuleLimit;++n){f.add(h<VkShaderModule>(2+n),code);preserved([&]{destroyArchivedShader(f.archive,h<VkShaderModule>(2+n));});}
        f.add(h<VkShaderModule>(99),code);f.summary();require(number(event("sr_shader_archive_summary"),"retained_modules")==32&&number(event("sr_shader_archive_summary"),"module_limit_refused")==1,"destroying modules does not reopen cumulative module budget");}
    {Fixture f;auto code=makeCode();code.resize(shaderArchiveModuleBytes/4,1u<<16);std::vector<unsigned char> data(shaderArchiveSpecializationDataBytes,0x39);VkSpecializationInfo specialization{0,nullptr,data.size(),data.data()};
        for(unsigned n=0;n<16;++n){f.add(h<VkShaderModule>(2+n),code);if(!n)f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2),VK_SUCCESS,&specialization);destroyArchivedShader(f.archive,h<VkShaderModule>(2+n));}
        f.add(h<VkShaderModule>(99),code);f.select(h<VkPipeline>(3),10);require(restored()==bytes(code),"maximum-size owned code survives cumulative budget exhaustion");f.summary();
        const auto& summary=event("sr_shader_archive_summary");require(number(summary,"retained_bytes")==shaderArchiveTotalBytes&&number(summary,"byte_limit_refused")==1,"cumulative retained bytes never exceed 16 MiB");
        require(number(summary,"shader_bytes_inspected")==shaderArchiveTotalBytes+code.size()*4,"live access parsing continues beyond the raw-code retention budget");
        require(number(summary,"output_chunks")==128&&number(summary,"output_bytes_attempted")<=shaderArchiveOutputBytes,"one selected 1 MiB module has bounded 128-chunk and 3 MiB total output");
        require(number(summary,"specialization_output_chunks")==8&&restored("sr_shader_archive_specialization_chunk","data_bytes")==data,"maximum specialization data is owned and emitted in eight bounded chunks beside maximum shader code");
        std::size_t emitted{};for(const auto& line:lines)emitted+=line.size()+1;
        require(emitted<=shaderArchiveOutputBytes&&number(summary,"archive_emission_bytes_attempted")==emitted-summary.size()-1,"start, chunks, end and summaries share the total emission budget including newlines");}
    {Fixture f;f.add(h<VkShaderModule>(2),makeCode());for(std::size_t n=0;n<shaderArchivePipelineLimit+1;++n)f.pipeline(h<VkPipeline>(100+n),100+n,h<VkShaderModule>(2));
        f.summary();require(number(event("sr_shader_archive_summary"),"pipeline_limit_refused")==1,"pipeline metadata has a fixed capacity and explicit overflow");
        destroyArchivedPipeline(f.archive,h<VkPipeline>(100));f.pipeline(h<VkPipeline>(999),999,h<VkShaderModule>(2));f.select(h<VkPipeline>(999),999);require(!restored().empty(),"retired pipeline slot can serve a new exact generation");}
}
void failures(){
    {Fixture f;f.add(h<VkShaderModule>(2),makeCode());f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2));throwChunk=true;f.select(h<VkPipeline>(3),10);f.summary();
        require(contains("\"all_chunk_sink_calls_returned\":false")&&number(event("sr_shader_archive_summary"),"selection_refused")==1&&number(event("sr_shader_archive_summary"),"sink_failures")==1,"sink failure is reported as incomplete and cannot escape the hook");}
    {struct Grouped:std::numpunct<char>{char do_thousands_sep()const override{return ',';}std::string do_grouping()const override{return "\3";}};
        const auto prior=std::locale();std::locale::global(std::locale(prior,new Grouped));try {Fixture f;f.add(h<VkShaderModule>(2),makeCode());f.pipeline(h<VkPipeline>(3),1234567,h<VkShaderModule>(2));f.select(h<VkPipeline>(3),1234567);
            require(contains("\"pipeline_generation\":1234567")&&!contains("\"pipeline_generation\":1,234,567"),"archive JSON uses classic locale");}catch(...){std::locale::global(prior);throw;}std::locale::global(prior);}
    preserved([]{destroyShaderArchive(nullptr);shaderArchiveSummary(nullptr);archiveShader(nullptr,VK_NULL_HANDLE,nullptr);archivePipeline(nullptr,VK_NULL_HANDLE,0,nullptr,VK_SUCCESS);selectArchivedShader(nullptr,VK_NULL_HANDLE,0);
        archiveSetLayout(nullptr,VK_NULL_HANDLE,0,nullptr,VK_SUCCESS);destroyArchivedSetLayout(nullptr,VK_NULL_HANDLE);archivePipelineLayout(nullptr,VK_NULL_HANDLE,0,nullptr,VK_SUCCESS);destroyArchivedPipelineLayout(nullptr,VK_NULL_HANDLE);});
}
void specializationAllocationFailures(){
    for(std::size_t allocation=0;allocation<2;++allocation){Fixture f;f.add(h<VkShaderModule>(2),makeCode());
        const auto layout=h<VkPipelineLayout>(31);VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};f.pipelineLayout(layout,222,&li);
        const std::array<VkSpecializationMapEntry,1> entries{{{7,0,4}}};const std::vector<unsigned char> data(64,0xA5);
        const VkSpecializationInfo info{std::uint32_t(entries.size()),entries.data(),data.size(),data.data()};
        const auto prior=archive_allocation_test::failures;archive_allocation_test::countdown=allocation;
        f.pipeline(h<VkPipeline>(3),10,h<VkShaderModule>(2),VK_SUCCESS,&info,layout);
        archive_allocation_test::countdown=std::numeric_limits<std::size_t>::max();
        require(archive_allocation_test::failures==prior+1,"one real entries/data vector allocation was forced to fail");
        f.select(h<VkPipeline>(3),10);f.summary();
        require(!contains("\"event\":\"sr_shader_archive_begin\"")&&contains("pipeline_not_recorded")&&!contains("\"pipeline_metadata_complete\":true"),
            "failed specialization allocation leaves no selectable partial pipeline or falsely complete metadata");
        require(number(event("sr_shader_archive_summary"),"specialization_bytes_retained_cumulative")==0&&
            number(event("sr_shader_archive_summary"),"sink_failures")==1,"failed copy neither consumes owned specialization budget nor hides the archive failure");
        f.pipeline(h<VkPipeline>(3),11,h<VkShaderModule>(2),VK_SUCCESS,&info,layout);f.select(h<VkPipeline>(3),11);
        require(contains("\"pipeline_entry_point\":\"main\"")&&contains("\"specialization_present\":true")&&
            contains("\"pipeline_metadata_complete\":true")&&restored("sr_shader_archive_specialization_chunk","data_bytes")==data,
            "a later exact pipeline publishes complete owned specialization and entry point after allocation failure");
    }
}
}
int main(int argc,char** argv){try{require(argc==1||(argc==2&&std::strcmp(argv[1],"--dump-metadata")==0),"optional CPU metadata dump flag only");dumpMetadata=argc==2;shaderImageReads();runtimeAccessAfterArchiveLimits();shaderDigestAdmission();faultSelectionSink();specializationAllocationFailures();ownedPipelineMetadata();specializationRefusals();layoutRefusals();retryAfterLookupRefusal();multipleSelections();lifecycle();qualification();bounds();failures();std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"module_limit\":32,\"byte_limit\":16777216,\"max_output_chunks\":128,\"dispatch_trace_started\":false,\"driver_calls\":0,\"game_touched\":false,\"shader_semantics_verified\":false,\"execute_permitted\":false}\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
