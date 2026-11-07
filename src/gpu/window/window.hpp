// The game window: an SDL3 window with a Vulkan surface (docs/design/graphics.md, "Command
// buffers to the first frame", item 22). Built only when SDL3 is found.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include <vulkan/vulkan.h>

struct SDL_Window;

namespace bb::window {

// One window per process. The thread that creates it handles its events (pump).
class Window {
public:
    // Initialises SDL's video and opens a resizable window of `width` x `height`. Nothing when
    // SDL can't (no display, no Vulkan); `why` then gives SDL's error.
    static std::unique_ptr<Window> create(const char* title, std::uint32_t width,
                                          std::uint32_t height, std::string& why);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // The instance extensions that create_surface needs.
    std::span<const char* const> instance_extensions() const;
    // A surface for the window in `instance`, made with instance_extensions(). The caller
    // destroys it. Throws std::runtime_error when SDL can't make one.
    VkSurfaceKHR create_surface(VkInstance instance);
    // The size of the window in pixels.
    VkExtent2D pixel_size() const;

    // Handles the pending events, after waiting up to `timeout_ms` for the first. Calls `resized`
    // with the new size in pixels when the window changed size. Returns false once the window is
    // to close.
    bool pump(std::int32_t timeout_ms, const std::function<void(VkExtent2D)>& resized);

private:
    explicit Window(SDL_Window* window) : window_(window) {}
    SDL_Window* window_;
};

}  // namespace bb::window
