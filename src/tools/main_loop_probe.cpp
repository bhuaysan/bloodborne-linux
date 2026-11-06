#include "tools/main_loop_probe.hpp"

namespace bb::tools {

bool MainLoopProbe::count(std::uintptr_t return_address) {
    if (return_address < begin_ || return_address >= end_ ||
        main_returned_.load(std::memory_order_acquire)) {
        return false;
    }
    // Stop counting once the threshold is passed, so the counter never wraps around to it again.
    if (calls_.load(std::memory_order_acquire) >= threshold_) {
        return false;
    }
    return calls_.fetch_add(1, std::memory_order_acq_rel) + 1 == threshold_;
}

}  // namespace bb::tools
