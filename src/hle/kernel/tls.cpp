#include "hle/kernel/tls.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace bb::hle::kernel {

namespace {

// ---- the eboot's static block ------------------------------------------------------------------

std::atomic<std::uintptr_t (*)()> eboot_block{nullptr};

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

}  // namespace bb::hle::kernel
