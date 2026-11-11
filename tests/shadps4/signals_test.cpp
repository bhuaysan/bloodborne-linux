// shadPS4's signal dispatcher on the host layer (src/shadps4_host/core/signals.h).

#include <vector>

#include <gtest/gtest.h>

#include "core/signals.h"

namespace {

// Fault handlers that record the order they ran in and claim the fault or pass it on.
std::vector<int> fault_calls;
bool claim_fault = false;

bool fault_handler_a(void*, void*) {
    fault_calls.push_back(0);
    return false;
}

bool fault_handler_b(void*, void*) {
    fault_calls.push_back(1);
    return claim_fault;
}

bool fault_handler_c(void*, void*) {
    fault_calls.push_back(2);
    return true;
}

// Every registered handler is kept and tried in priority order until one claims the fault, as
// upstream does: the page manager (priority 0) and the SRT walker (priority 1) both stay reachable,
// whichever registers first.
TEST(ShadPs4Signals, TriesHandlersByPriority) {
    Core::SignalDispatch dispatch;
    fault_calls.clear();
    EXPECT_FALSE(dispatch.DispatchAccessViolation(nullptr, nullptr));
    EXPECT_TRUE(fault_calls.empty());

    dispatch.RegisterAccessViolationHandler(fault_handler_b, 1);
    dispatch.RegisterAccessViolationHandler(fault_handler_a, 0);
    claim_fault = false;
    EXPECT_FALSE(dispatch.DispatchAccessViolation(nullptr, nullptr));
    EXPECT_EQ(fault_calls, (std::vector<int>{0, 1}));

    // An equal priority runs after the one registered before it.
    dispatch.RegisterAccessViolationHandler(fault_handler_c, 1);
    fault_calls.clear();
    EXPECT_TRUE(dispatch.DispatchAccessViolation(nullptr, nullptr));
    EXPECT_EQ(fault_calls, (std::vector<int>{0, 1, 2}));

    // A handler that claims the fault ends the dispatch.
    claim_fault = true;
    fault_calls.clear();
    EXPECT_TRUE(dispatch.DispatchAccessViolation(nullptr, nullptr));
    EXPECT_EQ(fault_calls, (std::vector<int>{0, 1}));
}

TEST(ShadPs4SignalsDeathTest, HandlerBeyondTheStorageIsFatal) {
    EXPECT_DEATH(
        {
            Core::SignalDispatch dispatch;
            for (int i = 0; i < 5; ++i) {
                dispatch.RegisterAccessViolationHandler(fault_handler_a, 0);
            }
        },
        "no room for another handler");
}

}  // namespace
