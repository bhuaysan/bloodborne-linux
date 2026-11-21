#include "launcher/text.hpp"

#include <format>

namespace bb::launcher {

namespace {

constexpr Strings english{
    .play = "Play",
    .set_up = "Set up game",
    .settings = "Settings",
    .picture_sound = "Picture and sound",
    .advanced = "Advanced",
    .controller = "Controller",
    .saves = "Saves",
    .quit = "Quit",
    .back = "Back",
    .status_no_game = "No game found yet",
    .status_version = "Version",
    .no_controller_title = "No controller connected",
    .no_controller_text = "Plug one in before you play.",
    .play_anyway = "Play anyway",
    .cancel = "Cancel",
    .no_gpu_title = "No usable graphics card",
    .status_no_gpu = "Your graphics card can't run the game.",
    .status_last_run_failed = "The game stopped unexpectedly.",
    .run_failed_text = "Its output is in user/logs/launcher-last-run.log.",
    .notice_title = "Note",
    .ok = "OK",
    .drop_hint = "Drag your game folder here\n(the one with eboot.bin)",
    .set_up_first = "Set up the game first.",
    .dialog_title = "Choose the game folder",
};

constexpr Strings german{
    .play = "Spielen",
    .set_up = "Spiel einrichten",
    .settings = "Einstellungen",
    .picture_sound = "Bild und Ton",
    .advanced = "Erweitert",
    .controller = "Controller",
    .saves = "Spielst\xc3\xa4nde",
    .quit = "Beenden",
    .back = "Zur\xc3\xbc" "ck",
    .status_no_game = "Noch kein Spiel gefunden",
    .status_version = "Version",
    .no_controller_title = "Kein Controller verbunden",
    .no_controller_text = "Schlie\xc3\x9f" "e einen Controller an, bevor du spielst.",
    .play_anyway = "Trotzdem spielen",
    .cancel = "Abbrechen",
    .no_gpu_title = "Keine nutzbare Grafikkarte",
    .status_no_gpu = "Deine Grafikkarte kann das Spiel nicht ausf\xc3\xbc" "hren.",
    .status_last_run_failed = "Das Spiel wurde unerwartet beendet.",
    .run_failed_text = "Seine Ausgabe steht in user/logs/launcher-last-run.log.",
    .notice_title = "Hinweis",
    .ok = "OK",
    .drop_hint = "Ziehe den Spielordner hierher\n(den mit der eboot.bin)",
    .set_up_first = "Richte zuerst das Spiel ein.",
    .dialog_title = "Spielordner ausw\xc3\xa4hlen",
};

}  // namespace

const Strings& strings(Lang lang) {
    return lang == Lang::de ? german : english;
}

std::string check_message(Lang lang, const common::FolderResult& r) {
    using common::FolderCheck;
    if (lang == Lang::en) {
        return r.message;
    }
    switch (r.kind) {
    case FolderCheck::ok:
        return std::format("Bloodborne Version {} gefunden.", display_version(r.app_version));
    case FolderCheck::not_a_folder:
        return "Das ist kein Ordner.";
    case FolderCheck::no_eboot:
        return "In diesem Ordner liegt keine eboot.bin. W\xc3\xa4hle den Ordner, der sie "
               "direkt enth\xc3\xa4lt.";
    case FolderCheck::no_param_sfo:
        return "Es fehlt sce_sys/param.sfo. Der Dump ist unvollst\xc3\xa4ndig: Erstelle ihn neu.";
    case FolderCheck::bad_param_sfo:
        return "sce_sys/param.sfo ist nicht lesbar. Der Dump ist besch\xc3\xa4" "digt: "
               "Erstelle ihn neu.";
    case FolderCheck::other_title:
        return std::format("Dieser Ordner geh\xc3\xb6rt zu einem anderen Spiel ({}). W\xc3\xa4hle "
                           "den Ordner von Bloodborne.", r.title_id);
    case FolderCheck::missing_update:
        return std::format("Das ist Bloodborne {} ohne Update 1.09. Spiele das Update 1.09 in den "
                           "Dump ein und w\xc3\xa4hle den Ordner erneut.",
                           display_version(r.app_version));
    case FolderCheck::unknown_eboot:
        return "Diese eboot.bin ist nicht die von Bloodborne 1.09. Nimm einen unver\xc3\xa4nderten "
               "Dump mit Update 1.09.";
    }
    return r.message;
}

}  // namespace bb::launcher
