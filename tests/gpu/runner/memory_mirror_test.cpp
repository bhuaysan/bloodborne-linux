// The GPU memory mirror spike (#268, docs/design/graphics.md, "GPU memory mirror"): guest memory
// imported as host pointers or as udmabuf dma-bufs of a memfd, bound into sparse buffers at its
// window offset, read and written by a translated program through address_bias. On lavapipe, or
// on the driver VK_LOADER_DRIVERS_SELECT picks ('*radeon*'); the tests print what they measured.

#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "gpu/gcn/programs.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/vk/device.hpp"

namespace {

constexpr std::uint64_t kib = 1024, mib = 1024 * kib, gib = 1024 * mib;
constexpr std::size_t guest_page = 16 * kib;  // the alignment of guest mappings
constexpr auto host_pointer = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
constexpr auto dma_buf = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

using bb::vk::check;
using bb::vk::make;

std::uint64_t at(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

// A guest window: a PROT_NONE reservation aligned to 64 KiB, and a memfd sealed against shrinking
// (udmabuf needs that) whose views are mapped into it.
struct Window {
    Window(std::size_t size, std::size_t memfd_size) : reserved(size + 64 * kib) {
        void* p = ::mmap(nullptr, reserved, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        fd = ::memfd_create("mirror-test", MFD_ALLOW_SEALING);
        if (p == MAP_FAILED || fd < 0 || ::ftruncate(fd, static_cast<off_t>(memfd_size)) != 0 ||
            ::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
            throw std::runtime_error("cannot reserve the window or create the memfd");
        }
        base = static_cast<std::byte*>(p);
        start = base + (-at(base) & (64 * kib - 1));
    }
    ~Window() {
        ::munmap(base, reserved);
        ::close(fd);
    }

    // Maps a memfd view at window offset `offset`, or anonymous memory if `memfd_offset` < 0.
    std::byte* map(std::uint64_t offset, std::size_t size, off_t memfd_offset) {
        const bool anon = memfd_offset < 0;
        void* p = ::mmap(start + offset, size, PROT_READ | PROT_WRITE,
                         MAP_FIXED | (anon ? MAP_PRIVATE | MAP_ANONYMOUS : MAP_SHARED),
                         anon ? -1 : fd, anon ? 0 : memfd_offset);
        if (p == MAP_FAILED) {
            throw std::runtime_error("cannot map into the window");
        }
        return static_cast<std::byte*>(p);
    }

    std::size_t reserved;
    std::byte* base = nullptr;
    std::byte* start = nullptr;
    int fd = -1;
};

struct MemoryMirror : testing::Test {
    void SetUp() override {
        ::setenv("VK_LOADER_DRIVERS_SELECT", "*lvp*", 0);  // as the compute runner
        auto app = make<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.apiVersion = VK_API_VERSION_1_3;
        auto instance_info = make<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        instance_info.pApplicationInfo = &app;
        if (vkCreateInstance(&instance_info, nullptr, &instance_) != VK_SUCCESS) {
            instance_ = VK_NULL_HANDLE;
            GTEST_SKIP() << "no Vulkan 1.3 instance";
        }
        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        if (std::ranges::none_of(devices, [this](VkPhysicalDevice d) { return create(d); })) {
            GTEST_SKIP() << "no Vulkan 1.3 device with sparse buffers and host and dma-buf import";
        }
    }

    void TearDown() override {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            for (VkBuffer b : buffers_) {
                vkDestroyBuffer(device_, b, nullptr);
            }
            for (VkDeviceMemory m : imports_) {  // before the host unmaps the window
                vkFreeMemory(device_, m, nullptr);
            }
            vkDestroyPipelineLayout(device_, layout_, nullptr);
            vkDestroyFence(device_, fence_, nullptr);
            vkDestroyCommandPool(device_, pool_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        vkDestroyInstance(instance_, nullptr);
        window_.reset();
        if (udmabuf_ >= 0) {
            ::close(udmabuf_);
        }
    }

    // Creates the device on `d` if it has what the mirror needs, and measures it.
    bool create(VkPhysicalDevice d) {
        auto host = make<VkPhysicalDeviceExternalMemoryHostPropertiesEXT>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT);
        auto p13 = make<VkPhysicalDeviceVulkan13Properties>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, &host);
        auto p12 = make<VkPhysicalDeviceVulkan12Properties>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p13);
        auto props = make<VkPhysicalDeviceProperties2>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12);
        vkGetPhysicalDeviceProperties(d, &props.properties);
        if (props.properties.apiVersion < VK_API_VERSION_1_3) {
            return false;
        }
        vkGetPhysicalDeviceProperties2(d, &props);
        auto f12 = make<VkPhysicalDeviceVulkan12Features>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        auto features = make<VkPhysicalDeviceFeatures2>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12);
        vkGetPhysicalDeviceFeatures2(d, &features);
        std::uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> offered(n);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &n, offered.data());
        const std::array extensions{VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
                                    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
        const auto offers = [&](std::string_view name) {
            return std::ranges::any_of(offered, [&](auto& e) { return e.extensionName == name; });
        };
        vkGetPhysicalDeviceQueueFamilyProperties(d, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &n, families.data());
        constexpr VkQueueFlags needed = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
        const auto family = static_cast<std::uint32_t>(std::ranges::find_if(families, [](auto& q) {
            return (q.queueFlags & needed) == needed;
        }) - families.begin());
        const VkPhysicalDeviceFeatures& f = features.features;
        if (!f.sparseBinding || !f.sparseResidencyBuffer || !f.shaderInt64 ||
            !f12.bufferDeviceAddress || !std::ranges::all_of(extensions, offers) || family == n) {
            return false;
        }

        auto enable12 = make<VkPhysicalDeviceVulkan12Features>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        enable12.bufferDeviceAddress = VK_TRUE;
        auto enable = make<VkPhysicalDeviceFeatures2>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &enable12);
        enable.features.sparseBinding = VK_TRUE;
        enable.features.sparseResidencyBuffer = VK_TRUE;
        enable.features.shaderInt64 = VK_TRUE;
        const float priority = 1.0f;
        auto queue_info = make<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        auto device_info = make<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enable);
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = extensions.size();
        device_info.ppEnabledExtensionNames = extensions.data();
        check(vkCreateDevice(d, &device_info, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, family, 0, &queue_);
        host_pointer_properties_ = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
            vkGetDeviceProcAddr(device_, "vkGetMemoryHostPointerPropertiesEXT"));
        fd_properties_ = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));
        auto pool_info = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool_info.queueFamilyIndex = family;
        check(vkCreateCommandPool(device_, &pool_info, nullptr, &pool_), "vkCreateCommandPool");
        auto commands = make<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        commands.commandPool = pool_;
        commands.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &commands, &commands_), "vkAllocateCommandBuffers");
        auto fence_info = make<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        check(vkCreateFence(device_, &fence_info, nullptr, &fence_), "vkCreateFence");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkDeviceAddress)};
        auto layout_info = make<VkPipelineLayoutCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &range;
        check(vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_),
              "vkCreatePipelineLayout");

        lavapipe_ = p12.driverID == VK_DRIVER_ID_MESA_LLVMPIPE;
        sparse_space_ = props.properties.limits.sparseAddressSpaceSize;
        // The tests' buffer size: 2 GiB, halved until two buffers fit the sparse address space.
        for (buffer_size_ = 2 * gib; 2 * buffer_size_ > sparse_space_;) {
            buffer_size_ /= 2;
        }
        VkMemoryRequirements needs;
        const VkBuffer probe = sparse_buffer(dma_buf);
        vkGetBufferMemoryRequirements(device_, probe, &needs);
        granularity_ = needs.alignment;
        vkDestroyBuffer(device_, probe, nullptr);  // so that the tests' buffers fit
        buffers_.pop_back();
        std::cout << props.properties.deviceName << ", " << p12.driverName << ' '
                  << p12.driverInfo << std::hex << "\nmaxBufferSize 0x" << p13.maxBufferSize
                  << ", sparseAddressSpaceSize 0x" << sparse_space_
                  << ", sparse buffer alignment 0x" << granularity_
                  << ", minImportedHostPointerAlignment 0x" << host.minImportedHostPointerAlignment
                  << std::dec << ", bufferDeviceAddressCaptureReplay "
                  << f12.bufferDeviceAddressCaptureReplay << '\n';
        // What the design relies on (docs/design/graphics.md, "GPU memory mirror").
        EXPECT_GE(p13.maxBufferSize, 2 * gib - 1);
        EXPECT_LE(host.minImportedHostPointerAlignment, guest_page);
        EXPECT_LE(granularity_, 64 * kib);
        return true;
    }

    // A sparse buffer (of the tests' size by default) that memory of `handle` type binds into.
    VkBuffer sparse_buffer(VkExternalMemoryHandleTypeFlagBits handle, VkDeviceSize size = 0) {
        auto external = make<VkExternalMemoryBufferCreateInfo>(
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
        external.handleTypes = handle;
        auto info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external);
        info.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
        info.size = size != 0 ? size : buffer_size_;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VkBuffer buffer;
        check(vkCreateBuffer(device_, &info, nullptr, &buffer), "vkCreateBuffer");
        buffers_.push_back(buffer);
        return buffer;
    }

    VkDeviceAddress address(VkBuffer buffer) const {
        auto info = make<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
        info.buffer = buffer;
        return vkGetBufferDeviceAddress(device_, &info);
    }

    // Imported memory: `import` is the import structure, `types` the importable memory types.
    VkDeviceMemory allocate(void* import, std::uint32_t types, VkDeviceSize size) {
        VkMemoryRequirements needs;
        vkGetBufferMemoryRequirements(device_, buffers_.back(), &needs);
        if ((types &= needs.memoryTypeBits) == 0) {
            throw std::runtime_error("no memory type for both the import and a sparse buffer");
        }
        auto flags = make<VkMemoryAllocateFlagsInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                                     import);
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        auto alloc = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags);
        alloc.allocationSize = size;
        alloc.memoryTypeIndex = static_cast<std::uint32_t>(__builtin_ctz(types));
        VkDeviceMemory memory;
        check(vkAllocateMemory(device_, &alloc, nullptr, &memory), "vkAllocateMemory (import)");
        imports_.push_back(memory);
        return memory;
    }

    VkDeviceMemory import_host(void* p, VkDeviceSize size) {
        auto pointer = make<VkMemoryHostPointerPropertiesEXT>(
            VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT);
        check(host_pointer_properties_(device_, host_pointer, p, &pointer),
              "vkGetMemoryHostPointerPropertiesEXT");
        auto import = make<VkImportMemoryHostPointerInfoEXT>(
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT);
        import.handleType = host_pointer;
        import.pHostPointer = p;
        return allocate(&import, pointer.memoryTypeBits, size);
    }

    // A memfd range as a dma-buf from udmabuf, imported.
    VkDeviceMemory import_memfd(int memfd, std::uint64_t offset, VkDeviceSize size) {
        udmabuf_create create{static_cast<std::uint32_t>(memfd), UDMABUF_FLAGS_CLOEXEC, offset,
                              size};
        const int fd = ::ioctl(udmabuf_, UDMABUF_CREATE, &create);
        if (fd < 0) {
            throw std::runtime_error("UDMABUF_CREATE failed: " + std::string(strerror(errno)));
        }
        auto fd_props = make<VkMemoryFdPropertiesKHR>(VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR);
        auto import = make<VkImportMemoryFdInfoKHR>(VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
        import.handleType = dma_buf;
        import.fd = fd;  // the memory owns it once it is allocated
        check(fd_properties_(device_, dma_buf, fd, &fd_props), "vkGetMemoryFdPropertiesKHR");
        return allocate(&import, fd_props.memoryTypeBits, size);
    }

    // Binds `memory` at `memory_offset` (or nothing) to `size` bytes of the buffer at `offset`.
    void bind(VkBuffer buffer, VkDeviceSize offset, VkDeviceMemory memory,
              VkDeviceSize memory_offset, VkDeviceSize size) {
        VkSparseMemoryBind range{offset, size, memory, memory_offset, 0};
        VkSparseBufferMemoryBindInfo buffer_bind{buffer, 1, &range};
        auto info = make<VkBindSparseInfo>(VK_STRUCTURE_TYPE_BIND_SPARSE_INFO);
        info.bufferBindCount = 1;
        info.pBufferBinds = &buffer_bind;
        check(vkQueueBindSparse(queue_, 1, &info, fence_), "vkQueueBindSparse");
        wait();
    }

    void wait() {
        check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 30'000'000'000), "vkWaitForFences");
        check(vkResetFences(device_, 1, &fence_), "vkResetFences");
    }

    // Runs mem_mirror on 64 lanes with the draw block at device address `block`.
    void run(VkDeviceAddress block) {
        const auto translation = bb::gcn::translate(
            bb::test::gcn::program_words("mem_mirror"), {},
            {.pgm_rsrc2 = bb::test::gcn::program_pgm_rsrc2("mem_mirror"),
             .num_threads = {64, 1, 1}});
        auto module_info = make<VkShaderModuleCreateInfo>(
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        module_info.codeSize = translation.spirv.size() * 4;
        module_info.pCode = translation.spirv.data();
        VkShaderModule module;
        check(vkCreateShaderModule(device_, &module_info, nullptr, &module),
              "vkCreateShaderModule");
        auto pipeline_info = make<VkComputePipelineCreateInfo>(
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = layout_;
        VkPipeline pipeline;  // both leak when a call fails, which fails the test
        check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &pipeline), "vkCreateComputePipelines");
        vkDestroyShaderModule(device_, module, nullptr);

        check(vkResetCommandPool(device_, pool_, 0), "vkResetCommandPool");
        auto begin = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(commands_, &begin), "vkBeginCommandBuffer");
        vkCmdBindPipeline(commands_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(commands_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof block,
                           &block);
        vkCmdDispatch(commands_, 1, 1, 1);
        auto barrier = make<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(commands_), "vkEndCommandBuffer");
        auto submit = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands_;
        check(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");
        wait();
        vkDestroyPipeline(device_, pipeline, nullptr);
    }

    // Mapping k lies in mirror buffer k. The program reads its input and writes its output there
    // through the bias of that buffer, twice, and sees what the CPU wrote in between.
    void read_and_write(std::span<const VkBuffer, 2> buffers, std::array<std::byte*, 2> mappings) {
        for (std::size_t k = 0; k < 2; ++k) {
            std::byte* p = mappings[k];
            const std::uint64_t bias = address(buffers[k]) - at(window_->start + k * buffer_size_);
            const std::array<std::uint64_t, 2> user_data{at(p + 0x1000), at(p + 0x100)};
            std::memcpy(p + bb::gcn::draw_user_data_offset, user_data.data(), sizeof user_data);
            std::memcpy(p + bb::gcn::draw_address_bias_offset, &bias, sizeof bias);
            for (std::uint32_t value : {0x1234'0000u, 0xabcd'0000u}) {
                std::memcpy(p + 0x100, &value, 4);
                run(at(p) + bias);
                for (std::uint32_t i = 0; i < 64; ++i) {  // lane i writes value + i to out[i]
                    std::uint32_t word;
                    std::memcpy(&word, p + 0x1000 + 4 * i, 4);
                    ASSERT_EQ(word, value + i) << "lane " << i;
                }
            }
        }
    }

    std::optional<Window> window_;  // outlives the imports
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    PFN_vkGetMemoryHostPointerPropertiesEXT host_pointer_properties_ = nullptr;
    PFN_vkGetMemoryFdPropertiesKHR fd_properties_ = nullptr;
    VkDeviceSize buffer_size_ = 0, granularity_ = 0, sparse_space_ = 0;
    bool lavapipe_ = false;
    int udmabuf_ = -1;
    std::vector<VkBuffer> buffers_;
    std::vector<VkDeviceMemory> imports_;
};

