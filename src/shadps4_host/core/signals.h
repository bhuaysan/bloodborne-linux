// The piece of shadPS4's signal dispatcher that the translator touches. It registers a fault
// handler for its SRT walker code, which runs while translating and on bad guest pointers. Whoever
// owns the process's SIGSEGV handling (the corpus test, the runtime from R4) calls Dispatch.
#pragma once

#include "common/singleton.h"
#include "common/types.h"

namespace Core {

using AccessViolationHandler = bool (*)(void* context, void* fault_address);

class SignalDispatch {
public:
    void RegisterAccessViolationHandler(AccessViolationHandler handler, u32 /*priority*/) {
        access_violation_handler = handler;
    }

    // Whether the registered handler took care of the fault, so the instruction can be retried.
    bool DispatchAccessViolation(void* context, void* fault_address) const {
        return access_violation_handler != nullptr &&
               access_violation_handler(context, fault_address);
    }

private:
    AccessViolationHandler access_violation_handler = nullptr;
};

using Signals = Common::Singleton<SignalDispatch>;

}  // namespace Core
