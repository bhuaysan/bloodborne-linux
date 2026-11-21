#include "launcher/fonts.hpp"

#include <fontconfig/fontconfig.h>

#include <cstring>
#include <string>

#include <imgui.h>

namespace bb::launcher {

namespace {

// The file of the best match for `family`, when it is a TrueType or CFF font; empty otherwise.
// With `exact`, fontconfig's stand-in for a family that isn't installed doesn't count.
std::string font_file(const char* family, bool exact, int& index) {
    std::string file;
    index = 0;
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(family));
    if (pattern != nullptr) {
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result;
        FcPattern* match = FcFontMatch(nullptr, pattern, &result);
        if (match != nullptr) {
            FcChar8* path = nullptr;
            FcChar8* format = nullptr;
            FcChar8* found = nullptr;
            const bool family_ok =
                !exact || (FcPatternGetString(match, FC_FAMILY, 0, &found) == FcResultMatch &&
                           FcStrCmpIgnoreCase(found, reinterpret_cast<const FcChar8*>(family)) == 0);
            if (family_ok && FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch &&
                FcPatternGetString(match, FC_FONTFORMAT, 0, &format) == FcResultMatch &&
                (std::strcmp(reinterpret_cast<char*>(format), "TrueType") == 0 ||
                 std::strcmp(reinterpret_cast<char*>(format), "CFF") == 0)) {
                file = reinterpret_cast<char*>(path);
                int face = 0;
                if (FcPatternGetInteger(match, FC_INDEX, 0, &face) == FcResultMatch && face >= 0) {
                    index = face & 0xffff;  // the high bits address variable-font instances
                }
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
    int index = 0;
    // A Garamond is closest to the game's menus; any serif font does.
    std::string file = font_file("EB Garamond", true, index);
    if (file.empty()) {
        file = font_file("Cormorant Garamond", true, index);
    }
    if (file.empty()) {
        file = font_file("serif", false, index);
    }
    ImFontConfig file_config;
    file_config.FontNo = static_cast<ImU32>(index);
    file_config.Flags |= ImFontFlags_NoLoadError;
    // A broken file must give null and not an assert in assert-enabled builds.
    const bool assert_was = io.ConfigErrorRecoveryEnableAssert;
    io.ConfigErrorRecoveryEnableAssert = false;
    ImFont* loaded =
        file.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(file.c_str(), size, &file_config);
    io.ConfigErrorRecoveryEnableAssert = assert_was;
    if (loaded != nullptr) {
        return true;
    }
    ImFontConfig config;
    config.SizePixels = size;
    io.Fonts->AddFontDefault(&config);
    return false;
}

}  // namespace bb::launcher
