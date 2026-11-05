#include "hle/kernel/tls.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "hle/link.hpp"
#include "loader/load_set.hpp"
#include "loader/relocate.hpp"
#include "support/game.hpp"

namespace {

using namespace bb::hle::kernel;

// ---- the eboot's static block (this test links bbeboottls) ------------------------------------

void expect_reservation_ends_at_thread_pointer() {
    const auto placement = eboot_tls_placement();
    ASSERT_TRUE(placement);
    EXPECT_EQ(placement->thread_pointer, thread_pointer());
    EXPECT_EQ(placement->block + eboot_tls_size, placement->thread_pointer);
    EXPECT_EQ(placement->block % eboot_tls_align, 0U);
    check_eboot_tls();  // aborts otherwise
}

// Reads the eboot's block the way the eboot does: fs:0 plus a negative offset.
std::uint64_t read_fs_relative(std::int64_t offset) {
    std::uint64_t value;
    asm volatile("mov %%fs:0, %%rax\n\tmov (%%rax,%1), %0" : "=r"(value) : "r"(offset) : "rax");
    return value;
}

TEST(EbootTls, ReservationEndsAtThreadPointerOnTheMainThread) {
    expect_reservation_ends_at_thread_pointer();
}

TEST(EbootTls, ReservationEndsAtThreadPointerOnANewThread) {
    std::thread([] { expect_reservation_ends_at_thread_pointer(); }).join();
}

// Each thread has its own block, zero-filled like the eboot's template (no file bytes).
TEST(EbootTls, EachThreadHasItsOwnZeroFilledBlock) {
    const auto block = reinterpret_cast<std::uint64_t*>(eboot_tls_placement()->block);
    block[0] = 0x1111;
    block[eboot_tls_size / 8 - 1] = 0x2222;
    EXPECT_EQ(read_fs_relative(-static_cast<std::int64_t>(eboot_tls_size)), 0x1111U);
    EXPECT_EQ(read_fs_relative(-8), 0x2222U);
    std::thread([] {
        const auto* bytes = reinterpret_cast<const std::byte*>(eboot_tls_placement()->block);
        EXPECT_TRUE(std::all_of(bytes, bytes + eboot_tls_size,
                                [](std::byte b) { return b == std::byte{0}; }));
    }).join();
    block[0] = 0;
    block[eboot_tls_size / 8 - 1] = 0;
}

TEST(EbootTls, ReservationIsSetOnce) {
    EXPECT_THROW(set_eboot_tls_reservation(+[] { return std::uintptr_t{0}; }), std::logic_error);
}

// ---- the TLS helper ----------------------------------------------------------------------------

// A template of 8 file bytes in a 0x40-byte block with alignment 16, linked at 8 mod 16.
constexpr std::uint64_t synthetic_id = 100;
constexpr std::array<std::byte, 8> synthetic_init = {std::byte{1}, std::byte{2}, std::byte{3},
                                                     std::byte{4}, std::byte{5}, std::byte{6},
                                                     std::byte{7}, std::byte{8}};
constexpr bb::loader::TlsTemplate synthetic_tls{0x1008, 8, 0x40, 16};

void register_synthetic() {
    static const bool registered =
        (register_tls_module(synthetic_id, synthetic_init.data(), synthetic_tls), true);
    (void)registered;
}

std::byte* helper(std::uint64_t module, std::uint64_t offset) {
    const std::array<std::uint64_t, 2> pair = {module, offset};
    return static_cast<std::byte*>(__tls_get_addr(pair.data()));
}

// The block holds the template's file bytes followed by zeros and keeps its link alignment.
void expect_fresh_block(const std::byte* block) {
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(block) % 16, 8U);
    EXPECT_EQ(std::memcmp(block, synthetic_init.data(), synthetic_init.size()), 0);
    EXPECT_TRUE(
        std::all_of(block + 8, block + 0x40, [](std::byte b) { return b == std::byte{0}; }));
}

TEST(TlsHelper, TwoThreadsGetDifferentBlocksFromTheTemplate) {
    register_synthetic();
    std::byte* first = nullptr;
    std::byte* second = nullptr;
    std::thread([&] {
        first = helper(synthetic_id, 0);
        expect_fresh_block(first);
        EXPECT_EQ(helper(synthetic_id, 0x10), first + 0x10);  // the same block again
        first[0] = std::byte{0xaa};  // not seen by the other thread
        std::thread([&] {
            second = helper(synthetic_id, 0);
            expect_fresh_block(second);
        }).join();
    }).join();
    EXPECT_NE(first, nullptr);
    EXPECT_NE(first, second);
}

