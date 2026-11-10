// bbvk's compute detiler and tiler: the shaders under spirv-val, and round trips on lavapipe.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "support/spirv_val.hpp"
#include "gpu/tile/tiling.hpp"
#include "gpu/vk/device.hpp"
#include "gpu/vk/scheduler.hpp"
#include "gpu/vk/tiler.hpp"

namespace {

using namespace bb::vk;

TEST(TilingShader, Validates) {
    if (!bb::test::gcn::have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    for (std::uint32_t bytes = 1; bytes <= 16; bytes *= 2) {
        for (const bool to_linear : {false, true}) {
            EXPECT_EQ(bb::test::gcn::validate(tiling_shader(bytes, to_linear)), "")
                << bytes << " bytes, to_linear " << to_linear;
        }
    }
    EXPECT_THROW(tiling_shader(3, true), std::invalid_argument);
    EXPECT_THROW(tiling_shader(32, true), std::invalid_argument);
}

// A host-visible, coherent storage buffer with a device address.
struct Buffer {
    VkDevice device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::byte* bytes = nullptr;
    VkDeviceAddress address = 0;

    Buffer(const Device& d, VkDeviceSize size) : device(d.handle()) {
        auto info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = size;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        check(vkCreateBuffer(device, &info, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements needs;
        vkGetBufferMemoryRequirements(device, buffer, &needs);
        const auto type = d.memory_type(needs.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!type) {
            throw std::runtime_error("no host-coherent memory for the buffer");
        }
        auto flags = make<VkMemoryAllocateFlagsInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags);
        alloc.allocationSize = needs.size;
        alloc.memoryTypeIndex = *type;
        check(vkAllocateMemory(device, &alloc, nullptr, &memory), "vkAllocateMemory");
        check(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory");
        void* map = nullptr;
        check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory");
        bytes = static_cast<std::byte*>(map);
        auto address_info = make<VkBufferDeviceAddressInfo>(
            VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
        address_info.buffer = buffer;
        address = vkGetBufferDeviceAddress(device, &address_info);
    }
    ~Buffer() {
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
};

TEST(Tiler, RoundTripOnLavapipe) {
    ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);  // as in device_test.cpp
    std::string why;
    const auto instance = Instance::create(why);
    if (!instance) {
        GTEST_SKIP() << "no Vulkan instance: " << why;
    }
    const auto device = Device::create(*instance, {.device = "llvmpipe"}, why);
    if (!device) {
        GTEST_SKIP() << "no lavapipe device: " << why;
    }
    Scheduler scheduler(*device);
    Tiler tiler(*device);

    // Every supported mode at every element size, as in tiling_test.cpp, and a bank swizzle.
    std::vector<bb::tile::Surface> surfaces;
    constexpr std::uint32_t modes[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 13, 14, 31};
    for (const std::uint32_t mode : modes) {
        for (std::uint32_t bits = 8; bits <= 128; bits *= 2) {
            surfaces.push_back({.tile_mode = mode, .bits = bits, .pitch = 200, .height = 100,
                                .layers = 2, .levels = 3});
        }
    }
    surfaces.push_back({.tile_mode = 14, .bits = 32, .pitch = 300, .height = 300, .layers = 2,
                        .levels = 2, .bank_swizzle = 5});
    for (const bb::tile::Surface& s : surfaces) {
        SCOPED_TRACE(testing::Message() << "mode " << s.tile_mode << ", " << s.bits << " bits");
        const auto layout = bb::tile::layout(s);
        ASSERT_TRUE(layout.has_value());
        // Our own data, tiled on the CPU.
        std::vector<std::byte> linear(layout->size);
        for (std::size_t i = 0; i < linear.size(); ++i) {
            linear[i] = static_cast<std::byte>(i * 7 + i / 251);
        }
        std::vector<std::byte> tiled(layout->size);
        bb::tile::tile(*layout, linear, tiled);

        std::vector<bb::tile::AddressTables> tables;
        std::vector<std::uint32_t> words;
        std::vector<std::size_t> starts;  // of each level's tables, in words
        for (std::uint32_t i = 0; i < layout->levels.size(); ++i) {
            tables.push_back(bb::tile::address_tables(*layout, i));
            starts.push_back(words.size());
            const std::vector<std::uint32_t> level = table_words(tables.back());
            words.insert(words.end(), level.begin(), level.end());
        }
        Buffer gpu_tiled(*device, layout->size), gpu_linear(*device, layout->size);
        Buffer gpu_tables(*device, 4 * words.size());
        std::memcpy(gpu_tables.bytes, words.data(), 4 * words.size());
        std::memcpy(gpu_tiled.bytes, tiled.data(), tiled.size());
        std::memset(gpu_linear.bytes, 0, linear.size());

        const auto run = [&](bool to_linear) {
            for (std::uint32_t i = 0; i < layout->levels.size(); ++i) {
                tiler.record(scheduler.commands(), *layout, i, tables[i],
                             {gpu_tiled.address, gpu_linear.address,
                              gpu_tables.address + 4 * starts[i]},
                             to_linear);
            }
            // Make the shader's writes visible to the host reads below.
            auto barrier = make<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(scheduler.commands(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0,
                                 nullptr);
            scheduler.finish();
        };
        // Detile on the GPU, then tile back on the GPU from the GPU's linear copy.
        run(true);
        ASSERT_EQ(std::memcmp(gpu_linear.bytes, linear.data(), linear.size()), 0);
        std::memset(gpu_tiled.bytes, 0, tiled.size());
        run(false);
        ASSERT_EQ(std::memcmp(gpu_tiled.bytes, tiled.data(), tiled.size()), 0);
    }
}

}  // namespace
