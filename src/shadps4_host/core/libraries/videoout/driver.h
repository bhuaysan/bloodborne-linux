// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/videoout/driver.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-11: only the flip labels of VideoOutPort, which
// liverpool.cpp waits on. The rest of the driver is adapted into src/hle/video (#432).
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <array>
#include <condition_variable>
#include <mutex>

#include "common/types.h"
#include "core/libraries/videoout/buffer.h"

namespace Libraries::VideoOut {

struct VideoOutPort {
    std::array<u64, MaxDisplayBuffers> buffer_labels; // should be contiguous in memory
    static_assert(sizeof(buffer_labels[0]) == 8u);
    std::mutex vo_mutex;
    std::condition_variable vo_cv;

    bool IsVoLabel(const u64* address) const {
        const u64* start = &buffer_labels[0];
        const u64* end = &buffer_labels[MaxDisplayBuffers - 1];
        return address >= start && address <= end;
    }

    void WaitVoLabel(auto&& pred) {
        std::unique_lock lk{vo_mutex};
        vo_cv.wait(lk, pred);
    }

    void SignalVoLabel() {
        std::scoped_lock lk{vo_mutex};
        vo_cv.notify_one();
    }
};

} // namespace Libraries::VideoOut
