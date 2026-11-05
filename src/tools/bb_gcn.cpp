// bb-gcn: disassembles a GCN shader binary (docs/design/graphics.md, "Source layout" and
// "Decoder and disassembler").

#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "common/file.hpp"
#include "gpu/gcn/gcn.hpp"

namespace {

constexpr std::string_view usage =
    "usage: bb-gcn disasm [--all] FILE\n"
    "Disassembles the GCN program at the start of FILE up to its final s_endpgm, in LLVM's\n"
    "syntax. With --all, disassembles every whole dword of FILE instead.\n";

// One line per instruction: the text, then the byte offset and the words as a comment.
void disasm(const char* file, bool all) {
    // Shader code is little-endian dwords, as on the host. A partial dword at the end isn't code.
    const auto bytes = bb::common::read_file(file);
    std::vector<std::uint32_t> code(bytes.size() / 4);
    if (!code.empty()) {
        std::memcpy(code.data(), bytes.data(), code.size() * 4);
    }
    const auto until = all ? bb::gcn::Until::code_end : bb::gcn::Until::program_end;
    for (const auto& inst : bb::gcn::decode(code, until)) {
        std::string words;
        for (unsigned i = 0; i < inst.size; ++i) {
            words += std::format(" {:08x}", code[inst.offset / 4 + i]);
        }
        std::cout << std::format("{:<48} // {:06x}:{}\n", bb::gcn::disassemble(inst), inst.offset,
                                 words);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view command = argc >= 2 ? argv[1] : "";
    if (command == "-h" || command == "--help") {
        std::cout << usage;
        return 0;
    }
    bool ok = command == "disasm";
    bool all = false;
    const char* file = nullptr;
    for (int i = 2; ok && i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--all") {
            all = true;
        } else if (file == nullptr && !arg.starts_with('-')) {
            file = argv[i];
        } else {
            ok = false;
        }
    }
    if (!ok || file == nullptr) {
        std::cerr << usage;
        return 2;
    }
    try {
        disasm(file, all);
    } catch (const std::exception& e) {
        std::cerr << "bb-gcn: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
