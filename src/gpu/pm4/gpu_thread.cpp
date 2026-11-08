#include "gpu/pm4/gpu_thread.hpp"

#include <algorithm>
#include <atomic>
#include <bit>

#include "common/log.hpp"
#include "gpu/pm4/default_state.hpp"

namespace bb::pm4 {

namespace {

constexpr std::string_view channel = "gpu.pm4";

// The first streams are logged at info, the rest at debug. Failed streams always at warn.
constexpr std::uint64_t logged_streams = 8;

constexpr auto poll_interval = std::chrono::microseconds(100);

}  // namespace

GpuThread::GpuThread(Parser::Readable readable, Parser::Interrupt interrupt, GpuTrace* trace)
    : trace_(trace) {
    regs_.set_context_defaults(context_defaults());
    regs_.clear_context();
    parser_.set_readable(std::move(readable));
    parser_.set_interrupt(std::move(interrupt));
    parser_.set_trace([this](const Packet& packet) {
        packets_.fetch_add(1, std::memory_order_relaxed);
        common::log(common::LogLevel::trace, channel, "packet {:#04x}, {} dwords",
                    packet.opcode(), packet.body.size());
        if (trace_ != nullptr) {
            trace_->packet(packet, regs_);
        }
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

void GpuThread::submit(Queue queue, std::span<const std::span<const std::uint32_t>> buffers,
                       bool ends_frame) {
    {
        const std::scoped_lock lock(mutex_);
        for (const auto buffer : buffers) {
            queues_[static_cast<std::size_t>(queue)].emplace_back(
                buffer, queue == Queue::constant ? Engine::constant : Engine::draw);
        }
        if (ends_frame && !buffers.empty()) {
            queues_[static_cast<std::size_t>(queue)].back().set_ends_frame();
        }
        stats_.submitted += buffers.size();
    }
    work_.notify_all();
}

int GpuThread::map_ring(std::span<const std::uint32_t> ring, std::uint32_t* read_pointer,
                        std::uint32_t interrupt_source) {
    if (!std::has_single_bit(ring.size()) || read_pointer == nullptr) {
        return -1;
    }
    const std::scoped_lock lock(mutex_);
    for (std::size_t i = 0; i < ring_count; ++i) {
        // A ring whose streams an unmap left queued is free once the thread dropped them.
        if (rings_[i].mapped || !queues_[queue_count + i].empty()) {
            continue;
        }
        rings_[i] = {ring, read_pointer, 0, interrupt_source, true};
        std::atomic_ref(*read_pointer).store(0, std::memory_order_release);
        common::log(common::LogLevel::info, channel, "compute ring {}: {} dwords at {}", i,
                    ring.size(), static_cast<const void*>(ring.data()));
        return static_cast<int>(i);
    }
    return -1;
}

bool GpuThread::unmap_ring(int ring) {
    {
        std::unique_lock lock(mutex_);
        if (ring < 0 || static_cast<std::size_t>(ring) >= ring_count || !rings_[ring].mapped) {
            return false;
        }
        rings_[ring].mapped = false;
        common::log(common::LogLevel::info, channel, "compute ring {} unmapped", ring);
        work_.notify_all();  // the thread drops the ring's streams
        // The guest may free the ring once this returns, so a run reading it must have returned.
        // Streams yield at waits, so this is short.
        idle_.wait(lock, [&] { return running_ring_ != ring; });
    }
    return true;
}

bool GpuThread::ring_doorbell(int ring, std::uint32_t write_offset) {
    {
        const std::scoped_lock lock(mutex_);
        if (ring < 0 || static_cast<std::size_t>(ring) >= ring_count || !rings_[ring].mapped ||
            write_offset > rings_[ring].dwords.size()) {
            return false;
        }
        Ring& r = rings_[ring];
        const auto size = static_cast<std::uint32_t>(r.dwords.size());
        write_offset &= size - 1;
        std::deque<Stream>& queue = queues_[queue_count + ring];
        const auto queue_range = [&](std::uint32_t from, std::uint32_t to) {
            queue.emplace_back(r.dwords.subspan(from, to - from), Engine::compute,
                               r.interrupt_source);
            ++stats_.submitted;
        };
        if (write_offset > r.written) {
            queue_range(r.written, write_offset);
        } else if (write_offset < r.written) {  // wrapped
            queue_range(r.written, size);
            if (write_offset != 0) {
                queue_range(0, write_offset);
            }
        }
        r.written = write_offset;
    }
    work_.notify_all();
    return true;
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
        for (std::size_t i = 0; i < queues_.size(); ++i) {
            progress = run_front(i, waiting) || progress;
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

void GpuThread::drop_streams(std::deque<Stream>& queue) {
    common::log(common::LogLevel::info, channel, "{} stream(s) of an unmapped ring dropped",
                queue.size());
    stats_.finished += queue.size();
    queue.clear();
}

bool GpuThread::run_front(std::size_t index, bool& waiting) {
    std::deque<Stream>& queue = queues_[index];
    Ring* const ring = index >= queue_count ? &rings_[index - queue_count] : nullptr;
    Stream* stream = nullptr;
    {
        const std::scoped_lock lock(mutex_);
        if (stop_ || queue.empty()) {
            return false;
        }
        if (ring != nullptr && !ring->mapped) {
            drop_streams(queue);
            idle_.notify_all();
            return true;
        }
        stream = &queue.front();
        running_ring_ = ring != nullptr ? static_cast<int>(index - queue_count) : -1;
    }
    const std::uint64_t packets = packets_.load(std::memory_order_relaxed);
    if (trace_ != nullptr) {
        trace_->set_source(ring != nullptr ? static_cast<int>(index - queue_count) : -1);
    }
    const RunResult result = parser_.run(*stream);
    // Outside the lock, because the trace writes to disk and guest threads submit under it. Before
    // the pop, so that wait_idle sees the frame closed.
    if (trace_ != nullptr && result != RunResult::waiting && stream->ends_frame()) {
        trace_->end_frame();
    }
    {
        const std::scoped_lock lock(mutex_);
        // The run has returned: an unmap waiting for it may go on, and the ring is not read again.
        running_ring_ = -1;
        idle_.notify_all();
        if (ring != nullptr && ring->mapped) {
            // The read offset: the stream's position in the ring, wrapped at its end. A failed
            // stream is dropped, so the offset moves past all of it, as for a finished one.
            const std::uint32_t* const read =
                result == RunResult::error ? stream->top_end() : stream->top_position();
            const auto offset = static_cast<std::uint32_t>(read - ring->dwords.data()) &
                                static_cast<std::uint32_t>(ring->dwords.size() - 1);
            std::atomic_ref(*ring->read_pointer).store(offset, std::memory_order_release);
        }
        if (result == RunResult::waiting) {
            waiting = true;
            return packets_.load(std::memory_order_relaxed) != packets;
        }
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
