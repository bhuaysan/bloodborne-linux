// shadPS4's SDL3 platform backend for ImGui (imgui/renderer/imgui_impl_sdl3.h), the part the
// presenter needs: the display size, the time step, and the keyboard and mouse for the IME dialog.
// No cursors, clipboard or gamepads. imgui_impl_sdl3.cpp here defines them.
#pragma once

struct SDL_Window;
union SDL_Event;

namespace ImGui::Sdl {

bool Init(SDL_Window* window);
void Shutdown();
void NewFrame(bool is_reusing_frame);
// Queues a keyboard, text, mouse or window focus event for the next NewFrame; returns whether it
// was one. Called on the thread that pumps the window's events, which is not the presenter's.
bool ProcessEvent(const SDL_Event* event);
// Drops what is queued and has the next NewFrame release every key and mouse button, so input
// from before the queue was fed never reaches ImGui.
void ClearInput();
void OnResize();

}  // namespace ImGui::Sdl
