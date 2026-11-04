#include "loader/self.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

#include "common/file.hpp"
#include "common/sha256.hpp"
#include "loader/bytes.hpp"
#include "loader/elf.hpp"
#include "support/game.hpp"

namespace {

using bb::loader::LoadError;
using bb::loader::self_to_elf;
using Bytes = std::vector<std::byte>;

template <class T>
void put(Bytes& out, std::size_t offset, T value) {
    if (out.size() < offset + sizeof value) {
        out.resize(offset + sizeof value);
    }
    std::memcpy(out.data() + offset, &value, sizeof value);
}

constexpr std::size_t code_size = 0x400;
constexpr std::uint64_t data_entry = 0x2804;  // signed, blocked, 16 KiB blocks
constexpr std::uint64_t table_entry = 0x10004;

struct Synthetic {
    Bytes elf;
    Bytes self;
};

// A minimal SELF like the ones in docs/formats/self.md: one PT_LOAD with data, a block digest
// table for it, and a PT_SCE_LIBVERSION trailer.
Synthetic build_self() {
    std::string_view libversion = "libversion";
    Bytes elf(0x1000 + code_size + libversion.size());
    std::memcpy(elf.data(), "\x7f" "ELF\x02\x01", 6);  // 64-bit, little-endian
    put<std::uint16_t>(elf, 0x10, bb::loader::et_sce_dynexec);
    put<std::uint16_t>(elf, 0x12, bb::loader::em_x86_64);
    put<std::uint64_t>(elf, 0x20, 0x40);  // e_phoff
    put<std::uint16_t>(elf, 0x34, 0x40);  // e_ehsize
    put<std::uint16_t>(elf, 0x36, 0x38);  // e_phentsize
    put<std::uint16_t>(elf, 0x38, 2);     // e_phnum
    auto phdr = [&](int i, std::uint32_t type, std::uint64_t offset, std::uint64_t size) {
        put<std::uint32_t>(elf, 0x40 + i * 0x38, type);
        put<std::uint64_t>(elf, 0x40 + i * 0x38 + 0x08, offset);
        put<std::uint64_t>(elf, 0x40 + i * 0x38 + 0x20, size);
    };
    phdr(0, bb::loader::pt_load, 0x1000, code_size);
    phdr(1, bb::loader::pt_sce_libversion, 0x1000 + code_size, libversion.size());
    for (std::size_t i = 0; i < code_size; ++i) {
        elf[0x1000 + i] = static_cast<std::byte>(i * 7);
    }
    std::memcpy(elf.data() + 0x1000 + code_size, libversion.data(), libversion.size());

    constexpr std::size_t elf_head = 0x40 + 2 * 0x38;
    constexpr std::size_t ext_off = (0x20 + 2 * 0x20 + elf_head + 15) & ~std::size_t{15};
    constexpr std::size_t header_size = ext_off + 0x40;
    constexpr std::size_t table_off = header_size + 0x40;  // after a zeroed meta area
    constexpr std::size_t data_off = table_off + 0x20;

    Bytes self(data_off);
    std::memcpy(self.data(), "\x4f\x15\x3d\x1d\x00\x01\x01\x12", 8);
    put<std::uint32_t>(self, 0x08, 0x101);
    put<std::uint16_t>(self, 0x0C, header_size);
    put<std::uint16_t>(self, 0x0E, 0x40);  // meta_size
    put<std::uint32_t>(self, 0x10, data_off + code_size);
    put<std::uint16_t>(self, 0x18, 2);
    auto entry = [&](int i, std::uint64_t props, std::uint64_t offset, std::uint64_t size) {
        put(self, 0x20 + i * 0x20, props);
        put(self, 0x28 + i * 0x20, offset);
        put(self, 0x30 + i * 0x20, size);
        put(self, 0x38 + i * 0x20, size);
    };
    entry(0, std::uint64_t{1} << 20 | table_entry, table_off, 0x20);
    entry(1, data_entry, data_off, code_size);
    std::memcpy(self.data() + 0x60, elf.data(), elf_head);
    auto digest = bb::common::sha256(elf);
    std::memcpy(self.data() + ext_off + 0x20, digest.data(), digest.size());
    self.insert(self.end(), elf.begin() + 0x1000, elf.end());  // code, then the trailer
    return {elf, self};
}

TEST(SelfToElf, RebuildsTheElf) {
    auto s = build_self();
    EXPECT_EQ(self_to_elf(s.self), s.elf);
}

TEST(SelfToElf, RejectsWrongMagic) {
    auto s = build_self();
    s.self[0] = std::byte{0};
    EXPECT_THROW(self_to_elf(s.self), LoadError);
}

TEST(SelfToElf, RejectsEncryptedOrCompressedData) {
    for (std::uint64_t flag : {std::uint64_t{2}, std::uint64_t{8}}) {
        auto s = build_self();
        put(s.self, 0x40, data_entry | flag);
        EXPECT_THROW(self_to_elf(s.self), LoadError) << "flag " << flag;
    }
}

TEST(SelfToElf, RejectsEntryForMissingProgramHeader) {
    auto s = build_self();
    put(s.self, 0x40, data_entry | std::uint64_t{2} << 20);
    EXPECT_THROW(self_to_elf(s.self), LoadError);
}

TEST(SelfToElf, RejectsSizeMismatch) {
    auto s = build_self();
    put<std::uint64_t>(s.self, 0x58, code_size - 1);
    EXPECT_THROW(self_to_elf(s.self), LoadError);
}

TEST(SelfToElf, RejectsMissingTrailer) {
    auto s = build_self();
    s.self.resize(s.self.size() - 1);
    EXPECT_THROW(self_to_elf(s.self), LoadError);
}

TEST(SelfToElf, RejectsDigestMismatch) {
    auto s = build_self();
    s.self[s.self.size() - 20] ^= std::byte{1};
    EXPECT_THROW(self_to_elf(s.self), LoadError);
}

TEST(SelfToElf, RejectsEveryTruncation) {
    auto s = build_self();
    for (std::size_t n = 0; n < s.self.size(); ++n) {
        EXPECT_THROW(self_to_elf(std::span(s.self).first(n)), LoadError) << "length " << n;
    }
}

// Corrupt header bytes must give a LoadError or a valid result, never a crash (run under ASan).
TEST(SelfToElf, SurvivesCorruptHeaders) {
    auto good = build_self();
    for (std::size_t i = 0; i < 0x150; ++i) {
        for (std::byte value : {std::byte{0x00}, std::byte{0x80}, std::byte{0xff}}) {
            auto self = good.self;
            self[i] = value;
            try {
                self_to_elf(self);
            } catch (const LoadError&) {
            }
        }
    }
}

TEST(SelfToElfGame, RebuildsThePinnedEboot) {
    BB_REQUIRE_GAME(game);
    auto self = bb::common::read_file(game / "eboot.bin");
    ASSERT_EQ(bb::common::to_hex(bb::common::sha256(self)), bb::loader::eboot_bin_sha256);
    // eboot.elf in PINNED_ELFS of tools/common/ps4elf.py: the output of tools/self2elf.
    EXPECT_EQ(bb::common::to_hex(bb::common::sha256(self_to_elf(self))),
              "941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7");
}

}  // namespace
