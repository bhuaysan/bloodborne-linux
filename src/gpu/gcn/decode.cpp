// Decoder for the scalar microcode formats (Sea Islands ISA manual, "Microcode Formats").

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>

#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/opcodes.hpp"

namespace bb::gcn {

GcnError::GcnError(const std::string& message, std::uint32_t offset, std::uint32_t word)
    : std::runtime_error(std::format("{} at offset {:#x} (word {:#010x})", message, offset, word)),
      offset_(offset),
      word_(word) {}

namespace {

using detail::Imm;
using detail::OpInfo;

std::uint32_t bits(std::uint32_t word, unsigned low, unsigned count) {
    return (word >> low) & ((1u << count) - 1);
}

// Decodes the instruction that starts at code[index].
class Decoder {
public:
    Decoder(std::span<const std::uint32_t> code, std::size_t index) : word_(code[index]) {
        inst_.offset = static_cast<std::uint32_t>(index * 4);
        inst_.size = 1;
    }

    Instruction run() {
        const std::uint32_t w = word_;
        if (bits(w, 23, 9) == 0x17f) {
            start(Format::sopp, bits(w, 16, 7), "SOPP");
            immediate(bits(w, 0, 16));
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

    void add_src(Operand op) { inst_.src[inst_.num_src++] = op; }

    Operand imm(OperandKind kind, std::uint32_t value) { return {kind, 0, 0, value}; }

    Operand branch(std::uint32_t simm16) {
        auto offset = static_cast<std::int16_t>(simm16);
        if (static_cast<std::int64_t>(inst_.offset) + 4 + 4 * offset < 0) {
            fail("branch target before the start of the code");
        }
        return imm(OperandKind::branch, static_cast<std::uint32_t>(std::int32_t{offset}));
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

    std::uint32_t word_;
    const OpInfo* info_ = nullptr;
    Instruction inst_;
};

}  // namespace

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
