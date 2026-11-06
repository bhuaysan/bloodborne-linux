// libkernel and libScePosix threads and thread attributes, on glibc threads
// (docs/design/m3-core-os.md, "Threads"; values and call sites in docs/formats/kernel-abi.md,
// "Threads"). The core functions return a host errno, which the sce entry points translate.

#include "hle/kernel/thread.hpp"

#include <pthread.h>
#include <sched.h>

#include <cerrno>
#include <csetjmp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

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
    std::vector<void (*)()> exit_handlers;  // guarded by mutex
};

Table& table() {
    static Table& t = *new Table;  // leaked: game threads may still run during exit
    return t;
}

thread_local Thread* current = nullptr;

// Frees the object of a thread the runtime didn't start when that thread exits.
struct AdoptedThread {
    Thread* thread = nullptr;
    ~AdoptedThread() {
        if (thread != nullptr) {
            const std::lock_guard lock(table().mutex);
            table().threads.erase(thread);
            delete thread;
            current = nullptr;
        }
    }
};
thread_local AdoptedThread adopted;

void run_exit_handlers() {
    std::vector<void (*)()> handlers;
    {
        const std::lock_guard lock(table().mutex);
        handlers = table().exit_handlers;
    }
    for (void (*handler)() : handlers) {
        handler();
    }
}

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
    {
        // Under the lock, so that a rename from another thread comes either before or after.
        const std::lock_guard lock(table().mutex);
        if (!thread->name.empty()) {
            common::set_thread_name(thread->name);
        }
    }
    run_entry(thread);
    run_exit_handlers();
    current = nullptr;
    const std::lock_guard lock(table().mutex);
    if (thread->detached) {
        table().threads.erase(thread);
        delete thread;
    } else {
        thread->finished = true;  // join_thread or detach_thread frees it
    }
    return nullptr;
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

int attr_setdetachstate(Cell* cell, int state) {
    Attr* attr = attr_at(cell);
    if (attr == nullptr || (state != guest_create_joinable && state != guest_create_detached)) {
        return EINVAL;
    }
    attr->values.detach_state = state;
    return 0;
}

// The parameter is one int, the priority (FreeBSD struct sched_param).
int attr_setschedparam(Cell* cell, const int* param) {
    Attr* attr = attr_at(cell);
    if (attr == nullptr || param == nullptr) {
        return EINVAL;
    }
    attr->values.sched_priority = *param;
    return 0;
}

int attr_setschedpolicy(Cell* cell, int policy) {
    Attr* attr = attr_at(cell);
    if (attr == nullptr) {
        return EINVAL;
    }
    attr->values.sched_policy = policy;
    return 0;
}

int attr_setinheritsched(Cell* cell, int inherit) {
    Attr* attr = attr_at(cell);
    if (attr == nullptr) {
        return EINVAL;
    }
    attr->values.inherit_sched = inherit;
    return 0;
}

int attr_setaffinity(Cell* cell, std::uint64_t mask) {
    Attr* attr = attr_at(cell);
    if (attr == nullptr) {
        return EINVAL;
    }
    attr->values.affinity_mask = mask;
    return 0;
}

int attr_getaffinity(Cell* cell, std::uint64_t* mask) {
    const Attr* attr = attr_at(cell);
    if (attr == nullptr || mask == nullptr) {
        return EINVAL;
    }
    *mask = attr->values.affinity_mask;
    return 0;
}

// Fills an initialised attribute with the thread's attributes and its current detach state.
int attr_get(Thread* thread, Cell* cell) {
    Attr* attr = attr_at(cell);
    if (attr == nullptr) {
        return EINVAL;
    }
    const std::lock_guard lock(table().mutex);
    if (!table().threads.contains(thread)) {
        return ESRCH;
    }
    attr->values = thread->attributes;
    attr->values.detach_state = thread->detached ? guest_create_detached : guest_create_joinable;
    return 0;
}

// ---- names, priority and affinity ------------------------------------------------------------

// The name goes to the log and the host. The log keeps a name per thread that only the thread
// itself can set, so a thread renamed by another one keeps its old name in its own log lines.
int rename_thread(Thread* thread, const char* name) {
    if (name == nullptr) {
        return EINVAL;
    }
    std::string old;
    {
        const std::lock_guard lock(table().mutex);
        if (!table().threads.contains(thread)) {
            return ESRCH;
        }
        old = std::exchange(thread->name, name);
        if (thread != current && !thread->joining && !thread->finished) {
            // A finished or joined host thread has no name left to set.
            const std::string host(std::string_view(name).substr(0, 15));
            pthread_setname_np(thread->host, host.c_str());
        }
    }
    if (thread == current) {
        common::set_thread_name(name);
    }
    common::log(common::LogLevel::info, channel, "thread \"{}\" renamed to \"{}\"", old, name);
    return 0;
}

// Runs `change` on the attributes of a live thread under the table's lock.
template <class F>
int with_attributes(Thread* thread, F change) {
    const std::lock_guard lock(table().mutex);
    if (!table().threads.contains(thread)) {
        return ESRCH;
    }
    change(thread->attributes);
    return 0;
}

