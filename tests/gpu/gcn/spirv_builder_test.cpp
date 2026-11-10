// SPIR-V builder: the word encoding, sharing of types and constants, misuse, and a compute and a
// fragment module shaped like the translator's output, validated by spirv-val. The word-encoding
// tests build fragments that spirv-val would reject (no capability, undefined ids), so they compare
// words only; every helper they use also appears in a validated module.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <spirv/unified1/GLSL.std.450.h>

#include "gpu/gcn/spirv_builder.hpp"
#include "support/spirv_val.hpp"

namespace {

using bb::gcn::spirv_word;
using bb::gcn::SpirvBuilder;
using bb::test::gcn::have_spirv_val;
using bb::test::gcn::validate;
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
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    EXPECT_EQ(validate(b.words()), "");
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

// Declarations called out of order land in their sections. Strings are padded to whole words.
TEST(SpirvBuilder, EncodesDeclarationSections) {
    SpirvBuilder b;
    b.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const Id one = b.constant_f32(1.0f);  // %1 is its type
    const Id minus_one = b.constant_i32(-1);
    b.decorate(7, spv::Decoration::BuiltIn, {spirv_word(spv::BuiltIn::LocalInvocationIndex)});
    b.member_decorate(7, 1, spv::Decoration::Offset, {16});
    b.name(7, "");
    b.name(7, "abc");
    b.name(7, "abcd");
    const Id glsl = b.import_extended("GLSL.std.450");
    b.extension("ab");
    EXPECT_EQ(one, 2u);
    EXPECT_EQ(minus_one, 4u);
    EXPECT_EQ(glsl, 5u);
    const std::vector<std::uint32_t> expected = {
        head(2, 10), 0x00006261,                  // OpExtension "ab"
        head(6, 11), 5, 0x4c534c47, 0x6474732e,   // %5 = OpExtInstImport "GLSL.std.450"
        0x3035342e, 0,                            //
        head(3, 14), 0, 1,                        // OpMemoryModel
        head(3, 5), 7, 0,                         // OpName %7 ""
        head(3, 5), 7, 0x00636261,                // "abc" and its nul share a word
        head(4, 5), 7, 0x64636261, 0,             // "abcd" needs a word for the nul
        head(4, 71), 7, 11, 29,                   // OpDecorate %7 BuiltIn LocalInvocationIndex
        head(5, 72), 7, 1, 35, 16,                // OpMemberDecorate %7 1 Offset 16
        head(3, 22), 1, 32,                       // %1 = OpTypeFloat 32
        head(4, 43), 1, 2, 0x3f800000,            // %2 = OpConstant %1 1.0
        head(4, 21), 3, 32, 1,                    // %3 = OpTypeInt 32 1
        head(4, 43), 3, 4, 0xffffffff,            // %4 = OpConstant %3 -1
    };
    const auto words = b.words();
    EXPECT_EQ(std::vector<std::uint32_t>(words.begin() + 5, words.end()), expected);
}

TEST(SpirvBuilder, SharesTypesAndConstantsButNotAggregates) {
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
    const Id uvec2 = b.type_vector(u32, 2);
    EXPECT_EQ(b.constant_composite(uvec2, {b.constant_u32(1), b.constant_u32(2)}),
              b.constant_composite(uvec2, {b.constant_u32(1), b.constant_u32(2)}));
    EXPECT_EQ(b.constant_null(uvec2), b.constant_null(uvec2));
    EXPECT_NE(b.type_struct({u32}), b.type_struct({u32}));
    EXPECT_NE(b.type_array(u32, b.constant_u32(4)), b.type_array(u32, b.constant_u32(4)));
    EXPECT_NE(b.type_runtime_array(u32), b.type_runtime_array(u32));
    EXPECT_EQ(b.import_extended("GLSL.std.450"), b.import_extended("GLSL.std.450"));
    const Id image = b.type_image(b.type_float(32), spv::Dim::Dim2D, 0, false, false, 1);
    EXPECT_EQ(b.type_image(b.type_float(32), spv::Dim::Dim2D, 0, false, false, 1), image);
    EXPECT_NE(b.type_image(b.type_float(32), spv::Dim::Dim2D, 0, true, false, 1), image);
    EXPECT_EQ(b.type_sampled_image(image), b.type_sampled_image(image));
    EXPECT_EQ(b.type_sampler(), b.type_sampler());

    b.capability(spv::Capability::Shader);
    b.capability(spv::Capability::Shader);
    b.extension("ab");
    b.extension("ab");
    b.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const auto words = b.words();
    EXPECT_EQ(words[5], head(2, 17));   // one OpCapability
    EXPECT_EQ(words[7], head(2, 10));   // one OpExtension
    EXPECT_EQ(words[9], head(6, 11));   // one OpExtInstImport
    EXPECT_EQ(words[15], head(3, 14));  // then the memory model
}

TEST(SpirvBuilder, RejectsMisuse) {
    SpirvBuilder b;
    const Id void_t = b.type_void();
    const Id u32 = b.type_int(32, false);
    EXPECT_THROW(b.op(Op::OpUndef, u32, {}), std::logic_error);  // outside a function
    EXPECT_THROW(b.local_variable(b.type_pointer(SC::Function, u32)), std::logic_error);
    EXPECT_THROW(b.variable(b.type_pointer(SC::Function, u32), SC::Function), std::logic_error);
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

TEST(SpirvBuilder, ValidatorRejectsAnInvalidModule) {
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    SpirvBuilder b;
    b.capability(spv::Capability::Shader);
    b.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const Id void_t = b.type_void();
    const Id main = b.begin_function(void_t, b.type_function(void_t));
    b.begin_block(b.id());
    // Needs the GroupNonUniform capability, which the module doesn't declare.
    b.op(Op::OpGroupNonUniformElect, b.type_bool(),
         {b.constant_u32(spirv_word(spv::Scope::Subgroup))});
    b.op_void(Op::OpReturn);
    b.end_function();
    b.entry_point(spv::ExecutionModel::GLCompute, main, "main", {});
    b.execution_mode(main, spv::ExecutionMode::LocalSize, {64, 1, 1});
    const std::string message = validate(b.words());
    EXPECT_NE(message.find("GroupNonUniform"), std::string::npos) << message;
}

// A compute shader with the pieces the translator uses: the draw block through a push constant
// and a buffer device address, subgroup operations, LDS, registers as local variables, an
// extended instruction, float and signed arithmetic, a function call, a selection and a loop
// with a phi.
TEST(SpirvBuilder, ComputeModuleValidates) {
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    SpirvBuilder b;
    for (auto cap : {spv::Capability::Shader, spv::Capability::Int64,
                     spv::Capability::PhysicalStorageBufferAddresses,
                     spv::Capability::GroupNonUniform, spv::Capability::GroupNonUniformBallot,
                     spv::Capability::GroupNonUniformShuffle}) {
        b.capability(cap);
    }
    // Core since SPIR-V 1.5, but still a valid declaration.
    b.extension("SPV_KHR_physical_storage_buffer");
    b.memory_model(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
    const Id glsl = b.import_extended("GLSL.std.450");

    const Id void_t = b.type_void();
    const Id bool_t = b.type_bool();
    const Id u32 = b.type_int(32, false);
    const Id i32 = b.type_int(32, true);
    const Id u64 = b.type_int(64, false);
    const Id f32 = b.type_float(32);
    const Id uvec4 = b.type_vector(u32, 4);
    const Id c0 = b.constant_u32(0);
    const Id c1 = b.constant_u32(1);
    const Id c4 = b.constant_u32(4);
    const Id c63 = b.constant_u32(63);
    const Id subgroup = b.constant_u32(spirv_word(spv::Scope::Subgroup));

    const Id draw_t = b.type_struct({u64});
    b.decorate(draw_t, spv::Decoration::Block);
    b.member_decorate(draw_t, 0, spv::Decoration::Offset, {0});
    const Id draw = b.variable(b.type_pointer(SC::PushConstant, draw_t), SC::PushConstant);
    b.name(draw, "draw");

    const Id words_t = b.type_runtime_array(u32);
    b.decorate(words_t, spv::Decoration::ArrayStride, {4});
    const Id buffer_t = b.type_struct({words_t});
    b.decorate(buffer_t, spv::Decoration::Block);
    b.member_decorate(buffer_t, 0, spv::Decoration::Offset, {0});
    const Id buffer_ptr = b.type_pointer(SC::PhysicalStorageBuffer, buffer_t);

    const Id index_var = b.variable(b.type_pointer(SC::Input, u32), SC::Input);
    b.decorate(index_var, spv::Decoration::BuiltIn,
               {spirv_word(spv::BuiltIn::LocalInvocationIndex)});
    const Id lds = b.variable(b.type_pointer(SC::Workgroup, b.type_array(u32, b.constant_u32(64))),
                              SC::Workgroup);
    // A saved lane mask, as a ballot.
    const Id mask = b.variable(b.type_pointer(SC::Private, uvec4), SC::Private,
                               b.constant_composite(uvec4, {c0, c1, c0, c1}));

    // A helper function with a parameter, called from main.
    const Id plus_one = b.begin_function(u32, b.type_function(u32, {u32}));
    const Id x = b.parameter(u32);
    b.begin_block(b.id());
    b.op_void(Op::OpReturnValue, {b.op(Op::OpIAdd, u32, {x, c1})});
    b.end_function();

    const Id main = b.begin_function(void_t, b.type_function(void_t));
    const Id u32_fn = b.type_pointer(SC::Function, u32);
    const Id v0 = b.local_variable(u32_fn, c0);
    b.begin_block(b.id());
    const Id index = b.op(Op::OpLoad, u32, {index_var});
    const Id address_ptr = b.op(Op::OpAccessChain, b.type_pointer(SC::PushConstant, u64),
                                {draw, c0});
    const Id buffer = b.op(Op::OpConvertUToPtr, buffer_ptr,
                           {b.op(Op::OpLoad, u64, {address_ptr})});
    const Id lane = b.op(Op::OpBitwiseAnd, u32, {index, c63});
    b.op_void(Op::OpStore, {v0, lane});
    const Id below_32 = b.op(Op::OpULessThan, bool_t, {lane, b.constant_u32(32)});
    const Id active = b.op(Op::OpLogicalAnd, bool_t, {below_32, b.constant_bool(true)});
    const Id ballot = b.op(Op::OpGroupNonUniformBallot, uvec4, {subgroup, active});
    b.op_void(Op::OpStore, {mask, ballot});
    const Id first = b.op(Op::OpGroupNonUniformBallotFindLSB, u32, {subgroup, ballot});
    // A dynamic lane id (SPIR-V 1.5), as v_readfirstlane_b32 needs it.
    const Id read = b.op(Op::OpGroupNonUniformBroadcast, u32, {subgroup, lane, first});
    const Id swizzled = b.op(Op::OpGroupNonUniformShuffle, u32,
                             {subgroup, lane, b.op(Op::OpBitwiseXor, u32, {lane, c1})});
    b.op_void(Op::OpStore,
              {b.op(Op::OpAccessChain, b.type_pointer(SC::Workgroup, u32), {lds, lane}), swizzled});
    const Id lsb = b.op(Op::OpExtInst, u32, {glsl, GLSLstd450FindILsb, read});
    // Declared late, emitted at the start of the first block.
    const Id v1 = b.local_variable(u32_fn, b.constant_null(u32));
    // (u32(f32(lsb) * 0.5) as i32) - 1, stored back as u32.
    const Id half = b.op(Op::OpFMul, f32,
                         {b.op(Op::OpConvertUToF, f32, {lsb}), b.constant_f32(0.5f)});
    const Id as_signed = b.op(Op::OpBitcast, i32, {b.op(Op::OpConvertFToU, u32, {half})});
    const Id decremented = b.op(Op::OpIAdd, i32, {as_signed, b.constant_i32(-1)});
    b.op_void(Op::OpStore, {v1, b.op(Op::OpBitcast, u32, {decremented})});

    const Id then_block = b.id();
    const Id merge = b.id();
    const Id is_lane0 = b.op(Op::OpIEqual, bool_t, {lane, c0});
    b.op_void(Op::OpSelectionMerge, {merge, spirv_word(spv::SelectionControlMask::MaskNone)});
    b.op_void(Op::OpBranchConditional, {is_lane0, then_block, merge});
    b.begin_block(then_block);
    b.op_void(Op::OpStore, {v0, read});
    b.op_void(Op::OpBranch, {merge});
    b.begin_block(merge);

    const Id header = b.id(), body = b.id(), cont = b.id(), exit = b.id(), next = b.id();
    b.op_void(Op::OpBranch, {header});
    b.begin_block(header);
    const Id i = b.op(Op::OpPhi, u32, {c0, merge, next, cont});
    const Id more = b.op(Op::OpULessThan, bool_t, {i, c4});
    b.op_void(Op::OpLoopMerge, {exit, cont, spirv_word(spv::LoopControlMask::MaskNone)});
    b.op_void(Op::OpBranchConditional, {more, body, exit});
    b.begin_block(body);
    b.op_void(Op::OpBranch, {cont});
    b.begin_block(cont);
    b.op(Op::OpFunctionCall, u32, {plus_one, i}, next);
    b.op_void(Op::OpBranch, {header});
    b.begin_block(exit);
    const Id out = b.op(Op::OpAccessChain, b.type_pointer(SC::PhysicalStorageBuffer, u32),
                        {buffer, c0, index});
    b.op_void(Op::OpStore, {out, b.op(Op::OpLoad, u32, {v0}),
                            spirv_word(spv::MemoryAccessMask::Aligned), 4});
    b.op_void(Op::OpReturn);
    b.end_function();

    b.entry_point(spv::ExecutionModel::GLCompute, main, "main", {draw, index_var, lds, mask});
    b.execution_mode(main, spv::ExecutionMode::LocalSize, {64, 1, 1});
    EXPECT_EQ(validate(b.words()), "");
}

// A fragment shader with an interpolated input, two render target outputs, a texture sampled
// through a separate image and sampler, and a demote to helper invocation under a condition.
TEST(SpirvBuilder, FragmentModuleValidates) {
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    SpirvBuilder b;
    b.capability(spv::Capability::Shader);
    b.capability(spv::Capability::DemoteToHelperInvocation);
    b.memory_model(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const Id f32 = b.type_float(32);
    const Id vec2 = b.type_vector(f32, 2);
    const Id vec4 = b.type_vector(f32, 4);
    const Id image_t = b.type_image(f32, spv::Dim::Dim2D, 0, false, false, 1);
    const Id image = b.variable(b.type_pointer(SC::UniformConstant, image_t), SC::UniformConstant);
    const Id sampler = b.variable(b.type_pointer(SC::UniformConstant, b.type_sampler()),
                                  SC::UniformConstant);
    b.decorate(image, spv::Decoration::DescriptorSet, {0});
    b.decorate(image, spv::Decoration::Binding, {0});
    b.decorate(sampler, spv::Decoration::DescriptorSet, {0});
    b.decorate(sampler, spv::Decoration::Binding, {1});
    const Id coord = b.variable(b.type_pointer(SC::Input, vec2), SC::Input);
    b.decorate(coord, spv::Decoration::Location, {0});
    b.decorate(coord, spv::Decoration::NoPerspective);
    Id out[2];
    for (std::uint32_t n = 0; n < 2; ++n) {
        out[n] = b.variable(b.type_pointer(SC::Output, vec4), SC::Output);
        b.decorate(out[n], spv::Decoration::Location, {n});
    }

    const Id void_t = b.type_void();
    const Id main = b.begin_function(void_t, b.type_function(void_t));
    b.begin_block(b.id());
    const Id uv = b.op(Op::OpLoad, vec2, {coord});
    const Id sampled = b.op(Op::OpSampledImage, b.type_sampled_image(image_t),
                            {b.op(Op::OpLoad, image_t, {image}),
                             b.op(Op::OpLoad, b.type_sampler(), {sampler})});
    const Id texel = b.op(Op::OpImageSampleImplicitLod, vec4, {sampled, uv});
    b.op_void(Op::OpStore, {out[0], texel});
    b.op_void(Op::OpStore, {out[1], b.constant_null(vec4)});
    const Id then = b.id(), merge = b.id();
    const Id u = b.op(Op::OpCompositeExtract, f32, {uv, 0});
    const Id left = b.op(Op::OpFOrdLessThan, b.type_bool(), {u, b.constant_f32(0.5f)});
    b.op_void(Op::OpSelectionMerge, {merge, spirv_word(spv::SelectionControlMask::MaskNone)});
    b.op_void(Op::OpBranchConditional, {left, then, merge});
    b.begin_block(then);
    b.op_void(Op::OpDemoteToHelperInvocation);
    b.op_void(Op::OpBranch, {merge});
    b.begin_block(merge);
    b.op_void(Op::OpReturn);
    b.end_function();

    b.entry_point(spv::ExecutionModel::Fragment, main, "main",
                  {image, sampler, coord, out[0], out[1]});
    b.execution_mode(main, spv::ExecutionMode::OriginUpperLeft);
    EXPECT_EQ(validate(b.words()), "");
}

}  // namespace
