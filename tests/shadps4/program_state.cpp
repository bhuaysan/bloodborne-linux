#include "shadps4/program_state.hpp"

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include "core/signals.h"
#include "shader_recompiler/frontend/copy_shader.h"
#include "shader_recompiler/frontend/tessellation.h"
#include "shadps4_host/failure.hpp"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/amdgpu/regs_vertex.h"

namespace bb::test::shadps4 {

namespace {

using gcn::Program;
using gcn::ProgramType;

// Draw-time sizes the files don't hold. Four vec4s per vertex and triangles are the common case,
// and neither changes which instructions the translator meets.
constexpr std::uint32_t vertex_size_dwords = 16;
constexpr std::uint32_t control_points = 3;
// Big enough for any ring offset and item size: 32 parameters of 4 components.
constexpr std::uint32_t max_export_dwords = 32 * 4;
constexpr std::uint32_t stride_bytes = vertex_size_dwords * 4;

constexpr std::uint32_t s_setpc_b64_s0 = 0xbe802000;
constexpr std::uint32_t s_swappc_b64_prefix = 0xbe802100;  // the low byte is the source pair

// The called fetch subroutine: returns at once, so no attribute is loaded.
const std::array<std::uint32_t, 1> empty_fetch_shader = {s_setpc_b64_s0};

constexpr std::size_t low_block_align = 0x10000;

// Memory below 4 GiB, 64 KiB aligned. A dword holding such an address, repeated, reads as that
// address both as a pointer (64 bits masked to 48) and as a V# base (44 bits).
void* low_block(std::size_t size) {
    void* memory = mmap(nullptr, size + low_block_align, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (memory == MAP_FAILED) {
        throw std::runtime_error("mmap failed");
    }
    return reinterpret_cast<void*>((reinterpret_cast<std::uintptr_t>(memory) + low_block_align - 1) &
                                   ~(low_block_align - 1));
}

std::uint32_t low_address(const void* block) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(block));
}

// Starts with the constants a hull or domain shader reads through a V#, and every later dword
// holds the block's own address. So a V# passed inline in user data that all hold this address
// has the constants as its base.
struct TessConstants {
    Shader::TessellationDataConstantBuffer constants;
    std::array<std::uint32_t, 0x10000> self_pointers;  // past every offset a shader reads
};

TessConstants* tess_constants() {
    static TessConstants* const data = [] {
        auto* block = new (low_block(sizeof(TessConstants))) TessConstants;
        block->constants = {
            .ls_stride = stride_bytes,
            .hs_cp_stride = stride_bytes,
            .num_patches = 1,
            .hs_output_base = control_points * stride_bytes,
            .patch_const_size = 16,
            .patch_const_base = 2 * control_points * stride_bytes,
            .patch_output_size = control_points * stride_bytes,
            .off_chip_tessellation_factor_threshold = 0.0f,
            .first_edge_tess_factor_index = 0,
        };
        block->self_pointers.fill(low_address(block));
        return block;
    }();
    return data;
}

// A table of V#s of those constants, for a program that reaches its V# through a pointer.
std::uint32_t* tess_v_sharp_table() {
    constexpr std::size_t size_dwords = 0x10000;
    static std::uint32_t* const table = [] {
        auto* dwords = static_cast<std::uint32_t*>(low_block(size_dwords * sizeof(std::uint32_t)));
        std::fill_n(dwords, size_dwords, low_address(tess_constants()));
        return dwords;
    }();
    return table;
}

// Both graphics settings words in the order the program's registers hold them.
struct Settings {
    std::uint32_t num_vgprs : 6, num_sgprs : 4, priority : 2, round32 : 2, round64 : 2,
        denorm32 : 2, denorm64 : 2, : 1, dx10_clamp : 1, debug : 1, ieee : 1, vgpr_comp_cnt : 2,
        : 6;
    std::uint32_t scratch : 1, user_regs : 5, : 1, tgid_enable : 3, : 5, lds_dwords : 9, : 8;
};
static_assert(sizeof(Settings) == 8);

Settings settings_of(const Program& program) {
    Settings settings{};
    if (program.has_block) {
        std::memcpy(&settings, &program.block[2], sizeof(settings));
    } else {
        settings.vgpr_comp_cnt = 3;
        settings.num_vgprs = 63;
    }
    return settings;
}

void fill_common(Shader::RuntimeInfo& info, const Settings& settings) {
    info.props.fp_round_mode32 = static_cast<AmdGpu::FpRoundMode>(settings.round32);
    info.props.fp_round_mode16_64 = static_cast<AmdGpu::FpRoundMode>(settings.round64);
    info.props.fp_denorm_mode32 = static_cast<AmdGpu::FpDenormMode>(settings.denorm32);
    info.props.fp_denorm_mode16_64 = static_cast<AmdGpu::FpDenormMode>(settings.denorm64);
    info.props.num_allocated_vgprs = (settings.num_vgprs + 1) * 4;
    info.props.num_user_data = settings.user_regs;
    info.props.num_input_vgprs = settings.vgpr_comp_cnt;
    info.props.dx10_clamp = settings.dx10_clamp;
}

void fill_pixel(Shader::RuntimeInfo& info, const Program& program) {
    if (program.has_block) {
        info = fragment_runtime_info(program.block);
    } else {
        info = fragment_runtime_info();
    }
    info.hw.fs.z_export_format = static_cast<AmdGpu::ShaderExportFormat>(program.block[4] & 0xf);
    const std::uint32_t db_shader_control = program.block[10];
    const std::uint32_t z = (db_shader_control >> 0) & 1;
    const std::uint32_t stencil = ((db_shader_control >> 6) | (db_shader_control >> 7)) & 1;
    const std::uint32_t mask = (db_shader_control >> 8) & 1;
    const std::uint32_t coverage = (db_shader_control >> 10) & 1;
    info.hw.fs.mrtz_mask = z | (stencil << 1) | (mask << 2) | (coverage << 3);
}

void fill_compute(Shader::RuntimeInfo& info, const Program& program, const Settings& settings) {
    info.props.num_allocated_vgprs = settings.num_vgprs * 4;
    info.hw.cs.shared_memory_size = settings.lds_dwords * 128 * 4;
    info.hw.cs.workgroup_size = {program.block[4], program.block[5], program.block[6]};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        info.hw.cs.tgid_enable[axis] = ((settings.tgid_enable >> axis) & 1) != 0;
    }
}

