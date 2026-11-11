// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/avplayer/avplayer_handle_streamer.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-09
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include <memory>

#include "core/file_sys/ifile.h"
#include "core/libraries/avplayer/avplayer_data_streamer.h"

struct AVIOContext;

namespace Libraries::AvPlayer {

/// AVIO streamer that reads from a Core::FileSys::IFile handle.
class AvPlayerHandleStreamer final : public IDataStreamer {
public:
    explicit AvPlayerHandleStreamer(std::unique_ptr<Core::FileSys::IFile> handle);
    ~AvPlayerHandleStreamer() override;
    bool Init(std::string_view path) override;
    void Reset() override;

    AVIOContext* GetContext() override {
        return m_avio_context;
    }

private:
    static s32 ReadPacket(void* opaque, u8* buffer, s32 size);
    static s64 Seek(void* opaque, s64 offset, int whence);

    std::unique_ptr<Core::FileSys::IFile> m_handle;
    u64 m_position{0};
    u64 m_file_size{0};
    AVIOContext* m_avio_context{nullptr};
};

} // namespace Libraries::AvPlayer
