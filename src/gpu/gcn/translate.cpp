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
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spirv/unified1/GLSL.std.450.h>

#include "gpu/gcn/alu.hpp"
#include "gpu/gcn/buffer_format.hpp"
#include "gpu/gcn/cfg.hpp"
#include "gpu/gcn/descriptors.hpp"
#include "gpu/gcn/fetch.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/image.hpp"
#include "gpu/gcn/lds.hpp"
#include "gpu/gcn/spirv_builder.hpp"
#include "gpu/gcn/stages.hpp"
#include "gpu/gcn/structurize.hpp"

namespace bb::gcn {

namespace {

using Id = SpirvBuilder::Id;
using Op = spv::Op;
using SC = spv::StorageClass;

// COMPUTE_PGM_RSRC2: USER_SGPR in bits 1-5, TGID_X_EN in bit 7. The other fields that change the
// initial registers (SCRATCH_EN in bit 0, TGID_Y_EN to TG_SIZE_EN in bits 8-10, TIDIG_COMP_CNT in
// bits 11-12) are not supported yet.
constexpr unsigned rsrc2_user_sgpr_shift = 1;
constexpr std::uint32_t rsrc2_tgid_x_en = 1u << 7;
constexpr std::uint32_t rsrc2_unsupported = 1u << 0 | 7u << 8 | 3u << 11;

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

// The SALU operations of scalar_arithmetic() that read their sources as plain values.
constexpr std::string_view salu_arithmetic[] = {
    "s_not_b32", "s_add_i32", "s_sub_i32", "s_min_i32", "s_min_u32", "s_max_i32", "s_max_u32",
    "s_mul_i32", "s_cselect_b32", "s_lshl_b32", "s_lshr_b32", "s_ashr_i32", "s_bfe_u32",
    "s_bfe_i32",
};

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

// An integer compare, v_cmp[x]_<cond>_<i32|u32>, s_cmp_<cond>_<i32|u32> or the SOPK form
// s_cmpk_<cond>_<i32|u32> (its immediate is extended by the caller).
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
        (part[1] != "cmp" && !(part[1] == "cmpx" && part[0] == "v") &&
         !(part[1] == "cmpk" && part[0] == "s")) ||
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
    {"v_lshlrev_b32", Op::OpShiftLeftLogical, true},  // the shifts use the low 5 bits
    {"v_lshl_b32", Op::OpShiftLeftLogical},
    {"v_lshrrev_b32", Op::OpShiftRightLogical, true},
    {"v_lshr_b32", Op::OpShiftRightLogical},
    {"v_ashrrev_i32", Op::OpShiftRightArithmetic, true},
    {"v_ashr_i32", Op::OpShiftRightArithmetic},
};

// v_{min,max,min3,max3,med3}_{i32,u32}: the stem and the number of sources.
struct MinMaxName {
    std::string_view stem;
    unsigned sources;
};
constexpr MinMaxName minmax_names[] = {
    {"min", 2}, {"max", 2}, {"min3", 3}, {"max3", 3}, {"med3", 3},
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

// PGM_RSRC1 float mode (any stage): FP_ROUND_32 in bits 12-13 (0 nearest even, 3 toward zero),
// FP_DENORM_32 in bits 16-17 (0 flush source and result, 3 keep), and DX10_CLAMP in bit 21.
constexpr unsigned rsrc1_round_shift = 12;
constexpr unsigned rsrc1_denorm_shift = 16;
constexpr std::uint32_t rsrc1_dx10_clamp = 1u << 21;

class Translator final : public detail::StageRegisters {
public:
    // The vertex stage's fetch subroutine is inlined at its call. Elsewhere s_swappc_b64 stays
    // an unsupported instruction.
    Translator(std::span<const std::uint32_t> code, std::span<const std::uint32_t> fetch,
               const ShaderKey& key)
        : code_(code),
          key_(key),
          program_(decode(code)),
          fetch_(key.stage == Stage::vertex ? detail::inline_fetch(program_, fetch)
                                            : detail::FetchSite{}),
          cfg_(build_cfg(program_)),
          image_sources_(find_image_sources(program_, cfg_, user_sgprs())) {}

    Translation run() {
        check_key();
        collect_masks();
        for (auto cap : {spv::Capability::Shader, spv::Capability::Int64,
                         spv::Capability::PhysicalStorageBufferAddresses}) {
            b_.capability(cap);
        }
        if (graphics()) {
            b_.capability(spv::Capability::GroupNonUniform);  // the lane ID
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
        interface_.insert(interface_.end(), images_.interface().begin(),
                          images_.interface().end());
        if (graphics()) {
            interface_.insert(interface_.end(), stage_.interface().begin(),
                              stage_.interface().end());
            b_.entry_point(stage_.model(), main, "main", interface_);
            stage_.finish(main);
        } else {
            b_.entry_point(spv::ExecutionModel::GLCompute, main, "main", interface_);
            b_.execution_mode(main, spv::ExecutionMode::LocalSize,
                              {key_.num_threads[0], key_.num_threads[1], key_.num_threads[2]});
        }
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
                 .images = image_sources_.images,
                 .samplers = image_sources_.samplers,
                 .stored_images = image_sources_.stored,
                 .wave_size_dependent = wave_size_dependent_,
                 .mask_round_trip = round_trip(),
                 .calls_fetch = fetch_.call != detail::FetchSite::none}};
    }

private:
    bool graphics() const { return key_.stage != Stage::compute; }

    void check_key() const {
        detail::StageIo::check_key(key_);
        if (graphics() && user_sgprs() > 16) {
            throw GcnError(std::format("USER_SGPR {} is more than 16", user_sgprs()), 0, 0);
        }
        if (!graphics() && ((key_.pgm_rsrc2 & rsrc2_unsupported) != 0 || user_sgprs() > 16)) {
            throw GcnError(std::format("COMPUTE_PGM_RSRC2 {:#x} is not supported yet",
                                       key_.pgm_rsrc2),
                           0, 0);
        }
        if (!graphics() && std::ranges::find(key_.num_threads, 0u) != key_.num_threads.end()) {
            throw GcnError("a workgroup size of 0", 0, 0);
        }
        if (key_.push_offset % 8 != 0 || key_.push_offset >= 128) {
            throw GcnError(std::format("push constant offset {}", key_.push_offset), 0, 0);
        }
    }

    // The mask type pass: the SGPR pairs that hold lane masks are the mask operands of the
    // instructions below, and what s_mov_b64 copies to or from a mask. VCC and EXEC are masks. A
    // mask's dwords can still be read as values (a ballot) and written as values (each lane
    // takes its bit). The other SGPRs have one type for the whole program; the initial registers
    // have one per instruction (see StartState). They run in turn until neither changes, because
    // an s_mov_b64 can carry a mask between the two kinds.
    void collect_masks() {
        const auto mark = [this](const Operand& o) {
            if (o.kind != OperandKind::scalar || o.dwords != 2 || o.code >= code::flat_scratch ||
                is_start_sgpr(o.code)) {
                return false;
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
            flow_start_sgprs();
            for (const Instruction& inst : program_) {
                inst_ = &inst;
                if (inst.name == "s_mov_b64" &&
                    (is_mask(inst.dst[0], When::after) || is_mask(inst.src[0]))) {
                    changed |= mark(inst.dst[0]);
                    changed |= mark(inst.src[0]);
                }
            }
        }
        check_start_sgprs();
    }

    // The registers set before the program starts (user data, TGID_X, the primitive mask): a dword
    // holds a value at the start and a lane mask once an instruction writes one to it, which is
    // how the compiler reuses a pair after its last read. Bit k stands for s<k>; both bits of a
    // dword are set where paths that disagree meet, and neither in an unreachable instruction.
    struct StartState {
        std::uint32_t value = 0, mask = 0;
        bool operator==(const StartState&) const = default;
    };

    // Where the initial registers end. A pair that starts in them counts as a whole.
    unsigned start_sgprs_end() const {
        unsigned count = user_sgprs();
        if (key_.stage == Stage::pixel || tgid_x()) {
            ++count;  // the primitive mask or the group ID
        }
        return (count + 1) & ~1u;
    }
    bool is_start_sgpr(unsigned c) const { return c < start_sgprs_end(); }
    static std::uint32_t pair_bits(unsigned pair) { return 3u << (pair & ~1u); }

    // Whether the source `o` holds a mask in state `s`.
    bool source_is_mask(const Operand& o, const StartState& s) const {
        if (o.kind != OperandKind::scalar || o.dwords != 2) {
            return false;
        }
        if (is_start_sgpr(o.code)) {
            return (s.mask & pair_bits(o.code)) != 0;
        }
        return o.code == code::exec || o.code == code::vcc || mask_sgprs_.contains(o.code);
    }

    void transfer_start_sgprs(const Instruction& inst, StartState& s) const {
        for (unsigned k = 0; k < inst.num_dst; ++k) {
            const Operand& dst = inst.dst[k];
            if (dst.kind != OperandKind::scalar || !is_start_sgpr(dst.code)) {
                continue;
            }
            const auto operands = mask_operands(inst);
            const bool mask = inst.name == "s_mov_b64"
                                  ? source_is_mask(inst.src[0], s)
                                  : std::ranges::find(operands, &dst) != operands.end();
            const std::uint32_t bits = ((1u << dst.dwords) - 1) << dst.code;
            s.value = mask ? s.value & ~bits : s.value | bits;
            s.mask = mask ? s.mask | bits : s.mask & ~bits;
        }
    }

    // Forward dataflow over the blocks, keeping the state before and after each instruction.
    void flow_start_sgprs() {
        start_before_.assign(program_.size(), {});
        start_after_.assign(program_.size(), {});
        std::vector<std::optional<StartState>> in(cfg_.blocks.size());
        in[0] = StartState{.value = (1u << start_sgprs_end()) - 1};
        for (bool changed = true; changed;) {
            changed = false;
            for (std::uint32_t b = 0; b < cfg_.blocks.size(); ++b) {
                if (!in[b]) {
                    continue;
                }
                StartState s = *in[b];
                const Block& block = cfg_.blocks[b];
                for (std::uint32_t i = block.first; i < block.first + block.count; ++i) {
                    start_before_[i] = s;
                    transfer_start_sgprs(program_[i], s);
                    start_after_[i] = s;
                }
                for (std::uint32_t next : successors(block)) {
                    StartState merged = in[next].value_or(s);
                    merged.value |= s.value;
                    merged.mask |= s.mask;
                    changed |= !in[next] || merged != *in[next];
                    in[next] = merged;
                }
            }
        }
    }

    // What the program can't get from one register: a dword read where a value and a mask meet,
    // and a mask operand that holds a value (it would have to keep the value's bits per lane).
    void check_start_sgprs() {
        for (const Instruction& inst : program_) {
            inst_ = &inst;
            const StartState& s = start_before_[index()];
            for (unsigned k = 0; k < inst.num_src; ++k) {
                const Operand& o = inst.src[k];
                if (o.kind != OperandKind::scalar || !is_start_sgpr(o.code)) {
                    continue;
                }
                if ((s.value & s.mask & (((1u << o.dwords) - 1) << o.code)) != 0) {
                    fail(std::format("initial s{} is a value or a mask, depending on the path",
                                     o.code));
                }
            }
            std::vector<const Operand*> reads = mask_operands(inst);
            if (inst.name == "s_mov_b64" && is_mask(inst.dst[0], When::after)) {
                reads.push_back(&inst.src[0]);
            }
            for (const Operand* o : reads) {
                const bool source = o >= inst.src.data() && o < inst.src.data() + inst.src.size();
                if (source && o->kind == OperandKind::scalar && is_start_sgpr(o->code) &&
                    !source_is_mask(*o, s)) {
                    fail(std::format("user data s{} is used as a mask", o->code & ~1u));
                }
            }
        }
    }

    std::uint32_t index() const { return static_cast<std::uint32_t>(inst_ - program_.data()); }

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
        if (bitwise_op(name, "_saveexec_b64") || name == "s_not_b64" || name == "s_wqm_b64") {
            return {&inst.dst[0], &inst.src[0]};
        }
        return {};
    }

    // An instruction reads its sources in the state before it and writes its destinations in the
    // state after it.
    enum class When { before, after };

    bool is_mask(const Operand& o, When when = When::before) const {
        if (o.kind != OperandKind::scalar || o.dwords != 2) {
            return false;
        }
        if (is_start_sgpr(o.code)) {
            const std::uint32_t bits = pair_bits(o.code);
            return (start_state(when).mask & bits) == bits;
        }
        return o.code == code::exec || o.code == code::vcc || mask_sgprs_.contains(o.code);
    }

    // A dword of EXEC, VCC or a mask pair.
    bool is_mask_half(unsigned c, When when = When::before) const {
        if (is_start_sgpr(c)) {
            return (start_state(when).mask >> c & 1) != 0;
        }
        const unsigned pair = c & ~1u;
        return pair == code::exec || pair == code::vcc || mask_sgprs_.contains(pair);
    }

    const StartState& start_state(When when) const {
        return (when == When::before ? start_before_ : start_after_)[index()];
    }

    unsigned user_sgprs() const { return (key_.pgm_rsrc2 >> rsrc2_user_sgpr_shift) & 31; }
    bool tgid_x() const { return !graphics() && (key_.pgm_rsrc2 & rsrc2_tgid_x_en) != 0; }

    // The draw block and the initial registers: user data in the first SGPRs, then the stage's.
    // In compute, the thread ID in X is in v0, the lane ID is LocalInvocationIndex modulo 64, and
    // TGID_X_EN puts the workgroup's X in the SGPR after the user data (the order of shadPS4's
    // Translator::EmitPrologue, whose only initial SGPR the game uses is this one).
    // In the graphics stages the lane ID is the subgroup's (docs/design/graphics.md, "Execution
    // model"), so subgroup_base is 0.
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
        b_.member_decorate(push_t, 0, spv::Decoration::Offset, {key_.push_offset});
        const Id push = b_.variable(b_.type_pointer(SC::PushConstant, push_t), SC::PushConstant);
        interface_.push_back(push);

        const Id c0 = b_.constant_u32(0);
        const Id address = load(u64_, b_.op(Op::OpAccessChain,
                                            b_.type_pointer(SC::PushConstant, u64_), {push, c0}));
        draw_ = b_.op(Op::OpConvertUToPtr, global_pointer(draw_t), {address});
        for (unsigned k = 0; k < user_sgprs(); ++k) {
            const Id p = b_.op(Op::OpAccessChain, global_pointer(u32_),
                               {draw_, c0, b_.constant_u32(k)});
            store(sgpr(k), load_global(u32_, p, 4));
        }
        bias_table_ = load_global(
            u64_, b_.op(Op::OpAccessChain, global_pointer(u64_), {draw_, b_.constant_u32(1)}), 8);

        if (graphics()) {
            lane_ = load(u32_, builtin(spv::BuiltIn::SubgroupLocalInvocationId));
            stage_.prologue(user_sgprs());
            return;
        }
        const Id index = load(u32_, builtin(spv::BuiltIn::LocalInvocationIndex, u32_));
        lane_ = b_.op(Op::OpBitwiseAnd, u32_, {index, b_.constant_u32(63)});
        const Id uvec3 = b_.type_vector(u32_, 3);
        const Id local = load(uvec3, builtin(spv::BuiltIn::LocalInvocationId, uvec3));
        store(vgpr(0), b_.op(Op::OpCompositeExtract, u32_, {local, 0}));
        if (tgid_x()) {
            const Id group = load(uvec3, builtin(spv::BuiltIn::WorkgroupId, uvec3));
            store(sgpr(user_sgprs()), b_.op(Op::OpCompositeExtract, u32_, {group, 0}));
        }
    }

