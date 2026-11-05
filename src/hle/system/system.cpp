#include "hle/system/system.hpp"

#include "hle/system/internal.hpp"

namespace bb::hle::system {

void reset() {
    reset_sysmodule();
    reset_user_service();
}

}  // namespace bb::hle::system
