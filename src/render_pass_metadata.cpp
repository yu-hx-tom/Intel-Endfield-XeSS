#include "render_pass_metadata.hpp"
#include "metadata_storage.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace xess_vk::render_pass_metadata {
namespace {
using LI=RecordedLayoutIssue;
VkImageAspectFlags sourceAspects(VkFormat format) noexcept;
template<class T> Handle bits(T handle) noexcept {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<Handle>(handle);
    else return static_cast<Handle>(handle);
}
VkImageAspectFlags aspects(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    default: return 0;
    }
}
bool layoutAllowed(VkImageLayout layout, VkImageAspectFlags aspect, bool initial) noexcept {
    if (initial && layout == VK_IMAGE_LAYOUT_UNDEFINED) return true;
    if (layout == VK_IMAGE_LAYOUT_GENERAL || layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        || layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) return true;
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT)
        return layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    return aspect == VK_IMAGE_ASPECT_DEPTH_BIT &&
        (layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
}
VkImageAspectFlags sourceAspects(VkFormat format) noexcept {
    if(format==VK_FORMAT_D32_SFLOAT_S8_UINT)return VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    return aspects(format);
}
bool sourceLayoutAllowed(VkImageLayout layout,VkImageAspectFlags aspect,bool old) noexcept {
    if(aspect==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT))aspect=VK_IMAGE_ASPECT_DEPTH_BIT;
    return layoutAllowed(layout,aspect,old);
}
bool sourceShape(const ImageDescription& d) noexcept {
    return sourceAspects(d.format)&&!d.unsupportedPNext&&!d.flags&&d.type==VK_IMAGE_TYPE_2D&&d.extent.width&&d.extent.height
        &&d.extent.depth==1&&d.mipLevels==1&&d.arrayLayers==1&&d.samples==VK_SAMPLE_COUNT_1_BIT
        &&d.tiling==VK_IMAGE_TILING_OPTIMAL&&d.sharing==VK_SHARING_MODE_EXCLUSIVE;
}
bool overlaps(const VkImageSubresourceRange& a,const VkImageSubresourceRange& b) noexcept {
    return (a.aspectMask&b.aspectMask)&&std::uint64_t(a.baseMipLevel)<std::uint64_t(b.baseMipLevel)+b.levelCount
        &&std::uint64_t(b.baseMipLevel)<std::uint64_t(a.baseMipLevel)+a.levelCount
        &&std::uint64_t(a.baseArrayLayer)<std::uint64_t(b.baseArrayLayer)+b.layerCount
        &&std::uint64_t(b.baseArrayLayer)<std::uint64_t(a.baseArrayLayer)+a.layerCount;
}
constexpr VkPipelineStageFlags2 sourceStageMask=VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT|VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
    |VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT
    |VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT|VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
    |VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
    |VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT|VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT|VK_PIPELINE_STAGE_2_HOST_BIT
    |VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT|VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_COPY_BIT|VK_PIPELINE_STAGE_2_RESOLVE_BIT
    |VK_PIPELINE_STAGE_2_BLIT_BIT|VK_PIPELINE_STAGE_2_CLEAR_BIT|VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT|VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT
    |VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
constexpr VkAccessFlags2 sourceAccessMask=VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_2_INDEX_READ_BIT|VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT
    |VK_ACCESS_2_UNIFORM_READ_BIT|VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT|VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT
    |VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
    |VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT|VK_ACCESS_2_HOST_READ_BIT
    |VK_ACCESS_2_HOST_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
    |VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
bool regularLoad(VkAttachmentLoadOp op) noexcept {
    return op == VK_ATTACHMENT_LOAD_OP_LOAD || op == VK_ATTACHMENT_LOAD_OP_CLEAR || op == VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}
bool regularStore(VkAttachmentStoreOp op) noexcept {
    return op == VK_ATTACHMENT_STORE_OP_STORE || op == VK_ATTACHMENT_STORE_OP_DONT_CARE;
}
bool sameRange(const VkImageSubresourceRange& a,const VkImageSubresourceRange& b) noexcept {
    return a.aspectMask==b.aspectMask && a.baseMipLevel==b.baseMipLevel && a.levelCount==b.levelCount
        && a.baseArrayLayer==b.baseArrayLayer && a.layerCount==b.layerCount;
}
bool sameSelection(const ProjectionSelection& a,const ProjectionSelection& b) noexcept {
    return a.selectionSerial==b.selectionSerial && a.image==b.image && sameRange(a.range,b.range)
        && a.sourceToken==b.sourceToken && a.passToken==b.passToken && a.enabled==b.enabled;
}
bool normalizedRange(const ImageDescription& image,VkImageSubresourceRange& r) noexcept {
    constexpr VkImageAspectFlags ordinary=VK_IMAGE_ASPECT_COLOR_BIT|VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!r.aspectMask || (r.aspectMask & ~ordinary) || !image.mipLevels || !image.arrayLayers
        || r.baseMipLevel>=image.mipLevels || r.baseArrayLayer>=image.arrayLayers) return false;
    if (r.levelCount==VK_REMAINING_MIP_LEVELS) r.levelCount=image.mipLevels-r.baseMipLevel;
    if (r.layerCount==VK_REMAINING_ARRAY_LAYERS) r.layerCount=image.arrayLayers-r.baseArrayLayer;
    return r.levelCount && r.layerCount && r.levelCount<=image.mipLevels-r.baseMipLevel
        && r.layerCount<=image.arrayLayers-r.baseArrayLayer;
}
}

