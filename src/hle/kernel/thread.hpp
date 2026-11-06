// Threads (docs/design/m3-core-os.md, "Threads"; values in docs/formats/kernel-abi.md,
// "Threads"). A thread handle of the scePthread functions and a libScePosix pthread_t are both a
// pointer to our Thread object. A guest thread attribute is one 8-byte cell that holds a pointer to
// our attribute object. The entry points are registered by NID in thread.cpp; this header serves
// the other thread modules (thread-specific data, names, priority).
#pragma once

#include <pthread.h>

#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bb::hle::kernel {

// The stack size of a new attribute, the eboot's own default (its wrapper at 0x263b310).
inline constexpr std::size_t default_thread_stack_size = 0x10000;
// The smallest stack size an attribute takes (the eboot's fallback, 0x263b387).
inline constexpr std::size_t min_thread_stack_size = 0x4000;
// Added to the requested size for the host stack: HLE calls, the logger and the sanitizers run on
// the game's thread stacks too.
inline constexpr std::size_t thread_stack_reserve = 0x10000;

// What an attribute holds.
struct ThreadAttributes {
    std::size_t stack_size = default_thread_stack_size;
};

using ThreadEntry = void* (*)(void*);

struct Thread {
    pthread_t host{};
    std::string name;
    ThreadAttributes attributes;
    ThreadEntry entry = nullptr;
    void* arg = nullptr;
    void* result = nullptr;
    std::jmp_buf exit_point;  // where an exit jumps back to in the start routine
    bool joining = false;  // guarded by the thread table's lock
};

// The core functions behind the entry points. Each returns a host errno.
int create_thread(Thread** thread, const std::uintptr_t* attr, ThreadEntry entry, void* arg,
                  const char* name);
int join_thread(Thread* thread, void** value);
// Ends the calling thread with `value` as its result. On a thread the runtime started it jumps
// back to the start routine without unwinding the frames in between.
[[noreturn]] void exit_thread(void* value);

}  // namespace bb::hle::kernel
