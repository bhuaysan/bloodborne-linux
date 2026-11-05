#include "hle/kernel/tls.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

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

}  // namespace
