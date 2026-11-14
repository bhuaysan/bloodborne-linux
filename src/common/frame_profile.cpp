#include "common/frame_profile.hpp"

#include <ctime>
#include <format>

#include "common/log.hpp"

namespace bb::common::frame_profile {
namespace {

constexpr std::size_t parts = static_cast<std::size_t>(Part::count);
constexpr std::array<std::string_view, parts> names = {
    "commands", "draw",   "pipeline", "bind",         "buffer",        "sync", "resident", "texture", "render",
    "vertex",   "state",  "upload",   "detile",       "fault",         "inval_buffer",
    "inval_texture", "protect", "copy", "irq", "queue", "submit", "wait",     "present", "flip_wait"};

struct Counters {
    std::atomic<bool> on{false};
    std::array<std::atomic<std::uint64_t>, parts> ns{};
    std::array<std::atomic<std::uint64_t>, parts> calls{};
    std::atomic<std::uint64_t> last_frame{0};
};

// Leaked on purpose, as the logger: a fault handler may run while the process exits.
Counters& counters() {
    static Counters& c = *new Counters;
    return c;
}

}  // namespace

std::string_view part_name(Part part) {
    return names.at(static_cast<std::size_t>(part));
}

bool enabled() noexcept {
    return counters().on.load(std::memory_order_relaxed);
}

void set_enabled(bool on) noexcept {
    counters().on.store(on, std::memory_order_relaxed);
}

std::uint64_t now_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000u +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

void add(Part part, std::uint64_t ns) noexcept {
    Counters& c = counters();
    const auto i = static_cast<std::size_t>(part);
    c.ns[i].fetch_add(ns, std::memory_order_relaxed);
    c.calls[i].fetch_add(1, std::memory_order_relaxed);
}

Totals take() noexcept {
    Counters& c = counters();
    Totals totals;
    for (std::size_t i = 0; i < parts; ++i) {
        totals.ns[i] = c.ns[i].exchange(0, std::memory_order_relaxed);
        totals.calls[i] = c.calls[i].exchange(0, std::memory_order_relaxed);
    }
    return totals;
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
    const std::uint64_t now = now_ns();
    const std::uint64_t last = counters().last_frame.exchange(now, std::memory_order_relaxed);
    const Totals totals = take();
    log_message(LogLevel::debug, "profile", format_frame(totals, now, last ? now - last : 0));
}

}  // namespace bb::common::frame_profile
