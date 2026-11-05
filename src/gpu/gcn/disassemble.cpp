// Disassembler in LLVM's AMDGPU assembly syntax for gfx7. Every line assembles back to the same
// words with llvm-mc, except for the encodings that LLVM's syntax can't express (see
// docs/design/graphics.md, "Decoder and disassembler").

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "gpu/gcn/gcn.hpp"

namespace bb::gcn {

namespace {

// The float inline constants, operand codes 240 to 247.
constexpr std::array<float, 8> float_constants = {0.5f, -0.5f, 1.0f, -1.0f,
                                                  2.0f, -2.0f, 4.0f, -4.0f};
constexpr std::array<std::string_view, 8> float_names = {"0.5", "-0.5", "1.0", "-1.0",
                                                         "2.0", "-2.0", "4.0", "-4.0"};

// A register range such as s[4:7], or s4 for one dword.
std::string range(std::string_view prefix, unsigned first, unsigned dwords) {
    if (dwords == 1) {
        return std::format("{}{}", prefix, first);
    }
    return std::format("{}[{}:{}]", prefix, first, first + dwords - 1);
}

// A scalar register operand. The decoder has checked that `c` is valid for `dwords`.
std::string scalar(unsigned c, unsigned dwords) {
    if (c < code::flat_scratch) {
        return range("s", c, dwords);
    }
    if (c >= code::ttmp0 && c < code::m0) {
        return range("ttmp", c - code::ttmp0, dwords);
    }
    switch (c) {
    case code::m0:
        return "m0";
    case code::vccz:
        return "src_vccz";
    case code::execz:
        return "src_execz";
    case code::scc:
        return "src_scc";
    default:
        break;
    }
    // A register pair (flat_scratch, vcc, tba, tma, exec) or one of its halves.
    const std::string_view pair = c < code::vcc   ? "flat_scratch"
                                  : c < code::tba ? "vcc"
                                  : c < code::tma ? "tba"
                                  : c < code::m0  ? "tma"
                                                  : "exec";
    if (dwords == 2) {
        return std::string(pair);
    }
    return std::format("{}_{}", pair, c % 2 == 0 ? "lo" : "hi");
}

// Whether llvm-mc would encode a literal of this value as an inline constant instead. On 64-bit
// operands it reads the 32-bit literal as a 64-bit integer, so only 0 to 64 are inline there.
bool inlinable(std::uint32_t value, unsigned dwords) {
    if (value <= 64) {
        return true;
    }
    if (dwords != 1) {
        return false;
    }
    if (value >= 0xfffffff0) {  // -16 to -1
        return true;
    }
    for (float f : float_constants) {
        if (value == std::bit_cast<std::uint32_t>(f)) {
            return true;
        }
    }
    return false;
}

// hwreg(id, offset, size), or hwreg(id) for the whole register. Every 16-bit value has a form.
std::string hwreg(std::uint32_t v) {
    constexpr std::array<std::string_view, 8> names = {
        "", "HW_REG_MODE", "HW_REG_STATUS", "HW_REG_TRAPSTS",
        "HW_REG_HW_ID", "HW_REG_GPR_ALLOC", "HW_REG_LDS_ALLOC", "HW_REG_IB_STS"};
    const unsigned id = v & 0x3f;
    const unsigned offset = (v >> 6) & 0x1f;
    const unsigned size = ((v >> 11) & 0x1f) + 1;
    const std::string name =
        id != 0 && id < names.size() ? std::string(names[id]) : std::to_string(id);
    if (offset == 0 && size == 32) {
        return std::format("hwreg({})", name);
    }
    return std::format("hwreg({}, {}, {})", name, offset, size);
}

// The counters of s_waitcnt: vmcnt in bits 0-3, expcnt in 4-6 and lgkmcnt in 8-11. Counters at
// their maximum are left out unless all are. A value with other bits set prints raw.
std::string waitcnt(std::uint32_t v) {
    const unsigned vm = v & 0xf;
    const unsigned exp = (v >> 4) & 0x7;
    const unsigned lgkm = (v >> 8) & 0xf;
    if (v != (vm | exp << 4 | lgkm << 8)) {
        return std::format("{:#x}", v);
    }
    const bool all = vm == 0xf && exp == 0x7 && lgkm == 0xf;
    std::string out;
    auto counter = [&](std::string_view name, unsigned value, unsigned max) {
        if (all || value != max) {
            out += std::format("{}{}({})", out.empty() ? "" : " ", name, value);
        }
    };
    counter("vmcnt", vm, 0xf);
    counter("expcnt", exp, 0x7);
    counter("lgkmcnt", lgkm, 0xf);
    return out;
}

// The message of s_sendmsg: id in bits 0-3, operation in 4-6, GS stream in 8-9. Messages that
// LLVM has no name for print as the raw value.
std::string sendmsg(std::uint32_t v) {
    constexpr std::array<std::string_view, 4> gs_ops = {"GS_OP_NOP", "GS_OP_CUT", "GS_OP_EMIT",
                                                        "GS_OP_EMIT_CUT"};
    constexpr std::array<std::string_view, 5> sys_ops = {
        "", "SYSMSG_OP_ECC_ERR_INTERRUPT", "SYSMSG_OP_REG_RD", "SYSMSG_OP_HOST_TRAP_ACK",
        "SYSMSG_OP_TTRACE_PC"};
    const unsigned id = v & 0xf;
    const unsigned op = (v >> 4) & 0x7;
    const unsigned stream = (v >> 8) & 0x3;
    if (v == 1) {
        return "sendmsg(MSG_INTERRUPT)";
    }
    // MSG_GS and MSG_GS_DONE take a 2-bit operation. MSG_GS needs one other than GS_OP_NOP.
    if ((id == 2 || id == 3) && (v & ~0x33fu) == 0) {
        const std::string_view msg = id == 2 ? "MSG_GS" : "MSG_GS_DONE";
        if (op != 0) {
            return std::format("sendmsg({}, {}, {})", msg, gs_ops[op], stream);
        }
        if (id == 3 && stream == 0) {
            return "sendmsg(MSG_GS_DONE, GS_OP_NOP)";
        }
    }
    if (id == 15 && (v & ~0x7fu) == 0 && op != 0 && op < sys_ops.size()) {
        return std::format("sendmsg(MSG_SYSMSG, {})", sys_ops[op]);
    }
    return std::to_string(v);
}

// `may_inline`: the operand field could hold an inline constant instead of the literal, so a
// literal with an inline value needs lit() to keep its encoding.
std::string operand(const Operand& op, bool may_inline) {
    switch (op.kind) {
    case OperandKind::none:
        break;
    case OperandKind::scalar:
        return scalar(op.code, op.dwords);
    case OperandKind::constant:
        if (op.code >= 240) {
            return std::string(float_names[op.code - 240]);
        }
        return std::to_string(static_cast<std::int32_t>(op.value));
    case OperandKind::literal:
        if (may_inline && inlinable(op.value, op.dwords)) {
            return std::format("lit({:#x})", op.value);
        }
        return std::format("{:#x}", op.value);
    case OperandKind::offset:
    case OperandKind::simm:
        return std::format("{:#x}", op.value);
    case OperandKind::count:
        return std::to_string(op.value);
    case OperandKind::branch:
        return std::to_string(static_cast<std::int32_t>(op.value));
    case OperandKind::hwreg:
        return hwreg(op.value);
    case OperandKind::waitcnt:
        return waitcnt(op.value);
    case OperandKind::sendmsg:
        return sendmsg(op.value);
    }
    return "?";
}

}  // namespace

std::string disassemble(const Instruction& inst) {
    // The literal of s_setreg_imm32_b32 is part of the encoding, and an SMRD offset of 255 or less
    // always gets the immediate form in LLVM, with or without lit().
    const bool may_inline =
        inst.format == Format::sop2 || inst.format == Format::sop1 || inst.format == Format::sopc;
    std::string out(inst.name);
    const char* separator = " ";
    auto append = [&](const Operand& op) {
        out += separator;
        out += operand(op, may_inline);
        separator = ", ";
    };
    for (unsigned i = 0; i < inst.num_dst; ++i) {
        append(inst.dst[i]);
    }
    for (unsigned i = 0; i < inst.num_src; ++i) {
        append(inst.src[i]);
    }
    return out;
}

}  // namespace bb::gcn
