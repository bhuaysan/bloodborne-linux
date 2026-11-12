// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/common/slot_vector.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-12
// License texts: third_party/shadps4/LICENSES/

#include <cstddef>
#include <sys/mman.h>
#include "common/assert.h"

namespace Common {

void* ReserveMemoryPages(std::size_t size) noexcept {
    void* const base =
        mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    ASSERT(base != MAP_FAILED);
    return base;
}

void CommitMemoryPages(void* address, std::size_t size) noexcept {
    void* const result =
        mmap(address, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    ASSERT(result == address);
}

void ReleaseMemoryPages(void* base, std::size_t size) noexcept {
    if (!base) {
        return;
    }
    ASSERT(munmap(base, size) == 0);
}

} // namespace Common
