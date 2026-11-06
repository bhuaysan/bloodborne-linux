// GCN to SPIR-V: one invocation per lane, the registers as function variables, and EXEC as
// predication (docs/design/graphics.md, "Execution model"). The instruction semantics follow the
// Sea Islands ISA manual (scalar and vector ALU operations, FLAT, DS on the LDS); the compute
// stage's initial registers follow COMPUTE_PGM_RSRC2 (Sea Islands register reference). Cross-lane
// operations run over the subgroup in lane-local mode, at the subgroup's wave position
// (subgroup_base).

#include "gpu/gcn/translate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <spirv/unified1/GLSL.std.450.h>

#include "gpu/gcn/cfg.hpp"
#include "gpu/gcn/descriptors.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/lds.hpp"
#include "gpu/gcn/spirv_builder.hpp"
#include "gpu/gcn/structurize.hpp"

namespace bb::gcn {

namespace {

using Id = SpirvBuilder::Id;
using Op = spv::Op;
using SC = spv::StorageClass;

// COMPUTE_PGM_RSRC2: USER_SGPR in bits 1-5. The other fields that change the initial registers
// (SCRATCH_EN in bit 0, TGID_X_EN to TG_SIZE_EN in bits 7-10, TIDIG_COMP_CNT in bits 11-12)
// are not supported yet.
constexpr unsigned rsrc2_user_sgpr_shift = 1;
constexpr std::uint32_t rsrc2_unsupported = 1u << 0 | 0xfu << 7 | 3u << 11;

// LDS_SIZE in bits 15-23 of COMPUTE_PGM_RSRC2, in units of 128 dwords (512 bytes). shadPS4's
// regs_shader.h says "lds_dwords is in units of 128 dwords". The LDS of one workgroup is at most
// 64 KB.
constexpr unsigned rsrc2_lds_size_shift = 15;
constexpr std::uint32_t lds_dwords_per_unit = 128;
constexpr std::uint32_t lds_max_dwords = 16384;

// SOPP opcode of s_barrier.
constexpr std::uint16_t op_barrier = 10;

// SOPP opcodes of the conditional branches on SCC.
constexpr std::uint16_t op_cbranch_scc0 = 4;
constexpr std::uint16_t op_cbranch_scc1 = 5;
constexpr std::uint16_t op_cbranch_vccz = 6;
constexpr std::uint16_t op_cbranch_vccnz = 7;
constexpr std::uint16_t op_cbranch_execz = 8;
constexpr std::uint16_t op_cbranch_execnz = 9;

// EXEC as a 64-bit operand, the implicit source and destination of the saveexec forms.
constexpr Operand exec_operand{.kind = OperandKind::scalar, .dwords = 2, .code = code::exec};

// The bitwise family of the SALU, in the order of its opcodes (s_<name>_b32, _b64, _saveexec_b64).
constexpr std::string_view bitwise_names[] = {"and", "or", "xor", "andn2", "orn2", "nand", "nor",
                                              "xnor"};

// The index of `name` in the bitwise family: "s_and_b64" with the suffix "_b64" is 0.
std::optional<unsigned> bitwise_op(std::string_view name, std::string_view suffix) {
    if (!name.starts_with("s_") || !name.ends_with(suffix)) {
        return std::nullopt;
    }
    const auto* it = std::ranges::find(
        bitwise_names, name.substr(2, name.size() - 2 - suffix.size()));
    if (it == std::end(bitwise_names)) {
        return std::nullopt;
    }
    return static_cast<unsigned>(it - std::begin(bitwise_names));
}

// The VALU operations with a carry out in a mask (dst[1]) and, for the second three, a carry in
// (src[2]).
struct CarryOp {
    std::string_view name;
    bool sub;
    bool rev;
    bool carry_in;
};
constexpr CarryOp carry_ops[] = {
    {"v_add_i32", false, false, false}, {"v_sub_i32", true, false, false},
    {"v_subrev_i32", true, true, false}, {"v_addc_u32", false, false, true},
    {"v_subb_u32", true, false, true},   {"v_subbrev_u32", true, true, true},
};

// An integer compare, v_cmp[x]_<cond>_<i32|u32> or s_cmp_<cond>_<i32|u32>.
struct Compare {
    std::string_view cond;
    bool is_signed;
    bool writes_exec;
};
std::optional<Compare> parse_compare(std::string_view name) {
    std::string_view part[4];
    for (auto& p : part) {
        const auto end = name.find('_');
        p = name.substr(0, end);
        name = end == std::string_view::npos ? std::string_view() : name.substr(end + 1);
    }
    if (!name.empty() || (part[0] != "v" && part[0] != "s") ||
        (part[1] != "cmp" && !(part[1] == "cmpx" && part[0] == "v")) ||
        (part[3] != "i32" && part[3] != "u32")) {
        return std::nullopt;
    }
    return Compare{part[2], part[3] == "i32", part[1] == "cmpx"};
}

std::string_view strip_encoding(std::string_view name) {
    if (name.ends_with("_e32") || name.ends_with("_e64")) {
        name.remove_suffix(4);
    }
    return name;
}

// Vector ALU operations on two sources; `rev` swaps them.
struct VectorOp {
    std::string_view name;
    Op op;
    bool rev = false;
};
constexpr VectorOp vector_ops[] = {
    {"v_and_b32", Op::OpBitwiseAnd},
    {"v_or_b32", Op::OpBitwiseOr},
    {"v_xor_b32", Op::OpBitwiseXor},
    {"v_lshlrev_b32", Op::OpShiftLeftLogical, true},  // by the low 5 bits
};

// A float compare, v_cmp[x]_<cond>_f32. The ordered conditions are false when an operand is NaN,
// the "n" ones are the negations of the ordered ones (true on NaN). o and u test for NaN.
struct FloatCompare {
    std::string_view cond;
    Op op;
};
constexpr FloatCompare float_compares[] = {
    {"lt", Op::OpFOrdLessThan},          {"eq", Op::OpFOrdEqual},
    {"le", Op::OpFOrdLessThanEqual},     {"gt", Op::OpFOrdGreaterThan},
    {"lg", Op::OpFOrdNotEqual},          {"ge", Op::OpFOrdGreaterThanEqual},
    {"nge", Op::OpFUnordLessThan},       {"nlg", Op::OpFUnordEqual},
    {"ngt", Op::OpFUnordLessThanEqual},  {"nle", Op::OpFUnordGreaterThan},
    {"neq", Op::OpFUnordNotEqual},       {"nlt", Op::OpFUnordGreaterThanEqual},
};

// The condition and the v_cmpx flag of a float compare's name, empty condition for another name.
struct ParsedFloatCompare {
    std::string_view cond;
    bool writes_exec = false;
};
ParsedFloatCompare parse_float_compare(std::string_view name) {
    for (const bool x : {false, true}) {
        const std::string_view prefix = x ? "v_cmpx_" : "v_cmp_";
        if (name.starts_with(prefix) && name.ends_with("_f32")) {
            return {name.substr(prefix.size(), name.size() - prefix.size() - 4), x};
        }
    }
    return {};
}

// Float VALU operations on two sources, `rev` swaps them.
struct FloatBinary {
    std::string_view name;
    Op op;
    bool rev = false;
};
constexpr FloatBinary float_binary[] = {
    {"v_add_f32", Op::OpFAdd},
    {"v_sub_f32", Op::OpFSub},
    {"v_subrev_f32", Op::OpFSub, true},
    {"v_mul_f32", Op::OpFMul},
};

// Float VALU operations of one source that GLSL.std.450 has an instruction for.
struct FloatUnary {
    std::string_view name;
    GLSLstd450 inst;
};
constexpr FloatUnary float_unary[] = {
    {"v_fract_f32", GLSLstd450Fract},   {"v_trunc_f32", GLSLstd450Trunc},
    {"v_ceil_f32", GLSLstd450Ceil},     {"v_floor_f32", GLSLstd450Floor},
    {"v_rndne_f32", GLSLstd450RoundEven},
};

// COMPUTE_PGM_RSRC1 float mode: FP_ROUND_32 in bits 12-13 (0 nearest even, 3 toward zero),
// FP_DENORM_32 in bits 16-17 (0 flush source and result, 3 keep), and DX10_CLAMP in bit 21.
constexpr unsigned rsrc1_round_shift = 12;
constexpr unsigned rsrc1_denorm_shift = 16;
constexpr std::uint32_t rsrc1_dx10_clamp = 1u << 21;

class Translator {
public:
    Translator(std::span<const std::uint32_t> code, const ShaderKey& key)
        : code_(code), key_(key), program_(decode(code)), cfg_(build_cfg(program_)) {}

