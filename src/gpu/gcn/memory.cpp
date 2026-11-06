// MUBUF, MTBUF, MIMG and EXP (Sea Islands ISA manual, "Microcode Formats"): the opcode tables,
// written from the opcode lists with regular families built in loops, the decoders, and the fields
// in LLVM's gfx7 syntax. Bits that the manual marks as reserved are ignored.

#include <algorithm>
#include <array>
#include <bit>
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
    sample,  // MIMG: a load that also takes a sampler
    gather,  // MIMG: a sample that returns four registers, whatever dmask is
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

// MTBUF has only the typed loads and stores of 1 to 4 components.
Table make_mtbuf() {
    Table t;
    for (unsigned i = 0; i < components.size(); ++i) {
        const auto n = static_cast<std::uint8_t>(i + 1);
        set(t, i, std::string("tbuffer_load_format_") + components[i], MemKind::load, n);
        set(t, 4 + i, std::string("tbuffer_store_format_") + components[i], MemKind::store, n);
    }
    return t;
}

const Table& mtbuf_table() {
    static const Table table = make_mtbuf();
    return table;
}

// One MIMG opcode. `addr` is the shortest address LLVM accepts: the number of coordinates and
// parameters follows from the dimension of the resource, which the instruction doesn't name.
struct ImgOp {
    std::string name;  // empty for opcodes the format doesn't define
    MemKind kind = MemKind::load;
    std::uint8_t addr = 1;
};

using ImgTable = std::vector<ImgOp>;

void set(ImgTable& t, unsigned op, std::string name, MemKind kind, unsigned addr = 1) {
    if (t.size() <= op) {
        t.resize(op + 1);
    }
    t[op] = {std::move(name), kind, static_cast<std::uint8_t>(addr)};
}

ImgTable make_mimg() {
    ImgTable t;
    constexpr std::array<const char*, 6> loads = {"load",         "load_mip",
                                                  "load_pck",     "load_pck_sgn",
                                                  "load_mip_pck", "load_mip_pck_sgn"};
    for (unsigned i = 0; i < loads.size(); ++i) {
        set(t, i, std::string("image_") + loads[i], MemKind::load);
    }
    constexpr std::array<const char*, 4> stores = {"store", "store_mip", "store_pck",
                                                   "store_mip_pck"};
    for (unsigned i = 0; i < stores.size(); ++i) {
        set(t, 8 + i, std::string("image_") + stores[i], MemKind::store);
    }
    set(t, 14, "image_get_resinfo", MemKind::load);
    for (unsigned i = 0; i < atomics.size(); ++i) {
        set(t, 15 + i, std::string("image_atomic_") + atomics[i], MemKind::atomic);
    }
    // Sampling: 8 variants in the low 3 bits, a compare value (+8) and an offset (+16). The
    // derivatives and the bias each add an address dword; the LOD and clamp variants add none.
    constexpr std::array<const char*, 8> variants = {"", "_cl", "_d", "_d_cl", "_l", "_b", "_b_cl",
                                                     "_lz"};
    constexpr std::array<unsigned, 8> extra = {0, 0, 1, 1, 0, 1, 1, 0};
    for (unsigned o = 0; o < 2; ++o) {
        for (unsigned c = 0; c < 2; ++c) {
            for (unsigned m = 0; m < 8; ++m) {
                const std::string suffix =
                    std::string(c ? "_c" : "") + variants[m] + (o ? "_o" : "");
                const unsigned addr = 1 + c + o + extra[m];
                const unsigned op = o * 16 + c * 8 + m;
                set(t, 32 + op, "image_sample" + suffix, MemKind::sample, addr);
                if (m != 2 && m != 3) {  // gather4 has no derivatives
                    set(t, 64 + op, "image_gather4" + suffix, MemKind::gather, addr);
                }
            }
        }
    }
    set(t, 96, "image_get_lod", MemKind::sample);
    // The variants with derivatives per quad: compare value (+2), offset (+4) and clamp (+1).
    for (unsigned o = 0; o < 2; ++o) {
        for (unsigned c = 0; c < 2; ++c) {
            for (unsigned cl = 0; cl < 2; ++cl) {
                const std::string suffix =
                    std::string(c ? "_c" : "") + "_cd" + (cl ? "_cl" : "") + (o ? "_o" : "");
                set(t, 104 + o * 4 + c * 2 + cl, "image_sample" + suffix, MemKind::sample,
                    2 + c + o);
            }
        }
    }
    return t;
}

const ImgTable& mimg_table() {
    static const ImgTable table = make_mimg();
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
    const std::uint32_t encoding = bits(word, 26, 6);
    return encoding == 0x38 || encoding == 0x3a || encoding == 0x3c || encoding == 0x3e;
}

