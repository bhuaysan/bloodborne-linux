// SPIR-V builder: the word encoding, sharing of types and constants, and misuse. Validation with
// spirv-val follows with the modules of each stage.

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/spirv_builder.hpp"

namespace {

using bb::gcn::SpirvBuilder;
using Id = SpirvBuilder::Id;
using Op = spv::Op;
using SC = spv::StorageClass;

// The first word of an instruction.
constexpr std::uint32_t head(std::uint32_t count, std::uint32_t opcode) {
    return count << 16 | opcode;
}

// An empty `void main()` compute shader with 64 invocations.
Id empty_compute(SpirvBuilder& b) {
    b.capability(spv::Capability::Shader);
    b.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const Id void_t = b.type_void();
    const Id main = b.begin_function(void_t, b.type_function(void_t));
    b.begin_block(b.id());
    b.op_void(Op::OpReturn);
    b.end_function();
    b.entry_point(spv::ExecutionModel::GLCompute, main, "main", {});
    b.execution_mode(main, spv::ExecutionMode::LocalSize, {64, 1, 1});
    return main;
}

// The words below are worked out from the SPIR-V specification's binary form.
TEST(SpirvBuilder, EncodesTheModule) {
    SpirvBuilder b;
    empty_compute(b);
    const std::vector<std::uint32_t> expected = {
        0x07230203, 0x00010600, 0, 5, 0,           // magic, 1.6, generator, bound, schema
        head(2, 17), 1,                            // OpCapability Shader
        head(3, 14), 0, 1,                         // OpMemoryModel Logical GLSL450
        head(5, 15), 5, 3, 0x6e69616d, 0,          // OpEntryPoint GLCompute %3 "main"
        head(6, 16), 3, 17, 64, 1, 1,              // OpExecutionMode %3 LocalSize 64 1 1
        head(2, 19), 1,                            // %1 = OpTypeVoid
        head(3, 33), 2, 1,                         // %2 = OpTypeFunction %1
        head(5, 54), 1, 3, 0, 2,                   // %3 = OpFunction %1 None %2
        head(2, 248), 4,                           // %4 = OpLabel
        head(1, 253),                              // OpReturn
        head(1, 56),                               // OpFunctionEnd
    };
    EXPECT_EQ(b.words(), expected);
}

// A function with a parameter and an instruction with a result, the vector and pointer types,
// the bool and u32 constants, and an entry point with interface ids.
TEST(SpirvBuilder, EncodesInstructionsTypesAndInterface) {
    SpirvBuilder b;
    b.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const Id u32 = b.type_int(32, false);
    const Id add_two = b.begin_function(u32, b.type_function(u32, {u32}));
    const Id a = b.parameter(u32);
    b.begin_block(b.id());
    const Id two = b.constant_u32(2);
    const Id sum = b.op(Op::OpIAdd, u32, {a, two});
    b.op_void(Op::OpReturnValue, {sum});
    b.end_function();
    const Id uvec4 = b.type_vector(u32, 4);
    b.type_pointer(SC::Private, uvec4);
    b.constant_bool(true);
    b.constant_bool(false);
    const Id input = b.id();
    const Id output = b.id();
    b.entry_point(spv::ExecutionModel::Fragment, add_two, "f", {input, output});
    const std::vector<std::uint32_t> expected = {
        0x07230203, 0x00010600, 0, 15, 0,          // header, bound 15
        head(3, 14), 0, 1,                         // OpMemoryModel Logical GLSL450
        head(6, 15), 4, 3, 0x00000066, 13, 14,     // OpEntryPoint Fragment %3 "f" %13 %14
        head(4, 21), 1, 32, 0,                     // %1 = OpTypeInt 32 0
        head(4, 33), 2, 1, 1,                      // %2 = OpTypeFunction %1 %1
        head(4, 43), 1, 6, 2,                      // %6 = OpConstant %1 2
        head(4, 23), 8, 1, 4,                      // %8 = OpTypeVector %1 4
        head(4, 32), 9, 6, 8,                      // %9 = OpTypePointer Private %8
        head(2, 20), 10,                           // %10 = OpTypeBool
        head(3, 41), 10, 11,                       // %11 = OpConstantTrue %10
        head(3, 42), 10, 12,                       // %12 = OpConstantFalse %10
        head(5, 54), 1, 3, 0, 2,                   // %3 = OpFunction %1 None %2
        head(3, 55), 1, 4,                         // %4 = OpFunctionParameter %1
        head(2, 248), 5,                           // %5 = OpLabel
        head(5, 128), 1, 7, 4, 6,                  // %7 = OpIAdd %1 %4 %6
        head(2, 254), 7,                           // OpReturnValue %7
        head(1, 56),                               // OpFunctionEnd
    };
    EXPECT_EQ(b.words(), expected);
}

TEST(SpirvBuilder, SharesTypesAndConstants) {
    SpirvBuilder b;
    const Id u32 = b.type_int(32, false);
    EXPECT_EQ(b.type_int(32, false), u32);
    EXPECT_NE(b.type_int(32, true), u32);
    EXPECT_EQ(b.type_pointer(SC::Function, u32), b.type_pointer(SC::Function, u32));
    EXPECT_NE(b.type_pointer(SC::Private, u32), b.type_pointer(SC::Function, u32));
    EXPECT_EQ(b.constant_u32(7), b.constant_u32(7));
    EXPECT_NE(b.constant(b.type_int(32, true), {7}), b.constant_u32(7));
    EXPECT_EQ(b.constant_bool(true), b.constant_bool(true));
    EXPECT_NE(b.constant_bool(false), b.constant_bool(true));
    EXPECT_EQ(b.type_function(u32, {u32}), b.type_function(u32, {u32}));

    b.capability(spv::Capability::Shader);
    b.capability(spv::Capability::Shader);
    b.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const auto words = b.words();
    EXPECT_EQ(words[5], head(2, 17));
    EXPECT_EQ(words[7], head(3, 14));  // the memory model right after one OpCapability
}

TEST(SpirvBuilder, RejectsMisuse) {
    SpirvBuilder b;
    const Id void_t = b.type_void();
    const Id u32 = b.type_int(32, false);
    EXPECT_THROW(b.op(Op::OpUndef, u32, {}), std::logic_error);  // outside a function
    EXPECT_THROW(b.words(), std::logic_error);  // no memory model
    b.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);

    b.begin_function(void_t, b.type_function(void_t, {u32}));
    b.parameter(u32);
    EXPECT_THROW(b.begin_function(void_t, b.type_function(void_t)), std::logic_error);
    EXPECT_THROW(b.op(Op::OpUndef, u32, {}), std::logic_error);  // before the first block
    EXPECT_THROW(b.end_function(), std::logic_error);            // without a block
    b.begin_block(b.id());
    EXPECT_THROW(b.parameter(u32), std::logic_error);
    EXPECT_THROW(b.begin_block(b.id()), std::logic_error);  // the block has no terminator
    EXPECT_THROW(b.end_function(), std::logic_error);
    EXPECT_THROW(b.words(), std::logic_error);  // unfinished function
    b.op_void(Op::OpReturn);
    EXPECT_THROW(b.op(Op::OpUndef, u32, {}), std::logic_error);  // after the terminator
    b.end_function();
    EXPECT_NO_THROW(b.words());
}

}  // namespace
