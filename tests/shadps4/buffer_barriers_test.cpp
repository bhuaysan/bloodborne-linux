// The runtime's buffer barrier tracking (src/shadps4/video_core/renderer_vulkan/vk_buffer_barriers.h):
// the hazards that need a barrier are found (read after write, write after write, write after
// read), the ones that don't are not, and over random accesses it answers exactly as the
// tracking it replaced (#593): one read and one write interval list per buffer, added to on
// every access. Needs no Vulkan device.

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "common/interval_set.h"
#include "video_core/renderer_vulkan/vk_buffer_barriers.h"

namespace {

using Vulkan::BufferBarrierSlot;
using Vulkan::BufferBarrierTracker;

constexpr bool kRead = true;
constexpr bool kWrite = true;

TEST(BufferBarriers, ReadAfterWriteIsAHazard) {
    BufferBarrierTracker tracker;
    BufferBarrierSlot buffer;
    tracker.Access(buffer, 0x100, 0x100, !kRead, kWrite);
    EXPECT_TRUE(tracker.IsAccessed(buffer, 0x180, 0x10, false));
    EXPECT_TRUE(tracker.IsAccessed(buffer, 0x0, 0x101, false));
    EXPECT_TRUE(tracker.IsAccessed(buffer, 0x1ff, 0x100, false));
}

TEST(BufferBarriers, WriteAfterWriteIsAHazard) {
    BufferBarrierTracker tracker;
    BufferBarrierSlot buffer;
    tracker.Access(buffer, 0x100, 0x100, kRead, kWrite);
    EXPECT_TRUE(tracker.IsAccessed(buffer, 0x100, 0x100, true));
}

TEST(BufferBarriers, WriteAfterReadIsAHazardOnlyWhenReadsAreChecked) {
    BufferBarrierTracker tracker;
    BufferBarrierSlot buffer;
    tracker.Access(buffer, 0x100, 0x100, kRead, !kWrite);
    // A read after a read needs no barrier.
    EXPECT_FALSE(tracker.IsAccessed(buffer, 0x100, 0x100, false));
    EXPECT_TRUE(tracker.IsAccessed(buffer, 0x100, 0x100, true));
    EXPECT_TRUE(tracker.IsAccessed(buffer, 0x1ff, 0x1, true));
}

TEST(BufferBarriers, TouchingAndDisjointRangesAreNoHazard) {
    BufferBarrierTracker tracker;
    BufferBarrierSlot buffer;
    tracker.Access(buffer, 0x100, 0x100, kRead, kWrite);
    EXPECT_FALSE(tracker.IsAccessed(buffer, 0x0, 0x100, true));
    EXPECT_FALSE(tracker.IsAccessed(buffer, 0x200, 0x100, true));
    EXPECT_FALSE(tracker.IsAccessed(buffer, 0x1000, 0x10, true));
}

TEST(BufferBarriers, WriteBetweenPendingReadsIsNoHazard) {
    BufferBarrierTracker tracker;
    BufferBarrierSlot buffer;
    // Reads out of order with gaps: the bounds cover the gap, the merged ranges don't.
    tracker.Access(buffer, 0x400, 0x100, kRead, !kWrite);
    tracker.Access(buffer, 0x100, 0x100, kRead, !kWrite);
    tracker.Access(buffer, 0x800, 0x100, kRead, !kWrite);
    EXPECT_FALSE(tracker.IsAccessed(buffer, 0x200, 0x200, true));
    EXPECT_FALSE(tracker.IsAccessed(buffer, 0x500, 0x300, true));
    EXPECT_TRUE(tracker.IsAccessed(buffer, 0x4ff, 0x2, true));
    // A gap of one word between two consecutive reads stays a gap.
    tracker.Access(buffer, 0x904, 0x4, kRead, !kWrite);
    EXPECT_FALSE(tracker.IsAccessed(buffer, 0x900, 0x4, true));
    // Reads after a merge are still seen.
    tracker.Access(buffer, 0x600, 0x10, kRead, !kWrite);
    EXPECT_TRUE(tracker.IsAccessed(buffer, 0x608, 0x4, true));
    EXPECT_FALSE(tracker.IsAccessed(buffer, 0x610, 0x1f0, true));
}

TEST(BufferBarriers, BuffersAreTrackedApart) {
    BufferBarrierTracker tracker;
    BufferBarrierSlot a;
    BufferBarrierSlot b;
    tracker.Access(a, 0x0, 0x1000, kRead, kWrite);
    EXPECT_FALSE(tracker.IsAccessed(b, 0x0, 0x1000, true));
    tracker.Access(b, 0x2000, 0x10, kRead, !kWrite);
    EXPECT_TRUE(tracker.IsAccessed(a, 0x0, 0x10, false));
    EXPECT_FALSE(tracker.IsAccessed(b, 0x0, 0x1000, true));
    EXPECT_TRUE(tracker.IsAccessed(b, 0x2000, 0x1, true));
}

TEST(BufferBarriers, ClearForgetsAllAccesses) {
    BufferBarrierTracker tracker;
    BufferBarrierSlot a;
    BufferBarrierSlot b;
    tracker.Access(a, 0x0, 0x100, kRead, kWrite);
    tracker.Access(b, 0x0, 0x100, kRead, !kWrite);
    tracker.Clear();
    EXPECT_FALSE(tracker.IsAccessed(a, 0x0, 0x100, true));
    EXPECT_FALSE(tracker.IsAccessed(b, 0x0, 0x100, true));
    // The next batch reuses the entries; b now takes the slot a had.
    tracker.Access(b, 0x0, 0x10, !kRead, kWrite);
    EXPECT_FALSE(tracker.IsAccessed(a, 0x0, 0x100, true));
    EXPECT_TRUE(tracker.IsAccessed(b, 0x0, 0x100, false));
    tracker.Access(a, 0x80, 0x10, kRead, !kWrite);
    EXPECT_FALSE(tracker.IsAccessed(a, 0x0, 0x10, true));
    EXPECT_TRUE(tracker.IsAccessed(a, 0x88, 0x10, true));
    EXPECT_FALSE(tracker.IsAccessed(b, 0x80, 0x10, true));
}

TEST(BufferBarriers, EmptyAndWrappingRangesAreIgnored) {
    BufferBarrierTracker tracker;
    BufferBarrierSlot buffer;
    tracker.Access(buffer, 0x100, 0, kRead, kWrite);
    tracker.Access(buffer, ~std::uint64_t{0} - 0xf, 0x20, kRead, kWrite);
    EXPECT_FALSE(tracker.IsAccessed(buffer, 0x0, 0x1000, true));
    tracker.Access(buffer, 0x0, 0x1000, kRead, kWrite);
    EXPECT_FALSE(tracker.IsAccessed(buffer, 0x100, 0, true));
}

// The tracking before #593, as Runtime had it.
class ReferenceTracker {
public:
    explicit ReferenceTracker(std::size_t buffers) : lists(buffers) {}