// The default mirror, 32 GiB in 2 GiB buffers, as far as the sparse address space holds it.
// Prints whether the buffers' device addresses happen to be consecutive.
TEST_F(MemoryMirror, BufferAddresses) {
    const std::uint64_t count = std::min<std::uint64_t>(16, sparse_space_ / (2 * gib));
    std::vector<VkDeviceAddress> addresses{0};
    bool consecutive = true;
    for (std::uint64_t k = 0; k < count; ++k) {
        addresses.push_back(address(sparse_buffer(dma_buf, 2 * gib)));
        consecutive &= k == 0 || addresses[k + 1] == addresses[k] + 2 * gib;
    }
    std::cout << count << " buffers of 2 GiB from 0x" << std::hex << addresses[count != 0]
              << std::dec << ", consecutive addresses: " << consecutive << '\n';
}

// Host pointers of anonymous memory. RADV refuses a host pointer into a memfd view, and lavapipe
// takes one, but its sparse binding of a host pointer import maps nothing: a shader's access
// would fault, so lavapipe skips.
TEST_F(MemoryMirror, HostPointerImports) {
    window_.emplace(2 * buffer_size_, 64 * kib);
    const std::array buffers{sparse_buffer(host_pointer), sparse_buffer(host_pointer)};
    try {
        import_host(window_->map(0, 64 * kib, 0), 64 * kib);
        std::cout << "host pointer import of a memfd view: accepted\n";
    } catch (const std::runtime_error& e) {
        std::cout << "host pointer import of a memfd view: " << e.what() << '\n';
    }
    if (lavapipe_) {
        GTEST_SKIP() << "lavapipe binds nothing for a host pointer import";
    }
    std::array<std::byte*, 2> mappings{};
    for (std::size_t k = 0; k < 2; ++k) {
        const std::uint64_t offset = k * buffer_size_ + (3 + 2 * k) * 64 * kib;
        mappings[k] = window_->map(offset, 64 * kib, -1);
        bind(buffers[k], offset % buffer_size_, import_host(mappings[k], 64 * kib), 0, 64 * kib);
    }
    read_and_write(buffers, mappings);
}

