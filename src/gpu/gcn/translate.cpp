// GCN to SPIR-V: one invocation per lane, the registers as function variables, and EXEC as
// predication (docs/design/graphics.md, "Execution model"). The instruction semantics follow the
// Sea Islands ISA manual (scalar and vector ALU operations, FLAT); the compute stage's initial
// registers follow COMPUTE_PGM_RSRC2 (Sea Islands register reference).

#include "gpu/gcn/translate.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <string>
#include <string_view>

#include "gpu/gcn/cfg.hpp"
#include "gpu/gcn/gcn.hpp"
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

// SOPP opcodes of the conditional branches on SCC.
constexpr std::uint16_t op_cbranch_scc0 = 4;
constexpr std::uint16_t op_cbranch_scc1 = 5;

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

class Translator {
public:
    Translator(std::span<const std::uint32_t> code, const ShaderKey& key)
        : code_(code), key_(key), program_(decode(code)), cfg_(build_cfg(program_)) {}

    Translation run() {
        check_key();
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
        return {b_.words(), {}};
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

    Id builtin(spv::BuiltIn which, Id type) {
        const Id var = b_.variable(b_.type_pointer(SC::Input, type), SC::Input);
        b_.decorate(var, spv::Decoration::BuiltIn, {spirv_word(which)});
        interface_.push_back(var);
        return var;
    }

    // Emits a block's instructions. Its branch or s_endpgm is the block's exit.
    void body(std::uint32_t index) {
        const Block& block = cfg_.blocks[index];
        for (std::uint32_t i = block.first; i < block.first + block.count; ++i) {
            const Instruction& inst = program_[i];
            inst_ = &inst;
            if (branch_target(inst) || is_endpgm(inst) || inst.name == "s_nop" ||
                inst.name == "s_waitcnt") {
                continue;
            }
            switch (inst.format) {
            case Format::sop1:
            case Format::sop2:
                scalar(inst);
                break;
            case Format::vop1:
            case Format::vop2:
            case Format::vop3:
                vector(inst);
                break;
            case Format::flat:
                flat(inst);
                break;
            default:
                unsupported();
            }
        }
    }

    // The condition of a block's conditional branch.
    Id condition(std::uint32_t index) {
        const Block& block = cfg_.blocks[index];
        inst_ = &program_[block.first + block.count - 1];
        if (inst_->opcode == op_cbranch_scc0) {
            return b_.op(Op::OpLogicalNot, bool_, {load(bool_, scc())});
        }
        if (inst_->opcode != op_cbranch_scc1) {
            unsupported();
        }
        return load(bool_, scc());
    }

    // SOP1 and SOP2. Scalar results are not predicated.
    void scalar(const Instruction& inst) {
        const std::string_view name = inst.name;
        const Operand& dst = inst.dst[0];
        if (name == "s_mov_b64" && dst.kind == OperandKind::scalar && dst.code == code::exec) {
            store(mask_exec(), read_mask(inst.src[0]));
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
        } else {
            unsupported();
        }
    }

    // VOP1, VOP2 and VOP3: the result is written as select(active, new, old).
    void vector(const Instruction& inst) {
        if (inst.neg != 0 || inst.abs != 0 || inst.omod != 0 || (inst.flags & flag::clamp) != 0) {
            fail(std::format("modifiers of {} are not supported yet", inst.name));
        }
        std::string_view name = inst.name;
        if (name.ends_with("_e32") || name.ends_with("_e64")) {
            name.remove_suffix(4);
        }
        if (name == "v_nop") {
            return;
        }
        const auto op = std::ranges::find(vector_ops, name, &VectorOp::name);
        if (name == "v_mov_b32") {
            write_vector(inst.dst[0], read(inst.src[0]));
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

    // flat_store_dword reaches guest memory through the address bias, in active lanes only. There
    // is no range check, and the LDS and scratch apertures are not handled.
    void flat(const Instruction& inst) {
        if (inst.name != "flat_store_dword") {
            unsupported();
        }
        const Id high = b_.op(Op::OpShiftLeftLogical, u64_,
                              {b_.op(Op::OpUConvert, u64_, {read(inst.src[0], 1)}),
                               b_.constant_u32(32)});
        const Id guest = b_.op(Op::OpBitwiseOr, u64_,
                               {high, b_.op(Op::OpUConvert, u64_, {read(inst.src[0], 0)})});
        const Id pointer = b_.op(Op::OpConvertUToPtr, global_pointer(u32_),
                                 {b_.op(Op::OpIAdd, u64_, {guest, bias_})});
        const Id data = read(inst.src[1]);
        const Id guard = active();
        const Id then = b_.id();
        const Id merge = b_.id();
        b_.op_void(Op::OpSelectionMerge, {merge, spirv_word(spv::SelectionControlMask::MaskNone)});
        b_.op_void(Op::OpBranchConditional, {guard, then, merge});
        b_.begin_block(then);
        b_.op_void(Op::OpStore, {pointer, data, spirv_word(spv::MemoryAccessMask::Aligned), 4});
        b_.op_void(Op::OpBranch, {merge});
        b_.begin_block(merge);
    }

    // Dword `dword` of a source operand, as a u32.
    Id read(const Operand& o, unsigned dword = 0) {
        switch (o.kind) {
        case OperandKind::vector:
            return load(u32_, vgpr(o.code + dword));
        case OperandKind::scalar:
            return load(u32_, value_sgpr(o.code + dword));
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

    // A lane mask source, EXEC or a constant: this lane's bit of it.
    Id read_mask(const Operand& o) {
        if (o.kind == OperandKind::scalar && o.code == code::exec) {
            return active();
        }
        if (o.kind != OperandKind::constant) {
            fail(std::format("unsupported mask operand of {}", inst_->name));
        }
        const std::uint32_t low = constant_word(o, 0);
        const std::uint32_t high = constant_word(o, 1);
        if (low == high && (low == 0 || low == ~0u)) {
            return b_.constant_bool(low != 0);
        }
        const Id lower = b_.op(Op::OpULessThan, bool_, {lane_, b_.constant_u32(32)});
        return lane_bit(b_.op(Op::OpSelect, u32_,
                              {lower, b_.constant_u32(low), b_.constant_u32(high)}));
    }

    // Bit (lane ID modulo 32) of `word`.
    Id lane_bit(Id word) {
        const Id shift = b_.op(Op::OpBitwiseAnd, u32_, {lane_, b_.constant_u32(31)});
        const Id bit = b_.op(Op::OpBitwiseAnd, u32_,
                             {b_.op(Op::OpShiftRightLogical, u32_, {word, shift}),
                              b_.constant_u32(1)});
        return b_.op(Op::OpINotEqual, bool_, {bit, b_.constant_u32(0)});
    }

    // Dword `dword` of a scalar destination. A 32-bit write to exec_lo or exec_hi sets the
    // lanes of that half to their bit of the value.
    void write_scalar(const Operand& o, Id value, unsigned dword = 0) {
        const unsigned c = o.code + dword;
        if (o.kind == OperandKind::scalar && (c & ~1u) == code::exec) {
            const Op half = (c & 1) != 0 ? Op::OpUGreaterThanEqual : Op::OpULessThan;
            const Id mine = b_.op(half, bool_, {lane_, b_.constant_u32(32)});
            store(mask_exec(), b_.op(Op::OpSelect, bool_, {mine, lane_bit(value), active()}));
            return;
        }
        if (o.kind != OperandKind::scalar) {
            unsupported();
        }
        store(value_sgpr(c), value);
    }

    void write_vector(const Operand& o, Id value) {
        const Id var = vgpr(o.code);
        store(var, b_.op(Op::OpSelect, u32_, {active(), value, load(u32_, var)}));
    }

    // Registers: a function variable each, declared on first use. SGPRs and M0 hold a u32, EXEC
    // this lane's bit, SCC a bool.
    Id value_sgpr(unsigned c) {
        if ((c & ~1u) == code::exec) {
            fail(std::format("{} is read as a value", c == code::exec ? "exec_lo" : "exec_hi"));
        }
        if (c >= code::flat_scratch && c != code::m0) {  // also SCC and the other condition bits
            fail(std::format("unsupported register operand {} of {}", c, inst_->name));
        }
        return sgpr(c);
    }
    Id sgpr(unsigned c) { return variable(sgprs_, c, u32_, b_.constant_u32(0)); }
    Id vgpr(unsigned n) { return variable(vgprs_, n, u32_, b_.constant_u32(0)); }
    Id mask_exec() { return variable(bools_, code::exec, bool_, b_.constant_bool(true)); }
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
    Id lane_ = 0, bias_ = 0;
    std::vector<Id> interface_;
    std::map<unsigned, Id> sgprs_, vgprs_, bools_;  // bools_: EXEC and SCC by operand code
    const Instruction* inst_ = nullptr;
};

}  // namespace

Translation translate(std::span<const std::uint32_t> code, std::span<const std::uint32_t> fetch,
                      const ShaderKey& key) {
    static_cast<void>(fetch);  // the vertex stages' fetch subroutine, not used in compute
    return Translator(code, key).run();
}

}  // namespace bb::gcn
