#include "loader/self.hpp"

#include <algorithm>
#include <utility>

#include "common/sha256.hpp"
#include "loader/bytes.hpp"
#include "loader/elf.hpp"

namespace bb::loader {
namespace {

constexpr std::size_t self_header_size = 0x20;
constexpr std::size_t entry_size = 0x20;
constexpr std::size_t ext_info_size = 0x40;  // program identification header
constexpr std::size_t digest_offset = 0x20;  // in the program identification header

constexpr std::uint8_t self_magic[] = {0x4f, 0x15, 0x3d, 0x1d};
constexpr std::uint8_t self_constant[] = {0x00, 0x01, 0x01, 0x12};  // header bytes 4..7

// Segment entry flag bits.
constexpr std::uint64_t entry_encrypted = 1 << 1;
constexpr std::uint64_t entry_compressed = 1 << 3;
constexpr std::uint64_t entry_block_table = 1 << 16;  // per-block digests, not ELF data

// Bounds the allocation for malformed input. PS4 executables are far smaller.
constexpr std::uint64_t max_elf_size = std::uint64_t{1} << 30;

// Whether `data` holds `want` at `offset`. The caller checks the bounds.
bool bytes_equal(std::span<const std::byte> data, std::size_t offset,
                 std::span<const std::uint8_t> want) {
    return std::equal(want.begin(), want.end(), data.begin() + offset,
                      [](std::uint8_t a, std::byte b) { return std::byte{a} == b; });
}

}  // namespace

std::vector<std::byte> self_to_elf(std::span<const std::byte> self) {
    need_bytes(self, 0, self_header_size, "SELF header");
    if (!bytes_equal(self, 0, self_magic)) {
        throw LoadError("not a SELF file (wrong magic)");
    }
    if (!bytes_equal(self, 4, self_constant)) {
        throw LoadError("unexpected SELF header bytes 4..7");
    }
    std::uint64_t header_size = read_le<std::uint16_t>(self, 0x0C, "SELF header");
    std::uint64_t meta_size = read_le<std::uint16_t>(self, 0x0E, "SELF header");
    std::uint64_t file_size = read_le<std::uint32_t>(self, 0x10, "SELF header");
    std::uint64_t num_entries = read_le<std::uint16_t>(self, 0x18, "SELF header");

    std::uint64_t elf_off = self_header_size + num_entries * entry_size;
    need_bytes(self, elf_off, elf_header_size, "embedded ELF header");
    auto embedded = self.subspan(elf_off);
    ElfHeader eh = read_elf_header(embedded);
    if (eh.phoff != elf_header_size || eh.ehsize != elf_header_size) {
        throw LoadError("unexpected ELF header layout");
    }
    if (eh.shoff != 0 || eh.shnum != 0) {
        throw LoadError("section headers are not supported");
    }
    std::vector<Phdr> phdrs = read_phdrs(embedded, eh);
    std::uint64_t head_size = elf_header_size + phdrs.size() * phdr_size;

    std::uint64_t ext_off = (elf_off + head_size + 15) & ~std::uint64_t{15};
    need_bytes(self, ext_off, ext_info_size, "program identification header");
    if (ext_off + ext_info_size > header_size || !in_bounds(header_size, meta_size, self.size())) {
        throw LoadError("SELF header and meta sizes don't fit the file");
    }

    std::uint64_t elf_size = head_size;
    for (std::size_t i = 0; i < phdrs.size(); ++i) {
        if (!in_bounds(phdrs[i].offset, phdrs[i].filesz, max_elf_size)) {
            throw LoadError(
                std::format("program header {} lies beyond {:#x} bytes", i, max_elf_size));
        }
        elf_size = std::max(elf_size, phdrs[i].offset + phdrs[i].filesz);
    }
    std::vector<std::byte> out(elf_size);
    std::copy_n(embedded.begin(), head_size, out.begin());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> covered{{0, head_size}};
    auto place = [&](const Phdr& ph, std::span<const std::byte> bytes) {
        std::copy(bytes.begin(), bytes.end(), out.begin() + static_cast<std::ptrdiff_t>(ph.offset));
        covered.emplace_back(ph.offset, ph.offset + ph.filesz);
    };

    for (std::uint64_t i = 0; i < num_entries; ++i) {
        std::uint64_t at = self_header_size + i * entry_size;
        auto props = read_le<std::uint64_t>(self, at, "segment entry");
        auto offset = read_le<std::uint64_t>(self, at + 0x08, "segment entry");
        auto stored_size = read_le<std::uint64_t>(self, at + 0x10, "segment entry");
        auto plain_size = read_le<std::uint64_t>(self, at + 0x18, "segment entry");
        if (props & entry_block_table) {
            continue;
        }
        if (props & (entry_encrypted | entry_compressed)) {
            throw LoadError(
                std::format("segment entry {} is encrypted or compressed ({:#x})", i, props));
        }
        std::uint64_t index = props >> 20;
        if (index >= phdrs.size()) {
            throw LoadError(std::format("segment entry {} points at program header {}", i, index));
        }
        const Phdr& ph = phdrs[index];
        if (stored_size != ph.filesz || plain_size != ph.filesz) {
            throw LoadError(std::format("segment entry {} size {:#x} != p_filesz {:#x}", i,
                                        stored_size, ph.filesz));
        }
        need_bytes(self, offset, stored_size, "segment data");
        place(ph, self.subspan(offset, stored_size));
    }

    // PT_SCE_LIBVERSION has no entry. Its bytes follow the signed part of the file.
    if (file_size < self.size()) {
        auto trailer = self.subspan(file_size);
        auto is_libversion = [](const Phdr& ph) { return ph.type == pt_sce_libversion; };
        auto count = std::count_if(phdrs.begin(), phdrs.end(), is_libversion);
        if (count != 1) {
            throw LoadError(std::format("{:#x} trailing bytes but {} PT_SCE_LIBVERSION segments",
                                        trailer.size(), count));
        }
        auto libversion = std::find_if(phdrs.begin(), phdrs.end(), is_libversion);
        if (libversion->filesz != trailer.size()) {
            throw LoadError(std::format("{:#x} trailing bytes but PT_SCE_LIBVERSION p_filesz {:#x}",
                                        trailer.size(), libversion->filesz));
        }
        place(*libversion, trailer);
    }

    for (std::size_t i = 0; i < phdrs.size(); ++i) {
        const Phdr& ph = phdrs[i];
        bool ok = ph.filesz == 0 || std::any_of(covered.begin(), covered.end(), [&](auto range) {
            return range.first <= ph.offset && ph.offset + ph.filesz <= range.second;
        });
        if (!ok) {
            throw LoadError(std::format("no data for program header {} (type {:#x})", i, ph.type));
        }
    }

    if (!bytes_equal(self, ext_off + digest_offset, common::sha256(out))) {
        throw LoadError("rebuilt ELF doesn't match the digest stored in the SELF");
    }
    return out;
}

}  // namespace bb::loader
