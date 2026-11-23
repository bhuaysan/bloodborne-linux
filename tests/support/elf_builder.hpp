// Writers for the synthetic PS4 ELFs of the loader, hook and harness tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "loader/elf.hpp"

namespace bb::test {

using Bytes = std::vector<std::byte>;

// Writes `value` at `offset`, growing `out` when it ends past its end.
template <class T>
void put(Bytes& out, std::size_t offset, T value) {
    if (out.size() < offset + sizeof value) {
        out.resize(offset + sizeof value);
    }
    std::memcpy(out.data() + offset, &value, sizeof value);
}

// Writes the ELF header (64-bit little-endian, x86-64, file type `type`, entry point `entry`) and
// `phdrs` right behind it.
void write_headers(Bytes& elf, std::span<const loader::Phdr> phdrs, std::uint64_t entry = 0,
                   std::uint16_t type = loader::et_sce_dynexec);

// Truncates `elf` to `dynlib_offset` and appends a PT_SCE_DYNLIBDATA blob with the smallest
// dynamic table parse_dynamic accepts: the module info record "self", empty string, symbol and
// relocation tables, and DT_SCE_PLTGOT `pltgot`. Returns the file offset of the dynamic table,
// which ends at the end of `elf`.
std::size_t append_minimal_dynamic(Bytes& elf, std::size_t dynlib_offset, std::uint64_t pltgot);

// A function the unwind tables cover.
struct UnwindFunction {
    std::uint64_t start, size;
    bool lsda = false;  // its FDE uses the "zPLR" CIE and has an LSDA pointer
};

// Where add_unwind_tables puts the tables.
struct UnwindLayout {
    std::uint64_t segment_offset;  // file offset of the code segment, which is at link address 0
    std::uint64_t eh_frame_link, eh_hdr_link;
    bool dead_fde = false;  // add an FDE with raw pc_begin 0 that the search table leaves out
};

// Unwind tables in the code segment (docs/formats/eh-frame.md): .eh_frame with a "zR" CIE, a
// "zPLR" CIE when a function has an LSDA, an FDE (pcrel|sdata4) per function, the dead FDE if
// asked for and the terminator, then .eh_frame_hdr, whose search table lists the functions'
// FDEs in the given order. Records are padded to a multiple of 4 with DW_CFA_nop.
void add_unwind_tables(Bytes& elf, const UnwindLayout& layout,
                       std::span<const UnwindFunction> functions);

// The size of the .eh_frame_hdr add_unwind_tables writes for `count` functions.
constexpr std::uint64_t eh_hdr_size(std::size_t count) {
    return 12 + 8 * count;
}

// The permissions ("r-xp", ...) of the mapping that holds `addr`, from /proc/self/maps, or
// "unmapped".
std::string protection(std::uintptr_t addr);

}  // namespace bb::test
