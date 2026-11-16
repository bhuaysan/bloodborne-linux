// The Havok AI world's step at the target frame rate (docs/design/frame-rate.md, "The AI step"):
// a wrapper around the game's step that gives the call from chr_ai_step frame_step() instead of
// its literal 1/30.
#include "game/ai/world_step.hpp"

#include "game/image.hpp"
#include "hook/hook.hpp"

namespace bb::game {

void ai_world_step_from(AiWorldStep* original, std::uintptr_t return_address, void* manager,
                        const FD4TaskData* task) {
    if (frame_rate() == default_frame_rate ||
        return_address != image().runtime(chr_ai_step_call_return)) {
        original(manager, task);
        return;
    }
    const FD4TaskData step{task->vtable, frame_step()};  // the game: 1/30 (0x01e2bcd1)
    original(manager, &step);
}

// 0x01dcdad0
[[gnu::noinline]] void ai_world_step(void* manager, const FD4TaskData* task) {
    // Entered through the hook's jmp, so the return address is the game's call site.
    const auto return_address = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    ai_world_step_from(reinterpret_cast<AiWorldStep*>(hook::original(image(), 0x01dcdad0)),
                       return_address, manager, task);
}

}  // namespace bb::game

BB_WRAP(0x01dcdad0, bb::game::ai_world_step, bb::game::ai_world_step_prologue);
