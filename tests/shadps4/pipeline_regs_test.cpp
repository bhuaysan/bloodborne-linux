// The register side of the pipeline lookup reuse (#780, video_core/amdgpu/pipeline_regs.h): which
// registers belong to the snapshot, the tracker that counts their value changes and the snapshot
// table that names equal states by one id. Needs no Vulkan device.

#include <memory>

#include <gtest/gtest.h>

#include "video_core/amdgpu/pipeline_regs.h"

namespace {

using AmdGpu::PipelineRegClass;
using AmdGpu::PipelineRegs;
using AmdGpu::PipelineRegSnapshots;
using AmdGpu::PipelineRegTracker;
using AmdGpu::Regs;

constexpr u32 Sh(u32 offset) {
    return Regs::ShRegWordOffset + offset;
}

u32 Word(size_t byte_offset) {
    return static_cast<u32>(byte_offset / sizeof(u32));
}

PipelineRegClass Class(u32 reg) {
    return PipelineRegs::Classes()[reg];
}

// Zeroed registers (Regs::SetDefaults is part of the renderer's library).
std::unique_ptr<Regs> MakeRegs() {
    return std::make_unique<Regs>();
}

// A SET_*_REG packet as Liverpool runs it: the tracker sees the words, then they are copied.
void Set(PipelineRegTracker& tracker, Regs& regs, u32 reg, std::initializer_list<u32> words) {
    tracker.OnWrite(regs, reg, std::data(words), static_cast<u32>(words.size()));
    u32 r = reg;
    for (const u32 w : words) {
        regs.reg_array[r++] = w;
    }
}

TEST(PipelineRegs, ClassifiesTheRegisters) {
    // The user data of every stage is checked through the sharps, not kept.
    for (u32 block = 0; block < 6; ++block) {
        for (u32 i = 0; i < 16; ++i) {
            EXPECT_EQ(Class(Sh(block * 0x40 + 0x0C + i)), PipelineRegClass::Ignored);
        }
    }
    // The vertex and pixel shader programs and the primitive type are read at every draw.
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_EQ(Class(Word(offsetof(Regs, ps_program)) + i), PipelineRegClass::PerDraw);
        EXPECT_EQ(Class(Word(offsetof(Regs, vs_program)) + i), PipelineRegClass::PerDraw);
    }
    EXPECT_EQ(Class(Word(offsetof(Regs, primitive_type))), PipelineRegClass::PerDraw);
    // What the lookup reads belongs to the snapshot, whole fields and arrays.
    EXPECT_EQ(Class(Word(offsetof(Regs, gs_program))), PipelineRegClass::Snapshot);
    EXPECT_EQ(Class(Word(offsetof(Regs, ls_program)) + 3), PipelineRegClass::Snapshot);
    EXPECT_EQ(Class(Word(offsetof(Regs, stage_enable))), PipelineRegClass::Snapshot);
    EXPECT_EQ(Class(Word(offsetof(Regs, color_buffers))), PipelineRegClass::Snapshot);
    EXPECT_EQ(Class(Word(offsetof(Regs, color_buffers) + sizeof(Regs::color_buffers)) - 1),
              PipelineRegClass::Snapshot);
    EXPECT_EQ(Class(Word(offsetof(Regs, blend_control)) + 7), PipelineRegClass::Snapshot);
    EXPECT_EQ(Class(Word(offsetof(Regs, ps_inputs)) + 31), PipelineRegClass::Snapshot);
    EXPECT_EQ(Class(Word(offsetof(Regs, depth_buffer))), PipelineRegClass::Snapshot);
    EXPECT_EQ(Class(Word(offsetof(Regs, ps_input_addr)) + 2), PipelineRegClass::Snapshot);
    EXPECT_EQ(Class(Word(offsetof(Regs, stage_enable)) - 7), PipelineRegClass::Snapshot);
    // What it does not read: dynamic state, the draw parameters, config registers.
    EXPECT_EQ(Class(Word(offsetof(Regs, depth_bounds_min))), PipelineRegClass::Ignored);
    EXPECT_EQ(Class(Word(offsetof(Regs, screen_scissor))), PipelineRegClass::Ignored);
    EXPECT_EQ(Class(Word(offsetof(Regs, viewports))), PipelineRegClass::Ignored);
    EXPECT_EQ(Class(Word(offsetof(Regs, num_indices))), PipelineRegClass::Ignored);
    EXPECT_EQ(Class(Word(offsetof(Regs, num_instances))), PipelineRegClass::Ignored);
    EXPECT_EQ(Class(Word(offsetof(Regs, index_base_address))), PipelineRegClass::Ignored);
    EXPECT_EQ(Class(Regs::ConfigRegWordOffset + 0x100), PipelineRegClass::Ignored);
    // The fields do not overlap and stay inside the register file.
    u32 words = 0;
    for (const auto& range : PipelineRegs::Fields()) {
        EXPECT_LT(range.begin, range.end);
        EXPECT_LE(range.end, Regs::NumRegs);
        words += range.end - range.begin;
    }
    u32 classified = 0;
    for (const auto c : PipelineRegs::Classes()) {
        classified += c != PipelineRegClass::Ignored;
    }
    EXPECT_EQ(classified, words);
}

