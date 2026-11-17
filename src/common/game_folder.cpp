#include "common/game_folder.hpp"

#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/config.hpp"
#include "common/file.hpp"
#include "common/sha256.hpp"

namespace bb::common {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view expected_title_id = "CUSA03173";
constexpr unsigned required_version_major = 1;
constexpr unsigned required_version_minor = 9;  // patch 1.09

std::uint32_t le32(const std::vector<std::byte>& d, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, d.data() + at, 4);
    return v;  // x86-64 is little-endian, like the file
}

std::uint16_t le16(const std::vector<std::byte>& d, std::size_t at) {
    std::uint16_t v = 0;
    std::memcpy(&v, d.data() + at, 2);
    return v;
}

// The string values of a param.sfo (format 0x0004 and 0x0204 entries), by key. Nothing when the
// file isn't a well-formed PSF.
std::optional<std::vector<std::pair<std::string, std::string>>> read_sfo(
    const std::vector<std::byte>& d) {
    if (d.size() < 20 || std::memcmp(d.data(), "\0PSF", 4) != 0) {
        return std::nullopt;
    }
    const std::uint32_t key_table = le32(d, 8);
    const std::uint32_t data_table = le32(d, 12);
    const std::uint32_t count = le32(d, 16);
    if (count > 1024 || key_table > d.size() || data_table > d.size() ||
        20 + std::size_t{16} * count > d.size()) {
        return std::nullopt;
    }
    std::vector<std::pair<std::string, std::string>> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t e = 20 + std::size_t{16} * i;
        const std::size_t key_at = std::size_t{key_table} + le16(d, e);
        const std::uint16_t format = le16(d, e + 2);
        const std::size_t len = le32(d, e + 4);
        const std::size_t value_at = std::size_t{data_table} + le32(d, e + 12);
        if (key_at >= d.size() || value_at > d.size() || len > d.size() - value_at) {
            return std::nullopt;
        }
        if (format != 0x0204 && format != 0x0004) {
            continue;  // integers and blobs aren't needed
        }
        std::string key;
        for (std::size_t k = key_at; k < d.size() && d[k] != std::byte{0}; ++k) {
            key += static_cast<char>(d[k]);
        }
        std::string value;
        for (std::size_t v = value_at; v < value_at + len && d[v] != std::byte{0}; ++v) {
            value += static_cast<char>(d[v]);
        }
        out.emplace_back(std::move(key), std::move(value));
    }
    return out;
}

// "01.09" as major and minor.
bool parse_version(const std::string& text, unsigned& major, unsigned& minor) {
    const std::size_t dot = text.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= text.size()) {
        return false;
    }
    unsigned* parts[2] = {&major, &minor};
    const std::string halves[2] = {text.substr(0, dot), text.substr(dot + 1)};
    for (int i = 0; i < 2; ++i) {
        if (halves[i].size() > 4) {
            return false;
        }
        unsigned v = 0;
        for (const char ch : halves[i]) {
            if (ch < '0' || ch > '9') {
                return false;
            }
            v = v * 10 + static_cast<unsigned>(ch - '0');
        }
        *parts[i] = v;
    }
    return true;
}

FolderResult fail(FolderCheck kind, std::string message) {
    FolderResult r;
    r.kind = kind;
    r.message = std::move(message);
    return r;
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// Why there is no eboot.bin, in words: the player often chose the parent or a subfolder.
FolderResult no_eboot(const fs::path& folder) {
    std::error_code ec;
    std::string hint;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && is_file(it->path() / "eboot.bin")) {
            hint = std::format(" The folder \"{}\" inside it has one: choose that folder instead.",
                               it->path().filename().string());
            break;
        }
    }
    if (hint.empty() && folder.has_parent_path() && is_file(folder.parent_path() / "eboot.bin")) {
        hint = " The folder above this one has one: you chose a folder inside the game folder, "
               "choose the one above instead.";
    }
    if (hint.empty()) {
        hint = " Choose the folder of your game dump that holds eboot.bin.";
    }
    return fail(FolderCheck::no_eboot,
                std::format("This folder has no eboot.bin.{}", hint));
}

}  // namespace

