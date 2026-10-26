#include "resource_memory_observer.hpp"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
namespace {std::atomic<bool> counting{};std::atomic<std::size_t> allocations{};}
void* operator new(std::size_t n){if(counting)++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace {
namespace mo=xess_vk::resource_memory_observer;namespace rm=xess_vk::resource_memory_facts;
unsigned checks{};mo::Context* context{};std::array<char,2048> lastLine{};
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class T>T h(std::uint64_t n){return reinterpret_cast<T>(n);}
void sink(const char* line){(void)mo::stats(context);std::strncpy(lastLine.data(),line,lastLine.size()-1);SetLastError(0xdead);}
void run(){VkPhysicalDeviceMemoryProperties properties{};properties.memoryTypeCount=properties.memoryHeapCount=1;properties.memoryTypes[0]={VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0};properties.memoryHeaps[0]={1ull<<30,VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    rm::Config config;config.deviceScope=rm::DeviceScope::SinglePhysicalDevice;config.bindingCoverage=rm::BindingCoverage::CompleteOrdinaryCalls;config.bufferImageGranularity=64;
    context=mo::create(h<VkDevice>(1),properties,config,sink);require(context!=nullptr,"adapter creation");
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;image.extent={64,32,1};image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;image.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    xess_vk::image_inventory::Record record{10,1,{std::uint32_t(image.format),64,32,1,1,1,1,image.usage,0,VK_IMAGE_TILING_OPTIMAL,VK_SHARING_MODE_EXCLUSIVE,VK_IMAGE_LAYOUT_UNDEFINED}};
    VkMemoryRequirements requirements{1024,64,1};VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=4096;
    VkBindImageMemoryInfo bind{VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};bind.image=h<VkImage>(10);bind.memory=h<VkDeviceMemory>(20);bind.memoryOffset=64;
    SetLastError(0xbeef);
    mo::imageCreated(context,record,&image,VK_SUCCESS,&requirements);mo::memoryAllocated(context,bind.memory,&allocation,VK_SUCCESS);
    require(GetLastError()==0xbeef,"payload page registration preserves LastError");counting=true;allocations=0;
    const auto ticket=mo::beforeImageBind(context,rm::BindApi::ImageCore2,1,&bind);mo::afterBind(context,ticket,VK_SUCCESS);
    auto facts=mo::query(context,{10,1});mo::summary(context);counting=false;
    require(!allocations&&GetLastError()==0xbeef,"all adapter hot calls and unlocked sink preserve zero allocations and LastError");
    require(facts.queried&&facts.binding.bindingBoundsKnown&&facts.binding.observedAliasFree&&!facts.localIssues&&!facts.globalIssues&&facts.binding.offset==64&&facts.binding.size==1024&&facts.binding.end==1088,"owned bounds and alias facts retain actual requirements");
    require(!facts.binding.resourceLifetimePinned&&!facts.binding.ownerKnown&&!facts.binding.gpuComplete&&!facts.binding.copyPermitted,"memory observations never grant GPU/pin/copy");
    require(std::strstr(lastLine.data(),"\"images\":[1,1,16384")&&std::strstr(lastLine.data(),"\"requirements_observed\":1")&&std::strstr(lastLine.data(),"\"copy_permitted\":false"),"full default-domain capacity and requirements summary exposed without resource spam");
    mo::memoryFreeing(context,bind.memory);facts=mo::query(context,{10,1});require(!facts.binding.observedAliasFree&&facts.binding.issues,"exact allocation retirement invalidates binding facts");
    mo::coverageLost(context);facts=mo::query(context,{10,1});require(facts.globalIssues&&!facts.binding.observedAliasFree,"unknown binding coverage is global fail-closed");
    mo::retire(context);require(mo::query(context,{10,1}).globalIssues,"retired adapter refuses query");mo::destroy(context);context=nullptr;
    VkImageFormatListCreateInfo formats{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};image.pNext=&formats;require(mo::imageRequirementsQuerySupported(&image),"bounded ordinary format-list whole-resource query supported");
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};image.pNext=&external;require(!mo::imageRequirementsQuerySupported(&image),"external image requirements are not queried by ordinary recipe");
}
}
int main(){try{run();std::cout<<"{\"stage\":\"resource-memory-observer\",\"passed\":true,\"checks\":"<<checks<<",\"hotPathAllocations\":"<<allocations<<",\"gpuTouched\":false,\"gameTouched\":false}\n";return 0;}catch(const std::exception& e){counting=false;mo::destroy(context);std::cerr<<e.what()<<'\n';return 1;}}
