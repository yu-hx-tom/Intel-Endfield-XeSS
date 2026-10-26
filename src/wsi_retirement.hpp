#pragma once
// Bookkeeping for a SELF-OWNED, serial WSI experiment. A successful acquire
// fence wait retires that image's previous present. Reacquiring the first
// successfully presented image of a replacement chain also retires inherited
// old chains, following the Khronos swapchain-recreation sample. This helper
// does not wait, call Vulkan, prove GPU completion, or authorize a game hook.
// https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vulkan/vulkan_core.h>

namespace xess_vk::testing {
class WsiRetirement {
public:
    static constexpr std::size_t maxGenerations = 8, maxImages = 32;
    struct Image { std::uint64_t pendingSerial{}; bool acquired{}; };
    struct Generation {
        std::uint64_t serial{}, firstPresentSerial{};
        std::uint32_t imageCount{}, firstPresentImage{};
        bool retired{}, retirementProven{}, destroyed{};
        std::array<Image,maxImages> images{};
    };
    struct Stats {
        std::uint64_t acquires{}, presents{}, retiredByAcquire{}, retiredBySuccessor{};
        std::uint64_t pending{}, highWaterPending{}, generations{}, destroyedGenerations{};
    };
    static bool accepted(VkResult result) noexcept { return result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR; }
    const Stats& stats() const noexcept { return totals_; }
    std::uint64_t current() const noexcept { return current_; }
    bool canCreate() const noexcept { return totals_.generations < maxGenerations && !current_; }
    const Generation& generation(std::uint64_t serial) const { return find(serial); }
    void created(std::uint32_t imageCount) {
        require(canCreate() && imageCount && imageCount<=maxImages,"WSI generation capacity or state");
        const auto serial=totals_.generations+1;
        auto& value=generations_[static_cast<std::size_t>(serial-1)];
        value.serial=serial;value.imageCount=imageCount;totals_.generations=serial;current_=serial;
    }
    // Call immediately BEFORE vkCreateSwapchainKHR(oldSwapchain). Retirement
    // is irreversible even when the driver returns failure. No fallback acquire.
    void retiringForReplacement() {
        require(current_ && totals_.generations<maxGenerations,"No current chain or generation capacity");
        auto& value=find(current_);
        for(std::uint32_t i=0;i<value.imageCount;++i)require(!value.images[i].acquired,"Cannot replace with an acquired image");
        value.retired=true;current_=0;
    }
    void acquiredAfterFence(std::uint64_t generationSerial,std::uint32_t index,std::uint64_t serial,
                            VkResult acquireResult,VkResult fenceResult) {
        require(accepted(acquireResult)&&fenceResult==VK_SUCCESS,"Acquire/fence has no completion evidence");
        auto& value=find(generationSerial);
        require(generationSerial==current_&&!value.retired&&!value.destroyed,"Cannot acquire a retired generation");
        require(index<value.imageCount&&serial==totals_.acquires+1&&serial,"Invalid acquisition identity");
        auto& image=value.images[index];require(!image.acquired,"Image acquired twice");
        const bool anchor=image.pendingSerial && image.pendingSerial==value.firstPresentSerial && index==value.firstPresentImage;
        if(image.pendingSerial){image.pendingSerial=0;--totals_.pending;++totals_.retiredByAcquire;}
        if(anchor) {
            for(std::uint64_t n=1;n<generationSerial;++n) {
                auto& old=find(n);require(old.retired,"Replacement lineage broken");
                if(old.retirementProven)continue;
                for(std::uint32_t i=0;i<old.imageCount;++i)if(old.images[i].pendingSerial){
                    old.images[i].pendingSerial=0;--totals_.pending;++totals_.retiredBySuccessor;
                }
                old.retirementProven=true;
            }
        }
        image.acquired=true;totals_.acquires=serial;
    }
    void presented(std::uint64_t generationSerial,std::uint32_t index,std::uint64_t serial,
                   VkResult aggregate,VkResult perSwapchain) {
        require(accepted(aggregate)&&accepted(perSwapchain),"Failed presentation is unresolved");
        auto& value=find(generationSerial);
        require(generationSerial==current_&&!value.retired&&!value.destroyed,"Present generation invalid");
        require(index<value.imageCount&&serial==totals_.presents+1&&serial,"Invalid presentation identity");
        auto& image=value.images[index];require(image.acquired&&!image.pendingSerial,"Present without fresh acquisition");
        image.acquired=false;image.pendingSerial=serial;
        if(!value.firstPresentSerial){value.firstPresentSerial=serial;value.firstPresentImage=index;}
        ++totals_.pending;if(totals_.pending>totals_.highWaterPending)totals_.highWaterPending=totals_.pending;
        totals_.presents=serial;
    }
    bool canDestroy(std::uint64_t serial) const {
        const auto& value=find(serial);return value.retired&&value.retirementProven&&!value.destroyed;
    }
    void destroyed(std::uint64_t serial) {
        require(canDestroy(serial),"Destroy lacks replacement retirement evidence");
        auto& value=find(serial);
        for(std::uint32_t i=0;i<value.imageCount;++i)require(!value.images[i].acquired&&!value.images[i].pendingSerial,"Destroy has outstanding image");
        value.destroyed=true;++totals_.destroyedGenerations;
    }
    bool conserved() const noexcept {
        std::uint64_t pending=0;
        for(const auto& generation:generations_)for(const auto& image:generation.images)if(image.pendingSerial)++pending;
        return pending==totals_.pending && totals_.presents==totals_.retiredByAcquire+totals_.retiredBySuccessor+pending;
    }
private:
    static void require(bool value,const char* message){if(!value)throw std::logic_error(message);}
    Generation& find(std::uint64_t serial) {
        require(serial&&serial<=totals_.generations,"Unknown WSI generation");return generations_[static_cast<std::size_t>(serial-1)];
    }
    const Generation& find(std::uint64_t serial) const {
        require(serial&&serial<=totals_.generations,"Unknown WSI generation");return generations_[static_cast<std::size_t>(serial-1)];
    }
    std::array<Generation,maxGenerations> generations_{};
    Stats totals_;
    std::uint64_t current_{};
};
} // namespace xess_vk::testing
