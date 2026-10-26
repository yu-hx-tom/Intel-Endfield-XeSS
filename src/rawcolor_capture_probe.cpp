#include "rawcolor_capture_probe.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <new>

namespace xess_vk::rawcolor_capture_probe {
namespace {
namespace bc=borrowed_capture_completion;namespace di=descriptor_image_observer;
struct ErrorGuard {DWORD saved=GetLastError();~ErrorGuard(){SetLastError(saved);}};
template<class T>T handle(std::uint64_t n) noexcept{return reinterpret_cast<T>(n);}
bool sameRange(const VkImageSubresourceRange& r,VkImageAspectFlags a) noexcept{return r.aspectMask==a&&!r.baseMipLevel&&!r.baseArrayLayer&&r.levelCount==1&&r.layerCount==1;}
bool recordingSupported(const CommandRecordingFacts& f) noexcept {
    return f.device&&f.contextIncarnation&&f.rpRecordingGeneration&&f.command&&f.allocationKnown&&f.poolKnown&&f.beginFlagsKnown
        &&f.stateKnown&&f.recording&&f.level==VK_COMMAND_BUFFER_LEVEL_PRIMARY&&f.queueFamily==0&&f.scopeKnown&&!f.inRenderPass&&!f.inDynamicRendering
        &&!(f.poolFlags&VK_COMMAND_POOL_CREATE_PROTECTED_BIT)&&!(f.beginUsageFlags&~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)
        &&!(f.reasons&~bit(CommandRecordingReason::NotOneTime));
}
bool functionsReady(const Functions& f) noexcept {return f.createImage&&f.destroyImage&&f.imageRequirements&&f.createBuffer&&f.destroyBuffer&&f.bufferRequirements
    &&f.allocateMemory&&f.freeMemory&&f.bindImageMemory&&f.bindBufferMemory&&f.createPool&&f.destroyPool&&f.allocateCommands&&f.beginCommand&&f.endCommand
    &&f.createFence&&f.destroyFence&&f.fenceStatus&&f.waitFences&&f.barrier&&f.copyImage&&f.copyImageToBuffer&&f.submit&&f.mapMemory&&f.unmapMemory;}
VkImageMemoryBarrier imageBarrier(VkImage image,VkImageLayout old,VkImageLayout next,VkAccessFlags src,VkAccessFlags dst) noexcept {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    b.oldLayout=old;b.newLayout=next;b.srcAccessMask=src;b.dstAccessMask=dst;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;return b;
}
std::uint32_t memoryType(const VkPhysicalDeviceMemoryProperties& p,std::uint32_t bits,VkMemoryPropertyFlags required,bool preferCoherent) noexcept {
    std::uint32_t fallback=VK_MAX_MEMORY_TYPES;
    for(std::uint32_t n=0;n<std::min(p.memoryTypeCount,static_cast<std::uint32_t>(VK_MAX_MEMORY_TYPES));++n){const auto& t=p.memoryTypes[n];
        if(!(bits&(1u<<n))||(t.propertyFlags&required)!=required||(t.propertyFlags&VK_MEMORY_PROPERTY_PROTECTED_BIT)||t.heapIndex>=p.memoryHeapCount
            ||(p.memoryHeaps[t.heapIndex].flags&VK_MEMORY_HEAP_MULTI_INSTANCE_BIT))continue;
        if(!preferCoherent||(t.propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return n;fallback=n;
    }
    return fallback;
}
bool requirementsKnown(const VkMemoryRequirements& r) noexcept{return r.size&&r.size<=maxCaptureBytes*4&&r.alignment&&!(r.alignment&(r.alignment-1))&&r.memoryTypeBits;}
}
struct Context {
    Config config;bc::Context completion;Status state;
    VkCommandPool pool{};VkCommandBuffer readback{};void* mapped{};VkDeviceSize stagingAllocationSize{};bool coherent{},attempted{},terminal{},closing{};
    std::uint64_t rawCandidates{},queryAttempts{};std::uint64_t refusalMaskEmitted{};
    explicit Context(const Config& c):config(c){}
};
namespace {
void log(Context& c,const char* event,std::uint64_t reasons,VkResult driver=VK_SUCCESS) noexcept {
    if(!c.config.sink)return;char line[768];const auto n=[](std::uint64_t v){return static_cast<unsigned long long>(v);};
    const auto size=std::snprintf(line,sizeof(line),"{\"event\":\"%s\",\"device\":\"0x%llx\",\"reasons\":%llu,\"driver\":%d,\"prepared\":%s,\"source_inserted\":%s,\"staging_writes\":%llu,\"files_written\":%llu,\"cleanup\":%s,\"quarantine\":%s,\"xefg_integrated\":false}",
        event,n(reinterpret_cast<std::uint64_t>(c.config.device)),n(reasons),int(driver),c.state.prepared?"true":"false",c.state.sourceInserted?"true":"false",n(c.state.stagingCopies),n(c.state.filesWritten),c.state.shutdownCleanupComplete?"true":"false",c.state.shutdownQuarantined?"true":"false");
    if(size>0&&std::size_t(size)<sizeof(line))try{c.config.sink(line);}catch(...){}
}
Result refused(Context& c,Reason reason,VkResult driver=VK_SUCCESS,bool terminal=false) noexcept {
    const auto reasons=bit(reason);c.state.reasons|=reasons;if(terminal)c.terminal=true;
    if(!(c.refusalMaskEmitted&reasons)){c.refusalMaskEmitted|=reasons;log(c,"rawcolor_probe_refused",reasons,driver);}return {reasons,driver,false};
}
void cleanup(Context& c) noexcept {
    auto& f=c.config.functions;const auto device=c.config.device;
    if(c.mapped){f.unmapMemory(device,c.state.stagingMemory);c.mapped=nullptr;}
    if(c.pool){f.destroyPool(device,c.pool,nullptr);c.pool=VK_NULL_HANDLE;c.readback=VK_NULL_HANDLE;}
    if(c.state.ownFence){f.destroyFence(device,c.state.ownFence,nullptr);c.state.ownFence=VK_NULL_HANDLE;}
    if(c.state.drainFence){f.destroyFence(device,c.state.drainFence,nullptr);c.state.drainFence=VK_NULL_HANDLE;}
    if(c.state.staging){f.destroyBuffer(device,c.state.staging,nullptr);c.state.staging=VK_NULL_HANDLE;}
    if(c.state.ownedImage){f.destroyImage(device,c.state.ownedImage,nullptr);c.state.ownedImage=VK_NULL_HANDLE;}
    if(c.state.stagingMemory){f.freeMemory(device,c.state.stagingMemory,nullptr);c.state.stagingMemory=VK_NULL_HANDLE;}
    if(c.state.imageMemory){f.freeMemory(device,c.state.imageMemory,nullptr);c.state.imageMemory=VK_NULL_HANDLE;}
    c.state.released=true;
}
Result prepare(Context& c,VkExtent2D extent,VkDeviceMemory sourceMemory) noexcept {
    auto& f=c.config.functions;const auto device=c.config.device;VkResult driver=VK_SUCCESS;
    const auto failed=[&]() {cleanup(c);return refused(c,Reason::DriverFailed,driver,true);};
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    image.extent={extent.width,extent.height,1};image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;image.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    driver=f.createImage(device,&image,nullptr,&c.state.ownedImage);if(driver!=VK_SUCCESS){c.state.ownedImage=VK_NULL_HANDLE;return failed();}
    VkMemoryRequirements req{};f.imageRequirements(device,c.state.ownedImage,&req);if(!requirementsKnown(req)||req.size>c.config.targetFormatProperties.maxResourceSize){driver=VK_ERROR_INITIALIZATION_FAILED;return failed();}
    auto type=memoryType(c.config.memory,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,false);if(type==VK_MAX_MEMORY_TYPES){driver=VK_ERROR_FEATURE_NOT_PRESENT;return failed();}
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;
    driver=f.allocateMemory(device,&allocation,nullptr,&c.state.imageMemory);if(driver!=VK_SUCCESS){c.state.imageMemory=VK_NULL_HANDLE;return failed();}
    if(c.state.imageMemory==sourceMemory){driver=VK_ERROR_INITIALIZATION_FAILED;return failed();}
    driver=f.bindImageMemory(device,c.state.ownedImage,c.state.imageMemory,0);if(driver!=VK_SUCCESS)return failed();
    c.state.extent=extent;c.state.pixelBytes=VkDeviceSize(extent.width)*extent.height*4;
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=c.state.pixelBytes;buffer.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    driver=f.createBuffer(device,&buffer,nullptr,&c.state.staging);if(driver!=VK_SUCCESS){c.state.staging=VK_NULL_HANDLE;return failed();}f.bufferRequirements(device,c.state.staging,&req);
    if(!requirementsKnown(req)||req.size<c.state.pixelBytes){driver=VK_ERROR_INITIALIZATION_FAILED;return failed();}
    type=memoryType(c.config.memory,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,true);if(type==VK_MAX_MEMORY_TYPES){driver=VK_ERROR_FEATURE_NOT_PRESENT;return failed();}
    c.coherent=(c.config.memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
    if(!c.coherent&&(!f.invalidate||!c.config.nonCoherentAtomSize)){driver=VK_ERROR_FEATURE_NOT_PRESENT;return failed();}
    allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;c.stagingAllocationSize=req.size;
    driver=f.allocateMemory(device,&allocation,nullptr,&c.state.stagingMemory);if(driver!=VK_SUCCESS){c.state.stagingMemory=VK_NULL_HANDLE;return failed();}
    if(c.state.stagingMemory==sourceMemory||c.state.stagingMemory==c.state.imageMemory){driver=VK_ERROR_INITIALIZATION_FAILED;return failed();}
    driver=f.bindBufferMemory(device,c.state.staging,c.state.stagingMemory,0);if(driver!=VK_SUCCESS)return failed();
    driver=f.mapMemory(device,c.state.stagingMemory,0,VK_WHOLE_SIZE,0,&c.mapped);if(driver!=VK_SUCCESS||!c.mapped){c.mapped=nullptr;return failed();}
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=c.config.queueFamily;
    driver=f.createPool(device,&pool,nullptr,&c.pool);if(driver!=VK_SUCCESS){c.pool=VK_NULL_HANDLE;return failed();}
    VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};commands.commandPool=c.pool;commands.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commands.commandBufferCount=1;
    driver=f.allocateCommands(device,&commands,&c.readback);if(driver!=VK_SUCCESS)return failed();
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    driver=f.beginCommand(c.readback,&begin);if(driver!=VK_SUCCESS)return failed();
    auto transition=imageBarrier(c.state.ownedImage,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    f.barrier(c.readback,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&transition);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};
    f.copyImageToBuffer(c.readback,c.state.ownedImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,c.state.staging,1,&copy);
    transition=imageBarrier(c.state.ownedImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.buffer=c.state.staging;host.offset=0;host.size=VK_WHOLE_SIZE;host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    f.barrier(c.readback,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,1,&transition);
    driver=f.endCommand(c.readback);if(driver!=VK_SUCCESS)return failed();
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};driver=f.createFence(device,&fence,nullptr,&c.state.ownFence);if(driver!=VK_SUCCESS){c.state.ownFence=VK_NULL_HANDLE;return failed();}
    driver=f.createFence(device,&fence,nullptr,&c.state.drainFence);if(driver!=VK_SUCCESS){c.state.drainFence=VK_NULL_HANDLE;return failed();}
    bc::PreparedOwner owner;owner.device=device;owner.queue=c.config.queue;owner.queueFamily=c.config.queueFamily;owner.image=c.state.ownedImage;owner.staging=c.state.staging;owner.readbackCommand=c.readback;owner.completionFence=c.state.ownFence;
    owner.originalQueueSubmit=f.submit;owner.originalFenceStatus=f.fenceStatus;owner.singlePhysicalDeviceKnown=owner.ordinaryUnprotectedQueueKnown=owner.uniqueExternallySynchronizedQueueKnown=true;
    owner.imageAndStagingOwned=owner.readbackExecutableKnown=owner.readbackSameFamilyKnown=owner.readbackWritesStagingOnce=owner.readbackRestoresOwnedFinalLayout=true;
    owner.transferToHostBarrierKnown=owner.hostCoherenceResponsibilityKnown=owner.inlineTargetStartsUndefined=owner.inlineTargetRestoresFinalLayout=owner.inlineSourceRestoresOriginalLayout=true;
    const auto initialized=c.completion.initialize(owner);if(!initialized.accepted()){driver=initialized.driver;return failed();}
    c.state.prepared=true;c.state.released=false;return {};
}
unsigned char channel(std::uint32_t packed,unsigned bits) noexcept {
    const auto exponent=packed>>bits;const auto mantissa=packed&((1u<<bits)-1);double value;
    if(exponent==31)value=mantissa?0:1;else if(!exponent)value=std::ldexp(double(mantissa),-14-int(bits));
    else value=std::ldexp(1.0+double(mantissa)/double(1u<<bits),int(exponent)-15);
    value=std::clamp(value,0.0,1.0);return static_cast<unsigned char>(std::lround(value*255.0));
}
bool writeBmp(Context& c) noexcept {
    if(!c.config.outputBmpPath[0])return true;
    const auto* path=c.config.outputBmpPath.data();if(!((path[0]>=L'A'&&path[0]<=L'Z')||(path[0]>=L'a'&&path[0]<=L'z'))||path[1]!=L':'||(path[2]!=L'/'&&path[2]!=L'\\'))return false;
    const HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    const auto width=c.state.extent.width,height=c.state.extent.height,rowBytes=(width*3+3)&~3u;
    std::array<unsigned char,54> header{};header[0]='B';header[1]='M';
    const auto put=[&](std::size_t at,std::uint32_t value){for(unsigned k=0;k<4;++k)header[at+k]=static_cast<unsigned char>(value>>(8*k));};
    put(2,54+rowBytes*height);put(10,54);put(14,40);put(18,width);put(22,static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)));header[26]=1;header[28]=24;put(34,rowBytes*height);
    DWORD written{};bool ok=WriteFile(file,header.data(),DWORD(header.size()),&written,nullptr)&&written==header.size();
    std::array<unsigned char,4096*3> row{};const auto* pixels=static_cast<const std::uint32_t*>(c.mapped);
    for(std::uint32_t y=0;ok&&y<height;++y){for(std::uint32_t x=0;x<width;++x){const auto pixel=pixels[std::size_t(y)*width+x];row[x*3]=channel(pixel>>22,5);row[x*3+1]=channel((pixel>>11)&2047,6);row[x*3+2]=channel(pixel&2047,6);}
        ok=WriteFile(file,row.data(),rowBytes,&written,nullptr)&&written==rowBytes;
    }
    CloseHandle(file);if(ok)++c.state.filesWritten;return ok;
}
}
Context* create(const Config& config) noexcept {ErrorGuard error;try{return new Context(config);}catch(...){return nullptr;}}
bool canAttempt(Context* c) noexcept {ErrorGuard error;return c&&c->config.enabled&&!c->attempted&&!c->terminal&&!c->closing&&!c->state.sourceInserted;}
bool requestSourceQuery(void* user) noexcept {
    ErrorGuard error;auto* c=static_cast<Context*>(user);if(!canAttempt(c))return false;
    if(c->rawCandidates!=UINT64_MAX)++c->rawCandidates;
    if(c->queryAttempts>=64){refused(*c,Reason::Capacity,VK_SUCCESS,true);return false;}
    if(c->rawCandidates<=16||c->rawCandidates%1200==0){++c->queryAttempts;return true;}return false;
}
Result preDispatch(Context* c,const di::DispatchSourceFacts& source) noexcept {
    ErrorGuard error;if(!c)return {bit(Reason::Disabled),VK_SUCCESS,false};if(!canAttempt(c))return {bit(Reason::AlreadyCaptured),VK_SUCCESS,false};
    const auto& config=c->config;
    if(!functionsReady(config.functions))return refused(*c,Reason::MissingFunction,VK_SUCCESS,true);
    if(!config.singlePhysicalDeviceKnown||!config.uniqueOrdinaryExternallySynchronizedQueueKnown||config.queueFamily!=0||!config.queue
        ||!(config.queueFlags&VK_QUEUE_TRANSFER_BIT))return refused(*c,Reason::UnsupportedDeviceQueue,VK_SUCCESS,true);
    if(!config.targetFormatTransferSupported)return refused(*c,Reason::UnsupportedSource,VK_SUCCESS,true);
    if(!config.defaultDeviceAndInstanceAllocatorsKnown||!config.applicationDeviceMemoryCallbackAbsentKnown||!config.applicationInstanceCallbacksAbsentKnown)return refused(*c,Reason::UnsafeDeferredLifetime,VK_SUCCESS,true);
    const auto& e=source.evidence;if(!source.observed||!e.metadataAccepted||e.issues||e.count>e.bindings.size())return {bit(Reason::NotRawPattern),VK_SUCCESS,false};
    std::size_t color=e.count,depth=e.count,motion=e.count;
    for(std::size_t n=0;n<e.count;++n){const auto& b=e.bindings[n];if(b.setIndex||b.descriptorType!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)continue;
        if(b.binding==5&&(source.sourceMasks[n]&(1u<<unsigned(di::Source::RawDepth))))depth=n;
        if(b.binding==6&&(source.sourceMasks[n]&(1u<<unsigned(di::Source::RawColor))))color=n;
        if(b.binding==7&&(source.sourceMasks[n]&(1u<<unsigned(di::Source::RawMotion))))motion=n;
    }
    if(color==e.count||depth==e.count||motion==e.count)return {bit(Reason::NotRawPattern),VK_SUCCESS,false};
    const auto& b=e.bindings[color];const auto& d=e.bindings[depth];const auto& m=e.bindings[motion];const auto& f=source.recording;
    if(!recordingSupported(f)||f.device!=reinterpret_cast<std::uint64_t>(config.device)||f.command!=e.command.handle)return refused(*c,Reason::UnsupportedRecording);
    const auto& image=b.imageDescription;
    if(b.viewFormat!=VK_FORMAT_B10G11R11_UFLOAT_PACK32||image.format!=std::uint32_t(VK_FORMAT_B10G11R11_UFLOAT_PACK32)||!sameRange(b.range,VK_IMAGE_ASPECT_COLOR_BIT)
        ||image.depth!=1||image.mipLevels!=1||image.arrayLayers!=1||image.samples!=VK_SAMPLE_COUNT_1_BIT||image.flags||image.tiling!=VK_IMAGE_TILING_OPTIMAL||image.sharingMode!=VK_SHARING_MODE_EXCLUSIVE
        ||!(image.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)||b.viewType!=VK_IMAGE_VIEW_TYPE_2D||d.viewFormat!=VK_FORMAT_D32_SFLOAT_S8_UINT||!sameRange(d.range,VK_IMAGE_ASPECT_DEPTH_BIT)
        ||m.viewFormat!=VK_FORMAT_R16G16_SFLOAT||!sameRange(m.range,VK_IMAGE_ASPECT_COLOR_BIT)||d.imageDescription.width!=image.width||d.imageDescription.height!=image.height
        ||m.imageDescription.width!=image.width||m.imageDescription.height!=image.height||b.image==d.image||b.image==m.image||d.image==m.image)return refused(*c,Reason::UnsupportedSource);
    if(!image.width||!image.height||image.width>4096||image.height>4096||VkDeviceSize(image.width)*image.height*4>maxCaptureBytes
        ||image.width>config.targetFormatProperties.maxExtent.width||image.height>config.targetFormatProperties.maxExtent.height||config.targetFormatProperties.maxExtent.depth<1
        ||!config.targetFormatProperties.maxMipLevels||!config.targetFormatProperties.maxArrayLayers||!(config.targetFormatProperties.sampleCounts&VK_SAMPLE_COUNT_1_BIT))return refused(*c,Reason::Capacity);
    const auto& layout=source.layouts[color];
    if(!layout.queried||!layout.recordedLayoutResolved||!layout.recordedPrefixSupported||layout.issues||layout.image!=RecordedImageKey{b.image.handle,b.image.generation}
        ||layout.device!=f.device||layout.contextIncarnation!=f.contextIncarnation||layout.command!=f.command||layout.rpRecordingGeneration!=f.rpRecordingGeneration||layout.prefixOrder!=f.commonRecordOrder
        ||!sameRange(layout.resolvedLayoutRange,VK_IMAGE_ASPECT_COLOR_BIT)||layout.recordedLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)return refused(*c,Reason::LayoutUnknown);
    const auto& memory=source.memory[color];const auto& binding=memory.binding;
    if(!memory.queried||memory.localIssues||memory.globalIssues||binding.issues||!binding.bindingBoundsKnown||!binding.observedAliasFree||!binding.memory.handle||!binding.memory.generation
        ||binding.device!=config.device||binding.resource.kind!=resource_memory_facts::ResourceKind::Image||binding.resource.key!=resource_memory_facts::Key{b.image.handle,b.image.generation})return refused(*c,Reason::MemoryUnknown);
    c->attempted=true;if(config.claimProcessLease&&!config.claimProcessLease(config.leaseUser))return refused(*c,Reason::AlreadyCaptured,VK_SUCCESS,true);
    ++c->state.insertionAttempts;auto prepared=prepare(*c,{image.width,image.height},handle<VkDeviceMemory>(binding.memory.handle));if(!prepared.accepted())return prepared;
    if(c->state.observationUnsafe){cleanup(*c);return {bit(Reason::ObservationUnsafe),VK_SUCCESS,false};}
    bc::RecordingAdmission admission;admission.key={config.device,f.contextIncarnation,handle<VkCommandBuffer>(f.command),f.rpRecordingGeneration};admission.poolGeneration=f.poolGeneration;admission.queueFamily=f.queueFamily;
    admission.level=f.level;admission.actualBeginFlags=f.beginUsageFlags;admission.allocationKnown=f.allocationKnown;admission.poolKnown=f.poolKnown;admission.beginFlagsKnown=f.beginFlagsKnown;
    admission.currentlyRecordingKnown=admission.outsideRenderingKnown=admission.ordinaryUnprotectedPoolKnown=admission.controlSupported=true;
    const auto armed=c->completion.armBeforeSourceCommands(admission);if(!armed.accepted()){cleanup(*c);return refused(*c,Reason::CompletionRefused,armed.driver,true);}
    auto& fn=c->config.functions;const auto command=handle<VkCommandBuffer>(f.command);
    std::array<VkImageMemoryBarrier,2> transitions{
        imageBarrier(handle<VkImage>(b.image.handle),layout.recordedLayout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT),
        imageBarrier(c->state.ownedImage,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT)};
    fn.barrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,transitions.data());
    VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.extent={image.width,image.height,1};
    fn.copyImage(command,handle<VkImage>(b.image.handle),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,c->state.ownedImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    transitions={imageBarrier(handle<VkImage>(b.image.handle),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,layout.recordedLayout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT),
        imageBarrier(c->state.ownedImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT)};
    fn.barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,transitions.data());
    const auto recorded=c->completion.sourceCommandsRecorded();c->state.sourceInserted=true;++c->state.sourceCopies;
    if(!recorded.accepted())return refused(*c,Reason::CompletionRefused,recorded.driver,true);
    log(*c,"rawcolor_probe_inline_recorded",0);return {0,VK_SUCCESS,true};
}
bc::SubmitTicket preSubmit(Context* c,const bc::SubmitObservation& observation) noexcept {ErrorGuard error;return c&&c->state.sourceInserted&&!c->closing&&!c->state.observationUnsafe?c->completion.preSubmit(observation):bc::SubmitTicket{};}
Result afterOriginalResult(Context* c,const bc::SubmitTicket& ticket,VkResult driver,const bc::QueueWindow& window) noexcept {
    ErrorGuard error;if(!c||c->closing)return {};if(c->state.observationUnsafe)return {bit(Reason::ObservationUnsafe),driver,false};
    if(!c->state.sourceInserted){if(driver==VK_ERROR_DEVICE_LOST)return refused(*c,Reason::DriverFailed,driver,true);return {};}
    // A different CB may acquire the lease while an unrelated application
    // original is outside the caller's lock. That old call owns no ticket for
    // this lease and must not poll an unsubmitted private fence.
    if(!ticket.relevant&&driver!=VK_ERROR_DEVICE_LOST)return {};
    if(driver==VK_ERROR_DEVICE_LOST&&!ticket.contextSerial){observationUnsafe(c);return refused(*c,Reason::DriverFailed,driver,true);}
    const auto result=c->completion.afterOriginalResult(ticket,driver,window);if(!result.accepted())return refused(*c,Reason::CompletionRefused,result.driver);
    c->state.stagingCopies=c->completion.stats().privateSubmitSuccesses;return poll(c);
}
Result poll(Context* c) noexcept {
    ErrorGuard error;if(c&&c->state.observationUnsafe)return {bit(Reason::ObservationUnsafe),VK_SUCCESS,false};
    if(!c||!c->state.sourceInserted||c->state.released||c->state.pixelsWritten||c->terminal)return {};
    ++c->state.fencePolls;const auto result=c->completion.pollOwnCompletion();if(result.driver==VK_NOT_READY)return {bit(Reason::Pending),VK_NOT_READY,false};
    if(!result.accepted())return refused(*c,Reason::CompletionRefused,result.driver);
    if(!c->completion.status().readbackReady)return {bit(Reason::Pending),VK_NOT_READY,false};
    if(!c->coherent){VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=c->state.stagingMemory;range.offset=0;range.size=VK_WHOLE_SIZE;
        const auto driver=c->config.functions.invalidate(c->config.device,1,&range);if(driver!=VK_SUCCESS)return refused(*c,Reason::DriverFailed,driver,true);}
    // Read actual packed staging contents before the consumption transition,
    // even for a fixture with file output disabled.
    volatile std::uint32_t actualPixel=static_cast<const std::uint32_t*>(c->mapped)[0];(void)actualPixel;
    if(!writeBmp(*c))return refused(*c,Reason::OutputFailed,VK_SUCCESS,true);
    c->state.pixelsWritten=true;const auto consumed=c->completion.readbackConsumed();if(!consumed.accepted())return refused(*c,Reason::CompletionRefused,consumed.driver);
    log(*c,"rawcolor_probe_pixels_ready",0);return {};
}
void recordingRetired(Context* c,const bc::RecordingKey& key) noexcept {ErrorGuard error;if(c)c->completion.observeRecordingRetired(key);}
void queueIdle(Context* c,VkQueue queue,VkResult driver) noexcept {ErrorGuard error;if(c)c->completion.observeQueueIdle(queue,driver);}
void deviceIdle(Context* c,VkDevice device,VkResult driver) noexcept {ErrorGuard error;if(c)c->completion.observeDeviceIdle(device,driver);}
void observationUnsafe(Context* c) noexcept {ErrorGuard error;if(c){c->state.observationUnsafe=true;refused(*c,Reason::ObservationUnsafe,VK_SUCCESS,true);}}
Result beforeDeviceDestroy(Context* c,std::uint64_t timeout) noexcept {
    ErrorGuard error;if(!c)return {};c->closing=true;
    if(c->state.shutdownCleanupComplete)return {};
    if(!c->state.sourceInserted){if(c->state.ownedImage||c->state.stagingMemory)cleanup(*c);c->state.shutdownCleanupComplete=true;return {};}
    auto& fn=c->config.functions;
    if(!c->state.drainSubmitted){VkSubmitInfo drain{VK_STRUCTURE_TYPE_SUBMIT_INFO};const auto driver=fn.submit(c->config.queue,1,&drain,c->state.drainFence);
        if(driver==VK_SUCCESS)c->state.drainSubmitted=true;
        else if(driver!=VK_ERROR_DEVICE_LOST){c->state.shutdownDriver=driver;c->state.shutdownQuarantined=true;log(*c,"rawcolor_probe_shutdown_quarantine",bit(Reason::Quarantined),driver);return {bit(Reason::Quarantined),driver,false};}}
    // An actual own-fence wait is the only physical completion evidence used
    // here. Its DEVICE_LOST result permits in-use retirement without pixels.
    const auto driver=fn.waitFences(c->config.device,1,&c->state.drainFence,VK_TRUE,timeout);c->state.shutdownDriver=driver;
    if(driver==VK_SUCCESS||driver==VK_ERROR_DEVICE_LOST){cleanup(*c);c->state.shutdownCleanupComplete=true;log(*c,"rawcolor_probe_shutdown_released",0,driver);return {0,driver,false};}
    c->state.shutdownQuarantined=true;c->state.reasons|=bit(Reason::Quarantined);log(*c,"rawcolor_probe_shutdown_quarantine",bit(Reason::Quarantined),driver);return {bit(Reason::Quarantined),driver,false};
}
void deviceTerminated(Context* c) noexcept {ErrorGuard error;if(c){c->completion.observeDeviceTerminated(c->config.device);if(c->state.released)c->completion.markOwnerReleased();}}
bool destroy(Context* c) noexcept {ErrorGuard error;if(!c)return true;if(c->state.sourceInserted&&!c->state.shutdownCleanupComplete)return false;if(!c->state.released&&(c->state.ownedImage||c->state.stagingMemory))cleanup(*c);delete c;return true;}
Status status(Context* c) noexcept {ErrorGuard error;Status out;if(c){out=c->state;out.completion=c->completion.status();out.rawCandidates=c->rawCandidates;out.sourceQueryAttempts=c->queryAttempts;
    if(out.observationUnsafe){out.completion.uncertain=true;out.completion.readbackReady=false;out.completion.release=bc::ReleaseDisposition::Retain;}}return out;}
std::span<const std::uint32_t> pixels(Context* c) noexcept {ErrorGuard error;return c&&!c->state.observationUnsafe&&c->completion.status().readbackReady&&c->state.pixelsWritten&&c->mapped?std::span<const std::uint32_t>(static_cast<const std::uint32_t*>(c->mapped),std::size_t(c->state.pixelBytes/4)):std::span<const std::uint32_t>{};}
} // namespace xess_vk::rawcolor_capture_probe