    void Access(std::size_t buffer, std::uint64_t offset, std::uint64_t size, bool is_read,
                bool is_write) {
        const Interval range{offset, offset + size};
        if (is_write) {
            lists[buffer].write_ranges.Add(range);
        }
        if (is_read) {
            lists[buffer].read_ranges.Add(range);
        }
    }

    bool IsAccessed(std::size_t buffer, std::uint64_t offset, std::uint64_t size,
                    bool check_read_access) const {
        bool has_access = lists[buffer].write_ranges.Overlaps(offset, offset + size);
        if (check_read_access && !has_access) {
            has_access |= lists[buffer].read_ranges.Overlaps(offset, offset + size);
        }
        return has_access;
    }

    void Clear() {
        for (auto& list : lists) {
            list.read_ranges.Clear();
            list.write_ranges.Clear();
        }
    }

private:
    struct Lists {
        IntervalList<Interval> read_ranges;
        IntervalList<Interval> write_ranges;
    };
    std::vector<Lists> lists;
};

TEST(BufferBarriers, MatchesTheReferenceOverRandomAccesses) {
    constexpr std::size_t kBuffers = 6;
    std::mt19937_64 rng(593);
    BufferBarrierTracker tracker;
    ReferenceTracker reference(kBuffers);
    std::vector<BufferBarrierSlot> slots(kBuffers);
    std::size_t checks = 0;
    std::size_t hazards = 0;
    for (int step = 0; step < 300000; ++step) {
        const std::size_t buffer = rng() % kBuffers;
        // Small sizes over a small space, so ranges touch, overlap, repeat and leave one-word
        // gaps often; a few accesses span the whole buffer, like the GDS buffer.
        const std::uint64_t offset = (rng() % 64) * 4;
        const std::uint64_t size = rng() % 64 == 0 ? 256 : (rng() % 5) * 4;
        const unsigned op = rng() % 1000;
        if (op < 4) {
            tracker.Clear();
            reference.Clear();
        } else if (op < 500) {
            const bool is_write = rng() % 4 == 0;
            const bool is_read = !is_write || rng() % 2 == 0;
            tracker.Access(slots[buffer], offset, size, is_read, is_write);
            reference.Access(buffer, offset, size, is_read, is_write);
        } else {
            const bool check_read = rng() % 2 == 0;
            const bool expected = reference.IsAccessed(buffer, offset, size, check_read);
            ASSERT_EQ(tracker.IsAccessed(slots[buffer], offset, size, check_read), expected)
                << "step " << step << " buffer " << buffer << " range " << offset << "+" << size
                << " check_read " << check_read;
            ++checks;
            hazards += expected;
        }
    }
    // Both answers come up often enough for the comparison to mean something.
    EXPECT_GT(hazards, checks / 10);
    EXPECT_LT(hazards, checks * 9 / 10);
}

} // namespace
