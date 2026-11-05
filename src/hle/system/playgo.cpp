// libScePlayGo: the game is fully installed, as one chunk.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string_view>

#include "hle/hle.hpp"
#include "hle/system/internal.hpp"
#include "hle/system/log.hpp"
#include "hle/system/system.hpp"

namespace bb::hle::system {
namespace {

constexpr std::string_view library = "libScePlayGo";

std::atomic<bool> opened = false;

bool valid(std::int32_t handle) {
    return opened && handle == playgo_handle;
}

}  // namespace

std::int32_t playgo_initialize(const void* /*params*/) {
    // The game passes a 2 MiB buffer (0x1fdd668, 0x1fde837) and ignores the result.
    log(library, BB_SYSTEM_CALLER(), "initialize");
    return 0;
}

std::int32_t playgo_open(std::int32_t* handle, const void* /*params*/) {
    log(library, BB_SYSTEM_CALLER(), "open");
    if (handle == nullptr) {
        return playgo_bb_error_invalid_argument;
    }
    *handle = playgo_handle;
    opened = true;
    return 0;
}

std::int32_t playgo_get_chunk_ids(std::int32_t handle, std::uint16_t* ids, std::uint32_t max,
                                  std::uint32_t* count) {
    if (!valid(handle)) {
        return playgo_bb_error_bad_handle;
    }
    if (count == nullptr || (ids == nullptr && max != 0)) {
        return playgo_bb_error_invalid_argument;
    }
    *count = std::min<std::uint32_t>(max, 1);
    if (*count != 0) {
        ids[0] = playgo_chunk_id;
    }
    return 0;
}

std::int32_t playgo_get_chunk_states(std::int32_t handle, const std::uint16_t* ids,
                                     std::uint32_t count, std::int8_t* states) {
    if (!valid(handle)) {
        return playgo_bb_error_bad_handle;
    }
    if (count != 0 && (ids == nullptr || states == nullptr)) {
        return playgo_bb_error_invalid_argument;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (ids[i] != playgo_chunk_id) {
            log(library, BB_SYSTEM_CALLER(), "chunk states: unknown chunk id {}", ids[i]);
        }
        states[i] = playgo_chunk_installed;  // everything is there, known chunk or not
    }
    return 0;
}

std::int32_t playgo_set_mode(std::int32_t handle, std::int32_t mode) {
    log(library, BB_SYSTEM_CALLER(), "4AAcTU9R3XM: mode {}: ignored", mode);
    return valid(handle) ? 0 : playgo_bb_error_bad_handle;
}

void reset_playgo() {
    opened = false;
}

BB_HLE_FUNCTION("libScePlayGo", "ts6GlZOKRrE", playgo_initialize);
BB_HLE_FUNCTION("libScePlayGo", "M1Gma1ocrGE", playgo_open);
BB_HLE_FUNCTION("libScePlayGo", "73fF1MFU8hA", playgo_get_chunk_ids);     // name unknown
BB_HLE_FUNCTION("libScePlayGo", "uWIYLFkkwqk", playgo_get_chunk_states);  // name unknown
BB_HLE_FUNCTION("libScePlayGo", "4AAcTU9R3XM", playgo_set_mode);          // name unknown

}  // namespace bb::hle::system
