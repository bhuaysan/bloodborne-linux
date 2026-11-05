#include "gpu/gcn/llvm_mc.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <unistd.h>

namespace bb::test::gcn {

namespace {

namespace fs = std::filesystem;

// Runs `command` through the shell and returns what it prints, with stderr.
std::string run(const std::string& command) {
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("cannot run " + command);
    }
    std::string out;
    char buffer[4096];
    while (std::size_t n = std::fread(buffer, 1, sizeof buffer, pipe)) {
        out.append(buffer, n);
    }
    pclose(pipe);  // llvm-mc fails on a rejected line, which the caller detects in the output
    return out;
}

// The words of an encoding as llvm-mc prints it, "[0x01,0x00,0x8b,0xbf]" (little-endian bytes).
std::vector<std::uint32_t> words(std::string_view bytes) {
    std::vector<std::uint32_t> out;
    unsigned index = 0;
    for (std::size_t pos = bytes.find("0x"); pos != std::string_view::npos;
         pos = bytes.find("0x", pos + 2), ++index) {
        auto byte = std::stoul(std::string(bytes.substr(pos + 2, 2)), nullptr, 16);
        if (index % 4 == 0) {
            out.push_back(0);
        }
        out.back() |= static_cast<std::uint32_t>(byte) << (8 * (index % 4));
    }
    return out;
}

}  // namespace

bool have_llvm_mc() {
    return !std::string_view(BB_LLVM_MC).empty();
}

std::vector<std::vector<std::uint32_t>> assemble_file(const fs::path& source) {
    const std::string output = run(std::string("'") + BB_LLVM_MC +
                                   "' -triple=amdgcn -mcpu=bonaire -show-encoding '" +
                                   source.string() + "'");
    std::vector<std::vector<std::uint32_t>> out;
    std::istringstream stream(output);
    for (std::string line; std::getline(stream, line);) {
        if (auto pos = line.find("; encoding: ["); pos != std::string::npos) {
            out.push_back(words(std::string_view(line).substr(pos)));
        }
    }
    if (output.find("error:") != std::string::npos) {
        throw std::runtime_error("llvm-mc rejected the source:\n" + output);
    }
    // A label's fixup is left unresolved, and its bytes print as "A".
    if (output.find("fixup") != std::string::npos) {
        throw std::runtime_error("branches need numeric offsets, not labels:\n" + output);
    }
    return out;
}

std::vector<std::vector<std::uint32_t>> assemble(const std::vector<std::string>& lines) {
    std::string path = (fs::temp_directory_path() / "bb-gcn-XXXXXX").string();
    int fd = ::mkstemp(path.data());
    if (fd < 0) {
        throw std::runtime_error("cannot create " + path);
    }
    ::close(fd);
    {
        std::ofstream source(path);
        for (const auto& line : lines) {
            source << line << '\n';
        }
    }
    std::vector<std::vector<std::uint32_t>> out;
    try {
        out = assemble_file(path);
    } catch (...) {
        fs::remove(path);
        throw;
    }
    fs::remove(path);
    if (out.size() != lines.size()) {
        throw std::runtime_error("llvm-mc gave " + std::to_string(out.size()) +
                                 " encodings for " + std::to_string(lines.size()) + " lines");
    }
    return out;
}

}  // namespace bb::test::gcn
