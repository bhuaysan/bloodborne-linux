// Hands the eboot's TLS reservation (eboot_tls.S) to bb::hle::kernel and checks its placement on
// the initial thread at start-up.

#include <cstdint>

#include "hle/kernel/tls.hpp"

extern "C" std::uintptr_t bb_eboot_tls_block();  // eboot_tls.S

namespace {

struct Reservation {
    Reservation() {
        bb::hle::kernel::set_eboot_tls_reservation(&bb_eboot_tls_block);
        bb::hle::kernel::check_eboot_tls();
    }
};

const Reservation reservation;

}  // namespace
