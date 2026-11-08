#include <ctime>

#include "hle/hle.hpp"
#include "hle/system/system.hpp"

namespace bb::hle::system {

std::int32_t sceAudioOutOutput(std::int32_t, const void*) {
    constexpr long period_nsec =
        static_cast<long>(1'000'000'000ull * audio_out_grain / audio_out_rate);
    const timespec t{0, period_nsec};
    clock_nanosleep(CLOCK_MONOTONIC, 0, &t, nullptr);
    return 0;
}

BB_HLE_FUNCTION("libSceAudioOut", "QOQtbeDqsT4", sceAudioOutOutput);

}  // namespace bb::hle::system