FolderResult check_game_folder(const fs::path& folder, std::string_view eboot_sha256) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        return fail(FolderCheck::not_a_folder,
                    std::format("\"{}\" is not a folder.", folder.string()));
    }
    const fs::path eboot = folder / "eboot.bin";
    if (!is_file(eboot)) {
        return no_eboot(folder);
    }
    const fs::path sfo_path = folder / "sce_sys" / "param.sfo";
    if (!is_file(sfo_path)) {
        return fail(FolderCheck::no_param_sfo,
                    "The folder has an eboot.bin but no sce_sys/param.sfo. The dump looks "
                    "incomplete: dump the game again, with its sce_sys folder.");
    }
    std::optional<std::vector<std::pair<std::string, std::string>>> sfo;
    try {
        sfo = read_sfo(read_file(sfo_path));
    } catch (const std::exception&) {
        sfo = std::nullopt;
    }
    const auto find = [&](std::string_view key) -> std::optional<std::string> {
        if (sfo) {
            for (const auto& [k, v] : *sfo) {
                if (k == key) {
                    return v;
                }
            }
        }
        return std::nullopt;
    };
    const std::optional<std::string> title_id = find("TITLE_ID");
    const std::optional<std::string> app_version = find("APP_VER");
    if (!title_id || !app_version) {
        return fail(FolderCheck::bad_param_sfo,
                    "sce_sys/param.sfo can't be read, or it names no game and version. The dump "
                    "looks damaged: dump the game again.");
    }
    FolderResult info;
    info.title_id = *title_id;
    info.app_version = *app_version;
    const auto failed = [&](FolderCheck kind, std::string message) {
        info.kind = kind;
        info.message = std::move(message);
        return info;
    };
    if (*title_id != expected_title_id) {
        return failed(FolderCheck::other_title,
                      std::format("This folder holds another game ({}). Choose the folder of "
                                  "Bloodborne ({}).", *title_id, expected_title_id));
    }
    unsigned major = 0;
    unsigned minor = 0;
    if (!parse_version(*app_version, major, minor)) {
        return failed(FolderCheck::bad_param_sfo,
                      std::format("sce_sys/param.sfo has a version that can't be read (\"{}\"). "
                                  "The dump looks damaged: dump the game again.", *app_version));
    }
    if (major < required_version_major ||
        (major == required_version_major && minor < required_version_minor)) {
        return failed(FolderCheck::missing_update,
                      std::format("This is Bloodborne version {}, without update 1.09. Apply "
                                  "update 1.09 to the dump, then choose the folder again.",
                                  *app_version));
    }
    std::string digest;
    try {
        digest = to_hex(sha256(read_file(eboot)));
    } catch (const std::exception& e) {
        return failed(FolderCheck::unknown_eboot,
                      std::format("eboot.bin can't be read: {}", e.what()));
    }
    if (digest != eboot_sha256) {
        return failed(FolderCheck::unknown_eboot,
                      "This eboot.bin is not the one of Bloodborne 1.09 that the game runs on "
                      "(its checksum is different). Use an unmodified dump with update 1.09 "
                      "applied.");
    }
    info.message = std::format("Bloodborne version {} found.", *app_version);
    return info;
}

SetupResult setup_game_folder(const fs::path& folder, const fs::path& config,
                              std::string_view eboot_sha256) {
    SetupResult result;
    result.folder = check_game_folder(folder, eboot_sha256);
    if (!result.folder.ok()) {
        return result;
    }
    try {
        Config settings;
        std::error_code ec;
        if (fs::exists(config, ec)) {
            settings = read_config(config).config;
        }
        settings.game_dir = fs::canonical(folder);
        write_config(config, settings);
        result.config_written = true;
    } catch (const std::exception& e) {
        result.config_error = e.what();
    }
    return result;
}

}  // namespace bb::common
