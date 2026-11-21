// The first-start screen of an AppImage: where the game keeps its settings, saves, caches and logs
// (docs/design/launcher.md, "One portable folder").
#include <algorithm>

#include <imgui.h>

#include "launcher/main_screen.hpp"
#include "launcher/page.hpp"
#include "launcher/widgets.hpp"

namespace bb::launcher {

namespace {

constexpr DataFolderText english{
    .title = "Where should the game keep its data?",
    .body = "Bloodborne keeps its settings, saves, caches and logs in one folder. Only its "
            "path is kept elsewhere, and deleting the folder removes everything.",
    .use_folder = "Use this folder",
    .choose = "Choose...",
    .dialog_title = "Choose the data folder",
    .no_proposal = "Choose a folder for the game's data.",
};

constexpr DataFolderText german{
    .title = "Wo soll das Spiel seine Daten ablegen?",
    .body = "Bloodborne legt Einstellungen, Spielst\xc3\xa4nde, Caches und Logs in einem Ordner "
            "ab. Nur sein Pfad wird woanders gespeichert, und wenn du den Ordner l\xc3\xb6scht, "
            "ist alles weg.",
    .use_folder = "Diesen Ordner verwenden",
    .choose = "Ausw\xc3\xa4hlen...",
    .dialog_title = "Datenordner ausw\xc3\xa4hlen",
    .no_proposal = "W\xc3\xa4hle einen Ordner f\xc3\xbcr die Daten des Spiels.",
};

}  // namespace

const DataFolderText& data_folder_text(Lang lang) {
    return lang == Lang::de ? german : english;
}

std::string data_folder_problem_text(Lang lang, DataFolderProblem problem) {
    const bool de = lang == Lang::de;
    switch (problem) {
    case DataFolderProblem::none: return {};
    case DataFolderProblem::not_absolute:
        return de ? "Der Ordner ist kein vollst\xc3\xa4ndiger Pfad." : "That is not a full path.";
    case DataFolderProblem::cannot_create:
        return de ? "Der Ordner l\xc3\xa4sst sich nicht anlegen. W\xc3\xa4hle einen anderen."
                  : "The folder can't be created. Choose another one.";
    case DataFolderProblem::not_writable:
        return de ? "In diesen Ordner kann nicht geschrieben werden. W\xc3\xa4hle einen anderen."
                  : "This folder can't be written. Choose another one.";
    case DataFolderProblem::overlaps:
        return de ? "Der Ordner \xc3\xbc" "berschneidet sich mit dem Spiel oder dem Quellcode. "
                    "W\xc3\xa4hle einen anderen."
                  : "This folder overlaps the game or the source code. Choose another one.";
    case DataFolderProblem::is_game:
        return de ? "Das ist der Spielordner (eboot.bin). W\xc3\xa4hle einen eigenen Ordner "
                    "f\xc3\xbcr die Daten."
                  : "This is the game's own folder (eboot.bin). Choose a folder of its own.";
    case DataFolderProblem::inside_game:
        return de ? "Dieser Ordner liegt im Spielordner. W\xc3\xa4hle einen Ordner au\xc3\x9f" "erhalb."
                  : "This folder lies inside the game's own folder. Choose one outside it.";
    case DataFolderProblem::pointer_failed:
        return de ? "Die Wahl konnte nicht gespeichert werden (~/.config/bloodborne-linux)."
                  : "The choice could not be saved (~/.config/bloodborne-linux).";
    }
    return {};
}

std::string game_in_data_folder_text(Lang lang) {
    return lang == Lang::de
               ? "Spielordner und Datenordner \xc3\xbc" "berschneiden sich. W\xc3\xa4hle einen Spielordner "
                 "au\xc3\x9f" "erhalb des Datenordners (oder lege ihn als game/ hinein)."
               : "The game folder and the data folder overlap. Choose a game folder outside the "
                 "data folder (or put it inside as game/).";
}

std::string data_folder_lost_text(Lang lang, common::NoDataFolder::Reason reason,
                                  const std::filesystem::path& folder) {
    const bool de = lang == Lang::de;
    switch (reason) {
    case common::NoDataFolder::Reason::not_chosen: return {};
    case common::NoDataFolder::Reason::pointer_unusable:
        return de ? "Die gespeicherte Wahl des Datenordners ist unbrauchbar. W\xc3\xa4hle ihn neu."
                  : "The saved choice of the data folder is unusable. Choose it again.";
    case common::NoDataFolder::Reason::folder_missing:
        return (de ? "Der Datenordner wurde nicht gefunden (ein nicht eingebundenes Laufwerk?): "
                   : "The data folder was not found (an unmounted drive?): ") +
               folder.string();
    case common::NoDataFolder::Reason::folder_unwritable:
        return (de ? "In den Datenordner kann nicht geschrieben werden: "
                   : "The data folder can't be written: ") +
               folder.string();
    }
    return {};
}

DataFolderChoice draw_data_folder_screen(const Context& ctx, const std::filesystem::path& proposed,
                                         const std::string& error, bool focus_first) {
    const float s = ctx.scale;
    const DataFolderText& t = data_folder_text(ctx.lang);
    constexpr float width = 432;
    rule(22, s);
    centred_text(t.title, 36 * s, colour::gold, width * s, 19 * s);
    rule(76, s);
    float y = centred_text(t.body, 100 * s, colour::text, width * s, text_size * s) + 18 * s;
    const std::string folder = proposed.empty() ? t.no_proposal : proposed.string();
    y = centred_text(ellipsize_left(folder, width * s).c_str(), y, colour::gold, width * s,
                     text_size * s) + 12 * s;
    if (!error.empty()) {
        y = centred_text(error.c_str(), y, colour::error, width * s, text_size * s) + 12 * s;
    }
    DataFolderChoice result = DataFolderChoice::none;
    y = std::max(y + 12 * s, 360 * s);
    if (!proposed.empty()) {
        ImGui::SetCursorPos(ImVec2((header_width - width) * 0.5f * s, y));
        if (menu_entry(t.use_folder, ImVec2(width * s, 54 * s), first_entry_size * s, focus_first)) {
            result = DataFolderChoice::use_proposed;
        }
        y += 54 * s;
    }
    ImGui::SetCursorPos(ImVec2((header_width - width) * 0.5f * s, y));
    if (menu_entry(t.choose, ImVec2(width * s, 46 * s), entry_size * s,
                   focus_first && proposed.empty())) {
        result = DataFolderChoice::choose_other;
    }
    return result;
}

}  // namespace bb::launcher
