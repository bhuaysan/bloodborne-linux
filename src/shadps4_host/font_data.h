// The compressed font that imgui_core.cpp adds next to its UI fonts. Upstream embeds ProggyVector
// from generated sources; the port takes the scalable default font built into Dear ImGui instead
// (ProggyForever, in imgui_draw.cpp), which comes in the same compressed TTF format.
#pragma once

const char* ImGui_GetDefaultCompressedFontDataProggyForever(int* out_size);

inline const char* const imgui_font_proggyvector_regular_compressed_data = [] {
    int size = 0;
    return ImGui_GetDefaultCompressedFontDataProggyForever(&size);
}();

inline const int imgui_font_proggyvector_regular_compressed_size = [] {
    int size = 0;
    ImGui_GetDefaultCompressedFontDataProggyForever(&size);
    return size;
}();
