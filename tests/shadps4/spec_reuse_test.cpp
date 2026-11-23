// The pipeline cache's permutation lookup (Vulkan::Program::FindPermutation, #592) against the
// lookup it replaces: build the stage's StageSpecialization and take the first permutation equal
// to it. Random draws of one program change the sharps, the fetch shader, the vertex buffer
// table, the runtime info and the first binding, often only in parts the specialization does
// not read (addresses, sizes), so that both the reused answer and the full lookup run. The
// pipeline reuse of #780 checks a draw with MatchesSharps, the part of Matches that reads guest
// memory. Needs no Vulkan device: the modules are placeholder handles.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

namespace {

using Shader::Backend::Bindings;

enum class Kind { Vertex, Fragment };

// GCN encodings of the instructions a fetch shader has (ParseFetchShader).
constexpr u32 s_load_dwordx4(u32 sdst, u32 sbase, u32 dword_offset) {
    return (0x18u << 27) | (2u << 22) | (sdst << 15) | (sbase << 9) | (1u << 8) | dword_offset;
}
constexpr std::array<u32, 2> buffer_load_format(u32 num_elements, u32 srsrc, u32 vdata,
                                                u32 inst_offset, u32 step_rate) {
    return {(0x38u << 26) | ((num_elements - 1) << 18) | inst_offset,
            ((srsrc / 4) << 16) | (vdata << 8) | step_rate};
}
constexpr u32 s_waitcnt(u32 imm) {
    return (0x17Fu << 23) | (12u << 16) | imm;
}
constexpr u32 s_setpc_b64(u32 sreg) {
    return (0x17Du << 23) | (32u << 8) | sreg;
}

// One program and the state a draw hands GetProgram: user data, the flattened user data, the
// fetch shader and the vertex buffer table it points to, the runtime info and the first binding.
class Draws {
public:
    static constexpr u32 FetchSgpr = 0;  // user data 0-1: the fetch shader
    static constexpr u32 TableSgpr = 2;  // user data 2-3: the vertex buffer table
    static constexpr u32 NumAttribs = 3;
    static constexpr u32 NumDraws = 5000;

    Draws(Kind stage, u32 seed)
        : stage{stage}, rng{seed},
          program{stage == Kind::Vertex ? Shader::HwStage::Vertex
                                                 : Shader::HwStage::Fragment,
                  stage == Kind::Vertex ? Shader::SwStage::Vertex
                                                 : Shader::SwStage::Fragment,
                  Shader::ShaderParams{.user_data = std::span<const u32, 16>{user_data},
                                       .code = {},
                                       .hash = 0x592}} {
        auto& info = program.info;
        u16 loc = 16;
        for (u32 i = 0; i < 3; ++i) {
            Shader::BufferResource buffer{};
            buffer.sharp_fetch.summary = Shader::SharpFetch<AmdGpu::Buffer>::Summary::SingleLoad;
            buffer.sharp_fetch.offsets[0] = loc;
            buffer.is_formatted = i == 1;
            info.buffers.push_back(buffer);
            buffer_locs.push_back(loc);
            loc += 4;
        }
        for (u32 i = 0; i < 2; ++i) {
            Shader::ImageResource image{};
            image.sharp_fetch.summary = Shader::SharpFetch<AmdGpu::Image>::Summary::SingleLoad;
            image.sharp_fetch.offsets[0] = loc;
            image.is_array = i == 0;
            image.is_written = i == 1;
            image.mip_fallback_mode = i == 1 ? Shader::MipStorageFallbackMode::DynamicIndex
                                             : Shader::MipStorageFallbackMode::None;
            info.images.push_back(image);
            image_locs.push_back(loc);
            loc += 8;
        }
        if (stage == Kind::Fragment) {
            info.fmasks.push_back(Shader::FMaskResource{.sharp_idx = loc});
            fmask_locs.push_back(loc);
            loc += 8;
        }
        for (u32 i = 0; i < 2; ++i) {
            Shader::SamplerResource sampler{};
            sampler.sharp_fetch.summary =
                Shader::SharpFetch<AmdGpu::Sampler>::Summary::SingleLoad;
            sampler.sharp_fetch.offsets[0] = loc;
            info.samplers.push_back(sampler);
            sampler_locs.push_back(loc);
            loc += 4;
        }
        info.flattened_ud_buf.assign(loc, 0);
        info.user_data = user_data;
        if (stage == Kind::Vertex) {
            info.has_fetch_shader = true;
            info.fetch_shader_sgpr_base = FetchSgpr;
            vertex_table.resize(4 * NumAttribs);
            SetPointer(TableSgpr, vertex_table.data());
            fetch_code.resize(64);
            SetPointer(FetchSgpr, fetch_code.data());
        }
        // A few variants per sharp, so that draws come back to the same specialization.
        for (auto& v : buffer_variants) {
            v = {RandomBuffer(), RandomBuffer(), RandomBuffer()};
        }
        for (auto& v : image_variants) {
            v = {RandomImage(), RandomImage(), RandomImage()};
        }
        for (auto& v : sampler_variants) {
            v = {RandomSampler(), RandomSampler()};
        }
        for (u32 i = 0; i < 6; ++i) {
            attrib_variants[i] = RandomBuffer();
        }
        Randomize(true);
    }

