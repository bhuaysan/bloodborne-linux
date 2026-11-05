#include "hook/hook.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "loader/elf.hpp"
#include "support/game.hpp"

namespace {

using namespace bb::loader;
using bb::hook::HookError;
using bb::hook::Replacement;
using Bytes = std::vector<std::byte>;

constexpr std::uintptr_t test_base = 0x30000000;
constexpr std::uint64_t code_size = 0x2000, eh_frame_link = 0x1800, eh_hdr_link = 0x1900,
                        dynlib_offset = 0x3000;

// A function of the test ELF: mov eax, value; ret, then int3 up to `size`.
struct Function {
    std::uint64_t start, size;
    std::uint8_t value;
};
// 0x18 lies inside 0x10, as with overlapping FDEs, 0x40 is one byte shorter than a patch, and the
// patch at 0xff8 crosses a page boundary.
constexpr Function functions[] = {
    {0x10, 0x20, 42}, {0x18, 0x18, 9}, {0x40, 0xd, 13}, {0xff8, 0x20, 7}};

template <class T>
void put(Bytes& out, std::size_t offset, T value) {
    if (out.size() < offset + sizeof value) {
        out.resize(offset + sizeof value);
    }
    std::memcpy(out.data() + offset, &value, sizeof value);
}

// .eh_frame with one "zR" CIE and an FDE (pcrel|sdata4) per function, then the terminator, and
// .eh_frame_hdr with the search table (docs/formats/eh-frame.md). The code segment starts at
// file offset 0x1000. Records are padded to a multiple of 4 with DW_CFA_nop (0).
void add_unwind_tables(Bytes& elf) {
    // id 0, version 1, "zR", code align 1, data align -8, return address r16, augmentation
    // length 1, FDE encoding pcrel|sdata4.
    const std::uint8_t cie[16] = {0, 0, 0, 0, 1, 'z', 'R', 0, 1, 0x78, 0x10, 1, 0x1b};
    put<std::uint32_t>(elf, 0x1000 + eh_frame_link, sizeof cie);
    std::memcpy(elf.data() + 0x1000 + eh_frame_link + 4, cie, sizeof cie);
    std::uint64_t pos = eh_frame_link + 4 + sizeof cie, hdr = 0x1000 + eh_hdr_link;
    for (std::size_t i = 0; i < std::size(functions); ++i) {
        const Function& f = functions[i];
        // CIE pointer, pc_begin, pc_range, augmentation length 0, three DW_CFA_nop.
        put<std::uint32_t>(elf, 0x1000 + pos, 16);
        put(elf, 0x1000 + pos + 4, static_cast<std::uint32_t>(pos + 4 - eh_frame_link));
        put(elf, 0x1000 + pos + 8, static_cast<std::int32_t>(f.start - (pos + 8)));
        put(elf, 0x1000 + pos + 12, static_cast<std::uint32_t>(f.size));
        put(elf, hdr + 12 + 8 * i, static_cast<std::int32_t>(f.start - eh_hdr_link));
        put(elf, hdr + 16 + 8 * i, static_cast<std::int32_t>(pos - eh_hdr_link));
        pos += 20;
    }
    put<std::uint32_t>(elf, 0x1000 + pos, 0);
    const std::uint8_t encodings[] = {1, 0x1b, 0x03, 0x3b};
    std::memcpy(elf.data() + hdr, encodings, 4);
    put(elf, hdr + 4, static_cast<std::int32_t>(eh_frame_link - eh_hdr_link - 4));
    put(elf, hdr + 8, static_cast<std::uint32_t>(std::size(functions)));
}

// A PS4 ELF with one code segment at link address 0 that holds the functions and their unwind
// tables, and the smallest dynamic table parse_dynamic accepts.
Bytes build_elf() {
    Bytes elf(dynlib_offset);
    std::memcpy(elf.data(), "\x7f" "ELF\x02\x01", 6);
    put<std::uint16_t>(elf, 0x10, et_sce_dynexec);
    put<std::uint16_t>(elf, 0x12, em_x86_64);
    put<std::uint64_t>(elf, 0x18, 0x10);  // e_entry
    put<std::uint64_t>(elf, 0x20, 0x40);  // e_phoff
    put<std::uint16_t>(elf, 0x36, 0x38);  // e_phentsize
    std::fill(elf.begin() + 0x1000, elf.begin() + 0x1000 + eh_frame_link, std::byte{0xcc});
    for (const Function& f : functions) {
        const std::uint8_t code[] = {0xb8, f.value, 0, 0, 0, 0xc3};  // mov eax, value; ret
        std::memcpy(elf.data() + 0x1000 + f.start, code, sizeof code);
    }
    add_unwind_tables(elf);

    // DynamicInfo needs only the module info record and the (here empty) tables.
    std::size_t dyn = dynlib_offset + 8;
    elf.resize(dyn);
    std::memcpy(elf.data() + dynlib_offset, "\0self\0\0", 8);
    const std::pair<std::uint64_t, std::uint64_t> entries[] = {
        {0x6100000D, 0x0101ULL << 32 | 1},  // DT_SCE_MODULE_INFO "self"
        {0x61000035, 0}, {0x61000037, 6},   // string table
        {0x61000039, 0}, {0x6100003F, 0}, {0x6100003B, 24},  // symbols
        {0x61000029, 0}, {0x6100002D, 0}, {0x6100002B, 7},   // PLT relocations
        {0x6100002F, 0}, {0x61000031, 0}, {0x61000033, 24},  // relocations
        {0x61000027, 0}, {0, 0}};                             // DT_SCE_PLTGOT, DT_NULL
    for (auto [tag, value] : entries) {
        put(elf, elf.size(), tag);
        put(elf, elf.size(), value);
    }

    const std::uint64_t hdr_size = 12 + 8 * std::size(functions);
    const Phdr phdrs[] = {
        {pt_load, pf_r | pf_x, 0x1000, 0, code_size, code_size, 0x4000},
        {pt_sce_dynlibdata, pf_r, dynlib_offset, 0, elf.size() - dynlib_offset, 0, 0x10},
        {pt_dynamic, pf_r, dyn, 0, elf.size() - dyn, elf.size() - dyn, 8},
        {pt_gnu_eh_frame, pf_r, 0x1000 + eh_hdr_link, eh_hdr_link, hdr_size, hdr_size, 4},
    };
    put<std::uint16_t>(elf, 0x38, static_cast<std::uint16_t>(std::size(phdrs)));
    for (std::size_t i = 0; i < std::size(phdrs); ++i) {
        const Phdr& ph = phdrs[i];
        std::size_t at = 0x40 + i * phdr_size;
        put(elf, at, ph.type);
        put(elf, at + 0x04, ph.flags);
        put(elf, at + 0x08, ph.offset);
        put(elf, at + 0x10, ph.vaddr);
        put(elf, at + 0x20, ph.filesz);
        put(elf, at + 0x28, ph.memsz);
        put(elf, at + 0x30, ph.align);
    }
    return elf;
}

// The permissions ("r-xp", ...) of the mapping that holds `addr`, from /proc/self/maps.
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

// Expects `f` to throw a HookError whose message contains `message`.
template <class F>
void expect_refusal(F f, std::string_view message) {
    try {
        f();
        ADD_FAILURE() << "no error, expected: " << message;
    } catch (const HookError& e) {
        EXPECT_NE(std::string_view(e.what()).find(message), std::string_view::npos) << e.what();
    }
}

using Patch = std::array<std::byte, bb::hook::patch_size>;

Patch bytes_at(const Image& image, std::uint64_t link) {
    Patch out;
    std::memcpy(out.data(), reinterpret_cast<const void*>(image.base + link), out.size());
    return out;
}

int replacement_a() {
    return 1001;
}

int replacement_b() {
    return 1002;
}

int registered() {
    return 1003;
}

BB_REPLACE(0x10, registered);

TEST(Hook, RunsTheReplacementUntilRemoved) {
    Image image = map_elf(build_elf(), test_base);
    image.finalize_protection();
    EXPECT_EQ(image.fn<int()>(0x10)(), 42);
    Patch original = bytes_at(image, 0x10);
    const Replacement hooks[] = {{0x10, reinterpret_cast<void*>(&replacement_a), "a"},
                                 {0xff8, reinterpret_cast<void*>(&replacement_b), "b"}};
    bb::hook::apply(image, hooks);
    EXPECT_EQ(image.fn<int()>(0x10)(), 1001);
    EXPECT_EQ(image.fn<int()>(0xff8)(), 1002);
    Patch patch = bytes_at(image, 0x10);
    const std::uint8_t jmp[] = {0xff, 0x25, 0, 0, 0, 0};
    EXPECT_EQ(std::memcmp(patch.data(), jmp, sizeof jmp), 0);
    void* target;
    std::memcpy(&target, patch.data() + 6, sizeof target);
    EXPECT_EQ(target, reinterpret_cast<void*>(&replacement_a));

    bb::hook::remove(image, hooks);
    EXPECT_EQ(std::memcmp(bytes_at(image, 0x10).data(), original.data(), original.size()), 0);
    EXPECT_EQ(image.fn<int()>(0x10)(), 42);
    EXPECT_EQ(image.fn<int()>(0xff8)(), 7);
}

TEST(Hook, KeepsTheProtections) {
    Image image = map_elf(build_elf(), test_base);
    const Replacement hooks[] = {{0x10, reinterpret_cast<void*>(&replacement_a), "a"},
                                 {0xff8, reinterpret_cast<void*>(&replacement_b), "b"}};
    // Before finalize_protection the code is still writable and is written directly.
    bb::hook::apply(image, hooks);
    EXPECT_EQ(protection(test_base), "rw-p");
    EXPECT_EQ(protection(test_base + 0x1000), "rw-p");
    image.finalize_protection();
    EXPECT_EQ(image.fn<int()>(0xff8)(), 1002);
    bb::hook::remove(image, hooks);
    EXPECT_EQ(image.fn<int()>(0xff8)(), 7);
    for (std::uintptr_t page : {test_base, test_base + 0x1000}) {
        EXPECT_EQ(protection(page), "r-xp") << page;
    }
    bb::hook::apply(image, hooks);
    for (std::uintptr_t page : {test_base, test_base + 0x1000}) {
        EXPECT_EQ(protection(page), "r-xp") << page;
    }
    bb::hook::remove(image, hooks);
}

TEST(Hook, RefusesBadEntries) {
    Image image = map_elf(build_elf(), test_base);
    image.finalize_protection();
    auto* a = reinterpret_cast<void*>(&replacement_a);
    auto apply = [&](std::initializer_list<Replacement> hooks) {
        bb::hook::apply(image, {hooks.begin(), hooks.size()});
    };
    expect_refusal([&] { apply({{0x40, a, "short"}}); },
                   "hook short at 0x40: the function is 13 bytes, a hook needs 14");
    expect_refusal([&] { apply({{0x11, a, "inside"}}); }, "hook inside at 0x11: not an FDE start");
    expect_refusal([&] { apply({{0x10, nullptr, "null"}}); }, "the replacement is null");
    expect_refusal([&] { apply({{0x1'0000'0000, a, "far"}}); },
                   "the patch lies outside the executable segments");
    expect_refusal([&] { apply({{0xff8, a, "one"}, {0xff8, a, "two"}}); },
                   "hook two at 0xff8: double hook: the patch overlaps another entry of the list");
    expect_refusal([&] { apply({{0x10, a, "one"}, {0x18, a, "two"}}); }, "hook two at 0x18");
    // Overlapping entries that are not next to each other in the list, out of address order.
    expect_refusal([&] { apply({{0x18, a, "one"}, {0xff8, a, "two"}, {0x10, a, "three"}}); },
                   "hook three at 0x10: double hook: the patch overlaps another entry");
    expect_refusal([&] { bb::hook::remove(image, std::array{Replacement{0x10, a, "a"}}); },
                   "hook a at 0x10: not hooked");

    // A refused list patches nothing, not even its good entries.
    Patch original = bytes_at(image, 0x10);
    expect_refusal([&] { apply({{0x10, a, "good"}, {0x40, a, "short"}}); }, "hook short");
    EXPECT_EQ(std::memcmp(bytes_at(image, 0x10).data(), original.data(), original.size()), 0);

    apply({{0x10, a, "first"}});
    expect_refusal([&] { apply({{0x10, a, "second"}}); },
                   "hook second at 0x10: double hook: the patch overlaps the hook at 0x10");
    expect_refusal([&] { apply({{0x18, a, "near"}}); }, "the patch overlaps the hook at 0x10");
    // A refused remove restores nothing, not even its hooked entries.
    expect_refusal([&] { bb::hook::remove(image, std::array{Replacement{0x10, a, "first"},
                                                            Replacement{0xff8, a, "b"}}); },
                   "hook b at 0xff8: not hooked");
    EXPECT_EQ(image.fn<int()>(0x10)(), 1001);
    bb::hook::remove(image, std::array{Replacement{0x10, a, "first"}});
}

TEST(Hook, RefusesABrokenFde) {
    // The FDE of 0x10 (the first one, after the 20-byte CIE) gets a pc_begin of 0.
    Bytes elf = build_elf();
    put<std::int32_t>(elf, 0x1000 + eh_frame_link + 20 + 8, 0);
    Image image = map_elf(elf, test_base);
    const Replacement hook[] = {{0x10, reinterpret_cast<void*>(&replacement_a), "a"}};
    expect_refusal([&] { bb::hook::apply(image, hook); }, "hook a at 0x10: FDE at 0x1814");
}

TEST(Hook, RegistersWithTheMacro) {
    auto all = bb::hook::replacements();
    auto it = std::find_if(all.begin(), all.end(), [](const Replacement& r) {
        return std::string_view(r.name) == "registered";
    });
    ASSERT_NE(it, all.end());
    EXPECT_EQ(it->link_addr, 0x10U);
    EXPECT_EQ(it->impl, reinterpret_cast<void*>(&registered));
}

TEST(Hook, ForgetsTheHooksOfAnUnmappedImage) {
    const Replacement hook[] = {{0x10, reinterpret_cast<void*>(&replacement_a), "a"}};
    {
        Image gone = map_elf(build_elf(), test_base);
        bb::hook::apply(gone, hook);
    }
    Image image = map_elf(build_elf(), test_base);
    expect_refusal([&] { bb::hook::remove(image, hook); }, "not hooked");
    bb::hook::apply(image, hook);
    image.finalize_protection();
    EXPECT_EQ(image.fn<int()>(0x10)(), 1001);
    bb::hook::remove(image, hook);
}

TEST(HookGame, PatchesAnEbootFunction) {
    BB_REQUIRE_GAME(game);
    Image image = map_eboot(game);
    image.finalize_protection();
    // 0xF0 is 0x77 bytes long and starts with c7 47 08 01 (mov dword [rdi+8], 1).
    Patch original = bytes_at(image, 0xF0);
    const std::uint8_t start[] = {0xc7, 0x47, 0x08, 0x01};
    ASSERT_EQ(std::memcmp(original.data(), start, sizeof start), 0);

    // The entry jumps straight to the replacement, so no game code runs.
    const Replacement hook[] = {{0xF0, reinterpret_cast<void*>(&replacement_a), "a"}};
    bb::hook::apply(image, hook);
    EXPECT_EQ(image.fn<int()>(0xF0)(), 1001);
    bb::hook::remove(image, hook);
    EXPECT_EQ(std::memcmp(bytes_at(image, 0xF0).data(), original.data(), original.size()), 0);
    EXPECT_EQ(protection(default_base + 0xF0), "r-xp");

    const Replacement too_short[] = {{0x34F0, reinterpret_cast<void*>(&replacement_a), "s"}};
    expect_refusal([&] { bb::hook::apply(image, too_short); },
                   "hook s at 0x34f0: the function is 3 bytes, a hook needs 14");
}

}  // namespace
