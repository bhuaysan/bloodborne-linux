// The window's parts without a window: ImGui runs without a backend, the header picture is
// loaded into a texture of SDL's software renderer on the dummy video driver.
#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <imgui.h>

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
    SDL_DestroyTexture(t);
}

TEST_F(Sdl, HeaderImageIsNullForMissingOrBrokenFiles) {
    EXPECT_EQ(load_header_texture(renderer_, "/nonexistent/pic1.png", 20, 10), nullptr);
    const fs::path bad = fs::temp_directory_path() / ("bb-ui-bad-" + std::to_string(getpid()));
    std::ofstream(bad) << "not a png";
    EXPECT_EQ(load_header_texture(renderer_, bad, 20, 10), nullptr);
    fs::remove(bad);
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
        for (MainState s : {MainState::no_folder, MainState::check_failed, MainState::ready,
                            MainState::no_controller, MainState::no_gpu, MainState::last_run_failed}) {
            MainView view;
            view.state = s;
            view.status = "status";
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("w");
            EXPECT_EQ(draw_main_screen(ctx, view), Action::none);
            ImGui::End();
            ImGui::Render();
            EXPECT_GT(ImGui::GetDrawData()->CmdListsCount, 0);
        }
        for (auto make : {make_picture_page, make_saves_page, make_help_page}) {
            auto page = make();
            EXPECT_NE(std::string(page->title(ctx)), "");
        }
    }
    ImGui::DestroyContext();
}

TEST(LauncherUi, FontsLoad) {
    ImGui::CreateContext();
    (void)load_fonts(16.0f);
    ImGui::DestroyContext();
}

}  // namespace
