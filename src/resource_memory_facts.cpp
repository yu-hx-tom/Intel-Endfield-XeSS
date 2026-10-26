#include "resource_memory_facts.hpp"
#include "metadata_storage.hpp"
#include <algorithm>
#include <limits>
#include <type_traits>

namespace xess_vk::resource_memory_facts {
namespace {
template<class T>Handle bits(T value)noexcept {if constexpr(std::is_pointer_v<T>)return reinterpret_cast<Handle>(value);else return static_cast<Handle>(value);}
constexpr auto hardObjects=16384u;
enum class ImageExtension {None,FormatList,Opaque};
struct ResourceState {
    Key key{};std::uint64_t issues{};bool isolationUnknown{},requirementsKnown{},bound{},linear{};ImageExtension imageExtension{};
    VkMemoryRequirements requirements{};Key memory{};VkDeviceSize offset{},end{};
    Resource previous{},next{},firstOverlap{};std::size_t overlaps{},pendingBindings{};
    image_inventory::Description imageDescription{};VkImageType imageType{};
};
struct MemoryState {
    Key key{};std::uint64_t issues{};bool isolationUnknown{},dedicated{};
    VkDeviceSize size{};std::uint32_t type{};Resource dedicatedTarget{};
    Resource head{};std::size_t pendingBindings{};
};
template<class T>struct Table {
    metadata_storage::Table<T,hardObjects> rows;ObjectUsage usage{};
    T* find(Handle handle)noexcept{return rows.find(handle);}
    const T* find(Handle handle)const noexcept{return rows.find(handle);}
    T* insert(Key key)noexcept {
        rows.limit=usage.capacity;auto* row=rows.insert(key);
        if(!row){++usage.capacityRefusals;return nullptr;}
        ++usage.live;usage.highWater=std::max(usage.highWater,usage.live);return row;
    }
    bool erase(Key key)noexcept {
        const auto* row=find(key.handle);if(!row||row->key!=key)return false;
        rows.erase(key.handle);--usage.live;return true;
    }
    std::size_t allocatedBytes()const noexcept{return rows.allocatedBytes();}
};
struct Entry {Resource resource{};Key memory{},previousMemory{};VkMemoryRequirements requirements{};VkDeviceSize offset{},end{};std::uint64_t issues{};};
struct Attempt {std::uint64_t generation{},issues{};BindApi api{};std::uint32_t count{};bool unlocalizable{};std::array<Entry,maxBindElements> entries{};};
bool sameRequirements(const VkMemoryRequirements& a,const VkMemoryRequirements& b)noexcept{return a.size==b.size&&a.alignment==b.alignment&&a.memoryTypeBits==b.memoryTypeBits;}
bool sameDescription(const image_inventory::Description& a,const image_inventory::Description& b)noexcept {
    return a.format==b.format&&a.width==b.width&&a.height==b.height&&a.depth==b.depth&&a.mipLevels==b.mipLevels&&a.arrayLayers==b.arrayLayers&&
        a.samples==b.samples&&a.usage==b.usage&&a.flags==b.flags&&a.tiling==b.tiling&&a.sharingMode==b.sharingMode&&a.initialLayout==b.initialLayout;
}
bool descriptionMatches(const image_inventory::Description& d,const VkImageCreateInfo& i)noexcept {
    return d.format==static_cast<std::uint32_t>(i.format)&&d.width==i.extent.width&&d.height==i.extent.height&&d.depth==i.extent.depth&&d.mipLevels==i.mipLevels&&
        d.arrayLayers==i.arrayLayers&&d.samples==static_cast<std::uint32_t>(i.samples)&&d.usage==i.usage&&d.flags==i.flags&&d.tiling==static_cast<std::uint32_t>(i.tiling)&&
        d.sharingMode==static_cast<std::uint32_t>(i.sharingMode)&&d.initialLayout==static_cast<std::uint32_t>(i.initialLayout);
}
bool multiplane(VkFormat format)noexcept {
    switch(format){
    case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16:case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16:case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16:case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM:case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM:case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM:case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16:case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16:case VK_FORMAT_G16_B16R16_2PLANE_444_UNORM:
    case VK_FORMAT_G14X2_B14X2R14X2_2PLANE_420_UNORM_3PACK16_ARM:case VK_FORMAT_G14X2_B14X2R14X2_2PLANE_422_UNORM_3PACK16_ARM:return true;
    default:return false;
    }
}
struct ImageCreation {std::uint64_t issues{};bool opaque{};ImageExtension extension{};};
ImageCreation classifyImage(const VkImageCreateInfo* info,const image_inventory::Description& description)noexcept {
    ImageCreation facts;
    if(!info||info->sType!=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO||!descriptionMatches(description,*info)){facts.issues|=bit(Issue::InvalidInput);facts.opaque=true;}
    if(!info){facts.extension=ImageExtension::Opaque;return facts;}
    constexpr VkImageCreateFlags allowed=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT|VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT|
        VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT|VK_IMAGE_CREATE_EXTENDED_USAGE_BIT|VK_IMAGE_CREATE_ALIAS_BIT;
    // PROTECTED and known non-disjoint multiplane shapes are locally refused,
    // but do not invent physical sharing with a different ordinary allocation.
    if(info->flags&VK_IMAGE_CREATE_PROTECTED_BIT)facts.issues|=bit(Issue::UnsupportedResource);
    if(info->flags&~(allowed|VK_IMAGE_CREATE_PROTECTED_BIT)){facts.issues|=bit(Issue::UnsupportedResource);facts.opaque=true;}
    if(info->format<=VK_FORMAT_UNDEFINED||info->format==VK_FORMAT_MAX_ENUM){facts.issues|=bit(Issue::UnsupportedResource);facts.opaque=true;}
    else if(multiplane(info->format))facts.issues|=bit(Issue::UnsupportedResource);
    if(info->tiling!=VK_IMAGE_TILING_LINEAR&&info->tiling!=VK_IMAGE_TILING_OPTIMAL){facts.issues|=bit(Issue::UnsupportedResource);facts.opaque=true;}
    if(info->pNext){const auto* head=static_cast<const VkBaseInStructure*>(info->pNext);
        if(head->sType==VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO&&!head->pNext){facts.extension=ImageExtension::FormatList;const auto* list=static_cast<const VkImageFormatListCreateInfo*>(info->pNext);
            // Only bounded structural metadata is read; viewFormats are neither
            // decoded nor retained and grant no format/copy qualification.
            if(list->viewFormatCount>64||(list->viewFormatCount&&!list->pViewFormats))facts.issues|=bit(Issue::UnsupportedResource);}
        else {facts.extension=ImageExtension::Opaque;facts.issues|=bit(Issue::UnsupportedResource);facts.opaque=true;}}
    return facts;
}
bool core(BindApi api)noexcept{return api==BindApi::ImageCore||api==BindApi::BufferCore;}
bool imageApi(BindApi api)noexcept{return api==BindApi::ImageCore||api==BindApi::ImageCore2||api==BindApi::ImageKhr2;}
bool validApi(BindApi api)noexcept{return static_cast<std::size_t>(api)<6;}
}

struct Tracker::Impl {
    VkDevice device{};VkPhysicalDeviceMemoryProperties properties{};Config config{};
    Table<ResourceState> images{},buffers{};Table<MemoryState> memories{};
    std::array<Attempt,maxPendingBinds> pending{};mutable Stats counters{};
    std::uint64_t imageSerial{},bufferSerial{},memorySerial{},attemptSerial{},globalIssues{};std::size_t opaqueObjects{},unlocalizablePending{};
    Impl(VkDevice d,const VkPhysicalDeviceMemoryProperties& p,Config c):device(d),properties(p),config(c) {
        images.usage.capacity=std::min<std::size_t>(c.images,hardObjects);buffers.usage.capacity=std::min<std::size_t>(c.buffers,hardObjects);memories.usage.capacity=std::min<std::size_t>(c.memories,hardObjects);
        counters.pendingBinds.capacity=std::min(c.pendingBinds,maxPendingBinds);
        if(!d||!p.memoryTypeCount||p.memoryTypeCount>VK_MAX_MEMORY_TYPES||!p.memoryHeapCount||p.memoryHeapCount>VK_MAX_MEMORY_HEAPS||
            c.images>hardObjects||c.buffers>hardObjects||c.memories>hardObjects||c.pendingBinds>maxPendingBinds)globalIssues|=bit(Issue::InvalidInput);
        if(c.deviceScope!=DeviceScope::SinglePhysicalDevice)globalIssues|=bit(Issue::UnsupportedDeviceScope);
        for(std::uint32_t n=0;n<std::min(p.memoryTypeCount,static_cast<std::uint32_t>(VK_MAX_MEMORY_TYPES));++n)
            if(p.memoryTypes[n].heapIndex>=p.memoryHeapCount)globalIssues|=bit(Issue::InvalidInput);
    }
    Result result(Key key,std::uint64_t issues)noexcept {if(issues)++counters.refusals;return {key,issues};}
    Table<ResourceState>& table(ResourceKind kind)noexcept{return kind==ResourceKind::Image?images:buffers;}
    const Table<ResourceState>& table(ResourceKind kind)const noexcept{return kind==ResourceKind::Image?images:buffers;}
    ResourceState* resource(Resource ref)noexcept {if(ref.kind!=ResourceKind::Image&&ref.kind!=ResourceKind::Buffer)return nullptr;auto* row=table(ref.kind).find(ref.key.handle);return row&&row->key==ref.key?row:nullptr;}
    const ResourceState* resource(Resource ref)const noexcept {if(ref.kind!=ResourceKind::Image&&ref.kind!=ResourceKind::Buffer)return nullptr;auto* row=table(ref.kind).find(ref.key.handle);return row&&row->key==ref.key?row:nullptr;}
    MemoryState* memory(Key key)noexcept {auto* row=memories.find(key.handle);return row&&row->key==key?row:nullptr;}
    const MemoryState* memory(Key key)const noexcept {auto* row=memories.find(key.handle);return row&&row->key==key?row:nullptr;}
    std::uint64_t resourceIdentity(Resource ref)const noexcept {if(ref.kind!=ResourceKind::Image&&ref.kind!=ResourceKind::Buffer)return bit(Issue::InvalidInput);if(!ref.key.handle||!ref.key.generation)return bit(Issue::UnknownObject);const auto* row=table(ref.kind).find(ref.key.handle);return !row?bit(Issue::UnknownObject):row->key==ref.key?0:bit(Issue::StaleGeneration);}
    std::uint64_t memoryIdentity(Key key)const noexcept {if(!key.handle||!key.generation)return bit(Issue::UnknownObject);const auto* row=memories.find(key.handle);return !row?bit(Issue::UnknownObject):row->key==key?0:bit(Issue::StaleGeneration);}
    void unknownMemory(Key key,std::uint64_t issues)noexcept {if(auto* row=memory(key))row->issues|=issues|bit(Issue::BindingUnknown);}
    void unknownEntry(const Entry& entry,std::uint64_t issues)noexcept {
        unknownMemory(entry.memory,issues);if(auto* row=resource(entry.resource)){if(row->bound)unknownMemory(row->memory,issues);row->issues|=issues|bit(Issue::BindingUnknown);}
    }
    bool overlap(const ResourceState& a,const ResourceState& b)const noexcept {
        const auto granularity=config.bufferImageGranularity;
        if(a.linear==b.linear||!granularity||(granularity&(granularity-1)))return a.offset<b.end&&b.offset<a.end;
        // Inclusive occupied granule indices avoid overflowing an outward
        // padded exclusive end at 2^64; exact binding ends never overflow.
        return a.offset/granularity<=(b.end-1)/granularity&&b.offset/granularity<=(a.end-1)/granularity;
    }
    void link(Resource ref,ResourceState& row,const Entry& entry)noexcept {
        auto* allocation=memory(entry.memory);row.memory=entry.memory;row.offset=entry.offset;row.end=entry.end;row.bound=true;
        for(auto at=allocation->head;at.key.handle;){auto* other=resource(at);++counters.bindingListSteps;
            if(overlap(row,*other)){if(!row.overlaps)row.firstOverlap=at;++row.overlaps;if(!other->overlaps)other->firstOverlap=ref;++other->overlaps;}at=other->next;}
        row.next=allocation->head;if(auto* head=resource(row.next))head->previous=ref;allocation->head=ref;
    }
    void unlink(Resource ref,ResourceState& row)noexcept {
        if(!row.bound)return;auto* allocation=memory(row.memory);if(!allocation)return;
        if(auto* previous=resource(row.previous))previous->next=row.next;else allocation->head=row.next;
        if(auto* next=resource(row.next))next->previous=row.previous;
        for(auto at=allocation->head;at.key.handle;){auto* other=resource(at);++counters.bindingListSteps;
            if(overlap(row,*other)){--other->overlaps;if(other->firstOverlap==ref){other->firstOverlap={};
                    if(other->overlaps)for(auto scan=allocation->head;scan.key.handle;){const auto* candidate=resource(scan);++counters.bindingListSteps;
                        if(scan!=at&&overlap(*other,*candidate)){other->firstOverlap=scan;break;}scan=candidate->next;}}}at=other->next;}
    }
    void finishPending(const Attempt& attempt)noexcept {
        if(attempt.unlocalizable)--unlocalizablePending;
        for(std::uint32_t n=0;n<attempt.count;++n){const auto& entry=attempt.entries[n];if(auto* row=resource(entry.resource))--row->pendingBindings;
            if(auto* allocation=memory(entry.memory))--allocation->pendingBindings;
            if(entry.previousMemory!=entry.memory)if(auto* old=memory(entry.previousMemory))--old->pendingBindings;}
    }
    std::uint64_t validate(const Entry& entry)const noexcept {
        auto issues=entry.issues|resourceIdentity(entry.resource)|memoryIdentity(entry.memory);const auto* row=resource(entry.resource);const auto* allocation=memory(entry.memory);
        if(row){issues|=row->issues;if(!row->requirementsKnown)issues|=bit(Issue::MissingRequirements);else if(!sameRequirements(row->requirements,entry.requirements))issues|=bit(Issue::InvalidRequirements);if(row->bound)issues|=bit(Issue::Lifecycle);}
        if(allocation){issues|=allocation->issues;if(entry.offset>allocation->size||entry.requirements.size>allocation->size-entry.offset)issues|=bit(Issue::OutOfBounds);
            if(allocation->type>=32||(entry.requirements.memoryTypeBits&(1u<<allocation->type))==0)issues|=bit(Issue::MemoryType);
            if(allocation->dedicated&&(entry.resource!=allocation->dedicatedTarget||entry.offset!=0))issues|=bit(Issue::DedicatedMismatch);}
        if(!entry.requirements.size||!entry.requirements.alignment||!entry.requirements.memoryTypeBits||(entry.requirements.alignment&(entry.requirements.alignment-1)))issues|=bit(Issue::InvalidRequirements);
        else if(entry.offset%entry.requirements.alignment)issues|=bit(Issue::Alignment);
        if(entry.requirements.size>std::numeric_limits<VkDeviceSize>::max()-entry.offset)issues|=bit(Issue::OutOfBounds);
        return issues;
    }
    template<class Info>BindTicket before(BindApi api,std::uint32_t count,const Info* infos,ResourceKind kind,VkStructureType type)noexcept {
        ++counters.bindAttempts;if(validApi(api))++counters.bindCalls[static_cast<std::size_t>(api)];
        if(!validApi(api)||imageApi(api)!=(kind==ResourceKind::Image)||!count||!infos||(core(api)&&count!=1)||count>maxBindElements){
            globalIssues|=bit(Issue::BindingUnknown)|(count>maxBindElements?bit(Issue::Capacity):bit(Issue::InvalidInput));++counters.refusals;return {0,0,globalIssues};}
        std::size_t slot{};while(slot<counters.pendingBinds.capacity&&pending[slot].generation)++slot;
        if(slot==counters.pendingBinds.capacity||attemptSerial==std::numeric_limits<std::uint64_t>::max()){
            const auto issue=slot==counters.pendingBinds.capacity?Issue::Capacity:Issue::GenerationExhausted;globalIssues|=bit(issue)|bit(Issue::BindingUnknown);
            if(issue==Issue::Capacity)++counters.pendingBinds.capacityRefusals;++counters.refusals;return {0,0,globalIssues};}
        auto& attempt=pending[slot];attempt={};attempt.generation=++attemptSerial;attempt.api=api;attempt.count=count;attempt.issues=globalIssues;
        ++counters.pendingBinds.live;counters.pendingBinds.highWater=std::max(counters.pendingBinds.highWater,counters.pendingBinds.live);
        for(std::uint32_t n=0;n<count;++n){auto& entry=attempt.entries[n];const auto& info=infos[n];Handle handle{};
            if constexpr(std::is_same_v<Info,VkBindImageMemoryInfo>)handle=bits(info.image);else handle=bits(info.buffer);
            entry.resource={kind,{handle,0}};entry.memory={bits(info.memory),0};entry.offset=info.memoryOffset;
            if(auto* row=table(kind).find(handle)){entry.resource.key=row->key;if(row->requirementsKnown)entry.requirements=row->requirements;if(row->bound)entry.previousMemory=row->memory;}
            else entry.issues|=bit(Issue::UnknownObject);
            if(auto* allocation=memories.find(bits(info.memory)))entry.memory=allocation->key;else {entry.issues|=bit(Issue::UnknownObject);attempt.unlocalizable=true;}
            if(info.sType!=type||info.pNext){entry.issues|=bit(Issue::UnsupportedBinding);attempt.unlocalizable=true;}
            entry.issues|=validate(entry);if(entry.requirements.size<=std::numeric_limits<VkDeviceSize>::max()-entry.offset)entry.end=entry.offset+entry.requirements.size;
            for(std::uint32_t previous=0;previous<n;++previous)if(attempt.entries[previous].resource==entry.resource)entry.issues|=bit(Issue::InvalidInput);
            attempt.issues|=entry.issues;
            if(auto* row=resource(entry.resource))++row->pendingBindings;if(auto* allocation=memory(entry.memory))++allocation->pendingBindings;
            if(entry.previousMemory!=entry.memory)if(auto* old=memory(entry.previousMemory))++old->pendingBindings;
        }
        if(attempt.unlocalizable)++unlocalizablePending;
        return {static_cast<std::uint32_t>(slot),attempt.generation,attempt.issues};
    }
};

Tracker::Tracker(VkDevice device,const VkPhysicalDeviceMemoryProperties& properties,Config config):impl_(std::make_unique<Impl>(device,properties,config)){}
Tracker::~Tracker()=default;
std::size_t Tracker::allocatedStorageBytes()noexcept{return sizeof(Impl);}
std::size_t Tracker::storageBytes() const noexcept {const auto& p=*impl_;return sizeof(Impl)+p.images.allocatedBytes()+p.buffers.allocatedBytes()+p.memories.allocatedBytes();}
Stats Tracker::stats()const noexcept {auto result=impl_->counters;result.storageBytes=storageBytes();result.images=impl_->images.usage;result.buffers=impl_->buffers.usage;result.memories=impl_->memories.usage;result.globalIssues=impl_->globalIssues;return result;}
void Tracker::coverageLost(Issue issue)noexcept{impl_->globalIssues|=bit(issue)|bit(Issue::BindingUnknown);}
void Tracker::memoryBindingUnknown(Key key,Issue issue)noexcept{if(!key.handle||!key.generation)coverageLost(Issue::UnknownObject);else impl_->unknownMemory(key,bit(issue));}
void Tracker::stop()noexcept{impl_->globalIssues|=bit(Issue::Stopped);}

Result Tracker::imageCreated(const image_inventory::Record& inventory,const VkImageCreateInfo* info,VkResult driver)noexcept {
    auto& p=*impl_;if(driver!=VK_SUCCESS)return p.result({},bit(Issue::DriverFailed));if(p.globalIssues)return p.result({},p.globalIssues);
    const Key key{inventory.image,inventory.generation};if(!key.handle||!key.generation){coverageLost(Issue::InvalidInput);return p.result(key,p.globalIssues);}
    const auto creation=classifyImage(info,inventory.description);
    if(auto* old=p.images.find(key.handle)){if(old->key==key&&info&&info->sType==VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO&&old->imageExtension==creation.extension&&old->isolationUnknown==creation.opaque&&
            (old->issues&creation.issues)==creation.issues&&sameDescription(old->imageDescription,inventory.description)&&old->imageType==info->imageType&&descriptionMatches(inventory.description,*info))return p.result(key,old->issues);
        coverageLost(Issue::Lifecycle);return p.result(key,p.globalIssues);}
    if(key.generation<=p.imageSerial){coverageLost(Issue::StaleGeneration);return p.result(key,p.globalIssues);}p.imageSerial=key.generation;
    auto* row=p.images.insert(key);if(!row){coverageLost(Issue::Capacity);return p.result(key,p.globalIssues);}++p.counters.registrations;row->imageDescription=inventory.description;
    row->issues=creation.issues;row->imageExtension=creation.extension;
    if(info){row->imageType=info->imageType;row->linear=info->tiling==VK_IMAGE_TILING_LINEAR;}row->isolationUnknown=creation.opaque;if(row->isolationUnknown)++p.opaqueObjects;return p.result(key,row->issues);
}
Result Tracker::bufferCreated(VkBuffer buffer,const VkBufferCreateInfo* info,VkResult driver)noexcept {
    auto& p=*impl_;if(driver!=VK_SUCCESS)return p.result({},bit(Issue::DriverFailed));if(p.globalIssues)return p.result({},p.globalIssues);
    const auto handle=bits(buffer);if(!handle||p.buffers.find(handle)){coverageLost(Issue::Lifecycle);return p.result({},p.globalIssues);}
    if(p.bufferSerial==std::numeric_limits<std::uint64_t>::max()){coverageLost(Issue::GenerationExhausted);return p.result({},p.globalIssues);}
    const Key key{handle,++p.bufferSerial};auto* row=p.buffers.insert(key);if(!row){coverageLost(Issue::Capacity);return p.result(key,p.globalIssues);}++p.counters.registrations;
    if(!info||info->sType!=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO||!info->size)row->issues|=bit(Issue::InvalidInput);
    if(!info||info->pNext||info->flags)row->issues|=bit(Issue::UnsupportedResource);row->linear=true;
    row->isolationUnknown=!info||(row->issues&bit(Issue::InvalidInput))||info->pNext||(info->flags&~VK_BUFFER_CREATE_PROTECTED_BIT);if(row->isolationUnknown)++p.opaqueObjects;return p.result(key,row->issues);
}
Result Tracker::memoryAllocated(VkDeviceMemory memory,const VkMemoryAllocateInfo* info,VkResult driver)noexcept {
    auto& p=*impl_;if(driver!=VK_SUCCESS)return p.result({},bit(Issue::DriverFailed));if(p.globalIssues)return p.result({},p.globalIssues);
    const auto handle=bits(memory);if(!handle||p.memories.find(handle)){coverageLost(Issue::Lifecycle);return p.result({},p.globalIssues);}
    if(p.memorySerial==std::numeric_limits<std::uint64_t>::max()){coverageLost(Issue::GenerationExhausted);return p.result({},p.globalIssues);}
    const Key key{handle,++p.memorySerial};auto* row=p.memories.insert(key);if(!row){coverageLost(Issue::Capacity);return p.result(key,p.globalIssues);}++p.counters.registrations;
    if(!info||info->sType!=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO||!info->allocationSize||info->memoryTypeIndex>=p.properties.memoryTypeCount){row->issues|=bit(Issue::InvalidInput);row->isolationUnknown=true;}
    if(info){row->size=info->allocationSize;row->type=info->memoryTypeIndex;
        if(row->type<p.properties.memoryTypeCount){const auto& type=p.properties.memoryTypes[row->type];if((type.propertyFlags&VK_MEMORY_PROPERTY_PROTECTED_BIT)||
            (p.properties.memoryHeaps[type.heapIndex].flags&VK_MEMORY_HEAP_MULTI_INSTANCE_BIT))row->issues|=bit(Issue::UnsupportedAllocation);}
        if(info->pNext){const auto* head=static_cast<const VkBaseInStructure*>(info->pNext);
            if(head->sType!=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO||head->pNext){row->issues|=bit(Issue::UnsupportedAllocation);row->isolationUnknown=true;}
            else {const auto* dedicated=static_cast<const VkMemoryDedicatedAllocateInfo*>(info->pNext);const auto image=bits(dedicated->image),buffer=bits(dedicated->buffer);
                if((image!=0)==(buffer!=0)){row->issues|=bit(Issue::DedicatedMismatch);row->isolationUnknown=true;}
                else {row->dedicated=true;row->dedicatedTarget.kind=image?ResourceKind::Image:ResourceKind::Buffer;
                    if(const auto* target=p.table(row->dedicatedTarget.kind).find(image?image:buffer))row->dedicatedTarget.key=target->key;
                    else {row->issues|=bit(Issue::UnknownObject);row->isolationUnknown=true;}}}
        }
    }
    if(row->isolationUnknown)++p.opaqueObjects;return p.result(key,row->issues);
}
Result Tracker::imageKey(VkImage image)const noexcept {const auto* row=impl_->images.find(bits(image));return {row?row->key:Key{},impl_->globalIssues|(row?row->issues:bit(Issue::UnknownObject))};}
Result Tracker::bufferKey(VkBuffer buffer)const noexcept {const auto* row=impl_->buffers.find(bits(buffer));return {row?row->key:Key{},impl_->globalIssues|(row?row->issues:bit(Issue::UnknownObject))};}
Result Tracker::memoryKey(VkDeviceMemory memory)const noexcept {const auto* row=impl_->memories.find(bits(memory));return {row?row->key:Key{},impl_->globalIssues|(row?row->issues:bit(Issue::UnknownObject))};}
Result Tracker::requirementsObserved(Resource resource,const VkMemoryRequirements& requirements)noexcept {
    auto& p=*impl_;auto issues=p.globalIssues|p.resourceIdentity(resource);auto* row=p.resource(resource);if(!row)return p.result(resource.key,issues);
    ++p.counters.requirementsObserved;if(!requirements.size||!requirements.alignment||!requirements.memoryTypeBits||(requirements.alignment&(requirements.alignment-1)))issues|=bit(Issue::InvalidRequirements);
    if(row->requirementsKnown&&!sameRequirements(row->requirements,requirements))issues|=bit(Issue::InvalidRequirements);
    if(issues){row->issues|=issues;if(row->bound)p.unknownMemory(row->memory,issues);return p.result(resource.key,issues);}
    row->requirements=requirements;row->requirementsKnown=true;return p.result(resource.key,row->issues);
}
void Tracker::imageRetiring(Key key)noexcept {auto& p=*impl_;if(auto* row=p.resource({ResourceKind::Image,key})){p.unlink({ResourceKind::Image,key},*row);if(row->isolationUnknown)--p.opaqueObjects;p.images.erase(key);++p.counters.retirements;}}
void Tracker::bufferRetiring(Key key)noexcept {auto& p=*impl_;if(auto* row=p.resource({ResourceKind::Buffer,key})){p.unlink({ResourceKind::Buffer,key},*row);if(row->isolationUnknown)--p.opaqueObjects;p.buffers.erase(key);++p.counters.retirements;}}
void Tracker::memoryRetiring(Key key)noexcept {auto& p=*impl_;if(auto* row=p.memory(key)){if(row->isolationUnknown)--p.opaqueObjects;p.memories.erase(key);++p.counters.retirements;}}
BindTicket Tracker::beforeImageBind(BindApi api,std::uint32_t count,const VkBindImageMemoryInfo* infos)noexcept{return impl_->before(api,count,infos,ResourceKind::Image,VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO);}
BindTicket Tracker::beforeBufferBind(BindApi api,std::uint32_t count,const VkBindBufferMemoryInfo* infos)noexcept{return impl_->before(api,count,infos,ResourceKind::Buffer,VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO);}
Result Tracker::afterBind(BindTicket ticket,VkResult driver)noexcept {
    auto& p=*impl_;if(ticket.slot>=p.pending.size()||!ticket.generation||ticket.generation>p.attemptSerial){coverageLost(Issue::BindingUnknown);return p.result({},p.globalIssues);}
    if(p.pending[ticket.slot].generation!=ticket.generation)return p.result({},bit(Issue::StaleGeneration)|p.globalIssues);
    auto& attempt=p.pending[ticket.slot];auto issues=attempt.issues|p.globalIssues;
    if(driver!=VK_SUCCESS){++p.counters.failedBinds;issues|=bit(Issue::DriverFailed);
        if(!core(attempt.api)){++p.counters.uncertainBatches;issues|=bit(Issue::BindingUnknown);for(std::uint32_t n=0;n<attempt.count;++n)p.unknownEntry(attempt.entries[n],bit(Issue::BindingUnknown));
            if(attempt.unlocalizable)p.globalIssues|=bit(Issue::BindingUnknown);}
    }else {for(std::uint32_t n=0;n<attempt.count;++n)issues|=p.validate(attempt.entries[n]);
        if(issues){for(std::uint32_t n=0;n<attempt.count;++n)p.unknownEntry(attempt.entries[n],bit(Issue::BindingUnknown));if(attempt.unlocalizable)p.globalIssues|=bit(Issue::BindingUnknown)|bit(Issue::UnsupportedBinding);}
        else {for(std::uint32_t n=0;n<attempt.count;++n){const auto& entry=attempt.entries[n];auto* row=p.resource(entry.resource);p.link(entry.resource,*row,entry);}p.counters.successfulBinds+=attempt.count;}
    }
    p.finishPending(attempt);attempt={};--p.counters.pendingBinds.live;return p.result({},issues|p.globalIssues);
}
BindingFacts Tracker::query(Resource ref)const noexcept {
    auto& p=*impl_;++p.counters.queries;BindingFacts facts;facts.device=p.device;facts.resource=ref;facts.issues=p.globalIssues|p.resourceIdentity(ref);
    const auto* row=p.resource(ref);if(!row)return facts;facts.issues|=row->issues;
    if(!row->requirementsKnown)facts.issues|=bit(Issue::MissingRequirements);if(!row->bound)facts.issues|=bit(Issue::MissingBinding);
    if(row->bound){facts.memory=row->memory;facts.offset=row->offset;facts.size=row->requirements.size;facts.end=row->end;facts.issues|=p.memoryIdentity(row->memory);
        if(const auto* allocation=p.memory(row->memory)){facts.issues|=allocation->issues;if(allocation->pendingBindings)facts.issues|=bit(Issue::BindingUnknown);}}
    if(row->pendingBindings||p.unlocalizablePending)facts.issues|=bit(Issue::BindingUnknown);
    facts.bindingBoundsKnown=facts.issues==0;
    facts.linearResource=row->linear;facts.bufferImageGranularity=p.config.bufferImageGranularity;
    const auto granularity=p.config.bufferImageGranularity;
    if(!granularity||(granularity&(granularity-1)))facts.issues|=bit(Issue::AliasGranularityUnknown);
    else if(facts.bindingBoundsKnown){facts.paddedAliasBegin=facts.offset-(facts.offset%granularity);const auto remainder=facts.end%granularity;
        const auto extra=remainder?granularity-remainder:0;if(extra>std::numeric_limits<VkDeviceSize>::max()-facts.end)facts.issues|=bit(Issue::AliasRangeOverflow);
        else {facts.paddedAliasEnd=facts.end+extra;facts.paddedAliasRangeKnown=true;}}
    if(p.config.bindingCoverage!=BindingCoverage::CompleteOrdinaryCalls)facts.issues|=bit(Issue::CoverageIncomplete);
    if(p.opaqueObjects)facts.issues|=bit(Issue::IsolationUnknown);
    if(facts.bindingBoundsKnown){facts.overlapCount=row->overlaps;facts.firstOverlap=row->firstOverlap;if(facts.overlapCount){facts.issues|=bit(Issue::Overlap);++p.counters.overlapQueries;}}
    facts.observedAliasFree=facts.bindingBoundsKnown&&facts.issues==0;return facts;
}
} // namespace xess_vk::resource_memory_facts
