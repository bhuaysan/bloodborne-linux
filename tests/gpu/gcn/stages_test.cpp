// The graphics stages without a device: the graphics test programs give modules that spirv-val
// accepts, and what isn't supported throws GcnError naming it.

#include <cstddef>
#include <cstdint>
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

// The number of instructions with opcode `op` in a module (and `operand` as the operand at
// `index`, when given).
int count(const std::vector<std::uint32_t>& spirv, spv::Op op, std::size_t index = 0,
          std::uint32_t operand = 0) {
    int n = 0;
    for (std::size_t i = 5; i < spirv.size() && (spirv[i] >> 16) != 0; i += spirv[i] >> 16) {
        if ((spirv[i] & 0xffff) == static_cast<std::uint32_t>(op) &&
            (index == 0 || (i + index < spirv.size() && spirv[i + index] == operand))) {
            ++n;
        }
    }
    return n;
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
}

}  // namespace
