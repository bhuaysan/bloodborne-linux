#include "loader/dynamic.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

#include "loader/bytes.hpp"

namespace bb::loader {
namespace {

constexpr std::uint64_t dt_needed = 1;
constexpr std::uint64_t dt_rela = 7;

// A dynamic tag with its name for error messages.
struct Tag {
    std::uint64_t value;
    std::string_view name;
};

constexpr Tag dt_init{0x0C, "DT_INIT"};
constexpr Tag dt_fini{0x0D, "DT_FINI"};
constexpr Tag dt_soname{0x0E, "DT_SONAME"};
constexpr Tag dt_textrel{0x16, "DT_TEXTREL"};
constexpr Tag dt_init_array{0x19, "DT_INIT_ARRAY"};
constexpr Tag dt_fini_array{0x1A, "DT_FINI_ARRAY"};
constexpr Tag dt_init_arraysz{0x1B, "DT_INIT_ARRAYSZ"};
constexpr Tag dt_fini_arraysz{0x1C, "DT_FINI_ARRAYSZ"};
constexpr Tag dt_preinit_array{0x20, "DT_PREINIT_ARRAY"};
constexpr Tag dt_preinit_arraysz{0x21, "DT_PREINIT_ARRAYSZ"};

// Sony tags, our names (docs/formats/elf-dynamic.md).
constexpr Tag dt_sce_module_info{0x6100000D, "DT_SCE_MODULE_INFO"};
constexpr std::uint64_t dt_sce_needed_module = 0x6100000F;
constexpr std::uint64_t dt_sce_export_lib = 0x61000013;
constexpr std::uint64_t dt_sce_import_lib = 0x61000015;
constexpr std::uint64_t dt_sce_export_lib_attr = 0x61000017;
constexpr std::uint64_t dt_sce_import_lib_attr = 0x61000019;
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
constexpr std::size_t nid_length = 11;
// The digits of NIDs and of the ids in symbol names, values 0..63.
constexpr std::string_view base64_digits =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

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

// The fields of a module, library or attribute record.
struct Record {
    std::uint16_t id, version;
    std::uint32_t low;  // string table offset, or the attribute word
};

Record split_record(std::uint64_t value) {
    return {static_cast<std::uint16_t>(value >> 48), static_cast<std::uint16_t>(value >> 32),
            static_cast<std::uint32_t>(value)};
}

Library* find_library(std::vector<Library>& libraries, bool exported, std::uint16_t id) {
    auto it = std::find_if(libraries.begin(), libraries.end(), [&](const Library& lib) {
        return lib.exported == exported && lib.id == id;
    });
    return it == libraries.end() ? nullptr : &*it;
}

// A one-digit id of a symbol name. Larger ids are refused: how they are written is unknown.
std::optional<std::uint16_t> decode_id(std::string_view text) {
    if (text.size() != 1 || base64_digits.find(text[0]) == std::string_view::npos) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(base64_digits.find(text[0]));
}

// Sets sym.kind and sym.decoded from the name, checked against the records of `info`.
void classify(DynamicInfo& info, Symbol& sym, std::uint64_t index) {
    auto fail = [&](std::string_view why) {
        throw LoadError(std::format("symbol {} '{}': {}", index, sym.name, why));
    };
    if (index == 0 || (sym.type == stt_section && sym.name.empty())) {
        return;  // the null symbol and the one unnamed section symbol carry nothing
    }
    std::vector<std::string_view> parts;
    for (std::string_view rest = sym.name;;) {
        auto hash = rest.find('#');
        parts.push_back(rest.substr(0, hash));
        if (hash == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(hash + 1);
    }
    if (parts.size() == 1) {
        // Anything else might be an import that would be dropped silently.
        if (sym.name != "module_start" && sym.name != "module_stop") {
            fail("plain name other than module_start or module_stop");
        }
        if (sym.bind != stb_weak || sym.type != stt_notype || sym.shndx != 0) {
            fail("plain name that is not a weak, untyped, undefined symbol");
        }
        sym.kind = Symbol::Kind::plain_weak;
        return;
    }
    if (parts.size() != 3) {
        fail("name is not NID#library#module");
    }
    if (parts[0].size() != nid_length ||
        parts[0].find_first_not_of(base64_digits) != std::string_view::npos) {
        fail("malformed NID");
    }
    auto lib_id = decode_id(parts[1]);
    auto mod_id = decode_id(parts[2]);
    if (!lib_id || !mod_id) {
        fail("library or module id is not one base64 digit");
    }
    bool defined = sym.shndx != 0;
    Library* lib = find_library(info.libraries, defined, *lib_id);
    if (lib == nullptr) {
        fail(std::format("no {} library with id {}", defined ? "exported" : "imported", *lib_id));
    }
    const Module* mod = nullptr;
    if (defined) {
        mod = *mod_id == 0 ? &info.self : nullptr;
    } else if (*mod_id >= 1 && *mod_id <= info.needed.size()) {
        mod = &info.needed[*mod_id - 1];
    }
    if (mod == nullptr) {
        fail(std::format("no {} module with id {}", defined ? "own" : "needed", *mod_id));
    }
    if (lib->module && *lib->module != mod->id) {
        fail(std::format("library {} is used with two modules", lib->name));
    }
    lib->module = mod->id;
    if (!defined && sym.type != stt_func && sym.type != stt_object) {
        fail(std::format("import of symbol type {}", sym.type));
    }
    sym.kind = defined ? Symbol::Kind::exported : Symbol::Kind::import;
    sym.decoded = Import{std::string(parts[0]), lib->name, mod->name, sym.type};
}

// Reads the module and library records into `info`.
void read_records(DynamicInfo& info,
                  std::span<const std::pair<std::uint64_t, std::uint64_t>> entries,
                  const auto& str) {
    std::vector<std::string> needed_files;
    std::vector<std::pair<bool, Record>> attrs;
    for (auto [tag, value] : entries) {
        Record rec = split_record(value);
        if (tag == dt_needed) {
            needed_files.push_back(str(value, "DT_NEEDED"));
        } else if (tag == dt_sce_needed_module) {
            auto same = [&](const Module& m) { return m.id == rec.id; };
            if (rec.id == 0 || std::any_of(info.needed.begin(), info.needed.end(), same)) {
                throw LoadError(
                    std::format("needed module record with id {}: 0 or a duplicate", rec.id));
            }
            info.needed.push_back(Module{rec.id, str(rec.low, "module record"), rec.version, {}});
        } else if (tag == dt_sce_export_lib || tag == dt_sce_import_lib) {
            bool exported = tag == dt_sce_export_lib;
            if (find_library(info.libraries, exported, rec.id) != nullptr) {
                throw LoadError(std::format("duplicate library id {}", rec.id));
            }
            info.libraries.push_back(Library{rec.id, str(rec.low, "library record"), rec.version,
                                             exported, std::nullopt, std::nullopt});
        } else if (tag == dt_sce_export_lib_attr || tag == dt_sce_import_lib_attr) {
            if (rec.version != 0) {
                throw LoadError(
                    std::format("library attribute record {:#x} has bits 47..32 set", value));
            }
            attrs.emplace_back(tag == dt_sce_export_lib_attr, rec);
        }
    }
    for (auto [exported, rec] : attrs) {
        Library* lib = find_library(info.libraries, exported, rec.id);
        if (lib == nullptr || lib->attr) {
            throw LoadError(std::format("unpaired attribute record for library {}", rec.id));
        }
        lib->attr = rec.low;
    }
    // Needed module k is the file of the k-th DT_NEEDED. Module names can differ from the file
    // names, so without that numbering there is no safe pairing.
    std::sort(info.needed.begin(), info.needed.end(),
              [](const Module& a, const Module& b) { return a.id < b.id; });
    if (info.needed.size() != needed_files.size() ||
        (!info.needed.empty() && info.needed.back().id != info.needed.size())) {
        throw LoadError("needed module records are not numbered 1..n in DT_NEEDED order");
    }
    for (std::size_t i = 0; i < info.needed.size(); ++i) {
        info.needed[i].file = needed_files[i];
    }
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
    const Phdr* dynlib = single_phdr(phdrs, pt_sce_dynlibdata, "PT_SCE_DYNLIBDATA");
    const Phdr* dynamic = single_phdr(phdrs, pt_dynamic, "PT_DYNAMIC");
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

    // The value of a tag that may appear at most once.
    auto optional_tag = [&](const Tag& tag) -> std::optional<std::uint64_t> {
        auto same = [&](const auto& entry) { return entry.first == tag.value; };
        if (std::count_if(entries.begin(), entries.end(), same) > 1) {
            throw LoadError(std::format("duplicate {} in the dynamic table", tag.name));
        }
        auto it = std::find_if(entries.begin(), entries.end(), same);
        return it == entries.end() ? std::nullopt : std::optional(it->second);
    };
    // The value of a tag that must appear exactly once.
    auto required = [&](const Tag& tag) {
        auto value = optional_tag(tag);
        if (!value) {
            throw LoadError(std::format("missing {} in the dynamic table", tag.name));
        }
        return *value;
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

    auto str = [&](std::uint64_t offset, std::string_view what) {
        return read_string(strtab, offset, what);
    };

    DynamicInfo info;
    Record self = split_record(required(dt_sce_module_info));
    if (self.id != 0) {
        throw LoadError(std::format("module info record has id {}, expected 0", self.id));
    }
    info.self = Module{0, str(self.low, "module info record"), self.version, {}};
    if (auto soname = optional_tag(dt_soname)) {
        info.soname = str(*soname, "DT_SONAME");
    }
    read_records(info, entries, str);

    for (std::uint64_t i = 0; i < symtab.size() / sym_size; ++i) {
        std::uint64_t at = i * sym_size;
        auto name = read_le<std::uint32_t>(symtab, at, "symbol");
        auto st_info = read_le<std::uint8_t>(symtab, at + 4, "symbol");
        Symbol sym{read_string(strtab, name, "symbol name"),
                   static_cast<std::uint8_t>(st_info & 0xF),
                   static_cast<std::uint8_t>(st_info >> 4),
                   read_le<std::uint16_t>(symtab, at + 6, "symbol"),
                   read_le<std::uint64_t>(symtab, at + 8, "symbol"),
                   Symbol::Kind::none,
                   {}};
        classify(info, sym, i);
        info.symbols.push_back(std::move(sym));
    }
    info.plt_relocs = read_relas(jmprel, info.symbols.size(), "PLT relocations");
    info.relocs = read_relas(relatab, info.symbols.size(), "relocations");
    info.pltgot = required(dt_sce_pltgot);
    info.textrel = optional_tag(dt_textrel).has_value();
    // DT_INIT and DT_FINI point at code.
    auto entry_point = [&](const Tag& tag) {
        auto addr = optional_tag(tag);
        if (addr && !find_load(phdrs, *addr, 1, pf_x)) {
            throw LoadError(
                std::format("{} {:#x} outside the executable PT_LOADs", tag.name, *addr));
        }
        return addr;
    };
    info.init = entry_point(dt_init);
    info.fini = entry_point(dt_fini);
    // A function pointer array has both tags or neither.
    auto func_array = [&](const Tag& addr_tag, const Tag& size_tag) -> std::optional<FuncArray> {
        auto addr = optional_tag(addr_tag), size = optional_tag(size_tag);
        if (addr.has_value() != size.has_value()) {
            throw LoadError(std::format("{} without {}", addr ? addr_tag.name : size_tag.name,
                                        addr ? size_tag.name : addr_tag.name));
        }
        if (!addr) {
            return std::nullopt;
        }
        if (*size % 8 != 0) {
            throw LoadError(std::format("{} {:#x} is not a multiple of 8", size_tag.name, *size));
        }
        if (!find_load(phdrs, *addr, *size)) {
            throw LoadError(std::format("{} {:#x} + {:#x} outside the PT_LOADs", addr_tag.name,
                                        *addr, *size));
        }
        return FuncArray{*addr, *size};
    };
    info.preinit_array = func_array(dt_preinit_array, dt_preinit_arraysz);
    info.init_array = func_array(dt_init_array, dt_init_arraysz);
    info.fini_array = func_array(dt_fini_array, dt_fini_arraysz);
    return info;
}

}  // namespace bb::loader
