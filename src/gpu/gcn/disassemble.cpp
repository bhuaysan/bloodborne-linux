// Disassembler in LLVM's AMDGPU assembly syntax for gfx7. Every line assembles back to the same
// words with llvm-mc, except for the encodings that LLVM's syntax can't express (see the
// known differences under "Disassembler" in the "Interface" section of docs/design/graphics.md).

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/opcodes.hpp"
#include "gpu/gcn/memory.hpp"

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

// A value as LLVM prints the 16-bit counts and the literal of s_setreg_imm32_b32: the integers
// of the inline constants (-16 to 64) in decimal, others in hex. LLVM prints the float
// constants' patterns as floats there too, but reads such a float back as another value, so they
// stay in hex.
std::string immediate(std::uint32_t value) {
    if (value <= 64 || value >= 0xfffffff0) {
        return std::to_string(static_cast<std::int32_t>(value));
    }
    return std::format("{:#x}", value);
}

// How a literal prints.
enum class Literal {
    lit,        // SOP2, SOP1, SOPC: the field could hold an inline constant instead, so a literal
                // with an inline value needs lit() to keep its encoding
    immediate,  // s_setreg_imm32_b32: always a literal, printed like LLVM prints it
    hex,        // SMRD offsets: LLVM encodes 255 or less in the immediate form, with or without
                // lit()
};

std::string operand(const Operand& op, Literal literal) {
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
        if (literal == Literal::immediate) {
            return immediate(op.value);
        }
        if (literal == Literal::lit && inlinable(op.value, op.dwords)) {
            return std::format("lit({:#x})", op.value);
        }
        return std::format("{:#x}", op.value);
    case OperandKind::offset:
    case OperandKind::simm:
        return std::format("{:#x}", op.value);
    case OperandKind::count:
        return immediate(op.value);
    case OperandKind::branch:
        return std::to_string(static_cast<std::int32_t>(op.value));
    case OperandKind::hwreg:
        return hwreg(op.value);
    case OperandKind::waitcnt:
        return waitcnt(op.value);
    case OperandKind::sendmsg:
        return sendmsg(op.value);
    case OperandKind::vector:
        return range("v", op.code, op.dwords);
    case OperandKind::off:
        return "off";
    }
    return "?";
}

}  // namespace

std::string disassemble(const Instruction& inst) {
    const Literal literal = inst.format == Format::sopk   ? Literal::immediate
                            : inst.format == Format::smrd ? Literal::hex
                                                          : Literal::lit;
    std::string out(inst.name);
    const char* separator = " ";
    auto append = [&](const Operand& op) {
        out += separator;
        out += operand(op, literal);
        separator = ", ";
    };
    for (unsigned i = 0; i < inst.num_dst; ++i) {
        append(inst.dst[i]);
    }
    for (unsigned i = 0; i < inst.num_src; ++i) {
        append(inst.src[i]);
    }
    if (inst.format == Format::ds || inst.format == Format::flat) {
        out += detail::ds_flat_modifiers(inst);
    }
    if (inst.format == Format::mubuf) {
        out += detail::memory_fields(inst);
    }
    return out;
}

}  // namespace bb::gcn
