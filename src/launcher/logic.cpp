#include "launcher/logic.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <charconv>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

#include "common/runtime_dirs.hpp"

namespace bb::launcher {

Lang choose_language(std::string_view locale) {
    // "de:en" lists languages by preference; the first one counts.
    locale = locale.substr(0, locale.find(':'));
    const bool german = locale.starts_with("de") &&
                        (locale.size() == 2 || !std::isalpha(static_cast<unsigned char>(locale[2])));
    return german ? Lang::de : Lang::en;
}

Lang language_from_environment() {
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANGUAGE", "LANG"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0') {
            return choose_language(value);
        }
    }
    return Lang::en;
}

std::string display_version(std::string_view v) {
    const std::size_t dot = v.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == v.size()) {
        return std::string(v);
    }
    const std::string_view major = v.substr(0, dot);
    const bool digits = std::ranges::all_of(
        major, [](char c) { return c >= '0' && c <= '9'; });
    const std::size_t first = major.find_first_not_of('0');
    if (!digits) {
        return std::string(v);
    }
    return std::string(first == std::string_view::npos ? "0" : major.substr(first)) +
           std::string(v.substr(dot));
}

CropRect crop_around_centre(int image_w, int image_h, int target_w, int target_h) {
    if (image_w <= 0 || image_h <= 0 || target_w <= 0 || target_h <= 0) {
        return {};
    }
    // Compare image_w / image_h with target_w / target_h without division.
    CropRect r{0, 0, image_w, image_h};
    if (static_cast<long long>(image_w) * target_h > static_cast<long long>(target_w) * image_h) {
        r.w = std::max(1, static_cast<int>(static_cast<long long>(image_h) * target_w / target_h));
    } else {
        r.h = std::max(1, static_cast<int>(static_cast<long long>(image_w) * target_h / target_w));
    }
    r.x = (image_w - r.w) / 2;
    r.y = (image_h - r.h) / 2;
    return r;
}

MainState select_state(const StateInput& in) {
    if (!in.have_folder) {
        return MainState::no_folder;
    }
    return in.folder_ok ? MainState::ready : MainState::check_failed;
}

Action first_button(MainState state) {
    switch (state) {
    case MainState::no_folder:
    case MainState::check_failed:
        return Action::set_up;
    default:
        return Action::play;
    }
}

bool shows_picture(MainState state) {
    return state == MainState::ready;
}

Prompt before_play(bool gpu_ok, bool controller) {
    if (!gpu_ok) {
        return Prompt::no_gpu;
    }
    return controller ? Prompt::none : Prompt::no_controller;
}

std::vector<std::string> bb_run_arguments(const std::filesystem::path& bb_run,
                                          const std::filesystem::path& config) {
    return {bb_run.string(), "--config", config.string()};
}

