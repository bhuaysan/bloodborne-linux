// What bb-run hands the renderer from its options (#725): a function of its own so that a test
// can check that each option reaches its field.
#pragma once

#include <cstdint>

#include "tools/run_options.hpp"
#include "video_core/renderer_vulkan/vk_options.h"

namespace bb::tools {

// The renderer's options for the device `gpu_id` (-1 for video_core's choice).
inline Vulkan::RendererOptions renderer_options(const RunOptions& options, std::int32_t gpu_id) {
    Vulkan::RendererOptions renderer;
    renderer.gpu_id = gpu_id;
    renderer.validation = {.enable = options.validation,
                           .sync = options.validation_sync,
                           .gpu_assisted = options.validation_gpu};
    switch (options.present_mode) {
    case PresentMode::fifo: renderer.present_mode = Vulkan::PresentMode::Fifo; break;
    case PresentMode::mailbox: renderer.present_mode = Vulkan::PresentMode::Mailbox; break;
    case PresentMode::immediate: renderer.present_mode = Vulkan::PresentMode::Immediate; break;
    }
    renderer.fsr = options.fsr;
    renderer.rcas = options.rcas;
    renderer.rcas_attenuation = options.rcas_attenuation;
    renderer.anisotropy = options.anisotropy;
    return renderer;
}

}  // namespace bb::tools
