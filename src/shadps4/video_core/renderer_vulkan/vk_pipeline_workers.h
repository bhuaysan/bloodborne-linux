// Worker threads that create Vulkan pipelines off the GPU thread, and how long the GPU thread may
// wait for them (#521, docs/design/renderer.md, "Pipeline builds").

#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>

#include "common/types.h"

namespace Vulkan {

// A fixed set of threads that run jobs in the order they were submitted. Destruction waits for
// every job, queued or running.
class PipelineWorkers {
public:
    // `name` names the threads for debuggers (at most 15 characters).
    PipelineWorkers(u32 num_threads, const char* name) {
        for (u32 i = 0; i < num_threads; ++i) {
            threads.emplace_back([this, name] {
                pthread_setname_np(pthread_self(), name);
                Run();
            });
        }
    }

    ~PipelineWorkers() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        work_cv.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
    }

    PipelineWorkers(const PipelineWorkers&) = delete;
    PipelineWorkers& operator=(const PipelineWorkers&) = delete;

    // The job must not throw.
    void Submit(std::move_only_function<void()> job) {
        {
            std::scoped_lock lock{mutex};
            queue.push_back(std::move(job));
        }
        work_cv.notify_one();
    }

    // Returns once every job submitted before the call has finished.
    void WaitIdle() {
        std::unique_lock lock{mutex};
        idle_cv.wait(lock, [this] { return queue.empty() && running == 0; });
    }

private:
    void Run() {
        std::unique_lock lock{mutex};
        while (true) {
            work_cv.wait(lock, [this] { return stopping || !queue.empty(); });
            if (queue.empty()) {
                return; // stopping, and nothing is left
            }
            auto job = std::move(queue.front());
            queue.pop_front();
            ++running;
            lock.unlock();
            job();
            lock.lock();
            --running;
            if (queue.empty() && running == 0) {
                idle_cv.notify_all();
            }
        }
    }

    std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable idle_cv;
    std::deque<std::move_only_function<void()>> queue;
    u32 running = 0;
    bool stopping = false;
    std::vector<std::thread> threads; // last, so the threads start after the rest is set up
};

// How long the GPU thread may still wait for pipeline builds in the current frame. Within the
// budget a draw waits for its pipeline; past it, the draw is skipped until the pipeline is ready.
class PipelineWaitBudget {
public:
    using Clock = std::chrono::steady_clock;

    explicit PipelineWaitBudget(Clock::duration per_frame_) : per_frame{per_frame_} {}

    Clock::duration Allowance() const {
        return spent < per_frame ? per_frame - spent : Clock::duration{};
    }

    void Spend(Clock::duration waited) {
        spent += waited;
    }

    void NewFrame() {
        spent = {};
    }

private:
    Clock::duration per_frame;
    Clock::duration spent{};
};

} // namespace Vulkan
