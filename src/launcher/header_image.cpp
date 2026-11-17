#include "launcher/header_image.hpp"

#include <stb_image.h>

#include <cstddef>
#include <exception>

#include "common/file.hpp"
#include "launcher/logic.hpp"

namespace bb::launcher {

SDL_Texture* load_header_texture(SDL_Renderer* renderer, const std::filesystem::path& png,
                                 int target_w, int target_h) {
    std::vector<std::byte> bytes;
    try {
        bytes = common::read_file(png);
    } catch (const std::exception&) {
        return nullptr;
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                            static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (pixels == nullptr) {
        return nullptr;
    }
    const CropRect c = crop_around_centre(w, h, target_w, target_h);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STATIC, c.w, c.h);
    if (texture != nullptr) {
        SDL_UpdateTexture(texture, nullptr,
                          pixels + (static_cast<std::size_t>(c.y) * w + c.x) * 4, w * 4);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    stbi_image_free(pixels);
    return texture;
}

}  // namespace bb::launcher
