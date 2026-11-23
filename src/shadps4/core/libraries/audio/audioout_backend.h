// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/audio/audioout_backend.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-12. 2026-11-23: SetPlaybackDevice and
// FindPlaybackDevice choose the output device (bb-run --audio-device, #750).
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Libraries::AudioOut {

struct PortOut;

class PortBackend {
public:
    virtual ~PortBackend() = default;

    /// Guaranteed to be called in intervals of at least port buffer time,
    /// with size equal to port buffer size.
    virtual void Output(void* ptr) = 0;

    virtual void SetVolume(const std::array<int, 8>& ch_volumes) = 0;
};

class AudioOutBackend {
public:
    AudioOutBackend() = default;
    virtual ~AudioOutBackend() = default;

    virtual std::unique_ptr<PortBackend> Open(PortOut& port) = 0;
};

class SDLAudioOut final : public AudioOutBackend {
public:
    std::unique_ptr<PortBackend> Open(PortOut& port) override;
};

// The playback device the ports open from now on (bb-run --audio-device, config key audio_device):
// a part of an SDL device name, as FindPlaybackDevice matches it. Empty for SDL's default device,
// which follows the desktop's default output. Set before the game opens its ports.
void SetPlaybackDevice(std::string name);

// The index in `names` of the device `part` picks: the whole name first (the launcher writes it),
// else the first name that contains it, ignoring case. Nothing when none does or `part` is empty.
std::optional<std::size_t> FindPlaybackDevice(const std::vector<std::string>& names,
                                              std::string_view part);

} // namespace Libraries::AudioOut