struct Metadata::Impl {
    static constexpr std::size_t imageLimit=1024, viewLimit=2048, rpLimit=256, fbLimit=512, commandLimit=128;
    struct Image { Key key; ImageDescription description; std::uint64_t issues{}; };
    struct View { Key key, image; VkFormat format{}; VkImageSubresourceRange range{}; std::uint64_t issues{}; ViewSnapshot facts; bool identityCopied{}; };
    enum class Role { Color, Depth, Input };
    struct Attachment {
        VkFormat format{}; VkImageLayout initial{}, reference{}, final{};
        VkImageAspectFlags aspects{}; Role role{}; bool used{}; std::uint64_t localIssues{};
    };
    struct RenderPass { Key key; std::array<Attachment,maxAttachments> attachments{}; std::size_t count{}; std::uint64_t issues{}, structuralIssues{}; RenderPassSnapshot facts; bool parsed{}; };
    struct Framebuffer {
        Key key, renderPass; std::array<Key,maxAttachments> views{};
        std::size_t count{}; std::uint32_t width{},height{}; std::uint64_t issues{}; FramebufferSnapshot facts; bool identityCopied{};
    };
    struct ProjectionAttachment { Key view,image; VkImageSubresourceRange range{}; };
    struct ProjectionPass {
        Key renderPass,framebuffer;
        std::array<ProjectionAttachment,maxAttachments> attachments{};
        std::size_t count{}; std::uint64_t beginOrder{},endOrder{};
    };
    struct ProjectionRecording {
        std::array<ProjectionSelection,projectionSlots> selections{};
        std::array<std::uint64_t,projectionSlots> selectionIssues{};
        std::array<ProjectionPass,maxProjectionPasses> passes{};
        std::size_t count{}; std::uint64_t issues{},lastOrder{},pendingTicket{},submissions{};
        std::uint64_t observedPasses{},droppedPasses{};
        std::size_t activePass{maxProjectionPasses}; // invalid when Begin was dropped
        bool inPass{};
    };
    struct ExplicitImageEvent {Key image;image_inventory::Description description;VkImageSubresourceRange range{};RecordedLayoutEvent event;std::uint64_t issues{};};
    struct RecordedPrefix {
        std::array<ExplicitImageEvent,maxExplicitImageEvents> images{};
        std::size_t count{},firstImage{};std::uint64_t currentOrder{},issues{},observedImages{},droppedImages{},callRefusals{};
        struct Touch {std::uint64_t order{};Handle image{};RecordedImageTouch kind{};};
        std::array<Touch,64> touches{};
        std::size_t touchCount{},firstTouch{};
        // Latest discarded event order, not the order of the append that evicted it.
        std::uint64_t lastExplicitOverflowOrder{},lastTouchOverflowOrder{};
        const ExplicitImageEvent& imageAt(std::size_t n,std::size_t capacity) const noexcept {return images[(firstImage+n)%capacity];}
    };
    enum class State { Invalid, Recording, Executable };
    struct Command {
        Key key; State state{State::Invalid}; std::uint64_t issues{}, lastOrder{}, submissions{};
        VkCommandBufferUsageFlags flags{}; bool inPass{};
        Key activeRenderPass, activeFramebuffer;
        std::array<LayoutOperation,maxOperations> operations{}; std::size_t count{}, activeBegin{};
        ProjectionRecording projection;
        RecordedPrefix prefix;
    };
    Config config;
    metadata_storage::Slots<Image,imageLimit> images;
    metadata_storage::Slots<View,viewLimit> views;
    metadata_storage::Slots<RenderPass,rpLimit> renderPasses;
    metadata_storage::Slots<Framebuffer,fbLimit> framebuffers;
    metadata_storage::Slots<Command,commandLimit> commands;
    std::uint64_t objectGeneration{}, imageGeneration{}, commandGeneration{};
    std::uint64_t selectionGeneration{},ticketGeneration{};
    std::array<ProjectionSelection,projectionSlots> selections{};
    std::array<std::uint64_t,projectionSlots> selectionIssues{};
    Stats totals;
    explicit Impl(Config c):config(c) {
        if (!c.images || c.images>imageLimit || !c.views || c.views>viewLimit || !c.renderPasses || c.renderPasses>rpLimit
            || !c.framebuffers || c.framebuffers>fbLimit || !c.commands || c.commands>commandLimit
            || !c.operationsPerCommand || c.operationsPerCommand>maxOperations
            || !c.projectionPassesPerCommand || c.projectionPassesPerCommand>maxProjectionPasses
            || !c.explicitImageEventsPerCommand || c.explicitImageEventsPerCommand>maxExplicitImageEvents)
            throw std::invalid_argument("render pass metadata capacity outside hard bounds");
    }
    template<class T, std::size_t N> static T* find(metadata_storage::Slots<T,N>& entries, std::size_t limit, Handle handle) noexcept {
        if (handle) for (std::size_t i=0;i<std::min(limit,entries.extent());++i) if(auto* row=entries.get(i);row&&row->key.handle==handle) return row;
        return nullptr;
    }
    template<class T, std::size_t N> T* insert(metadata_storage::Slots<T,N>& entries, std::size_t limit, Handle handle) noexcept {
        if (!handle) return nullptr;
        if (find(entries,limit,handle)) { totals.globalIssues |= bit(Issue::Lifecycle); return nullptr; }
        for (std::size_t i=0;i<limit;++i) if (!entries.get(i)||!entries[i].key.handle) {
            if (objectGeneration==UINT64_MAX) { totals.globalIssues |= bit(Issue::Capacity); return nullptr; }
            if(!entries.ensure(i)){totals.globalIssues|=bit(Issue::Capacity);return nullptr;}
            std::destroy_at(&entries[i]);std::construct_at(&entries[i]);
            entries[i].key={handle,++objectGeneration}; ++totals.registrations; return &entries[i];
        }
        totals.globalIssues |= bit(Issue::Capacity); return nullptr;
    }
    Image* image(Handle h) noexcept { return find(images,config.images,h); }
    View* view(Handle h) noexcept { return find(views,config.views,h); }
    RenderPass* renderPass(Handle h) noexcept { return find(renderPasses,config.renderPasses,h); }
    Framebuffer* framebuffer(Handle h) noexcept { return find(framebuffers,config.framebuffers,h); }
    Command* command(Handle h) noexcept { return find(commands,config.commands,h); }
    Result failed(VkResult result) noexcept {
        if (result==VK_ERROR_DEVICE_LOST) totals.globalIssues |= bit(Issue::DeviceLost);
        return {{},bit(Issue::DriverFailed)|totals.globalIssues};
    }
    template<class T> Result result(T* object) const noexcept {
        return object ? Result{object->key,object->issues|totals.globalIssues}
            : Result{{},totals.globalIssues ? totals.globalIssues : bit(Issue::InvalidInput)};
    }
    Command* recording(Key key, std::uint64_t order, Result& result) noexcept {
        auto* c=command(key.handle);
        if (!c) { result={{},bit(Issue::UnknownObject)|totals.globalIssues}; return nullptr; }
        if (c->key!=key) { result={key,bit(Issue::StaleGeneration)|totals.globalIssues}; return nullptr; }
        prefixPoint(*c,order);
        auto& projection=c->projection;
        if (c->state!=State::Recording || !order || order<=projection.lastOrder) projection.issues |= bit(Issue::Lifecycle);
        else projection.lastOrder=order;
        if (c->state!=State::Recording || !order || order<=c->lastOrder) c->issues |= bit(Issue::Lifecycle);
        else c->lastOrder=order;
        result=this->result(c); return c;
    }
    void prefixPoint(Command& c,std::uint64_t order) noexcept {
        if(c.state!=State::Recording||!order||order<c.prefix.currentOrder)c.prefix.issues|=xess_vk::bit(LI::PrefixMismatch);
        else c.prefix.currentOrder=order;
    }
    RecordedPrefix* explicitCall(Key key,std::uint64_t order,std::uint32_t count,const void* entries) noexcept {
        auto* c=command(key.handle);if(!c||c->key!=key)return nullptr;prefixPoint(*c,order);auto& out=c->prefix;
        const auto add=[&](std::uint64_t& counter,std::uint64_t amount){
            if(amount>UINT64_MAX-counter){counter=UINT64_MAX;out.lastExplicitOverflowOrder=out.currentOrder;
                out.issues|=xess_vk::bit(LI::JournalOverflow);}else counter+=amount;};
        add(out.observedImages,count);
        const auto capacity=config.explicitImageEventsPerCommand;
        if(count>capacity||(!entries&&count)){
            if(count>capacity)out.lastExplicitOverflowOrder=out.currentOrder;
            add(out.droppedImages,count);add(out.callRefusals,1);out.issues|=xess_vk::bit(count>capacity?LI::JournalOverflow:LI::OpaqueControl);return nullptr;
        }
        // Retain the complete newest call; prefix reconstruction still sees the loss.
        while(count>capacity-out.count){
            out.lastExplicitOverflowOrder=std::max(out.lastExplicitOverflowOrder,out.images[out.firstImage].event.order);
            out.firstImage=(out.firstImage+1)%capacity;--out.count;add(out.droppedImages,1);
            out.issues|=xess_vk::bit(LI::JournalOverflow);
        }
        return &out;
    }
    template<class B> ExplicitImageEvent imageBarrier(const B& b,std::uint64_t order,std::uint32_t sequence,
        RecordedLayoutEventKind kind,VkPipelineStageFlags2 src,VkPipelineStageFlags2 dst,const image_inventory::Snapshot* inventory=nullptr) noexcept {
        ExplicitImageEvent out;out.image.handle=bits(b.image);out.range=b.subresourceRange;out.event.order=order;out.event.sequence=sequence;out.event.kind=kind;
        out.event.oldLayout=b.oldLayout;out.event.newLayout=b.newLayout;out.event.srcStage=src;out.event.dstStage=dst;
        out.event.srcAccess=b.srcAccessMask;out.event.dstAccess=b.dstAccessMask;out.event.explicitScopesKnown=true;
        if(b.pNext)out.issues|=xess_vk::bit(LI::UnsupportedPNext);
        constexpr auto expected=std::is_same_v<B,VkImageMemoryBarrier>?VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER:VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        if(b.sType!=expected)out.issues|=xess_vk::bit(LI::OpaqueControl);
        // Even equal non-IGNORED family indices are unaudited owner facts.
        if(b.srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED||b.dstQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED)out.issues|=xess_vk::bit(LI::OwnershipTransfer);
        if((src|dst)&~sourceStageMask||(b.srcAccessMask|b.dstAccessMask)&~sourceAccessMask)out.issues|=xess_vk::bit(LI::UnsupportedBarrierScope);
        if(inventory){
            if(inventory->status==image_inventory::Status::LiveHandleObserved&&inventory->record.image==out.image.handle&&inventory->record.generation){
                out.image={inventory->record.image,inventory->record.generation};out.description=inventory->record.description;
            }else out.issues|=xess_vk::bit(LI::UnknownObject);
        }else if(auto* i=image(out.image.handle)){
            out.image=i->key;const auto& d=i->description;
            out.description={std::uint32_t(d.format),d.extent.width,d.extent.height,d.extent.depth,d.mipLevels,d.arrayLayers,std::uint32_t(d.samples),d.usage,d.flags,std::uint32_t(d.tiling),std::uint32_t(d.sharing),0};
        }else out.issues|=xess_vk::bit(LI::UnknownObject);
        if(out.image.generation){ImageDescription bounds;bounds.mipLevels=out.description.mipLevels;bounds.arrayLayers=out.description.arrayLayers;
            if(!normalizedRange(bounds,out.range)||(out.range.aspectMask&~sourceAspects(static_cast<VkFormat>(out.description.format))))out.issues|=xess_vk::bit(LI::RangeAmbiguous);
        }
        return out;
    }
    template<class T> void attachment(RenderPass& rp, std::size_t i, const T& a) noexcept {
        auto& out=rp.attachments[i]; out.format=a.format; out.aspects=aspects(a.format);
        out.initial=a.initialLayout; out.final=a.finalLayout;
        if (!out.aspects) out.localIssues |= bit(Issue::UnsupportedFormat);
        if (a.flags) out.localIssues |= bit(Issue::Aliasing);
        if (a.flags & ~VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT) rp.structuralIssues |= bit(Issue::Aliasing);
        if (a.samples!=VK_SAMPLE_COUNT_1_BIT || !regularLoad(a.loadOp) || !regularStore(a.storeOp)) out.localIssues |= bit(Issue::UnsupportedAttachment);
        if (a.loadOp==VK_ATTACHMENT_LOAD_OP_LOAD && a.initialLayout==VK_IMAGE_LAYOUT_UNDEFINED) out.localIssues |= bit(Issue::InvalidInput);
        if (!layoutAllowed(a.initialLayout,out.aspects,true) || !layoutAllowed(a.finalLayout,out.aspects,false)) out.localIssues |= bit(Issue::UnsupportedLayout);
        if constexpr (requires { a.pNext; }) {
            if (a.pNext) rp.structuralIssues |= bit(Issue::UnsupportedExtension);
            if (a.sType!=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2) rp.structuralIssues |= bit(Issue::InvalidInput);
        }
        rp.issues |= out.localIssues|rp.structuralIssues;
    }
    template<class T> void reference(RenderPass& rp, const T& ref, Role role) noexcept {
        if constexpr (requires { ref.pNext; }) {
            if (ref.pNext) rp.structuralIssues |= bit(Issue::UnsupportedExtension);
            if (ref.sType!=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2) rp.structuralIssues |= bit(Issue::InvalidInput);
        }
        rp.issues |= rp.structuralIssues;
        if (ref.attachment==VK_ATTACHMENT_UNUSED) return;
        if (ref.attachment>=rp.count) { rp.structuralIssues |= bit(Issue::InvalidInput); rp.issues |= bit(Issue::InvalidInput); return; }
        auto& a=rp.attachments[ref.attachment];
        auto& facts=rp.facts.attachments[ref.attachment];++facts.referenceCount;facts.reference=ref.layout;facts.role=static_cast<std::uint32_t>(role);
        // Reject repeated references, including valid feedback paths, in R0.
        if (a.used) a.localIssues |= bit(Issue::UnsupportedAttachment);
        a.used=true; a.role=role; a.reference=ref.layout;
        bool valid=ref.layout==VK_IMAGE_LAYOUT_GENERAL;
        if (role==Role::Color) {
            valid |= ref.layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            valid &= a.aspects==VK_IMAGE_ASPECT_COLOR_BIT;
        } else if (role==Role::Depth) {
            valid |= ref.layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || ref.layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            valid &= a.aspects==VK_IMAGE_ASPECT_DEPTH_BIT;
        } else {
            valid |= ref.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                (a.aspects==VK_IMAGE_ASPECT_DEPTH_BIT && ref.layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        }
        if (!valid) a.localIssues |= bit(Issue::UnsupportedLayout);
        if constexpr (requires { ref.aspectMask; }) {
            // For input references require the complete, explicit supported aspect.
            if ((role==Role::Input && ref.aspectMask!=a.aspects)
                || (role!=Role::Input && ref.aspectMask && ref.aspectMask!=a.aspects))
                a.localIssues |= bit(Issue::UnsupportedAttachment);
        }
        rp.issues |= a.localIssues;
    }
    template<class T> void parseRenderPass(RenderPass& rp, const T* info) noexcept {
        // Classify at the original parsing site. Never subtract inherited R0
        // bits later to guess whether an issue was local to an attachment.
        const auto structural=[&](Issue issue) { rp.structuralIssues |= bit(issue); rp.issues |= bit(issue); };
        if (!info) { structural(Issue::InvalidInput); return; }
        auto& facts=rp.facts;facts.hasPNext=info->pNext!=nullptr;facts.flags=info->flags;
        facts.attachmentCount=info->attachmentCount;facts.subpassCount=info->subpassCount;facts.dependencyCount=info->dependencyCount;
        if(info->attachmentCount<=maxAttachments&&info->pAttachments){
            facts.copiedAttachmentCount=info->attachmentCount;
            for(std::size_t i=0;i<info->attachmentCount;++i){const auto& a=info->pAttachments[i];auto& out=facts.attachments[i];
                out.format=a.format;out.samples=a.samples;out.flags=a.flags;out.loadOp=a.loadOp;out.storeOp=a.storeOp;
                out.stencilLoadOp=a.stencilLoadOp;out.stencilStoreOp=a.stencilStoreOp;out.initial=a.initialLayout;out.final=a.finalLayout;
                out.aspects=aspects(a.format);if constexpr(requires{a.pNext;})out.hasPNext=a.pNext!=nullptr;
            }
        }
        if (info->pNext) structural(Issue::UnsupportedExtension);
        if (info->flags) structural(Issue::UnsupportedAttachment);
        if (!info->attachmentCount || info->attachmentCount>maxAttachments || !info->pAttachments) {
            structural(info->attachmentCount>maxAttachments ? Issue::Capacity : Issue::InvalidInput); return;
        }
        if (info->subpassCount!=1 || !info->pSubpasses) { structural(Issue::MultipleSubpasses); return; }
        if (info->dependencyCount>16 || (info->dependencyCount && !info->pDependencies)) { structural(Issue::Capacity); return; }
        rp.count=info->attachmentCount;
        for (std::size_t i=0;i<rp.count;++i) attachment(rp,i,info->pAttachments[i]);
        const auto& sub=info->pSubpasses[0];
        if (sub.flags || sub.pipelineBindPoint!=VK_PIPELINE_BIND_POINT_GRAPHICS || sub.pResolveAttachments || sub.preserveAttachmentCount)
            structural(Issue::UnsupportedAttachment);
        if constexpr (requires { sub.pNext; }) {
            if (sub.pNext) structural(Issue::UnsupportedExtension);
            if (sub.viewMask || info->correlatedViewMaskCount) structural(Issue::UnsupportedAttachment);
            if (sub.sType!=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2) structural(Issue::InvalidInput);
        }
        if (sub.colorAttachmentCount>maxAttachments || sub.inputAttachmentCount>maxAttachments
            || (sub.colorAttachmentCount && !sub.pColorAttachments) || (sub.inputAttachmentCount && !sub.pInputAttachments)) {
            structural(Issue::Capacity); return;
        }
        for (std::size_t i=0;i<sub.colorAttachmentCount;++i) reference(rp,sub.pColorAttachments[i],Role::Color);
        for (std::size_t i=0;i<sub.inputAttachmentCount;++i) reference(rp,sub.pInputAttachments[i],Role::Input);
        if (sub.pDepthStencilAttachment) reference(rp,*sub.pDepthStencilAttachment,Role::Depth);
        // Unused attachments STILL undergo initial->final transitions by spec.
        // Refuse the entire pass rather than silently omitting those changes.
        for (std::size_t i=0;i<rp.count;++i) if (!rp.attachments[i].used) rp.issues |= bit(Issue::UnsupportedAttachment);
        for (std::size_t i=0;i<info->dependencyCount;++i) {
            const auto& d=info->pDependencies[i];
            if ((d.srcSubpass!=0 && d.srcSubpass!=VK_SUBPASS_EXTERNAL)
                || (d.dstSubpass!=0 && d.dstSubpass!=VK_SUBPASS_EXTERNAL)
                || (d.srcSubpass==VK_SUBPASS_EXTERNAL && d.dstSubpass==VK_SUBPASS_EXTERNAL)) structural(Issue::InvalidInput);
            if (d.dependencyFlags & ~VK_DEPENDENCY_BY_REGION_BIT) structural(Issue::UnsupportedExtension);
            if constexpr (requires { d.pNext; }) {
                if (d.pNext || d.viewOffset) structural(Issue::UnsupportedExtension);
                if (d.sType!=VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2) structural(Issue::InvalidInput);
            }
        }
        rp.parsed=true;
    }
    void beginProjection(Command& command,std::uint64_t order,const VkRenderPassBeginInfo* info,VkSubpassContents contents) noexcept {
        auto& out=command.projection;
        std::uint64_t issues=0;
        if(out.observedPasses==UINT64_MAX)issues|=bit(Issue::Capacity);else ++out.observedPasses;
        if (out.inPass) issues |= bit(Issue::Lifecycle)|bit(Issue::IncompleteRenderPass);
        out.inPass=true;out.activePass=maxProjectionPasses;
        const auto drop=[&](std::uint64_t reason) noexcept {
            out.issues |= issues|reason;
            if(out.droppedPasses==UINT64_MAX)out.issues|=bit(Issue::Capacity);else ++out.droppedPasses;
        };
        if (contents!=VK_SUBPASS_CONTENTS_INLINE) issues |= bit(Issue::Secondary);
        if (!info || info->sType!=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO) { drop(bit(Issue::InvalidInput));return; }
        if (info->pNext) issues |= bit(Issue::UnsupportedExtension);
        auto* rp=renderPass(bits(info->renderPass)); auto* fb=framebuffer(bits(info->framebuffer));
        if (!rp || !fb) { drop(bit(Issue::UnknownObject));return; }
        if (!rp->parsed || !fb->identityCopied) issues |= bit(Issue::UnknownObject);
        issues |= rp->structuralIssues;
        if (fb->facts.hasPNext) issues |= bit(Issue::UnsupportedExtension);
        if (fb->facts.flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) issues |= bit(Issue::Imageless);
        if (fb->facts.flags || fb->facts.layers!=1 || !fb->width || !fb->height
            || fb->renderPass!=rp->key || fb->count!=rp->count) issues |= bit(Issue::IncompatibleFramebuffer);
        if (info->renderArea.offset.x<0 || info->renderArea.offset.y<0 || !info->renderArea.extent.width || !info->renderArea.extent.height
            || std::uint64_t(info->renderArea.offset.x)+info->renderArea.extent.width>fb->width
            || std::uint64_t(info->renderArea.offset.y)+info->renderArea.extent.height>fb->height
            || info->clearValueCount>maxAttachments || (info->clearValueCount && !info->pClearValues)) issues |= bit(Issue::InvalidInput);
        if (out.count>=config.projectionPassesPerCommand) { drop(bit(Issue::Capacity));return; }
        ProjectionPass pass;pass.renderPass=rp->key;pass.framebuffer=fb->key;pass.count=fb->count;pass.beginOrder=order;
        if (pass.count>maxAttachments) { drop(bit(Issue::Capacity));return; }
        // All attachment identities are required, even unused or unrelated ones.
        for (std::size_t n=0;n<pass.count;++n) {
            auto* v=view(fb->views[n].handle);
            if (!v) { issues |= bit(Issue::UnknownObject); continue; }
            if (v->key!=fb->views[n]) { issues |= bit(Issue::StaleGeneration); continue; }
            auto* i=image(v->image.handle);
            if (!i) { issues |= bit(Issue::UnknownObject); continue; }
            if (i->key!=v->image) { issues |= bit(Issue::StaleGeneration); continue; }
            if (!v->identityCopied) issues |= bit(Issue::UnknownObject);
            if (v->facts.hasPNext || i->description.unsupportedPNext) issues |= bit(Issue::UnsupportedExtension);
            // These affect how view subresources map to an image. Ordinary 2D
            // array/mip ranges are copied even when R0 rejects their shape.
            if (v->facts.flags || (v->facts.type!=VK_IMAGE_VIEW_TYPE_2D && v->facts.type!=VK_IMAGE_VIEW_TYPE_2D_ARRAY)
                || i->description.type!=VK_IMAGE_TYPE_2D || i->description.extent.depth!=1) issues |= bit(Issue::UnsupportedView);
            auto& identity=pass.attachments[n];identity.view=v->key;identity.image=i->key;identity.range=v->facts.rawRange;
            if (!normalizedRange(i->description,identity.range)) issues |= bit(Issue::UnsupportedView);
        }
        if(issues){drop(0);return;}
        out.activePass=out.count;out.passes[out.count++]=pass;
    }
    void endProjection(Command& command,std::uint64_t order) noexcept {
        auto& out=command.projection;
        if (!out.inPass) out.issues |= bit(Issue::Lifecycle)|bit(Issue::IncompleteRenderPass);
        out.inPass=false;
        const auto active=out.activePass;out.activePass=maxProjectionPasses;
        // A failed Begin has no entry to end. Never mutate a preceding pass or
        // manufacture an IncompleteRenderPass solely from a dropped entry.
        if(active==maxProjectionPasses)return;
        if(active>=out.count || out.passes[active].endOrder || order<=out.passes[active].beginOrder)
            out.issues |= bit(Issue::IncompleteRenderPass);
        else out.passes[active].endOrder=order;
    }
    std::uint64_t liveProjectionPass(const ProjectionPass& pass) noexcept {
        auto* rp=renderPass(pass.renderPass.handle); auto* fb=framebuffer(pass.framebuffer.handle);
        if (!rp || !fb) return bit(Issue::UnknownObject);
        if (rp->key!=pass.renderPass || fb->key!=pass.framebuffer || fb->renderPass!=rp->key) return bit(Issue::StaleGeneration);
        if (!pass.endOrder || pass.endOrder<=pass.beginOrder || pass.count!=fb->count || pass.count>maxAttachments)
            return bit(Issue::IncompleteRenderPass);
        std::uint64_t issues=0;
        for (std::size_t n=0;n<pass.count;++n) {
            const auto& attachment=pass.attachments[n]; auto* v=view(attachment.view.handle);auto* i=image(attachment.image.handle);
            if (!v || !i) { issues |= bit(Issue::UnknownObject); continue; }
            if (v->key!=attachment.view || i->key!=attachment.image || v->image!=i->key || fb->views[n]!=v->key)
                issues |= bit(Issue::StaleGeneration);
        }
        return issues;
    }
    std::uint64_t candidateAttachment(const ProjectionSelection& selection,const ProjectionPass& pass,std::size_t n) noexcept {
        const auto& identity=pass.attachments[n];auto* i=image(identity.image.handle);auto* v=view(identity.view.handle);
        auto* rp=renderPass(pass.renderPass.handle);auto* fb=framebuffer(pass.framebuffer.handle);
        if (!i || !v || !rp || !fb) return bit(Issue::UnknownObject);
        const auto& attachment=rp->attachments[n];const auto& desc=i->description;
        std::uint64_t issues=i->issues|v->issues|attachment.localIssues;
        if (!sameRange(selection.range,identity.range)) issues |= bit(Issue::UnsupportedView);
        if (v->format!=attachment.format || identity.range.aspectMask!=attachment.aspects
            || desc.extent.width<fb->width || desc.extent.height<fb->height) issues |= bit(Issue::IncompatibleFramebuffer);
        if (attachment.used) {
            const VkImageUsageFlags required=attachment.role==Role::Color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT :
                attachment.role==Role::Depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
            if (!(desc.usage & required)) issues |= bit(Issue::IncompatibleFramebuffer);
        }
        // Any declared pass alias or duplicate image attachment makes direct
        // candidate layout attribution unsupported in this first version.
        for (std::size_t a=0;a<pass.count;++a) {
            if (rp->facts.attachments[a].flags) issues |= bit(Issue::Aliasing);
            for (std::size_t b=0;b<a;++b) if (pass.attachments[a].image==pass.attachments[b].image) issues |= bit(Issue::Aliasing);
        }
        return issues;
    }
    std::uint64_t liveOperation(const LayoutOperation& op) noexcept {
        auto* i=image(op.image.handle); auto* v=view(op.view.handle);
        auto* r=renderPass(op.renderPass.handle); auto* f=framebuffer(op.framebuffer.handle);
        if (!i || !v || !r || !f) return bit(Issue::UnknownObject);
        if (i->key!=op.image || v->key!=op.view || r->key!=op.renderPass || f->key!=op.framebuffer
            || v->image!=op.image || f->renderPass!=op.renderPass) return bit(Issue::StaleGeneration);
        return i->issues|v->issues|r->issues|f->issues;
    }
};

Metadata::Metadata(Config config):impl_(std::make_unique<Impl>(config)) {}
Metadata::~Metadata()=default;
std::size_t Metadata::allocatedStorageBytes() noexcept { return sizeof(Impl); }
std::size_t Metadata::storageBytes() const noexcept {const auto& p=*impl_;return sizeof(Impl)+p.images.allocatedBytes()+p.views.allocatedBytes()+p.renderPasses.allocatedBytes()+p.framebuffers.allocatedBytes()+p.commands.allocatedBytes();}
Result Metadata::observeImage(const ImageDescription& desc,VkResult driver) noexcept {
    auto& p=*impl_; if (driver!=VK_SUCCESS) return p.failed(driver);
    if (p.totals.globalIssues) return {{},p.totals.globalIssues};
    if (!desc.key.handle || !desc.key.generation || desc.key.generation<=p.imageGeneration) {
        p.totals.globalIssues |= bit(Issue::StaleGeneration); return {desc.key,p.totals.globalIssues};
    }
    auto* image=p.insert(p.images,p.config.images,desc.key.handle); if (!image) return p.result(image);
    image->key=desc.key; image->description=desc; p.imageGeneration=desc.key.generation;
    if (desc.unsupportedPNext) image->issues |= bit(Issue::UnsupportedExtension);
    if (!aspects(desc.format)) image->issues |= bit(Issue::UnsupportedFormat);
    if (desc.type!=VK_IMAGE_TYPE_2D || !desc.extent.width || !desc.extent.height || desc.extent.depth!=1
        || desc.mipLevels!=1 || desc.arrayLayers!=1 || desc.samples!=VK_SAMPLE_COUNT_1_BIT || desc.flags
        || desc.tiling!=VK_IMAGE_TILING_OPTIMAL || desc.sharing!=VK_SHARING_MODE_EXCLUSIVE) image->issues |= bit(Issue::UnsupportedImage);
    return p.result(image);
}
Result Metadata::createView(Handle handle,const VkImageViewCreateInfo* info,VkResult driver) noexcept {
    auto& p=*impl_; if (driver!=VK_SUCCESS) return p.failed(driver);
    if (p.totals.globalIssues) return {{},p.totals.globalIssues};
    auto* view=p.insert(p.views,p.config.views,handle); if (!view) return p.result(view);
    if (!info || info->sType!=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO) { view->issues |= bit(Issue::InvalidInput); return p.result(view); }
    view->facts.hasPNext=info->pNext!=nullptr;view->facts.type=info->viewType;view->facts.flags=info->flags;
    view->facts.format=info->format;view->facts.components=info->components;view->facts.rawRange=info->subresourceRange;
    view->facts.image={bits(info->image),0};
    if (info->pNext) view->issues |= bit(Issue::UnsupportedExtension);
    auto* image=p.image(bits(info->image));
    if (!image) { view->issues |= bit(Issue::UnknownObject); return p.result(view); }
    view->identityCopied=true;
    view->image=image->key; view->format=info->format; view->range=info->subresourceRange; view->issues |= image->issues;
    const auto& r=view->range;
    if (info->flags || info->viewType!=VK_IMAGE_VIEW_TYPE_2D || info->format!=image->description.format
        || r.aspectMask!=aspects(info->format) || r.baseMipLevel || r.baseArrayLayer
        || (r.levelCount!=1 && r.levelCount!=VK_REMAINING_MIP_LEVELS)
        || (r.layerCount!=1 && r.layerCount!=VK_REMAINING_ARRAY_LAYERS)
        || info->components.r!=VK_COMPONENT_SWIZZLE_IDENTITY || info->components.g!=VK_COMPONENT_SWIZZLE_IDENTITY
        || info->components.b!=VK_COMPONENT_SWIZZLE_IDENTITY || info->components.a!=VK_COMPONENT_SWIZZLE_IDENTITY)
        view->issues |= bit(Issue::UnsupportedView);
    view->range.levelCount=view->range.layerCount=1;
    return p.result(view);
}
Result Metadata::createRenderPass(Handle handle,const VkRenderPassCreateInfo* info,VkResult driver) noexcept {
    auto& p=*impl_; if (driver!=VK_SUCCESS) return p.failed(driver);
    if (p.totals.globalIssues) return {{},p.totals.globalIssues};
    auto* rp=p.insert(p.renderPasses,p.config.renderPasses,handle); if (!rp) return p.result(rp);
    if (!info || info->sType!=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO) rp->issues |= bit(Issue::InvalidInput);
    else p.parseRenderPass(*rp,info);
    return p.result(rp);
}
Result Metadata::createRenderPass2(Handle handle,const VkRenderPassCreateInfo2* info,VkResult driver) noexcept {
    auto& p=*impl_; if (driver!=VK_SUCCESS) return p.failed(driver);
    if (p.totals.globalIssues) return {{},p.totals.globalIssues};
    auto* rp=p.insert(p.renderPasses,p.config.renderPasses,handle); if (!rp) return p.result(rp);
    if (!info || info->sType!=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2) rp->issues |= bit(Issue::InvalidInput);
    else p.parseRenderPass(*rp,info);
    return p.result(rp);
}
Result Metadata::createFramebuffer(Handle handle,const VkFramebufferCreateInfo* info,VkResult driver) noexcept {
    auto& p=*impl_; if (driver!=VK_SUCCESS) return p.failed(driver);
    if (p.totals.globalIssues) return {{},p.totals.globalIssues};
    auto* fb=p.insert(p.framebuffers,p.config.framebuffers,handle); if (!fb) return p.result(fb);
    if (!info || info->sType!=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO) { fb->issues |= bit(Issue::InvalidInput); return p.result(fb); }
    fb->facts.hasPNext=info->pNext!=nullptr;fb->facts.flags=info->flags;fb->facts.width=info->width;fb->facts.height=info->height;
    fb->facts.layers=info->layers;fb->facts.attachmentCount=info->attachmentCount;fb->facts.renderPass={bits(info->renderPass),0};
    if (info->pNext) fb->issues |= bit(Issue::UnsupportedExtension);
    if (info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) { fb->issues |= bit(Issue::Imageless); return p.result(fb); }
    if(info->attachmentCount<=maxAttachments&&info->pAttachments)
        for(std::size_t n=0;n<info->attachmentCount;++n)fb->facts.views[n]={bits(info->pAttachments[n]),0};
    if (info->flags || info->layers!=1 || !info->width || !info->height) fb->issues |= bit(Issue::IncompatibleFramebuffer);
    auto* rp=p.renderPass(bits(info->renderPass));
    if (!rp) { fb->issues |= bit(Issue::UnknownObject); return p.result(fb); }
    fb->renderPass=rp->key; fb->issues |= rp->issues; fb->width=info->width; fb->height=info->height;
    if (info->attachmentCount!=rp->count || info->attachmentCount>maxAttachments || !info->pAttachments) {
        fb->issues |= bit(Issue::IncompatibleFramebuffer); return p.result(fb);
    }
    fb->count=info->attachmentCount;
    fb->identityCopied=true;
    for (std::size_t n=0;n<fb->count;++n) {
        auto* v=p.view(bits(info->pAttachments[n]));
        if (!v) { fb->issues |= bit(Issue::UnknownObject); continue; }
        fb->views[n]=v->key; fb->issues |= v->issues;
        auto* image=p.image(v->image.handle);
        if (!image || image->key!=v->image) { fb->issues |= bit(Issue::StaleGeneration); continue; }
        const auto& a=rp->attachments[n]; const auto& desc=image->description;
        const VkImageUsageFlags required=a.role==Impl::Role::Color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT :
            a.role==Impl::Role::Depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        if (v->format!=a.format || v->range.aspectMask!=a.aspects || desc.extent.width<fb->width || desc.extent.height<fb->height
            || !(desc.usage & required)) fb->issues |= bit(Issue::IncompatibleFramebuffer);
        for (std::size_t j=0;j<n;++j) {
            const auto* previous=p.view(fb->views[j].handle);
            if (previous && previous->image==v->image) fb->issues |= bit(Issue::Aliasing);
        }
    }
    return p.result(fb);
}
void Metadata::destroyImage(Key key) noexcept { auto* item=impl_->image(key.handle); if (item && item->key==key) *item={}; }
void Metadata::destroyView(Handle h) noexcept { if (auto* item=impl_->view(h)) *item={}; }
void Metadata::destroyRenderPass(Handle h) noexcept { if (auto* item=impl_->renderPass(h)) *item={}; }
void Metadata::destroyFramebuffer(Handle h) noexcept { if (auto* item=impl_->framebuffer(h)) *item={}; }

Result Metadata::beginCommand(Key key,VkCommandBufferLevel level,const VkCommandBufferBeginInfo* info,VkResult driver) noexcept {
    auto& p=*impl_; auto* c=p.command(key.handle);
    if (driver!=VK_SUCCESS) { if (c) { c->state=Impl::State::Invalid; c->issues |= bit(Issue::DriverFailed); c->projection.issues |= bit(Issue::DriverFailed); } return p.failed(driver); }
    if (p.totals.globalIssues) return {key,p.totals.globalIssues};
    if (!key.handle || !key.generation || key.generation<=p.commandGeneration) {
        if (c) { c->state=Impl::State::Invalid; c->issues |= bit(Issue::StaleGeneration); c->projection.issues |= bit(Issue::StaleGeneration); }
        return {key,bit(Issue::StaleGeneration)};
    }
    const bool alreadyRecording=c && c->state==Impl::State::Recording;
    if (!c) c=p.insert(p.commands,p.config.commands,key.handle);
    if (!c) return p.result(c);
    std::destroy_at(c);std::construct_at(c);c->key=key; c->state=Impl::State::Recording; p.commandGeneration=key.generation;
    if (alreadyRecording) c->issues |= bit(Issue::Lifecycle);
    if (!info || info->sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO) c->issues |= bit(Issue::InvalidInput);
    else {
        c->flags=info->flags;
        if (info->pNext || info->pInheritanceInfo) c->issues |= bit(Issue::UnsupportedExtension);
        if (info->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) c->issues |= bit(Issue::SimultaneousUse);
        if (info->flags & ~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) c->issues |= bit(Issue::Secondary);
    }
    if (level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY) c->issues |= bit(Issue::Secondary);
    c->projection.selections=p.selections;c->projection.selectionIssues=p.selectionIssues;
    c->projection.issues=c->issues; // only freshly evaluated CB Begin facts
    return p.result(c);
}
Result Metadata::endCommand(Key key,VkResult driver) noexcept {
    auto& p=*impl_; auto* c=p.command(key.handle);
    if (!c || c->key!=key) return {key,bit(Issue::StaleGeneration)|p.totals.globalIssues};
    if (driver!=VK_SUCCESS) { c->state=Impl::State::Invalid; c->issues |= bit(Issue::DriverFailed); c->projection.issues |= bit(Issue::DriverFailed); return p.failed(driver); }
    if (c->state!=Impl::State::Recording) c->projection.issues |= bit(Issue::Lifecycle);
    if (c->projection.inPass) c->projection.issues |= bit(Issue::IncompleteRenderPass);
    if (c->state!=Impl::State::Recording) c->issues |= bit(Issue::Lifecycle);
    if (c->inPass) c->issues |= bit(Issue::IncompleteRenderPass);
    c->state=Impl::State::Executable; return p.result(c);
}
void Metadata::resetCommand(Handle h,VkResult driver) noexcept {
    auto& p=*impl_; if (driver!=VK_SUCCESS) { (void)p.failed(driver); return; }
    if (auto* c=p.command(h)) { const auto key=c->key;std::destroy_at(c);std::construct_at(c);c->key=key; }
}
void Metadata::freeCommand(Handle h) noexcept {if(auto* c=impl_->command(h)){std::destroy_at(c);std::construct_at(c);}}
Result Metadata::unsupported(Key key,Issue reason) noexcept {
    auto& p=*impl_; auto* c=p.command(key.handle);
    if (!c || c->key!=key) return {key,bit(Issue::StaleGeneration)|p.totals.globalIssues};
    c->issues |= bit(reason==Issue::None ? Issue::UnsupportedExtension : reason);
    c->projection.issues |= bit(reason==Issue::None ? Issue::UnsupportedExtension : reason); return p.result(c);
}
Result Metadata::beginRenderPass(Key key,std::uint64_t order,const VkRenderPassBeginInfo* info,VkSubpassContents contents) noexcept {
    auto& p=*impl_; Result status; auto* c=p.recording(key,order,status); if (!c) return status;
    p.beginProjection(*c,order,info,contents);
    if (c->inPass) c->issues |= bit(Issue::Lifecycle);
    c->inPass=true;
    if (contents!=VK_SUBPASS_CONTENTS_INLINE) c->issues |= bit(Issue::Secondary);
    if (!info || info->sType!=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO) { c->issues |= bit(Issue::InvalidInput); return p.result(c); }
    if (info->pNext) c->issues |= bit(Issue::UnsupportedExtension);
    auto* rp=p.renderPass(bits(info->renderPass)); auto* fb=p.framebuffer(bits(info->framebuffer));
    if (!rp || !fb) { c->issues |= bit(Issue::UnknownObject); return p.result(c); }
    c->issues |= rp->issues|fb->issues;
    if (fb->renderPass!=rp->key) c->issues |= bit(Issue::IncompatibleFramebuffer);
    if (info->renderArea.offset.x<0 || info->renderArea.offset.y<0 || !info->renderArea.extent.width || !info->renderArea.extent.height
        || std::uint64_t(info->renderArea.offset.x)+info->renderArea.extent.width>fb->width
        || std::uint64_t(info->renderArea.offset.y)+info->renderArea.extent.height>fb->height) c->issues |= bit(Issue::InvalidInput);
    if (info->clearValueCount>maxAttachments || (info->clearValueCount && !info->pClearValues)) c->issues |= bit(Issue::InvalidInput);
    if (c->count+rp->count*3>p.config.operationsPerCommand) c->issues |= bit(Issue::Capacity);
    if (c->issues || p.totals.globalIssues) return p.result(c);
    c->activeRenderPass=rp->key; c->activeFramebuffer=fb->key; c->activeBegin=c->count;
    // Validate all referenced generations before adding any operations.
    for (std::size_t i=0;i<rp->count;++i) {
        auto* view=p.view(fb->views[i].handle);
        if (!view || view->key!=fb->views[i]) { c->issues |= bit(Issue::StaleGeneration); continue; }
        LayoutOperation op; op.image=view->image; op.view=view->key; op.renderPass=rp->key; op.framebuffer=fb->key;
        c->issues |= p.liveOperation(op);
    }
    if (c->issues) return p.result(c);
    for (std::size_t i=0;i<rp->count;++i) {
        const auto* view=p.view(fb->views[i].handle); const auto& a=rp->attachments[i];
        LayoutOperation op; op.image=view->image; op.view=view->key; op.renderPass=rp->key; op.framebuffer=fb->key;
        op.command=key; op.range=view->range; op.oldLayout=op.newLayout=a.initial;
        op.recordOrder=order; op.attachment=std::uint32_t(i); op.sequenceWithinCall=std::uint32_t(i*2);
        op.sourceKind=SourceKind::InitialLayoutAssertion; c->operations[c->count++]=op;
        op.newLayout=a.reference; op.sourceKind=SourceKind::SubpassReferenceTransition; ++op.sequenceWithinCall;
        c->operations[c->count++]=op;
    }
    return p.result(c);
}
Result Metadata::beginRenderPass2(Key key,std::uint64_t order,const VkRenderPassBeginInfo* info,const VkSubpassBeginInfo* sub) noexcept {
    if (!sub || sub->sType!=VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO) (void)unsupported(key,Issue::InvalidInput);
    else if (sub->pNext) (void)unsupported(key,Issue::UnsupportedExtension);
    return beginRenderPass(key,order,info,sub ? sub->contents : VK_SUBPASS_CONTENTS_INLINE);
}
Result Metadata::endRenderPass(Key key,std::uint64_t order) noexcept {
    auto& p=*impl_; Result status; auto* c=p.recording(key,order,status); if (!c) return status;
    p.endProjection(*c,order);
    if (!c->inPass) c->issues |= bit(Issue::Lifecycle);
    c->inPass=false;
    auto* rp=p.renderPass(c->activeRenderPass.handle);
    if (!rp || rp->key!=c->activeRenderPass) c->issues |= bit(Issue::StaleGeneration);
    if (c->issues || p.totals.globalIssues) return p.result(c);
    for (std::size_t i=0;i<rp->count;++i) {
        auto op=c->operations[c->activeBegin+i*2+1]; c->issues |= p.liveOperation(op);
        op.oldLayout=op.newLayout; op.newLayout=rp->attachments[i].final;
        op.sourceKind=SourceKind::FinalLayoutTransition; op.recordOrder=order; op.sequenceWithinCall=std::uint32_t(i);
        c->operations[c->count++]=op;
    }
    return p.result(c);
}
Result Metadata::endRenderPass2(Key key,std::uint64_t order,const VkSubpassEndInfo* sub) noexcept {
    if (!sub || sub->sType!=VK_STRUCTURE_TYPE_SUBPASS_END_INFO) (void)unsupported(key,Issue::InvalidInput);
    else if (sub->pNext) (void)unsupported(key,Issue::UnsupportedExtension);
    return endRenderPass(key,order);
}
Result Metadata::nextSubpass(Key key,std::uint64_t order) noexcept {
    Result status; auto* c=impl_->recording(key,order,status); if (!c) return status;
    c->projection.issues |= bit(Issue::MultipleSubpasses);
    c->issues |= bit(Issue::MultipleSubpasses); if (!c->inPass) c->issues |= bit(Issue::Lifecycle); return impl_->result(c);
}
Result Metadata::dynamicRendering(Key key,std::uint64_t order) noexcept {
    Result status; auto* c=impl_->recording(key,order,status); if (!c) return status;
    c->projection.issues |= bit(Issue::DynamicRendering);
    c->issues |= bit(Issue::DynamicRendering); return impl_->result(c);
}
Batch Metadata::submitted(Key key,VkResult driver) noexcept {
    auto& p=*impl_; Batch batch;
    if (driver!=VK_SUCCESS) { batch.issues=p.failed(driver).issues; ++p.totals.refusedSubmissions; return batch; }
    batch.driverSubmissionSucceeded=true; ++p.totals.successfulSubmissions;
    auto* c=p.command(key.handle);
    if (!c || c->key!=key) batch.issues=bit(Issue::StaleGeneration);
    else {
        batch.issues=c->issues;
        if (c->state!=Impl::State::Executable || c->inPass) batch.issues |= bit(Issue::Lifecycle);
        if (c->submissions && (c->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)) batch.issues |= bit(Issue::OneTimeResubmitted);
        ++c->submissions;
        for (std::size_t i=0;i<c->count;++i) batch.issues |= p.liveOperation(c->operations[i]);
    }
    batch.issues |= p.totals.globalIssues;
    if (batch.issues) { ++p.totals.refusedSubmissions; return batch; }
    batch.count=c->count; std::copy_n(c->operations.begin(),c->count,batch.operations.begin());
    batch.metadataAccepted=true; return batch;
}
Result Metadata::setProjectionSelection(std::size_t slot,const ProjectionSelection& selection) noexcept {
    auto& p=*impl_;
    if (slot>=projectionSlots) return {selection.image,bit(Issue::InvalidInput)};
    std::uint64_t issues=p.totals.globalIssues;
    if (!selection.selectionSerial || selection.selectionSerial<=p.selectionGeneration) issues |= bit(Issue::StaleGeneration);
    if (selection.enabled) {
        auto* image=p.image(selection.image.handle);
        if (!image) issues |= bit(Issue::UnknownObject);
        else if (image->key!=selection.image) issues |= bit(Issue::StaleGeneration);
        else {
            auto normalized=selection.range;
            if (!normalizedRange(image->description,normalized) || !sameRange(normalized,selection.range)) issues |= bit(Issue::UnsupportedView);
        }
    }
    if (issues) {
        p.selectionIssues[slot] |= issues;
        // A rejected change is also observable: do not rehabilitate recordings
        // from before that uncertainty by merely correcting a later selection.
        for (std::size_t n=0;n<std::min(p.config.commands,p.commands.extent());++n) if (p.commands.get(n)&&p.commands[n].key.handle)
            p.commands[n].projection.selectionIssues[slot] |= issues;
        return {selection.image,issues};
    }
    p.selectionGeneration=selection.selectionSerial;p.selections[slot]=selection;p.selectionIssues[slot]=0;
    return {selection.image,0};
}
ProjectionTicket Metadata::captureProjectionSubmission(Key key) noexcept {
    auto& p=*impl_;ProjectionTicket ticket;ticket.command=key;ticket.issues=p.totals.globalIssues;
    auto* c=p.command(key.handle);
    if (!c || c->key!=key) { ticket.issues |= bit(Issue::StaleGeneration);return ticket; }
    auto& recording=c->projection;ticket.frozenSelections=recording.selections;
    if (c->state!=Impl::State::Executable || recording.inPass || recording.pendingTicket) recording.issues |= bit(Issue::Lifecycle);
    if (p.ticketGeneration==UINT64_MAX) recording.issues |= bit(Issue::Capacity);
    else { ticket.serial=++p.ticketGeneration;recording.pendingTicket=ticket.serial; }
    ticket.issues |= recording.issues;
    return ticket;
}
ProjectionSubmission Metadata::projectSubmitted(const ProjectionTicket& ticket,VkResult driver) noexcept {
    auto& p=*impl_;ProjectionSubmission out;out.issues=ticket.issues|p.totals.globalIssues;
    auto* c=p.command(ticket.command.handle);
    const bool matches=c && c->key==ticket.command;
    const bool pending=matches && ticket.serial && c->projection.pendingTicket==ticket.serial;
    if (!matches) out.issues |= bit(Issue::StaleGeneration);
    else {
        auto& recording=c->projection;
        out.observedPasses=recording.observedPasses;out.retainedPasses=recording.count;out.droppedPasses=recording.droppedPasses;
        if (!pending) recording.issues |= bit(Issue::Lifecycle);
        else recording.pendingTicket=0;
        out.issues |= recording.issues;
        if (c->state!=Impl::State::Executable || recording.inPass) out.issues |= bit(Issue::Lifecycle);
        if (pending && driver==VK_SUCCESS) {
            if (recording.submissions && (c->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)) out.issues |= bit(Issue::OneTimeResubmitted);
            if (recording.submissions==UINT64_MAX) recording.issues |= bit(Issue::Capacity);
            else ++recording.submissions;
            out.issues |= recording.issues;
        }
        for (std::size_t n=0;n<projectionSlots;++n)
            if (!sameSelection(ticket.frozenSelections[n],recording.selections[n])) out.issues |= bit(Issue::StaleGeneration);
        // Revalidate ALL identities before any slot can exclude a pass.
        for (std::size_t n=0;n<recording.count;++n) out.issues |= p.liveProjectionPass(recording.passes[n]);
    }
    if (driver!=VK_SUCCESS) out.issues |= p.failed(driver).issues;
    else out.driverSubmissionSucceeded=true;
    for (std::size_t slot=0;slot<projectionSlots;++slot) {
        auto& batch=out.slots[slot];batch.selection=ticket.frozenSelections[slot];
        batch.observedPasses=out.observedPasses;batch.retainedPasses=out.retainedPasses;batch.droppedPasses=out.droppedPasses;
        if (!batch.selection.enabled && !p.selections[slot].enabled) continue;
        batch.classification=ProjectionClassification::Refused;batch.issues=out.issues|p.selectionIssues[slot];
        if (!sameSelection(batch.selection,p.selections[slot])) batch.issues |= bit(Issue::StaleGeneration);
        if (matches) batch.issues |= c->projection.selectionIssues[slot];
        auto* image=p.image(batch.selection.image.handle);
        if (!image) batch.issues |= bit(Issue::UnknownObject);
        else if (image->key!=batch.selection.image) batch.issues |= bit(Issue::StaleGeneration);
        if (batch.issues || !matches) continue;
        const auto& recording=c->projection;std::size_t required=0;
        for (std::size_t n=0;n<recording.count;++n) {
            const auto& pass=recording.passes[n];bool direct=false;++batch.examinedPasses;
            const auto* rp=p.renderPass(pass.renderPass.handle);
            for (std::size_t a=0;a<pass.count;++a) if (pass.attachments[a].image==batch.selection.image) {
                direct=true;batch.issues |= p.candidateAttachment(batch.selection,pass,a);
                required += rp->attachments[a].used ? 3 : 2;
            }
            if (direct) ++batch.directPasses;else ++batch.noDirectPasses;
        }
        if (required>p.config.operationsPerCommand || required>maxOperations) batch.issues |= bit(Issue::Capacity);
        if (batch.issues) continue;
        for (std::size_t n=0;n<recording.count;++n) {
            const auto& pass=recording.passes[n];const auto* rp=p.renderPass(pass.renderPass.handle);
            for (std::size_t a=0;a<pass.count;++a) if (pass.attachments[a].image==batch.selection.image) {
                const auto& identity=pass.attachments[a];const auto& attachment=rp->attachments[a];LayoutOperation op;
                op.command=ticket.command;op.image=identity.image;op.view=identity.view;op.renderPass=pass.renderPass;op.framebuffer=pass.framebuffer;
                op.range=identity.range;op.attachment=static_cast<std::uint32_t>(a);op.recordOrder=pass.beginOrder;
                op.sequenceWithinCall=static_cast<std::uint32_t>(a*2);op.oldLayout=op.newLayout=attachment.initial;
                op.sourceKind=SourceKind::InitialLayoutAssertion;batch.operations[batch.count++]=op;
                if (attachment.used) {
                    op.newLayout=attachment.reference;op.sourceKind=SourceKind::SubpassReferenceTransition;++op.sequenceWithinCall;
                    batch.operations[batch.count++]=op;op.oldLayout=attachment.reference;op.sourceKind=SourceKind::FinalLayoutTransition;
                } else op.sourceKind=SourceKind::UnusedInitialToFinalTransition;
                op.newLayout=attachment.final;op.recordOrder=pass.endOrder;op.sequenceWithinCall=static_cast<std::uint32_t>(a);
                batch.operations[batch.count++]=op;
            }
        }
        batch.metadataAccepted=true;
        batch.classification=batch.directPasses ? ProjectionClassification::DirectAttachmentLayouts : ProjectionClassification::NoDirectAttachmentReference;
    }
    return out;
}
std::size_t Metadata::explicitJournalStorageBytesPerCommand() noexcept {return sizeof(Impl::RecordedPrefix);}
void Metadata::recordingPoint(Key key,std::uint64_t order) noexcept {
    if(auto* c=impl_->command(key.handle);c&&c->key==key)impl_->prefixPoint(*c,order);
}
void Metadata::unknownExplicitBarrier(Key key,std::uint64_t order) noexcept {
    if(auto* c=impl_->command(key.handle);c&&c->key==key){impl_->prefixPoint(*c,order);c->prefix.issues|=xess_vk::bit(LI::OpaqueControl);}
}
void Metadata::explicitImageBarriers(Key key,std::uint64_t order,VkPipelineStageFlags src,VkPipelineStageFlags dst,
    VkDependencyFlags flags,std::uint32_t count,const VkImageMemoryBarrier* entries,const image_inventory::Snapshot* identities) noexcept {
    auto* out=impl_->explicitCall(key,order,count,entries);if(!out)return;
    if(flags)out->issues|=xess_vk::bit(LI::UnsupportedDependency);
    for(std::uint32_t n=0;n<count;++n){auto event=impl_->imageBarrier(entries[n],order,n,RecordedLayoutEventKind::LegacyBarrier,src,dst,identities?&identities[n]:nullptr);
        out->issues|=event.issues&(xess_vk::bit(LI::UnknownObject)|xess_vk::bit(LI::UnsupportedPNext)|xess_vk::bit(LI::OpaqueControl)|xess_vk::bit(LI::UnsupportedBarrierScope));
        out->images[(out->firstImage+out->count++)%impl_->config.explicitImageEventsPerCommand]=event;
    }
}
void Metadata::explicitImageBarriers2(Key key,std::uint64_t order,const VkDependencyInfo* info,const image_inventory::Snapshot* identities) noexcept {
    if(!info){unknownExplicitBarrier(key,order);return;}
    auto* out=impl_->explicitCall(key,order,info->imageMemoryBarrierCount,info->pImageMemoryBarriers);if(!out)return;
    if(info->sType!=VK_STRUCTURE_TYPE_DEPENDENCY_INFO)out->issues|=xess_vk::bit(LI::OpaqueControl);
    if(info->pNext)out->issues|=xess_vk::bit(LI::UnsupportedPNext);
    if(info->dependencyFlags)out->issues|=xess_vk::bit(LI::UnsupportedDependency);
    for(std::uint32_t n=0;n<info->imageMemoryBarrierCount;++n){const auto& b=info->pImageMemoryBarriers[n];
        auto event=impl_->imageBarrier(b,order,n,RecordedLayoutEventKind::Sync2Barrier,b.srcStageMask,b.dstStageMask,identities?&identities[n]:nullptr);
        out->issues|=event.issues&(xess_vk::bit(LI::UnknownObject)|xess_vk::bit(LI::UnsupportedPNext)|xess_vk::bit(LI::OpaqueControl)|xess_vk::bit(LI::UnsupportedBarrierScope));
        out->images[(out->firstImage+out->count++)%impl_->config.explicitImageEventsPerCommand]=event;
    }
}
void Metadata::imageTouched(Key key,std::uint64_t order,Handle image,RecordedImageTouch kind) noexcept {
    auto* c=impl_->command(key.handle);if(!c||c->key!=key)return;
    impl_->prefixPoint(*c,order);auto& p=c->prefix;
    if(!image){p.issues|=xess_vk::bit(LI::OpaqueControl);return;}
    if(p.touchCount==p.touches.size()){
        p.lastTouchOverflowOrder=std::max(p.lastTouchOverflowOrder,p.touches[p.firstTouch].order);
        p.firstTouch=(p.firstTouch+1)%p.touches.size();--p.touchCount;p.issues|=xess_vk::bit(LI::JournalOverflow);
    }
    p.touches[(p.firstTouch+p.touchCount++)%p.touches.size()]={order,image,kind};
}
RecordedImageInterval Metadata::imageInterval(Key key,std::uint64_t afterRead,std::uint64_t beforeUse,Key wanted) const noexcept {
    RecordedImageInterval out;out.queried=true;out.issues=0;
    auto& p=*impl_;const auto* c=p.command(key.handle);
    if(!c||c->key!=key){out.issues=xess_vk::bit(LI::IdentityMismatch);return out;}
    if(!afterRead||afterRead>=beforeUse||beforeUse!=c->prefix.currentOrder){out.issues=xess_vk::bit(LI::PrefixMismatch);return out;}
    if(c->state!=Impl::State::Recording||c->projection.inPass)out.issues|=xess_vk::bit(LI::RecordingUnsupported);
    constexpr auto unknownControl=bit(Issue::InvalidInput)|bit(Issue::Lifecycle)|bit(Issue::UnsupportedExtension)|
        bit(Issue::Secondary)|bit(Issue::DynamicRendering)|bit(Issue::IncompleteRenderPass)|bit(Issue::Stopped)|bit(Issue::DeviceLost);
    if(c->issues&unknownControl)out.issues|=xess_vk::bit(LI::OpaqueControl);
    auto prefixIssues=c->prefix.issues;
    out.lastExplicitOverflowOrder=c->prefix.lastExplicitOverflowOrder;out.lastTouchOverflowOrder=c->prefix.lastTouchOverflowOrder;
    const auto lastOverflow=std::max(out.lastExplicitOverflowOrder,out.lastTouchOverflowOrder);
    // Only loss after the supplied read point can hide an interval mutation.
    // Full-prefix layout reconstruction still rejects any journal overflow.
    if((prefixIssues&xess_vk::bit(LI::JournalOverflow))&&lastOverflow&&lastOverflow<=afterRead){
        prefixIssues&=~xess_vk::bit(LI::JournalOverflow);out.precedingOverflowIgnored=true;
    }
    out.issues|=prefixIssues;
    if(p.totals.globalIssues||c->projection.droppedPasses)out.issues|=xess_vk::bit(LI::OpaqueControl);
    const auto* image=p.image(wanted.handle);
    if(!image||image->key!=wanted)out.issues|=xess_vk::bit(LI::StaleObject);
    for(std::size_t n=0;n<c->prefix.count;++n){const auto& e=c->prefix.imageAt(n,p.config.explicitImageEventsPerCommand);
        if(e.event.order>afterRead&&e.event.order<beforeUse&&e.image.handle==wanted.handle){++out.barriers;out.issues|=e.issues;}}
    for(std::size_t n=0;n<c->projection.count;++n){const auto& pass=c->projection.passes[n];
        if(pass.beginOrder>=beforeUse||(pass.endOrder&&pass.endOrder<=afterRead))continue;
        if(!pass.endOrder||p.liveProjectionPass(pass))out.issues|=xess_vk::bit(LI::PassIdentityUnknown);
        for(std::size_t a=0;a<pass.count;++a)if(pass.attachments[a].image.handle==wanted.handle)++out.attachments;
    }
    for(std::size_t n=0;n<c->prefix.touchCount;++n){const auto& e=c->prefix.touches[(c->prefix.firstTouch+n)%c->prefix.touches.size()];
        if(e.order>afterRead&&e.order<beforeUse&&e.image==wanted.handle){
            if(e.kind==RecordedImageTouch::Clear)++out.clears;else ++out.transfers;}}
    out.untouched=!out.issues&&!out.barriers&&!out.attachments&&!out.transfers&&!out.clears;return out;
}
bool Metadata::prefixImageKeys(Key key,std::uint64_t point,std::array<RecordedImageKey,maxExplicitImageEvents>& keys,std::size_t& count) const noexcept {
    const auto* c=impl_->command(key.handle);count=0;if(!c||c->key!=key||!point||c->prefix.currentOrder!=point)return false;
    count=c->prefix.count;for(std::size_t n=0;n<count;++n){const auto& e=c->prefix.imageAt(n,impl_->config.explicitImageEventsPerCommand);keys[n]={e.image.handle,e.image.generation};}return true;
}
RecordedSourceLayout Metadata::resolveSourceAt(Key key,std::uint64_t point,RecordedImageKey wanted,
    const VkImageSubresourceRange& copiedRange,VkImageLayout declared,const RecordedPrefixLiveness* liveness) noexcept {
    auto& p=*impl_;RecordedSourceLayout out;out.queried=true;out.issues=0;out.command=key.handle;out.rpRecordingGeneration=key.generation;
    out.prefixOrder=point;out.image=wanted;out.copiedAspect=copiedRange.aspectMask;out.declaredLayout=declared;
    const auto fail=[&](LI issue){out.issues|=xess_vk::bit(issue);};
    auto* c=p.command(key.handle);
    if(!c||c->key!=key){fail(LI::IdentityMismatch);return out;}
    const auto& prefix=c->prefix;out.observedExplicitImages=prefix.observedImages;out.retainedExplicitImages=prefix.count;out.droppedExplicitImages=prefix.droppedImages;
    out.explicitImageHighWater=prefix.count;out.explicitImageCapacity=p.config.explicitImageEventsPerCommand;out.explicitCallRefusals=prefix.callRefusals;
    out.issues|=prefix.issues;
    if(!point||point!=prefix.currentOrder)fail(LI::PrefixMismatch);
    if(c->state!=Impl::State::Recording||c->projection.inPass||c->projection.issues)fail(LI::RecordingUnsupported);
    if(c->projection.droppedPasses)fail(LI::PassIdentityUnknown);
    if(p.totals.globalIssues)fail(LI::OpaqueControl);
    if(liveness){
        if(liveness->count!=prefix.count)fail(LI::PrefixMismatch);
        if(liveness->source.status!=image_inventory::Status::LiveHandleObserved){fail(LI::UnknownObject);return out;}
        if(liveness->source.record.image!=wanted.handle||liveness->source.record.generation!=wanted.generation){fail(LI::StaleObject);return out;}
    }
    auto* image=p.image(wanted.handle);
    if(!image){fail(LI::UnknownObject);return out;}
    if(image->key!=Key{wanted.handle,wanted.generation}){fail(LI::StaleObject);return out;}
    const auto& description=image->description;
    if(!sourceShape(description)){fail(LI::UnsupportedShape);return out;}
    auto copied=copiedRange;const auto allAspects=sourceAspects(description.format);
    if(!normalizedRange(description,copied)||!copied.aspectMask||(copied.aspectMask&~allAspects)
        ||copied.baseMipLevel||copied.baseArrayLayer||copied.levelCount!=1||copied.layerCount!=1){fail(LI::RangeAmbiguous);return out;}
    out.resolvedLayoutRange={allAspects,0,1,0,1};
    // Recheck every pass identity in the prefix BEFORE excluding unrelated
    // attachments. Candidate-local unsupported attachments stay isolated.
    for(std::size_t n=0;n<c->projection.count;++n){const auto& pass=c->projection.passes[n];
        if(pass.beginOrder>=point)continue;
        ++out.examinedPasses;
        if(pass.endOrder>=point||p.liveProjectionPass(pass)){fail(LI::PassIdentityUnknown);continue;}
        bool direct=false;for(std::size_t a=0;a<pass.count;++a){const auto& identity=pass.attachments[a];
            if(identity.image!=Key{wanted.handle,wanted.generation})continue;
            if(!overlaps(identity.range,out.resolvedLayoutRange))continue;
            direct=true;
            ProjectionSelection selection;selection.image=identity.image;selection.range=out.resolvedLayoutRange;
            if(p.candidateAttachment(selection,pass,a))fail(LI::PassUnsupported);
        }
        if(direct)++out.directPasses;else ++out.noDirectPasses;
    }
    for(std::size_t n=0;n<prefix.count;++n){const auto& e=prefix.imageAt(n,p.config.explicitImageEventsPerCommand);if(e.event.order>=point)continue;
        // All retained image keys are rechecked before candidate exclusion.
        if(liveness){
            if(n>=liveness->count||liveness->images[n].status!=image_inventory::Status::LiveHandleObserved){fail(LI::UnknownObject);continue;}
            const auto& live=liveness->images[n].record;
            if(live.image!=e.image.handle||live.generation!=e.image.generation){fail(LI::StaleObject);continue;}
        }else{
            const auto* live=p.image(e.image.handle);if(!live){fail(LI::UnknownObject);continue;}
            if(live->key!=e.image){fail(LI::StaleObject);continue;}
        }
        if(e.image!=Key{wanted.handle,wanted.generation})continue;
        if(!overlaps(e.range,out.resolvedLayoutRange)&&!(e.issues&xess_vk::bit(LI::RangeAmbiguous)))continue;
        out.issues|=e.issues;
        // Equal sync2 fields are ignored, not a partial range layout change.
        if(e.event.kind==RecordedLayoutEventKind::Sync2Barrier&&e.event.oldLayout==e.event.newLayout)continue;
        if(!sameRange(e.range,out.resolvedLayoutRange)){
            fail(LI::RangeAmbiguous);if(allAspects==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT))fail(LI::CombinedAspectRequired);
        }
    }
    if(out.issues)return out;
    // Cursor merge: no flattening allocation and no dependency on the legacy
    // 96-operation batch. Pass Begin initial/reference and End final events
    // retain their original per-call sequence.
    std::size_t explicitCursor=0,passCursor=0,passEventCursor=0;
    const auto nextExplicit=[&](RecordedLayoutEvent& event) noexcept {
        while(explicitCursor<prefix.count){const auto& e=prefix.imageAt(explicitCursor++,p.config.explicitImageEventsPerCommand);
            if(e.event.order<point&&e.image==Key{wanted.handle,wanted.generation}&&overlaps(e.range,out.resolvedLayoutRange)){event=e.event;return true;}}
        return false;
    };
    const auto nextPass=[&](RecordedLayoutEvent& event) noexcept {
        while(passCursor<c->projection.count){const auto& pass=c->projection.passes[passCursor];
            if(pass.beginOrder>=point||pass.endOrder>=point){++passCursor;passEventCursor=0;continue;}
            const auto* rp=p.renderPass(pass.renderPass.handle);
            while(passEventCursor<pass.count*3){const auto index=passEventCursor++;const bool end=index>=pass.count*2;
                const auto a=end?index-pass.count*2:index/2;const auto& identity=pass.attachments[a];const auto& attachment=rp->attachments[a];
                if(identity.image!=Key{wanted.handle,wanted.generation}||!overlaps(identity.range,out.resolvedLayoutRange))continue;
                event={};event.order=end?pass.endOrder:pass.beginOrder;event.sequence=static_cast<std::uint32_t>(end?a:index);
                if(end){event.kind=RecordedLayoutEventKind::RenderPassFinal;event.oldLayout=attachment.used?attachment.reference:attachment.initial;event.newLayout=attachment.final;}
                else if(index%2){if(!attachment.used)continue;event.kind=RecordedLayoutEventKind::RenderPassReference;event.oldLayout=attachment.initial;event.newLayout=attachment.reference;}
                else{event.kind=RecordedLayoutEventKind::RenderPassInitialAssertion;event.oldLayout=event.newLayout=attachment.initial;}
                return true;
            }
            ++passCursor;passEventCursor=0;
        }
        return false;
    };
    RecordedLayoutEvent explicitEvent,passEvent;bool haveExplicit=nextExplicit(explicitEvent),havePass=nextPass(passEvent),known=false;
    VkImageLayout current=VK_IMAGE_LAYOUT_UNDEFINED;
    const auto consume=[&](const RecordedLayoutEvent& event) noexcept {
        ++out.matchedEvents;
        if(event.kind==RecordedLayoutEventKind::Sync2Barrier&&event.oldLayout==event.newLayout){++out.ignoredEqualSync2Events;return;}
        const bool assertion=event.kind==RecordedLayoutEventKind::RenderPassInitialAssertion;
        if(!sourceLayoutAllowed(event.oldLayout,allAspects,true)||(!assertion&&!sourceLayoutAllowed(event.newLayout,allAspects,false))){fail(LI::UnsupportedLayout);return;}
        if(event.oldLayout!=VK_IMAGE_LAYOUT_UNDEFINED&&known&&current!=event.oldLayout){fail(LI::ChainMismatch);return;}
        if(assertion){if(known)out.lastEvent=event;return;}
        if(event.oldLayout==event.newLayout){if(known)out.lastEvent=event;return;} // never an anchor
        known=true;current=event.newLayout;out.anchor=out.lastEvent=event;
    };
    while(haveExplicit||havePass){
        const bool takeExplicit=haveExplicit&&(!havePass||explicitEvent.order<passEvent.order
            ||(explicitEvent.order==passEvent.order&&explicitEvent.sequence<passEvent.sequence));
        if(takeExplicit){consume(explicitEvent);haveExplicit=nextExplicit(explicitEvent);}
        else{consume(passEvent);havePass=nextPass(passEvent);}
    }
    if(!known)fail(LI::NoTransitionAnchor);
    // Descriptor layout is compared only after an independently resolved chain.
    if(known&&current!=declared)fail(LI::DeclaredLayoutMismatch);
    if(!out.issues){out.recordedLayout=current;out.recordedLayoutResolved=out.recordedPrefixSupported=true;}
    return out;
}
Stats Metadata::stats() const noexcept {auto s=impl_->totals;s.storageBytes=storageBytes();return s;}
ImageSnapshot Metadata::imageSnapshot(Handle handle) const noexcept {
    if(const auto* item=impl_->image(handle))return {true,item->description,item->issues};return {};
}
ViewSnapshot Metadata::viewSnapshot(Handle handle) const noexcept {
    ViewSnapshot out;if(const auto* item=impl_->view(handle)){out=item->facts;out.known=true;out.key=item->key;out.issues=item->issues;if(item->image.handle)out.image=item->image;}return out;
}
RenderPassSnapshot Metadata::renderPassSnapshot(Handle handle) const noexcept {
    RenderPassSnapshot out;if(const auto* item=impl_->renderPass(handle)){out=item->facts;out.known=true;out.key=item->key;out.issues=item->issues;}return out;
}
FramebufferSnapshot Metadata::framebufferSnapshot(Handle handle) const noexcept {
    FramebufferSnapshot out;if(const auto* item=impl_->framebuffer(handle)){out=item->facts;out.known=true;out.key=item->key;out.issues=item->issues;
        if(item->renderPass.handle)out.renderPass=item->renderPass;for(std::size_t i=0;i<maxAttachments;++i)if(item->views[i].handle)out.views[i]=item->views[i];}return out;
}
CommandSnapshot Metadata::commandSnapshot(Key key) const noexcept {
    if(const auto* item=impl_->command(key.handle);item&&item->key==key) {
        return {true,item->inPass,item->key,item->activeRenderPass,item->activeFramebuffer,item->issues,item->lastOrder,item->count};
    }
    return {};
}
void Metadata::stop() noexcept { impl_->totals.globalIssues |= bit(Issue::Stopped); }
} // namespace xess_vk::render_pass_metadata