    // The input variable of a built-in, declared on first use. `type` defaults to u32.
    Id builtin(spv::BuiltIn which, Id type = 0) {
        auto [it, added] = builtins_.try_emplace(spirv_word(which), 0);
        if (added) {
            const Id pointer = b_.type_pointer(SC::Input, type != 0 ? type : u32_);
            it->second = b_.variable(pointer, SC::Input);
            b_.decorate(it->second, spv::Decoration::BuiltIn, {spirv_word(which)});
            if (key_.stage == Stage::pixel) {  // integer fragment inputs are flat
                b_.decorate(it->second, spv::Decoration::Flat);
            }
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
            case Format::sopk:
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
            case Format::mtbuf:
                typed_memory(inst);
                break;
            case Format::ds:
                ds(inst);
                break;
            case Format::mimg:
                images_.translate(inst, i);
                break;
            case Format::exp:
                stage_.exp(inst);
                break;
            case Format::vintrp:
                stage_.interpolate(inst);
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
        if (static_cast<std::uint32_t>(&inst - program_.data()) == fetch_.call) {
            return_address(inst);
        } else if (name == "s_mov_b64" && is_mask(dst, When::after)) {
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
        } else if (!scalar_arithmetic(inst)) {
            unsupported();
        }
    }

    // The SALU integer arithmetic, shifts, selects, the bit field extract, the SOPK forms with a
    // 16-bit immediate, and the mask operations s_not_b64 and s_wqm_b64 (SCC as the manual says
    // for each). Returns false for any other instruction.
    bool scalar_arithmetic(const Instruction& inst) {
        const std::string_view name = inst.name;
        const Operand& dst = inst.dst[0];
        const auto u = [&](Op op, Id x, Id y) { return b_.op(op, u32_, {x, y}); };
        const auto nonzero = [&](Id r) {
            store(scc(), b_.op(Op::OpINotEqual, bool_, {r, b_.constant_u32(0)}));
        };
        if (inst.format == Format::sopk) {
            scalar_immediate(inst);
            return true;
        }
        if (name == "s_not_b64" || name == "s_wqm_b64") {
            mask_flow(inst.src[0], dst);
            const Id bit = read_mask(inst.src[0]);
            const Id r = name == "s_not_b64" ? b_.op(Op::OpLogicalNot, bool_, {bit})
                                             : alu_.quad_any(bit);
            uses_subgroup_ = true;  // the quad swap needs the whole quad
            write_mask(dst, r);
            store(scc(), any(r));
            return true;
        }
        if (std::ranges::find(salu_arithmetic, name) == std::end(salu_arithmetic)) {
            return false;
        }
        const Id a = read(inst.src[0]);
        const Id b = inst.num_src > 1 ? read(inst.src[1]) : 0;
        const Id c31 = b_.constant_u32(31);
        Id r = 0;
        if (name == "s_not_b32") {
            r = b_.op(Op::OpNot, u32_, {a});
            nonzero(r);
        } else if (name == "s_add_i32" || name == "s_sub_i32") {  // SCC: signed overflow
            const bool add = name == "s_add_i32";
            r = u(add ? Op::OpIAdd : Op::OpISub, a, b);
            // add: a and b have one sign and r the other; sub: a and b differ and r follows b.
            const Id sign = u(Op::OpBitwiseAnd, u(Op::OpBitwiseXor, a, r),
                              u(Op::OpBitwiseXor, add ? b : a, add ? r : b));
            store(scc(), b_.op(Op::OpSLessThan, bool_, {sign, b_.constant_u32(0)}));
        } else if (name.starts_with("s_min_") || name.starts_with("s_max_")) {
            const bool min = name.starts_with("s_min_"), sign = name.ends_with("_i32");
            const Id first = b_.op(min ? (sign ? Op::OpSLessThan : Op::OpULessThan)
                                       : (sign ? Op::OpSGreaterThan : Op::OpUGreaterThan),
                                   bool_, {a, b});  // SCC: src0 is taken
            r = b_.op(Op::OpSelect, u32_, {first, a, b});
            store(scc(), first);
        } else if (name == "s_mul_i32") {
            r = u(Op::OpIMul, a, b);
        } else if (name == "s_cselect_b32") {
            r = b_.op(Op::OpSelect, u32_, {load(bool_, scc()), a, b});
        } else if (name == "s_lshl_b32" || name == "s_lshr_b32" || name == "s_ashr_i32") {
            r = u(name == "s_lshl_b32"   ? Op::OpShiftLeftLogical
                  : name == "s_lshr_b32" ? Op::OpShiftRightLogical
                                         : Op::OpShiftRightArithmetic,
                  a, u(Op::OpBitwiseAnd, b, c31));
            nonzero(r);
        } else if (name == "s_bfe_u32" || name == "s_bfe_i32") {  // the width is S1[22:16]
            const Id width = u(Op::OpBitwiseAnd, u(Op::OpShiftRightLogical, b, b_.constant_u32(16)),
                               b_.constant_u32(0x7f));
            const Id too_wide = b_.op(Op::OpUGreaterThan, bool_, {width, b_.constant_u32(32)});
            const Id clamped = b_.op(Op::OpSelect, u32_, {too_wide, b_.constant_u32(32), width});
            r = alu_.bit_field(a, u(Op::OpBitwiseAnd, b, c31), clamped, name == "s_bfe_i32");
            nonzero(r);
        }
        write_scalar(dst, r);
        return true;
    }

    // SOPK: the destination is also the first source, the 16-bit immediate sign-extends except in
    // the unsigned compares.
    void scalar_immediate(const Instruction& inst) {
        const std::string_view name = inst.name;
        const auto compare = parse_compare(name);
        const Operand& imm = inst.src[compare ? 1 : 0];
        const Id simm = b_.constant_u32(compare && !compare->is_signed
                                            ? imm.value & 0xffff
                                            : static_cast<std::uint32_t>(
                                                  static_cast<std::int16_t>(imm.value)));
        if (compare) {
            store(scc(), int_compare(*compare, read(inst.src[0]), simm));
        } else if (name == "s_movk_i32") {
            write_scalar(inst.dst[0], simm);
        } else if (name == "s_cmovk_i32") {
            const Id old = read(inst.dst[0]);
            write_scalar(inst.dst[0], b_.op(Op::OpSelect, u32_, {load(bool_, scc()), simm, old}));
        } else if (name == "s_addk_i32") {  // SCC: signed overflow
            const Id d = read(inst.dst[0]);
            const Id r = b_.op(Op::OpIAdd, u32_, {d, simm});
            const Id same = b_.op(Op::OpBitwiseAnd, u32_,
                                  {b_.op(Op::OpBitwiseXor, u32_, {d, r}),
                                   b_.op(Op::OpBitwiseXor, u32_, {simm, r})});
            write_scalar(inst.dst[0], r);
            store(scc(), b_.op(Op::OpSLessThan, bool_, {same, b_.constant_u32(0)}));
        } else if (name == "s_mulk_i32") {
            write_scalar(inst.dst[0], b_.op(Op::OpIMul, u32_, {read(inst.dst[0]), simm}));
        } else {
            unsupported();
        }
    }

    // The s_swappc_b64 whose fetch subroutine follows inline: its destination gets the address
    // of the next instruction, the guest's return address, from the draw block's code_address.
    void return_address(const Instruction& inst) {
        const Id code = load_global(
            u64_, b_.op(Op::OpAccessChain, global_pointer(u64_), {draw_, b_.constant_u32(2)}), 8);
        const Id address = add(code, c64(inst.offset + 4 * inst.size));
        write_scalar(inst.dst[0], b_.op(Op::OpUConvert, u32_, {address}), 0);
        write_scalar(inst.dst[0],
                     b_.op(Op::OpUConvert, u32_,
                           {b_.op(Op::OpShiftRightLogical, u64_, {address, b_.constant_u32(32)})}),
                     1);
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
            if (op->op != Op::OpBitwiseAnd && op->op != Op::OpBitwiseOr &&
                op->op != Op::OpBitwiseXor) {  // a shift
                y = b_.op(Op::OpBitwiseAnd, u32_, {y, b_.constant_u32(31)});
            }
            write_vector(inst.dst[0], b_.op(op->op, u32_, {x, y}));
        } else if (!vector_integer(inst, name)) {
            unsupported();
        }
    }

