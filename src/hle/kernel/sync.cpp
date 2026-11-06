// libkernel and libScePosix mutexes, mutex attributes, rwlocks and condition variables on glibc
// primitives (docs/design/m3-core-os.md, "Sync"; values in docs/formats/kernel-abi.md, "Mutexes
// and rwlocks" and "Condition variables"). The core functions return a host errno; the sce and
// POSIX entry points translate it into their own error form.

#include "hle/kernel/sync.hpp"

#include <pthread.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <list>
#include <mutex>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"

namespace bb::hle::kernel {
namespace {

using Cell = std::uintptr_t;

// Cell values below this are no pointers to our objects, so they are refused without reading.
constexpr Cell lowest_object = 0x10000;

// Each object type has a tag and the host primitive, and makes the object for a cell that holds
// the static initialiser with create_default.
struct Mutex {
    static constexpr std::uint64_t tag_value = 0x786574756d5f6262;  // "bb_mutex"
    std::uint64_t tag = tag_value;
    pthread_mutex_t host;

    static int create_default(Cell* cell);
};

struct Rwlock {
    static constexpr std::uint64_t tag_value = 0x6b636f6c77725f62;  // "b_rwlock"
    std::uint64_t tag = tag_value;
    pthread_rwlock_t host;

    static int create_default(Cell* cell);
};

// A thread in a condition variable wait. It sleeps on its own host condition variable until a
// signal or broadcast sets `woken`, or its deadline passes.
struct CondWaiter {
    pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
    bool woken = false;
};

// The waiters are kept by hand rather than in a glibc condition variable, so that destroy knows
// which ones are still blocked. `blocked` holds them in arrival order; signal and broadcast move
// them out and count them in `leaving` until they have left our bookkeeping. Destroy refuses with
// EBUSY only while a waiter is blocked, and otherwise waits for the leaving ones on `drained`:
// they leave before they take the guest mutex back, so the destroying thread may hold it. All
// fields are guarded by `state`.
struct Cond {
    static constexpr std::uint64_t tag_value = 0x646e6f635f5f6262;  // "bb__cond"
    std::uint64_t tag = tag_value;
    pthread_mutex_t state = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t drained = PTHREAD_COND_INITIALIZER;
    std::list<CondWaiter*> blocked;
    int leaving = 0;

