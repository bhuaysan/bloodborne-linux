#include "support/elf_builder.hpp"

#include <fstream>
#include <initializer_list>
#include <sstream>
#include <utility>

#include "loader/dynamic.hpp"

namespace bb::test {

using namespace bb::loader;

namespace {

Bytes raw(std::initializer_list<std::uint8_t> values) {
    Bytes out;
    for (std::uint8_t v : values) {
        out.push_back(std::byte{v});
    }
    return out;
}

}  // namespace

void write_headers(Bytes& elf, std::span<const Phdr> phdrs, std::uint64_t entry,
                   std::uint16_t type) {
    std::memcpy(elf.data(), "\x7f" "ELF\x02\x01", 6);  // 64-bit, little-endian
    put<std::uint16_t>(elf, 0x10, type);
    put<std::uint16_t>(elf, 0x12, em_x86_64);
    put<std::uint64_t>(elf, 0x18, entry);
    put<std::uint64_t>(elf, 0x20, elf_header_size);  // e_phoff
    put<std::uint16_t>(elf, 0x34, elf_header_size);  // e_ehsize
    put<std::uint16_t>(elf, 0x36, phdr_size);        // e_phentsize
    put<std::uint16_t>(elf, 0x38, static_cast<std::uint16_t>(phdrs.size()));
    for (std::size_t i = 0; i < phdrs.size(); ++i) {
        const Phdr& ph = phdrs[i];
        std::size_t at = elf_header_size + i * phdr_size;
        put(elf, at, ph.type);
        put(elf, at + 0x04, ph.flags);
        put(elf, at + 0x08, ph.offset);
        put(elf, at + 0x10, ph.vaddr);
        put(elf, at + 0x20, ph.filesz);
        put(elf, at + 0x28, ph.memsz);
        put(elf, at + 0x30, ph.align);
    }
}

std::size_t append_minimal_dynamic(Bytes& elf, std::size_t dynlib_offset, std::uint64_t pltgot) {
    // DynamicInfo needs only the module info record and the (here empty) tables.
    std::size_t dyn = dynlib_offset + 8;
    elf.resize(dyn);
    std::memcpy(elf.data() + dynlib_offset, "\0self\0\0", 8);
    const std::pair<std::uint64_t, std::uint64_t> entries[] = {
        {dt_sce_module_info, 0x0101ULL << 32 | 1},  // "self", version 1.1
        {dt_sce_strtab, 0}, {dt_sce_strsz, 6},
        {dt_sce_symtab, 0}, {dt_sce_symtabsz, 0}, {dt_sce_syment, 24},
        {dt_sce_jmprel, 0}, {dt_sce_pltrelsz, 0}, {dt_sce_pltrel, dt_rela},
        {dt_sce_rela, 0}, {dt_sce_relasz, 0}, {dt_sce_relaent, 24},
        {dt_sce_pltgot, pltgot}, {0, 0}};  // DT_NULL
    for (auto [tag, value] : entries) {
        put(elf, elf.size(), tag);
        put(elf, elf.size(), value);
    }
    return dyn;
}

void add_unwind_tables(Bytes& elf, const UnwindLayout& layout,
                       std::span<const UnwindFunction> functions) {
    const std::uint64_t base = layout.segment_offset;
    std::uint64_t pos = layout.eh_frame_link;
    auto record = [&](Bytes body) {
        body.resize((body.size() + 3) / 4 * 4);  // DW_CFA_nop padding
        std::uint64_t at = pos;
        put(elf, base + at, static_cast<std::uint32_t>(body.size()));
        std::memcpy(elf.data() + base + at + 4, body.data(), body.size());
        pos += 4 + body.size();
        return at;
    };
    // id 0, version 1, augmentation, code align 1, data align -8, return address r16, then the
    // augmentation data: length, personality (indirect pcrel sdata4), LSDA and FDE encodings.
    std::uint64_t cie_r = record(raw({0, 0, 0, 0, 1, 'z', 'R', 0, 1, 0x78, 0x10, 1, 0x1b}));
    std::uint64_t cie_plr = 0;
    for (const UnwindFunction& f : functions) {
        if (f.lsda) {
            cie_plr = record(raw({0, 0, 0, 0, 1, 'z', 'P', 'L', 'R', 0, 1, 0x78, 0x10, 7, 0x9b, 0,
                                  0, 0, 0, 0x1b, 0x1b}));
            break;
        }
    }
    // CIE pointer, pc_begin, pc_range, augmentation length (4 for the zeroed LSDA pointer).
    auto fde = [&](std::uint64_t cie, std::uint64_t start, std::uint64_t size, bool lsda) {
        Bytes body(lsda ? 17 : 13);
        put(body, 0, static_cast<std::uint32_t>(pos + 4 - cie));
        put(body, 4, static_cast<std::uint32_t>(start - (pos + 8)));
        put(body, 8, static_cast<std::uint32_t>(size));
        body[12] = std::byte{lsda ? std::uint8_t{4} : std::uint8_t{0}};
        return record(body);
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> table;  // (function, FDE)
    for (const UnwindFunction& f : functions) {
        table.emplace_back(f.start, fde(f.lsda ? cie_plr : cie_r, f.start, f.size, f.lsda));
    }
    if (layout.dead_fde) {
        fde(cie_r, pos + 8, 0x40, false);  // raw pc_begin 0
    }
    put<std::uint32_t>(elf, base + pos, 0);

    const std::uint64_t hdr = base + layout.eh_hdr_link;
    const Bytes encodings = raw({1, 0x1b, 0x03, 0x3b});
    std::memcpy(elf.data() + hdr, encodings.data(), encodings.size());
    put(elf, hdr + 4,
        static_cast<std::int32_t>(layout.eh_frame_link - layout.eh_hdr_link - 4));
    put(elf, hdr + 8, static_cast<std::uint32_t>(table.size()));
    for (std::size_t i = 0; i < table.size(); ++i) {
        put(elf, hdr + 12 + 8 * i, static_cast<std::int32_t>(table[i].first - layout.eh_hdr_link));
        put(elf, hdr + 16 + 8 * i,
            static_cast<std::int32_t>(table[i].second - layout.eh_hdr_link));
    }
}

std::string protection(std::uintptr_t addr) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream in(line);
        std::uintptr_t start, end;
        char dash;
        std::string perms;
        in >> std::hex >> start >> dash >> end >> perms;
        if (start <= addr && addr < end) {
            return perms;
        }
    }
    return "unmapped";
}

}  // namespace bb::test
