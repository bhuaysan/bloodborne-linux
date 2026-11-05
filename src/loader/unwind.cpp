#include "loader/unwind.hpp"

#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <string_view>

#include "loader/bytes.hpp"

namespace bb::loader {
namespace {

// version 1, eh_frame_ptr pcrel|sdata4, fde_count udata4, table datarel|sdata4
constexpr std::byte hdr_encodings[] = {std::byte{1}, std::byte{0x1b}, std::byte{0x03},
                                       std::byte{0x3b}};
constexpr std::uint64_t hdr_size = 12, entry_size = 8;
constexpr std::uint8_t pe_pcrel = 0x10, pe_sdata4 = 0x0b;

// The file data of one PT_LOAD, read by link address. Every read is bounds-checked.
struct SegmentData {
    std::span<const std::byte> bytes;
    std::uint64_t link;

    std::uint64_t offset(std::uint64_t addr, std::uint64_t size, std::string_view what) const {
        if (addr < link || !in_bounds(addr - link, size, bytes.size())) {
            throw LoadError(std::format("{} at {:#x} + {:#x} lies outside its segment's file data",
                                        what, addr, size));
        }
        return addr - link;
    }

    template <class T>
    T get(std::uint64_t addr, std::string_view what) const {
        return read_le<T>(bytes, offset(addr, sizeof(T), what), what);
    }

