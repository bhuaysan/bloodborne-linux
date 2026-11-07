#include "gpu/vk/tiler.hpp"

#include <bit>
#include <stdexcept>

#include "gpu/gcn/spirv_builder.hpp"
#include "gpu/vk/device.hpp"

namespace bb::vk {

std::vector<std::uint32_t> tiling_shader(std::uint32_t bytes, bool to_linear) {
    if (!std::has_single_bit(bytes) || bytes > 16) {
        throw std::invalid_argument("tiling_shader: element size not 1, 2, 4, 8 or 16 bytes");
    }
    using gcn::spirv_word;
    using Id = gcn::SpirvBuilder::Id;
    using Op = spv::Op;
    using SC = spv::StorageClass;
    gcn::SpirvBuilder b;
    b.capability(spv::Capability::Shader);
    b.capability(spv::Capability::Int64);
    b.capability(spv::Capability::PhysicalStorageBufferAddresses);
    if (bytes == 1) {
        b.capability(spv::Capability::StorageBuffer8BitAccess);
    } else if (bytes == 2) {
        b.capability(spv::Capability::StorageBuffer16BitAccess);
    }
    b.memory_model(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);

    const Id u32 = b.type_int(32, false);
    const Id u64 = b.type_int(64, false);
    const Id uvec2 = b.type_vector(u32, 2);
    const Id uvec3 = b.type_vector(u32, 3);
    const Id element = bytes <= 4 ? b.type_int(8 * bytes, false) : b.type_vector(u32, bytes / 4);
    const auto c = [&](std::uint32_t value) { return b.constant_u32(value); };

    // TilingConstants: three addresses, then six words.
    const Id constants_t = b.type_struct({u64, u64, u64, u32, u32, u32, u32, u32, u32});
    b.decorate(constants_t, spv::Decoration::Block);
    for (std::uint32_t i = 0; i < 9; ++i) {
        b.member_decorate(constants_t, i, spv::Decoration::Offset, {i < 3 ? 8 * i : 12 + 4 * i});
    }
    const Id constants = b.variable(b.type_pointer(SC::PushConstant, constants_t), SC::PushConstant);
    const Id id_var = b.variable(b.type_pointer(SC::Input, uvec3), SC::Input);
    b.decorate(id_var, spv::Decoration::BuiltIn, {spirv_word(spv::BuiltIn::GlobalInvocationId)});

    const Id void_t = b.type_void();
    const Id main = b.begin_function(void_t, b.type_function(void_t));
    b.begin_block(b.id());
    const auto constant = [&](std::uint32_t member) {
        const Id type = member < 3 ? u64 : u32;
        return b.op(Op::OpLoad, type,
                    {b.op(Op::OpAccessChain, b.type_pointer(SC::PushConstant, type),
                          {constants, c(member)})});
    };
    const auto op = [&](Op opcode, Id type, Id x, Id y) { return b.op(opcode, type, {x, y}); };
    const Id id = b.op(Op::OpLoad, uvec3, {id_var});
    const Id x = b.op(Op::OpCompositeExtract, u32, {id, 0});
    const Id y = b.op(Op::OpCompositeExtract, u32, {id, 1});
    const Id z = b.op(Op::OpCompositeExtract, u32, {id, 2});
    const Id pitch = constant(3);
    const Id height = constant(4);
    const Id bool_t = b.type_bool();
    const Id inside = op(Op::OpLogicalAnd, bool_t, op(Op::OpULessThan, bool_t, x, pitch),
                         op(Op::OpULessThan, bool_t, y, height));
    const Id body = b.id();
    const Id merge = b.id();
    b.op_void(Op::OpSelectionMerge, {merge, spirv_word(spv::SelectionControlMask::MaskNone)});
    b.op_void(Op::OpBranchConditional, {inside, body, merge});
    b.begin_block(body);

    // Loads or stores through a device address with the alignment of the type.
    const auto pointer = [&](Id type, Id address) {
        return b.op(Op::OpConvertUToPtr, b.type_pointer(SC::PhysicalStorageBuffer, type),
                    {address});
    };
    const auto at = [&](Id base, Id offset) {
        return op(Op::OpIAdd, u64, base, b.op(Op::OpUConvert, u64, {offset}));
    };
    const Id tables = constant(2);
    const auto entry = [&](Id index) {
        return b.op(Op::OpLoad, uvec2,
                    {pointer(uvec2, at(tables, op(Op::OpShiftLeftLogical, u32, index, c(3)))),
                     spirv_word(spv::MemoryAccessMask::Aligned), 8});
    };
    const Id col = entry(x);
    const Id row = entry(op(Op::OpIAdd, u32, pitch, y));
    const Id layer = entry(op(Op::OpIAdd, u32, op(Op::OpIAdd, u32, pitch, height), z));
    const auto part = [&](Id e, std::uint32_t i) { return b.op(Op::OpCompositeExtract, u32, {e, i}); };

    // bb::tile::combine without the level's offset.
    const Id split = op(Op::OpIAdd, u32, op(Op::OpShiftRightLogical, u32, part(col, 1), c(16)),
                        op(Op::OpShiftRightLogical, u32, part(row, 1), c(16)));
    const Id total =
        op(Op::OpIAdd, u32,
           op(Op::OpIAdd, u32, op(Op::OpIAdd, u32, part(col, 0), part(row, 0)), part(layer, 0)),
           op(Op::OpIMul, u32, split, constant(5)));
    const Id pipe_bank = op(
        Op::OpBitwiseAnd, u32,
        op(Op::OpBitwiseXor, u32,
           op(Op::OpBitwiseXor, u32, op(Op::OpBitwiseXor, u32, part(col, 1), part(row, 1)),
              part(layer, 1)),
           op(Op::OpIMul, u32, split, constant(6))),
        constant(8));
    const Id high = op(Op::OpShiftLeftLogical, u32, op(Op::OpShiftRightLogical, u32, total, c(8)),
                       op(Op::OpIAdd, u32, constant(7), c(8)));
    const Id tiled_offset =
        op(Op::OpBitwiseOr, u32,
           op(Op::OpBitwiseOr, u32, op(Op::OpBitwiseAnd, u32, total, c(255)),
              op(Op::OpShiftLeftLogical, u32, pipe_bank, c(8))),
           high);
    // ((z * height + y) * pitch + x) * bytes
    const Id linear_offset = op(
        Op::OpIMul, u32,
        op(Op::OpIAdd, u32,
           op(Op::OpIMul, u32, op(Op::OpIAdd, u32, op(Op::OpIMul, u32, z, height), y), pitch), x),
        c(bytes));
    const Id tiled = at(constant(0), tiled_offset);
    const Id linear = at(constant(1), linear_offset);
    const Id value = b.op(Op::OpLoad, element,
                          {pointer(element, to_linear ? tiled : linear),
                           spirv_word(spv::MemoryAccessMask::Aligned), bytes});
    b.op_void(Op::OpStore, {pointer(element, to_linear ? linear : tiled), value,
                            spirv_word(spv::MemoryAccessMask::Aligned), bytes});
    b.op_void(Op::OpBranch, {merge});
    b.begin_block(merge);
    b.op_void(Op::OpReturn);
    b.end_function();
    b.entry_point(spv::ExecutionModel::GLCompute, main, "main", {constants, id_var});
    b.execution_mode(main, spv::ExecutionMode::LocalSize, {8, 8, 1});
    return b.words();
}

std::vector<std::uint32_t> table_words(const tile::AddressTables& t) {
    std::vector<std::uint32_t> words;
    words.reserve(2 * (t.columns.size() + t.rows.size() + t.layers.size()));
    for (const auto* part : {&t.columns, &t.rows, &t.layers}) {
        for (const auto& entry : *part) {
            words.insert(words.end(), entry.begin(), entry.end());
        }
    }
    return words;
}

Tiler::Tiler(const Device& device) : device_(device.handle()) {
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TilingConstants)};
    auto info = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device_, &info, nullptr, &layout_), "vkCreatePipelineLayout");
}

