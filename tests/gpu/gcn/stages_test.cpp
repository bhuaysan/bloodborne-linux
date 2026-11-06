// The graphics stages without a device: the graphics test programs give modules that spirv-val
// accepts, and what isn't supported throws GcnError naming it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <spirv/unified1/spirv.hpp11>

#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/spirv_val.hpp"
#include "gpu/gcn/translate.hpp"

namespace {

using namespace bb::test::gcn;
using bb::gcn::GcnError;
using bb::gcn::ShaderKey;
using bb::gcn::Stage;
using bb::gcn::translate;

constexpr std::uint32_t s_endpgm = sopp(1);

// The key of vs_attrs: 4 user SGPRs, VGPR_COMP_CNT 3, step rates 1 and 2.
const ShaderKey vertex_key = {.stage = Stage::vertex,
                              .pgm_rsrc1 = 3u << 24,
                              .pgm_rsrc2 = 4u << 1,
                              .instance_step_rate = {1, 2}};

using Words = std::vector<std::uint32_t>;

// The instructions of a module after its header, each with its opcode word first.
std::vector<std::span<const std::uint32_t>> instructions(const Words& spirv) {
    std::vector<std::span<const std::uint32_t>> out;
    for (std::size_t i = 5; i < spirv.size() && (spirv[i] >> 16) != 0; i += spirv[i] >> 16) {
        out.push_back(std::span(spirv).subspan(i, std::min<std::size_t>(spirv[i] >> 16,
                                                                         spirv.size() - i)));
    }
    return out;
}

bool is(std::span<const std::uint32_t> inst, spv::Op op) {
    return (inst[0] & 0xffff) == static_cast<std::uint32_t>(op);
}

// The number of instructions with opcode `op` in a module (and `operand` as the operand at
// `index`, when given).
int count(const Words& spirv, spv::Op op, std::size_t index = 0, std::uint32_t operand = 0) {
    int n = 0;
    for (const auto inst : instructions(spirv)) {
        if (is(inst, op) && (index == 0 || (index < inst.size() && inst[index] == operand))) {
            ++n;
        }
    }
    return n;
}

// The channels the module stores into the output decorated with `decoration` `value` (a
// location or a built-in), one entry per OpStore through an access chain with a constant index.
std::multiset<std::uint32_t> stored_channels(const Words& spirv, spv::Decoration decoration,
                                             std::uint32_t value) {
    std::uint32_t output = 0;
    std::map<std::uint32_t, std::uint32_t> constants;  // by id
    std::map<std::uint32_t, std::uint32_t> channels;   // by the id of an access chain into output
    std::multiset<std::uint32_t> stored;
    for (const auto inst : instructions(spirv)) {
        if (is(inst, spv::Op::OpDecorate) && inst.size() == 4 &&
            inst[2] == static_cast<std::uint32_t>(decoration) && inst[3] == value) {
            output = inst[1];
        } else if (is(inst, spv::Op::OpConstant) && inst.size() == 4) {
            constants[inst[2]] = inst[3];
        } else if (is(inst, spv::Op::OpAccessChain) && inst.size() == 5 && inst[3] == output &&
                   output != 0) {
            channels[inst[2]] = constants.at(inst[4]);
        } else if (is(inst, spv::Op::OpStore) && channels.contains(inst[1])) {
            stored.insert(channels[inst[1]]);
        }
    }
    return stored;
}

TEST(GcnStages, ProgramsPassSpirvVal) {
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    const auto vs = translate(program_words("vs_attrs"), {}, vertex_key);
    EXPECT_EQ(validate(vs.spirv), "");
    EXPECT_EQ(count(vs.spirv, spv::Op::OpEntryPoint, 1, 0), 1);  // Vertex
    EXPECT_EQ(count(vs.spirv, spv::Op::OpUDiv), 2);              // v1 and v2
    EXPECT_FALSE(vs.interface.wave_size_dependent);
    // pos0 and param0 store all four channels, param1 only x, at location 1.
    const std::multiset<std::uint32_t> xyzw = {0, 1, 2, 3};
    EXPECT_EQ(stored_channels(vs.spirv, spv::Decoration::BuiltIn,
                              static_cast<std::uint32_t>(spv::BuiltIn::Position)),
              xyzw);
    EXPECT_EQ(stored_channels(vs.spirv, spv::Decoration::Location, 0), xyzw);
    EXPECT_EQ(stored_channels(vs.spirv, spv::Decoration::Location, 1),
              std::multiset<std::uint32_t>{0});
    EXPECT_EQ(count(vs.spirv, spv::Op::OpDecorate, 2,
                    static_cast<std::uint32_t>(spv::Decoration::Location)),
              2);
    // A step rate of 0 gives 0, and with VGPR_COMP_CNT 0 there is no instance index.
    ShaderKey key = vertex_key;
    key.instance_step_rate = {0, 0};
    EXPECT_EQ(count(translate(program_words("vs_attrs"), {}, key).spirv, spv::Op::OpUDiv), 0);
    key.pgm_rsrc1 = 0;
    const auto first = translate(program_words("vs_attrs"), {}, key);
    EXPECT_EQ(validate(first.spirv), "");
    EXPECT_EQ(count(first.spirv, spv::Op::OpDecorate, 2,
                    static_cast<std::uint32_t>(spv::Decoration::BuiltIn)),
              3);  // SubgroupLocalInvocationId, VertexIndex, Position
}

// The float mode comes from the same RSRC1 as VGPR_COMP_CNT and applies to the VS as in compute.
TEST(GcnStages, VertexFloatMode) {
    ShaderKey key = vertex_key;
    key.pgm_rsrc1 |= 3u << 12;  // FP_ROUND_32: toward zero
    const Words add = {vop2(3, 1, 256, 2), s_endpgm};  // v_add_f32 v1, v0, v2
    const auto vs = translate(add, {}, key).spirv;
    if (have_spirv_val()) {
        EXPECT_EQ(validate(vs), "");
    }
    EXPECT_EQ(count(vs, spv::Op::OpExecutionMode, 2,
                    static_cast<std::uint32_t>(spv::ExecutionMode::RoundingModeRTZ)),
              1);
    EXPECT_EQ(count(vs, spv::Op::OpEntryPoint, 1, 0), 1);  // still a Vertex entry point
}

// The message of the GcnError that translating `code` throws.
std::string error(std::vector<std::uint32_t> code, const ShaderKey& key) {
    code.push_back(s_endpgm);
    try {
        translate(code, {}, key);
    } catch (const GcnError& e) {
        const std::string what = e.what();
        return what.substr(0, what.find(" at offset "));
    }
    return "no error";
}

TEST(GcnStages, ErrorsNameWhatIsUnsupported) {
    EXPECT_EQ(error(xport(1, 9, 0, 0, 0, 0, xp::done), vertex_key), "no error");  // exp null
    EXPECT_EQ(error(xport(1, 0, 0, 0, 0, 0, xp::done), vertex_key),  // exp mrt0 v0 done
              "export target 0 is not supported in this stage yet");
    EXPECT_EQ(error(xport(1, 13, 0, 0, 0, 0, xp::done), vertex_key),  // exp pos1 v0 done
              "export target 13 is not supported in this stage yet");
    EXPECT_EQ(error(xport(0xf, 32, 0, 1, 0, 0, xp::compr), vertex_key),
              "a compressed export to a position or parameter");
    EXPECT_EQ(error(xport(1, 12, 0, 0, 0, 0, xp::done), {}), "exp in a compute shader");
    EXPECT_EQ(error({}, {.stage = Stage::pixel}), "the pixel stage is not translated yet");
    EXPECT_EQ(error({}, {.stage = Stage::vertex, .pgm_rsrc2 = 1u << 12}),  // SO_EN
              "SPI_SHADER_PGM_RSRC2_VS 0x1000 is not supported yet");
    EXPECT_EQ(error({}, {.stage = Stage::vertex, .pgm_rsrc2 = 1u << 6}), "no error");  // TRAP
    EXPECT_EQ(error({}, {.stage = Stage::vertex, .pgm_rsrc2 = 17u << 1}),
              "USER_SGPR 17 is more than 16");
    EXPECT_EQ(error({vop2(3, 1, 256, 2)}, {.stage = Stage::vertex, .pgm_rsrc1 = 1u << 12}),
              "PGM_RSRC1 0x1000: this float mode is not supported");
}

}  // namespace
