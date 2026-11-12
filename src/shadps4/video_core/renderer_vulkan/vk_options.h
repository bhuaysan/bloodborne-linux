// What bb-run hands the renderer (bb-run --device, --validation*, --present-mode).

#pragma once

#include "common/types.h"

namespace Vulkan {

// The Khronos validation layer. `enable` loads it with its core checks; `sync` and `gpu_assisted`
// add synchronization validation and GPU-assisted validation (bb-run --validation,
// --validation-sync and --validation-gpu).
struct ValidationOptions {
    bool enable = false;
    bool sync = false;
    bool gpu_assisted = false;
};

enum class PresentMode : u8 {
    Fifo,
    Mailbox,
    Immediate,
};

// What bb-run hands the renderer, in place of a settings object.
struct RendererOptions {
    s32 gpu_id = -1; // Index of the Vulkan device, -1 for the instance's own choice.
    ValidationOptions validation;
    PresentMode present_mode = PresentMode::Mailbox;
};

} // namespace Vulkan
