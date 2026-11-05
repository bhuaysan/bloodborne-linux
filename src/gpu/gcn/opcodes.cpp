// Opcode tables of the scalar formats, from the opcode lists of the Sea Islands ISA manual
// (chapter "Microcode Formats"). Regular families are built in loops.

#include "gpu/gcn/opcodes.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace bb::gcn::detail {

namespace {

using Table = std::vector<OpInfo>;

void set(Table& t, unsigned op, std::string name, std::uint8_t dst, std::uint8_t src0,
         std::uint8_t src1 = 0, Imm imm = Imm::none) {
    if (t.size() <= op) {
        t.resize(op + 1);
    }
    t[op] = {std::move(name), dst, src0, src1, imm};
}

constexpr std::array<const char*, 8> bitwise = {"and", "or",  "xor", "andn2",
                                                "orn2", "nand", "nor", "xnor"};
constexpr std::array<const char*, 6> conditions = {"eq", "lg", "gt", "ge", "lt", "le"};
constexpr std::array<const char*, 2> compare_types = {"i32", "u32"};

Table make_sop2() {
    Table t;
    constexpr std::array<const char*, 10> arith = {
        "s_add_u32", "s_sub_u32", "s_add_i32", "s_sub_i32", "s_addc_u32",
        "s_subb_u32", "s_min_i32", "s_min_u32", "s_max_i32", "s_max_u32"};
    for (unsigned i = 0; i < arith.size(); ++i) {
        set(t, i, arith[i], 1, 1, 1);
    }
    set(t, 10, "s_cselect_b32", 1, 1, 1);
    set(t, 11, "s_cselect_b64", 2, 2, 2);
    for (unsigned i = 0; i < bitwise.size(); ++i) {
        set(t, 14 + 2 * i, std::string("s_") + bitwise[i] + "_b32", 1, 1, 1);
        set(t, 15 + 2 * i, std::string("s_") + bitwise[i] + "_b64", 2, 2, 2);
    }
    // Shifts take a 32-bit shift amount also in their 64-bit forms.
    set(t, 30, "s_lshl_b32", 1, 1, 1);
    set(t, 31, "s_lshl_b64", 2, 2, 1);
    set(t, 32, "s_lshr_b32", 1, 1, 1);
    set(t, 33, "s_lshr_b64", 2, 2, 1);
    set(t, 34, "s_ashr_i32", 1, 1, 1);
    set(t, 35, "s_ashr_i64", 2, 2, 1);
    set(t, 36, "s_bfm_b32", 1, 1, 1);
    set(t, 37, "s_bfm_b64", 2, 1, 1);
    set(t, 38, "s_mul_i32", 1, 1, 1);
    set(t, 39, "s_bfe_u32", 1, 1, 1);
    set(t, 40, "s_bfe_i32", 1, 1, 1);
    set(t, 41, "s_bfe_u64", 2, 2, 1);
    set(t, 42, "s_bfe_i64", 2, 2, 1);
    set(t, 43, "s_cbranch_g_fork", 0, 2, 2);
    set(t, 44, "s_absdiff_i32", 1, 1, 1);
    return t;
}

Table make_sopk() {
    Table t;
    set(t, 0, "s_movk_i32", 1, 0, 0, Imm::simm);
    set(t, 2, "s_cmovk_i32", 1, 0, 0, Imm::simm);
    // s_cmpk_*: the sdst field is the compared register.
    for (unsigned type = 0; type < compare_types.size(); ++type) {
        for (unsigned c = 0; c < conditions.size(); ++c) {
            set(t, 3 + 6 * type + c,
                std::string("s_cmpk_") + conditions[c] + "_" + compare_types[type], 0, 1, 0,
                Imm::simm);
        }
    }
    set(t, 15, "s_addk_i32", 1, 0, 0, Imm::simm);
    set(t, 16, "s_mulk_i32", 1, 0, 0, Imm::simm);
    set(t, 17, "s_cbranch_i_fork", 0, 2, 0, Imm::branch);
    set(t, 18, "s_getreg_b32", 1, 0, 0, Imm::hwreg);
    set(t, 19, "s_setreg_b32", 0, 1, 0, Imm::setreg);
    set(t, 20, "s_getreg_regrd_b32", 1, 0, 0, Imm::hwreg);
    set(t, 21, "s_setreg_imm32_b32", 0, 0, 0, Imm::setreg_imm32);
    return t;
}

Table make_sop1() {
    Table t;
    constexpr std::array<const char*, 5> moves = {"mov", "cmov", "not", "wqm", "brev"};
    for (unsigned i = 0; i < moves.size(); ++i) {
        set(t, 3 + 2 * i, std::string("s_") + moves[i] + "_b32", 1, 1);
        set(t, 4 + 2 * i, std::string("s_") + moves[i] + "_b64", 2, 2);
    }
    constexpr std::array<const char*, 5> counts = {"bcnt0", "bcnt1", "ff0", "ff1", "flbit"};
    for (unsigned i = 0; i < counts.size(); ++i) {
        set(t, 13 + 2 * i, std::string("s_") + counts[i] + "_i32_b32", 1, 1);
        set(t, 14 + 2 * i, std::string("s_") + counts[i] + "_i32_b64", 1, 2);
    }
    set(t, 23, "s_flbit_i32", 1, 1);
    set(t, 24, "s_flbit_i32_i64", 1, 2);
    set(t, 25, "s_sext_i32_i8", 1, 1);
    set(t, 26, "s_sext_i32_i16", 1, 1);
    set(t, 27, "s_bitset0_b32", 1, 1);
    set(t, 28, "s_bitset0_b64", 2, 1);
    set(t, 29, "s_bitset1_b32", 1, 1);
    set(t, 30, "s_bitset1_b64", 2, 1);
    set(t, 31, "s_getpc_b64", 2, 0);
    set(t, 32, "s_setpc_b64", 0, 2);
    set(t, 33, "s_swappc_b64", 2, 2);
    set(t, 34, "s_rfe_b64", 0, 2);
    for (unsigned i = 0; i < bitwise.size(); ++i) {
        set(t, 36 + i, std::string("s_") + bitwise[i] + "_saveexec_b64", 2, 2);
    }
    set(t, 44, "s_quadmask_b32", 1, 1);
    set(t, 45, "s_quadmask_b64", 2, 2);
    set(t, 46, "s_movrels_b32", 1, 1);
    set(t, 47, "s_movrels_b64", 2, 2);
    set(t, 48, "s_movreld_b32", 1, 1);
    set(t, 49, "s_movreld_b64", 2, 2);
    set(t, 50, "s_cbranch_join", 0, 1);
    set(t, 51, "s_mov_regrd_b32", 1, 1);
    set(t, 52, "s_abs_i32", 1, 1);
    set(t, 53, "s_mov_fed_b32", 1, 1);
    return t;
}

Table make_sopc() {
    Table t;
    for (unsigned type = 0; type < compare_types.size(); ++type) {
        for (unsigned c = 0; c < conditions.size(); ++c) {
            set(t, 6 * type + c, std::string("s_cmp_") + conditions[c] + "_" + compare_types[type],
                0, 1, 1);
        }
    }
    set(t, 12, "s_bitcmp0_b32", 0, 1, 1);
    set(t, 13, "s_bitcmp1_b32", 0, 1, 1);
    set(t, 14, "s_bitcmp0_b64", 0, 2, 1);
    set(t, 15, "s_bitcmp1_b64", 0, 2, 1);
    set(t, 16, "s_setvskip", 0, 1, 1);
    return t;
}

Table make_sopp() {
    Table t;
    set(t, 0, "s_nop", 0, 0, 0, Imm::count);
    set(t, 1, "s_endpgm", 0, 0);
    set(t, 2, "s_branch", 0, 0, 0, Imm::branch);
    constexpr std::array<const char*, 6> branches = {"scc0", "scc1", "vccz",
                                                     "vccnz", "execz", "execnz"};
    for (unsigned i = 0; i < branches.size(); ++i) {
        set(t, 4 + i, std::string("s_cbranch_") + branches[i], 0, 0, 0, Imm::branch);
    }
    set(t, 10, "s_barrier", 0, 0);
    set(t, 11, "s_setkill", 0, 0, 0, Imm::count);
    set(t, 12, "s_waitcnt", 0, 0, 0, Imm::waitcnt);
    set(t, 13, "s_sethalt", 0, 0, 0, Imm::count);
    set(t, 14, "s_sleep", 0, 0, 0, Imm::count);
    set(t, 15, "s_setprio", 0, 0, 0, Imm::count);
    set(t, 16, "s_sendmsg", 0, 0, 0, Imm::sendmsg);
    set(t, 17, "s_sendmsghalt", 0, 0, 0, Imm::sendmsg);
    set(t, 18, "s_trap", 0, 0, 0, Imm::count);
    set(t, 19, "s_icache_inv", 0, 0);
    set(t, 20, "s_incperflevel", 0, 0, 0, Imm::count);
    set(t, 21, "s_decperflevel", 0, 0, 0, Imm::count);
    set(t, 22, "s_ttracedata", 0, 0);
    constexpr std::array<const char*, 4> debug = {"sys", "user", "sys_or_user", "sys_and_user"};
    for (unsigned i = 0; i < debug.size(); ++i) {
        set(t, 23 + i, std::string("s_cbranch_cdbg") + debug[i], 0, 0, 0, Imm::branch);
    }
    return t;
}

Table make_smrd() {
    Table t;
    constexpr std::array<const char*, 5> sizes = {"", "x2", "x4", "x8", "x16"};
    for (unsigned i = 0; i < sizes.size(); ++i) {
        auto dwords = static_cast<std::uint8_t>(1u << i);
        set(t, i, std::string("s_load_dword") + sizes[i], dwords, 2, 1);
        set(t, 8 + i, std::string("s_buffer_load_dword") + sizes[i], dwords, 4, 1);
    }
    set(t, 29, "s_dcache_inv_vol", 0, 0);
    set(t, 30, "s_memtime", 2, 0);
    set(t, 31, "s_dcache_inv", 0, 0);
    return t;
}

}  // namespace

const std::vector<OpInfo>& opcodes(Format format) {
    static const Table sop2 = make_sop2();
    static const Table sopk = make_sopk();
    static const Table sop1 = make_sop1();
    static const Table sopc = make_sopc();
    static const Table sopp = make_sopp();
    static const Table smrd = make_smrd();
    static const Table none;
    switch (format) {
    case Format::sop2:
        return sop2;
    case Format::sopk:
        return sopk;
    case Format::sop1:
        return sop1;
    case Format::sopc:
        return sopc;
    case Format::sopp:
        return sopp;
    case Format::smrd:
        return smrd;
    default:
        return none;
    }
}

}  // namespace bb::gcn::detail