    static int create_default(Cell* cell);
};

// Serialises the creation of objects for cells that hold the static initialiser.
constinit std::mutex creation_lock;

std::atomic_ref<Cell> atomic(Cell* cell) {
    return std::atomic_ref<Cell>(*cell);
}

// The object of ours that `value` points to, or nullptr.
template <class T>
T* object_at(Cell value) {
    if (value < lowest_object || value % alignof(T) != 0) {
        return nullptr;
    }
    T* object = reinterpret_cast<T*>(value);
    return object->tag == T::tag_value ? object : nullptr;
}

// Stores a new mutex in `cell`. Returns a host errno.
int create_mutex(Cell* cell, int host_mutex_type) {
    auto* m = new Mutex;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, host_mutex_type);
    const int error = pthread_mutex_init(&m->host, &attr);
    pthread_mutexattr_destroy(&attr);
    if (error != 0) {
        delete m;
        return error;
    }
    atomic(cell).store(reinterpret_cast<Cell>(m), std::memory_order_release);
    return 0;
}

// Stores a new rwlock in `cell`. Returns a host errno.
int create_rwlock(Cell* cell) {
    auto* r = new Rwlock;
    const int error = pthread_rwlock_init(&r->host, nullptr);
    if (error != 0) {
        delete r;
        return error;
    }
    atomic(cell).store(reinterpret_cast<Cell>(r), std::memory_order_release);
    return 0;
}

int Rwlock::create_default(Cell* cell) {
    return create_rwlock(cell);
}

// The absolute CLOCK_MONOTONIC time `usec` microseconds from now.
timespec deadline_after(std::uint32_t usec) {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += usec / 1000000;
    t.tv_nsec += static_cast<long>(usec % 1000000) * 1000;
    if (t.tv_nsec >= 1000000000) {
        t.tv_sec += 1;
        t.tv_nsec -= 1000000000;
    }
    return t;
}

// The object of `cell`, created with the default attributes if the cell holds the static
// initialiser. Sets `*error` and returns nullptr on failure.
template <class T>
T* lookup(Cell* cell, int* error) {
    *error = EINVAL;
    if (cell == nullptr) {
        return nullptr;
    }
    Cell value = atomic(cell).load(std::memory_order_acquire);
    if (value == 0) {
        const std::lock_guard lock(creation_lock);
        value = atomic(cell).load(std::memory_order_acquire);
        if (value == 0) {
            *error = T::create_default(cell);
            if (*error != 0) {
                return nullptr;
            }
            value = atomic(cell).load(std::memory_order_relaxed);
        }
    }
    T* object = object_at<T>(value);
    *error = object != nullptr ? 0 : EINVAL;
    return object;
}

// Frees the object of `cell` and resets the cell to the static initialiser. A cell that holds the
// static initialiser has nothing to free. `host_destroy` takes the object.
template <class T, class Destroy>
int destroy(Cell* cell, Destroy host_destroy) {
    if (cell == nullptr) {
        return EINVAL;
    }
    const Cell value = atomic(cell).load(std::memory_order_acquire);
    if (value == 0) {
        return 0;
    }
    T* object = object_at<T>(value);
    if (object == nullptr) {
        return EINVAL;
    }
    if (const int error = host_destroy(object); error != 0) {
        return error;
    }
    object->tag = 0;
    delete object;
    atomic(cell).store(0, std::memory_order_release);
    return 0;
}

// ---- mutex attributes ----------------------------------------------------------------------
// The guest attribute is one 8-byte cell. It holds the values themselves, tagged in the upper
// half, so it needs no object of ours: type in bits 0-7, protocol in bits 8-15.

constexpr std::uint64_t attr_tag = 0x4d41545400000000;  // "MATT" in the upper half

std::uint64_t pack_attr(int type, int protocol) {
    return attr_tag | static_cast<std::uint64_t>(protocol) << 8 | static_cast<std::uint64_t>(type);
}

bool valid_attr(const std::uint64_t* attr) {
    return attr != nullptr && (*attr & 0xffffffff00000000) == attr_tag;
}

int attr_type(std::uint64_t attr) {
    return static_cast<int>(attr & 0xff);
}

int attr_protocol(std::uint64_t attr) {
    return static_cast<int>(attr >> 8 & 0xff);
}

int mutexattr_init(std::uint64_t* attr) {
    if (attr == nullptr) {
        return EINVAL;
    }
    *attr = pack_attr(guest_mutex_errorcheck, guest_prio_none);
    return 0;
}

int mutexattr_destroy(std::uint64_t* attr) {
    if (!valid_attr(attr)) {
        return EINVAL;
    }
    *attr = 0;
    return 0;
}

int mutexattr_settype(std::uint64_t* attr, int type) {
    if (!valid_attr(attr) || type < guest_mutex_errorcheck || type > guest_mutex_adaptive) {
        return EINVAL;
    }
    *attr = pack_attr(type, attr_protocol(*attr));
    return 0;
}

// Accepted and not applied (docs/formats/kernel-abi.md, "Protocol").
int mutexattr_setprotocol(std::uint64_t* attr, int protocol) {
    if (!valid_attr(attr) || protocol < guest_prio_none || protocol > guest_prio_protect) {
        return EINVAL;
    }
    *attr = pack_attr(attr_type(*attr), protocol);
    return 0;
}

// ---- mutexes -------------------------------------------------------------------------------

int Mutex::create_default(Cell* cell) {
    return create_mutex(cell, PTHREAD_MUTEX_ERRORCHECK);
}

// The adaptive type spins before it sleeps on the guest. It is error-checking here, which is
// what libSceFios2, its only user, needs (0x2e52b).
int host_mutex_type(int guest_type) {
    switch (guest_type) {
    case guest_mutex_recursive:
        return PTHREAD_MUTEX_RECURSIVE;
    case guest_mutex_normal:
        return PTHREAD_MUTEX_NORMAL;
    default:
        return PTHREAD_MUTEX_ERRORCHECK;
    }
}

int mutex_init(Cell* mutex, const std::uint64_t* attr) {
    if (mutex == nullptr || (attr != nullptr && !valid_attr(attr))) {
        return EINVAL;
    }
    const int type = attr != nullptr ? attr_type(*attr) : guest_mutex_errorcheck;
    return create_mutex(mutex, host_mutex_type(type));
}

int mutex_destroy(Cell* mutex) {
    return destroy<Mutex>(mutex, [](Mutex* m) { return pthread_mutex_destroy(&m->host); });
}

int mutex_lock(Cell* mutex) {
    int error = 0;
    Mutex* m = lookup<Mutex>(mutex, &error);
    return m != nullptr ? pthread_mutex_lock(&m->host) : error;
}

int mutex_trylock(Cell* mutex) {
    int error = 0;
    Mutex* m = lookup<Mutex>(mutex, &error);
    return m != nullptr ? pthread_mutex_trylock(&m->host) : error;
}

// `usec` is relative (docs/formats/kernel-abi.md, "Timeouts").
int mutex_timedlock(Cell* mutex, std::uint32_t usec) {
    int error = 0;
    Mutex* m = lookup<Mutex>(mutex, &error);
    if (m == nullptr) {
        return error;
    }
    const timespec deadline = deadline_after(usec);
    return pthread_mutex_clocklock(&m->host, CLOCK_MONOTONIC, &deadline);
}

int mutex_unlock(Cell* mutex) {
    int error = 0;
    Mutex* m = lookup<Mutex>(mutex, &error);
    return m != nullptr ? pthread_mutex_unlock(&m->host) : error;
}

// ---- rwlocks -------------------------------------------------------------------------------

// No rwlock attribute function is imported, so `attr` can only be null (libSceFios2 0x2e72b).
int rwlock_init(Cell* rwlock, const void* /*attr*/) {
    return rwlock == nullptr ? EINVAL : create_rwlock(rwlock);
}

// glibc destroys a held rwlock without complaint, and the next unlock then hangs. The guest gets
// EBUSY instead, as for a mutex.
int destroy_host_rwlock(Rwlock* r) {
    if (pthread_rwlock_trywrlock(&r->host) != 0) {
        return EBUSY;
    }
    pthread_rwlock_unlock(&r->host);
    return pthread_rwlock_destroy(&r->host);
}

int rwlock_destroy(Cell* rwlock) {
    return destroy<Rwlock>(rwlock, destroy_host_rwlock);
}

template <int (*Op)(pthread_rwlock_t*)>
int rwlock_op(Cell* rwlock) {
    int error = 0;
    Rwlock* r = lookup<Rwlock>(rwlock, &error);
    return r != nullptr ? Op(&r->host) : error;
}

template <int (*Op)(pthread_rwlock_t*, clockid_t, const timespec*)>
int rwlock_timed(Cell* rwlock, std::uint32_t usec) {
    int error = 0;
    Rwlock* r = lookup<Rwlock>(rwlock, &error);
    if (r == nullptr) {
        return error;
    }
    const timespec deadline = deadline_after(usec);
    return Op(&r->host, CLOCK_MONOTONIC, &deadline);
}

// ---- condition variables -------------------------------------------------------------------
// The guest object is one 8-byte cell, like a mutex (docs/formats/kernel-abi.md, "Condition
// variables"). A waiter releases the host mutex behind the guest mutex while it sleeps.

int create_cond(Cell* cell) {
    atomic(cell).store(reinterpret_cast<Cell>(new Cond), std::memory_order_release);
    return 0;
}

int Cond::create_default(Cell* cell) {
    return create_cond(cell);
}

// No condition variable attribute function is imported, and every caller passes null (libc
// 0x64bf, libSceFios2 0x2e0df, eboot 0x2083d64), so `attr` is ignored.
int cond_init(Cell* cond, const void* /*attr*/) {
    return cond == nullptr ? EINVAL : create_cond(cond);
}

// Busy only while a waiter is blocked. Waiters that a signal or broadcast has released are
// waited for, as glibc's pthread_cond_destroy does.
int destroy_host_cond(Cond* c) {
    pthread_mutex_lock(&c->state);
    if (!c->blocked.empty()) {
        pthread_mutex_unlock(&c->state);
        return EBUSY;
    }
    while (c->leaving != 0) {
        pthread_cond_wait(&c->drained, &c->state);
    }
    pthread_mutex_unlock(&c->state);
    pthread_cond_destroy(&c->drained);
    pthread_mutex_destroy(&c->state);
    return 0;
}

int cond_destroy(Cell* cond) {
    return destroy<Cond>(cond, destroy_host_cond);
}

// Moves the first blocked waiter to the leaving ones and wakes it. Called with `state` held.
void release_first(Cond* c) {
    CondWaiter* w = c->blocked.front();
    c->blocked.pop_front();
    w->woken = true;
    ++c->leaving;
    pthread_cond_signal(&w->wake);
}

// Releases the guest mutex, sleeps until woken or until `sleep(wake, state)` times out, and takes
// the guest mutex back. `sleep` returns 0 or ETIMEDOUT, as pthread_cond_clockwait does.
template <class Sleep>
int cond_wait_with(Cell* cond, Cell* mutex, Sleep sleep) {
    int error = 0;
    Cond* c = lookup<Cond>(cond, &error);
    if (c == nullptr) {
        return error;
    }
    pthread_mutex_t* m = host_mutex(mutex, &error);
    if (m == nullptr) {
        return error;
    }
    CondWaiter self;
    pthread_mutex_lock(&c->state);
    // A signaller that holds the guest mutex takes `state` after us, so it sees this waiter.
    // An error-checking guest mutex refuses the unlock with EPERM if this thread doesn't hold it.
    if (error = pthread_mutex_unlock(m); error != 0) {
        pthread_mutex_unlock(&c->state);
        return error;
    }
    const auto position = c->blocked.insert(c->blocked.end(), &self);
    while (!self.woken) {
        if (sleep(&self.wake, &c->state) == ETIMEDOUT && !self.woken) {
            c->blocked.erase(position);
            error = ETIMEDOUT;
            break;
        }
    }
    if (self.woken && --c->leaving == 0) {
        pthread_cond_broadcast(&c->drained);
    }
    pthread_mutex_unlock(&c->state);
    pthread_cond_destroy(&self.wake);
    pthread_mutex_lock(m);
    return error;
}

int cond_wait(Cell* cond, Cell* mutex) {
    return cond_wait_with(cond, mutex, pthread_cond_wait);
}

// The sce timeout is relative, in microseconds (libc 0x6e50 and libSceFios2 0x2e1e4 convert their
// deadlines to it, docs/formats/kernel-abi.md, "Condition variables").
int cond_timedwait_usec(Cell* cond, Cell* mutex, std::uint32_t usec) {
    const timespec deadline = deadline_after(usec);
    return cond_wait_with(cond, mutex, [&](pthread_cond_t* c, pthread_mutex_t* m) {
        return pthread_cond_clockwait(c, m, CLOCK_MONOTONIC, &deadline);
    });
}

// The POSIX timeout is an absolute time on the wall clock (the eboot adds it to gettimeofday,
// 0x65c91).
int cond_timedwait_abs(Cell* cond, Cell* mutex, const GuestTimespec* abstime) {
    if (abstime == nullptr || abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000) {
        return EINVAL;
    }
    const timespec deadline = host_timespec(*abstime);
    return cond_wait_with(cond, mutex, [&](pthread_cond_t* c, pthread_mutex_t* m) {
        return pthread_cond_clockwait(c, m, CLOCK_REALTIME, &deadline);
    });
}

int cond_signal(Cell* cond) {
    int error = 0;
    Cond* c = lookup<Cond>(cond, &error);
    if (c == nullptr) {
        return error;
    }
    pthread_mutex_lock(&c->state);
    if (!c->blocked.empty()) {
        release_first(c);
    }
    pthread_mutex_unlock(&c->state);
    return 0;
}

int cond_broadcast(Cell* cond) {
    int error = 0;
    Cond* c = lookup<Cond>(cond, &error);
    if (c == nullptr) {
        return error;
    }
    pthread_mutex_lock(&c->state);
    while (!c->blocked.empty()) {
        release_first(c);
    }
    pthread_mutex_unlock(&c->state);
    return 0;
}

// ---- entry points --------------------------------------------------------------------------

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

// The name is not kept (libc.prx passes one, 0x64e7; the eboot passes null, 0xd1ac41).
std::int32_t sce_mutex_init(Cell* mutex, const std::uint64_t* attr, const char* /*name*/) {
    return sce_fail(mutex_init(mutex, attr));
}

// The name is not kept (the eboot passes null, 0x207f2d1).
std::int32_t sce_rwlock_init(Cell* rwlock, const void* attr, const char* /*name*/) {
    return sce_fail(rwlock_init(rwlock, attr));
}

// The name is not kept (libc.prx passes a formatted buffer, 0x5f3e; libSceFios2 a constant or
// null, 0x2e0e1 and 0x2e0fa).
std::int32_t sce_cond_init(Cell* cond, const void* attr, const char* /*name*/) {
    return sce_fail(cond_init(cond, attr));
}

}  // namespace

