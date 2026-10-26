#pragma once
#include <atomic>
#include <cstdint>

namespace xess_vk::game_frame_cap {
// The game menu keeps its original labels. Only its 30 FPS slot is remapped.
inline int map(int requested,bool enabled,unsigned target) noexcept {return enabled&&requested==30&&target>=30&&target<=120?int(target):requested;}
inline std::atomic<bool> requested{},installed{};
inline std::atomic<std::uint64_t> calls{},remapped{};
inline std::atomic<int> lastRequested{},lastForwarded{},readback{};
inline std::atomic<const char*> state{"disabled"};
}
