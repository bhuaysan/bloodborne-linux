// Event queues: the game creates, waits on and deletes them; our own HLE modules (video out,
// later audio) trigger events on them (docs/design/m3-core-os.md, "Sync"). The record layout
// and the results are in docs/formats/kernel-abi.md, "Event queue". The libkernel names of the
// three entry points are in symbols/nids.csv (sceKernelCreateEqueue, Delete, Wait).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bb::hle {

// One event as the guest receives it, 32 bytes. No caller in the game reads a field, so the
// layout is ours. `id` and `kind` identify the event source (the producer chooses both),
// `user_data` is the value the guest gave when it asked for the event.
struct GuestEvent {
    std::uint64_t id = 0;
    std::int16_t kind = 0;
    std::uint16_t flags = 0;
    std::uint32_t detail = 0;
    std::int64_t data = 0;
    std::uint64_t user_data = 0;
    bool operator==(const GuestEvent&) const = default;
};

class EventQueue {
public:
    explicit EventQueue(std::string_view name) : name_(name) {}
    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    // Creates a queue and returns its guest handle, never 0.
    static std::uint64_t create(std::string_view name);

    // The queue with guest handle `handle`, or nullptr if there is none (or it was deleted).
    static std::shared_ptr<EventQueue> find(std::uint64_t handle);

    // Deletes the queue with guest handle `handle` and wakes its waiters, which return the
    // delete result. False if there is no such queue.
    static bool remove(std::uint64_t handle);

    // The API for other HLE modules. Adds `event` to the queue with guest handle `handle` and
    // wakes a waiter. Returns false, and does nothing, if there is no such queue: a producer
    // that holds a handle the game has since deleted just drops its events.
    //
    // Events are returned in the order they were triggered. A trigger whose (id, kind) pair is
    // already pending replaces that record in place, so a producer that fires faster than the
    // game waits does not grow the queue. Safe to call from any thread.
    static bool trigger(std::uint64_t handle, const GuestEvent& event);
    void trigger(const GuestEvent& event);

    // What wait() reports besides a number of events.
    enum class WaitError { timed_out, deleted };

    // Moves up to out.size() pending events into `out`, oldest first, and returns how many.
    // Blocks until at least one is pending, the timeout has passed (std::nullopt waits forever)
    // or the queue is deleted.
    struct WaitResult {
        std::size_t count = 0;
        std::optional<WaitError> error;
    };
    WaitResult wait(std::span<GuestEvent> out, std::optional<std::chrono::microseconds> timeout);

    const std::string& name() const { return name_; }

    // The number of threads blocked in wait() right now.
    std::size_t waiting();

private:
    void close();

    const std::string name_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<GuestEvent> pending_;
    std::size_t waiting_ = 0;
    bool deleted_ = false;
};

namespace kernel {

// The three libkernel entry points, registered by NID.

// D0OdFMjp46I: creates a queue and writes its handle to `*out`. `name` may be null.
std::int32_t equeue_create(std::uint64_t* out, const char* name);

// jpFjmgAC5AE: deletes a queue.
std::int32_t equeue_delete(std::uint64_t handle);

// fzyMKs9kim0: waits for events. `events` has room for `capacity` records, `*count` receives
// the number written. `timeout` points to microseconds, null waits forever.
std::int32_t equeue_wait(std::uint64_t handle, GuestEvent* events, std::int32_t capacity,
                         std::int32_t* count, const std::uint32_t* timeout);

}  // namespace kernel
}  // namespace bb::hle
