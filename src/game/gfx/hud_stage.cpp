// The HUD's world markers in stage coordinates at any frame buffer size (#713).
#include "game/gfx/hud_stage.hpp"

#include <cstring>
#include <format>
#include <stdexcept>

namespace bb::game {

std::array<std::uint8_t, hud_size_load_bytes> game_size_load(const HudSizeLoad& load) {
    const auto rel = static_cast<std::int32_t>(static_cast<std::int64_t>(load.global) -
                                               static_cast<std::int64_t>(load.link + 7));
    std::array<std::uint8_t, hud_size_load_bytes> bytes = {
        0x48, 0x8d, std::uint8_t(load.rcx ? 0x0d : 0x05), 0, 0, 0,
        0,    0x8b, std::uint8_t(load.rcx ? 0x09 : 0x00)};
    std::memcpy(bytes.data() + 3, &rel, sizeof rel);
    return bytes;
}

std::array<std::uint8_t, hud_size_load_bytes> stage_size_load(const HudSizeLoad& load) {
    const std::uint32_t size =
        load.global == frame_buffer_width ? hud_stage_width : hud_stage_height;
    std::array<std::uint8_t, hud_size_load_bytes> bytes = {
        std::uint8_t(load.rcx ? 0xb9 : 0xb8), 0, 0, 0, 0, 0x0f, 0x1f, 0x40, 0x00};
    std::memcpy(bytes.data() + 1, &size, sizeof size);
    return bytes;
}

void set_hud_stage_loads(loader::Image& eboot, std::uint32_t width, std::uint32_t height) {
    const bool stage = width != hud_stage_width || height != hud_stage_height;
    for (const HudSizeLoad& load : hud_size_loads) {
        const auto* at = reinterpret_cast<const std::uint8_t*>(eboot.runtime(load.link));
        if (std::memcmp(at, game_size_load(load).data(), hud_size_load_bytes) != 0 &&
            std::memcmp(at, stage_size_load(load).data(), hud_size_load_bytes) != 0) {
            throw std::runtime_error(std::format(
                "set_hud_stage_loads: {:#010x} holds neither the game's load nor the stage's",
                load.link));
        }
    }
    for (const HudSizeLoad& load : hud_size_loads) {
        const auto bytes = stage ? stage_size_load(load) : game_size_load(load);
        write_image_bytes(eboot, load.link, bytes.data(), bytes.size());
    }
}

}  // namespace bb::game
