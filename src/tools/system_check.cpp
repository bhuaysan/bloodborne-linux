#include "tools/system_check.hpp"

#include <dlfcn.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "hle/kernel/vmem.hpp"
#include "hle/video/video_core_presenter.hpp"
#include "loader/image.hpp"
#include "sdl_window.h"
#include "syslibs/media_check.hpp"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

namespace bb::tools {
namespace {

// The eboot of patch 1.09 spans about 87 MiB from the loader's base and its PRXs follow it; this
// much is checked.
constexpr std::uintptr_t image_area_size = std::uintptr_t{256} << 20;

// The window and the device of the check, never destroyed: the process ends after it, and the
// instance's destructor expects ImGui set up. Held here, so LeakSanitizer sees them reachable.
Frontend::WindowSDL* checked_window = nullptr;
Vulkan::Instance* checked_instance = nullptr;

void report(std::string_view part, bool ok, std::string_view what) {
    std::printf("%.*s: %s, %.*s\n", static_cast<int>(part.size()), part.data(),
                ok ? "ok" : "failed", static_cast<int>(what.size()), what.data());
    std::fflush(stdout);
}

std::string join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        out += (out.empty() ? "" : ", ") + name;
    }
    return out;
}

// Reserves [start, end) as the loader and the kernel do and gives it back. Empty, or why not.
std::string reserve(std::uintptr_t start, std::uintptr_t end) {
    void* const want = reinterpret_cast<void*>(start);
    const std::size_t size = end - start;
    void* const got = ::mmap(want, size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1,
                             0);
    if (got == MAP_FAILED) {
        return std::format("{:#x}-{:#x}: {}", start, end, std::strerror(errno));
    }
    ::munmap(got, size);
    if (got != want) {  // a kernel before 4.17 takes MAP_FIXED_NOREPLACE as a hint
        return std::format("{:#x}-{:#x}: in use", start, end);
    }
    return {};
}

bool check_guest_memory() {
    const std::uintptr_t image_start = loader::default_base;
    const std::uintptr_t image_end = image_start + image_area_size;
    for (const auto& [start, end] : {std::pair{image_start, image_end},
                                     std::pair{hle::kernel::vmem::window_start,
                                               hle::kernel::vmem::window_end}}) {
        if (const std::string why = reserve(start, end); !why.empty()) {
            report("guest memory", false, why);
            return false;
        }
    }
    report("guest memory", true,
           std::format("{:#x}-{:#x} and {:#x}-{:#x} free", image_start, image_end,
                       hle::kernel::vmem::window_start, hle::kernel::vmem::window_end));
    return true;
}

bool check_ffmpeg() {
    const shadps4::MediaSupport media = shadps4::media_support();
    if (!media.missing.empty()) {
        report("ffmpeg", false, "missing " + join(media.missing));
        return false;
    }
    report("ffmpeg", true, join(media.found));
    return true;
}

// Opens the window the game would, or with --headless SDL's video on its offscreen driver; the
// Vulkan check then creates its instance for that window's system.
Frontend::WindowSDL* check_video(const RunOptions& options) {
    if (options.headless) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            report("video", false, std::format("SDL offscreen driver: {}", SDL_GetError()));
            return nullptr;
        }
        report("video", true, "SDL offscreen driver (--headless)");
        return new Frontend::WindowSDL;  // no window: the headless instance
    }
    if (std::getenv("WAYLAND_DISPLAY") == nullptr && std::getenv("DISPLAY") == nullptr) {
        report("video", false, "neither WAYLAND_DISPLAY nor DISPLAY is set (--headless checks "
                               "without a display)");
        return nullptr;
    }
    std::string why;
    std::string display_note;
    std::unique_ptr<Frontend::WindowSDL> window = Frontend::WindowSDL::Open(
        "Bloodborne", static_cast<std::int32_t>(options.window_width),
        static_cast<std::int32_t>(options.window_height), false, options.display, display_note,
        why);
    if (!window) {
        report("video", false, why);
        return nullptr;
    }
    report("video", true,
           std::format("SDL {} driver, display \"{}\"{}", SDL_GetCurrentVideoDriver(),
                       window->DisplayName(), display_note.empty() ? "" : ", " + display_note));
    return window.release();
}

bool check_audio(const RunOptions& options) {
    if (options.headless) {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        report("audio", false, std::format("SDL: {}", SDL_GetError()));
        return false;
    }
    // The format AudioOut opens its streams with does not matter here: SDL converts.
    const SDL_AudioDeviceID device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (device == 0) {
        report("audio", false,
               std::format("SDL {} driver, default playback device: {}",
                           SDL_GetCurrentAudioDriver(), SDL_GetError()));
        return false;
    }
    const char* const name = SDL_GetAudioDeviceName(device);
    report("audio", true,
           std::format("SDL {} driver, device \"{}\"{}", SDL_GetCurrentAudioDriver(),
                       name != nullptr ? name : "", options.headless ? " (--headless)" : ""));
    SDL_CloseAudioDevice(device);
    return true;
}

