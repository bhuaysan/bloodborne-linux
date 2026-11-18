// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/renderer_vulkan/vk_runtime.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-18
// License texts: third_party/shadps4/LICENSES/

// The ranges of each buffer that the commands since the last barrier read and wrote, which
// decide whether the next access needs a barrier first. Taken out of Runtime (its BufferBarriers
// and MakeCurrent) so the common case is cheap (#593): a buffer finds its ranges through a slot
// it carries instead of a search, and reads are only appended until a write asks about them.

#pragma once

#include <algorithm>
#include <limits>
#include <vector>

#include "common/interval_set.h"
#include "common/types.h"

namespace Vulkan {

/// Where a buffer's ranges sit in the tracker. A buffer carries one; the tracker sets it.
struct BufferBarrierSlot {
    u64 batch = 0; ///< Batch that `index` belongs to; 0 is never a batch.
    u32 index = 0;
};

class BufferBarrierTracker {
public:
    /// Records an access of [offset, offset + size) of a buffer since the last barrier.
    void Access(BufferBarrierSlot& slot, u64 offset, u64 size, bool is_read, bool is_write) {
        const u64 end = offset + size;
        if (end <= offset || (!is_read && !is_write)) {
            return;
        }
        Entry& entry = Get(slot);
        if (is_write) {
            entry.write_ranges.Add({offset, end});
        }
        if (is_read) {
            AddRead(entry, offset, end);
        }
    }

    /// Whether [offset, offset + size) overlaps a write since the last barrier, or with
    /// `check_read_access` also a read.
    bool IsAccessed(const BufferBarrierSlot& slot, u64 offset, u64 size, bool check_read_access) {
        const u64 end = offset + size;
        if (slot.batch != batch || end <= offset) {
            return false;
        }
        Entry& entry = entries[slot.index];
        if (entry.write_ranges.Overlaps(offset, end)) {
            return true;
        }
        if (!check_read_access || offset >= entry.read_end || end <= entry.read_start) {
            return false;
        }
        MergePendingReads(entry);
        return entry.read_ranges.Overlaps(offset, end);
    }

    /// Forgets every range: a barrier has ordered all accesses so far.
    void Clear() {
        for (std::size_t i = 0; i < used; ++i) {
            entries[i].Reset();
        }
        used = 0;
        ++batch;
    }

private:
    struct Entry {
        IntervalList<Interval> write_ranges;
        /// Reads merged into a sorted list, and those appended since, merged only when a write
        /// asks whether it overlaps a read.
        IntervalList<Interval> read_ranges;
        std::vector<Interval> pending_reads;
        /// Bounds of all reads, so a write outside them needs no merge.
        u64 read_start = std::numeric_limits<u64>::max();
        u64 read_end = 0;

        void Reset() {
            write_ranges.Clear();
            read_ranges.Clear();
            pending_reads.clear();
            read_start = std::numeric_limits<u64>::max();
            read_end = 0;
        }
    };

    Entry& Get(BufferBarrierSlot& slot) {
        if (slot.batch == batch) {
            return entries[slot.index];
        }
        if (used == entries.size()) {
            entries.emplace_back();
        }
        slot = {batch, static_cast<u32>(used)};
        return entries[used++];
    }

    static void AddRead(Entry& entry, u64 start, u64 end) {
        entry.read_start = std::min(entry.read_start, start);
        entry.read_end = std::max(entry.read_end, end);
        // Consecutive reads of one buffer often touch or repeat (the stream buffer, a buffer
        // bound to several stages), so they extend the last pending read instead.
        if (!entry.pending_reads.empty()) {
            Interval& last = entry.pending_reads.back();
            if (start <= last.end && end >= last.start) {
                last.start = std::min(last.start, start);
                last.end = std::max(last.end, end);
                return;
            }
        }
        entry.pending_reads.push_back({start, end});
    }

    static void MergePendingReads(Entry& entry) {
        auto& pending = entry.pending_reads;
        if (pending.empty()) {
            return;
        }
        std::ranges::sort(pending, {}, &Interval::start);
        Interval run = pending.front();
        for (const Interval& next : pending) {
            if (next.start <= run.end) {
                run.end = std::max(run.end, next.end);
            } else {
                entry.read_ranges.Add(run);
                run = next;
            }
        }
        entry.read_ranges.Add(run);
        pending.clear();
    }

    /// Entries [0, used) belong to the current batch; the rest keep their allocations.
    std::vector<Entry> entries;
    std::size_t used = 0;
    u64 batch = 1;
};

} // namespace Vulkan