Tiler::~Tiler() {
    for (const auto& pair : pipelines_) {
        for (const VkPipeline pipeline : pair) {
            vkDestroyPipeline(device_, pipeline, nullptr);
        }
    }
    vkDestroyPipelineLayout(device_, layout_, nullptr);
}

void Tiler::record(VkCommandBuffer commands, const tile::Layout& layout, std::uint32_t level,
                   const tile::AddressTables& tables, const Addresses& at, bool to_linear) {
    const tile::Level& lv = layout.levels.at(level);
    VkPipeline& pipeline = pipelines_.at(std::countr_zero(layout.bytes)).at(to_linear);
    if (pipeline == VK_NULL_HANDLE) {
        const std::vector<std::uint32_t> spirv = tiling_shader(layout.bytes, to_linear);
        auto module_info =
            make<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        module_info.codeSize = 4 * spirv.size();
        module_info.pCode = spirv.data();
        VkShaderModule module = VK_NULL_HANDLE;
        check(vkCreateShaderModule(device_, &module_info, nullptr, &module),
              "vkCreateShaderModule");
        auto info =
            make<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        info.stage = make<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = layout_;
        const VkResult result =
            vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        vkDestroyShaderModule(device_, module, nullptr);
        check(result, "vkCreateComputePipelines");
    }
    const TilingConstants constants{
        .tiled = at.tiled + lv.offset,
        .linear = at.linear + lv.offset,
        .tables = at.tables,
        .pitch = lv.pitch,
        .height = lv.height,
        .split_bytes = tables.split_bytes,
        .split_rotation = tables.split_rotation,
        .pipe_bank_bits = tables.pipe_bank_bits,
        .pipe_bank_mask = tables.pipe_bank_mask,
    };
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof constants,
                       &constants);
    vkCmdDispatch(commands, (lv.pitch + 7) / 8, (lv.height + 7) / 8, layout.layers);
}

}  // namespace bb::vk
