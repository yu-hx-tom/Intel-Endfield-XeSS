#pragma once
#include "render_pass_metadata.hpp"
#include <array>
#include <cstdint>
#include <limits>

namespace xess_vk::render_pass_diagnostics {
namespace rp=render_pass_metadata;
enum class Site : unsigned { Image,View,RenderPass,RenderPass2,Framebuffer,BeginCommand,EndCommand,
    BeginRenderPass,BeginRenderPass2,EndRenderPass,EndRenderPass2,Unsupported,NextSubpass,DynamicRendering,SelectedImage,IgnoredView,Count };
inline constexpr std::size_t siteCount=static_cast<std::size_t>(Site::Count), samplesPerSite=8, sampleLimit=siteCount*samplesPerSite;
inline const char* siteName(Site site) noexcept {
    constexpr const char* names[]{"image","view","render_pass","render_pass2","framebuffer","begin_command","end_command",
        "begin_render_pass","begin_render_pass2","end_render_pass","end_render_pass2","unsupported","next_subpass","dynamic_rendering","selected_image","ignored_view"};
    const auto index=static_cast<std::size_t>(site);return index<siteCount?names[index]:"invalid";
}
struct Sample {
    Site site{}; std::uint64_t issues{},incomingIssues{},recordOrder{};std::uint32_t attachment{},roleMask{};
    VkResult driver{VK_SUCCESS}; bool beginHasPNext{},ignoredViewCreationKnown{}; VkSubpassContents contents{};VkRect2D renderArea{};std::uint32_t clearValueCount{};
    rp::CommandSnapshot command;
    rp::ImageSnapshot image;
    rp::ViewSnapshot view;
    rp::RenderPassSnapshot pass;
    rp::FramebufferSnapshot framebuffer;
};
// Exact semantic key, excluding handles/generations/order: repeated object
// instances share a frequency bucket, but its first immutable sample is retained.
struct Signature {std::array<std::uint64_t,192> words{};friend bool operator==(const Signature&,const Signature&)=default;};
inline Signature signature(const Sample& s) noexcept {
    Signature result;std::size_t index=0;const auto add=[&](std::uint64_t value){if(index<result.words.size())result.words[index++]=value;};
    add(static_cast<unsigned>(s.site));add(s.issues);add(s.incomingIssues);add(s.attachment);add(s.roleMask);add(std::uint32_t(s.driver));
    add(s.beginHasPNext);add(s.ignoredViewCreationKnown);add(s.contents);add(s.renderArea.offset.x<0);add(s.renderArea.offset.y<0);
    add(s.renderArea.extent.width!=0);add(s.renderArea.extent.height!=0);add(s.clearValueCount);
    add(s.command.known);add(s.command.inPass);add(s.command.activeRenderPass.handle!=0);add(s.command.operationCount);
    const auto& image=s.image.description;add(s.image.known);add(s.image.issues);add(image.type);add(image.format);
    add(image.extent.width!=0);add(image.extent.height!=0);add(image.extent.depth);add(image.mipLevels);add(image.arrayLayers);
    add(image.samples);add(image.usage);add(image.flags);add(image.tiling);add(image.sharing);add(image.unsupportedPNext);
    const auto& view=s.view;add(view.known);add(view.hasPNext);add(view.issues);add(view.type);add(view.format);add(view.flags);
    add(view.components.r);add(view.components.g);add(view.components.b);add(view.components.a);
    add(view.rawRange.aspectMask);add(view.rawRange.baseMipLevel);add(view.rawRange.levelCount);add(view.rawRange.baseArrayLayer);add(view.rawRange.layerCount);
    add(view.image==image.key);add(s.framebuffer.known);add(s.framebuffer.issues);add(s.framebuffer.flags);add(s.framebuffer.hasPNext);
    add(s.framebuffer.width!=0);add(s.framebuffer.height!=0);add(s.framebuffer.layers);add(s.framebuffer.attachmentCount);
    add(image.extent.width<s.framebuffer.width);add(image.extent.height<s.framebuffer.height);
    add(s.framebuffer.renderPass==s.pass.key);add(s.pass.known);add(s.pass.issues);add(s.pass.hasPNext);add(s.pass.flags);
    add(s.pass.attachmentCount);add(s.pass.subpassCount);add(s.pass.dependencyCount);add(s.pass.copiedAttachmentCount);
    for(const auto& a:s.pass.attachments){add(a.format);add(a.samples);add(a.flags);add(a.loadOp);add(a.storeOp);add(a.stencilLoadOp);add(a.stencilStoreOp);
        add(a.initial);add(a.reference);add(a.final);add(a.aspects);add(a.role);add(a.referenceCount);add(a.hasPNext);}
    // 63 scalar words + 8 * 14 attachment words fit in 192; no pointer or byte padding enters the key.
    return result;
}
struct Bucket {bool occupied{},emitted{};std::uint64_t occurrences{};Signature key;Sample first;};
struct Totals {
    std::uint64_t observations{},storedSamples{},omittedSamples{},emittedSamples{};
    std::array<std::uint64_t,siteCount> bySite{},omittedBySite{};
};
inline void increment(std::uint64_t& value) noexcept {if(value!=std::numeric_limits<std::uint64_t>::max())++value;}
class Store {
public:
    std::array<Bucket,sampleLimit> buckets{};Totals totals;
    void observe(const Sample& sample) noexcept {
        const auto site=static_cast<std::size_t>(sample.site);if(site>=siteCount)return;
        increment(totals.observations);increment(totals.bySite[site]);const auto key=signature(sample);
        Bucket* vacant=nullptr;
        for(std::size_t i=site*samplesPerSite;i<(site+1)*samplesPerSite;++i){auto& bucket=buckets[i];
            if(bucket.occupied&&bucket.key==key){increment(bucket.occurrences);return;}
            if(!bucket.occupied&&!vacant)vacant=&bucket;
        }
        if(!vacant){increment(totals.omittedSamples);increment(totals.omittedBySite[site]);return;}
        vacant->occupied=true;vacant->occurrences=1;vacant->key=key;vacant->first=sample;increment(totals.storedSamples);
    }
    bool take(std::size_t index,Sample& sample,std::uint64_t& occurrences) noexcept {
        if(index>=buckets.size())return false;auto& b=buckets[index];if(!b.occupied||b.emitted)return false;
        sample=b.first;occurrences=b.occurrences;b.emitted=true;increment(totals.emittedSamples);return true;
    }
};
} // namespace xess_vk::render_pass_diagnostics