    // The multiplies of alu.hpp, the bit field extracts, the 64-bit shifts and the integer min,
    // max and median. Returns false for any other instruction.
    bool vector_integer(const Instruction& inst, std::string_view name) {
        const auto u = [&](Op op, Id x, Id y) { return b_.op(op, u32_, {x, y}); };
        const auto src = [&](unsigned i) { return read(inst.src[i]); };
        if (const auto mul = detail::classify_int_mul(name)) {
            write_vector(inst.dst[0], alu_.int_mul(*mul, src(0), src(1), mul->add ? src(2) : 0));
        } else if (name == "v_bfe_u32" || name == "v_bfe_i32") {  // offset and width: 5 bits
            const Id c31 = b_.constant_u32(31);
            write_vector(inst.dst[0], alu_.bit_field(src(0), u(Op::OpBitwiseAnd, src(1), c31),
                                                     u(Op::OpBitwiseAnd, src(2), c31),
                                                     name == "v_bfe_i32"));
        } else if (name == "v_lshl_b64" || name == "v_lshr_b64" || name == "v_ashr_i64") {
            const Id count = u(Op::OpBitwiseAnd, src(1), b_.constant_u32(63));
            const Id r = b_.op(name == "v_lshl_b64"   ? Op::OpShiftLeftLogical
                               : name == "v_lshr_b64" ? Op::OpShiftRightLogical
                                                      : Op::OpShiftRightArithmetic,
                               u64_, {read64(inst.src[0]), count});
            write_vector(inst.dst[0], b_.op(Op::OpUConvert, u32_, {r}), 0);
            write_vector(inst.dst[0],
                         b_.op(Op::OpUConvert, u32_,
                               {b_.op(Op::OpShiftRightLogical, u64_, {r, b_.constant_u32(32)})}),
                         1);
        } else if (const auto mm = integer_minmax(name)) {
            const bool sign = name.ends_with("_i32");
            const GLSLstd450 low = sign ? GLSLstd450SMin : GLSLstd450UMin;
            const GLSLstd450 high = sign ? GLSLstd450SMax : GLSLstd450UMax;
            const auto pick = [&](GLSLstd450 f, Id x, Id y) {
                return b_.op(Op::OpExtInst, u32_, {b_.import_extended("GLSL.std.450"), f, x, y});
            };
            const bool min = mm->stem.starts_with("min");
            Id r;
            if (mm->stem == "med3") {  // max(min(a, b), min(max(a, b), c))
                r = pick(high, pick(low, src(0), src(1)),
                         pick(low, pick(high, src(0), src(1)), src(2)));
            } else {
                r = pick(min ? low : high, src(0), src(1));
                if (mm->sources == 3) {
                    r = pick(min ? low : high, r, src(2));
                }
            }
            write_vector(inst.dst[0], r);
        } else {
            return false;
        }
        return true;
    }

    // The entry of v_{min,max,min3,max3,med3}_{i32,u32}, or null for another name.
    static const MinMaxName* integer_minmax(std::string_view name) {
        if (!name.starts_with("v_") || (!name.ends_with("_i32") && !name.ends_with("_u32"))) {
            return nullptr;
        }
        const auto it = std::ranges::find(minmax_names, name.substr(2, name.size() - 6),
                                          &MinMaxName::stem);
        return it == std::end(minmax_names) ? nullptr : &*it;
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
        } else if (const auto math = detail::classify_float_math(name)) {
            r = alu_.float_math(*math, src(0));
        } else if (name == "v_cvt_f32_f16") {  // abs and neg of the half are those of its f32
            const Operand& s = inst.src[0];
            if (s.kind == OperandKind::constant && s.code >= 240 && s.code <= 247) {
                // CI has no f16 inline constants; which half the hardware reads is unknown.
                fail(std::format("a float constant as the half source of {}", inst.name));
            }
            r = source_modifiers(inst, 0, alu_.half_to_float(read(s)));
        } else if (name == "v_cvt_pkrtz_f16_f32") {  // toward zero, whatever the float mode
            bits = alu_.pack_half_rtz(src(0), src(1));
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
        return source_modifiers(inst, i, b_.op(Op::OpBitcast, b_.type_float(32),
                                               {read(inst.src[i])}));
    }