namespace {

// MUBUF and MTBUF share the layout except for the opcode field and the typed format.
Instruction decode_buffer(std::span<const std::uint32_t> code, std::size_t index, bool typed) {
    const std::uint32_t w0 = code[index];
    Instruction inst;
    inst.offset = static_cast<std::uint32_t>(index * 4);
    inst.size = 2;
    inst.format = typed ? Format::mtbuf : Format::mubuf;
    inst.opcode = static_cast<std::uint16_t>(typed ? bits(w0, 16, 3) : bits(w0, 18, 7));
    const Operands ops(inst.offset, w0);
    const Table& table = typed ? mtbuf_table() : mubuf_table();
    if (inst.opcode >= table.size() || table[inst.opcode].name.empty()) {
        ops.fail(std::format("unknown {} opcode {}", typed ? "MTBUF" : "MUBUF", inst.opcode));
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
    if (typed) {
        inst.dfmt = static_cast<std::uint8_t>(bits(w0, 19, 4));
        inst.nfmt = static_cast<std::uint8_t>(bits(w0, 23, 3));
    } else {
        set_if(bits(w0, 16, 1), flag::lds);
    }
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

// MIMG: vdata, vaddr, the image descriptor and, for sampling, the sampler. The dmask goes to
// mem_offset. vdata is a register per dmask bit (at least one) and a gather's four, with a status
// register more under tfe. The address is its shortest form: see ImgOp.
// The hardware also writes the status register under lwe alone, but LLVM's assembler counts only
// tfe in the range of vdata (dst for a load, sample or gather, src[0] for a store or atomic), so
// that range is kept for the round trip. The translator must treat an lwe instruction without tfe
// as writing one more VGPR after the range (graphics.md).
Instruction decode_image(std::span<const std::uint32_t> code, std::size_t index) {
    const std::uint32_t w0 = code[index];
    Instruction inst;
    inst.offset = static_cast<std::uint32_t>(index * 4);
    inst.size = 2;
    inst.format = Format::mimg;
    inst.opcode = static_cast<std::uint16_t>(bits(w0, 18, 7));
    const Operands ops(inst.offset, w0);
    const ImgTable& table = mimg_table();
    if (inst.opcode >= table.size() || table[inst.opcode].name.empty()) {
        ops.fail(std::format("unknown MIMG opcode {}", inst.opcode));
    }
    const ImgOp& info = table[inst.opcode];
    inst.name = info.name;
    if (index + 1 >= code.size()) {
        ops.fail("second dword past the end of the code");
    }
    const std::uint32_t w1 = code[index + 1];
    inst.mem_offset = static_cast<std::uint16_t>(bits(w0, 8, 4));
    const std::array<std::pair<unsigned, std::uint16_t>, 7> modes = {{
        {12, flag::unorm}, {13, flag::glc}, {14, flag::da},  {15, flag::r128},
        {16, flag::tfe},   {17, flag::lwe}, {25, flag::slc}}};
    for (const auto& [bit, f] : modes) {
        if (bits(w0, bit, 1)) {
            inst.flags |= f;
        }
    }
    const unsigned dmask = inst.mem_offset;
    unsigned data = info.kind == MemKind::gather ? 4 : std::max(1, std::popcount(dmask));
    data += inst.flags & flag::tfe ? 1 : 0;
    const Operand vdata = ops.vgpr(bits(w1, 8, 8), data);
    const bool is_source = info.kind == MemKind::store || info.kind == MemKind::atomic;
    (is_source ? inst.src[inst.num_src++] : inst.dst[inst.num_dst++]) = vdata;
    inst.src[inst.num_src++] = ops.vgpr(bits(w1, 0, 8), info.addr);
    // The resource is 8 SGPRs, also with r128, which then uses only the first 4.
    inst.src[inst.num_src++] = ops.descriptor(bits(w1, 16, 5), 8);
    if (info.kind == MemKind::sample || info.kind == MemKind::gather) {
        inst.src[inst.num_src++] = ops.descriptor(bits(w1, 21, 5), 4);
    }
    return inst;
}

// The export targets: mrt0-7 are 0-7, mrtz 8, null 9, pos0-3 12-15 and param0-31 32-63.
bool valid_target(unsigned t) {
    return t <= 9 || (t >= 12 && t <= 15) || t >= 32;
}

// EXP: the target in mem_offset, the four sources in src (a VGPR, or `off` where the enable bit
// is clear). With `compr` each VGPR holds two 16-bit values and en has a bit per value: the
// source at position N is then vsrc[N / 2].
Instruction decode_export(std::span<const std::uint32_t> code, std::size_t index) {
    const std::uint32_t w0 = code[index];
    Instruction inst;
    inst.offset = static_cast<std::uint32_t>(index * 4);
    inst.size = 2;
    inst.format = Format::exp;
    inst.name = "exp";
    const Operands ops(inst.offset, w0);
    if (index + 1 >= code.size()) {
        ops.fail("second dword past the end of the code");
    }
    const std::uint32_t w1 = code[index + 1];
    const unsigned target = bits(w0, 4, 6);
    if (!valid_target(target)) {
        ops.fail(std::format("reserved export target {}", target));
    }
    inst.mem_offset = static_cast<std::uint16_t>(target);
    const bool compr = bits(w0, 10, 1) != 0;
    if (bits(w0, 11, 1)) {
        inst.flags |= flag::done;
    }
    if (compr) {
        inst.flags |= flag::compr;
    }
    if (bits(w0, 12, 1)) {
        inst.flags |= flag::vm;
    }
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned vsrc = bits(w1, 8 * (compr ? i / 2 : i), 8);
        inst.src[inst.num_src++] =
            bits(w0, i, 1) ? ops.vgpr(vsrc, 1) : Operand{OperandKind::off, 0, 0, 0};
    }
    return inst;
}

}  // namespace

Instruction decode_memory(std::span<const std::uint32_t> code, std::size_t index) {
    switch (bits(code[index], 26, 6)) {
    case 0x3a:
        return decode_buffer(code, index, true);
    case 0x3c:
        return decode_image(code, index);
    case 0x3e:
        return decode_export(code, index);
    default:
        return decode_buffer(code, index, false);
    }
}

namespace {

constexpr std::array<const char*, 16> data_formats = {
    "INVALID",     "8",         "16",          "8_8",         "32",       "16_16",
    "10_11_11",    "11_11_10",  "10_10_10_2",  "2_10_10_10",  "8_8_8_8",  "32_32",
    "16_16_16_16", "32_32_32",  "32_32_32_32", "RESERVED_15"};
constexpr std::array<const char*, 8> num_formats = {"UNORM", "SNORM", "USCALED",   "SSCALED",
                                                    "UINT",  "SINT",  "SNORM_OGL", "FLOAT"};

// LLVM leaves out the data format 8 and the numeric format UNORM, and the whole field when both
// are left out.
std::string typed_format(const Instruction& inst) {
    constexpr unsigned default_dfmt = 1, default_nfmt = 0;
    std::string parts;
    if (inst.dfmt != default_dfmt) {
        parts += std::string("BUF_DATA_FORMAT_") + data_formats[inst.dfmt & 15];
    }
    if (inst.nfmt != default_nfmt) {
        parts += parts.empty() ? "" : ",";
        parts += std::string("BUF_NUM_FORMAT_") + num_formats[inst.nfmt & 7];
    }
    return parts.empty() ? parts : " format:[" + parts + "]";
}

// MIMG: dmask (left out when 0), then the flags in LLVM's order.
std::string image_fields(const Instruction& inst) {
    std::string out;
    if (inst.mem_offset != 0) {
        out += std::format(" dmask:{:#x}", inst.mem_offset);
    }
    constexpr std::array<std::pair<std::uint16_t, const char*>, 7> names = {{
        {flag::unorm, "unorm"}, {flag::glc, "glc"}, {flag::slc, "slc"}, {flag::r128, "r128"},
        {flag::tfe, "tfe"},     {flag::lwe, "lwe"}, {flag::da, "da"}}};
    for (const auto& [f, name] : names) {
        if (inst.flags & f) {
            out += ' ';
            out += name;
        }
    }
    return out;
}

}  // namespace

std::string memory_fields(const Instruction& inst) {
    std::string out;
    if (inst.format == Format::mimg) {
        return image_fields(inst);
    }
    if (inst.num_src == 0) {
        return out;  // the cache invalidations have no fields
    }
    if (inst.format == Format::mtbuf) {
        out += typed_format(inst);
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

// The whole line of an EXP instruction: the target comes before the sources.
std::string export_text(const Instruction& inst) {
    const unsigned t = inst.mem_offset;
    std::string out = "exp ";
    if (t <= 7) {
        out += std::format("mrt{}", t);
    } else if (t == 8) {
        out += "mrtz";
    } else if (t == 9) {
        out += "null";
    } else if (t <= 15) {
        out += std::format("pos{}", t - 12);
    } else {
        out += std::format("param{}", t - 32);
    }
    for (unsigned i = 0; i < inst.num_src; ++i) {
        out += i == 0 ? " " : ", ";
        out += inst.src[i].kind == OperandKind::off ? "off" : std::format("v{}", inst.src[i].code);
    }
    out += inst.flags & flag::done ? " done" : "";
    out += inst.flags & flag::compr ? " compr" : "";
    out += inst.flags & flag::vm ? " vm" : "";
    return out;
}

}  // namespace bb::gcn::detail
