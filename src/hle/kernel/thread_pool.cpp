// The pool that Thread objects come from (thread.hpp, guest_handle_limit;
// docs/design/m3-core-os.md, "Threads"): one MAP_32BIT mapping, which Linux places below 2 GiB
// on x86-64, cut into equal slots with a free list. The mapping is MAP_NORESERVE and only the
// slots in use are touched, so its size costs nothing up front.

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

#include "hle/kernel/thread.hpp"

namespace bb::hle::kernel {
namespace {

constexpr std::size_t pool_size = std::size_t{4} << 20;
constexpr std::size_t slot_size =
    (sizeof(Thread) + alignof(std::max_align_t) - 1) / alignof(std::max_align_t) *
    alignof(std::max_align_t);

struct FreeSlot {
    FreeSlot* next;
};

struct Pool {
    std::mutex mutex;
    std::byte* base = nullptr;  // guarded by mutex, mapped on first use
    std::size_t used = 0;       // slots handed out at least once, guarded by mutex
    FreeSlot* free = nullptr;   // guarded by mutex
};

Pool& pool() {
    static Pool& p = *new Pool;  // leaked: game threads may still run during exit
    return p;
}

}  // namespace

void* Thread::operator new(std::size_t size) {
    if (size > slot_size) {
        throw std::bad_alloc();
    }
    Pool& p = pool();
    const std::lock_guard lock(p.mutex);
    if (p.free != nullptr) {
        FreeSlot* slot = p.free;
        p.free = slot->next;
        return slot;
    }
    if (p.base == nullptr) {
        void* base = mmap(nullptr, pool_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_32BIT, -1, 0);
        if (base == MAP_FAILED) {
            throw std::bad_alloc();
        }
        p.base = static_cast<std::byte*>(base);
    }
    if ((p.used + 1) * slot_size > pool_size) {
        throw std::bad_alloc();
    }
    return p.base + p.used++ * slot_size;
}

void Thread::operator delete(void* object) noexcept {
    if (object == nullptr) {
        return;
    }
    Pool& p = pool();
    const std::lock_guard lock(p.mutex);
    auto* slot = static_cast<FreeSlot*>(object);
    slot->next = p.free;
    p.free = slot;
}

}  // namespace bb::hle::kernel
