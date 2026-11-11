// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/gnmdriver/gnmdriver.cpp (sceGnmInsertPushMarker,
//   sceGnmInsertSetMarker, sceGnmInsertPopMarker, sceGnmIsUserPaEnabled,
//   sceGnmDriverCaptureInProgress) @0d5f2739d2544d2a35c73e354cba7a32b7a81bea
// Changed for bloodborne-linux, 2026-11-06: one writer for both string markers, sizes from the
// game's writers, our packet headers and logging.
#include "hle/video/gnm_state.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <string_view>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/video/gnm_default_state.hpp"
#include "si_ci_vi_merged_pm4_it_opcodes.h"

namespace bb::hle::video {
namespace {

constexpr std::string_view channel = "hle.libSceGnmDriver";

std::uint32_t init_default_state(std::string_view call, std::uint32_t* cmd, std::uint32_t dwords,
                                 std::span<const std::uint32_t> state) {
    const std::uint32_t written = pm4::write_default_state(cmd, dwords, state);
    if (written == 0) {
        common::log(common::LogLevel::warn, channel, "{}: no room ({} dwords at {})", call,
                    dwords, static_cast<const void*>(cmd));
    }
    return written;
}

std::int32_t string_marker(std::uint32_t tag, std::uint32_t* cmd, std::uint32_t dwords,
                           const char* marker) {
    if (cmd == nullptr || marker == nullptr) {
        return gnm_marker_error;
    }
    const std::size_t length = std::strlen(marker);
    if (length > 0xFFFF || dwords != marker_dwords(static_cast<std::uint32_t>(length))) {
        common::log(common::LogLevel::debug, channel, "marker \"{}\": {} dwords don't fit", marker,
                    dwords);
        return gnm_marker_error;
    }
    cmd[0] = pm4::type3_header(IT_NOP, dwords - 1);
    cmd[1] = tag;
    auto* text = reinterpret_cast<char*>(cmd + 2);
    const std::size_t bytes = std::size_t{dwords - 2} * 4;
    std::memcpy(text, marker, length);
    std::fill(text + length, text + bytes, '\0');
    return 0;
}

}  // namespace

std::uint32_t sceGnmDrawInitDefaultHardwareState200(std::uint32_t* cmd, std::uint32_t dwords) {
    return init_default_state("draw default state", cmd, dwords, pm4::draw_default_state());
}

std::uint32_t sceGnmDispatchInitDefaultHardwareState(std::uint32_t* cmd, std::uint32_t dwords) {
    return init_default_state("dispatch default state", cmd, dwords,
                              pm4::dispatch_default_state());
}

std::int32_t sceGnmInsertPushMarker(std::uint32_t* cmd, std::uint32_t dwords,
                                    const char* marker) {
    return string_marker(marker_push_tag, cmd, dwords, marker);
}

std::int32_t sceGnmInsertSetMarker(std::uint32_t* cmd, std::uint32_t dwords, const char* marker) {
    return string_marker(marker_set_tag, cmd, dwords, marker);
}

std::int32_t sceGnmInsertPopMarker(std::uint32_t* cmd, std::uint32_t dwords) {
    if (cmd == nullptr || dwords != pop_marker_dwords) {
        return gnm_marker_error;
    }
    cmd[0] = pm4::type3_header(IT_NOP, pop_marker_dwords - 1);
    cmd[1] = marker_pop_tag;
    std::fill_n(cmd + 2, pop_marker_dwords - 2, 0u);
    return 0;
}

std::int32_t sceGnmIsUserPaEnabled() { return 0; }

std::int32_t sceGnmDriverCaptureInProgress() { return 0; }

BB_HLE_FUNCTION("libSceGnmDriver", "0H2vBYbTLHI", sceGnmDrawInitDefaultHardwareState200);
BB_HLE_FUNCTION("libSceGnmDriver", "nF6bFRUBRAU", sceGnmDispatchInitDefaultHardwareState);
BB_HLE_FUNCTION("libSceGnmDriver", "W1Etj-jlW7Y", sceGnmInsertPushMarker);
BB_HLE_FUNCTION("libSceGnmDriver", "jiItzS6+22g", sceGnmInsertSetMarker);
BB_HLE_FUNCTION("libSceGnmDriver", "7qZVNgEu+SY", sceGnmInsertPopMarker);
BB_HLE_FUNCTION("libSceGnmDriver", "jg33rEKLfVs", sceGnmIsUserPaEnabled);
BB_HLE_FUNCTION("libSceGnmDriver", "TLV4mswiZ4A", sceGnmDriverCaptureInProgress);

}  // namespace bb::hle::video