TEST(TlsHelper, BlocksAreFreedAtThreadExit) {
    register_synthetic();
    const std::size_t before = live_tls_blocks();
    std::thread([&] {
        helper(synthetic_id, 0);
        helper(synthetic_id, 0x3f);
        EXPECT_EQ(live_tls_blocks(), before + 1);
    }).join();
    EXPECT_EQ(live_tls_blocks(), before);
}

// libSceFios2 passes pairs that are only 4-byte aligned.
TEST(TlsHelper, ReadsAMisalignedPair) {
    register_synthetic();
    alignas(8) std::array<std::byte, 20> buffer{};
    const std::uint64_t pair[2] = {synthetic_id, 0x20};
    std::memcpy(buffer.data() + 4, pair, sizeof pair);
    EXPECT_EQ(static_cast<std::byte*>(__tls_get_addr(buffer.data() + 4)),
              helper(synthetic_id, 0) + 0x20);
}

TEST(TlsHelper, RegisteringThrowsForADuplicateOrABadTemplate) {
    register_synthetic();
    EXPECT_THROW(register_tls_module(synthetic_id, synthetic_init.data(), synthetic_tls),
                 std::logic_error);
    EXPECT_THROW(register_tls_module(bb::loader::eboot_module_id, nullptr, synthetic_tls),
                 std::invalid_argument);
    EXPECT_THROW(register_tls_module(101, nullptr, {0, 0, 0x40, 12}), std::invalid_argument);
    EXPECT_THROW(register_tls_module(101, nullptr, {0, 0x41, 0x40, 16}), std::invalid_argument);
}

TEST(TlsHelperDeathTest, AbortsForAnUnknownModule) {
    EXPECT_DEATH(helper(99, 0), "no TLS template registered for the module \\(module 99");
}

TEST(TlsHelperDeathTest, AbortsForAnOffsetOutsideTheBlock) {
    register_synthetic();
    EXPECT_DEATH(helper(synthetic_id, 0x40), "offset outside the module's block");
}

TEST(TlsHelper, IsTheLibkernelImplementation) {
    const bb::loader::Import imp{"vNe1w4diLCs", "libkernel", "libkernel", 2 /* STT_FUNC */};
    EXPECT_EQ(bb::hle::resolve(imp), reinterpret_cast<std::uintptr_t>(&__tls_get_addr));
}

// ---- the game ----------------------------------------------------------------------------------

// libc.prx (module 3) has 0x220 file bytes in a 0x4a0-byte block, libSceFios2.prx (module 2) none
// in 0x40 bytes. The eboot's template fits the reservation.
TEST(TlsGame, HelperBlocksHoldTheLoadSetsTemplates) {
    BB_REQUIRE_GAME(game);
    bb::loader::LoadSet set = bb::loader::map_load_set(game);
    bb::hle::link(set);  // registers the TLS templates
    for (bb::loader::LoadedModule& m : set.modules) {
        m.image.finalize_protection();
    }
    const bb::loader::TlsTemplate& eboot = *set.modules[0].image.tls;
    EXPECT_EQ(eboot.mem_size, eboot_tls_size);
    EXPECT_EQ(eboot.align, eboot_tls_align);
    EXPECT_EQ(eboot.file_size, 0U);

    const bb::loader::LoadedModule& libc = set.modules[2];
    ASSERT_EQ(libc.id, 3U);
    const bb::loader::TlsTemplate& tls = *libc.image.tls;
    ASSERT_EQ(tls.file_size, 0x220U);
    const auto* init = reinterpret_cast<const std::byte*>(libc.image.runtime(tls.link_addr));
    std::thread([&] {
        const std::byte* block = helper(libc.id, 0);
        EXPECT_EQ(std::memcmp(block, init, tls.file_size), 0);
        EXPECT_TRUE(std::all_of(block + tls.file_size, block + tls.mem_size,
                                [](std::byte b) { return b == std::byte{0}; }));
        const std::byte* fios2 = helper(2, 0x3f) - 0x3f;
        EXPECT_TRUE(
            std::all_of(fios2, fios2 + 0x40, [](std::byte b) { return b == std::byte{0}; }));
    }).join();
}

}  // namespace