    // Changes the draw state: each part keeps its value, takes one of a few variants, or changes
    // only in what the specialization does not read.
    void Randomize(bool all = false) {
        auto& flat = program.info.flattened_ud_buf;
        const auto pick = [&](u32 n) { return std::uniform_int_distribution<u32>{0, n - 1}(rng); };
        const auto change = [&] { return all || pick(6) == 0; };
        for (size_t i = 0; i < buffer_locs.size(); ++i) {
            if (change()) {
                auto buffer = buffer_variants[i][pick(3)];
                buffer.base_address = pick(1u << 30);
                buffer.num_records = pick(5) == 0 ? 0 : 1 + pick(1u << 20);
                std::memcpy(&flat[buffer_locs[i]], &buffer, sizeof(buffer));
            }
        }
        for (size_t i = 0; i < image_locs.size(); ++i) {
            if (change()) {
                auto image = image_variants[i][pick(3)];
                image.base_address = pick(5) == 0 ? 0 : 1 + pick(1u << 30);
                image.width = pick(1u << 14);
                image.height = pick(1u << 14);
                std::memcpy(&flat[image_locs[i]], &image, sizeof(image));
            }
        }
        for (const u32 loc : fmask_locs) {
            if (change()) {
                auto fmask = image_variants[0][0];
                fmask.base_address = pick(3) == 0 ? 0 : 1 + pick(1u << 30);
                fmask.width = 64u << pick(2);
                fmask.height = 64u << pick(2);
                std::memcpy(&flat[loc], &fmask, sizeof(fmask));
            }
        }
        for (size_t i = 0; i < sampler_locs.size(); ++i) {
            if (change()) {
                auto sampler = sampler_variants[i][pick(2)];
                std::memcpy(&flat[sampler_locs[i]], &sampler, sizeof(sampler));
            }
        }
        if (stage == Kind::Vertex) {
            if (change()) {
                WriteFetchShader(pick(2) == 0 ? 4 : 2, pick(3), pick(2) == 0);
            }
            for (u32 i = 0; i < NumAttribs; ++i) {
                if (change()) {
                    auto attrib = attrib_variants[pick(i == 0 ? 6 : 2)];
                    attrib.base_address = pick(1u << 30);
                    attrib.num_records = pick(6) == 0 ? 0 : 1 + pick(1u << 20);
                    std::memcpy(&vertex_table[4 * i], &attrib, sizeof(attrib));
                }
            }
        }
        if (change() && !fixed_registers) {
            runtime_variant = pick(3);
        }
        if (change() && !fixed_registers) {
            start = pick(2) == 0 ? Bindings{} : Bindings{.unified = 3, .buffer = 1};
        }
    }

    // What the user data points to, for changing it in place (#780).
    u32* FlatBuffer(size_t buffer) {
        return &program.info.flattened_ud_buf[buffer_locs[buffer]];
    }
    u32* VertexSharp(size_t attrib) {
        return &vertex_table[4 * attrib];
    }
    u32* FetchCode() {
        return fetch_code.data();
    }

    Shader::RuntimeInfo RuntimeInfo() const {
        Shader::RuntimeInfo info{};
        if (stage == Kind::Vertex) {
            info.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
            info.sw.vs.step_rate_0 = 1 + runtime_variant;
            info.sw.vs.step_rate_1 = 4;
            info.hw.vs.num_outputs = 2;
        } else {
            info.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
            info.hw.fs.num_inputs = 2 + (runtime_variant == 2);
            info.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat(runtime_variant);
        }
        return info;
    }

    Kind stage;
    std::mt19937 rng;
    std::array<u32, 16> user_data{};
    Vulkan::Program program;
    Bindings start{};
    // Randomize leaves the runtime info and the first binding alone (#780: the registers they
    // come from did not change).
    bool fixed_registers = false;

private:
    void SetPointer(u32 sgpr, const void* p) {
        const u64 addr = reinterpret_cast<u64>(p);
        std::memcpy(&user_data[sgpr], &addr, sizeof(addr));
    }

