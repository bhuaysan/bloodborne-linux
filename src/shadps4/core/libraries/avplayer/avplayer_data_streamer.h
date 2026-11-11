// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/avplayer/avplayer_data_streamer.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <string_view>

struct AVIOContext;

namespace Libraries::AvPlayer {

class IDataStreamer {
public:
    virtual ~IDataStreamer() = default;
    virtual bool Init(std::string_view path) = 0;
    virtual void Reset() = 0;
    virtual AVIOContext* GetContext() = 0;
};

} // namespace Libraries::AvPlayer
