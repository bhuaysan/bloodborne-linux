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

// Detach states (eboot 0x263b3b3 and libSceFios2 0x2ea7c pass 0 and join, the eboot passes 1 at
// 0xd1aad5 and 0x6654a and never joins).
inline constexpr int guest_create_joinable = 0;
inline constexpr int guest_create_detached = 1;

// What an attribute holds.
struct ThreadAttributes {
    std::size_t stack_size = default_thread_stack_size;
    int detach_state = guest_create_joinable;
    // Recorded and logged, not applied.
    int sched_policy = 0;
    int sched_priority = 0;
    int inherit_sched = 0;
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
    // A thread the runtime didn't start (the main thread, a std::thread): it can't be joined.
    bool adopted = false;
    // Guarded by the thread table's lock.
    bool detached = false, joining = false, finished = false;
};

// The calling thread's object. A thread the runtime didn't start gets one on first use, freed at
// its exit.
Thread* current_thread();

// Registers `handler` to run on every thread the runtime started, when its entry returns or it
// exits, in registration order and before glibc runs the thread's thread_local destructors
// (docs/design/m3-core-os.md, "TLS"). For the game's own exit hooks, e.g. the destructors of
// thread-specific data.
void register_thread_exit_handler(void (*handler)());

// The core functions behind the entry points. Each returns a host errno.
int create_thread(Thread** thread, const std::uintptr_t* attr, ThreadEntry entry, void* arg,
                  const char* name);
int join_thread(Thread* thread, void** value);
int detach_thread(Thread* thread);
// Ends the calling thread with `value` as its result. On a thread the runtime started it jumps
// back to the start routine without unwinding the frames in between.
[[noreturn]] void exit_thread(void* value);

}  // namespace bb::hle::kernel
