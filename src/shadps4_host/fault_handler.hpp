// The process's SIGSEGV and SIGBUS handler (docs/design/renderer.md, "Write tracking with mprotect
// and SIGSEGV"). video_core's page manager write-protects guest pages that the caches hold, and
// registers a handler with Core::Signals that invalidates a page when the game writes it. This is
// what passes the faults to Core::Signals.
#pragma once

#include <cstdint>

namespace bb::shadps4 {

// Installs the handler for SIGSEGV and SIGBUS, once; later calls do nothing. A fault that a
// Core::Signals handler claims is retried. Any other fault puts back the action from before the
// install (the default, or AddressSanitizer's report) and returns, so that the retried
// instruction faults again under it and the process ends as it would without this handler.
// Signals sent with kill or raise are passed on the same way. The handler runs on the faulting
// thread, on its alternate signal stack where it has one (common/signal_stack.hpp).
void install_fault_handler();

// How many faults the Core::Signals handlers claimed since the install.
std::uint64_t claimed_faults();

}  // namespace bb::shadps4
