// The worker threads that create pipelines off the GPU thread and the GPU thread's wait budget
// (vk_pipeline_workers.h, #521). No Vulkan device needed.

#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_pipeline_workers.h"

namespace {

using namespace std::chrono_literals;
using Vulkan::PipelineWaitBudget;
using Vulkan::PipelineWorkers;

TEST(PipelineWorkers, RunsEveryJobBeforeWaitIdleReturns) {
    PipelineWorkers workers{3, "test-workers"};
    std::atomic<int> done{0};
    for (int i = 0; i < 100; ++i) {
        workers.Submit([&done] { ++done; });
    }
    workers.WaitIdle();
    EXPECT_EQ(done.load(), 100);
}

TEST(PipelineWorkers, RunsJobsAtTheSameTime) {
    // Each job waits until both have started, which only ends if two threads run them.
    PipelineWorkers workers{2, "test-workers"};
    std::latch both_started{2};
    std::atomic<int> done{0};
    for (int i = 0; i < 2; ++i) {
        workers.Submit([&] {
            both_started.arrive_and_wait();
            ++done;
        });
    }
    workers.WaitIdle();
    EXPECT_EQ(done.load(), 2);
}

TEST(PipelineWorkers, DestructionFinishesQueuedJobs) {
    std::atomic<int> done{0};
    {
        PipelineWorkers workers{1, "test-workers"};
        for (int i = 0; i < 20; ++i) {
            workers.Submit([&done] {
                std::this_thread::sleep_for(1ms);
                ++done;
            });
        }
    }
    EXPECT_EQ(done.load(), 20);
}

TEST(PipelineWorkers, TakesMoveOnlyJobs) {
    PipelineWorkers workers{1, "test-workers"};
    auto value = std::make_unique<int>(7);
    std::atomic<int> seen{0};
    workers.Submit([&seen, value = std::move(value)] { seen = *value; });
    workers.WaitIdle();
    EXPECT_EQ(seen.load(), 7);
}

TEST(PipelineWaitBudget, SpendsTheBudgetOncePerFrame) {
    PipelineWaitBudget budget{8ms};
    EXPECT_EQ(budget.Allowance(), 8ms);
    budget.Spend(5ms);
    EXPECT_EQ(budget.Allowance(), 3ms);
    budget.Spend(4ms); // a wait may overshoot its allowance a little
    EXPECT_EQ(budget.Allowance(), 0ms);
    budget.NewFrame();
    EXPECT_EQ(budget.Allowance(), 8ms);
}

} // namespace
