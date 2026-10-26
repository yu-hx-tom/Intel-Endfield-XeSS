#include "resource_memory_facts.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace {std::atomic<bool> measured{};std::atomic<std::size_t> allocations{};}
void* operator new(std::size_t size){if(measured)++allocations;if(auto* value=std::malloc(size?size:1))return value;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* value)noexcept{std::free(value);}void operator delete[](void* value)noexcept{std::free(value);}
void operator delete(void* value,std::size_t)noexcept{std::free(value);}void operator delete[](void* value,std::size_t)noexcept{std::free(value);}
namespace {
namespace rm=xess_vk::resource_memory_facts;
unsigned checks{},cases{};std::uint64_t hotQueries{},denseMutationSteps{},shortChainMutationSteps{};
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void ok(rm::Result result){require(result.accepted(),"memory fact unexpectedly refused");}
void issue(rm::Result result,rm::Issue expected){require((result.issues&rm::bit(expected))!=0,"expected memory refusal bit");}
template<class T>T h(std::uint64_t value){if constexpr(std::is_pointer_v<T>)return reinterpret_cast<T>(value);else return static_cast<T>(value);}
rm::Config config(){rm::Config c;c.images=c.buffers=c.memories=1024;c.deviceScope=rm::DeviceScope::SinglePhysicalDevice;c.bindingCoverage=rm::BindingCoverage::CompleteOrdinaryCalls;c.bufferImageGranularity=64;return c;}
VkPhysicalDeviceMemoryProperties properties(){VkPhysicalDeviceMemoryProperties p{};p.memoryHeapCount=1;p.memoryHeaps[0].size=std::numeric_limits<VkDeviceSize>::max();p.memoryHeaps[0].flags=VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    p.memoryTypeCount=3;p.memoryTypes[0]={VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0};p.memoryTypes[1]={VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_PROTECTED_BIT,0};p.memoryTypes[2]={VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,0};return p;}
struct Fixture {
    rm::Tracker tracker;std::uint64_t serial{};
    explicit Fixture(rm::Config c=config()):tracker(h<VkDevice>(1),properties(),c){}
    rm::Resource image(std::uint64_t handle,VkDeviceSize size=64,VkDeviceSize alignment=16,std::uint32_t types=1,VkImageTiling tiling=VK_IMAGE_TILING_OPTIMAL,VkImageCreateFlags flags=0,const void* extension=nullptr,bool expectRefusal=false,VkFormat format=VK_FORMAT_R8G8B8A8_UNORM){
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.pNext=extension;info.flags=flags;info.imageType=VK_IMAGE_TYPE_2D;info.format=format;
        info.extent={7,5,1};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=tiling;info.usage=VK_IMAGE_USAGE_SAMPLED_BIT;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        xess_vk::image_inventory::Record record{handle,++serial,{std::uint32_t(info.format),7,5,1,1,1,std::uint32_t(info.samples),info.usage,flags,std::uint32_t(tiling),std::uint32_t(info.sharingMode),std::uint32_t(info.initialLayout)}};
        const auto result=tracker.imageCreated(record,&info,VK_SUCCESS);if(!expectRefusal&&!flags&&!extension&&(tiling==VK_IMAGE_TILING_OPTIMAL||tiling==VK_IMAGE_TILING_LINEAR))ok(result);
        rm::Resource resource{rm::ResourceKind::Image,result.key};if(size)ok(tracker.requirementsObserved(resource,{size,alignment,types}));return resource;
    }
    rm::Resource buffer(std::uint64_t handle,VkDeviceSize size=64,VkBufferCreateFlags flags=0,const void* extension=nullptr){VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.pNext=extension;info.flags=flags;info.size=17;info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        const auto result=tracker.bufferCreated(h<VkBuffer>(handle),&info,VK_SUCCESS);if(!flags&&!extension)ok(result);rm::Resource resource{rm::ResourceKind::Buffer,result.key};if(size)ok(tracker.requirementsObserved(resource,{size,16,1}));return resource;
    }
    rm::Key memory(std::uint64_t handle,VkDeviceSize size=4096,std::uint32_t type=0,const void* extension=nullptr){VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};info.pNext=extension;info.allocationSize=size;info.memoryTypeIndex=type;
        const auto result=tracker.memoryAllocated(h<VkDeviceMemory>(handle),&info,VK_SUCCESS);if(!extension&&type==0)ok(result);return result.key;}
    rm::BindTicket before(rm::Resource resource,rm::Key memory,VkDeviceSize offset=0,rm::BindApi api=rm::BindApi::ImageCore){if(resource.kind==rm::ResourceKind::Image){VkBindImageMemoryInfo info{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};info.image=h<VkImage>(resource.key.handle);info.memory=h<VkDeviceMemory>(memory.handle);info.memoryOffset=offset;return tracker.beforeImageBind(api,1,&info);}
        VkBindBufferMemoryInfo info{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};info.buffer=h<VkBuffer>(resource.key.handle);info.memory=h<VkDeviceMemory>(memory.handle);info.memoryOffset=offset;
        return tracker.beforeBufferBind(api==rm::BindApi::ImageCore?rm::BindApi::BufferCore:api,1,&info);}
    void bind(rm::Resource resource,rm::Key memory,VkDeviceSize offset=0){ok(tracker.afterBind(before(resource,memory,offset),VK_SUCCESS));}
};
void falseClaims(const rm::BindingFacts& facts){require(!facts.resourceLifetimePinned&&!facts.ownerKnown&&!facts.gpuComplete&&!facts.copyPermitted,"CPU facts grant no lifetime/owner/GPU/copy proof");}
void healthy(const rm::BindingFacts& facts){require(facts.bindingBoundsKnown&&facts.observedAliasFree&&!facts.issues&&!facts.overlapCount,"exact bounded binding has no observed alias");falseClaims(facts);}
void refused(const rm::BindingFacts& facts,rm::Issue expected){require(!facts.observedAliasFree&&(facts.issues&rm::bit(expected)),"alias admission must carry exact refusal");falseClaims(facts);}

