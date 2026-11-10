// The compute runner on lavapipe, with a SPIR-V module written by hand with SpirvBuilder.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/spirv_builder.hpp"
#include "support/spirv_val.hpp"
#include "gpu/runner/compute_runner.hpp"

namespace {

using namespace bb::test::vk;
using bb::gcn::SpirvBuilder;
using bb::gcn::spirv_word;
using Id = SpirvBuilder::Id;
using Op = spv::Op;
using SC = spv::StorageClass;

constexpr std::uint32_t lanes = 128;  // two workgroups of 64

// A compute shader that reads the draw block like a translated program will:
//   out = user_data[0..1], in = user_data[2..3] (guest addresses, low word first)
//   out[i] = in[i] + user_data[4] for every invocation i
//   invocation 0: out[user_data[5] + 0..2] = SubgroupSize, code_address low, code_address high
std::vector<std::uint32_t> add_module() {
    SpirvBuilder b;
    for (auto cap : {spv::Capability::Shader, spv::Capability::Int64,
                     spv::Capability::PhysicalStorageBufferAddresses,
                     spv::Capability::GroupNonUniform}) {
        b.capability(cap);
    }
    b.memory_model(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);

    const Id void_t = b.type_void();
    const Id bool_t = b.type_bool();
    const Id u32 = b.type_int(32, false);
    const Id u64 = b.type_int(64, false);
    const Id uvec3 = b.type_vector(u32, 3);
    const Id c0 = b.constant_u32(0), c1 = b.constant_u32(1), c2 = b.constant_u32(2);
    const Id c32 = b.constant_u32(32);

    const Id user_t = b.type_array(u32, b.constant_u32(16));
    b.decorate(user_t, spv::Decoration::ArrayStride, {4});
    const Id draw_t = b.type_struct({user_t, u64, u64});
    b.decorate(draw_t, spv::Decoration::Block);
    b.member_decorate(draw_t, 0, spv::Decoration::Offset, {draw_user_data_offset});
    b.member_decorate(draw_t, 1, spv::Decoration::Offset, {draw_address_bias_offset});
    b.member_decorate(draw_t, 2, spv::Decoration::Offset, {draw_code_address_offset});
    const Id draw_ptr_t = b.type_pointer(SC::PhysicalStorageBuffer, draw_t);
    const Id push_t = b.type_struct({u64});
    b.decorate(push_t, spv::Decoration::Block);
    b.member_decorate(push_t, 0, spv::Decoration::Offset, {0});
    const Id push = b.variable(b.type_pointer(SC::PushConstant, push_t), SC::PushConstant);

    const Id words_t = b.type_runtime_array(u32);
    b.decorate(words_t, spv::Decoration::ArrayStride, {4});
    const Id memory_t = b.type_struct({words_t});
    b.decorate(memory_t, spv::Decoration::Block);
    b.member_decorate(memory_t, 0, spv::Decoration::Offset, {0});
    const Id memory_ptr_t = b.type_pointer(SC::PhysicalStorageBuffer, memory_t);
    const Id u32_ptr_t = b.type_pointer(SC::PhysicalStorageBuffer, u32);
    const Id u64_ptr_t = b.type_pointer(SC::PhysicalStorageBuffer, u64);

    const Id global_id = b.variable(b.type_pointer(SC::Input, uvec3), SC::Input);
    b.decorate(global_id, spv::Decoration::BuiltIn, {spirv_word(spv::BuiltIn::GlobalInvocationId)});
    const Id subgroup_size = b.variable(b.type_pointer(SC::Input, u32), SC::Input);
    b.decorate(subgroup_size, spv::Decoration::BuiltIn,
               {spirv_word(spv::BuiltIn::SubgroupSize)});

    const Id main = b.begin_function(void_t, b.type_function(void_t));
    b.begin_block(b.id());
    const auto load = [&](Id type, Id pointer, std::uint32_t align) {
        return b.op(Op::OpLoad, type,
                    {pointer, spirv_word(spv::MemoryAccessMask::Aligned), align});
    };
    const auto store = [&](Id pointer, Id value) {
        b.op_void(Op::OpStore, {pointer, value, spirv_word(spv::MemoryAccessMask::Aligned), 4});
    };

    const Id draw_address = b.op(Op::OpLoad, u64,
                                 {b.op(Op::OpAccessChain, b.type_pointer(SC::PushConstant, u64),
                                       {push, c0})});
    const Id draw = b.op(Op::OpConvertUToPtr, draw_ptr_t, {draw_address});
    const auto user = [&](std::uint32_t k) {
        return load(u32, b.op(Op::OpAccessChain, u32_ptr_t, {draw, c0, b.constant_u32(k)}), 4);
    };
    // The runner's bias table holds the same bias in every entry.
    const Id table = load(u64, b.op(Op::OpAccessChain, u64_ptr_t, {draw, c1}), 8);
    const Id bias = load(u64, b.op(Op::OpConvertUToPtr, u64_ptr_t, {table}), 8);
    const Id code = load(u64, b.op(Op::OpAccessChain, u64_ptr_t, {draw, c2}), 8);
    // The memory at the guest address in user_data[k] (low) and [k + 1] (high).
    const auto memory = [&](std::uint32_t k) {
        const Id low = b.op(Op::OpUConvert, u64, {user(k)});
        const Id high = b.op(Op::OpShiftLeftLogical, u64, {b.op(Op::OpUConvert, u64, {user(k + 1)}),
                                                           c32});
        const Id guest = b.op(Op::OpBitwiseOr, u64, {high, low});
        return b.op(Op::OpConvertUToPtr, memory_ptr_t, {b.op(Op::OpIAdd, u64, {guest, bias})});
    };
    const Id out = memory(0);
    const Id in = memory(2);
    const auto word = [&](Id base, Id index) {
        return b.op(Op::OpAccessChain, u32_ptr_t, {base, c0, index});
    };

    const Id i = b.op(Op::OpCompositeExtract, u32, {b.op(Op::OpLoad, uvec3, {global_id}), 0});
    store(word(out, i), b.op(Op::OpIAdd, u32, {load(u32, word(in, i), 4), user(4)}));

    const Id first = b.id(), merge = b.id();
    const Id is_first = b.op(Op::OpIEqual, bool_t, {i, c0});
    b.op_void(Op::OpSelectionMerge, {merge, spirv_word(spv::SelectionControlMask::MaskNone)});
    b.op_void(Op::OpBranchConditional, {is_first, first, merge});
    b.begin_block(first);
    const Id end = user(5);
    store(word(out, end), b.op(Op::OpLoad, u32, {subgroup_size}));
    store(word(out, b.op(Op::OpIAdd, u32, {end, c1})), b.op(Op::OpUConvert, u32, {code}));
    store(word(out, b.op(Op::OpIAdd, u32, {end, c2})),
          b.op(Op::OpUConvert, u32, {b.op(Op::OpShiftRightLogical, u64, {code, c32})}));
    b.op_void(Op::OpBranch, {merge});
    b.begin_block(merge);
    b.op_void(Op::OpReturn);
    b.end_function();

    b.entry_point(spv::ExecutionModel::GLCompute, main, "main", {push, global_id, subgroup_size});
    b.execution_mode(main, spv::ExecutionMode::LocalSize, {64, 1, 1});
    return b.words();
}

class ComputeRunnerTest : public testing::Test {
protected:
    void SetUp() override {
        std::string why;
        runner_ = ComputeRunner::create(why);
        if (!runner_) {
            GTEST_SKIP() << why;
        }
    }

