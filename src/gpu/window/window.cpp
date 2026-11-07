#include "gpu/window/window.hpp"

#include <stdexcept>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace bb::window {

std::unique_ptr<Window> Window::create(const char* title, std::uint32_t width,
                                       std::uint32_t height, std::string& why) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        why = SDL_GetError();
        return nullptr;
    }
    SDL_Window* window =
        SDL_CreateWindow(title, static_cast<int>(width), static_cast<int>(height),
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        why = SDL_GetError();
        SDL_Quit();
        return nullptr;
    }
    why.clear();
    return std::unique_ptr<Window>(new Window(window));
}

Window::~Window() {
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

std::span<const char* const> Window::instance_extensions() const {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    return {names, names == nullptr ? 0 : count};
}

VkSurfaceKHR Window::create_surface(VkInstance instance) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
    }
    return surface;
}

VkExtent2D Window::pixel_size() const {
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

bool Window::pump(std::int32_t timeout_ms, const std::function<void(VkExtent2D)>& resized) {
    bool open = true;
    SDL_Event event;
    for (bool got = SDL_WaitEventTimeout(&event, timeout_ms); got; got = SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            open = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            resized({static_cast<std::uint32_t>(event.window.data1),
                     static_cast<std::uint32_t>(event.window.data2)});
            break;
        default:
            break;
        }
    }
    return open;
}

}  // namespace bb::window
