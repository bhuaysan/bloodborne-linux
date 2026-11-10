// shadPS4's UI font stack (Noto fonts per console language, generated at build time) replaced by
// Dear ImGui's built-in scalable font, at the requested size. The port only shows the game frame
// through ImGui, so the UI font is never visible in normal play.
#pragma once

#include <imgui.h>

namespace ImGui::FontStack {

inline ImFont* AddPrimaryUiFont(ImFontAtlas* atlas, float font_size, int /*console_language*/,
                                const ImFontConfig& base_cfg, bool /*include_cjk_fallback*/) {
    ImFontConfig config = base_cfg;
    config.SizePixels = font_size;
    return atlas->AddFontDefaultVector(&config);
}

}  // namespace ImGui::FontStack
