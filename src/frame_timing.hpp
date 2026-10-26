#pragma once
#include <cstdint>
namespace xess_vk {
struct FrameTimingTicket {
    std::uint32_t sdkId{};
    std::uint64_t contextGeneration{};
    bool latencyEnabled{};
    explicit operator bool() const noexcept {
        return sdkId && contextGeneration;
    }
    friend bool operator==(const FrameTimingTicket &, const FrameTimingTicket &) = default;
};
struct FrameTimingAssociation {
    std::uint32_t engineFrame{};
    FrameTimingTicket ticket;
    std::uint32_t bootstrapSdkId{};
    bool valid{};
};
} // namespace xess_vk