    std::unique_ptr<ComputeRunner> runner_;
};

TEST_F(ComputeRunnerTest, ReportsDevice) {
    const DeviceInfo& info = runner_->info();
    std::cout << info.describe();
    EXPECT_NE(info.name.find("llvmpipe"), std::string::npos) << info.name;
    EXPECT_TRUE(std::has_single_bit(info.subgroup_size));
    EXPECT_LE(info.min_subgroup_size, info.subgroup_size);
    EXPECT_LE(info.subgroup_size, info.max_subgroup_size);
    EXPECT_LE(info.max_subgroup_size, 64u);  // a wider subgroup is not supported
}

TEST_F(ComputeRunnerTest, HandWrittenModuleWritesBuffer) {
    const auto module = add_module();
    if (bb::test::gcn::have_spirv_val()) {
        ASSERT_EQ(bb::test::gcn::validate(module), "");
    }
    // Guest addresses above 2^32, so the high words matter. The second run gets new guest
    // memory at another base and reuses the command buffer and fence of the first.
    constexpr struct {
        std::uint64_t base;
        std::uint32_t addend;
    } runs[] = {{0x12'0000'0000, 7}, {0xffff'f000'0000, 0x8000'0000}};
    constexpr std::uint64_t in_offset = 0x100, out_offset = 0x400;
    for (const auto& r : runs) {
        SCOPED_TRACE(testing::Message() << "base 0x" << std::hex << r.base);
        auto memory = runner_->guest_memory(r.base, 0x1000);
        std::vector<std::uint32_t> in(lanes);
        for (std::uint32_t i = 0; i < lanes; ++i) {
            in[i] = 3 * i + 1;
        }
        std::memcpy(memory.data() + in_offset, in.data(), in.size() * 4);

        const auto low = [](std::uint64_t a) { return static_cast<std::uint32_t>(a); };
        const auto high = [](std::uint64_t a) { return static_cast<std::uint32_t>(a >> 32); };
        DrawBlock draw;
        draw.user_data = {low(r.base + out_offset), high(r.base + out_offset),
                          low(r.base + in_offset), high(r.base + in_offset), r.addend, lanes};
        draw.code_address = 0xab'cdef'0100;
        runner_->run(module, draw, {lanes / 64, 1, 1});

        std::vector<std::uint32_t> out(lanes + 4);
        std::memcpy(out.data(), memory.data() + out_offset, out.size() * 4);
        for (std::uint32_t i = 0; i < lanes; ++i) {
            EXPECT_EQ(out[i], 3 * i + 1 + r.addend) << "invocation " << i;
        }
        // The pipeline asks for no subgroup size, so any size in the device's range may run.
        const DeviceInfo& info = runner_->info();
        EXPECT_TRUE(std::has_single_bit(out[lanes])) << out[lanes];
        EXPECT_GE(out[lanes], info.min_subgroup_size);
        EXPECT_LE(out[lanes], info.max_subgroup_size);
        EXPECT_EQ(out[lanes + 1], 0xcdef'0100u);
        EXPECT_EQ(out[lanes + 2], 0xabu);
        EXPECT_EQ(out[lanes + 3], 0u);
        // The input is unchanged.
        EXPECT_EQ(std::memcmp(memory.data() + in_offset, in.data(), in.size() * 4), 0);
    }
}

TEST_F(ComputeRunnerTest, RejectsRunWithoutGuestMemory) {
    EXPECT_THROW(runner_->run(add_module(), DrawBlock{}, {1, 1, 1}), std::logic_error);
    EXPECT_THROW(runner_->guest_memory(0, 0), std::invalid_argument);
}

// With no driver to load, creating the runner fails with a reason instead of throwing, so the
// tests above skip. The fixture points the loader at a manifest that doesn't exist and puts the
// environment back afterwards.
class ComputeRunnerNoDriver : public testing::Test {
protected:
    static constexpr const char* kNames[] = {
        "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_LOADER_DRIVERS_SELECT"};

    void SetUp() override {
        for (std::size_t i = 0; i < std::size(kNames); ++i) {
            const char* value = std::getenv(kNames[i]);
            saved_[i] = value ? std::optional<std::string>(value) : std::nullopt;
        }
        // The manifest path lies in a directory this test just created, so no other process can
        // have put a driver manifest there for the loader to parse and load.
        std::string dir_template =
            (std::filesystem::temp_directory_path() / "bb-no-icd-XXXXXX").string();
        ASSERT_NE(::mkdtemp(dir_template.data()), nullptr) << "cannot create a temp directory";
        dir_ = dir_template;
        const std::string none = (dir_ / "none.json").string();
        ASSERT_FALSE(std::filesystem::exists(none)) << none;
        ::setenv("VK_DRIVER_FILES", none.c_str(), 1);
        ::setenv("VK_ICD_FILENAMES", none.c_str(), 1);
    }

    void TearDown() override {
        for (std::size_t i = 0; i < std::size(kNames); ++i) {
            saved_[i] ? ::setenv(kNames[i], saved_[i]->c_str(), 1) : ::unsetenv(kNames[i]);
        }
        if (!dir_.empty()) {
            std::filesystem::remove(dir_);
        }
    }

    std::optional<std::string> saved_[std::size(kNames)];
    std::filesystem::path dir_;
};

// The runner's own filter loads only lavapipe, so the reason names lavapipe.
TEST_F(ComputeRunnerNoDriver, OwnFilterNamesLavapipe) {
    ::unsetenv("VK_LOADER_DRIVERS_SELECT");
    std::string why;
    const auto runner = ComputeRunner::create(why);
    EXPECT_EQ(runner, nullptr);
    EXPECT_EQ(why.rfind("no lavapipe driver", 0), 0u) << why;
    EXPECT_NE(why.find("VK_LOADER_DRIVERS_SELECT=*lvp*"), std::string::npos) << why;
}

// A filter from the environment may select other drivers, so the reason names the instance.
TEST_F(ComputeRunnerNoDriver, EnvironmentFilterNamesInstance) {
    ::setenv("VK_LOADER_DRIVERS_SELECT", "*", 1);
    std::string why;
    const auto runner = ComputeRunner::create(why);
    EXPECT_EQ(runner, nullptr);
    EXPECT_EQ(why.rfind("no Vulkan instance", 0), 0u) << why;
}

}  // namespace
