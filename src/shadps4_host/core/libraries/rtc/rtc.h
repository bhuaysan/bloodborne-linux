// The piece of shadPS4's RTC header that ImeDialog uses: the tick type in its event structs.
// libSceRtc itself is ours (src/hle/system/date_time.cpp).
#pragma once

#include "common/types.h"

namespace Libraries::Rtc {

struct OrbisRtcTick {
    u64 tick;
};

}  // namespace Libraries::Rtc
