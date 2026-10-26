#include "render_pass_metadata.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>

// Instrument only the pure fixture's hot calls, after constructing storage.
namespace { std::atomic<bool> countAllocations{}; std::atomic<std::size_t> allocations{}, allocationBytes{};
    std::size_t constructorAllocations{},constructorHeapBytes{}; }
void* operator new(std::size_t size) {
    if (countAllocations) { ++allocations;allocationBytes+=size; }
    if (void* value=std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value,std::size_t) noexcept { std::free(value); }
void operator delete[](void* value,std::size_t) noexcept { std::free(value); }

namespace {
using namespace xess_vk::render_pass_metadata;
unsigned checks{},cases{};
void require(bool value,const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void accepted(Result result) { require(result.accepted(),"expected supported metadata"); }
void issue(Result result,Issue reason) { require((result.issues & bit(reason))!=0,"expected rejection reason"); }
void refused(const Batch& batch,Issue reason) {
    require(!batch.metadataAccepted && !batch.count && (batch.issues & bit(reason)),"refusal must expose no operations");
    require(!batch.copyPermitted && !batch.gpuComplete && !batch.resourceLifetimePinned && !batch.lastWriterKnown
        && !batch.completeHookCoverage,"metadata must not grant GPU/copy claims");
}
template<class T> T handle(Handle value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
Config small() { Config c; c.images=8;c.views=16;c.renderPasses=8;c.framebuffers=8;c.commands=8;return c; }
struct Fixture {
    Metadata metadata;
    ImageDescription image;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkAttachmentDescription attachment{};
    VkAttachmentReference reference{};
    VkSubpassDescription subpass{};
    VkRenderPassCreateInfo renderPass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    VkImageView attachmentView=handle<VkImageView>(20);
    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    VkCommandBufferBeginInfo commandBegin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Key command{50,1};
    Fixture(Config config=small(),bool depth=false,bool registerObjects=true):metadata(config) {
        image.key={10,1}; image.format=depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        image.type=VK_IMAGE_TYPE_2D;image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
        image.sharing=VK_SHARING_MODE_EXCLUSIVE;image.unsupportedPNext=false;
        image.extent={64,32,1}; image.mipLevels=image.arrayLayers=1;
        image.usage=depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        image.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        view.image=handle<VkImage>(10);view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=image.format;
        view.subresourceRange={depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        attachment.format=image.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        reference.attachment=0;reference.layout=depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        if (depth) subpass.pDepthStencilAttachment=&reference;
        else { subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference; }
        renderPass.attachmentCount=1;renderPass.pAttachments=&attachment;renderPass.subpassCount=1;renderPass.pSubpasses=&subpass;
        framebuffer.renderPass=handle<VkRenderPass>(30);framebuffer.attachmentCount=1;framebuffer.pAttachments=&attachmentView;
        framebuffer.width=64;framebuffer.height=32;framebuffer.layers=1;
        begin.renderPass=handle<VkRenderPass>(30);begin.framebuffer=handle<VkFramebuffer>(40);
        begin.renderArea={{0,0},{64,32}};
        if (registerObjects) registerAll();
    }
    void registerAll() {
        accepted(metadata.observeImage(image,VK_SUCCESS));accepted(metadata.createView(20,&view,VK_SUCCESS));
        accepted(metadata.createRenderPass(30,&renderPass,VK_SUCCESS));accepted(metadata.createFramebuffer(40,&framebuffer,VK_SUCCESS));
    }
    void start() { accepted(metadata.beginCommand(command,VK_COMMAND_BUFFER_LEVEL_PRIMARY,&commandBegin,VK_SUCCESS)); }
    void record(std::uint64_t startOrder=2,std::uint64_t endOrder=4) {
        accepted(metadata.beginRenderPass(command,startOrder,&begin,VK_SUBPASS_CONTENTS_INLINE));
        accepted(metadata.endRenderPass(command,endOrder));
    }
    Batch submit(VkResult driver=VK_SUCCESS) { accepted(metadata.endCommand(command,VK_SUCCESS));return metadata.submitted(command,driver); }
};

void happyPaths() {
    ++cases;
    for (bool depth : {false,true}) {
        Fixture f(small(),depth);f.start();f.record();auto b=f.submit();
        require(b.metadataAccepted && b.driverSubmissionSucceeded && b.count==3 && !b.issues,"three operations accepted");
        require(b.operations[0].sourceKind==SourceKind::InitialLayoutAssertion
            && b.operations[1].sourceKind==SourceKind::SubpassReferenceTransition
            && b.operations[2].sourceKind==SourceKind::FinalLayoutTransition,"explicit source kinds");
        require(b.operations[0].oldLayout==VK_IMAGE_LAYOUT_UNDEFINED && b.operations[0].newLayout==VK_IMAGE_LAYOUT_UNDEFINED,"initial is assertion only");
        require(b.operations[1].oldLayout==VK_IMAGE_LAYOUT_UNDEFINED && b.operations[1].newLayout==f.reference.layout
            && b.operations[2].oldLayout==f.reference.layout && b.operations[2].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,"exact layout chain");
        require(b.operations[0].recordOrder==2 && b.operations[1].recordOrder==2 && b.operations[2].recordOrder==4
            && b.operations[1].sequenceWithinCall==1,"mergeable recording order");
        for (const auto& op : {b.operations[0],b.operations[1],b.operations[2]})
            require(op.image==f.image.key && op.command==f.command && op.view.handle==20 && op.renderPass.handle==30
                && op.framebuffer.handle==40 && op.range.levelCount==1 && op.range.layerCount==1,"provenance and full subresource");
        require(!b.gpuComplete && !b.copyPermitted && !b.lastWriterKnown && !b.resourceLifetimePinned && !b.completeHookCoverage,"no GPU claims");
        // Non-one-time executable recordings may legitimately be submitted again.
        require(f.metadata.submitted(f.command,VK_SUCCESS).metadataAccepted,"ordinary repeat submit");
    }
    Fixture f; f.start();f.begin.renderArea={{5,6},{8,9}};f.record();auto b=f.submit();
    require(b.operations[2].range.baseMipLevel==0 && b.operations[2].range.layerCount==1,"render area does not shrink layout subresource");
}
void deepCopiesAndInput() {
    ++cases;Fixture f(small(),false,false);
    f.subpass.colorAttachmentCount=0;f.subpass.pColorAttachments=nullptr;f.subpass.inputAttachmentCount=1;f.subpass.pInputAttachments=&f.reference;
    f.reference.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;f.registerAll();
    f.attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;f.reference.layout=VK_IMAGE_LAYOUT_GENERAL;
    f.attachmentView=handle<VkImageView>(999);f.view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    f.start();f.record();auto b=f.submit();
    require(b.metadataAccepted && b.operations[1].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        && b.operations[2].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,"create structures were deep copied");
}
void renderPass2() {
    ++cases;Fixture f(small(),false,false);
    accepted(f.metadata.observeImage(f.image,VK_SUCCESS));accepted(f.metadata.createView(20,&f.view,VK_SUCCESS));
    VkAttachmentDescription2 attachment{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
    attachment.format=f.attachment.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference2 ref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};ref.attachment=0;ref.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription2 sub{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
    VkRenderPassCreateInfo2 rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&sub;
    accepted(f.metadata.createRenderPass2(30,&rp,VK_SUCCESS));accepted(f.metadata.createFramebuffer(40,&f.framebuffer,VK_SUCCESS));
    f.start();VkSubpassBeginInfo sb{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};sb.contents=VK_SUBPASS_CONTENTS_INLINE;
    VkSubpassEndInfo se{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
    accepted(f.metadata.beginRenderPass2(f.command,1,&f.begin,&sb));accepted(f.metadata.endRenderPass2(f.command,2,&se));
    require(f.submit().operations[2].newLayout==VK_IMAGE_LAYOUT_GENERAL,"RP2 path has same semantics");
    sub.viewMask=1;issue(f.metadata.createRenderPass2(31,&rp,VK_SUCCESS),Issue::UnsupportedAttachment);sub.viewMask=0;
    ref.pNext=&se;issue(f.metadata.createRenderPass2(32,&rp,VK_SUCCESS),Issue::UnsupportedExtension);ref.pNext=nullptr;
    attachment.pNext=&se;issue(f.metadata.createRenderPass2(33,&rp,VK_SUCCESS),Issue::UnsupportedExtension);attachment.pNext=nullptr;
    rp.correlatedViewMaskCount=1;issue(f.metadata.createRenderPass2(34,&rp,VK_SUCCESS),Issue::UnsupportedAttachment);
}
void failedDriver() {
    ++cases;Fixture f(small(),false,false);
    const auto before=f.metadata.stats().registrations;
    issue(f.metadata.observeImage(f.image,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    issue(f.metadata.createView(20,nullptr,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    issue(f.metadata.createRenderPass(30,nullptr,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    issue(f.metadata.createFramebuffer(40,nullptr,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    require(f.metadata.stats().registrations==before,"failed create commits no registration");f.registerAll();f.start();f.record();
    accepted(f.metadata.endCommand(f.command,VK_SUCCESS));auto b=f.metadata.submitted(f.command,VK_ERROR_OUT_OF_HOST_MEMORY);
    refused(b,Issue::DriverFailed);require(!b.driverSubmissionSucceeded,"failed submission reported literally");
    require(f.metadata.submitted(f.command,VK_SUCCESS).metadataAccepted,"failed submit consumed no recording");
    f.metadata.resetCommand(50,VK_ERROR_OUT_OF_HOST_MEMORY);
    require(f.metadata.submitted(f.command,VK_SUCCESS).metadataAccepted,"failed reset preserves recording");
    f.command.generation=2;issue(f.metadata.beginCommand(f.command,VK_COMMAND_BUFFER_LEVEL_PRIMARY,&f.commandBegin,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    refused(f.metadata.submitted({50,1},VK_SUCCESS),Issue::DriverFailed);
    accepted(f.metadata.beginCommand(f.command,VK_COMMAND_BUFFER_LEVEL_PRIMARY,&f.commandBegin,VK_SUCCESS));f.record();
    issue(f.metadata.endCommand(f.command,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    refused(f.metadata.submitted(f.command,VK_SUCCESS),Issue::DriverFailed);
}
void lifecycleAndOrdering() {
    ++cases;
    for (unsigned variant=0;variant<11;++variant) {
        Fixture f;f.start();Issue expected=Issue::Lifecycle;
        switch (variant) {
        case 0: issue(f.metadata.endRenderPass(f.command,1),Issue::Lifecycle);break;
        case 1: accepted(f.metadata.beginRenderPass(f.command,1,&f.begin,VK_SUBPASS_CONTENTS_INLINE));
            issue(f.metadata.beginRenderPass(f.command,2,&f.begin,VK_SUBPASS_CONTENTS_INLINE),Issue::Lifecycle);break;
        case 2: accepted(f.metadata.beginRenderPass(f.command,2,&f.begin,VK_SUBPASS_CONTENTS_INLINE));
            issue(f.metadata.endRenderPass(f.command,2),Issue::Lifecycle);break;
        case 3: accepted(f.metadata.beginRenderPass(f.command,1,&f.begin,VK_SUBPASS_CONTENTS_INLINE));expected=Issue::IncompleteRenderPass;break;
        case 4: f.record();f.metadata.resetCommand(50,VK_SUCCESS);break;
        case 5: f.record();f.metadata.freeCommand(50);expected=Issue::StaleGeneration;break;
        case 6: f.record();f.command.generation=2;expected=Issue::StaleGeneration;break;
        case 7: expected=Issue::MultipleSubpasses;accepted(f.metadata.beginRenderPass(f.command,1,&f.begin,VK_SUBPASS_CONTENTS_INLINE));
            issue(f.metadata.nextSubpass(f.command,2),expected);break;
        case 8: expected=Issue::DynamicRendering;issue(f.metadata.dynamicRendering(f.command,1),expected);break;
        case 9: expected=Issue::Secondary;issue(f.metadata.beginRenderPass(f.command,1,&f.begin,VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS),expected);break;
        case 10: expected=Issue::UnsupportedExtension;issue(f.metadata.unsupported(f.command,expected),expected);break;
        }
        (void)f.metadata.endCommand(f.command,VK_SUCCESS);refused(f.metadata.submitted(f.command,VK_SUCCESS),expected);
    }
    Fixture f;f.start();f.record();accepted(f.metadata.endCommand(f.command,VK_SUCCESS));
    f.command.generation=2;f.start();f.record();require(f.submit().metadataAccepted,"new generation clears old recording");
    refused(f.metadata.submitted({50,1},VK_SUCCESS),Issue::StaleGeneration);
    issue(f.metadata.beginCommand({50,1},VK_COMMAND_BUFFER_LEVEL_PRIMARY,&f.commandBegin,VK_SUCCESS),Issue::StaleGeneration);
    refused(f.metadata.submitted(f.command,VK_SUCCESS),Issue::StaleGeneration);
}
void destroyAndReuse() {
    ++cases;
    for (unsigned variant=0;variant<8;++variant) {
        Fixture f;f.start();f.record();accepted(f.metadata.endCommand(f.command,VK_SUCCESS));
        const bool recreate=variant>=4;const unsigned object=variant%4;
        if (object==0) { f.metadata.destroyImage(f.image.key);if(recreate){f.image.key.generation=2;accepted(f.metadata.observeImage(f.image,VK_SUCCESS));} }
        if (object==1) { f.metadata.destroyView(20);if(recreate)accepted(f.metadata.createView(20,&f.view,VK_SUCCESS)); }
        if (object==2) { f.metadata.destroyRenderPass(30);if(recreate)accepted(f.metadata.createRenderPass(30,&f.renderPass,VK_SUCCESS)); }
        if (object==3) { f.metadata.destroyFramebuffer(40);if(recreate)accepted(f.metadata.createFramebuffer(40,&f.framebuffer,VK_SUCCESS)); }
        refused(f.metadata.submitted(f.command,VK_SUCCESS),recreate?Issue::StaleGeneration:Issue::UnknownObject);
    }
    Fixture f;accepted(f.metadata.createRenderPass(31,&f.renderPass,VK_SUCCESS));f.begin.renderPass=handle<VkRenderPass>(31);f.start();
    issue(f.metadata.beginRenderPass(f.command,1,&f.begin,VK_SUBPASS_CONTENTS_INLINE),Issue::IncompatibleFramebuffer);
    (void)f.metadata.endCommand(f.command,VK_SUCCESS);refused(f.metadata.submitted(f.command,VK_SUCCESS),Issue::IncompatibleFramebuffer);
    Fixture stale;stale.metadata.destroyImage({10,999});stale.start();stale.record();require(stale.submit().metadataAccepted,"wrong generation destroy cannot retire live image");
    Fixture rp;rp.metadata.destroyRenderPass(30);accepted(rp.metadata.createRenderPass(30,&rp.renderPass,VK_SUCCESS));rp.start();
    issue(rp.metadata.beginRenderPass(rp.command,1,&rp.begin,VK_SUBPASS_CONTENTS_INLINE),Issue::IncompatibleFramebuffer);
}
void multipleAttachments() {
    ++cases;Fixture f(small(),false,false);
    accepted(f.metadata.observeImage(f.image,VK_SUCCESS));accepted(f.metadata.createView(20,&f.view,VK_SUCCESS));
    auto depth=f.image;depth.key={11,2};depth.format=VK_FORMAT_D32_SFLOAT;depth.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    accepted(f.metadata.observeImage(depth,VK_SUCCESS));auto depthView=f.view;depthView.image=handle<VkImage>(11);
    depthView.format=depth.format;depthView.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    accepted(f.metadata.createView(21,&depthView,VK_SUCCESS));
    std::array<VkAttachmentDescription,2> attachments{f.attachment,f.attachment};attachments[1].format=depth.format;
    attachments[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference depthRef{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    f.subpass.pDepthStencilAttachment=&depthRef;f.renderPass.attachmentCount=2;f.renderPass.pAttachments=attachments.data();
    accepted(f.metadata.createRenderPass(30,&f.renderPass,VK_SUCCESS));
    const std::array<VkImageView,2> views{handle<VkImageView>(20),handle<VkImageView>(21)};
    f.framebuffer.attachmentCount=2;f.framebuffer.pAttachments=views.data();accepted(f.metadata.createFramebuffer(40,&f.framebuffer,VK_SUCCESS));
    f.start();f.record();auto b=f.submit();require(b.metadataAccepted && b.count==6,"two attachments emit six exact operations");
    require(b.operations[4].image==f.image.key && b.operations[5].image==depth.key
        && b.operations[5].newLayout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
        && b.operations[5].range.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT,"color and depth final layouts are not conflated");

    Fixture alias(small(),false,false);accepted(alias.metadata.observeImage(alias.image,VK_SUCCESS));
    accepted(alias.metadata.createView(20,&alias.view,VK_SUCCESS));accepted(alias.metadata.createView(21,&alias.view,VK_SUCCESS));
    attachments={alias.attachment,alias.attachment};
    const std::array<VkAttachmentReference,2> refs{{{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}};
    alias.subpass.colorAttachmentCount=2;alias.subpass.pColorAttachments=refs.data();alias.renderPass.attachmentCount=2;alias.renderPass.pAttachments=attachments.data();
    accepted(alias.metadata.createRenderPass(30,&alias.renderPass,VK_SUCCESS));
    alias.framebuffer.attachmentCount=2;alias.framebuffer.pAttachments=views.data();
    issue(alias.metadata.createFramebuffer(40,&alias.framebuffer,VK_SUCCESS),Issue::Aliasing);
}
void unsupportedCreateCases() {
    ++cases;
    for (unsigned variant=0;variant<13;++variant) {
        Fixture f(small(),false,false);Issue expected=Issue::UnsupportedAttachment;
        switch (variant) {
        case 0: f.renderPass.pNext=&f.begin;expected=Issue::UnsupportedExtension;break;
        case 1: f.renderPass.subpassCount=2;expected=Issue::MultipleSubpasses;break;
        case 2: f.attachment.flags=VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT;expected=Issue::Aliasing;break;
        case 3: f.attachment.samples=VK_SAMPLE_COUNT_4_BIT;break;
        case 4: f.subpass.pResolveAttachments=&f.reference;break;
        case 5: f.reference.attachment=VK_ATTACHMENT_UNUSED;break;
        case 6: f.reference.attachment=8;expected=Issue::InvalidInput;break;
        case 7: f.attachment.format=VK_FORMAT_D24_UNORM_S8_UINT;expected=Issue::UnsupportedFormat;break;
        case 8: f.attachment.finalLayout=VK_IMAGE_LAYOUT_UNDEFINED;expected=Issue::UnsupportedLayout;break;
        case 9: f.reference.layout=VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;expected=Issue::UnsupportedLayout;break;
        case 10: f.renderPass.attachmentCount=9;expected=Issue::Capacity;break;
        case 11: f.renderPass.dependencyCount=17;expected=Issue::Capacity;break;
        case 12: f.subpass.pColorAttachments=nullptr;expected=Issue::Capacity;break;
        }
        issue(f.metadata.createRenderPass(30,&f.renderPass,VK_SUCCESS),expected);
    }
    for (unsigned variant=0;variant<10;++variant) {
        Fixture f(small(),false,false);Issue expected=Issue::UnsupportedImage;
        switch(variant) {
        case 0:f.image.mipLevels=2;break;
        case 1:f.image.arrayLayers=2;break;
        case 2:f.image.type=VK_IMAGE_TYPE_3D;break;
        case 3:f.image.samples=VK_SAMPLE_COUNT_2_BIT;break;
        case 4:f.image.flags=VK_IMAGE_CREATE_ALIAS_BIT;break;
        case 5:f.image.sharing=VK_SHARING_MODE_CONCURRENT;break;
        case 6:f.image.tiling=VK_IMAGE_TILING_LINEAR;break;
        case 7:f.image.format=VK_FORMAT_D24_UNORM_S8_UINT;expected=Issue::UnsupportedFormat;break;
        case 8:f.image.unsupportedPNext=true;expected=Issue::UnsupportedExtension;break;
        case 9:f.image.extent.depth=2;break;
        }
        issue(f.metadata.observeImage(f.image,VK_SUCCESS),expected);
        require(!f.metadata.createView(20,&f.view,VK_SUCCESS).accepted(),"view cannot launder unsupported image");
    }
    for (unsigned variant=0;variant<9;++variant) {
        Fixture f(small(),false,false);accepted(f.metadata.observeImage(f.image,VK_SUCCESS));Issue expected=Issue::UnsupportedView;
        switch(variant) {
        case 0:f.view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;break;
        case 1:f.view.subresourceRange.baseMipLevel=1;break;
        case 2:f.view.subresourceRange.layerCount=0;break;
        case 3:f.view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;break;
        case 4:f.view.components.r=VK_COMPONENT_SWIZZLE_B;break;
        case 5:f.view.pNext=&f.begin;expected=Issue::UnsupportedExtension;break;
        case 6:f.view.image=handle<VkImage>(999);expected=Issue::UnknownObject;break;
        case 7:f.view.format=VK_FORMAT_R32_SFLOAT;break;
        case 8:f.view.flags=VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT;break;
        }
        issue(f.metadata.createView(20,&f.view,VK_SUCCESS),expected);
    }
}
void framebufferAndBeginRefusals() {
    ++cases;
    for (unsigned variant=0;variant<7;++variant) {
        Fixture f;Issue expected=Issue::IncompatibleFramebuffer;
        switch(variant) {
        case 0:f.framebuffer.flags=VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;f.framebuffer.pAttachments=nullptr;expected=Issue::Imageless;break;
        case 1:f.framebuffer.pNext=&f.begin;expected=Issue::UnsupportedExtension;break;
        case 2:f.framebuffer.layers=2;break;
        case 3:f.framebuffer.width=65;break;
        case 4:f.framebuffer.attachmentCount=9;break;
        case 5:f.attachmentView=handle<VkImageView>(999);expected=Issue::UnknownObject;break;
        case 6:f.framebuffer.renderPass=handle<VkRenderPass>(999);expected=Issue::UnknownObject;break;
        }
        issue(f.metadata.createFramebuffer(41,&f.framebuffer,VK_SUCCESS),expected);
        f.begin.framebuffer=handle<VkFramebuffer>(41);f.start();require(!f.metadata.beginRenderPass(f.command,1,&f.begin,VK_SUBPASS_CONTENTS_INLINE).accepted(),"invalid framebuffer cannot emit operations");
    }
    for(unsigned variant=0;variant<6;++variant) {
        Fixture f;f.start();Issue expected=Issue::InvalidInput;
        switch(variant) {
        case 0:f.begin.pNext=&f.framebuffer;expected=Issue::UnsupportedExtension;break;
        case 1:f.begin.renderArea.offset.x=-1;break;
        case 2:f.begin.renderArea.extent.width=UINT32_MAX;break;
        case 3:f.begin.clearValueCount=1;f.begin.pClearValues=nullptr;break;
        case 4:f.begin.renderArea.extent.height=0;break;
        case 5:f.begin.renderPass=handle<VkRenderPass>(999);expected=Issue::UnknownObject;break;
        }
        issue(f.metadata.beginRenderPass(f.command,1,&f.begin,VK_SUBPASS_CONTENTS_INLINE),expected);
        (void)f.metadata.endRenderPass(f.command,2);(void)f.metadata.endCommand(f.command,VK_SUCCESS);
        refused(f.metadata.submitted(f.command,VK_SUCCESS),expected);
    }
}
void budgetsAndFlags() {
    ++cases;Config cfg=small();cfg.operationsPerCommand=5;Fixture f(cfg);f.start();f.record();
    issue(f.metadata.beginRenderPass(f.command,6,&f.begin,VK_SUBPASS_CONTENTS_INLINE),Issue::Capacity);
    (void)f.metadata.endRenderPass(f.command,7);(void)f.metadata.endCommand(f.command,VK_SUCCESS);
    refused(f.metadata.submitted(f.command,VK_SUCCESS),Issue::Capacity);
    cfg=small();cfg.views=1;Fixture v(cfg);issue(v.metadata.createView(21,&v.view,VK_SUCCESS),Issue::Capacity);
    require(!v.metadata.beginCommand(v.command,VK_COMMAND_BUFFER_LEVEL_PRIMARY,&v.commandBegin,VK_SUCCESS).accepted(),"capacity is sticky coverage failure");
    Fixture one;one.commandBegin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;one.start();one.record();require(one.submit().metadataAccepted,"first one-time submit");
    refused(one.metadata.submitted(one.command,VK_SUCCESS),Issue::OneTimeResubmitted);
    Fixture simultaneous;simultaneous.commandBegin.flags=VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    issue(simultaneous.metadata.beginCommand(simultaneous.command,VK_COMMAND_BUFFER_LEVEL_PRIMARY,&simultaneous.commandBegin,VK_SUCCESS),Issue::SimultaneousUse);
    Fixture secondary;issue(secondary.metadata.beginCommand(secondary.command,VK_COMMAND_BUFFER_LEVEL_SECONDARY,&secondary.commandBegin,VK_SUCCESS),Issue::Secondary);
    Fixture stopped;stopped.start();stopped.record();accepted(stopped.metadata.endCommand(stopped.command,VK_SUCCESS));stopped.metadata.stop();refused(stopped.metadata.submitted(stopped.command,VK_SUCCESS),Issue::Stopped);
    Fixture lost;lost.start();lost.record();accepted(lost.metadata.endCommand(lost.command,VK_SUCCESS));refused(lost.metadata.submitted(lost.command,VK_ERROR_DEVICE_LOST),Issue::DeviceLost);
    refused(lost.metadata.submitted(lost.command,VK_SUCCESS),Issue::DeviceLost);
    Fixture duplicate;issue(duplicate.metadata.createView(20,&duplicate.view,VK_SUCCESS),Issue::Lifecycle);
    Fixture recording;recording.start();recording.command.generation=2;
    issue(recording.metadata.beginCommand(recording.command,VK_COMMAND_BUFFER_LEVEL_PRIMARY,&recording.commandBegin,VK_SUCCESS),Issue::Lifecycle);
    (void)recording.metadata.endCommand(recording.command,VK_SUCCESS);refused(recording.metadata.submitted(recording.command,VK_SUCCESS),Issue::Lifecycle);
    Fixture expired;expired.metadata.destroyImage(expired.image.key);issue(expired.metadata.observeImage(expired.image,VK_SUCCESS),Issue::StaleGeneration);
    cfg=small();cfg.commands=1;Fixture commandCap(cfg);commandCap.start();
    issue(commandCap.metadata.beginCommand({51,2},VK_COMMAND_BUFFER_LEVEL_PRIMARY,&commandCap.commandBegin,VK_SUCCESS),Issue::Capacity);
    cfg=small();cfg.images=1;Fixture imageCap(cfg);auto second=imageCap.image;second.key={11,2};
    issue(imageCap.metadata.observeImage(second,VK_SUCCESS),Issue::Capacity);
    cfg=small();cfg.renderPasses=1;Fixture rpCap(cfg);issue(rpCap.metadata.createRenderPass(31,&rpCap.renderPass,VK_SUCCESS),Issue::Capacity);
    cfg=small();cfg.framebuffers=1;Fixture fbCap(cfg);issue(fbCap.metadata.createFramebuffer(41,&fbCap.framebuffer,VK_SUCCESS),Issue::Capacity);
    bool invalidConfig=false;try { cfg.images=0;Metadata bad(cfg); } catch(const std::invalid_argument&) { invalidConfig=true; }
    require(invalidConfig,"invalid limits rejected before constructing active metadata");
}
void noPerCallAllocation() {
    ++cases;Fixture f(small(),false,false);
    require(f.metadata.storageBytes()==Metadata::allocatedStorageBytes()&&f.metadata.storageBytes()<1024*1024,"render-pass constructor has no unused command payloads");
    f.registerAll();f.start();
    require(f.metadata.storageBytes()>Metadata::allocatedStorageBytes()&&f.metadata.storageBytes()<4*1024*1024,"render-pass payload grows only for touched pages");
    f.metadata.resetCommand(50,VK_SUCCESS);
    ++f.command.generation;
    allocations=0;countAllocations=true;
    f.start();f.record();auto batch=f.submit();
    f.metadata.resetCommand(50,VK_SUCCESS);f.metadata.freeCommand(50);
    f.metadata.destroyFramebuffer(40);f.metadata.destroyRenderPass(30);f.metadata.destroyView(20);f.metadata.destroyImage(f.image.key);
    countAllocations=false;
    require(batch.metadataAccepted && allocations==0,"metadata hot path allocated heap memory");
}
void globalGenerationContract() {
    ++cases;Fixture f;f.start();f.record();require(f.submit().metadataAccepted,"first command generation");
    issue(f.metadata.beginCommand({51,1},VK_COMMAND_BUFFER_LEVEL_PRIMARY,&f.commandBegin,VK_SUCCESS),Issue::StaleGeneration);
    accepted(f.metadata.beginCommand({51,2},VK_COMMAND_BUFFER_LEVEL_PRIMARY,&f.commandBegin,VK_SUCCESS));
    accepted(f.metadata.beginRenderPass({51,2},1,&f.begin,VK_SUBPASS_CONTENTS_INLINE));
    accepted(f.metadata.endRenderPass({51,2},2));accepted(f.metadata.endCommand({51,2},VK_SUCCESS));
    require(f.metadata.submitted({51,2},VK_SUCCESS).metadataAccepted,"global serial across different command handles");
    f.metadata.freeCommand(50);
    issue(f.metadata.beginCommand({50,1},VK_COMMAND_BUFFER_LEVEL_PRIMARY,&f.commandBegin,VK_SUCCESS),Issue::StaleGeneration);
    accepted(f.metadata.beginCommand({50,3},VK_COMMAND_BUFFER_LEVEL_PRIMARY,&f.commandBegin,VK_SUCCESS));
    auto next=f.image;next.key={11,2};accepted(f.metadata.observeImage(next,VK_SUCCESS));
    auto repeated=f.image;repeated.key={12,2};issue(f.metadata.observeImage(repeated,VK_SUCCESS),Issue::StaleGeneration);
    require((f.metadata.stats().globalIssues & bit(Issue::StaleGeneration))!=0,"image generation replay is sticky");
    Fixture defaults(small(),false,false);ImageDescription incomplete;
    incomplete.key={10,1};incomplete.format=VK_FORMAT_R8G8B8A8_UNORM;incomplete.extent={64,32,1};incomplete.mipLevels=incomplete.arrayLayers=1;
    const auto rejected=defaults.metadata.observeImage(incomplete,VK_SUCCESS);
    issue(rejected,Issue::UnsupportedImage);issue(rejected,Issue::UnsupportedExtension);
}
void copiedDiagnosticSnapshots() {
    ++cases;Fixture f;const auto before=f.metadata.stats();
    const auto image=f.metadata.imageSnapshot(10);const auto view=f.metadata.viewSnapshot(20);
    const auto pass=f.metadata.renderPassSnapshot(30);const auto fb=f.metadata.framebufferSnapshot(40);
    require(image.known&&image.description.key==f.image.key&&view.known&&view.image==f.image.key,"snapshot preserves observed generation relationships");
    require(pass.known&&pass.attachments[0].format==f.image.format&&pass.attachments[0].reference==f.reference.layout
        &&pass.attachments[0].referenceCount==1&&fb.renderPass==pass.key&&fb.views[0]==view.key,"snapshot contains actual reference and framebuffer creation identity");
    f.attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;f.view.components.r=VK_COMPONENT_SWIZZLE_B;
    require(f.metadata.renderPassSnapshot(30).attachments[0].final==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        &&f.metadata.viewSnapshot(20).components.r==VK_COMPONENT_SWIZZLE_IDENTITY,"snapshots do not retain application pointers");
    const auto after=f.metadata.stats();require(before.registrations==after.registrations&&before.globalIssues==after.globalIssues,"read-only snapshots do not alter acceptance state");
    f.start();require(f.metadata.commandSnapshot(f.command).known&&!f.metadata.commandSnapshot({50,2}).known,"command diagnostics require exact generation");
    accepted(f.metadata.beginRenderPass(f.command,1,&f.begin,VK_SUBPASS_CONTENTS_INLINE));
    require(f.metadata.commandSnapshot(f.command).activeRenderPass==pass.key,"command snapshot shows established active pass");
    f.metadata.destroyView(20);require(!f.metadata.viewSnapshot(20).known&&view.known,"copied snapshots survive while live lookup respects destruction");
    Fixture unsupported(small(),false,false);unsupported.attachment.format=VK_FORMAT_D24_UNORM_S8_UINT;
    (void)unsupported.metadata.createRenderPass(30,&unsupported.renderPass,VK_SUCCESS);
    const auto rejected=unsupported.metadata.renderPassSnapshot(30);
    require(rejected.known&&rejected.attachments[0].format==VK_FORMAT_D24_UNORM_S8_UINT&&(rejected.issues&bit(Issue::UnsupportedFormat)),"unsupported format remains observable without acceptance");
}
void observedSingleColorFormats() {
    ++cases;
    constexpr std::array formats{VK_FORMAT_B10G11R11_UFLOAT_PACK32,VK_FORMAT_R16G16_UNORM,VK_FORMAT_A2B10G10R10_UNORM_PACK32,VK_FORMAT_R32_UINT};
    for(const auto format:formats){
        Fixture f(small(),false,false);f.image.format=f.view.format=f.attachment.format=format;
        f.registerAll();f.start();f.record();const auto batch=f.submit();
        require(batch.metadataAccepted&&batch.count==3&&!batch.issues,"observed single-color format completes full object and submit path");
        require(batch.operations[0].sourceKind==SourceKind::InitialLayoutAssertion&&batch.operations[1].sourceKind==SourceKind::SubpassReferenceTransition
            &&batch.operations[2].sourceKind==SourceKind::FinalLayoutTransition,"format extension retains exact three-operation contract");
        require(batch.operations[0].range.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&batch.operations[1].newLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
            &&batch.operations[2].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&!batch.copyPermitted&&!batch.completeHookCoverage,"format recognition grants no synchronization or copy claim");
        for(unsigned variant=0;variant<5;++variant){
            Fixture rejected(small(),false,false);rejected.image.format=rejected.view.format=rejected.attachment.format=format;
            Issue expected=Issue::UnsupportedImage;
            if(variant==0)rejected.image.mipLevels=2;
            if(variant==1)rejected.image.arrayLayers=2;
            if(variant==2)rejected.image.flags=VK_IMAGE_CREATE_ALIAS_BIT;
            if(variant==3){rejected.attachment.flags=VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT;expected=Issue::Aliasing;}
            if(variant==4){rejected.image.samples=rejected.attachment.samples=VK_SAMPLE_COUNT_2_BIT;expected=Issue::UnsupportedImage;}
            const auto image=rejected.metadata.observeImage(rejected.image,VK_SUCCESS);
            (void)rejected.metadata.createView(20,&rejected.view,VK_SUCCESS);
            const auto pass=rejected.metadata.createRenderPass(30,&rejected.renderPass,VK_SUCCESS);
            (void)rejected.metadata.createFramebuffer(40,&rejected.framebuffer,VK_SUCCESS);
            require((image.issues|pass.issues)&bit(expected),"new color formats retain shape/sample/alias rejection");
            rejected.start();(void)rejected.metadata.beginRenderPass(rejected.command,1,&rejected.begin,VK_SUBPASS_CONTENTS_INLINE);
            (void)rejected.metadata.endRenderPass(rejected.command,2);(void)rejected.metadata.endCommand(rejected.command,VK_SUCCESS);
            refused(rejected.metadata.submitted(rejected.command,VK_SUCCESS),expected);
        }
    }
    for(const auto excluded:{VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_D32_SFLOAT_S8_UINT,VK_FORMAT_E5B9G9R9_UFLOAT_PACK32}){
        Fixture f(small(),false,false);f.image.format=f.view.format=f.attachment.format=excluded;
        issue(f.metadata.observeImage(f.image,VK_SUCCESS),Issue::UnsupportedFormat);
        (void)f.metadata.createView(20,&f.view,VK_SUCCESS);(void)f.metadata.createRenderPass(30,&f.renderPass,VK_SUCCESS);(void)f.metadata.createFramebuffer(40,&f.framebuffer,VK_SUCCESS);
        f.start();(void)f.metadata.beginRenderPass(f.command,1,&f.begin,VK_SUBPASS_CONTENTS_INLINE);
        (void)f.metadata.endRenderPass(f.command,2);(void)f.metadata.endCommand(f.command,VK_SUCCESS);
        refused(f.metadata.submitted(f.command,VK_SUCCESS),Issue::UnsupportedFormat);
    }
}
ProjectionSelection selectionFor(Key image,VkImageSubresourceRange range,std::uint64_t serial=1) {
    ProjectionSelection selection;selection.enabled=true;selection.selectionSerial=serial;selection.image=image;
    selection.range=range;selection.sourceToken=101;selection.passToken=202;return selection;
}
ProjectionSelection selectionFor(const Fixture& f,std::uint64_t serial=1) { return selectionFor(f.image.key,f.view.subresourceRange,serial); }
void projectionRefused(const ProjectionBatch& batch,Issue reason) {
    require(!batch.metadataAccepted&&!batch.count&&(batch.issues&bit(reason))&&batch.classification==ProjectionClassification::Refused,"projection fails closed per slot");
    require(batch.memoryAliasUnverified&&!batch.copyPermitted&&!batch.completeHookCoverage&&!batch.gpuComplete
        &&!batch.lastWriterKnown&&!batch.resourceLifetimePinned,"projection never grants synchronization or copy claims");
}
ProjectionSubmission finishProjection(Fixture& f,VkResult driver=VK_SUCCESS) {
    (void)f.metadata.endCommand(f.command,VK_SUCCESS);
    return f.metadata.projectSubmitted(f.metadata.captureProjectionSubmission(f.command),driver);
}
void projectionHappyAndLegacy() {
    ++cases;
    for(bool depth:{false,true}) {
        Fixture f(small(),depth);accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));
        f.commandBegin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;f.start();f.begin.renderArea={{3,4},{8,9}};f.record();
        accepted(f.metadata.endCommand(f.command,VK_SUCCESS));const auto ticket=f.metadata.captureProjectionSubmission(f.command);
        const auto legacy=f.metadata.submitted(f.command,VK_SUCCESS);const auto projected=f.metadata.projectSubmitted(ticket,VK_SUCCESS);
        const auto& batch=projected.slots[0];
        require(legacy.metadataAccepted&&batch.metadataAccepted&&batch.count==3&&!projected.issues,"legacy and projection do not double-count one-time submit");
        require(batch.classification==ProjectionClassification::DirectAttachmentLayouts&&batch.examinedPasses==1&&batch.directPasses==1
            &&!batch.noDirectPasses&&batch.selection.selectionSerial==1&&batch.selection.sourceToken==101&&batch.selection.passToken==202,"projection preserves opaque selection provenance");
        for(std::size_t n=0;n<3;++n) {
            const auto& a=batch.operations[n];const auto& b=legacy.operations[n];
            require(a.image==b.image&&a.command==b.command&&a.view==b.view&&a.renderPass==b.renderPass&&a.framebuffer==b.framebuffer
                &&a.sourceKind==b.sourceKind&&a.oldLayout==b.oldLayout&&a.newLayout==b.newLayout&&a.recordOrder==b.recordOrder
                &&a.range.levelCount==1&&a.range.layerCount==1&&a.range.baseMipLevel==0,"projected layout facts match legacy supported path");
        }
        require(batch.memoryAliasUnverified&&!batch.copyPermitted&&!batch.gpuComplete&&!batch.completeHookCoverage,"successful projection remains metadata only");
        require(projected.slots[1].classification==ProjectionClassification::Disabled&&!projected.slots[1].metadataAccepted,"unset slots disabled");
        const auto twice=f.metadata.projectSubmitted(f.metadata.captureProjectionSubmission(f.command),VK_SUCCESS);
        projectionRefused(twice.slots[0],Issue::OneTimeResubmitted);
        refused(f.metadata.submitted(f.command,VK_SUCCESS),Issue::OneTimeResubmitted);
    }
}
enum class OtherAttachment { CombinedDepthStencil, UnknownColorFormat, MipLayer, Swizzle, Reinterpret, Layout, BadRange, ImagePNext, ViewPNext, MayAlias, UnknownView, DuplicateImage };
struct MixedFixture:Fixture {
    ImageDescription otherImage;
    VkImageViewCreateInfo otherView{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    std::array<VkAttachmentDescription,2> attachments;
    std::array<VkAttachmentReference,2> references;
    std::array<VkImageView,2> views{handle<VkImageView>(20),handle<VkImageView>(21)};
    MixedFixture(OtherAttachment kind=OtherAttachment::CombinedDepthStencil,bool unusedCandidate=false):Fixture(small(),false,false) {
        otherImage=image;otherImage.key={11,2};otherView=view;otherView.image=handle<VkImage>(11);
        attachments={attachment,attachment};references={reference,reference};references[1].attachment=1;
        subpass.colorAttachmentCount=2;subpass.pColorAttachments=references.data();
        if(kind==OtherAttachment::CombinedDepthStencil) {
            otherImage.format=otherView.format=attachments[1].format=VK_FORMAT_D32_SFLOAT_S8_UINT;
            otherImage.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;otherView.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
            references[1].layout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            subpass.colorAttachmentCount=1;subpass.pDepthStencilAttachment=&references[1];
        }
        if(kind==OtherAttachment::UnknownColorFormat) otherImage.format=otherView.format=attachments[1].format=VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        if(kind==OtherAttachment::MipLayer) { otherImage.mipLevels=2;otherImage.arrayLayers=2;otherImage.extent={128,64,1};otherView.subresourceRange.baseMipLevel=1;otherView.subresourceRange.baseArrayLayer=1; }
        if(kind==OtherAttachment::Swizzle) otherView.components.r=VK_COMPONENT_SWIZZLE_B;
        if(kind==OtherAttachment::Reinterpret) { otherImage.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;otherView.format=attachments[1].format=VK_FORMAT_R8G8B8A8_SRGB; }
        if(kind==OtherAttachment::Layout) references[1].layout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if(kind==OtherAttachment::BadRange) otherView.subresourceRange.baseMipLevel=9;
        if(kind==OtherAttachment::ImagePNext) otherImage.unsupportedPNext=true;
        if(kind==OtherAttachment::ViewPNext) otherView.pNext=&begin;
        if(kind==OtherAttachment::MayAlias) attachments[1].flags=VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT;
        if(kind==OtherAttachment::UnknownView) views[1]=handle<VkImageView>(999);
        if(kind==OtherAttachment::DuplicateImage) otherView.image=handle<VkImage>(10);
        if(unusedCandidate) { subpass.colorAttachmentCount=0;subpass.pColorAttachments=nullptr; }
        renderPass.attachmentCount=2;renderPass.pAttachments=attachments.data();framebuffer.attachmentCount=2;framebuffer.pAttachments=views.data();
        accepted(metadata.observeImage(image,VK_SUCCESS));accepted(metadata.createView(20,&view,VK_SUCCESS));
        (void)metadata.observeImage(otherImage,VK_SUCCESS);(void)metadata.createView(21,&otherView,VK_SUCCESS);
        (void)metadata.createRenderPass(30,&renderPass,VK_SUCCESS);(void)metadata.createFramebuffer(40,&framebuffer,VK_SUCCESS);
    }
    void recordMixed() { start();(void)metadata.beginRenderPass(command,2,&begin,VK_SUBPASS_CONTENTS_INLINE);(void)metadata.endRenderPass(command,4); }
};
void projectionLocalIsolation() {
    ++cases;
    for(const auto kind:{OtherAttachment::CombinedDepthStencil,OtherAttachment::UnknownColorFormat,OtherAttachment::MipLayer,
        OtherAttachment::Swizzle,OtherAttachment::Reinterpret,OtherAttachment::Layout}) {
        MixedFixture f(kind);accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));
        accepted(f.metadata.setProjectionSelection(1,selectionFor(f.otherImage.key,f.otherView.subresourceRange,2)));
        auto absent=f.image;absent.key={12,3};accepted(f.metadata.observeImage(absent,VK_SUCCESS));
        accepted(f.metadata.setProjectionSelection(2,selectionFor(absent.key,f.view.subresourceRange,3)));
        f.recordMixed();const auto projected=finishProjection(f);const auto& good=projected.slots[0];const auto& excluded=projected.slots[1];
        require(!projected.issues&&good.metadataAccepted&&good.count==3&&good.directPasses==1,"unrelated attachment local exclusion does not poison candidate");
        require(!excluded.metadataAccepted&&!excluded.count&&excluded.issues,"same CB keeps unsupported selected image independent");
        require(projected.slots[2].metadataAccepted&&!projected.slots[2].count&&projected.slots[2].noDirectPasses==1
            &&projected.slots[2].classification==ProjectionClassification::NoDirectAttachmentReference
            &&projected.slots[2].memoryAliasUnverified&&!projected.slots[2].copyPermitted,"complete all-attachment identities establish only no direct reference");
        const auto legacy=f.metadata.submitted(f.command,VK_SUCCESS);
        require(!legacy.metadataAccepted&&!legacy.count&&legacy.issues,"old all-command refusal unchanged");
        require(!(good.issues&bit(Issue::StaleGeneration)),"local RP failure creates no projection stale cascade");
    }
    MixedFixture unused(OtherAttachment::CombinedDepthStencil,true);accepted(unused.metadata.setProjectionSelection(0,selectionFor(unused)));
    unused.recordMixed();const auto batch=finishProjection(unused).slots[0];
    require(batch.metadataAccepted&&batch.count==2&&batch.directPasses==1&&batch.operations[0].sourceKind==SourceKind::InitialLayoutAssertion
        &&batch.operations[1].sourceKind==SourceKind::UnusedInitialToFinalTransition,"unused candidate gets initial assertion plus implicit initial-to-final");
    require(batch.operations[1].oldLayout==VK_IMAGE_LAYOUT_UNDEFINED&&batch.operations[1].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        &&batch.operations[0].recordOrder==2&&batch.operations[1].recordOrder==4,"unused attachment never fabricates subpass reference");
    refused(unused.metadata.submitted(unused.command,VK_SUCCESS),Issue::UnsupportedAttachment);
}
void projectionStructuralAndIdentity() {
    ++cases;
    for(const auto kind:{OtherAttachment::BadRange,OtherAttachment::ImagePNext,OtherAttachment::ViewPNext,OtherAttachment::MayAlias,
        OtherAttachment::UnknownView,OtherAttachment::DuplicateImage}) {
        MixedFixture f(kind);accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));f.recordMixed();const auto out=finishProjection(f);
        const auto expected=kind==OtherAttachment::BadRange ? Issue::UnsupportedView : kind==OtherAttachment::UnknownView ? Issue::UnknownObject :
            (kind==OtherAttachment::ImagePNext||kind==OtherAttachment::ViewPNext) ? Issue::UnsupportedExtension : Issue::Aliasing;
        projectionRefused(out.slots[0],expected);
    }
    // Even a non-candidate attachment's destruction prevents exclusion evidence.
    for(unsigned variant=0;variant<5;++variant) {
        MixedFixture f;accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));f.recordMixed();
        if(variant==0) f.metadata.destroyView(21);
        if(variant==1) { f.metadata.destroyView(21);(void)f.metadata.createView(21,&f.otherView,VK_SUCCESS); }
        if(variant==2) f.metadata.destroyImage(f.otherImage.key);
        if(variant==3) { f.metadata.destroyImage(f.otherImage.key);f.otherImage.key.generation=3;(void)f.metadata.observeImage(f.otherImage,VK_SUCCESS); }
        if(variant==4) { f.metadata.destroyFramebuffer(40);(void)f.metadata.createFramebuffer(40,&f.framebuffer,VK_SUCCESS); }
        projectionRefused(finishProjection(f).slots[0],(variant==0||variant==2) ? Issue::UnknownObject : Issue::StaleGeneration);
    }
    Fixture mismatch;accepted(mismatch.metadata.createRenderPass(31,&mismatch.renderPass,VK_SUCCESS));
    mismatch.begin.renderPass=handle<VkRenderPass>(31);accepted(mismatch.metadata.setProjectionSelection(0,selectionFor(mismatch)));
    mismatch.start();(void)mismatch.metadata.beginRenderPass(mismatch.command,1,&mismatch.begin,VK_SUBPASS_CONTENTS_INLINE);
    (void)mismatch.metadata.endRenderPass(mismatch.command,2);projectionRefused(finishProjection(mismatch).slots[0],Issue::IncompatibleFramebuffer);
    for(unsigned variant=0;variant<8;++variant) {
        Fixture f(small(),false,false);
        if(variant==0) f.renderPass.pNext=&f.begin;
        if(variant==1) f.subpass.pResolveAttachments=&f.reference;
        if(variant==2) f.subpass.preserveAttachmentCount=1;
        if(variant==3) f.renderPass.subpassCount=2;
        (void)f.metadata.observeImage(f.image,VK_SUCCESS);(void)f.metadata.createView(20,&f.view,VK_SUCCESS);
        (void)f.metadata.createRenderPass(30,&f.renderPass,VK_SUCCESS);
        if(variant==4) f.framebuffer.flags=VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
        if(variant==5) f.framebuffer.pNext=&f.begin;
        (void)f.metadata.createFramebuffer(40,&f.framebuffer,VK_SUCCESS);
        accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));f.start();
        if(variant==6) f.begin.pNext=&f.framebuffer;
        (void)f.metadata.beginRenderPass(f.command,1,&f.begin,variant==7 ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);
        (void)f.metadata.endRenderPass(f.command,2);const auto out=finishProjection(f);
        require(!out.slots[0].metadataAccepted&&!out.slots[0].count&&out.issues,"unsupported structure refuses all possibly affected candidates");
    }
}
void projectionSelectionEpochsAndTickets() {
    ++cases;
    for(unsigned variant=0;variant<5;++variant) {
        Fixture f;if(variant!=0) accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));
        f.start();f.record();
        auto changed=selectionFor(f,2);
        if(variant==1) changed.enabled=false;
        if(variant==2) ++changed.sourceToken;
        if(variant==3) ++changed.passToken;
        accepted(f.metadata.setProjectionSelection(0,changed));
        projectionRefused(finishProjection(f).slots[0],Issue::StaleGeneration);
        // Even restoring exactly the same image cannot backfill the old epoch.
        changed=selectionFor(f,3);accepted(f.metadata.setProjectionSelection(0,changed));
        projectionRefused(f.metadata.projectSubmitted(f.metadata.captureProjectionSubmission(f.command),VK_SUCCESS).slots[0],Issue::StaleGeneration);
        f.command.generation=2;f.start();f.record();require(finishProjection(f).slots[0].metadataAccepted,"new successful recording freezes new selection epoch");
    }
    Fixture independent;accepted(independent.metadata.setProjectionSelection(0,selectionFor(independent)));
    accepted(independent.metadata.setProjectionSelection(1,selectionFor(independent,2)));independent.start();independent.record();
    accepted(independent.metadata.setProjectionSelection(0,selectionFor(independent,3)));
    const auto split=finishProjection(independent);projectionRefused(split.slots[0],Issue::StaleGeneration);
    require(split.slots[1].metadataAccepted&&split.slots[1].count==3,"selection changes affect only their slot");
    Fixture invalid;accepted(invalid.metadata.setProjectionSelection(0,selectionFor(invalid)));invalid.start();invalid.record();
    issue(invalid.metadata.setProjectionSelection(0,selectionFor(invalid)),Issue::StaleGeneration);
    accepted(invalid.metadata.setProjectionSelection(0,selectionFor(invalid,2)));projectionRefused(finishProjection(invalid).slots[0],Issue::StaleGeneration);
    invalid.command.generation=2;invalid.start();invalid.record();require(finishProjection(invalid).slots[0].metadataAccepted,"invalid setter remains sticky only for prior recordings");
    issue(invalid.metadata.setProjectionSelection(projectionSlots,selectionFor(invalid,3)),Issue::InvalidInput);
    Fixture retry;accepted(retry.metadata.setProjectionSelection(0,selectionFor(retry)));retry.commandBegin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    retry.start();retry.record();const auto failed=finishProjection(retry,VK_ERROR_OUT_OF_HOST_MEMORY);projectionRefused(failed.slots[0],Issue::DriverFailed);
    require(!failed.driverSubmissionSucceeded,"driver failure literal in projection");
    const auto successful=retry.metadata.projectSubmitted(retry.metadata.captureProjectionSubmission(retry.command),VK_SUCCESS);
    require(successful.slots[0].metadataAccepted,"failed submission consumes ticket but not one-time successful submission");
    for(unsigned variant=0;variant<6;++variant) {
        Fixture f;accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));f.start();f.record();accepted(f.metadata.endCommand(f.command,VK_SUCCESS));
        auto ticket=f.metadata.captureProjectionSubmission(f.command);
        if(variant==0) ++ticket.frozenSelections[0].sourceToken;
        if(variant==1) ++ticket.serial;
        if(variant==2) f.metadata.resetCommand(f.command.handle,VK_SUCCESS);
        if(variant==3) f.metadata.freeCommand(f.command.handle);
        if(variant==4) { f.command.generation=2;f.start();f.record();accepted(f.metadata.endCommand(f.command,VK_SUCCESS)); }
        if(variant==5) (void)f.metadata.captureProjectionSubmission(f.command);
        const auto out=f.metadata.projectSubmitted(ticket,VK_SUCCESS);
        projectionRefused(out.slots[0],(variant==0||variant==3||variant==4) ? Issue::StaleGeneration : Issue::Lifecycle);
    }
    Fixture replay;accepted(replay.metadata.setProjectionSelection(0,selectionFor(replay)));replay.start();replay.record();
    accepted(replay.metadata.endCommand(replay.command,VK_SUCCESS));const auto ticket=replay.metadata.captureProjectionSubmission(replay.command);
    require(replay.metadata.projectSubmitted(ticket,VK_SUCCESS).slots[0].metadataAccepted,"ticket first use");
    projectionRefused(replay.metadata.projectSubmitted(ticket,VK_SUCCESS).slots[0],Issue::Lifecycle);
    Fixture early;accepted(early.metadata.setProjectionSelection(0,selectionFor(early)));early.start();
    (void)early.metadata.captureProjectionSubmission(early.command);early.record();projectionRefused(finishProjection(early).slots[0],Issue::Lifecycle);
}
void projectionBudgetsAndLifecycle() {
    ++cases;
    // Keep the original 32/33 selected-pass operation boundary independent of
    // the newly configurable full (mostly unselected) pass journal capacity.
    for(std::size_t passes:{std::size_t(32),std::size_t(33)}) {
        auto config=small();config.projectionPassesPerCommand=32;Fixture f(config);
        accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));f.start();
        for(std::size_t n=0;n<passes;++n) {
            (void)f.metadata.beginRenderPass(f.command,n*2+1,&f.begin,VK_SUBPASS_CONTENTS_INLINE);
            (void)f.metadata.endRenderPass(f.command,n*2+2);
        }
        const auto out=finishProjection(f);
        if(passes==32) require(out.slots[0].metadataAccepted&&out.slots[0].count==maxOperations,"full configured 32-pass journal and 96 operations succeed");
        else projectionRefused(out.slots[0],Issue::Capacity);
    }
    auto cfg=small();cfg.operationsPerCommand=2;Fixture limited(cfg);accepted(limited.metadata.setProjectionSelection(0,selectionFor(limited)));limited.start();
    (void)limited.metadata.beginRenderPass(limited.command,1,&limited.begin,VK_SUBPASS_CONTENTS_INLINE);(void)limited.metadata.endRenderPass(limited.command,2);
    projectionRefused(finishProjection(limited).slots[0],Issue::Capacity);
    for(unsigned variant=0;variant<8;++variant) {
        Fixture f;accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));f.start();
        (void)f.metadata.beginRenderPass(f.command,1,&f.begin,VK_SUBPASS_CONTENTS_INLINE);
        Issue expected=Issue::Lifecycle;
        if(variant==0) { (void)f.metadata.dynamicRendering(f.command,2);expected=Issue::DynamicRendering; }
        if(variant==1) { (void)f.metadata.nextSubpass(f.command,2);expected=Issue::MultipleSubpasses; }
        if(variant==2) { (void)f.metadata.unsupported(f.command,Issue::Secondary);expected=Issue::Secondary; }
        if(variant==3) { (void)f.metadata.beginRenderPass(f.command,2,&f.begin,VK_SUBPASS_CONTENTS_INLINE); }
        if(variant==4) { (void)f.metadata.endRenderPass(f.command,1); }
        if(variant==5) { expected=Issue::IncompleteRenderPass; }
        if(variant==6) { f.metadata.stop();expected=Issue::Stopped; }
        if(variant==7) { (void)f.metadata.endCommand(f.command,VK_ERROR_OUT_OF_HOST_MEMORY);expected=Issue::DriverFailed; }
        if(variant!=5) (void)f.metadata.endRenderPass(f.command,3);
        projectionRefused(finishProjection(f).slots[0],expected);
    }
    Fixture empty;accepted(empty.metadata.setProjectionSelection(0,selectionFor(empty)));empty.start();const auto noPass=finishProjection(empty);
    require(noPass.slots[0].metadataAccepted&&!noPass.slots[0].count&&!noPass.slots[0].examinedPasses
        &&noPass.slots[0].classification==ProjectionClassification::NoDirectAttachmentReference,"empty command asserts only no recorded direct RP attachment reference");
    Fixture loss;accepted(loss.metadata.setProjectionSelection(0,selectionFor(loss)));loss.start();loss.record();projectionRefused(finishProjection(loss,VK_ERROR_DEVICE_LOST).slots[0],Issue::DeviceLost);
    Fixture range;auto selected=selectionFor(range);selected.range.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    accepted(range.metadata.setProjectionSelection(0,selected));range.start();range.record();projectionRefused(finishProjection(range).slots[0],Issue::UnsupportedView);
    Fixture candidateDestroyed;accepted(candidateDestroyed.metadata.setProjectionSelection(0,selectionFor(candidateDestroyed)));candidateDestroyed.start();
    candidateDestroyed.metadata.destroyImage(candidateDestroyed.image.key);projectionRefused(finishProjection(candidateDestroyed).slots[0],Issue::UnknownObject);
    Fixture failedCreate(small(),false,false);accepted(failedCreate.metadata.observeImage(failedCreate.image,VK_SUCCESS));
    accepted(failedCreate.metadata.createView(20,&failedCreate.view,VK_SUCCESS));const auto registrations=failedCreate.metadata.stats().registrations;
    issue(failedCreate.metadata.createRenderPass(30,&failedCreate.renderPass,VK_ERROR_OUT_OF_HOST_MEMORY),Issue::DriverFailed);
    require(failedCreate.metadata.stats().registrations==registrations,"failed RP creation has no projection metadata commit");
    (void)failedCreate.metadata.createFramebuffer(40,&failedCreate.framebuffer,VK_SUCCESS);
    accepted(failedCreate.metadata.setProjectionSelection(0,selectionFor(failedCreate)));failedCreate.start();
    (void)failedCreate.metadata.beginRenderPass(failedCreate.command,1,&failedCreate.begin,VK_SUBPASS_CONTENTS_INLINE);
    (void)failedCreate.metadata.endRenderPass(failedCreate.command,2);projectionRefused(finishProjection(failedCreate).slots[0],Issue::UnknownObject);
}
void projectionSceneJournalCapacity() {
    ++cases;
    {
        Fixture selected;accepted(selected.metadata.setProjectionSelection(0,selectionFor(selected)));selected.start();
        for(std::uint64_t n=0;n<33;++n){
            (void)selected.metadata.beginRenderPass(selected.command,n*2+1,&selected.begin,VK_SUBPASS_CONTENTS_INLINE);
            (void)selected.metadata.endRenderPass(selected.command,n*2+2);
        }
        const auto out=finishProjection(selected);projectionRefused(out.slots[0],Issue::Capacity);
        require(out.observedPasses==33&&out.retainedPasses==33&&!out.droppedPasses&&!out.issues,
            "33 complete selected passes exceed only the unchanged per-slot 96 operation budget");
    }
    for(std::size_t passCount:{std::size_t(149),maxProjectionPasses,maxProjectionPasses+1}) {
        Fixture f;auto other=f.image;other.key={11,2};accepted(f.metadata.observeImage(other,VK_SUCCESS));
        auto view=f.view;view.image=handle<VkImage>(11);accepted(f.metadata.createView(21,&view,VK_SUCCESS));
        auto framebuffer=f.framebuffer;auto imageView=handle<VkImageView>(21);framebuffer.pAttachments=&imageView;
        accepted(f.metadata.createFramebuffer(41,&framebuffer,VK_SUCCESS));auto unrelated=f.begin;unrelated.framebuffer=handle<VkFramebuffer>(41);
        accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));f.start();
        allocations=0;countAllocations=true;
        for(std::size_t n=0;n<passCount;++n){
            const auto* begin=n+1==passCount?&f.begin:&unrelated;
            (void)f.metadata.beginRenderPass(f.command,n*2+1,begin,VK_SUBPASS_CONTENTS_INLINE);
            (void)f.metadata.endRenderPass(f.command,n*2+2);
        }
        const auto out=finishProjection(f);countAllocations=false;
        require(allocations==0,"large projection journal recording/submission has zero hot allocations");
        require(out.observedPasses==passCount&&out.retainedPasses==std::min(passCount,maxProjectionPasses)
            &&out.droppedPasses==(passCount>maxProjectionPasses?1u:0u),"full journal observed/retained/dropped counts");
        require(out.slots[0].observedPasses==out.observedPasses&&out.slots[0].retainedPasses==out.retainedPasses
            &&out.slots[0].droppedPasses==out.droppedPasses,"each slot retains common journal diagnostic counts");
        if(passCount<=maxProjectionPasses){
            require(out.slots[0].metadataAccepted&&out.slots[0].count==3&&out.slots[0].directPasses==1
                &&out.slots[0].noDirectPasses==passCount-1&&out.slots[0].examinedPasses==passCount,"only last pass selected after many unrelated passes");
            require(out.slots[0].operations[0].recordOrder==passCount*2-1&&out.slots[0].operations[2].recordOrder==passCount*2,"last selected pass has its own begin/end order");
        }else{
            projectionRefused(out.slots[0],Issue::Capacity);
            require(!(out.issues&bit(Issue::IncompleteRenderPass)),"overflow does not manufacture incomplete pass from prior completed entry");
            require(!f.metadata.stats().globalIssues,"journal overflow stays local to command generation");
        }
        f.command.generation=2;f.start();f.record();const auto fresh=finishProjection(f);
        require(fresh.slots[0].metadataAccepted&&fresh.slots[0].count==3&&fresh.observedPasses==1
            &&fresh.retainedPasses==1&&!fresh.droppedPasses,"new successful Begin resets prior capacity and all pass counts");
    }
    for(unsigned variant=0;variant<3;++variant){
        Fixture f;auto other=f.image;other.key={11,2};accepted(f.metadata.observeImage(other,VK_SUCCESS));
        auto view=f.view;view.image=handle<VkImage>(11);accepted(f.metadata.createView(21,&view,VK_SUCCESS));
        auto framebuffer=f.framebuffer;auto imageView=handle<VkImageView>(21);framebuffer.pAttachments=&imageView;
        accepted(f.metadata.createFramebuffer(41,&framebuffer,VK_SUCCESS));auto unrelated=f.begin;unrelated.framebuffer=handle<VkFramebuffer>(41);
        accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));f.start();
        for(std::uint64_t n=0;n<149;++n){
            (void)f.metadata.beginRenderPass(f.command,n*2+1,n==148?&f.begin:&unrelated,VK_SUBPASS_CONTENTS_INLINE);
            (void)f.metadata.endRenderPass(f.command,n*2+2);
        }
        (void)f.metadata.endCommand(f.command,VK_SUCCESS);const auto ticket=f.metadata.captureProjectionSubmission(f.command);
        if(variant==0)f.metadata.destroyView(21);
        if(variant==1){f.metadata.destroyImage(other.key);other.key.generation=3;accepted(f.metadata.observeImage(other,VK_SUCCESS));}
        if(variant==2){f.metadata.destroyFramebuffer(41);accepted(f.metadata.createFramebuffer(41,&framebuffer,VK_SUCCESS));}
        const auto out=f.metadata.projectSubmitted(ticket,VK_SUCCESS);
        projectionRefused(out.slots[0],variant==0?Issue::UnknownObject:Issue::StaleGeneration);
        require(out.observedPasses==149&&out.retainedPasses==149&&!out.droppedPasses,"all earlier unrelated identities still revalidated after long recording");
    }
}
void projectionDroppedBeginAndStorage() {
    ++cases;
    for(unsigned variant=0;variant<4;++variant){
        auto config=small();config.projectionPassesPerCommand=1;Fixture f(config);
        accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));f.start();f.record(1,2);
        auto invalid=f.begin;if(variant==1)invalid.renderPass=VK_NULL_HANDLE;
        if(variant==2)invalid.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
        if(variant==3)invalid.pNext=&invalid;
        (void)f.metadata.beginRenderPass(f.command,3,&invalid,VK_SUBPASS_CONTENTS_INLINE);
        (void)f.metadata.endRenderPass(f.command,4);const auto out=finishProjection(f);
        require(out.observedPasses==2&&out.retainedPasses==1&&out.droppedPasses==1,"failed Begin records a dropped pass, not a partial entry");
        require(!out.slots[0].metadataAccepted&&!out.slots[0].count&&!(out.issues&bit(Issue::IncompleteRenderPass)),"matching End after dropped Begin never touches prior pass");
    }
    Fixture lone;accepted(lone.metadata.setProjectionSelection(0,selectionFor(lone)));lone.start();
    (void)lone.metadata.beginRenderPass(lone.command,1,nullptr,VK_SUBPASS_CONTENTS_INLINE);
    (void)lone.metadata.endRenderPass(lone.command,2);const auto invalid=finishProjection(lone);
    require(invalid.observedPasses==1&&!invalid.retainedPasses&&invalid.droppedPasses==1
        &&(invalid.issues&bit(Issue::InvalidInput))&&!(invalid.issues&bit(Issue::IncompleteRenderPass)),"first rejected Begin and matching End has no fabricated incompleteness");
    Fixture unmatched;accepted(unmatched.metadata.setProjectionSelection(0,selectionFor(unmatched)));unmatched.start();
    (void)unmatched.metadata.endRenderPass(unmatched.command,1);projectionRefused(finishProjection(unmatched).slots[0],Issue::IncompleteRenderPass);
    for(std::size_t limit:{std::size_t(0),maxProjectionPasses+1}){
        auto config=small();config.projectionPassesPerCommand=limit;bool rejected=false;
        try{Metadata metadata(config);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"projection journal config rejected outside 1..256");
    }
    allocations=0;allocationBytes=0;countAllocations=true;
    { Metadata metadata;countAllocations=false;constructorAllocations=allocations;constructorHeapBytes=allocationBytes; }
    require(constructorAllocations==1&&constructorHeapBytes==Metadata::allocatedStorageBytes(),"default construction uses one exact fixed-size heap block");
    allocations=0;
}
void projectionRP2AndAllocation() {
    ++cases;
    for(unsigned variant=0;variant<6;++variant) {
        Fixture f(small(),false,false);accepted(f.metadata.observeImage(f.image,VK_SUCCESS));accepted(f.metadata.createView(20,&f.view,VK_SUCCESS));
        VkAttachmentDescription2 attachment{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};attachment.format=f.image.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference2 reference{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};reference.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription2 sub{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&reference;
        VkSubpassDependency2 dependency{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};dependency.srcSubpass=VK_SUBPASS_EXTERNAL;dependency.dstSubpass=0;
        VkRenderPassCreateInfo2 rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&sub;
        if(variant==1) attachment.pNext=&rp;
        if(variant==2) reference.pNext=&rp;
        if(variant==3) sub.viewMask=1;
        if(variant==4) { rp.dependencyCount=1;rp.pDependencies=&dependency;dependency.pNext=&rp; }
        if(variant==5) rp.correlatedViewMaskCount=1;
        (void)f.metadata.createRenderPass2(30,&rp,VK_SUCCESS);(void)f.metadata.createFramebuffer(40,&f.framebuffer,VK_SUCCESS);
        accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));f.start();
        VkSubpassBeginInfo begin{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};begin.contents=VK_SUBPASS_CONTENTS_INLINE;
        VkSubpassEndInfo end{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
        (void)f.metadata.beginRenderPass2(f.command,1,&f.begin,&begin);(void)f.metadata.endRenderPass2(f.command,2,&end);
        const auto out=finishProjection(f);
        if(!variant) require(out.slots[0].metadataAccepted&&out.slots[0].count==3,"RP2 positive shares classified parser");
        else require(!out.slots[0].metadataAccepted&&!out.slots[0].count&&out.issues,"RP2 nested extensions and multiview rejected globally");
    }
    Fixture f(small(),false,false);f.registerAll();accepted(f.metadata.setProjectionSelection(0,selectionFor(f)));f.start();
    allocations=0;countAllocations=true;f.record();const auto out=finishProjection(f);
    auto disabled=selectionFor(f,2);disabled.enabled=false;accepted(f.metadata.setProjectionSelection(0,disabled));
    f.metadata.resetCommand(50,VK_SUCCESS);f.metadata.freeCommand(50);f.metadata.destroyFramebuffer(40);f.metadata.destroyRenderPass(30);
    f.metadata.destroyView(20);f.metadata.destroyImage(f.image.key);countAllocations=false;
    require(out.slots[0].metadataAccepted&&allocations==0,"projection hot path performs no heap allocation");
}
VkImageMemoryBarrier sourceBarrier(const Fixture& f,VkImageLayout old,VkImageLayout next){
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=handle<VkImage>(f.image.key.handle);b.subresourceRange=f.view.subresourceRange;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.oldLayout=old;b.newLayout=next;
    b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;return b;
}
void sourceEvent(Fixture& f,std::uint64_t order,const VkImageMemoryBarrier& b){f.metadata.explicitImageBarriers(f.command,order,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b);}
auto sourceQuery(Fixture& f,std::uint64_t order,VkImageLayout declared=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL){
    f.metadata.recordingPoint(f.command,order);return f.metadata.resolveSourceAt(f.command,order,{f.image.key.handle,f.image.key.generation},f.view.subresourceRange,declared);
}
using LI=xess_vk::RecordedLayoutIssue;
bool layoutIssue(const xess_vk::RecordedSourceLayout& r,LI issue){return (r.issues&xess_vk::bit(issue))!=0;}
void layoutRefused(const xess_vk::RecordedSourceLayout& r,LI issue){
    require(!r.recordedLayoutResolved&&!r.recordedPrefixSupported&&layoutIssue(r,issue),"recorded-prefix refusal has explicit reason and no layout claim");
    require(!r.layoutVerified&&!r.gpuComplete&&!r.ownerVerified&&!r.resourceLifetimePinned&&!r.copyPermitted,"layout prefix never grants GPU/owner/lifetime/copy proof");
}
void sourceReadIntervals(){
    ++cases;
    {Fixture f(small(),true,false);f.attachment.flags=VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT;
        accepted(f.metadata.observeImage(f.image,VK_SUCCESS));accepted(f.metadata.createView(20,&f.view,VK_SUCCESS));
        (void)f.metadata.createRenderPass(30,&f.renderPass,VK_SUCCESS);(void)f.metadata.createFramebuffer(40,&f.framebuffer,VK_SUCCESS);
        f.start();(void)f.metadata.beginRenderPass(f.command,2,&f.begin,VK_SUBPASS_CONTENTS_INLINE);(void)f.metadata.endRenderPass(f.command,4);
        f.metadata.recordingPoint(f.command,5);f.metadata.recordingPoint(f.command,9);
        const auto old=f.metadata.resolveSourceAt(f.command,9,{10,1},f.view.subresourceRange,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        require(old.issues&xess_vk::bit(LI::PassUnsupported),"old aliased pass remains refused by full layout reconstruction");
        const auto interval=f.metadata.imageInterval(f.command,5,9,{10,1});
        require(interval.queried&&interval.untouched&&!interval.issues,"completed unsupported pass before an actual read cannot contaminate a later untouched interval");}
    for(unsigned variant=0;variant<5;++variant){Fixture f;f.start();f.metadata.recordingPoint(f.command,5);
        if(variant==0)sourceEvent(f,6,sourceBarrier(f,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        if(variant==1)f.record(6,7);
        if(variant==2)f.metadata.imageTouched(f.command,6,10,xess_vk::RecordedImageTouch::Transfer);
        if(variant==3)f.metadata.imageTouched(f.command,6,10,xess_vk::RecordedImageTouch::Clear);
        if(variant==4)f.metadata.imageTouched(f.command,6,11,xess_vk::RecordedImageTouch::Clear);
        f.metadata.recordingPoint(f.command,9);const auto r=f.metadata.imageInterval(f.command,5,9,{10,1});
        require(r.queried&&!r.issues&&r.untouched==(variant==4),"only unrelated transfer/clear is excluded; equal-layout barrier still counts as touching");
        require(r.barriers==(variant==0)&&r.attachments==(variant==1)&&r.transfers==(variant==2)&&r.clears==(variant==3),"interval reports the exact touch reason");}
    {Fixture f;f.start();f.metadata.recordingPoint(f.command,5);f.metadata.recordingPoint(f.command,9);
        require(!f.metadata.imageInterval({50,2},5,9,{10,1}).untouched&&!f.metadata.imageInterval(f.command,9,9,{10,1}).untouched&&
            !f.metadata.imageInterval(f.command,5,8,{10,1}).untouched&&!f.metadata.imageInterval(f.command,5,9,{10,2}).untouched,
            "stale recording/image, invalid interval and stale endpoint cannot grant unchanged state");}
    {Fixture f;f.start();for(unsigned n=0;n<65;++n)f.metadata.imageTouched(f.command,n+2,11,xess_vk::RecordedImageTouch::Transfer);
        f.metadata.recordingPoint(f.command,70);const auto r=f.metadata.imageInterval(f.command,1,70,{10,1});
        require(!r.untouched&&(r.issues&xess_vk::bit(LI::JournalOverflow)),"bounded touch journal overflow refuses rather than proving absence");}
    for(unsigned variant=0;variant<6;++variant){Fixture f;f.start();sourceEvent(f,1,sourceBarrier(f,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        for(unsigned n=0;n<65;++n)f.metadata.imageTouched(f.command,n+2,11,xess_vk::RecordedImageTouch::Transfer);
        f.metadata.recordingPoint(f.command,70);
        if(variant==1)f.metadata.imageTouched(f.command,75,10,xess_vk::RecordedImageTouch::Clear);
        if(variant==2)f.metadata.imageTouched(f.command,75,11,xess_vk::RecordedImageTouch::Transfer);
        if(variant==3)sourceEvent(f,75,sourceBarrier(f,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        if(variant==4)f.metadata.unknownExplicitBarrier(f.command,75);
        if(variant==5)(void)f.metadata.unsupported(f.command,Issue::Secondary);
        f.metadata.recordingPoint(f.command,80);allocations=0;countAllocations=true;
        const auto r=f.metadata.imageInterval(f.command,70,80,{10,1});countAllocations=false;
        require(r.untouched==(variant==0||variant==2)&&!allocations,"old history loss cannot poison a complete untouched interval, without allocation");
        require(r.lastTouchOverflowOrder==(variant==1||variant==2?3u:2u)&&r.precedingOverflowIgnored,
            "loss watermark follows the evicted event, not the newest retained touch");
        require(variant!=1||(r.clears==1&&!r.issues),"a real target clear is retained and refused after rollover");
        const auto layout=f.metadata.resolveSourceAt(f.command,80,{10,1},f.view.subresourceRange,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        require(!layout.recordedPrefixSupported&&(layout.issues&xess_vk::bit(LI::JournalOverflow)),"interval acceptance cannot clear full-prefix layout overflow");}
    for(unsigned variant=0;variant<7;++variant){auto config=small();config.explicitImageEventsPerCommand=1;Fixture f(config);f.start();
        const auto b=sourceBarrier(f,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);sourceEvent(f,1,b);sourceEvent(f,2,b);
        f.metadata.recordingPoint(f.command,5);
        if(variant==1)sourceEvent(f,6,b);
        if(variant==2)f.metadata.imageTouched(f.command,6,10,xess_vk::RecordedImageTouch::Transfer);
        if(variant==3)f.metadata.unknownExplicitBarrier(f.command,6);
        if(variant==4){sourceEvent(f,6,b);sourceEvent(f,7,b);}
        if(variant==5||variant==6){std::array<VkImageMemoryBarrier,2> batch{b,b};
            f.metadata.explicitImageBarriers(f.command,6,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,2,batch.data());}
        if(variant==6)sourceEvent(f,7,b);
        f.metadata.recordingPoint(f.command,9);const auto r=f.metadata.imageInterval(f.command,5,9,{10,1});
        require(r.untouched==(variant==0)&&r.lastExplicitOverflowOrder==(variant>=4?6u:variant==1?2u:1u),"only loss before the read is excluded from the explicit-barrier interval");
        require(r.precedingOverflowIgnored==(variant<4)&&((variant<4)||bool(r.issues&xess_vk::bit(LI::JournalOverflow))),
            "an oversized call or an eviction inside the interval remains refused, including later rollover");
        const auto layout=f.metadata.resolveSourceAt(f.command,9,{10,1},f.view.subresourceRange,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        require(layout.issues&xess_vk::bit(LI::JournalOverflow),"full explicit prefix retains its original refusal");}
    {Fixture f;f.start();for(unsigned n=0;n<200;++n)f.metadata.imageTouched(f.command,n+1,11,xess_vk::RecordedImageTouch::Transfer);
        f.metadata.recordingPoint(f.command,210);f.metadata.recordingPoint(f.command,220);
        require(f.metadata.imageInterval(f.command,210,220,{10,1}).untouched,"multiple ring wraps before the read do not stop a later interval");
        for(unsigned n=0;n<65;++n)f.metadata.imageTouched(f.command,n+221,n?11:10,xess_vk::RecordedImageTouch::Clear);
        f.metadata.recordingPoint(f.command,300);const auto r=f.metadata.imageInterval(f.command,210,300,{10,1});
        require(!r.untouched&&!r.clears&&r.lastTouchOverflowOrder==221&&(r.issues&xess_vk::bit(LI::JournalOverflow)),
            "a target mutation evicted inside the interval cannot turn into a false untouched result");}
    {auto config=small();config.explicitImageEventsPerCommand=3;Fixture f(config);f.start();
        auto other=f.image;other.key={11,2};accepted(f.metadata.observeImage(other,VK_SUCCESS));
        auto b=sourceBarrier(f,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for(unsigned n=0;n<10;++n)sourceEvent(f,n+1,b);
        f.metadata.recordingPoint(f.command,20);b.image=handle<VkImage>(11);sourceEvent(f,21,b);
        f.metadata.recordingPoint(f.command,30);const auto r=f.metadata.imageInterval(f.command,20,30,{10,1});
        require(r.untouched&&r.lastExplicitOverflowOrder==8,"new unrelated barriers remain visible after multiple wraps without poisoning the target interval");
        std::array<xess_vk::RecordedImageKey,maxExplicitImageEvents> keys{};std::size_t count{};
        require(f.metadata.prefixImageKeys(f.command,30,keys,count)&&count==3&&keys[0].handle==10&&keys[1].handle==10&&keys[2].handle==11,
            "liveness key order follows chronological ring order");
        f.metadata.resetCommand(50,VK_SUCCESS);++f.command.generation;f.start();f.metadata.recordingPoint(f.command,5);f.metadata.recordingPoint(f.command,9);
        const auto reset=f.metadata.imageInterval(f.command,5,9,{10,1});require(reset.untouched&&!reset.lastExplicitOverflowOrder&&!reset.lastTouchOverflowOrder,
            "command reset clears retained events, ring positions and loss watermarks");}
    {Fixture f;f.start();f.metadata.recordingPoint(f.command,5);(void)f.metadata.unsupported(f.command,Issue::Secondary);
        f.metadata.recordingPoint(f.command,9);require(!f.metadata.imageInterval(f.command,5,9,{10,1}).untouched,
            "unobserved secondary contents cannot prove an untouched image");}
}
void recordedPrefixPositiveAndLegacyIsolation(){
    ++cases;Fixture f(small(),false,false);f.attachment.initialLayout=f.attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;f.registerAll();f.start();
    auto first=sourceBarrier(f,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);sourceEvent(f,1,first);first.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    f.record(2,3);sourceEvent(f,4,sourceBarrier(f,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    accepted(f.metadata.setProjectionSelection(0,selectionFor(f))); // Deliberately selected AFTER begin and barriers.
    allocations=0;countAllocations=true;const auto r=sourceQuery(f,5);countAllocations=false;
    require(r.recordedLayoutResolved&&r.recordedPrefixSupported&&!r.issues&&r.recordedLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,"actual full-range barrier/RP/barrier chain resolves independent of begin selection epoch");
    require(r.observedExplicitImages==2&&r.retainedExplicitImages==2&&!r.droppedExplicitImages&&r.examinedPasses==1&&r.directPasses==1&&r.matchedEvents==5,"exact journal and pass merge counters");
    require(r.anchor.order==4&&r.lastEvent.order==4&&r.lastEvent.explicitScopesKnown&&r.lastEvent.srcStage==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
        &&r.lastEvent.srcAccess==VK_ACCESS_SHADER_WRITE_BIT&&r.prefixOrder==5&&!allocations,"latest actual anchor/scopes and owned current-point query allocate nothing");
    require(!r.layoutVerified&&!r.gpuComplete&&!r.ownerVerified&&!r.resourceLifetimePinned&&!r.copyPermitted,"positive recorded chain stays CPU-only");
    Fixture longRecording;longRecording.start();for(std::uint64_t n=0;n<34;++n){(void)longRecording.metadata.beginRenderPass(longRecording.command,n*2+1,&longRecording.begin,VK_SUBPASS_CONTENTS_INLINE);(void)longRecording.metadata.endRenderPass(longRecording.command,n*2+2);}
    const auto longResult=sourceQuery(longRecording,69);require(longResult.recordedLayoutResolved&&longResult.examinedPasses==34&&longResult.lastEvent.order==68&&!longResult.lastEvent.explicitScopesKnown,"all-pass query is independent of legacy 96-operation overflow and does not invent RP scopes");
    require(longRecording.metadata.commandSnapshot(longRecording.command).issues&bit(Issue::Capacity),"legacy overflow acceptance remains unchanged");
    const auto old=sourceQuery(f,6);(void)f.metadata.beginRenderPass(f.command,7,&f.begin,VK_SUBPASS_CONTENTS_INLINE);(void)f.metadata.endRenderPass(f.command,8);
    layoutRefused(f.metadata.resolveSourceAt(f.command,6,{10,1},f.view.subresourceRange,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),LI::PrefixMismatch);
    require(old.recordedLayoutResolved&&old.prefixOrder==6,"later passes cannot alter an already owned earlier result");
    Fixture nonUndefined;nonUndefined.start();sourceEvent(nonUndefined,1,sourceBarrier(nonUndefined,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    const auto general=sourceQuery(nonUndefined,2);require(general.recordedLayoutResolved&&general.anchor.oldLayout==VK_IMAGE_LAYOUT_GENERAL,"a full actual GENERAL-to-shader-read transition anchors without an UNDEFINED event in this CB");
}
void recordedPrefixRefusals(){
    ++cases;
    {Fixture f;f.start();layoutRefused(sourceQuery(f,1),LI::NoTransitionAnchor);}
    {Fixture f(small(),false,false);f.reference.layout=VK_IMAGE_LAYOUT_GENERAL;f.attachment.initialLayout=f.attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;f.registerAll();f.start();f.record(1,2);layoutRefused(sourceQuery(f,3,VK_IMAGE_LAYOUT_GENERAL),LI::NoTransitionAnchor);}
    for(unsigned variant=0;variant<11;++variant){
        auto config=small();if(variant==10)config.explicitImageEventsPerCommand=2;Fixture f(config);f.start();auto b=sourceBarrier(f,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);sourceEvent(f,1,b);
        b=sourceBarrier(f,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if(variant==0)b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        if(variant==1)b.subresourceRange.levelCount=2;
        if(variant==2)b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        if(variant==3)b.pNext=&f.begin;
        if(variant==4)b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=0;
        if(variant==5)b.image=handle<VkImage>(999);
        if(variant==6){(void)f.metadata.beginRenderPass(f.command,2,nullptr,VK_SUBPASS_CONTENTS_INLINE);(void)f.metadata.endRenderPass(f.command,3);}
        if(variant==7)f.metadata.explicitImageBarriers(f.command,2,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_DEPENDENCY_BY_REGION_BIT,1,&b);
        else if(variant==8)b.srcAccessMask=static_cast<VkAccessFlags>(VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT);
        if(variant==9){b.oldLayout=b.newLayout=VK_IMAGE_LAYOUT_GENERAL;sourceEvent(f,2,b);}
        else if(variant==10){std::array<VkImageMemoryBarrier,2> batch{b,b};f.metadata.explicitImageBarriers(f.command,2,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,2,batch.data());}
        else if(variant!=6&&variant!=7)sourceEvent(f,2,b);
        const auto r=sourceQuery(f,4);const LI expected[]={LI::ChainMismatch,LI::RangeAmbiguous,LI::RangeAmbiguous,LI::UnsupportedPNext,LI::OwnershipTransfer,LI::UnknownObject,LI::PassIdentityUnknown,LI::UnsupportedDependency,LI::UnsupportedBarrierScope,LI::DeclaredLayoutMismatch,LI::JournalOverflow};
        layoutRefused(r,expected[variant]);
        if(variant==10){require(r.observedExplicitImages==3&&r.retainedExplicitImages==2&&r.droppedExplicitImages==1&&r.explicitImageHighWater==2&&r.explicitImageCapacity==2&&!r.explicitCallRefusals,"rollover retains the complete newest call while full-prefix queries still report lost history");sourceEvent(f,5,b);layoutRefused(sourceQuery(f,6),LI::JournalOverflow);}
    }
    {Fixture f;f.start();sourceEvent(f,1,sourceBarrier(f,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));f.metadata.recordingPoint(f.command,2);
        layoutRefused(f.metadata.resolveSourceAt(f.command,2,{10,2},f.view.subresourceRange,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),LI::StaleObject);
        layoutRefused(f.metadata.resolveSourceAt({50,2},2,{10,1},f.view.subresourceRange,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),LI::IdentityMismatch);
        f.metadata.destroyImage({10,1});layoutRefused(sourceQuery(f,3),LI::UnknownObject);}
    {Fixture f;f.start();sourceEvent(f,1,sourceBarrier(f,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL));f.record(2,3);
        f.metadata.destroyView(20);layoutRefused(sourceQuery(f,4),LI::PassIdentityUnknown);}
}
void recordedPrefixSync2AndCombinedDepthStencil(){
    ++cases;
    for(bool seed:{false,true}){Fixture f;f.start();if(seed)sourceEvent(f,1,sourceBarrier(f,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL));
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.image=handle<VkImage>(10);b.subresourceRange=f.view.subresourceRange;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.srcStageMask=b.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;b.oldLayout=b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=1;dep.pImageMemoryBarriers=&b;f.metadata.explicitImageBarriers2(f.command,2,&dep);
        const auto r=sourceQuery(f,3,VK_IMAGE_LAYOUT_GENERAL);if(seed)require(r.recordedLayoutResolved&&r.recordedLayout==VK_IMAGE_LAYOUT_GENERAL&&r.ignoredEqualSync2Events==1&&r.anchor.order==1,"equal sync2 ignored fields do not validate or change the anchored GENERAL layout");
        else layoutRefused(r,LI::NoTransitionAnchor);
        dep.pNext=&f.begin;f.metadata.explicitImageBarriers2(f.command,4,&dep);layoutRefused(sourceQuery(f,5,VK_IMAGE_LAYOUT_GENERAL),LI::UnsupportedPNext);
    }
    for(bool combined:{false,true}){Fixture f(small(),true,false);f.image.format=VK_FORMAT_D32_SFLOAT_S8_UINT;f.view.format=f.image.format;(void)f.metadata.observeImage(f.image,VK_SUCCESS);f.start();
        auto b=sourceBarrier(f,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);if(combined)b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;sourceEvent(f,1,b);
        const auto r=sourceQuery(f,2);if(combined)require(r.recordedLayoutResolved&&r.copiedAspect==VK_IMAGE_ASPECT_DEPTH_BIT&&r.resolvedLayoutRange.aspectMask==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT),"D32S8 depth-only copied aspect requires independently anchored combined layout range");
        else layoutRefused(r,LI::CombinedAspectRequired);
    }
    {Fixture f;f.start();auto b=sourceBarrier(f,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);sourceEvent(f,1,b);layoutRefused(sourceQuery(f,2),LI::NoTransitionAnchor);}
}
}
int main() {
    try {
        happyPaths();deepCopiesAndInput();renderPass2();failedDriver();lifecycleAndOrdering();destroyAndReuse();multipleAttachments();
        unsupportedCreateCases();framebufferAndBeginRefusals();budgetsAndFlags();noPerCallAllocation();globalGenerationContract();copiedDiagnosticSnapshots();observedSingleColorFormats();
        projectionHappyAndLegacy();projectionLocalIsolation();projectionStructuralAndIdentity();projectionSelectionEpochsAndTickets();projectionBudgetsAndLifecycle();projectionRP2AndAllocation();
        projectionSceneJournalCapacity();projectionDroppedBeginAndStorage();
        recordedPrefixPositiveAndLegacyIsolation();recordedPrefixRefusals();recordedPrefixSync2AndCombinedDepthStencil();
        sourceReadIntervals();
        std::cout << "{\"stage\":\"pure-render-pass-metadata\",\"testPassed\":true,\"cases\":" << cases << ",\"checks\":" << checks
            << ",\"storageBytes\":" << Metadata::allocatedStorageBytes() << ",\"hotPathAllocations\":" << allocations
            << ",\"metadataObjectBytes\":" << sizeof(Metadata) << ",\"constructorHeapAllocations\":" << constructorAllocations
            << ",\"constructorHeapBytes\":" << constructorHeapBytes << ",\"maxProjectionPasses\":" << maxProjectionPasses
            << ",\"maxExplicitImageEvents\":" << maxExplicitImageEvents << ",\"explicitJournalBytesPerCommand\":" << Metadata::explicitJournalStorageBytesPerCommand()
            << ",\"vulkanLoaded\":false,\"gpuTouched\":false,\"gameTouched\":false,\"productionLayerIntegrated\":false}\n";
    } catch(const std::exception& error) { countAllocations=false;std::cerr << "metadata test failed: " << error.what() << '\n';return 1; }
}
