#include "shadps4_host/fault_handler.hpp"

#include <signal.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>

#include "common/log.hpp"
#include "core/signals.h"

namespace bb::shadps4 {
namespace {

// The actions from before the install, by slot().
struct sigaction previous[2]{};
std::atomic<std::uint64_t> claimed{0};
Core::SignalDispatch* dispatch = nullptr;

int slot(int sig) {
    return sig == SIGSEGV ? 0 : 1;
}

void on_fault(int sig, siginfo_t* info, void* context) {
    const int saved_errno = errno;
    // Only a fault the CPU raised (si_code > 0) has a fault address and can be retried.
    bool handled = false;
    if (info->si_code > 0) {
        try {
            handled = dispatch->DispatchAccessViolation(context, info->si_addr);
        } catch (const std::exception& e) {
            // An assertion of the derived code throws (failure.hpp); it can't leave this frame.
            common::log_fatal("signal", std::string("fault handler: ") + e.what());
        }
    }
    if (handled) {
        claimed.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Not ours. The action from before takes over: the retried instruction faults again, and
        // a sent signal is raised again, to arrive once this handler returns. Another thread's
        // fault in between meets that action too; the process is ending anyway.
        ::sigaction(sig, &previous[slot(sig)], nullptr);
        if (info->si_code <= 0) {
            ::raise(sig);
        }
    }
    errno = saved_errno;
}

}  // namespace

void install_fault_handler() {
    static std::once_flag once;
    std::call_once(once, [] {
        // Created here, not on the first fault: the handler must not allocate.
        dispatch = Core::Signals::Instance();
        struct sigaction action{};
        action.sa_sigaction = on_fault;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (const int sig : {SIGSEGV, SIGBUS}) {
            if (::sigaction(sig, &action, &previous[slot(sig)]) != 0) {
                common::log_fatal("signal", std::string("fault handler: sigaction: ") +
                                                std::strerror(errno));
            }
        }
    });
}

std::uint64_t claimed_faults() {
    return claimed.load(std::memory_order_relaxed);
}

}  // namespace bb::shadps4
