// Shared between the source files of the system services, not part of their interface.
#pragma once

namespace bb::hle::system {

// Each module's part of reset().
void reset_sysmodule();
void reset_user_service();

}  // namespace bb::hle::system
