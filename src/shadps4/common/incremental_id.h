// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/common/incremental_id.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once
#include <atomic>

namespace Common {
template <typename CounterType>
class IncrementalIdProvider {
public:
    IncrementalIdProvider() : counter(0) {}
    ~IncrementalIdProvider() = default;

    CounterType Next() {
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }

private:
    std::atomic<CounterType> counter;
};

} // namespace Common