    Translation run() {
        check_key();
        collect_masks();
        for (auto cap : {spv::Capability::Shader, spv::Capability::Int64,
                         spv::Capability::PhysicalStorageBufferAddresses}) {
            b_.capability(cap);
        }
        b_.memory_model(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
        bool_ = b_.type_bool();
        u32_ = b_.type_int(32, false);
        u64_ = b_.type_int(64, false);
        const Id void_t = b_.type_void();
        const Id main = b_.begin_function(void_t, b_.type_function(void_t));
        b_.begin_block(b_.id());
        prologue();
        structurize(cfg_, b_,
                    {.body = [this](std::uint32_t block) { body(block); },
                     .condition = [this](std::uint32_t block) { return condition(block); }});
        b_.end_function();
        b_.entry_point(spv::ExecutionModel::GLCompute, main, "main", interface_);
        b_.execution_mode(main, spv::ExecutionMode::LocalSize,
                          {key_.num_threads[0], key_.num_threads[1], key_.num_threads[2]});
        if (uses_subgroup_) {
            // A subgroup operation can follow a branch that isn't uniform in the subgroup (the
            // predicated store of flat()). Only maximal reconvergence promises that the
            // invocations meet again at its merge, so that a ballot or a subgroup barrier sees
            // all of them.
            b_.extension("SPV_KHR_maximal_reconvergence");
            b_.execution_mode(main, spv::ExecutionMode::MaximallyReconvergesKHR);
        }
        float_modes(main);
        return {b_.words(),
                {.buffers = find_buffer_sources(program_, cfg_, user_sgprs()),
                 .wave_size_dependent = wave_size_dependent_,
                 .mask_round_trip = round_trip()}};
    }

private:
    void check_key() const {
        if (key_.stage != Stage::compute) {
            throw GcnError("only the compute stage is translated so far", 0, 0);
        }
        if ((key_.pgm_rsrc2 & rsrc2_unsupported) != 0 || user_sgprs() > 16) {
            throw GcnError(std::format("COMPUTE_PGM_RSRC2 {:#x} is not supported yet",
                                       key_.pgm_rsrc2),
                           0, 0);
        }
        if (std::ranges::find(key_.num_threads, 0u) != key_.num_threads.end()) {
            throw GcnError("a workgroup size of 0", 0, 0);
        }
    }

    // The mask type pass: the SGPR pairs that hold lane masks are the mask operands of the
    // instructions below, and what s_mov_b64 copies to or from a mask. VCC and EXEC are masks. A
    // mask's dwords can still be read as values (a ballot) and written as values (each lane
    // takes its bit).
    void collect_masks() {
        const auto mark = [this](const Operand& o) {
            if (o.kind != OperandKind::scalar || o.dwords != 2 || o.code >= code::flat_scratch) {
                return false;
            }
            if ((o.code & ~1u) < user_sgprs()) {
                fail(std::format("user data s{} is used as a mask", o.code & ~1u));
            }
            return mask_sgprs_.insert(o.code & ~1u).second;
        };
        for (const Instruction& inst : program_) {
            inst_ = &inst;
            for (const Operand* o : mask_operands(inst)) {
                mark(*o);
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (const Instruction& inst : program_) {
                inst_ = &inst;
                if (inst.name == "s_mov_b64" && (is_mask(inst.dst[0]) || is_mask(inst.src[0]))) {
                    changed |= mark(inst.dst[0]);
                    changed |= mark(inst.src[0]);
                }
            }
        }
    }

    // The operands of an instruction that are lane masks.
    static std::vector<const Operand*> mask_operands(const Instruction& inst) {
        const std::string_view name = strip_encoding(inst.name);
        if (name.starts_with("v_cmp")) {
            return {&inst.dst[0]};
        }
        if (name == "v_cndmask_b32") {
            return {&inst.src[2]};
        }
        if (const auto* c = std::ranges::find(carry_ops, name, &CarryOp::name);
            c != std::end(carry_ops)) {
            return c->carry_in ? std::vector{&inst.dst[1], &inst.src[2]}
                               : std::vector{&inst.dst[1]};
        }
        if (bitwise_op(name, "_b64")) {
            return {&inst.dst[0], &inst.src[0], &inst.src[1]};
        }
        if (bitwise_op(name, "_saveexec_b64")) {
            return {&inst.dst[0], &inst.src[0]};
        }
        return {};
    }

    bool is_mask(const Operand& o) const {
        return o.kind == OperandKind::scalar && o.dwords == 2 &&
               (o.code == code::exec || o.code == code::vcc || mask_sgprs_.contains(o.code));
    }

    // A dword of EXEC, VCC or a mask pair.
    bool is_mask_half(unsigned c) const {
        const unsigned pair = c & ~1u;
        return pair == code::exec || pair == code::vcc || mask_sgprs_.contains(pair);
    }

    unsigned user_sgprs() const { return (key_.pgm_rsrc2 >> rsrc2_user_sgpr_shift) & 31; }

    // The draw block and the initial registers: user data in the first SGPRs, the thread ID in
    // X in v0. The lane ID is LocalInvocationIndex modulo 64.
    void prologue() {
        const Id user_t = b_.type_array(u32_, b_.constant_u32(16));
        b_.decorate(user_t, spv::Decoration::ArrayStride, {4});
        const Id draw_t = b_.type_struct({user_t, u64_, u64_});
        b_.decorate(draw_t, spv::Decoration::Block);
        b_.member_decorate(draw_t, 0, spv::Decoration::Offset, {draw_user_data_offset});
        b_.member_decorate(draw_t, 1, spv::Decoration::Offset, {draw_address_bias_offset});
        b_.member_decorate(draw_t, 2, spv::Decoration::Offset, {draw_code_address_offset});
        const Id push_t = b_.type_struct({u64_});
        b_.decorate(push_t, spv::Decoration::Block);
        b_.member_decorate(push_t, 0, spv::Decoration::Offset, {0});
        const Id push = b_.variable(b_.type_pointer(SC::PushConstant, push_t), SC::PushConstant);
        interface_.push_back(push);

        const Id c0 = b_.constant_u32(0);
        const Id address = load(u64_, b_.op(Op::OpAccessChain,
                                            b_.type_pointer(SC::PushConstant, u64_), {push, c0}));
        const Id draw = b_.op(Op::OpConvertUToPtr, global_pointer(draw_t), {address});
        for (unsigned k = 0; k < user_sgprs(); ++k) {
            const Id p = b_.op(Op::OpAccessChain, global_pointer(u32_),
                               {draw, c0, b_.constant_u32(k)});
            store(sgpr(k), load_global(u32_, p, 4));
        }
        bias_ = load_global(
            u64_, b_.op(Op::OpAccessChain, global_pointer(u64_), {draw, b_.constant_u32(1)}), 8);

        const Id index = load(u32_, builtin(spv::BuiltIn::LocalInvocationIndex, u32_));
        lane_ = b_.op(Op::OpBitwiseAnd, u32_, {index, b_.constant_u32(63)});
        const Id uvec3 = b_.type_vector(u32_, 3);
        const Id local = load(uvec3, builtin(spv::BuiltIn::LocalInvocationId, uvec3));
        store(vgpr(0), b_.op(Op::OpCompositeExtract, u32_, {local, 0}));
    }

    // The input variable of a built-in, declared on first use. `type` defaults to u32.
    Id builtin(spv::BuiltIn which, Id type = 0) {
        auto [it, added] = builtins_.try_emplace(spirv_word(which), 0);
        if (added) {
            const Id pointer = b_.type_pointer(SC::Input, type != 0 ? type : u32_);
            it->second = b_.variable(pointer, SC::Input);
            b_.decorate(it->second, spv::Decoration::BuiltIn, {spirv_word(which)});
            interface_.push_back(it->second);
        }
        return it->second;
    }

    // Emits a block's instructions. Its branch or s_endpgm is the block's exit.
    void body(std::uint32_t index) {
        const Block& block = cfg_.blocks[index];
        for (std::uint32_t i = block.first; i < block.first + block.count; ++i) {
            const Instruction& inst = program_[i];
            inst_ = &inst;
            if (inst.format == Format::sopp && inst.opcode == op_barrier) {
                barrier();
                continue;
            }
            if (branch_target(inst) || is_endpgm(inst) || inst.name == "s_nop" ||
                inst.name == "s_waitcnt") {
                continue;
            }
            switch (inst.format) {
            case Format::sop1:
            case Format::sop2:
                scalar(inst);
                break;
            case Format::sopc:
                scalar_compare(inst);
                break;
            case Format::vop1:
            case Format::vop2:
            case Format::vopc:
            case Format::vop3:
                vector(inst);
                break;
            case Format::flat:
                flat(inst);
                break;
            case Format::smrd:
                scalar_memory(inst);
                break;
            case Format::mubuf:
                buffer_memory(inst);
                break;
            case Format::ds:
                ds(inst);
                break;
            default:
                unsupported();
            }
        }
    }

    // The condition of a block's conditional branch. The branches on a mask test it over the
    // subgroup (any), which is the wave's value only if the subgroup is the wave.
    Id condition(std::uint32_t index) {
        const Block& block = cfg_.blocks[index];
        inst_ = &program_[block.first + block.count - 1];
        const auto negate = [&](Id v) { return b_.op(Op::OpLogicalNot, bool_, {v}); };
        switch (inst_->opcode) {
        case op_cbranch_scc0: return negate(load(bool_, scc()));
        case op_cbranch_scc1: return load(bool_, scc());
        case op_cbranch_vccz: return negate(any(load(bool_, mask_var(code::vcc))));
        case op_cbranch_vccnz: return any(load(bool_, mask_var(code::vcc)));
        case op_cbranch_execz: return negate(any(active()));
        case op_cbranch_execnz: return any(active());
        default: unsupported();
        }
    }

    // SOP1 and SOP2. Scalar results are not predicated.
    void scalar(const Instruction& inst) {
        const std::string_view name = inst.name;
        const Operand& dst = inst.dst[0];
        if (name == "s_mov_b64" && is_mask(dst)) {
            mask_copy(dst, inst.src[0]);
        } else if (name == "s_mov_b32" || name == "s_mov_b64") {
            const unsigned dwords = name == "s_mov_b64" ? 2 : 1;
            Id value[2] = {};
            for (unsigned d = 0; d < dwords; ++d) {
                value[d] = read(inst.src[0], d);
            }
            for (unsigned d = 0; d < dwords; ++d) {
                write_scalar(dst, value[d], d);
            }
        } else if (name == "s_add_u32" || name == "s_sub_u32") {  // SCC: the carry or borrow
            const bool add = name == "s_add_u32";
            const Id a = read(inst.src[0]);
            const Id b = read(inst.src[1]);
            const Id r = b_.op(add ? Op::OpIAdd : Op::OpISub, u32_, {a, b});
            write_scalar(dst, r);
            store(scc(), b_.op(Op::OpULessThan, bool_, add ? SpirvBuilder::Words{r, a}
                                                           : SpirvBuilder::Words{a, b}));
        } else if (name.starts_with("s_bcnt") || name.starts_with("s_ff") ||
                   name.starts_with("s_flbit")) {
            scalar_count(inst);
        } else if (const auto op = bitwise_op(name, "_b32")) {
            const Id r = combine(*op, u32_, read(inst.src[0]), read(inst.src[1]));
            write_scalar(dst, r);
            store(scc(), b_.op(Op::OpINotEqual, bool_, {r, b_.constant_u32(0)}));
        } else if (const auto op = bitwise_op(name, "_b64")) {
            mask_flow(inst.src[0], dst);
            mask_flow(inst.src[1], dst);
            const Id r = combine(*op, bool_, read_mask(inst.src[0]), read_mask(inst.src[1]));
            write_mask(dst, r);
            store(scc(), any(r));
        } else if (const auto op = bitwise_op(name, "_saveexec_b64")) {  // dst = EXEC first
            mask_flow(inst.src[0], exec_operand);
            mask_flow(exec_operand, dst);
            const Id old = active();
            const Id r = combine(*op, bool_, read_mask(inst.src[0]), old);
            write_mask(dst, old);
            store(mask_exec(), r);
            store(scc(), any(r));
        } else {
            unsupported();
        }
    }

    // The SOP1 counts of a 32- or 64-bit source: s_bcnt0/1 count the clear or set bits (SCC: the
    // count isn't 0), s_ff0/1 find the first clear or set bit from the LSB, s_flbit_i32_b32/b64
    // the first set bit from the MSB, and the signed s_flbit_i32/_i64 the first bit from the MSB
    // that differs from the sign. A search that finds nothing gives -1. A mask source is read
    // through a ballot.
    void scalar_count(const Instruction& inst) {
        const std::string_view name = inst.name;
        const unsigned n = inst.src[0].dwords;
        std::array<Id, 2> w = read_words(inst.src[0]);
        Id r;
        if (name.starts_with("s_bcnt")) {
            r = b_.op(Op::OpBitCount, u32_, {w[0]});
            if (n == 2) {
                r = b_.op(Op::OpIAdd, u32_, {r, b_.op(Op::OpBitCount, u32_, {w[1]})});
            }
            if (name.starts_with("s_bcnt0")) {
                r = b_.op(Op::OpISub, u32_, {b_.constant_u32(32 * n), r});
            }
            store(scc(), b_.op(Op::OpINotEqual, bool_, {r, b_.constant_u32(0)}));
        } else if (name.starts_with("s_ff")) {
            if (name.starts_with("s_ff0")) {
                for (unsigned d = 0; d < n; ++d) {
                    w[d] = b_.op(Op::OpNot, u32_, {w[d]});
                }
            }
            r = find_first(w, n, false);
        } else {
            if (name == "s_flbit_i32" || name == "s_flbit_i32_i64") {
                const Id sign = b_.op(Op::OpShiftRightArithmetic, u32_,
                                      {w[n - 1], b_.constant_u32(31)});
                for (unsigned d = 0; d < n; ++d) {
                    w[d] = b_.op(Op::OpBitwiseXor, u32_, {w[d], sign});
                }
            }
            r = find_first(w, n, true);
        }
        write_scalar(inst.dst[0], r);
    }

    // The position of the first set bit of the n-dword value `w` (low dword first), counted from
    // the LSB, or from the MSB when `from_msb`; -1 when no bit is set.
    Id find_first(const std::array<Id, 2>& w, unsigned n, bool from_msb) {
        const Id glsl = b_.import_extended("GLSL.std.450");
        Id r = b_.constant_u32(~0u);
        for (unsigned k = 0; k < n; ++k) {  // the dword searched last first
            const unsigned d = from_msb ? k : n - 1 - k;
            Id pos;
            if (from_msb) {
                const Id msb = b_.op(Op::OpExtInst, u32_, {glsl, GLSLstd450FindUMsb, w[d]});
                pos = b_.op(Op::OpISub, u32_, {b_.constant_u32(32 * (n - d) - 1), msb});
            } else {
                const Id lsb = b_.op(Op::OpExtInst, u32_, {glsl, GLSLstd450FindILsb, w[d]});
                pos = b_.op(Op::OpIAdd, u32_, {lsb, b_.constant_u32(32 * d)});
            }
            r = b_.op(Op::OpSelect, u32_,
                      {b_.op(Op::OpINotEqual, bool_, {w[d], b_.constant_u32(0)}), pos, r});
        }
        return r;
    }

    // SOPC: s_cmp_<cond>_<i32|u32> sets SCC.
    void scalar_compare(const Instruction& inst) {
        const auto compare = parse_compare(inst.name);
        if (!compare) {
            unsupported();
        }
        store(scc(), int_compare(*compare, read(inst.src[0]), read(inst.src[1])));
    }

    // VOP1, VOP2 and VOP3: the result is written as select(active, new, old).
    void vector(const Instruction& inst) {
        const std::string_view name = strip_encoding(inst.name);
        if (float_vector(inst, name)) {  // takes the modifiers itself
            return;
        }
        if (has_modifiers(inst)) {
            fail(std::format("modifiers of {} are not supported yet", inst.name));
        }
        if (name == "v_nop") {
            return;
        }
        const auto op = std::ranges::find(vector_ops, name, &VectorOp::name);
        const auto carry = std::ranges::find(carry_ops, name, &CarryOp::name);
        if (name == "v_mov_b32") {
            write_vector(inst.dst[0], read(inst.src[0]));
        } else if (name == "v_cndmask_b32") {  // src1 where the mask bit is set
            write_vector(inst.dst[0], b_.op(Op::OpSelect, u32_,
                                            {read_mask(inst.src[2]), read(inst.src[1]),
                                             read(inst.src[0])}));
        } else if (const auto compare = parse_compare(name)) {
            // The mask gets `active && result`, and v_cmpx sets EXEC to it as well.
            const Id r = b_.op(Op::OpLogicalAnd, bool_,
                               {active(), int_compare(*compare, read(inst.src[0]),
                                                      read(inst.src[1]))});
            write_mask(inst.dst[0], r);
            if (compare->writes_exec) {
                store(mask_exec(), r);
            }
        } else if (carry != std::end(carry_ops)) {
            vector_carry(*carry, inst);
        } else if (name == "v_readfirstlane_b32") {  // lane 0 when EXEC is 0
            const Id first = find_first(ballot(active()), 2, false);
            const Id none = b_.op(Op::OpIEqual, bool_, {first, b_.constant_u32(~0u)});
            const Id lane = b_.op(Op::OpSelect, u32_, {none, b_.constant_u32(0), first});
            write_scalar(inst.dst[0], broadcast(read(inst.src[0]), lane));
        } else if (name == "v_readlane_b32" || name == "v_writelane_b32") {
            // Both ignore EXEC. The lane select is taken modulo 64.
            const Id lane = b_.op(Op::OpBitwiseAnd, u32_, {read(inst.src[1]), b_.constant_u32(63)});
            if (name == "v_readlane_b32") {
                write_scalar(inst.dst[0], broadcast(read(inst.src[0]), lane));
            } else {
                const Id var = vgpr(inst.dst[0].code);
                store(var, b_.op(Op::OpSelect, u32_,
                                 {b_.op(Op::OpIEqual, bool_, {lane_, lane}), read(inst.src[0]),
                                  load(u32_, var)}));
            }
        } else if (name == "v_mbcnt_lo_u32_b32" || name == "v_mbcnt_hi_u32_b32") {
            // src1 plus the bits of src0 that belong to the lanes below this one: of lanes 0-31
            // for _lo, of lanes 32-63 for _hi.
            const bool hi = name == "v_mbcnt_hi_u32_b32";
            const Id below = b_.op(
                Op::OpISub, u32_,
                {b_.op(Op::OpShiftLeftLogical, u32_,
                       {b_.constant_u32(1),
                        b_.op(Op::OpBitwiseAnd, u32_, {lane_, b_.constant_u32(31)})}),
                 b_.constant_u32(1)});
            const Id lower = b_.op(Op::OpULessThan, bool_, {lane_, b_.constant_u32(32)});
            const Id mask = b_.op(Op::OpSelect, u32_,
                                  {lower, hi ? b_.constant_u32(0) : below,
                                   hi ? below : b_.constant_u32(~0u)});
            const Id count = b_.op(Op::OpBitCount, u32_,
                                   {b_.op(Op::OpBitwiseAnd, u32_, {read(inst.src[0]), mask})});
            write_vector(inst.dst[0], b_.op(Op::OpIAdd, u32_, {count, read(inst.src[1])}));
        } else if (op != std::end(vector_ops)) {
            const Id x = read(inst.src[op->rev ? 1 : 0]);
            Id y = read(inst.src[op->rev ? 0 : 1]);
            if (op->op == Op::OpShiftLeftLogical) {
                y = b_.op(Op::OpBitwiseAnd, u32_, {y, b_.constant_u32(31)});
            }
            write_vector(inst.dst[0], b_.op(op->op, u32_, {x, y}));
        } else {
            unsupported();
        }
    }

    // The VALU additions and subtractions with a carry or borrow out in a mask, and from one.
    void vector_carry(const CarryOp& c, const Instruction& inst) {
        Id a = read(inst.src[0]);
        Id b = read(inst.src[1]);
        if (c.rev) {
            std::swap(a, b);
        }
        const auto op = [&](Op o, Id x, Id y) { return b_.op(o, u32_, {x, y}); };
        const auto test = [&](Op o, Id x, Id y) { return b_.op(o, bool_, {x, y}); };
        Id r = op(c.sub ? Op::OpISub : Op::OpIAdd, a, b);
        Id out = c.sub ? test(Op::OpULessThan, a, b) : test(Op::OpULessThan, r, a);
        if (c.carry_in) {
            const Id in = b_.op(Op::OpSelect, u32_, {read_mask(inst.src[2]), b_.constant_u32(1),
                                                     b_.constant_u32(0)});
            const Id t = r;
            r = op(c.sub ? Op::OpISub : Op::OpIAdd, t, in);
            out = b_.op(Op::OpLogicalOr, bool_,
                        {out, c.sub ? test(Op::OpULessThan, t, in) : test(Op::OpULessThan, r, t)});
        }
        write_vector(inst.dst[0], r);
        write_mask(inst.dst[1], b_.op(Op::OpLogicalAnd, bool_, {active(), out}));
    }

    // `a` compared with `b` under `c`; the operands are u32 whatever the signedness. Not equal is
    // spelled "ne" by the VALU compares and "lg" by the SOPC ones.
    Id int_compare(const Compare& c, Id a, Id b) {
        if (c.cond == "f" || c.cond == "t") {
            return b_.constant_bool(c.cond == "t");
        }
        const bool s = c.is_signed;
        Op op;
        if (c.cond == "eq") {
            op = Op::OpIEqual;
        } else if (c.cond == "ne" || c.cond == "lg") {
            op = Op::OpINotEqual;
        } else if (c.cond == "lt") {
            op = s ? Op::OpSLessThan : Op::OpULessThan;
        } else if (c.cond == "le") {
            op = s ? Op::OpSLessThanEqual : Op::OpULessThanEqual;
        } else if (c.cond == "gt") {
            op = s ? Op::OpSGreaterThan : Op::OpUGreaterThan;
        } else {
            op = s ? Op::OpSGreaterThanEqual : Op::OpUGreaterThanEqual;
        }
        return b_.op(op, bool_, {a, b});
    }

    // The bitwise operation `op` of the SALU family on bools (lane bits) or u32 values.
    Id combine(unsigned op, Id type, Id a, Id b) {
        const bool logical = type == bool_;
        const auto bin = [&](Op l, Op w, Id x, Id y) {
            return b_.op(logical ? l : w, type, {x, y});
        };
        const auto inv = [&](Id x) {
            return b_.op(logical ? Op::OpLogicalNot : Op::OpNot, type, {x});
        };
        const auto land = [&](Id x, Id y) { return bin(Op::OpLogicalAnd, Op::OpBitwiseAnd, x, y); };
        const auto lor = [&](Id x, Id y) { return bin(Op::OpLogicalOr, Op::OpBitwiseOr, x, y); };
        const auto lxor = [&](Id x, Id y) {
            return bin(Op::OpLogicalNotEqual, Op::OpBitwiseXor, x, y);
        };
        switch (op) {
        case 0: return land(a, b);
        case 1: return lor(a, b);
        case 2: return lxor(a, b);
        case 3: return land(a, inv(b));
        case 4: return lor(a, inv(b));
        case 5: return inv(land(a, b));
        case 6: return inv(lor(a, b));
        default: return inv(lxor(a, b));
        }
    }

    // True when `v` is true in any lane of the subgroup. This is the wave's "any" only if the
    // subgroup is the whole wave, so the shader depends on the wave size.
    Id any(Id v) {
        cross_lane();
        b_.capability(spv::Capability::GroupNonUniformVote);
        return b_.op(Op::OpGroupNonUniformAny, bool_,
                     {b_.constant_u32(spirv_word(spv::Scope::Subgroup)), v});
    }

    // ---- Float VALU, on f32 ----
    // The registers hold u32, so a float source is a bit cast. VOP3's neg and abs apply to float
    // sources (abs first), omod and clamp to float results. A source or result of another type
    // with such a modifier is an error, as the modifiers of the integer instructions are.

    static bool has_modifiers(const Instruction& inst) {
        return inst.neg != 0 || inst.abs != 0 || inst.omod != 0 || (inst.flags & flag::clamp) != 0;
    }

    // The float VALU instructions (not the masks: float compares write one). Returns false for
    // any other instruction.
    bool float_vector(const Instruction& inst, std::string_view name) {
        if (const auto cmp = parse_float_compare(name); !cmp.cond.empty()) {
            float_compare(cmp, inst);
            return true;
        }
        const Id f32 = b_.type_float(32);
        const auto src = [&](unsigned i) { return float_source(inst, i); };
        const auto ext = [&](GLSLstd450 f, std::initializer_list<Id> args) {
            SpirvBuilder::Words words = {b_.import_extended("GLSL.std.450"), f};
            words.insert(words.end(), args.begin(), args.end());
            return b_.op(Op::OpExtInst, f32, words);
        };
        Id r = 0;     // an f32 result
        Id bits = 0;  // a u32 result
        const auto b = std::ranges::find(float_binary, name, &FloatBinary::name);
        const auto u = std::ranges::find(float_unary, name, &FloatUnary::name);
        if (b != std::end(float_binary)) {
            r = arith(b->op, src(b->rev ? 1 : 0), src(b->rev ? 0 : 1));
        } else if (u != std::end(float_unary)) {
            r = ext(u->inst, {src(0)});
            if (u->inst == GLSLstd450Fract) {
                // x - floor(x) rounds to 1.0 for a tiny negative x; the hardware stays below 1
                // (LLVM's model of CI and later: min(x - floor(x), 0x1.fffffep-1)). A NaN is no
                // 1.0 and stays.
                const Id is_one = b_.op(Op::OpFOrdEqual, bool_, {r, b_.constant_f32(1.0f)});
                const Id below_one = b_.constant_f32(std::nextafter(1.0f, 0.0f));
                r = b_.op(Op::OpSelect, f32, {is_one, below_one, r});
            }
        } else if (name == "v_mul_legacy_f32") {
            r = legacy_mul(src(0), src(1));
        } else if (name == "v_mac_f32" || name == "v_mac_legacy_f32") {  // vdst += src0 * src1
            note_float();
            r = fmad(name == "v_mac_legacy_f32", src(0), src(1),
                     b_.op(Op::OpBitcast, f32, {load(u32_, vgpr(inst.dst[0].code))}));
        } else if (name == "v_mad_f32" || name == "v_mad_legacy_f32" || name == "v_madmk_f32" ||
                   name == "v_madak_f32") {  // src0 * src1 + src2, with K in place of one source
            r = fmad(name == "v_mad_legacy_f32", src(0), src(1), src(2));
        } else if (name == "v_fma_f32") {
            r = ext(GLSLstd450Fma, {src(0), src(1), src(2)});
        } else if (name == "v_min_f32" || name == "v_max_f32") {  // a NaN operand gives the other
            const Id a = src(0), b = src(1);
            const Id m = ext(name == "v_min_f32" ? GLSLstd450FMin : GLSLstd450FMax, {a, b});
            const Id not_b = b_.op(Op::OpSelect, f32, {is_nan(b), a, m});
            r = b_.op(Op::OpSelect, f32, {is_nan(a), b, not_b});
        } else if (name == "v_min_legacy_f32" || name == "v_max_legacy_f32") {
            const bool min = name == "v_min_legacy_f32";
            const Id a = src(0), b = src(1);
            const Id first = b_.op(min ? Op::OpFOrdLessThan : Op::OpFOrdGreaterThanEqual, bool_,
                                   {a, b});
            r = b_.op(Op::OpSelect, f32, {first, a, b});
        } else if (name == "v_cvt_f32_i32" || name == "v_cvt_f32_u32") {
            no_source_modifiers(inst);
            r = b_.op(name == "v_cvt_f32_i32" ? Op::OpConvertSToF : Op::OpConvertUToF, f32,
                      {read(inst.src[0])});
        } else if (name == "v_cvt_i32_f32" || name == "v_cvt_u32_f32" ||
                   name == "v_cvt_flr_i32_f32" || name == "v_cvt_rpi_i32_f32") {
            Id x = src(0);  // floor(x + 0.5) for rpi
            if (name == "v_cvt_flr_i32_f32") {
                x = ext(GLSLstd450Floor, {x});
            } else if (name == "v_cvt_rpi_i32_f32") {
                x = ext(GLSLstd450Floor, {arith(Op::OpFAdd, x, b_.constant_f32(0.5f))});
            }
            bits = float_to_int(x, name != "v_cvt_u32_f32");
        } else {
            return false;
        }
        note_float();
        if (bits != 0) {  // an integer result takes no omod or clamp
            if (inst.omod != 0 || (inst.flags & flag::clamp) != 0) {
                fail(std::format("modifiers of {} are not supported yet", inst.name));
            }
            write_vector(inst.dst[0], bits);
        } else {
            write_vector(inst.dst[0], b_.op(Op::OpBitcast, u32_, {float_result(inst, r)}));
        }
        return true;
    }

    void float_compare(const ParsedFloatCompare& c, const Instruction& inst) {
        if (inst.omod != 0 || (inst.flags & flag::clamp) != 0) {
            fail(std::format("modifiers of {} are not supported yet", inst.name));
        }
        note_float();
        Id result;
        const Id a = float_source(inst, 0), b = float_source(inst, 1);
        if (c.cond == "f" || c.cond == "tru") {
            result = b_.constant_bool(c.cond == "tru");
        } else if (c.cond == "o" || c.cond == "u") {  // OpOrdered needs the Kernel capability
            result = b_.op(Op::OpLogicalOr, bool_, {is_nan(a), is_nan(b)});
            if (c.cond == "o") {
                result = b_.op(Op::OpLogicalNot, bool_, {result});
            }
        } else {
            const auto* cmp = std::ranges::find(float_compares, c.cond, &FloatCompare::cond);
            if (cmp == std::end(float_compares)) {
                unsupported();
            }
            result = b_.op(cmp->op, bool_, {a, b});
        }
        const Id r = b_.op(Op::OpLogicalAnd, bool_, {active(), result});
        write_mask(inst.dst[0], r);
        if (c.writes_exec) {
            store(mask_exec(), r);
        }
    }

    // Source `i` as an f32, with its abs and neg modifiers.
    Id float_source(const Instruction& inst, unsigned i) {
        const Id f32 = b_.type_float(32);
        Id x = b_.op(Op::OpBitcast, f32, {read(inst.src[i])});
        if ((inst.abs >> i & 1) != 0) {
            x = b_.op(Op::OpExtInst, f32, {b_.import_extended("GLSL.std.450"), GLSLstd450FAbs, x});
        }
        if ((inst.neg >> i & 1) != 0) {
            x = b_.op(Op::OpFNegate, f32, {x});
        }
        return x;
    }

    void no_source_modifiers(const Instruction& inst) const {
        if (inst.neg != 0 || inst.abs != 0) {
            fail(std::format("modifiers of {} are not supported yet", inst.name));
        }
    }

    // A float result after omod (1: x2, 2: x4, 3: /2), then clamp to [0, 1]. With DX10_CLAMP a
    // NaN clamps to 0, without it the NaN stays.
    Id float_result(const Instruction& inst, Id x) {
        const Id f32 = b_.type_float(32);
        static constexpr float scale[] = {1.0f, 2.0f, 4.0f, 0.5f};
        if (inst.omod != 0) {
            x = arith(Op::OpFMul, x, b_.constant_f32(scale[inst.omod & 3]));
        }
        if ((inst.flags & flag::clamp) != 0) {
            const Id glsl = b_.import_extended("GLSL.std.450");
            const Id c = b_.op(Op::OpExtInst, f32,
                               {glsl, GLSLstd450FClamp, x, b_.constant_f32(0.0f),
                                b_.constant_f32(1.0f)});
            const bool dx10 = (key_.pgm_rsrc1 & rsrc1_dx10_clamp) != 0;
            x = b_.op(Op::OpSelect, f32, {is_nan(x), dx10 ? b_.constant_f32(0.0f) : x, c});
        }
        return x;
    }

    Id is_nan(Id x) { return b_.op(Op::OpIsNan, bool_, {x}); }

    // A float add, subtract or multiply that the driver must not fuse with the operation before
    // or after it: GCN rounds every instruction's result, and only v_fma_f32 fuses.
    Id arith(Op op, Id x, Id y) {
        const Id r = b_.op(op, b_.type_float(32), {x, y});
        b_.decorate(r, spv::Decoration::NoContraction);
        return r;
    }

    // x * y + z with the product rounded first, as v_mad_f32 does. The legacy form takes 0 for a
    // product with a zero operand, also with an infinity or NaN as the other.
    Id fmad(bool legacy, Id x, Id y, Id z) {
        return arith(Op::OpFAdd, legacy ? legacy_mul(x, y) : arith(Op::OpFMul, x, y), z);
    }

    Id legacy_mul(Id x, Id y) {
        const Id f32 = b_.type_float(32);
        const Id zero = b_.constant_f32(0.0f);
        const Id either = b_.op(Op::OpLogicalOr, bool_, {b_.op(Op::OpFOrdEqual, bool_, {x, zero}),
                                                         b_.op(Op::OpFOrdEqual, bool_, {y, zero})});
        return b_.op(Op::OpSelect, f32, {either, zero, arith(Op::OpFMul, x, y)});
    }

    // A float converted to a 32-bit integer, rounded toward zero and saturated; a NaN gives 0.
    Id float_to_int(Id x, bool is_signed) {
        const Id f32 = b_.type_float(32);
        const Id glsl = b_.import_extended("GLSL.std.450");
        // The largest float below 2^31 and below 2^32 keep the conversion in range.
        const float low = is_signed ? -2147483648.0f : 0.0f;
        const float high = is_signed ? 2147483520.0f : 4294967040.0f;
        const Id in_range = b_.op(Op::OpExtInst, f32,
                                  {glsl, GLSLstd450FClamp, x, b_.constant_f32(low),
                                   b_.constant_f32(high)});
        Id r = b_.op(is_signed ? Op::OpConvertFToS : Op::OpConvertFToU, u32_, {in_range});
        const Id over = b_.op(Op::OpFOrdGreaterThanEqual, bool_,
                              {x, b_.constant_f32(is_signed ? 2147483648.0f : 4294967296.0f)});
        r = b_.op(Op::OpSelect, u32_, {over, b_.constant_u32(is_signed ? 0x7fffffff : ~0u), r});
        return b_.op(Op::OpSelect, u32_, {is_nan(x), b_.constant_u32(0), r});
    }

    // The first float instruction asks for the execution modes of the key's float mode.
    void note_float() { float_used_ = true; }

    // The f32 execution modes: GCN keeps signed zeros, infinities and NaNs, rounds to nearest even
    // or toward zero and flushes denormals or keeps them, as COMPUTE_PGM_RSRC1 says. The driver
    // must support them (the float controls properties); the run time checks that.
    void float_modes(Id main) {
        if (!float_used_) {
            return;
        }
        const unsigned round = key_.pgm_rsrc1 >> rsrc1_round_shift & 3;
        const unsigned denorm = key_.pgm_rsrc1 >> rsrc1_denorm_shift & 3;
        if ((round != 0 && round != 3) || (denorm != 0 && denorm != 3)) {
            throw GcnError(std::format("COMPUTE_PGM_RSRC1 {:#x}: this float mode is not supported",
                                       key_.pgm_rsrc1),
                           0, 0);
        }
        const auto mode = [&](spv::Capability cap, spv::ExecutionMode m) {
            b_.capability(cap);
            b_.execution_mode(main, m, {32});
        };
        using Cap = spv::Capability;
        using Mode = spv::ExecutionMode;
        mode(Cap::SignedZeroInfNanPreserve, Mode::SignedZeroInfNanPreserve);
        if (round == 0) {
            mode(Cap::RoundingModeRTE, Mode::RoundingModeRTE);
        } else {
            mode(Cap::RoundingModeRTZ, Mode::RoundingModeRTZ);
        }
        if (denorm == 0) {
            mode(Cap::DenormFlushToZero, Mode::DenormFlushToZero);
        } else {
            mode(Cap::DenormPreserve, Mode::DenormPreserve);
        }
    }

    // flat_store_dword reaches guest memory through the address bias, in active lanes only. There
    // is no range check, and the LDS and scratch apertures are not handled.
    void flat(const Instruction& inst) {
        if (inst.name != "flat_store_dword") {
            unsupported();
        }
        guarded_store(active(), b_.op(Op::OpIAdd, u64_, {read64(inst.src[0]), bias_}),
                      read(inst.src[1]));
    }

    // SMRD, in every lane: s_load_dword* reads at a 64-bit address from an SGPR pair, with no
    // range check. s_buffer_load_dword* reads through a V# in four SGPRs, and each dword that
    // is not inside the buffer (`in_range`) reads as 0. The offset is in dwords when it is the
    // instruction's immediate or the literal that follows it, and in bytes when it is an SGPR (the
    // low 2 bits are ignored).
    void scalar_memory(const Instruction& inst) {
        const std::string_view name = inst.name;
        const bool buffer = name.starts_with("s_buffer_load_dword");
        if (!buffer && !name.starts_with("s_load_dword")) {
            unsupported();
        }
        const Operand& o = inst.src[1];
        Id offset;
        if (o.kind == OperandKind::scalar) {
            offset = b_.op(Op::OpUConvert, u64_,
                           {b_.op(Op::OpBitwiseAnd, u32_, {read(o), b_.constant_u32(~3u)})});
        } else {
            offset = b_.op(Op::OpShiftLeftLogical, u64_,
                           {b_.op(Op::OpUConvert, u64_, {read(o)}), b_.constant_u32(2)});
        }
        Vsharp v;
        if (buffer) {
            v = vsharp(inst.src[0]);
        }
        const Id guest = b_.op(Op::OpIAdd, u64_, {buffer ? v.base : read64(inst.src[0]), offset});
        for (unsigned d = 0; d < inst.dst[0].dwords; ++d) {
            const Id address = add(add(guest, c64(4 * d)), bias_);
            write_scalar(inst.dst[0],
                         buffer ? guarded_load(in_range(v, offset, d), address)
                                : load_global(u32_, device_pointer(address), 4),
                         d);
        }
    }

    // MUBUF loads and stores of dwords, untyped, in active lanes (docs/design/graphics.md,
    // "Resources and memory"). The byte offset is the instruction's, plus the VGPR offset with
    // `offen`; with `idxen` the VGPR index adds index * stride; soffset only moves the address
    // and is never part of the range check. Each dword is checked on its own: raw (no `idxen`, or
    // a stride of 0) by `in_range` on the offset, structured (`idxen` and a stride) by index <
    // num_records, with no check of the offset. `addr64` adds a 64-bit VGPR address and is not
    // range-checked. Out-of-range loads read 0 and out-of-range stores are dropped.
    void buffer_memory(const Instruction& inst) {
        const std::string_view name = inst.name;
        const bool load = name.starts_with("buffer_load_dword");
        if (!load && !name.starts_with("buffer_store_dword")) {
            unsupported();
        }
        if ((inst.flags & (flag::tfe | flag::lds)) != 0) {
            fail(std::format("{} with tfe or lds is not supported yet", name));
        }
        const Operand& data = load ? inst.dst[0] : inst.src[0];
        const Operand* a = &inst.src[load ? 0 : 1];
        const Vsharp v = vsharp(a[1]);
        Id guest = b_.op(Op::OpIAdd, u64_, {v.base, b_.op(Op::OpUConvert, u64_, {read(a[2])})});
        Id offset = c64(inst.mem_offset), index = c64(0);
        unsigned next = 0;
        if ((inst.flags & flag::addr64) != 0) {
            guest = b_.op(Op::OpIAdd, u64_, {guest, read64(a[0])});
        }
        if ((inst.flags & flag::idxen) != 0) {
            index = b_.op(Op::OpUConvert, u64_, {read(a[0], next++)});
        }
        if ((inst.flags & flag::offen) != 0) {
            offset = b_.op(Op::OpIAdd, u64_,
                           {offset, b_.op(Op::OpUConvert, u64_, {read(a[0], next++)})});
        }
        guest = add(guest, add(offset, b_.op(Op::OpIMul, u64_, {index, v.stride})));
        const bool checked = (inst.flags & flag::addr64) == 0;
        const bool by_index = (inst.flags & flag::idxen) != 0;
        const Id structured = b_.op(Op::OpINotEqual, bool_, {v.stride, c64(0)});
        const Id index_ok = b_.op(Op::OpULessThan, bool_, {index, v.records});
        for (unsigned d = 0; d < data.dwords; ++d) {
            const Id address = add(add(guest, c64(4 * d)), bias_);
            Id ok = b_.constant_bool(true);
            if (checked) {
                ok = in_range(v, offset, d);
                if (by_index) {
                    ok = b_.op(Op::OpSelect, bool_, {structured, index_ok, ok});
                }
            }
            const Id cond = b_.op(Op::OpLogicalAnd, bool_, {active(), ok});
            if (load) {
                write_vector(data, guarded_load(cond, address), d);
            } else {
                guarded_store(cond, address, read(data, d));
            }
        }
    }

    // The fields of a V#: its 48-bit base address, its stride, num_records and its size in bytes.
    struct Vsharp {
        Id base = 0, stride = 0, records = 0, size = 0;  // u64
    };
    Vsharp vsharp(const Operand& o) {
        const Id dw1 = read(o, 1);
        const Id high = b_.op(Op::OpBitwiseAnd, u32_, {dw1, b_.constant_u32(0xffff)});
        Vsharp v;
        v.base = b_.op(Op::OpBitwiseOr, u64_,
                       {b_.op(Op::OpShiftLeftLogical, u64_,
                              {b_.op(Op::OpUConvert, u64_, {high}), b_.constant_u32(32)}),
                        b_.op(Op::OpUConvert, u64_, {read(o, 0)})});
        const Id stride = b_.op(Op::OpBitFieldUExtract, u32_,
                                {dw1, b_.constant_u32(16), b_.constant_u32(14)});
        v.stride = b_.op(Op::OpUConvert, u64_, {stride});
        v.records = b_.op(Op::OpUConvert, u64_, {read(o, 2)});
        // The size is num_records bytes for a stride of 0. For a stride, the unit of num_records
        // is taken to be records (unverified for a raw access, see docs/design/graphics.md).
        const Id unit = b_.op(Op::OpSelect, u64_,
                              {b_.op(Op::OpIEqual, bool_, {stride, b_.constant_u32(0)}),
                               c64(1), v.stride});
        v.size = b_.op(Op::OpIMul, u64_, {v.records, unit});
        return v;
    }

    // The raw range rule: dword `d` of an access at byte `offset` of the buffer is inside when
    // offset + 4 * d < size. The access is not all-or-nothing: each dword of a pair or a quad is
    // checked on its own.
    Id in_range(const Vsharp& v, Id offset, unsigned d) {
        return b_.op(Op::OpULessThan, bool_, {add(offset, c64(4 * d)), v.size});
    }

    // Runs `body` in the lanes where `cond` holds, as a selection.
    template <class F>
    void when(Id cond, F&& body) {
        const Id then = b_.id();
        const Id merge = b_.id();
        b_.op_void(Op::OpSelectionMerge, {merge, spirv_word(spv::SelectionControlMask::MaskNone)});
        b_.op_void(Op::OpBranchConditional, {cond, then, merge});
        b_.begin_block(then);
        body();
        b_.op_void(Op::OpBranch, {merge});
        b_.begin_block(merge);
    }

    // A dword at a device address where `cond` holds, 0 elsewhere; an address that `cond`
    // excludes is never touched. Accesses are 4-byte aligned, as the compilers emit them.
    Id guarded_load(Id cond, Id address) {
        const Id result = b_.local_variable(b_.type_pointer(SC::Function, u32_));
        store(result, b_.constant_u32(0));
        when(cond, [&] { store(result, load_global(u32_, device_pointer(address), 4)); });
        return load(u32_, result);
    }
    void guarded_store(Id cond, Id address, Id value) {
        when(cond, [&] {
            b_.op_void(Op::OpStore, {device_pointer(address), value,
                                     spirv_word(spv::MemoryAccessMask::Aligned), 4});
        });
    }
    Id device_pointer(Id address) { return b_.op(Op::OpConvertUToPtr, global_pointer(u32_), {address}); }
    Id add(Id a, Id b) { return b_.op(Op::OpIAdd, u64_, {a, b}); }
    Id c64(std::uint64_t value) {
        return b_.constant(u64_, {static_cast<std::uint32_t>(value),
                                  static_cast<std::uint32_t>(value >> 32)});
    }
    // The first two dwords of a source as a u64.
    Id read64(const Operand& o) {
        const Id high = b_.op(Op::OpShiftLeftLogical, u64_,
                              {b_.op(Op::OpUConvert, u64_, {read(o, 1)}), b_.constant_u32(32)});
        return b_.op(Op::OpBitwiseOr, u64_,
                     {high, b_.op(Op::OpUConvert, u64_, {read(o, 0)})});
    }

    // s_barrier: every wave of the workgroup waits, and its LDS writes become visible.
    void barrier() {
        b_.op_void(Op::OpControlBarrier,
                   {b_.constant_u32(spirv_word(spv::Scope::Workgroup)),
                    b_.constant_u32(spirv_word(spv::Scope::Workgroup)),
                    b_.constant_u32(spirv_word(spv::MemorySemanticsMask::AcquireRelease) |
                                    spirv_word(spv::MemorySemanticsMask::WorkgroupMemory))});
    }

    // The DS instructions of a wave reach the LDS in program order, so a lane sees what the other
    // lanes wrote with the earlier ones, with no s_barrier. SPIR-V gives no such order between
    // invocations of a subgroup, so every DS instruction waits for the earlier ones of the
    // subgroup first. That equals the wave only in exact mode, and the shader says so
    // (ShaderInterface::wave_size_dependent). The barrier needs the whole subgroup, also after
    // the guarded write of lds_write(), so it declares maximal reconvergence like cross_lane().
    void wave_sync() {
        uses_subgroup_ = true;
        b_.op_void(Op::OpControlBarrier,
                   {b_.constant_u32(spirv_word(spv::Scope::Subgroup)),
                    b_.constant_u32(spirv_word(spv::Scope::Workgroup)),
                    b_.constant_u32(spirv_word(spv::MemorySemanticsMask::AcquireRelease) |
                                    spirv_word(spv::MemorySemanticsMask::WorkgroupMemory))});
    }

    // DS on the LDS (docs/design/graphics.md, "Resources and memory"). The address is in a VGPR
    // plus the offset field. An access outside the LDS reads 0 and is dropped; the clamp that M0
    // sets on the hardware is ignored. Writes and atomics run only in active lanes.
    void ds(const Instruction& inst) {
        const auto d = detail::classify_ds(inst.name);
        if (!d) {
            unsupported();
        }
        if ((inst.flags & flag::gds) != 0) {
            fail("GDS is not supported yet");
        }
        if (d->kind == detail::DsKind::nop) {
            return;
        }
        if (d->kind == detail::DsKind::atomic) {
            unsupported();
        }
        wave_size_dependent_ = true;
        wave_sync();
        const Id address = read(inst.src[0]);
        const unsigned dwords = std::max(1u, d->bytes / 4u);
        for (unsigned n = 0; n < (d->pair ? 2u : 1u); ++n) {
            const unsigned field = d->pair ? (inst.mem_offset >> (8 * n)) & 0xff : inst.mem_offset;
            const unsigned offset = d->pair ? field * d->bytes * (d->stride64 ? 64 : 1) : field;
            const Id at = b_.op(Op::OpIAdd, u32_, {address, b_.constant_u32(offset)});
            for (unsigned k = 0; k < dwords; ++k) {
                if (d->kind == detail::DsKind::read) {
                    Operand dst = inst.dst[0];
                    dst.code += n * dwords + k;
                    write_vector(dst, lds_read(at, *d, k));
                } else {
                    lds_write(at, *d, read(inst.src[1 + n], k), k);
                }
            }
        }
    }

    unsigned lds_dwords() {
        const unsigned dwords = ((key_.pgm_rsrc2 >> rsrc2_lds_size_shift) & 0x1ff) *
                                lds_dwords_per_unit;
        if (dwords == 0 || dwords > lds_max_dwords) {
            fail(std::format("LDS_SIZE of {} dwords is not supported", dwords));
        }
        return dwords;
    }

    // The dword of the LDS at byte `address` plus `dword` dwords, and whether it is in range. The
    // pointer is valid even when it is not.
    struct LdsRef {
        Id pointer, in_range;
    };
    LdsRef lds_ref(Id address, unsigned dword) {
        if (lds_ == 0) {
            const Id array = b_.type_array(u32_, b_.constant_u32(lds_dwords()));
            lds_ = b_.variable(b_.type_pointer(SC::Workgroup, array), SC::Workgroup);
            interface_.push_back(lds_);
        }
        const Id index = b_.op(Op::OpIAdd, u32_,
                               {b_.op(Op::OpShiftRightLogical, u32_, {address, b_.constant_u32(2)}),
                                b_.constant_u32(dword)});
        const Id in_range = b_.op(Op::OpULessThan, bool_, {index, b_.constant_u32(lds_dwords())});
        const Id safe = b_.op(Op::OpSelect, u32_, {in_range, index, b_.constant_u32(0)});
        return {b_.op(Op::OpAccessChain, b_.type_pointer(SC::Workgroup, u32_), {lds_, safe}),
                in_range};
    }

    // Dword `k` of an access at `address`; a sub-dword read extracts and extends its bytes.
    Id lds_read(Id address, const detail::DsOperation& d, unsigned k) {
        const auto [pointer, in_range] = lds_ref(address, k);
        Id value = b_.op(Op::OpSelect, u32_,
                         {in_range, load(u32_, pointer), b_.constant_u32(0)});
        if (d.bytes < 4) {
            const Id shift = byte_shift(address);
            value = b_.op(d.is_signed ? Op::OpBitFieldSExtract : Op::OpBitFieldUExtract, u32_,
                          {value, shift, b_.constant_u32(8 * d.bytes)});
        }
        return value;
    }

    void lds_write(Id address, const detail::DsOperation& d, Id value, unsigned k) {
        const auto [pointer, in_range] = lds_ref(address, k);
        const Id guard = b_.op(Op::OpLogicalAnd, bool_, {active(), in_range});
        when(guard, [&] {
            if (d.bytes >= 4) {
                b_.op_void(Op::OpStore, {pointer, value});
                return;
            }
            // Other lanes write other bytes of this dword, or this byte, at the same time. Merge
            // this lane's bits in one atomic update: each write is whole, and of the lanes that
            // write the same byte one wins, as on the hardware.
            const Id shift = byte_shift(address);
            const Id mask = b_.constant_u32((1u << (8 * d.bytes)) - 1);
            const Id kept = b_.op(Op::OpNot, u32_,
                                  {b_.op(Op::OpShiftLeftLogical, u32_, {mask, shift})});
            const Id bits = b_.op(Op::OpShiftLeftLogical, u32_,
                                  {b_.op(Op::OpBitwiseAnd, u32_, {value, mask}), shift});
            atomic_update(pointer, [&](Id v) {
                return b_.op(Op::OpBitwiseOr, u32_,
                             {b_.op(Op::OpBitwiseAnd, u32_, {v, kept}), bits});
            });
        });
    }

    // The bit offset of the byte at `address` in its dword.
    Id byte_shift(Id address) {
        return b_.op(Op::OpShiftLeftLogical, u32_,
                     {b_.op(Op::OpBitwiseAnd, u32_, {address, b_.constant_u32(3)}),
                      b_.constant_u32(3)});
    }

    // Replaces the dword at `pointer` by f(old) atomically, as a loop on a compare-and-swap, and
    // returns the old value.
    template <typename F>
    Id atomic_update(Id pointer, F f) {
        const Id scope = b_.constant_u32(spirv_word(spv::Scope::Workgroup));
        const Id none = b_.constant_u32(0);
        // The other lanes update the dword atomically at the same time, so the first read is an
        // atomic one too, or it would race with them. A stale value is fixed by the loop.
        const Id current = b_.local_variable(b_.type_pointer(SC::Function, u32_));
        store(current, b_.op(Op::OpAtomicLoad, u32_, {pointer, scope, none}));
        const Id header = b_.id(), body = b_.id(), cont = b_.id(), merge = b_.id();
        b_.op_void(Op::OpBranch, {header});
        b_.begin_block(header);
        b_.op_void(Op::OpLoopMerge, {merge, cont, spirv_word(spv::LoopControlMask::MaskNone)});
        b_.op_void(Op::OpBranch, {body});
        b_.begin_block(body);
        const Id old = load(u32_, current);
        const Id seen = b_.op(Op::OpAtomicCompareExchange, u32_,
                              {pointer, scope, none, none, f(old), old});
        store(current, seen);
        b_.op_void(Op::OpBranchConditional,
                   {b_.op(Op::OpIEqual, bool_, {seen, old}), merge, cont});
        b_.begin_block(cont);
        b_.op_void(Op::OpBranch, {header});
        b_.begin_block(merge);
        return old;
    }

    // Dword `dword` of a source operand, as a u32.
    Id read(const Operand& o, unsigned dword = 0) {
        switch (o.kind) {
        case OperandKind::vector:
            return load(u32_, vgpr(o.code + dword));
        case OperandKind::scalar:
            if (const unsigned c = o.code + dword; is_mask_half(c)) {
                return mask_value(c & ~1u)[c & 1];
            }
            return load(u32_, value_sgpr(o.code + dword));
        case OperandKind::offset:
            return b_.constant_u32(o.value);
        case OperandKind::constant:
        case OperandKind::literal:
            return b_.constant_u32(constant_word(o, dword));
        default:
            fail(std::format("unsupported operand of {}", inst_->name));
        }
    }

    // A constant's dword: integers sign-extend, and a float constant on a 64-bit operand has
    // `value` as its high dword and 0 as its low one (gcn.hpp).
    std::uint32_t constant_word(const Operand& o, unsigned dword) const {
        if (o.dwords == 1) {
            return o.value;
        }
        if (o.kind == OperandKind::literal) {
            fail("64-bit literals are not supported yet");
        }
        if (o.code >= 240) {  // the float constants
            return dword != 0 ? o.value : 0;
        }
        return dword == 0 ? o.value : (static_cast<std::int32_t>(o.value) < 0 ? ~0u : 0u);
    }

    // The dwords of a 32- or 64-bit source; a mask takes one ballot.
    std::array<Id, 2> read_words(const Operand& o) {
        if (is_mask(o)) {
            return mask_value(o.code);
        }
        return {read(o, 0), o.dwords == 2 ? read(o, 1) : Id{0}};
    }

    // A mask pair read as a value, through a ballot. A value written to its dwords comes back
    // exact only when the subgroup has all 64 lanes; the bits of the other subgroups' lanes read
    // as 0. So the pairs that are written and read as values are recorded for mask_round_trip.
    // EXEC is left out: its value is a lane mask by definition.
    std::array<Id, 2> mask_value(unsigned pair) {
        if (pair != code::exec) {
            value_reads_.insert(pair);
        }
        return ballot(load(bool_, mask_var(pair)));
    }

    // Whether a value written to VCC or an SGPR mask pair is read back as a value, from that pair
    // or from one that mask_flow() moves it to, directly or through other pairs (flow-insensitive,
    // like the type pass): typically a compiler reusing a pair for other data.
    bool round_trip() const {
        std::set<unsigned> reached = value_writes_;
        std::vector<unsigned> pending(value_writes_.begin(), value_writes_.end());
        while (!pending.empty()) {
            const unsigned pair = pending.back();
            pending.pop_back();
            const auto [first, last] = mask_copies_.equal_range(pair);
            for (auto it = first; it != last; ++it) {
                if (reached.insert(it->second).second) {
                    pending.push_back(it->second);
                }
            }
        }
        return std::ranges::any_of(reached,
                                   [&](unsigned pair) { return value_reads_.contains(pair); });
    }

    // The wave mask of a lane bit, as its low and high dword: a ballot over the subgroup, moved
    // to the subgroup's position in the wave. The bits of lanes in other subgroups are 0.
    std::array<Id, 2> ballot(Id bit) {
        cross_lane();
        const Id bits = b_.op(Op::OpGroupNonUniformBallot, b_.type_vector(u32_, 4),
                              {b_.constant_u32(spirv_word(spv::Scope::Subgroup)), bit});
        const auto wide = [&](std::uint32_t k) {
            return b_.op(Op::OpUConvert, u64_, {b_.op(Op::OpCompositeExtract, u32_, {bits, k})});
        };
        const Id c32 = b_.constant_u32(32);
        const Id mask = b_.op(Op::OpBitwiseOr, u64_,
                              {wide(0), b_.op(Op::OpShiftLeftLogical, u64_, {wide(1), c32})});
        const Id wave = b_.op(Op::OpShiftLeftLogical, u64_, {mask, subgroup_base()});
        return {b_.op(Op::OpUConvert, u32_, {wave}),
                b_.op(Op::OpUConvert, u32_, {b_.op(Op::OpShiftRightLogical, u64_, {wave, c32})})};
    }

    // `value` in wave lane `lane`, which must be uniform in the subgroup, or 0 when that lane
    // belongs to another subgroup.
    Id broadcast(Id value, Id lane) {
        cross_lane();
        const Id local = b_.op(Op::OpISub, u32_, {lane, subgroup_base()});
        const Id inside = b_.op(Op::OpULessThan, bool_,
                                {local, load(u32_, builtin(spv::BuiltIn::SubgroupSize))});
        const Id c0 = b_.constant_u32(0);
        const Id from = b_.op(Op::OpSelect, u32_, {inside, local, c0});
        const Id r = b_.op(Op::OpGroupNonUniformBroadcast, u32_,
                           {b_.constant_u32(spirv_word(spv::Scope::Subgroup)), value, from});
        return b_.op(Op::OpSelect, u32_, {inside, r, c0});
    }

    // The wave lane of subgroup invocation 0. It assumes that a subgroup covers consecutive
    // values of LocalInvocationIndex, which the execution tests check.
    Id subgroup_base() {
        return b_.op(Op::OpISub, u32_,
                     {lane_, load(u32_, builtin(spv::BuiltIn::SubgroupLocalInvocationId))});
    }

    // A cross-lane operation: its result can differ between the subgroups of a wave.
    void cross_lane() {
        b_.capability(spv::Capability::GroupNonUniform);
        b_.capability(spv::Capability::GroupNonUniformBallot);
        uses_subgroup_ = true;
        wave_size_dependent_ = true;
    }

    // A lane mask source (EXEC, VCC, a mask pair) or a constant: this lane's bit of it.
    Id read_mask(const Operand& o) {
        if (is_mask(o)) {
            return load(bool_, mask_var(o.code));
        }
        if (o.kind != OperandKind::constant) {
            fail(std::format("unsupported mask operand of {}", inst_->name));
        }
        if (uniform_constant(o)) {
            return b_.constant_bool(constant_word(o, 0) != 0);
        }
        const std::uint32_t low = constant_word(o, 0);
        const std::uint32_t high = constant_word(o, 1);
        const Id lower = b_.op(Op::OpULessThan, bool_, {lane_, b_.constant_u32(32)});
        return lane_bit(b_.op(Op::OpSelect, u32_,
                              {lower, b_.constant_u32(low), b_.constant_u32(high)}));
    }

    // Whether a mask constant is 0 or -1, the same bit for every lane.
    bool uniform_constant(const Operand& o) const {
        const std::uint32_t low = constant_word(o, 0);
        return low == constant_word(o, 1) && (low == 0 || low == ~0u);
    }

    // Bit (lane ID modulo 32) of `word`.
    Id lane_bit(Id word) {
        const Id shift = b_.op(Op::OpBitwiseAnd, u32_, {lane_, b_.constant_u32(31)});
        const Id bit = b_.op(Op::OpBitwiseAnd, u32_,
                             {b_.op(Op::OpShiftRightLogical, u32_, {word, shift}),
                              b_.constant_u32(1)});
        return b_.op(Op::OpINotEqual, bool_, {bit, b_.constant_u32(0)});
    }

    // Dword `dword` of a scalar destination. A 32-bit write to half of a mask (exec_lo, vcc_hi,
    // ...) sets the lanes of that half to their bit of the value.
    void write_scalar(const Operand& o, Id value, unsigned dword = 0) {
        const unsigned c = o.code + dword;
        if (o.kind == OperandKind::scalar && is_mask_half(c)) {
            const Op half = (c & 1) != 0 ? Op::OpUGreaterThanEqual : Op::OpULessThan;
            const Id mine = b_.op(half, bool_, {lane_, b_.constant_u32(32)});
            const Id var = mask_var(c & ~1u);
            if ((c & ~1u) != code::exec) {
                value_writes_.insert(c & ~1u);
            }
            store(var, b_.op(Op::OpSelect, bool_, {mine, lane_bit(value), load(bool_, var)}));
            return;
        }
        if (o.kind != OperandKind::scalar) {
            unsupported();
        }
        store(value_sgpr(c), value);
    }

    // s_mov_b64 to a mask.
    void mask_copy(const Operand& dst, const Operand& src) {
        mask_flow(src, dst);
        write_mask(dst, read_mask(src));
    }

    // The bits of the mask source `src` go into the mask `dst` (s_mov_b64, the SALU bitwise
    // family, saveexec through EXEC). For mask_round_trip, a mask source is recorded as a copy,
    // and a constant other than 0 and -1, whose bits are a value rather than a uniform lane bit,
    // as a value written to `dst`.
    void mask_flow(const Operand& src, const Operand& dst) {
        if (is_mask(src)) {
            mask_copies_.emplace(src.code, dst.code);
        } else if (src.kind == OperandKind::constant && !uniform_constant(src) &&
                   dst.code != code::exec) {
            value_writes_.insert(dst.code);
        }
    }

    void write_mask(const Operand& o, Id value) {
        if (!is_mask(o)) {
            fail(std::format("unsupported mask operand of {}", inst_->name));
        }
        store(mask_var(o.code), value);
    }

    void write_vector(const Operand& o, Id value, unsigned dword = 0) {
        const Id var = vgpr(o.code + dword);
        store(var, b_.op(Op::OpSelect, u32_, {active(), value, load(u32_, var)}));
    }

    // Registers: a function variable each, declared on first use. SGPRs and M0 hold a u32. EXEC,
    // VCC and the mask pairs hold this lane's bit, and so does SCC, a bool. The halves of a mask
    // are read and written as values through read() and write_scalar(), not here.
    Id value_sgpr(unsigned c) {
        if (c >= code::flat_scratch && c != code::m0) {  // also SCC and the other condition bits
            fail(std::format("unsupported register operand {} of {}", c, inst_->name));
        }
        return sgpr(c);
    }
    Id sgpr(unsigned c) { return variable(sgprs_, c, u32_, b_.constant_u32(0)); }
    Id vgpr(unsigned n) { return variable(vgprs_, n, u32_, b_.constant_u32(0)); }
    Id mask_var(unsigned c) {
        return variable(bools_, c, bool_, b_.constant_bool(c == code::exec));
    }
    Id mask_exec() { return mask_var(code::exec); }
    Id scc() { return variable(bools_, code::scc, bool_, b_.constant_bool(false)); }

    Id variable(std::map<unsigned, Id>& vars, unsigned n, Id type, Id initial) {
        auto [it, added] = vars.try_emplace(n, 0);
        if (added) {
            it->second = b_.local_variable(b_.type_pointer(SC::Function, type), initial);
        }
        return it->second;
    }

    Id active() { return load(bool_, mask_exec()); }

    Id load(Id type, Id pointer) { return b_.op(Op::OpLoad, type, {pointer}); }
    void store(Id pointer, Id value) { b_.op_void(Op::OpStore, {pointer, value}); }
    Id global_pointer(Id type) { return b_.type_pointer(SC::PhysicalStorageBuffer, type); }
    Id load_global(Id type, Id pointer, std::uint32_t align) {
        return b_.op(Op::OpLoad, type,
                     {pointer, spirv_word(spv::MemoryAccessMask::Aligned), align});
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw GcnError(message, inst_->offset, code_[inst_->offset / 4]);
    }
    [[noreturn]] void unsupported() const {
        fail(std::format("unsupported instruction {}", inst_->name));
    }

    std::span<const std::uint32_t> code_;
    const ShaderKey& key_;
    std::vector<Instruction> program_;
    Cfg cfg_;
    SpirvBuilder b_;
    Id bool_ = 0, u32_ = 0, u64_ = 0;
    Id lane_ = 0, bias_ = 0, lds_ = 0;
    std::vector<Id> interface_;
    std::map<unsigned, Id> sgprs_, vgprs_, bools_;  // bools_: masks and SCC by operand code
    std::map<std::uint32_t, Id> builtins_;
    std::set<unsigned> mask_sgprs_;                 // the first SGPR of each mask pair
    const Instruction* inst_ = nullptr;
    std::set<unsigned> value_writes_, value_reads_;  // mask pairs written or read as values
    std::multimap<unsigned, unsigned> mask_copies_;  // mask_flow() between masks: source, dest
    bool uses_subgroup_ = false;
    bool wave_size_dependent_ = false;
    bool float_used_ = false;
};

}  // namespace

Translation translate(std::span<const std::uint32_t> code, std::span<const std::uint32_t> fetch,
                      const ShaderKey& key) {
    static_cast<void>(fetch);  // the vertex stages' fetch subroutine, not used in compute
    return Translator(code, key).run();
}

}  // namespace bb::gcn
