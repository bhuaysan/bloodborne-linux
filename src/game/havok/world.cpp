// The Havok physics world's step at the target frame rate
// (docs/design/frame-rate.md, "The Havok world step").
#include "game/havok/world.hpp"

#include <immintrin.h>

#include <cstdint>

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hle/hle.hpp"
#include "hook/hook.hpp"

namespace bb::game {
namespace {

const char* text(std::uint64_t link) {
    return reinterpret_cast<const char*>(image().runtime(link));
}

// The pthread functions the game's Havok build calls (libScePosix, symbols/nids.csv). The mutex
// is a cell that holds the guest mutex.
using MutexCall = int(void* mutex);
MutexCall* posix(const char* nid) {
    return reinterpret_cast<MutexCall*>(hle::import_address("libScePosix", nid));
}

// Havok's assert of a failed pthread call: "<file>:<line>:<function>", perror(<call>), int3.
// The game goes on after the trap.
void havok_pthread_failed(std::uint64_t file, int line, std::uint64_t function,
                          std::uint64_t call) {
    using Printf = int(const char*, ...);
    using Perror = void(const char*);
    reinterpret_cast<Printf*>(hle::import_address("libc", "hcuQgD53UxM"))(
        text(0x049311d2), text(file), line, text(function));  // "%s:%d:%s\n"
    reinterpret_cast<Perror*>(hle::import_address("libc", "EMutwaQ34Jo"))(text(call));
    asm volatile("int3");
}

// The job queue's hkCriticalSection (+0x88: the mutex cell, +0x90: the spin count), entered with
// Havok's lockMutexWithSpinCount (hkPthreadUtil.h): trylock up to `spin count` times with ten
// pauses after each failure, then lock.
void enter_critical_section(void* section) {
    static MutexCall* const trylock = posix("K-jXhbt2gn4");
    static MutexCall* const lock = posix("7H0iTOciTLo");
    for (std::uint32_t spins = at<std::uint32_t>(section, 8); spins != 0; --spins) {
        if (trylock(section) == 0) {
            return;
        }
        for (int i = 0; i < 10; ++i) {
            _mm_pause();
        }
    }
    if (lock(section) != 0) {
        // hkPthreadUtil.h, line 0x3b, "lockMutexWithSpinCount", "pthread_mutex_lock(&mutex)"
        havok_pthread_failed(0x0493127c, 0x3b, 0x049312e3, 0x049312fa);
    }
}

void leave_critical_section(void* section) {
    static MutexCall* const unlock = posix("2Z+PpY6CaJg");
    if (unlock(section) != 0) {
        // hkPosixCriticalSection.inl, line 0x26, "leave", "pthread_mutex_unlock(&m_mutex)"
        havok_pthread_failed(0x049311dc, 0x26, 0x04931257, 0x0493125d);
    }
}

// The per-frame counter of an unidentified singleton (0x0553b168): every 200 counts it hands the
// count / 200 to each item of its entries. Not part of the step.
// TODO(name): what the singleton counts.
void hand_out_counter() {
    char* const stats = global<char*>(0x0553b168);
    if (stats == nullptr) {
        return;
    }
    const std::int32_t count = at<std::int32_t>(stats, 0x30);
    const std::int32_t per200 = count / 200;
    at<std::int32_t>(stats, 0x34) = count;
    at<std::int32_t>(stats, 0x30) = 0;
    if (at<std::int32_t>(stats, 0x38) == per200) {
        return;
    }
    at<std::int32_t>(stats, 0x38) = per200;
    if (static_cast<std::uint32_t>(count) + 199u < 399u) {  // per200 == 0
        return;
    }
    const std::int32_t entries = at<std::int32_t>(stats, 0x20);
    for (std::int32_t i = 0; i < entries; ++i) {
        if (count < 200) {
            continue;
        }
        char* const entry = at<char*>(stats, 0x28) + static_cast<std::uint64_t>(i) * 0x1f8;
        const auto& items = at<std::int32_t>(entry, 0xc8);
        char* const& first = at<char*>(entry, 0xd0);
        if (items <= 0) {
            continue;
        }
        std::uint64_t offset = 0;
        for (std::int32_t j = 0; j < items; ++j, offset += 0x2e0) {
            fn<void(void*, std::int32_t)>(0x015a5fc0)(first + offset, per200);
        }
    }
}

}  // namespace

// 0x017f99d0
void havok_world_step(void* self) {
    const std::uint8_t held = at<std::uint8_t>(self, 0x30);
    if (void* const before = at<void*>(self, 0x260); before != nullptr) {
        fn<void(void*)>(0x017fd220)(before);
    }
    if (at<void*>(self, 0x28) != nullptr) {
        hand_out_counter();
        fn<void(void*)>(0x01806e90)(at<void*>(self, 0x28));
    }

    if (held != 0) {
        // The world isn't stepped; the call takes the world's +0xd8.
        if (void* const holder = at<void*>(self, 0x28); holder != nullptr) {
            fn<void(void*)>(0x00b50410)(at<char*>(holder, 8) + 0xd8);
        }
        return;
    }
    void* const pool = at<void*>(self, 0x70);  // TODO(name): Havok's job thread pool, by its use
    if (at<std::uint8_t>(self, 0x26c) == 0 || pool == nullptr || at<void*>(self, 0x28) == nullptr) {
        return;
    }

    HavokStepInfo info{};
    info.delta_time = frame_step();  // the game: 1/30 (0x017f9b71)
    info.manager_c8 = at<std::uint64_t>(self, 0xc8);
    info.threads = static_cast<std::int32_t>(vcall<std::uint32_t>(pool, 0x48) + 1u);
    info.zero = 0;

    // Marks the job queue (+0x68) under its lock.
    char* const queue = at<char*>(self, 0x68);
    enter_critical_section(queue + 0x88);
    if (queue[0xc9] == 0) {
        queue[0xc9] = 1;
    }
    leave_critical_section(queue + 0x88);

    vcall<void>(at<void*>(self, 0x70), 0x30, at<void*>(self, 0x68), std::uint64_t{2});
    fn<void(void*)>(0x01e42140)(at<void*>(self, 0x20));
    std::uint64_t stage = 0;  // written by the collide stage, read by the solve stage
    fn<void(void*, HavokStepInfo*, void*, std::uint64_t*)>(0x00b5a2e0)(
        at<void*>(at<void*>(self, 0x28), 8), &info, at<void*>(self, 0x68), &stage);
    fn<void(void*, std::uint64_t*, void*)>(0x00b5b100)(at<void*>(at<void*>(self, 0x28), 8),
                                                        &stage, at<void*>(self, 0x68));
    fn<void(void*)>(0x0043b0a0)(at<void*>(self, 0x68));
}

}  // namespace bb::game

BB_REPLACE(0x017f99d0, bb::game::havok_world_step);
