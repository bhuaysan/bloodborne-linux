// The default hardware state, debug marker and profiling query imports of libSceGnmDriver
// (docs/design/graphics.md, item 18; docs/formats/gnm.md, "libSceGnmDriver imports"). The
// unreachable imports of that note stay logging stubs: nothing registers them.
#pragma once

#include <cstdint>

namespace bb::hle::video {

inline constexpr std::int32_t gnm_marker_error = -1;

// The first payload dword of the marker NOPs.
inline constexpr std::uint32_t marker_push_tag = 0x68750001;
inline constexpr std::uint32_t marker_pop_tag = 0x68750002;
inline constexpr std::uint32_t marker_set_tag = 0x68750003;

inline constexpr std::uint32_t pop_marker_dwords = 6;

// NID 0H2vBYbTLHI. Writes the graphics default state (pm4::draw_default_state) and pads it to
// pm4::default_state_dwords with a NOP. Returns the dwords written, which the 13 callers of
// 0x1072870 add to the cursor, or 0 when `dwords` is smaller.
std::uint32_t sceGnmDrawInitDefaultHardwareState200(std::uint32_t* cmd, std::uint32_t dwords);

// NID nF6bFRUBRAU. The same for compute command buffers (pm4::dispatch_default_state), called
// through 0x1089c50, e.g. by the compute queue set-up 0x26b1130.
std::uint32_t sceGnmDispatchInitDefaultHardwareState(std::uint32_t* cmd, std::uint32_t dwords);

// The dwords a push or set marker with a string of `length` bytes takes, as the game's writers
// size it (0x1077fe0, 0x1077470): 2 + (length + 8) / 4 + 2 * ((length + 12) / 8).
constexpr std::uint32_t marker_dwords(std::uint32_t length) {
    return 2 + (length + 8) / 4 + 2 * ((length + 12) / 8);
}

// NID W1Etj-jlW7Y. One NOP of `dwords`: marker_push_tag, then the string with its terminator,
// zero-filled. Fails with gnm_marker_error and writes nothing when `cmd` or `marker` is null or
// `dwords` is not marker_dwords(strlen(marker)); the writers advance the cursor only on 0.
std::int32_t sceGnmInsertPushMarker(std::uint32_t* cmd, std::uint32_t dwords, const char* marker);

// NID jiItzS6+22g. As sceGnmInsertPushMarker with marker_set_tag.
std::int32_t sceGnmInsertSetMarker(std::uint32_t* cmd, std::uint32_t dwords, const char* marker);

// NID 7qZVNgEu+SY. One NOP of pop_marker_dwords: marker_pop_tag and four zeros. Fails with
// gnm_marker_error when `cmd` is null or `dwords` is not pop_marker_dwords (the callers ignore
// the result and always advance by 6).
std::int32_t sceGnmInsertPopMarker(std::uint32_t* cmd, std::uint32_t dwords);

// NID jg33rEKLfVs. GPU profiling is off: returns 0, so the draw trailer and the performance
// counter writers stay unused.
std::int32_t sceGnmIsUserPaEnabled();

// NID TLV4mswiZ4A. No capture runs: returns 0, which leaves the flag at 0x55406d1 alone.
std::int32_t sceGnmDriverCaptureInProgress();

}  // namespace bb::hle::video
