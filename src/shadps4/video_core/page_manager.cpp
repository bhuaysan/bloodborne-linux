// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/page_manager.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-19
// License texts: third_party/shadps4/LICENSES/

#include <algorithm>
#include <atomic>
#include <utility>
#include "common/adaptive_mutex.h"
#include "common/assert.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/multi_level_page_table.h"
#include "common/signal_context.h"
#include "common/thread.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "common/frame_profile.hpp"

namespace VideoCore {

struct PageManager::Impl {
    // Pages are only write protected: with readbacks off nothing watches reads.
    struct PageState {
        u8 num_write_watchers;

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                           : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return Core::MemoryPermission::Read | WritePerm();
        }

        constexpr Core::MemoryPermission Update(PageOp write_op, bool update_write = true) {
            if (update_write) {
                if (write_op == PageOp::Track) {
                    ASSERT_MSG(num_write_watchers < 255, "Too many watchers");
                } else if (write_op == PageOp::Untrack) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                }
                num_write_watchers += std::to_underlying(write_op);
            }
            return Perms();
        }
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / NUM_REGION_PAGES;
    // Ours (#631): atomic, and cleared when its page manager goes, so a fault after that is not
    // ours. The handler stays registered. A fault that races with the destruction is not covered:
    // bb-run's rasterizer lives to the end of the process, and the tests destroy theirs once no
    // guest thread writes tracked pages.
    inline static std::atomic<Vulkan::Rasterizer*> rasterizer;

    Impl() = default;
    virtual ~Impl() = default;

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
        EnsurePages(address, address + size);
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    void EnsurePages(VAddr begin, VAddr end) {
        end = std::min(end, VAddr{1} << ADDRESS_BITS) - 1;
        const size_t start_page = begin >> PM_PAGE_BITS;
        const size_t end_page = end >> PM_PAGE_BITS;
        cached_pages.reserve(start_page, end_page);
        locks.reserve(start_page, end_page);
    }

