// libkernel thread-specific data and once (docs/design/m3-core-os.md, "Threads"; values and call
// sites in docs/formats/kernel-abi.md, "Threads", "Thread-specific data and once"). libc.prx is
// the only caller. The core functions return a host errno, which the sce entry points translate.

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/thread.hpp"

namespace bb::hle::kernel {
namespace {

// ---- keys ------------------------------------------------------------------------------------

using Destructor = void (*)(void*);

// Our own limit; libc.prx creates one key (0x55792).
constexpr std::size_t max_keys = 256;
// How often the exit handler runs the destructors while they leave values behind.
constexpr int destructor_rounds = 4;

// A guest key is a 4-byte int (libc reads it with a 32-bit load at 0x54f94): the slot index plus
// one, so the zero of a key that was never created is refused.
using Key = int;

// A slot's generation is odd while its key exists. Create and delete each add one, so a value set
// under a deleted key never shows under the key that reuses its slot.
struct Keys {
    std::mutex mutex;
    std::array<std::atomic<std::uint32_t>, max_keys> generation{};
    std::array<Destructor, max_keys> destructor{};  // guarded by mutex
};

Keys& keys() {
    static Keys& k = *new Keys;  // leaked: game threads may still run during exit
    return k;
}

struct Value {
    std::uint32_t generation = 0;
    void* value = nullptr;
};

// The calling thread's values, by slot. Freed by glibc after the exit handler ran.
thread_local std::vector<Value> values;

// The slot of a key that exists, or -1.
int slot_of(Key key) {
    if (key < 1 || static_cast<std::size_t>(key) > max_keys) {
        return -1;
    }
    const int slot = key - 1;
    return keys().generation[slot].load(std::memory_order_acquire) % 2 == 1 ? slot : -1;
}

int key_create(Key* key, Destructor destructor) {
    if (key == nullptr) {
        return EINVAL;
    }
    Keys& k = keys();
    const std::lock_guard lock(k.mutex);
    for (std::size_t slot = 0; slot < max_keys; ++slot) {
        if (k.generation[slot].load(std::memory_order_relaxed) % 2 == 0) {
            k.destructor[slot] = destructor;
            k.generation[slot].fetch_add(1, std::memory_order_release);
            *key = static_cast<Key>(slot + 1);
            return 0;
        }
    }
    return EAGAIN;
}

// The values that threads still hold stay where they are and are never destroyed, as POSIX says.
int key_delete(Key key) {
    Keys& k = keys();
    const std::lock_guard lock(k.mutex);
    const int slot = slot_of(key);
    if (slot < 0) {
        return EINVAL;
    }
    k.destructor[slot] = nullptr;
    k.generation[slot].fetch_add(1, std::memory_order_release);
    return 0;
}

void* get_specific(Key key) {
    const int slot = slot_of(key);
    if (slot < 0 || static_cast<std::size_t>(slot) >= values.size()) {
        return nullptr;
    }
    const Value& v = values[slot];
    const std::uint32_t generation = keys().generation[slot].load(std::memory_order_acquire);
    return v.generation == generation ? v.value : nullptr;
}

int set_specific(Key key, const void* value) {
    const int slot = slot_of(key);
    if (slot < 0) {
        return EINVAL;
    }
    if (static_cast<std::size_t>(slot) >= values.size()) {
        values.resize(slot + 1);
    }
    values[slot] = {keys().generation[slot].load(std::memory_order_acquire),
                    const_cast<void*>(value)};
    return 0;
}

// Runs at the end of every thread the runtime started (register_thread_exit_handler): calls the
// destructor of each key that exists and has a value on this thread, with the value, after
// setting it to null. A destructor may set values again, so this repeats up to
// destructor_rounds times. A thread the runtime didn't start runs it only when it calls
// exit_thread, which runs the exit handlers before the host's pthread_exit.
void run_destructors() {
    Keys& k = keys();
    for (int round = 0; round < destructor_rounds; ++round) {
        bool called = false;
        for (std::size_t slot = 0; slot < values.size(); ++slot) {
            Destructor destructor = nullptr;
            void* value = nullptr;
            {
                const std::lock_guard lock(k.mutex);
                Value& v = values[slot];
                if (v.value == nullptr || v.generation != k.generation[slot].load() ||
                    k.destructor[slot] == nullptr) {
                    continue;
                }
                destructor = k.destructor[slot];
                value = std::exchange(v.value, nullptr);
            }
            destructor(value);  // may set values, which can grow the vector
            called = true;
        }
        if (!called) {
            return;
        }
    }
}

[[maybe_unused]] const bool destructors_registered =
    (register_thread_exit_handler(run_destructors), true);

// ---- once ------------------------------------------------------------------------------------

// The control is the guest's (libc passes static ones: 0xbef80, 0xbef98, 0xbf558, 0xcd398, zero
// at start). We use the int at its offset 0: 0 means the routine hasn't run, which libc's
// zero-filled controls fix. The other two values are ours.
constexpr int once_not_run = 0, once_done = 1, once_running = 2;

struct OnceWait {
    std::mutex mutex;
    std::condition_variable done;
};

OnceWait& once_wait() {
    static OnceWait& w = *new OnceWait;  // leaked: game threads may still run during exit
    return w;
}

// Runs `routine` once per control. Other threads that arrive while it runs wait for it to end.
// The routine runs without our lock held, so it may use other controls. A routine that ends its
// thread leaves the control running, and the threads waiting on it wait forever.
int once(int* control, void (*routine)()) {
    if (control == nullptr || routine == nullptr) {
        return EINVAL;
    }
    std::atomic_ref<int> state(*control);
    if (state.load(std::memory_order_acquire) == once_done) {
        return 0;
    }
    OnceWait& w = once_wait();
    std::unique_lock lock(w.mutex);
    for (;;) {
        const int value = state.load(std::memory_order_acquire);
        if (value == once_done) {
            return 0;
        }
        if (value == once_not_run) {
            break;
        }
        if (value != once_running) {
            return EINVAL;  // not a control
        }
        w.done.wait(lock);
    }
    state.store(once_running, std::memory_order_relaxed);
    lock.unlock();
    routine();
    lock.lock();
    state.store(once_done, std::memory_order_release);
    w.done.notify_all();
    return 0;
}

std::int32_t sce_key_create(Key* key, Destructor destructor) {
    return sce_fail(key_create(key, destructor));
}
std::int32_t sce_key_delete(Key key) {
    return sce_fail(key_delete(key));
}
std::int32_t sce_set_specific(Key key, const void* value) {
    return sce_fail(set_specific(key, value));
}
std::int32_t sce_once(int* control, void (*routine)()) {
    return sce_fail(once(control, routine));
}

}  // namespace
}  // namespace bb::hle::kernel

namespace k = bb::hle::kernel;

BB_HLE_FUNCTION("libkernel", "geDaqgH9lTg", k::sce_key_create);
BB_HLE_FUNCTION("libkernel", "PrdHuuDekhY", k::sce_key_delete);
BB_HLE_FUNCTION("libkernel", "eoht7mQOCmo", k::get_specific);
BB_HLE_FUNCTION("libkernel", "+BzXYkqYeLE", k::sce_set_specific);
BB_HLE_FUNCTION("libkernel", "14bOACANTBo", k::sce_once);