bool inside_game_folder(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path current = fs::weakly_canonical(folder, ec);
    if (ec) {
        current = folder.lexically_normal();
    }
    while (!current.empty()) {
        if (fs::exists(current / "eboot.bin", ec)) {
            return true;
        }
        const fs::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return false;
}

std::filesystem::path propose_data_folder(const std::filesystem::path& appimage,
                                          std::string_view home, bool appimage_dir_writable) {
    // A folder inside a dump (the AppImage was kept in the game's folder, say) is never proposed:
    // the game's folder is read-only for us.
    if (appimage.is_absolute() && appimage_dir_writable) {
        const std::filesystem::path next_to = appimage.parent_path() / data_folder_name;
        if (!inside_game_folder(next_to)) {
            return next_to;
        }
    }
    if (!home.empty() && std::filesystem::path(home).is_absolute()) {
        const std::filesystem::path in_home = std::filesystem::path(home) / data_folder_name;
        if (!inside_game_folder(in_home)) {
            return in_home;
        }
    }
    return {};
}

std::filesystem::path game_search_dir(bool appimage, const std::filesystem::path& data_folder,
                                      const std::filesystem::path& executable_dir) {
    return (appimage ? data_folder : executable_dir) / "game";
}

DataFolderResult adopt_data_folder(const std::filesystem::path& folder,
                                   const std::filesystem::path& pointer_file) {
    namespace fs = std::filesystem;
    const auto fail = [](DataFolderProblem p, std::string detail) {
        return DataFolderResult{p, std::move(detail)};
    };
    if (pointer_file.empty() || folder.empty()) {
        return fail(DataFolderProblem::no_place, "no folder or no file to keep the choice in");
    }
    if (!folder.is_absolute()) {
        return fail(DataFolderProblem::not_absolute, folder.string());
    }
    if (std::error_code none; inside_game_folder(folder)) {  // before anything is created there
        return fail(fs::exists(folder / "eboot.bin", none) ? DataFolderProblem::is_game
                                                     : DataFolderProblem::inside_game,
                    folder.string());
    }
    std::error_code ec;
    fs::path current;
    for (const fs::path& part : folder.lexically_normal()) {  // missing folders get 0700
        current /= part;
        if (fs::is_directory(current, ec)) {
            continue;
        }
        if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
            return fail(DataFolderProblem::cannot_create,
                        current.string() + ": " + std::strerror(errno));
        }
    }
    if (!fs::is_directory(folder, ec) || ::access(folder.c_str(), W_OK | X_OK) != 0) {
        return fail(DataFolderProblem::not_writable, folder.string());
    }
    try {
        common::check_data_folder(folder, {});
    } catch (const std::exception& e) {
        return fail(DataFolderProblem::overlaps, e.what());
    }
    try {
        common::write_data_dir_pointer(fs::canonical(folder), pointer_file);
    } catch (const std::exception& e) {
        return fail(DataFolderProblem::pointer_failed, e.what());
    }
    return {};
}

std::vector<ScriptStep> parse_script(std::string_view text) {
    std::vector<ScriptStep> steps;
    int line_no = 0;
    while (!text.empty()) {
        ++line_no;
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t space = line.find(' ');
        const std::string_view word = line.substr(0, space);
        const std::string_view arg =
            space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
        const auto fail = [&](std::string_view why) {
            throw std::invalid_argument(std::format("script line {}: {}", line_no, why));
        };
        const auto number = [&]() {
            int n = 0;
            const auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), n);
            if (ec != std::errc() || ptr != arg.data() + arg.size() || n < 0 || n > 1000000) {
                fail("a number from 0 to 1000000 is needed");
            }
            return n;
        };
        ScriptStep step;
        if (word == "wait") {
            step.kind = StepKind::wait;
            step.number = number();
        } else if (word == "waitgame") {
            step.kind = StepKind::waitgame;
            step.number = number();
        } else if (word == "drop" || word == "shot" || word == "config" || word == "datafolder" ||
                   word == "movedata") {
            if (arg.empty()) {
                fail("a path is needed");
            }
            step.kind = word == "drop"   ? StepKind::drop
                        : word == "shot" ? StepKind::shot
                        : word == "config" ? StepKind::config
                        : word == "movedata" ? StepKind::movedata
                                           : StepKind::datafolder;
            step.text = std::string(arg);
        } else if (word == "press" || word == "otherpad") {
            step.kind = word == "press" ? StepKind::press : StepKind::otherpad;
            if (arg == "accept") {
                step.button = PadButton::accept;
            } else if (arg == "back") {
                step.button = PadButton::back;
            } else if (arg == "up") {
                step.button = PadButton::up;
            } else if (arg == "down") {
                step.button = PadButton::down;
            } else if (arg == "left") {
                step.button = PadButton::left;
            } else if (arg == "right") {
                step.button = PadButton::right;
            } else {
                fail(std::format("{} needs accept, back, up, down, left or right", word));
            }
        } else if (word == "nopad" || word == "search" || word == "quit" ||
                   word == "datadefault" || word == "movedefault") {
            step.kind = word == "nopad"      ? StepKind::nopad
                        : word == "search"   ? StepKind::search
                        : word == "datadefault" ? StepKind::datadefault
                        : word == "movedefault" ? StepKind::movedefault
                                             : StepKind::quit;
        } else {
            fail(std::format("unknown step \"{}\"", word));
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

}  // namespace bb::launcher
