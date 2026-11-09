#include "shadps4/translate.hpp"

#include <array>
#include <bit>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/profile.h"

namespace bb::test::shadps4 {

namespace {

Shader::Profile reference_profile() {
    return Shader::Profile{
        .max_viewport_width = 16384,
        .max_viewport_height = 16384,
        .max_shared_memory_size = 65536,
        .supported_spirv = 0x00010600,
        .subgroup_size = 64,
        .support_int8 = true,
        .support_int16 = true,
        .support_int64 = true,
        .support_float16 = true,
        .support_float64 = true,
        .supports_image_load_store_lod = true,
        .supports_trinary_minmax = true,
        .supports_fragment_shader_barycentric = true,
    };
}

}  // namespace

Shader::RuntimeInfo fragment_runtime_info() {
    Shader::RuntimeInfo info{};
    info.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    info.hw.fs.color_buffers[0].data_format = AmdGpu::DataFormat::Format8_8_8_8;
    info.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Unorm;
    info.hw.fs.color_buffers[0].export_format = AmdGpu::ShaderExportFormat::ABGR_32;
    return info;
}

Shader::RuntimeInfo fragment_runtime_info(const std::array<std::uint32_t, 16>& block) {
    const std::uint32_t col_format = block[5];
    const std::uint32_t ps_input_ena = block[6];
    const std::uint32_t ps_input_addr = block[7];
    const std::uint32_t num_interp = block[8] & 0x3f;

    Shader::RuntimeInfo info = fragment_runtime_info();
    for (std::size_t target = 0; target < info.hw.fs.color_buffers.size(); ++target) {
        info.hw.fs.color_buffers[target].export_format =
            static_cast<AmdGpu::ShaderExportFormat>((col_format >> (target * 4)) & 0xf);
    }
    info.hw.fs.en_flags = std::bit_cast<AmdGpu::PsInput>(ps_input_ena);
    info.hw.fs.addr_flags = std::bit_cast<AmdGpu::PsInput>(ps_input_addr);
    info.hw.fs.num_inputs = num_interp;
    for (std::uint32_t input = 0; input < num_interp && input < info.hw.fs.inputs.size(); ++input) {
        info.hw.fs.inputs[input].param_index = input;
    }
    return info;
}

std::vector<std::uint32_t> translate(std::span<const std::uint32_t> code,
                                     Shader::RuntimeInfo runtime_info, Shader::Pools& pools) {
    static const std::array<std::uint32_t, Shader::ShaderParams::NumShaderUserData> user_data{};
    const Shader::ShaderParams params{.user_data = user_data, .code = code, .hash = 0};
    Shader::Info info(runtime_info.hw_stage, runtime_info.sw_stage, params);
    const Shader::Profile profile = reference_profile();
    const Shader::IR::Program program =
        Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    Shader::Backend::Bindings bindings{};
    return Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
}

std::vector<std::uint32_t> translate(std::span<const std::uint32_t> code,
                                     Shader::RuntimeInfo runtime_info) {
    Shader::Pools pools;
    return translate(code, runtime_info, pools);
}

}  // namespace bb::test::shadps4
