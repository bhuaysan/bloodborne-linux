// shadPS4's notification overlay, left out: notifications (the screenshot message) are dropped.
#pragma once

#include <string>
#include <vector>

#include "common/types.h"

namespace shadNotifications {

enum class position {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

inline void QueueNotification(std::string, float, position, std::vector<u8>) {}

}  // namespace shadNotifications