void fill_geometry(Shader::RuntimeInfo& info, std::span<const std::uint32_t> copy_shader) {
    auto& gs = info.hw.gs;
    // The vertex count is what the copy shader reads back. The ring's item size is draw-time
    // state, so it gets an upper bound: writes the copy shader doesn't read must not wrap around.
    const Shader::CopyShaderData copy = Shader::ParseCopyShader(copy_shader);
    gs.num_invocations = 1;
    gs.output_vertices = copy.output_vertices;
    gs.in_vertex_data_size = max_export_dwords;
    gs.out_vertex_data_size = max_export_dwords;
    gs.in_primitive = AmdGpu::PrimitiveType::TriangleList;
    gs.out_primitive.fill(AmdGpu::GsOutputPrimitiveType::TriangleStrip);
    gs.mode = AmdGpu::GsScenario::ScenarioG;
    gs.vs_copy = copy_shader;
    gs.vs_copy_hash = 1;
}

void fill_tess_control(Shader::RuntimeInfo& info) {
    auto& tcs = info.sw.tcs;
    tcs.hs_output_cp_stride = stride_bytes;
    tcs.num_input_control_points = control_points;
    tcs.num_threads = control_points;
    tcs.tess_type = AmdGpu::TessellationType::Triangle;
    tcs.ls_stride = stride_bytes;
    tcs.hs_output_base = control_points * stride_bytes;
}

void fill_tess_eval(Shader::RuntimeInfo& info) {
    auto& tes = info.sw.tes;
    tes.hs_output_cp_stride = stride_bytes;
    tes.tess_type = AmdGpu::TessellationType::Triangle;
    tes.tess_topology = AmdGpu::TessellationTopology::TriangleCw;
    tes.tess_partitioning = AmdGpu::TessellationPartitioning::Integer;
}

}  // namespace

