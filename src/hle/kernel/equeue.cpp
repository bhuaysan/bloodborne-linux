#include "hle/kernel/equeue.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>

#include "hle/handle_table.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"

namespace bb::hle {
namespace {

static_assert(sizeof(GuestEvent) == 32);
static_assert(offsetof(GuestEvent, kind) == 8 && offsetof(GuestEvent, detail) == 12);
static_assert(offsetof(GuestEvent, data) == 16 && offsetof(GuestEvent, user_data) == 24);

// The longest name kept, for logs. The game's names are short ("FlipEventQueue").
constexpr std::size_t max_name_length = 31;

HandleTable<EventQueue>& registry() {
    static auto& r = *new HandleTable<EventQueue>;  // leaked: game threads may call in during exit
    return r;
}

}  // namespace

std::uint64_t EventQueue::create(std::string_view name) {
    return registry().insert(
        std::make_shared<EventQueue>(name.substr(0, max_name_length)));
}

std::shared_ptr<EventQueue> EventQueue::find(std::uint64_t handle) {
    return registry().find(handle);
}

bool EventQueue::remove(std::uint64_t handle) {
    const std::shared_ptr<EventQueue> queue = registry().take(handle);
    if (!queue) {
        return false;
    }
    // Waiters hold their own reference, so the object outlives the registry entry.
    queue->close();
    return true;
}

bool EventQueue::trigger(std::uint64_t handle, const GuestEvent& event) {
    const auto queue = find(handle);
    if (!queue) {
        return false;
    }
    queue->trigger(event);
    return true;
}

void EventQueue::trigger(const GuestEvent& event) {
    {
        const std::lock_guard lock(mutex_);
        const auto same = std::ranges::find_if(pending_, [&](const GuestEvent& e) {
            return e.id == event.id && e.kind == event.kind;
        });
        if (same != pending_.end()) {
            *same = event;
        } else {
            pending_.push_back(event);
        }
    }
    ready_.notify_one();
}

void EventQueue::close() {
    {
        const std::lock_guard lock(mutex_);
        deleted_ = true;
    }
    ready_.notify_all();
}

EventQueue::WaitResult EventQueue::wait(std::span<GuestEvent> out,
                                        std::optional<std::chrono::microseconds> timeout) {
    std::unique_lock lock(mutex_);
    const auto done = [this] { return deleted_ || !pending_.empty(); };
    bool woken = true;
    ++waiting_;
    if (!timeout) {
        ready_.wait(lock, done);
    } else {
        const auto deadline = std::chrono::steady_clock::now() + *timeout;
        woken = ready_.wait_until(lock, deadline, done);
    }
    --waiting_;
    if (!woken) {
        return {0, WaitError::timed_out};
    }
    if (deleted_) {
        return {0, WaitError::deleted};
    }
    const std::size_t n = std::min(out.size(), pending_.size());
    std::copy_n(pending_.begin(), n, out.begin());
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
    // Events left over go to the next waiter.
    if (!pending_.empty()) {
        ready_.notify_one();
    }
    return {n, std::nullopt};
}

std::size_t EventQueue::waiting() {
    const std::lock_guard lock(mutex_);
    return waiting_;
}

namespace kernel {

std::int32_t equeue_create(std::uint64_t* out, const char* name) {
    if (out == nullptr) {
        return sce_fail(EINVAL);
    }
    const std::string_view n = name == nullptr
                                   ? std::string_view{}
                                   : std::string_view(name, ::strnlen(name, max_name_length));
    const std::uint64_t handle = EventQueue::create(n);
    std::memcpy(out, &handle, sizeof handle);
    return 0;
}

std::int32_t equeue_delete(std::uint64_t handle) {
    return EventQueue::remove(handle) ? 0 : sce_fail(EBADF);
}

std::int32_t equeue_wait(std::uint64_t handle, GuestEvent* events, std::int32_t capacity,
                         std::int32_t* count, const std::uint32_t* timeout) {
    if (events == nullptr || capacity < 1 || count == nullptr) {
        return sce_fail(EINVAL);
    }
    const auto queue = EventQueue::find(handle);
    if (!queue) {
        return sce_fail(EBADF);
    }
    std::optional<std::chrono::microseconds> limit;
    if (timeout != nullptr) {
        std::uint32_t us = 0;
        std::memcpy(&us, timeout, sizeof us);  // the guest pointer need not be aligned
        limit = std::chrono::microseconds(us);
    }
    // At most 64 records per call; more stay pending for the next one. The game asks for 1.
    GuestEvent buffer[64];
    const std::size_t room = std::min<std::size_t>(64, static_cast<std::size_t>(capacity));
    const auto result = queue->wait(std::span(buffer, room), limit);
    const auto n = static_cast<std::int32_t>(result.count);
    std::memcpy(events, buffer, result.count * sizeof(GuestEvent));
    std::memcpy(count, &n, sizeof n);
    if (result.error == EventQueue::WaitError::timed_out) {
        return sce_fail(ETIMEDOUT);
    }
    if (result.error == EventQueue::WaitError::deleted) {
        return sce_fail(ECANCELED);
    }
    return 0;
}

}  // namespace kernel

// Names unknown (docs/design/m3-core-os.md, "Naming"); behaviour in docs/formats/kernel-abi.md.
BB_HLE_FUNCTION("libkernel", "D0OdFMjp46I", kernel::equeue_create);
BB_HLE_FUNCTION("libkernel", "jpFjmgAC5AE", kernel::equeue_delete);
BB_HLE_FUNCTION("libkernel", "fzyMKs9kim0", kernel::equeue_wait);

}  // namespace bb::hle
