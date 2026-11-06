// MUBUF (Sea Islands ISA manual, "Microcode Formats"): its opcode table, written from the opcode
// list with regular families built in loops, the decoder, and the fields in LLVM's gfx7 syntax.
// Bits that the manual marks as reserved are ignored.

#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gpu/gcn/memory.hpp"

namespace bb::gcn::detail {

namespace {

enum class MemKind : std::uint8_t {
    none,    // no operands: the cache invalidations
    load,    // vdata is the destination
    store,   // vdata is a source
    atomic,  // vdata is a source; with glc the old value returns into its first registers
};

// One opcode.
struct MemOp {
    std::string name;  // empty for opcodes the format doesn't define
    MemKind kind = MemKind::none;
    std::uint8_t data = 0;  // VGPRs of vdata, without the one tfe adds to a load
};

using Table = std::vector<MemOp>;

void set(Table& t, unsigned op, std::string name, MemKind kind, std::uint8_t data) {
    if (t.size() <= op) {
        t.resize(op + 1);
    }
    t[op] = {std::move(name), kind, data};
}

constexpr std::array<const char*, 4> components = {"x", "xy", "xyz", "xyzw"};

// The atomics in opcode order, from 48 and their 64-bit forms from 80.
constexpr std::array<const char*, 17> atomics = {
    "swap", "cmpswap", "add", "sub", "rsub", "smin", "umin", "smax", "umax",
    "and",  "or",      "xor", "inc", "dec",  "fcmpswap", "fmin", "fmax"};
constexpr unsigned rsub = 4, cmpswap = 1, fcmpswap = 14;

Table make_mubuf() {
    Table t;
    for (unsigned i = 0; i < components.size(); ++i) {
        const auto n = static_cast<std::uint8_t>(i + 1);
        set(t, i, std::string("buffer_load_format_") + components[i], MemKind::load, n);
        set(t, 4 + i, std::string("buffer_store_format_") + components[i], MemKind::store, n);
    }
    constexpr std::array<const char*, 4> narrow = {"ubyte", "sbyte", "ushort", "sshort"};
    for (unsigned i = 0; i < narrow.size(); ++i) {
        set(t, 8 + i, std::string("buffer_load_") + narrow[i], MemKind::load, 1);
    }
    set(t, 12, "buffer_load_dword", MemKind::load, 1);
    set(t, 13, "buffer_load_dwordx2", MemKind::load, 2);
    set(t, 14, "buffer_load_dwordx4", MemKind::load, 4);
    set(t, 15, "buffer_load_dwordx3", MemKind::load, 3);  // CI
    set(t, 24, "buffer_store_byte", MemKind::store, 1);
    set(t, 26, "buffer_store_short", MemKind::store, 1);
    set(t, 28, "buffer_store_dword", MemKind::store, 1);
    set(t, 29, "buffer_store_dwordx2", MemKind::store, 2);
    set(t, 30, "buffer_store_dwordx4", MemKind::store, 4);
    set(t, 31, "buffer_store_dwordx3", MemKind::store, 3);  // CI
    for (unsigned i = 0; i < atomics.size(); ++i) {
        if (i == rsub) {
            continue;  // llvm-mc rejects buffer_atomic_rsub for gfx7
        }
        const std::uint8_t n = i == cmpswap || i == fcmpswap ? 2 : 1;
        set(t, 48 + i, std::string("buffer_atomic_") + atomics[i], MemKind::atomic, n);
        set(t, 80 + i, std::string("buffer_atomic_") + atomics[i] + "_x2", MemKind::atomic,
            static_cast<std::uint8_t>(2 * n));
    }
    set(t, 112, "buffer_wbinvl1_vol", MemKind::none, 0);  // CI
    set(t, 113, "buffer_wbinvl1", MemKind::none, 0);
    return t;
}

const Table& mubuf_table() {
    static const Table table = make_mubuf();
    return table;
}

// Builds the operands of one instruction. Errors name its offset and first word.
class Operands {
public:
    Operands(std::uint32_t offset, std::uint32_t word) : offset_(offset), word_(word) {}

    [[noreturn]] void fail(const std::string& message) const {
        throw GcnError(message, offset_, word_);
    }

    Operand vgpr(unsigned first, unsigned dwords) const {
        if (first + dwords > 256) {
            fail(std::format("VGPR range v{} with {} dwords past v255", first, dwords));
        }
        return {OperandKind::vector, static_cast<std::uint8_t>(dwords),
                static_cast<std::uint16_t>(first), 0};
    }

