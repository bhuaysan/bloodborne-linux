// The GPU thread (docs/design/graphics.md, "One GPU thread"). The submit imports only queue
// streams; this thread runs them with one parser and one register mirror. It visits the queues
// round-robin and runs the front stream of each until it ends, fails or waits. A failing stream
// is logged and dropped; a waiting one stays at the front of its queue and is resumed on a later
// round, so a wait on one queue doesn't hold up the others. When every queue is empty or waiting,
// the thread sleeps until a submission arrives, or polls the waits every 100 µs.
//
// The graphics queue is the only queue so far. The CCB stream (#272) and the compute rings
// (#275) become further queues.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <thread>

#include "gpu/pm4/parser.hpp"
#include "gpu/pm4/registers.hpp"

namespace bb::pm4 {

enum class Queue : std::uint8_t { graphics };
inline constexpr std::size_t queue_count = 1;

class GpuThread {
public:
    struct Stats {
        std::uint64_t submitted = 0;  // streams queued
        std::uint64_t finished = 0;   // streams that ended or failed
        std::uint64_t errors = 0;     // of those, the failed ones
        std::uint64_t packets = 0;    // type-3 packets that ran
    };

    // Installs the CLEAR_STATE defaults (context_defaults()) and starts with them in the context
    // registers, then starts the thread. `readable` checks INDIRECT_BUFFER targets
    // (Parser::set_readable).
    explicit GpuThread(Parser::Readable readable = {});
    // Stops the thread, also while a stream waits. Queued streams are dropped.
    ~GpuThread();

    GpuThread(const GpuThread&) = delete;
    GpuThread& operator=(const GpuThread&) = delete;

    // Queues one stream per buffer, in order, at the end of `queue`. The buffers must stay
    // readable until their streams finished.
    void submit(Queue queue, std::span<const std::span<const std::uint32_t>> buffers);

    // Waits until every queue is empty. False if `timeout` passed first.
    bool wait_idle(std::chrono::milliseconds timeout);

    Stats stats() const;

    // The register mirror. Read it only after wait_idle returned true and before the next submit.
    const RegisterMirror& registers() const { return regs_; }

private:
    void loop();
    // Runs the front stream of one queue. Sets `waiting` if it waits. Returns whether any packet
    // ran or the stream finished.
    bool run_front(std::deque<Stream>& queue, bool& waiting);

    RegisterMirror regs_;
    Parser parser_{regs_};

    mutable std::mutex mutex_;
    std::condition_variable work_;  // a submission arrived, or stop
    std::condition_variable idle_;  // a stream finished
    // Submissions only append, so the front stream stays in place while the thread runs it
    // outside the lock (a deque keeps references on push_back).
    std::array<std::deque<Stream>, queue_count> queues_;
    Stats stats_;
    std::atomic<std::uint64_t> packets_{0};
    bool stop_ = false;

    std::thread thread_;  // last: starts once the members above exist
};

}  // namespace bb::pm4
