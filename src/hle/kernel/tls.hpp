// Thread-local storage of the game's modules (docs/design/m3-core-os.md, "TLS: one thread pointer
// for game and runtime"). Game code runs with glibc's thread pointer. The eboot's block is a
// reservation directly below it (the OBJECT library bbeboottls), the PRXs' blocks come from the
// TLS helper.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "loader/image.hpp"
#include "loader/load_set.hpp"

namespace bb::hle::kernel {

// ---- the eboot's static block ------------------------------------------------------------------

// The reservation for the eboot's PT_TLS block (p_memsz 0x750, p_align 0x10, p_filesz 0). The
// eboot reaches it as fs:0 plus constants in [-0x750, -0xb].
inline constexpr std::size_t eboot_tls_size = 0x750;
inline constexpr std::size_t eboot_tls_align = 16;

// The calling thread's thread pointer (fs:0, the TCB's self pointer in glibc).
std::uintptr_t thread_pointer();

// Where the calling thread's eboot block lies.
struct EbootTlsPlacement {
    std::uintptr_t block;           // start of the reservation
    std::uintptr_t thread_pointer;  // fs:0

    bool ends_at_thread_pointer() const { return block + eboot_tls_size == thread_pointer; }
};

// The calling thread's placement, or nothing when the executable doesn't link bbeboottls.
std::optional<EbootTlsPlacement> eboot_tls_placement();

// Aborts with both addresses unless the calling thread's eboot block ends exactly at its thread
// pointer. Does nothing when the executable doesn't link bbeboottls. bbeboottls runs it at
// start-up; every thread that runs game code runs it before the first game instruction.
void check_eboot_tls();

// Called once by bbeboottls at static initialization: `block` returns the start of the calling
// thread's reservation. Throws std::logic_error when called twice.
void set_eboot_tls_reservation(std::uintptr_t (*block)());

// ---- the PRXs' blocks --------------------------------------------------------------------------

// Registers the TLS template of module `id`: the initialization image at `init` (tls.file_size
// bytes), the block size tls.mem_size and the alignment tls.align. tls.link_addr places the block:
// it starts at an address congruent to link_addr modulo align, so the offsets the module was
// linked with keep their alignment. Throws std::logic_error when `id` is already registered,
// std::invalid_argument when id is eboot_module_id (the eboot's block is the reservation), the
// alignment is not a power of two (0 counts as 1) or file_size exceeds mem_size.
void register_tls_module(std::uint64_t id, const std::byte* init, const loader::TlsTemplate& tls);

// Registers the template of every module of `set` other than the eboot that has a PT_TLS segment.
// Throws std::runtime_error when the eboot's template doesn't fit the reservation (eboot_tls_size,
// eboot_tls_align, no initialization image), else as register_tls_module.
void register_tls_modules(const loader::LoadSet& set);

// The TLS helper, libkernel vNe1w4diLCs: `index` points to a {module id, offset} pair of two
// 64-bit cells, not necessarily 8-byte aligned. Returns the address at `offset` in the calling
// thread's block of that module, which it creates on first use from the module's template (file
// bytes copied, the rest zero). The block is freed when the thread exits. Aborts for a module
// without a registered template or an offset outside its block.
void* __tls_get_addr(const void* index);

// The number of blocks __tls_get_addr has created and not yet freed, over all threads.
std::size_t live_tls_blocks();

}  // namespace bb::hle::kernel
