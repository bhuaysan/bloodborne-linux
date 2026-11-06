// The graphics stages (docs/design/graphics.md, "Stage interfaces and ShaderKey"), so far the
// vertex stage. The register fields follow the Sea Islands register reference; the VS input VGPRs
// were checked against shadPS4 (facts only, see docs/references.md). EXP follows the Sea Islands
// ISA manual.

#include "gpu/gcn/stages.hpp"

#include <format>

namespace bb::gcn::detail {

namespace {

using Id = SpirvBuilder::Id;
using Op = spv::Op;
using SC = spv::StorageClass;

// SPI_SHADER_PGM_RSRC2_VS: SCRATCH_EN (bit 0), OC_LDS_EN (7) and SO_BASE0_EN to SO_EN (8-12)
// add initial SGPRs.
constexpr std::uint32_t vs_rsrc2_unsupported = 1u << 0 | 0x3fu << 7;

// Export targets (Instruction::mem_offset of EXP).
constexpr unsigned target_null = 9;
constexpr unsigned target_pos0 = 12;
constexpr unsigned target_param0 = 32;

}  // namespace

void StageIo::check_key(const ShaderKey& key) {
    const auto reject = [](const std::string& what) { throw GcnError(what, 0, 0); };
    if (key.stage == Stage::pixel) {
        reject("the pixel stage is not translated yet");
    }
    if (key.stage == Stage::vertex && (key.pgm_rsrc2 & vs_rsrc2_unsupported) != 0) {
        reject(std::format("SPI_SHADER_PGM_RSRC2_VS {:#x} is not supported yet", key.pgm_rsrc2));
    }
}

void StageIo::prologue() {
    const auto index = [&](spv::BuiltIn which) {
        return load(u32(), builtin(which, SC::Input, u32()));
    };
    store(regs_.vgpr(0), index(spv::BuiltIn::VertexIndex));
    const unsigned count = (key_.pgm_rsrc1 >> 24) & 3;  // VGPR_COMP_CNT
    const Id instance = count > 0 ? index(spv::BuiltIn::InstanceIndex) : 0;
    for (unsigned n = 1; n <= count; ++n) {
        Id v = instance;
        if (n < 3) {
            const std::uint32_t rate = key_.instance_step_rate[n - 1];
            v = rate == 0 ? b_.constant_u32(0)
                          : b_.op(Op::OpUDiv, u32(), {instance, b_.constant_u32(rate)});
        }
        store(regs_.vgpr(n), v);
    }
}

// An export runs in the active lanes. pos0 is Position and param<n> the output at location n.
void StageIo::exp(const Instruction& inst) {
    const unsigned target = inst.mem_offset;
    if (key_.stage == Stage::compute) {
        regs_.fail("exp in a compute shader");
    }
    if (target == target_null) {
        return;
    }
    if (target != target_pos0 && target < target_param0) {
        regs_.fail(std::format("export target {} is not supported in this stage yet", target));
    }
    if ((inst.flags & flag::compr) != 0) {
        regs_.fail("a compressed export to a position or parameter");
    }
    Id values[4] = {};
    for (unsigned c = 0; c < 4; ++c) {
        if (inst.src[c].kind != OperandKind::off) {
            values[c] = value(inst.src[c]);
        }
    }
    const Id vec4 = b_.type_vector(f32(), 4);
    store_components(target == target_pos0 ? builtin(spv::BuiltIn::Position, SC::Output, vec4)
                                            : output(target - target_param0, vec4),
                     f32(), values);
}

Id StageIo::builtin(spv::BuiltIn which, spv::StorageClass storage, Id type) {
    auto [it, added] = builtins_.try_emplace(spirv_word(which), 0);
    if (added) {
        it->second = b_.variable(b_.type_pointer(storage, type), storage,
                                 storage == SC::Output ? b_.constant_null(type) : 0);
        b_.decorate(it->second, spv::Decoration::BuiltIn, {spirv_word(which)});
        interface_.push_back(it->second);
    }
    return it->second;
}

// The output at `location`. Outputs start as 0, so the channels an export leaves out are defined.
Id StageIo::output(std::uint32_t location, Id type) {
    auto [it, added] = outputs_.try_emplace(location, 0);
    if (added) {
        it->second = b_.variable(b_.type_pointer(SC::Output, type), SC::Output,
                                 b_.constant_null(type));
        b_.decorate(it->second, spv::Decoration::Location, {location});
        interface_.push_back(it->second);
    }
    return it->second;
}

// Stores the channels of an export that have a value into `var`, in the active lanes.
void StageIo::store_components(Id var, Id type, const Id (&values)[4]) {
    const Id pointer = b_.type_pointer(SC::Output, type);
    regs_.guarded(regs_.active(), [&] {
        for (std::uint32_t c = 0; c < 4; ++c) {
            if (values[c] != 0) {
                store(b_.op(Op::OpAccessChain, pointer, {var, b_.constant_u32(c)}), values[c]);
            }
        }
    });
}

Id StageIo::value(const Operand& o) {
    return b_.op(Op::OpBitcast, f32(), {load(u32(), regs_.vgpr(o.code))});
}

}  // namespace bb::gcn::detail
