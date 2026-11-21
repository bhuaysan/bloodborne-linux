// The Advanced page: settings that change how the game behaves rather than how it looks, in the
// form of Picture and sound (docs/design/launcher.md, "Advanced"). Changes are written to the
// config file when the page closes.
#include <imgui.h>

#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "common/config.hpp"
#include "launcher/advanced_settings.hpp"
#include "launcher/form.hpp"
#include "launcher/page.hpp"
#include "launcher/widgets.hpp"

namespace bb::launcher {

namespace {

// This page's own strings, in both languages.
struct Text {
    const char* title;
    const char* load_hold;
    const char* seconds;          // a hold in whole seconds
    const char* seconds_default;  // the game's hold
    const char* load_hold_help;  // a no-break space between a number and its unit
};
constexpr Text english{
    .title = "Advanced",
    .load_hold = "Loading after death",
    .seconds = "{} s",
    .seconds_default = "{} s (default)",
    .load_hold_help = "The shortest time the loading screen stays up after you die. The game "
                      "still waits until the area is ready, so 0\xC2\xA0s loads about 7\xC2\xA0s faster.",
};
constexpr Text german{
    .title = "Erweitert",
    .load_hold = "Laden nach dem Tod",
    .seconds = "{} s",
    .seconds_default = "{} s (Standard)",
    .load_hold_help = "Die k\xc3\xbcrzeste Zeit, die der Ladebildschirm nach dem Tod zu sehen "
                      "ist. Das Spiel wartet trotzdem, bis das Gebiet bereit ist; mit "
                      "0\xC2\xA0s l\xc3\xa4" "dt es etwa 7\xC2\xA0s schneller.",
};

const Text& text(Lang lang) {
    return lang == Lang::de ? german : english;
}

class AdvancedPage : public Page {
public:
    const char* title(const Context& ctx) const override { return text(ctx.lang).title; }

    Action draw(const Context& ctx, float height) override {
        const bool first_frame = !loaded_;
        load(ctx);
        const Text& t = text(ctx.lang);
        const float s = ctx.scale;
        const auto label = [&](int v) {
            return std::vformat(v == game_load_hold ? t.seconds_default : t.seconds,
                                std::make_format_args(v));
        };
        Form form("advanced", height, s, first_frame);
        form.row(t.load_hold, [&] {
            return line_slider("##hold", hold_, 0, game_load_hold, game_load_hold, 1,
                               value_column({label(game_load_hold)}, s), label, s);
        });
        const float y = ImGui::GetCursorPosY() + 12 * s;
        centred_text(t.load_hold_help, y, colour::dim, 432 * s, text_size * s);
        return Action::none;
    }

    void on_close(const Context& ctx) override {
        if (!loaded_ || hold_ == before_ || ctx.config_path.empty()) {
            return;
        }
        // Read again: the file is the only state, and the other keys stay.
        try {
            common::update_config(ctx.config_path, [&](common::Config& config) {
                apply_load_hold(config, before_, hold_);
            });
        } catch (const common::ConfigError&) {
            // An unreadable file is left as it is; the player sees the old setting next time.
        }
    }

private:
    void load(const Context& ctx) {
        if (loaded_) {
            return;
        }
        loaded_ = true;
        common::Config config;
        try {
            if (!ctx.config_path.empty() && std::filesystem::exists(ctx.config_path)) {
                config = common::read_config(ctx.config_path).config;
            }
        } catch (const common::ConfigError&) {
            config = {};
        }
        before_ = hold_ = load_hold_choice(config);
    }

    bool loaded_ = false;
    int before_ = game_load_hold;
    int hold_ = game_load_hold;
};

}  // namespace

std::unique_ptr<Page> make_advanced_page() {
    return std::make_unique<AdvancedPage>();
}

std::vector<const char*> advanced_strings(Lang lang) {
    const Text& t = text(lang);
    return {t.title, t.load_hold, t.seconds, t.seconds_default, t.load_hold_help};
}

}  // namespace bb::launcher
