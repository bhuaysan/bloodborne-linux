// Decoder for the scalar microcode formats (Sea Islands ISA manual, "Microcode Formats"). DS is
// decoded in ds_flat.cpp (with FLAT), MUBUF, MTBUF, MIMG and EXP in memory.cpp.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>

#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/memory.hpp"
#include "gpu/gcn/opcodes.hpp"
#include "gpu/gcn/vop.hpp"

namespace bb::gcn {

GcnError::GcnError(const std::string& message, std::uint32_t offset, std::uint32_t word)
    : std::runtime_error(std::format("{} at offset {:#x} (word {:#010x})", message, offset, word)),
      offset_(offset),
      word_(word) {}

namespace detail {

// `value` of the float inline constant with code `c`: the f32 pattern, or on a 64-bit operand the
// high dword of the f64 (its low dword is 0 for every float constant).
std::uint32_t float_constant(unsigned c, unsigned dwords) {
    constexpr std::array<double, 8> numbers = {0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 4.0, -4.0};
    const double d = numbers[c - 240];
    return dwords == 2 ? static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(d) >> 32)
                       : std::bit_cast<std::uint32_t>(static_cast<float>(d));
}

std::uint32_t bits(std::uint32_t word, unsigned low, unsigned count) {
    return (word >> low) & ((1u << count) - 1);
}

// Whether operand code `c` names a register operand of `dwords` dwords. SGPR and trap temporary
// ranges are aligned to 2 dwords for pairs and to 4 for larger ranges. The condition bits vccz,
// execz and scc are sources of either size (LLVM accepts them as 64-bit sources too).
bool valid_register(unsigned c, unsigned dwords) {
    const bool aligned = dwords == 1 || c % (dwords == 2 ? 2 : 4) == 0;
    if (c < code::flat_scratch) {
        return aligned && c + dwords <= code::flat_scratch;
    }
    if (c >= code::ttmp0 && c < code::m0) {
        return aligned && c + dwords <= code::m0;
    }
    if (c >= code::vccz && c <= code::scc) {
        return dwords <= 2;
    }
    if (dwords == 1) {
        return c < 128 && c != 125;
    }
    return dwords == 2 && (c == code::flat_scratch || c == code::vcc || c == code::tba ||
                           c == code::tma || c == code::exec);
}

}  // namespace detail

namespace {

using detail::bits;
using detail::float_constant;
using detail::Imm;
using detail::OpInfo;
using detail::valid_register;

// Decodes the instruction that starts at code[index].
class Decoder {
public:
    Decoder(std::span<const std::uint32_t> code, std::size_t index)
        : code_(code), word_(code[index]), index_(index) {
        inst_.offset = static_cast<std::uint32_t>(index * 4);
        inst_.size = 1;
    }

    Instruction run() {
        const std::uint32_t w = word_;
        if (bits(w, 23, 9) == 0x17d) {
            start(Format::sop1, bits(w, 8, 8), "SOP1");
            sop_operands(bits(w, 16, 7), bits(w, 0, 8), 0);
        } else if (bits(w, 23, 9) == 0x17e) {
            start(Format::sopc, bits(w, 16, 7), "SOPC");
            sop_operands(0, bits(w, 0, 8), bits(w, 8, 8));
        } else if (bits(w, 23, 9) == 0x17f) {
            start(Format::sopp, bits(w, 16, 7), "SOPP");
            immediate(bits(w, 0, 16));
        } else if (bits(w, 28, 4) == 0xb) {
            start(Format::sopk, bits(w, 23, 5), "SOPK");
            sopk_operands(bits(w, 16, 7), bits(w, 0, 16));
        } else if (bits(w, 30, 2) == 2) {
            start(Format::sop2, bits(w, 23, 7), "SOP2");
            sop_operands(bits(w, 16, 7), bits(w, 0, 8), bits(w, 8, 8));
        } else if (bits(w, 27, 5) == 0x18) {
            start(Format::smrd, bits(w, 22, 5), "SMRD");
            smrd_operands();
        } else if (detail::is_ds_flat(w)) {
            return detail::decode_ds_flat(code_, index_);
        } else if (detail::is_memory_encoding(w)) {
            return detail::decode_memory(code_, index_);
        } else if (bits(w, 26, 6) == 0x34) {
            return detail::decode_vop3(code_, index_);
        } else if (bits(w, 26, 6) == 0x32) {
            return detail::decode_vintrp(code_, index_);
        } else if (bits(w, 31, 1) == 0) {
            return detail::decode_vop(code_, index_);  // VOP1, VOP2, VOPC
        } else {
            fail("unsupported encoding");
        }
        return inst_;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw GcnError(message, inst_.offset, word_);
    }

    void start(Format format, unsigned opcode, const char* format_name) {
        const auto& table = detail::opcodes(format);
        if (opcode >= table.size() || table[opcode].name.empty()) {
            fail(std::format("unknown {} opcode {}", format_name, opcode));
        }
        info_ = &table[opcode];
        inst_.format = format;
        inst_.opcode = static_cast<std::uint16_t>(opcode);
        inst_.name = info_->name;
    }

    void add_dst(Operand op) { inst_.dst[inst_.num_dst++] = op; }
    void add_src(Operand op) { inst_.src[inst_.num_src++] = op; }

