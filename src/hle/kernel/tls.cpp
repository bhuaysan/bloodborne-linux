#include "hle/kernel/tls.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

#include "hle/hle.hpp"
#include "loader/relocate.hpp"

namespace bb::hle::kernel {

namespace {

// ---- the eboot's static block ------------------------------------------------------------------

std::atomic<std::uintptr_t (*)()> eboot_block{nullptr};

// ---- the PRXs' blocks --------------------------------------------------------------------------

struct Module {
    std::uint64_t id;
    const std::byte* init;
    std::size_t file_size, mem_size, align;
    std::size_t first_byte;  // link_addr % align: where the block starts in its aligned allocation
};

std::mutex modules_mutex;
std::vector<Module> modules;  // guarded by modules_mutex

std::atomic<std::size_t> live_blocks{0};

[[noreturn]] void fail(const char* message, std::uint64_t module, std::uint64_t offset) {
    std::fprintf(stderr, "hle.kernel: TLS helper: %s (module %" PRIu64 ", offset %#" PRIx64 ")\n",
                 message, module, offset);
    std::abort();
}

// One thread's blocks. Frees them when the thread exits.
class ThreadBlocks {
public:
    ThreadBlocks() = default;
    ThreadBlocks(const ThreadBlocks&) = delete;
    ThreadBlocks& operator=(const ThreadBlocks&) = delete;

    ~ThreadBlocks() {
        for (const Block& b : blocks_) {
            ::operator delete(b.memory, std::align_val_t{b.align});
            live_blocks.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // The start of this thread's block of `module`, created on first use. Aborts as
    // __tls_get_addr does.
    std::byte* block(std::uint64_t module, std::uint64_t offset) {
        for (const Block& b : blocks_) {
            if (b.module == module) {
                if (offset >= b.size) {
                    fail("offset outside the module's block", module, offset);
                }
                return b.start;
            }
        }
        return create(module, offset);
    }

private:
    struct Block {
        std::uint64_t module;
        std::byte* memory;  // the allocation
        std::byte* start;   // memory + first_byte
        std::size_t size, align;
    };

    std::byte* create(std::uint64_t id, std::uint64_t offset) {
        Module m;
        {
            const std::lock_guard lock(modules_mutex);
            const auto it = std::ranges::find(modules, id, &Module::id);
            if (it == modules.end()) {
                fail("no TLS template registered for the module", id, offset);
            }
            m = *it;
        }
        if (offset >= m.mem_size) {
            fail("offset outside the module's block", id, offset);
        }
        auto* memory = static_cast<std::byte*>(
            ::operator new(m.first_byte + m.mem_size, std::align_val_t{m.align}));
        std::byte* start = memory + m.first_byte;
        if (m.file_size != 0) {
            std::memcpy(start, m.init, m.file_size);
        }
        std::memset(start + m.file_size, 0, m.mem_size - m.file_size);
        blocks_.push_back({id, memory, start, m.mem_size, m.align});
        live_blocks.fetch_add(1, std::memory_order_relaxed);
        return start;
    }

    std::vector<Block> blocks_;
};

thread_local ThreadBlocks thread_blocks;

}  // namespace

// ---- the eboot's static block ------------------------------------------------------------------

std::uintptr_t thread_pointer() {
    std::uintptr_t tp;
    asm("mov %%fs:0, %0" : "=r"(tp));
    return tp;
}

std::optional<EbootTlsPlacement> eboot_tls_placement() {
    const auto block = eboot_block.load(std::memory_order_acquire);
    if (block == nullptr) {
        return std::nullopt;
    }
    return EbootTlsPlacement{block(), thread_pointer()};
}

void check_eboot_tls() {
    const auto placement = eboot_tls_placement();
    if (placement && !placement->ends_at_thread_pointer()) {
        std::fprintf(stderr,
                     "hle.kernel: the eboot's TLS reservation [%#" PRIxPTR ", %#" PRIxPTR
                     ") does not end at the thread pointer %#" PRIxPTR
                     " (docs/design/m3-core-os.md, \"TLS\")\n",
                     placement->block, placement->block + eboot_tls_size,
                     placement->thread_pointer);
        std::abort();
    }
}

void set_eboot_tls_reservation(std::uintptr_t (*block)()) {
    std::uintptr_t (*expected)() = nullptr;
    if (!eboot_block.compare_exchange_strong(expected, block, std::memory_order_acq_rel)) {
        throw std::logic_error("hle.kernel: the eboot's TLS reservation is set twice");
    }
}

// ---- the PRXs' blocks --------------------------------------------------------------------------

void register_tls_module(std::uint64_t id, const std::byte* init, const loader::TlsTemplate& tls) {
    if (id == loader::eboot_module_id) {
        throw std::invalid_argument(
            "hle.kernel: the eboot's TLS block is the reservation, not a helper block");
    }
    const std::size_t align = std::max<std::size_t>(tls.align, 1);
    if (!std::has_single_bit(align) || tls.file_size > tls.mem_size) {
        throw std::invalid_argument(std::format(
            "hle.kernel: TLS module {}: bad template ({:#x} file bytes, {:#x} bytes, align {:#x})",
            id, tls.file_size, tls.mem_size, tls.align));
    }
    const std::lock_guard lock(modules_mutex);
    if (std::ranges::find(modules, id, &Module::id) != modules.end()) {
        throw std::logic_error(std::format("hle.kernel: TLS module {} is registered twice", id));
    }
    modules.push_back(
        {id, init, tls.file_size, tls.mem_size, align, tls.link_addr & (align - 1)});
}

void register_tls_modules(const loader::LoadSet& set) {
    for (const loader::LoadedModule& m : set.modules) {
        if (!m.image.tls) {
            continue;
        }
        const loader::TlsTemplate& tls = *m.image.tls;
        if (m.id == loader::eboot_module_id) {
            if (tls.mem_size > eboot_tls_size || tls.align > eboot_tls_align ||
                tls.file_size != 0) {
                throw std::runtime_error(std::format(
                    "hle.kernel: the eboot's TLS template ({:#x} bytes, align {:#x}, {:#x} file "
                    "bytes) doesn't fit its reservation",
                    tls.mem_size, tls.align, tls.file_size));
            }
            continue;
        }
        // Without file bytes the link address may lie outside every segment (read_tls).
        const auto* init = tls.file_size == 0
                               ? nullptr
                               : reinterpret_cast<const std::byte*>(m.image.runtime(tls.link_addr));
        register_tls_module(m.id, init, tls);
    }
}

void* __tls_get_addr(const void* index) {
    std::uint64_t pair[2];
    std::memcpy(pair, index, sizeof pair);
    return thread_blocks.block(pair[0], pair[1]) + pair[1];
}

std::size_t live_tls_blocks() {
    return live_blocks.load(std::memory_order_relaxed);
}

// The TLS helper's name is matched (symbols/nids.csv).
BB_HLE_FUNCTION("libkernel", "vNe1w4diLCs", __tls_get_addr);

}  // namespace bb::hle::kernel