    // The fetch shader in place: loads the table's V#s and `num_attribs` attributes with
    // `num_elements` components; `step` sets the first attribute's instance step rate, and
    // `wait` the s_waitcnt immediate (which ParseFetchShader ignores).
    void WriteFetchShader(u32 num_elements, u32 step, bool wait) {
        std::vector<u32> code;
        code.push_back(s_load_dwordx4(8, TableSgpr / 2, 0));
        code.push_back(s_load_dwordx4(12, TableSgpr / 2, 4));
        code.push_back(s_load_dwordx4(16, TableSgpr / 2, 8));
        code.push_back(s_waitcnt(wait ? 0x7f : 0x0f));
        for (u32 i = 0; i < NumAttribs; ++i) {
            const auto inst = buffer_load_format(i == 0 ? num_elements : 4, 8 + 4 * i, 4 * i, 0,
                                                 i == 0 ? step : 0);
            code.insert(code.end(), inst.begin(), inst.end());
        }
        code.push_back(s_setpc_b64(30));
        std::fill(fetch_code.begin(), fetch_code.end(), 0u);
        std::copy(code.begin(), code.end(), fetch_code.begin());
    }

    // Any number format but SnormNz, which MapNumberConversion takes only with some formats.
    static u32 NumberFormat(const auto& pick) {
        const u32 format = pick(9);
        return format == u32(AmdGpu::NumberFormat::SnormNz) ? u32(AmdGpu::NumberFormat::Srgb)
                                                            : format;
    }

    AmdGpu::Buffer RandomBuffer() {
        const auto pick = [&](u32 n) { return std::uniform_int_distribution<u32>{0, n - 1}(rng); };
        AmdGpu::Buffer buffer{};
        buffer.stride = pick(3) * 16;
        buffer.swizzle_enable = pick(2);
        buffer.dst_sel_x = 4 + pick(4);
        buffer.dst_sel_y = 4 + pick(4);
        buffer.dst_sel_z = pick(8);
        buffer.dst_sel_w = pick(8);
        buffer.num_format = NumberFormat(pick);
        buffer.data_format = pick(16);
        buffer.element_size = pick(4);
        buffer.index_stride = pick(4);
        return buffer;
    }

    AmdGpu::Image RandomImage() {
        const auto pick = [&](u32 n) { return std::uniform_int_distribution<u32>{0, n - 1}(rng); };
        AmdGpu::Image image{};
        image.data_format = pick(3) == 0 ? u32(AmdGpu::DataFormat::Format32)
                                         : u32(AmdGpu::DataFormat::Format8_8_8_8);
        image.num_format = NumberFormat(pick);
        image.dst_sel_x = 4 + pick(4);
        image.dst_sel_y = 4 + pick(4);
        image.dst_sel_z = 4 + pick(4);
        image.dst_sel_w = 4 + pick(4);
        image.base_level = pick(2);
        image.last_level = 2 + pick(2);
        image.type = pick(2) == 0 ? u32(AmdGpu::ImageType::Color2D)
                                  : u32(AmdGpu::ImageType::Color2DArray);
        return image;
    }

    AmdGpu::Sampler RandomSampler() {
        const auto pick = [&](u32 n) { return std::uniform_int_distribution<u32>{0, n - 1}(rng); };
        AmdGpu::Sampler sampler{};
        sampler.raw0 = (u64{pick(1u << 31)} << 32) | pick(1u << 31);
        sampler.raw1 = pick(1u << 31);
        return sampler;
    }

