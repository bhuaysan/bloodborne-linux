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

}  // namespace

const std::vector<OpInfo>& opcodes(Format format) {
    static const Table sopp = make_sopp();
    static const Table none;
    switch (format) {
    case Format::sopp:
        return sopp;
    default:
        return none;
    }
}

}  // namespace bb::gcn::detail
