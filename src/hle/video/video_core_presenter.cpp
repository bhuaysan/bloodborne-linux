#include "hle/video/video_core_presenter.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "common/log.hpp"
#include "core/emulator_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

namespace bb::hle::video {
namespace {

constexpr std::string_view channel = "video";

bool contains_ignoring_case(std::string_view text, std::string_view part) {
    const auto lower = [](unsigned char c) { return std::tolower(c); };
    return !std::ranges::search(text, part, {}, lower, lower).empty();
}

}  // namespace

VideoCorePresenter::VideoCorePresenter(Frontend::WindowSDL& window)
    : liverpool_(std::make_unique<AmdGpu::Liverpool>()),
      presenter_(std::make_unique<Vulkan::Presenter>(window, liverpool_.get())) {}

// In reverse: the presenter's rasterizer is bound to Liverpool, whose GPU thread stops last.
VideoCorePresenter::~VideoCorePresenter() {
    presenter_.reset();
    liverpool_.reset();
}

void VideoCorePresenter::set_vo_port(VideoOutPort* port) {
    liverpool_->SetVoPort(port);
}

void VideoCorePresenter::send_command(std::function<void()> work) {
    liverpool_->SendCommand(std::move(work));
}

void VideoCorePresenter::register_surface(const BufferAttributeGroup& group,
                                          std::uint64_t address) {
    presenter_->RegisterVideoOutSurface(group, address);
}

Vulkan::Frame* VideoCorePresenter::prepare_frame(const BufferAttributeGroup& group,
                                                 std::uint64_t address) {
    return presenter_->PrepareFrame(group, address);
}

Vulkan::Frame* VideoCorePresenter::prepare_blank_frame(bool present_thread) {
    return presenter_->PrepareBlankFrame(present_thread);
}

Vulkan::Frame* VideoCorePresenter::prepare_last_frame() {
    return presenter_->PrepareLastFrame();
}

void VideoCorePresenter::present(Vulkan::Frame* frame, bool is_reusing_frame,
                                 bool is_game_frame) {
    presenter_->Present(frame, is_reusing_frame, is_game_frame);
    if (is_game_frame && !is_reusing_frame && ++game_frames_ == 1) {
        common::log(common::LogLevel::info, channel, "first game frame presented");
    }
}

void VideoCorePresenter::set_hdr(bool enable) {
    presenter_->SetHDR(enable);
}

bool VideoCorePresenter::must_keep_drawing() {
    return ImGui::Core::MustKeepDrawing();
}

std::optional<std::int32_t> resolve_gpu_id(std::string_view selector,
                                          Frontend::WindowSystemType window_type,
                                          std::string& why) {
    if (selector.empty()) {
        return -1;
    }
    // Created as the Presenter's Vulkan::Instance creates its own: the same window system type,
    // so the same surface extension, and the same layers. The loader's implicit layers (Mesa's
    // device_select) may order the devices by what the instance enables, so a headless instance
    // could list them in another order. Not a Vulkan::Instance: its destructor needs a logical
    // device.
    const vk::UniqueInstance instance =
        Vulkan::CreateInstance(window_type, EmulatorSettings.IsVkValidationEnabled(),
                               EmulatorSettings.IsVkCrashDiagnosticEnabled());
    const auto [result, devices] = instance->enumeratePhysicalDevices();
    if (result != vk::Result::eSuccess) {
        why = "vkEnumeratePhysicalDevices: " + vk::to_string(result);
        return std::nullopt;
    }
    std::string names;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const vk::PhysicalDeviceProperties properties = devices[i].getProperties();
        const std::string_view name = properties.deviceName.data();
        if (contains_ignoring_case(name, selector)) {
            common::log(common::LogLevel::info, channel, "--device {}: GPU {} ({})", selector, i,
                        name);
            return static_cast<std::int32_t>(i);
        }
        names += names.empty() ? "" : ", ";
        names += name;
    }
    why = "no Vulkan device name contains \"" + std::string(selector) + "\" (devices: " + names +
          ")";
    return std::nullopt;
}

}  // namespace bb::hle::video
