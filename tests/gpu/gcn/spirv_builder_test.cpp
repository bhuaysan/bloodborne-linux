// SPIR-V builder: the word encoding, sharing of types and constants, and misuse. Validation with
// spirv-val follows with the modules of each stage.

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/spirv_builder.hpp"

namespace {

using bb::gcn::spirv_word;
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

// Names, decorations and constants, called out of order, land in their sections. Strings are
// padded to whole words.
TEST(SpirvBuilder, EncodesNamesDecorationsAndConstants) {
    SpirvBuilder b;
    b.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const Id one = b.constant_f32(1.0f);  // %1 is its type
    const Id minus_one = b.constant_i32(-1);
    b.decorate(7, spv::Decoration::BuiltIn, {spirv_word(spv::BuiltIn::LocalInvocationIndex)});
    b.name(7, "");
    b.name(7, "abc");
    b.name(7, "abcd");
    EXPECT_EQ(one, 2u);
    EXPECT_EQ(minus_one, 4u);
    const std::vector<std::uint32_t> expected = {
        head(3, 14), 0, 1,                        // OpMemoryModel
        head(3, 5), 7, 0,                         // OpName %7 ""
        head(3, 5), 7, 0x00636261,                // "abc" and its nul share a word
        head(4, 5), 7, 0x64636261, 0,             // "abcd" needs a word for the nul
        head(4, 71), 7, 11, 29,                   // OpDecorate %7 BuiltIn LocalInvocationIndex
        head(3, 22), 1, 32,                       // %1 = OpTypeFloat 32
        head(4, 43), 1, 2, 0x3f800000,            // %2 = OpConstant %1 1.0
        head(4, 21), 3, 32, 1,                    // %3 = OpTypeInt 32 1
        head(4, 43), 3, 4, 0xffffffff,            // %4 = OpConstant %3 -1
    };
    const auto words = b.words();
    EXPECT_EQ(std::vector<std::uint32_t>(words.begin() + 5, words.end()), expected);
}

TEST(SpirvBuilder, SharesTypesAndConstants) {
    SpirvBuilder b;
    const Id u32 = b.type_int(32, false);
    EXPECT_EQ(b.type_int(32, false), u32);
    EXPECT_NE(b.type_int(32, true), u32);
    EXPECT_EQ(b.type_pointer(SC::Function, u32), b.type_pointer(SC::Function, u32));
    EXPECT_NE(b.type_pointer(SC::Private, u32), b.type_pointer(SC::Function, u32));
    EXPECT_EQ(b.constant_u32(7), b.constant_u32(7));
    EXPECT_NE(b.constant_i32(7), b.constant_u32(7));
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
