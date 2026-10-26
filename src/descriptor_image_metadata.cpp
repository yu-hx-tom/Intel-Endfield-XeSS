#include "descriptor_image_metadata.hpp"
#include "metadata_storage.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace xess_vk::descriptor_image_metadata {
namespace {
constexpr std::size_t kinds=static_cast<std::size_t>(ObjectKind::Count);
template<class T> Handle bits(T value) noexcept{return reinterpret_cast<Handle>(value);}
void add(std::uint64_t& value,std::uint64_t amount=1) noexcept {
    value=amount>UINT64_MAX-value?UINT64_MAX:value+amount;
}
bool imageType(VkDescriptorType type) noexcept {
    return type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE||type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE||type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}
bool ordinaryType(VkDescriptorType type) noexcept {
    return imageType(type)||type==VK_DESCRIPTOR_TYPE_SAMPLER||type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||
        type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER||type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||
        type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
bool descriptorLayout(VkImageLayout layout) noexcept {
    switch(layout){
    case VK_IMAGE_LAYOUT_GENERAL:case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:return true;
    default:return false;
    }
}
// referenceEdges heads this object's exact list of publishing sets; when the
// edge pool ran out for it, referenceEdgesIncomplete falls back to buckets.
struct Image {Key key{};std::uint64_t issues{};image_inventory::Description description{};std::uint64_t referenceUsers{};std::uint32_t referenceEdges{};bool referenceEdgesIncomplete{};};
struct View {Key key{},image{};std::uint64_t issues{};VkImageViewType type{};VkFormat format{};VkComponentMapping components{};VkImageSubresourceRange range{};std::uint64_t referenceUsers{};std::uint32_t referenceEdges{};bool referenceEdgesIncomplete{};};
struct Binding {std::uint32_t binding{},count{},imageBase{};VkDescriptorType type{};VkShaderStageFlags stages{};};
struct SetLayout {Key key{};std::uint64_t issues{};std::array<Binding,maxLayoutBindings> bindings{};std::size_t count{},imageCount{},nonImageCount{},dynamicCount{};bool continuousInputShape{};std::uint64_t referenceUsers{};};
struct PipelineLayout {Key key{};std::uint64_t issues{};std::array<Key,maxBoundSets> sets{};std::size_t count{};bool noPushConstants{};};
struct Pool {
    Key key{};
    std::uint64_t issues{},driverLiveSets{};
    VkDescriptorPoolCreateFlags flags{};
    std::uint32_t maxSets{};
    std::uint64_t referenceUsers{};
};
struct Value {Key view{};VkImageLayout layout{};std::uint64_t issues{};bool written{};};
struct Set {Key key{},layout{},pool{};std::uint64_t issues{},version{};std::array<Value,maxSetImageBindings> values{};ReferenceMask referenceMask;Set* referencePrevious{};Set* referenceNext{};std::uint32_t referenceEdges{};bool referenceBuildPending{},referenceMaskDirty{true};};
// One set->object relation recorded by the set's latest mask publication.
// head points at the object's list head while the object lives; detached
// edges (head==nullptr) stay on the set's list until it republishes or dies.
struct ReferenceEdge {Set* set{};std::uint32_t* head{};std::uint32_t previous{},next{},setNext{};};
struct Pipeline {Key key{},layout{};std::uint64_t issues{};bool compactAllowed{};};
struct UpdateTemplate {Key key{},layout{};std::uint64_t issues{};std::array<VkDescriptorUpdateTemplateEntry,maxTemplateEntries> entries{};std::size_t count{},elements{},bytes{},span{};};
struct CommandPool {Key key{};std::uint64_t issues{};std::uint32_t family{};};
struct BoundSet {Key set{},bindingLayout{};std::uint64_t version{},issues{};};
struct Reference {Key set{};std::uint64_t version{};};
struct Command {
    Key key{},pool{},pipeline{};std::uint64_t issues{},ordinal{},allocationIssues{},bindingIssues{};
    VkCommandBufferLevel level{};bool recording{},executable{};
    std::array<BoundSet,maxBoundSets> bound{};
    std::uint32_t boundSetHighWater{};
    std::array<Reference,maxReferencedSets> references{};std::size_t referenceCount{};
};

template<class T,std::size_t N> using Table=metadata_storage::Table<T,N>;

template<class T> std::uint64_t exact(const T* value,Key key) noexcept {
    return !value?bit(Issue::UnknownObject):value->key!=key?bit(Issue::StaleGeneration):value->issues;
}
template<class T> std::uint64_t identityLifecycle(const T* value,Key key) noexcept {
    // Historical references protect object lifetime, not unrelated old metadata.
    return !value?bit(Issue::UnknownObject):value->key!=key?bit(Issue::StaleGeneration):value->issues&bit(Issue::Lifecycle);
}
const Binding* binding(const SetLayout* layout,std::uint32_t number) noexcept {
    if(layout)for(std::size_t i=0;i<layout->count;++i)if(layout->bindings[i].binding==number)return &layout->bindings[i];
    return nullptr;
}
bool within(const Binding* b,std::uint32_t start,std::uint32_t count) noexcept {
    return b&&count&&start<=b->count&&count<=b->count-start;
}
std::size_t descriptorSize(VkDescriptorType type) noexcept {
    if(imageType(type)||type==VK_DESCRIPTOR_TYPE_SAMPLER)return sizeof(VkDescriptorImageInfo);
    if(type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)return sizeof(VkBufferView);
    if(ordinaryType(type))return sizeof(VkDescriptorBufferInfo);return 0;
}
std::uint64_t templateEntryIssues(const VkDescriptorUpdateTemplateEntry& e,const SetLayout* l,std::size_t& elements,std::size_t& bytes,std::size_t& span) noexcept {
    std::uint64_t issues{};const auto* b=binding(l,e.dstBinding);const auto size=descriptorSize(e.descriptorType);
    if(!size||!b||b->type!=e.descriptorType)issues|=bit(Issue::UnsupportedDescriptor);
    if(!within(b,e.dstArrayElement,e.descriptorCount))issues|=bit(Issue::CrossBinding);
    if(e.descriptorCount>maxTemplateElements-elements)return issues|bit(Issue::Capacity);
    if(!size||!e.descriptorCount)return issues;
    if(e.descriptorCount>(maxTemplateBytes-bytes)/size)return issues|bit(Issue::Capacity);
    const auto maximum=std::numeric_limits<std::size_t>::max();
    if(e.offset>maximum-size||(e.descriptorCount>1&&e.stride>(maximum-e.offset-size)/(e.descriptorCount-1)))return issues|bit(Issue::InvalidInput);
    const auto end=e.offset+(e.descriptorCount-1)*e.stride+size;
    if(end>maxTemplateSpan)return issues|bit(Issue::Capacity);
    elements+=e.descriptorCount;bytes+=e.descriptorCount*size;span=std::max(span,end);return issues;
}
}

struct Tracker::Impl {
    Table<Image,16384> images;Table<View,32768> views;Table<SetLayout,2048> layouts;
    Table<PipelineLayout,1024> pipelineLayouts;Table<Pool,512> pools;Table<Set,maxDescriptorSets> sets;
    Table<Pipeline,4096> pipelines;Table<CommandPool,256> commandPools;Table<Command,512> commands;
    Table<UpdateTemplate,1024> templates;
    Stats counters{};std::uint64_t serial{},imageSerial{};bool srDispatchSelection{};
    std::atomic<std::uint64_t> referenceEpoch{1};
    std::array<Key,referenceMaskBits> referenceImages{};std::size_t referenceImageCount{};
    bool referenceMasksEnabled{};
    void retireReferenceBuild(Set& set) noexcept {
        if(!set.referenceBuildPending)return;
        set.referenceBuildPending=false;++counters.referenceMaskRetiredBuilds;--counters.referenceMaskPendingBuilds;
        if(set.referenceMask.consumed())++counters.referenceMaskConsumedBuilds;else ++counters.referenceMaskUnconsumedBuilds;
    }
    std::array<unsigned,referenceMaskBits*2> referenceIndex{};
    // Lifetime fan-out is bucketed, then matched by exact object generation.
    // Historical bucket bits can add work but can never authorize a stale set.
    std::array<Set*,64> referenceSets{};
    void invalidateReferences() noexcept {const auto old=referenceEpoch.load(std::memory_order_relaxed);referenceEpoch.store(old&&old<UINT64_MAX?old+1:0,std::memory_order_release);}
    static std::size_t referenceBucket(Key key) noexcept {auto h=(key.handle>>4)^(key.generation*UINT64_C(0x9e3779b97f4a7c15));h^=h>>29;return h&(referenceMaskBits*2-1);}
    std::uint64_t imageReferenceBit(Key image)const noexcept {
        auto at=referenceBucket(image);while(const auto slot=referenceIndex[at]){if(referenceImages[slot-1]==image)return UINT64_C(1)<<(slot-1);at=(at+1)&(referenceIndex.size()-1);}return 0;
    }
    static unsigned referenceSetBucket(Key key) noexcept {return static_cast<unsigned>(Table<Set,maxDescriptorSets>::bucket(key.handle^key.generation)&63);}
    void linkReferenceSet(Set& set) noexcept {
        auto& head=referenceSets[referenceSetBucket(set.key)];
        if(set.referencePrevious||set.referenceNext||head==&set)return;
        set.referenceNext=head;if(head)head->referencePrevious=&set;head=&set;
    }
    void unlinkReferenceSet(Set& set) noexcept {
        auto& head=referenceSets[referenceSetBucket(set.key)];
        if(set.referencePrevious)set.referencePrevious->referenceNext=set.referenceNext;
        else if(head==&set)head=set.referenceNext;
        if(set.referenceNext)set.referenceNext->referencePrevious=set.referencePrevious;
        set.referencePrevious=set.referenceNext=nullptr;
    }
    // Exact image/view -> set relations. Popular textures accumulate every
    // historical bucket bit, so the bucket scan visited ~3,000 sets per image
    // retirement in game; these edges visit only the publishing sets.
    static constexpr std::uint32_t referenceEdgeChunk=4096,referenceEdgeLimit=1u<<18;
    std::array<std::unique_ptr<ReferenceEdge[]>,referenceEdgeLimit/referenceEdgeChunk> referenceEdgeChunks{};
    std::uint32_t referenceEdgeAllocated{},referenceEdgeFree{},referenceEdgeCapacity{referenceEdgeLimit};
    ReferenceEdge& edge(std::uint32_t i) noexcept {return referenceEdgeChunks[(i-1)/referenceEdgeChunk][(i-1)%referenceEdgeChunk];}
    std::uint32_t allocateEdge() noexcept {
        if(const auto i=referenceEdgeFree){referenceEdgeFree=edge(i).next;edge(i).next=0;return i;}
        if(referenceEdgeAllocated>=referenceEdgeCapacity)return 0;
        auto& chunk=referenceEdgeChunks[referenceEdgeAllocated/referenceEdgeChunk];
        if(!chunk)try{chunk=std::make_unique<ReferenceEdge[]>(referenceEdgeChunk);}catch(...){return 0;}
        return ++referenceEdgeAllocated;
    }
    template<class T> void linkReferenceEdge(Set& set,T& object) noexcept {
        if(object.referenceEdgesIncomplete)return;
        // The set's old edges were cleared before this publication and lists
        // grow at the head, so a repeated element finds its own edge first.
        if(object.referenceEdges&&edge(object.referenceEdges).set==&set)return;
        const auto i=allocateEdge();
        if(!i){object.referenceEdgesIncomplete=true;++counters.referenceEdgeOverflows;return;}
        auto& e=edge(i);e={&set,&object.referenceEdges,0,object.referenceEdges,set.referenceEdges};
        if(object.referenceEdges)edge(object.referenceEdges).previous=i;
        object.referenceEdges=i;set.referenceEdges=i;++counters.referenceEdges;
    }
    void clearSetEdges(Set& set) noexcept {
        for(auto i=set.referenceEdges;i;){auto& e=edge(i);const auto following=e.setNext;
            if(e.head){if(e.previous)edge(e.previous).next=e.next;else *e.head=e.next;if(e.next)edge(e.next).previous=e.previous;}
            e={};e.next=referenceEdgeFree;referenceEdgeFree=i;--counters.referenceEdges;i=following;}
        set.referenceEdges=0;
    }
    template<class T> void detachObjectEdges(T& object) noexcept {
        for(auto i=object.referenceEdges;i;){auto& e=edge(i);const auto following=e.next;e.head=nullptr;e.previous=e.next=0;i=following;}
        object.referenceEdges=0;
    }
    template<class T> void invalidateReferenceUsers(const T& object) noexcept {
        if constexpr(requires{object.referenceUsers;}){
            if(!referenceMasksEnabled||!object.referenceUsers)return;
            ++counters.referenceLifetimeScans;
            auto users=object.referenceUsers;const auto epoch=referenceEpoch.load(std::memory_order_relaxed);
            if constexpr(requires{object.referenceEdgesIncomplete;})if(!object.referenceEdgesIncomplete){
                // Every published set referencing this object is on its list; a
                // rewrite republishes and moves the edges, so no content match is needed.
                for(auto i=object.referenceEdges;i;i=edge(i).next){auto* set=edge(i).set;
                    ++counters.referenceLifetimeCandidates;const auto snapshot=set->referenceMask.read();
                    if(!snapshot.known||snapshot.epoch!=epoch)continue;
                    retireReferenceBuild(*set);set->referenceMask.publish({set->key,set->version,epoch,snapshot.mask,false});++counters.referenceLifetimeInvalidations;
                }
                return;
            }
            while(users){const auto bucket=std::countr_zero(users);users&=users-1;
                for(auto* set=referenceSets[bucket];set;set=set->referenceNext){
                    ++counters.referenceLifetimeCandidates;const auto snapshot=set->referenceMask.read();
                    if(!snapshot.known||snapshot.epoch!=epoch)continue;
                    bool matched=false;
                    if constexpr(std::is_same_v<T,SetLayout>)matched=set->layout==object.key;
                    else if constexpr(std::is_same_v<T,Pool>)matched=set->pool==object.key;
                    else for(const auto& value:set->values)if(value.written){
                        if constexpr(std::is_same_v<T,View>)matched=value.view==object.key;
                        else if constexpr(std::is_same_v<T,Image>){const auto* view=views.find(value.view.handle);matched=view&&view->key==value.view&&view->image==object.key;}
                        if(matched)break;
                    }
                    if(matched){retireReferenceBuild(*set);set->referenceMask.publish({set->key,set->version,epoch,snapshot.mask,false});++counters.referenceLifetimeInvalidations;}
                }
            }
        }
    }
    SetImageReferences references(const Set*,std::uint64_t referenceUser=0,Set* edgeOwner=nullptr) noexcept;
    void publishReferenceMask(Set& set) noexcept {
        if(!referenceMasksEnabled)return;
        retireReferenceBuild(set);
        linkReferenceSet(set);clearSetEdges(set);
        const auto refs=references(&set,UINT64_C(1)<<referenceSetBucket(set.key),&set);ReferenceMaskSnapshot out{set.key,set.version,referenceEpoch.load(std::memory_order_relaxed),0,refs.imageReferencesKnown};
        ++counters.referenceMaskBuilds;counters.referenceMaskImages+=refs.referencedCount;
        for(std::size_t n=0;n<refs.referencedCount;++n)out.mask|=imageReferenceBit(refs.referencedImages[n]);
        out.known&=out.epoch!=0;set.referenceMask.publish(out);set.referenceMaskDirty=false;
        if(counters.referenceConsumptionMeasured){set.referenceBuildPending=true;++counters.referenceMaskMeasuredBuilds;++counters.referenceMaskPendingBuilds;}
    }
    void invalidateReferenceMask(Set& set) noexcept {
        if(!referenceMasksEnabled)return;
        retireReferenceBuild(set);clearSetEdges(set);unlinkReferenceSet(set);set.referenceMaskDirty=true;
        set.referenceMask.publish({set.key,set.version,referenceEpoch.load(std::memory_order_relaxed),0,false});
    }
    explicit Impl(const Config& c){
        if(c.referenceEdges>referenceEdgeLimit)throw std::invalid_argument("reference edge capacity outside hard bound");
        referenceEdgeCapacity=static_cast<std::uint32_t>(c.referenceEdges);
        counters.continuousDispatchSelection=c.continuousDispatchSelection;
        counters.compactOrdinaryDispatch=c.compactOrdinaryDispatch;
        counters.referenceConsumptionMeasured=c.measureReferenceConsumption;
        srDispatchSelection=c.srDispatchSelection;
        const std::array<std::size_t,kinds> limits{c.images,c.views,c.setLayouts,c.pipelineLayouts,c.pools,c.sets,c.pipelines,c.commandPools,c.commands,c.templates};
        const std::array<std::size_t,kinds> maximums{16384,32768,2048,1024,512,maxDescriptorSets,4096,256,512,1024};
        for(std::size_t i=0;i<kinds;++i){if(!limits[i]||limits[i]>maximums[i])throw std::invalid_argument("descriptor metadata capacity outside hard bounds");counters.objects[i].capacity=limits[i];}
        images.limit=c.images;views.limit=c.views;layouts.limit=c.setLayouts;pipelineLayouts.limit=c.pipelineLayouts;
        pools.limit=c.pools;sets.limit=c.sets;pipelines.limit=c.pipelines;commandPools.limit=c.commandPools;commands.limit=c.commands;templates.limit=c.templates;
    }
    std::uint64_t next() noexcept {
        if(serial==UINT64_MAX){referenceEpoch.store(0,std::memory_order_release);counters.fatalIssues|=bit(Issue::GenerationExhausted);return 0;}return ++serial;
    }
    Result result(Key key,std::uint64_t issues) noexcept {
        issues|=counters.fatalIssues;counters.observedIssues|=issues;return {key,issues};
    }
    bool failed(VkResult r) noexcept {if(r==VK_SUCCESS)return false;add(counters.failedDriverCalls);return true;}
    void ledgerAdd(std::uint64_t& value,std::uint64_t amount) noexcept {
        if(amount>UINT64_MAX-value) {
            counters.driverSetLedgerComplete=false;
            value=UINT64_MAX;
        } else value+=amount;
    }
    void ledgerAllocate(const VkDescriptorSetAllocateInfo* info) noexcept {
        ledgerAdd(counters.driverSetAllocationCalls,1);
        if(!info||info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO) {
            counters.driverSetLedgerComplete=false;
            return;
        }
        const auto count=static_cast<std::uint64_t>(info->descriptorSetCount);
        ledgerAdd(counters.driverSetsAllocated,count);
        auto* pool=pools.find(bits(info->descriptorPool));
        if(!pool||count>UINT64_MAX-pool->driverLiveSets||count>UINT64_MAX-counters.driverLiveSets) {
            counters.driverSetLedgerComplete=false;
            return;
        }
        pool->driverLiveSets+=count;
        counters.driverLiveSets+=count;
        counters.driverSetHighWater=std::max(counters.driverSetHighWater,counters.driverLiveSets);
        if(pool->driverLiveSets>pool->maxSets) counters.driverSetLedgerComplete=false;
    }
    void ledgerFree(Pool* pool,std::uint32_t count) noexcept {
        ledgerAdd(counters.driverSetFreeCalls,1);
        ledgerAdd(counters.driverSetsFreed,count);
        if(!pool||count>pool->driverLiveSets||count>counters.driverLiveSets) {
            counters.driverSetLedgerComplete=false;
            return;
        }
        pool->driverLiveSets-=count;
        counters.driverLiveSets-=count;
    }
    void ledgerClear(Pool& pool) noexcept {
        if(pool.driverLiveSets>counters.driverLiveSets) {
            counters.driverSetLedgerComplete=false;
            counters.driverLiveSets=0;
        } else counters.driverLiveSets-=pool.driverLiveSets;
        pool.driverLiveSets=0;
    }
    template<class T,std::size_t N> T* create(Table<T,N>& table,Handle h,ObjectKind kind) noexcept {
        if(counters.fatalIssues||!h)return nullptr;
        if(auto* old=table.find(h)){
            invalidateReferenceUsers(*old);old->issues|=bit(Issue::Lifecycle);
            if constexpr(std::is_same_v<T,Set>)invalidateReferenceMask(*old);
            return old;
        }
        if(table.live>=table.limit){auto& usage=counters.objects[static_cast<std::size_t>(kind)];add(usage.capacityRefusals);add(counters.capacityRefusals);return nullptr;}
        const auto gen=next();if(!gen)return nullptr;auto* value=table.insert({h,gen});if(!value){add(counters.capacityRefusals);return nullptr;}add(counters.registrations);
        auto& usage=counters.objects[static_cast<std::size_t>(kind)];usage.highWater=std::max(usage.highWater,table.live);return value;
    }
    template<class T,std::size_t N> void remove(Table<T,N>& table,Handle h) noexcept {if(auto* old=table.find(h)){
        if constexpr(std::is_same_v<T,Set>){retireReferenceBuild(*old);unlinkReferenceSet(*old);clearSetEdges(*old);}
        // erase() resets the slot, so no edge may keep pointing at its list head.
        if constexpr(requires{old->referenceEdgesIncomplete;})detachObjectEdges(*old);
        table.erase(h);add(counters.retirements);
    }}
    std::uint64_t missingCreate(Handle h)const noexcept{return counters.fatalIssues?counters.fatalIssues:h?bit(Issue::Capacity):bit(Issue::InvalidInput);}
    void touch(Set& set,std::uint64_t issues=0) noexcept {
        set.version=next();set.issues|=issues;add(counters.mutations);
        invalidateReferenceMask(set);
        // Old CB references retain the previous version even after bindings are
        // disturbed. preDispatch/validateEvidence rejects any mismatch; no scan
        // of all CBs is needed on the high-frequency update path.
    }
    std::uint64_t setIssues(const Set* s)const noexcept {
        if(!s)return bit(Issue::UnknownObject);
        return s->issues|exact(layouts.find(s->layout.handle),s->layout)|exact(pools.find(s->pool.handle),s->pool);
    }
    std::uint64_t valueIssues(const Value& value)const noexcept {
        if(!value.written)return bit(Issue::UndefinedDescriptor);
        const auto* view=views.find(value.view.handle);auto issues=value.issues|exact(view,value.view);
        if(view&&view->key==value.view)issues|=exact(images.find(view->image.handle),view->image);
        return issues;
    }
    std::uint64_t commandIssues(const Command* c)const noexcept {
        if(!c)return bit(Issue::UnknownObject);
        auto issues=c->issues|exact(commandPools.find(c->pool.handle),c->pool)|counters.fatalIssues;
        if(!c->recording&&!c->executable)issues|=bit(Issue::Lifecycle);
        for(std::size_t i=0;i<c->referenceCount;++i){const auto& ref=c->references[i];const auto* s=sets.find(ref.set.handle);
            issues|=identityLifecycle(s,ref.set);if(s&&s->key==ref.set){
                // A captured pool identity must survive. A pool missing from the
                // original metadata remains a current-set qualification issue.
                if(s->pool.handle)issues|=identityLifecycle(pools.find(s->pool.handle),s->pool);
                if(s->version!=ref.version)issues|=bit(Issue::DescriptorChanged);}}
        return issues;
    }
    void reference(Command& c,const Set& set) noexcept {
        for(std::size_t i=0;i<c.referenceCount;++i)if(c.references[i].set==set.key){
            if(c.references[i].version!=set.version)c.issues|=bit(Issue::DescriptorChanged);return;}
        if(c.referenceCount==c.references.size()){c.issues|=bit(Issue::Capacity);add(counters.referenceOverflowRefusals);add(counters.capacityRefusals);return;}
        c.references[c.referenceCount++]={set.key,set.version};
        counters.referencedSetsHighWater=std::max(counters.referencedSetsHighWater,c.referenceCount);
    }
    // Backward walk: erase moves the last dense slot, already visited, into this position.
    void retirePoolSets(Key pool) noexcept {
        for(std::size_t i=sets.live;i-->0;){auto& set=sets.entries[sets.dense[i]];
            if(set.pool==pool){touch(set,bit(Issue::StaleGeneration));remove(sets,set.key.handle);}}
    }
    void resetCommand(Command& c,bool allocatedOnly) noexcept {
        const auto handle=c.key.handle;const auto pool=c.pool;const auto level=c.level;const auto allocationIssues=c.allocationIssues;
        c={};c.key={handle,next()};c.pool=pool;c.level=level;c.allocationIssues=allocationIssues;c.issues=allocationIssues;
        (void)allocatedOnly;
    }
    Result updates(std::uint32_t,const VkWriteDescriptorSet*,std::uint32_t,const VkCopyDescriptorSet*) noexcept;
    void templateRefusal(std::uint64_t issues) noexcept {
        add(counters.templateUpdateRefusals);if(issues&bit(Issue::TemplateReadFailed))add(counters.templateReadRefusals);
        for(std::size_t i=0;i<counters.templateRefusedIssueRecords.size();++i)if(issues&(UINT64_C(1)<<i))add(counters.templateRefusedIssueRecords[i]);
    }
};

const char* objectName(ObjectKind kind) noexcept {
    constexpr const char* names[]{"images","views","set_layouts","pipeline_layouts","descriptor_pools","descriptor_sets","pipelines","command_pools","command_buffers","descriptor_update_templates"};
    const auto i=static_cast<std::size_t>(kind);return i<kinds?names[i]:"unknown";
}
Tracker::Tracker(Config c):impl_(std::make_unique<Impl>(c)){}
Tracker::~Tracker()=default;
std::size_t Tracker::allocatedStorageBytes() noexcept{return sizeof(Impl);}
std::size_t Tracker::storageBytes() const noexcept {const auto& p=*impl_;return sizeof(Impl)+p.images.allocatedBytes()+p.views.allocatedBytes()+p.layouts.allocatedBytes()+p.pipelineLayouts.allocatedBytes()+p.pools.allocatedBytes()+p.sets.allocatedBytes()+p.pipelines.allocatedBytes()+p.commandPools.allocatedBytes()+p.commands.allocatedBytes()+p.templates.allocatedBytes()+
    (p.referenceEdgeAllocated+Impl::referenceEdgeChunk-1)/Impl::referenceEdgeChunk*Impl::referenceEdgeChunk*sizeof(ReferenceEdge);}
void Tracker::stop(Issue issue) noexcept{impl_->referenceEpoch.store(0,std::memory_order_release);impl_->counters.fatalIssues|=bit(issue)|bit(Issue::Stopped);}
Stats Tracker::stats()const noexcept {
    const auto& p=*impl_;auto s=p.counters;s.storageBytes=storageBytes();
    const std::array<std::size_t,kinds> live{p.images.live,p.views.live,p.layouts.live,p.pipelineLayouts.live,p.pools.live,p.sets.live,p.pipelines.live,p.commandPools.live,p.commands.live,p.templates.live};
    for(std::size_t i=0;i<kinds;++i)s.objects[i].live=live[i];
    s.liveImages=live[0];s.liveViews=live[1];s.liveSetLayouts=live[2];s.livePipelineLayouts=live[3];s.livePools=live[4];
    s.liveSets=live[5];s.livePipelines=live[6];s.liveCommandPools=live[7];s.liveCommands=live[8];s.liveTemplates=live[9];return s;
}
Result Tracker::imageObserved(const image_inventory::Record& record) noexcept {
    auto& p=*impl_;const Key key{record.image,record.generation};
    if(!key.handle||!key.generation)return p.result(key,bit(Issue::InvalidInput));
    if(auto* image=p.images.find(key.handle)){
        if(image->key==key&&std::memcmp(&image->description,&record.description,sizeof(record.description))==0)return p.result(key,image->issues);
        p.invalidateReferenceUsers(*image);image->issues|=bit(Issue::StaleGeneration);return p.result(key,image->issues);
    }
    if(key.generation<=p.imageSerial)return p.result(key,bit(Issue::StaleGeneration));
    if(p.counters.fatalIssues)return p.result(key,0);
    if(p.images.live>=p.images.limit){add(p.counters.capacityRefusals);add(p.counters.objects[0].capacityRefusals);return p.result(key,bit(Issue::Capacity));}
    auto* image=p.images.insert(key);if(!image){add(p.counters.capacityRefusals);return p.result(key,bit(Issue::Capacity));}image->description=record.description;p.imageSerial=key.generation;
    if(!record.description.width||!record.description.height||!record.description.depth||!record.description.mipLevels||!record.description.arrayLayers)
        image->issues|=bit(Issue::InvalidInput);
    add(p.counters.registrations);p.counters.objects[0].highWater=std::max(p.counters.objects[0].highWater,p.images.live);return p.result(key,image->issues);
}
void Tracker::imageRetiring(Key key) noexcept {auto& p=*impl_;const auto* image=p.images.find(key.handle);if(image&&image->key==key){p.invalidateReferenceUsers(*image);p.remove(p.images,key.handle);}}
Result Tracker::viewCreated(VkImageView handle,const VkImageViewCreateInfo* info,VkResult driver) noexcept {
    auto& p=*impl_;if(p.failed(driver))return p.result({},bit(Issue::DriverFailed));
    auto* v=p.create(p.views,bits(handle),ObjectKind::Views);if(!v)return p.result({},p.missingCreate(bits(handle)));
    if(v->issues&bit(Issue::Lifecycle))return p.result(v->key,v->issues);
    if(!info||info->sType!=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO)return p.result(v->key,v->issues|=bit(Issue::InvalidInput));
    if(info->pNext||info->flags)v->issues|=bit(Issue::UnsupportedExtension);
    const auto* image=p.images.find(bits(info->image));if(!image)v->issues|=bit(Issue::UnknownObject);
    else {v->image=image->key;v->issues|=image->issues;}
    v->type=info->viewType;v->format=info->format;v->components=info->components;v->range=info->subresourceRange;
    if(info->viewType<VK_IMAGE_VIEW_TYPE_1D||info->viewType>VK_IMAGE_VIEW_TYPE_CUBE_ARRAY||info->format==VK_FORMAT_UNDEFINED)v->issues|=bit(Issue::UnsupportedView);
    constexpr VkImageAspectFlags aspects=VK_IMAGE_ASPECT_COLOR_BIT|VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    if(!v->range.aspectMask||(v->range.aspectMask&~aspects))v->issues|=bit(Issue::UnsupportedView);
    for(auto component:{info->components.r,info->components.g,info->components.b,info->components.a})
        if(component<VK_COMPONENT_SWIZZLE_IDENTITY||component>VK_COMPONENT_SWIZZLE_A)v->issues|=bit(Issue::UnsupportedView);
    if(image){const auto& d=image->description;
        if(v->range.levelCount==VK_REMAINING_MIP_LEVELS&&v->range.baseMipLevel<d.mipLevels)v->range.levelCount=d.mipLevels-v->range.baseMipLevel;
        if(v->range.layerCount==VK_REMAINING_ARRAY_LAYERS&&v->range.baseArrayLayer<d.arrayLayers)v->range.layerCount=d.arrayLayers-v->range.baseArrayLayer;
        if(!v->range.levelCount||v->range.baseMipLevel>=d.mipLevels||v->range.levelCount>d.mipLevels-v->range.baseMipLevel||
            !v->range.layerCount||v->range.baseArrayLayer>=d.arrayLayers||v->range.layerCount>d.arrayLayers-v->range.baseArrayLayer)v->issues|=bit(Issue::UnsupportedView);
    }
    return p.result(v->key,v->issues);
}
void Tracker::viewDestroying(VkImageView view) noexcept{if(const auto* object=impl_->views.find(bits(view))){impl_->invalidateReferenceUsers(*object);impl_->remove(impl_->views,bits(view));}}
Result Tracker::setLayoutCreated(VkDescriptorSetLayout handle,const VkDescriptorSetLayoutCreateInfo* info,VkResult driver) noexcept {
    auto& p=*impl_;if(p.failed(driver))return p.result({},bit(Issue::DriverFailed));
    auto* l=p.create(p.layouts,bits(handle),ObjectKind::SetLayouts);if(!l)return p.result({},p.missingCreate(bits(handle)));
    if(l->issues&bit(Issue::Lifecycle))return p.result(l->key,l->issues);
    if(!info||info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO)return p.result(l->key,l->issues|=bit(Issue::InvalidInput));
    if(info->pNext||info->flags)l->issues|=bit(Issue::UnsupportedExtension);
    if(info->bindingCount>maxLayoutBindings||(!info->pBindings&&info->bindingCount))return p.result(l->key,l->issues|=bit(Issue::Capacity));
    for(std::uint32_t i=0;i<info->bindingCount;++i){const auto& b=info->pBindings[i];
        if(binding(l,b.binding))l->issues|=bit(Issue::InvalidInput);
        if(!ordinaryType(b.descriptorType))l->issues|=bit(Issue::UnsupportedDescriptor);
        // Sampler-only bindings remain opaque. Only a combined image binding
        // with immutable samplers needs unsupported semantics in this subset.
        if(b.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER&&b.pImmutableSamplers)l->issues|=bit(Issue::ImmutableSampler);
        auto& copied=l->bindings[l->count++];copied={b.binding,b.descriptorCount,static_cast<std::uint32_t>(l->imageCount),b.descriptorType,b.stageFlags};
        if(imageType(b.descriptorType)){
            if(b.descriptorCount>maxSetImageBindings-l->imageCount)l->issues|=bit(Issue::Capacity);
            else l->imageCount+=b.descriptorCount;
        }else ++l->nonImageCount;
        if(b.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||b.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC){
            if(b.descriptorCount>maxUpdates-l->dynamicCount)l->issues|=bit(Issue::Capacity);else l->dynamicCount+=b.descriptorCount;
        }
    }
    const auto* depth=binding(l,5);const auto* motion=binding(l,7);
    l->continuousInputShape=depth&&motion&&depth->count&&motion->count&&imageType(depth->type)&&imageType(motion->type);
    return p.result(l->key,l->issues);
}
void Tracker::setLayoutDestroying(VkDescriptorSetLayout layout) noexcept{if(const auto* object=impl_->layouts.find(bits(layout))){impl_->invalidateReferenceUsers(*object);impl_->remove(impl_->layouts,bits(layout));}}
Result Tracker::pipelineLayoutCreated(VkPipelineLayout handle,const VkPipelineLayoutCreateInfo* info,VkResult driver) noexcept {
    auto& p=*impl_;if(p.failed(driver))return p.result({},bit(Issue::DriverFailed));
    auto* l=p.create(p.pipelineLayouts,bits(handle),ObjectKind::PipelineLayouts);if(!l)return p.result({},p.missingCreate(bits(handle)));
    if(l->issues&bit(Issue::Lifecycle))return p.result(l->key,l->issues);
    if(!info||info->sType!=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO)return p.result(l->key,l->issues|=bit(Issue::InvalidInput));
    if(info->pNext||info->flags)l->issues|=bit(Issue::UnsupportedExtension);
    if(info->setLayoutCount>maxBoundSets||(!info->pSetLayouts&&info->setLayoutCount)||info->pushConstantRangeCount>maxUpdates||
        (!info->pPushConstantRanges&&info->pushConstantRangeCount))return p.result(l->key,l->issues|=bit(Issue::Capacity));
    l->count=info->setLayoutCount;
    l->noPushConstants=info->pushConstantRangeCount==0;
    for(std::size_t i=0;i<l->count;++i){const auto* set=p.layouts.find(bits(info->pSetLayouts[i]));
        if(!set)l->issues|=bit(Issue::UnknownObject);else {l->sets[i]=set->key;l->issues|=set->issues;}}
    // Positive compatibility remains same-key; restoration can separately
    // prove incompatibility from differing definitions or empty/nonempty ranges.
    return p.result(l->key,l->issues);
}
void Tracker::pipelineLayoutDestroying(VkPipelineLayout layout) noexcept{impl_->remove(impl_->pipelineLayouts,bits(layout));}
Result Tracker::poolCreated(VkDescriptorPool handle,const VkDescriptorPoolCreateInfo* info,VkResult driver) noexcept {
    auto& p=*impl_;if(p.failed(driver))return p.result({},bit(Issue::DriverFailed));
    auto* pool=p.create(p.pools,bits(handle),ObjectKind::Pools);
    if(!pool) {
        p.counters.driverSetLedgerComplete=false;
        return p.result({},p.missingCreate(bits(handle)));
    }
    if(pool->issues&bit(Issue::Lifecycle)) {
        p.counters.driverSetLedgerComplete=false;
        return p.result(pool->key,pool->issues);
    }
    if(!info||info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO) {
        p.counters.driverSetLedgerComplete=false;
        return p.result(pool->key,pool->issues|=bit(Issue::InvalidInput));
    }
    pool->maxSets=info->maxSets;
    p.ledgerAdd(p.counters.declaredPoolMaxSets,pool->maxSets);
    if(!pool->maxSets) p.counters.driverSetLedgerComplete=false;
    pool->flags=info->flags;
    if(info->pNext||(info->flags&~VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT))pool->issues|=bit(Issue::UnsupportedExtension);
    if(!info->maxSets||info->poolSizeCount>maxUpdates||(!info->pPoolSizes&&info->poolSizeCount))pool->issues|=bit(Issue::InvalidInput);
    return p.result(pool->key,pool->issues);
}
void Tracker::poolReset(VkDescriptorPool handle,VkResult driver) noexcept {
    auto& p=*impl_;if(p.failed(driver))return;
    p.ledgerAdd(p.counters.driverPoolResetCalls,1);
    auto* pool=p.pools.find(bits(handle));
    if(!pool) {p.counters.driverSetLedgerComplete=false;return;}
    p.ledgerClear(*pool);
    p.retirePoolSets(pool->key);pool->key.generation=p.next();
}
void Tracker::poolDestroying(VkDescriptorPool handle) noexcept {
    auto& p=*impl_;
    p.ledgerAdd(p.counters.driverPoolDestroyCalls,1);
    if(!handle) return; // Vulkan permits a null destroy as a no-op.
    auto* pool=p.pools.find(bits(handle));
    if(!pool) {p.counters.driverSetLedgerComplete=false;return;}
    p.ledgerClear(*pool);
    if(pool->maxSets>p.counters.declaredPoolMaxSets) {
        p.counters.driverSetLedgerComplete=false;
        p.counters.declaredPoolMaxSets=0;
    } else p.counters.declaredPoolMaxSets-=pool->maxSets;
    p.retirePoolSets(pool->key);
    p.remove(p.pools,bits(handle));
}
Result Tracker::setsAllocated(const VkDescriptorSetAllocateInfo* info,const VkDescriptorSet* handles,VkResult driver) noexcept {
    auto& p=*impl_;if(p.failed(driver))return p.result({},bit(Issue::DriverFailed));
    p.ledgerAllocate(info);
    if(!info||info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO||info->descriptorSetCount>maxUpdates||
        (!handles&&info->descriptorSetCount)||(!info->pSetLayouts&&info->descriptorSetCount))return p.result({},bit(Issue::InvalidInput));
    auto* pool=p.pools.find(bits(info->descriptorPool));std::uint64_t issues=pool?pool->issues:bit(Issue::UnknownObject);
    if(info->pNext)issues|=bit(Issue::UnsupportedExtension);
    if(info->descriptorSetCount>p.sets.limit-p.sets.live){add(p.counters.capacityRefusals);add(p.counters.objects[5].capacityRefusals);return p.result({},issues|bit(Issue::Capacity));}
    // Full output-array preflight precedes all publications.
    for(std::uint32_t i=0;i<info->descriptorSetCount;++i){
        if(!bits(handles[i]))return p.result({},issues|bit(Issue::InvalidInput));
        if(auto* old=p.sets.find(bits(handles[i]))){
            p.counters.driverSetLedgerComplete=false;
            old->issues|=bit(Issue::Lifecycle);p.invalidateReferenceMask(*old);return p.result({},issues|bit(Issue::Lifecycle));
        }
        for(std::uint32_t j=0;j<i;++j)if(handles[i]==handles[j]){
            p.counters.driverSetLedgerComplete=false;
            return p.result({},issues|bit(Issue::InvalidInput));
        }
    }
    std::uint64_t aggregate=issues;
    for(std::uint32_t i=0;i<info->descriptorSetCount;++i){auto* set=p.create(p.sets,bits(handles[i]),ObjectKind::Sets);if(!set)return p.result({},aggregate|p.missingCreate(bits(handles[i])));
        set->pool=pool?pool->key:Key{};set->issues=issues;set->version=p.next();
        const auto* layout=p.layouts.find(bits(info->pSetLayouts[i]));
        if(!layout)set->issues|=bit(Issue::UnknownObject);else{set->layout=layout->key;set->issues|=layout->issues;}
        p.counters.observedIssues|=set->issues;
        aggregate|=set->issues;
    }
    return p.result(pool?pool->key:Key{},aggregate);
}
void Tracker::setsFreed(VkDescriptorPool pool,std::uint32_t count,const VkDescriptorSet* sets,VkResult driver) noexcept {
    auto& p=*impl_;if(p.failed(driver))return;
    const auto* owner=p.pools.find(bits(pool));
    p.ledgerFree(p.pools.find(bits(pool)),count);
    if(count>maxUpdates||(!sets&&count)){stop(Issue::Capacity);return;}
    for(std::uint32_t i=0;i<count;++i)if(auto* set=p.sets.find(bits(sets[i]))){
        if(!owner||set->pool!=owner->key){p.counters.driverSetLedgerComplete=false;p.touch(*set,bit(Issue::Lifecycle));continue;}
        p.touch(*set,bit(Issue::StaleGeneration));p.remove(p.sets,bits(sets[i]));
    }
}
void Tracker::setUnsupported(VkDescriptorSet handle,Issue issue) noexcept {
    auto& p=*impl_;if(auto* set=p.sets.find(bits(handle))){p.touch(*set,bit(issue));p.counters.observedIssues|=bit(issue);}
}

Result Tracker::updateDescriptors(std::uint32_t writeCount,const VkWriteDescriptorSet* writes,std::uint32_t copyCount,const VkCopyDescriptorSet* copies) noexcept {
    add(impl_->counters.ordinaryUpdateCalls);return impl_->updates(writeCount,writes,copyCount,copies);
}
Result Tracker::Impl::updates(std::uint32_t writeCount,const VkWriteDescriptorSet* writes,std::uint32_t copyCount,const VkCopyDescriptorSet* copies) noexcept {
    auto& p=*this;std::uint64_t issues{};
    if(writeCount>maxUpdates||copyCount>maxUpdates-writeCount||(!writes&&writeCount)||(!copies&&copyCount)){
        referenceEpoch.store(0,std::memory_order_release);counters.fatalIssues|=bit(Issue::Capacity)|bit(Issue::Stopped);return p.result({},bit(Issue::Capacity));
    }
    for(std::uint32_t i=0;i<writeCount;++i){const auto& w=writes[i];auto* dst=p.sets.find(bits(w.dstSet));
        if(!dst){issues|=bit(Issue::UnknownObject);continue;}
        std::uint64_t local=p.setIssues(dst);const auto* l=p.layouts.find(dst->layout.handle);const auto* b=binding(l,w.dstBinding);
        if(w.sType!=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET)local|=bit(Issue::InvalidInput);
        if(w.pNext)local|=bit(Issue::UnsupportedExtension);
        if(!within(b,w.dstArrayElement,w.descriptorCount))local|=bit(Issue::CrossBinding);
        if(!b||b->type!=w.descriptorType)local|=bit(Issue::UnsupportedDescriptor);
        const bool image=imageType(w.descriptorType);
        if(image&&!w.pImageInfo)local|=bit(Issue::InvalidInput);
        if(!image){
            if(w.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER&&!w.pImageInfo)local|=bit(Issue::InvalidInput);
            if((w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||w.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)&&!w.pTexelBufferView)local|=bit(Issue::InvalidInput);
            if((w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||w.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER||
                w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||w.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)&&!w.pBufferInfo)local|=bit(Issue::InvalidInput);
        }
        std::array<Value,maxSetImageBindings> values{};std::uint64_t valueIssues{};
        if(image&&!local){
            for(std::uint32_t j=0;j<w.descriptorCount;++j){const auto& info=w.pImageInfo[j];const auto* view=p.views.find(bits(info.imageView));
                auto& value=values[j];value={view?view->key:Key{bits(info.imageView),0},info.imageLayout,0,true};
                if(!view)value.issues|=bit(Issue::UnknownObject);
                else value.issues|=view->issues|exact(p.images.find(view->image.handle),view->image);
                if(!descriptorLayout(info.imageLayout)||(w.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE&&info.imageLayout!=VK_IMAGE_LAYOUT_GENERAL))value.issues|=bit(Issue::UnsupportedLayout);
                // Sampler is ignored for SAMPLED_IMAGE and STORAGE_IMAGE.
                if(w.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER&&!info.sampler)value.issues|=bit(Issue::UnsupportedDescriptor);
                valueIssues|=value.issues;
            }
        }
        // Known ranges replace every value, including bad/missing view facts.
        // Only structural/opaque mutations poison the set. Every real update
        // still changes its version and invalidates historical CB references.
        p.touch(*dst,local);issues|=local|valueIssues;
        if(image&&!local)for(std::uint32_t j=0;j<w.descriptorCount;++j)dst->values[b->imageBase+w.dstArrayElement+j]=values[j];
    }
    for(std::uint32_t i=0;i<copyCount;++i){const auto& c=copies[i];auto* dst=p.sets.find(bits(c.dstSet));const auto* src=p.sets.find(bits(c.srcSet));
        if(!dst){issues|=bit(Issue::UnknownObject);continue;}
        std::uint64_t local=p.setIssues(dst)|p.setIssues(src);
        const auto* dl=p.layouts.find(dst->layout.handle);const auto* sl=src?p.layouts.find(src->layout.handle):nullptr;
        const auto* db=binding(dl,c.dstBinding);const auto* sb=binding(sl,c.srcBinding);
        if(c.sType!=VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET)local|=bit(Issue::InvalidInput);
        if(c.pNext)local|=bit(Issue::UnsupportedExtension);
        if(!within(sb,c.srcArrayElement,c.descriptorCount)||!within(db,c.dstArrayElement,c.descriptorCount))local|=bit(Issue::CrossBinding);
        if(!sb||!db||sb->type!=db->type)local|=bit(Issue::UnsupportedDescriptor);
        if(src==dst&&c.srcBinding==c.dstBinding&&within(sb,c.srcArrayElement,c.descriptorCount)&&within(db,c.dstArrayElement,c.descriptorCount)&&
            c.srcArrayElement<c.dstArrayElement+c.descriptorCount&&c.dstArrayElement<c.srcArrayElement+c.descriptorCount)local|=bit(Issue::InvalidInput);
        const bool image=db&&imageType(db->type);std::array<Value,maxSetImageBindings> values{};
        if(image&&!local)for(std::uint32_t j=0;j<c.descriptorCount;++j){const auto& value=src->values[sb->imageBase+c.srcArrayElement+j];
            // Vulkan can copy undefined descriptors. Explicitly overwrite the
            // destination with undefined; never leave a previously valid value.
            if(p.valueIssues(value)==0)values[j]=value;
        }
        p.touch(*dst,local);issues|=local;
        if(image&&!local)for(std::uint32_t j=0;j<c.descriptorCount;++j)dst->values[db->imageBase+c.dstArrayElement+j]=values[j];
    }
    return p.result({},issues);
}
Result Tracker::templateCreated(VkDescriptorUpdateTemplate handle,const VkDescriptorUpdateTemplateCreateInfo* info,VkResult driver) noexcept {
    auto& p=*impl_;if(p.failed(driver))return p.result({},bit(Issue::DriverFailed));auto* t=p.create(p.templates,bits(handle),ObjectKind::Templates);
    if(!t)return p.result({},p.missingCreate(bits(handle)));if(t->issues&bit(Issue::Lifecycle))return p.result(t->key,t->issues);
    if(!info||info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO)return p.result(t->key,t->issues|=bit(Issue::InvalidInput));
    if(info->pNext||info->flags)t->issues|=bit(Issue::UnsupportedExtension);
    if(info->templateType!=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET)return p.result(t->key,t->issues|=bit(Issue::UnsupportedDescriptor));
    const auto* layout=p.layouts.find(bits(info->descriptorSetLayout));if(!layout)t->issues|=bit(Issue::UnknownObject);else {t->layout=layout->key;t->issues|=layout->issues;}
    if(info->descriptorUpdateEntryCount>maxTemplateEntries)return p.result(t->key,t->issues|=bit(Issue::Capacity));
    if(info->descriptorUpdateEntryCount&&!info->pDescriptorUpdateEntries)return p.result(t->key,t->issues|=bit(Issue::InvalidInput));
    t->count=info->descriptorUpdateEntryCount;
    for(std::size_t i=0;i<t->count;++i){t->entries[i]=info->pDescriptorUpdateEntries[i];t->issues|=templateEntryIssues(t->entries[i],layout,t->elements,t->bytes,t->span);}
    return p.result(t->key,t->issues);
}
void Tracker::templateDestroying(VkDescriptorUpdateTemplate handle) noexcept {impl_->remove(impl_->templates,bits(handle));}
Result Tracker::updateWithTemplate(VkDescriptorSet handle,VkDescriptorUpdateTemplate update,const void* data,TemplateUpdateKind kind,TemplateReader reader) noexcept {
    auto& p=*impl_;if(kind==TemplateUpdateKind::Core)add(p.counters.coreTemplateUpdateCalls);else if(kind==TemplateUpdateKind::Khr)add(p.counters.khrTemplateUpdateCalls);
    auto* set=p.sets.find(bits(handle));const auto* t=p.templates.find(bits(update));
    auto refuse=[&](std::uint64_t issues){issues|=p.counters.fatalIssues;if(set)p.touch(*set,issues);p.templateRefusal(issues);return p.result(set?set->key:Key{},issues);};
    std::uint64_t issues=p.setIssues(set)|p.counters.fatalIssues;
    if(kind!=TemplateUpdateKind::Core&&kind!=TemplateUpdateKind::Khr)issues|=bit(Issue::InvalidInput);
    if(!t)issues|=bit(Issue::UnknownObject);else {issues|=t->issues|exact(p.layouts.find(t->layout.handle),t->layout);if(set&&set->layout!=t->layout)issues|=bit(Issue::IncompatibleLayout);}
    if(issues)return refuse(issues);
    // Entries are owned and immutable; the exact layout generation was checked above.
    const auto address=reinterpret_cast<std::uintptr_t>(data);
    if(t->elements&&!data)issues|=bit(Issue::InvalidInput);
    if(t->span&&address>UINTPTR_MAX-(t->span-1))issues|=bit(Issue::InvalidInput);
    if(issues)return refuse(issues);
    std::array<std::byte,maxTemplateBytes> payload;
    bool spanRead=false;std::size_t first=t->span;
    if(reader.read&&reader.allowSpanRead&&t->elements>1){
        for(std::size_t i=0;i<t->count;++i)first=std::min(first,t->entries[i].offset);
        const auto span=t->span-first;
        p.counters.templateSpanMaxBytes=std::max(p.counters.templateSpanMaxBytes,std::uint64_t(span));
        if(span<=payload.size()){
            add(p.counters.templateSpanReadCalls);add(p.counters.templateReadCalls);
            add(p.counters.templateReadBytes,span);add(p.counters.templateReadElements,t->elements);
            try{spanRead=reader.read(reader.user,reinterpret_cast<const void*>(address+first),payload.data(),span)==span;}
            catch(...){spanRead=false;}
            if(spanRead)add(p.counters.templateSpanReadSuccesses);else add(p.counters.templateSpanReadFallbacks);
        }else add(p.counters.templateSpanReadOversize);
    }
    if(reader.read&&!spanRead){
        std::size_t offset{},runBytes{},runElements{},copied{};
        const auto readRun=[&]{
            if(!runBytes)return true;
            add(p.counters.templateReadCalls);add(p.counters.templateReadBytes,runBytes);add(p.counters.templateReadElements,runElements);
            const auto read=reader.read(reader.user,reinterpret_cast<const void*>(address+offset),payload.data()+copied,runBytes);
            if(read!=runBytes)return false;
            copied+=runBytes;runBytes=runElements=0;return true;
        };
        try{
            for(std::size_t i=0;i<t->count;++i){const auto& e=t->entries[i];const auto size=descriptorSize(e.descriptorType);
                for(std::size_t j=0;j<e.descriptorCount;++j){const auto next=e.offset+j*e.stride;
                    // Coalesce only adjacent referenced bytes, never padding or sparse gaps.
                    if(runBytes&&next!=offset+runBytes&&!readRun())return refuse(bit(Issue::TemplateReadFailed));
                    if(!runBytes)offset=next;
                    runBytes+=size;++runElements;
                }
            }
            if(!readRun())return refuse(bit(Issue::TemplateReadFailed));
        }catch(...){return refuse(bit(Issue::TemplateReadFailed));}
    }
    std::array<VkWriteDescriptorSet,maxTemplateEntries> writes;
    std::array<VkDescriptorImageInfo,maxTemplateElements> images;std::array<VkDescriptorBufferInfo,maxTemplateElements> buffers;
    std::array<VkBufferView,maxTemplateElements> texels;std::size_t at{},decoded{};
    for(std::size_t i=0;i<t->count;++i){const auto& e=t->entries[i];auto& w=writes[i];w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=handle;
        w.dstBinding=e.dstBinding;w.dstArrayElement=e.dstArrayElement;w.descriptorCount=e.descriptorCount;w.descriptorType=e.descriptorType;const auto size=descriptorSize(e.descriptorType);
        if(imageType(e.descriptorType)||e.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER)w.pImageInfo=images.data()+at;
        else if(e.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER||e.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)w.pTexelBufferView=texels.data()+at;
        else w.pBufferInfo=buffers.data()+at;
        for(std::size_t j=0;j<e.descriptorCount;++j){void* destination=w.pImageInfo?static_cast<void*>(&images[at+j]):w.pTexelBufferView?static_cast<void*>(&texels[at+j]):static_cast<void*>(&buffers[at+j]);
            const auto* source=reader.read?static_cast<const void*>(payload.data()+(spanRead?e.offset+j*e.stride-first:decoded)):reinterpret_cast<const void*>(address+e.offset+j*e.stride);
            std::memcpy(destination,source,size);decoded+=size;}
        at+=e.descriptorCount;
    }
    // The ordinary publisher is reached only after all typed reads succeeded.
    const auto result=p.updates(static_cast<std::uint32_t>(t->count),writes.data(),0,nullptr);
    if(!result.accepted())p.templateRefusal(result.issues);return result;
}

Result Tracker::commandPoolCreated(VkCommandPool handle,const VkCommandPoolCreateInfo* info,VkResult driver) noexcept {
    auto& p=*impl_;if(p.failed(driver))return p.result({},bit(Issue::DriverFailed));
    auto* pool=p.create(p.commandPools,bits(handle),ObjectKind::CommandPools);if(!pool)return p.result({},p.missingCreate(bits(handle)));
    if(pool->issues&bit(Issue::Lifecycle))return p.result(pool->key,pool->issues);
    if(!info||info->sType!=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO)return p.result(pool->key,pool->issues|=bit(Issue::InvalidInput));
    pool->family=info->queueFamilyIndex;
    if(info->pNext||(info->flags&~(VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT)))pool->issues|=bit(Issue::UnsupportedExtension);
    return p.result(pool->key,pool->issues);
}
void Tracker::commandPoolReset(VkCommandPool handle,VkResult driver) noexcept {
    auto& p=*impl_;if(p.failed(driver))return;const auto* pool=p.commandPools.find(bits(handle));if(!pool)return;
    for(std::size_t n=0;n<p.commands.live;++n){auto& c=p.commands.entries[p.commands.dense[n]];if(c.pool==pool->key)p.resetCommand(c,true);}
}
void Tracker::commandPoolDestroying(VkCommandPool handle) noexcept {
    auto& p=*impl_;const auto* pool=p.commandPools.find(bits(handle));if(!pool)return;
    const auto key=pool->key;for(std::size_t n=p.commands.live;n-->0;){auto& c=p.commands.entries[p.commands.dense[n]];if(c.pool==key)p.remove(p.commands,c.key.handle);}
    p.remove(p.commandPools,key.handle);
}
Result Tracker::commandsAllocated(const VkCommandBufferAllocateInfo* info,const VkCommandBuffer* handles,VkResult driver) noexcept {
    auto& p=*impl_;if(p.failed(driver))return p.result({},bit(Issue::DriverFailed));
    if(!info||info->sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO||info->commandBufferCount>maxUpdates||(!handles&&info->commandBufferCount))return p.result({},bit(Issue::InvalidInput));
    auto* pool=p.commandPools.find(bits(info->commandPool));std::uint64_t issues=pool?pool->issues:bit(Issue::UnknownObject);
    if(info->pNext)issues|=bit(Issue::UnsupportedExtension);
    if(info->level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY)issues|=bit(Issue::Secondary);
    if(info->commandBufferCount>p.commands.limit-p.commands.live){add(p.counters.capacityRefusals);add(p.counters.objects[8].capacityRefusals);return p.result({},issues|bit(Issue::Capacity));}
    for(std::uint32_t i=0;i<info->commandBufferCount;++i){if(!bits(handles[i]))return p.result({},issues|bit(Issue::InvalidInput));
        if(auto* c=p.commands.find(bits(handles[i]))){c->issues|=bit(Issue::Lifecycle);return p.result({},issues|bit(Issue::Lifecycle));}
        for(std::uint32_t j=0;j<i;++j)if(handles[i]==handles[j])return p.result({},issues|bit(Issue::InvalidInput));}
    for(std::uint32_t i=0;i<info->commandBufferCount;++i){auto* c=p.create(p.commands,bits(handles[i]),ObjectKind::Commands);if(!c)return p.result({},issues|p.missingCreate(bits(handles[i])));
        c->pool=pool?pool->key:Key{};c->level=info->level;c->issues=c->allocationIssues=issues;}
    return p.result(pool?pool->key:Key{},issues);
}
void Tracker::commandsFreed(std::uint32_t count,const VkCommandBuffer* handles) noexcept {
    auto& p=*impl_;if(count>maxUpdates||(!handles&&count)){stop(Issue::Capacity);return;}
    for(std::uint32_t i=0;i<count;++i)p.remove(p.commands,bits(handles[i]));
}
Result Tracker::begin(VkCommandBuffer handle,const VkCommandBufferBeginInfo* info,VkResult driver) noexcept {
    auto& p=*impl_;auto* c=p.commands.find(bits(handle));if(!c)return p.result({},bit(Issue::UnknownObject));
    if(p.failed(driver)){c->issues|=bit(Issue::DriverFailed);c->recording=c->executable=false;return p.result(c->key,c->issues);}
    p.resetCommand(*c,false);c->recording=true;
    if(c->level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY)c->issues|=bit(Issue::Secondary);
    if(!info||info->sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO)c->issues|=bit(Issue::InvalidInput);
    else {
        if(info->pNext||(info->flags&~(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT|VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)))c->issues|=bit(Issue::UnsupportedExtension);
        if(info->flags&VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)c->issues|=bit(Issue::SimultaneousUse);
    }
    c->issues|=exact(p.commandPools.find(c->pool.handle),c->pool);return p.result(c->key,c->issues);
}
Result Tracker::end(VkCommandBuffer handle,VkResult driver) noexcept {
    auto& p=*impl_;auto* c=p.commands.find(bits(handle));if(!c)return p.result({},bit(Issue::UnknownObject));
    if(!c->recording)c->issues|=bit(Issue::Lifecycle);
    if(p.failed(driver))c->issues|=bit(Issue::DriverFailed);
    c->recording=false;c->executable=driver==VK_SUCCESS;return p.result(c->key,p.commandIssues(c));
}
void Tracker::reset(VkCommandBuffer handle,VkResult driver) noexcept {auto& p=*impl_;if(p.failed(driver))return;if(auto* c=p.commands.find(bits(handle)))p.resetCommand(*c,true);}
Result Tracker::pipelineCreated(VkPipeline handle,const VkComputePipelineCreateInfo* info,VkResult driver) noexcept {
    auto& p=*impl_;if(!handle){if(driver!=VK_SUCCESS)add(p.counters.failedDriverCalls);return p.result({},driver==VK_SUCCESS?bit(Issue::InvalidInput):bit(Issue::DriverFailed));}
    auto* pipe=p.create(p.pipelines,bits(handle),ObjectKind::Pipelines);if(!pipe)return p.result({},p.missingCreate(bits(handle)));
    if(pipe->issues&bit(Issue::Lifecycle))return p.result(pipe->key,pipe->issues);
    if(!info||info->sType!=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO)return p.result(pipe->key,pipe->issues|=bit(Issue::InvalidInput));
    if(info->pNext||info->flags||info->stage.pNext||info->stage.flags)pipe->issues|=bit(Issue::UnsupportedExtension);
    if(info->stage.sType!=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO||info->stage.stage!=VK_SHADER_STAGE_COMPUTE_BIT)pipe->issues|=bit(Issue::InvalidInput);
    const auto* layout=p.pipelineLayouts.find(bits(info->layout));if(!layout)pipe->issues|=bit(Issue::UnknownObject);else{pipe->layout=layout->key;pipe->issues|=layout->issues;}
    return p.result(pipe->key,pipe->issues);
}
bool Tracker::allowCompactPipeline(Key key) noexcept {
    auto& p=*impl_;auto* pipe=p.pipelines.find(key.handle);
    if(!key.handle||!key.generation||exact(pipe,key)||p.counters.fatalIssues)return false;
    if(!pipe->compactAllowed)add(p.counters.compactPipelineClassifications);
    pipe->compactAllowed=true;return true;
}
void Tracker::pipelineDestroying(VkPipeline handle) noexcept{impl_->remove(impl_->pipelines,bits(handle));}
void Tracker::bindPipeline(VkCommandBuffer handle,VkPipelineBindPoint point,VkPipeline pipe) noexcept {
    if(point!=VK_PIPELINE_BIND_POINT_COMPUTE)return;auto& p=*impl_;auto* c=p.commands.find(bits(handle));if(!c)return;
    if(!c->recording)c->issues|=bit(Issue::Lifecycle);const auto* value=p.pipelines.find(bits(pipe));
    c->pipeline=value?value->key:Key{bits(pipe),0};
}
void Tracker::bindSets(VkCommandBuffer handle,VkPipelineBindPoint point,VkPipelineLayout layout,std::uint32_t first,
    std::uint32_t count,const VkDescriptorSet* handles,std::uint32_t dynamicCount,const std::uint32_t* offsets) noexcept {
    if(point!=VK_PIPELINE_BIND_POINT_COMPUTE)return;auto& p=*impl_;auto* c=p.commands.find(bits(handle));if(!c)return;
    if(!c->recording)c->issues|=bit(Issue::Lifecycle);
    // A conservative metadata clear does not prove driver sets were disturbed.
    c->boundSetHighWater=first<=maxBoundSets&&count<=maxBoundSets-first?
        std::max(c->boundSetHighWater,first+count):static_cast<std::uint32_t>(maxBoundSets+1);
    const auto* l=p.pipelineLayouts.find(bits(layout));
    if(!l||first>maxBoundSets||count>maxBoundSets-first||(!handles&&count)||dynamicCount>maxUpdates||(!offsets&&dynamicCount)){
        c->bound={};c->bindingIssues=bit(Issue::InvalidInput)|(!l?bit(Issue::UnknownObject):bit(Issue::Capacity));return;
    }
    c->bindingIssues=0;
    if(first>l->count||count>l->count-first){c->bound={};c->bindingIssues=bit(Issue::IncompatibleLayout);return;}
    const auto incompatible=[&](Key previous,std::uint32_t set){
        if(previous==l->key)return false;
        const auto* old=p.pipelineLayouts.find(previous.handle);
        if(exact(old,previous)||l->issues||set>=old->count)return false;
        if(old->noPushConstants!=l->noPushConstants)return true;
        for(std::uint32_t i=0;i<=set;++i){
            const auto* a=p.layouts.find(old->sets[i].handle);const auto* b=p.layouts.find(l->sets[i].handle);
            if(exact(a,old->sets[i])||exact(b,l->sets[i]))return false;
            const auto different=[](const SetLayout* x,const SetLayout* y){
                for(std::size_t j=0;j<x->count;++j){const auto& v=x->bindings[j];if(!v.count)continue;
                    const auto* w=binding(y,v.binding);
                    if(!w||v.count!=w->count||v.type!=w->type||v.stages!=w->stages)return true;
                }return false;
            };
            if(different(a,b)||different(b,a))return true;
        }return false; // Equal or incompletely compared layouts do not prove disturbance.
    };
    // An incompatible rebind at N makes all higher sets undefined in Vulkan.
    // Clearing our metadata alone is insufficient; require the old layout witness.
    if(c->boundSetHighWater<=maxBoundSets&&c->boundSetHighWater>first+count)for(std::uint32_t i=first;i<first+count;++i)
        if(c->bound[i].set.handle&&incompatible(c->bound[i].bindingLayout,i)){
            if(c->boundSetHighWater>first+count){c->boundSetHighWater=first+count;add(p.counters.restoreRangeDisturbances);}break;
        }
    for(const auto& b:c->bound)if(b.set.handle&&b.bindingLayout!=l->key){c->bound={};break;}
    std::size_t expectedOffsets{};
    for(std::uint32_t i=0;i<count;++i){auto& target=c->bound[first+i];target={};const auto* set=p.sets.find(bits(handles[i]));
        if(!set){target={Key{bits(handles[i]),0},l->key,0,bit(Issue::UnknownObject)};continue;}
        target={set->key,l->key,set->version,0};p.reference(*c,*set);
        if(set->layout!=l->sets[first+i])target.issues|=bit(Issue::IncompatibleLayout);
        const auto* sl=p.layouts.find(set->layout.handle);if(sl)expectedOffsets+=sl->dynamicCount;
    }
    if(expectedOffsets!=dynamicCount){
        if(!count)c->bindingIssues|=bit(Issue::InvalidInput);
        for(std::uint32_t i=0;i<count;++i)c->bound[first+i].issues|=bit(Issue::InvalidInput);
    }
}
void Tracker::commandUnsupported(VkCommandBuffer handle,Issue issue) noexcept {auto& p=*impl_;if(auto* c=p.commands.find(bits(handle))){c->issues|=bit(issue);c->bound={};p.counters.observedIssues|=bit(issue);}}

DispatchEvidence Tracker::preDispatch(VkCommandBuffer handle,std::uint32_t x,std::uint32_t y,std::uint32_t z,DispatchKind kind,bool diagnosticAllBindings) noexcept {
    auto& p=*impl_;DispatchEvidence out;out.groups={x,y,z};add(p.counters.dispatches);bool compact=false;
    auto* c=p.commands.find(bits(handle));if(!c)out.issues=p.commandIssues(c);
    if(c){out.command=c->key;if(c->ordinal==UINT64_MAX)c->issues|=bit(Issue::GenerationExhausted);else ++c->ordinal;
        out.recordOrdinal=c->ordinal;out.issues|=c->issues;if(!c->recording)out.issues|=bit(Issue::Lifecycle);
        if(kind!=DispatchKind::Direct){out.issues|=bit(Issue::UnsupportedDispatch);
            if(kind==DispatchKind::Unknown)c->issues|=bit(Issue::UnsupportedDispatch);}
        out.issues|=c->bindingIssues;
        const auto* pipe=p.pipelines.find(c->pipeline.handle);out.pipeline=c->pipeline;out.issues|=exact(pipe,c->pipeline);
        const auto* layout=pipe?p.pipelineLayouts.find(pipe->layout.handle):nullptr;
        if(pipe){out.pipelineLayout=pipe->layout;out.issues|=exact(layout,pipe->layout);}
        if(p.counters.continuousDispatchSelection&&kind==DispatchKind::Direct&&c->recording&&!out.issues&&
           !p.counters.fatalIssues&&!exact(p.commandPools.find(c->pool.handle),c->pool)&&layout){
            const auto* first=layout->count?p.layouts.find(layout->sets[0].handle):nullptr;
            const bool srShape=p.srDispatchSelection&&first&&layout->count==2&&
                (first->imageCount==3||first->imageCount>=10);
            if(!layout->count||(first&&!exact(first,layout->sets[0])&&!first->continuousInputShape&&!srShape)){
                // Historical references stay intact for the next candidate dispatch.
                if(!diagnosticAllBindings){out.filteredOut=true;add(p.counters.filteredDispatches);return out;}
                out.diagnosticSelectionBypassed=true;
                compact=p.counters.compactOrdinaryDispatch&&pipe->compactAllowed;
            }
        }
        out.issues|=p.commandIssues(c);
        if(layout&&out.issues==0){
            for(std::size_t i=0;i<layout->count;++i){const auto* sl=p.layouts.find(layout->sets[i].handle);out.issues|=exact(sl,layout->sets[i]);if(!sl)continue;
                out.ignoredNonImageBindingCount+=sl->nonImageCount;
                const auto& bound=c->bound[i];const auto* set=p.sets.find(bound.set.handle);out.issues|=bound.issues;
                // Opaque-only slots need no image evidence, but an explicitly
                // bound slot must still qualify its current binding metadata.
                if(!sl->imageCount&&!bound.set.handle)continue;
                out.issues|=exact(set,bound.set);if(!set){out.issues|=bit(Issue::MissingBinding);continue;}
                if(bound.bindingLayout!=layout->key||set->layout!=sl->key)out.issues|=bit(Issue::IncompatibleLayout);
                if(set->version!=bound.version)out.issues|=bit(Issue::DescriptorChanged);
                out.issues|=p.setIssues(set);
                if(!sl->imageCount)continue;
                for(std::size_t j=0;j<sl->count;++j){const auto& b=sl->bindings[j];if(!imageType(b.type))continue;
                    for(std::uint32_t k=0;k<b.count&&k<maxImageBindings;++k){
                        if(b.imageBase+k>=maxImageBindings||out.count==maxImageBindings){out.issues|=bit(Issue::Capacity);break;}
                        const auto& value=set->values[b.imageBase+k];const auto issue=p.valueIssues(value);out.issues|=issue;if(issue)continue;
                        const auto* view=p.views.find(value.view.handle);const auto* image=p.images.find(view->image.handle);
                        auto& target=out.bindings[out.count++];target.setIndex=static_cast<std::uint32_t>(i);target.binding=b.binding;target.arrayElement=k;
                        target.descriptorType=b.type;target.set=set->key;target.setLayout=set->layout;target.pool=set->pool;target.descriptorVersion=set->version;
                        target.image=image->key;
                        if(compact)continue;
                        target.view=view->key;target.image=image->key;target.viewType=view->type;target.viewFormat=view->format;target.components=view->components;
                        target.range=view->range;target.declaredLayout=value.layout;target.imageDescription=image->description;
                    }
                }
            }
        }
    }
    const auto rejectedGuardBindings=[&]{
        out.guardImagesKnown=c&&c->recording&&c->boundSetHighWater<=maxBoundSets&&p.pipelineLayouts.find(out.pipelineLayout.handle);
        if(c)for(const auto& bound:c->bound)if(bound.set.handle){
            const auto snapshot=diagnosticSetImages(reinterpret_cast<VkDescriptorSet>(bound.set.handle));
            out.guardImagesKnown&=snapshot.known&&!snapshot.issues&&snapshot.set==bound.set;
            for(std::size_t n=0;n<snapshot.count;++n){
                if(out.guardImageCount==out.bindings.size()){out.guardImagesKnown=false;break;}
                out.bindings[out.guardImageCount++].image=snapshot.bindings[n].image;
            }
        }
    };
    if(compact){
        // Same owner, history, set, value and generation checks as typed evidence.
        // These identities cannot qualify an SDK input or a restoration range.
        out.membershipOnly=out.filteredOut=true;out.guardImageCount=out.count;out.count=0;
        out.guardImagesKnown=out.issues==0;
        if(out.issues){out.guardImageCount=0;rejectedGuardBindings();}
        add(p.counters.filteredDispatches);add(p.counters.compactDispatches);
        add(p.counters.compactImageReferences,out.guardImageCount);
        if(!out.guardImagesKnown)add(p.counters.compactUnknownDispatches);
        p.counters.observedIssues|=out.issues;return out;
    }
    out.metadataAccepted=out.issues==0;
    if(c)out.computeBoundSetHighWater=c->boundSetHighWater;
    if(const auto* layout=p.pipelineLayouts.find(out.pipelineLayout.handle))out.computePipelineSetCount=static_cast<std::uint32_t>(layout->count);
    if(out.metadataAccepted&&c&&c->boundSetHighWater&&c->boundSetHighWater<=maxBoundSets) {
        const auto* layout=p.pipelineLayouts.find(out.pipelineLayout.handle);
        bool known=layout&&c->boundSetHighWater<=layout->count;
        for(std::size_t i=0;known&&i<c->boundSetHighWater;++i) {
            const auto& bound=c->bound[i];const auto* set=p.sets.find(bound.set.handle);
            const auto* setLayout=set?p.layouts.find(set->layout.handle):nullptr;
            known=set&&setLayout&&!bound.issues&&bound.bindingLayout==layout->key&&
                !exact(set,bound.set)&&set->version==bound.version&&!p.setIssues(set)&&
                set->layout==layout->sets[i]&&!setLayout->dynamicCount;
            if(known)out.computeRestoreSets[i]=bound.set;
        }
        if(known) {out.computeRestoreKnown=true;out.computeRestoreSetCount=c->boundSetHighWater;out.computePushConstantsAbsent=layout->noPushConstants;}
        else out.computeRestoreSets={};
    }
    if(diagnosticAllBindings&&out.metadataAccepted){
        out.guardImagesKnown=true;out.guardImageCount=out.count;
    }else if(diagnosticAllBindings)rejectedGuardBindings();
    if(out.metadataAccepted)add(p.counters.acceptedDispatches);else{out.count=0;add(p.counters.refusedDispatches);}
    p.counters.observedIssues|=out.issues;return out;
}
Key Tracker::setIdentity(VkDescriptorSet handle) const noexcept {
    const auto* set=impl_->sets.find(bits(handle));return set?set->key:Key{};
}
SetImageReferences Tracker::imageReferences(VkDescriptorSet handle) const noexcept {
    return impl_->references(impl_->sets.find(bits(handle)));
}
SetImageReferences Tracker::Impl::references(const Set* set,std::uint64_t referenceUser,Set* edgeOwner) noexcept {
    auto& p=*this;SetImageReferences out;
    if(!set){out.issues=bit(Issue::UnknownObject);return out;}
    out.known=true;out.set=set->key;out.layout=set->layout;out.pool=set->pool;out.version=set->version;
    out.issues=p.counters.fatalIssues|p.setIssues(set);out.setIssues=set->issues;
    auto* layout=p.layouts.find(set->layout.handle);
    if(!layout){out.issues|=bit(Issue::UnknownObject);return out;}
    if(referenceUser){layout->referenceUsers|=referenceUser;if(auto* pool=p.pools.find(set->pool.handle))pool->referenceUsers|=referenceUser;}
    out.layoutIssues=layout->issues;out.layoutBindingCount=layout->count;
    out.imageReferencesKnown=out.issues==0;
    for(std::size_t j=0;j<layout->count;++j){
        const auto& binding=layout->bindings[j];if(!imageType(binding.type))continue;
        out.declaredImageCount+=binding.count;
        if(binding.imageBase>maxSetImageBindings||binding.count>maxSetImageBindings-binding.imageBase||out.declaredImageCount>maxSetImageBindings){
            out.imageReferencesKnown=false;out.issues|=bit(Issue::Capacity);
        }
        for(std::uint32_t k=0;k<binding.count&&k<maxSetImageBindings&&binding.imageBase+k<maxSetImageBindings;++k){
            const auto& value=set->values[binding.imageBase+k];
            if(!value.written){++out.unwrittenCount;continue;}
            auto* view=p.views.find(value.view.handle);auto* image=view?p.images.find(view->image.handle):nullptr;
            if(identityLifecycle(view,value.view)||!view||identityLifecycle(image,view->image)||!image||out.referencedCount==out.referencedImages.size())
                out.imageReferencesKnown=false;
            else {out.referencedImages[out.referencedCount++]=image->key;if(referenceUser){view->referenceUsers|=referenceUser;image->referenceUsers|=referenceUser;}
                // A reference skipped above makes the publication unknown, so it needs no edge.
                if(edgeOwner){linkReferenceEdge(*edgeOwner,*view);linkReferenceEdge(*edgeOwner,*image);}}
        }
    }
    return out;
}
std::uint64_t Tracker::referenceEpoch()const noexcept{return impl_->referenceEpoch.load(std::memory_order_acquire);}
std::uint64_t Tracker::watchReferenceImages(std::span<const Key> images) noexcept {
    auto& p=*impl_;if(images.size()>referenceMaskBits||!referenceEpoch())return 0;
    for(std::size_t n=0;n<images.size();++n){if((images[n].handle==0)!=(images[n].generation==0))return 0;
        if(images[n].handle)for(std::size_t j=0;j<n;++j)if(images[j]==images[n])return 0;}
    if(p.referenceMasksEnabled&&images.size()==p.referenceImageCount&&std::equal(images.begin(),images.end(),p.referenceImages.begin()))return referenceEpoch();
    p.invalidateReferences();p.referenceMasksEnabled=true;p.referenceImageCount=images.size();p.referenceImages={};p.referenceIndex={};
    for(std::size_t n=0;n<images.size();++n){p.referenceImages[n]=images[n];if(!images[n].handle)continue;
        auto at=p.referenceBucket(images[n]);while(p.referenceIndex[at])at=(at+1)&(p.referenceIndex.size()-1);p.referenceIndex[at]=static_cast<unsigned>(n+1);}
    return referenceEpoch();
}
ReferenceBinding Tracker::referenceBinding(VkDescriptorSet handle) noexcept {
    auto& p=*impl_;auto* set=p.sets.find(bits(handle));if(!set)return {};
    const auto snapshot=set->referenceMask.read();
    if(set->referenceMaskDirty||snapshot.epoch!=referenceEpoch()||snapshot.version!=set->version||snapshot.set!=set->key)p.publishReferenceMask(*set);
    return {set->key,&set->referenceMask};
}
SetImageSnapshot Tracker::diagnosticSetImages(VkDescriptorSet handle) const noexcept {
    const auto& p=*impl_;SetImageSnapshot out;const auto refs=imageReferences(handle);
    out.set=refs.set;out.layout=refs.layout;out.pool=refs.pool;out.version=refs.version;out.issues=refs.issues;
    out.known=refs.known;out.referencedImages=refs.referencedImages;out.referencedCount=refs.referencedCount;
    out.unwrittenCount=refs.unwrittenCount;out.layoutBindingCount=refs.layoutBindingCount;out.declaredImageCount=refs.declaredImageCount;
    out.setIssues=refs.setIssues;out.layoutIssues=refs.layoutIssues;out.imageReferencesKnown=refs.imageReferencesKnown;
    const auto* set=p.sets.find(bits(handle));const auto* layout=set?p.layouts.find(set->layout.handle):nullptr;
    if(!layout)return out;
    for(std::size_t j=0;j<layout->count;++j){
        const auto& binding=layout->bindings[j];if(!imageType(binding.type))continue;
        for(std::uint32_t k=0;k<binding.count&&k<maxImageBindings;++k){
            if(binding.imageBase+k>=maxImageBindings||out.count==maxImageBindings){out.issues|=bit(Issue::Capacity);break;}
            const auto& value=set->values[binding.imageBase+k];
            const auto issue=p.valueIssues(value);out.issues|=issue;if(issue)continue;
            const auto* view=p.views.find(value.view.handle);const auto* image=p.images.find(view->image.handle);
            auto& target=out.bindings[out.count++];target.binding=binding.binding;target.arrayElement=k;
            target.descriptorType=binding.type;target.set=set->key;target.setLayout=set->layout;target.pool=set->pool;target.descriptorVersion=set->version;
            target.view=view->key;target.image=image->key;target.viewType=view->type;target.viewFormat=view->format;target.components=view->components;
            target.range=view->range;target.declaredLayout=value.layout;target.imageDescription=image->description;
        }
    }
    return out;
}
Result Tracker::validateEvidence(const DispatchEvidence& evidence)const noexcept {
    const auto& p=*impl_;auto issues=p.counters.fatalIssues|evidence.issues;
    if(!evidence.metadataAccepted||evidence.count>maxImageBindings||!evidence.recordOrdinal)issues|=bit(Issue::InvalidInput);
    const auto* c=p.commands.find(evidence.command.handle);issues|=exact(c,evidence.command)|p.commandIssues(c);
    if(c&&evidence.recordOrdinal>c->ordinal)issues|=bit(Issue::Lifecycle);
    const auto* pipe=p.pipelines.find(evidence.pipeline.handle);issues|=exact(pipe,evidence.pipeline);
    issues|=exact(p.pipelineLayouts.find(evidence.pipelineLayout.handle),evidence.pipelineLayout);
    for(std::size_t i=0;i<evidence.count&&i<maxImageBindings;++i){const auto& b=evidence.bindings[i];
        const auto* set=p.sets.find(b.set.handle);issues|=exact(set,b.set)|p.setIssues(set);
        if(set&&set->version!=b.descriptorVersion)issues|=bit(Issue::DescriptorChanged);
        const auto* view=p.views.find(b.view.handle);issues|=exact(view,b.view)|exact(p.images.find(b.image.handle),b.image);
        if(view&&view->image!=b.image)issues|=bit(Issue::StaleGeneration);
    }
    return {evidence.command,issues};
}
Result Tracker::validateImageBinding(const ImageBinding& binding)const noexcept {
    const auto& p=*impl_;const auto* view=p.views.find(binding.view.handle);
    auto issues=p.counters.fatalIssues|exact(view,binding.view)|exact(p.images.find(binding.image.handle),binding.image);
    if(view&&view->image!=binding.image)issues|=bit(Issue::StaleGeneration);
    return {binding.image,issues};
}
std::size_t Tracker::matchingImage(const DispatchEvidence& evidence,Key image) noexcept {
    if(!evidence.metadataAccepted||evidence.issues||!image.handle||!image.generation||evidence.count>maxImageBindings)return 0;
    std::size_t count{};for(std::size_t i=0;i<evidence.count;++i)if(evidence.bindings[i].image==image)++count;return count;
}
} // namespace xess_vk::descriptor_image_metadata
