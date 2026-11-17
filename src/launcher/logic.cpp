#include "launcher/logic.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <format>
#include <stdexcept>

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
    if (!in.folder_ok) {
        return MainState::check_failed;
    }
    if (!in.gpu_ok) {
        return MainState::no_gpu;
    }
    if (in.last_run_failed) {
        return MainState::last_run_failed;
    }
    return in.controller ? MainState::ready : MainState::no_controller;
}

Action first_button(MainState state) {
    switch (state) {
    case MainState::no_folder:
    case MainState::check_failed:
        return Action::set_up;
    case MainState::no_gpu:
        return Action::help;
    default:
        return Action::play;
    }
}

bool help_highlighted(MainState state) {
    return state == MainState::last_run_failed;
}

bool shows_picture(MainState state) {
    return state != MainState::no_folder && state != MainState::check_failed;
}

std::vector<std::string> bb_run_arguments(const std::filesystem::path& bb_run,
                                          const std::filesystem::path& config) {
    return {bb_run.string(), "--config", config.string()};
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
        } else if (word == "drop" || word == "shot" || word == "config") {
            if (arg.empty()) {
                fail("a path is needed");
            }
            step.kind = word == "drop" ? StepKind::drop
                        : word == "shot" ? StepKind::shot
                                         : StepKind::config;
            step.text = std::string(arg);
        } else if (word == "press") {
            step.kind = StepKind::press;
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
                fail("press needs accept, back, up, down, left or right");
            }
        } else if (word == "quit") {
            step.kind = StepKind::quit;
        } else {
            fail(std::format("unknown step \"{}\"", word));
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

}  // namespace bb::launcher
