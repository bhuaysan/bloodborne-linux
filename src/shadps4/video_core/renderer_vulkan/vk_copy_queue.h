// SPDX-License-Identifier: GPL-2.0-or-later
// Copies of CPU-written guest memory into staging space, run on copy threads instead of the GPU
// thread (#777, docs/design/command-recording.md). The GPU thread reserves the staging space,
// records the Vulkan copy that reads it and pushes the memmove here; Push returns a ticket. The
// scheduler stamps every submit with the last ticket pushed before it and submits only once every
// job up to that ticket is done, so the GPU never reads staging space a job has not filled. The
// GPU thread itself waits only where the guest could see progress past a draw (Liverpool's
// labels, interrupts and memory writes) and where it waits for the GPU anyway (Finish, Wait).

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "common/types.h"
#include "common/unique_function.h"

namespace Vulkan {

class CopyQueue {
public:
    using Job = Common::UniqueFunction<void>;

    /// The threads start with the first job, so a queue that never gets one costs nothing.
    explicit CopyQueue(unsigned threads);
    /// Runs every queued job, then stops the threads.
    ~CopyQueue();
    CopyQueue(const CopyQueue&) = delete;
    CopyQueue& operator=(const CopyQueue&) = delete;

    /// Queues a job and returns its ticket. Tickets count up from 1.
    u64 Push(Job&& job);

    /// The ticket of the last job pushed, or 0.
    [[nodiscard]] u64 Issued() const noexcept {
        return issued.load(std::memory_order_relaxed);
    }

    /// Every job up to this ticket is done.
    [[nodiscard]] u64 Completed() const noexcept {
        return completed.load(std::memory_order_acquire);
    }

    /// Returns once every job up to ticket is done. Any thread.
    void Wait(u64 ticket);

    /// Returns once every job pushed so far is done.
    void WaitIdle() {
        Wait(Issued());
    }

private:
    struct Queued {
        Job job;
        u64 ticket;
    };

    void Loop();
    void Finish(u64 ticket);

    const unsigned num_threads;
    std::mutex mutex;
    std::condition_variable work_cv; // a job queued or stop
    std::condition_variable done_cv; // completed moved on
    std::deque<Queued> queue;
    // Whether each ticket from completed + 1 on is done; jobs finish out of order across threads.
    std::deque<bool> done;
    std::atomic<u64> issued{0};
    std::atomic<u64> completed{0};
    bool stop = false;
    std::vector<std::thread> threads;
};

} // namespace Vulkan
