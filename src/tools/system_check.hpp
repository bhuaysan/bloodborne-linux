// bb-run --check-system: checks what the game needs of the system, without the game, so that the
// portable package can be tested on a distribution (tools/distro-check.sh) and a player can see
// what is missing (docs/distributions.md, "Checking a system").
#pragma once

#include "tools/run_options.hpp"

namespace bb::tools {

// Checks, in this order, and prints one line per part to standard output, "<part>: ok, <what>" or
// "<part>: failed, <why>":
// - guest memory: the address ranges the loader and the kernel's guest window reserve are free;
// - ffmpeg: the decoders and the demuxer of the game's movies and sounds;
// - video: SDL opens a window on the display (with options.headless: SDL's offscreen driver);
// - audio: SDL opens the default playback device (with options.headless: SDL's dummy driver);
// - vulkan: the Vulkan instance and the device the renderer creates, on options.device.
// Returns 0, or 1 at the first part that fails (the later parts are not checked). A part that
// fails inside video_core's device setup ends the process there with its assertion message.
int check_system(const RunOptions& options);

}  // namespace bb::tools
