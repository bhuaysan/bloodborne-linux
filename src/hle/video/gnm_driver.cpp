// The GPU submit calls of libSceGnmDriver, completed at once without running anything. Kept
// minimal so that M4 can replace this file; every other Gnm import stays a stub.
#include <atomic>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/video/video.hpp"

namespace bb::hle::video {
namespace {

constexpr std::string_view channel = "hle.libSceGnmDriver";

// The first submissions are logged at info, the rest at debug.
constexpr std::uint64_t logged_submits = 3;

std::atomic<std::uint64_t> submits{0};

void log_submit(std::string_view call, std::uint32_t count, const std::uint32_t* draw_sizes) {
    const std::uint64_t n = submits.fetch_add(1, std::memory_order_relaxed) + 1;
    const common::LogLevel level =
        n <= logged_submits ? common::LogLevel::info : common::LogLevel::debug;
    common::log(level, channel, "submission {} ({}): {} buffer(s), first {} bytes", n, call,
                count, count > 0 && draw_sizes != nullptr ? draw_sizes[0] : 0);
}

}  // namespace

std::int32_t gnm_submit(std::uint32_t count, const std::uint64_t*, const std::uint32_t* draw_sizes,
                        const std::uint64_t*, const std::uint32_t*) {
    log_submit("submit", count, draw_sizes);
    return 0;
}

std::int32_t gnm_submit_other(std::uint32_t count, const std::uint64_t*,
                              const std::uint32_t* draw_sizes, const std::uint64_t*,
                              const std::uint32_t*) {
    log_submit("submit (other)", count, draw_sizes);
    return 0;
}

std::int32_t gnm_submit_and_flip(std::uint32_t count, const std::uint64_t*,
                                 const std::uint32_t* draw_sizes, const std::uint64_t*,
                                 const std::uint32_t*, std::int32_t handle, std::int32_t index,
                                 std::int32_t mode, std::int64_t arg) {
    log_submit("submit and flip", count, draw_sizes);
    return sceVideoOutSubmitFlip(handle, index, mode, arg);
}

std::int32_t sceGnmSubmitDone() {
    return 0;
}

BB_HLE_FUNCTION("libSceGnmDriver", "zwY0YV91TTI", gnm_submit);           // name unknown
BB_HLE_FUNCTION("libSceGnmDriver", "iCO804ZgzdA", gnm_submit_other);     // name unknown
BB_HLE_FUNCTION("libSceGnmDriver", "xbxNatawohc", gnm_submit_and_flip);  // name unknown
BB_HLE_FUNCTION("libSceGnmDriver", "yvZ73uQUqrk", sceGnmSubmitDone);

}  // namespace bb::hle::video
