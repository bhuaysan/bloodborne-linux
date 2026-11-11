// Alternate signal stacks for the threads that run game code (docs/design/renderer.md, "Write
// tracking with mprotect and SIGSEGV"). The fault handler runs on the faulting thread. On its own
// stack it could overflow a game thread that is nearly out of stack, and it could not run at all
// after a stack overflow.
#pragma once

namespace bb::common {

// Gives the calling thread an alternate signal stack with a guard page below it, unless it has
// one already (AddressSanitizer gives every thread its own). The stack goes when the thread ends.
void ensure_signal_stack();

}  // namespace bb::common
