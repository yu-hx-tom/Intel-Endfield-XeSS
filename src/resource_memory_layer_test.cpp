// All calls below use production wrappers and own-process downstream stubs.
#include "vulkan_observer.cpp"
#include <iostream>
#include <memory>
#include <stdexcept>

namespace memory_layer_fixture {
namespace mo=xess_vk::resource_memory_observer;namespace rm=xess_vk::resource_memory_facts;
unsigned checks{},calls{},imageQueries{},bufferQueries{};
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class T>T handle(std::uintptr_t n){return reinterpret_cast<T>(n);}
int marker;struct Dispatch {void* key;};Dispatch deviceObject{&marker},queueObject{&marker},instanceObject{&marker};
const auto device=reinterpret_cast<VkDevice>(&deviceObject);const auto queue=reinterpret_cast<VkQueue>(&queueObject);const auto instance=reinterpret_cast<VkInstance>(&instanceObject);
enum Tag {Image,Buffer,Allocation,DestroyImage,DestroyBuffer,FreeMemory,BindImage,BindImage2,BindImageKhr,BindBuffer,BindBuffer2,BindBufferKhr,Sparse};
Tag lastTag{};std::array<const void*,4> pointers{};std::array<std::uint64_t,4> numbers{};
std::uintptr_t returned{};VkResult chosen=VK_SUCCESS;mo::Context* active{};const char* unavailable{};
void noLocks(){require(TryAcquireSRWLockExclusive(&stateLock)!=0,"stateLock released across original/query");ReleaseSRWLockExclusive(&stateLock);if(active)(void)mo::stats(active);}
void hit(Tag tag){++calls;lastTag=tag;pointers={};numbers={};noLocks();SetLastError(0x76543210);}
void start(){calls=0;}
void once(Tag tag){require(calls==1&&lastTag==tag,"one exact original called independently of extra readonly query");require(GetLastError()==0x76543210,"original driver LastError preserved across additional query and metadata");}
template<Tag tag,class Info,class Object>VkResult create(VkDevice d,const Info* info,const VkAllocationCallbacks* a,Object* out){hit(tag);pointers={d,info,a,out};*out=handle<Object>(returned);return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL image(VkDevice d,const VkImageCreateInfo* i,const VkAllocationCallbacks* a,VkImage* o){return create<Image>(d,i,a,o);}
VKAPI_ATTR VkResult VKAPI_CALL buffer(VkDevice d,const VkBufferCreateInfo* i,const VkAllocationCallbacks* a,VkBuffer* o){return create<Buffer>(d,i,a,o);}
VKAPI_ATTR VkResult VKAPI_CALL allocation(VkDevice d,const VkMemoryAllocateInfo* i,const VkAllocationCallbacks* a,VkDeviceMemory* o){return create<Allocation>(d,i,a,o);}
template<Tag tag,class Object>void destroy(VkDevice d,Object object,const VkAllocationCallbacks* a){hit(tag);pointers={d,a};numbers[0]=bits(object);}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice d,VkImage i,const VkAllocationCallbacks* a){destroy<DestroyImage>(d,i,a);}
VKAPI_ATTR void VKAPI_CALL destroyBuffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks* a){destroy<DestroyBuffer>(d,b,a);}
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice d,VkDeviceMemory m,const VkAllocationCallbacks* a){destroy<FreeMemory>(d,m,a);}
VKAPI_ATTR void VKAPI_CALL imageRequirements(VkDevice d,VkImage image,VkMemoryRequirements* out){++imageQueries;noLocks();require(d==device&&bits(image)==returned,"additional image query uses actual newly returned handle");*out={1024,64,1};SetLastError(0xdeadcafe);}
VKAPI_ATTR void VKAPI_CALL bufferRequirements(VkDevice d,VkBuffer buffer,VkMemoryRequirements* out){++bufferQueries;noLocks();require(d==device&&bits(buffer)==returned,"additional buffer query uses actual newly returned handle");*out={512,128,1};SetLastError(0xdeadcafe);}
VKAPI_ATTR VkResult VKAPI_CALL bindImage(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize o){hit(BindImage);pointers[0]=d;numbers={bits(i),bits(m),o};return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL bindBuffer(VkDevice d,VkBuffer b,VkDeviceMemory m,VkDeviceSize o){hit(BindBuffer);pointers[0]=d;numbers={bits(b),bits(m),o};return chosen;}
template<Tag tag,class Info>VKAPI_ATTR VkResult VKAPI_CALL bind2(VkDevice d,std::uint32_t n,const Info* i){hit(tag);pointers={d,i};numbers[0]=n;return chosen;}
VKAPI_ATTR VkResult VKAPI_CALL sparse(VkQueue q,std::uint32_t n,const VkBindSparseInfo* i,VkFence f){hit(Sparse);pointers={q,i};numbers={n,bits(f)};return chosen;}
VKAPI_ATTR void VKAPI_CALL noop(){}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice,const char* name){return unavailable&&std::strcmp(unavailable,name)==0?nullptr:reinterpret_cast<PFN_vkVoidFunction>(noop);}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance,const char* name){return unavailable&&std::strcmp(unavailable,name)==0?nullptr:reinterpret_cast<PFN_vkVoidFunction>(noop);}
struct Fixture {
    DeviceState state{};InstanceState parent{};std::unique_ptr<xess_vk::image_inventory::Inventory<>> inventory;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};VkMemoryAllocateInfo allocationInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};VkAllocationCallbacks allocator{};
    VkImage image{};VkBuffer buffer{};VkDeviceMemory memory{};
    Fixture(){state.device=device;state.key=&marker;state.gdpa=gdpa;parent.instance=instance;parent.key=&marker;parent.gipa=gipa;
        state.createImage=::memory_layer_fixture::image;state.destroyImage=destroyImage;state.createBuffer=::memory_layer_fixture::buffer;state.destroyBuffer=destroyBuffer;state.allocateMemory=allocation;state.freeMemory=freeMemory;
        state.imageMemoryRequirements=imageRequirements;state.bufferMemoryRequirements=bufferRequirements;state.bindImageMemory=bindImage;state.bindBufferMemory=bindBuffer;
        state.bindImageMemory2=bind2<BindImage2,VkBindImageMemoryInfo>;state.bindImageMemory2Khr=bind2<BindImageKhr,VkBindImageMemoryInfo>;
        state.bindBufferMemory2=bind2<BindBuffer2,VkBindBufferMemoryInfo>;state.bindBufferMemory2Khr=bind2<BindBufferKhr,VkBindBufferMemoryInfo>;state.bindSparse=sparse;
        inventory=std::make_unique<xess_vk::image_inventory::Inventory<>>();state.imageInventory=inventory.get();
        VkPhysicalDeviceMemoryProperties properties{};properties.memoryTypeCount=properties.memoryHeapCount=1;properties.memoryTypes[0]={VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0};properties.memoryHeaps[0]={1ull<<30,VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
        rm::Config config;config.deviceScope=rm::DeviceScope::SinglePhysicalDevice;config.bindingCoverage=rm::BindingCoverage::CompleteOrdinaryCalls;config.bufferImageGranularity=64;
        state.resourceMemory=mo::create(device,properties,config,nullptr);require(state.resourceMemory!=nullptr,"memory context allocated");devices=&state;instances=&parent;active=state.resourceMemory;chosen=VK_SUCCESS;imageQueries=bufferQueries=0;unavailable=nullptr;
        imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;imageInfo.extent={16,8,1};imageInfo.mipLevels=imageInfo.arrayLayers=1;imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;imageInfo.tiling=VK_IMAGE_TILING_OPTIMAL;imageInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;imageInfo.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.size=256;bufferInfo.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bufferInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;allocationInfo.allocationSize=4096;allocator.pUserData=this;
    }
    ~Fixture(){active=nullptr;devices=nullptr;instances=nullptr;mo::destroy(state.resourceMemory);}
    void createObjects(){returned=0x101;start();require(vkCreateImage(device,&imageInfo,&allocator,&image)==VK_SUCCESS,"actual image success");once(Image);require(pointers[1]==&imageInfo&&pointers[2]==&allocator&&pointers[3]==&image,"image original input/output/allocator pointers");
        returned=0x102;start();require(vkCreateBuffer(device,&bufferInfo,&allocator,&buffer)==VK_SUCCESS,"actual buffer success");once(Buffer);
        returned=0x103;start();require(vkAllocateMemory(device,&allocationInfo,&allocator,&memory)==VK_SUCCESS,"actual allocation success");once(Allocation);}
    auto facts(){const auto r=inventory->lookup(bits(image)).record;return mo::query(state.resourceMemory,{r.image,r.generation});}
};
void actualRequirementsAndAlias(){Fixture f;f.createObjects();require(imageQueries==1&&bufferQueries==1&&mo::stats(f.state.resourceMemory).requirementsObserved==2,"one unhooked additional real query for each ordinary newly created resource");
    start();require(vkBindImageMemory(device,f.image,f.memory,0)==VK_SUCCESS,"actual image bind");once(BindImage);require(numbers[0]==bits(f.image)&&numbers[1]==bits(f.memory)&&numbers[2]==0,"core image bind arguments unchanged");
    auto facts=f.facts();require(facts.binding.bindingBoundsKnown&&facts.binding.observedAliasFree&&facts.binding.size==1024&&facts.binding.end==1024,"driver requirement size used, never width/format estimated");
    start();require(vkBindBufferMemory(device,f.buffer,f.memory,512)==VK_SUCCESS,"actual buffer alias bind");once(BindBuffer);facts=f.facts();
    require(!facts.binding.observedAliasFree&&facts.binding.overlapCount==1&&(facts.localIssues&rm::bit(rm::Issue::Overlap))&&!facts.globalIssues,"same actual allocation image/buffer overlap is local and refused");
    start();vkDestroyBuffer(device,f.buffer,&f.allocator);once(DestroyBuffer);require(f.facts().binding.observedAliasFree,"exact buffer retirement removes ordinary overlap before original destroy");
    start();vkFreeMemory(device,f.memory,&f.allocator);once(FreeMemory);require(!f.facts().binding.observedAliasFree,"freed actual memory generation cannot stay alias-free");}