    // The literal that follows the instruction. Every operand that reads it gets the same value.
    std::uint32_t literal() {
        if (index_ + 1 >= code_.size()) {
            fail("literal past the end of the code");
        }
        inst_.size = 2;
        return code_[index_ + 1];
    }

public:
    // Operand from an 8-bit scalar source field or a 7-bit destination field.
    Operand scalar(unsigned c, std::uint8_t dwords) {
        auto c16 = static_cast<std::uint16_t>(c);
        if (c == code::literal) {
            return {OperandKind::literal, dwords, c16, literal()};
        }
        if (c >= 128 && c <= 208) {
            const std::uint32_t value = c <= 192 ? c - 128 : 192 - c;  // 0 to 64, -1 to -16
            return {OperandKind::constant, dwords, c16, value};
        }
        if (c >= 240 && c <= 247) {
            return {OperandKind::constant, dwords, c16, float_constant(c, dwords)};
        }
        if (!valid_register(c, dwords)) {
            fail(std::format("invalid {}-dword scalar operand {}", dwords, c));
        }
        return {OperandKind::scalar, dwords, c16, 0};
    }

private:
    Operand imm(OperandKind kind, std::uint32_t value) { return {kind, 0, 0, value}; }

    Operand branch(std::uint32_t simm16) {
        auto offset = static_cast<std::int16_t>(simm16);
        if (static_cast<std::int64_t>(inst_.offset) + 4 + 4 * offset < 0) {
            fail("branch target before the start of the code");
        }
        return imm(OperandKind::branch, static_cast<std::uint32_t>(std::int32_t{offset}));
    }

    // SOP2, SOP1 and SOPC.
    void sop_operands(unsigned sdst, unsigned ssrc0, unsigned ssrc1) {
        if (info_->dst) {
            add_dst(scalar(sdst, info_->dst));
        }
        if (info_->src0) {
            add_src(scalar(ssrc0, info_->src0));
        }
        if (info_->src1) {
            add_src(scalar(ssrc1, info_->src1));
        }
    }

    void sopk_operands(unsigned sdst, std::uint32_t simm16) {
        switch (info_->imm) {
        case Imm::simm:
            if (info_->dst) {
                add_dst(scalar(sdst, info_->dst));
            } else {
                add_src(scalar(sdst, info_->src0));
            }
            add_src(imm(OperandKind::simm, simm16));
            break;
        case Imm::branch:
            add_src(scalar(sdst, info_->src0));
            add_src(branch(simm16));
            break;
        case Imm::hwreg:
            add_dst(scalar(sdst, info_->dst));
            add_src(imm(OperandKind::hwreg, simm16));
            break;
        case Imm::setreg:
            add_src(imm(OperandKind::hwreg, simm16));
            add_src(scalar(sdst, info_->src0));
            break;
        case Imm::setreg_imm32:
            add_src(imm(OperandKind::hwreg, simm16));
            add_src({OperandKind::literal, 1, code::literal, literal()});
            break;
        default:
            fail("SOPK opcode without an immediate");  // not in the table
        }
    }

    // SOPP.
    void immediate(std::uint32_t simm16) {
        switch (info_->imm) {
        case Imm::none:
            break;
        case Imm::count:
            add_src(imm(OperandKind::count, simm16));
            break;
        case Imm::branch:
            add_src(branch(simm16));
            break;
        case Imm::waitcnt:
            add_src(imm(OperandKind::waitcnt, simm16));
            break;
        case Imm::sendmsg:
            add_src(imm(OperandKind::sendmsg, simm16));
            break;
        default:
            fail("SOPP opcode with a SOPK immediate");  // not in the table
        }
    }

    void smrd_operands() {
        if (info_->dst) {
            add_dst(scalar(bits(word_, 15, 7), info_->dst));
        }
        if (info_->src0) {
            add_src(scalar(bits(word_, 9, 6) * 2, info_->src0));
        }
        if (info_->src1) {
            const unsigned offset = bits(word_, 0, 8);
            if (bits(word_, 8, 1)) {
                add_src(imm(OperandKind::offset, offset));
            } else if (offset >= 128 && offset != code::literal) {
                fail(std::format("invalid SMRD offset register {}", offset));
            } else {
                add_src(scalar(offset, 1));  // an SGPR, or a literal (CI)
            }
        }
    }

    std::span<const std::uint32_t> code_;
    std::uint32_t word_;
    std::size_t index_;
    const OpInfo* info_ = nullptr;
    Instruction inst_;
};

}  // namespace

Operand detail::scalar_operand(std::span<const std::uint32_t> code, std::size_t index, unsigned c,
                               std::uint8_t dwords) {
    return Decoder(code, index).scalar(c, dwords);
}

std::vector<Instruction> decode(std::span<const std::uint32_t> code, Until until) {
    std::vector<Instruction> out;
    std::uint32_t furthest = 0;  // furthest branch target so far
    std::size_t index = 0;
    while (index < code.size()) {
        out.push_back(Decoder(code, index).run());
        const Instruction& inst = out.back();
        index += inst.size;
        if (auto target = branch_target(inst)) {
            furthest = std::max(furthest, *target);
        }
        if (until == Until::program_end && is_endpgm(inst) && inst.offset >= furthest) {
            return out;
        }
    }
    if (until == Until::program_end) {
        throw GcnError("no s_endpgm at or beyond the furthest branch target",
                       static_cast<std::uint32_t>(code.size() * 4), 0);
    }
    return out;
}

std::optional<std::uint32_t> branch_target(const Instruction& inst) {
    for (unsigned i = 0; i < inst.num_src; ++i) {
        if (inst.src[i].kind == OperandKind::branch) {
            return inst.offset + 4 * inst.size + 4 * inst.src[i].value;  // value is signed
        }
    }
    return std::nullopt;
}

bool is_endpgm(const Instruction& inst) {
    return inst.format == Format::sopp && inst.opcode == 1;
}

}  // namespace bb::gcn