TEST(PipelineRegs, TrackerCountsValueChangesOfSnapshotRegisters) {
    auto regs = MakeRegs();
    PipelineRegTracker tracker;
    const u64 start = tracker.Generation();
    const u32 blend = Word(offsetof(Regs, blend_control));

    // The same value again, user data, the programs and the draw parameters change nothing.
    Set(tracker, *regs, blend, {regs->reg_array[blend]});
    Set(tracker, *regs, Sh(0x4C), {1, 2, 3, 4});
    Set(tracker, *regs, Word(offsetof(Regs, vs_program)), {0x1234, 0x5});
    Set(tracker, *regs, Word(offsetof(Regs, primitive_type)), {4});
    Set(tracker, *regs, Word(offsetof(Regs, num_indices)), {300});
    EXPECT_EQ(tracker.Generation(), start);

    // A changed snapshot register in the middle of a packet does.
    Set(tracker, *regs, blend - 1, {regs->reg_array[blend - 1], regs->reg_array[blend] ^ 1});
    const u64 changed = tracker.Generation();
    EXPECT_NE(changed, start);

    // A packet that starts in registers the lookup does not read and ends in one it reads.
    const u32 enable = Word(offsetof(Regs, stage_enable));
    Set(tracker, *regs, enable - 1, {5, regs->reg_array[enable] + 1});
    EXPECT_NE(tracker.Generation(), changed);

    const u64 before_reset = tracker.Generation();
    regs->reg_array.fill(0); // what CLEAR_STATE does, in short
    tracker.OnReset();
    EXPECT_NE(tracker.Generation(), before_reset);
}

TEST(PipelineRegs, SnapshotsNameEqualStatesByOneId) {
    auto regs = MakeRegs();
    PipelineRegSnapshots snapshots;
    const u32 blend = Word(offsetof(Regs, blend_control));

    const u32 first = snapshots.Resolve(*regs);
    EXPECT_EQ(snapshots.Resolve(*regs), first);

    // What is not part of the snapshot keeps the id.
    regs->reg_array[Sh(0x4C)] = 0xDEAD;
    regs->reg_array[Word(offsetof(Regs, ps_program))] = 0xBEEF;
    regs->reg_array[Word(offsetof(Regs, num_indices))] = 77;
    EXPECT_EQ(snapshots.Resolve(*regs), first);

    // A changed register gets a new id, and the old state its old id back.
    const u32 saved = regs->reg_array[blend];
    regs->reg_array[blend] = saved ^ 0x10;
    const u32 second = snapshots.Resolve(*regs);
    EXPECT_NE(second, first);
    regs->reg_array[blend] = saved;
    EXPECT_EQ(snapshots.Resolve(*regs), first);
    regs->reg_array[blend] = saved ^ 0x10;
    EXPECT_EQ(snapshots.Resolve(*regs), second);

    // The last word of an array counts, dynamic state does not.
    regs->reg_array[Word(offsetof(Regs, ps_inputs)) + 31] ^= 1;
    const u32 third = snapshots.Resolve(*regs);
    EXPECT_NE(third, second);
    regs->reg_array[Word(offsetof(Regs, depth_bounds_min))] ^= 1;
    EXPECT_EQ(snapshots.Resolve(*regs), third);
    EXPECT_EQ(snapshots.Size(), 3u);
}

TEST(PipelineRegs, FullSnapshotTableStartsOver) {
    auto regs = MakeRegs();
    PipelineRegSnapshots snapshots{3};
    const u32 blend = Word(offsetof(Regs, blend_control));
    const u64 epoch = snapshots.Epoch();
    for (u32 i = 0; i < 3; ++i) {
        regs->reg_array[blend] = i;
        EXPECT_EQ(snapshots.Resolve(*regs), i);
    }
    EXPECT_EQ(snapshots.Epoch(), epoch);
    regs->reg_array[blend] = 3;
    EXPECT_EQ(snapshots.Resolve(*regs), 0u);
    EXPECT_NE(snapshots.Epoch(), epoch);
    EXPECT_EQ(snapshots.Size(), 1u);
}

TEST(PipelineRegs, DrawRegsAreTheProgramsAndThePrimitiveType) {
    auto regs = MakeRegs();
    const u32 ps = Word(offsetof(Regs, ps_program));
    const u32 vs = Word(offsetof(Regs, vs_program));
    for (u32 i = 0; i < 4; ++i) {
        regs->reg_array[ps + i] = 0x100 + i;
        regs->reg_array[vs + i] = 0x200 + i;
    }
    regs->reg_array[Word(offsetof(Regs, primitive_type))] = 4;
    const auto draw = AmdGpu::PipelineDrawRegs::Read(*regs);
    EXPECT_EQ(draw.ps_program, (std::array<u32, 4>{0x100, 0x101, 0x102, 0x103}));
    EXPECT_EQ(draw.vs_program, (std::array<u32, 4>{0x200, 0x201, 0x202, 0x203}));
    EXPECT_EQ(draw.primitive_type, 4u);
}

} // namespace
