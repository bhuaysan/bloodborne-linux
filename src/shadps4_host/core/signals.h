// The piece of shadPS4's signal dispatcher that the translator touches. It registers a fault
// handler for its SRT walker code. Nothing runs a walker before the renderer does (R4), which
// connects this to the runtime's fault handling, so for now the registration is only kept.
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

    AccessViolationHandler access_violation_handler = nullptr;
};

using Signals = Common::Singleton<SignalDispatch>;

}  // namespace Core
