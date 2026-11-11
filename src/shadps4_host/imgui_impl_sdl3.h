// shadPS4's SDL3 platform backend for ImGui (imgui/renderer/imgui_impl_sdl3.h), the part the
// presenter needs: the display size and the time step. No input, cursors or gamepads.
// imgui_impl_sdl3.cpp here defines them.
#pragma once

struct SDL_Window;
union SDL_Event;

namespace ImGui::Sdl {

bool Init(SDL_Window* window);
void Shutdown();
void NewFrame(bool is_reusing_frame);
inline bool ProcessEvent(const SDL_Event*) { return false; }
void OnResize();

}  // namespace ImGui::Sdl