    // The f32 `x` with the abs and neg modifiers of source `i`, abs first.
    Id source_modifiers(const Instruction& inst, unsigned i, Id x) {
        const Id f32 = b_.type_float(32);
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
    // or toward zero and flushes denormals or keeps them, as the stage's RSRC1 says. The driver
    // must support them (the float controls properties); the run time checks that.
    void float_modes(Id main) {
        if (!float_used_) {
            return;
        }
        const unsigned round = key_.pgm_rsrc1 >> rsrc1_round_shift & 3;
        const unsigned denorm = key_.pgm_rsrc1 >> rsrc1_denorm_shift & 3;
        if ((round != 0 && round != 3) || (denorm != 0 && denorm != 3)) {
            throw GcnError(std::format("PGM_RSRC1 {:#x}: this float mode is not supported",
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
        guarded_store(active(), device_address(read64(inst.src[0])),
                      read(inst.src[1]));
    }

    // SMRD, in every lane: s_load_dword* reads at a 64-bit address from an SGPR pair, with no
    // range check. s_buffer_load_dword* reads through a V# in four SGPRs, and each dword that
    // is not inside the buffer (`scalar_in_range`) reads as 0. The offset is in dwords when it is
    // the instruction's immediate or the literal that follows it, and in bytes when it is an SGPR
    // (the low 2 bits are ignored).
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
            const Id address = device_address(add(guest, c64(4 * d)));
            write_scalar(inst.dst[0],
                         buffer ? guarded_load(scalar_in_range(v, offset, d), address)
                                : load_global(u32_, device_pointer(address), 4),
                         d);
        }
    }

    // MUBUF loads and stores of dwords, bytes and shorts, untyped, in active lanes (docs/design/
    // graphics.md, "Resources and memory"). The address is base + soffset + buffer_offset
    // (8.1.5), see buffer_access. Each dword is checked on its own by `inside`; a byte or
    // short is checked at its first byte and read or written whole, from any address. `addr64`
    // adds a 64-bit VGPR address (and soffset) and is not range-checked (Table 8.2). Out-of-range
    // loads read 0 and out-of-range stores are dropped.
    void buffer_memory(const Instruction& inst) {
        const std::string_view name = inst.name;
        if (name.starts_with("buffer_load_format_") || name.starts_with("buffer_store_format_")) {
            typed_memory(inst);
            return;
        }
        if (name.starts_with("buffer_atomic_")) {
            buffer_atomic(inst);
            return;
        }
        const bool load = name.starts_with("buffer_load_");
        const unsigned narrow = name.ends_with("byte") ? 1 : name.ends_with("short") ? 2 : 0;
        if (narrow == 0 && !name.starts_with("buffer_load_dword") &&
            !name.starts_with("buffer_store_dword")) {
            unsupported();
        }
        if ((inst.flags & (flag::tfe | flag::lds)) != 0) {
            fail(std::format("{} with tfe or lds is not supported yet", name));
        }
        const Operand& data = load ? inst.dst[0] : inst.src[0];
        const BufferAccess m = buffer_access(inst, &inst.src[load ? 0 : 1]);
        if (narrow != 0) {
            const Id cond = b_.op(Op::OpLogicalAnd, bool_, {active(), inside(m, 0)});
            const Id at = device_address(address(m, 0));
            if (!load) {
                when(cond, [&] { store_bytes(at, read(data), narrow); });
                return;
            }
            Id value = b_.constant_u32(0);
            for (unsigned j = 0; j < narrow; ++j) {
                value = b_.op(Op::OpBitwiseOr, u32_,
                              {value, b_.op(Op::OpShiftLeftLogical, u32_,
                                            {guarded_byte(cond, add(at, c64(j))),
                                             b_.constant_u32(8 * j)})});
            }
            if (name[12] == 's') {  // buffer_load_sbyte, buffer_load_sshort
                value = b_.op(Op::OpBitFieldSExtract, u32_,
                              {value, b_.constant_u32(0), b_.constant_u32(8 * narrow)});
            }
            write_vector(data, value);
            return;
        }
        for (unsigned d = 0; d < data.dwords; ++d) {
            const Id address = device_address(this->address(m, 4 * d));
            const Id cond = b_.op(Op::OpLogicalAnd, bool_, {active(), inside(m, d)});
            if (load) {
                write_vector(data, guarded_load(cond, address), d);
            } else {
                guarded_store(cond, address, read(data, d));
            }
        }
    }

    // MUBUF atomics on a dword, or under _x2 a qword (Sea Islands ISA Table 12.8), in active
    // lanes. The address is buffer_memory's. The range check is all or nothing (8.1.5.3), taken
    // at the first byte as for a format access: an access outside is dropped and returns 0. With
    // glc the old value returns into the first dword or two of vdata (Table 8.2). The compare
    // and swaps take src from the first data VGPRs and cmp from the next.
    void buffer_atomic(const Instruction& inst) {
        using A = detail::DsAtomic;
        static constexpr std::pair<std::string_view, A> ops[] = {
            {"swap", A::exchange}, {"cmpswap", A::cmpst},   {"add", A::add},     {"sub", A::sub},
            {"smin", A::smin},     {"umin", A::umin},       {"smax", A::smax},   {"umax", A::umax},
            {"and", A::bit_and},   {"or", A::bit_or},       {"xor", A::bit_xor}, {"inc", A::inc},
            {"dec", A::dec},       {"fcmpswap", A::fcmpst}, {"fmin", A::fmin},   {"fmax", A::fmax}};
        std::string_view stem = inst.name;
        stem.remove_prefix(std::string_view("buffer_atomic_").size());
        const bool wide = stem.ends_with("_x2");
        stem.remove_suffix(wide ? 3 : 0);
        const auto* found = std::ranges::find(ops, stem, &std::pair<std::string_view, A>::first);
        if (found == std::end(ops)) {
            unsupported();
        }
        if ((inst.flags & flag::tfe) != 0) {
            fail(std::format("{} with tfe is not supported yet", inst.name));
        }
        const A op = found->second;
        const Id type = wide ? u64_ : u32_;
        if (wide) {
            b_.capability(spv::Capability::Int64Atomics);
        }
        const Operand& data = inst.src[0];
        const auto value = [&](unsigned k) {  // the k-th value of vdata
            const auto code = static_cast<std::uint16_t>(data.code + 2 * k);
            return wide ? read64({OperandKind::vector, 2, code, 0}) : read(data, k);
        };
        const bool compare = op == A::cmpst || op == A::fcmpst;
        const Id src = value(0), cmp = compare ? value(1) : 0;
        const BufferAccess m = buffer_access(inst, &inst.src[1]);
        const Id pointer = b_.op(Op::OpConvertUToPtr, global_pointer(type),
                                 {device_address(address(m, 0))});
        const Id old = atomic(op, type, pointer, spv::Scope::Device,
                              b_.op(Op::OpLogicalAnd, bool_, {active(), inside(m, 0)}),
                              compare ? cmp : src, compare ? src : 0);
        for (unsigned d = 0; (inst.flags & flag::glc) != 0 && d < (wide ? 2u : 1u); ++d) {
            const Id half = d == 0 ? old : b_.op(Op::OpShiftRightLogical, u64_, {old, c64(32)});
            write_vector(data, wide ? b_.op(Op::OpUConvert, u32_, {half}) : old, d);
        }
    }

    // The fields of a V#: its base address, its stride and num_records (all u64). The base is 40
    // bits: the game's V#s have 0x80 or 0xb0 in bits 40-47, which are no part of the address.
    struct Vsharp {
        Id base = 0, stride = 0, records = 0;
    };
    Vsharp vsharp(const Operand& o) {
        const Id dw1 = read(o, 1);
        const Id high = b_.op(Op::OpBitwiseAnd, u32_, {dw1, b_.constant_u32(0xff)});
        Vsharp v;
        v.base = b_.op(Op::OpBitwiseOr, u64_,
                       {b_.op(Op::OpShiftLeftLogical, u64_,
                              {b_.op(Op::OpUConvert, u64_, {high}), b_.constant_u32(32)}),
                        b_.op(Op::OpUConvert, u64_, {read(o, 0)})});
        const Id stride = b_.op(Op::OpBitFieldUExtract, u32_,
                                {dw1, b_.constant_u32(16), b_.constant_u32(14)});
        v.stride = b_.op(Op::OpUConvert, u64_, {stride});
        v.records = b_.op(Op::OpUConvert, u64_, {read(o, 2)});
        return v;
    }

    // An MUBUF or MTBUF access: what its address and range check need (Sea Islands ISA 8.1.5 and
    // Table 8.5). The modes of the V# are run-time values: `swizzle_enable` (dword 1 bit 31),
    // `element_size` and `index_stride` (dword 3 bits 19-20 and 21-22, as shifts), `add_tid_enable`
    // (dword 3 bit 23) and `heap` (dword 3 bit 26).
    struct BufferAccess {
        Vsharp v;
        Id base = 0;     // u64: base + soffset (+ the VGPR address of addr64), without the bias
        Id soffset = 0;  // u64
        Id offset = 0;   // u64: the instruction's offset plus the VGPR's under `offen`
        Id index = 0;    // u64: the VGPR index under `idxen`, plus the lane under add_tid_enable
        Id swizzle = 0, add_tid = 0, heap = 0;  // bool
        Id element_shift = 0, index_shift = 0;  // u64: log2 of element_size and index_stride
        bool addr64 = false, idxen = false;
    };

    // The address and range check of an instruction whose vaddr, V# and soffset operands are
    // a[0] to a[2]. base = V# base + soffset; index = the VGPR under `idxen` (else 0) plus the lane
    // (thread_id[5:0]) under `add_tid_enable`; offset = the instruction's offset plus the VGPR
    // under `offen` (8.1.5). `addr64` ignores the stride and the modes (Table 8.2: illegal with
    // `add_tid_enable`).
    BufferAccess buffer_access(const Instruction& inst, const Operand* a) {
        BufferAccess m;
        m.addr64 = (inst.flags & flag::addr64) != 0;
        m.idxen = (inst.flags & flag::idxen) != 0;
        if (m.addr64 && (m.idxen || (inst.flags & flag::offen) != 0)) {
            fail(std::format("{} with addr64 and idxen or offen is not allowed", inst.name));
        }
        m.v = vsharp(a[1]);
        const auto bit = [&](unsigned dword, unsigned n) {
            const Id masked =
                b_.op(Op::OpBitwiseAnd, u32_, {read(a[1], dword), b_.constant_u32(1u << n)});
            return b_.op(Op::OpINotEqual, bool_, {masked, b_.constant_u32(0)});
        };
        const auto field = [&](unsigned low, unsigned plus) {  // a 2-bit field of dword 3, + plus
            return b_.op(Op::OpUConvert, u64_,
                         {b_.op(Op::OpIAdd, u32_,
                                {b_.op(Op::OpBitFieldUExtract, u32_,
                                       {read(a[1], 3), b_.constant_u32(low), b_.constant_u32(2)}),
                                 b_.constant_u32(plus)})});
        };
        m.swizzle = bit(1, 31);
        m.add_tid = bit(3, 23);
        m.heap = bit(3, 26);
        m.element_shift = field(19, 1);  // 2, 4, 8, 16 bytes
        m.index_shift = field(21, 3);    // 8, 16, 32, 64 indices
        m.soffset = b_.op(Op::OpUConvert, u64_, {read(a[2])});
        m.base = add(m.v.base, m.soffset);
        m.offset = c64(inst.mem_offset);
        m.index = c64(0);
        unsigned next = 0;
        if (m.addr64) {
            m.base = add(m.base, read64(a[0]));
            return m;
        }
        if (m.idxen) {
            m.index = b_.op(Op::OpUConvert, u64_, {read(a[0], next++)});
        }
        if ((inst.flags & flag::offen) != 0) {
            m.offset = add(m.offset, b_.op(Op::OpUConvert, u64_, {read(a[0], next++)}));
        }
        m.index = add(m.index, b_.op(Op::OpSelect, u64_,
                                     {m.add_tid, b_.op(Op::OpUConvert, u64_, {lane_}), c64(0)}));
        return m;
    }

    // The buffer_offset of the byte at offset + delta (8.1.5.1 and 8.1.5.2): index * stride +
    // offset, or swizzled under `swizzle_enable`: ((index / index_stride) * stride + (offset /
    // element_size) * element_size) * index_stride + (index % index_stride) * element_size +
    // offset % element_size. Each dword of a wider access has its own: with an element_size of
    // 4, the dwords of a pair are index_stride elements apart.
    Id buffer_offset(const BufferAccess& m, std::uint64_t delta) {
        const Id at = add(m.offset, c64(delta));
        const Id linear = add(b_.op(Op::OpIMul, u64_, {m.index, m.v.stride}), at);
        const auto shr = [&](Id x, Id n) { return b_.op(Op::OpShiftRightLogical, u64_, {x, n}); };
        const auto shl = [&](Id x, Id n) { return b_.op(Op::OpShiftLeftLogical, u64_, {x, n}); };
        const auto low = [&](Id x, Id n) {  // x % 2^n
            return b_.op(Op::OpBitwiseAnd, u64_,
                         {x, b_.op(Op::OpISub, u64_, {shl(c64(1), n), c64(1)})});
        };
        const Id high = add(b_.op(Op::OpIMul, u64_, {shr(m.index, m.index_shift), m.v.stride}),
                            shl(shr(at, m.element_shift), m.element_shift));
        const Id swizzled = add(add(shl(high, m.index_shift),
                                    shl(low(m.index, m.index_shift), m.element_shift)),
                                low(at, m.element_shift));
        return b_.op(Op::OpSelect, u64_, {m.swizzle, swizzled, linear});
    }

    // The guest address of the byte at offset + delta, without the bias.
    Id address(const BufferAccess& m, std::uint64_t delta) {
        return add(m.base, m.addr64 ? add(m.offset, c64(delta)) : buffer_offset(m, delta));
    }

    // Whether dword `d` of the access is inside the buffer (Sea Islands ISA 8.1.5.3, p. 8-8);
    // `addr64` is not range-checked. The offset of dword `d` includes 4 * d: "x2" and "x4" are
    // checked per component. A stride of 0 makes `num_records` a byte size, and the access is out
    // of range when buffer_offset + soffset >= num_records. The manual writes the left side as
    // `buffer_offset >= num_records - soffset`; the sum in 64 bits is the same without the wrap of
    // the subtraction, so a soffset beyond the size is always out of range. Otherwise the access
    // is out of range when index >= num_records, or, with `idxen` or `add_tid_enable`, when the
    // offset is not below the stride. Without either the offset is not checked at all, even with
    // a stride, and soffset is never part of the check with a stride. `heap` adds (Table 8.5): out
    // of range when the offset is 0 or not below num_records (our reading: the per-dword offset,
    // unverified).
    Id inside(const BufferAccess& m, unsigned d) {
        if (m.addr64) {
            return b_.constant_bool(true);
        }
        const auto less = [&](Id x, Id y) { return b_.op(Op::OpULessThan, bool_, {x, y}); };
        const auto both = [&](Id x, Id y) { return b_.op(Op::OpLogicalAnd, bool_, {x, y}); };
        const Id at = add(m.offset, c64(4 * d));
        const Id raw = less(add(buffer_offset(m, 4 * d), m.soffset), m.v.records);
        const Id check_offset = m.idxen ? b_.constant_bool(true) : m.add_tid;
        const Id structured = both(less(m.index, m.v.records),
                                   b_.op(Op::OpLogicalOr, bool_,
                                         {b_.op(Op::OpLogicalNot, bool_, {check_offset}),
                                          less(at, m.v.stride)}));
        const Id in = b_.op(Op::OpSelect, bool_,
                            {b_.op(Op::OpINotEqual, bool_, {m.v.stride, c64(0)}), structured, raw});
        const Id heap_ok = both(b_.op(Op::OpINotEqual, bool_, {at, c64(0)}), less(at, m.v.records));
        return both(in, b_.op(Op::OpLogicalOr, bool_,
                              {b_.op(Op::OpLogicalNot, bool_, {m.heap}), heap_ok}));
    }

    // The clamp of s_buffer_load_dword* (7.2.2, p. 7-2): dword `d` of an access at byte `offset`
    // reads 0 when offset + 4 * d >= size, with size = num_records bytes for a stride of 0 and
    // stride * num_records otherwise (the size in bytes of Table 8.5's footnote). The manual's
    // clamp for a stride is `offset > size`, which would let the dword at `size` read from
    // outside the buffer; we use `>=` for both cases, like the MUBUF check. Checked per dword.
    Id scalar_in_range(const Vsharp& v, Id offset, unsigned d) {
        const Id unit = b_.op(Op::OpSelect, u64_,
                              {b_.op(Op::OpIEqual, bool_, {v.stride, c64(0)}), c64(1), v.stride});
        return b_.op(Op::OpULessThan, bool_,
                     {add(offset, c64(4 * d)), b_.op(Op::OpIMul, u64_, {v.records, unit})});
    }

    void guarded(Id cond, const std::function<void()>& body) override { when(cond, body); }
    void write_vgpr(unsigned n, Id value) override {
        write_vector({OperandKind::vector, 1, static_cast<std::uint16_t>(n), 0}, value);
    }
    Id exec_var() override { return mask_exec(); }

    // Typed loads and stores of 1 to 4 components of an element, in active lanes (docs/
    // design/graphics.md, "Typed buffer formats"). The address and the range check are those of
    // buffer_memory, but the element is all-or-nothing (8.1.5.3): it is inside when its first byte
    // is, an element that starts outside reads 0 in every component, and one that starts inside is
    // written whole. An element of 4 bytes or more is accessed as aligned dwords (the two low bits
    // of its address are ignored); a narrower one byte by byte, from any address. In MTBUF dst_sel
    // plays no part: the components are X, Y, Z, W of the format, those the format doesn't have
    // read 0, and a store gives 0 to those the instruction doesn't have. The formatted MUBUF forms
    // take the format and dst_sel from the V# at run time (formatted). Under tfe a load also
    // writes 0 to the VGPR after its data: no fetch fails.
    void typed_memory(const Instruction& inst) {
        const std::string_view name = inst.name;
        const bool mubuf = inst.format == Format::mubuf;
        const bool load = name.starts_with(mubuf ? "buffer_load_format_" : "tbuffer_load_format_");
        if (!load && !name.starts_with(mubuf ? "buffer_store_format_" : "tbuffer_store_format_")) {
            unsupported();
        }
        if ((inst.flags & flag::lds) != 0) {
            fail(std::format("{} with lds is not supported yet", name));
        }
        if (!mubuf) {
            if (const std::string why = format_error(inst.dfmt, inst.nfmt, !load); !why.empty()) {
                fail(std::format("{}: {}", name, why));
            }
        }
        const auto converts = [&](unsigned nf) {
            return nf != nfmt::uint_ && nf != nfmt::sint && (load || nf != nfmt::float_);
        };
        if (mubuf ? std::ranges::any_of(formatted_pairs, [&](auto p) { return converts(p[1]); })
                  : converts(inst.nfmt)) {
            // The other loads convert through f32 (or unpack a half float), and the UNORM and
            // SNORM stores clamp, scale and round in f32. The float modes keep a half float's
            // infinities, NaNs and -0 on the way; no conversion makes an f32 denormal, and the
            // 16-bit integers convert exactly. The rounding mode does reach the divisions and
            // products of the normalized formats (a known difference, see the design note). A
            // FLOAT store has no float operation: it copies 32 bits or rounds to a small float
            // in integer operations. A formatted MUBUF access may have any of its pairs.
            note_float();
        }
        const bool tfe = load && (inst.flags & flag::tfe) != 0;
        const Operand& data = load ? inst.dst[0] : inst.src[0];
        const unsigned count = data.dwords - (tfe ? 1 : 0);
        const Operand* a = &inst.src[load ? 0 : 1];
        const BufferAccess m = buffer_access(inst, a);
        const Id inside_buffer = inside(m, 0);
        const Id zero = b_.constant_u32(0);
        FormatEmitter::Words value;
        for (unsigned c = 0; c < 4; ++c) {
            value[c] = !load && c < count ? read(data, c) : zero;
        }
        const auto element = [&](unsigned dfmt, unsigned nf, const FormatEmitter::Words& in) {
            return typed_element(load, m, inside_buffer, dfmt, nf, in);
        };
        value = mubuf ? formatted(read(a[1], 3), load, value, element)
                      : element(inst.dfmt, inst.nfmt, value);
        for (unsigned c = 0; load && c < count; ++c) {
            write_vector(data, value[c], c);
        }
        if (tfe) {
            write_vector(data, zero, count);
        }
    }

    // The element of a typed access in the format (dfmt, nf): written from `value`, or read and
    // converted, with 0 in every component when it starts outside the buffer.
    FormatEmitter::Words typed_element(bool load, const BufferAccess& m, Id inside_buffer,
                                       unsigned dfmt, unsigned nf,
                                       const FormatEmitter::Words& value) {
        const FormatLayout& layout = *format_layout(dfmt);
        const Id cond = b_.op(Op::OpLogicalAnd, bool_, {active(), inside_buffer});
        const auto where = [&](unsigned delta) { return device_address(address(m, delta)); };
        const Id zero = b_.constant_u32(0);
        if (!load) {
            const auto raw = FormatEmitter(b_, u32_).encode(dfmt, nf, value);
            if (layout.bytes < 4) {  // contiguous bytes, also in a swizzled buffer
                when(cond, [&] { store_bytes(where(0), raw[0], layout.bytes); });
            }
            for (unsigned d = 0; d < layout.bytes / 4; ++d) {
                guarded_store(cond, b_.op(Op::OpBitwiseAnd, u64_, {where(4 * d), c64(~3ull)}),
                              raw[d]);
            }
            return raw;
        }
        FormatEmitter::Words raw;
        raw.fill(zero);
        if (layout.bytes < 4) {
            for (unsigned j = 0; j < layout.bytes; ++j) {
                const Id byte = guarded_byte(cond, add(where(0), c64(j)));
                raw[0] = b_.op(Op::OpBitwiseOr, u32_,
                               {raw[0], b_.op(Op::OpShiftLeftLogical, u32_,
                                              {byte, b_.constant_u32(8 * j)})});
            }
        }
        for (unsigned d = 0; d < layout.bytes / 4; ++d) {
            raw[d] = guarded_load(cond, b_.op(Op::OpBitwiseAnd, u64_, {where(4 * d), c64(~3ull)}));
        }
        auto decoded = FormatEmitter(b_, u32_).decode(dfmt, nf, raw);
        for (Id& c : decoded) {
            // Zero raw bits are not a zero component in every number format (SNORM_OGL).
            c = b_.op(Op::OpSelect, u32_, {inside_buffer, c, zero});
        }
        return decoded;
    }

    // A formatted MUBUF access: the V#'s dword 3 `dword3` has dst_sel_x to dst_sel_w in bits 0-11
    // (0 is 0, 1 is 1, 4 to 7 are X to W), num_format in bits 12-14 and data_format in bits 15-18
    // (Table 8.5). A switch runs `element` on the pair: each of formatted_pairs has a case, and
    // any other pair reads 0 in all four components, dst_sel ignored, and writes nothing, as an
    // unbound V# (Table 8.5, note 1). A load returns the components through dst_sel, with 1 as
    // 1.0, or the integer 1 in UINT and SINT; a store gives memory component k the first VGPR
    // component whose dst_sel is 4 + k, and 0 when there is none (shadPS4's inverse mapping).
    template <class F>
    FormatEmitter::Words formatted(Id dword3, bool is_load, const FormatEmitter::Words& vgprs,
                                   F&& element) {
        const auto field = [&](unsigned low, unsigned count) {
            return b_.op(Op::OpBitFieldUExtract, u32_,
                         {dword3, b_.constant_u32(low), b_.constant_u32(count)});
        };
        const auto is = [&](Id x, std::uint32_t v) {
            return b_.op(Op::OpIEqual, bool_, {x, b_.constant_u32(v)});
        };
        const auto pick = [&](Id c, Id x, Id y) { return b_.op(Op::OpSelect, u32_, {c, x, y}); };
        const Id zero = b_.constant_u32(0);
        FormatEmitter::Words sel, in = vgprs;
        for (unsigned c = 0; c < 4; ++c) {
            sel[c] = field(3 * c, 3);
        }
        for (unsigned k = 0; k < 4 && !is_load; ++k) {
            in[k] = zero;
            for (unsigned c = 4; c-- > 0;) {
                in[k] = pick(is(sel[c], 4 + k), vgprs[c], in[k]);
            }
        }
        std::array<Id, 5> result;  // X to W, then the constant 1 of the number format
        for (Id& v : result) {
            // Stored here, not as the initializer, which runs once on function entry: an access
            // in a loop would otherwise return a pair without a case the previous pass's values.
            v = b_.local_variable(b_.type_pointer(SC::Function, u32_));
            store(v, zero);
        }
        SpirvBuilder::Words words = {field(12, 7), b_.id()};  // nfmt | dfmt << 3, the merge
        for (const auto [dfmt, nf] : formatted_pairs) {
            words.insert(words.end(), {nf | dfmt << 3, b_.id()});
        }
        b_.op_void(Op::OpSelectionMerge,
                   {words[1], spirv_word(spv::SelectionControlMask::MaskNone)});
        b_.op_void(Op::OpSwitch, words);
        for (std::size_t i = 2; i < words.size(); i += 2) {
            b_.begin_block(words[i + 1]);
            const unsigned nf = words[i] & 7;
            const auto out = element(words[i] >> 3, nf, in);
            for (unsigned c = 0; c < 4 && is_load; ++c) {
                store(result[c], out[c]);
            }
            const bool integer = nf == nfmt::uint_ || nf == nfmt::sint;
            store(result[4], b_.constant_u32(integer ? 1 : 0x3f800000));
            b_.op_void(Op::OpBranch, {words[1]});
        }
        b_.begin_block(words[1]);  // without a case, all five stay 0
        FormatEmitter::Words out;
        for (unsigned c = 0; c < 4; ++c) {
            out[c] = pick(is(sel[c], 1), load(u32_, result[4]), zero);
            for (unsigned k = 0; k < 4; ++k) {
                out[c] = pick(is(sel[c], 4 + k), load(u32_, result[k]), out[c]);
            }
        }
        return out;
    }

    // Writes the low `bytes` (1 or 2) bytes of `value` at the device address `address`, which
    // may cross into the next dword. Other lanes may write the other bytes of the same dword at
    // the same time, so each dword takes the bits with one compare-and-swap loop, as the LDS's
    // sub-dword writes do.
    void store_bytes(Id address, Id value, unsigned bytes) {
        const std::uint32_t mask = (1u << (8 * bytes)) - 1;
        const Id aligned = b_.op(Op::OpBitwiseAnd, u64_, {address, c64(~3ull)});
        const Id shift = byte_shift(b_.op(Op::OpUConvert, u32_, {address}));
        const auto wide = [&](Id x) {  // x << shift, as a u64
            return b_.op(Op::OpShiftLeftLogical, u64_, {b_.op(Op::OpUConvert, u64_, {x}), shift});
        };
        const Id bits = wide(b_.op(Op::OpBitwiseAnd, u32_, {value, b_.constant_u32(mask)}));
        const Id written = wide(b_.constant_u32(mask));
        const auto half = [&](Id x, unsigned d) {  // the low (d = 0) or high dword of x
            return b_.op(Op::OpUConvert, u32_,
                         {d == 0 ? x : b_.op(Op::OpShiftRightLogical, u64_, {x, c64(32)})});
        };
        const auto merge = [&](unsigned d) {
            const Id kept = b_.op(Op::OpNot, u32_, {half(written, d)});
            const Id pointer = device_pointer(add(aligned, c64(4 * d)));
            atomic_update(pointer, spv::Scope::Device, [&](Id v) {
                return b_.op(Op::OpBitwiseOr, u32_,
                             {b_.op(Op::OpBitwiseAnd, u32_, {v, kept}), half(bits, d)});
            });
        };
        merge(0);
        if (bytes > 1) {
            when(b_.op(Op::OpINotEqual, bool_, {half(written, 1), b_.constant_u32(0)}),
                 [&] { merge(1); });
        }
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
    // The byte at a device address where `cond` holds, 0 elsewhere, read through its aligned
    // dword.
    Id guarded_byte(Id cond, Id address) {
        const Id word = guarded_load(cond, b_.op(Op::OpBitwiseAnd, u64_, {address, c64(~3ull)}));
        const Id low =
            b_.op(Op::OpUConvert, u32_, {b_.op(Op::OpBitwiseAnd, u64_, {address, c64(3)})});
        const Id shift = b_.op(Op::OpShiftLeftLogical, u32_, {low, b_.constant_u32(3)});
        return b_.op(Op::OpBitwiseAnd, u32_,
                     {b_.op(Op::OpShiftRightLogical, u32_, {word, shift}), b_.constant_u32(0xff)});
    }
    Id device_pointer(Id address) { return b_.op(Op::OpConvertUToPtr, global_pointer(u32_), {address}); }
    Id add(Id a, Id b) { return b_.op(Op::OpIAdd, u64_, {a, b}); }
    // The device address of a guest address: guest + bias[(guest >> 31) & 15], from the bias
    // table that the draw block's address_bias field points to (docs/design/graphics.md, "GPU
    // memory mirror").
    Id device_address(Id guest) {
        const Id index = b_.op(Op::OpBitwiseAnd, u64_,
                               {b_.op(Op::OpShiftRightLogical, u64_, {guest, c64(bias_shift)}),
                                c64(bias_entries - 1)});
        const Id entry = add(bias_table_, b_.op(Op::OpShiftLeftLogical, u64_, {index, c64(3)}));
        return add(guest, load_global(u64_, b_.op(Op::OpConvertUToPtr, global_pointer(u64_),
                                                  {entry}), 8));
    }
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

    // s_barrier: every wave of the workgroup waits, and its LDS writes become visible. The LDS
    // and the workgroup are compute-only so far; a Vulkan graphics stage has neither.
    void barrier() {
        if (graphics()) {
            fail("s_barrier in a graphics stage is not supported yet");
        }
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
        const bool swizzle = inst.name == "ds_swizzle_b32";
        const auto d = detail::classify_ds(inst.name);
        if (!d && !swizzle) {
            unsupported();
        }
        if ((inst.flags & flag::gds) != 0) {
            fail("GDS is not supported yet");
        }
        if (swizzle) {
            ds_swizzle(inst);
            return;
        }
        if (d->kind == detail::DsKind::nop) {
            return;
        }
        if (graphics()) {
            fail("the LDS in a graphics stage is not supported yet");
        }
        wave_size_dependent_ = true;
        wave_sync();
        const Id address = read(inst.src[0]);
        if (d->kind == detail::DsKind::atomic) {
            ds_atomic(inst, *d, address);
            return;
        }
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

    // ds_swizzle_b32: lane i takes the VGPR of another lane of its wave, or 0 when that lane is
    // inactive. It does not touch the LDS. The 16-bit pattern (offset1:offset0) has two modes. With
    // bit 15 set it is a quad permutation: bits 2k+1:2k pick the lane of the quad that lane k of
    // each aligned quad reads. Otherwise it is a bit mask on the lane within its group of 32:
    // source = ((i & and) | or) ^ xor, with and in bits 4:0, or in bits 9:5 and xor in bits 14:10.
    // The FFT and rotate modes are GFX9 and later. A source in another subgroup, or in a
    // subgroup that is not full and has no such lane, reads 0 (lane-local mode, see shuffle()).
    void ds_swizzle(const Instruction& inst) {
        const unsigned pattern = inst.mem_offset & 0xffff;
        const auto constant = [&](unsigned v) { return b_.constant_u32(v); };
        const auto land = [&](Id a, Id b) { return b_.op(Op::OpBitwiseAnd, u32_, {a, b}); };
        Id source;
        if ((pattern & 0x8000) != 0) {
            const Id shift = b_.op(Op::OpShiftLeftLogical, u32_,
                                   {land(lane_, constant(3)), constant(1)});
            const Id pick = b_.op(Op::OpBitFieldUExtract, u32_,
                                  {constant(pattern & 0xff), shift, constant(2)});
            source = b_.op(Op::OpBitwiseOr, u32_, {land(lane_, constant(~3u)), pick});
        } else {
            const Id kept = land(lane_, constant(0x20 | (pattern & 0x1f)));
            const Id with_or = b_.op(Op::OpBitwiseOr, u32_, {kept, constant(pattern >> 5 & 0x1f)});
            source = b_.op(Op::OpBitwiseXor, u32_, {with_or, constant(pattern >> 10 & 0x1f)});
        }
        const Id value = b_.op(Op::OpSelect, u32_,
                               {active(), read(inst.src[0]), b_.constant_u32(0)});
        write_vector(inst.dst[0], shuffle(value, source));
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
            atomic_update(pointer, spv::Scope::Workgroup, [&](Id v) {
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

    // The 32-bit LDS atomics: add, sub, rsub, inc, dec, min, max, and, or, xor, mskor, wrxchg and
    // cmpst. With a return value, VDST gets the dword from before the operation.
    void ds_atomic(const Instruction& inst, const detail::DsOperation& d, Id address) {
        const Id at = b_.op(Op::OpIAdd, u32_, {address, b_.constant_u32(inst.mem_offset)});
        const auto [pointer, in_range] = lds_ref(at, 0);
        const Id data = read(inst.src[1]);  // data0
        const Id data1 = inst.num_src > 2 ? read(inst.src[2]) : 0;
        const Id old = atomic(d.op, u32_, pointer, spv::Scope::Workgroup,
                              b_.op(Op::OpLogicalAnd, bool_, {active(), in_range}), data, data1);
        if (d.returns) {
            write_vector(inst.dst[0], old);
        }
    }

    // Applies `op` to the dword or qword (`type`) at `pointer` at scope `s` where `cond` holds,
    // and returns the old value, 0 elsewhere. `data1` is the second operand of mskor, cmpst and
    // fcmpst (MEM = MEM == data ? data1 : MEM). The operations that have no SPIR-V atomic update
    // the memory in a compare-and-swap loop.
    Id atomic(detail::DsAtomic op, Id type, Id pointer, spv::Scope s, Id cond, Id data, Id data1) {
        using A = detail::DsAtomic;
        const Id scope = b_.constant_u32(spirv_word(s));
        const Id none = b_.constant_u32(0);
        const auto direct = [&](Op o) { return b_.op(o, type, {pointer, scope, none, data}); };
        const Id result = b_.local_variable(b_.type_pointer(SC::Function, type));
        store(result, type == u64_ ? c64(0) : b_.constant_u32(0));
        when(cond, [&] {
            Id old = 0;
            switch (op) {
            case A::add: old = direct(Op::OpAtomicIAdd); break;
            case A::sub: old = direct(Op::OpAtomicISub); break;
            case A::smin: old = direct(Op::OpAtomicSMin); break;
            case A::smax: old = direct(Op::OpAtomicSMax); break;
            case A::umin: old = direct(Op::OpAtomicUMin); break;
            case A::umax: old = direct(Op::OpAtomicUMax); break;
            case A::bit_and: old = direct(Op::OpAtomicAnd); break;
            case A::bit_or: old = direct(Op::OpAtomicOr); break;
            case A::bit_xor: old = direct(Op::OpAtomicXor); break;
            case A::exchange: old = direct(Op::OpAtomicExchange); break;
            case A::cmpst:  // MEM = MEM == data0 ? data1 : MEM
                old = b_.op(Op::OpAtomicCompareExchange, type,
                            {pointer, scope, none, none, data1, data});
                break;
            default:
                old = atomic_update(
                    pointer, s, [&](Id v) { return updated(op, type, v, data, data1); }, type);
            }
            store(result, old);
        });
        return load(type, result);
    }

    // The new value of the dword or qword `v` (`type`) for the operations that need a
    // compare-and-swap loop. The float ones compare the bits as IEEE values, so denormals are
    // kept whatever the float mode (the manual: "handles NaN/INF/denorm"). Our reading,
    // unverified: fmin and fmax give the other operand when one is NaN and order -0 below +0, and
    // fcmpst takes +0 and -0 as equal and a NaN as equal to nothing.
    Id updated(detail::DsAtomic op, Id type, Id v, Id data, Id data1) {
        using A = detail::DsAtomic;
        const bool wide = type == u64_;
        const auto k = [&](std::uint64_t x) {
            return wide ? c64(x) : b_.constant_u32(static_cast<std::uint32_t>(x));
        };
        const auto u = [&](Op o, Id x, Id y) { return b_.op(o, type, {x, y}); };
        const auto test = [&](Op o, Id x, Id y) { return b_.op(o, bool_, {x, y}); };
        const auto pick = [&](Id c, Id x, Id y) { return b_.op(Op::OpSelect, type, {c, x, y}); };
        const Id sign = k(wide ? 1ull << 63 : 1ull << 31);
        const Id magnitude = b_.op(Op::OpNot, type, {sign});
        const auto nan = [&](Id x) {
            return test(Op::OpUGreaterThan, u(Op::OpBitwiseAnd, x, magnitude),
                        k(wide ? 0x7ff0'0000'0000'0000ull : 0x7f80'0000u));
        };
        const auto key = [&](Id x) {  // ordered as unsigned like the floats, -0 below +0
            return pick(test(Op::OpINotEqual, u(Op::OpBitwiseAnd, x, sign), k(0)),
                        b_.op(Op::OpNot, type, {x}), u(Op::OpBitwiseOr, x, sign));
        };
        switch (op) {
        case detail::DsAtomic::rsub:
            return u(Op::OpISub, data, v);
        case A::inc:  // wraps to 0 after data
            return pick(test(Op::OpUGreaterThanEqual, v, data), k(0), u(Op::OpIAdd, v, k(1)));
        case A::dec:  // wraps to data after 0 and above data
            return pick(b_.op(Op::OpLogicalOr, bool_, {test(Op::OpIEqual, v, k(0)),
                                                       test(Op::OpUGreaterThan, v, data)}),
                        data, u(Op::OpISub, v, k(1)));
        case A::fmin:
        case A::fmax: {
            const Id better = op == A::fmin ? test(Op::OpULessThan, key(data), key(v))
                                            : test(Op::OpUGreaterThan, key(data), key(v));
            return pick(nan(data), v, pick(nan(v), data, pick(better, data, v)));
        }
        case A::fcmpst: {
            const Id zeros = test(Op::OpIEqual, u(Op::OpBitwiseAnd, u(Op::OpBitwiseOr, v, data),
                                                  magnitude), k(0));
            const Id equal = b_.op(Op::OpLogicalOr, bool_, {test(Op::OpIEqual, v, data), zeros});
            const Id numbers = b_.op(Op::OpLogicalNot, bool_,
                                     {b_.op(Op::OpLogicalOr, bool_, {nan(v), nan(data)})});
            return pick(b_.op(Op::OpLogicalAnd, bool_, {equal, numbers}), data1, v);
        }
        default:  // mskor: (MEM & ~data0) | data1
            return u(Op::OpBitwiseOr, u(Op::OpBitwiseAnd, v, b_.op(Op::OpNot, type, {data})),
                     data1);
        }
    }

    // Replaces the dword (or the value of `type`) at `pointer` by f(old) atomically, as a loop on
    // a compare-and-swap at scope `s` (the LDS's workgroup, or the device for buffers), and
    // returns the old value.
    template <typename F>
    Id atomic_update(Id pointer, spv::Scope s, F f, Id type = 0) {
        type = type != 0 ? type : u32_;
        const Id scope = b_.constant_u32(spirv_word(s));
        const Id none = b_.constant_u32(0);
        // The other lanes update the dword atomically at the same time, so the first read is an
        // atomic one too, or it would race with them. A stale value is fixed by the loop.
        const Id current = b_.local_variable(b_.type_pointer(SC::Function, type));
        store(current, b_.op(Op::OpAtomicLoad, type, {pointer, scope, none}));
        const Id header = b_.id(), body = b_.id(), cont = b_.id(), merge = b_.id();
        b_.op_void(Op::OpBranch, {header});
        b_.begin_block(header);
        b_.op_void(Op::OpLoopMerge, {merge, cont, spirv_word(spv::LoopControlMask::MaskNone)});
        b_.op_void(Op::OpBranch, {body});
        b_.begin_block(body);
        const Id old = load(type, current);
        const Id seen = b_.op(Op::OpAtomicCompareExchange, type,
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
        const auto [from, inside] = subgroup_lane(lane);
        const Id r = b_.op(Op::OpGroupNonUniformBroadcast, u32_,
                           {b_.constant_u32(spirv_word(spv::Scope::Subgroup)), value, from});
        return b_.op(Op::OpSelect, u32_, {inside, r, b_.constant_u32(0)});
    }

    // `value` of wave lane `lane` (any value per lane), or 0 when that lane belongs to another
    // subgroup or does not exist. A subgroup is not full when the workgroup size is not a
    // multiple of the subgroup size, and shuffling from an invocation that is not there is
    // undefined, so the source must be set in the subgroup's ballot of the invocations.
    Id shuffle(Id value, Id lane) {
        b_.capability(spv::Capability::GroupNonUniformShuffle);
        const auto [from, inside] = subgroup_lane(lane);
        const Id scope = b_.constant_u32(spirv_word(spv::Scope::Subgroup));
        const Id present = b_.op(Op::OpGroupNonUniformBallot, b_.type_vector(u32_, 4),
                                 {scope, b_.constant_bool(true)});
        const Id exists = b_.op(Op::OpGroupNonUniformBallotBitExtract, bool_,
                                {scope, present, from});
        const Id r = b_.op(Op::OpGroupNonUniformShuffle, u32_, {scope, value, from});
        const Id readable = b_.op(Op::OpLogicalAnd, bool_, {inside, exists});
        return b_.op(Op::OpSelect, u32_, {readable, r, b_.constant_u32(0)});
    }

    // The invocation of this subgroup that is wave lane `lane` (0 when there is none), and whether
    // the lane is in the subgroup.
    struct SubgroupLane {
        Id from, inside;
    };
    SubgroupLane subgroup_lane(Id lane) {
        cross_lane();
        const Id local = b_.op(Op::OpISub, u32_, {lane, subgroup_base()});
        const Id inside = b_.op(Op::OpULessThan, bool_,
                                {local, load(u32_, builtin(spv::BuiltIn::SubgroupSize))});
        return {b_.op(Op::OpSelect, u32_, {inside, local, b_.constant_u32(0)}), inside};
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
        if (o.kind == OperandKind::scalar && is_mask_half(c, When::after)) {
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
        if (!is_mask(o, When::after)) {
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
    Id sgpr(unsigned c) override { return variable(sgprs_, c, u32_, b_.constant_u32(0)); }
    Id vgpr(unsigned n) override { return variable(vgprs_, n, u32_, b_.constant_u32(0)); }
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

    Id active() override { return load(bool_, mask_exec()); }

    Id load(Id type, Id pointer) { return b_.op(Op::OpLoad, type, {pointer}); }
    void store(Id pointer, Id value) { b_.op_void(Op::OpStore, {pointer, value}); }
    Id global_pointer(Id type) { return b_.type_pointer(SC::PhysicalStorageBuffer, type); }
    Id load_global(Id type, Id pointer, std::uint32_t align) {
        return b_.op(Op::OpLoad, type,
                     {pointer, spirv_word(spv::MemoryAccessMask::Aligned), align});
    }

    // An error in an inlined fetch instruction is at the call and names the instruction's
    // offset in the fetch code.
    [[noreturn]] void fail(const std::string& message) const override {
        const auto index = static_cast<std::uint32_t>(inst_ - program_.data());
        if (fetch_.inlined(index)) {
            throw GcnError(std::format("fetch subroutine: {} at offset {:#x}", message,
                                       fetch_.offsets[index - fetch_.call - 1]),
                           inst_->offset, code_[inst_->offset / 4]);
        }
        throw GcnError(message, inst_->offset, code_[inst_->offset / 4]);
    }
    [[noreturn]] void unsupported() const {
        fail(std::format("unsupported instruction {}", inst_->name));
    }

    std::span<const std::uint32_t> code_;
    const ShaderKey& key_;
    std::vector<Instruction> program_;
    detail::FetchSite fetch_;
    Cfg cfg_;
    ImageSources image_sources_;
    SpirvBuilder b_;
    detail::StageIo stage_{b_, key_, *this};
    detail::ImageOps images_{b_, key_, *this, image_sources_};
    detail::Alu alu_{b_};
    Id bool_ = 0, u32_ = 0, u64_ = 0;
    Id lane_ = 0, bias_table_ = 0, lds_ = 0;
    Id draw_ = 0;  // the draw block's pointer
    std::vector<Id> interface_;
    std::map<unsigned, Id> sgprs_, vgprs_, bools_;  // bools_: masks and SCC by operand code
    std::map<std::uint32_t, Id> builtins_;
    std::set<unsigned> mask_sgprs_;                 // the first SGPR of each mask pair
    std::vector<StartState> start_before_, start_after_;  // per instruction of program_
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
    return Translator(code, fetch, key).run();
}

}  // namespace bb::gcn
