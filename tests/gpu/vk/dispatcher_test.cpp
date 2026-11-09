// DISPATCH_DIRECT and DISPATCH_INDIRECT through the parser, the compute pipeline cache, the
// scheduler and the memory mirror on lavapipe (docs/design/graphics.md, item 25). The shader
// adds 1 to word 0 of the buffer that user data 0-3 describe (the shader uses no group ID), so
// the word counts the groups that ran, with a workgroup of one thread.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/pm4/packet.hpp"
#include "gpu/pm4/parser.hpp"
#include "gpu/vk/compute_pipeline.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/dispatcher.hpp"
#include "gpu/vk/memory_mirror.hpp"
#include "gpu/vk/scheduler.hpp"
#include "gpu/vk/texture_desc.hpp"
#include "gpu/vk/texture_binder.hpp"
#include "gpu/vk/texture_cache.hpp"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/vmem.hpp"
#include "test_device.hpp"
#include "gpu/gcn/encode.hpp"
#include "tools/mirror_observer.hpp"

namespace {

using namespace bb::hle::kernel;
using namespace bb::vk;
using bb::pm4::Parser;
using bb::pm4::RegisterMirror;
using bb::pm4::RunResult;
using bb::pm4::Stream;
using bb::test::gcn::mimg;
using bb::test::gcn::mubuf;
using bb::test::gcn::sopp;
using bb::test::gcn::vop1;

constexpr std::uint64_t kib = 1024;
constexpr std::uint64_t code_at = 0, out_at = 0x1000, counts_at = 0x2000, bad_at = 0x3000,
                         image_code_at = 0x400, image_at = 0x4000;

void packet(std::vector<std::uint32_t>& out, std::uint8_t opcode,
            std::initializer_list<std::uint32_t> body) {
    out.push_back(bb::pm4::type3_header(opcode, static_cast<std::uint32_t>(body.size())));
    out.insert(out.end(), body);
}

class VkDispatcher : public testing::Test {
protected:
    void SetUp() override {
        const std::string selector = bb::test::vk_test_device();
        std::string why;
        instance_ = Instance::create(why);
        if (!instance_) {
            GTEST_SKIP() << "no Vulkan instance: " << why;
        }
        device_ = Device::create(*instance_, {.device = selector}, why);
        if (!device_) {
            GTEST_SKIP() << "no Vulkan device: " << why;
        }
        ASSERT_GE(dmem::fd(), 0);
        mirror_ = MemoryMirror::create(*device_,
                                       {.memfd = dmem::fd(),
                                        .window_start = vmem::window_start,
                                        .size = mirror_buffer_size,
                                        .min_block = 64 * kib},
                                       why);
        if (!mirror_) {
            GTEST_SKIP() << "no mirror: " << why;
        }
        observer_ = std::make_unique<bb::tools::MirrorObserver>(*mirror_);
        vmem::set_direct_observer(observer_.get());
        std::uint64_t offset = ~std::uint64_t{0};
        ASSERT_EQ(allocate_direct(0, direct_memory_size(), 256 * kib, 64 * kib, 0, &offset), 0);
        offset_ = offset;
        ASSERT_EQ(map_direct(&memory_, 256 * kib, 3, 0, offset, 64 * kib), 0);
        guest_ = reinterpret_cast<std::uintptr_t>(memory_);
        std::memset(memory_, 0, 256 * kib);

        // v_mov_b32 v1, 1; buffer_atomic_add v1, off, s[0:3], 0
        std::vector<std::uint32_t> code = {vop1(1, 1, 129)};
        for (const auto w : mubuf(50, 1, 0, 0, 128)) code.push_back(w);
        code.push_back(sopp(1));
        std::memcpy(bytes() + code_at, code.data(), code.size() * 4);
        std::memset(bytes() + bad_at, 0xFF, 16);

        scheduler_ = std::make_unique<Scheduler>(*device_);
        pipelines_ = std::make_unique<ComputePipelineCache>(
            *device_, [this](std::uint64_t address) -> std::span<const std::uint32_t> {
                if (address < guest_ || address >= guest_ + 256 * kib) return {};
                return {reinterpret_cast<const std::uint32_t*>(address),
                        (guest_ + 256 * kib - address) / 4};
            });
        textures_ = std::make_unique<TextureCache>(*device_, *scheduler_);
        binder_ = std::make_unique<TextureBinder>(
            *device_, *scheduler_, *textures_,
            [this](std::uint64_t address, std::uint64_t bytes) -> std::span<const std::byte> {
                if (address < guest_ || address + bytes > guest_ + 256 * kib) return {};
                return {reinterpret_cast<const std::byte*>(address), bytes};
            });
        dispatcher_ = std::make_unique<Dispatcher>(*device_, *scheduler_, *pipelines_, *mirror_,
                                                   binder_.get());
        parser_ = std::make_unique<Parser>(regs_);
        parser_->set_dispatch([this](const RegisterMirror& r, const bb::pm4::DispatchCall& c) {
            dispatcher_->dispatch(r, c);
        });
        parser_->set_sync([this](bb::pm4::SyncPoint p) { dispatcher_->sync(p); });
    }

