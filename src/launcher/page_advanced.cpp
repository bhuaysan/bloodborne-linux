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
    const char* show_fps;
    const char* off_on[2];
    const char* data_folder;
    const char* change;
    const char* back_to_default;
    const char* locked;  // BB_USER_DIR fixes the folder
    const char* read_only;  // the program's folder can't be written, so the choice can't change
};
constexpr Text english{
    .title = "Advanced",
    .load_hold = "Loading after death",
    .seconds = "{} s",
    .seconds_default = "{} s (default)",
    .load_hold_help = "The shortest time the loading screen stays up after you die. The game "
                      "still waits until the area is ready, so 0\xC2\xA0s loads about 7\xC2\xA0s faster.",
    .show_fps = "Frame rate counter",
    .off_on = {"Off", "On (F10 in the game)"},
    .data_folder = "Data folder",
    .change = "Change...",
    .back_to_default = "Back to default",
    .locked = "BB_USER_DIR sets the data folder; unset it to change the folder here.",
    .read_only = "The program's folder is read-only, so the data folder can't be changed here.",
};
constexpr Text german{
    .title = "Erweitert",
    .load_hold = "Laden nach dem Tod",
    .seconds = "{} s",
    .seconds_default = "{} s (Standard)",
    .load_hold_help = "Die k\xc3\xbcrzeste Zeit, die der Ladebildschirm nach dem Tod zu sehen "
                      "ist. Das Spiel wartet trotzdem, bis das Gebiet bereit ist; mit "
                      "0\xC2\xA0s l\xc3\xa4" "dt es etwa 7\xC2\xA0s schneller.",
    .show_fps = "Bildratenanzeige",
    .off_on = {"Aus", "An (F10 im Spiel)"},
    .data_folder = "Datenordner",
    .change = "\xc3\x84ndern...",
    .back_to_default = "Zur\xc3\xbc" "ck zum Standard",
    .locked = "BB_USER_DIR legt den Datenordner fest; hebe es auf, um ihn hier zu \xc3\xa4ndern.",
    .read_only = "Der Programmordner ist schreibgesch\xc3\xbctzt; der Datenordner l\xc3\xa4sst sich hier "
                 "nicht \xc3\xa4ndern.",
};

const Text& text(Lang lang) {
    return lang == Lang::de ? german : english;
}

