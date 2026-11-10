// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/common/adaptive_mutex.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#if __unix__
#include <pthread.h>
#endif

namespace Common {

#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
class AdaptiveMutex {
public:
    AdaptiveMutex() = default;
    ~AdaptiveMutex() {
        pthread_mutex_destroy(&mutex);
    }
    AdaptiveMutex(const AdaptiveMutex&) = delete;
    AdaptiveMutex& operator=(const AdaptiveMutex&) = delete;

    void lock() {
        pthread_mutex_lock(&mutex);
    }
    void unlock() {
        pthread_mutex_unlock(&mutex);
    }
    [[nodiscard]] bool try_lock() {
        return pthread_mutex_trylock(&mutex) == 0;
    }

private:
    pthread_mutex_t mutex = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;
};
#endif // PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP

} // namespace Common
