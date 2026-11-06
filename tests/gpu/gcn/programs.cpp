#include "gpu/gcn/programs.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>

namespace bb::test::gcn {

namespace fs = std::filesystem;

namespace {

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// One line of .hex text: 1 or 2 words. No gfx7 instruction is longer: a 32-bit encoding may take
// a literal, a 64-bit encoding takes none.
std::vector<std::uint32_t> parse_line(std::string_view line, std::size_t number) {
    const auto bad = [&] {
        return std::runtime_error(".hex line " + std::to_string(number) + " is not 1 or 2 words " +
                                  "of 8 lower-case hex digits: \"" + std::string(line) + "\"");
    };
    std::vector<std::uint32_t> words;
    for (std::size_t pos = 0;; pos += 9) {
        const std::string_view word = line.substr(pos, 8);
        if (word.size() != 8 || !std::all_of(word.begin(), word.end(), is_hex_digit)) {
            throw bad();
        }
        words.push_back(static_cast<std::uint32_t>(std::stoul(std::string(word), nullptr, 16)));
        if (pos + 8 == line.size()) {
            break;
        }
        if (line[pos + 8] != ' ') {
            throw bad();
        }
    }
    if (words.size() > 2) {
        throw bad();
    }
    return words;
}

}  // namespace

fs::path programs_dir() {
    return BB_GCN_PROGRAMS_DIR;
}

std::vector<std::string> program_names() {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(programs_dir())) {
        const auto ext = entry.path().extension();
        if (ext == ".s" || ext == ".hex") {
            names.insert(entry.path().stem().string());
        }
    }
    return {names.begin(), names.end()};
}

std::vector<std::vector<std::uint32_t>> parse_hex(std::string_view text) {
    if (!text.empty() && text.back() != '\n') {
        throw std::runtime_error(".hex text doesn't end in a newline");
    }
    std::vector<std::vector<std::uint32_t>> out;
    for (std::size_t start = 0; start < text.size();) {
        const std::size_t end = text.find('\n', start);
        out.push_back(parse_line(text.substr(start, end - start), out.size() + 1));
        start = end + 1;
    }
    return out;
}

std::vector<std::vector<std::uint32_t>> program_instructions(std::string_view name) {
    const fs::path path = programs_dir() / (std::string(name) + ".hex");
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot read " + path.string());
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return parse_hex(text);
}

std::vector<std::uint32_t> program_words(std::string_view name) {
    std::vector<std::uint32_t> out;
    for (const auto& inst : program_instructions(name)) {
        out.insert(out.end(), inst.begin(), inst.end());
    }
    return out;
}

}  // namespace bb::test::gcn