    std::vector<u16> buffer_locs, image_locs, fmask_locs, sampler_locs;
    std::array<std::array<AmdGpu::Buffer, 3>, 3> buffer_variants{};
    std::array<std::array<AmdGpu::Image, 3>, 2> image_variants{};
    std::array<std::array<AmdGpu::Sampler, 2>, 2> sampler_variants{};
    std::array<AmdGpu::Buffer, 6> attrib_variants{};
    std::vector<u32> vertex_table;
    std::vector<u32> fetch_code;
    u32 runtime_variant{};
};

// The lookup before #592: the first permutation equal to the specialization, or a new one.
size_t full_lookup(const Vulkan::Program& program, const Shader::StageSpecialization& spec) {
    const auto it = std::ranges::find(program.modules, spec, &Vulkan::Program::Module::spec);
    return std::distance(program.modules.begin(), it);
}

vk::ShaderModule placeholder_module(size_t idx) {
    return vk::ShaderModule{reinterpret_cast<VkShaderModule>(static_cast<uintptr_t>(idx + 1))};
}

void run_draws(Kind stage, u32 seed) {
    Draws draws{stage, seed};
    const Shader::Profile profile{};
    auto& program = draws.program;
    u32 reused = 0;
    u32 built = 0;
    u32 added = 0;
    for (u32 draw = 0; draw < Draws::NumDraws; ++draw) {
        draws.Randomize();
        const auto runtime_info = draws.RuntimeInfo();
        const Shader::StageSpecialization spec{program.info, runtime_info, profile, draws.start};
        const size_t expected = full_lookup(program, spec);

        std::optional<Shader::StageSpecialization> made;
        const size_t idx = program.FindPermutation(runtime_info, profile, draws.start, made);
        ASSERT_EQ(idx, expected) << "draw " << draw;
        if (made) {
            ++built;
            ASSERT_TRUE(made->SameAs(spec)) << "draw " << draw;
        } else {
            ++reused;
        }
        if (idx == program.modules.size()) {
            ASSERT_TRUE(made.has_value());
            program.AddPermut(placeholder_module(idx), std::move(*made));
            ++added;
        }
    }
    // Both ways ran often, and draws needed several permutations.
    EXPECT_GT(reused, Draws::NumDraws / 10);
    EXPECT_GT(built, Draws::NumDraws / 10);
    EXPECT_GT(added, 3u);
    std::printf("%s seed %u: %u reused, %u built, %zu permutations\n",
                stage == Kind::Vertex ? "vertex" : "fragment", seed, reused, built,
                program.modules.size());
}

}  // namespace

TEST(SpecReuse, VertexStageGetsTheFullLookupsPermutation) {
    for (u32 seed = 1; seed <= 3; ++seed) {
        run_draws(Kind::Vertex, seed);
    }
}

TEST(SpecReuse, FragmentStageGetsTheFullLookupsPermutation) {
    for (u32 seed = 1; seed <= 3; ++seed) {
        run_draws(Kind::Fragment, seed);
    }
}

// Matches says yes only for inputs whose specialization is SameAs the stored one, and says yes for
// all of those while the fetch shader's words stay the same.
TEST(SpecReuse, MatchesOnlyTheSameSpecialization) {
    for (const auto stage : {Kind::Vertex, Kind::Fragment}) {
        Draws draws{stage, 7};
        const Shader::Profile profile{};
        auto& info = draws.program.info;
        std::optional<Shader::StageSpecialization> last;
        std::vector<u32> last_code;
        u32 same = 0;
        u32 differ = 0;
        for (u32 draw = 0; draw < Draws::NumDraws; ++draw) {
            draws.Randomize();
            const auto runtime_info = draws.RuntimeInfo();
            Shader::StageSpecialization spec{info, runtime_info, profile, draws.start};
            std::vector<u32> code;
            if (!spec.fetch_shader_data.Empty()) {
                const u32* p = Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
                code.assign(p, p + spec.fetch_shader_data.size / sizeof(u32));
            }
            if (last) {
                const bool matches = last->Matches(info, runtime_info, draws.start, last_code);
                const bool is_same = spec.SameAs(*last);
                if (matches) {
                    ASSERT_TRUE(is_same) << "draw " << draw;
                }
                if (is_same && code == last_code) {
                    ASSERT_TRUE(matches) << "draw " << draw;
                }
                ++(is_same ? same : differ);
            }
            last = std::move(spec);
            last_code = std::move(code);
        }
        EXPECT_GT(same, Draws::NumDraws / 10);
        EXPECT_GT(differ, Draws::NumDraws / 10);
    }
}

// A permutation put in place from the disk cache drops the remembered answer.
TEST(SpecReuse, InsertedPermutationDropsTheLastAnswer) {
    Draws draws{Kind::Fragment, 11};
    const Shader::Profile profile{};
    auto& program = draws.program;
    const auto runtime_info = draws.RuntimeInfo();

    std::optional<Shader::StageSpecialization> made;
    ASSERT_EQ(program.FindPermutation(runtime_info, profile, draws.start, made), 0u);
    ASSERT_TRUE(made.has_value());
    program.AddPermut(placeholder_module(0), std::move(*made));
    made.reset();
    ASSERT_EQ(program.FindPermutation(runtime_info, profile, draws.start, made), 0u);
    EXPECT_FALSE(made.has_value());  // reused

    // Slot 0 now holds a permutation of other inputs, as a preload may leave it.
    draws.start = Bindings{.unified = 7, .buffer = 2};
    Shader::StageSpecialization other{program.info, runtime_info, profile, draws.start};
    draws.start = Bindings{};
    program.InsertPermut(placeholder_module(5), std::move(other), 0);
    made.reset();
    EXPECT_EQ(program.FindPermutation(runtime_info, profile, draws.start, made), 1u);
    EXPECT_TRUE(made.has_value());
}

