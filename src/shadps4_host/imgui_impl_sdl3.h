// shadPS4's SDL3 platform backend for ImGui, left out for now. Its NewFrame and OnResize set the
// display size the presenter docks the game frame in, so they fail loudly until #432 ports them.
#pragma once

#include "shadps4_host/failure.hpp"

struct SDL_Window;
union SDL_Event;

namespace ImGui::Sdl {

inline bool Init(SDL_Window*) { return true; }
inline void Shutdown() {}
inline void NewFrame(bool) { bb::shadps4::host_missing("ImGui::Sdl::NewFrame (#432)"); }
inline bool ProcessEvent(const SDL_Event*) { return false; }
inline void OnResize() { bb::shadps4::host_missing("ImGui::Sdl::OnResize (#432)"); }

}  // namespace ImGui::Sdl
