#include "gpu/gcn/corpus.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "common/file.hpp"

namespace bb::test::gcn {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view footer_marker = "OrbShdr";
constexpr std::size_t footer_size = 0x1c;

}  // namespace

Program read_program(const fs::path& file, std::string& why) {
    const auto bytes = bb::common::read_file(file);
    const std::string_view data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto block = data.find(footer_marker);
    if (block == std::string_view::npos || block + footer_size > data.size()) {
        why = "no OrbShdr footer block";
        return {};
    }
    std::uint32_t info = 0;
    std::memcpy(&info, data.data() + block + 8, 4);  // the code size is in bits 8 to 31
    const std::size_t size = info >> 8;
    if (size == 0 || size % 4 != 0 || size > block) {
        why = "the footer's code size " + std::to_string(size) + " doesn't fit the file";
        return {};
    }
    Program program;
    program.type = (info >> 2) & 0xf;
    program.code.resize(size / 4);
    std::memcpy(program.code.data(), data.data(), size);
    return program;
}

std::vector<fs::path> program_files(const fs::path& root) {
    std::vector<fs::path> files;
    for (const auto& archive : fs::directory_iterator(root)) {
        if (!fs::is_directory(archive.path() / "programs")) {
            continue;
        }
        for (const auto& entry : fs::recursive_directory_iterator(archive.path() / "programs")) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

const char* program_type_name(unsigned type) {
    static const char* const names[] = {"ps", "vs", "es", "ls", "cs", "gs", "copy-vs", "hs", "ds"};
    return type < std::size(names) ? names[type] : "unknown";
}

}  // namespace bb::test::gcn