pthread_mutex_t* host_mutex(Cell* mutex, int* host_errno) {
    Mutex* m = lookup<Mutex>(mutex, host_errno);
    return m != nullptr ? &m->host : nullptr;
}

}  // namespace bb::hle::kernel

namespace k = bb::hle::kernel;

BB_HLE_FUNCTION("libkernel", "F8bUHwAG284", k::Sce<k::mutexattr_init>::call);
BB_HLE_FUNCTION("libkernel", "smWEktiyyG0", k::Sce<k::mutexattr_destroy>::call);
BB_HLE_FUNCTION("libkernel", "iMp8QpE+XO4", k::Sce<k::mutexattr_settype>::call);
BB_HLE_FUNCTION("libkernel", "1FGvU0i9saQ", k::Sce<k::mutexattr_setprotocol>::call);
BB_HLE_FUNCTION("libkernel", "cmo1RIYva9o", k::sce_mutex_init);
BB_HLE_FUNCTION("libkernel", "2Of0f+3mhhE", k::Sce<k::mutex_destroy>::call);
BB_HLE_FUNCTION("libkernel", "9UK1vLZQft4", k::Sce<k::mutex_lock>::call);
BB_HLE_FUNCTION("libkernel", "upoVrzMHFeE", k::Sce<k::mutex_trylock>::call);
BB_HLE_FUNCTION("libkernel", "IafI2PxcPnQ", k::Sce<k::mutex_timedlock>::call);
BB_HLE_FUNCTION("libkernel", "tn3VlD0hG60", k::Sce<k::mutex_unlock>::call);

