// What src/shadps4 reads of shadPS4's settings object, fixed at the values this port runs with.
// A port for one game has nothing to configure here. The values are upstream's defaults
// (core/emulator_settings.h of v.0.21.0): FSR and every debug aid off, RCAS on, present mode
// Mailbox; for the system libraries the default audio device at full volume, the standard pad
// with motion controls, and ImeDialog's accessibility and short URL panel off. The pipeline cache
// stays off until R8 makes it work on disk (docs/design/renderer.md). The settings that were off
// and only selected code paths (DMA, readbacks, userfaultfd, the null GPU, copied command
// buffers, HDR, the OpenAL backend) are gone together with those paths.
#pragma once

#include <string>
#include <string_view>

#include "common/types.h"

namespace bb::shader {

// The index of the Vulkan device the renderer's instance uses, -1 (the default) for its own
// choice. Set before the instance is created.
s32 gpu_id();
void set_gpu_id(s32 id);

struct FixedSettings {
    // GPU.
    static constexpr std::string_view GetPresentMode() { return "Mailbox"; }
    static constexpr bool IsFsrEnabled() { return false; }
    static constexpr bool IsRcasEnabled() { return true; }
    static constexpr int GetRcasAttenuation() { return 250; }

    // Vulkan. GPU id -1 lets the instance pick the device; bb-run sets the index --device names
    // (set_gpu_id).
    static s32 GetGpuId() { return gpu_id(); }
    static constexpr bool IsVkValidationEnabled() { return false; }
    static constexpr bool IsVkValidationCoreEnabled() { return true; }
    static constexpr bool IsVkValidationSyncEnabled() { return false; }
    static constexpr bool IsVkValidationGpuEnabled() { return false; }
    static constexpr bool IsPipelineCacheEnabled() { return false; }
    static constexpr bool IsPipelineCacheArchived() { return false; }

    // Sound (AudioOut).
    static constexpr int GetVolumeSlider() { return 100; }
    static std::string GetSDLMainOutputDevice() { return "Default Device"; }
    static std::string GetSDLPadSpkOutputDevice() { return "Default Device"; }

    // Input (Pad, the controller, ImeDialog).
    static constexpr bool IsUsingSpecialPad() { return false; }
    static constexpr int GetSpecialPadClass() { return 1; }
    static constexpr bool IsMotionControlsEnabled() { return true; }
    static constexpr bool IsImeAccessibilityEnabled() { return false; }
    static constexpr bool IsImeUrlMailShortPanel() { return false; }

    // The language of the ImGui fonts: 1, upstream's default (English).
    static constexpr int GetConsoleLanguage() { return 1; }
};

}  // namespace bb::shader

inline constexpr bb::shader::FixedSettings EmulatorSettings{};
