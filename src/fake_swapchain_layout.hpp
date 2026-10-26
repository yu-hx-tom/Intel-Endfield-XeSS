#pragma once
#include <cstdint>
#include <new>
#include <vector>
#include <vulkan/vulkan.h>

namespace xess_vk::presentation::fake_layout {
#ifdef XESS_METADATA_CONSUMER_TEST
inline bool failNextCopy{};
#endif
inline void beforeCopy() {
#ifdef XESS_METADATA_CONSUMER_TEST
    if (failNextCopy) { failNextCopy = false; throw std::bad_alloc(); }
#endif
}
inline VkImageLayout physical(VkImageLayout layout) noexcept {
    return layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? VK_IMAGE_LAYOUT_GENERAL : layout;
}
template<class Barrier, class Predicate>
bool needsBarriers(std::uint32_t count, const Barrier* input, Predicate fakeImage) {
    if (!input) return false;
    for (std::uint32_t i = 0; i < count; ++i)
        if ((input[i].oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || input[i].newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) && fakeImage(input[i].image))
            return true;
    return false;
}
template<class Barrier, class Predicate>
const Barrier* barriers(std::uint32_t count, const Barrier* input, Predicate fakeImage, std::vector<Barrier>& copy) {
    if (!needsBarriers(count, input, fakeImage)) return input;
    beforeCopy();
    copy.assign(input, input + count);
    for (auto& barrier : copy) if (fakeImage(barrier.image)) {
        barrier.oldLayout = physical(barrier.oldLayout);
        barrier.newLayout = physical(barrier.newLayout);
    }
    return copy.data();
}
template<class Predicate>
const VkDependencyInfo* dependencies(std::uint32_t count, const VkDependencyInfo* input, Predicate fakeImage,
    std::vector<VkDependencyInfo>& copy, std::vector<std::vector<VkImageMemoryBarrier2>>& images) {
    if (!input) return input;
    bool changed{};
    for (std::uint32_t i = 0; i < count && !changed; ++i)
        changed = needsBarriers(input[i].imageMemoryBarrierCount, input[i].pImageMemoryBarriers, fakeImage);
    if (!changed) return input;
    beforeCopy();
    copy.assign(input, input + count);
    images.resize(count);
    for (std::uint32_t i = 0; i < count; ++i)
        copy[i].pImageMemoryBarriers = barriers(input[i].imageMemoryBarrierCount, input[i].pImageMemoryBarriers, fakeImage, images[i]);
    return copy.data();
}
template<class Info, class Attachment>
const Info* renderPass(bool enabled, const Info* input, Info& copy, std::vector<Attachment>& attachments) {
    if (!enabled || !input || !input->pAttachments) return input;
    bool changed{};
    for (std::uint32_t i = 0; i < input->attachmentCount && !changed; ++i)
        changed = input->pAttachments[i].initialLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || input->pAttachments[i].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (!changed) return input;
    beforeCopy();
    attachments.assign(input->pAttachments, input->pAttachments + input->attachmentCount);
    for (auto& attachment : attachments) {
        attachment.initialLayout = physical(attachment.initialLayout);
        attachment.finalLayout = physical(attachment.finalLayout);
    }
    copy = *input;
    copy.pAttachments = attachments.data();
    return &copy;
}
} // namespace xess_vk::presentation::fake_layout
