// The GPU thread (docs/design/graphics.md, "One GPU thread"). The submit imports only queue
// streams; this thread runs them with one parser and one register mirror. It visits the queues
// round-robin and runs the front stream of each until it ends, fails or waits. A failing stream
// is logged and dropped; a waiting one stays at the front of its queue and is resumed on a later
// round, so a wait on one queue doesn't hold up the others. When every queue is empty or waiting,
// the thread sleeps until a submission arrives, or polls the waits every 100 µs.
//
// The graphics queue runs draw command buffers on the draw engine, the constant queue constant
// command buffers on the constant engine; the counters in the parser order the two. Each mapped
// compute ring is a further queue on the compute engine: a doorbell queues the ring from the last
// written offset to the new one (two streams when it wraps), and the thread writes the read
// offset back to the game after each run (docs/design/graphics.md, second-wave item 20).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <thread>

#include "gpu/pm4/gpu_trace.hpp"
#include "gpu/pm4/parser.hpp"
#include "gpu/pm4/registers.hpp"
#include "gpu/pm4/targets.hpp"

namespace bb::pm4 {

enum class Queue : std::uint8_t { graphics, constant };
inline constexpr std::size_t queue_count = 2;
// Compute rings: 7 pipes of 8 queues (shadPS4's limit; pipe 7 belongs to the system).
inline constexpr std::size_t compute_pipes = 7;
inline constexpr std::size_t queues_per_pipe = 8;
inline constexpr std::size_t ring_count = compute_pipes * queues_per_pipe;

class GpuThread {
public:
    struct Stats {
        std::uint64_t submitted = 0;  // streams queued
        std::uint64_t finished = 0;   // streams that ended or failed
        std::uint64_t errors = 0;     // of those, the failed ones
        std::uint64_t packets = 0;    // type-3 packets that ran
    };

    // Installs the CLEAR_STATE defaults (context_defaults()) and starts with them in the context
    // registers, then starts the thread. `readable` checks INDIRECT_BUFFER targets and the memory
    // packets' addresses (Parser::set_readable); `interrupt` receives the end-of-pipe interrupts
    // (Parser::set_interrupt) on the GPU thread.
    // `trace`, if not null, logs the packets, draws and dispatches (gpu_trace.hpp) and must
    // outlive the thread; without one a packet costs one branch more.
    explicit GpuThread(Parser::Readable readable = {}, Parser::Interrupt interrupt = {},
                       GpuTrace* trace = nullptr);
    // Stops the thread, also while a stream waits. Queued streams are dropped.
    ~GpuThread();

    GpuThread(const GpuThread&) = delete;
    GpuThread& operator=(const GpuThread&) = delete;

    // Queues one stream per buffer, in order, at the end of `queue`. The buffers must stay
    // readable until their streams finished.
    // `ends_frame` marks the last stream: the trace closes a frame when it finished.
    void submit(Queue queue, std::span<const std::span<const std::uint32_t>> buffers,
                bool ends_frame = false);

    // Hands dispatches, the parser's sync points and draws to a backend (Parser::set_dispatch,
    // set_sync and set_draw; without `draw`, draws are skipped). All run on the GPU thread, which
    // reads them without a lock: call it only while nothing is submitted (before the first
    // submit_* or map_ring); it asserts the queues are idle.
    void set_backend(Parser::Dispatch dispatch, Parser::Sync sync, Parser::Draw draw = {}) {
        const std::lock_guard lock(mutex_);
        assert(std::ranges::all_of(queues_, [](const auto& q) { return q.empty(); }) &&
               "set_backend while streams are queued");
        parser_.set_dispatch(std::move(dispatch));
        parser_.set_sync(std::move(sync));
        parser_.set_draw(std::move(draw));
    }

    // Waits until every queue is empty. False if `timeout` passed first.
    bool wait_idle(std::chrono::milliseconds timeout);

    // Maps a compute ring: `ring` is the game's ring, its size a power of two; the read offset in
    // dwords is written to `read_pointer` (here 0 at once, then after each run of the ring); its
    // interrupts report `interrupt_source`. Both must stay mapped until unmap_ring. Returns the
    // ring's index, or -1 when all are in use or the size isn't a power of two.
    int map_ring(std::span<const std::uint32_t> ring, std::uint32_t* read_pointer,
                 std::uint32_t interrupt_source);
    // Unmaps a ring: its queued streams are dropped and its read pointer no longer written. A run
    // under way finishes first: this returns only once the thread no longer reads the ring, so the
    // caller may free it. Don't call it from the parser's callbacks. False if `ring` isn't mapped.
    bool unmap_ring(int ring);
    // The game wrote the ring up to `write_offset` dwords (modulo its size): queues the dwords
    // from the previous write offset on. False if `ring` isn't mapped or the offset is past it.
    bool ring_doorbell(int ring, std::uint32_t write_offset);

    Stats stats() const;

    // The target size hints of the packets that ran. The draw callback reads them on the GPU
    // thread; anything else reads them only after wait_idle returned true, before the next submit.
    const ExtentHints& extent_hints() const { return hints_; }

    // The register mirror. Read it only after wait_idle returned true and before the next submit.
    const RegisterMirror& registers() const { return regs_; }
    // The parser, with CE RAM and the counters. The same rule as for registers().
    const Parser& parser() const { return parser_; }

private:
    void loop();
    struct Ring {
        std::span<const std::uint32_t> dwords;
        std::uint32_t* read_pointer = nullptr;
        std::uint32_t written = 0;  // the last write offset
        std::uint32_t interrupt_source = 0;
        bool mapped = false;
    };

    // Runs the front stream of queue `index` (the rings follow the two fixed queues). Sets
    // `waiting` if it waits. Returns whether any packet ran or the stream finished.
    bool run_front(std::size_t index, bool& waiting);
    // Drops the streams of an unmapped ring's queue. Call with the lock held.
    void drop_streams(std::deque<Stream>& queue);

    RegisterMirror regs_;
    ExtentHints hints_;  // fed by the trace callback, in stream order
    Parser parser_{regs_};

    mutable std::mutex mutex_;
    std::condition_variable work_;  // a submission arrived, or stop
    std::condition_variable idle_;  // a stream finished, or a run returned
    // Submissions only append, so the front stream stays in place while the thread runs it
    // outside the lock (a deque keeps references on push_back).
    std::array<std::deque<Stream>, queue_count + ring_count> queues_;
    std::array<Ring, ring_count> rings_;
    int running_ring_ = -1;  // the ring whose stream the thread is running, else -1
    Stats stats_;
    GpuTrace* trace_;
    std::atomic<std::uint64_t> packets_{0};
    bool stop_ = false;

    std::thread thread_;  // last: starts once the members above exist
};

}  // namespace bb::pm4
