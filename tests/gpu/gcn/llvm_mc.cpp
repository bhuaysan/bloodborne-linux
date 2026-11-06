#include "gpu/gcn/llvm_mc.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <unistd.h>

namespace bb::test::gcn {

namespace {

namespace fs = std::filesystem;

// Runs `command` through the shell and returns what it prints, with stderr unless `stderr_to`
// redirects it (a pipe's buffering would cut a line of stdout in two with the other stream).
std::string run(const std::string& command, const std::string& stderr_to = "&1") {
    FILE* pipe = popen((command + " 2>" + stderr_to).c_str(), "r");
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

std::string run_mc(const fs::path& source, std::string_view mcpu,
                   const std::string& stderr_to = "&1") {
    return run(std::string("'") + BB_LLVM_MC + "' -triple=amdgcn -mcpu=" + std::string(mcpu) +
                   " -show-encoding '" + source.string() + "'",
               stderr_to);
}

// Every instruction llvm-mc assembled from the output, in order.
std::vector<Assembled> encodings(const std::string& output) {
    // Each line is "\t<text> ; encoding: [<bytes>]", with the text padded by spaces.
    std::vector<Assembled> out;
    std::istringstream stream(output);
    for (std::string line; std::getline(stream, line);) {
        if (auto pos = line.find("; encoding: ["); pos != std::string::npos) {
            const auto first = line.find_first_not_of(" \t");
            const auto last = line.find_last_not_of(' ', pos - 1);
            out.push_back({line.substr(first, last + 1 - first),
                           words(std::string_view(line).substr(pos))});
        }
    }
    return out;
}

// Every instruction llvm-mc assembles from the source file, in order.
std::vector<Assembled> assemble_path(const fs::path& source) {
    const std::string output = run_mc(source, "bonaire");
    if (output.find("error:") != std::string::npos) {
        throw std::runtime_error("llvm-mc rejected the source:\n" + output);
    }
    // A label's fixup is left unresolved, and its bytes print as "A".
    if (output.find("  fixup ") != std::string::npos) {
        throw std::runtime_error("branches need numeric offsets, not labels:\n" + output);
    }
    return encodings(output);
}

// Writes the lines to a new temporary file and returns its path.
fs::path write_source(const std::vector<std::string>& lines) {
    std::string path = (fs::temp_directory_path() / "bb-gcn-XXXXXX").string();
    int fd = ::mkstemp(path.data());
    if (fd < 0) {
        throw std::runtime_error("cannot create " + path);
    }
    ::close(fd);
    std::ofstream source(path);
    for (const auto& line : lines) {
        source << line << '\n';
    }
    return path;
}

}  // namespace

bool have_llvm_mc() {
    return !std::string_view(BB_LLVM_MC).empty();
}

std::vector<std::vector<std::uint32_t>> assemble_file(const fs::path& source) {
    std::vector<std::vector<std::uint32_t>> out;
    for (auto& inst : assemble_path(source)) {
        out.push_back(std::move(inst.words));
    }
    return out;
}

std::vector<Assembled> assemble(const std::vector<std::string>& lines) {
    const fs::path path = write_source(lines);
    std::vector<Assembled> out;
    try {
        out = assemble_path(path);
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

std::vector<Attempt> assemble_each(const std::vector<std::string>& lines, std::string_view mcpu) {
    const fs::path path = write_source(lines);
    const fs::path error_path = path.string() + ".err";
    std::string output, errors;
    try {
        output = run_mc(path, mcpu, "'" + error_path.string() + "'");
        std::ifstream stream(error_path);
        errors.assign(std::istreambuf_iterator<char>(stream), {});
    } catch (...) {
        fs::remove(path);
        fs::remove(error_path);
        throw;
    }
    fs::remove(path);
    fs::remove(error_path);
    // "<path>:<line>:<column>: error: <message>", then the line and a caret. llvm-mc goes on with
    // the next line and prints no encoding for this one.
    std::vector<Attempt> out(lines.size());
    const std::string prefix = path.string() + ":";
    std::size_t rejected = 0;
    std::istringstream stream(errors);
    for (std::string line; std::getline(stream, line);) {
        const auto error = line.find(": error: ");
        if (!line.starts_with(prefix) || error == std::string::npos) {
            continue;
        }
        const std::size_t number = std::stoul(line.substr(prefix.size()));
        if (number == 0 || number > lines.size()) {
            throw std::runtime_error("llvm-mc error for line " + std::to_string(number) + ":\n" +
                                     errors);
        }
        if (out[number - 1].error.empty()) {
            out[number - 1].error = line.substr(error + 9);
            ++rejected;
        }
    }
    auto assembled = encodings(output);
    if (assembled.size() + rejected != lines.size()) {
        throw std::runtime_error("llvm-mc gave " + std::to_string(assembled.size()) +
                                 " encodings and " + std::to_string(rejected) + " errors for " +
                                 std::to_string(lines.size()) + " lines");
    }
    auto next = assembled.begin();
    for (auto& attempt : out) {
        if (attempt.error.empty()) {
            attempt.assembled = std::move(*next++);
        }
    }
    return out;
}

}  // namespace bb::test::gcn
