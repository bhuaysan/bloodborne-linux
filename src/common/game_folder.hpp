// The setup check of the game folder (docs/design/launcher.md, "First start"): the folder of the
// player's own dump that holds eboot.bin, with update 1.09 applied. The launcher calls this
// directly and bb-run --setup calls it too. It only reads the folder.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace bb::common {

// SHA-256 of eboot.bin of CUSA03173 patch 1.09, as in docs/source-policy.md.
inline constexpr std::string_view default_eboot_sha256 =
    "d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9";

enum class FolderCheck {
    ok,
    not_a_folder,     // the path doesn't exist or isn't a directory
    no_eboot,         // no eboot.bin (often the parent or a subfolder was chosen)
    no_param_sfo,     // sce_sys/param.sfo is missing
    bad_param_sfo,    // sce_sys/param.sfo can't be read or has no title ID and version
    other_title,      // another game
    missing_update,   // the base game, or a patch older than 1.09
    unknown_eboot,    // eboot.bin is not the 1.09 build of docs/source-policy.md
};

struct FolderResult {
    FolderCheck kind = FolderCheck::ok;
    std::string message;      // plain words for the player: what is wrong, or "ok" text
    std::string title_id;     // from param.sfo, when it could be read
    std::string app_version;  // APP_VER from param.sfo, e.g. "01.09"

    bool ok() const { return kind == FolderCheck::ok; }
};

// Checks `folder`: its layout (eboot.bin, sce_sys/param.sfo), the title ID CUSA03173, the version
// from param.sfo (1.09 or newer) and the SHA-256 of eboot.bin. The first failure decides.
// `eboot_sha256` is the expected digest, in lower-case hex; the default is the pinned build of
// docs/source-policy.md. Tests on synthetic trees pass their own.
FolderResult check_game_folder(const std::filesystem::path& folder,
                               std::string_view eboot_sha256 = default_eboot_sha256);

// check_game_folder, then the folder (absolute, symlinks resolved) written as game_dir to the
// config file at `config`, keeping the other settings of an existing file. The file is not
// touched when the check fails. When the config file can't be read or written, the folder result
// is ok, `config_written` is false and `config_error` says why.
struct SetupResult {
    FolderResult folder;
    bool config_written = false;
    std::string config_error;  // empty unless the check passed and the config file failed

    bool ok() const { return folder.ok() && config_written; }
    // The message to show: the check's, or the config error.
    std::string message() const { return config_error.empty() ? folder.message : config_error; }
};
SetupResult setup_game_folder(const std::filesystem::path& folder,
                              const std::filesystem::path& config,
                              std::string_view eboot_sha256 = default_eboot_sha256);

}  // namespace bb::common
