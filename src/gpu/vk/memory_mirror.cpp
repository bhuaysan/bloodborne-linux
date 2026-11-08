#include "gpu/vk/memory_mirror.hpp"

#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "common/log.hpp"
#include "gpu/gcn/translate.hpp"
#include "gpu/vk/device.hpp"

namespace bb::vk {
namespace {

using common::LogLevel;

constexpr auto dma_buf = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
constexpr std::uint64_t guest_page = 0x4000;
constexpr std::string_view channel = "gpu.vk";

constexpr std::uint64_t round_down(std::uint64_t value, std::uint64_t alignment) {
    return value & ~(alignment - 1);
}

constexpr std::uint64_t round_up(std::uint64_t value, std::uint64_t alignment) {
    return round_down(value + alignment - 1, alignment);
}

VkDeviceAddress buffer_address(VkDevice device, VkBuffer buffer) {
    auto info = make<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &info);
}

// Memory of type `type` with a device address, importing `import` if not null.
VkDeviceMemory allocate(VkDevice device, VkDeviceSize size, std::uint32_t type,
                        void* import = nullptr) {
    auto flags = make<VkMemoryAllocateFlagsInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                                 import);
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    auto info = make<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags);
    info.allocationSize = size;
    info.memoryTypeIndex = type;
    VkDeviceMemory memory;
    check(vkAllocateMemory(device, &info, nullptr, &memory), "vkAllocateMemory");
    return memory;
}

}  // namespace