// Dma-bufs of memfd ranges, as direct memory is mapped. The second mapping is one guest page in
// the middle of a binding block: its window offset and memfd offset agree modulo the block, and
// the whole block of the memfd is bound. Unbinding at the end is the mirror's unmap.
TEST_F(MemoryMirror, MemfdImports) {
    udmabuf_ = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udmabuf_ < 0) {
        GTEST_SKIP() << "cannot open /dev/udmabuf: " << strerror(errno);
    }
    const std::uint64_t block = std::max<std::uint64_t>(granularity_, guest_page);
    window_.emplace(2 * buffer_size_, 4 * block);
    const std::array buffers{sparse_buffer(dma_buf), sparse_buffer(dma_buf)};
    const std::array<std::uint64_t, 2> offsets{3 * block, buffer_size_ + 5 * block + guest_page};
    const std::array<std::uint64_t, 2> memfd_offsets{0, 2 * block + guest_page};
    const std::array<std::size_t, 2> sizes{block, guest_page};
    VkDeviceMemory memory = import_memfd(window_->fd, 0, 4 * block);
    std::array<std::byte*, 2> mappings{};
    for (std::size_t k = 0; k < 2; ++k) {
        mappings[k] = window_->map(offsets[k], sizes[k], static_cast<off_t>(memfd_offsets[k]));
        const std::uint64_t skew = offsets[k] % block;
        bind(buffers[k], offsets[k] % buffer_size_ - skew, memory, memfd_offsets[k] - skew, block);
    }
    read_and_write(buffers, mappings);
    bind(buffers[1], offsets[1] % buffer_size_ / block * block, VK_NULL_HANDLE, 0, block);
}

}  // namespace