BB_HLE_FUNCTION("libScePosix", "dQHWEsJtoE4", k::Posix<k::mutexattr_init>::call);
BB_HLE_FUNCTION("libScePosix", "HF7lK46xzjY", k::Posix<k::mutexattr_destroy>::call);
BB_HLE_FUNCTION("libScePosix", "mDmgMOGVUqg", k::Posix<k::mutexattr_settype>::call);
BB_HLE_FUNCTION("libScePosix", "ttHNfU+qDBU", k::Posix<k::mutex_init>::call);
BB_HLE_FUNCTION("libScePosix", "ltCfaGr2JGE", k::Posix<k::mutex_destroy>::call);
BB_HLE_FUNCTION("libScePosix", "7H0iTOciTLo", k::Posix<k::mutex_lock>::call);
BB_HLE_FUNCTION("libScePosix", "K-jXhbt2gn4", k::Posix<k::mutex_trylock>::call);
BB_HLE_FUNCTION("libScePosix", "2Z+PpY6CaJg", k::Posix<k::mutex_unlock>::call);

BB_HLE_FUNCTION("libkernel", "6ULAa0fq4jA", k::sce_rwlock_init);
BB_HLE_FUNCTION("libkernel", "BB+kb08Tl9A", k::Sce<k::rwlock_destroy>::call);
BB_HLE_FUNCTION("libkernel", "Ox9i0c7L5w0", k::Sce<k::rwlock_op<pthread_rwlock_rdlock>>::call);
BB_HLE_FUNCTION("libkernel", "mqdNorrB+gI", k::Sce<k::rwlock_op<pthread_rwlock_wrlock>>::call);
BB_HLE_FUNCTION("libkernel", "XD3mDeybCnk", k::Sce<k::rwlock_op<pthread_rwlock_tryrdlock>>::call);
BB_HLE_FUNCTION("libkernel", "bIHoZCTomsI", k::Sce<k::rwlock_op<pthread_rwlock_trywrlock>>::call);
BB_HLE_FUNCTION("libkernel", "+L98PIbGttk", k::Sce<k::rwlock_op<pthread_rwlock_unlock>>::call);
BB_HLE_FUNCTION("libkernel", "iPtZRWICjrM",
                k::Sce<k::rwlock_timed<pthread_rwlock_clockrdlock>>::call);