    void TearDown() override {
        dispatcher_.reset();  // before the memory it dispatches over goes
        binder_.reset();
        textures_.reset();
        if (memory_ != nullptr) {
            EXPECT_EQ(sceKernelMunmap(memory_, 256 * kib), 0);
            EXPECT_EQ(release_direct(offset_, 256 * kib), 0);
        }
        vmem::set_direct_observer(nullptr);
    }

    std::byte* bytes() const { return static_cast<std::byte*>(memory_); }
    std::uint32_t word(std::uint64_t at, unsigned i) const {
        std::uint32_t value;
        std::memcpy(&value, bytes() + at + 4 * i, 4);
        return value;
    }

    // The CS registers for the program at `code`: threads, program, RSRC2 (4 user SGPRs) and the user data (the output buffer's V#).
    void set_state(std::vector<std::uint32_t>& stream, std::uint64_t code) {
        const std::uint64_t out = guest_ + out_at;
        packet(stream, IT_SET_SH_REG, {0x207, 1, 1, 1});
        const std::uint64_t pgm = guest_ + code;
        packet(stream, IT_SET_SH_REG,
               {0x20C, static_cast<std::uint32_t>(pgm >> 8), static_cast<std::uint32_t>(pgm >> 40)});
        packet(stream, IT_SET_SH_REG, {0x213, 4u << 1});
        packet(stream, IT_SET_SH_REG,
               {0x240, static_cast<std::uint32_t>(out), static_cast<std::uint32_t>(out >> 32),
                0x1000, 0});
    }

    RunResult run(const std::vector<std::uint32_t>& stream) {
        Stream s(stream);
        const RunResult result = parser_->run(s);
        // Memory packets sync before a write; a stream's end needs the same to read the result.
        dispatcher_->sync(bb::pm4::SyncPoint::before_write);
        return result;
    }

