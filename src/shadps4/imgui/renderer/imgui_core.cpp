// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/imgui/renderer/imgui_core.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-12
// License texts: third_party/shadps4/LICENSES/

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <SDL3/SDL_events.h>
#include <imgui.h>

#include "common/path_util.h"
#include "font_data.h"
#include "font_stack.h"
#include "imgui/imgui_layer.h"
#include "imgui_core.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "imgui_internal.h"
#include "sdl_window.h"
#include "texture_manager.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

static void CheckVkResult(const vk::Result err) {
    LOG_ERROR(ImGui, "Vulkan error {}", vk::to_string(err));
}

static std::vector<ImGui::Layer*> layers;

// Update layers before rendering to allow layer changes to be applied during rendering.
// Using deque to keep the order of changes in case a Layer is removed then added again between
// frames.
static std::deque<std::pair<bool, ImGui::Layer*>> change_layers{};
static std::mutex change_layers_mutex{};

static std::atomic<std::uint32_t> force_gamepad_input_capture_count{0};

namespace ImGui {

namespace Core {

void AcquireGamepadInputCapture() {
    force_gamepad_input_capture_count.fetch_add(1, std::memory_order_relaxed);
}

void ReleaseGamepadInputCapture() {
    std::uint32_t expected = force_gamepad_input_capture_count.load(std::memory_order_relaxed);
    while (expected != 0) {
        if (force_gamepad_input_capture_count.compare_exchange_weak(
                expected, expected - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return;
        }
    }
    LOG_WARNING(ImGui, "ReleaseGamepadInputCapture called with no active capture");
}

bool IsGamepadInputCaptured() {
    return force_gamepad_input_capture_count.load(std::memory_order_relaxed) > 0;
}

void Initialize(const ::Vulkan::Instance& instance, const Frontend::WindowSDL& window,
                const u32 image_count, vk::Format surface_format,
                const vk::AllocationCallbacks* allocator) {

    const auto config_path = GetUserPath(Common::FS::PathType::UserDir) / "imgui.ini";
    const auto log_path = GetUserPath(Common::FS::PathType::LogDir) / "imgui_log.txt";

    CreateContext();
    ImGuiIO& io = GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.DisplaySize = ImVec2((float)window.GetWidth(), (float)window.GetHeight());
    PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f); // Makes the window edges rounded

    auto path = config_path.u8string();
    char* config_file_buf = new char[path.size() + 1]();
    std::memcpy(config_file_buf, path.c_str(), path.size());
    io.IniFilename = config_file_buf;

    path = log_path.u8string();
    char* log_file_buf = new char[path.size() + 1]();
    std::memcpy(log_file_buf, path.c_str(), path.size());
    io.LogFilename = log_file_buf;

    ImFontConfig font_cfg{};
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 1;
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    constexpr int console_language = 1; // English, upstream's default.
    io.FontDefault = FontStack::AddPrimaryUiFont(io.Fonts, 32.0f, console_language, font_cfg, true);

    io.Fonts->AddFontFromMemoryCompressedTTF(imgui_font_proggyvector_regular_compressed_data,
                                             imgui_font_proggyvector_regular_compressed_size,
                                             32.0f);

    // Avoid exploding atlas size on Metal/MoltenVK when CJK fallback is enabled.
    FontStack::AddPrimaryUiFont(io.Fonts, 128.0f, console_language, font_cfg, false);

    // Big Picture
    FontStack::AddPrimaryUiFont(ImGui::GetIO().Fonts, 64.0f, console_language,
                                font_cfg, true);

    // Let the atlas size grow to the largest image the device can create,
    // floored to the power of two the packer requires.
    const u32 max_dim = instance.GetPhysicalDevice().getProperties().limits.maxImageDimension2D;
    const int atlas_max = static_cast<int>(std::bit_floor(std::max<u32>(max_dim, 512u)));
    io.Fonts->TexMaxWidth = atlas_max;
    io.Fonts->TexMaxHeight = atlas_max;

    io.FontGlobalScale = 0.5f;

    StyleColorsDark();

    Sdl::Init(window.GetSDLWindow());

    const Vulkan::InitInfo vk_info{
        .instance = instance.GetInstance(),
        .physical_device = instance.GetPhysicalDevice(),
        .device = instance.GetDevice(),
        .queue_family = instance.GetPresentQueueFamilyIndex(),
        .queue = instance.GetPresentQueue(),
        .image_count = image_count,
        .min_allocation_size = 1024 * 1024,
        .pipeline_rendering_create_info{
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &surface_format,
        },
        .allocator = allocator,
        .check_vk_result_fn = &CheckVkResult,
    };
    Vulkan::Init(vk_info);

    if (const auto dpi = SDL_GetWindowDisplayScale(window.GetSDLWindow()); dpi > 0.0f) {
        GetIO().FontGlobalScale *= dpi;
    }

    std::at_quick_exit([] { SaveIniSettingsToDisk(GetIO().IniFilename); });
}

void OnResize() {
    Sdl::OnResize();
}

void Shutdown(const vk::Device& device) {
    auto result = device.waitIdle();
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(ImGui, "Failed to wait for Vulkan device idle on shutdown: {}",
                    vk::to_string(result));
    }

