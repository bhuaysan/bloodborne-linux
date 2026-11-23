#include "hook/hook.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "loader/elf.hpp"
#include "support/elf_builder.hpp"

namespace {

using namespace bb::loader;
using bb::hook::HookError;
using bb::hook::Replacement;
using bb::test::Bytes;
using bb::test::protection;
using bb::test::put;

constexpr std::uintptr_t test_base = 0x30000000;
constexpr std::uint64_t code_size = 0x2000, eh_frame_link = 0x1800, eh_hdr_link = 0x1900,
                        dynlib_offset = 0x3000;

// A function of the test ELF: mov eax, value; ret, then int3 up to `size`.
struct Function {
    std::uint64_t start, size;
    std::uint8_t value;
};
// 0x18 lies inside 0x10, as with overlapping FDEs, 0x40 is one byte shorter than a patch, and the
// patch at 0xff8 crosses a page boundary. 0x100 gets the code `summing` instead, and 0x10e lies
// within its prologue.
constexpr Function functions[] = {{0x10, 0x20, 42}, {0x18, 0x18, 9},  {0x40, 0xd, 13},
                                  {0x100, 0x18, 0}, {0x10e, 0x10, 0}, {0xff8, 0x20, 7}};

// mov eax, 1; mov ecx, 2; mov edx, 3 (the prologue a wrapper declares), add eax, ecx;
// add eax, edx; ret: returns 6.
constexpr std::array<std::uint8_t, 15> summing_prologue = {
    0xb8, 1, 0, 0, 0, 0xb9, 2, 0, 0, 0, 0xba, 3, 0, 0, 0};
constexpr std::uint8_t summing_tail[] = {0x01, 0xc8, 0x01, 0xd0, 0xc3};

// A PS4 ELF with one code segment at link address 0 that holds the functions and their unwind
// tables, and the smallest dynamic table parse_dynamic accepts.
Bytes build_elf() {
    Bytes elf(dynlib_offset);
    std::fill(elf.begin() + 0x1000, elf.begin() + 0x1000 + eh_frame_link, std::byte{0xcc});
    for (const Function& f : functions) {
        const std::uint8_t code[] = {0xb8, f.value, 0, 0, 0, 0xc3};  // mov eax, value; ret
        std::memcpy(elf.data() + 0x1000 + f.start, code, sizeof code);
    }
    std::memcpy(elf.data() + 0x1000 + 0x100, summing_prologue.data(), summing_prologue.size());
    std::memcpy(elf.data() + 0x1000 + 0x100 + summing_prologue.size(), summing_tail,
                sizeof summing_tail);
    // One "zR" CIE, then an FDE per function: the FDE of 0x10 is at eh_frame_link + 20.
    std::vector<bb::test::UnwindFunction> unwind;
    for (const Function& f : functions) {
        unwind.push_back({f.start, f.size, false});
    }
    bb::test::add_unwind_tables(elf, {0x1000, eh_frame_link, eh_hdr_link, false}, unwind);
    std::size_t dyn = bb::test::append_minimal_dynamic(elf, dynlib_offset, 0);

    const std::uint64_t hdr_size = bb::test::eh_hdr_size(std::size(functions));
    const Phdr phdrs[] = {
        {pt_load, pf_r | pf_x, 0x1000, 0, code_size, code_size, 0x4000},
        {pt_sce_dynlibdata, pf_r, dynlib_offset, 0, elf.size() - dynlib_offset, 0, 0x10},
        {pt_dynamic, pf_r, dyn, 0, elf.size() - dyn, elf.size() - dyn, 8},
        {pt_gnu_eh_frame, pf_r, 0x1000 + eh_hdr_link, eh_hdr_link, hdr_size, hdr_size, 4},
    };
    bb::test::write_headers(elf, phdrs, 0x10);
    return elf;
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
    // The first entry that overlaps an earlier one is refused, not the first one found by address.
    expect_refusal([&] { apply({{0x10, a, "one"}, {0x1a, a, "two"}, {0x15, a, "three"}}); },
                   "hook two at 0x1a: double hook: the patch overlaps another entry");
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

const Image* wrapped_image = nullptr;

int wrapper() {
    return 100 + reinterpret_cast<int (*)()>(bb::hook::original(*wrapped_image, 0x100))();
}

TEST(Hook, CallsTheOriginalThroughATrampoline) {
    Image image = map_elf(build_elf(), test_base);
    image.finalize_protection();
    wrapped_image = &image;
    EXPECT_EQ(wrapper(), 106);  // not hooked: original() is the function itself
    const Replacement hook[] = {
        {0x100, reinterpret_cast<void*>(&wrapper), "wrapper", summing_prologue}};
    bb::hook::apply(image, hook);
    EXPECT_EQ(image.fn<int()>(0x100)(), 106);
    void* trampoline = bb::hook::original(image, 0x100);
    EXPECT_NE(trampoline, reinterpret_cast<void*>(image.base + 0x100));
    EXPECT_EQ(protection(reinterpret_cast<std::uintptr_t>(trampoline)), "r-xp");
    EXPECT_EQ(reinterpret_cast<int (*)()>(trampoline)(), 6);
    bb::hook::remove(image, hook);
    EXPECT_EQ(protection(reinterpret_cast<std::uintptr_t>(trampoline)), "unmapped");
    EXPECT_EQ(bb::hook::original(image, 0x100), reinterpret_cast<void*>(image.base + 0x100));
    EXPECT_EQ(image.fn<int()>(0x100)(), 6);

    const Replacement plain[] = {{0x10, reinterpret_cast<void*>(&replacement_a), "a"}};
    bb::hook::apply(image, plain);
    expect_refusal([&] { bb::hook::original(image, 0x10); },
                   "hook at 0x10: no prologue, so no original");
    bb::hook::remove(image, plain);

    // A hook inside the prologue would change the bytes the trampoline jumps back behind.
    bb::hook::apply(image, hook);
    const Replacement inside[] = {{0x10e, reinterpret_cast<void*>(&replacement_a), "inside"}};
    expect_refusal([&] { bb::hook::apply(image, inside); },
                   "hook inside at 0x10e: double hook: the patch overlaps the hook at 0x100");
    bb::hook::remove(image, hook);
    wrapped_image = nullptr;
}

TEST(Hook, RefusesBadPrologues) {
    Image image = map_elf(build_elf(), test_base);
    image.finalize_protection();
    auto* w = reinterpret_cast<void*>(&wrapper);
    auto apply = [&](std::span<const std::uint8_t> prologue) {
        const Replacement hook[] = {{0x100, w, "w", prologue}};
        bb::hook::apply(image, hook);
    };
    expect_refusal([&] { apply(std::span(summing_prologue).first(10)); },
                   "hook w at 0x100: a prologue of 10 bytes, it needs 14 to 32 within the "
                   "function's 24");
    std::array<std::uint8_t, 25> long_one{};
    std::copy(summing_prologue.begin(), summing_prologue.end(), long_one.begin());
    expect_refusal([&] { apply(long_one); }, "a prologue of 25 bytes");
    auto changed = summing_prologue;
    changed[11] = 4;
    expect_refusal([&] { apply(changed); }, "the prologue differs from the bytes in memory");
    EXPECT_EQ(image.fn<int()>(0x100)(), 6);  // nothing patched
}

// A stub for a patch inside `summing` at 0x105, in place of mov ecx, 2; mov edx, 3; add eax, ecx;
// add eax, edx: the same with 20 and 30, then back to the ret at 0x113. The function returns 51.
// With hook_test_stub_skip set it skips those adds and jumps to the ret at 0x113 through
// hook_test_stub_far instead; the function then returns 0.
extern "C" {
std::uint64_t hook_test_stub_back = 0;
std::uint64_t hook_test_stub_far = 0;
std::uint8_t hook_test_stub_skip = 0;
void hook_test_stub();
}
asm(R"(
    .pushsection .text
    .globl hook_test_stub
    .intel_syntax noprefix
hook_test_stub:
    cmp byte ptr [rip + hook_test_stub_skip], 0
    je 1f
    xor eax, eax
    jmp qword ptr [rip + hook_test_stub_far]
1:
    mov ecx, 20
    mov edx, 30
    add eax, ecx
    add eax, edx
    jmp qword ptr [rip + hook_test_stub_back]
    .att_syntax prefix
    .popsection
)");

// The 14 bytes at 0x105 that the stub takes over.
constexpr std::array<std::uint8_t, 14> summing_middle = {
    0xb9, 2, 0, 0, 0, 0xba, 3, 0, 0, 0, 0x01, 0xc8, 0x01, 0xd0};

Replacement inside_patch(std::uint64_t function, std::uint64_t link,
                         std::span<const std::uint8_t> patched, const char* name = "stub") {
    return Replacement{link,     reinterpret_cast<void*>(&hook_test_stub),
                       name,     patched,
                       function, &hook_test_stub_back};
}

TEST(Hook, PatchesInsideAFunction) {
    Image image = map_elf(build_elf(), test_base);
    image.finalize_protection();
    hook_test_stub_back = 0;
    const Replacement patch[] = {inside_patch(0x100, 0x105, summing_middle)};
    bb::hook::apply(image, patch);
    EXPECT_EQ(hook_test_stub_back, image.base + 0x113);  // apply filled the continuation
    EXPECT_EQ(image.fn<int()>(0x100)(), 51);
    Patch written = bytes_at(image, 0x105);
    const std::uint8_t jmp[] = {0xff, 0x25, 0, 0, 0, 0};
    EXPECT_EQ(std::memcmp(written.data(), jmp, sizeof jmp), 0);
    EXPECT_EQ(protection(test_base + 0x1000), "r-xp");
    expect_refusal([&] { bb::hook::original(image, 0x105); },
                   "hook at 0x105: a patch inside a function has no original");
    bb::hook::remove(image, patch);
    EXPECT_EQ(std::memcmp(bytes_at(image, 0x105).data(), summing_middle.data(), 14), 0);
    EXPECT_EQ(image.fn<int()>(0x100)(), 6);
}

TEST(Hook, RefusesBadInsidePatches) {
    Image image = map_elf(build_elf(), test_base);
    image.finalize_protection();
    auto apply = [&](std::initializer_list<Replacement> hooks) {
        bb::hook::apply(image, {hooks.begin(), hooks.size()});
    };
    expect_refusal([&] { apply({inside_patch(0x101, 0x105, summing_middle)}); },
                   "hook stub at 0x105: the function 0x101 is not an FDE start");
    expect_refusal([&] { apply({inside_patch(0x100, 0x100, summing_middle)}); },
                   "the patched bytes don't lie inside the function 0x100 after its entry "
                   "(24 bytes)");
    expect_refusal([&] { apply({inside_patch(0x100, 0x10f, summing_middle)}); },
                   "don't lie inside the function 0x100");
    expect_refusal(
        [&] { apply({inside_patch(0x100, 0x105, std::span(summing_middle).first(13))}); },
                   "13 patched bytes, a patch inside a function takes 14 to 64");
    std::array<std::uint8_t, 65> long_one{};
    expect_refusal([&] { apply({inside_patch(0x100, 0x105, long_one)}); }, "65 patched bytes");
    auto changed = summing_middle;
    changed[6] = 4;
    expect_refusal([&] { apply({inside_patch(0x100, 0x105, changed)}); },
                   "the patched bytes differ from the bytes in memory");
    const Replacement no_stub[] = {
        {0x105, nullptr, "null", summing_middle, 0x100, &hook_test_stub_back}};
    expect_refusal([&] { bb::hook::apply(image, no_stub); }, "the replacement is null");
    EXPECT_EQ(image.fn<int()>(0x100)(), 6);  // nothing patched

    // Its span counts for overlaps: in the list, and against live hooks either way round.
    auto* a = reinterpret_cast<void*>(&replacement_a);
    expect_refusal(
        [&] { apply({inside_patch(0x100, 0x105, summing_middle), {0x10e, a, "e"}}); },
        "hook e at 0x10e: double hook: the patch overlaps another entry of the list");
    expect_refusal(
        [&] { apply({{0x10e, a, "e"}, inside_patch(0x100, 0x105, summing_middle)}); },
        "hook stub at 0x105: double hook: the patch overlaps another entry of the list");
    apply({inside_patch(0x100, 0x105, summing_middle)});
    expect_refusal([&] { apply({{0x10e, a, "e"}}); },
                   "hook e at 0x10e: double hook: the patch overlaps the hook at 0x105");
    bb::hook::remove(image, std::array{inside_patch(0x100, 0x105, summing_middle)});
    wrapped_image = &image;
    apply({{0x100, reinterpret_cast<void*>(&wrapper), "wrapper", summing_prologue}});
    expect_refusal([&] { apply({inside_patch(0x100, 0x105, summing_middle)}); },
                   "hook stub at 0x105: double hook: the patch overlaps the hook at 0x100");
    bb::hook::remove(image, std::array{Replacement{0x100, reinterpret_cast<void*>(&wrapper),
                                                   "wrapper", summing_prologue}});
    wrapped_image = nullptr;
    EXPECT_EQ(image.fn<int()>(0x100)(), 6);  // nothing left patched
}

TEST(Hook, RefusesAPatchWithoutItsContinuation) {
    Image image = map_elf(build_elf(), test_base);
    image.finalize_protection();
    // A patch whose continuation global was never named: its stub would jump through 0.
    Replacement unset = inside_patch(0x100, 0x105, summing_middle, "unset");
    unset.continuation = nullptr;
    // Also when a valid entry comes first: apply patches nothing.
    const Replacement both[] = {{0x10, reinterpret_cast<void*>(&replacement_a), "a"}, unset};
    expect_refusal([&] { bb::hook::apply(image, both); },
                   "hook unset at 0x105: no continuation: the stub would jump back through "
                   "address 0");
    EXPECT_EQ(image.fn<int()>(0x100)(), 6);
    EXPECT_EQ(image.fn<int()>(0x10)(), 42);
    // A valid patch before a refused entry: its continuation is not written either.
    hook_test_stub_back = 0;
    const Replacement then_refused[] = {inside_patch(0x100, 0x105, summing_middle),
                                        {0x10, nullptr, "null"}};
    expect_refusal([&] { bb::hook::apply(image, then_refused); },
                   "hook null at 0x10: the replacement is null");
    EXPECT_EQ(hook_test_stub_back, 0u);
    EXPECT_EQ(image.fn<int()>(0x100)(), 6);
    // A continuation belongs to a patch inside a function only.
    const Replacement at_entry[] = {
        {0x10, reinterpret_cast<void*>(&replacement_a), "a", {}, 0, &hook_test_stub_back}};
    expect_refusal([&] { bb::hook::apply(image, at_entry); },
                   "hook a at 0x10: a continuation or jump targets, but no patch inside a "
                   "function");
}

TEST(Hook, FillsAPatchsJumpTargets) {
    Image image = map_elf(build_elf(), test_base);
    image.finalize_protection();
    hook_test_stub_back = 0;
    hook_test_stub_far = 0;
    const bb::hook::JumpTarget far[] = {{0x113, &hook_test_stub_far}};
    Replacement patch = inside_patch(0x100, 0x105, summing_middle);
    patch.targets = far;
    bb::hook::apply(image, std::array{patch});
    EXPECT_EQ(hook_test_stub_far, image.base + 0x113);
    hook_test_stub_skip = 1;
    EXPECT_EQ(image.fn<int()>(0x100)(), 0);  // the stub jumped through the target
    hook_test_stub_skip = 0;
    EXPECT_EQ(image.fn<int()>(0x100)(), 51);
    bb::hook::remove(image, std::array{patch});

    // A target without a slot, or outside the function (0x100, 24 bytes), is refused.
    const bb::hook::JumpTarget no_slot[] = {{0x113, nullptr}};
    patch.targets = no_slot;
    expect_refusal([&] { bb::hook::apply(image, std::array{patch}); },
                   "hook stub at 0x105: the jump target 0x113 has no slot");
    for (std::uint64_t outside : {0xffu, 0x118u}) {
        const bb::hook::JumpTarget away[] = {{outside, &hook_test_stub_far}};
        patch.targets = away;
        expect_refusal([&] { bb::hook::apply(image, std::array{patch}); },
                       std::format("the jump target {:#x} lies outside the function 0x100",
                                   outside));
    }
    // Nor one behind the patch's first byte inside the patched bytes, which the patch overwrites.
    const bb::hook::JumpTarget mid_patch[] = {{0x106, &hook_test_stub_far}};
    patch.targets = mid_patch;
    expect_refusal([&] { bb::hook::apply(image, std::array{patch}); },
                   "the jump target 0x106 lies inside the patched bytes");
    EXPECT_EQ(image.fn<int()>(0x100)(), 6);
}

}  // namespace
