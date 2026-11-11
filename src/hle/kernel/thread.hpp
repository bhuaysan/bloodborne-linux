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

// The CPU affinity mask of a new attribute and of a thread the runtime didn't start: the six cores
// the eboot ever puts in a mask (it ands every mask with 0x3f, 0x207f9a4, 0x263b463). libc counts
// its bits as the number of CPUs (0x81ae2).
inline constexpr std::uint64_t default_affinity_mask = 0x3f;

// What an attribute holds.
struct ThreadAttributes {
    std::size_t stack_size = default_thread_stack_size;
    int detach_state = guest_create_joinable;
    // Recorded and logged, not applied.
    int sched_policy = 0;
    int sched_priority = 0;
    int inherit_sched = 0;
    std::uint64_t affinity_mask = default_affinity_mask;
};

using ThreadEntry = void* (*)(void*);

// Every handle the guest sees must fit in 40 bits: the eboot's mutex (0x20368c0) keeps its
// owner's handle masked with 0xffffffffff and compares that with the full handle, so a thread whose
// handle is wider never sees that it took the lock. Thread objects therefore come from a pool
// below 2 GiB (thread_pool.cpp), which also keeps them below the sanitizers' shadow memory.
inline constexpr std::uintptr_t guest_handle_limit = std::uintptr_t{1} << 40;

struct Thread {
    // From the pool; throws std::bad_alloc when it is full.
    static void* operator new(std::size_t size);
    static void operator delete(void* object) noexcept;

    pthread_t host{};
    // Guarded by the thread table's lock: a rename from another thread changes them. The priority
    // and affinity calls change attributes.sched_priority and .affinity_mask.
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
// create_thread with the attributes given directly, for the runtime's own guest threads (the
// workers of the system libraries, src/shadps4_host/core/libraries/kernel/threads.h).
int create_thread_with(Thread** thread, const ThreadAttributes& attributes, ThreadEntry entry,
                       void* arg, const char* name);
int join_thread(Thread* thread, void** value);
int detach_thread(Thread* thread);
// Ends the calling thread with `value` as its result. On a thread the runtime started it jumps
// back to the start routine without unwinding the frames in between.
[[noreturn]] void exit_thread(void* value);

}  // namespace bb::hle::kernel