// MatchesSharps (#780), for draws whose registers did not change: yes only for user data that
// points to what gives a SameAs specialization, and yes for all of it while the fetch shader's
// words stay the same.
TEST(SpecReuse, MatchesSharpsSeesWhatTheUserDataPointsTo) {
    for (const auto stage : {Kind::Vertex, Kind::Fragment}) {
        Draws draws{stage, 13};
        draws.fixed_registers = true;
        const Shader::Profile profile{};
        auto& info = draws.program.info;
        const auto runtime_info = draws.RuntimeInfo();
        std::optional<Shader::StageSpecialization> last;
        std::vector<u32> last_code;
        u32 same = 0;
        u32 differ = 0;
        for (u32 draw = 0; draw < Draws::NumDraws; ++draw) {
            draws.Randomize();
            ASSERT_TRUE(draws.RuntimeInfo() == runtime_info);
            Shader::StageSpecialization spec{info, runtime_info, profile, draws.start};
            std::vector<u32> code;
            if (!spec.fetch_shader_data.Empty()) {
                const u32* p = Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
                code.assign(p, p + spec.fetch_shader_data.size / sizeof(u32));
            }
            if (last) {
                const bool matches = last->MatchesSharps(info, last_code);
                const bool is_same = spec.SameAs(*last);
                ASSERT_EQ(matches, last->Matches(info, runtime_info, draws.start, last_code));
                if (matches) {
                    ASSERT_TRUE(is_same) << "draw " << draw;
                }
                if (is_same && code == last_code) {
                    ASSERT_TRUE(matches) << "draw " << draw;
                }
                ++(is_same ? same : differ);
            }
            last = std::move(spec);
            last_code = std::move(code);
        }
        EXPECT_GT(same, Draws::NumDraws / 10);
        EXPECT_GT(differ, Draws::NumDraws / 10);
    }
}

// The cases of #780's check one by one: a sharp the shader table points to, a vertex buffer
// sharp and a fetch shader word change in guest memory, with every register the same.
TEST(SpecReuse, MatchesSharpsFailsWhenGuestMemoryChanged) {
    const Shader::Profile profile{};
    {
        Draws draws{Kind::Fragment, 17};
        auto& info = draws.program.info;
        // Buffer 1 is formatted: its data format is part of the specialization once it is bound.
        AmdGpu::Buffer sharp;
        std::memcpy(&sharp, draws.FlatBuffer(1), sizeof(sharp));
        sharp.num_records = 100;
        std::memcpy(draws.FlatBuffer(1), &sharp, sizeof(sharp));
        const Shader::StageSpecialization spec{info, draws.RuntimeInfo(), profile, draws.start};
        ASSERT_TRUE(spec.MatchesSharps(info, {}));
        sharp.data_format = sharp.data_format == 4 ? 5 : 4;
        std::memcpy(draws.FlatBuffer(1), &sharp, sizeof(sharp));
        EXPECT_FALSE(spec.MatchesSharps(info, {}));
    }
    {
        Draws draws{Kind::Vertex, 19};
        auto& info = draws.program.info;
        AmdGpu::Buffer attrib;
        std::memcpy(&attrib, draws.VertexSharp(0), sizeof(attrib));
        attrib.num_records = 100;
        std::memcpy(draws.VertexSharp(0), &attrib, sizeof(attrib));
        const Shader::StageSpecialization spec{info, draws.RuntimeInfo(), profile, draws.start};
        ASSERT_FALSE(spec.fetch_shader_data.Empty());
        const u32* p = Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
        const std::vector<u32> code(p, p + spec.fetch_shader_data.size / sizeof(u32));
        ASSERT_TRUE(spec.MatchesSharps(info, code));

        const AmdGpu::Buffer saved = attrib;
        // Another number class: the vertex input's type follows it.
        attrib.num_format = AmdGpu::GetNumberClass(attrib.GetNumberFmt()) ==
                                    AmdGpu::NumberClass::Float
                                ? u32(AmdGpu::NumberFormat::Uint)
                                : u32(AmdGpu::NumberFormat::Float);
        std::memcpy(draws.VertexSharp(0), &attrib, sizeof(attrib));
        EXPECT_FALSE(spec.MatchesSharps(info, code));
        std::memcpy(draws.VertexSharp(0), &saved, sizeof(saved));
        ASSERT_TRUE(spec.MatchesSharps(info, code));

        draws.FetchCode()[0] ^= 1u; // the first load's offset
        EXPECT_FALSE(spec.MatchesSharps(info, code));
    }
}
