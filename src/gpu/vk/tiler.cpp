#include "gpu/vk/tiler.hpp"

#include <bit>
#include <stdexcept>

#include "gpu/gcn/spirv_builder.hpp"

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

}  // namespace bb::vk
