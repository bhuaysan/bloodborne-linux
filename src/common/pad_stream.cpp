#include "common/pad_stream.hpp"

#include <charconv>
#include <format>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

namespace bb::common {
namespace {

struct Named {
    std::string_view name;
    int index;
};

constexpr Named buttons[] = {{"A", 0}, {"B", 1}, {"X", 2},  {"Y", 3},  {"BACK", 4}, {"GUIDE", 5},
                             {"START", 6}, {"L3", 7}, {"R3", 8}, {"LB", 9}, {"RB", 10}};
constexpr int dpad_up = 11, dpad_down = 12, dpad_left = 13, dpad_right = 14;
constexpr Named axes[] = {{"LX", 0}, {"LY", 1}, {"RX", 2}, {"RY", 3}, {"LT", 4}, {"RT", 5}};
constexpr int axis_min = -32768, axis_max = 32767, trigger_max = 255;

template <std::size_t N>
int find(const Named (&table)[N], std::string_view name, const char* what) {
    for (const Named& entry : table) {
        if (entry.name == name) {
            return entry.index;
        }
    }
    throw std::invalid_argument(std::format("unknown {} \"{}\"", what, name));
}

int number(std::string_view text, int low, int high, std::string_view what) {
    int value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end || value < low || value > high) {
        throw std::invalid_argument(
            std::format("{}: \"{}\" is not a number from {} to {}", what, text, low, high));
    }
    return value;
}

// The next word of `rest`, which loses it and the blanks before it.
std::string_view word(std::string_view& rest) {
    const std::size_t begin = rest.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) {
        rest = {};
        return {};
    }
    rest.remove_prefix(begin);
    const std::size_t end = rest.find_first_of(" \t\r");
    const std::string_view result = rest.substr(0, end);
    rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
    return result;
}

}  // namespace

std::vector<PadUpdate> parse_pad_line(std::string_view line) {
    std::string_view rest = line;
    const std::string_view command = word(rest);
    if (command.empty() || command.front() == '#') {
        return {};
    }
    if (command == "neutral") {
        if (!word(rest).empty()) {
            throw std::invalid_argument("neutral takes no argument");
        }
        return neutral_pad_updates();
    }
    const std::string_view name = word(rest);
    const std::string_view value = word(rest);
    if (name.empty() || value.empty() || !word(rest).empty()) {
        throw std::invalid_argument(std::format("\"{}\" needs a name and a value", command));
    }
    if (command == "button") {
        return {{false, find(buttons, name, "button"), number(value, 0, 1, "button")}};
    }
    if (command != "axis") {
        throw std::invalid_argument(std::format("unknown command \"{}\"", command));
    }
    if (name == "HX" || name == "HY") {
        const int v = number(value, -1, 1, name);
        const int low = name == "HX" ? dpad_left : dpad_up;
        const int high = name == "HX" ? dpad_right : dpad_down;
        return {{false, low, v < 0 ? 1 : 0}, {false, high, v > 0 ? 1 : 0}};
    }
    const int index = find(axes, name, "axis");
    if (name == "LT" || name == "RT") {
        const int pull = number(value, 0, trigger_max, name);
        return {{true, index, axis_min + pull * (axis_max - axis_min) / trigger_max}};
    }
    return {{true, index, number(value, axis_min, axis_max, name)}};
}

std::vector<PadUpdate> neutral_pad_updates() {
    std::vector<PadUpdate> result;
    for (int i = 0; i < pad_stream_buttons; ++i) {
        result.push_back({false, i, 0});
    }
    for (int i = 0; i < pad_stream_axes; ++i) {
        result.push_back({true, i, i >= 4 ? axis_min : 0});
    }
    return result;
}

}  // namespace bb::common
