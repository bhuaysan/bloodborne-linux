// The rasterizer's draw and dispatch counts, kept for bb-run's log.
#pragma once

#include <atomic>

#include "common/types.h"

struct GpuStatsImpl {
    void IncDrawCall() noexcept { draws.fetch_add(1, std::memory_order_relaxed); }
    void IncDispatch() noexcept { dispatches.fetch_add(1, std::memory_order_relaxed); }
    std::atomic<u64> draws{0};
    std::atomic<u64> dispatches{0};
};

inline GpuStatsImpl GpuStats;
