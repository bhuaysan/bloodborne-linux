#include "common/signal_stack.hpp"

#include <signal.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstddef>
#include <cstring>

#include "common/log.hpp"
#include "common/memory.hpp"

namespace bb::common {
namespace {

// The fault handler runs video_core's invalidation on it, so more than the usual few KiB.
constexpr std::size_t stack_size = std::size_t{128} << 10;

// The stack this thread installed, removed and unmapped when the thread ends.
struct OwnStack {
    void* base = nullptr;  // the guard page, then the stack
    std::size_t length = 0;
    ~OwnStack() {
        if (base == nullptr) {
            return;
        }
        stack_t off{};
        off.ss_flags = SS_DISABLE;
        ::sigaltstack(&off, nullptr);
        ::munmap(base, length);
    }
};
thread_local OwnStack own;

}  // namespace

void ensure_signal_stack() {
    stack_t current{};
    if (::sigaltstack(nullptr, &current) != 0 || (current.ss_flags & SS_DISABLE) == 0) {
        return;
    }
    const std::size_t guard = page_size();
    void* const base = ::mmap(nullptr, guard + stack_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (base == MAP_FAILED) {
        log(LogLevel::warn, "signal", "no alternate signal stack: {}", std::strerror(errno));
        return;
    }
    stack_t stack{};
    stack.ss_sp = static_cast<char*>(base) + guard;
    stack.ss_size = stack_size;
    if (::mprotect(base, guard, PROT_NONE) != 0 || ::sigaltstack(&stack, nullptr) != 0) {
        log(LogLevel::warn, "signal", "no alternate signal stack: {}", std::strerror(errno));
        ::munmap(base, guard + stack_size);
        return;
    }
    own.base = base;
    own.length = guard + stack_size;
}

}  // namespace bb::common
