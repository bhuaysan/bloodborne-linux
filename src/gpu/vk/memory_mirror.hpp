// bbvk's GPU memory mirror (docs/design/graphics.md, "GPU memory mirror", item 21): the first
// part of the guest window in sparse buffers of 2 GiB, the GPU-visible views of the direct
// memory memfd bound into them at their window offsets as udmabuf dma-bufs, and the bias table
// that the draw block's address_bias field points to.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

namespace bb::vk {

class Device;

inline constexpr std::uint64_t mirror_buffer_size = std::uint64_t{2} << 30;
// One bias table entry per buffer: at most 16 buffers (bb::gcn::bias_entries).
inline constexpr std::uint64_t mirror_max_size = 16 * mirror_buffer_size;
// The largest import: udmabuf's default size_limit_mb.
inline constexpr std::uint64_t mirror_import_limit = std::uint64_t{64} << 20;

struct MirrorOptions {
    int memfd = -1;               // sealed against shrinking (F_SEAL_SHRINK)
    std::uintptr_t window_start;  // the guest address of the mirror's start: a multiple of 2 GiB
    std::uint64_t size = mirror_max_size;  // a multiple of 2 GiB, at most mirror_max_size
    // The binding block is max(the buffers' binding granularity, 16 KiB, min_block). Tests raise
    // it to RADV's 64 KiB on lavapipe. A power of two up to mirror_import_limit.
    std::uint64_t min_block = 0;
};

// Guest ranges are bound in whole blocks. The memfd block of a window block is fixed while the
// window block is bound, which needs window offset ≡ memfd offset (mod block). Each bound block
// counts the guest pages (16 KiB) bound in it and is unbound when the count drops to 0. Imports
// are made per run of newly bound blocks, at most mirror_import_limit long and never across a
// multiple of it in the window (so never across two buffers); an import is freed once none of its
// blocks is bound.
//
// Thread-safe. Calls wait for their binds on the device's sparse queue. Vulkan failures throw
// std::runtime_error. A failed bind() leaves the mirror as it was; a failed unbind() may leave
// blocks bound that it meant to unbind.
class MemoryMirror {
public:
    // Nothing when the device can't hold the mirror or /dev/udmabuf doesn't open; `why` then says
    // why. The device and the memfd must outlive the mirror.
    static std::unique_ptr<MemoryMirror> create(Device& device, const MirrorOptions& options,
                                                std::string& why);
    ~MemoryMirror();
    MemoryMirror(const MemoryMirror&) = delete;
    MemoryMirror& operator=(const MemoryMirror&) = delete;

    // The device address of the bias table: bias[(address >> 31) & 15] takes guest address
    // `address` in buffer k to the same offset of buffer k. The entries of the window's 2 GiB
    // steps past the mirror repeat buffer 0's bias.
    VkDeviceAddress bias_table() const { return table_address_; }
    std::uint64_t block() const { return block_; }
    std::uintptr_t window_start() const { return window_start_; }
    std::uint64_t size() const { return buffers_.size() * mirror_buffer_size; }

    // Binds the guest pages [address, address + length), a view of the memfd at `offset`.
    // Returns false and logs, binding nothing, when the range is not whole guest pages, reaches
    // past the mirror or breaks the congruence. A block already bound to another memfd block
    // keeps it (logged).
    bool bind(std::uintptr_t address, std::uint64_t length, std::uint64_t offset);
    // Unbinds the guest pages [address, address + length), a view of the memfd at `offset`,
    // that bind() bound.
    void unbind(std::uintptr_t address, std::uint64_t length, std::uint64_t offset);
    // Direct memory [offset, offset + length) is about to be released: unbinds every block that
    // views it and frees the imports that hold it (they pin its pages). The other blocks of
    // those imports are bound again. Bound blocks that view the range are logged. Returns the
    // window ranges (address, length) of the blocks it dropped: whole blocks, so also the pages
    // of a neighbouring mapping that shares a block with the range.
    std::vector<std::pair<std::uintptr_t, std::uint64_t>> release(std::uint64_t offset,
                                                                  std::uint64_t length);
    // Buffer k of the mirror (window offsets k * 2 GiB), for tests that copy from it.
    VkBuffer buffer(std::size_t k) const { return buffers_.at(k); }
    // Entry `e` of the bias table as the host wrote it, for tests.
    std::uint64_t bias(std::size_t e) const { return table_map_[e]; }
    // The number of live imports, for tests.
    std::size_t imports() const;

private:
    struct Block {
        std::uint64_t offset = 0;  // in the memfd
        std::uint64_t pages = 0;   // guest pages bound in it
        std::uint64_t import = 0;  // the id of its import
    };
    struct Import {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::uint64_t offset = 0, length = 0;  // in the memfd
        std::uint64_t blocks = 0;              // bound blocks
    };

    explicit MemoryMirror(Device& device) : device_(device) {}
    VkDeviceMemory import(std::uint64_t offset, std::uint64_t length);
    // Imports and binds `blocks`, by window offset; all or, on a throw, none. Needs mutex_.
    void bind_new(const std::map<std::uint64_t, Block>& blocks);
    // Queues a bind of `memory` at `memory_offset` (or nothing) over window offsets
    // [window, window + length) in one buffer; submit() binds and waits.
    void queue(std::uint64_t window, std::uint64_t length, VkDeviceMemory memory,
               std::uint64_t memory_offset);
    void submit();
    // Queues the unbind of `block` and forgets it, and its import once that has no bound block
    // left; the caller frees the import's memory after submit(). Needs mutex_.
    void drop(std::map<std::uint64_t, Block>::iterator block, std::vector<VkDeviceMemory>& freed);

    // Submits the queued unbinds, then frees `freed`, also when the submit throws.
    void release(std::vector<VkDeviceMemory>& freed);

    Device& device_;
    int memfd_ = -1, udmabuf_ = -1;
    std::uintptr_t window_start_ = 0;
    std::uint64_t block_ = 0;
    std::uint32_t memory_types_ = 0;  // the sparse buffers' memoryTypeBits
    std::vector<VkBuffer> buffers_;
    VkBuffer table_ = VK_NULL_HANDLE;
    VkDeviceMemory table_memory_ = VK_NULL_HANDLE;
    VkDeviceAddress table_address_ = 0;
    const std::uint64_t* table_map_ = nullptr;
    VkFence fence_ = VK_NULL_HANDLE;
    PFN_vkGetMemoryFdPropertiesKHR fd_properties_ = nullptr;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, Block> blocks_;    // bound blocks by window offset
    std::map<std::uint64_t, Import> imports_;  // by id, from next_import_
    std::uint64_t next_import_ = 0;
    std::vector<std::vector<VkSparseMemoryBind>> pending_;  // per buffer, for submit()
};

}  // namespace bb::vk
