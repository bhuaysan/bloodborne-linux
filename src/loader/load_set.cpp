#include "loader/load_set.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <utility>

#include "common/memory.hpp"
#include "loader/bytes.hpp"
#include "loader/elf.hpp"
#include "loader/relocate.hpp"
#include "loader/self.hpp"

namespace bb::loader {
namespace {

// The ELF of the pinned PRX `name` in the game's sce_module/.
std::vector<std::byte> read_prx(const std::filesystem::path& game_dir, std::string_view name) {
    auto pinned = std::ranges::find(prx_sha256, name, &std::pair<std::string_view,
                                                                 std::string_view>::first);
    if (pinned == prx_sha256.end()) {
        throw LoadError(std::format("{} is not a pinned PRX (see docs/clean-room.md)", name));
    }
    return read_pinned_self(game_dir / "sce_module" / name, pinned->second);
}

// The alignment map_elf needs for the base of `elf`: the largest PT_LOAD p_align, at least a page.
std::uint64_t base_alignment(std::span<const std::byte> elf) {
    std::uint64_t align = common::page_size();
    for (const Phdr& ph : read_phdrs(elf, read_elf_header(elf))) {
        if (ph.type == pt_load) {
            align = std::max(align, ph.align);
        }
    }
    return align;
}

// The first address after the reserved span of `image`.
std::uintptr_t image_end(const Image& image) {
    std::uint64_t span = 0;
    for (const Segment& s : image.segments) {
        span = std::max(span, common::page_up(s.link_addr + s.mem_size));
    }
    return image.base + span;
}

}  // namespace

std::vector<Export> exports(const Image& image) {
    std::vector<Export> out;
    for (const Symbol& sym : image.dyn.symbols) {
        if (sym.kind != Symbol::Kind::exported || sym.type == stt_tls) {
            continue;
        }
        if (!image.segment_at(sym.value)) {
            throw LoadError(std::format("export {} at {:#x} lies outside the image", sym.name,
                                        sym.value));
        }
        out.push_back({sym.decoded.library, sym.decoded.nid, image.base + sym.value});
    }
    return out;
}

Image map_prx(const std::filesystem::path& game_dir, std::string_view name, std::uintptr_t base) {
    return map_elf(read_prx(game_dir, name), base);
}

LoadSet map_load_set(const std::filesystem::path& game_dir, std::uintptr_t base) {
    LoadSet set;
    set.modules.push_back({"eboot.bin", eboot_module_id, map_eboot(game_dir, base)});
    for (std::string_view name : load_set_prxs) {
        std::vector<std::byte> elf = read_prx(game_dir, name);
        const std::uint64_t align = base_alignment(elf);
        const std::uintptr_t next = image_end(set.modules.back().image);
        const std::uintptr_t prx_base = (next + align - 1) & ~(align - 1);
        set.modules.push_back({std::string(name), eboot_module_id + set.modules.size(),
                               map_elf(elf, prx_base)});
    }
    return set;
}

}  // namespace bb::loader
