// What bb-run hands the renderer (bb-run --device, --validation*, --present-mode, --fsr and
// RCAS, --anisotropy).

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
    // FSR 1 upscales the game's frame when the window is larger (bb-run --fsr); RCAS then
    // sharpens the result by `rcas_attenuation` stops below its maximum (--no-rcas,
    // --rcas-attenuation). Off, on and 0.25, as shadPS4's defaults have them.
    bool fsr = false;
    bool rcas = true;
    float rcas_attenuation = 0.25f;
    // The anisotropy of samplers with linear filtering and mipmaps (bb-run --anisotropy, #658):
    // 0 keeps the game's value, 1 to 16 sets it (VideoCore::ChooseAnisotropy).
    u32 anisotropy = 0;
};

} // namespace Vulkan