// A button in a form row's control space; true when pressed. The focused button lights the row.
bool row_button(const char* label) {
    return ImGui::Button(label, ImVec2(ImGui::CalcItemWidth(), 0));
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
        int fps = show_fps_ ? 1 : 0;
        if (form.row(t.show_fps,
                     [&] { return dropdown("##fps", {t.off_on[0], t.off_on[1]}, fps); })) {
            show_fps_ = fps == 1;
        }
        Action action = Action::none;
        ImGui::BeginDisabled(ctx.data_locked || !ctx.data_changeable);
        if (form.row(t.data_folder, [&] { return row_button(t.change); })) {
            action = Action::change_data;
        }
        if (!ctx.data_is_default &&
            form.row("", [&] { return row_button(t.back_to_default); })) {
            action = Action::data_default;
        }
        ImGui::EndDisabled();
        float y = ImGui::GetCursorPosY() + 12 * s;
        y = centred_text(t.load_hold_help, y, colour::dim, 432 * s, text_size * s) + 8 * s;
        y = centred_text(ellipsize_left(ctx.data_folder.string(), 432 * s).c_str(), y, colour::gold,
                         432 * s, text_size * s) + 4 * s;
        const std::string extra = ctx.data_locked ? t.locked
                                  : !ctx.data_changeable ? t.read_only : ctx.data_status;
        if (!extra.empty()) {
            centred_text(extra.c_str(), y, ctx.data_locked || !ctx.data_changeable ? colour::dim : colour::error, 432 * s,
                         text_size * s);
        }
        return action;
    }

    void on_close(const Context& ctx) override {
        if (!loaded_ || (hold_ == before_ && show_fps_ == show_fps_before_) ||
            ctx.config_path.empty()) {
            return;
        }
        // Read again: the file is the only state, and the other keys stay.
        try {
            common::update_config(ctx.config_path, [&](common::Config& config) {
                apply_load_hold(config, before_, hold_);
                apply_show_fps(config, show_fps_before_, show_fps_);
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
        show_fps_before_ = show_fps_ = show_fps_choice(config);
    }

    bool loaded_ = false;
    int before_ = game_load_hold;
    int hold_ = game_load_hold;
    bool show_fps_before_ = false;
    bool show_fps_ = false;
};

}  // namespace

std::unique_ptr<Page> make_advanced_page() {
    return std::make_unique<AdvancedPage>();
}

std::vector<const char*> advanced_strings(Lang lang) {
    const Text& t = text(lang);
    return {t.title,     t.load_hold,   t.seconds,   t.seconds_default, t.load_hold_help,
            t.show_fps,  t.off_on[0],   t.off_on[1], t.data_folder,     t.change,
            t.back_to_default, t.locked, t.read_only};
}

std::string move_problem_text(Lang lang, MoveProblem problem) {
    const bool de = lang == Lang::de;
    const char* head = de ? "Der Datenordner wurde nicht verschoben. "
                          : "The data folder was not moved. ";
    switch (problem) {
    case MoveProblem::none: return {};
    case MoveProblem::not_absolute:
        return head + std::string(de ? "Das ist kein vollst\xc3\xa4ndiger Pfad."
                                     : "That is not a full path.");
    case MoveProblem::same_folder:
        return head + std::string(de ? "Das ist schon der Datenordner."
                                     : "That is the data folder already.");
    case MoveProblem::nested:
        return head + std::string(de ? "Der neue Ordner darf nicht im alten liegen und ihn "
                                       "nicht enthalten."
                                     : "The new folder can't lie inside the old one or hold it.");
    case MoveProblem::overlaps:
        return head + std::string(de ? "Der Ordner \xc3\xbc" "berschneidet sich mit dem Spiel oder dem "
                                       "Quellcode."
                                     : "That folder overlaps the game or the source code.");
    case MoveProblem::is_game:
    case MoveProblem::inside_game:
        return head + std::string(de ? "Dieser Ordner geh\xc3\xb6rt zum Spielordner."
                                     : "That folder belongs to the game's own folder.");
    case MoveProblem::not_empty:
        return head + std::string(de ? "Der neue Ordner muss leer sein."
                                     : "The new folder must be empty.");
    case MoveProblem::cannot_create:
        return head + std::string(de ? "Der Ordner l\xc3\xa4sst sich nicht anlegen oder beschreiben."
                                     : "The folder can't be created or written.");
    case MoveProblem::copy_failed:
    case MoveProblem::verify_failed:
        return head + std::string(de ? "Kopieren oder Pr\xc3\xbc" "fen schlug fehl; alles blieb beim "
                                       "Alten."
                                     : "Copying or checking failed; everything stays as it was.");
    case MoveProblem::config_failed:
        return head + std::string(de ? "Die Einstellungen lie\xc3\x9f" "en sich nicht \xc3\xbc" "bertragen."
                                     : "The settings could not be carried over.");
    case MoveProblem::no_place:
        return head + std::string(de ? "Es gibt keinen Standardordner oder keinen Ort, an dem sich "
                                       "die Wahl speichern l\xc3\xa4sst."
                                     : "There is no default folder or no place to keep the choice.");
    case MoveProblem::pointer_failed:
        return head + std::string(de ? "Die Wahl lie\xc3\x9f sich nicht speichern."
                                     : "The choice could not be saved.");
    }
    return {};
}

std::string move_leftovers_text(Lang lang, const std::filesystem::path& first) {
    return (lang == Lang::de ? "Verschoben, aber alte Dateien blieben zur\xc3\xbc" "ck: "
                             : "Moved, but old files remain: ") + first.string();
}

}  // namespace bb::launcher
