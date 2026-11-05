#include "hle/system/system.hpp"

#include "hle/system/internal.hpp"

namespace bb::hle::system {

void reset() {
    reset_sysmodule();
    reset_user_service();
    reset_playgo();
}

}  // namespace bb::hle::system
