// Function sizes from the unwind tables: the .eh_frame_hdr search table of PT_GNU_EH_FRAME and
// the FDEs in .eh_frame (docs/formats/eh-frame.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "loader/elf.hpp"

namespace bb::loader {

// The search table of .eh_frame_hdr, checked when the image is mapped.
struct UnwindTable {
    std::uint64_t link_addr;  // start of .eh_frame_hdr
    std::uint32_t fde_count;  // entries in the search table
    // The PT_LOAD whose file data holds .eh_frame_hdr. The FDEs and CIEs must lie there too.
    std::uint64_t segment_link, segment_file_size;
};

// Reads PT_GNU_EH_FRAME, if any. Throws LoadError when there are several, when it doesn't lie
// inside the file data of a readable PT_LOAD, when its encodings are not the ones of every pinned
// file (version 1, eh_frame_ptr pcrel|sdata4, fde_count udata4, table datarel|sdata4), or when
// the search table doesn't fit p_filesz or isn't sorted by strictly ascending start.
std::optional<UnwindTable> read_unwind_table(std::span<const std::byte> elf,
                                             std::span<const Phdr> phdrs);

// The pc_range of the FDE that the search table lists for a function starting exactly at
// `link`, nothing when no entry starts there. `segment` is the file data of the table's segment
// (in the file or in the mapped image). Dead FDEs are never found: they are not in the table.
// Throws LoadError when the listed FDE or its CIE is malformed, lies outside `segment`, has an
// augmentation other than "z" followed by R, L and P, a pointer encoding other than pcrel with
// sdata4, udata4, sdata8, udata8 or absptr size, or doesn't start at `link`.
std::optional<std::uint64_t> fde_range(const UnwindTable& table,
                                       std::span<const std::byte> segment, std::uint64_t link);

}  // namespace bb::loader
