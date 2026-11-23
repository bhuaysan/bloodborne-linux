// SPDX-License-Identifier: GPL-2.0-or-later
// The registers a graphics pipeline lookup reads, for reusing its result (#780,
// docs/design/command-recording.md, "Reusing pipeline lookups").

#include "video_core/amdgpu/pipeline_regs.h"

#include <cstring>
#include <functional>
#include <string_view>

namespace AmdGpu {

namespace {

constexpr u32 WordOf(size_t byte_offset) {
    return static_cast<u32>(byte_offset / sizeof(u32));
}

// The words a field of Regs covers.
#define BB_REG_FIELD(name)                                                                         \
    PipelineRegs::Range {                                                                          \
        WordOf(offsetof(Regs, name)),                                                              \
            WordOf(offsetof(Regs, name) + sizeof(Regs::name) + sizeof(u32) - 1)                    \
    }
// The program registers of a stage: address and settings, without the user data.
#define BB_REG_PROGRAM(name)                                                                       \
    PipelineRegs::Range {                                                                          \
        WordOf(offsetof(Regs, name)), WordOf(offsetof(Regs, name) + offsetof(ShaderProgram, user_data)) \
    }

// Bit fields have no offsetof; they sit at a fixed distance from a neighbour.
constexpr u32 NumInterpReg = WordOf(offsetof(Regs, ps_input_addr)) + 2;
static_assert(NumInterpReg + 2 == WordOf(offsetof(Regs, barycentric_control)));
constexpr u32 GsMaxVertOutReg = WordOf(offsetof(Regs, vgt_gsvs_ring_itemsize)) + 34;
static_assert(GsMaxVertOutReg + 7 == WordOf(offsetof(Regs, stage_enable)));
static_assert(offsetof(ShaderProgram, user_data) == 4 * sizeof(u32));

// Every register a graphics lookup reads: each `regs.<field>` of vk_pipeline_cache.cpp
// (RefreshGraphicsKey, RefreshGraphicsStages, BuildRuntimeInfo, GetProgram through
// ProgramForStage and GetParams), whole fields even where it reads a part. The user data of the
// programs is not here: the lookup reads through it only the sharps and the fetch shader, which
// the reuse checks at every draw. Only vertex and pixel shader draws reuse a lookup
// (stage_enable is part of the snapshot), so the other stages' fields matter only for telling
// their states apart.
constexpr std::array ReadFields{
    BB_REG_FIELD(aa_config),
    BB_REG_FIELD(barycentric_control),
    BB_REG_FIELD(blend_control),
    BB_REG_FIELD(clipper_control),
    BB_REG_FIELD(color_buffers),
    BB_REG_FIELD(color_control),
    BB_REG_FIELD(color_export_format),
    BB_REG_FIELD(color_shader_mask),
    BB_REG_FIELD(color_target_mask),
    BB_REG_FIELD(depth_buffer),
    BB_REG_FIELD(depth_render_override),
    BB_REG_FIELD(depth_shader_control),
    BB_REG_FIELD(ls_hs_config),
    PipelineRegs::Range{NumInterpReg, NumInterpReg + 1},
    BB_REG_FIELD(polygon_control),
    BB_REG_FIELD(primitive_type),
    BB_REG_FIELD(ps_input_addr),
    BB_REG_FIELD(ps_input_ena),
    BB_REG_FIELD(ps_inputs),
    BB_REG_FIELD(stage_enable),
    BB_REG_FIELD(tess_config),
    BB_REG_FIELD(vgt_esgs_ring_itemsize),
    BB_REG_FIELD(vgt_gs_instance_cnt),
    PipelineRegs::Range{GsMaxVertOutReg, GsMaxVertOutReg + 1},
    BB_REG_FIELD(vgt_gs_mode),
    BB_REG_FIELD(vgt_gs_out_prim_type),
    BB_REG_FIELD(vgt_gs_vert_itemsize),
    BB_REG_FIELD(vgt_instance_step_rate_0),
    BB_REG_FIELD(vgt_instance_step_rate_1),
    BB_REG_FIELD(vgt_strmout_config),
    BB_REG_FIELD(vs_output_control),
    BB_REG_FIELD(z_export_format),
    BB_REG_PROGRAM(ps_program),
    BB_REG_PROGRAM(vs_program),
    BB_REG_PROGRAM(gs_program),
    BB_REG_PROGRAM(es_program),
    BB_REG_PROGRAM(hs_program),
    BB_REG_PROGRAM(ls_program),
};

#undef BB_REG_FIELD
#undef BB_REG_PROGRAM

constexpr u32 PsProgramReg = WordOf(offsetof(Regs, ps_program));
constexpr u32 VsProgramReg = WordOf(offsetof(Regs, vs_program));
constexpr u32 PrimitiveTypeReg = WordOf(offsetof(Regs, primitive_type));

std::array<PipelineRegClass, Regs::NumRegs> MakeClasses() {
    std::array<PipelineRegClass, Regs::NumRegs> classes{};
    classes.fill(PipelineRegClass::Ignored);
    for (const auto& range : ReadFields) {
        for (u32 r = range.begin; r < range.end; ++r) {
            classes[r] = PipelineRegClass::Snapshot;
        }
    }
    for (u32 word = 0; word < 4; ++word) {
        classes[PsProgramReg + word] = PipelineRegClass::PerDraw;
        classes[VsProgramReg + word] = PipelineRegClass::PerDraw;
    }
    classes[PrimitiveTypeReg] = PipelineRegClass::PerDraw;
    return classes;
}

// The runs of consecutive snapshot registers, which Resolve copies.
std::vector<PipelineRegs::Range> MakeRuns(const std::array<PipelineRegClass, Regs::NumRegs>& c) {
    std::vector<PipelineRegs::Range> runs;
    for (u32 r = 0; r < Regs::NumRegs; ++r) {
        if (c[r] != PipelineRegClass::Snapshot) {
            continue;
        }
        if (!runs.empty() && runs.back().end == r) {
            ++runs.back().end;
        } else {
            runs.push_back({r, r + 1});
        }
    }
    return runs;
}

} // namespace

std::span<const PipelineRegs::Range> PipelineRegs::Fields() noexcept {
    return ReadFields;
}

const std::array<PipelineRegClass, Regs::NumRegs>& PipelineRegs::Classes() noexcept {
    static const auto classes = MakeClasses();
    return classes;
}

PipelineDrawRegs PipelineDrawRegs::Read(const Regs& regs) noexcept {
    PipelineDrawRegs d{};
    for (u32 word = 0; word < 4; ++word) {
        d.ps_program[word] = regs.reg_array[PsProgramReg + word];
        d.vs_program[word] = regs.reg_array[VsProgramReg + word];
    }
    d.primitive_type = regs.reg_array[PrimitiveTypeReg];
    return d;
}

u32 PipelineRegSnapshots::Resolve(const Regs& regs) {
    static const auto runs = MakeRuns(PipelineRegs::Classes());
    scratch.clear();
    for (const auto& run : runs) {
        scratch.insert(scratch.end(), regs.reg_array.begin() + run.begin,
                       regs.reg_array.begin() + run.end);
    }
    const u64 hash = std::hash<std::string_view>{}(std::string_view{
        reinterpret_cast<const char*>(scratch.data()), scratch.size() * sizeof(u32)});
    const auto [first, last] = by_hash.equal_range(hash);
    for (auto it = first; it != last; ++it) {
        if (states[it->second] == scratch) {
            return it->second;
        }
    }
    if (states.size() >= max_states) {
        states.clear();
        by_hash.clear();
        ++epoch;
    }
    const u32 id = static_cast<u32>(states.size());
    states.push_back(scratch);
    by_hash.emplace(hash, id);
    return id;
}

} // namespace AmdGpu