    std::unique_ptr<Instance> instance_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<MemoryMirror> mirror_;
    std::unique_ptr<bb::tools::MirrorObserver> observer_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<ComputePipelineCache> pipelines_;
    std::unique_ptr<TextureCache> textures_;
    std::unique_ptr<TextureBinder> binder_;
    std::unique_ptr<Dispatcher> dispatcher_;
    RegisterMirror regs_;
    std::unique_ptr<Parser> parser_;
    void* memory_ = nullptr;
    std::uint64_t offset_ = 0;
    std::uintptr_t guest_ = 0;
};

TEST_F(VkDispatcher, DirectDispatchRunsTheGroups) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {2, 3, 2, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 12u);
    EXPECT_EQ(word(out_at, 1), 0u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(dispatcher_->stats().untranslated, 0u);
    EXPECT_EQ(dispatcher_->stats().failed, 0u);
}

TEST_F(VkDispatcher, RecentHoldsOnlyWhatWasRecorded) {
    EXPECT_TRUE(dispatcher_->recent().empty());
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    ASSERT_EQ(dispatcher_->recent().size(), 1u);
    EXPECT_EQ(dispatcher_->recent()[0].number, 1u);
    EXPECT_EQ(dispatcher_->recent()[0].indirect, 0u);
}

TEST_F(VkDispatcher, OnlyALostDeviceReportsTheRecentDispatches) {
    dispatcher_->report(std::runtime_error("not a Vulkan error"));
    EXPECT_FALSE(dispatcher_->reported());
    dispatcher_->report(VulkanError("out of memory", VK_ERROR_OUT_OF_DEVICE_MEMORY));
    EXPECT_FALSE(dispatcher_->reported());
    dispatcher_->report(VulkanError("lost", VK_ERROR_DEVICE_LOST));
    EXPECT_TRUE(dispatcher_->reported());
}

TEST_F(VkDispatcher, RecentKeepsTheLastDispatchesOldestFirst) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    const std::uint32_t total = recent_dispatches + 3;
    for (std::uint32_t i = 1; i <= total; ++i) {
        packet(stream, IT_DISPATCH_DIRECT, {i, 1, 1, 1});
    }
    ASSERT_EQ(run(stream), RunResult::done);
    const auto recent = dispatcher_->recent();
    ASSERT_EQ(recent.size(), recent_dispatches);
    const std::uint64_t out = guest_ + out_at;
    EXPECT_EQ(recent.front().number, 4u);
    EXPECT_EQ(recent.front().groups[0], 4u);
    EXPECT_EQ(recent.back().number, total);
    EXPECT_EQ(recent.back().groups[0], total);
    EXPECT_EQ(recent.back().program, guest_ + code_at);
    EXPECT_NE(recent.back().block, 0u);
    EXPECT_EQ(recent.back().user[0], static_cast<std::uint32_t>(out));
    EXPECT_EQ(recent.back().user[1], static_cast<std::uint32_t>(out >> 32));
}

TEST(RecentDispatch, DescribeNamesTheProgramTheCountsAndTheUserData) {
    RecentDispatch d{.number = 7, .program = 0x80d1b00100, .block = 0xabc00, .groups = {2, 3, 4}};
    d.user[0] = 0x12345678;
    const std::string direct = describe(d);
    EXPECT_NE(direct.find("dispatch 7: program 0x80d1b00100, block 0xabc00"), std::string::npos);
    EXPECT_NE(direct.find("groups 2x3x4"), std::string::npos);
    EXPECT_NE(direct.find("user data 12345678 00000000"), std::string::npos);
    d.indirect = 0x8000000100;
    EXPECT_NE(describe(d).find("indirect 0x8000000100"), std::string::npos);
}

// New code at the address of a cached program runs instead of the old one (#371).
TEST_F(VkDispatcher, CodeOverwrittenInPlaceRunsTheNewProgram) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {2, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 2u);

    const std::uint32_t add_two = vop1(1, 1, 130);  // v_mov_b32 v1, 2
    std::memcpy(bytes() + code_at, &add_two, 4);
    stream.clear();
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {3, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 2u + 3u * 2u);
    EXPECT_EQ(pipelines_->translations(), 2u);
}

