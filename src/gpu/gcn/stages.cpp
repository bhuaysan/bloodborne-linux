// The vertex and pixel stages (docs/design/graphics.md, "Stage interfaces and ShaderKey" and
// "Pixel shaders"). The register fields follow the Sea Islands register reference; the VS input
// VGPRs, the PS input VGPR order and the export formats were checked against shadPS4 (facts only,
// see docs/references.md). EXP follows the Sea Islands ISA manual.

#include "gpu/gcn/stages.hpp"

#include <format>

#include <spirv/unified1/GLSL.std.450.h>

namespace bb::gcn::detail {

namespace {

using Id = SpirvBuilder::Id;
using Op = spv::Op;
using SC = spv::StorageClass;

// SPI_SHADER_PGM_RSRC2_VS: SCRATCH_EN (bit 0), OC_LDS_EN (7) and SO_BASE0_EN to SO_EN (8-12)
// add initial SGPRs.
constexpr std::uint32_t vs_rsrc2_unsupported = 1u << 0 | 0x3fu << 7;
constexpr std::uint32_t ps_rsrc2_unsupported = 1u << 0;  // SPI_SHADER_PGM_RSRC2_PS: SCRATCH_EN

// The VGPRs of each SPI_PS_INPUT_ADDR/ENA bit, in VGPR order (docs/design/graphics.md, "Pixel
// initial registers").
constexpr unsigned ps_input_vgprs[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
// The bits that may be enabled so far: the six barycentric pairs and POS_*_FLOAT.
constexpr std::uint32_t ps_input_supported = 0x77u | 0xfu << 8;

// Export targets (Instruction::mem_offset of EXP).
constexpr unsigned target_mrtz = 8;
constexpr unsigned target_null = 9;
constexpr unsigned target_pos0 = 12;
constexpr unsigned target_param0 = 32;

// SPI_SHADER_COL_FORMAT values, 4 bits per render target.
enum ExportFormat : unsigned {
    zero = 0, r32 = 1, gr32 = 2, ar32 = 3, fp16 = 4, unorm16 = 5, snorm16 = 6, uint16 = 7,
    sint16 = 8, abgr32 = 9,
};

}  // namespace

void StageIo::check_key(const ShaderKey& key) {
    const auto reject = [](const std::string& what) { throw GcnError(what, 0, 0); };
    if (key.stage == Stage::vertex && (key.pgm_rsrc2 & vs_rsrc2_unsupported) != 0) {
        reject(std::format("SPI_SHADER_PGM_RSRC2_VS {:#x} is not supported yet", key.pgm_rsrc2));
    }
    if (key.stage != Stage::pixel) {
        return;
    }
    if ((key.pgm_rsrc2 & ps_rsrc2_unsupported) != 0) {
        reject(std::format("SPI_SHADER_PGM_RSRC2_PS {:#x} is not supported yet", key.pgm_rsrc2));
    }
    if ((key.ps_input_ena & ~key.ps_input_addr) != 0) {
        reject(std::format("SPI_PS_INPUT_ENA {:#x} enables inputs outside SPI_PS_INPUT_ADDR {:#x}",
                           key.ps_input_ena, key.ps_input_addr));
    }
    if ((key.ps_input_ena & ~ps_input_supported) != 0) {
        reject(std::format("SPI_PS_INPUT_ENA {:#x} is not supported yet", key.ps_input_ena));
    }
}

spv::ExecutionModel StageIo::model() const {
    return key_.stage == Stage::vertex ? spv::ExecutionModel::Vertex
                                       : spv::ExecutionModel::Fragment;
}

void StageIo::prologue(unsigned user_sgprs) {
    if (key_.stage == Stage::pixel) {
        pixel_prologue(user_sgprs);
        return;
    }
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

// The input VGPRs as SPI_PS_INPUT_ADDR places them, loaded where SPI_PS_INPUT_ENA says.
void StageIo::pixel_prologue(unsigned user_sgprs) {
    const Id c0 = b_.constant_u32(0);
    // The primitive mask follows the user data. Programs move it to M0 for v_interp, which the
    // translation doesn't need, so it is 0.
    store(regs_.sgpr(user_sgprs), c0);
    // EXEC starts as the covered pixels: helper invocations are off until s_wqm_b64.
    const Id bool_t = b_.type_bool();
    const Id helper = load(bool_t, builtin(spv::BuiltIn::HelperInvocation, SC::Input, bool_t));
    store(regs_.exec_var(), b_.op(Op::OpLogicalNot, bool_t, {helper}));

    // The barycentric VGPRs hold 0: v_interp reads them only to choose the interpolation.
    unsigned v = 0;
    for (unsigned bit = 0; bit < 16; ++bit) {
        if (((key_.ps_input_addr >> bit) & 1) == 0) {
            continue;
        }
        const bool enabled = ((key_.ps_input_ena >> bit) & 1) != 0;
        for (unsigned k = 0; k < ps_input_vgprs[bit]; ++k, ++v) {
            Id value = c0;
            if (enabled && bit >= 8 && bit <= 11) {
                const Id vec4 = b_.type_vector(f32(), 4);
                const Id coord = load(vec4, builtin(spv::BuiltIn::FragCoord, SC::Input, vec4));
                Id c = b_.op(Op::OpCompositeExtract, f32(), {coord, bit - 8});
                if (bit == 11) {  // the hardware gives W, FragCoord.w is 1/W
                    c = b_.op(Op::OpFDiv, f32(), {b_.constant_f32(1.0f), c});
                }
                value = b_.op(Op::OpBitcast, u32(), {c});
            }
            store(regs_.vgpr(v), value);
        }
    }
}

// An export runs in the active lanes. pos0 is Position and param<n> the output at location n.
void StageIo::exp(const Instruction& inst) {
    const unsigned target = inst.mem_offset;
    if (key_.stage == Stage::compute) {
        regs_.fail("exp in a compute shader");
    }
    if (key_.stage == Stage::pixel) {
        pixel_export(inst);
        return;
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

// In a pixel shader, `vm` makes EXEC the valid mask, and at `done` the lanes outside the last
// valid mask are discarded: they demote to helper invocations, which keeps them for the
// derivatives of their quad but writes nothing.
void StageIo::pixel_export(const Instruction& inst) {
    const unsigned target = inst.mem_offset;
    if (target < target_mrtz) {
        render_target(inst, target);
    } else if (target != target_null) {
        regs_.fail(std::format("export target {} is not supported in this stage yet", target));
    }
    if ((inst.flags & flag::vm) != 0) {
        store(valid(), regs_.active());
    }
    if ((inst.flags & flag::done) != 0) {
        b_.capability(spv::Capability::DemoteToHelperInvocation);
        const Id discarded = b_.op(Op::OpLogicalNot, b_.type_bool(),
                                   {load(b_.type_bool(), valid())});
        regs_.guarded(discarded, [&] { b_.op_void(Op::OpDemoteToHelperInvocation); });
    }
}

// mrt<n>: converted per SPI_SHADER_COL_FORMAT. The 32-bit formats take the channels they name
// (R, GR, AR or ABGR) as floats; the 16-bit formats are compressed exports, two values per VGPR,
// unpacked to float, or to a uint or int output for UINT16 and SINT16. Channels that the format
// leaves out stay 0.
void StageIo::render_target(const Instruction& inst, unsigned n) {
    const unsigned format = (key_.col_format >> (4 * n)) & 0xf;
    if (format == zero) {
        return;
    }
    const bool compr = (inst.flags & flag::compr) != 0;
    if (format > abgr32 || compr != (format >= fp16 && format <= sint16)) {
        regs_.fail(std::format("{} export to mrt{} with SPI_SHADER_COL_FORMAT {}",
                               compr ? "a compressed" : "an uncompressed", n, format));
    }
    const Id type = format == uint16   ? u32()
                    : format == sint16 ? b_.type_int(32, true)
                                       : f32();
    Id values[4] = {};
    for (unsigned c = 0; c < 4; ++c) {
        const Operand& o = inst.src[c];
        const bool named = format == abgr32 || c == 0 || (format == gr32 && c == 1) ||
                           (format == ar32 && c == 3);
        if (o.kind == OperandKind::off) {
            continue;
        }
        if (compr) {
            values[c] = half(load(u32(), regs_.vgpr(o.code)), c & 1, format, type);
        } else if (named) {
            values[c] = value(o);
        }
    }
    store_components(output(n, b_.type_vector(type, 4)), type, values);
}

Id StageIo::half(Id packed, unsigned high, unsigned format, Id type) {
    if (format == uint16 || format == sint16) {
        const Op op = format == uint16 ? Op::OpBitFieldUExtract : Op::OpBitFieldSExtract;
        const Id base = format == uint16 ? packed : b_.op(Op::OpBitcast, type, {packed});
        return b_.op(op, type, {base, b_.constant_u32(16 * high), b_.constant_u32(16)});
    }
    const auto unpack = format == fp16      ? GLSLstd450UnpackHalf2x16
                        : format == unorm16 ? GLSLstd450UnpackUnorm2x16
                                            : GLSLstd450UnpackSnorm2x16;
    const Id pair = b_.op(Op::OpExtInst, b_.type_vector(f32(), 2),
                          {b_.import_extended("GLSL.std.450"), unpack, packed});
    return b_.op(Op::OpCompositeExtract, f32(), {pair, high});
}

void StageIo::finish(Id main) {
    if (key_.stage == Stage::pixel) {
        b_.execution_mode(main, spv::ExecutionMode::OriginUpperLeft);
    }
}

Id StageIo::builtin(spv::BuiltIn which, spv::StorageClass storage, Id type) {
    auto [it, added] = builtins_.try_emplace(spirv_word(which), 0);
    if (added) {
        it->second = b_.variable(b_.type_pointer(storage, type), storage,
                                 storage == SC::Output ? b_.constant_null(type) : 0);
        b_.decorate(it->second, spv::Decoration::BuiltIn, {spirv_word(which)});
        if (which == spv::BuiltIn::HelperInvocation) {  // demote changes it (SPIR-V 1.6)
            b_.decorate(it->second, spv::Decoration::Volatile);
        }
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

Id StageIo::valid() {
    if (valid_ == 0) {
        valid_ = b_.local_variable(b_.type_pointer(SC::Function, b_.type_bool()),
                                   b_.constant_bool(true));
    }
    return valid_;
}

}  // namespace bb::gcn::detail
