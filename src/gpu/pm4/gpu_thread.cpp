#include "gpu/pm4/gpu_thread.hpp"

#include <algorithm>

#include "common/log.hpp"
#include "gpu/pm4/default_state.hpp"

namespace bb::pm4 {

namespace {

constexpr std::string_view channel = "gpu.pm4";

// The first streams are logged at info, the rest at debug. Failed streams always at warn.
constexpr std::uint64_t logged_streams = 8;

constexpr auto poll_interval = std::chrono::microseconds(100);

}  // namespace

GpuThread::GpuThread(Parser::Readable readable) {
    regs_.set_context_defaults(context_defaults());
    regs_.clear_context();
    parser_.set_readable(std::move(readable));
    parser_.set_trace([this](const Packet& packet) {
        packets_.fetch_add(1, std::memory_order_relaxed);
        common::log(common::LogLevel::trace, channel, "packet {:#04x}, {} dwords",
                    packet.opcode(), packet.body.size());
    });
    thread_ = std::thread([this] { loop(); });
}

GpuThread::~GpuThread() {
    {
        const std::scoped_lock lock(mutex_);
        stop_ = true;
    }
    work_.notify_all();
    thread_.join();
}

void GpuThread::submit(Queue queue, std::span<const std::span<const std::uint32_t>> buffers) {
    {
        const std::scoped_lock lock(mutex_);
        for (const auto buffer : buffers) {
            queues_[static_cast<std::size_t>(queue)].emplace_back(buffer);
        }
        stats_.submitted += buffers.size();
    }
    work_.notify_all();
}

bool GpuThread::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return idle_.wait_for(lock, timeout, [this] {
        return std::ranges::all_of(queues_, [](const auto& queue) { return queue.empty(); });
    });
}

GpuThread::Stats GpuThread::stats() const {
    const std::scoped_lock lock(mutex_);
    Stats stats = stats_;
    stats.packets = packets_.load(std::memory_order_relaxed);
    return stats;
}

void GpuThread::loop() {
    common::set_thread_name("gpu");
    for (;;) {
        bool progress = false;
        bool waiting = false;
        for (auto& queue : queues_) {
            progress = run_front(queue, waiting) || progress;
        }
        std::unique_lock lock(mutex_);
        if (stop_) {
            return;
        }
        if (progress) {
            continue;
        }
        if (waiting) {
            work_.wait_for(lock, poll_interval);
        } else {
            work_.wait(lock, [this] {
                return stop_ ||
                       std::ranges::any_of(queues_, [](const auto& q) { return !q.empty(); });
            });
        }
    }
}

bool GpuThread::run_front(std::deque<Stream>& queue, bool& waiting) {
    Stream* stream = nullptr;
    {
        const std::scoped_lock lock(mutex_);
        if (stop_ || queue.empty()) {
            return false;
        }
        stream = &queue.front();
    }
    const std::uint64_t packets = packets_.load(std::memory_order_relaxed);
    const RunResult result = parser_.run(*stream);
    if (result == RunResult::waiting) {
        waiting = true;
        return packets_.load(std::memory_order_relaxed) != packets;
    }
    {
        const std::scoped_lock lock(mutex_);
        queue.pop_front();
        ++stats_.finished;
        if (result == RunResult::error) {
            ++stats_.errors;
            common::log(common::LogLevel::warn, channel, "stream {} failed and was dropped",
                        stats_.finished);
        } else {
            common::log(stats_.finished <= logged_streams ? common::LogLevel::info
                                                          : common::LogLevel::debug,
                        channel, "stream {} done, {} packets so far", stats_.finished,
                        packets_.load(std::memory_order_relaxed));
        }
    }
    idle_.notify_all();
    return true;
}

}  // namespace bb::pm4