Shader::RuntimeInfo runtime_info_of(const Program& program,
                                    std::span<const std::uint32_t> copy_shader) {
    using Shader::HwStage;
    using Shader::SwStage;
    Shader::RuntimeInfo info{};
    const Settings settings = settings_of(program);
    switch (static_cast<ProgramType>(program.type)) {
    case ProgramType::ps:
        fill_pixel(info, program);
        break;
    case ProgramType::vs:
    case ProgramType::copy_vs:
        info.Initialize(HwStage::Vertex, SwStage::Vertex);
        break;
    case ProgramType::es:
        info.Initialize(HwStage::Export, SwStage::Vertex);
        info.hw.es.vertex_data_size = max_export_dwords;
        break;
    case ProgramType::ls:
        info.Initialize(HwStage::Local, SwStage::Vertex);
        info.hw.ls.ls_stride = stride_bytes;
        break;
    case ProgramType::cs:
        info.Initialize(HwStage::Compute, SwStage::Compute);
        break;
    case ProgramType::gs:
        info.Initialize(HwStage::Geometry, SwStage::Geometry);
        fill_geometry(info, copy_shader);
        break;
    case ProgramType::hs:
        info.Initialize(HwStage::Hull, SwStage::TessellationControl);
        fill_tess_control(info);
        break;
    case ProgramType::ds:
        info.Initialize(HwStage::Vertex, SwStage::TessellationEval);
        fill_tess_eval(info);
        break;
    }
    fill_common(info, settings);
    if (static_cast<ProgramType>(program.type) == ProgramType::cs) {
        fill_compute(info, program, settings);
    }
    return info;
}

UserData user_data_of(const Program& program, TessConstantsForm form) {
    const auto type = static_cast<ProgramType>(program.type);
    UserData user_data{};
    const auto set_pair = [&](std::size_t reg, const void* target) {
        const auto address = reinterpret_cast<std::uint64_t>(target);
        user_data[reg] = static_cast<std::uint32_t>(address);
        user_data[reg + 1] = static_cast<std::uint32_t>(address >> 32);
    };
    if (type == ProgramType::vs || type == ProgramType::es || type == ProgramType::ls) {
        // s_swappc_b64 names the pair that holds the fetch shader. A literal that looks like one
        // would only put the pointer in a register the program doesn't read as one.
        for (const std::uint32_t word : program.code) {
            if ((word & 0xffffff00) >> 8 == s_swappc_b64_prefix >> 8 && (word & 0xff) + 1 < 16) {
                set_pair(word & 0xff, empty_fetch_shader.data());
                break;
            }
        }
    } else if (type == ProgramType::hs || type == ProgramType::ds) {
        const void* block = form == TessConstantsForm::inline_v_sharp
                                ? static_cast<const void*>(tess_constants())
                                : tess_v_sharp_table();
        user_data.fill(low_address(block));
    }
    return user_data;
}

void report_failure(std::string_view message) {
    std::string line(failed_marker);
    line += message;
    std::replace(line.begin(), line.end(), '\n', ' ');
    line += '\n';
    (void)!write(STDOUT_FILENO, line.data(), line.size());
    _exit(0);
}

void install_walker_fault_handler() {
    struct sigaction action{};
    action.sa_sigaction = [](int, siginfo_t* info, void* context) {
        // The handler asserts when it can't patch a walker, and a throw can't leave a signal
        // handler, so it ends here.
        try {
            if (!Core::Signals::Instance()->DispatchAccessViolation(context, info->si_addr)) {
                signal(SIGSEGV, SIG_DFL);
            }
        } catch (const bb::shader::TranslationFailure& failure) {
            report_failure(failure.what());
        }
    };
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);

    // Sirit aborts on an undefined id instead of asserting, so it would end up as a bare signal.
    signal(SIGABRT, [](int) { report_failure("abort: Sirit met an undefined SPIR-V id"); });
}

}  // namespace bb::test::shadps4
