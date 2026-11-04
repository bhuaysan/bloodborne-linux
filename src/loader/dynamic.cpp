#include "loader/dynamic.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

#include "loader/bytes.hpp"

namespace bb::loader {
namespace {

constexpr std::uint64_t dt_rela = 7;

// A dynamic tag with its name for error messages.
struct Tag {
    std::uint64_t value;
    std::string_view name;
};

// Sony tags, our names (docs/formats/elf-dynamic.md).
constexpr Tag dt_sce_pltgot{0x61000027, "DT_SCE_PLTGOT"};
constexpr Tag dt_sce_jmprel{0x61000029, "DT_SCE_JMPREL"};
constexpr Tag dt_sce_pltrel{0x6100002B, "DT_SCE_PLTREL"};
constexpr Tag dt_sce_pltrelsz{0x6100002D, "DT_SCE_PLTRELSZ"};
constexpr Tag dt_sce_rela{0x6100002F, "DT_SCE_RELA"};
constexpr Tag dt_sce_relasz{0x61000031, "DT_SCE_RELASZ"};
constexpr Tag dt_sce_relaent{0x61000033, "DT_SCE_RELAENT"};
constexpr Tag dt_sce_strtab{0x61000035, "DT_SCE_STRTAB"};
constexpr Tag dt_sce_strsz{0x61000037, "DT_SCE_STRSZ"};
constexpr Tag dt_sce_symtab{0x61000039, "DT_SCE_SYMTAB"};
constexpr Tag dt_sce_syment{0x6100003B, "DT_SCE_SYMENT"};
constexpr Tag dt_sce_symtabsz{0x6100003F, "DT_SCE_SYMTABSZ"};

constexpr std::size_t dyn_size = 16;
constexpr std::size_t sym_size = 24;
constexpr std::size_t rela_size = 24;

// The program header of `type`, or nullptr. Throws LoadError when there are several.
const Phdr* find_phdr(std::span<const Phdr> phdrs, std::uint32_t type, std::string_view what) {
    auto is_type = [&](const Phdr& ph) { return ph.type == type; };
    if (std::count_if(phdrs.begin(), phdrs.end(), is_type) > 1) {
        throw LoadError(std::format("more than one {} segment", what));
    }
    auto it = std::find_if(phdrs.begin(), phdrs.end(), is_type);
    return it == phdrs.end() ? nullptr : &*it;
}

// The NUL-terminated ASCII string at `offset` in the string table.
std::string read_string(std::span<const std::byte> strtab, std::uint64_t offset,
                        std::string_view what) {
    if (offset >= strtab.size()) {
        throw LoadError(
            std::format("{}: string offset {:#x} outside the string table", what, offset));
    }
    auto rest = strtab.subspan(offset);
    auto end = std::find(rest.begin(), rest.end(), std::byte{0});
    if (end == rest.end()) {
        throw LoadError(std::format("{}: unterminated string at {:#x}", what, offset));
    }
    std::string out;
    for (auto it = rest.begin(); it != end; ++it) {
        auto c = std::to_integer<unsigned char>(*it);
        if (c >= 0x80) {
            throw LoadError(std::format("{}: non-ASCII string at {:#x}", what, offset));
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::vector<Rela> read_relas(std::span<const std::byte> table, std::size_t symbol_count,
                             std::string_view what) {
    std::vector<Rela> out;
    out.reserve(table.size() / rela_size);
    for (std::uint64_t at = 0; at < table.size(); at += rela_size) {
        auto info = read_le<std::uint64_t>(table, at + 8, what);
        Rela rela{read_le<std::uint64_t>(table, at, what), static_cast<std::uint32_t>(info),
                  static_cast<std::uint32_t>(info >> 32),
                  read_le<std::int64_t>(table, at + 16, what)};
        if (rela.sym >= symbol_count) {
            throw LoadError(std::format("{} entry {}: symbol index {} but only {} symbols", what,
                                        at / rela_size, rela.sym, symbol_count));
        }
        out.push_back(rela);
    }
    return out;
}

}  // namespace

DynamicInfo parse_dynamic(std::span<const std::byte> elf, std::span<const Phdr> phdrs) {
    const Phdr* dynlib = find_phdr(phdrs, pt_sce_dynlibdata, "PT_SCE_DYNLIBDATA");
    const Phdr* dynamic = find_phdr(phdrs, pt_dynamic, "PT_DYNAMIC");
    if (dynlib == nullptr || dynamic == nullptr) {
        throw LoadError("no PT_SCE_DYNLIBDATA or PT_DYNAMIC segment");
    }
    need_bytes(elf, dynlib->offset, dynlib->filesz, "PT_SCE_DYNLIBDATA");
    if (dynamic->offset < dynlib->offset ||
        !in_bounds(dynamic->offset - dynlib->offset, dynamic->filesz, dynlib->filesz)) {
        throw LoadError("PT_DYNAMIC lies outside PT_SCE_DYNLIBDATA");
    }
    auto blob = elf.subspan(dynlib->offset, dynlib->filesz);
    auto table_bytes = blob.subspan(dynamic->offset - dynlib->offset, dynamic->filesz);

    std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;  // up to DT_NULL
    for (std::uint64_t at = 0;; at += dyn_size) {
        if (!in_bounds(at, dyn_size, table_bytes.size())) {
            throw LoadError("dynamic table has no DT_NULL");
        }
        auto tag = read_le<std::uint64_t>(table_bytes, at, "dynamic table");
        if (tag == 0) {
            break;
        }
        entries.emplace_back(tag, read_le<std::uint64_t>(table_bytes, at + 8, "dynamic table"));
    }

    // The value of a tag that must appear exactly once.
    auto required = [&](const Tag& tag) {
        auto same = [&](const auto& entry) { return entry.first == tag.value; };
        auto count = std::count_if(entries.begin(), entries.end(), same);
        if (count != 1) {
            throw LoadError(std::format("{} {} in the dynamic table",
                                        count == 0 ? "missing" : "duplicate", tag.name));
        }
        return std::find_if(entries.begin(), entries.end(), same)->second;
    };
    auto expect = [&](const Tag& tag, std::uint64_t want) {
        std::uint64_t value = required(tag);
        if (value != want) {
            throw LoadError(std::format("{} is {:#x}, expected {:#x}", tag.name, value, want));
        }
    };
    expect(dt_sce_pltrel, dt_rela);
    expect(dt_sce_syment, sym_size);
    expect(dt_sce_relaent, rela_size);
    required(dt_sce_pltgot);
    auto table = [&](const Tag& offset_tag, const Tag& size_tag, std::size_t entry_size,
                     std::string_view what) {
        std::uint64_t offset = required(offset_tag), size = required(size_tag);
        if (!in_bounds(offset, size, blob.size())) {
            throw LoadError(std::format("{} at {:#x} + {:#x} lies outside PT_SCE_DYNLIBDATA", what,
                                        offset, size));
        }
        if (size % entry_size != 0) {
            throw LoadError(std::format("{} size {:#x} is not a multiple of {}", what, size,
                                        entry_size));
        }
        return blob.subspan(offset, size);
    };
    auto strtab = table(dt_sce_strtab, dt_sce_strsz, 1, "string table");
    auto symtab = table(dt_sce_symtab, dt_sce_symtabsz, sym_size, "symbol table");
    auto jmprel = table(dt_sce_jmprel, dt_sce_pltrelsz, rela_size, "PLT relocations");
    auto relatab = table(dt_sce_rela, dt_sce_relasz, rela_size, "relocations");

    DynamicInfo info;
    for (std::uint64_t i = 0; i < symtab.size() / sym_size; ++i) {
        std::uint64_t at = i * sym_size;
        auto name = read_le<std::uint32_t>(symtab, at, "symbol");
        auto st_info = read_le<std::uint8_t>(symtab, at + 4, "symbol");
        info.symbols.push_back(Symbol{read_string(strtab, name, "symbol name"),
                                      static_cast<std::uint8_t>(st_info & 0xF),
                                      static_cast<std::uint8_t>(st_info >> 4),
                                      read_le<std::uint16_t>(symtab, at + 6, "symbol"),
                                      read_le<std::uint64_t>(symtab, at + 8, "symbol")});
    }
    info.plt_relocs = read_relas(jmprel, info.symbols.size(), "PLT relocations");
    info.relocs = read_relas(relatab, info.symbols.size(), "relocations");
    return info;
}

}  // namespace bb::loader
