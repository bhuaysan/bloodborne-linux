// The Havok physics world's step at the target frame rate
// (docs/design/frame-rate.md, "The Havok world step").
#pragma once

#include <cstddef>
#include <cstdint>

namespace bb::game {

// The step record havok_world_step hands to the world's collide stage (0x00b5a2e0).
struct HavokStepInfo {
    float delta_time;          // seconds; the game's 1/30
    std::uint32_t padding;     // the game leaves it unwritten (stack garbage); 0 here
    std::uint64_t manager_c8;  // the manager's +0xc8. TODO(name)
    std::int32_t threads;      // the thread pool's slot +0x48 + 1. TODO(name): read as its thread
                               // count plus the calling thread
    std::int32_t zero;
};
static_assert(sizeof(HavokStepInfo) == 0x18 && offsetof(HavokStepInfo, manager_c8) == 8 &&
              offsetof(HavokStepInfo, threads) == 0x10);

// 0x017f99d0: one step of the game's Havok world by frame_step() seconds (the game: 1/30). `self`
// is the game's Havok manager (layout in docs/design/frame-rate.md); the world steps only while
// stepping is on (+0x26c), the world, the job queue and the thread pool exist and the manager
// isn't held (+0x30).
void havok_world_step(void* self);

}  // namespace bb::game
