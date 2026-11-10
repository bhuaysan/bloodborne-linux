// shadPS4's SDL3 platform backend for ImGui (input, cursor, display size), left out for now: the
// presenter only needs ImGui to put the frame on the swapchain, which the Vulkan backend does.
// Input to ImGui windows comes with the window work in R5 if it is needed at all.
#pragma once

struct SDL_Window;
union SDL_Event;

namespace ImGui::Sdl {

inline bool Init(SDL_Window*) { return true; }
inline void Shutdown() {}
inline void NewFrame(bool) {}
inline bool ProcessEvent(const SDL_Event*) { return false; }
inline void OnResize() {}

}  // namespace ImGui::Sdl