    // A resource descriptor: the field holds the first SGPR divided by 4.
    Operand descriptor(unsigned field, unsigned dwords) const {
        const unsigned c = field * 4;
        if (!valid_register(c, dwords)) {
            fail(std::format("invalid {}-dword descriptor operand {}", dwords, c));
        }
        return {OperandKind::scalar, static_cast<std::uint8_t>(dwords),
                static_cast<std::uint16_t>(c), 0};
    }

    // SOFFSET: an SGPR, a special register or an inline constant. There is no literal.
    Operand soffset(unsigned c) const {
        const auto c16 = static_cast<std::uint16_t>(c);
        if (c >= 128 && c <= 208) {
            return {OperandKind::constant, 1, c16, c <= 192 ? c - 128 : 192 - c};
        }
        if (c >= 240 && c <= 247) {
            return {OperandKind::constant, 1, c16, float_constant(c, 1)};
        }
        if (!valid_register(c, 1)) {
            fail(std::format("invalid soffset operand {}", c));
        }
        return {OperandKind::scalar, 1, c16, 0};
    }

private:
    std::uint32_t offset_;
    std::uint32_t word_;
};

}  // namespace

bool is_memory_encoding(std::uint32_t word) {
    return bits(word, 26, 6) == 0x38;  // MUBUF
}

Instruction decode_memory(std::span<const std::uint32_t> code, std::size_t index) {
    const std::uint32_t w0 = code[index];
    Instruction inst;
    inst.offset = static_cast<std::uint32_t>(index * 4);
    inst.size = 2;
    inst.format = Format::mubuf;
    inst.opcode = static_cast<std::uint16_t>(bits(w0, 18, 7));
    const Operands ops(inst.offset, w0);
    const Table& table = mubuf_table();
    if (inst.opcode >= table.size() || table[inst.opcode].name.empty()) {
        ops.fail(std::format("unknown MUBUF opcode {}", inst.opcode));
    }
    const MemOp& info = table[inst.opcode];
    inst.name = info.name;
    if (index + 1 >= code.size()) {
        ops.fail("second dword past the end of the code");
    }
    const std::uint32_t w1 = code[index + 1];

    inst.mem_offset = static_cast<std::uint16_t>(bits(w0, 0, 12));
    const auto set_if = [&](std::uint32_t bit, std::uint16_t f) {
        if (bit != 0) {
            inst.flags |= f;
        }
    };
    set_if(bits(w0, 12, 1), flag::offen);
    set_if(bits(w0, 13, 1), flag::idxen);
    set_if(bits(w0, 14, 1), flag::glc);
    set_if(bits(w0, 15, 1), flag::addr64);
    set_if(bits(w0, 16, 1), flag::lds);
    set_if(bits(w1, 22, 1), flag::slc);
    set_if(bits(w1, 23, 1), flag::tfe);
    const bool lds = inst.flags & flag::lds, tfe = inst.flags & flag::tfe;
    if (info.kind == MemKind::none) {
        return inst;
    }
    const bool load = info.kind == MemKind::load;
    if (lds && !load) {
        ops.fail("lds bit on a store or atomic");
    }
    if (!load) {
        inst.src[inst.num_src++] = ops.vgpr(bits(w1, 8, 8), info.data);
    } else if (!lds) {  // a load into LDS doesn't use vdata, so its field isn't checked
        inst.dst[inst.num_dst++] = ops.vgpr(bits(w1, 8, 8), info.data + (tfe ? 1 : 0));
    }
    // The address: an index, an offset, both (index first), or a 64-bit address.
    const unsigned addr = inst.flags & flag::addr64
                              ? 2
                              : unsigned{(inst.flags & flag::idxen) != 0} +
                                    unsigned{(inst.flags & flag::offen) != 0};
    inst.src[inst.num_src++] =
        addr ? ops.vgpr(bits(w1, 0, 8), addr) : Operand{OperandKind::off, 0, 0, 0};
    inst.src[inst.num_src++] = ops.descriptor(bits(w1, 16, 5), 4);
    inst.src[inst.num_src++] = ops.soffset(bits(w1, 24, 8));
    return inst;
}

std::string memory_fields(const Instruction& inst) {
    std::string out;
    if (inst.num_src == 0) {
        return out;  // the cache invalidations have no fields
    }
    auto field = [&](std::uint16_t f, std::string_view name) {
        if (inst.flags & f) {
            out += ' ';
            out += name;
        }
    };
    field(flag::idxen, "idxen");
    field(flag::offen, "offen");
    field(flag::addr64, "addr64");
    if (inst.mem_offset != 0) {
        out += std::format(" offset:{}", inst.mem_offset);
    }
    field(flag::glc, "glc");
    field(flag::slc, "slc");
    field(flag::lds, "lds");
    field(flag::tfe, "tfe");
    return out;
}

}  // namespace bb::gcn::detail