    // Start of search table entry `i`, relative to .eh_frame_hdr.
    std::int32_t start(const UnwindTable& t, std::uint32_t i) const {
        return get<std::int32_t>(t.link_addr + hdr_size + entry_size * i, ".eh_frame_hdr entry");
    }
};

// Size of a DW_EH_PE value format (the low nibble of an encoding), 0 when unsupported.
unsigned value_size(std::uint8_t enc) {
    switch (enc & 0x0f) {
        case 0x00: case 0x04: case 0x0c: return 8;  // absptr, udata8, sdata8
        case 0x03: case pe_sdata4: return 4;        // udata4, sdata4
        default: return 0;
    }
}

// The value of encoding `enc` at `addr`. An sdata4 value is sign-extended when `sign` is set.
std::uint64_t read_value(const SegmentData& d, std::uint64_t addr, std::uint8_t enc, bool sign) {
    if (value_size(enc) == 8) {
        return d.get<std::uint64_t>(addr, "FDE");
    }
    if (sign && (enc & 0x0f) == pe_sdata4) {
        return static_cast<std::uint64_t>(std::int64_t{d.get<std::int32_t>(addr, "FDE")});
    }
    return d.get<std::uint32_t>(addr, "FDE");
}

// The FDE pointer encoding of the CIE at `cie` (augmentation "R", absptr without one).
std::uint8_t fde_encoding(const SegmentData& d, std::uint64_t cie) {
    auto length = d.get<std::uint32_t>(cie, "CIE");
    if (length == 0 || length == 0xffffffff || d.get<std::uint32_t>(cie + 4, "CIE") != 0) {
        throw LoadError(std::format("no supported CIE at {:#x}", cie));
    }
    d.offset(cie, 4 + std::uint64_t{length}, "CIE");
    std::uint64_t end = cie + 4 + length;
    std::uint64_t at = cie + 8;
    auto byte = [&] {
        if (at >= end) {
            throw LoadError(std::format("CIE at {:#x} overruns its length", cie));
        }
        return d.get<std::uint8_t>(at++, "CIE");
    };
    auto skip_leb128 = [&] { while (byte() & 0x80) {} };
    std::uint8_t version = byte();
    std::string aug;
    for (std::uint8_t c; (c = byte()) != 0;) {
        aug += static_cast<char>(c);
    }
    skip_leb128();  // code alignment
    skip_leb128();  // data alignment
    if (version == 1) {
        byte();  // return address register
    } else {
        skip_leb128();
    }
    auto unsupported = [&] {
        return LoadError(std::format("unsupported CIE augmentation \"{}\" at {:#x}", aug, cie));
    };
    std::string_view fields = aug;  // one per augmentation data field
    if (!aug.empty()) {
        if (aug[0] != 'z') {
            throw unsupported();
        }
        skip_leb128();  // augmentation data length
        fields.remove_prefix(1);
    }
    std::uint8_t enc = 0;  // absptr
    for (char c : fields) {
        if (c == 'R') {
            enc = byte();
        } else if (c == 'L') {
            byte();
        } else if (c == 'P') {
            unsigned size = value_size(byte());
            if (size == 0) {
                throw LoadError(std::format("unsupported personality encoding at {:#x}", cie));
            }
            at += size;
        } else {
            throw unsupported();
        }
    }
    if (at > end) {
        throw LoadError(std::format("CIE at {:#x} overruns its length", cie));
    }
    return enc;
}

}  // namespace

std::optional<UnwindTable> read_unwind_table(std::span<const std::byte> elf,
                                             std::span<const Phdr> phdrs) {
    const Phdr* hdr = single_phdr(phdrs, pt_gnu_eh_frame, "PT_GNU_EH_FRAME");
    if (!hdr) {
        return std::nullopt;
    }
    for (const Phdr& load : phdrs) {
        if (load.type != pt_load || hdr->vaddr < load.vaddr ||
            !in_bounds(hdr->vaddr - load.vaddr, hdr->filesz, load.filesz)) {
            continue;
        }
        if (!(load.flags & pf_r)) {
            throw LoadError("PT_GNU_EH_FRAME lies in a segment that is not readable");
        }
        need_bytes(elf, load.offset, load.filesz, "PT_LOAD file data");
        SegmentData d{elf.subspan(load.offset, load.filesz), load.vaddr};
        if (hdr->filesz < hdr_size) {
            throw LoadError(
                std::format("PT_GNU_EH_FRAME of {:#x} bytes is too small", hdr->filesz));
        }
        const std::byte* at = d.bytes.data() + (hdr->vaddr - load.vaddr);
        if (std::memcmp(at, hdr_encodings, sizeof hdr_encodings) != 0) {
            throw LoadError(std::format(
                "unsupported .eh_frame_hdr encodings {:02x} {:02x} {:02x} {:02x}",
                static_cast<int>(at[0]), static_cast<int>(at[1]), static_cast<int>(at[2]),
                static_cast<int>(at[3])));
        }
        UnwindTable t{hdr->vaddr, d.get<std::uint32_t>(hdr->vaddr + 8, "fde_count"), load.vaddr,
                      load.filesz};
        if (hdr_size + entry_size * t.fde_count > hdr->filesz) {
            throw LoadError(std::format("search table of {} entries exceeds PT_GNU_EH_FRAME",
                                        t.fde_count));
        }
        for (std::uint32_t i = 1; i < t.fde_count; ++i) {
            if (d.start(t, i) <= d.start(t, i - 1)) {
                throw LoadError(
                    std::format(".eh_frame_hdr search table is not sorted at entry {}", i));
            }
        }
        return t;
    }
    throw LoadError(std::format("PT_GNU_EH_FRAME at {:#x} + {:#x} lies outside the file data of "
                                "the PT_LOAD segments",
                                hdr->vaddr, hdr->filesz));
}

std::optional<std::uint64_t> fde_range(const UnwindTable& table, std::uintptr_t base,
                                       std::uint64_t link) {
    const auto* data = reinterpret_cast<const std::byte*>(base + table.segment_link);
    SegmentData d{{data, table.segment_file_size}, table.segment_link};
    // Starts are 32-bit offsets from .eh_frame_hdr, so nothing farther away can be listed.
    auto rel = static_cast<std::int64_t>(link - table.link_addr);
    if (rel < std::numeric_limits<std::int32_t>::min() ||
        rel > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    std::uint32_t lo = 0, hi = table.fde_count;
    while (lo < hi) {
        std::uint32_t mid = lo + (hi - lo) / 2;
        if (d.start(table, mid) < rel) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == table.fde_count || d.start(table, lo) != rel) {
        return std::nullopt;
    }

    std::uint64_t fde = table.link_addr + static_cast<std::uint64_t>(static_cast<std::int64_t>(
        d.get<std::int32_t>(table.link_addr + hdr_size + entry_size * lo + 4, "FDE pointer")));
    auto length = d.get<std::uint32_t>(fde, "FDE");
    auto cie_ptr = d.get<std::uint32_t>(fde + 4, "FDE");
    if (length == 0 || length == 0xffffffff || cie_ptr == 0) {
        throw LoadError(std::format("no supported FDE at {:#x} for {:#x}", fde, link));
    }
    std::uint8_t enc = fde_encoding(d, fde + 4 - cie_ptr);
    unsigned size = value_size(enc);
    if (size == 0 || (enc & 0xf0) != pe_pcrel) {
        throw LoadError(std::format("unsupported FDE pointer encoding {:#x} at {:#x}", enc, fde));
    }
    if (8 + 2 * size > 4 + std::uint64_t{length}) {
        throw LoadError(std::format("FDE at {:#x} is too short", fde));
    }
    d.offset(fde, 4 + std::uint64_t{length}, "FDE");
    std::uint64_t begin = fde + 8 + read_value(d, fde + 8, enc, true);  // pcrel: from the field
    if (begin != link) {
        throw LoadError(std::format("FDE at {:#x} starts at {:#x}, the search table says {:#x}",
                                    fde, begin, link));
    }
    return read_value(d, fde + 8 + size, enc, false);
}

}  // namespace bb::loader
