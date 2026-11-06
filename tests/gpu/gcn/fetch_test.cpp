// The fetch subroutine: a synthetic vertex shader that calls one with s_swappc_b64, inlined at
// the call, and the fetch subroutines that can't be inlined.

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <spirv/unified1/spirv.hpp11>

#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/fetch.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/spirv_module.hpp"
#include "gpu/gcn/spirv_val.hpp"
#include "gpu/gcn/translate.hpp"

namespace {

using namespace bb::gcn;
using namespace bb::test::gcn;

using Words = std::vector<std::uint32_t>;

constexpr std::uint32_t s_nop = sopp(0);
constexpr std::uint32_t s_endpgm = sopp(1);
constexpr std::uint32_t s_branch_next = sopp(2);  // s_branch to the next instruction
constexpr std::uint32_t s_waitcnt_0 = sopp(12);
constexpr std::uint32_t s_waitcnt_lgkm = sopp(12, 0x7f);  // lgkmcnt(0)
constexpr std::uint32_t s_swappc = sop1(33, 0, 0);       // s_swappc_b64 s[0:1], s[0:1]
constexpr std::uint32_t s_setpc = sop1(32, 0, 0);        // s_setpc_b64 s[0:1]
constexpr std::uint32_t constant_0 = 128;

Words cat(std::initializer_list<Words> parts) {
    Words out;
    for (const Words& p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

// The vertex shader: user data s[0:1] the fetch subroutine's address and s[2:3] the vertex buffer
// table. It calls the fetch subroutine, which loads v[4:7], and exports them as pos0, and the
// return address the call left in s[0:1] as param0.
const Words vs = cat({{s_nop, s_swappc},
                      xport(0xf, 12, 4, 5, 6, 7, xp::done),
                      {vop1(1, 8, 0), vop1(1, 9, 1)},  // v_mov_b32 v8, s0; v_mov_b32 v9, s1
                      xport(0x3, 32, 8, 9, 0, 0),
                      {s_endpgm}});

// The fetch subroutine, in the generator's pattern (docs/formats/gnm.md, "Fetch shaders") but
// with buffer_load_dwordx4 for buffer_load_format_xyzw: the V# from the table, the attribute by
// the vertex index in v0, the return, then a data dword that isn't code.
const Words fetch = cat({{smrd(2, 8, 2, true, 0), s_waitcnt_lgkm},  // s_load_dwordx4 s[8:11]
                         mubuf(14, 4, 0, 8, constant_0, 0, buf::idxen),
                         {s_waitcnt_0, s_setpc, 0xffffffffu}});

const ShaderKey vertex_key = {.stage = Stage::vertex, .pgm_rsrc1 = 3u << 24, .pgm_rsrc2 = 4u << 1};

TEST(GcnFetch, DecodeStopsAtTheReturn) {
    const auto body = decode(fetch, Until::subroutine_end);
    ASSERT_EQ(body.size(), 5u);
    EXPECT_EQ(body.back().name, "s_setpc_b64");
    EXPECT_EQ(body.back().offset, 20u);
    EXPECT_TRUE(is_setpc(body.back()));
    EXPECT_FALSE(is_setpc(body.front()));
    EXPECT_THROW(decode(Words{s_waitcnt_0}, Until::subroutine_end), GcnError);
}

TEST(GcnFetch, InlinesAtTheCall) {
    auto program = decode(vs);
    const auto site = detail::inline_fetch(program, fetch);
    std::vector<std::string_view> names;
    std::vector<std::uint32_t> offsets;
    for (const Instruction& inst : program) {
        names.push_back(inst.name);
        offsets.push_back(inst.offset);
    }
    EXPECT_EQ(names, (std::vector<std::string_view>{
                         "s_nop", "s_swappc_b64", "s_load_dwordx4", "s_waitcnt",
                         "buffer_load_dwordx4", "s_waitcnt", "exp", "v_mov_b32_e32",
                         "v_mov_b32_e32", "exp", "s_endpgm"}));
    EXPECT_EQ(offsets, (std::vector<std::uint32_t>{0, 4, 4, 4, 4, 4, 8, 16, 20, 24, 32}));
    EXPECT_EQ(site.call, 1u);
    EXPECT_EQ(site.offsets, (std::vector<std::uint32_t>{0, 4, 8, 16}));
    EXPECT_FALSE(site.inlined(1));
    EXPECT_TRUE(site.inlined(2));
    EXPECT_TRUE(site.inlined(5));
    EXPECT_FALSE(site.inlined(6));

    // Without a call nothing changes, and the fetch code isn't decoded.
    auto plain = decode(Words{s_nop, s_endpgm});
    EXPECT_EQ(detail::inline_fetch(plain, {}).call, detail::FetchSite::none);
    EXPECT_EQ(plain.size(), 2u);
}

TEST(GcnFetch, VertexShaderTranslates) {
    const auto translation = translate(vs, fetch, vertex_key);
    if (have_spirv_val()) {
        EXPECT_EQ(validate(translation.spirv), "");
    }
    // The fetch subroutine's V# is found like the program's own: the pointer in s[2:3], dword 0.
    EXPECT_EQ(translation.interface.buffers,
              (std::vector<ResourceSource>{{.path = {2, 0}}}));
    // The fetch code may end anywhere after its return.
    const Words longer = cat({fetch, {s_endpgm, 0}});
    EXPECT_EQ(translate(vs, longer, vertex_key).spirv, translation.spirv);
    // The return address goes into the call's destination, also another pair than s[0:1].
    const Words other = {sop1(33, 4, 0), s_endpgm};  // s_swappc_b64 s[4:5], s[0:1]
    const auto returned = translate(other, Words{sop1(32, 0, 4)}, vertex_key);
    if (have_spirv_val()) {
        EXPECT_EQ(validate(returned.spirv), "");
    }
}

// A return address that a module writes: the 64-bit constant added to the draw block's
// code_address, and the variables that the sum's low dword and its high dword are stored to.
struct ReturnAddress {
    std::uint64_t offset = 0;
    std::uint32_t low = 0;
    std::uint32_t high = 0;

    bool operator==(const ReturnAddress&) const = default;
};

// The variables of a module's stores, in order, and its return addresses, followed through the
// ids, once their high dword is stored. The prologue's stores come first: user-data dword k into
// the variable of s[k].
struct Stores {
    std::vector<std::uint32_t> variables;
    std::vector<ReturnAddress> returns;
};

Stores stores(const Words& spirv) {
    using spv::Op;
    std::map<std::uint32_t, std::uint64_t> wide;  // the 64-bit constants by id
    std::map<std::uint32_t, std::uint32_t> shifted, lows, highs;  // the sum's id by id
    std::map<std::uint32_t, ReturnAddress> returns;               // by the sum's id
    Stores out;
    for (const auto inst : instructions(spirv)) {
        if (is(inst, Op::OpConstant) && inst.size() == 5) {
            wide[inst[2]] = inst[3] | std::uint64_t{inst[4]} << 32;
        } else if (is(inst, Op::OpIAdd) && wide.contains(inst[4])) {  // type, id, a, b
            returns[inst[2]].offset = wide[inst[4]];
        } else if (is(inst, Op::OpShiftRightLogical) && returns.contains(inst[3])) {
            shifted[inst[2]] = inst[3];
        } else if (is(inst, Op::OpUConvert) && returns.contains(inst[3])) {
            lows[inst[2]] = inst[3];
        } else if (is(inst, Op::OpUConvert) && shifted.contains(inst[3])) {
            highs[inst[2]] = shifted[inst[3]];
        } else if (is(inst, Op::OpStore)) {  // pointer, object
            out.variables.push_back(inst[1]);
            if (lows.contains(inst[2])) {
                returns[lows[inst[2]]].low = inst[1];
            } else if (highs.contains(inst[2])) {  // the low dword's store comes first
                ReturnAddress& r = returns[highs[inst[2]]];
                r.high = inst[1];
                out.returns.push_back(r);
            }
        }
    }
    return out;
}

// The return address is the draw block's code_address plus the offset after the call, its low
// dword in the call's first destination SGPR and its high dword in the second. Running a vertex
// shader waits for the graphics runner, so the value is followed through the module.
TEST(GcnFetch, ReturnAddressGoesIntoTheDestination) {
    const auto first = stores(translate(vs, fetch, vertex_key).spirv);
    EXPECT_EQ(first.returns,
              (std::vector<ReturnAddress>{{8, first.variables.at(0), first.variables.at(1)}}));
    // A later call returns further on.
    const auto later = stores(translate(cat({{s_nop, s_nop, s_nop}, vs}), fetch, vertex_key).spirv);
    EXPECT_EQ(later.returns,
              (std::vector<ReturnAddress>{{20, later.variables.at(0), later.variables.at(1)}}));
    // Another pair: s[4:5], here filled with user data too so that its variables are known.
    const ShaderKey six = {.stage = Stage::vertex, .pgm_rsrc2 = 6u << 1};
    const Words other = {sop1(33, 4, 0), s_endpgm};  // s_swappc_b64 s[4:5], s[0:1]
    const auto into = stores(translate(other, Words{sop1(32, 0, 4)}, six).spirv);
    EXPECT_EQ(into.returns,
              (std::vector<ReturnAddress>{{4, into.variables.at(4), into.variables.at(5)}}));
}

// The message of the GcnError that translating `program` with `fetch_code` throws, and its offset.
struct Failure {
    std::string message;
    std::uint32_t offset = 0;

    bool operator==(const Failure&) const = default;
};

Failure failure(const Words& program, const Words& fetch_code, const ShaderKey& key = vertex_key) {
    try {
        translate(program, fetch_code, key);
    } catch (const GcnError& e) {
        const std::string what = e.what();
        return {what.substr(0, what.rfind(" at offset ")), e.offset()};
    }
    return {"no error", 0};
}

TEST(GcnFetch, ErrorsAreAtTheCall) {
    const Words call = {s_nop, s_swappc, s_endpgm};
    const auto at_call = [](std::string message) { return Failure{std::move(message), 4}; };
    EXPECT_EQ(failure(call, {s_setpc}), (Failure{"no error", 0}));
    EXPECT_EQ(failure(call, {}), at_call("s_swappc_b64 without a fetch subroutine"));
    EXPECT_EQ(failure({s_swappc, s_swappc, s_endpgm}, {s_setpc}),
              (Failure{"a second s_swappc_b64", 0}));
    EXPECT_EQ(failure({s_nop, sop1(33, 0, 2), s_endpgm}, {s_setpc}),
              at_call("s_swappc_b64 doesn't call the fetch subroutine in s[0:1]"));
    EXPECT_EQ(failure(call, {s_waitcnt_0}).message.find("fetch subroutine: no s_setpc_b64"), 0u);
    EXPECT_EQ(failure(call, {sop1(32, 0, 2)}),
              at_call("the fetch subroutine doesn't return through the call's destination"));
    EXPECT_EQ(failure(call, {s_nop, s_branch_next, s_setpc}),
              at_call("fetch subroutine: s_branch at offset 0x4 is not supported"));
    EXPECT_EQ(failure(call, {s_endpgm, s_setpc}),
              at_call("fetch subroutine: s_endpgm at offset 0x0 is not supported"));
    EXPECT_EQ(failure(call, {sop1(31, 2, 0), s_setpc}),  // s_getpc_b64 s[2:3]
              at_call("fetch subroutine: s_getpc_b64 at offset 0x0 is not supported"));
    EXPECT_EQ(failure(call, {sop1(3, 1, constant_0), s_setpc}),  // s_mov_b32 s1, 0
              at_call("fetch subroutine: s_mov_b32 at offset 0x0 writes the return address"));
    // An inlined instruction the translator doesn't support: at the call, with its fetch offset.
    const Words format = cat({{s_nop, s_nop}, mubuf(3, 4, 0, 8, constant_0, 0, buf::idxen),
                              {s_setpc}});  // buffer_load_format_xyzw
    EXPECT_EQ(failure(call, format),
              at_call("fetch subroutine: unsupported instruction buffer_load_format_xyzw at "
                      "offset 0x8"));
    // Only the vertex stage inlines it.
    EXPECT_EQ(failure(call, {s_setpc}, {}), at_call("unsupported instruction s_swappc_b64"));
}

}  // namespace
