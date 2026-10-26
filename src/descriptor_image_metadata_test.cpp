#include "descriptor_image_metadata.hpp"
#include "metadata_storage.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace { std::atomic<bool> measure{},failNextAllocation{};std::atomic<std::size_t> allocations{},allocationBytes{}; }
void* operator new(std::size_t size){if(failNextAllocation.exchange(false))throw std::bad_alloc();if(measure){++allocations;allocationBytes+=size;}if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}

namespace {
using namespace xess_vk::descriptor_image_metadata;
unsigned checks{},cases{};std::uint64_t benchmarkNs{},benchmarkChecksum{};std::size_t heapAllocations{},heapBytes{};
void require(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
void ok(Result result){require(result.accepted(),"metadata result unexpectedly refused");}
void issue(Result result,Issue expected){require(!result.accepted()&&(result.issues&bit(expected)),"expected exact refusal bit");}
void refused(const DispatchEvidence& e,Issue expected){
    if(e.metadataAccepted||e.count||!(e.issues&bit(expected)))std::cerr<<"refusal mismatch case="<<cases<<" expected="<<bit(expected)<<" actual="<<e.issues<<" count="<<e.count<<'\n';
    require(!e.metadataAccepted&&!e.count&&(e.issues&bit(expected)),"refused evidence must publish zero image bindings");
    require(!e.completeResourceSet&&!e.sameFrameVerified&&!e.layoutVerified&&!e.gpuComplete&&!e.resourceLifetimePinned&&!e.copyPermitted&&!e.shaderSemanticsVerified,
        "refused metadata grants no resource, frame, layout, pixel or execution proof");
}
void qualified(const DispatchEvidence& e,std::size_t count=3){
    require(e.metadataAccepted&&!e.issues&&e.count==count,"accepted evidence contains all exact image bindings");
    require(!e.completeResourceSet&&!e.sameFrameVerified&&!e.layoutVerified&&!e.gpuComplete&&!e.resourceLifetimePinned&&!e.copyPermitted&&!e.shaderSemanticsVerified,
        "accepted image metadata still grants no resource, frame, layout, pixel or execution proof");
}
template<class T>T h(std::uint64_t n){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(n);else return static_cast<T>(n);}
Config small(){Config c;c.images=16;c.views=32;c.setLayouts=16;c.pipelineLayouts=16;c.pools=8;c.sets=16;c.pipelines=16;c.commandPools=8;c.commands=16;c.templates=16;return c;}
struct Fixture {
    Tracker t;
    VkDescriptorSetLayout setLayout=h<VkDescriptorSetLayout>(2001);
    VkPipelineLayout pipelineLayout=h<VkPipelineLayout>(3001);
    VkDescriptorPool pool=h<VkDescriptorPool>(4001);
    std::array<VkDescriptorSet,2> sets{h<VkDescriptorSet>(5001),h<VkDescriptorSet>(5002)};
    VkCommandPool commandPool=h<VkCommandPool>(6001);
    std::array<VkCommandBuffer,2> commands{h<VkCommandBuffer>(7001),h<VkCommandBuffer>(7002)};
    VkPipeline pipeline=h<VkPipeline>(8001);
    std::array<VkImageView,2> views{h<VkImageView>(1001),h<VkImageView>(1002)};
    std::array<xess_vk::image_inventory::Record,2> images{};
    VkDescriptorSetLayoutBinding bindings[4]{{2,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {5,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {9,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {10,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    Fixture(Config config=small(),bool initialize=true):t(config){if(initialize)objects();}
    VkImageViewCreateInfo viewInfo(std::size_t index)const{
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};info.image=h<VkImage>(images[index].image);
        info.viewType=VK_IMAGE_VIEW_TYPE_2D;info.format=static_cast<VkFormat>(images[index].description.format);
        info.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};return info;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo()const {VkDescriptorSetLayoutCreateInfo i{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};i.bindingCount=4;i.pBindings=bindings;return i;}
    VkPipelineLayoutCreateInfo pipelineLayoutInfo()const {VkPipelineLayoutCreateInfo i{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};i.setLayoutCount=1;i.pSetLayouts=&setLayout;return i;}
    VkDescriptorPoolCreateInfo poolInfo()const {VkDescriptorPoolCreateInfo i{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};i.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;i.maxSets=8;return i;}
    VkComputePipelineCreateInfo pipelineInfo()const {
        VkComputePipelineCreateInfo i{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};i.layout=pipelineLayout;
        i.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;i.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;i.stage.module=h<VkShaderModule>(99);i.stage.pName="main";return i;
    }
    void allocate(){const VkDescriptorSetLayout layouts[]{setLayout,setLayout};VkDescriptorSetAllocateInfo i{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        i.descriptorPool=pool;i.descriptorSetCount=2;i.pSetLayouts=layouts;ok(t.setsAllocated(&i,sets.data(),VK_SUCCESS));}
    void objects(){
        for(std::size_t i=0;i<images.size();++i){images[i]={101+i,1+i,{static_cast<std::uint32_t>(VK_FORMAT_R16G16_SFLOAT),64,32,1,1,1,
            VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT,0,VK_IMAGE_TILING_OPTIMAL,VK_SHARING_MODE_EXCLUSIVE,VK_IMAGE_LAYOUT_UNDEFINED}};
            ok(t.imageObserved(images[i]));const auto info=viewInfo(i);ok(t.viewCreated(views[i],&info,VK_SUCCESS));}
        const auto l=layoutInfo();ok(t.setLayoutCreated(setLayout,&l,VK_SUCCESS));
        const auto pl=pipelineLayoutInfo();ok(t.pipelineLayoutCreated(pipelineLayout,&pl,VK_SUCCESS));
        const auto p=poolInfo();ok(t.poolCreated(pool,&p,VK_SUCCESS));allocate();
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=3;
        ok(t.commandPoolCreated(commandPool,&cp,VK_SUCCESS));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=commandPool;ca.commandBufferCount=2;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ok(t.commandsAllocated(&ca,commands.data(),VK_SUCCESS));
        const auto pi=pipelineInfo();ok(t.pipelineCreated(pipeline,&pi,VK_SUCCESS));
    }
    Result write(VkDescriptorSet set,VkImageView first=VK_NULL_HANDLE){
        VkDescriptorImageInfo values[3]{{h<VkSampler>(0xffff),first?first:views[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {h<VkSampler>(0xffff),views[1],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{h<VkSampler>(0xffff),views[1],VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2]{};for(auto& w:writes){w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;}
        writes[0].dstBinding=2;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;writes[0].descriptorCount=2;writes[0].pImageInfo=values;
        writes[1].dstBinding=5;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[1].descriptorCount=1;writes[1].pImageInfo=values+2;
        return t.updateDescriptors(2,writes,0,nullptr);
    }
    void begin(std::size_t command=0){VkCommandBufferBeginInfo i{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ok(t.begin(commands[command],&i,VK_SUCCESS));}
    void bind(std::size_t command=0,std::size_t set=0){t.bindSets(commands[command],VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&sets[set],0,nullptr);t.bindPipeline(commands[command],VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);}
    DispatchEvidence dispatch(std::size_t command=0){return t.preDispatch(commands[command],8,4,1);}
    DispatchEvidence ready(){ok(write(sets[0]));begin();bind();return dispatch();}
    VkCopyDescriptorSet copy(VkDescriptorSet src,VkDescriptorSet dst,std::uint32_t srcElement=0,std::uint32_t dstElement=0,std::uint32_t count=1)const{
        VkCopyDescriptorSet c{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};c.srcSet=src;c.dstSet=dst;c.srcBinding=c.dstBinding=2;
        c.srcArrayElement=srcElement;c.dstArrayElement=dstElement;c.descriptorCount=count;return c;
    }
};

void basic(){++cases;Fixture f;const auto e=f.ready();qualified(e);require(e.ignoredNonImageBindingCount==2,"opaque buffer and sampler bindings do not reject image evidence");
    require(e.bindings[0].image==Key{101,1}&&e.bindings[1].image==Key{102,2},"exact common image generations copied without a second image domain");
    require(e.bindings[0].binding==2&&e.bindings[0].arrayElement==0&&e.bindings[1].arrayElement==1&&e.bindings[2].binding==5,"binding/array element preserved");
    require(e.bindings[0].declaredLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&e.bindings[2].declaredLayout==VK_IMAGE_LAYOUT_GENERAL,"declared descriptor layout retained as declaration only");
    require(Tracker::matchingImage(e,{101,1})==1&&Tracker::matchingImage(e,{102,2})==2&&!Tracker::matchingImage(e,{101,9}),"exact candidate-image matching never joins generations");
    ok(f.t.validateEvidence(e));ok(f.t.end(f.commands[0],VK_SUCCESS));ok(f.t.validateEvidence(e));
    refused(f.dispatch(),Issue::Lifecycle);
}
void diagnosticSnapshots(){++cases;Fixture f(small(),false);for(auto& b:f.bindings)b.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;f.objects();ok(f.write(f.sets[0]));
    const auto before=f.t.stats();const auto snapshot=f.t.diagnosticSetImages(f.sets[0]);const auto after=f.t.stats();
    require(snapshot.known&&!snapshot.issues&&snapshot.count==3,"diagnostic graphics declaration snapshots reuse exact ordinary image values");
    require(snapshot.bindings[0].image==Key{101,1}&&snapshot.bindings[0].set==snapshot.set&&snapshot.bindings[0].descriptorVersion==snapshot.version,"diagnostic image/view/set generations and descriptor version are copied");
    require(before.dispatches==after.dispatches&&before.mutations==after.mutations&&before.registrations==after.registrations,"const diagnostic snapshot never mutates compute counters or admission");
    require(!f.t.diagnosticSetImages(h<VkDescriptorSet>(0xdead)).known,"unknown diagnostic set is explicitly unknown");
    f.t.imageRetiring({101,1});const auto retired=f.t.diagnosticSetImages(f.sets[0]);
    require(retired.issues&&retired.count<snapshot.count,"retired image generation is not published as a live graphics input");
}
void diagnosticImageMembership(){++cases;Fixture f;
    const auto equivalent=[&](VkDescriptorSet set){
        const auto full=f.t.diagnosticSetImages(set);const auto refs=f.t.imageReferences(set);
        require(full.set==refs.set&&full.layout==refs.layout&&full.pool==refs.pool&&full.version==refs.version&&full.known==refs.known&&
            full.referencedImages==refs.referencedImages&&full.referencedCount==refs.referencedCount&&full.imageReferencesKnown==refs.imageReferencesKnown&&
            full.unwrittenCount==refs.unwrittenCount&&full.layoutBindingCount==refs.layoutBindingCount&&full.declaredImageCount==refs.declaredImageCount&&
            full.layoutIssues==refs.layoutIssues&&full.setIssues==refs.setIssues,"compact identity query preserves consumer membership without typed SDK payloads");
        require(f.t.setIdentity(set)==full.set,"bind identity query returns the exact set generation");
    };
    static_assert(sizeof(SetImageReferences)*4<sizeof(SetImageSnapshot));
    equivalent(f.sets[0]);equivalent(h<VkDescriptorSet>(0xdead));
    auto snapshot=f.t.diagnosticSetImages(f.sets[0]);
    require(snapshot.imageReferencesKnown&&snapshot.referencedCount==0&&snapshot.unwrittenCount==3&&
        (snapshot.issues&bit(Issue::UndefinedDescriptor)),"known unused unwritten slots name no image without qualifying typed evidence");
    ok(f.write(f.sets[0]));
    VkDescriptorImageInfo value{VK_NULL_HANDLE,f.views[0],VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=f.sets[0];w.dstBinding=2;
    w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=&value;
    issue(f.t.updateDescriptors(1,&w,0,nullptr),Issue::UnsupportedLayout);
    snapshot=f.t.diagnosticSetImages(f.sets[0]);
    equivalent(f.sets[0]);
    require(snapshot.imageReferencesKnown&&snapshot.referencedCount==3&&snapshot.referencedImages[0]==Key{101,1}&&snapshot.count==2,
        "typed layout rejection does not hide an exact live image reference from a consumer guard");
    f.begin();f.bind();refused(f.dispatch(),Issue::UnsupportedLayout);
    f.t.viewDestroying(f.views[0]);const auto other=f.viewInfo(1);ok(f.t.viewCreated(f.views[0],&other,VK_SUCCESS));
    equivalent(f.sets[0]);
    require(!f.t.diagnosticSetImages(f.sets[0]).imageReferencesKnown,"reused view handle cannot qualify old image membership");
    f.t.setUnsupported(f.sets[1],Issue::Capacity);
    equivalent(f.sets[1]);
    require(!f.t.diagnosticSetImages(f.sets[1]).imageReferencesKnown,"lost structural updates remain unknown even with unwritten stored slots");

    Fixture wide(small(),false);wide.bindings[0].descriptorCount=maxSetImageBindings;
    const auto li=wide.layoutInfo();issue(wide.t.setLayoutCreated(wide.setLayout,&li,VK_SUCCESS),Issue::Capacity);
    const auto pi=wide.poolInfo();ok(wide.t.poolCreated(wide.pool,&pi,VK_SUCCESS));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=wide.pool;
    ai.descriptorSetCount=1;ai.pSetLayouts=&wide.setLayout;
    issue(wide.t.setsAllocated(&ai,wide.sets.data(),VK_SUCCESS),Issue::Capacity);
    snapshot=wide.t.diagnosticSetImages(wide.sets[0]);
    require(!snapshot.imageReferencesKnown&&snapshot.declaredImageCount==maxSetImageBindings+1&&snapshot.layoutBindingCount==4&&
        (snapshot.layoutIssues&bit(Issue::Capacity))&&(snapshot.setIssues&bit(Issue::Capacity)),
        "overflowing image declarations remain blocked and report actual declaration counts without enlarging storage");
}
void graphicsLayoutThirtyThreeImages(){++cases;Fixture f;
    std::array<VkDescriptorSetLayoutBinding,55> bindings{};std::array<VkDescriptorUpdateTemplateEntry,55> entries{};
    std::array<unsigned char,55*32> data{};
    for(unsigned n=0;n<bindings.size();++n){
        const auto type=n<33?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[n]={n,type,1,VK_SHADER_STAGE_ALL,nullptr};entries[n]={n,0,1,type,n*32,32};
        if(n<33){const VkDescriptorImageInfo value{VK_NULL_HANDLE,f.views[n==32?1:0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            std::memcpy(data.data()+n*32,&value,sizeof(value));}
        else {const VkDescriptorBufferInfo value{h<VkBuffer>(99),0,64};std::memcpy(data.data()+n*32,&value,sizeof(value));}
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=bindings.size();li.pBindings=bindings.data();
    const auto layout=h<VkDescriptorSetLayout>(2700);ok(f.t.setLayoutCreated(layout,&li,VK_SUCCESS));
    const auto set=h<VkDescriptorSet>(5700);VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool=f.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;ok(f.t.setsAllocated(&ai,&set,VK_SUCCESS));
    VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};ti.descriptorSetLayout=layout;
    ti.descriptorUpdateEntryCount=entries.size();ti.pDescriptorUpdateEntries=entries.data();const auto temp=h<VkDescriptorUpdateTemplate>(9700);
    ok(f.t.templateCreated(temp,&ti,VK_SUCCESS));ok(f.t.updateWithTemplate(set,temp,data.data()));
    const auto snapshot=f.t.diagnosticSetImages(set);
    require(!snapshot.issues&&snapshot.imageReferencesKnown&&snapshot.count==33&&snapshot.referencedCount==33&&
        snapshot.layoutBindingCount==55&&snapshot.declaredImageCount==33&&snapshot.referencedImages[32]==Key{102,2},
        "55-binding/33-image template publishes the final distinct image instead of rejecting the whole set");
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&layout;
    const auto pipelineLayout=h<VkPipelineLayout>(3700);ok(f.t.pipelineLayoutCreated(pipelineLayout,&pl,VK_SUCCESS));
    auto pi=f.pipelineInfo();pi.layout=pipelineLayout;const auto pipeline=h<VkPipeline>(8700);ok(f.t.pipelineCreated(pipeline,&pi,VK_SUCCESS));
    f.begin();f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);
    const auto evidence=f.t.preDispatch(f.commands[0],8,4,1,DispatchKind::Direct,true);qualified(evidence,33);
    require(evidence.guardImagesKnown&&evidence.guardImageCount==33&&evidence.bindings[32].image==Key{102,2},
        "dispatch and consumer evidence retain the thirty-third descriptor without a truncated success");
    ok(f.t.validateEvidence(evidence));
}
void ordinaryUpdates(){++cases;Fixture f;const auto before=f.ready();const auto version=before.bindings[0].descriptorVersion;
    ok(f.write(f.sets[0]));refused(f.dispatch(),Issue::DescriptorChanged);issue(f.t.validateEvidence(before),Issue::DescriptorChanged);
    f.bind();refused(f.dispatch(),Issue::DescriptorChanged);f.begin();f.bind();const auto after=f.dispatch();qualified(after);
    require(after.command!=before.command&&after.bindings[0].descriptorVersion>version,"new Begin, not rebinding, recovers changed ordinary set");
    ok(f.t.end(f.commands[0],VK_SUCCESS));ok(f.write(f.sets[0]));issue(f.t.validateEvidence(after),Issue::DescriptorChanged);
}
void historicalReferences(){++cases;Fixture f;ok(f.write(f.sets[0]));ok(f.write(f.sets[1]));f.begin();f.bind();qualified(f.dispatch());
    f.bind(0,1);qualified(f.dispatch());ok(f.write(f.sets[0]));refused(f.dispatch(),Issue::DescriptorChanged);
    f.begin();f.bind(0,1);qualified(f.dispatch());
    f.t.setUnsupported(f.sets[1]);refused(f.dispatch(),Issue::DescriptorChanged);
    f.begin();f.bind(0,1);refused(f.dispatch(),Issue::UnsupportedDescriptor);
}
void writeThenCopies(){++cases;Fixture f;ok(f.write(f.sets[1]));
    VkDescriptorImageInfo info{VK_NULL_HANDLE,f.views[1],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=f.sets[0];w.dstBinding=2;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.descriptorCount=1;w.pImageInfo=&info;
    auto c=f.copy(f.sets[0],f.sets[1]);ok(f.t.updateDescriptors(1,&w,1,&c));f.begin();f.bind(0,1);const auto e=f.dispatch();qualified(e);
    require(e.bindings[0].image==Key{102,2},"copy observes preceding writes in the same update call");
    VkCopyDescriptorSet sequence[2]{f.copy(f.sets[1],f.sets[0]),f.copy(f.sets[0],f.sets[1],0,1)};
    ok(f.t.updateDescriptors(0,nullptr,2,sequence));f.begin();f.bind(0,1);qualified(f.dispatch());
}
void undefinedCopies(){++cases;Fixture f;ok(f.write(f.sets[1]));auto copy=f.copy(f.sets[0],f.sets[1]);
    ok(f.t.updateDescriptors(0,nullptr,1,&copy));f.begin();f.bind(0,1);refused(f.dispatch(),Issue::UndefinedDescriptor);
    // A defined source whose view was destroyed also copies as undefined.
    Fixture g;ok(g.write(g.sets[0]));ok(g.write(g.sets[1]));g.t.viewDestroying(g.views[0]);copy=g.copy(g.sets[0],g.sets[1]);
    ok(g.t.updateDescriptors(0,nullptr,1,&copy));g.begin();g.bind(0,1);refused(g.dispatch(),Issue::UndefinedDescriptor);
}
void crossingAndOverlap(){++cases;
    for(unsigned mode=0;mode<5;++mode){Fixture f;ok(f.write(f.sets[0]));ok(f.write(f.sets[1]));
        auto copy=f.copy(f.sets[0],f.sets[1]);
        if(mode==0){copy.srcArrayElement=1;copy.descriptorCount=2;}
        if(mode==1){copy.dstArrayElement=1;copy.descriptorCount=2;}
        if(mode==2){copy.srcArrayElement=UINT32_MAX;copy.descriptorCount=UINT32_MAX;}
        if(mode==3){copy.dstSet=copy.srcSet;copy.descriptorCount=2;}
        if(mode==4){copy.dstBinding=5;}
        const auto result=f.t.updateDescriptors(0,nullptr,1,&copy);
        issue(result,mode<3?Issue::CrossBinding:mode==3?Issue::InvalidInput:Issue::UnsupportedDescriptor);
        f.begin();f.bind(0,mode==3?0:1);require(!f.dispatch().metadataAccepted,"unsupported copy poisons destination instead of preserving false old success");
    }
    Fixture f;ok(f.write(f.sets[0]));auto copy=f.copy(f.sets[0],f.sets[0],0,1,1);ok(f.t.updateDescriptors(0,nullptr,1,&copy));
    f.begin();f.bind();const auto e=f.dispatch();qualified(e);require(e.bindings[1].image==Key{101,1},"same-set non-overlapping copy accepted");
}
void badWrites(){++cases;
    for(unsigned mode=0;mode<8;++mode){Fixture f;ok(f.write(f.sets[0]));
        VkDescriptorImageInfo info{VK_NULL_HANDLE,f.views[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=f.sets[0];w.dstBinding=2;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.descriptorCount=1;w.pImageInfo=&info;
        VkBaseInStructure extension{};
        const Issue expected[]{Issue::CrossBinding,Issue::CrossBinding,Issue::InvalidInput,Issue::UnsupportedExtension,
            Issue::UnsupportedLayout,Issue::UnknownObject,Issue::UnsupportedDescriptor,Issue::UnsupportedLayout};
        if(mode==0){w.dstArrayElement=1;w.descriptorCount=2;}if(mode==1){w.dstArrayElement=UINT32_MAX;w.descriptorCount=UINT32_MAX;}
        if(mode==2)w.pImageInfo=nullptr;if(mode==3)w.pNext=&extension;if(mode==4)info.imageLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(mode==5)info.imageView=h<VkImageView>(9999);if(mode==6)w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if(mode==7){w.dstBinding=5;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;}
        issue(f.t.updateDescriptors(1,&w,0,nullptr),expected[mode]);f.begin();f.bind();refused(f.dispatch(),expected[mode]);
    }
}
void objectReuse(){++cases;Fixture f;const auto old=f.ready();const auto original=f.images[0];
    f.t.imageRetiring({original.image,original.generation+1});ok(f.t.validateEvidence(old));
    f.t.imageRetiring({original.image,original.generation});auto replacement=original;replacement.generation=3;ok(f.t.imageObserved(replacement));
    f.t.imageRetiring({original.image,original.generation});require(f.t.stats().liveImages==2,"stale retirement cannot delete replacement common generation");
    refused(f.dispatch(),Issue::StaleGeneration);issue(f.t.validateEvidence(old),Issue::StaleGeneration);
    f.t.viewDestroying(f.views[0]);const auto vi=f.viewInfo(0);ok(f.t.viewCreated(f.views[0],&vi,VK_SUCCESS));
    f.begin();f.bind();refused(f.dispatch(),Issue::StaleGeneration);ok(f.write(f.sets[0]));f.begin();f.bind();const auto current=f.dispatch();qualified(current);
    require(current.bindings[0].image.generation==3&&current.bindings[0].view!=old.bindings[0].view,"new descriptor binds exact new image and view identities");
    ok(f.t.imageObserved(replacement));require(f.t.stats().liveImages==2,"same-key observation is idempotent");
    issue(f.t.imageObserved(original),Issue::StaleGeneration);
}
void poolAndSetLifetimes(){++cases;Fixture f;const auto old=f.ready();
    f.t.poolReset(f.pool,VK_ERROR_OUT_OF_HOST_MEMORY);ok(f.t.validateEvidence(old));
    f.t.setsFreed(f.pool,1,f.sets.data(),VK_ERROR_OUT_OF_HOST_MEMORY);ok(f.t.validateEvidence(old));
    f.t.setsFreed(f.pool,1,f.sets.data(),VK_SUCCESS);issue(f.t.validateEvidence(old),Issue::UnknownObject);
    const auto count=f.t.stats().liveSets;f.t.poolReset(f.pool,VK_SUCCESS);require(count==1&&f.t.stats().liveSets==0,"successful pool reset retires every owned set");
    f.allocate();ok(f.write(f.sets[0]));f.begin();f.bind();const auto next=f.dispatch();qualified(next);
    require(next.bindings[0].set!=old.bindings[0].set&&next.bindings[0].pool!=old.bindings[0].pool,"pool reset and reused set handles have new epochs");
    f.t.poolDestroying(f.pool);refused(f.dispatch(),Issue::UnknownObject);require(f.t.stats().liveSets==0,"pool destruction retires sets");
}
void failedCreatesAndAllocation(){++cases;Fixture f;const auto old=f.ready();const auto before=f.t.stats().registrations;
    const auto vi=f.viewInfo(0);issue(f.t.viewCreated(f.views[0],&vi,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    const auto li=f.layoutInfo();issue(f.t.setLayoutCreated(f.setLayout,&li,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    const auto pi=f.pipelineLayoutInfo();issue(f.t.pipelineLayoutCreated(f.pipelineLayout,&pi,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    const auto po=f.poolInfo();issue(f.t.poolCreated(f.pool,&po,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    const VkDescriptorSetLayout layouts[]{f.setLayout,f.setLayout};VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool=f.pool;ai.descriptorSetCount=2;ai.pSetLayouts=layouts;
    const VkDescriptorSet outputs[]{h<VkDescriptorSet>(5501),VK_NULL_HANDLE};
    issue(f.t.setsAllocated(&ai,outputs,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    issue(f.t.setsAllocated(&ai,outputs,VK_SUCCESS),Issue::InvalidInput);
    require(f.t.stats().registrations==before&&f.t.stats().liveSets==2,"failed calls and invalid full output preflight publish nothing");ok(f.t.validateEvidence(old));
}
void commandLifetimes(){++cases;Fixture f;auto old=f.ready();
    f.t.reset(f.commands[0],VK_ERROR_OUT_OF_HOST_MEMORY);ok(f.t.validateEvidence(old));
    f.t.reset(f.commands[0],VK_SUCCESS);issue(f.t.validateEvidence(old),Issue::StaleGeneration);refused(f.dispatch(),Issue::Lifecycle);
    f.begin();f.bind();qualified(f.dispatch());f.t.commandPoolReset(f.commandPool,VK_SUCCESS);refused(f.dispatch(),Issue::Lifecycle);
    f.begin();f.bind();old=f.dispatch();qualified(old);f.t.commandsFreed(1,f.commands.data());issue(f.t.validateEvidence(old),Issue::UnknownObject);
    f.t.commandPoolDestroying(f.commandPool);require(f.t.stats().liveCommands==0,"command pool destroy retires all allocated command buffers");
}
void stickyUnknownAllocation(){++cases;
    for(unsigned mode=0;mode<3;++mode){Fixture f;f.t.commandsFreed(1,f.commands.data());
        VkBaseInStructure extension{};VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool=f.commandPool;ai.commandBufferCount=1;ai.level=mode==0?VK_COMMAND_BUFFER_LEVEL_SECONDARY:VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        if(mode==1)ai.pNext=&extension;
        const auto expected=mode==0?Issue::Secondary:mode==1?Issue::UnsupportedExtension:Issue::SimultaneousUse;
        const auto a=f.t.commandsAllocated(&ai,f.commands.data(),VK_SUCCESS);if(mode<2)issue(a,expected);else ok(a);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};if(mode==2)bi.flags=VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        issue(f.t.begin(f.commands[0],&bi,VK_SUCCESS),expected);f.bind();refused(f.dispatch(),expected);
    }
}
void unsupportedLayouts(){++cases;
    for(unsigned mode=0;mode<5;++mode){Fixture f;auto li=f.layoutInfo();VkBaseInStructure extension{};VkSampler sampler=h<VkSampler>(44);
        if(mode==0)li.pNext=&extension;if(mode==1)li.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        if(mode==2){f.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;f.bindings[0].pImmutableSamplers=&sampler;}
        if(mode==3)f.bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        if(mode==4)f.bindings[1].binding=2;
        const Issue expected[]{Issue::UnsupportedExtension,Issue::UnsupportedExtension,Issue::ImmutableSampler,Issue::UnsupportedDescriptor,Issue::InvalidInput};
        issue(f.t.setLayoutCreated(h<VkDescriptorSetLayout>(2200+mode),&li,VK_SUCCESS),expected[mode]);
        const auto isolated=f.ready();qualified(isolated);
    }
    Fixture f;auto li=f.layoutInfo();VkSampler sampler=h<VkSampler>(1);f.bindings[3].pImmutableSamplers=&sampler;
    ok(f.t.setLayoutCreated(h<VkDescriptorSetLayout>(2299),&li,VK_SUCCESS));
}
void ordinaryViewRanges(){++cases;Fixture f;auto image=f.images[0];image.image=103;image.generation=3;image.description.format=VK_FORMAT_D32_SFLOAT_S8_UINT;
    image.description.mipLevels=4;image.description.arrayLayers=3;image.description.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;ok(f.t.imageObserved(image));
    auto vi=f.viewInfo(0);vi.image=h<VkImage>(103);vi.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;vi.format=VK_FORMAT_D32_SFLOAT_S8_UINT;
    vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,1,VK_REMAINING_MIP_LEVELS,1,VK_REMAINING_ARRAY_LAYERS};vi.components.g=VK_COMPONENT_SWIZZLE_R;
    ok(f.t.viewCreated(h<VkImageView>(1099),&vi,VK_SUCCESS));ok(f.write(f.sets[0],h<VkImageView>(1099)));f.begin();f.bind();const auto e=f.dispatch();qualified(e);
    require(e.bindings[0].range.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT&&e.bindings[0].range.levelCount==3&&e.bindings[0].range.layerCount==2,
        "ordinary view facts retain depth and multi-range identities independently of RP subset");
    require(e.bindings[0].components.g==VK_COMPONENT_SWIZZLE_R,"swizzle retained without a pixel interpretation claim");
    vi.subresourceRange.baseMipLevel=UINT32_MAX;issue(f.t.viewCreated(h<VkImageView>(1098),&vi,VK_SUCCESS),Issue::UnsupportedView);
}
void compatibilityAndDisturb(){++cases;Fixture f;ok(f.write(f.sets[0]));f.begin();f.bind();qualified(f.dispatch());
    const auto info=f.pipelineLayoutInfo();const auto other=h<VkPipelineLayout>(3002);ok(f.t.pipelineLayoutCreated(other,&info,VK_SUCCESS));
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,other,0,1,f.sets.data(),0,nullptr);refused(f.dispatch(),Issue::IncompatibleLayout);
    auto pipe=f.pipelineInfo();pipe.layout=other;ok(f.t.pipelineCreated(h<VkPipeline>(8002),&pipe,VK_SUCCESS));
    f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,h<VkPipeline>(8002));qualified(f.dispatch());
    // Graphics state is separate; compute bindings remain unchanged.
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_GRAPHICS,VK_NULL_HANDLE,UINT32_MAX,UINT32_MAX,nullptr,UINT32_MAX,nullptr);
    f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_GRAPHICS,VK_NULL_HANDLE);qualified(f.dispatch());
    const auto earlier=f.dispatch();ok(f.write(f.sets[0]));issue(f.t.validateEvidence(earlier),Issue::DescriptorChanged);
}
void missingAndUnsupportedDispatch(){++cases;Fixture f;ok(f.write(f.sets[0]));f.begin();f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);
    refused(f.dispatch(),Issue::MissingBinding);f.bind();qualified(f.dispatch());
    for(auto kind:{DispatchKind::Indirect,DispatchKind::Base}){f.begin();f.bind();const auto before=f.dispatch();qualified(before);
        refused(f.t.preDispatch(f.commands[0],1,1,1,kind),Issue::UnsupportedDispatch);qualified(f.dispatch());ok(f.t.validateEvidence(before));}
    f.begin();f.bind();refused(f.t.preDispatch(f.commands[0],1,1,1,DispatchKind::Unknown),Issue::UnsupportedDispatch);refused(f.dispatch(),Issue::UnsupportedDispatch);
    f.begin();f.bind();f.t.commandUnsupported(f.commands[0]);refused(f.dispatch(),Issue::UnsupportedExtension);
    f.begin();f.bind();qualified(f.dispatch());
}
void unsupportedPoolAndAlloc(){++cases;Fixture f;auto p=f.poolInfo();p.flags|=VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    issue(f.t.poolCreated(h<VkDescriptorPool>(4099),&p,VK_SUCCESS),Issue::UnsupportedExtension);
    VkBaseInStructure extension{};VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.pNext=&extension;
    ai.descriptorPool=f.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&f.setLayout;const auto set=h<VkDescriptorSet>(5099);
    issue(f.t.setsAllocated(&ai,&set,VK_SUCCESS),Issue::UnsupportedExtension);
    f.begin();f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipelineLayout,0,1,&set,0,nullptr);
    f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);refused(f.dispatch(),Issue::UnsupportedExtension);
}
void pipelineAndLayoutReuse(){++cases;Fixture f;const auto old=f.ready();f.t.pipelineDestroying(f.pipeline);
    const auto pi=f.pipelineInfo();ok(f.t.pipelineCreated(f.pipeline,&pi,VK_ERROR_OUT_OF_HOST_MEMORY));
    refused(f.dispatch(),Issue::StaleGeneration);issue(f.t.validateEvidence(old),Issue::StaleGeneration);
    // Current-binding CPU metadata can describe the replacement. This is not
    // validation of the entire recording's pipeline lifetime or submission.
    f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);qualified(f.dispatch());
    f.t.pipelineLayoutDestroying(f.pipelineLayout);refused(f.dispatch(),Issue::UnknownObject);
    const auto pl=f.pipelineLayoutInfo();ok(f.t.pipelineLayoutCreated(f.pipelineLayout,&pl,VK_SUCCESS));refused(f.dispatch(),Issue::StaleGeneration);
    Fixture g;const auto e=g.ready();g.t.setLayoutDestroying(g.setLayout);issue(g.t.validateEvidence(e),Issue::UnknownObject);
}
void capacityAndBudgets(){++cases;auto config=small();config.images=2;config.views=2;config.setLayouts=1;config.pipelineLayouts=1;config.pools=1;config.sets=2;config.pipelines=1;config.commandPools=1;config.commands=2;config.templates=1;
    Fixture f(config);auto record=f.images[0];record.image=103;record.generation=3;issue(f.t.imageObserved(record),Issue::Capacity);
    const auto vi=f.viewInfo(0);issue(f.t.viewCreated(h<VkImageView>(1999),&vi,VK_SUCCESS),Issue::Capacity);
    const auto li=f.layoutInfo();issue(f.t.setLayoutCreated(h<VkDescriptorSetLayout>(2999),&li,VK_SUCCESS),Issue::Capacity);
    const auto pl=f.pipelineLayoutInfo();issue(f.t.pipelineLayoutCreated(h<VkPipelineLayout>(3999),&pl,VK_SUCCESS),Issue::Capacity);
    const auto po=f.poolInfo();issue(f.t.poolCreated(h<VkDescriptorPool>(4999),&po,VK_SUCCESS),Issue::Capacity);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=f.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&f.setLayout;
    const auto ds=h<VkDescriptorSet>(5999);issue(f.t.setsAllocated(&ai,&ds,VK_SUCCESS),Issue::Capacity);
    const auto pi=f.pipelineInfo();issue(f.t.pipelineCreated(h<VkPipeline>(8999),&pi,VK_SUCCESS),Issue::Capacity);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};issue(f.t.commandPoolCreated(h<VkCommandPool>(6999),&cp,VK_SUCCESS),Issue::Capacity);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=f.commandPool;ca.commandBufferCount=1;const auto cb=h<VkCommandBuffer>(7999);
    issue(f.t.commandsAllocated(&ca,&cb,VK_SUCCESS),Issue::Capacity);
    VkDescriptorUpdateTemplateEntry entry{2,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,0,sizeof(VkDescriptorImageInfo)};
    VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};ti.descriptorSetLayout=f.setLayout;ti.descriptorUpdateEntryCount=1;ti.pDescriptorUpdateEntries=&entry;
    ok(f.t.templateCreated(h<VkDescriptorUpdateTemplate>(9001),&ti,VK_SUCCESS));issue(f.t.templateCreated(h<VkDescriptorUpdateTemplate>(9002),&ti,VK_SUCCESS),Issue::Capacity);
    const auto s=f.t.stats();for(const auto& usage:s.objects){require(usage.live==usage.capacity&&usage.highWater==usage.capacity,"per-object capacity and high water visible");require(usage.capacityRefusals==1,"per-object capacity rejection counted");}
    require(s.capacityRefusals==10&&!s.fatalIssues,"isolated object capacity failures do not poison whole device");qualified(f.ready());
    issue(f.t.updateDescriptors(UINT32_MAX,nullptr,0,nullptr),Issue::Capacity);refused(f.dispatch(),Issue::Stopped);
}
void aggregateDispatchCapacity(){++cases;Fixture f;
    VkDescriptorSetLayoutBinding b{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,17,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=1;li.pBindings=&b;
    const auto layout=h<VkDescriptorSetLayout>(2100);ok(f.t.setLayoutCreated(layout,&li,VK_SUCCESS));
    const VkDescriptorSetLayout layouts[]{layout,layout};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=2;pl.pSetLayouts=layouts;
    const auto pipelineLayout=h<VkPipelineLayout>(3100);ok(f.t.pipelineLayoutCreated(pipelineLayout,&pl,VK_SUCCESS));
    const VkDescriptorSet sets[]{h<VkDescriptorSet>(5100),h<VkDescriptorSet>(5101)};VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=f.pool;ai.descriptorSetCount=2;ai.pSetLayouts=layouts;ok(f.t.setsAllocated(&ai,sets,VK_SUCCESS));
    std::array<VkDescriptorImageInfo,17> values{};for(auto& v:values)v={VK_NULL_HANDLE,f.views[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for(auto set:sets){VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.descriptorType=b.descriptorType;w.descriptorCount=17;w.pImageInfo=values.data();ok(f.t.updateDescriptors(1,&w,0,nullptr));}
    auto pi=f.pipelineInfo();pi.layout=pipelineLayout;const auto pipe=h<VkPipeline>(8100);ok(f.t.pipelineCreated(pipe,&pi,VK_SUCCESS));
    f.begin();f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,2,sets,0,nullptr);f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,pipe);refused(f.dispatch(),Issue::Capacity);
}
void dynamicOpaqueBindings(){++cases;Fixture f;f.bindings[2].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    const auto li=f.layoutInfo();const auto sl=h<VkDescriptorSetLayout>(2300);ok(f.t.setLayoutCreated(sl,&li,VK_SUCCESS));
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&sl;const auto l=h<VkPipelineLayout>(3300);ok(f.t.pipelineLayoutCreated(l,&pl,VK_SUCCESS));
    const auto s=h<VkDescriptorSet>(5300);VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=f.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&sl;ok(f.t.setsAllocated(&ai,&s,VK_SUCCESS));
    ok(f.write(s));auto pi=f.pipelineInfo();pi.layout=l;const auto p=h<VkPipeline>(8300);ok(f.t.pipelineCreated(p,&pi,VK_SUCCESS));
    f.begin();const std::uint32_t offset=256;f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,l,0,1,&s,1,&offset);f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,p);const auto dynamic=f.dispatch();qualified(dynamic);
    require(!dynamic.computeRestoreKnown&&!dynamic.computeRestoreSetCount,"unretained dynamic offsets refuse compute restoration while preserving transfer evidence");
    f.begin();f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,l,0,1,&s,0,nullptr);f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,p);refused(f.dispatch(),Issue::InvalidInput);
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,l,0,1,&s,1,&offset);qualified(f.dispatch());
}
void recoverCurrentBindings(){++cases;Fixture f;ok(f.write(f.sets[0]));ok(f.write(f.sets[1]));f.begin();f.bind();const auto before=f.dispatch();qualified(before);
    const auto unknown=h<VkPipeline>(8999);f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,unknown);refused(f.dispatch(),Issue::UnknownObject);
    f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);qualified(f.dispatch());ok(f.t.validateEvidence(before));
    auto pi=f.pipelineInfo();pi.flags=VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;const auto unsupported=h<VkPipeline>(8998);
    issue(f.t.pipelineCreated(unsupported,&pi,VK_SUCCESS),Issue::UnsupportedExtension);
    f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,unsupported);refused(f.dispatch(),Issue::UnsupportedExtension);
    f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);qualified(f.dispatch());ok(f.t.validateEvidence(before));
    const auto missing=h<VkDescriptorSet>(5999);f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipelineLayout,0,1,&missing,0,nullptr);refused(f.dispatch(),Issue::UnknownObject);
    f.bind();qualified(f.dispatch());
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,h<VkPipelineLayout>(3999),0,1,f.sets.data(),0,nullptr);refused(f.dispatch(),Issue::UnknownObject);
    f.bind();qualified(f.dispatch());
    // An opaque mutation before this set's first reference is local metadata.
    // Updating it after that reference still invalidates the whole recording.
    f.t.setUnsupported(f.sets[1]);f.bind(0,1);refused(f.dispatch(),Issue::UnsupportedDescriptor);f.bind();qualified(f.dispatch());
    f.t.setUnsupported(f.sets[1]);refused(f.dispatch(),Issue::DescriptorChanged);f.bind();refused(f.dispatch(),Issue::DescriptorChanged);
}
void recoverLocalLayouts(){++cases;Fixture f;ok(f.write(f.sets[0]));
    auto li=f.layoutInfo();li.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;const auto sl=h<VkDescriptorSetLayout>(2400);
    issue(f.t.setLayoutCreated(sl,&li,VK_SUCCESS),Issue::UnsupportedExtension);
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&sl;const auto l=h<VkPipelineLayout>(3400);
    issue(f.t.pipelineLayoutCreated(l,&pl,VK_SUCCESS),Issue::UnsupportedExtension);
    const auto set=h<VkDescriptorSet>(5400);VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=f.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&sl;
    issue(f.t.setsAllocated(&ai,&set,VK_SUCCESS),Issue::UnsupportedExtension);auto pi=f.pipelineInfo();pi.layout=l;const auto p=h<VkPipeline>(8400);
    issue(f.t.pipelineCreated(p,&pi,VK_SUCCESS),Issue::UnsupportedExtension);
    f.begin();f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,p);f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,l,0,1,&set,0,nullptr);refused(f.dispatch(),Issue::UnsupportedExtension);
    f.bind();qualified(f.dispatch());
    VkBaseInStructure extension{};pl.pNext=&extension;pl.pSetLayouts=&f.setLayout;const auto opaqueLayout=h<VkPipelineLayout>(3401);
    issue(f.t.pipelineLayoutCreated(opaqueLayout,&pl,VK_SUCCESS),Issue::UnsupportedExtension);pi.layout=opaqueLayout;const auto opaquePipeline=h<VkPipeline>(8401);
    issue(f.t.pipelineCreated(opaquePipeline,&pi,VK_SUCCESS),Issue::UnsupportedExtension);
    f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,opaquePipeline);f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,opaqueLayout,0,1,f.sets.data(),0,nullptr);refused(f.dispatch(),Issue::UnsupportedExtension);
    f.bind();qualified(f.dispatch());
}
void currentOpaqueBindingIssues(){++cases;Fixture f;ok(f.write(f.sets[0]));
    VkDescriptorSetLayoutBinding b{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=1;li.pBindings=&b;const auto sl=h<VkDescriptorSetLayout>(2500);ok(f.t.setLayoutCreated(sl,&li,VK_SUCCESS));
    const VkDescriptorSetLayout layouts[]{f.setLayout,sl};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=2;pl.pSetLayouts=layouts;
    const auto l=h<VkPipelineLayout>(3500);ok(f.t.pipelineLayoutCreated(l,&pl,VK_SUCCESS));const auto set=h<VkDescriptorSet>(5500);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=f.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&sl;ok(f.t.setsAllocated(&ai,&set,VK_SUCCESS));
    auto pi=f.pipelineInfo();pi.layout=l;const auto p=h<VkPipeline>(8500);ok(f.t.pipelineCreated(p,&pi,VK_SUCCESS));f.begin();
    f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,p);f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,l,0,1,f.sets.data(),0,nullptr);
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,l,1,1,&set,0,nullptr);refused(f.dispatch(),Issue::InvalidInput);
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,l,0,1,f.sets.data(),0,nullptr);refused(f.dispatch(),Issue::InvalidInput);
    const std::uint32_t offset=256;f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,l,1,1,&set,1,&offset);qualified(f.dispatch());
}
void recoverKnownValues(){++cases;
    for(unsigned mode=0;mode<5;++mode){Fixture f;ok(f.write(f.sets[1],f.views[1]));
        VkDescriptorImageInfo info{VK_NULL_HANDLE,f.views[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=f.sets[0];w.dstBinding=2;w.descriptorCount=1;
        w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=&info;
        const Issue expected[]{Issue::UnknownObject,Issue::UnsupportedLayout,Issue::UnsupportedView,Issue::UnsupportedExtension,Issue::StaleGeneration};
        if(mode==0)info.imageView=h<VkImageView>(1999);
        if(mode==1)info.imageLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(mode==2||mode==3){auto vi=f.viewInfo(0);VkImageViewUsageCreateInfo extension{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};extension.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
            if(mode==2)vi.subresourceRange.baseMipLevel=UINT32_MAX;else vi.pNext=&extension;
            info.imageView=h<VkImageView>(1998);issue(f.t.viewCreated(info.imageView,&vi,VK_SUCCESS),expected[mode]);}
        if(mode==4){f.t.imageRetiring({f.images[0].image,f.images[0].generation});auto replacement=f.images[0];replacement.generation=3;ok(f.t.imageObserved(replacement));}
        issue(f.t.updateDescriptors(1,&w,0,nullptr),expected[mode]);
        // Fill only the other image slots without repairing the bad value.
        VkDescriptorImageInfo other[2]{{VK_NULL_HANDLE,f.views[1],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,f.views[1],VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet remaining[2]{w,w};remaining[0].dstArrayElement=1;remaining[0].pImageInfo=other;
        remaining[1].dstBinding=5;remaining[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;remaining[1].pImageInfo=other+1;
        ok(f.t.updateDescriptors(2,remaining,0,nullptr));
        f.begin();f.bind();refused(f.dispatch(),expected[mode]);f.bind(0,1);qualified(f.dispatch());
        // A correct write replaces bad value facts; the old CB still cannot be
        // repaired because an ordinary historically referenced set was updated.
        ok(f.write(f.sets[0],f.views[1]));f.bind();refused(f.dispatch(),Issue::DescriptorChanged);
        f.begin();f.bind();qualified(f.dispatch());
    }
    // Correct replacement before first binding can be used in that recording.
    Fixture f;issue(f.write(f.sets[0],h<VkImageView>(1999)),Issue::UnknownObject);ok(f.write(f.sets[0]));f.begin();f.bind();qualified(f.dispatch());
    // Unknown structure continues to poison the set despite good image writes.
    VkBaseInStructure extension{};VkDescriptorImageInfo info{VK_NULL_HANDLE,f.views[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.pNext=&extension;w.dstSet=f.sets[1];w.dstBinding=2;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=&info;
    issue(f.t.updateDescriptors(1,&w,0,nullptr),Issue::UnsupportedExtension);issue(f.write(f.sets[1]),Issue::UnsupportedExtension);
    f.begin();f.bind(0,1);refused(f.dispatch(),Issue::UnsupportedExtension);f.bind();qualified(f.dispatch());
}
void historicalSetRetirement(){++cases;
    for(unsigned mode=0;mode<3;++mode){Fixture f;ok(f.write(f.sets[0]));ok(f.write(f.sets[1]));f.begin();f.bind();const auto old=f.dispatch();qualified(old);f.bind(0,1);qualified(f.dispatch());
        if(mode==0)f.t.setsFreed(f.pool,1,f.sets.data(),VK_SUCCESS);
        if(mode==1){f.t.setsFreed(f.pool,1,f.sets.data(),VK_SUCCESS);VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=f.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&f.setLayout;
            ok(f.t.setsAllocated(&ai,f.sets.data(),VK_SUCCESS));ok(f.write(f.sets[0]));}
        if(mode==2){f.t.poolReset(f.pool,VK_SUCCESS);f.allocate();ok(f.write(f.sets[0]));ok(f.write(f.sets[1]));}
        const auto expected=mode==0?Issue::UnknownObject:Issue::StaleGeneration;
        f.bind(0,1);refused(f.dispatch(),expected);issue(f.t.validateEvidence(old),expected);
        f.begin();f.bind(0,1);qualified(f.dispatch());
    }
}
void referenceHistoryBudget(){++cases;auto config=small();config.sets=1024;Fixture f(config);
    std::array<VkDescriptorSet,maxReferencedSets+1> sets{};
    for(std::size_t i=0;i<sets.size();++i){sets[i]=h<VkDescriptorSet>(10000+i);VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool=f.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&f.setLayout;ok(f.t.setsAllocated(&ai,&sets[i],VK_SUCCESS));ok(f.write(sets[i]));}
    f.begin();f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);DispatchEvidence first;
    allocations=allocationBytes=0;measure=true;
    for(std::size_t i=0;i<maxReferencedSets;++i){f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipelineLayout,0,1,&sets[i],0,nullptr);
        const auto e=f.dispatch();qualified(e);if(!i)first=e;
        if(i+1==65||i+1==149||i+1==512){require(f.t.stats().referencedSetsHighWater==i+1,"65/149/512 unique historical set references stay accepted and visible");ok(f.t.validateEvidence(first));}}
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipelineLayout,0,1,&sets.back(),0,nullptr);refused(f.dispatch(),Issue::Capacity);
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipelineLayout,0,1,&sets.front(),0,nullptr);refused(f.dispatch(),Issue::Capacity);issue(f.t.validateEvidence(first),Issue::Capacity);
    measure=false;require(allocations==0&&allocationBytes==0,"full reference history and overflow allocate no hot-path heap");
    const auto state=f.t.stats();require(state.referencedSetsHighWater==512&&state.referencedSetsCapacity==512&&state.referenceOverflowRefusals==1&&state.capacityRefusals==1,
        "reference capacity/high-water/overflow accounting remains bounded and exact");
    f.t.reset(f.commands[0],VK_SUCCESS);f.begin();f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipelineLayout,0,1,&sets.back(),0,nullptr);qualified(f.dispatch());
    require(f.t.stats().referenceOverflowRefusals==1,"new recording recovers without erasing lifetime overflow telemetry");
}
struct TemplatePayload {
    std::array<unsigned char,256> data{};std::array<VkDescriptorUpdateTemplateEntry,4> entries{{
        {2,0,2,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,33},{5,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,100,0},
        {9,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,137,0},{10,0,1,VK_DESCRIPTOR_TYPE_SAMPLER,177,0}}};
    explicit TemplatePayload(const Fixture& f){VkDescriptorImageInfo a{VK_NULL_HANDLE,f.views[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo b{VK_NULL_HANDLE,f.views[1],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorImageInfo storage{VK_NULL_HANDLE,f.views[1],VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo buffer{h<VkBuffer>(123),0,64};VkDescriptorImageInfo sampler{h<VkSampler>(124),VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};
        std::memcpy(data.data()+1,&a,sizeof(a));std::memcpy(data.data()+34,&b,sizeof(b));std::memcpy(data.data()+100,&storage,sizeof(storage));
        std::memcpy(data.data()+137,&buffer,sizeof(buffer));std::memcpy(data.data()+177,&sampler,sizeof(sampler));}
    void pack(){static_assert(sizeof(VkDescriptorImageInfo)==sizeof(VkDescriptorBufferInfo));const auto original=data;std::size_t at{};
        for(auto& e:entries){for(std::size_t j=0;j<e.descriptorCount;++j){std::memcpy(data.data()+at,original.data()+e.offset+j*e.stride,sizeof(VkDescriptorImageInfo));at+=sizeof(VkDescriptorImageInfo);}
            e.offset=at-e.descriptorCount*sizeof(VkDescriptorImageInfo);e.stride=sizeof(VkDescriptorImageInfo);}}
    VkDescriptorUpdateTemplateCreateInfo info(VkDescriptorSetLayout layout)const {VkDescriptorUpdateTemplateCreateInfo i{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        i.descriptorSetLayout=layout;i.descriptorUpdateEntryCount=static_cast<std::uint32_t>(entries.size());i.pDescriptorUpdateEntries=entries.data();return i;}
};
struct TemplateReadState {const unsigned char* base{};std::size_t calls{},bytes{};unsigned failureMode{};};
std::size_t templateRead(void* user,const void* source,void* destination,std::size_t bytes){auto& state=*static_cast<TemplateReadState*>(user);++state.calls;
    if(state.failureMode&&state.calls==2){if(state.failureMode==3)throw std::runtime_error("injected template read failure");
        if(state.failureMode==2){std::memcpy(destination,source,bytes-1);return bytes-1;}return 0;}
    const auto offset=static_cast<const unsigned char*>(source)-state.base;require(offset==1||offset==34||offset==100||offset==137||offset==177,"template reads typed values only, never padding");
    require(bytes==sizeof(VkDescriptorImageInfo)||bytes==sizeof(VkDescriptorBufferInfo),"template reads exact ordinary typed element size");state.bytes+=bytes;std::memcpy(destination,source,bytes);return bytes;
}
void ordinaryTemplates(){++cases;Fixture f;TemplatePayload payload(f);auto info=payload.info(f.setLayout);const auto handle=h<VkDescriptorUpdateTemplate>(9001);
    const auto created=f.t.templateCreated(handle,&info,VK_SUCCESS);ok(created);TemplateReadState reader{payload.data.data()};
    // Caller metadata storage can disappear/change immediately after creation.
    payload.entries[0].dstBinding=999;allocations=allocationBytes=0;measure=true;
    ok(f.t.updateWithTemplate(f.sets[0],handle,payload.data.data(),TemplateUpdateKind::Core,{templateRead,&reader}));f.begin();f.bind();const auto e=f.dispatch();qualified(e);measure=false;
    require(!allocations&&reader.calls==5&&reader.bytes==120,"mixed image/buffer/sampler, unaligned padded stride decoding allocates nothing");
    require(e.bindings[0].image==Key{101,1}&&e.bindings[1].image==Key{102,2},"template arrays retain exact image identities and array elements");
    f.t.templateDestroying(handle);ok(f.t.validateEvidence(e));qualified(f.dispatch());require(f.t.stats().liveTemplates==0,"destroying template does not revoke published descriptors");
    payload.entries[0].dstBinding=2;payload.entries[0].stride=0;info=payload.info(f.setLayout);const auto reused=f.t.templateCreated(handle,&info,VK_SUCCESS);ok(reused);
    require(reused.key!=created.key,"template handle reuse gets a new generation without rewriting previous values");ok(f.t.validateEvidence(e));
    reader={payload.data.data()};ok(f.t.updateWithTemplate(f.sets[1],handle,payload.data.data(),TemplateUpdateKind::Khr,{templateRead,&reader}));f.bind(0,1);const auto zero=f.dispatch();qualified(zero);
    require(zero.bindings[0].image==zero.bindings[1].image,"legal zero stride repeats one typed value across descriptor array");
    const auto state=f.t.stats();require(state.ordinaryUpdateCalls==0&&state.coreTemplateUpdateCalls==1&&state.khrTemplateUpdateCalls==1&&!state.templateUpdateRefusals,"ordinary/core/KHR mutation diagnostics are separate exact counters");
    ok(f.t.updateWithTemplate(f.sets[0],handle,payload.data.data()));refused(f.dispatch(),Issue::DescriptorChanged);issue(f.t.validateEvidence(e),Issue::DescriptorChanged);
    f.begin();f.bind();qualified(f.dispatch());
}
void templateReadFailures(){++cases;
    for(unsigned mode=1;mode<=3;++mode){Fixture f;const auto before=f.ready();TemplatePayload payload(f);const auto info=payload.info(f.setLayout);const auto handle=h<VkDescriptorUpdateTemplate>(9001);ok(f.t.templateCreated(handle,&info,VK_SUCCESS));
        TemplateReadState reader{payload.data.data(),0,0,mode};const auto mutations=f.t.stats().mutations;
        issue(f.t.updateWithTemplate(f.sets[0],handle,payload.data.data(),TemplateUpdateKind::Khr,{templateRead,&reader}),Issue::TemplateReadFailed);
        require(reader.calls==2&&f.t.stats().mutations==mutations+1,"late failure performs no partial ordinary-write publication before poisoning destination once");
        refused(f.dispatch(),Issue::DescriptorChanged);issue(f.t.validateEvidence(before),Issue::DescriptorChanged);f.begin();f.bind();refused(f.dispatch(),Issue::TemplateReadFailed);
        const auto state=f.t.stats();require(state.templateUpdateRefusals==1&&state.templateReadRefusals==1&&state.templateRefusedIssueRecords[21]==1,"short/failed/throwing reads have a specific bounded diagnostic reason");}
}
struct PackedReadState {
    std::size_t calls{},bytes{},largest{};unsigned failure{};bool rpm{};unsigned spanFailure{};
};
std::size_t packedRead(void* user,const void* source,void* destination,std::size_t bytes){auto& state=*static_cast<PackedReadState*>(user);
    ++state.calls;state.bytes+=bytes;state.largest=std::max(state.largest,bytes);
    if(state.spanFailure&&bytes>sizeof(VkDescriptorImageInfo)){
        if(state.spanFailure==3)throw 3;
        if(state.spanFailure==2){std::memcpy(destination,source,bytes-1);return bytes-1;}
        return 0;
    }
    if(state.failure==3)throw 3;
    if(state.failure==1)return 0;
    if(state.failure==2){std::memcpy(destination,source,bytes-1);return bytes-1;}
    if(state.rpm){SIZE_T copied{};return ReadProcessMemory(GetCurrentProcess(),source,destination,bytes,&copied)?copied:0;}
    std::memcpy(destination,source,bytes);return bytes;
}
void packedTemplates(){++cases;
    for(unsigned failure=0;failure<=3;++failure){Fixture f;const auto before=f.ready();TemplatePayload payload(f);payload.pack();
        const auto info=payload.info(f.setLayout);const auto temp=h<VkDescriptorUpdateTemplate>(9700);ok(f.t.templateCreated(temp,&info,VK_SUCCESS));
        PackedReadState reader{0,0,0,failure};const auto mutations=f.t.stats().mutations;allocations=allocationBytes=0;measure=true;
        const auto result=f.t.updateWithTemplate(f.sets[0],temp,payload.data.data(),TemplateUpdateKind::Core,{packedRead,&reader});measure=false;
        require(!allocations,"contiguous template reads use bounded stack storage, without heap allocation");
        require(reader.calls==1&&reader.bytes==120,"adjacent mixed entries and descriptor arrays share one exact read");
        const auto stats=f.t.stats();require(stats.templateReadCalls==1&&stats.templateReadBytes==120&&stats.templateReadElements==5,
            "bulk read counters retain exact attempted calls, requested bytes and represented element counts");
        if(failure){issue(result,Issue::TemplateReadFailed);require(f.t.stats().mutations==mutations+1,"failed bulk read poisons once and publishes no partial writes");
            issue(f.t.validateEvidence(before),Issue::DescriptorChanged);f.begin();f.bind();refused(f.dispatch(),Issue::TemplateReadFailed);}
        else {ok(result);f.begin();f.bind();const auto value=f.dispatch();qualified(value);
            require(value.bindings[0].image==Key{101,1}&&value.bindings[1].image==Key{102,2},"bulk decoding retains exact image and array identities");}
    }
    Fixture f;TemplatePayload payload(f);payload.pack();std::array<VkDescriptorUpdateTemplateEntry,2> entries{{payload.entries[0],payload.entries[1]}};
    std::swap(entries[0],entries[1]);auto info=payload.info(f.setLayout);info.descriptorUpdateEntryCount=entries.size();info.pDescriptorUpdateEntries=entries.data();
    const auto temp=h<VkDescriptorUpdateTemplate>(9701);ok(f.t.templateCreated(temp,&info,VK_SUCCESS));PackedReadState reader;
    ok(f.t.updateWithTemplate(f.sets[0],temp,payload.data.data(),TemplateUpdateKind::Khr,{packedRead,&reader}));
    require(reader.calls==2&&reader.bytes==72&&reader.largest==48,"backward entry order splits runs without reading any gap");
    f.begin();f.bind();qualified(f.dispatch());
    Fixture guarded;TemplatePayload packed(guarded);packed.pack();const auto guardedInfo=packed.info(guarded.setLayout);
    ok(guarded.t.templateCreated(temp,&guardedInfo,VK_SUCCESS));
    auto* pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));require(pages!=nullptr,"guard-page fixture allocation");
    DWORD oldProtect{};require(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&oldProtect)!=0,"template boundary guard page");
    auto* boundary=pages+4096-120;std::memcpy(boundary,packed.data.data(),120);PackedReadState guardedReader;guardedReader.rpm=true;
    const auto result=guarded.t.updateWithTemplate(guarded.sets[0],temp,boundary,TemplateUpdateKind::Core,{packedRead,&guardedReader});
    require(VirtualFree(pages,0,MEM_RELEASE)!=0,"guard-page fixture released");ok(result);
    require(guardedReader.calls==1&&guardedReader.bytes==120,"real RPM stops at the exact final element, without reading into the guard page");
    guarded.begin();guarded.bind();qualified(guarded.dispatch());
}
void paddedTemplates(){++cases;
    for(unsigned mode=0;mode<=6;++mode){Fixture f;const auto before=f.ready();TemplatePayload payload(f);
        if(mode==4)std::reverse(payload.entries.begin(),payload.entries.end());
        if(mode==5)payload.entries[0].stride=0;
        const auto info=payload.info(f.setLayout);const auto temp=h<VkDescriptorUpdateTemplate>(9800);ok(f.t.templateCreated(temp,&info,VK_SUCCESS));
        PackedReadState reader;reader.spanFailure=mode<=3?mode:0;reader.failure=mode==6?1:0;
        const auto mutations=f.t.stats().mutations;allocations=allocationBytes=0;measure=true;
        const auto result=f.t.updateWithTemplate(f.sets[0],temp,payload.data.data(),TemplateUpdateKind::Khr,{packedRead,&reader,true});measure=false;
        const auto stats=f.t.stats();require(!allocations&&stats.templateSpanReadCalls==1,"bounded span probes allocate nothing and retain explicit attempt counts");
        if(mode==6){issue(result,Issue::TemplateReadFailed);require(reader.calls==2&&stats.templateReadRefusals==1&&stats.mutations==mutations+1,
            "a failed span followed by a failed typed read poisons once without partial publication");issue(f.t.validateEvidence(before),Issue::DescriptorChanged);continue;}
        ok(result);f.begin();f.bind();const auto e=f.dispatch();qualified(e);
        require(e.bindings[0].image==Key{101,1}&&e.bindings[1].image==(mode==5?Key{101,1}:Key{102,2}),"padded, reversed and zero-stride spans decode exact typed offsets");
        require(reader.calls==(mode>=1&&mode<=3?6u:1u)&&stats.templateSpanReadFallbacks==(mode>=1&&mode<=3?1u:0u)&&!stats.templateReadRefusals,
            "unreadable, short and throwing span probes fall back to original typed reads without a false refusal");
        if(!mode)require(reader.bytes==200&&stats.templateReadElements==5,"only the bounded first-to-last referenced span is read");
    }
    Fixture sparse;TemplatePayload payload(sparse);payload.entries[0].stride=20000;
    std::vector<unsigned char> large(20032);std::memcpy(large.data(),payload.data.data(),payload.data.size());
    std::memcpy(large.data()+20001,payload.data.data()+34,sizeof(VkDescriptorImageInfo));
    auto info=payload.info(sparse.setLayout);const auto temp=h<VkDescriptorUpdateTemplate>(9801);ok(sparse.t.templateCreated(temp,&info,VK_SUCCESS));
    PackedReadState reader;reader.rpm=true;ok(sparse.t.updateWithTemplate(sparse.sets[0],temp,large.data(),TemplateUpdateKind::Core,{packedRead,&reader,true}));
    require(reader.calls==5&&!sparse.t.stats().templateSpanReadCalls&&sparse.t.stats().templateSpanReadOversize==1&&sparse.t.stats().templateSpanMaxBytes==20024,
        "spans larger than the existing scratch cap use exact reads and report their extent, never a larger buffer");
    sparse.begin();sparse.bind();qualified(sparse.dispatch());

    Fixture gap;TemplatePayload gapped(gap);gapped.entries[0].stride=8192;info=gapped.info(gap.setLayout);ok(gap.t.templateCreated(temp,&info,VK_SUCCESS));
    auto* pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,12288,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));require(pages!=nullptr,"sparse protected-page fixture allocation");
    std::memcpy(pages,gapped.data.data(),gapped.data.size());std::memcpy(pages+8193,gapped.data.data()+34,sizeof(VkDescriptorImageInfo));
    DWORD protection{};require(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&protection)!=0,"sparse span contains an unreadable gap");
    reader={};reader.rpm=true;const auto result=gap.t.updateWithTemplate(gap.sets[0],temp,pages,TemplateUpdateKind::Core,{packedRead,&reader,true});
    require(VirtualFree(pages,0,MEM_RELEASE)!=0,"sparse protected-page fixture released");ok(result);
    require(reader.calls==6&&gap.t.stats().templateSpanReadFallbacks==1&&!gap.t.stats().templateReadRefusals,
        "real RPM failure across an unreadable gap preserves valid descriptors through exact-read fallback");
    gap.begin();gap.bind();qualified(gap.dispatch());
}
void templateBenchmark(bool padded=false){Fixture f;TemplatePayload payload(f);if(!padded)payload.pack();const auto info=payload.info(f.setLayout);
    const auto temp=h<VkDescriptorUpdateTemplate>(9702);ok(f.t.templateCreated(temp,&info,VK_SUCCESS));PackedReadState reader;reader.rpm=true;
    for(unsigned i=0;i<256;++i)ok(f.t.updateWithTemplate(f.sets[0],temp,payload.data.data(),TemplateUpdateKind::Core,{packedRead,&reader,true}));
    reader.calls=reader.bytes=reader.largest=0;constexpr unsigned iterations=32768;allocations=allocationBytes=0;measure=true;
    const auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<iterations;++i)if(!f.t.updateWithTemplate(f.sets[0],temp,payload.data.data(),TemplateUpdateKind::Core,{packedRead,&reader,true}).accepted())throw std::runtime_error("benchmark update refused");
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();measure=false;
    f.begin();f.bind();qualified(f.dispatch());require(!allocations,"benchmark updates allocate no heap");
    std::cout<<"{\"passed\":true,\"iterations\":"<<iterations<<",\"nanoseconds\":"<<elapsed<<",\"readCalls\":"<<reader.calls
        <<",\"readBytes\":"<<reader.bytes<<",\"largestRead\":"<<reader.largest<<",\"hotPathAllocations\":"<<allocations
        <<",\"storageBytes\":"<<Tracker::allocatedStorageBytes()<<",\"realReadProcessMemory\":true,\"gpuTouched\":false,\"gameTouched\":false}\n";
}
void badTemplateStructures(){++cases;
    for(unsigned mode=0;mode<10;++mode){Fixture f;TemplatePayload payload(f);auto info=payload.info(f.setLayout);VkBaseInStructure extension{};
        const Issue expected[]{Issue::UnsupportedDescriptor,Issue::UnsupportedExtension,Issue::CrossBinding,Issue::UnsupportedDescriptor,Issue::Capacity,Issue::Capacity,Issue::InvalidInput,Issue::InvalidInput,Issue::Capacity,Issue::InvalidInput};
        if(mode==0)info.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS;if(mode==1)info.pNext=&extension;
        if(mode==2)payload.entries[3].descriptorCount=2;if(mode==3)payload.entries[3].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        if(mode==4)info.descriptorUpdateEntryCount=maxTemplateEntries+1;if(mode==5)payload.entries[0].descriptorCount=maxTemplateElements+1;
        if(mode==6)payload.entries[0].offset=SIZE_MAX;if(mode==7)payload.entries[0].stride=SIZE_MAX;
        if(mode==8)payload.entries[0].offset=maxTemplateSpan;if(mode==9)info.pDescriptorUpdateEntries=nullptr;
        const auto handle=h<VkDescriptorUpdateTemplate>(9001);issue(f.t.templateCreated(handle,&info,VK_SUCCESS),expected[mode]);TemplateReadState reader{payload.data.data()};
        issue(f.t.updateWithTemplate(f.sets[0],handle,payload.data.data(),TemplateUpdateKind::Core,{templateRead,&reader}),expected[mode]);require(!reader.calls,"all template entries are structurally preflighted before any data read");
        f.begin();f.bind();refused(f.dispatch(),expected[mode]);}
}
void templateIdentitiesAndAddresses(){++cases;
    for(unsigned mode=0;mode<5;++mode){Fixture f;TemplatePayload payload(f);auto info=payload.info(f.setLayout);const auto handle=h<VkDescriptorUpdateTemplate>(9001);ok(f.t.templateCreated(handle,&info,VK_SUCCESS));Issue expected=Issue::UnknownObject;
        if(mode==0)f.t.templateDestroying(handle);
        if(mode==1){const auto li=f.layoutInfo();const auto other=h<VkDescriptorSetLayout>(2500);ok(f.t.setLayoutCreated(other,&li,VK_SUCCESS));f.t.templateDestroying(handle);info.descriptorSetLayout=other;ok(f.t.templateCreated(handle,&info,VK_SUCCESS));expected=Issue::IncompatibleLayout;}
        if(mode==2){f.t.setLayoutDestroying(f.setLayout);const auto li=f.layoutInfo();ok(f.t.setLayoutCreated(f.setLayout,&li,VK_SUCCESS));expected=Issue::StaleGeneration;}
        if(mode==3){issue(f.t.templateCreated(handle,&info,VK_SUCCESS),Issue::Lifecycle);expected=Issue::Lifecycle;}
        if(mode==4)expected=Issue::InvalidInput;TemplateReadState reader{payload.data.data()};const void* data=mode==4?reinterpret_cast<const void*>(UINTPTR_MAX-7):payload.data.data();
        issue(f.t.updateWithTemplate(f.sets[0],handle,data,TemplateUpdateKind::Core,{templateRead,&reader}),expected);require(!reader.calls,"missing/stale/incompatible/overflow template source is never dereferenced");}
    Fixture f;TemplatePayload p(f);const auto info=p.info(f.setLayout);const auto h1=h<VkDescriptorUpdateTemplate>(9001);issue(f.t.templateCreated(h1,&info,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    require(!f.t.stats().liveTemplates,"failed driver create publishes no template");ok(f.t.templateCreated(h1,&info,VK_SUCCESS));issue(f.t.updateWithTemplate(f.sets[0],h1,nullptr),Issue::InvalidInput);
}
void templateTypedBounds(){++cases;Fixture f;std::array<VkDescriptorSetLayoutBinding,maxTemplateEntries> bindings{};
    std::array<VkDescriptorUpdateTemplateEntry,maxTemplateEntries> entries{};std::array<VkDescriptorBufferInfo,maxTemplateElements> data{};
    for(std::size_t i=0;i<bindings.size();++i){bindings[i]={static_cast<std::uint32_t>(i),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,4,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        entries[i]={static_cast<std::uint32_t>(i),0,4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,i*4*sizeof(VkDescriptorBufferInfo),sizeof(VkDescriptorBufferInfo)};}
    for(auto& value:data)value={h<VkBuffer>(123),0,64};const auto layout=h<VkDescriptorSetLayout>(2600);
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=bindings.size();li.pBindings=bindings.data();ok(f.t.setLayoutCreated(layout,&li,VK_SUCCESS));
    const auto set=h<VkDescriptorSet>(5600);VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=f.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;ok(f.t.setsAllocated(&ai,&set,VK_SUCCESS));
    VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};ti.descriptorSetLayout=layout;ti.descriptorUpdateEntryCount=entries.size();ti.pDescriptorUpdateEntries=entries.data();const auto temp=h<VkDescriptorUpdateTemplate>(9600);ok(f.t.templateCreated(temp,&ti,VK_SUCCESS));
    PackedReadState reader;allocations=allocationBytes=0;measure=true;ok(f.t.updateWithTemplate(set,temp,data.data(),TemplateUpdateKind::Core,{packedRead,&reader}));measure=false;
    require(!allocations&&reader.calls==1&&reader.bytes==sizeof(data),"64 entries and 256 adjacent typed elements share one bounded, allocation-free read");
    Fixture g(small(),false);g.bindings[3].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;g.objects();TemplatePayload payload(g);payload.entries[3].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    const auto view=h<VkBufferView>(1234);std::memcpy(payload.data.data()+177,&view,sizeof(view));const auto info=payload.info(g.setLayout);const auto handle=h<VkDescriptorUpdateTemplate>(9601);ok(g.t.templateCreated(handle,&info,VK_SUCCESS));
    ok(g.t.updateWithTemplate(g.sets[0],handle,payload.data.data()));g.begin();g.bind();qualified(g.dispatch());
    g.t.poolReset(g.pool,VK_SUCCESS);g.allocate();ok(g.t.updateWithTemplate(g.sets[1],handle,payload.data.data()));g.bind(0,1);refused(g.dispatch(),Issue::StaleGeneration);
    g.begin();g.bind(0,1);qualified(g.dispatch());
}
void fixedHashChurn(){++cases;Tracker t(small());
    for(std::uint64_t i=1;i<=4000;++i){xess_vk::image_inventory::Record record{(i%13)+1,i,{83,16,16,1,1,1,1,4,0,0,0,0}};
        ok(t.imageObserved(record));t.imageRetiring({record.image,record.generation});}
    require(t.stats().liveImages==0&&t.stats().objects[0].highWater==1,"fixed hash deletion/free-chain survives handle churn");
}

Config selectedConfig(){auto c=small();c.continuousDispatchSelection=true;return c;}
void equivalentCandidate(const DispatchEvidence& a,const DispatchEvidence& b){
    require(!a.filteredOut&&!b.filteredOut&&a.command==b.command&&a.pipeline==b.pipeline&&a.pipelineLayout==b.pipelineLayout&&
        a.recordOrdinal==b.recordOrdinal&&a.groups==b.groups&&a.issues==b.issues&&a.metadataAccepted==b.metadataAccepted&&
        a.count==b.count&&a.ignoredNonImageBindingCount==b.ignoredNonImageBindingCount,"selected candidate has identical full-path identity, ordinal and refusal");
    for(std::size_t i=0;i<a.count;++i){const auto& x=a.bindings[i];const auto& y=b.bindings[i];
        require(x.setIndex==y.setIndex&&x.binding==y.binding&&x.arrayElement==y.arrayElement&&x.descriptorType==y.descriptorType&&
            x.set==y.set&&x.setLayout==y.setLayout&&x.pool==y.pool&&x.view==y.view&&x.image==y.image&&x.descriptorVersion==y.descriptorVersion&&
            x.viewType==y.viewType&&x.viewFormat==y.viewFormat&&x.declaredLayout==y.declaredLayout,"candidate binding keys, versions and layouts identical");
        require(x.components.r==y.components.r&&x.components.g==y.components.g&&x.components.b==y.components.b&&x.components.a==y.components.a&&
            x.range.aspectMask==y.range.aspectMask&&x.range.baseMipLevel==y.range.baseMipLevel&&x.range.levelCount==y.range.levelCount&&
            x.range.baseArrayLayer==y.range.baseArrayLayer&&x.range.layerCount==y.range.layerCount,"candidate view components and ranges identical");
        const auto& u=x.imageDescription;const auto& v=y.imageDescription;
        require(u.format==v.format&&u.width==v.width&&u.height==v.height&&u.depth==v.depth&&u.mipLevels==v.mipLevels&&u.arrayLayers==v.arrayLayers&&
            u.samples==v.samples&&u.usage==v.usage&&u.flags==v.flags&&u.tiling==v.tiling&&u.sharingMode==v.sharingMode&&u.initialLayout==v.initialLayout,
            "candidate image description identical");
    }
}
struct CandidateObjects {
    std::array<VkDescriptorSetLayout,2> layouts{h<VkDescriptorSetLayout>(2101),h<VkDescriptorSetLayout>(2102)};
    std::array<VkDescriptorSet,2> sets{h<VkDescriptorSet>(5101),h<VkDescriptorSet>(5102)};
    VkPipelineLayout layout=h<VkPipelineLayout>(3101);VkPipeline pipeline=h<VkPipeline>(8101);
    VkDescriptorUpdateTemplate update=h<VkDescriptorUpdateTemplate>(9101);
};
void populateCandidate(Fixture& f,const CandidateObjects& c){
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=f.pool;ai.descriptorSetCount=2;ai.pSetLayouts=c.layouts.data();
    ok(f.t.setsAllocated(&ai,c.sets.data(),VK_SUCCESS));
    const std::array<VkDescriptorImageInfo,2> data{{{VK_NULL_HANDLE,f.views[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,f.views[1],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
    ok(f.t.updateWithTemplate(c.sets[0],c.update,data.data()));
    const VkDescriptorBufferInfo buffer{h<VkBuffer>(123),0,64};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=c.sets[1];write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&buffer;
    ok(f.t.updateDescriptors(1,&write,0,nullptr));
    VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};copy.srcSet=f.sets[0];copy.srcBinding=2;copy.srcArrayElement=1;
    copy.dstSet=c.sets[0];copy.dstBinding=5;copy.descriptorCount=1;ok(f.t.updateDescriptors(0,nullptr,1,&copy));
}
CandidateObjects candidateObjects(Fixture& f){
    CandidateObjects c;
    // Stage flags are deliberately not a new selection requirement of the old consumer.
    const std::array<VkDescriptorSetLayoutBinding,2> images{{{5,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{7,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=images.size();li.pBindings=images.data();ok(f.t.setLayoutCreated(c.layouts[0],&li,VK_SUCCESS));
    const VkDescriptorSetLayoutBinding buffer{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};li.bindingCount=1;li.pBindings=&buffer;ok(f.t.setLayoutCreated(c.layouts[1],&li,VK_SUCCESS));
    const std::array<VkDescriptorUpdateTemplateEntry,2> entries{{{5,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,0,sizeof(VkDescriptorImageInfo)},{7,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,sizeof(VkDescriptorImageInfo),sizeof(VkDescriptorImageInfo)}}};
    VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};ti.descriptorSetLayout=c.layouts[0];ti.descriptorUpdateEntryCount=entries.size();ti.pDescriptorUpdateEntries=entries.data();ok(f.t.templateCreated(c.update,&ti,VK_SUCCESS));
    populateCandidate(f,c); // Early owned writes, companion and external copy all precede the pipeline.
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=2;pi.pSetLayouts=c.layouts.data();ok(f.t.pipelineLayoutCreated(c.layout,&pi,VK_SUCCESS));
    auto pipeline=f.pipelineInfo();pipeline.layout=c.layout;ok(f.t.pipelineCreated(c.pipeline,&pipeline,VK_SUCCESS));return c;
}
void bindCandidate(Fixture& f,const CandidateObjects& c){
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,c.layout,0,2,c.sets.data(),0,nullptr);
    f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,c.pipeline);
}
void computeRestoreRange(){++cases;Fixture f;ok(f.write(f.sets[0]));const auto c=candidateObjects(f);
    f.begin();bindCandidate(f,c);const auto full=f.dispatch();qualified(full,2);
    require(full.computeRestoreKnown&&full.computeRestoreSetCount==2&&full.computeRestoreSets[0].handle==reinterpret_cast<std::uint64_t>(c.sets[0])&&
        full.computeRestoreSets[1].handle==reinterpret_cast<std::uint64_t>(c.sets[1])&&full.computeRestoreSets[1].generation,
        "compute restoration includes the exact higher opaque buffer set and both generations");
    f.bind();const auto lower=f.dispatch();qualified(lower);
    require(lower.computeRestoreKnown&&lower.computeRestoreSetCount==1,
        "an incompatible set-zero rebind actually disturbs the former higher driver sets");
    bindCandidate(f,c);const auto rebound=f.dispatch();qualified(rebound,2);
    require(rebound.computeRestoreKnown&&rebound.computeRestoreSetCount==2,"full same-layout rebind recovers every observed set slot");
    f.begin();f.bind();const auto reset=f.dispatch();qualified(reset);
    require(reset.computeRestoreKnown&&reset.computeRestoreSetCount==1,"new recording resets the full bound-set range");
    f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipelineLayout,maxBoundSets,1,f.sets.data(),0,nullptr);
    f.bind();const auto unknown=f.dispatch();qualified(unknown);
    require(!unknown.computeRestoreKnown,"an untracked higher set range never enables shader insertion after a metadata recovery");
}
void computeRestoreDisturbance(){++cases;
    for(unsigned mode=0;mode<5;++mode){Fixture f;ok(f.write(f.sets[0]));const auto c=candidateObjects(f);
        const auto wider=h<VkPipelineLayout>(3199);
        std::array<VkDescriptorSetLayout,3> layouts{f.setLayout,f.setLayout,f.setLayout};
        std::array<VkDescriptorSet,3> sets{f.sets[0],f.sets[0],f.sets[0]};
        if(mode){layouts={c.layouts[0],c.layouts[1],c.layouts[1]};sets={c.sets[0],c.sets[1],c.sets[1]};}
        if(mode==2){
            // Different handles with identical definitions are still compatible.
            layouts[0]=h<VkDescriptorSetLayout>(2199);
            const std::array<VkDescriptorSetLayoutBinding,2> bindings{{{5,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{7,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}}};
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=bindings.size();li.pBindings=bindings.data();ok(f.t.setLayoutCreated(layouts[0],&li,VK_SUCCESS));
        }
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=layouts.size();pi.pSetLayouts=layouts.data();ok(f.t.pipelineLayoutCreated(wider,&pi,VK_SUCCESS));
        f.begin();f.t.bindSets(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,wider,0,sets.size(),sets.data(),0,nullptr);
        if(mode>=3)f.t.pipelineLayoutDestroying(wider);
        if(mode==4){layouts={f.setLayout,f.setLayout,f.setLayout};ok(f.t.pipelineLayoutCreated(wider,&pi,VK_SUCCESS));}
        bindCandidate(f,c);const auto e=f.dispatch();qualified(e,2);
        require(e.computeRestoreKnown==(mode==0),"only a proven incompatible rebind discards historical higher slots; compatible, identical-definition and unknown layouts retain refusal");
        require(f.t.stats().restoreRangeDisturbances==(mode==0?1u:0u),"range truncation requires an exact live layout generation witness");
        if(mode==0)require(e.computeRestoreSetCount==2,"three-to-two FSR transition restores exactly its two still-valid descriptor sets");
    }
}
void continuousDispatchSelection(){++cases;
    {Fixture selected(selectedConfig());ok(selected.write(selected.sets[0]));selected.begin();selected.bind();
        require(selected.dispatch().filteredOut,"default product selection remains active");
        const auto observed=selected.t.preDispatch(selected.commands[0],1,1,1,DispatchKind::Direct,true);qualified(observed);
        require(!observed.filteredOut&&observed.diagnosticSelectionBypassed&&observed.count,"bounded diagnostic explicitly obtains ordinary bindings without promoting the product route");
        require(selected.dispatch().filteredOut,"next unrequested call immediately restores product selection");}
    for(unsigned mode=0;mode<7;++mode){Fixture full,selected(selectedConfig());
        auto each=[&](auto action){action(full);action(selected);};
        each([](Fixture& f){ok(f.write(f.sets[0]));ok(f.write(f.sets[1]));});
        const auto c=candidateObjects(full);(void)candidateObjects(selected);
        each([](Fixture& f){f.begin();f.bind();});
        const auto ordinary=full.dispatch(),filtered=selected.dispatch();qualified(ordinary);
        require(filtered.filteredOut&&!filtered.metadataAccepted&&!filtered.issues&&!filtered.count&&filtered.recordOrdinal==ordinary.recordOrdinal,
            "known ordinary Direct dispatch is filtered, not accepted or refused");
        each([&](Fixture& f){
            if(mode==1||mode==4)ok(f.write(f.sets[0],f.views[1]));
            if(mode==2||mode==5)f.t.setsFreed(f.pool,1,f.sets.data(),VK_SUCCESS);
            if(mode==3){f.t.poolReset(f.pool,VK_SUCCESS);f.allocate();ok(f.write(f.sets[0]));populateCandidate(f,c);}
            if(mode==4){f.t.reset(f.commands[0],VK_SUCCESS);f.begin();}
            if(mode==5){f.t.commandPoolReset(f.commandPool,VK_SUCCESS);f.begin();}
            if(mode==6){const auto unknown=f.t.preDispatch(f.commands[0],1,1,1,DispatchKind::Unknown);require(!unknown.filteredOut,"Unknown dispatch retains sticky original handling");}
            bindCandidate(f,c);
        });
        const auto a=full.dispatch(),b=selected.dispatch();equivalentCandidate(a,b);
        if(mode==0||mode==4||mode==5){qualified(a,2);require(a.ignoredNonImageBindingCount==1&&a.bindings[0].image==Key{102,2},"candidate retains buffer companion and early external copy");}
        else {const auto expected=mode==1?Issue::DescriptorChanged:mode==2?Issue::UnknownObject:mode==3?Issue::StaleGeneration:Issue::UnsupportedDispatch;refused(b,expected);}
        if(mode==0){
            each([](Fixture& f){f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);});
            (void)full.dispatch();require(selected.dispatch().filteredOut,"switch to ordinary pipeline filters without discarding candidate sets");
            each([&](Fixture& f){f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,c.pipeline);});
            equivalentCandidate(full.dispatch(),selected.dispatch());
        }
    }
    for(unsigned mode=0;mode<7;++mode){Fixture full,selected(selectedConfig());
        auto each=[&](auto action){action(full);action(selected);};each([](Fixture& f){ok(f.write(f.sets[0]));f.begin();f.bind();});
        each([&](Fixture& f){
            if(mode==0){f.t.pipelineDestroying(f.pipeline);auto pi=f.pipelineInfo();ok(f.t.pipelineCreated(f.pipeline,&pi,VK_SUCCESS));}
            if(mode==1){f.t.pipelineLayoutDestroying(f.pipelineLayout);auto pi=f.pipelineLayoutInfo();ok(f.t.pipelineLayoutCreated(f.pipelineLayout,&pi,VK_SUCCESS));}
            if(mode==2){f.t.setLayoutDestroying(f.setLayout);auto li=f.layoutInfo();ok(f.t.setLayoutCreated(f.setLayout,&li,VK_SUCCESS));}
            if(mode==3)f.t.commandUnsupported(f.commands[0]);
            if(mode==4)f.t.stop(Issue::Capacity);
            if(mode==5)f.t.commandPoolDestroying(f.commandPool);
            if(mode==6){f.t.pipelineDestroying(f.pipeline);auto pi=f.pipelineInfo();pi.flags=VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;issue(f.t.pipelineCreated(f.pipeline,&pi,VK_SUCCESS),Issue::UnsupportedExtension);f.t.bindPipeline(f.commands[0],VK_PIPELINE_BIND_POINT_COMPUTE,f.pipeline);}
        });
        const auto a=full.dispatch(),b=selected.dispatch();equivalentCandidate(a,b);require(!b.metadataAccepted,"unknown/stale/unsupported/fatal owner chain never filters around refusal");
    }
    for(auto kind:{DispatchKind::Indirect,DispatchKind::Base}){Fixture full,selected(selectedConfig());(void)full.ready();ok(selected.write(selected.sets[0]));selected.begin();selected.bind();(void)selected.dispatch();
        equivalentCandidate(full.t.preDispatch(full.commands[0],1,1,1,kind),selected.t.preDispatch(selected.commands[0],1,1,1,kind));}
}
Config compactConfig(){auto c=selectedConfig();c.compactOrdinaryDispatch=true;return c;}
void compactOrdinaryEvidence(){++cases;
    for(unsigned mode=0;mode<10;++mode){Fixture full(selectedConfig()),compact(compactConfig());
        auto each=[&](auto action){action(full);action(compact);};
        each([](Fixture& f){ok(f.write(f.sets[0]));f.begin();f.bind();});
        const auto a=full.t.preDispatch(full.commands[0],8,4,1,DispatchKind::Direct,true);
        const auto b=compact.t.preDispatch(compact.commands[0],8,4,1,DispatchKind::Direct,true);
        equivalentCandidate(a,b);qualified(b);
        require(!b.membershipOnly,"unclassified/missing shader metadata retains the full path");
        require(!compact.t.allowCompactPipeline({b.pipeline.handle,b.pipeline.generation+1}),"stale classification cannot affect a live generation");
        require(compact.t.allowCompactPipeline(b.pipeline),"exact archived ordinary pipeline can opt into compact evidence");
        each([&](Fixture& f){
            if(mode==1)f.t.imageRetiring({101,1});
            if(mode==2)f.t.viewDestroying(f.views[0]);
            if(mode==3)ok(f.write(f.sets[0],f.views[1]));
            if(mode==4)f.t.setsFreed(f.pool,1,f.sets.data(),VK_SUCCESS);
            if(mode==5)f.t.poolReset(f.pool,VK_SUCCESS);
            if(mode==6){f.t.setLayoutDestroying(f.setLayout);auto li=f.layoutInfo();ok(f.t.setLayoutCreated(f.setLayout,&li,VK_SUCCESS));}
            if(mode==7)f.t.commandUnsupported(f.commands[0]);
            if(mode==8){f.t.setUnsupported(f.sets[0]);}
            if(mode==9)f.t.stop(Issue::Capacity);
        });
        const auto x=full.t.preDispatch(full.commands[0],8,4,1,DispatchKind::Direct,true);
        const auto y=compact.t.preDispatch(compact.commands[0],8,4,1,DispatchKind::Direct,true);
        require(x.guardImagesKnown==y.guardImagesKnown&&x.guardImageCount==y.guardImageCount&&x.issues==y.issues,
            "ordinary guard completeness and refusals match full evidence after mutation, retirement, reuse and stop");
        for(std::size_t n=0;n<x.guardImageCount;++n)require(x.bindings[n].image==y.bindings[n].image,"all consumer identities remain exact");
        if(mode==0){
            require(y.membershipOnly&&y.filteredOut&&y.diagnosticSelectionBypassed&&!y.metadataAccepted&&!y.count&&
                y.guardImageCount==3&&y.guardImagesKnown&&!y.computeRestoreKnown&&!y.bindings[0].view.handle,
                "compact consumer identities never grant typed inputs, copied view payloads or SDK restoration");
            require(Tracker::matchingImage(y,{101,1})==0&&!compact.t.validateEvidence(y).accepted(),"compact membership cannot be reused as accepted input evidence");
            const auto stats=compact.t.stats();require(stats.compactDispatches==1&&!stats.compactUnknownDispatches&&stats.compactImageReferences==3,
                "compact work and completeness have separate counters");
        }
    }
    {Fixture full(selectedConfig()),compact(compactConfig());
        for(auto* f:{&full,&compact}){ok(f->write(f->sets[0]));f->begin();f->bind();}
        const auto key=compact.t.preDispatch(compact.commands[0],1,1,1,DispatchKind::Direct,true).pipeline;
        (void)full.t.preDispatch(full.commands[0],1,1,1,DispatchKind::Direct,true);
        require(compact.t.allowCompactPipeline(key),"ordinary classification before candidate switch");
        const auto cf=candidateObjects(full),cc=candidateObjects(compact);bindCandidate(full,cf);bindCandidate(compact,cc);
        const auto x=full.t.preDispatch(full.commands[0],8,4,1,DispatchKind::Direct,true);
        const auto y=compact.t.preDispatch(compact.commands[0],8,4,1,DispatchKind::Direct,true);equivalentCandidate(x,y);qualified(y,2);
        require(!y.membershipOnly&&y.computeRestoreKnown&&y.computeRestoreSetCount==2&&y.ignoredNonImageBindingCount==1,
            "FSR retains all sets, including the buffer-only companion, early template writes and external copies");
        compact.t.pipelineDestroying(compact.pipeline);auto pi=compact.pipelineInfo();ok(compact.t.pipelineCreated(compact.pipeline,&pi,VK_SUCCESS));
        compact.begin();compact.bind();const auto reused=compact.t.preDispatch(compact.commands[0],1,1,1,DispatchKind::Direct,true);
        require(!reused.membershipOnly&&reused.pipeline!=key&&!compact.t.allowCompactPipeline(key),"handle reuse loses the old compact permission");
    }
}
std::uint64_t compactFullNs{},compactSelectedNs{};
void compactBenchmark(){++cases;Fixture full(selectedConfig()),compact(compactConfig());
    for(auto* f:{&full,&compact}){ok(f->write(f->sets[0]));f->begin();f->bind();}
    const auto key=compact.t.preDispatch(compact.commands[0],8,4,1,DispatchKind::Direct,true).pipeline;
    require(compact.t.allowCompactPipeline(key),"benchmark exact pipeline classification");
    std::uint64_t fullImages{},compactImages{};allocations=allocationBytes=0;measure=true;
    const auto batch=[&](Fixture& f,bool lean){const auto start=std::chrono::steady_clock::now();
        for(unsigned n=0;n<2048;++n){const auto e=f.t.preDispatch(f.commands[0],8,4,1,DispatchKind::Direct,true);
            if(lean)compactImages+=e.guardImageCount;else fullImages+=e.count;}
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        if(lean)compactSelectedNs+=ns;else compactFullNs+=ns;
    };
    for(unsigned n=0;n<16;++n){if(n&1){batch(compact,true);batch(full,false);}else{batch(full,false);batch(compact,true);}}
    measure=false;require(fullImages==32768*3&&compactImages==fullImages&&!allocations,"compact/full interleaved benchmark preserves all identities without allocations");
}
std::uint64_t selectionFullNs{},selectionSelectedNs{};
void selectionBenchmark(){++cases;Fixture full,selected(selectedConfig());ok(full.write(full.sets[0]));ok(selected.write(selected.sets[0]));full.begin();full.bind();selected.begin();selected.bind();
    for(unsigned i=0;i<64;++i){(void)full.dispatch();(void)selected.dispatch();}
    std::uint64_t fullImages{},filtered{};allocations=allocationBytes=0;measure=true;
    auto batch=[&](Fixture& f,bool limited){const auto start=std::chrono::steady_clock::now();for(unsigned i=0;i<2048;++i){const auto e=f.dispatch();if(limited)filtered+=e.filteredOut;else fullImages+=e.count;}
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();if(limited)selectionSelectedNs+=elapsed;else selectionFullNs+=elapsed;};
    for(unsigned i=0;i<16;++i){if(i&1){batch(selected,true);batch(full,false);}else{batch(full,false);batch(selected,true);}}
    measure=false;require(fullImages==32768*3&&filtered==32768&&!allocations,"fixed interleaved full/selected workload keeps exact work and no hot allocations");
    require(selected.t.stats().filteredDispatches==32768+64&&!selected.t.stats().acceptedDispatches&&!selected.t.stats().refusedDispatches,"filtered counters are separate from metadata acceptance quality");
}
void referenceBindingCache(){++cases;Fixture f;ReferenceBindingCache cache,otherDevice;
    const std::array<Key,2> watched{{{101,1},{102,2}}};f.t.watchReferenceImages(watched);ok(f.write(f.sets[0]));
    const auto handle=static_cast<Handle>(reinterpret_cast<std::uintptr_t>(f.sets[0]));
    const auto epoch=f.t.referenceEpoch();auto token=f.t.referenceBinding(f.sets[0]);cache.remember(token);
    require(cache.find(handle,epoch).set==token.set&&!otherDevice.find(handle,epoch).published,"warm cache is isolated to its owning runtime/device");
    allocations=0;measure=true;for(unsigned n=0;n<10000;++n)(void)cache.find(handle,epoch);measure=false;
    require(!allocations,"cross-recording token hints do not allocate");
    ok(f.write(f.sets[0]));require(!cache.find(handle,epoch).published,"descriptor update invalidates cached publication before lazy rebuild");
    cache.remember(f.t.referenceBinding(f.sets[0]));require(cache.find(handle,epoch).published,"lazy query rewarms dirty token");
    f.t.watchReferenceImages({});require(!cache.find(handle,f.t.referenceEpoch()).published,"watch epoch change cannot accept an old mask");
    cache.remember(f.t.referenceBinding(f.sets[0]));f.t.setsFreed(f.pool,2,f.sets.data(),VK_SUCCESS);
    require(!cache.find(handle,f.t.referenceEpoch()).published,"free invalidates stable token");
    f.allocate();ok(f.write(f.sets[0]));const auto reused=f.t.referenceBinding(f.sets[0]);
    require(reused.set!=token.set&&!cache.find(handle,f.t.referenceEpoch()).published,"same handle reallocation rejects old generation even after valid publication");
    cache.remember(reused);f.t.poolReset(f.pool,VK_SUCCESS);require(!cache.find(handle,f.t.referenceEpoch()).published,"pool reset invalidates cached identities");
    ReferenceMask a,b;a.publish({{0x1000,1},1,1,0,true});b.publish({{0x1000,2},1,1,0,true});
    cache.remember({{0x1000,1},&a});cache.remember({{0x1000,2},&b});require(cache.find(0x1000,1).set.generation==2,"cache stores replacement generation");
    ReferenceMask collision;const auto hash=[](Handle h){return ((h>>4)^(h>>12)^(h>>20)^(h>>32))&255;};
    Handle other=0x1010;while(hash(other)!=hash(0x1000))other+=16;
    collision.publish({{other,3},1,1,0,true});cache.remember({{other,3},&collision});
    require(!cache.find(0x1000,1).published&&cache.find(other,1).published,"collisions only evict hints and never return another handle");
    cache.remember({{0x1000,2},&b});
    require(!cache.find(0,1).published&&!cache.find(0x1000,0).published,"null identities and epochs are misses");
    b.publish({});require(!cache.find(0x1000,1).published,"unknown metadata remains a miss, never an authorization");
}
void referenceConsumption(){++cases;auto config=small();config.measureReferenceConsumption=true;Fixture f(config);
    const std::array<Key,2> watched{{{101,1},{102,2}}};f.t.watchReferenceImages(watched);
    ok(f.write(f.sets[0]));auto token=f.t.referenceBinding(f.sets[0]);auto value=token.published->read();
    require(value.known&&f.t.stats().referenceMaskPendingBuilds==1,"published build remains pending until its lifetime ends");
    ok(f.write(f.sets[0]));require(f.t.stats().referenceMaskUnconsumedBuilds==1,"identity-only reads do not count as guard consumption");
    f.t.referenceBinding(f.sets[0]);
    value=token.published->read();token.published->consume(value);token.published->consume(value);
    ok(f.write(f.sets[0]));require(f.t.stats().referenceMaskConsumedBuilds==1,"repeated guard reads count one consumed publication");
    f.t.referenceBinding(f.sets[0]);const auto stale=token.published->read();ok(f.write(f.sets[0]));f.t.referenceBinding(f.sets[0]);token.published->consume(stale);
    auto before=f.t.stats();ok(f.write(f.sets[0]));require(f.t.stats().referenceMaskUnconsumedBuilds==before.referenceMaskUnconsumedBuilds+1,"late consumption of an old publication cannot mark its replacement");
    f.t.referenceBinding(f.sets[0]);
    value=token.published->read();allocations=0;measure=true;
    for(unsigned n=0;n<10000;++n)token.published->consume(value);
    measure=false;require(!allocations,"consumption instrumentation allocates nothing on the guard path");
    ok(f.write(f.sets[1]));f.t.viewDestroying(f.views[0]);require(f.t.stats().referenceMaskConsumedBuilds==2,"object invalidation finalizes the consumed build");
    f.t.poolReset(f.pool,VK_SUCCESS);
    const auto final=f.t.stats();require(!final.referenceMaskPendingBuilds&&final.referenceMaskRetiredBuilds==final.referenceMaskMeasuredBuilds&&
        final.referenceMaskRetiredBuilds==final.referenceMaskConsumedBuilds+final.referenceMaskUnconsumedBuilds,"pool reset closes every measured build with exact accounting");
}
void lazyReferenceMasks(){
    for(const bool retireImage:{false,true}){++cases;Fixture f;
        std::array<Key,2> watched{{{101,1},{102,2}}};f.t.watchReferenceImages(watched);
        ok(f.write(f.sets[0]));ok(f.write(f.sets[1]));
        require(!f.t.stats().referenceMaskBuilds&&!f.t.stats().referenceEdges,"unqueried writes do not build masks or lifetime edges");
        auto token=f.t.referenceBinding(f.sets[0]);const auto initial=token.published->read();
        require(initial.known&&initial.mask==3&&f.t.stats().referenceMaskBuilds==1,"first query lazily builds the complete current mask");
        ok(f.write(f.sets[0]));const auto dirty=token.published->read();
        require(!dirty.known&&dirty.set==initial.set&&dirty.version!=initial.version&&!f.t.stats().referenceEdges&&f.t.stats().referenceMaskBuilds==1,"a post-bind write immediately publishes unknown and removes stale edges");
        f.t.referenceBinding(f.sets[0]);require(token.published->read().known&&f.t.stats().referenceMaskBuilds==2,"the next bind refreshes a dirty mask once");
        const auto builds=f.t.stats().referenceMaskBuilds;f.t.referenceBinding(f.sets[0]);
        require(f.t.stats().referenceMaskBuilds==builds,"an unchanged queried set reuses its publication");
        allocations=0;measure=true;
        for(unsigned n=0;n<1000;++n){ok(f.write(f.sets[0]));const auto binding=f.t.referenceBinding(f.sets[0]);require(binding.published->read().known,"hot rewrite/rebind stays complete");}
        measure=false;require(!allocations,"warm lazy rewrites and queries allocate nothing");
        std::swap(watched[0],watched[1]);const auto epoch=f.t.watchReferenceImages(watched);
        require(token.published->read().epoch!=epoch,"epoch remapping makes an old token stale");f.t.referenceBinding(f.sets[0]);
        require(token.published->read().epoch==epoch&&token.published->read().known,"query rebuilds lazily after epoch remapping");
        if(retireImage)f.t.imageRetiring({101,1});else f.t.viewDestroying(f.views[0]);
        require(!token.published->read().known&&f.t.stats().referenceLifetimeInvalidations==1,"a lazily built set remains covered by exact image/view invalidation");
        const auto count=f.t.stats().referenceMaskBuilds;f.t.referenceBinding(f.sets[0]);
        require(!token.published->read().known&&f.t.stats().referenceMaskBuilds==count,"a computed invalid publication is not mistaken for an unwritten dirty mask");
        f.t.poolReset(f.pool,VK_SUCCESS);require(!f.t.stats().referenceEdges&&token.published->read().set!=initial.set,"pool reset retires lazy edges and the published generation");
    }
}
void referenceMasks(){++cases;Fixture f;ok(f.write(f.sets[0]));
    require(!f.t.stats().referenceMaskBuilds,"non-SR descriptor updates do not build unused reference masks");
    std::array<Key,2> watched{{{101,1},{102,2}}};require(f.t.watchReferenceImages(watched)!=0,"reference mapping configured");
    const auto token=f.t.referenceBinding(f.sets[0]);const auto first=token.published->read();
    require(first.known&&first.set==token.set&&first.mask==3&&first.epoch==f.t.referenceEpoch(),"per-set bitmap preserves both referenced image identities");
    const auto builds=f.t.stats().referenceMaskBuilds;ok(f.write(f.sets[1]));
    require(f.t.referenceEpoch()==first.epoch&&token.published->read().version==first.version,"unrelated writes do not invalidate a live set bitmap");
    require(f.t.stats().referenceMaskBuilds==builds,"an unqueried multi-entry update does not build a bitmap");
    allocations=0;measure=true;for(unsigned n=0;n<10000;++n){const auto value=token.published->read();if(!value.known||value.mask!=3){measure=false;require(false,"stable bitmap read changed without a publication");}}
    measure=false;require(!allocations,"hot per-set bitmap reads allocate no memory");
    std::swap(watched[0],watched[1]);f.t.watchReferenceImages(watched);
    require(token.published->read().epoch!=f.t.referenceEpoch(),"changed mapping invalidates old bit positions");
    const auto rebound=f.t.referenceBinding(f.sets[0]);require(rebound.published==token.published&&rebound.published->read().epoch==f.t.referenceEpoch(),"refresh preserves stable token address");
    f.t.viewDestroying(f.views[0]);require(token.published->read().epoch==f.t.referenceEpoch()&&!token.published->read().known,"view retirement invalidates its dependent publication without remapping every set");
    require(!f.t.referenceBinding(f.sets[0]).published->read().known,"retired views cannot publish a safe membership result");
    const auto old=token.set;f.t.setsFreed(f.pool,1,&f.sets[0],VK_SUCCESS);
    require(token.published->read().set!=old,"freed slot invalidates retained reader tokens");
    const VkDescriptorSetLayout layout=f.setLayout;VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=f.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;
    ok(f.t.setsAllocated(&ai,&f.sets[0],VK_SUCCESS));require(f.t.referenceBinding(f.sets[0]).set!=old,"handle reuse cannot authorize the old generation");
    std::array<Key,65> wide{};for(unsigned n=0;n<wide.size();++n)wide[n]={9000+n,1};
    require(!f.t.watchReferenceImages(wide)&&!f.t.stats().fatalIssues,"bitmap width overflow is an optimization fallback, not a tracking failure");
    Tracker empty(small());failNextAllocation=true;issue(empty.imageObserved(f.images[0]),Issue::Capacity);ok(empty.imageObserved(f.images[0]));
    require(empty.stats().objects[0].live==1,"failed image-page allocation remains retryable without dereferencing a null slot");
}
void referenceLifetimeScope(){++cases;
    for(unsigned mode=0;mode<9;++mode){Fixture f;
        const auto otherLayout=h<VkDescriptorSetLayout>(0xB100);const auto layoutInfo=f.layoutInfo();ok(f.t.setLayoutCreated(otherLayout,&layoutInfo,VK_SUCCESS));
        f.t.setsFreed(f.pool,1,&f.sets[1],VK_SUCCESS);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=f.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&otherLayout;
        ok(f.t.setsAllocated(&ai,&f.sets[1],VK_SUCCESS));
        const auto write=[&](VkDescriptorSet set,VkImageView view){
            std::array<VkDescriptorImageInfo,3> values;for(auto& value:values)value={VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};
            std::array<VkWriteDescriptorSet,2> writes{};for(auto& value:writes){value.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;value.dstSet=set;}
            writes[0].dstBinding=2;writes[0].descriptorCount=2;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;writes[0].pImageInfo=values.data();
            writes[1].dstBinding=5;writes[1].descriptorCount=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[1].pImageInfo=values.data()+2;
            ok(f.t.updateDescriptors(writes.size(),writes.data(),0,nullptr));
        };
        write(f.sets[0],f.views[0]);write(f.sets[1],f.views[1]);
        // Neither image is in the watched map: lifetime protection must still hold.
        const std::array<Key,1> watched{{{0xFFFF,9}}};const auto epoch=f.t.watchReferenceImages(watched);
        const auto first=f.t.referenceBinding(f.sets[0]),second=f.t.referenceBinding(f.sets[1]);
        require(first.published->read().known&&second.published->read().known,"disjoint sets publish valid empty masks");
        if(mode==0){
            allocations=0;measure=true;
            for(unsigned n=0;n<1000;++n){auto image=f.images[0];image.image=0xC100;image.generation=3+n;ok(f.t.imageObserved(image));
                auto view=f.viewInfo(0);view.image=h<VkImage>(image.image);const auto handle=h<VkImageView>(0xC200);ok(f.t.viewCreated(handle,&view,VK_SUCCESS));
                const auto layout=h<VkDescriptorSetLayout>(0xC300);ok(f.t.setLayoutCreated(layout,&layoutInfo,VK_SUCCESS));
                f.t.viewDestroying(handle);f.t.imageRetiring({image.image,image.generation});f.t.setLayoutDestroying(layout);
                require(f.t.referenceBinding(f.sets[0]).published->read().known&&f.t.referenceBinding(f.sets[1]).published->read().known,"unrelated streaming lifetimes preserve both cached publications");
            }
            measure=false;const auto stats=f.t.stats();require(!allocations&&f.t.referenceEpoch()==epoch&&stats.referenceMaskBuilds==2&&!stats.referenceLifetimeScans&&!stats.referenceLifetimeInvalidations,
                "1000 unrelated image/view/layout retirements need no allocation, epoch change, mask rebuild or dependent scan");continue;
        }
        if(mode==1)f.t.viewDestroying(f.views[0]);
        if(mode==2)f.t.imageRetiring({101,1});
        if(mode==3)f.t.setLayoutDestroying(f.setLayout);
        if(mode==4){const auto info=f.viewInfo(0);issue(f.t.viewCreated(f.views[0],&info,VK_SUCCESS),Issue::Lifecycle);}
        if(mode==5){auto image=f.images[0];++image.description.width;issue(f.t.imageObserved(image),Issue::StaleGeneration);}
        if(mode==6)issue(f.t.setLayoutCreated(f.setLayout,&layoutInfo,VK_SUCCESS),Issue::Lifecycle);
        if(mode==7){const auto info=f.poolInfo();issue(f.t.poolCreated(f.pool,&info,VK_SUCCESS),Issue::Lifecycle);}
        if(mode==8){write(f.sets[0],f.views[1]);f.t.referenceBinding(f.sets[0]);f.t.viewDestroying(f.views[0]);}
        require(f.t.referenceEpoch()==epoch,"object lifetime changes never renumber unrelated reference masks");
        require(first.published->read().known==(mode==8)&&second.published->read().known==(mode!=7),
            "retirement/duplicate lifetime invalidates exact dependent sets only; historical bucket bits cannot invalidate a rewritten set");
        require(f.t.stats().referenceLifetimeInvalidations==(mode==8?0u:mode==7?2u:1u),"scoped invalidation counts actual affected publications");
    }
}
void referenceLifetimeSetReuse(){++cases;Config config=small();config.sets=64;Fixture f(config);
    const auto pool=h<VkDescriptorPool>(0xD100);auto pi=f.poolInfo();pi.maxSets=64;ok(f.t.poolCreated(pool,&pi,VK_SUCCESS));
    const auto otherLayout=h<VkDescriptorSetLayout>(0xD200);const auto li=f.layoutInfo();ok(f.t.setLayoutCreated(otherLayout,&li,VK_SUCCESS));
    std::array<VkDescriptorSet,32> sets;std::array<VkDescriptorSetLayout,32> layouts;
    for(unsigned n=0;n<sets.size();++n){sets[n]=h<VkDescriptorSet>(0xD300+n);layouts[n]=n&1?otherLayout:f.setLayout;}
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=sets.size();ai.pSetLayouts=layouts.data();
    const std::array<Key,1> watched{{{0xFFFF,9}}};const auto epoch=f.t.watchReferenceImages(watched);
    std::array<ReferenceBinding,32> tokens;
    const auto populate=[&]{ok(f.t.setsAllocated(&ai,sets.data(),VK_SUCCESS));
        for(unsigned n=0;n<sets.size();++n){const VkDescriptorImageInfo value{VK_NULL_HANDLE,f.views[n&1],VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=sets[n];write.dstBinding=2;
            write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=&value;ok(f.t.updateDescriptors(1,&write,0,nullptr));
            tokens[n]=f.t.referenceBinding(sets[n]);require(tokens[n].published->read().known,"reused set has a fresh complete publication");
        }
    };
    struct BucketKey {Key key;};populate();std::uint64_t buckets{};for(const auto& token:tokens)buckets|=UINT64_C(1)<<(xess_vk::metadata_storage::Table<BucketKey,64>::bucket(token.set.handle^token.set.generation)&63);
    require(std::popcount(buckets)<sets.size(),"lifetime fixture exercises dependency-bucket collisions");
    allocations=0;measure=true;
    for(unsigned round=0;round<40;++round){const auto old=tokens[0];f.t.setsFreed(pool,sets.size(),sets.data(),VK_SUCCESS);
        require(!old.published->read().known,"free unlinks old publications before reusing their slots");populate();require(tokens[0].set!=old.set,"set generation changes across dependency-bucket reuse");
    }
    measure=false;require(!allocations,"dependency registration and set reuse add no hot allocations after initial pages");
    f.t.viewDestroying(f.views[0]);for(unsigned n=0;n<tokens.size();++n)require(tokens[n].published->read().known==bool(n&1),"bucket collisions never invalidate sets referencing a different view");
    f.t.setLayoutDestroying(otherLayout);for(const auto& token:tokens)require(!token.published->read().known,"layout retirement invalidates all and only its remaining dependent publications");
    require(f.t.referenceEpoch()==epoch&&f.t.stats().referenceLifetimeInvalidations==32,"view/layout fan-out stays local after repeated slot and generation reuse");
    f.t.poolReset(pool,VK_SUCCESS);require(!f.t.stats().fatalIssues,"pool reset unlinks every dependency bucket without a tracking failure");
    // Old bucket hints remain on the image, but freed sets must not be traversed.
    const auto before=f.t.stats().referenceLifetimeCandidates;f.t.imageRetiring({102,2});
    require(f.t.stats().referenceLifetimeCandidates==before,"retiring an image after pool reset never visits freed linked slots");
}
// Exact edges visit only the sets that reference the retiring object, move with
// a rewrite, and an exhausted edge pool falls back to the bucket scan without
// losing an invalidation.
void referenceLifetimeExactEdges(){
    for(const bool exhausted:{false,true}){++cases;Config config=small();config.sets=64;if(exhausted)config.referenceEdges=4;Fixture f(config);
        const auto pool=h<VkDescriptorPool>(0xE100);auto pi=f.poolInfo();pi.maxSets=64;require(f.t.poolCreated(pool,&pi,VK_SUCCESS).accepted(),"exact-edge pool created");
        std::array<VkDescriptorSet,32> sets;std::array<VkDescriptorSetLayout,32> layouts;
        for(unsigned n=0;n<sets.size();++n){sets[n]=h<VkDescriptorSet>(0xE300+n);layouts[n]=f.setLayout;}
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=sets.size();ai.pSetLayouts=layouts.data();
        const std::array<Key,1> watched{{{0xFFFF,9}}};f.t.watchReferenceImages(watched);require(f.t.setsAllocated(&ai,sets.data(),VK_SUCCESS).accepted(),"exact-edge sets allocated");
        const auto write=[&](VkDescriptorSet set,VkImageView view){const VkDescriptorImageInfo value{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=2;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=&value;
            require(f.t.updateDescriptors(1,&w,0,nullptr).accepted(),"exact-edge write accepted");return f.t.referenceBinding(set);};
        std::array<ReferenceBinding,32> tokens;
        for(unsigned n=0;n<sets.size();++n){tokens[n]=write(sets[n],f.views[n==0?0:1]);require(tokens[n].published->read().known,"exact-edge fixture publishes every set");}
        const auto known=[&](unsigned n){return tokens[n].published->read().known;};
        if(!exhausted){
            {const VkDescriptorImageInfo both[2]{{VK_NULL_HANDLE,f.views[0],VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,f.views[0],VK_IMAGE_LAYOUT_GENERAL}};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=sets[0];w.dstBinding=2;w.descriptorCount=2;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=both;
                require(f.t.updateDescriptors(1,&w,0,nullptr).accepted(),"exact-edge repeated write accepted");tokens[0]=f.t.referenceBinding(sets[0]);
                require(known(0)&&f.t.stats().referenceEdges==2*sets.size(),"a view repeated across array elements links one view edge and one image edge");}
            // Move set 1 onto view 0 first: the old view's list must no longer reach it.
            tokens[1]=write(sets[1],f.views[0]);const auto moved=f.t.stats();f.t.viewDestroying(f.views[1]);const auto destroyed=f.t.stats();
            require(destroyed.referenceLifetimeCandidates-moved.referenceLifetimeCandidates==sets.size()-2&&known(0)&&known(1),
                "a rewrite moves the set's edges; the destroyed view visits only its remaining sets");
            for(unsigned n=2;n<sets.size();++n)require(!known(n),"every set still referencing the destroyed view is invalidated");
            const auto before=f.t.stats();f.t.imageRetiring({101,1});const auto after=f.t.stats();
            require(!known(0)&&!known(1)&&after.referenceLifetimeInvalidations-before.referenceLifetimeInvalidations==2,"retiring an image invalidates exactly its referencing sets");
            require(after.referenceLifetimeCandidates-before.referenceLifetimeCandidates==2,"exact edges visit only the referencing sets, not their bucket neighbours");
            require(!after.referenceEdgeOverflows,"default edge capacity covers the fixture");
        }else{
            const auto before=f.t.stats();f.t.imageRetiring({102,2});const auto after=f.t.stats();
            require(after.referenceEdgeOverflows>0,"the small edge pool overflows");
            require(known(0)&&after.referenceLifetimeInvalidations-before.referenceLifetimeInvalidations==sets.size()-1,
                "an overflowed object falls back to the bucket scan and still invalidates every referencing set");
            for(unsigned n=1;n<sets.size();++n)require(!known(n),"no referencing set survives the fallback");
        }
        f.t.setsFreed(pool,sets.size(),sets.data(),VK_SUCCESS);
        require(!f.t.stats().referenceEdges,"freeing every set releases every exact edge");
    }
}
void referenceMaskPublication(){++cases;ReferenceMask publication;std::atomic<bool> done{};std::atomic<bool> bad{};
    std::thread writer([&]{for(std::uint64_t n=1;n<100000;++n)publication.publish({{n,n+1},n+2,n+3,n+4,true});done=true;});
    do {const auto value=publication.read();if(value.known&&(value.set.generation!=value.set.handle+1||value.version!=value.set.handle+2||value.epoch!=value.set.handle+3||value.mask!=value.set.handle+4))bad=true;}while(!done.load());
    writer.join();require(!bad,"concurrent readers never accept a mixed atomic publication");
    ReferenceMask empty;publication=empty;require(!publication.read().known,"slot clearing publishes an unknown result without reviving an old identity");
}
void pagedStorage(){++cases;
    struct Row {Key key;std::array<std::uint64_t,8> payload{};};
    auto table=std::make_unique<xess_vk::metadata_storage::Table<Row,65536>>();
    require(table->allocatedBytes()==0&&!table->find(1),"empty tables allocate no object payload and lookups do not allocate");
    failNextAllocation=true;require(!table->insert({1,1})&&!table->live&&table->freeHead==0,"page allocation failure leaves indexes and free slots intact");
    auto* first=table->insert({1,1});require(first!=nullptr,"a refused page can be retried");first->payload[0]=42;
    for(std::uint64_t n=2;n<=65536;++n)require(table->insert({n,n})!=nullptr,"paging preserves the full 65536-object capacity");
    require(!table->insert({65537,1})&&table->find(1)==first&&first->payload[0]==42,"full-table refusal preserves stable entry pointers");
    const auto bytes=table->allocatedBytes();
    for(std::uint64_t n=2;n<=65536;n+=2)table->erase(n);
    for(std::uint64_t n=1;n<=65536;n+=2)require(table->find(n)&&table->find(n)->key.generation==n,"backshift deletion retains all hash collision chains");
    allocations=0;measure=true;
    for(std::uint64_t n=65537;n<=98304;++n)if(!table->insert({n,n})){measure=false;require(false,"freed paged slots remain reusable");}
    measure=false;require(!allocations&&table->allocatedBytes()==bytes&&table->find(1)==first,"high-water pages are reused without allocation or relocation");
    Tracker empty;require(empty.storageBytes()==Tracker::allocatedStorageBytes()&&empty.storageBytes()<4*1024*1024,"descriptor constructor no longer writes the old 181 MiB payload");
    Fixture live;require(live.t.storageBytes()>Tracker::allocatedStorageBytes()&&live.t.storageBytes()<8*1024*1024,"populated small metadata pays only for touched pages");
}
void heapAndBenchmark(){++cases;
    allocations=allocationBytes=0;measure=true;auto* tracker=new Tracker;measure=false;
    heapAllocations=allocations;heapBytes=allocationBytes;delete tracker;
    require(heapAllocations==2&&heapBytes==Tracker::allocatedStorageBytes()+sizeof(Tracker),"one fixed implementation allocation plus fixture owner");
    Fixture f;qualified(f.ready());allocations=allocationBytes=0;const auto start=std::chrono::steady_clock::now();measure=true;
    for(unsigned i=0;i<100000;++i){const auto e=f.dispatch();benchmarkChecksum+=e.count;benchmarkChecksum+=f.t.validateEvidence(e).issues;}
    measure=false;const auto finish=std::chrono::steady_clock::now();benchmarkNs=std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count();
    require(allocations==0&&allocationBytes==0,"100000 pre-dispatch and validation calls allocate no hot-path heap");require(benchmarkChecksum==300000,"microbenchmark keeps all exact image evidence");
    require(f.t.stats().acceptedDispatches==100001,"detail-free dispatch accounting remains exact");
}
}
int main(int argc,char** argv){try{
    if(argc==2&&std::strcmp(argv[1],"--template-benchmark")==0){templateBenchmark();return 0;}
    if(argc==2&&std::strcmp(argv[1],"--template-benchmark-padded")==0){templateBenchmark(true);return 0;}
    referenceBindingCache();referenceConsumption();lazyReferenceMasks();referenceMasks();referenceLifetimeScope();referenceLifetimeSetReuse();referenceLifetimeExactEdges();referenceMaskPublication();pagedStorage();basic();diagnosticSnapshots();diagnosticImageMembership();graphicsLayoutThirtyThreeImages();ordinaryUpdates();historicalReferences();writeThenCopies();undefinedCopies();crossingAndOverlap();badWrites();objectReuse();
    poolAndSetLifetimes();failedCreatesAndAllocation();commandLifetimes();stickyUnknownAllocation();unsupportedLayouts();ordinaryViewRanges();
    compatibilityAndDisturb();missingAndUnsupportedDispatch();unsupportedPoolAndAlloc();pipelineAndLayoutReuse();capacityAndBudgets();
    aggregateDispatchCapacity();dynamicOpaqueBindings();recoverCurrentBindings();recoverLocalLayouts();currentOpaqueBindingIssues();recoverKnownValues();historicalSetRetirement();referenceHistoryBudget();
    ordinaryTemplates();templateReadFailures();packedTemplates();paddedTemplates();badTemplateStructures();templateIdentitiesAndAddresses();templateTypedBounds();fixedHashChurn();computeRestoreRange();computeRestoreDisturbance();continuousDispatchSelection();compactOrdinaryEvidence();compactBenchmark();selectionBenchmark();heapAndBenchmark();
    std::cout<<"{\"passed\":true,\"cases\":"<<cases<<",\"checks\":"<<checks<<",\"storageBytes\":"<<Tracker::allocatedStorageBytes()
        <<",\"fixedHeapAllocations\":"<<heapAllocations-1<<",\"hotPathAllocations\":"<<allocations
        <<",\"microbenchmarkIterations\":100000,\"microbenchmarkNanoseconds\":"<<benchmarkNs
        <<",\"selectionBenchmarkIterationsPerMode\":32768,\"selectionFullNanoseconds\":"<<selectionFullNs<<",\"selectionSelectedNanoseconds\":"<<selectionSelectedNs
        <<",\"compactBenchmarkIterationsPerMode\":32768,\"compactFullNanoseconds\":"<<compactFullNs<<",\"compactSelectedNanoseconds\":"<<compactSelectedNs
        <<",\"vulkanLoaded\":false,\"gpuTouched\":false,\"gameTouched\":false,\"copyPermitted\":false}\n";return 0;
}catch(const std::exception& e){measure=false;std::cerr<<e.what()<<'\n';return 1;}}
