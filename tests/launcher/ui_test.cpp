// The window's parts without a window: ImGui runs without a backend, the header picture is
// loaded into a texture of SDL's software renderer on the dummy video driver.
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include <SDL3/SDL.h>
#include <fontconfig/fontconfig.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "launcher/fonts.hpp"
#include "launcher/header_image.hpp"
#include "launcher/main_screen.hpp"
#include "launcher/page.hpp"
#include "launcher/text.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::launcher;

std::uint32_t crc32(const std::vector<std::uint8_t>& d, std::size_t from) {
    std::uint32_t c = 0xffffffffu;
    for (std::size_t i = from; i < d.size(); ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) {
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
        }
    }
    return ~c;
}

void be32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) {
        o.push_back(static_cast<std::uint8_t>(v >> s));
    }
}

void chunk(std::vector<std::uint8_t>& o, const char* type, const std::vector<std::uint8_t>& body) {
    be32(o, static_cast<std::uint32_t>(body.size()));
    const std::size_t start = o.size();
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), body.begin(), body.end());
    be32(o, crc32(o, start));
}

// An RGB PNG of w x h with a stored (uncompressed) deflate stream.
void write_png(const fs::path& path, int w, int h) {
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            raw.insert(raw.end(), {static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), 7});
        }
    }
    std::vector<std::uint8_t> z{0x78, 0x01, 0x01};
    z.push_back(static_cast<std::uint8_t>(raw.size() & 0xff));
    z.push_back(static_cast<std::uint8_t>(raw.size() >> 8));
    z.push_back(static_cast<std::uint8_t>(~raw.size() & 0xff));
    z.push_back(static_cast<std::uint8_t>((~raw.size() >> 8) & 0xff));
    z.insert(z.end(), raw.begin(), raw.end());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);
    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

class Sdl : public ::testing::Test {
protected:
    void SetUp() override {
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
        surface_ = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
        renderer_ = SDL_CreateSoftwareRenderer(surface_);
        ASSERT_NE(renderer_, nullptr);
    }
    void TearDown() override {
        SDL_DestroyRenderer(renderer_);
        SDL_DestroySurface(surface_);
        SDL_Quit();
    }
    SDL_Surface* surface_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

TEST_F(Sdl, HeaderImageIsCroppedAroundTheCentre) {
    const fs::path png = fs::temp_directory_path() / ("bb-ui-test-" + std::to_string(getpid()));
    write_png(png, 40, 30);
    SDL_Texture* t = load_header_texture(renderer_, png, 20, 10);
    fs::remove(png);
    ASSERT_NE(t, nullptr);
    float w = 0;
    float h = 0;
    SDL_GetTextureSize(t, &w, &h);
    EXPECT_EQ(w, 40.0f);
    EXPECT_EQ(h, 20.0f);  // a 2:1 crop of 40 x 30
    // The pixels encode x in red and y in green: the crop starts at (0, 5).
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    const SDL_FRect dst{0, 0, 40, 20};
    SDL_RenderTexture(renderer_, t, nullptr, &dst);
    const SDL_Rect rect{0, 0, 40, 20};
    SDL_Surface* read = SDL_RenderReadPixels(renderer_, &rect);
    ASSERT_NE(read, nullptr);
    Uint8 r = 0, g = 0, b = 0, a = 0;
    SDL_ReadSurfacePixel(read, 0, 0, &r, &g, &b, &a);
    EXPECT_EQ(std::make_tuple(r, g, b), std::make_tuple(0, 5, 7));
    SDL_ReadSurfacePixel(read, 39, 19, &r, &g, &b, &a);
    EXPECT_EQ(std::make_tuple(r, g, b), std::make_tuple(39, 24, 7));
    SDL_DestroySurface(read);
    SDL_DestroyTexture(t);
}

TEST_F(Sdl, HeaderImageIsNullForMissingOrBrokenFiles) {
    EXPECT_EQ(load_header_texture(renderer_, "/nonexistent/pic1.png", 20, 10), nullptr);
    const fs::path bad = fs::temp_directory_path() / ("bb-ui-bad-" + std::to_string(getpid()));
    std::ofstream(bad) << "not a png";
    EXPECT_EQ(load_header_texture(renderer_, bad, 20, 10), nullptr);
    fs::remove(bad);
}

// The label of a state's first button.
const char* first_label(const Strings& t, Action a) {
    switch (a) {
    case Action::set_up: return t.set_up;
    case Action::play: return t.play;
    default: return "";
    }
}

// Whether fontconfig matches "serif" to a TrueType or CFF file, the fonts load_fonts uses.
bool system_serif_available() {
    bool found = false;
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>("serif"));
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    if (FcPattern* match = FcFontMatch(nullptr, pattern, &result)) {
        FcChar8* format = nullptr;
        FcChar8* file = nullptr;
        found = FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch &&
                FcPatternGetString(match, FC_FONTFORMAT, 0, &format) == FcResultMatch &&
                (std::strcmp(reinterpret_cast<char*>(format), "TrueType") == 0 ||
                 std::strcmp(reinterpret_cast<char*>(format), "CFF") == 0);
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pattern);
    return found;
}