TEST_F(VkDispatcher, IndirectDispatchReadsTheCountsAtBasePlusOffset) {
    const std::uint32_t counts[3] = {7, 1, 1};
    std::memcpy(bytes() + counts_at + 16, counts, sizeof counts);
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    const std::uint64_t base = guest_ + counts_at;
    packet(stream, IT_SET_BASE,
           {1, static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32)});
    packet(stream, IT_DISPATCH_INDIRECT, {16, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 7u);
    EXPECT_EQ(word(out_at, 1), 0u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(dispatcher_->recent().back().indirect, base + 16);
}

TEST_F(VkDispatcher, SecondDispatchSeesTheFirstOnesWrites) {
    // Two dispatches in one recording with one pipeline.
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {3, 1, 1, 1});
    packet(stream, IT_DISPATCH_DIRECT, {9, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 12u);
    EXPECT_EQ(dispatcher_->stats().recorded, 2u);
    EXPECT_EQ(pipelines_->translations(), 1u);
}

TEST_F(VkDispatcher, DisabledComputeShaderDispatchesNothing) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {5, 1, 1, 0});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), 0u);
    EXPECT_EQ(dispatcher_->stats().recorded, 0u);
}

TEST_F(VkDispatcher, UntranslatedShadersAreCountedAndTheStreamGoesOn) {
    std::vector<std::uint32_t> stream;
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});  // no program
    set_state(stream, bad_at);
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {2, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(dispatcher_->stats().untranslated, 3u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(word(out_at, 0), 2u);
}

TEST_F(VkDispatcher, CountsOutsideTheMirrorFailTheDispatchOnly) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    const std::uint64_t base = vmem::window_start + 3 * (std::uint64_t{1} << 30);
    packet(stream, IT_SET_BASE,
           {1, static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32)});
    packet(stream, IT_DISPATCH_INDIRECT, {0, 1});
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(dispatcher_->stats().failed, 1u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(word(out_at, 0), 1u);
}

TEST_F(VkDispatcher, UnalignedCountsFailTheDispatchOnly) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    const std::uint64_t base = guest_ + counts_at;
    packet(stream, IT_SET_BASE,
           {1, static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32)});
    packet(stream, IT_DISPATCH_INDIRECT, {2, 1});
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(dispatcher_->stats().failed, 1u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(word(out_at, 0), 1u);
}

TEST_F(VkDispatcher, GroupCountsOverTheDeviceLimitFailTheDispatchOnly) {
    const std::uint32_t limit = device_->properties().core.limits.maxComputeWorkGroupCount[0];
    if (limit == UINT32_MAX) {
        GTEST_SKIP() << "the group count limit is the largest u32, so no count is over it";
    }
    const std::uint32_t over = limit + 1;
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {over, 1, 1, 1});
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(dispatcher_->stats().failed, 1u);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(word(out_at, 0), 1u);
}

TEST_F(VkDispatcher, ManyDispatchesRollOverTheDrawBlockChunks) {
    // 256 blocks per chunk: 600 dispatches use three chunks and defer the first two.
    constexpr unsigned count = 600;
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    for (unsigned i = 0; i < count; ++i) packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(word(out_at, 0), count);
    EXPECT_EQ(dispatcher_->stats().recorded, count);
    EXPECT_EQ(dispatcher_->stats().failed, 0u);
}

TEST_F(VkDispatcher, SyncBeforeWaitSubmitsWithoutWaiting) {
    std::vector<std::uint32_t> stream;
    set_state(stream, code_at);
    packet(stream, IT_DISPATCH_DIRECT, {4, 1, 1, 1});
    Stream s(stream);
    ASSERT_EQ(parser_->run(s), RunResult::done);
    ASSERT_TRUE(scheduler_->recording());
    dispatcher_->sync(bb::pm4::SyncPoint::before_wait);
    EXPECT_FALSE(scheduler_->recording());
    EXPECT_GE(scheduler_->submitted(), 1u);
    dispatcher_->sync(bb::pm4::SyncPoint::before_write);
    EXPECT_EQ(word(out_at, 0), 4u);
}

}  // namespace

// Dispatches with images (docs/design/graphics.md, "Textures and samplers (item 28)"). The T# is
// in user data 0-7, the S# in 8-11 and the output buffer's V# in 12-15.
constexpr std::uint32_t image_side = 8;

