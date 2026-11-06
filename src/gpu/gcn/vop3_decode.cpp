// Decoder for VOP3 (Sea Islands ISA manual, "Microcode Formats").

#include <format>
#include <string>

#include "gpu/gcn/opcodes.hpp"
#include "gpu/gcn/vop.hpp"

namespace bb::gcn::detail {

namespace {

std::uint32_t bits(std::uint32_t word, unsigned low, unsigned count) {
    return (word >> low) & ((1u << count) - 1);
}

class Vop3Decoder {
public:
    Vop3Decoder(std::span<const std::uint32_t> code, std::size_t index)
        : code_(code), index_(index), word_(code[index]) {
        inst_.offset = static_cast<std::uint32_t>(index * 4);
        inst_.size = 2;
        inst_.format = Format::vop3;
    }

    Instruction run() {
        if (index_ + 1 >= code_.size()) {
            fail("VOP3 instruction past the end of the code");
        }
        const std::uint32_t w1 = code_[index_ + 1];
        const unsigned opcode = bits(word_, 17, 9);
        const auto& table = vop_opcodes(Format::vop3);
        if (opcode >= table.size() || table[opcode].name.empty()) {
            fail(std::format("unknown VOP3 opcode {:#x}", opcode));
        }
        const VopInfo& info = table[opcode];
        inst_.opcode = static_cast<std::uint16_t>(opcode);
        inst_.name = info.name;
        // VOP3b has an SGPR pair destination in bits 8-14 where VOP3a has abs and clamp.
        const bool vop3b = info.kind == Vop::carry_out || info.kind == Vop::carry ||
                           info.kind == Vop::ternary_sdst;
        if (bits(word_, vop3b ? 15 : 12, vop3b ? 2 : 5) != 0) {
            fail("reserved bits set");
        }
        // The modifiers (abs, clamp, omod, neg) are decoded in the next part of #118.
        const bool modifiers = bits(w1, 27, 5) != 0 || (!vop3b && bits(word_, 8, 4) != 0);
        if (modifiers && info.kind != Vop::none) {
            fail("VOP3 modifiers are not supported yet");
        }
        const unsigned vdst = bits(word_, 0, 8);
        const unsigned s0 = bits(w1, 0, 9);
        const unsigned s1 = bits(w1, 9, 9);
        const unsigned s2 = bits(w1, 18, 9);
        switch (info.kind) {
        case Vop::none:
            break;  // v_nop, v_clrexcp: the other fields are unused
        case Vop::sdst:
            add_dst(scalar_dst(vdst, 1));
            add_src(source(s0, info.src0, true));
            break;
        case Vop::plain:
            add_dst(vgpr(vdst, info.dst));
            add_src(source(s0, info.src0, true));
            break;
        case Vop::readlane:
        case Vop::writelane:
            add_dst(info.kind == Vop::readlane ? scalar_dst(vdst, 1) : vgpr(vdst, 1));
            add_src(source(s0, 1, true));
            add_src(source(s1, 1, false, true));
            break;
        case Vop::compare:
            add_dst(scalar_dst(vdst, 2));
            add_src(source(s0, info.src0, true));
            add_src(source(s1, info.src1, false));
            break;
        case Vop::cndmask:
            add_dst(vgpr(vdst, 1));
            add_src(source(s0, 1, true));
            add_src(source(s1, 1, false));
            add_src(source(s2, 2, false, true));
            break;
        case Vop::carry_out:
        case Vop::carry:
        case Vop::ternary_sdst:
            add_dst(vgpr(vdst, info.dst));
            add_dst(scalar_dst(bits(word_, 8, 7), 2));
            add_src(source(s0, info.src0, true));
            add_src(source(s1, info.src1, false));
            if (info.kind != Vop::carry_out) {
                const bool carry_in = info.kind == Vop::carry;
                add_src(source(s2, carry_in ? 2 : info.src2, false, carry_in));
            }
            break;
        case Vop::binary:
        case Vop::ternary:
            add_dst(vgpr(vdst, info.dst));
            add_src(source(s0, info.src0, true));
            add_src(source(s1, info.src1, false));
            if (info.src2 != 0) {
                add_src(source(s2, info.src2, false));
            }
            break;
        default:
            fail("VOP3 opcode with a VOP2 operand layout");  // not in the table
        }
        return inst_;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw GcnError(message, inst_.offset, word_);
    }

    void add_dst(Operand op) { inst_.dst[inst_.num_dst++] = op; }
    void add_src(Operand op) { inst_.src[inst_.num_src++] = op; }

    Operand vgpr(unsigned n, std::uint8_t dwords) const {
        if (n + dwords > 256) {
            fail(std::format("invalid {}-dword vector register v{}", dwords, n));
        }
        return {OperandKind::vector, dwords, static_cast<std::uint16_t>(n), 0};
    }

    // An SGPR or special register destination (SGPR pairs for the mask results).
    Operand scalar_dst(unsigned c, std::uint8_t dwords) const {
        if (c >= 128) {
            fail(std::format("invalid scalar destination {}", c));
        }
        return scalar_operand(code_, index_, c, dwords);
    }

    // A 9-bit source: a VGPR, a scalar operand or a constant. Only src0 of a 32-bit operand may be
    // LDS_DIRECT, and VOP3 has no literal. `scalar_only` is for a mask, carry or lane select.
    Operand source(unsigned c, std::uint8_t dwords, bool first, bool scalar_only = false) const {
        if (c >= 256) {
            if (scalar_only) {
                fail("a vector register as a scalar operand");
            }
            return vgpr(c - 256, dwords);
        }
        if (c == code::literal) {
            fail("a literal in VOP3");
        }
        if (c == code::lds_direct && (!first || dwords != 1 || scalar_only)) {
            fail("LDS_DIRECT is valid only as a 32-bit src0");
        }
        return c == code::lds_direct ? Operand{OperandKind::scalar, 1, code::lds_direct, 0}
                                     : scalar_operand(code_, index_, c, dwords);
    }

    std::span<const std::uint32_t> code_;
    std::size_t index_;
    std::uint32_t word_;
    Instruction inst_;
};

}  // namespace

Instruction decode_vop3(std::span<const std::uint32_t> code, std::size_t index) {
    return Vop3Decoder(code, index).run();
}

}  // namespace bb::gcn::detail