TEST(LauncherUi, DrawsTheMainScreenForEveryStateWithoutPressing) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(480, 600);
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int fw = 0;
    int fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    for (Lang lang : {Lang::en, Lang::de}) {
        Context ctx;
        ctx.lang = lang;
        ctx.text = &strings(lang);
        for (MainState s : {MainState::no_folder, MainState::check_failed, MainState::ready}) {
            MainView view;
            view.state = s;
            view.status = "status";
            view.focus_first = true;
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("w");
            EXPECT_EQ(draw_main_screen(ctx, view), Action::none);
            // The focus is on the state's own first entry, in the context's language (a
            // menu_entry's ID is its label's, then "##b").
            ImGui::PushID(first_label(*ctx.text, first_button(s)));
            const ImGuiID first = ImGui::GetID("##b");
            ImGui::PopID();
            EXPECT_EQ(ImGui::GetFocusID(), first) << static_cast<int>(s);
            ImGui::End();
            ImGui::Render();
            EXPECT_GT(ImGui::GetDrawData()->CmdListsCount, 0);
        }
        for (auto make : {make_settings_page, make_game_folder_page, make_controller_page,
                          make_picture_page, make_advanced_page, make_saves_page}) {
            auto page = make();
            EXPECT_NE(std::string(page->title(ctx)), "");
        }
    }
    ImGui::DestroyContext();
}

TEST(LauncherUi, FontsLoad) {
    if (!system_serif_available()) {
        GTEST_SKIP() << "fontconfig finds no TrueType or CFF serif font on this machine";
    }
    ImGui::CreateContext();
    EXPECT_TRUE(load_fonts(16.0f));  // the system font, not the built-in fallback
    EXPECT_EQ(ImGui::GetIO().Fonts->Fonts.Size, 1);
    ImGui::DestroyContext();
}

// Draws one frame of the main screen and returns what it reports.
struct Frame {
    float below = 0;
    Action action = Action::none;
};

Frame frame(const Context& ctx, const MainView& view, bool top_only = false) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("w");
    Frame f;
    if (top_only) {
        f.below = draw_top(ctx, view);
    } else {
        f.action = draw_main_screen(ctx, view);
    }
    ImGui::End();
    ImGui::Render();
    return f;
}

TEST(LauncherUi, LongStatusPushesTheMenuDown) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(480, 600);
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int fw = 0;
    int fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    Context ctx;
    ctx.text = &strings(Lang::en);
    MainView view;
    view.status = "short";
    const float short_y = frame(ctx, view, true).below;
    EXPECT_EQ(short_y, 300 * ctx.scale);
    view.status = std::string(400, 'x') + " " + std::string(400, 'y');  // wraps to several lines
    const float long_y = frame(ctx, view, true).below;
    EXPECT_GT(long_y, short_y);
    // The menu starts below the text: at least three lines of it.
    EXPECT_GE(long_y, 266 + 3 * text_size);
    ImGui::DestroyContext();
}

TEST(LauncherUi, EnterPressesTheFirstButtonOfEachState) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(480, 600);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int fw = 0;
    int fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    Context ctx;
    ctx.text = &strings(Lang::en);
    for (MainState s : {MainState::no_folder, MainState::check_failed, MainState::ready}) {
        MainView view;
        view.state = s;
        view.status = "status";
        view.focus_first = true;
        frame(ctx, view);
        view.focus_first = false;
        frame(ctx, view);
        io.AddKeyEvent(ImGuiKey_Enter, true);
        Action got = frame(ctx, view).action;
        io.AddKeyEvent(ImGuiKey_Enter, false);
        if (got == Action::none) {
            got = frame(ctx, view).action;
        }
        EXPECT_EQ(got, first_button(s)) << static_cast<int>(s);
        frame(ctx, view);
    }
    ImGui::DestroyContext();
}

// Every string of the pages that keep their own tables is there in both languages, and German
// has its own words except where the two languages share them.
TEST(LauncherText, PagesHaveEveryStringInBothLanguages) {
    const std::string same[] = {"VSync", "{} s"};
    for (auto strings_of : {picture_strings, advanced_strings}) {
        const std::vector<const char*> en = strings_of(Lang::en);
        const std::vector<const char*> de = strings_of(Lang::de);
        ASSERT_EQ(en.size(), de.size());
        int differ = 0;
        for (size_t i = 0; i < en.size(); ++i) {
            ASSERT_NE(en[i], nullptr) << i;
            ASSERT_NE(de[i], nullptr) << i;
            EXPECT_STRNE(en[i], "") << i;
            EXPECT_STRNE(de[i], "") << i;
            if (std::find(std::begin(same), std::end(same), en[i]) == std::end(same)) {
                EXPECT_STRNE(de[i], en[i]) << i;
            }
            differ += std::strcmp(en[i], de[i]) != 0;
        }
        EXPECT_GT(differ, 0);
    }
}

}  // namespace
