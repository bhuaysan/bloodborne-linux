// Thread-local storage of the game's modules (docs/design/m3-core-os.md, "TLS: one thread pointer
// for game and runtime"). Game code runs with glibc's thread pointer. The eboot's block is a
// reservation directly below it (the OBJECT library bbeboottls).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

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

}  // namespace bb::hle::kernel