    const ImGuiIO& io = GetIO();
    const auto ini_filename = (void*)io.IniFilename;
    const auto log_filename = (void*)io.LogFilename;

    Vulkan::Shutdown();
    Sdl::Shutdown();
    DestroyContext();

    delete[] (char*)ini_filename;
    delete[] (char*)log_filename;
}

bool ProcessEvent(SDL_Event* event) {
    Sdl::ProcessEvent(event);
    switch (event->type) {
    // Don't block release/up events
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const auto& io = GetIO();
        return io.WantCaptureMouse && io.Ctx->NavWindow != nullptr &&
               (io.Ctx->NavWindow->Flags & ImGuiWindowFlags_NoNav) == 0;
    }
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_KEY_DOWN: {
        if (IsGamepadInputCaptured()) {
            // Keep keyboard events flowing through the regular input-binding path while IME/OSK
            // captures gamepad input, so keyboard equivalents of pad buttons still update the
            // virtual controller state.
            return false;
        }
        const auto& io = GetIO();
        return io.WantCaptureKeyboard && io.Ctx->NavWindow != nullptr;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
        if (IsGamepadInputCaptured()) {
            // Let controller events continue to input bindings so OSK shortcuts and virtual
            // controller state keep updating; game-side pad reads are blocked in libScePad.
            return false;
        }
        const auto& io = GetIO();
        return io.NavActive && io.Ctx->NavWindow != nullptr;
    }
    default:
        return false;
    }
}

bool HasLayers() {
    // Layer changes queued from other threads take effect here, between two frames.
    std::scoped_lock lock{change_layers_mutex};
    while (!change_layers.empty()) {
        const auto [to_be_added, layer] = change_layers.front();
        if (to_be_added) {
            layers.push_back(layer);
        } else {
            const auto [begin, end] = std::ranges::remove(layers, layer);
            layers.erase(begin, end);
        }
        change_layers.pop_front();
    }
    return !layers.empty();
}

void NewFrame(bool is_reusing_frame) {
    Sdl::NewFrame(is_reusing_frame);
    ImGui::NewFrame();
    SetKeyOwner(ImGuiKey_GamepadFaceUp, ImHashStr("shadps4/pad"));

    for (auto* layer : layers) {
        layer->Draw();
    }
}

void Render(const vk::CommandBuffer& cmdbuf, const vk::ImageView& image_view,
            const vk::Extent2D& extent) {
    ImGui::Render();
    ImDrawData* draw_data = GetDrawData();
    // Font atlas uploads are recorded ahead of the render pass that samples them.
    Vulkan::UpdateTextures(*draw_data, cmdbuf);
    if (draw_data->CmdListsCount == 0) {
        return;
    }

    vk::RenderingAttachmentInfo color_attachments[1]{
        {
            .imageView = image_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eLoad, // on top of the blitted frame
            .storeOp = vk::AttachmentStoreOp::eStore,
        },
    };
    vk::RenderingInfo render_info{};
    render_info.renderArea = vk::Rect2D{
        .offset = {0, 0},
        .extent = extent,
    };
    render_info.layerCount = 1;
    render_info.colorAttachmentCount = 1;
    render_info.pColorAttachments = color_attachments;
    cmdbuf.beginRendering(render_info);
    Vulkan::RenderDrawData(*draw_data, cmdbuf);
    cmdbuf.endRendering();
}

bool MustKeepDrawing() {
    return std::ranges::any_of(layers, [](Layer* layer) { return layer->ShouldKeepDrawing(); }) ||
           change_layers.size() > 1;
}

} // namespace Core

void Layer::AddLayer(Layer* layer) {
    std::scoped_lock lock{change_layers_mutex};
    change_layers.emplace_back(true, layer);
}

void Layer::RemoveLayer(Layer* layer) {
    std::scoped_lock lock{change_layers_mutex};
    change_layers.emplace_back(false, layer);
}

} // namespace ImGui
