// The DS (LDS and GDS) and FLAT formats: opcode tables, decoding and modifier text, from the
// Sea Islands ISA manual (chapter "Microcode Formats"). Regular families are built in loops.

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/opcodes.hpp"

namespace bb::gcn::detail {

namespace {

constexpr unsigned ds_encoding = 0x36;    // bits 31-26
constexpr unsigned flat_encoding = 0x37;  // bits 31-26 (CI)

std::uint32_t bits(std::uint32_t word, unsigned low, unsigned count) {
    return (word >> low) & ((1u << count) - 1);
}

// How a DS instruction uses its offset field.
enum class DsOffset : std::uint8_t {
    none,     // ds_nop
    single,   // offset:N, the 16-bit field
    pair,     // offset0:N offset1:M, the two bytes (read2, write2, wrxchg2)
    swizzle,  // ds_swizzle_b32: the pattern, printed in hex
};

// One DS opcode. The register sizes are in dwords, 0 when the field is unused. `addr` is the
// ADDR field: the address, or the value of ds_gws_init, ds_gws_sema_br and ds_gws_barrier,
// which LLVM encodes there.
struct DsOp {
    std::string name;  // empty for opcodes the format doesn't define
    std::uint8_t dst = 0;
    std::uint8_t addr = 0;
    std::uint8_t data0 = 0;
    std::uint8_t data1 = 0;
    DsOffset offset = DsOffset::single;
};

// One FLAT opcode. The address is always a VGPR pair. An atomic returns `data` dwords into VDST
// (half of them for the compare-swaps) when GLC is set.
struct FlatOp {
    std::string name;
    std::uint8_t dst = 0;
    std::uint8_t data = 0;
    bool atomic = false;
};

template <typename Op>
void set(std::vector<Op>& t, unsigned op, Op row) {
    if (t.size() <= op) {
        t.resize(op + 1);
    }
    t[op] = std::move(row);
}

// The DS operations at 0-19 and their copies: with return at +32, on 64 bits at +64 and +96, and
// the src2 forms at +128 and +192. The type letter goes before the size.
struct DsFamily {
    const char* stem;
    char type;
    const char* rtn_stem;  // name of the form with return, when it isn't stem + "_rtn"
    std::uint8_t data;     // data operands: 1, or 2 (data0 and data1)
    bool src2;             // has a src2 form
};

constexpr std::array<DsFamily, 20> ds_families = {{
    {"add", 'u', nullptr, 1, true},     {"sub", 'u', nullptr, 1, true},
    {"rsub", 'u', nullptr, 1, true},    {"inc", 'u', nullptr, 1, true},
    {"dec", 'u', nullptr, 1, true},     {"min", 'i', nullptr, 1, true},
    {"max", 'i', nullptr, 1, true},     {"min", 'u', nullptr, 1, true},
    {"max", 'u', nullptr, 1, true},     {"and", 'b', nullptr, 1, true},
    {"or", 'b', nullptr, 1, true},      {"xor", 'b', nullptr, 1, true},
    {"mskor", 'b', nullptr, 2, false},  {"write", 'b', "wrxchg_rtn", 1, true},
    {"write2", 'b', "wrxchg2_rtn", 2, false},
    {"write2st64", 'b', "wrxchg2st64_rtn", 2, false},
    {"cmpst", 'b', nullptr, 2, false},  {"cmpst", 'f', nullptr, 2, false},
    {"min", 'f', nullptr, 1, true},     {"max", 'f', nullptr, 1, true},
}};

std::vector<DsOp> make_ds() {
    std::vector<DsOp> t;
    for (unsigned i = 0; i < ds_families.size(); ++i) {
        const DsFamily& f = ds_families[i];
        const bool two_address = i == 14 || i == 15;
        const DsOffset offset = two_address ? DsOffset::pair : DsOffset::single;
        for (std::uint8_t size : {1, 2}) {
            const std::string type = std::string(1, f.type) + (size == 1 ? "32" : "64");
            const unsigned base = size == 1 ? 0 : 64;
            const auto data1 = static_cast<std::uint8_t>(f.data == 2 ? size : 0);
            const std::string rtn = f.rtn_stem ? f.rtn_stem : std::string(f.stem) + "_rtn";
            const auto rtn_dst = static_cast<std::uint8_t>(two_address ? 2 * size : size);
            set(t, base + i, {std::format("ds_{}_{}", f.stem, type), 0, 1, size, data1, offset});
            set(t, base + 32 + i, {std::format("ds_{}_{}", rtn, type), rtn_dst, 1, size, data1,
                                   offset});
            if (f.src2) {
                set(t, base + 128 + i, {std::format("ds_{}_src2_{}", f.stem, type), 0, 1});
            }
        }
    }
    set(t, 20, {"ds_nop", 0, 0, 0, 0, DsOffset::none});
    constexpr std::array<std::pair<const char*, std::uint8_t>, 6> gws = {{
        {"sema_release_all", 0}, {"init", 1}, {"sema_v", 0}, {"sema_br", 1}, {"sema_p", 0},
        {"barrier", 1}}};
    for (unsigned i = 0; i < gws.size(); ++i) {
        set(t, 24 + i, {std::string("ds_gws_") + gws[i].first, 0, gws[i].second});
    }
    set(t, 30, {"ds_write_b8", 0, 1, 1});
    set(t, 31, {"ds_write_b16", 0, 1, 1});
    set(t, 52, {"ds_wrap_rtn_b32", 1, 1, 1, 1});
    set(t, 53, {"ds_swizzle_b32", 1, 1, 0, 0, DsOffset::swizzle});
    set(t, 54, {"ds_read_b32", 1, 1});
    set(t, 55, {"ds_read2_b32", 2, 1, 0, 0, DsOffset::pair});
    set(t, 56, {"ds_read2st64_b32", 2, 1, 0, 0, DsOffset::pair});
    constexpr std::array<const char*, 4> narrow = {"i8", "u8", "i16", "u16"};
    for (unsigned i = 0; i < narrow.size(); ++i) {
        set(t, 57 + i, {std::string("ds_read_") + narrow[i], 1, 1});
    }
    set(t, 61, {"ds_consume", 1, 0});
    set(t, 62, {"ds_append", 1, 0});
    set(t, 63, {"ds_ordered_count", 1, 1});
    set(t, 118, {"ds_read_b64", 2, 1});
    set(t, 119, {"ds_read2_b64", 4, 1, 0, 0, DsOffset::pair});
    set(t, 120, {"ds_read2st64_b64", 4, 1, 0, 0, DsOffset::pair});
    set(t, 126, {"ds_condxchg32_rtn_b64", 2, 1, 2});
    set(t, 222, {"ds_write_b96", 0, 1, 3});
    set(t, 223, {"ds_write_b128", 0, 1, 4});
    set(t, 253, {"ds_condxchg32_rtn_b128", 4, 1, 4});
    set(t, 254, {"ds_read_b96", 3, 1});
    set(t, 255, {"ds_read_b128", 4, 1});
    return t;
}

std::vector<FlatOp> make_flat() {
    std::vector<FlatOp> t;
    // Loads at 8-15 and stores at 24-31, with the 3-dword forms after the 4-dword ones.
    constexpr std::array<std::pair<const char*, std::uint8_t>, 8> loads = {{
        {"ubyte", 1}, {"sbyte", 1}, {"ushort", 1}, {"sshort", 1},
        {"dword", 1}, {"dwordx2", 2}, {"dwordx4", 4}, {"dwordx3", 3}}};
    for (unsigned i = 0; i < loads.size(); ++i) {
        set(t, 8 + i, {std::string("flat_load_") + loads[i].first, loads[i].second, 0});
    }
    constexpr std::array<std::pair<unsigned, const char*>, 2> narrow = {{{24, "byte"},
                                                                          {26, "short"}}};
    for (auto [op, name] : narrow) {
        set(t, op, {std::string("flat_store_") + name, 0, 1});
    }
    for (unsigned i = 4; i < loads.size(); ++i) {
        set(t, 24 + i, {std::string("flat_store_") + loads[i].first, 0, loads[i].second});
    }
    // Atomics at 48-64, and their 64-bit forms at 80-96. 52 and 84 (rsub) are left out because
    // LLVM rejects flat_atomic_rsub for gfx7.
    constexpr std::array<const char*, 17> atomics = {
        "swap", "cmpswap", "add", "sub", nullptr, "smin", "umin", "smax", "umax",
        "and",  "or",      "xor", "inc", "dec",   "fcmpswap", "fmin", "fmax"};
    for (unsigned i = 0; i < atomics.size(); ++i) {
        if (atomics[i] == nullptr) {
            continue;
        }
        const bool cmpswap = i == 1 || i == 14;
        for (std::uint8_t size : {1, 2}) {
            const auto data = static_cast<std::uint8_t>(cmpswap ? 2 * size : size);
            set(t, (size == 1 ? 48 : 80) + i,
                {std::format("flat_atomic_{}{}", atomics[i], size == 1 ? "" : "_x2"), size, data,
                 true});
        }
    }
    return t;
}

const std::vector<DsOp>& ds_table() {
    static const std::vector<DsOp> t = make_ds();
    return t;
}

const std::vector<FlatOp>& flat_table() {
    static const std::vector<FlatOp> t = make_flat();
    return t;
}

[[noreturn]] void fail(const Instruction& inst, std::uint32_t word, const std::string& message) {
    throw GcnError(message, inst.offset, word);
}

// The VGPR range at `first`. Unlike SGPRs, VGPR ranges need no alignment.
Operand vgpr(const Instruction& inst, std::uint32_t word, unsigned first, unsigned dwords) {
    if (first + dwords > 256) {
        fail(inst, word, std::format("invalid {}-dword vector register {}", dwords, first));
    }
    return {OperandKind::vector, static_cast<std::uint8_t>(dwords),
            static_cast<std::uint16_t>(first), 0};
}

void decode_ds(Instruction& inst, std::uint32_t w0, std::uint32_t w1) {
    const auto& table = ds_table();
    const unsigned op = bits(w0, 18, 8);
    if (op >= table.size() || table[op].name.empty()) {
        fail(inst, w0, std::format("unknown DS opcode {}", op));
    }
    const DsOp& info = table[op];
    inst.format = Format::ds;
    inst.opcode = static_cast<std::uint16_t>(op);
    inst.name = info.name;
    inst.mem_offset = static_cast<std::uint16_t>(bits(w0, 0, 16));
    inst.flags = bits(w0, 17, 1) ? flag::gds : 0;
    if (info.dst) {
        inst.dst[inst.num_dst++] = vgpr(inst, w0, bits(w1, 24, 8), info.dst);
    }
    // ADDR, DATA0 and DATA1, in that order.
    const std::array<std::pair<unsigned, std::uint8_t>, 3> sources = {{
        {bits(w1, 0, 8), info.addr}, {bits(w1, 8, 8), info.data0}, {bits(w1, 16, 8), info.data1}}};
    for (auto [first, dwords] : sources) {
        if (dwords) {
            inst.src[inst.num_src++] = vgpr(inst, w0, first, dwords);
        }
    }
}

void decode_flat(Instruction& inst, std::uint32_t w0, std::uint32_t w1) {
    const auto& table = flat_table();
    const unsigned op = bits(w0, 18, 7);
    if (op >= table.size() || table[op].name.empty()) {
        fail(inst, w0, std::format("unknown FLAT opcode {}", op));
    }
    const FlatOp& info = table[op];
    inst.format = Format::flat;
    inst.opcode = static_cast<std::uint16_t>(op);
    inst.name = info.name;
    inst.flags = static_cast<std::uint16_t>((bits(w0, 16, 1) ? flag::glc : 0) |
                                            (bits(w0, 17, 1) ? flag::slc : 0) |
                                            (bits(w1, 23, 1) ? flag::tfe : 0));
    // TFE writes one more dword after the result.
    const bool returns = !info.atomic || (inst.flags & flag::glc);
    if (info.dst && returns) {
        const unsigned dwords = info.dst + ((inst.flags & flag::tfe) ? 1 : 0);
        inst.dst[inst.num_dst++] = vgpr(inst, w0, bits(w1, 24, 8), dwords);
    }
    inst.src[inst.num_src++] = vgpr(inst, w0, bits(w1, 0, 8), 2);
    if (info.data) {
        inst.src[inst.num_src++] = vgpr(inst, w0, bits(w1, 8, 8), info.data);
    }
}

// The offset modifiers of a DS instruction, as LLVM prints them: offsets in decimal, left out
// when 0. LLVM prints the swizzle pattern in its swizzle() form, and accepts the number.
std::string ds_offset(const Instruction& inst) {
    const unsigned offset0 = inst.mem_offset & 0xff;
    const unsigned offset1 = inst.mem_offset >> 8;
    std::string out;
    switch (ds_table()[inst.opcode].offset) {
    case DsOffset::none:
        break;
    case DsOffset::single:
        if (inst.mem_offset != 0) {
            out += std::format(" offset:{}", inst.mem_offset);
        }
        break;
    case DsOffset::pair:
        if (offset0 != 0) {
            out += std::format(" offset0:{}", offset0);
        }
        if (offset1 != 0) {
            out += std::format(" offset1:{}", offset1);
        }
        break;
    case DsOffset::swizzle:
        if (inst.mem_offset != 0) {
            out += std::format(" offset:{:#x}", inst.mem_offset);
        }
        break;
    }
    return out;
}

}  // namespace

bool is_ds_flat(std::uint32_t word) {
    return bits(word, 26, 6) == ds_encoding || bits(word, 26, 6) == flat_encoding;
}

Instruction decode_ds_flat(std::span<const std::uint32_t> code, std::size_t index) {
    const std::uint32_t w0 = code[index];
    Instruction inst;
    inst.offset = static_cast<std::uint32_t>(index * 4);
    inst.size = 2;
    if (index + 1 >= code.size()) {
        fail(inst, w0, "instruction past the end of the code");
    }
    if (bits(w0, 26, 6) == ds_encoding) {
        decode_ds(inst, w0, code[index + 1]);
    } else {
        decode_flat(inst, w0, code[index + 1]);
    }
    return inst;
}

std::string ds_flat_modifiers(const Instruction& inst) {
    std::string out = inst.format == Format::ds ? ds_offset(inst) : "";
    constexpr std::array<std::pair<std::uint16_t, const char*>, 4> names = {
        {{flag::gds, " gds"}, {flag::glc, " glc"}, {flag::slc, " slc"}, {flag::tfe, " tfe"}}};
    for (auto [bit, name] : names) {
        if (inst.flags & bit) {
            out += name;
        }
    }
    return out;
}

}  // namespace bb::gcn::detail
