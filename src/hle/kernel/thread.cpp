// libkernel thread creation, join and exit, and thread attributes, on glibc threads
// (docs/design/m3-core-os.md, "Threads"; values and call sites in docs/formats/kernel-abi.md,
// "Threads"). The core functions return a host errno, which the sce entry points translate.

#include "hle/kernel/thread.hpp"

#include <pthread.h>

#include <cerrno>
#include <csetjmp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_set>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/tls.hpp"

namespace bb::hle::kernel {
namespace {

using Cell = std::uintptr_t;

constexpr std::string_view channel = "hle.libkernel";

// ---- the thread table ------------------------------------------------------------------------

// Every Thread object that is alive. A handle counts as a thread only while it is in the table, so
// a stale or made-up handle is refused without reading through it.
struct Table {
    std::mutex mutex;
    std::unordered_set<Thread*> threads;  // guarded by mutex
};

Table& table() {
    static Table& t = *new Table;  // leaked: game threads may still run during exit
    return t;
}

thread_local Thread* current = nullptr;

// Runs the entry. exit_thread jumps back to the setjmp here; no frame in between has anything to
// destroy (game code, the exit entry points).
[[gnu::noinline]] void run_entry(Thread* thread) {
    if (setjmp(thread->exit_point) == 0) {
        thread->result = thread->entry(thread->arg);
    }
}

void* start(void* argument) {
    auto* thread = static_cast<Thread*>(argument);
    current = thread;
    check_eboot_tls();
    if (!thread->name.empty()) {
        common::set_thread_name(thread->name);
    }
    run_entry(thread);
    current = nullptr;
    return nullptr;  // join_thread frees the object
}

// ---- attributes ------------------------------------------------------------------------------

// The object behind a guest attribute cell.
struct Attr {
    static constexpr std::uint64_t tag_value = 0x727474615f646874;  // "thd_attr"
    std::uint64_t tag = tag_value;
    ThreadAttributes values;
};

// Cell values below this are no pointers to our objects, so they are refused without reading.
constexpr Cell lowest_object = 0x10000;

// The attribute object of `cell`, or nullptr.
Attr* attr_at(const Cell* cell) {
    if (cell == nullptr || *cell < lowest_object || *cell % alignof(Attr) != 0) {
        return nullptr;
    }
    auto* attr = reinterpret_cast<Attr*>(*cell);
    return attr->tag == Attr::tag_value ? attr : nullptr;
}

int attr_init(Cell* cell) {
    if (cell == nullptr) {
        return EINVAL;
    }
    *cell = reinterpret_cast<Cell>(new Attr);
    return 0;
}

int attr_destroy(Cell* cell) {
    Attr* attr = attr_at(cell);
    if (attr == nullptr) {
        return EINVAL;
    }
    attr->tag = 0;
    delete attr;
    *cell = 0;
    return 0;
}

int attr_setstacksize(Cell* cell, std::size_t size) {
    Attr* attr = attr_at(cell);
    if (attr == nullptr || size < min_thread_stack_size) {
        return EINVAL;
    }
    attr->values.stack_size = size;
    return 0;
}

// ---- entry points ----------------------------------------------------------------------------

// Sce<f>::call wraps a core function `f` in the sce error form.
template <auto F>
struct Sce;
template <class... A, int (*F)(A...)>
struct Sce<F> {
    static std::int32_t call(A... args) { return sce_fail(F(args...)); }
};

}  // namespace

int create_thread(Thread** thread, const Cell* attr, ThreadEntry entry, void* arg,
                  const char* name) {
    if (thread == nullptr || entry == nullptr) {
        return EINVAL;
    }
    auto t = std::make_unique<Thread>();
    if (attr != nullptr) {
        const Attr* a = attr_at(attr);
        if (a == nullptr) {
            return EINVAL;
        }
        t->attributes = a->values;
    }
    t->name = name != nullptr ? name : "";
    t->entry = entry;
    t->arg = arg;
    common::log(common::LogLevel::debug, channel, "thread \"{}\": stack {:#x}", t->name,
                t->attributes.stack_size);

    // The setter takes any size from the minimum up, so a garbage size near SIZE_MAX would wrap
    // around to a tiny host stack here. No host can give a stack that large anyway.
    if (t->attributes.stack_size > SIZE_MAX - thread_stack_reserve) {
        return EAGAIN;
    }
    pthread_attr_t host;
    pthread_attr_init(&host);
    int error = pthread_attr_setstacksize(&host, t->attributes.stack_size + thread_stack_reserve);
    if (error == 0) {
        error = pthread_create(&t->host, &host, start, t.get());
    }
    pthread_attr_destroy(&host);
    if (error != 0) {
        return error;
    }
    const std::lock_guard lock(table().mutex);
    table().threads.insert(t.get());
    *thread = t.release();
    return 0;
}

int join_thread(Thread* thread, void** value) {
    {
        const std::lock_guard lock(table().mutex);
        if (!table().threads.contains(thread)) {
            return ESRCH;
        }
        if (thread == current) {
            return EDEADLK;
        }
        if (thread->joining) {
            return EINVAL;
        }
        thread->joining = true;
    }
    if (const int error = pthread_join(thread->host, nullptr); error != 0) {
        return error;
    }
    void* result = thread->result;
    {
        const std::lock_guard lock(table().mutex);
        table().threads.erase(thread);
    }
    delete thread;
    if (value != nullptr) {
        *value = result;
    }
    return 0;
}

void exit_thread(void* value) {
    if (current != nullptr) {
        current->result = value;
        std::longjmp(current->exit_point, 1);
    }
    common::log(common::LogLevel::warn, channel,
                "thread exit on a thread the runtime didn't start: host pthread_exit");
    pthread_exit(value);
}

}  // namespace bb::hle::kernel

namespace k = bb::hle::kernel;

BB_HLE_FUNCTION("libkernel", "6UgtwV+0zb4", k::Sce<k::create_thread>::call);
BB_HLE_FUNCTION("libkernel", "onNY9Byn-W8", k::Sce<k::join_thread>::call);
BB_HLE_FUNCTION("libkernel", "3kg7rT0NQIs", k::exit_thread);
BB_HLE_FUNCTION("libkernel", "nsYoNRywwNg", k::Sce<k::attr_init>::call);
BB_HLE_FUNCTION("libkernel", "62KCwEMmzcM", k::Sce<k::attr_destroy>::call);
BB_HLE_FUNCTION("libkernel", "UTXzJbWhhTE", k::Sce<k::attr_setstacksize>::call);
