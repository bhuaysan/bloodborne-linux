// SPDX-License-Identifier: GPL-2.0-or-later
// The registers a graphics pipeline lookup reads, for reusing its result (#780,
// docs/design/command-recording.md, "Reusing pipeline lookups"). Liverpool tells the tracker about
// every register value it changes; the pipeline cache identifies the registers' state by an id
// from the snapshot table and looks up the draw in its reuse table by that id and the few
// registers that change from draw to draw.

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/amdgpu/regs.h"

namespace AmdGpu {

// How the reuse treats one register.
enum class PipelineRegClass : u8 {
    // Not read by the lookup, or read only through the user data (what the shaders read through
    // it is checked at every draw).
    Ignored,
    // Part of the snapshot: a value change starts a new state.
    Snapshot,
    // Read at every draw into the reuse table's key: the program registers of the vertex and
    // pixel shader and the primitive type, which change between most draws.
    PerDraw,
};

struct PipelineRegs {
    // A range of registers, by word.
    struct Range {
        u32 begin;
        u32 end;
    };

    // The registers vk_pipeline_cache.cpp reads for a graphics lookup, field by field
    // (pipeline_regs.cpp). A field it starts to read has to be added there.
    static std::span<const Range> Fields() noexcept;

    // The class of every register, indexed by register.
    static const std::array<PipelineRegClass, Regs::NumRegs>& Classes() noexcept;
};

// The registers of the reuse table's key that are read at every draw.
struct PipelineDrawRegs {
    std::array<u32, 4> ps_program;
    std::array<u32, 4> vs_program;
    u32 primitive_type;

    static PipelineDrawRegs Read(const Regs& regs) noexcept;
    bool operator==(const PipelineDrawRegs&) const = default;
};

// Counts the changes of snapshot registers. Liverpool calls OnWrite before it copies the words
// of a SET_*_REG packet into Regs and OnReset when it resets them; every other write of a
// snapshot register has to go through one of the two (LOAD_*_REG, which loads registers from
// memory, is not implemented: it would call OnWrite with the loaded words).
class PipelineRegTracker {
public:
    void OnWrite(const Regs& regs, u32 reg, const u32* words, u32 count) noexcept {
        for (u32 i = 0; i < count; ++i) {
            const u32 r = reg + i;
            if (r < Regs::NumRegs && regs.reg_array[r] != words[i] &&
                classes[r] == PipelineRegClass::Snapshot) {
                ++generation;
                return;
            }
        }
    }

    void OnReset() noexcept {
        ++generation;
    }

    // Changes whenever a snapshot register changed its value.
    [[nodiscard]] u64 Generation() const noexcept {
        return generation;
    }

private:
    const PipelineRegClass* classes = PipelineRegs::Classes().data();
    u64 generation = 1;
};

// The distinct states of the snapshot registers seen so far, by an id each. Equal states get the
// same id, so a state that comes back (the next frame, the next pass) finds the reuse table's
// entries made for it. Identity is by the whole snapshot, not by its hash.
class PipelineRegSnapshots {
public:
    explicit PipelineRegSnapshots(size_t max_states = 1024) : max_states{max_states} {}

    // The id of the snapshot registers' state in `regs`. When the table is full it starts over:
    // ids handed out before may then name another state, which Epoch tells.
    u32 Resolve(const Regs& regs);

    // Changes when the table starts over.
    [[nodiscard]] u64 Epoch() const noexcept {
        return epoch;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return states.size();
    }

private:
    size_t max_states;
    u64 epoch = 0;
    std::vector<u32> scratch;
    std::vector<std::vector<u32>> states;
    std::unordered_multimap<u64, u32> by_hash;
};

} // namespace AmdGpu
