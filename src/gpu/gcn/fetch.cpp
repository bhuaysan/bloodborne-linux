// The fetch subroutine of the vertex stage, inlined at its call. The run time reads the fetch code
// from the address in user-data dwords 0-1 (docs/formats/gnm.md, "Fetch shaders"); the vertex
// shader calls it with s_swappc_b64 and it returns with s_setpc_b64 (Sea Islands ISA manual,
// "SOP1 Instructions": s_swappc_b64 writes PC + 4 into its destination and jumps to its source).

#include "gpu/gcn/fetch.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace bb::gcn::detail {

namespace {

constexpr std::uint16_t op_swappc = 33;  // SOP1

bool is_swappc(const Instruction& inst) {
    return inst.format == Format::sop1 && inst.opcode == op_swappc;
}

// The instructions that change control besides the branches and s_endpgm. In the fetch
// subroutine, s_getpc_b64 would give an address in the fetch code, which isn't inlined there.
bool changes_control(const Instruction& inst) {
    constexpr std::string_view names[] = {"s_getpc_b64",    "s_swappc_b64",     "s_rfe_b64",
                                          "s_cbranch_join", "s_cbranch_i_fork", "s_cbranch_g_fork"};
    return branch_target(inst) || is_endpgm(inst) ||
           std::ranges::find(names, inst.name) != std::end(names);
}

// Whether a destination of the instruction covers one of the SGPRs from `first` on of a pair.
bool writes_pair(const Instruction& inst, unsigned first) {
    return std::ranges::any_of(std::span(inst.dst).first(inst.num_dst), [&](const Operand& o) {
        return o.kind == OperandKind::scalar && o.code < first + 2 && o.code + o.dwords > first;
    });
}

}  // namespace

FetchSite inline_fetch(std::vector<Instruction>& program, std::span<const std::uint32_t> fetch) {
    const auto call = std::ranges::find_if(program, is_swappc);
    if (call == program.end()) {
        return {};
    }
    const std::uint32_t at = call->offset;
    const auto error = [&](const std::string& message) { return GcnError(message, at, 0); };
    if (std::find_if(call + 1, program.end(), is_swappc) != program.end()) {
        throw error("a second s_swappc_b64");
    }
    if (call->src[0].kind != OperandKind::scalar || call->src[0].code != 0) {
        throw error("s_swappc_b64 doesn't call the fetch subroutine in s[0:1]");
    }
    if (fetch.empty()) {
        throw error("s_swappc_b64 without a fetch subroutine");
    }
    std::vector<Instruction> body;
    try {
        body = decode(fetch, Until::subroutine_end);
    } catch (const GcnError& e) {
        throw error(std::format("fetch subroutine: {}", e.what()));
    }
    const Operand back = body.back().src[0];  // of the s_setpc_b64
    if (back.kind != OperandKind::scalar || back.code != call->dst[0].code) {
        throw error("the fetch subroutine doesn't return through the call's destination");
    }
    const std::uint32_t dwords = (body.back().offset >> 2) + body.back().size;
    body.pop_back();

    FetchSite site{.call = static_cast<std::uint32_t>(call - program.begin()),
                   .offsets = {},
                   .dwords = dwords};
    for (Instruction& inst : body) {
        if (changes_control(inst)) {
            throw error(std::format("fetch subroutine: {} at offset {:#x} is not supported",
                                    inst.name, inst.offset));
        }
        if (writes_pair(inst, back.code)) {
            throw error(std::format(
                "fetch subroutine: {} at offset {:#x} writes the return address", inst.name,
                inst.offset));
        }
        site.offsets.push_back(inst.offset);
        inst.offset = at;
    }
    program.insert(call + 1, body.begin(), body.end());
    return site;
}

}  // namespace bb::gcn::detail
