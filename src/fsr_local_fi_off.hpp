#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace xess_vk::fsr_local_fi_off {
inline constexpr std::array<std::uint32_t, 2> siteRvas{0x1efac1, 0x1f0693};
inline constexpr std::array<std::uint8_t, 5> cmpBytes{0x41, 0x80, 0x7d, 0x3b, 0x00};
struct BodyGuard {
    std::uint32_t rva;
    const std::uint8_t* bytes;
    std::size_t size;
};
std::span<const BodyGuard> bodyGuards() noexcept;
// Allocate the two fixed near islands before either MH_CreateHook call.
bool prepare(std::uintptr_t moduleBase) noexcept;
void* entry(std::size_t index) noexcept;
void** originalSlot(std::size_t index) noexcept;
// Before enabling either hook. The caller separately verifies the pinned
// module, both sites and bodyGuards using its existing protected PE reader.
bool configureSite(std::size_t index, void* trampoline, void* continuation) noexcept;
bool siteReady(std::size_t index) noexcept;
// Publish only after the complete installation transaction's postchecks.
bool activate() noexcept;
void stop() noexcept;
bool active() noexcept;
}
