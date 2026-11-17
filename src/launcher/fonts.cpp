#include "launcher/fonts.hpp"

#include <fontconfig/fontconfig.h>

#include <cstring>
#include <string>

#include <imgui.h>

namespace bb::launcher {

namespace {

// The file of the best "sans-serif" match, when it is a TrueType or CFF font; empty otherwise.
std::string system_sans_file() {
    std::string file;
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>("sans-serif"));
    if (pattern != nullptr) {
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result;
        FcPattern* match = FcFontMatch(nullptr, pattern, &result);
        if (match != nullptr) {
            FcChar8* path = nullptr;
            FcChar8* format = nullptr;
            if (FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch &&
                FcPatternGetString(match, FC_FONTFORMAT, 0, &format) == FcResultMatch &&
                (std::strcmp(reinterpret_cast<char*>(format), "TrueType") == 0 ||
                 std::strcmp(reinterpret_cast<char*>(format), "CFF") == 0)) {
                file = reinterpret_cast<char*>(path);
            }
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pattern);
    }
    return file;
}

}  // namespace

bool load_fonts(float size) {
    ImGuiIO& io = ImGui::GetIO();
    const std::string file = system_sans_file();
    if (!file.empty() && io.Fonts->AddFontFromFileTTF(file.c_str(), size) != nullptr) {
        return true;
    }
    ImFontConfig config;
    config.SizePixels = size;
    io.Fonts->AddFontDefault(&config);
    return false;
}

}  // namespace bb::launcher