void aliasesFailureAndFormatList(){for(bool khr:{false,true}){Fixture f;VkFormat format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};list.viewFormatCount=1;list.pViewFormats=&format;f.imageInfo.pNext=&list;f.createObjects();
    VkBindImageMemoryInfo image{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};image.image=f.image;image.memory=f.memory;start();require((khr?vkBindImageMemory2KHR(device,1,&image):vkBindImageMemory2(device,1,&image))==VK_SUCCESS,"actual image core/KHR2 bind");once(khr?BindImageKhr:BindImage2);require(pointers[1]==&image&&numbers[0]==1,"bind2 original full pointer/count");
    VkDeviceMemory independent{};returned=0x104;start();require(vkAllocateMemory(device,&f.allocationInfo,nullptr,&independent)==VK_SUCCESS,"independent ordinary allocation");once(Allocation);
    VkBindBufferMemoryInfo buffer{VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO};buffer.buffer=f.buffer;buffer.memory=independent;
    start();require((khr?vkBindBufferMemory2KHR(device,1,&buffer):vkBindBufferMemory2(device,1,&buffer))==VK_SUCCESS,"actual independent buffer core/KHR2");once(khr?BindBufferKhr:BindBuffer2);
    require(f.facts().binding.observedAliasFree&&imageQueries==1,"benign format-list image and independent allocation do not poison source");
    chosen=VK_ERROR_OUT_OF_DEVICE_MEMORY;start();require((khr?vkBindImageMemory2KHR(device,1,&image):vkBindImageMemory2(device,1,&image))==chosen,"literal failed batch result");once(khr?BindImageKhr:BindImage2);
    require(!f.facts().binding.observedAliasFree&&(f.facts().binding.issues&rm::bit(rm::Issue::BindingUnknown))&&mo::stats(f.state.resourceMemory).uncertainBatches==1,"failed batch cannot pretend no partial driver effect");
    for(const char* name:{"vkBindImageMemory2","vkBindImageMemory2KHR","vkBindBufferMemory2","vkBindBufferMemory2KHR"}){require(vkGetDeviceProcAddr(device,name)!=nullptr&&vkGetInstanceProcAddr(instance,name)!=nullptr,"core/KHR hook exposed independently through GDPA/GIPA");unavailable=name;require(!vkGetDeviceProcAddr(device,name)&&!vkGetInstanceProcAddr(instance,name),"absent alias availability not invented");unavailable=nullptr;}}
}
void unsupportedAndMissing(){
    {Fixture f;f.imageInfo.flags=VK_IMAGE_CREATE_SPARSE_BINDING_BIT;f.createObjects();require(imageQueries==0,"unsafe sparse image gets no extra requirements query");start();require(vkBindImageMemory(device,f.image,f.memory,0)==VK_SUCCESS,"sparse ordinary bind forwarded");once(BindImage);require(!f.facts().binding.observedAliasFree,"sparse is explicitly unqualified");}
    {Fixture f;f.state.imageMemoryRequirements=nullptr;f.createObjects();start();require(vkBindImageMemory(device,f.image,f.memory,0)==VK_SUCCESS,"missing requirement bind still forwarded");once(BindImage);require(f.facts().binding.issues&rm::bit(rm::Issue::MissingRequirements),"missing actual function remains explicit unknown");}
    {Fixture f;f.createObjects();VkBindSparseInfo info{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};start();require(vkQueueBindSparse(queue,1,&info,handle<VkFence>(0x909))==VK_SUCCESS,"sparse original forwarded");once(Sparse);require(pointers[1]==&info&&numbers[1]==0x909&&mo::stats(f.state.resourceMemory).globalIssues,"unsupported sparse mechanism loses ordinary binding coverage");}
    {Fixture f;VkImportMemoryWin32HandleInfoKHR external{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};f.allocationInfo.pNext=&external;f.createObjects();start();require(vkBindImageMemory(device,f.image,f.memory,0)==VK_SUCCESS,"external allocation bind forwarded");once(BindImage);require(!f.facts().binding.observedAliasFree,"external memory is explicitly unqualified");}
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};create.pNext=&features;
    require(mo::deviceScope(&create)==rm::DeviceScope::SinglePhysicalDevice,"ordinary feature pNext does not invent device group");VkDeviceGroupDeviceCreateInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO};features.pNext=&group;require(mo::deviceScope(&create)==rm::DeviceScope::DeviceGroup,"explicit effective device group rejected");
}
}
int main(){using namespace memory_layer_fixture;try{SetEnvironmentVariableW(L"XESS_OBSERVER_LOG",nullptr);InitOnceExecuteOnce(&logOnce,initializeLog,nullptr,nullptr);actualRequirementsAndAlias();aliasesFailureAndFormatList();unsupportedAndMissing();std::cout<<"{\"stage\":\"resource-memory-actual-wrappers\",\"passed\":true,\"checks\":"<<checks<<",\"realVulkanCalls\":0,\"gpuTouched\":false,\"gameTouched\":false}\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