int set_affinity(Thread* thread, std::uint64_t mask) {
    const int error = with_attributes(thread, [&](ThreadAttributes& a) { a.affinity_mask = mask; });
    if (error == 0) {
        common::log(common::LogLevel::debug, channel, "thread affinity {:#x} (not applied)", mask);
    }
    return error;
}

int get_affinity(Thread* thread, std::uint64_t* mask) {
    if (mask == nullptr) {
        return EINVAL;
    }
    return with_attributes(thread, [&](const ThreadAttributes& a) { *mask = a.affinity_mask; });
}

// W0Hpm2X0uPE (name unknown): (thread, priority). The eboot passes the attribute's priority
// right after a create (0xd1ab46) and values from 0x100 to 0x2ff at 0x263b63d; Fios's wrapper
// 0x2ec20 passes its arguments through. A failure at 0xd1ab46 fails the eboot's thread
// creation, so any priority is taken.
int set_priority(Thread* thread, int priority) {
    const int error =
        with_attributes(thread, [&](ThreadAttributes& a) { a.sched_priority = priority; });
    if (error == 0) {
        common::log(common::LogLevel::debug, channel, "thread priority {} (not applied)", priority);
    }
    return error;
}

// 1tKyG7RlMJo (name unknown): (thread, int* priority). The eboot reads the priority before it
// changes it (0x263b59d); Fios's wrappers return it for a thread and for Self (0x2ec5b, 0x2eca3).
int get_priority(Thread* thread, int* priority) {
    if (priority == nullptr) {
        return EINVAL;
    }
    return with_attributes(thread,
                           [&](const ThreadAttributes& a) { *priority = a.sched_priority; });
}

// ---- entry points ----------------------------------------------------------------------------

int equal(Thread* a, Thread* b) {
    return a == b ? 1 : 0;
}

void yield() {
    sched_yield();
}

int posix_sched_yield() {
    return sched_yield() == 0 ? 0 : posix_fail(errno);
}

// Sce<f>::call and Posix<f>::call wrap a core function `f` in the sce or POSIX error form.
template <auto F>
struct Sce;
template <class... A, int (*F)(A...)>
struct Sce<F> {
    static std::int32_t call(A... args) { return sce_fail(F(args...)); }
};

template <auto F>
struct Posix;
template <class... A, int (*F)(A...)>
struct Posix<F> {
    static int call(A... args) { return pthread_error(F(args...)); }
};

int posix_create(Thread** thread, const Cell* attr, ThreadEntry entry, void* arg) {
    return pthread_error(create_thread(thread, attr, entry, arg, nullptr));
}

}  // namespace

Thread* current_thread() {
    if (current == nullptr) {
        auto* thread = new Thread;
        thread->host = pthread_self();
        thread->adopted = true;
        {
            const std::lock_guard lock(table().mutex);
            table().threads.insert(thread);
        }
        adopted.thread = thread;
        current = thread;
    }
    return current;
}

void register_thread_exit_handler(void (*handler)()) {
    const std::lock_guard lock(table().mutex);
    table().exit_handlers.push_back(handler);
}

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
    t->detached = t->attributes.detach_state == guest_create_detached;
    const ThreadAttributes& v = t->attributes;
    common::log(common::LogLevel::debug, channel,
                "thread \"{}\": stack {:#x}, {}, policy {}, priority {}, inherit {}, affinity "
                "{:#x} (not applied)",
                t->name, v.stack_size, t->detached ? "detached" : "joinable", v.sched_policy,
                v.sched_priority, v.inherit_sched, v.affinity_mask);

    // The setter takes any size from the minimum up, so a garbage size near SIZE_MAX would wrap
    // around to a tiny host stack here. No host can give a stack that large anyway.
    if (t->attributes.stack_size > SIZE_MAX - thread_stack_reserve) {
        return EAGAIN;
    }
    pthread_attr_t host;
    pthread_attr_init(&host);
    int error = pthread_attr_setstacksize(&host, t->attributes.stack_size + thread_stack_reserve);
    if (error != 0) {
        pthread_attr_destroy(&host);
        return error;
    }
    pthread_attr_setdetachstate(&host,
                                t->detached ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);

    // The handle is published before the thread starts, as glibc stores its pthread_t: the entry
    // may read its handle variable, or pass its own handle on to be joined or detached, at once.
    // The table's lock is held until the host handle is stored: join and detach read it under the
    // lock, and a detached thread that ends at once frees its object under it.
    Thread* const previous = *thread;
    const std::lock_guard lock(table().mutex);
    table().threads.insert(t.get());
    *thread = t.get();
    pthread_t host_thread{};
    error = pthread_create(&host_thread, &host, start, t.get());
    pthread_attr_destroy(&host);
    if (error != 0) {
        *thread = previous;
        table().threads.erase(t.get());
        // glibc adds its guard page and refuses a size that then wraps around with EINVAL.
        return error == EINVAL ? EAGAIN : error;
    }
    t->host = host_thread;
    t.release();  // join_thread, detach_thread or the end of a detached thread frees it
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
        if (thread->adopted || thread->detached || thread->joining) {
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

int detach_thread(Thread* thread) {
    const std::lock_guard lock(table().mutex);
    if (!table().threads.contains(thread)) {
        return ESRCH;
    }
    if (thread->adopted || thread->detached || thread->joining) {
        return EINVAL;
    }
    pthread_detach(thread->host);
    if (thread->finished) {
        table().threads.erase(thread);
        delete thread;
    } else {
        thread->detached = true;  // the thread frees it when it ends
    }
    return 0;
}

void exit_thread(void* value) {
    if (current != nullptr && !current->adopted) {
        current->result = value;
        std::longjmp(current->exit_point, 1);
    }
    common::log(common::LogLevel::warn, channel,
                "thread exit on a thread the runtime didn't start: host pthread_exit");
    run_exit_handlers();
    pthread_exit(value);
}

}  // namespace bb::hle::kernel

