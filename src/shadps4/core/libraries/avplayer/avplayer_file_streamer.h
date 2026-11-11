// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/avplayer/avplayer_file_streamer.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <string_view>
#include "core/libraries/avplayer/avplayer.h"
#include "core/libraries/avplayer/avplayer_data_streamer.h"

struct AVIOContext;

namespace Libraries::AvPlayer {

class AvPlayerFileStreamer : public IDataStreamer {
public:
    AvPlayerFileStreamer(const AvPlayerFileReplacement& file_replacement);
    ~AvPlayerFileStreamer();

    bool Init(std::string_view path) override;
    void Reset() override;

    AVIOContext* GetContext() override {
        return m_avio_context;
    }

private:
    static s32 ReadPacket(void* opaque, u8* buffer, s32 size);
    static s64 Seek(void* opaque, s64 buffer, int whence);

    AvPlayerFileReplacement m_file_replacement;

    int m_fd = -1;
    u64 m_position{};
    u64 m_file_size{};
    AVIOContext* m_avio_context{};
};

} // namespace Libraries::AvPlayer
