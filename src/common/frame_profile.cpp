#include "common/frame_profile.hpp"

#include <algorithm>
#include <ctime>
#include <format>
#include <mutex>
#include <utility>

#include "common/log.hpp"

namespace bb::common::frame_profile {
namespace {

using detail::parts;
using detail::ThreadCounters;

constexpr std::array<std::string_view, parts> names = {
    "commands", "draw",   "pipeline", "bind",         "buffer",        "sync", "resident", "texture", "render",
    "vertex",   "state",  "upload",   "detile",   "upload_new",    "fault",         "inval_buffer",
    "inval_texture", "protect", "copy", "irq", "queue", "submit", "wait",     "present", "flip_wait"};

// Counters every thread may add to: add() with a time in ns, and the ticks of threads that
// found no free ThreadCounters.
struct Shared {
    std::array<std::atomic<std::uint64_t>, parts> ns{};
    std::array<std::atomic<std::uint64_t>, parts> ticks{};
    std::array<std::atomic<std::uint64_t>, parts> calls{};
};

// A thread keeps its counters for good: a thread that exits leaves its counts to be collected,
// and a fault handler must not allocate or register a destructor. Threads past the pool add to
// Shared, slower and still counted.
constexpr std::size_t pool_size = 256;

struct Taken {
    std::array<std::uint64_t, parts> ticks{};
    std::array<std::uint64_t, parts> calls{};
};

struct State {
    std::array<ThreadCounters, pool_size> pool{};
    std::atomic<std::size_t> claimed{0};
    Shared shared;

    std::mutex take_mutex;
    // Guarded by take_mutex: what take() last read from each pool entry, the clock pair the tick
    // rate is measured from, and the time of the last frame line.
    std::array<Taken, pool_size> taken{};
    std::uint64_t anchor_ticks = 0;
    std::uint64_t anchor_ns = 0;
    std::uint64_t last_frame = 0;
};

// Leaked on purpose, as the logger: a fault handler may run while the process exits.
State& state() {
    static State& s = *new State;
    return s;
}

struct ClockPair {
    std::uint64_t ticks;
    std::uint64_t ns;
};

// The two clocks at one moment: retries when the thread was interrupted between the reads.
ClockPair read_clocks() noexcept {
    ClockPair pair{};
    for (int attempt = 0; attempt < 8; ++attempt) {
        pair.ticks = detail::ticks_now();
        pair.ns = now_ns();
        if (detail::ticks_now() - pair.ticks < 2000) {
            break;
        }
    }
    return pair;
}

// ns per tick, measured from the first call to now, and the clocks now. The first calls wait
// until that span is long enough to measure (1 ms: an error under 0.01 %).
double tick_rate(State& s, ClockPair& now) noexcept {
    constexpr std::uint64_t shortest_ns = 1'000'000;
    if (s.anchor_ticks == 0) {
        const ClockPair anchor = read_clocks();
        s.anchor_ticks = anchor.ticks;
        s.anchor_ns = anchor.ns;
    }
    do {
        now = read_clocks();
    } while (now.ns - s.anchor_ns < shortest_ns);
    const std::uint64_t ticks = now.ticks - s.anchor_ticks;
    if (ticks == 0 || ticks > (std::uint64_t{1} << 62)) {
        return 0.0;  // a counter that stands still or ran backwards: the times read 0
    }
    return static_cast<double>(now.ns - s.anchor_ns) / static_cast<double>(ticks);
}

Totals take_locked(State& s, ClockPair& now) noexcept {
    std::array<std::uint64_t, parts> ticks{};
    Totals totals;
    const std::size_t claimed = std::min(s.claimed.load(std::memory_order_acquire), pool_size);
    for (std::size_t t = 0; t < claimed; ++t) {
        const ThreadCounters& counters = s.pool[t];
        Taken& taken = s.taken[t];
        for (std::size_t i = 0; i < parts; ++i) {
            const std::uint64_t part_ticks = counters.ticks[i].load(std::memory_order_relaxed);
            const std::uint64_t part_calls = counters.calls[i].load(std::memory_order_relaxed);
            ticks[i] += part_ticks - taken.ticks[i];
            totals.calls[i] += part_calls - taken.calls[i];
            taken.ticks[i] = part_ticks;
            taken.calls[i] = part_calls;
        }
    }
    const double rate = tick_rate(s, now);
    for (std::size_t i = 0; i < parts; ++i) {
        ticks[i] += s.shared.ticks[i].exchange(0, std::memory_order_relaxed);
        totals.ns[i] = static_cast<std::uint64_t>(static_cast<double>(ticks[i]) * rate) +
                       s.shared.ns[i].exchange(0, std::memory_order_relaxed);
        totals.calls[i] += s.shared.calls[i].exchange(0, std::memory_order_relaxed);
    }
    return totals;
}

}  // namespace

namespace detail {

void add_ticks_slow(Part part, std::uint64_t ticks) noexcept {
    State& s = state();
    const auto i = static_cast<std::size_t>(part);
    // fetch_add may run past the pool: take() caps what it reads.
    if (s.claimed.load(std::memory_order_relaxed) < pool_size) {
        const std::size_t slot = s.claimed.fetch_add(1, std::memory_order_acq_rel);
        if (slot < pool_size) {
            mine = &s.pool[slot];
            add_ticks(part, ticks);
            return;
        }
    }
    s.shared.ticks[i].fetch_add(ticks, std::memory_order_relaxed);
    s.shared.calls[i].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace detail

std::string_view part_name(Part part) {
    return names.at(static_cast<std::size_t>(part));
}

void set_enabled(bool on) noexcept {
    (void)state();  // made here, so the first Scope of a fault handler does not allocate it
    detail::on.store(on, std::memory_order_relaxed);
}

std::uint64_t now_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000u +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

void add(Part part, std::uint64_t ns) noexcept {
    Shared& shared = state().shared;
    const auto i = static_cast<std::size_t>(part);
    shared.ns[i].fetch_add(ns, std::memory_order_relaxed);
    shared.calls[i].fetch_add(1, std::memory_order_relaxed);
}

Totals take() noexcept {
    State& s = state();
    const std::lock_guard lock(s.take_mutex);
    ClockPair now{};
    return take_locked(s, now);
}

std::string format_frame(const Totals& totals, std::uint64_t at_ns, std::uint64_t interval_ns) {
    std::string line = std::format("Frame profile: at {} ns, {} us since the last", at_ns,
                                   interval_ns / 1000);
    for (std::size_t i = 0; i < parts; ++i) {
        if (totals.calls[i] != 0) {
            line += std::format(", {}={} us/{}", names[i], totals.ns[i] / 1000, totals.calls[i]);
        }
    }
    return line;
}

void log_frame() {
    if (!enabled()) {
        return;
    }
    State& s = state();
    Totals totals;
    ClockPair now{};
    std::uint64_t last = 0;
    {
        const std::lock_guard lock(s.take_mutex);
        totals = take_locked(s, now);
        last = std::exchange(s.last_frame, now.ns);
    }
    log_message(LogLevel::debug, "profile",
                format_frame(totals, now.ns, last ? now.ns - last : 0));
}

}  // namespace bb::common::frame_profile
