#pragma once
#include "sr_execute_overlay.hpp"
#include <array>
#include <atomic>
#include <cassert>

namespace xess_vk::fsr4 {
// Writers are serialized by the bridge metadata mutex. Only command()/empty()
// run without it; Vulkan callers already serialize reuse of the same logical CB.
class CommandMap {
    struct Entry {
        sr_overlay::RecordingKey key{};
        std::atomic<std::uint64_t> version{};
        std::atomic<VkCommandBuffer> source{},tail{};
    };
    std::array<Entry,16> entries_; // Matches Overlay's existing 16 pending recordings.
    std::atomic<unsigned> count_{};
public:
    static_assert(std::atomic<VkCommandBuffer>::is_always_lock_free);
    static constexpr std::size_t capacity=16;
    bool empty()const noexcept{return count_.load(std::memory_order_acquire)==0;}
    std::size_t reserve()const noexcept {
        for(std::size_t n=0;n<entries_.size();++n)if(!entries_[n].source.load(std::memory_order_relaxed))return n;
        return capacity;
    }
    void publish(std::size_t n,sr_overlay::RecordingKey key,VkCommandBuffer tail)noexcept {
        assert(n<entries_.size()&&key.command&&tail&&!entries_[n].source.load());
        auto& e=entries_[n];e.key=key;
        e.version.fetch_add(1,std::memory_order_acq_rel);
        e.tail.store(tail,std::memory_order_release);
        e.source.store(reinterpret_cast<VkCommandBuffer>(std::uintptr_t(key.command)),std::memory_order_release);
        e.version.fetch_add(1,std::memory_order_release);
        count_.fetch_add(1,std::memory_order_release);
    }
    void erase(sr_overlay::RecordingKey key)noexcept {
        for(auto& e:entries_)if(e.key==key&&e.source.load(std::memory_order_relaxed)){
            e.version.fetch_add(1,std::memory_order_acq_rel);
            e.source.store(VK_NULL_HANDLE,std::memory_order_release);
            e.version.fetch_add(1,std::memory_order_release);
            count_.fetch_sub(1,std::memory_order_release);
        }
    }
    VkCommandBuffer command(VkCommandBuffer original)const noexcept {
        if(empty())return original;
        for(const auto& e:entries_){
            const auto version=e.version.load(std::memory_order_acquire);
            if((version&1)||e.source.load(std::memory_order_acquire)!=original)continue;
            const auto tail=e.tail.load(std::memory_order_acquire);
            // The version also rejects source A -> B -> A slot reuse.
            if(tail&&e.version.load(std::memory_order_acquire)==version)return tail;
        }
        return original;
    }
};
}