// An 8_8_8_8 image of `image_side` square texels at image_at, linear, with the number format
// `nfmt`, as the T# the user data holds.
std::array<std::uint32_t, 8> image_descriptor(std::uint64_t guest, std::uint32_t nfmt) {
    const std::uint64_t at = guest + image_at;
    return {static_cast<std::uint32_t>(at >> 8),
            static_cast<std::uint32_t>(at >> 40) | 10u << 20 | nfmt << 26,
            (image_side - 1) | (image_side - 1) << 14,
            4u | 5u << 3 | 6u << 6 | 7u << 9 | 8u << 20 | 9u << 28,  // XYZW, linear, 2D
            (image_side - 1) << 13,
            0, 0, 0};
}

class VkDispatchImages : public VkDispatcher {
protected:
    // The CS registers for the program at image_code_at with the 16 user SGPRs `user`.
    void set_image_state(std::vector<std::uint32_t>& stream, const std::vector<std::uint32_t>& code,
                         const std::array<std::uint32_t, 16>& user) {
        std::memcpy(bytes() + image_code_at, code.data(), code.size() * 4);
        packet(stream, IT_SET_SH_REG, {0x207, 1, 1, 1});
        const std::uint64_t pgm = guest_ + image_code_at;
        packet(stream, IT_SET_SH_REG,
               {0x20C, static_cast<std::uint32_t>(pgm >> 8), static_cast<std::uint32_t>(pgm >> 40)});
        packet(stream, IT_SET_SH_REG, {0x213, 16u << 1});
        std::vector<std::uint32_t> body = {0x240};
        body.insert(body.end(), user.begin(), user.end());
        stream.push_back(bb::pm4::type3_header(IT_SET_SH_REG, static_cast<std::uint32_t>(body.size())));
        stream.insert(stream.end(), body.begin(), body.end());
    }

    std::array<std::uint32_t, 16> user_data(std::uint32_t nfmt) {
        const std::uint64_t out = guest_ + out_at;
        const auto t = image_descriptor(guest_, nfmt);
        std::array<std::uint32_t, 16> user{};
        std::ranges::copy(t, user.begin());
        user[12] = static_cast<std::uint32_t>(out);
        user[13] = static_cast<std::uint32_t>(out >> 32);
        user[14] = 0x1000;
        return user;
    }