BB_HLE_FUNCTION("libkernel", "adh--6nIqTk",
                k::Sce<k::rwlock_timed<pthread_rwlock_clockwrlock>>::call);

BB_HLE_FUNCTION("libkernel", "2Tb92quprl0", k::sce_cond_init);
BB_HLE_FUNCTION("libkernel", "g+PZd2hiacg", k::Sce<k::cond_destroy>::call);
BB_HLE_FUNCTION("libkernel", "WKAXJ4XBPQ4", k::Sce<k::cond_wait>::call);
BB_HLE_FUNCTION("libkernel", "BmMjYxmew1w", k::Sce<k::cond_timedwait_usec>::call);
BB_HLE_FUNCTION("libkernel", "kDh-NfxgMtE", k::Sce<k::cond_signal>::call);
BB_HLE_FUNCTION("libkernel", "JGgj7Uvrl+A", k::Sce<k::cond_broadcast>::call);

BB_HLE_FUNCTION("libScePosix", "0TyVk4MSLt0", k::Posix<k::cond_init>::call);
BB_HLE_FUNCTION("libScePosix", "RXXqi4CtF8w", k::Posix<k::cond_destroy>::call);
BB_HLE_FUNCTION("libScePosix", "Op8TBGY5KHg", k::Posix<k::cond_wait>::call);
BB_HLE_FUNCTION("libScePosix", "27bAgiJmOh0", k::Posix<k::cond_timedwait_abs>::call);
BB_HLE_FUNCTION("libScePosix", "2MOy+rUfuhQ", k::Posix<k::cond_signal>::call);
BB_HLE_FUNCTION("libScePosix", "mkx2fVhNMsg", k::Posix<k::cond_broadcast>::call);
