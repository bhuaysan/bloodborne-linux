// The counted sync object: five unnamed libkernel imports (docs/design/m3-core-os.md, "Sync";
// call sites and results in docs/formats/kernel-abi.md, "Counted sync object"). Names unknown
// (docs/design/m3-core-os.md, "Naming"), so the C++ names describe what the calls do.

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"

namespace bb::hle::kernel {
namespace {

// Wake orders that create accepts. Both wake waiters in the order of their requests here.
constexpr std::int32_t order_fifo = 1;
constexpr std::int32_t order_priority = 2;

// Longer names are refused with ENAMETOOLONG (our limit; the game's names are short).
constexpr std::size_t max_name_length = 31;

class CountedSync {
public:
    CountedSync(std::int32_t count, std::int32_t max) : count_(count), max_(max) {}

    // Takes `need` from the count, waiting behind earlier requests. Returns a host errno.
    int wait(std::int32_t need, std::optional<std::chrono::microseconds> timeout) {
        if (need < 1 || need > max_) {
            return EINVAL;
        }
        std::unique_lock lock(mutex_);
        if (queue_.empty() && count_ >= need) {
            count_ -= need;
            return 0;
        }
        if (timeout && timeout->count() == 0) {
            return ETIMEDOUT;
        }
        Waiter self{need};
        queue_.push_back(&self);
        const auto done = [&] { return self.granted || deleted_; };
        if (!timeout) {
            changed_.wait(lock, done);
        } else {
            changed_.wait_until(lock, std::chrono::steady_clock::now() + *timeout, done);
        }
        if (self.granted) {
            return 0;
        }
        std::erase(queue_, &self);
        if (deleted_) {
            return EACCES;
        }
        // The head may have gone, so the next requests may fit now.
        serve();
        return ETIMEDOUT;
    }

    // Like wait, but fails with EBUSY instead of waiting.
    int try_wait(std::int32_t need) {
        if (need < 1 || need > max_) {
            return EINVAL;
        }
        const std::lock_guard lock(mutex_);
        if (!queue_.empty() || count_ < need) {
            return EBUSY;
        }
        count_ -= need;
        return 0;
    }

    // Adds `n` to the count. A count above the maximum is refused and changes nothing.
    int post(std::int32_t n) {
        const std::lock_guard lock(mutex_);
        if (n < 1 || std::int64_t{count_} + n > max_) {
            return EINVAL;
        }
        count_ += n;
        serve();
        return 0;
    }

    void close() {
        {
            const std::lock_guard lock(mutex_);
            deleted_ = true;
        }
        changed_.notify_all();
    }

private:
    struct Waiter {
        std::int32_t need;
        bool granted = false;
    };

    // Grants requests from the head of the queue while the count covers them. Holds mutex_.
    void serve() {
        bool any = false;
        while (!queue_.empty() && queue_.front()->need <= count_) {
            count_ -= queue_.front()->need;
            queue_.front()->granted = true;
            queue_.pop_front();
            any = true;
        }
        if (any) {
            changed_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Waiter*> queue_;
    std::int32_t count_;
    const std::int32_t max_;
    bool deleted_ = false;
};

struct Registry {
    std::mutex mutex;
    std::uint64_t next_handle = 1;
    std::unordered_map<std::uint64_t, std::shared_ptr<CountedSync>> objects;
};

Registry& registry() {
    static Registry& r = *new Registry;  // leaked: game threads may call in during exit
    return r;
}

std::shared_ptr<CountedSync> find(std::uint64_t handle) {
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    const auto it = r.objects.find(handle);
    return it == r.objects.end() ? nullptr : it->second;
}

// `options` is null at every call site (eboot 0xd1ad44, 0x263acee) and is ignored.
std::int32_t counted_create(std::uint64_t* out, const char* name, std::int32_t order,
                            std::int32_t initial, std::int32_t max, const void* /*options*/) {
    if (out == nullptr || (order != order_fifo && order != order_priority) || max < 1 ||
        initial < 0 || initial > max) {
        return sce_fail(EINVAL);
    }
    if (name != nullptr && ::strnlen(name, max_name_length + 1) > max_name_length) {
        return sce_fail(ENAMETOOLONG);
    }
    auto object = std::make_shared<CountedSync>(initial, max);
    Registry& r = registry();
    const std::lock_guard lock(r.mutex);
    const std::uint64_t handle = r.next_handle++;
    r.objects.emplace(handle, std::move(object));
    std::memcpy(out, &handle, sizeof handle);
    return 0;
}

// Waiters wake with EACCES.
std::int32_t counted_destroy(std::uint64_t handle) {
    std::shared_ptr<CountedSync> object;
    {
        Registry& r = registry();
        const std::lock_guard lock(r.mutex);
        const auto it = r.objects.find(handle);
        if (it == r.objects.end()) {
            return sce_fail(ESRCH);
        }
        object = std::move(it->second);
        r.objects.erase(it);
    }
    object->close();
    return 0;
}

// `timeout` points to a relative time in microseconds, null to wait without one. The remaining
// time is not written back.
std::int32_t counted_wait(std::uint64_t handle, std::int32_t need, const std::uint32_t* timeout) {
    const auto object = find(handle);
    if (!object) {
        return sce_fail(ESRCH);
    }
    std::optional<std::chrono::microseconds> limit;
    if (timeout != nullptr) {
        std::uint32_t usec = 0;
        std::memcpy(&usec, timeout, sizeof usec);  // the guest pointer need not be aligned
        limit = std::chrono::microseconds(usec);
    }
    return sce_fail(object->wait(need, limit));
}

std::int32_t counted_try_wait(std::uint64_t handle, std::int32_t need) {
    const auto object = find(handle);
    return object ? sce_fail(object->try_wait(need)) : sce_fail(ESRCH);
}

std::int32_t counted_post(std::uint64_t handle, std::int32_t n) {
    const auto object = find(handle);
    return object ? sce_fail(object->post(n)) : sce_fail(ESRCH);
}

}  // namespace
}  // namespace bb::hle::kernel

namespace k = bb::hle::kernel;

// Names unknown (docs/design/m3-core-os.md, "Naming").
BB_HLE_FUNCTION("libkernel", "188x57JYp0g", k::counted_create);
BB_HLE_FUNCTION("libkernel", "R1Jvn8bSCW8", k::counted_destroy);
BB_HLE_FUNCTION("libkernel", "Zxa0VhQVTsk", k::counted_wait);
BB_HLE_FUNCTION("libkernel", "12wOHk8ywb0", k::counted_try_wait);
BB_HLE_FUNCTION("libkernel", "4czppHBiriw", k::counted_post);
