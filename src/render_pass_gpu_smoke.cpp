// Self-owned offscreen GPU clear passes. No game, WSI takeover, or XeFG calls.
#include "native_source_fixture.hpp"
#include <iostream>
#include <memory>

namespace {
using xess_vk::testing::check;
struct Fixture {
    xess_vk::testing::SourceOwner source;
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkRenderPass pass{};
    VkFramebuffer framebuffer{};
    bool cleanupSucceeded{};

    void initialize() {
        source.initialize({64, 64, 5000});
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D; info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {64,64,1}; info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(source.dev<PFN_vkCreateImage>("vkCreateImage")(source.device, &info, nullptr, &image), "create attachment");
        VkMemoryRequirements required{};
        source.dev<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(source.device, image, &required);
        uint32_t type = UINT32_MAX;
        for (uint32_t i=0; i<source.memory.memoryTypeCount; ++i)
            if ((required.memoryTypeBits & (1u<<i)) && (source.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type=i; break; }
        if (type == UINT32_MAX) throw std::runtime_error("No local attachment memory");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = required.size; allocation.memoryTypeIndex = type;
        check(source.dev<PFN_vkAllocateMemory>("vkAllocateMemory")(source.device, &allocation, nullptr, &memory), "allocate attachment");
        check(source.dev<PFN_vkBindImageMemory>("vkBindImageMemory")(source.device, image, memory, 0), "bind attachment");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; viewInfo.format = info.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        check(source.dev<PFN_vkCreateImageView>("vkCreateImageView")(source.device, &viewInfo, nullptr, &view), "create view");
        VkAttachmentDescription attachment{};
        attachment.format = info.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &attachment; rp.subpassCount = 1; rp.pSubpasses = &subpass;
        check(source.dev<PFN_vkCreateRenderPass>("vkCreateRenderPass")(source.device, &rp, nullptr, &pass), "create pass");
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass=pass; fb.attachmentCount=1; fb.pAttachments=&view; fb.width=fb.height=64; fb.layers=1;
        check(source.dev<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(source.device, &fb, nullptr, &framebuffer), "create framebuffer");
    }
    void run(unsigned frames) {
        for (unsigned i=0; i<frames; ++i) {
            source.wait();
            check(source.dev<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(source.command,0), "reset command");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(source.dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(source.command,&begin), "begin command");
            VkClearValue clear{}; clear.color.float32[0]=static_cast<float>(i%2); clear.color.float32[3]=1;
            VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rp.renderPass=pass; rp.framebuffer=framebuffer; rp.renderArea.extent={64,64}; rp.clearValueCount=1; rp.pClearValues=&clear;
            source.dev<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(source.command,&rp,VK_SUBPASS_CONTENTS_INLINE);
            source.dev<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass")(source.command);
            check(source.dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(source.command), "end command");
            check(source.dev<PFN_vkResetFences>("vkResetFences")(source.device,1,&source.completion), "reset fence");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&source.command;
            check(source.dev<PFN_vkQueueSubmit>("vkQueueSubmit")(source.queue,1,&submit,source.completion), "submit clear pass");
        }
        source.wait();
    }
    bool cleanup() noexcept {
        if (cleanupSucceeded) return true;
        try {
            if (!source.device || !source.drainQueue(5000)) return false;
            if (framebuffer) source.dev<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(source.device,framebuffer,nullptr);
            if (pass) source.dev<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(source.device,pass,nullptr);
            if (view) source.dev<PFN_vkDestroyImageView>("vkDestroyImageView")(source.device,view,nullptr);
            if (image) source.dev<PFN_vkDestroyImage>("vkDestroyImage")(source.device,image,nullptr);
            if (memory) source.dev<PFN_vkFreeMemory>("vkFreeMemory")(source.device,memory,nullptr);
            framebuffer={}; pass={}; view={}; image={}; memory={}; cleanupSucceeded=true;
            return true;
        } catch (...) { return false; }
    }
};
}
int main() {
    // Failure retains the entire owner until this bounded test process exits.
    auto fixture = std::make_unique<Fixture>();
    try {
        fixture->initialize(); fixture->run(12);
        if (!fixture->cleanup()) throw std::runtime_error("GPU tail/attachment cleanup not verified");
        fixture.reset();
        std::cout << "{\"testPassed\":true,\"renderPassSubmissions\":12,\"expectedLayoutOperations\":36,\"attachmentCleanupSucceeded\":true,\"pixelsReadBack\":false,\"gameTested\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        fixture.release();
        std::cerr << error.what() << '\n'; return 1;
    }
}