    void UpdatePageWatchers(VAddr addr, u64 size, PageOp write_op) {
        const u64 page_start = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        Core::MemoryPermission perms{};
        u64 range_begin = page_start;
        u64 range_pages = 0;
        u64 potential_pages = 0;

        const auto release_pending = [&] {
            if (range_pages > 0) {
                Protect(range_begin << PM_PAGE_BITS, range_pages << PM_PAGE_BITS, perms);
                range_pages = 0;
                potential_pages = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page_start << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer.load(std::memory_order_acquire)->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
            EnsurePages(aligned_addr, aligned_end);
        }

        for (u64 page = page_start; page != page_end; ++page) {
            PageState* state = cached_pages.find(page);
            if (!state) {
                continue;
            }

            locks[page].lock();

            const auto old_perms = state->Perms();
            if (page == page_start) {
                perms = old_perms;
            }

            // Apply the change to the page state
            const auto new_perms = state->Update(write_op);
            if (new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_pages != 0) {
                ++potential_pages;
            }

            // If the page must be (un)protected
            if (new_perms != old_perms) {
                if (range_pages == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_pages = 1;
                }
                // Extend current range up to potential range
                range_pages = potential_pages;
            }
        }

        // Add pending (un)protect action
        release_pending();

        for (u64 page = page_start; page != page_end; ++page) {
            if (auto* lock = locks.find(page)) {
                lock->unlock();
            }
        }
    }

    void UpdatePageWatchersForRegion(VAddr base_addr, const Bounds& bounds,
                                     const RegionBits& write_mask, PageOp write_op) {
        const u64 base_page = base_addr >> PM_PAGE_BITS;
        const u64 page_start = bounds.start_word * PAGES_PER_WORD + bounds.start_page;
        const u64 page_end = bounds.end_word * PAGES_PER_WORD + bounds.end_page + 1;

        Core::MemoryPermission perms{};
        u64 range_begin = base_page + page_start;
        u64 range_pages = 0;
        u64 potential_pages = 0;

        const auto release_pending = [&] {
            if (range_pages > 0) {
                Protect(range_begin << PM_PAGE_BITS, range_pages << PM_PAGE_BITS, perms);
                range_pages = 0;
                potential_pages = 0;
            }
        };

        for (u64 page = page_start; page != page_end; ++page) {
            PageState* state = cached_pages.find(base_page + page);
            if (!state) {
                continue;
            }

            locks[base_page + page].lock();

            const auto old_perms = state->Perms();
            if (page == page_start) {
                perms = old_perms;
            }

            // Apply the change to the page state
            const bool update_write = write_op != PageOp::None && write_mask.GetPage(page);
            const auto new_perms = state->Update(write_op, update_write);

            if (new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_pages != 0) {
                // If the protection did not change, extend the potential range
                ++potential_pages;
            }

            // If the page must be (un)protected
            if (new_perms != old_perms) {
                if (range_pages == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_pages = 1;
                }
                // Extend current rango up to potential range
                range_pages = potential_pages;
            }
        }

        // Add pending (un)protect action
        release_pending();

        for (u64 page = page_start; page != page_end; ++page) {
            if (auto* lock = locks.find(base_page + page)) {
                lock->unlock();
            }
        }
    }

    u64 UntrackForFault(VAddr base_addr, u64 first_page, u64 max_pages) {
        const u64 first = (base_addr >> PM_PAGE_BITS) + first_page;
        // Pages are locked in ascending order, as in the other updates, and stay locked until
        // their protection matches their state.
        u64 pages = 0;
        for (; pages < max_pages; ++pages) {
            PageState* const state = cached_pages.find(first + pages);
            if (!state) {
                break;
            }
            auto& lock = locks[first + pages];
            lock.lock();
            // Another watcher (an image of the texture cache) keeps a following page protected,
            // and its fault must reach that watcher, so the run ends before it.
            if (pages > 0 && state->num_write_watchers != 1) {
                lock.unlock();
                break;
            }
            state->Update(PageOp::Untrack);
            if (pages == 0 && state->num_write_watchers != 0) {
                // The first page stays protected for another watcher: nothing to unprotect.
                locks[first].unlock();
                return 1;
            }
        }
        if (pages > 0) {
            Protect(first << PM_PAGE_BITS, pages << PM_PAGE_BITS,
                    Core::MemoryPermission::ReadWrite);
        }
        for (u64 page = first; page != first + pages; ++page) {
            locks[page].unlock();
        }
        return pages;
    }

    struct PageTraits {
        using Entry = PageState;
        static constexpr size_t ADDRESS_SPACE_BITS = ADDRESS_BITS;
        static constexpr size_t L1_BITS = 16;
        static constexpr size_t PAGE_BITS = PM_PAGE_BITS;
        static constexpr bool NULL_CHECK = false;
    };
    Common::MultiLevelPageTable<PageTraits> cached_pages;
    struct MutexTraits {
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
        using Entry = Common::AdaptiveMutex;
#else
        using Entry = std::mutex;
#endif
        static constexpr size_t ADDRESS_SPACE_BITS = ADDRESS_BITS;
        static constexpr size_t L1_BITS = 16;
        static constexpr size_t PAGE_BITS = PM_PAGE_BITS;
        static constexpr bool NULL_CHECK = false;
    };
    Common::MultiLevelPageTable<MutexTraits> locks;
};

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_, Core::MemoryManager& memory_)
        : Impl(), memory{memory_}, own{rasterizer_} {
        rasterizer.store(rasterizer_, std::memory_order_release);

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        bb::common::frame_profile::Scope profile(bb::common::frame_profile::Part::protect);
        auto& impl = memory.GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        impl.Protect(address, size, perms);
    }

    // Pages are never read protected, so only a write fault can be ours.
    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        if (!Common::IsWriteError(context)) {
            return false;
        }
        bb::common::frame_profile::Scope profile(bb::common::frame_profile::Part::fault);
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        const auto size = std::min<u64>(8, PageManager::GetNextPageAddr(addr) - addr);
        Vulkan::Rasterizer* const current = rasterizer.load(std::memory_order_acquire);
        return current != nullptr && current->InvalidateFromFault(addr, size);
    }

    ~SignalImpl() override {
        Vulkan::Rasterizer* expected = own;
        rasterizer.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }

    Core::MemoryManager& memory;
    Vulkan::Rasterizer* const own;
};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_, Core::MemoryManager& memory) {
    LOG_INFO(Config, "Memory tracking method: signals");
    impl = std::make_unique<SignalImpl>(rasterizer_, memory);
}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

void PageManager::UpdatePageWatchers(VAddr addr, u64 size, PageOp write_op) const {
    impl->UpdatePageWatchers(addr, size, write_op);
}

u64 PageManager::UntrackForFault(VAddr base_addr, u64 first_page, u64 max_pages) const {
    return impl->UntrackForFault(base_addr, first_page, max_pages);
}

void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, const Bounds& bounds,
                                              const RegionBits& write_mask,
                                              PageOp write_op) const {
    impl->UpdatePageWatchersForRegion(base_addr, bounds, write_mask, write_op);
}

} // namespace VideoCore
