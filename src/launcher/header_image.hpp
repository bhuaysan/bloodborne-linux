#pragma once

#include <filesystem>

#include <SDL3/SDL.h>

namespace bb::launcher {

// Loads a PNG, crops it around its centre to the aspect ratio target_w : target_h and returns it
// as a texture. Null when the file is missing or not a PNG.
SDL_Texture* load_header_texture(SDL_Renderer* renderer, const std::filesystem::path& png,
                                 int target_w, int target_h);

}  // namespace bb::launcher
