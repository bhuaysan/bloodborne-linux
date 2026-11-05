// bb-load: maps the eboot, relocates it with the HLE stubs, sets its final protections and prints
// its layout, relocations and imports. Runs no game code (docs/design/runtime.md, "Tools").

#include <sys/mman.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hle/hle.hpp"
#include "loader/image.hpp"
#include "loader/relocate.hpp"

namespace {

using namespace bb::loader;

constexpr std::string_view usage =
    "usage: bb-load [--game-dir DIR]\n"
    "Maps and relocates eboot.bin of the game in DIR (default: $BB_GAME_DIR) and prints its\n"
    "layout, relocations and imports. Runs no game code.\n";

std::string prot_string(int prot) {
    return {(prot & PROT_READ) ? 'r' : '-', (prot & PROT_WRITE) ? 'w' : '-',
            (prot & PROT_EXEC) ? 'x' : '-'};
}

std::string_view reloc_name(std::uint32_t type) {
    switch (type) {
    case r_x86_64_64:
        return "R_X86_64_64";
    case r_x86_64_glob_dat:
        return "R_X86_64_GLOB_DAT";
    case r_x86_64_jump_slot:
        return "R_X86_64_JUMP_SLOT";
    case r_x86_64_relative:
        return "R_X86_64_RELATIVE";
    }
    return "?";  // relocate() accepts no other type
}

// The relocations of both tables by type, and what bb::hle::resolve created for the imports.
void print_relocations(const DynamicInfo& dyn) {
    std::map<std::uint32_t, std::size_t> by_type;
    for (const auto* table : {&dyn.relocs, &dyn.plt_relocs}) {
        for (const Rela& r : *table) {
            ++by_type[r.type];
        }
    }
    for (const auto& [type, count] : by_type) {
        std::cout << std::format("reloc      {} {}\n", reloc_name(type), count);
    }
    const bb::hle::Counts hle = bb::hle::counts();
    std::cout << std::format("hle        {} stubs, {} data blocks\n", hle.stubs, hle.data_blocks);
}

struct ImportCounts {
    std::size_t func = 0, object = 0;
    std::size_t total() const { return func + object; }
};

void print_imports(const DynamicInfo& dyn) {
    std::map<std::string, ImportCounts> by_library;
    for (const Symbol& sym : dyn.symbols) {
        if (sym.kind == Symbol::Kind::import) {
            ImportCounts& counts = by_library[sym.decoded.library];
            ++(sym.type == stt_func ? counts.func : counts.object);
        }
    }
    // The order of tools/elfimports: most imports first, then by name.
    std::vector<std::pair<std::string, ImportCounts>> sorted(by_library.begin(), by_library.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second.total() > b.second.total();
    });
    std::size_t total = 0;
    for (const auto& [library, counts] : sorted) {
        total += counts.total();
    }
    std::cout << std::format("imports    {} in {} libraries\n", total, sorted.size());
    for (const auto& [library, counts] : sorted) {
        std::cout << std::format("  {:<24} {:>4}  {} func, {} object\n", library, counts.total(),
                                 counts.func, counts.object);
    }
}

void print(const Image& image) {
    std::cout << std::format("base       {:#x}\n", image.base);
    std::cout << std::format("entry      {:#x}\n", image.entry);
    for (const Segment& s : image.segments) {
        std::cout << std::format("segment    {:#x} size {:#x} file {:#x} {}\n", s.link_addr,
                                 s.mem_size, s.file_size, prot_string(s.prot));
    }
    if (image.tls) {
        std::cout << std::format("tls        {:#x} file {:#x} mem {:#x} align {:#x}\n",
                                 image.tls->link_addr, image.tls->file_size, image.tls->mem_size,
                                 image.tls->align);
    } else {
        std::cout << "tls        none\n";
    }
    if (image.proc_param) {
        std::cout << std::format("procparam  {:#x}\n", *image.proc_param);
    } else {
        std::cout << "procparam  none\n";
    }
    print_relocations(image.dyn);
    print_imports(image.dyn);
}

}  // namespace

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> game_dir;
    if (const char* env = std::getenv("BB_GAME_DIR"); env && *env) {
        game_dir = env;
    }
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--game-dir" && i + 1 < argc) {
            game_dir = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << usage;
            return 0;
        } else {
            std::cerr << usage;
            return 2;
        }
    }
    if (!game_dir) {
        std::cerr << "bb-load: set BB_GAME_DIR or pass --game-dir DIR\n";
        return 2;
    }
    try {
        Image image = map_eboot(*game_dir);
        relocate(image, bb::hle::resolve, eboot_module_id);
        image.finalize_protection();
        print(image);
    } catch (const std::exception& e) {
        std::cerr << "bb-load: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
