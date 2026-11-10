// shadPS4's helper for the text of the last system error, which video_core puts into assertions.
#pragma once

#include <cerrno>
#include <cstring>
#include <string>

namespace Common {

inline std::string GetLastErrorMsg() {
    return std::strerror(errno);
}

}  // namespace Common