// What video_core asserts on before it creates the device, checked through the loader first so
// that a missing loader, driver or device fails with a line of its own. Empty, or why not.
std::string probe_vulkan() {
    // The loader stays mapped; video_core opens it again.
    void* const lib = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        return "the Vulkan loader libvulkan.so.1 is not installed";
    }
    const auto get =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(::dlsym(lib, "vkGetInstanceProcAddr"));
    if (get == nullptr) {
        return "libvulkan.so.1 has no vkGetInstanceProcAddr";
    }
    const auto version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        get(nullptr, "vkEnumerateInstanceVersion"));
    std::uint32_t api = VK_API_VERSION_1_0;
    if (version != nullptr && version(&api) != VK_SUCCESS) {
        return "vkEnumerateInstanceVersion failed";
    }
    if (api < Vulkan::TargetVulkanApiVersion) {
        return std::format("the Vulkan loader supports {}.{}, the renderer needs {}.{}",
                           VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
                           VK_API_VERSION_MAJOR(Vulkan::TargetVulkanApiVersion),
                           VK_API_VERSION_MINOR(Vulkan::TargetVulkanApiVersion));
    }
    const auto create =
        reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = Vulkan::TargetVulkanApiVersion;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (const VkResult r = create != nullptr ? create(&info, nullptr, &instance)
                                             : VK_ERROR_INITIALIZATION_FAILED;
        r != VK_SUCCESS) {
        return std::format("vkCreateInstance: error {} (no Vulkan driver installed?)",
                           static_cast<int>(r));
    }
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        get(instance, "vkEnumeratePhysicalDevices"));
    const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        get(instance, "vkGetPhysicalDeviceProperties"));
    const auto destroy =
        reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
    if (enumerate == nullptr || properties == nullptr || destroy == nullptr) {
        return "the Vulkan loader lacks vkEnumeratePhysicalDevices, "
               "vkGetPhysicalDeviceProperties or vkDestroyInstance";
    }
    std::uint32_t count = 0;
    std::vector<VkPhysicalDevice> devices;
    if (enumerate(instance, &count, nullptr) == VK_SUCCESS) {
        devices.resize(count);
        if (enumerate(instance, &count, devices.data()) < 0) {
            devices.clear();
        }
        devices.resize(std::min<std::size_t>(count, devices.size()));
    }
    std::string why;
    bool usable = false;
    std::string names;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties p{};
        properties(device, &p);
        usable = usable || p.apiVersion >= Vulkan::TargetVulkanApiVersion;
        names += std::format("{}{} (Vulkan {}.{})", names.empty() ? "" : ", ", p.deviceName,
                             VK_API_VERSION_MAJOR(p.apiVersion),
                             VK_API_VERSION_MINOR(p.apiVersion));
    }
    if (devices.empty()) {
        why = "no Vulkan device";
    } else if (!usable) {
        why = std::format("no device with Vulkan {}.{}: {}",
                          VK_API_VERSION_MAJOR(Vulkan::TargetVulkanApiVersion),
                          VK_API_VERSION_MINOR(Vulkan::TargetVulkanApiVersion), names);
    }
    destroy(instance, nullptr);
    return why;
}

bool check_vulkan(const RunOptions& options, Frontend::WindowSDL& window) {
    if (const std::string why = probe_vulkan(); !why.empty()) {
        report("vulkan", false, why);
        return false;
    }
    const Vulkan::ValidationOptions validation{options.validation, options.validation_sync,
                                               options.validation_gpu};
    std::string why;
    const std::optional<std::int32_t> gpu_id = hle::video::resolve_gpu_id(
        options.device, window.GetWindowInfo().type, why, validation);
    if (!gpu_id) {
        report("vulkan", false, std::format("--device: {}", why));
        return false;
    }
    // As the presenter creates it: the device with every feature and extension the renderer
    // asks for.
    checked_instance = new Vulkan::Instance(window, *gpu_id, validation);
    report("vulkan", true,
           std::format("{}, driver {} {}", checked_instance->GetModelName(),
                       vk::to_string(checked_instance->GetDriverID()),
                       checked_instance->GetDriverVersionName()));
    return true;
}

}  // namespace

int check_system(const RunOptions& options) {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!check_guest_memory() || !check_ffmpeg()) {
        return 1;
    }
    checked_window = check_video(options);
    if (checked_window == nullptr || !check_audio(options) ||
        !check_vulkan(options, *checked_window)) {
        return 1;
    }
    return 0;
}

}  // namespace bb::tools
