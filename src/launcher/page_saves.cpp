// The Saves page: the profiles bb-run knows, each with the time it was last played. Choose one
// (written to the config file at once), make a new one, rename one, open its folder. No delete.
#include <SDL3/SDL_misc.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ctime>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "common/config.hpp"
#include "common/runtime_dirs.hpp"
#include "launcher/page.hpp"
#include "launcher/profiles.hpp"
#include "launcher/widgets.hpp"

namespace bb::launcher {

namespace {

namespace fs = std::filesystem;

// This page's own strings, in both languages.
struct Text {
    const char* title;
    const char* in_use;
    const char* never_played;
    const char* last_played;  // before the time
    const char* use;
    const char* open_folder;
    const char* make_new;
    const char* rename;
    const char* name_label;
    const char* ok;
    const char* cancel;
    const char* choose_first;
    const char* no_dirs;
    const char* opened;       // before the folder
    const char* open_failed;
    const char* chosen;       // before the name
    const char* created;
    const char* renamed;
    const char* empty;
    const char* too_long;
    const char* bad_character;
    const char* exists;
    const char* config_error;
    const char* failed;
};
constexpr Text english{"Saves",
                       "in use",
                       "not played yet",
                       "last played",
                       "Use this one",
                       "Open folder",
                       "New",
                       "Rename",
                       "Name:",
                       "OK",
                       "Cancel",
                       "Choose a save first.",
                       "No place for save data: the game's folder can't be used.",
                       "Opening",
                       "The file manager could not be opened.",
                       "Playing with",
                       "Made",
                       "Renamed to",
                       "A name needs at least one letter.",
                       "That name is too long. Use at most 32 characters.",
                       "Use only letters without accents, digits, - and _ (no spaces).",
                       "A save with that name already exists.",
                       "The config file could not be used",
                       "That did not work"};
constexpr Text german{"Spielst\xc3\xa4nde",
                      "in Benutzung",
                      "noch nicht gespielt",
                      "zuletzt gespielt",
                      "Diesen nehmen",
                      "Ordner \xc3\xb6\x66\x66nen",
                      "Neu",
                      "Umbenennen",
                      "Name:",
                      "OK",
                      "Abbrechen",
                      "W\xc3\xa4hle zuerst einen Spielstand.",
                      "Kein Ort f\xc3\xbcr Spielst\xc3\xa4nde: der Ordner des Spiels ist nicht nutzbar.",
                      "\xc3\x96\x66\x66nen:",
                      "Der Dateimanager konnte nicht ge\xc3\xb6\x66\x66net werden.",
                      "Gespielt wird mit",
                      "Angelegt:",
                      "Umbenannt in",
                      "Ein Name braucht mindestens ein Zeichen.",
                      "Der Name ist zu lang. H\xc3\xb6\x63hstens 32 Zeichen.",
                      "Nur Buchstaben ohne Umlaute, Ziffern, - und _ (keine Leerzeichen).",
                      "Einen Spielstand mit diesem Namen gibt es schon.",
                      "Die Konfigurationsdatei ist nicht nutzbar",
                      "Das hat nicht geklappt"};

const Text& text(const Context& ctx) {
    return ctx.lang == Lang::de ? german : english;
}

const char* problem_text(const Text& t, NameProblem p) {
    switch (p) {
    case NameProblem::empty: return t.empty;
    case NameProblem::too_long: return t.too_long;
    case NameProblem::bad_character: return t.bad_character;
    case NameProblem::exists: return t.exists;
    case NameProblem::ok: break;
    }
    return "";
}

std::string format_time(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

class SavesPage : public Page {
public:
    const char* title(const Context& ctx) const override { return text(ctx).title; }

    Action draw(const Context& ctx, float height) override {
        const Text& t = text(ctx);
        const float s = ctx.scale;
        if (!loaded_) {
            load(ctx);
        }
        if (data_dir_.empty()) {
            ImGui::TextWrapped("%s", message_.empty() ? t.no_dirs : message_.c_str());
            return Action::none;
        }
        const float side = 24 * s;  // the app's margin; the page is as wide as the Back button
        const float width = 432 * s;
        const float button_h = 40 * s;
        const float line = ImGui::GetFrameHeightWithSpacing();
        // The list takes what is left after the two button rows, the editor and the message.
        const float below = 2 * (button_h + 4 * s) + (editing_ == Edit::none ? 0 : line) + 2 * line;
        const float list_h = std::max(line * 2, height - below);
        ImGui::SetCursorPosX(side);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * s, 8 * s));
        const bool list_open = ImGui::BeginChild(
            "profiles", ImVec2(width, list_h),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        if (list_open) {
            for (std::size_t i = 0; i < profiles_.size(); ++i) {
                const ProfileInfo& p = profiles_[i];
                std::string label = p.name;
                label += p.name == current_ ? std::string("  [") + t.in_use + "]" : "";
                label += "   ";
                label += p.last_played
                             ? std::string(t.last_played) + " " + format_time(*p.last_played)
                             : t.never_played;
                if (ImGui::Selectable((label + "##" + p.name).c_str(), p.name == selected_)) {
                    selected_ = p.name;
                    editing_ = Edit::none;
                }
            }
        }
        ImGui::EndChild();

        const ImVec2 size((width - 8 * s) / 2, button_h);
        ImGui::SetCursorPosX(side);
        if (menu_entry(t.use, size, text_size * s)) {
            choose(ctx);
        }
        ImGui::SameLine(0, 8 * s);
        if (menu_entry(t.open_folder, size, text_size * s)) {
            open_folder(ctx);
        }
        ImGui::SetCursorPosX(side);
        if (menu_entry(t.make_new, size, text_size * s)) {
            start_edit(Edit::make, "");
        }
        ImGui::SameLine(0, 8 * s);
        if (menu_entry(t.rename, size, text_size * s)) {
            if (selected_.empty()) {
                message_ = t.choose_first;
            } else {
                start_edit(Edit::rename, selected_);
            }
        }
        if (editing_ != Edit::none) {
            ImGui::SetCursorPosX(side);
            draw_editor(ctx);
        }
        if (!message_.empty()) {
            ImGui::SetCursorPosX(side);
            ImGui::PushTextWrapPos(side + width);
            ImGui::TextWrapped("%s", message_.c_str());
            ImGui::PopTextWrapPos();
        }
        return Action::none;
    }

private:
    enum class Edit { none, make, rename };

    void load(const Context& ctx) {
        loaded_ = true;
        config_path_ = ctx.config_path;
        current_ = std::string(common::default_profile);
        try {
            if (std::error_code ec; fs::exists(ctx.config_path, ec)) {
                current_ = common::read_config(ctx.config_path).config.profile.value_or(current_);
            }
            const auto dirs = common::RuntimeDirs::from_environment(current_, {});
            data_dir_ = dirs.data();
            cache_dir_ = dirs.cache();
        } catch (const std::exception& e) {
            message_ = std::string(text(ctx).config_error) + ": " + e.what();
        }
        refresh();
    }

    void refresh() {
        profiles_ = data_dir_.empty() ? std::vector<ProfileInfo>{}
                                      : list_profiles(data_dir_, current_);
        if (selected_.empty()) {
            selected_ = current_;
        }
    }

    void start_edit(Edit what, const std::string& initial) {
        editing_ = what;
        message_.clear();
        std::snprintf(buffer_, sizeof buffer_, "%s", initial.c_str());
        focus_input_ = true;
    }

    void draw_editor(const Context& ctx) {
        const Text& t = text(ctx);
        ImGui::TextUnformatted(t.name_label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
        if (focus_input_) {
            ImGui::SetKeyboardFocusHere();
            focus_input_ = false;
        }
        const bool enter = ImGui::InputText("##name", buffer_, sizeof buffer_,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button(t.ok) || enter) {
            apply_edit(ctx);
        }
        ImGui::SameLine();
        if (ImGui::Button(t.cancel)) {
            editing_ = Edit::none;
        }
    }

    void apply_edit(const Context& ctx) {
        const Text& t = text(ctx);
        const std::string name = buffer_;
        try {
            NameProblem p = NameProblem::ok;
            if (editing_ == Edit::make) {
                p = create_profile(data_dir_, name);
            } else {
                p = rename_profile(data_dir_, cache_dir_, selected_, name);
            }
            if (p != NameProblem::ok) {
                message_ = problem_text(t, p);  // the editor stays open
                return;
            }
            if (editing_ == Edit::rename && selected_ == current_) {
                save_profile(name);  // the profile in use keeps being the one in use
                current_ = name;
            }
            message_ = std::string(editing_ == Edit::make ? t.created : t.renamed) + " " + name;
            selected_ = name;
            editing_ = Edit::none;
        } catch (const std::exception& e) {
            message_ = std::string(t.failed) + ": " + e.what();
        }
        refresh();
    }

    void choose(const Context& ctx) {
        const Text& t = text(ctx);
        if (selected_.empty()) {
            message_ = t.choose_first;
            return;
        }
        try {
            save_profile(selected_);
            current_ = selected_;
            message_ = std::string(t.chosen) + " " + current_;
        } catch (const std::exception& e) {
            message_ = std::string(t.config_error) + ": " + e.what();
        }
        refresh();
    }

    // Writes `name` as the profile into the config file. The file is read again first: a drop
    // may have set the game folder while the page was open.
    void save_profile(const std::string& name) {
        common::update_config(config_path_, [&](common::Config& c) { c.profile = name; });
    }

    void open_folder(const Context& ctx) {
        const Text& t = text(ctx);
        if (selected_.empty()) {
            message_ = t.choose_first;
            return;
        }
        try {
            const fs::path folder = profile_folder(data_dir_, selected_);
            fs::create_directories(folder);  // a new profile has none until its first run
            const std::string url = file_url(folder);
            // Unattended checks: BB_LAUNCHER_URL_LOG names a file that gets the URL instead of
            // a file manager window.
            if (const char* log = std::getenv("BB_LAUNCHER_URL_LOG"); log != nullptr && *log) {
                std::ofstream(log, std::ios::app) << url << '\n';
                message_ = std::string(t.opened) + " " + folder.string();
            } else if (SDL_OpenURL(url.c_str())) {
                message_ = std::string(t.opened) + " " + folder.string();
            } else {
                message_ = t.open_failed;
            }
        } catch (const std::exception& e) {
            message_ = std::string(t.failed) + ": " + e.what();
        }
    }

    bool loaded_ = false;
    Edit editing_ = Edit::none;
    bool focus_input_ = false;
    char buffer_[64] = {};
    std::string current_;
    std::string selected_;
    std::string message_;
    fs::path data_dir_, cache_dir_, config_path_;
    std::vector<ProfileInfo> profiles_;
};

}  // namespace

std::unique_ptr<Page> make_saves_page() {
    return std::make_unique<SavesPage>();
}

}  // namespace bb::launcher
