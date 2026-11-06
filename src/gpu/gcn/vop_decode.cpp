// Decoder for VOP1 (Sea Islands ISA manual, "Microcode Formats").

#include <format>
#include <string>

#include "gpu/gcn/opcodes.hpp"
#include "gpu/gcn/vop.hpp"

namespace bb::gcn::detail {

namespace {

std::uint32_t bits(std::uint32_t word, unsigned low, unsigned count) {
    return (word >> low) & ((1u << count) - 1);
}

class VopDecoder {
public:
    VopDecoder(std::span<const std::uint32_t> code, std::size_t index)
        : code_(code), index_(index), word_(code[index]) {
        inst_.offset = static_cast<std::uint32_t>(index * 4);
        inst_.size = 1;
    }

    Instruction run() {
        const std::uint32_t w = word_;
        if (bits(w, 25, 7) == 0x3f) {
            start(Format::vop1, bits(w, 9, 8), "VOP1");
            operands(bits(w, 17, 8));
        } else {
            fail("unsupported encoding");  // VOP2, VOPC
        }
        return inst_;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw GcnError(message, inst_.offset, word_);
    }

    void start(Format format, unsigned opcode, const char* format_name) {
        const auto& table = vop_opcodes(format);
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

    Operand vgpr(unsigned n, std::uint8_t dwords) const {
        if (n + dwords > 256) {
            fail(std::format("invalid {}-dword vector register v{}", dwords, n));
        }
        return {OperandKind::vector, dwords, static_cast<std::uint16_t>(n), 0};
    }

    // An SGPR, special register, constant or literal from an 8-bit field, or the 9-bit src0.
    Operand scalar(unsigned c, std::uint8_t dwords) {
        const Operand op = scalar_operand(code_, index_, c, dwords);
        if (op.kind == OperandKind::literal) {
            inst_.size = 2;
        }
        return op;
    }

    Operand src0(std::uint8_t dwords) {
        const unsigned c = bits(word_, 0, 9);
        return c >= 256 ? vgpr(c - 256, dwords) : scalar(c, dwords);
    }

    void operands(unsigned vdst) {
        const VopInfo& info = *info_;
        switch (info.kind) {
        case Vop::none:
            return;
        case Vop::sdst:
            if (vdst >= 128) {
                fail(std::format("invalid scalar destination {}", vdst));
            }
            add_dst(scalar(vdst, 1));
            add_src(src0(1));
            return;
        case Vop::plain:
            add_dst(vgpr(vdst, info.dst));
            add_src(src0(info.src0));
            return;
        }
    }

    std::span<const std::uint32_t> code_;
    std::size_t index_;
    std::uint32_t word_;
    const VopInfo* info_ = nullptr;
    Instruction inst_;
};

}  // namespace

Instruction decode_vop(std::span<const std::uint32_t> code, std::size_t index) {
    return VopDecoder(code, index).run();
}

}  // namespace bb::gcn::detail