namespace k = bb::hle::kernel;

BB_HLE_FUNCTION("libkernel", "6UgtwV+0zb4", k::Sce<k::create_thread>::call);
BB_HLE_FUNCTION("libkernel", "onNY9Byn-W8", k::Sce<k::join_thread>::call);
BB_HLE_FUNCTION("libkernel", "4qGrR6eoP9Y", k::Sce<k::detach_thread>::call);
BB_HLE_FUNCTION("libkernel", "3kg7rT0NQIs", k::exit_thread);
BB_HLE_FUNCTION("libkernel", "aI+OeCz8xrQ", k::current_thread);
BB_HLE_FUNCTION("libkernel", "3PtV6p3QNX4", k::equal);
BB_HLE_FUNCTION("libkernel", "T72hz6ffq08", k::yield);
BB_HLE_FUNCTION("libkernel", "6XG4B33N09g", k::posix_sched_yield);
BB_HLE_FUNCTION("libkernel", "nsYoNRywwNg", k::Sce<k::attr_init>::call);
BB_HLE_FUNCTION("libkernel", "62KCwEMmzcM", k::Sce<k::attr_destroy>::call);
BB_HLE_FUNCTION("libkernel", "UTXzJbWhhTE", k::Sce<k::attr_setstacksize>::call);
BB_HLE_FUNCTION("libkernel", "-Wreprtu0Qs", k::Sce<k::attr_setdetachstate>::call);
BB_HLE_FUNCTION("libkernel", "DzES9hQF4f4", k::Sce<k::attr_setschedparam>::call);
BB_HLE_FUNCTION("libkernel", "4+h9EzwKF4I", k::Sce<k::attr_setschedpolicy>::call);
BB_HLE_FUNCTION("libkernel", "eXbUSpEaTsA", k::Sce<k::attr_setinheritsched>::call);
BB_HLE_FUNCTION("libkernel", "x1X76arYMxU", k::Sce<k::attr_get>::call);
BB_HLE_FUNCTION("libkernel", "3qxgM4ezETA", k::Sce<k::attr_setaffinity>::call);
BB_HLE_FUNCTION("libkernel", "8+s5BzZjxSg", k::Sce<k::attr_getaffinity>::call);
BB_HLE_FUNCTION("libkernel", "GBUY7ywdULE", k::Sce<k::rename_thread>::call);
BB_HLE_FUNCTION("libkernel", "bt3CTBKmGyI", k::Sce<k::set_affinity>::call);
BB_HLE_FUNCTION("libkernel", "rcrVFJsQWRY", k::Sce<k::get_affinity>::call);
BB_HLE_FUNCTION("libkernel", "W0Hpm2X0uPE", k::Sce<k::set_priority>::call);
BB_HLE_FUNCTION("libkernel", "1tKyG7RlMJo", k::Sce<k::get_priority>::call);

// libScePosix: the same with POSIX error numbers. Jmi+9w9u0E4 is pthread_create with a name as
// fifth argument (docs/formats/kernel-imports.md).
BB_HLE_FUNCTION("libScePosix", "OxhIB8LB-PQ", k::posix_create);
BB_HLE_FUNCTION("libScePosix", "Jmi+9w9u0E4", k::Posix<k::create_thread>::call);
BB_HLE_FUNCTION("libScePosix", "h9CcP3J0oVM", k::Posix<k::join_thread>::call);
BB_HLE_FUNCTION("libScePosix", "FJrT5LuUBAU", k::exit_thread);
BB_HLE_FUNCTION("libScePosix", "EotR8a3ASf4", k::current_thread);
BB_HLE_FUNCTION("libScePosix", "wtkt-teR1so", k::Posix<k::attr_init>::call);
BB_HLE_FUNCTION("libScePosix", "zHchY8ft5pk", k::Posix<k::attr_destroy>::call);
BB_HLE_FUNCTION("libScePosix", "2Q0z6rnBrTE", k::Posix<k::attr_setstacksize>::call);
BB_HLE_FUNCTION("libScePosix", "E+tyo3lp5Lw", k::Posix<k::attr_setdetachstate>::call);
BB_HLE_FUNCTION("libScePosix", "euKRgm0Vn2M", k::Posix<k::attr_setschedparam>::call);
