// What src/shadps4 reads of shadPS4's settings object, fixed at the values this port runs with.
// A port for one game has nothing to configure here. The values are upstream's defaults
// (core/emulator_settings.h of v.0.21.0): DMA, inlined fetch shaders, readbacks, userfaultfd, the
// null GPU, HDR, FSR and every debug aid off, RCAS on, present mode Mailbox; for the system
// libraries the SDL audio backend on the default device at full volume, the standard pad with
// motion controls, and ImeDialog's accessibility and short URL panel off. The pipeline cache
// stays off until R8 makes it work on disk (docs/design/renderer.md).
#pragma once

#include <string>
#include <string_view>

#include "common/types.h"

// The values of upstream's readbacks_mode setting, which the buffer cache compares against.
enum GpuReadbacksMode : int {
    Disabled,
    Relaxed,
    Precise,
};

// The values of upstream's audio_backend setting. OpenAL is not built (syslibs/left_out.cpp).
enum AudioBackend : int {
    SDL,
    OpenAL,
};

namespace bb::shader {

// The index of the Vulkan device the renderer's instance uses, -1 (the default) for its own
// choice. Set before the instance is created.
s32 gpu_id();
void set_gpu_id(s32 id);

struct FixedSettings {
    // Shader translation.
    static constexpr bool IsDumpShaders() { return false; }
    static constexpr bool IsDirectMemoryAccessEnabled() { return false; }
    static constexpr bool IsInlineFetchShader() { return false; }
    static constexpr bool IsPatchShaders() { return false; }
    static constexpr bool IsFastShaderMath() { return false; }
    static constexpr bool IsShaderCollect() { return false; }

    // GPU.
    static constexpr bool IsNullGPU() { return false; }
    static constexpr bool IsCopyGpuBuffers() { return false; }
    static constexpr u32 GetReadbacksMode() { return GpuReadbacksMode::Disabled; }
    static constexpr bool IsReadbackLinearImagesEnabled() { return false; }
    static constexpr bool IsUserfaultfdTracking() { return false; }
    static constexpr std::string_view GetPresentMode() { return "Mailbox"; }
    static constexpr bool IsHdrAllowed() { return false; }
    static constexpr bool IsFsrEnabled() { return false; }
    static constexpr bool IsRcasEnabled() { return true; }
    static constexpr int GetRcasAttenuation() { return 250; }

    // Vulkan. GPU id -1 lets the instance pick the device; bb-run sets the index --device names
    // (set_gpu_id).
    static s32 GetGpuId() { return gpu_id(); }
    static constexpr bool IsRenderdocEnabled() { return false; }
    static constexpr bool IsVkValidationEnabled() { return false; }
    static constexpr bool IsVkValidationCoreEnabled() { return true; }
    static constexpr bool IsVkValidationSyncEnabled() { return false; }
    static constexpr bool IsVkValidationGpuEnabled() { return false; }
    static constexpr bool IsVkCrashDiagnosticEnabled() { return false; }
    static constexpr bool IsVkHostMarkersEnabled() { return false; }
    static constexpr bool IsVkGuestMarkersEnabled() { return false; }
    static constexpr bool IsPipelineCacheEnabled() { return false; }
    static constexpr bool IsPipelineCacheArchived() { return false; }

    // Sound (AudioOut).
    static constexpr u32 GetAudioBackend() { return AudioBackend::SDL; }
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