    // The 8_8_8_8 texels of the image (4 bytes each), copied out of the Vulkan image. The image
    // is moved to the sampled layout first, as a draw reading it would.
    std::vector<std::uint8_t> read_back(std::uint32_t nfmt) {
        const auto desc = decode_texture(image_descriptor(guest_, nfmt));
        const TextureView* view = textures_->get(*desc, {bytes() + image_at, 4 * kib});
        EXPECT_NE(view, nullptr);
        std::vector<std::uint8_t> out(4 * image_side * image_side);
        if (view == nullptr) return out;
        const VkDevice d = device_->handle();
        auto info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = out.size();
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer;
        check(vkCreateBuffer(d, &info, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements needs;
        vkGetBufferMemoryRequirements(d, buffer, &needs);
        auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        alloc.allocationSize = needs.size;
        alloc.memoryTypeIndex = *device_->memory_type(
            needs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory memory;
        check(vkAllocateMemory(d, &alloc, nullptr, &memory), "vkAllocateMemory");
        check(vkBindBufferMemory(d, buffer, memory, 0), "vkBindBufferMemory");
        void* map;
        check(vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory");
        const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                       {image_side, image_side, 1}};
        vkCmdCopyImageToBuffer(scheduler_->commands(), view->image, TextureView::layout, buffer, 1,
                               &region);
        scheduler_->finish();  // ends with a barrier to host reads
        std::memcpy(out.data(), map, out.size());
        vkDestroyBuffer(d, buffer, nullptr);
        vkFreeMemory(d, memory, nullptr);
        return out;
    }
};

TEST_F(VkDispatchImages, ImageStoreWritesAStorageImage) {
    constexpr std::uint32_t uint_format = 4;
    std::memset(bytes() + image_at, 0xee, 4 * kib);
    // v0 = x = 1, v1 = y = 2, v2..v5 = 10, 20, 30, 40; image_store v[2:5], v[0:1], s[0:7]
    std::vector<std::uint32_t> code = {vop1(1, 0, 129), vop1(1, 1, 130), vop1(1, 2, 138),
                                       vop1(1, 3, 148), vop1(1, 4, 158), vop1(1, 5, 168)};
    for (const auto w : mimg(8, 2, 0, 0, 0, 0xf, bb::test::gcn::img::unorm)) code.push_back(w);
    code.push_back(sopp(1));
    std::vector<std::uint32_t> stream;
    set_image_state(stream, code, user_data(uint_format));
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(dispatcher_->stats().untranslated, 0u);
    EXPECT_EQ(dispatcher_->stats().failed, 0u);

    // Texel (1, 2) holds the stored components and the rest what the guest memory held.
    const auto texels = read_back(uint_format);
    for (std::uint32_t y = 0; y < image_side; ++y) {
        for (std::uint32_t x = 0; x < image_side; ++x) {
            const std::uint8_t* t = &texels[(y * image_side + x) * 4];
            const std::array<std::uint8_t, 4> expected =
                x == 1 && y == 2 ? std::array<std::uint8_t, 4>{10, 20, 30, 40}
                                 : std::array<std::uint8_t, 4>{0xee, 0xee, 0xee, 0xee};
            EXPECT_TRUE(std::equal(expected.begin(), expected.end(), t)) << x << ", " << y;
        }
    }
    // The stores stay in the image: the unchanged guest bytes do not upload it again.
    EXPECT_EQ(textures_->uploads(), 1u);
}

TEST_F(VkDispatchImages, SampledImageIsReadByTheDispatch) {
    // Red is the texel's index, tiled as the T#'s mode says.
    const auto layout = TextureCache::guest_layout(*decode_texture(image_descriptor(guest_, 0)));
    ASSERT_TRUE(layout.has_value());
    std::vector<std::byte> linear(layout->size), tiled(layout->size);
    for (std::uint32_t i = 0; i < image_side; ++i) {
        for (std::uint32_t x = 0; x < image_side; ++x) {
            linear[(i * layout->levels[0].pitch + x) * 4] = static_cast<std::byte>(i * image_side + x);
        }
    }
    bb::tile::tile(*layout, linear, tiled);
    std::memcpy(bytes() + image_at, tiled.data(), tiled.size());
    // v0 = 3.5 / 8, v1 = 5.5 / 8; image_sample_lz v2, v[0:1], s[0:7], s[8:11] dmask 1;
    // buffer_store_dword v2 to the V# in s[12:15].
    std::vector<std::uint32_t> code = {vop1(1, 0, 255), 0x3ee00000, vop1(1, 1, 255), 0x3f300000};
    for (const auto w : mimg(39, 2, 0, 0, 8, 1)) code.push_back(w);
    for (const auto w : mubuf(28, 2, 0, 12, 128)) code.push_back(w);
    code.push_back(sopp(1));
    std::vector<std::uint32_t> stream;
    set_image_state(stream, code, user_data(0));
    packet(stream, IT_DISPATCH_DIRECT, {1, 1, 1, 1});
    ASSERT_EQ(run(stream), RunResult::done);
    EXPECT_EQ(dispatcher_->stats().recorded, 1u);
    EXPECT_EQ(dispatcher_->stats().untranslated, 0u);
    EXPECT_EQ(dispatcher_->stats().failed, 0u);
    float red;
    std::memcpy(&red, bytes() + out_at, 4);
    EXPECT_NEAR(red, (5 * image_side + 3) / 255.0f, 1e-6f);
}