void boundsAndRetirement(){++cases;Fixture f;const auto image=f.image(100),buffer=f.buffer(200,128);const auto memory=f.memory(300,1024);f.bind(image,memory);f.bind(buffer,memory,64);
    healthy(f.tracker.query(image));auto facts=f.tracker.query(buffer);healthy(facts);require(facts.size==128&&facts.offset==64&&facts.end==192,"actual requirements size, not buffer visible size, defines bounds");
    require(facts.paddedAliasRangeKnown&&facts.paddedAliasBegin==64&&facts.paddedAliasEnd==192&&facts.linearResource,"padded and exact ranges are separately reported");
    const auto alias=f.buffer(201,64);f.bind(alias,memory,32);facts=f.tracker.query(image);refused(facts,rm::Issue::Overlap);require(facts.bindingBoundsKnown&&facts.overlapCount==1,"overlap does not erase exact bounds");
    refused(f.tracker.query(buffer),rm::Issue::Overlap);f.tracker.bufferRetiring(alias.key);healthy(f.tracker.query(image));healthy(f.tracker.query(buffer));
    f.tracker.memoryRetiring(memory);refused(f.tracker.query(image),rm::Issue::UnknownObject);require(!f.tracker.query(image).bindingBoundsKnown,"freed memory cannot support old bound evidence");
}
void paddedAliasing(){++cases;auto c=config();c.bufferImageGranularity=256;Fixture f(c);const auto memory=f.memory(300);const auto image=f.image(100),second=f.image(101);const auto buffer=f.buffer(200),linearImage=f.image(102,64,16,1,VK_IMAGE_TILING_LINEAR);
    f.bind(image,memory,0);f.bind(second,memory,64);healthy(f.tracker.query(image));healthy(f.tracker.query(second)); // same nonlinear class, touching exact ends
    f.bind(buffer,memory,128);auto facts=f.tracker.query(image);refused(facts,rm::Issue::Overlap);require(facts.overlapCount==1&&facts.end==64&&facts.paddedAliasEnd==256,"different linearity aliases through granularity despite disjoint exact bytes");
    f.bind(linearImage,memory,192);facts=f.tracker.query(buffer);refused(facts,rm::Issue::Overlap);require(facts.overlapCount==2,"linear buffer/linear image touch without alias; both nonlinear images alias");
    f.tracker.imageRetiring(image.key);f.tracker.imageRetiring(second.key);healthy(f.tracker.query(buffer));healthy(f.tracker.query(linearImage));
}
void dedicated(){++cases;Fixture f;const auto image=f.image(100),buffer=f.buffer(200);VkMemoryDedicatedAllocateInfo d{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};d.image=h<VkImage>(100);
    const auto imageMemory=f.memory(300,128,0,&d);ok(f.tracker.memoryKey(h<VkDeviceMemory>(300)));f.bind(image,imageMemory);healthy(f.tracker.query(image));require(f.tracker.query(image).size==64,"legal oversized dedicated allocation retains exact requirement span");
    d.image={};d.buffer=h<VkBuffer>(200);const auto bufferMemory=f.memory(301,64,0,&d);f.bind(buffer,bufferMemory);healthy(f.tracker.query(buffer));
    f.tracker.imageRetiring(image.key);const auto reused=f.image(100);issue(f.tracker.afterBind(f.before(reused,imageMemory),VK_SUCCESS),rm::Issue::DedicatedMismatch);refused(f.tracker.query(reused),rm::Issue::BindingUnknown);
    Fixture offset;const auto target=offset.image(500);d.buffer={};d.image=h<VkImage>(500);const auto allocation=offset.memory(600,128,0,&d);
    issue(offset.tracker.afterBind(offset.before(target,allocation,16),VK_SUCCESS),rm::Issue::DedicatedMismatch);
}
void failedBatches(){++cases;Fixture f;const auto healthyImage=f.image(100),first=f.image(101),second=f.image(102);const auto goodMemory=f.memory(300),firstMemory=f.memory(301),secondMemory=f.memory(302);
    f.bind(healthyImage,goodMemory);VkBindImageMemoryInfo infos[2]{{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO},{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO}};
    infos[0].image=h<VkImage>(101);infos[0].memory=h<VkDeviceMemory>(301);infos[1].image=h<VkImage>(102);infos[1].memory=h<VkDeviceMemory>(302);
    const auto ticket=f.tracker.beforeImageBind(rm::BindApi::ImageKhr2,2,infos);infos[0].memory=h<VkDeviceMemory>(300); // caller bytes are not retained
    issue(f.tracker.afterBind(ticket,VK_ERROR_OUT_OF_DEVICE_MEMORY),rm::Issue::BindingUnknown);refused(f.tracker.query(first),rm::Issue::BindingUnknown);refused(f.tracker.query(second),rm::Issue::BindingUnknown);healthy(f.tracker.query(healthyImage));
    require((f.tracker.memoryKey(h<VkDeviceMemory>(301)).issues&rm::bit(rm::Issue::BindingUnknown))!=0,"failed batch localizes even potentially successful first entry");
    f.tracker.memoryRetiring(firstMemory);f.tracker.memoryRetiring(secondMemory);const auto newMemory=f.memory(301);const auto newImage=f.image(103);f.bind(newImage,newMemory);healthy(f.tracker.query(newImage));
    require(f.tracker.stats().uncertainBatches==1&&!f.tracker.stats().globalIssues,"known failed2 batch poisons addressed generations, not entire context");
    Fixture core;const auto source=core.image(500);const auto allocation=core.memory(600);issue(core.tracker.afterBind(core.before(source,allocation),VK_ERROR_OUT_OF_DEVICE_MEMORY),rm::Issue::DriverFailed);
    require((core.tracker.query(source).issues&rm::bit(rm::Issue::BindingUnknown))==0,"ordinary core failure does not invent a successful bind");core.bind(source,allocation);healthy(core.tracker.query(source));
}
void allOrNothing(){++cases;Fixture f;const auto first=f.image(100),second=f.image(101);const auto memory=f.memory(300,128);VkBindImageMemoryInfo infos[2]{{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO},{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO}};
    infos[0].image=h<VkImage>(100);infos[0].memory=h<VkDeviceMemory>(memory.handle);infos[1].image=h<VkImage>(101);infos[1].memory=h<VkDeviceMemory>(memory.handle);infos[1].memoryOffset=96;
    issue(f.tracker.afterBind(f.tracker.beforeImageBind(rm::BindApi::ImageCore2,2,infos),VK_SUCCESS),rm::Issue::OutOfBounds);
    require(f.tracker.stats().successfulBinds==0&&!f.tracker.query(first).bindingBoundsKnown&&!f.tracker.query(second).bindingBoundsKnown,"one invalid final entry prevents every publication");
}
void generationAndPending(){++cases;Fixture f;const auto old=f.image(100);const auto oldMemory=f.memory(300);const auto ticket=f.before(old,oldMemory);
    refused(f.tracker.query(old),rm::Issue::BindingUnknown);f.tracker.imageRetiring(old.key);f.tracker.memoryRetiring(oldMemory);const auto fresh=f.image(100);const auto freshMemory=f.memory(300);
    issue(f.tracker.afterBind(ticket,VK_SUCCESS),rm::Issue::StaleGeneration);f.tracker.imageRetiring(old.key);f.tracker.memoryRetiring(oldMemory);
    issue(f.tracker.requirementsObserved(old,{64,16,1}),rm::Issue::StaleGeneration);f.bind(fresh,freshMemory);healthy(f.tracker.query(fresh));
    require(fresh.key.generation>old.key.generation&&freshMemory.generation>oldMemory.generation,"image inventory and independent memory serials resist ABA");
    issue(f.tracker.afterBind(ticket,VK_SUCCESS),rm::Issue::StaleGeneration);healthy(f.tracker.query(fresh));
    const auto other=f.buffer(200);const auto pending=f.before(other,freshMemory,64);refused(f.tracker.query(fresh),rm::Issue::BindingUnknown);
    ok(f.tracker.afterBind(pending,VK_SUCCESS));healthy(f.tracker.query(fresh));healthy(f.tracker.query(other));
    f.tracker.memoryBindingUnknown(freshMemory);refused(f.tracker.query(fresh),rm::Issue::BindingUnknown);f.tracker.memoryRetiring(freshMemory);
    const auto newestMemory=f.memory(300);f.tracker.memoryBindingUnknown(freshMemory);const auto newestImage=f.image(101);f.bind(newestImage,newestMemory);healthy(f.tracker.query(newestImage));
    f.tracker.bufferRetiring(other.key);const auto recycledBuffer=f.buffer(200);f.tracker.bufferRetiring(other.key);f.bind(recycledBuffer,newestMemory,64);healthy(f.tracker.query(recycledBuffer));
    require(recycledBuffer.key.generation>other.key.generation,"buffer generations are independent and monotonic across handle reuse");
}
void rangeFailures(){++cases;
    for(unsigned mode=0;mode<5;++mode){Fixture f;const auto image=f.image(100,mode==4?0:64);const auto memory=f.memory(300,mode==2?std::numeric_limits<VkDeviceSize>::max():128);
        const auto offset=mode==0?VkDeviceSize(1):mode==1?VkDeviceSize(80):mode==2?std::numeric_limits<VkDeviceSize>::max()-31:0;
        if(mode==3)issue(f.tracker.requirementsObserved(image,{64,16,4}),rm::Issue::InvalidRequirements);
        const auto result=f.tracker.afterBind(f.before(image,memory,offset),VK_SUCCESS);issue(result,mode==0?rm::Issue::Alignment:mode==3?rm::Issue::InvalidRequirements:mode==4?rm::Issue::MissingRequirements:rm::Issue::OutOfBounds);
        require(!f.tracker.query(image).bindingBoundsKnown,"invalid range/type/requirements cannot establish bounds");}
    Fixture type;const auto image=type.image(100,64,16,4);const auto memory=type.memory(300);issue(type.tracker.afterBind(type.before(image,memory),VK_SUCCESS),rm::Issue::MemoryType);
    Fixture end;const auto target=end.image(100,1,1,1);const auto allocation=end.memory(300,std::numeric_limits<VkDeviceSize>::max());end.bind(target,allocation,std::numeric_limits<VkDeviceSize>::max()-1);
    const auto facts=end.tracker.query(target);require(facts.bindingBoundsKnown&&!facts.observedAliasFree&&(facts.issues&rm::bit(rm::Issue::AliasRangeOverflow)),"padded-end overflow preserves exact bounds but refuses alias assertion");
}
void unsupported(){++cases;Fixture f;const auto image=f.image(100);const auto memory=f.memory(300);f.bind(image,memory);healthy(f.tracker.query(image));
    VkExportMemoryAllocateInfo external{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    const auto exported=f.memory(301,4096,0,&external);refused(f.tracker.query(image),rm::Issue::IsolationUnknown);f.tracker.memoryRetiring(exported);healthy(f.tracker.query(image));
    const auto protectedMemory=f.memory(302,4096,1);issue(f.tracker.memoryKey(h<VkDeviceMemory>(302)),rm::Issue::UnsupportedAllocation);f.tracker.memoryRetiring(protectedMemory);
    const auto sparse=f.image(101,0,16,1,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_CREATE_SPARSE_BINDING_BIT);refused(f.tracker.query(image),rm::Issue::IsolationUnknown);f.tracker.imageRetiring(sparse.key);healthy(f.tracker.query(image));
    VkExternalMemoryImageCreateInfo extension{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};extension.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    const auto externalImage=f.image(102,0,16,1,VK_IMAGE_TILING_OPTIMAL,0,&extension);refused(f.tracker.query(image),rm::Issue::IsolationUnknown);f.tracker.imageRetiring(externalImage.key);healthy(f.tracker.query(image));
    VkImageCreateInfo multi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};multi.imageType=VK_IMAGE_TYPE_2D;multi.format=VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;multi.extent={8,8,1};multi.mipLevels=multi.arrayLayers=1;multi.samples=VK_SAMPLE_COUNT_1_BIT;multi.tiling=VK_IMAGE_TILING_OPTIMAL;multi.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
    xess_vk::image_inventory::Record record{103,++f.serial,{std::uint32_t(multi.format),8,8,1,1,1,1,multi.usage,0,0,0,0}};const auto result=f.tracker.imageCreated(record,&multi,VK_SUCCESS);issue(result,rm::Issue::UnsupportedResource);healthy(f.tracker.query(image));
    const rm::Resource multiResource{rm::ResourceKind::Image,result.key};issue(f.tracker.requirementsObserved(multiResource,{64,16,1}),rm::Issue::UnsupportedResource);
    const auto multiMemory=f.memory(304);issue(f.tracker.afterBind(f.before(multiResource,multiMemory),VK_SUCCESS),rm::Issue::UnsupportedResource);healthy(f.tracker.query(image));
    refused(f.tracker.query(multiResource),rm::Issue::BindingUnknown);f.tracker.imageRetiring(result.key);healthy(f.tracker.query(image));
    Fixture bindUnknown;const auto target=bindUnknown.image(100);const auto allocation=bindUnknown.memory(300);VkBindImageMemoryInfo info{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};VkBaseInStructure next{};info.pNext=&next;info.image=h<VkImage>(target.key.handle);info.memory=h<VkDeviceMemory>(allocation.handle);
    issue(bindUnknown.tracker.afterBind(bindUnknown.tracker.beforeImageBind(rm::BindApi::ImageCore2,1,&info),VK_SUCCESS),rm::Issue::UnsupportedBinding);require(bindUnknown.tracker.stats().globalIssues!=0,"unknown binding extension cannot pretend the named memory localizes every effect");
}
void knownCreationMetadata(){++cases;Fixture f;const auto raw=f.image(100);const auto memoryA=f.memory(300),memoryB=f.memory(301);f.bind(raw,memoryA);
    const VkFormat formats[]{VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB};VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};list.viewFormatCount=2;list.pViewFormats=formats;
    const auto unrelated=f.image(101,64,16,1,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,&list);f.bind(unrelated,memoryB);healthy(f.tracker.query(raw));healthy(f.tracker.query(unrelated));
    VkImageCreateInfo replay{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};replay.pNext=&list;replay.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;replay.imageType=VK_IMAGE_TYPE_2D;replay.format=VK_FORMAT_R8G8B8A8_UNORM;
    replay.extent={7,5,1};replay.mipLevels=replay.arrayLayers=1;replay.samples=VK_SAMPLE_COUNT_1_BIT;replay.tiling=VK_IMAGE_TILING_OPTIMAL;replay.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
    const xess_vk::image_inventory::Record replayRecord{101,unrelated.key.generation,{std::uint32_t(replay.format),7,5,1,1,1,1,replay.usage,replay.flags,0,0,0}};
    const auto registered=f.tracker.stats().registrations;ok(f.tracker.imageCreated(replayRecord,&replay,VK_SUCCESS));require(f.tracker.stats().registrations==registered,"same-key known creation metadata is idempotent");
    list.viewFormatCount=0;list.pViewFormats=nullptr;const auto alias=f.image(102,64,16,1,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,&list);f.bind(alias,memoryA);
    refused(f.tracker.query(raw),rm::Issue::Overlap);refused(f.tracker.query(alias),rm::Issue::Overlap);healthy(f.tracker.query(unrelated));
    f.tracker.imageRetiring(alias.key);healthy(f.tracker.query(raw));
    const auto ordinaryExtended=f.image(103,64,16,1,VK_IMAGE_TILING_OPTIMAL,0,nullptr,false,VK_FORMAT_A4R4G4B4_UNORM_PACK16);f.bind(ordinaryExtended,memoryA,64);healthy(f.tracker.query(raw));healthy(f.tracker.query(ordinaryExtended));
    VkBaseInStructure unknown{};list.pNext=&unknown;const auto opaque=f.image(104,0,16,1,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,&list);refused(f.tracker.query(raw),rm::Issue::IsolationUnknown);
    f.tracker.imageRetiring(opaque.key);healthy(f.tracker.query(raw));healthy(f.tracker.query(unrelated));
    std::array<VkFormat,65> excessiveFormats{};excessiveFormats.fill(VK_FORMAT_R8G8B8A8_UNORM);list.pNext=nullptr;list.viewFormatCount=65;list.pViewFormats=excessiveFormats.data();
    const auto localUnsupported=f.image(105,0,16,1,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,&list);healthy(f.tracker.query(raw));
    issue(f.tracker.requirementsObserved(localUnsupported,{64,16,1}),rm::Issue::UnsupportedResource);issue(f.tracker.afterBind(f.before(localUnsupported,memoryB,64),VK_SUCCESS),rm::Issue::UnsupportedResource);
    healthy(f.tracker.query(raw));refused(f.tracker.query(unrelated),rm::Issue::BindingUnknown);
}
void coverageAndCapacity(){++cases;auto c=config();c.bindingCoverage=rm::BindingCoverage::NotEstablished;Fixture incomplete(c);const auto image=incomplete.image(100);const auto memory=incomplete.memory(300);incomplete.bind(image,memory);
    auto facts=incomplete.tracker.query(image);require(facts.bindingBoundsKnown,"coverage assertion is independent of actual bound size");refused(facts,rm::Issue::CoverageIncomplete);
    c=config();c.bufferImageGranularity=0;Fixture granularity(c);const auto target=granularity.image(100);const auto allocation=granularity.memory(300);granularity.bind(target,allocation);facts=granularity.tracker.query(target);require(facts.bindingBoundsKnown,"unknown granularity does not erase exact bounds");refused(facts,rm::Issue::AliasGranularityUnknown);
    c=config();c.deviceScope=rm::DeviceScope::DeviceGroup;Fixture grouped(c);VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=64;issue(grouped.tracker.bufferCreated(h<VkBuffer>(1),&info,VK_SUCCESS),rm::Issue::UnsupportedDeviceScope);
    c=config();c.images=1;Fixture full(c);const auto first=full.image(100);full.image(101,0,16,1,VK_IMAGE_TILING_OPTIMAL,0,nullptr,true);require(full.tracker.stats().images.capacityRefusals==1&&full.tracker.stats().globalIssues,"capacity cannot drop a live creation silently");refused(full.tracker.query(first),rm::Issue::Capacity);
    c=config();c.pendingBinds=1;Fixture pending(c);const auto a=pending.image(100),b=pending.image(101);const auto m=pending.memory(300);const auto tracked=pending.before(a,m);const auto refusedTicket=pending.before(b,m,64);
    require(tracked.tracked()&&!refusedTicket.tracked()&&pending.tracker.stats().pendingBinds.capacityRefusals==1,"pending overflow is bounded and permanently fail closed");pending.tracker.afterBind(tracked,VK_SUCCESS);
    Fixture oversized;VkBindBufferMemoryInfo many[rm::maxBindElements+1]{};require(!oversized.tracker.beforeBufferBind(rm::BindApi::BufferCore2,std::uint32_t(std::size(many)),many).tracked()&&oversized.tracker.stats().globalIssues,"unretained binding batch cannot pass later queries");
    Fixture missed;issue(missed.tracker.afterBind({},VK_SUCCESS),rm::Issue::BindingUnknown);require(missed.tracker.stats().globalIssues!=0,"a missing before-call ticket cannot localize a lost binding");
}
void shortMemoryChainAtCapacity(){++cases;auto c=config();c.images=16384;Fixture f(c);rm::Resource selected{};
    for(std::uint64_t n=1;n<=16384;++n){const auto resource=f.image(n);if(n==1)selected=resource;}
    const auto memory=f.memory(50000);f.bind(selected,memory);const auto before=f.tracker.stats().bindingListSteps;
    measured=true;for(unsigned n=0;n<10000;++n){const auto facts=f.tracker.query(selected);require(facts.observedAliasFree,"short memory chain remains healthy at full resource capacity");++hotQueries;}measured=false;
    require(f.tracker.stats().bindingListSteps==before,"queries do not traverse unrelated full resource tables");shortChainMutationSteps=before;require(f.tracker.stats().images.highWater==16384,"hard image capacity exercised without enlarging tables");
}
void denseMemoryCache(){++cases;Fixture f;const auto memory=f.memory(50000);std::array<rm::Resource,256> resources{};
    for(std::size_t n=0;n<resources.size();++n){resources[n]=f.buffer(1000+n);f.bind(resources[n],memory);}
    require(f.tracker.storageBytes()>rm::Tracker::allocatedStorageBytes()&&f.tracker.storageBytes()<4*1024*1024,"resource payload pages grow with registrations rather than all hash buckets");
    measured=true;
    const auto before=f.tracker.stats().bindingListSteps;for(unsigned n=0;n<10000;++n){const auto facts=f.tracker.query(resources.back());require(facts.bindingBoundsKnown&&facts.overlapCount==255&&!facts.observedAliasFree,"dense alias counts come from mutation cache");++hotQueries;}
    require(f.tracker.stats().bindingListSteps==before,"dense queries do not rescan the binding list");
    for(std::size_t n=0;n<128;++n)f.tracker.bufferRetiring(resources[n].key);auto facts=f.tracker.query(resources.back());require(facts.overlapCount==127&&facts.firstOverlap.key.generation,"retirement updates cached count and current first alias");
    for(std::size_t n=128;n+1<resources.size();++n)f.tracker.bufferRetiring(resources[n].key);healthy(f.tracker.query(resources.back()));measured=false;denseMutationSteps=f.tracker.stats().bindingListSteps;
}
}
int main(){try{boundsAndRetirement();paddedAliasing();dedicated();failedBatches();allOrNothing();generationAndPending();rangeFailures();unsupported();knownCreationMetadata();coverageAndCapacity();shortMemoryChainAtCapacity();denseMemoryCache();
    require(allocations==0,"all measured fact mutation/query methods allocate zero bytes");std::cout<<"{\"stage\":\"resource-memory-facts-cpu\",\"passed\":true,\"cases\":"<<cases<<",\"checks\":"<<checks<<",\"hotPathAllocations\":"<<allocations
        <<",\"hotQueries\":"<<hotQueries<<",\"shortChainMutationSteps\":"<<shortChainMutationSteps<<",\"denseMutationSteps\":"<<denseMutationSteps<<",\"allocatedStorageBytes\":"<<rm::Tracker::allocatedStorageBytes()
        <<",\"vulkanLoaded\":false,\"gpuTouched\":false,\"productionLinked\":false,\"resourceLifetimePinned\":false,\"ownerKnown\":false,\"gpuComplete\":false,\"copyPermitted\":false}\n";return 0;
    }catch(const std::exception& failure){measured=false;std::cerr<<"case="<<cases<<" checks="<<checks<<" "<<failure.what()<<'\n';return 1;}}
