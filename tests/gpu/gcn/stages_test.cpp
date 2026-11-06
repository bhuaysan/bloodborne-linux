// The graphics stages without a device: the graphics test programs give modules that spirv-val
// accepts, and what isn't supported throws GcnError naming it.

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <spirv/unified1/spirv.hpp11>

#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/spirv_module.hpp"
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

// The key of ps_attrs, as its header describes the inputs and exports; attr3 is the default 1111.
ShaderKey pixel_key() {
    ShaderKey key{.stage = Stage::pixel, .pgm_rsrc2 = 2u << 1};
    key.ps_input_ena = key.ps_input_addr = 1u << 1 | 1u << 6 | 1u << 8 | 1u << 9;
    key.ps_input_cntl[1] = 1;
    key.ps_input_cntl[2] = 2;
    key.ps_input_cntl[3] = 0x20 | 3u << 8;
    key.col_format = 0x249;
    return key;
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

// The other decorations of the variables at `location`, inputs and outputs. The translator
// decorates a variable with its Location first.
std::set<spv::Decoration> decorations(const Words& spirv, std::uint32_t location) {
    std::set<std::uint32_t> at_location;
    std::set<spv::Decoration> out;
    for (const auto inst : instructions(spirv)) {
        if (is(inst, spv::Op::OpDecorate) && inst.size() == 4 &&
            inst[2] == static_cast<std::uint32_t>(spv::Decoration::Location) &&
            inst[3] == location) {
            at_location.insert(inst[1]);
        } else if (is(inst, spv::Op::OpDecorate) && at_location.contains(inst[1])) {
            out.insert(static_cast<spv::Decoration>(inst[2]));
        }
    }
    return out;
}

TEST(GcnStages, ProgramsPassSpirvVal) {
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    const auto vs = translate(program_words("vs_attrs"), {}, vertex_key);
    EXPECT_EQ(validate(vs.spirv), "");
    EXPECT_EQ(count(vs.spirv, spv::Op::OpEntryPoint, 1, 0), 1);  // Vertex
    EXPECT_EQ(count(vs.spirv, spv::Op::OpExecutionMode, 2,
                    static_cast<std::uint32_t>(spv::ExecutionMode::LocalSize)),
              0);
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

    const auto ps = translate(program_words("ps_attrs"), {}, pixel_key());
    EXPECT_EQ(validate(ps.spirv), "");  // which also requires OriginUpperLeft in Vulkan
    EXPECT_EQ(count(ps.spirv, spv::Op::OpEntryPoint, 1, 4), 1);  // Fragment
    EXPECT_EQ(count(ps.spirv, spv::Op::OpDemoteToHelperInvocation), 1);
    EXPECT_EQ(count(ps.spirv, spv::Op::OpDecorate, 2,
                    static_cast<std::uint32_t>(spv::Decoration::Location)),
              6);  // three inputs, three render targets
    EXPECT_FALSE(ps.interface.wave_size_dependent);
    using D = spv::Decoration;  // attr0 smooth at the center, attr1 linear centroid, attr2 flat
    EXPECT_EQ(decorations(ps.spirv, 0), std::set<D>{});
    EXPECT_EQ(decorations(ps.spirv, 1), (std::set{D::NoPerspective, D::Centroid}));
    EXPECT_EQ(decorations(ps.spirv, 2), std::set{D::Flat});
    EXPECT_EQ(count(ps.spirv, spv::Op::OpConstant, 3, 0x3f800000), 1);  // attr3.x of 1111
    EXPECT_EQ(stored_channels(ps.spirv, D::Location, 0), xyzw);  // 32_ABGR, after input 0
    EXPECT_EQ(stored_channels(ps.spirv, D::Location, 2), (std::multiset<std::uint32_t>{0, 1}));
    const std::uint32_t frag_coord = static_cast<std::uint32_t>(spv::BuiltIn::FragCoord);
    EXPECT_EQ(count(ps.spirv, spv::Op::OpDecorate, 3, frag_coord), 1);  // POS_X and POS_Y
    // PERSP_SAMPLE for attr0 (v4 and v5 placed but not loaded: POS_*_FLOAT with a sample pair is
    // an error), mrt0 32_AR (x and w of the export), and attr3 the default value 0001 (x is 0).
    ShaderKey other = pixel_key();
    other.ps_input_addr = 1u << 0 | 1u << 6 | 1u << 10 | 1u << 11;
    other.ps_input_ena = 1u << 0 | 1u << 6;
    other.col_format = 0x243;
    other.ps_input_cntl[3] = 0x20 | 1u << 8;
    const auto ps2 = translate(program_words("ps_attrs"), {}, other);
    EXPECT_EQ(validate(ps2.spirv), "");  // and the SampleRateShading capability of Sample
    EXPECT_EQ(decorations(ps2.spirv, 0), std::set{D::Sample});
    EXPECT_EQ(count(ps2.spirv, spv::Op::OpDecorate, 3, frag_coord), 0);
    EXPECT_EQ(count(ps2.spirv, spv::Op::OpConstant, 3, 0x3f800000), 0);  // attr3.x is 0
    EXPECT_EQ(stored_channels(ps2.spirv, D::Location, 0), (std::multiset<std::uint32_t>{0, 3}));
    // POS_Z and POS_W in v4 and v5 (W is 1/FragCoord.w), with the center pair and 0001 again.
    other.ps_input_ena = other.ps_input_addr = 1u << 1 | 1u << 6 | 1u << 10 | 1u << 11;
    const auto ps3 = translate(program_words("ps_attrs"), {}, other);
    EXPECT_EQ(validate(ps3.spirv), "");
    EXPECT_EQ(count(ps3.spirv, spv::Op::OpFDiv), 1);
    EXPECT_EQ(count(ps3.spirv, spv::Op::OpConstant, 3, 0x3f800000), 1);  // 1.0f of the 1/W
    for (const std::uint32_t format : {5u, 6u, 7u, 8u}) {  // mrt1 UNORM16, SNORM16, UINT16, SINT16
        other.col_format = 0x203 | format << 4;
        EXPECT_EQ(validate(translate(program_words("ps_attrs"), {}, other).spirv), "") << format;
    }
}

// s_wqm_b64 in a pixel shader is the same two quad swaps as in compute: a fragment shader's
// quads are its 2x2 pixel quads, helper invocations included.
TEST(GcnStages, PixelWholeQuadMode) {
    Words code = {sop1(10, 126, 126)};  // s_wqm_b64 exec, exec
    const auto out = xport(0xf, 0, 0, 1, 2, 3, xp::done | xp::vm);  // exp mrt0 v0-v3 done vm
    code.insert(code.end(), out.begin(), out.end());
    code.push_back(s_endpgm);
    const auto ps = translate(code, {}, pixel_key());
    if (have_spirv_val()) {
        EXPECT_EQ(validate(ps.spirv), "");
    }
    EXPECT_EQ(count(ps.spirv, spv::Op::OpEntryPoint, 1, 4), 1);  // Fragment
    EXPECT_EQ(count(ps.spirv, spv::Op::OpGroupNonUniformQuadSwap), 2);
    EXPECT_EQ(count(ps.spirv, spv::Op::OpCapability, 1,
                    static_cast<std::uint32_t>(spv::Capability::GroupNonUniformQuad)),
              1);
    EXPECT_EQ(count(ps.spirv, spv::Op::OpDemoteToHelperInvocation), 1);
    EXPECT_TRUE(ps.interface.wave_size_dependent);  // the SCC is any over the subgroup
}

// A default value is the same at the three vertices, so v_interp_mov_f32 of its differences P10
// and P20 gives 0 and only p0 gives the value (attr3 is 1111 in pixel_key).
TEST(GcnStages, DefaultValueDifferencesAreZero) {
    for (const unsigned param : {0u, 1u, 2u}) {  // p10, p20, p0
        const auto ps = translate(Words{vintrp(2, 6, param, 3, 0), s_endpgm}, {}, pixel_key());
        if (have_spirv_val()) {
            EXPECT_EQ(validate(ps.spirv), "") << param;
        }
        EXPECT_EQ(count(ps.spirv, spv::Op::OpConstant, 3, 0x3f800000), param == 2 ? 1 : 0)
            << param;
    }
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
    EXPECT_EQ(error({}, {.stage = Stage::pixel}), "no error");
    EXPECT_EQ(error({}, {.stage = Stage::vertex, .pgm_rsrc2 = 1u << 12}),  // SO_EN
              "SPI_SHADER_PGM_RSRC2_VS 0x1000 is not supported yet");
    EXPECT_EQ(error({}, {.stage = Stage::vertex, .pgm_rsrc2 = 1u << 6}), "no error");  // TRAP
    EXPECT_EQ(error({}, {.stage = Stage::vertex, .pgm_rsrc2 = 17u << 1}),
              "USER_SGPR 17 is more than 16");
    EXPECT_EQ(error({vop2(3, 1, 256, 2)}, {.stage = Stage::vertex, .pgm_rsrc1 = 1u << 12}),
              "PGM_RSRC1 0x1000: this float mode is not supported");
    // ds_read_b32 v1, v0 and s_barrier: the LDS and the workgroup are compute-only so far.
    EXPECT_EQ(error({0xd8d80000u, 0x01000000u}, vertex_key),
              "the LDS in a graphics stage is not supported yet");
    EXPECT_EQ(error({sopp(10)}, vertex_key), "s_barrier in a graphics stage is not supported yet");
}

TEST(GcnStages, PixelErrorsNameWhatIsUnsupported) {
    const ShaderKey pixel = pixel_key();
    EXPECT_EQ(error(xport(1, 12, 0, 0, 0, 0, xp::done), pixel),  // exp pos0 v0 done
              "export target 12 is not supported in this stage yet");
    EXPECT_EQ(error(xport(1, 8, 0, 0, 0, 0, xp::done), pixel),  // exp mrtz v0 done
              "export target 8 is not supported in this stage yet");
    EXPECT_EQ(error(xport(1, 9, 0, 0, 0, 0, xp::done), pixel), "no error");  // exp null
    EXPECT_EQ(error(xport(0xf, 0, 0, 1, 0, 0, xp::compr), pixel),  // mrt0 is 32_ABGR
              "a compressed export to mrt0 with SPI_SHADER_COL_FORMAT 9");
    EXPECT_EQ(error(xport(0xf, 1, 0, 1, 2, 3), pixel),  // mrt1 is FP16_ABGR
              "an uncompressed export to mrt1 with SPI_SHADER_COL_FORMAT 4");
    EXPECT_EQ(error(xport(0xf, 3, 0, 1, 2, 3), pixel), "no error");  // mrt3 is ZERO

    EXPECT_EQ(error({vintrp(0, 6, 0, 0, 0)}, vertex_key),
              "v_interp_p1_f32 outside a pixel shader");
    EXPECT_EQ(error({vintrp(0, 6, 4, 0, 0)}, pixel),  // v4 holds POS_X
              "v_interp_p1_f32 reads v4, which holds no I barycentric");
    EXPECT_EQ(error({vintrp(1, 6, 0, 0, 0)}, pixel),  // v0 holds I
              "v_interp_p2_f32 reads v0, which holds no J barycentric");
    EXPECT_EQ(error({vintrp(2, 6, 0, 2, 0)}, pixel),  // v_interp_mov_f32 v6, p10, attr2.x
              "v_interp_mov_f32 of p10 or p20 is not supported yet");
    // Attribute 4 shares location 0 with attribute 0, linear and centroid instead of perspective.
    EXPECT_EQ(error({vintrp(0, 6, 0, 0, 0), vintrp(0, 7, 2, 4, 0)}, pixel),
              "the input at location 0 is read with two interpolations");
    ShaderKey flat = pixel;
    flat.ps_input_cntl[0] = 1u << 10;  // FLAT_SHADE: p1 and p2 read the flat input of mov
    EXPECT_EQ(error({vintrp(0, 6, 0, 0, 0), vintrp(2, 7, 2, 0, 1)}, flat), "no error");
    EXPECT_EQ(error({vintrp(0, 6, 0, 0, 0), vintrp(2, 7, 2, 0, 1)}, pixel),
              "the input at location 0 is read with two interpolations");

    ShaderKey key = pixel;
    key.ps_input_addr = 1u << 1;
    EXPECT_EQ(error({}, key),
              "SPI_PS_INPUT_ENA 0x342 enables inputs outside SPI_PS_INPUT_ADDR 0x2");
    key.ps_input_ena = key.ps_input_addr = 1u << 12;  // FRONT_FACE_ENA
    EXPECT_EQ(error({}, key), "SPI_PS_INPUT_ENA 0x1000 is not supported yet");
    key.ps_input_ena = 0;  // placed but not loaded: the VGPR holds 0
    EXPECT_EQ(error({}, key), "no error");
    // A sample pair runs the shader per sample, which moves FragCoord off the pixel center.
    key.ps_input_ena = key.ps_input_addr = 1u << 0 | 1u << 8;  // PERSP_SAMPLE, POS_X_FLOAT
    EXPECT_EQ(error({}, key), "SPI_PS_INPUT_ENA 0x101: POS_*_FLOAT with a sample barycentric "
                              "pair is not supported yet");
    key.ps_input_ena = key.ps_input_addr = 1u << 4 | 1u << 11;  // LINEAR_SAMPLE, POS_W_FLOAT
    EXPECT_EQ(error({}, key), "SPI_PS_INPUT_ENA 0x810: POS_*_FLOAT with a sample barycentric "
                              "pair is not supported yet");
    key.ps_input_ena = key.ps_input_addr = 1u << 2 | 1u << 8;  // PERSP_CENTROID: no error
    EXPECT_EQ(error({}, key), "no error");
    EXPECT_EQ(error({}, {.stage = Stage::pixel, .pgm_rsrc2 = 1u}),  // SCRATCH_EN
              "SPI_SHADER_PGM_RSRC2_PS 0x1 is not supported yet");
    ShaderKey cntl = pixel;
    cntl.ps_input_cntl[5] = 1u << 17;  // PT_SPRITE_TEX
    EXPECT_EQ(error({}, cntl), "SPI_PS_INPUT_CNTL_5 0x20000 is not supported yet");
    cntl.ps_input_cntl[5] = 1u << 13 | 1;  // CYL_WRAP
    EXPECT_EQ(error({}, cntl), "SPI_PS_INPUT_CNTL_5 0x2001 is not supported yet");
}

}  // namespace