std::unique_ptr<MemoryMirror> MemoryMirror::create(Device& device, const MirrorOptions& options,
                                                   std::string& why) {
    const std::uint64_t count = options.size / mirror_buffer_size;
    if (options.size % mirror_buffer_size != 0 || count == 0 || count > 16 ||
        options.window_start % mirror_buffer_size != 0) {
        why = "the mirror needs a size of 2 to 32 GiB and a start in whole 2 GiB";
        return nullptr;
    }
    if (options.size > device.properties().core.limits.sparseAddressSpaceSize) {
        why = "the device's sparse address space is smaller than the mirror";
        return nullptr;
    }
    std::unique_ptr<MemoryMirror> m(new MemoryMirror(device));
    m->memfd_ = options.memfd;
    m->window_start_ = options.window_start;
    m->udmabuf_ = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (m->udmabuf_ < 0) {
        why = std::string("cannot open /dev/udmabuf: ") + std::strerror(errno);
        return nullptr;
    }
    const VkDevice d = device.handle();
    const std::uint32_t families[] = {device.graphics().family, device.sparse().family};
    auto external = make<VkExternalMemoryBufferCreateInfo>(
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
    external.handleTypes = dma_buf;
    auto info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external);
    info.flags = mirror_buffer_flags;
    info.size = mirror_buffer_size;
    info.usage = mirror_buffer_usage;
    if (families[0] != families[1]) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    }
    for (std::uint64_t k = 0; k < count; ++k) {
        check(vkCreateBuffer(d, &info, nullptr, &m->buffers_.emplace_back()), "vkCreateBuffer");
    }
    m->pending_.resize(count);
    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(d, m->buffers_[0], &needs);
    m->memory_types_ = needs.memoryTypeBits;
    m->block_ = std::max({needs.alignment, guest_page, options.min_block});
    if (m->block_ > mirror_import_limit || (m->block_ & (m->block_ - 1)) != 0) {
        why = "the mirror's binding block is no power of two up to 64 MiB";
        return nullptr;
    }
    m->fd_properties_ = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(d, "vkGetMemoryFdPropertiesKHR"));
    auto fence_info = make<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    check(vkCreateFence(d, &fence_info, nullptr, &m->fence_), "vkCreateFence");

    // The bias table, in host-coherent memory that the GPU reads.
    auto table_info = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    table_info.size = gcn::bias_table_size;
    table_info.usage = mirror_buffer_usage;
    check(vkCreateBuffer(d, &table_info, nullptr, &m->table_), "vkCreateBuffer");
    vkGetBufferMemoryRequirements(d, m->table_, &needs);
    const auto type = device.memory_type(needs.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!type) {
        why = "no host-coherent memory for the bias table";
        return nullptr;
    }
    m->table_memory_ = allocate(d, needs.size, *type);
    check(vkBindBufferMemory(d, m->table_, m->table_memory_, 0), "vkBindBufferMemory");
    void* map = nullptr;  // stays mapped for bias()
    check(vkMapMemory(d, m->table_memory_, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory");
    m->table_map_ = static_cast<const std::uint64_t*>(map);
    for (std::uint64_t e = 0; e < gcn::bias_entries; ++e) {
        std::uint64_t k = (e - (options.window_start / mirror_buffer_size)) % gcn::bias_entries;
        k = k < count ? k : 0;
        const std::uint64_t bias = buffer_address(d, m->buffers_[k]) -
                                   (options.window_start + k * mirror_buffer_size);
        std::memcpy(static_cast<std::byte*>(map) + 8 * e, &bias, sizeof bias);
    }
    m->table_address_ = buffer_address(d, m->table_);
    common::log(LogLevel::info, channel, "memory mirror: {} GiB from {:#x}, blocks of {:#x}",
                options.size >> 30, options.window_start, m->block_);
    why.clear();
    return m;
}

MemoryMirror::~MemoryMirror() {
    const VkDevice d = device_.handle();
    {
        const std::scoped_lock lock(device_.sparse().lock);
        vkQueueWaitIdle(device_.sparse().handle);
    }
    for (VkBuffer b : buffers_) {
        vkDestroyBuffer(d, b, nullptr);
    }
    for (const auto& [window, i] : imports_) {
        vkFreeMemory(d, i.memory, nullptr);
    }
    vkDestroyBuffer(d, table_, nullptr);
    vkFreeMemory(d, table_memory_, nullptr);
    vkDestroyFence(d, fence_, nullptr);
    if (udmabuf_ >= 0) {
        ::close(udmabuf_);
    }
}

std::size_t MemoryMirror::imports() const {
    const std::scoped_lock lock(mutex_);
    return imports_.size();
}

VkDeviceMemory MemoryMirror::import(std::uint64_t offset, std::uint64_t length) {
    udmabuf_create create{static_cast<std::uint32_t>(memfd_), UDMABUF_FLAGS_CLOEXEC, offset,
                          length};
    const int fd = ::ioctl(udmabuf_, UDMABUF_CREATE, &create);
    if (fd < 0) {
        throw std::runtime_error(std::string("UDMABUF_CREATE: ") + std::strerror(errno));
    }
    try {
        auto props = make<VkMemoryFdPropertiesKHR>(VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR);
        check(fd_properties_(device_.handle(), dma_buf, fd, &props), "vkGetMemoryFdPropertiesKHR");
        const std::uint32_t types = props.memoryTypeBits & memory_types_;
        if (types == 0) {
            throw std::runtime_error("no memory type for a dma-buf in a sparse buffer");
        }
        auto info = make<VkImportMemoryFdInfoKHR>(VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
        info.handleType = dma_buf;
        info.fd = fd;  // the memory owns it once it is allocated
        return allocate(device_.handle(), length,
                        static_cast<std::uint32_t>(__builtin_ctz(types)), &info);
    } catch (...) {
        ::close(fd);
        throw;
    }
}

void MemoryMirror::queue(std::uint64_t window, std::uint64_t length, VkDeviceMemory memory,
                         std::uint64_t memory_offset) {
    auto& binds = pending_[window / mirror_buffer_size];
    window %= mirror_buffer_size;
    if (!binds.empty()) {  // extends the last bind where it continues
        VkSparseMemoryBind& last = binds.back();
        if (last.memory == memory && last.resourceOffset + last.size == window &&
            (memory == VK_NULL_HANDLE || last.memoryOffset + last.size == memory_offset)) {
            last.size += length;
            return;
        }
    }
    binds.push_back({window, length, memory, memory_offset, 0});
}

void MemoryMirror::submit() {
    std::vector<VkSparseBufferMemoryBindInfo> buffers;
    for (std::size_t k = 0; k < buffers_.size(); ++k) {
        if (!pending_[k].empty()) {
            buffers.push_back({buffers_[k], static_cast<std::uint32_t>(pending_[k].size()),
                               pending_[k].data()});
        }
    }
    if (buffers.empty()) {
        return;
    }
    auto info = make<VkBindSparseInfo>(VK_STRUCTURE_TYPE_BIND_SPARSE_INFO);
    info.bufferBindCount = static_cast<std::uint32_t>(buffers.size());
    info.pBufferBinds = buffers.data();
    Queue& queue = device_.sparse();
    const VkDevice d = device_.handle();
    {
        const std::scoped_lock lock(queue.lock);
        const VkResult result = vkQueueBindSparse(queue.handle, 1, &info, fence_);
        for (auto& binds : pending_) {
            binds.clear();
        }
        check(result, "vkQueueBindSparse");
    }
    check(vkWaitForFences(d, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    check(vkResetFences(d, 1, &fence_), "vkResetFences");
}

void MemoryMirror::bind_new(const std::map<std::uint64_t, Block>& blocks) {
    std::vector<std::uint64_t> done;  // the blocks of this call that are bound
    try {
        for (auto it = blocks.begin(); it != blocks.end();) {
            // A run: contiguous in the window and the memfd, inside one import_limit step.
            auto end = std::next(it);
            std::uint64_t length = block_;
            while (end != blocks.end() && end->first == it->first + length &&
                   end->second.offset == it->second.offset + length &&
                   end->first % mirror_import_limit != 0) {
                ++end;
                length += block_;
            }
            const VkDeviceMemory memory = import(it->second.offset, length);
            queue(it->first, length, memory, 0);
            try {
                submit();
            } catch (...) {
                vkFreeMemory(device_.handle(), memory, nullptr);
                throw;
            }
            const std::uint64_t id = next_import_++;
            imports_.emplace(id, Import{memory, it->second.offset, length, length / block_});
            for (; it != end; ++it) {
                blocks_.insert_or_assign(it->first, Block{it->second.offset, it->second.pages, id});
                done.push_back(it->first);
            }
        }
    } catch (...) {
        // Undo the runs that were bound; the failure itself is the one to report.
        try {
            std::vector<VkDeviceMemory> freed;
            for (std::uint64_t at : done) {
                drop(blocks_.find(at), freed);
            }
            release(freed);
        } catch (...) {
        }
        throw;
    }
}

bool MemoryMirror::bind(std::uintptr_t address, std::uint64_t length, std::uint64_t offset) {
    const std::uint64_t window = address - window_start_;
    if (address < window_start_ || length == 0 || (window | length | offset) % guest_page != 0 ||
        window >= size() || length > size() - window || window % block_ != offset % block_) {
        common::log(LogLevel::warn, channel,
                    "memory mirror: {:#x}-{:#x} at memfd offset {:#x} not mirrored", address,
                    address + length, offset);
        return false;
    }
    const std::scoped_lock lock(mutex_);
    std::map<std::uint64_t, Block> fresh;
    std::vector<std::pair<Block*, std::uint64_t>> shared;  // counted once the new blocks are bound
    for (std::uint64_t at = round_down(window, block_); at < window + length; at += block_) {
        const std::uint64_t pages =
            (std::min(at + block_, window + length) - std::max(at, window)) / guest_page;
        const std::uint64_t memfd = offset + at - window;
        const auto it = blocks_.find(at);
        if (it == blocks_.end()) {
            fresh.emplace(at, Block{memfd, pages, 0});
        } else if (it->second.offset == memfd) {
            shared.emplace_back(&it->second, pages);
        } else {
            common::log(LogLevel::warn, channel,
                        "memory mirror: block {:#x} stays bound to memfd offset {:#x}, not {:#x}",
                        window_start_ + at, it->second.offset, memfd);
        }
    }
    bind_new(fresh);  // map nodes keep their addresses, so `shared` stays valid
    for (auto& [block, pages] : shared) {
        block->pages += pages;
    }
    return true;
}

void MemoryMirror::drop(std::map<std::uint64_t, Block>::iterator block,
                        std::vector<VkDeviceMemory>& freed) {
    queue(block->first, block_, VK_NULL_HANDLE, 0);
    const auto import = imports_.find(block->second.import);
    if (--import->second.blocks == 0) {
        freed.push_back(import->second.memory);
        imports_.erase(import);
    }
    blocks_.erase(block);
}

void MemoryMirror::unbind(std::uintptr_t address, std::uint64_t length, std::uint64_t offset) {
    const std::uint64_t window = address - window_start_;
    const std::scoped_lock lock(mutex_);
    std::vector<VkDeviceMemory> freed;
    for (auto it = blocks_.lower_bound(round_down(window, block_));
         it != blocks_.end() && it->first < window + length;) {
        const std::uint64_t at = it->first;
        const std::uint64_t pages =
            (std::min(at + block_, window + length) - std::max(at, window)) / guest_page;
        auto next = std::next(it);
        if (it->second.offset == offset + at - window) {  // else bind() left it to another view
            it->second.pages -= std::min(pages, it->second.pages);
            if (it->second.pages == 0) {
                drop(it, freed);
            }
        }
        it = next;
    }
    release(freed);
}

void MemoryMirror::release(std::vector<VkDeviceMemory>& freed) {
    try {
        submit();  // the imports go only once nothing is bound to them
    } catch (...) {
        // The maps have forgotten them: free them once the queue is idle, whatever it bound.
        {
            const std::scoped_lock lock(device_.sparse().lock);
            vkQueueWaitIdle(device_.sparse().handle);
        }
        for (VkDeviceMemory memory : freed) {
            vkFreeMemory(device_.handle(), memory, nullptr);
        }
        throw;
    }
    for (VkDeviceMemory memory : freed) {
        vkFreeMemory(device_.handle(), memory, nullptr);
    }
}

void MemoryMirror::release(std::uint64_t offset, std::uint64_t length) {
    const std::scoped_lock lock(mutex_);
    // The imports that hold part of the range are freed whole, so every block of theirs goes.
    // The blocks that don't touch the range come back.
    std::map<std::uint64_t, Block> survivors;
    std::vector<VkDeviceMemory> freed;
    std::vector<std::uint64_t> hit;
    for (const auto& [id, i] : imports_) {
        if (i.offset < offset + length && offset < i.offset + i.length) {
            hit.push_back(id);
        }
    }
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        auto next = std::next(it);
        const Block& b = it->second;
        if (std::find(hit.begin(), hit.end(), b.import) != hit.end()) {
            if (b.offset + block_ <= offset || b.offset >= offset + length) {
                survivors.emplace(it->first, b);
            } else {
                common::log(LogLevel::warn, channel,
                            "memory mirror: memfd {:#x}-{:#x} released while bound at {:#x}",
                            offset, offset + length, window_start_ + it->first);
            }
            drop(it, freed);
        }
        it = next;
    }
    release(freed);
    bind_new(survivors);
}

}  // namespace bb::vk
