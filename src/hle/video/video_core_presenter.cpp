#include "hle/video/video_core_presenter.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "common/log.hpp"
#include "core/gpu_stats.h"
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

// Every stats_frames-th game frame logs the rasterizer's counts.
constexpr std::uint64_t stats_frames = 300;

bool contains_ignoring_case(std::string_view text, std::string_view part) {
    const auto lower = [](unsigned char c) { return std::tolower(c); };
    return !std::ranges::search(text, part, {}, lower, lower).empty();
}

}  // namespace

VideoCorePresenter::VideoCorePresenter(Frontend::WindowSDL* window)
    : liverpool_(std::make_unique<AmdGpu::Liverpool>()),
      presenter_(window != nullptr
                     ? std::make_unique<Vulkan::Presenter>(*window, liverpool_.get())
                     : nullptr) {}

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

// Without the Presenter there are no surfaces and no frames.
void VideoCorePresenter::register_surface(const BufferAttributeGroup& group,
                                          std::uint64_t address) {
    if (presenter_) {
        presenter_->RegisterVideoOutSurface(group, address);
    }
}

Vulkan::Frame* VideoCorePresenter::prepare_frame(const BufferAttributeGroup& group,
                                                 std::uint64_t address) {
    return presenter_ ? presenter_->PrepareFrame(group, address) : nullptr;
}

Vulkan::Frame* VideoCorePresenter::prepare_blank_frame(bool present_thread) {
    return presenter_ ? presenter_->PrepareBlankFrame(present_thread) : nullptr;
}

Vulkan::Frame* VideoCorePresenter::prepare_last_frame() {
    return presenter_ ? presenter_->PrepareLastFrame() : nullptr;
}

void VideoCorePresenter::present(Vulkan::Frame* frame, bool is_reusing_frame,
                                 bool is_game_frame) {
    if (!presenter_) {
        return;
    }
    presenter_->Present(frame, is_reusing_frame, is_game_frame);
    if (!is_game_frame || is_reusing_frame) {
        return;
    }
    const std::uint64_t n = ++game_frames_;
    if (n == 1) {
        common::log(common::LogLevel::info, channel, "first game frame presented");
    }
    if (n % stats_frames == 0) {
        common::log(common::LogLevel::info, channel, "game frame {}: {} draws, {} dispatches", n,
                    draws(), dispatches());
    }
}

void VideoCorePresenter::set_hdr(bool enable) {
    if (presenter_) {
        presenter_->SetHDR(enable);
    }
}

bool VideoCorePresenter::must_keep_drawing() {
    return presenter_ && ImGui::Core::MustKeepDrawing();
}

void VideoCorePresenter::submit_gfx(std::span<const std::uint32_t> dcb,
                                    std::span<const std::uint32_t> ccb) {
    liverpool_->SubmitGfx(dcb, ccb);
}

void VideoCorePresenter::submit_asc(std::uint32_t gnm_vqid, std::span<const std::uint32_t> acb) {
    liverpool_->SubmitAsc(gnm_vqid, acb);
}

void VideoCorePresenter::submit_done() {
    liverpool_->SubmitDone();
}

bool VideoCorePresenter::is_gpu_idle() {
    return liverpool_->IsGpuIdle();
}

std::uint32_t VideoCorePresenter::map_queue(std::uint32_t* ring, std::uint32_t ring_dwords,
                                            std::uint32_t* read_pointer, std::uint32_t pipe) {
    return liverpool_->asc_queues
        .Insert(reinterpret_cast<VAddr>(ring), read_pointer, ring_dwords, pipe)
        .index;
}

bool VideoCorePresenter::unmap_queue(std::uint32_t index) {
    if (!liverpool_->asc_queues.IsAllocated(Common::SlotId{index})) {
        return false;
    }
    liverpool_->asc_queues.Erase(Common::SlotId{index});
    return true;
}

std::span<const std::uint32_t> VideoCorePresenter::queue_ring(std::uint32_t index) {
    if (!liverpool_->asc_queues.IsAllocated(Common::SlotId{index})) {
        return {};
    }
    const auto& queue = liverpool_->asc_queues[Common::SlotId{index}];
    return {reinterpret_cast<const std::uint32_t*>(queue.map_addr), queue.ring_size_dw};
}

std::uint64_t VideoCorePresenter::draws() {
    return GpuStats.draws.load(std::memory_order_relaxed);
}

std::uint64_t VideoCorePresenter::dispatches() {
    return GpuStats.dispatches.load(std::memory_order_relaxed);
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
        Vulkan::CreateInstance(window_type, EmulatorSettings.IsVkValidationEnabled());
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
