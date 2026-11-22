// SPDX-License-Identifier: GPL-2.0-or-later
// Copy threads for staging uploads (#777); see vk_copy_queue.h.

#include <fmt/format.h>

#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_copy_queue.h"

namespace Vulkan {

CopyQueue::CopyQueue(unsigned threads_) : num_threads{threads_} {}

CopyQueue::~CopyQueue() {
    {
        std::scoped_lock lk{mutex};
        stop = true;
    }
    work_cv.notify_all();
    for (auto& thread : threads) {
        thread.join();
    }
}

u64 CopyQueue::Push(Job&& job) {
    const u64 ticket = issued.load(std::memory_order_relaxed) + 1;
    {
        std::scoped_lock lk{mutex};
        if (threads.empty()) {
            for (unsigned i = 0; i < num_threads; ++i) {
                threads.emplace_back([this, i] { Loop(i); });
            }
        }
        queue.push_back({std::move(job), ticket});
        done.push_back(false);
        issued.store(ticket, std::memory_order_relaxed);
    }
    work_cv.notify_one();
    return ticket;
}

void CopyQueue::Wait(u64 ticket) {
    if (Completed() >= ticket) {
        return;
    }
    std::unique_lock lk{mutex};
    done_cv.wait(lk, [&] { return Completed() >= ticket; });
}

void CopyQueue::Loop(unsigned index) {
    Common::SetCurrentThreadName(fmt::format("shadPS4:GpuCopy{}", index).c_str());
    std::unique_lock lk{mutex};
    while (true) {
        work_cv.wait(lk, [this] { return stop || !queue.empty(); });
        if (queue.empty()) {
            return; // stop, with every job run
        }
        Queued item = std::move(queue.front());
        queue.pop_front();
        lk.unlock();
        item.job();
        item.job = {};
        lk.lock();
        Finish(item.ticket);
    }
}

// Under mutex: marks ticket done and moves completed past every done ticket in a row.
void CopyQueue::Finish(u64 ticket) {
    u64 first = completed.load(std::memory_order_relaxed);
    done[ticket - first - 1] = true;
    bool moved = false;
    while (!done.empty() && done.front()) {
        done.pop_front();
        ++first;
        moved = true;
    }
    if (moved) {
        completed.store(first, std::memory_order_release);
        done_cv.notify_all();
    }
}

} // namespace Vulkan
