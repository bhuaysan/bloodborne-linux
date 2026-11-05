// bb-load: maps the load set (the eboot and the bundled PRXs), binds and relocates it through the
// HLE layer, sets the final protections and prints each module's layout, relocations and imports.
// Runs no game code (docs/design/runtime.md, "Tools").

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
#include "hle/link.hpp"
#include "loader/image.hpp"
#include "loader/load_set.hpp"

namespace {

using namespace bb::loader;

constexpr std::string_view usage =
    "usage: bb-load [--game-dir DIR]\n"
    "Maps and relocates eboot.bin and the bundled PRXs it loads of the game in DIR (default:\n"
    "$BB_GAME_DIR) and prints their layout, relocations and imports. Runs no game code.\n";

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
    case r_x86_64_dtpmod64:
        return "R_X86_64_DTPMOD64";
    }
    return "?";  // relocate() accepts no other type
}

// The relocations of both tables by type.
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
}

struct ImportCounts {
    std::size_t func = 0, object = 0;
    // What bb::hle::resolve bound them to. Stubs include data blocks; unused are never resolved.
    std::size_t exported = 0, implemented = 0, stub = 0, unused = 0;
    std::size_t total() const { return func + object; }
};

void count_binding(ImportCounts& counts, const Import& imp) {
    const std::optional<bb::hle::Binding> b = bb::hle::binding(imp.library, imp.nid);
    if (!b) {
        ++counts.unused;
    } else if (*b == bb::hle::Binding::exported) {
        ++counts.exported;
    } else if (*b == bb::hle::Binding::implemented) {
        ++counts.implemented;
    } else {
        ++counts.stub;
    }
}

void print_imports(const DynamicInfo& dyn) {
    std::map<std::string, ImportCounts> by_library;
    for (const Symbol& sym : dyn.symbols) {
        if (sym.kind == Symbol::Kind::import) {
            ImportCounts& counts = by_library[sym.decoded.library];
            ++(sym.type == stt_func ? counts.func : counts.object);
            count_binding(counts, sym.decoded);
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
        std::cout << std::format("  {:<24} {:>4}  {} func, {} object; {} export, {} hle, {} stub, "
                                 "{} unused\n",
                                 library, counts.total(), counts.func, counts.object,
                                 counts.exported, counts.implemented, counts.stub, counts.unused);
    }
}

void print(const LoadedModule& module) {
    const Image& image = module.image;
    std::cout << std::format("module     {} id {}\n", module.file, module.id);
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
        LoadSet set = map_load_set(*game_dir);
        bb::hle::link(set);
        for (LoadedModule& module : set.modules) {
            module.image.finalize_protection();
        }
        for (const LoadedModule& module : set.modules) {
            print(module);
        }
        const bb::hle::Counts hle = bb::hle::counts();
        std::cout << std::format("hle        {} stubs, {} data blocks\n", hle.stubs,
                                 hle.data_blocks);
    } catch (const std::exception& e) {
        std::cerr << "bb-load: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
