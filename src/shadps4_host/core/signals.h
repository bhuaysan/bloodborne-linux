// The piece of shadPS4's signal dispatcher that the translator touches. It registers a fault
// handler for its SRT walker code, which runs while translating and on bad guest pointers. The
// process's SIGSEGV handler (fault_handler.cpp, and the tests' own handlers) calls Dispatch.
// video_core's page manager registers its write-tracking handler too, at priority 0.
#pragma once

#include <array>
#include <atomic>

#include "common/log.hpp"
#include "common/singleton.h"
#include "common/types.h"

namespace Core {

using AccessViolationHandler = bool (*)(void* context, void* fault_address);

class SignalDispatch {
public:
    // Lower priorities run first, equal ones in registration order. Fixed storage, so a fault on
    // another thread never reads freed memory; registration is unsynchronized, as upstream.
    void RegisterAccessViolationHandler(AccessViolationHandler handler, u32 priority) {
        size_t at = count.load(std::memory_order_relaxed);
        if (at == handlers.size()) {
            bb::common::log_fatal("shadps4", "Core::SignalDispatch: no room for another handler");
        }
        for (; at > 0 && handlers[at - 1].priority > priority; --at) {
            handlers[at] = handlers[at - 1];
        }
        handlers[at] = {handler, priority};
        count.fetch_add(1, std::memory_order_release);
    }

    // Whether a handler took care of the fault, so the instruction can be retried. They are tried
    // in priority order.
    bool DispatchAccessViolation(void* context, void* fault_address) const {
        const size_t n = count.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i) {
            if (handlers[i].handler(context, fault_address)) {
                return true;
            }
        }
        return false;
    }

private:
    struct Entry {
        AccessViolationHandler handler;
        u32 priority;
    };
    std::array<Entry, 4> handlers{};
    std::atomic<size_t> count{0};
};

using Signals = Common::Singleton<SignalDispatch>;

}  // namespace Core
