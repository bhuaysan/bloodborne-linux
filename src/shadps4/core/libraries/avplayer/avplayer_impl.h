// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/core/libraries/avplayer/avplayer_impl.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-12
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include "core/libraries/avplayer/avplayer.h"
#include "core/libraries/avplayer/avplayer_state.h"

#include <memory>
#include <mutex>
#include <string_view>

namespace Libraries::AvPlayer {

class AvPlayer {
public:
    AvPlayer(const AvPlayerInitData& data, Core::MemoryManager& memory);

    s32 PostInit(const AvPlayerPostInitData& data);
    s32 AddSource(std::string_view filename);
    s32 AddSourceEx(std::string_view path, AvPlayerSourceType source_type);
    s32 GetStreamCount();
    s32 GetStreamInfo(u32 stream_index, AvPlayerStreamInfo& info);
    s32 EnableStream(u32 stream_index);
    s32 Start();
    s32 Pause();
    s32 Resume();
    s32 SetAvSyncMode(AvPlayerAvSyncMode sync_mode);
    bool GetAudioData(AvPlayerFrameInfo& audio_info);
    bool GetVideoData(AvPlayerFrameInfo& video_info);
    bool GetVideoData(AvPlayerFrameInfoEx& video_info);
    bool IsActive();
    u64 CurrentTime();
    s32 Stop();
    bool SetLooping(bool is_looping);

private:
    // Memory Replacement
    static void* PS4_SYSV_ABI Allocate(void* handle, u32 alignment, u32 size);
    static void PS4_SYSV_ABI Deallocate(void* handle, void* memory);
    static void* PS4_SYSV_ABI AllocateTexture(void* handle, u32 alignment, u32 size);
    static void PS4_SYSV_ABI DeallocateTexture(void* handle, void* memory);

    // File Replacement
    static int PS4_SYSV_ABI OpenFile(void* handle, const char* filename);
    static int PS4_SYSV_ABI CloseFile(void* handle);
    static int PS4_SYSV_ABI ReadOffsetFile(void* handle, u8* buffer, u64 position, u32 length);
    static u64 PS4_SYSV_ABI SizeFile(void* handle);

    AvPlayerInitData StubInitData(const AvPlayerInitData& data);

    AvPlayerInitData m_init_data{};
    AvPlayerInitData m_init_data_original{};
    std::mutex m_file_io_mutex{};

    std::unique_ptr<AvPlayerState> m_state{};
};

} // namespace Libraries::AvPlayer
